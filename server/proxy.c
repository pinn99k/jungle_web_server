#include "server/proxy.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "common/buf.h"
#include "common/http_parser.h"
#include "common/http_response.h"
#include "common/net.h"
#include "server/balance.h"
#include "server/gate.h"

#define UPSTREAM_CHUNK 16384

static const char *const HOP_BY_HOP[] = {
    "Connection", "Keep-Alive", "Proxy-Connection", "TE", "Trailer", "Transfer-Encoding",
    "Upgrade", "Proxy-Authorization", "Proxy-Authenticate", "X-Forwarded-For",
};

// 다음 홉에 넘기면 안 되는 헤더(+ 우리가 다시 쓰는 X-Forwarded-For)면 1
// 이유: Connection 같은 헤더는 손님과 우리 사이 연결에만 해당해서, 백엔드 · 손님 쪽 연결에 그대로 넘기면 안 된다
static int is_hop_by_hop(const char *name)
{
    for (size_t i = 0; i < sizeof HOP_BY_HOP / sizeof HOP_BY_HOP[0]; i++) {
        if (strcasecmp(name, HOP_BY_HOP[i]) == 0)
            return 1;
    }
    return 0;
}

// *p의 글자 하나를 %XX면 풀어서 반환하고 *p를 다음 글자로 옮긴다
// 이유: 백엔드는 %2e를 .으로, %2f를 /로 읽을 수 있으므로 풀린 글자로 검사해야 한다
static char next_char(const char **p, const char *end)
{
    const char *s = *p;
    if (*s == '%' && end - s >= 3 && isxdigit((unsigned char)s[1])
        && isxdigit((unsigned char)s[2])) {
        char hex[3] = {s[1], s[2], '\0'};
        *p = s + 3;
        return (char)strtol(hex, NULL, 16);
    }
    *p = s + 1;
    return *s;
}

// 요청 경로에 상위로 올라가는 .. 조각이 있으면 1. 조각의 ; 뒤는 무시하고 \도 구분자로 본다
// 이유: 우리는 정리 전 경로로 라우트를 고르므로, 백엔드가 ..을 정리하면 공개하지 않은 경로에 닿는다
int proxy_path_escapes(const char *target)
{
    const char *end = target + strcspn(target, "?");
    char seg[2] = {0, 0};
    size_t len = 0;
    int params = 0;
    for (const char *p = target; p < end;) {
        char c = next_char(&p, end);
        if (c == '/' || c == '\\') {
            if (len == 2 && seg[0] == '.' && seg[1] == '.')
                return 1;
            len = 0;
            params = 0;
        } else if (c == ';') {
            params = 1;
        } else if (!params) {
            if (len < sizeof seg)
                seg[len] = c;
            len++;
        }
    }
    return len == 2 && seg[0] == '.' && seg[1] == '.';
}

// 형식 문자열 한 줄을 out에 붙인다. 성공 0, 실패(너무 김 · 메모리 부족) -1
// 이유: 요청 줄 · 헤더 줄마다 잘림 검사를 빠뜨리지 않게 한 곳에 모은다
static int add_line(struct buf *out, const char *fmt, const char *a, const char *b)
{
    char line[HTTP_LINE_MAX + 64];
    int n = snprintf(line, sizeof line, fmt, a, b);
    if (n < 0 || (size_t)n >= sizeof line)
        return -1;
    return buf_append(out, line, (size_t)n);
}

// 백엔드에 보낼 요청을 만든다: 요청 줄(경로 그대로, Q1) + 헤더(Host 그대로 Q2, hop-by-hop 제외)
// + X-Forwarded-For + Connection: close + 본문. 성공 0, 실패 -1
// 이유: 백엔드는 진짜 손님 IP를 모르므로 XFF로 알려 주고, 응답 끝을 알기 쉽게 요청마다 새 연결로 끝낸다
static int build_upstream_request(const struct request_ctx *ctx, struct buf *out)
{
    const struct http_message *m = ctx->msg;
    if (add_line(out, "%s %s HTTP/1.1\r\n", m->method, m->target) < 0)
        return -1;
    for (int i = 0; i < m->header_count; i++) {
        const struct http_header *h = &m->headers[i];
        if (!is_hop_by_hop(h->name) && add_line(out, "%s: %s\r\n", h->name, h->value) < 0)
            return -1;
    }
    // 손님이 이미 XFF를 보냈으면 그 뒤에 손님 IP를 붙인다 (Q6)
    const char *xff = http_header_get(m, "X-Forwarded-For");
    int rc = xff ? add_line(out, "X-Forwarded-For: %s, %s\r\n", xff, ctx->ip)
                 : add_line(out, "X-Forwarded-For: %s%s\r\n", ctx->ip, "");
    if (rc < 0 || buf_append(out, "Connection: close\r\n\r\n", 21) < 0)
        return -1;
    return buf_append(out, ctx->raw + m->header_len, m->total_len - m->header_len);
}

// 오류 응답을 손님에게 보내고 접근 로그 칸을 채운다. 보냈으면 0, 실패 -1
// 이유: 502 · 504를 정적 핸들러 오류와 같은 모양으로 보낸다
static int send_error(struct request_ctx *ctx, int status)
{
    struct buf out;
    buf_init(&out);
    int rc = -1;
    if (http_build_error(&out, status, ctx->keep_alive) == 0)
        rc = net_write_all(ctx->fd, out.data, out.len);
    ctx->status = status;
    buf_free(&out);
    return rc;
}

// 짧은 JSON 응답을 보내고 접근 로그 칸을 채운다. extra는 추가 헤더 줄. 보냈으면 0, 실패 -1
// 이유: 줄 서기 · 자리 없음 응답이 같은 모양으로 나가게 한다
static int send_json(struct request_ctx *ctx, int status, const char *extra, const char *body)
{
    struct buf out;
    buf_init(&out);
    size_t len = strlen(body);
    int rc = -1;
    if (http_build_header(&out, status, "application/json", (long)len, ctx->keep_alive, extra) == 0
        && buf_append(&out, body, len) == 0)
        rc = net_write_all(ctx->fd, out.data, out.len);
    ctx->status = status;
    buf_free(&out);
    return rc;
}

// 백엔드 응답 헤더를 손님 쪽 헤더로 다시 써서 out에 만든다 (hop-by-hop은 빼고 Connection은 우리 기준). 성공 0
// 이유: 백엔드가 보낸 "Connection: close"는 백엔드 연결 이야기라, 손님 연결의 keep-alive와는 따로 정해야 한다 (Q7)
static int build_client_header(const struct http_message *resp, int keep, struct buf *out)
{
    char status[32];
    snprintf(status, sizeof status, "%d", resp->status);
    if (add_line(out, "HTTP/1.1 %s %s\r\n", status, http_reason(resp->status)) < 0)
        return -1;
    for (int i = 0; i < resp->header_count; i++) {
        const struct http_header *h = &resp->headers[i];
        if (!is_hop_by_hop(h->name) && add_line(out, "%s: %s\r\n", h->name, h->value) < 0)
            return -1;
    }
    return add_line(out, "Connection: %s%s\r\n\r\n", keep ? "keep-alive" : "close", "");
}

// 백엔드 응답 헤더가 다 올 때까지 읽는다. 헤더 완성 0, 첫 바이트 전 시간 초과 504, 그 외 실패 502
// 이유: 응답 줄 · 헤더를 해석해야 손님 쪽 헤더와 본문 길이(keep-alive 가능 여부)를 정할 수 있다
static int read_response_head(int bfd, struct http_parser *p, struct buf *in)
{
    char chunk[UPSTREAM_CHUNK];
    for (;;) {
        ssize_t r = net_read(bfd, chunk, sizeof chunk);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return in->len == 0 ? 504 : 502;
        if (r <= 0 || buf_append(in, chunk, (size_t)r) < 0)
            return 502;
        enum parse_result res = http_parse(p, in->data, in->len);
        if (res == PARSE_ERROR)
            return 502;
        // 응답 모드 파서는 본문까지 다 와야 DONE이므로, 헤더 길이가 정해졌으면 헤더 완성으로 본다
        if (res == PARSE_DONE || p->msg.header_len > 0)
            return 0;
    }
}

// 이 응답에 올 본문 길이를 반환한다. 본문이 없는 응답이면 0, 길이를 모르면 -1
// 이유: HEAD · 204 · 304는 Content-Length가 있어도 본문이 오지 않으므로, 기다리거나 넘기면 안 된다
static long body_length(const struct http_message *req, const struct http_message *resp)
{
    if (strcmp(req->method, "HEAD") == 0 || resp->status == 204 || resp->status == 304)
        return 0;
    return resp->content_length;
}

// done바이트를 보낸 뒤 손님에게 더 보내도 되는 바이트 수를 max 이하로 반환한다 (길이를 모르면 max)
// 이유: 백엔드가 약속한 길이보다 많이 보내도 넘기지 않아야 손님 쪽 다음 응답의 경계가 어긋나지 않는다
static size_t body_room(long cl, size_t done, size_t max)
{
    if (cl < 0)
        return max;
    size_t left = (size_t)cl > done ? (size_t)cl - done : 0;
    return left < max ? left : max;
}

// 백엔드 응답을 읽어 손님에게 그대로 흘려보낸다. 0 = 손님 연결 계속 가능, -1 = 닫아야 함,
// 양수 = 손님에게 아무것도 보내기 전에 실패함 (502 · 504, 부른 쪽이 다시 보내거나 오류로 답함)
// 이유: 응답을 통째로 모으지 않고 받는 대로 보내, 큰 응답도 연결마다 16KB만 쓰게 한다
static int relay_response(int bfd, struct request_ctx *ctx)
{
    struct http_parser p;
    struct buf in;
    struct buf head;
    http_parser_init(&p, PARSE_RESPONSE);
    buf_init(&in);
    buf_init(&head);
    int rc = read_response_head(bfd, &p, &in);
    if (rc != 0) {
        buf_free(&in);
        buf_free(&head);
        return rc;
    }
    long cl = body_length(ctx->msg, &p.msg);
    int keep = ctx->keep_alive && cl >= 0; /* 길이를 모르면 손님 연결도 닫아야 끝을 알린다 (Q8) */
    size_t body = body_room(cl, 0, in.len - p.msg.header_len);
    int ok = build_client_header(&p.msg, keep, &head) == 0
             && net_write_all(ctx->fd, head.data, head.len) == 0
             && net_write_all(ctx->fd, in.data + p.msg.header_len, body) == 0;
    ctx->status = p.msg.status;
    char chunk[UPSTREAM_CHUNK];
    while (ok && (cl < 0 || (long)body < cl)) {
        ssize_t r = net_read(bfd, chunk, body_room(cl, body, sizeof chunk));
        if (r <= 0) {
            ok = cl < 0 && r == 0; /* 길이 없는 본문은 백엔드가 닫으면 끝. 그 외는 중간에 끊김 (Q9) */
            break;
        }
        ok = net_write_all(ctx->fd, chunk, (size_t)r) == 0;
        body += (size_t)r;
    }
    ctx->bytes = body;
    buf_free(&in);
    buf_free(&head);
    return ok && keep ? 0 : -1;
}

// 백엔드 b에 요청을 한 번 보낸다. 0 · -1은 relay_response와 같고, 양수면 실패 상태 코드
// *sent는 백엔드에 요청 바이트를 보내기 시작했으면 1 (연결만 실패했으면 0)
// 이유: 연결 실패는 다시 보내도 안전하지만, 보낸 뒤 실패는 백엔드가 이미 처리했을 수 있어 구분해야 한다
static int attempt(struct request_ctx *ctx, int b, const struct buf *req, int *sent)
{
    const struct backend *be = &ctx->route->backends[b];
    int timeout = ctx->cfg->backend_timeout_ms;
    int rc = 502;
    *sent = 0;
    ctx->status = 0;
    snprintf(ctx->upstream, sizeof ctx->upstream, "%s:%d", be->host, be->port);
    int fd = net_connect(be->host, (uint16_t)be->port, ctx->cfg->backend_connect_timeout_ms);
    if (fd < 0)
        return 502;
    if (net_set_timeout(fd, timeout, timeout) == 0) {
        *sent = 1;
        if (net_write_all(fd, req->data, req->len) == 0)
            rc = relay_response(fd, ctx);
    }
    close(fd);
    return rc;
}

// 실패한 요청을 다른 백엔드에 다시 보내도 되면 1
// 이유: 신청처럼 두 번 처리되면 안 되는 요청은 다시 보내지 않고, 조회(GET · HEAD)만 다시 보낸다
static int safe_to_retry(const struct request_ctx *ctx, int sent)
{
    const char *m = ctx->msg->method;
    if (!sent)
        return 1;
    return ctx->cfg->retry_safe && (strcmp(m, "GET") == 0 || strcmp(m, "HEAD") == 0);
}

// 자리가 있는 백엔드를 골라 자리를 잡고 번호를 반환한다. 다 찼으면 -1
// 이유: 고른 뒤 자리를 잡기 전에 다른 워커가 채울 수 있어, 그때는 다시 고른다
static int reserve(const struct server_config *c, int route, int n, unsigned tried)
{
    int b;
    do {
        b = balance_pick(c, route, n, tried, balance_now());
    } while (b >= 0 && balance_begin(c, route, b) < 0);
    return b;
}

// 백엔드를 골라 보내고, 다시 보내도 되는 실패면 다른 백엔드에 한 번 더 보낸다. 0 계속, -1 닫기
// 이유: 고르기(balance)와 보내기(attempt)를 나눠, 정책을 바꿔도 전달 코드는 그대로다
static int forward(struct request_ctx *ctx, const struct buf *req)
{
    int route = (int)(ctx->route - ctx->cfg->routes);
    int n = ctx->route->backend_count;
    int status = 502;
    unsigned tried = 0;
    for (int t = 0; t < (n > 1 ? 2 : 1); t++) {
        int sent;
        int b = reserve(ctx->cfg, route, n, tried);
        if (b < 0)
            break;
        int rc = attempt(ctx, b, req, &sent);
        balance_end(ctx->cfg, route, b, rc > 0 || ctx->status >= 500, balance_now());
        if (rc <= 0)
            return rc;
        status = rc;
        tried |= 1u << b;
        if (!safe_to_retry(ctx, sent))
            break;
    }
    // 한 번도 못 보냈으면 서버가 아픈 게 아니라 자리가 다 찬 것이다
    return tried == 0 ? send_json(ctx, 503, "Retry-After: 1\r\n", "{\"busy\":true}\n")
                      : send_error(ctx, status);
}

// Cookie 헤더에서 qticket 값(32자)을 찾아 out에 적는다. 없으면 0, 있으면 1
// 이유: 웹서버는 번호표만 본다. 누구인지 알려 주는 세션 · 토큰은 백엔드가 본다
static int ticket_of(const struct request_ctx *ctx, char out[GATE_TICKET_LEN + 1])
{
    const char *c = http_header_get(ctx->msg, "Cookie");
    const char *p = c != NULL ? strstr(c, "qticket=") : NULL;
    if (p == NULL || strspn(p + 8, "0123456789abcdef") != GATE_TICKET_LEN)
        return 0;
    memcpy(out, p + 8, GATE_TICKET_LEN);
    out[GATE_TICKET_LEN] = '\0';
    return 1;
}

// 줄을 세우는 경로의 POST 요청이면 1
// 이유: 줄 서기를 켜지 않으면(gate_path 없음) 예전과 똑같이 동작한다
static int gated(const struct request_ctx *ctx)
{
    const char *path = ctx->cfg->gate_path;
    size_t n = strcspn(ctx->msg->target, "?");
    return path[0] != '\0' && strcmp(ctx->msg->method, "POST") == 0 && strlen(path) == n
           && strncmp(ctx->msg->target, path, n) == 0;
}

// 번호표 순서에 따라 들이거나 기다리게 한다. 들어가도 되면 1(번호표는 ticket에), 이미 답했으면 0
// 이유: 기다리는 손님에게 연결 · 워커를 쓰지 않고 바로 답해, 줄이 길어도 서버가 막히지 않는다
static int pass_gate(struct request_ctx *ctx, char ticket[GATE_TICKET_LEN + 1], int *rc)
{
    const struct server_config *c = ctx->cfg;
    struct gate_rule rule = {c->gate_slots, c->gate_grace_ms, c->gate_lottery};
    char mine[GATE_TICKET_LEN + 1];
    char extra[160];
    char body[64];
    int ahead;
    const char *key = ticket_of(ctx, mine) ? mine : NULL;
    switch (gate_visit(key, &rule, balance_now(), ticket, &ahead)) {
    case GATE_PASS:
        return 1;
    case GATE_BUSY:
        *rc = send_json(ctx, 409, "Retry-After: 1\r\n", "{\"running\":true}\n");
        return 0;
    case GATE_DONE:
        *rc = send_json(ctx, 200, NULL, "{\"already\":true}\n");
        return 0;
    default:
        snprintf(extra, sizeof extra, "Set-Cookie: qticket=%s; Path=/; HttpOnly; SameSite=Lax\r\n"
                 "Retry-After: 1\r\n", ticket);
        snprintf(body, sizeof body, "{\"ahead\":%d}\n", ahead);
        *rc = send_json(ctx, 503, ticket[0] ? extra : "Retry-After: 1\r\n", body);
        return 0;
    }
}

// 프록시 핸들러: 줄 서기 → 백엔드 선택 → 전달 → 응답 중계. 0 계속, -1 닫기
// 이유: 라우터가 /api/처럼 프록시 라우트로 고른 요청을 백엔드와 손님 사이에서 대신 전달한다
int proxy_handle(struct request_ctx *ctx)
{
    char ticket[GATE_TICKET_LEN + 1] = "";
    struct buf req;
    int rc = -1;
    int through = gated(ctx);
    if (proxy_path_escapes(ctx->msg->target))
        return send_error(ctx, 400);
    if (through && !pass_gate(ctx, ticket, &rc))
        return rc;
    buf_init(&req);
    if (build_upstream_request(ctx, &req) < 0)
        rc = send_error(ctx, 502);
    else
        rc = forward(ctx, &req);
    buf_free(&req);
    if (through)
        gate_finish(ticket, ctx->status == 200);
    return rc;
}
