#include "server/proxy.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "common/buf.h"
#include "common/http_parser.h"
#include "common/http_response.h"
#include "common/net.h"

#define UPSTREAM_CHUNK 16384

// 라우트마다 하나씩, 여러 워커가 함께 올리는 Round-Robin 카운터. 설정(cfg)은 읽기 전용으로 남긴다 (Q4)
static atomic_uint rr_counters[CONFIG_MAX_ROUTES];

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

// 이 라우트의 카운터를 원자적으로 1 올려 다음 백엔드 번호를 고른다
// 이유: 여러 워커가 동시에 골라도 번호가 겹치지 않아 백엔드를 정확히 번갈아 쓴다
static int pick_backend(const struct request_ctx *ctx)
{
    size_t idx = (size_t)(ctx->route - ctx->cfg->routes);
    unsigned n = atomic_fetch_add(&rr_counters[idx], 1);
    return (int)(n % (unsigned)ctx->route->backend_count);
}

// 고른 백엔드에 연결하고, 실패하면 다음 백엔드로 한 번 더 시도한다. 성공 fd, 실패 -1
// 이유: 백엔드 하나가 죽어도 손님은 502 대신 살아 있는 쪽의 응답을 받게 한다
static int connect_backend(struct request_ctx *ctx)
{
    const struct route *r = ctx->route;
    int first = pick_backend(ctx);
    int tries = r->backend_count > 1 ? 2 : 1;
    for (int t = 0; t < tries; t++) {
        const struct backend *b = &r->backends[(first + t) % r->backend_count];
        snprintf(ctx->upstream, sizeof ctx->upstream, "%s:%d", b->host, b->port);
        int fd = net_connect(b->host, (uint16_t)b->port, ctx->cfg->backend_connect_timeout_ms);
        if (fd >= 0)
            return fd;
    }
    return -1;
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

// 백엔드 응답을 읽어 손님에게 그대로 흘려보낸다. 0 = 손님 연결 계속 가능, -1 = 닫아야 함
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
        return send_error(ctx, rc) == 0 ? 0 : -1;
    }
    long cl = p.msg.content_length;
    int keep = ctx->keep_alive && cl >= 0; /* 길이를 모르면 손님 연결도 닫아야 끝을 알린다 (Q8) */
    size_t body = in.len - p.msg.header_len;
    int ok = build_client_header(&p.msg, keep, &head) == 0
             && net_write_all(ctx->fd, head.data, head.len) == 0
             && net_write_all(ctx->fd, in.data + p.msg.header_len, body) == 0;
    ctx->status = p.msg.status;
    char chunk[UPSTREAM_CHUNK];
    while (ok && (cl < 0 || (long)body < cl)) {
        ssize_t r = net_read(bfd, chunk, sizeof chunk);
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

// 프록시 핸들러: 백엔드 선택 → 연결(실패 502) → 요청 전달 → 응답 중계(시간 초과 504). 0 계속, -1 닫기
// 이유: 라우터가 /api/처럼 프록시 라우트로 고른 요청을 백엔드와 손님 사이에서 대신 전달한다
int proxy_handle(struct request_ctx *ctx)
{
    int bfd = connect_backend(ctx);
    if (bfd < 0)
        return send_error(ctx, 502);
    struct buf req;
    buf_init(&req);
    int rc;
    int timeout = ctx->cfg->backend_timeout_ms;
    if (net_set_timeout(bfd, timeout, timeout) < 0 || build_upstream_request(ctx, &req) < 0
        || net_write_all(bfd, req.data, req.len) < 0)
        rc = send_error(ctx, 502);
    else
        rc = relay_response(bfd, ctx);
    buf_free(&req);
    close(bfd);
    return rc;
}
