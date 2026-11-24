#include "server/conn.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "common/buf.h"
#include "common/http_parser.h"
#include "common/http_response.h"
#include "common/net.h"
#include "server/handler.h"
#include "server/router.h"

#define READ_CHUNK 4096
#define MAX_REQUESTS 100                /* 연결당 최대 요청 수 (5장 기본값) */
#define LINGER_MS 1000                  /* lingering close 최대 시간 (Q2) */
#define LINGER_BYTES 65536              /* lingering close로 버릴 최대 바이트 (Q2) */

enum read_result { READ_DONE, READ_ERROR, READ_CLOSED, READ_IDLE, READ_TIMEOUT };

// 연결 하나의 상태. 이 연결을 맡은 워커만 쓰므로 락이 없다
struct conn {
    int fd;
    char ip[16];
    const struct server_config *cfg;
    struct buf in;                      /* 받은 바이트 (파이프라이닝으로 다음 요청이 남을 수 있음) */
    struct http_parser parser;
    int requests;                       /* 이 연결에서 처리한 요청 수 */
    int keep;                           /* 이번 응답을 keep-alive로 보내는가 */
    int linger;                         /* 닫을 때 lingering close가 필요한가 */
};

// start부터 지금까지 흐른 시간을 밀리초로 반환한다
// 이유: 시스템 시계가 바뀌어도 거꾸로 가지 않는 CLOCK_MONOTONIC으로 마감 시간을 잰다
static long elapsed_ms(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000L + (now.tv_nsec - start->tv_nsec) / 1000000L;
}

// 연결 객체를 만들고 소켓 읽기 · 쓰기 타임아웃을 건다. 실패 NULL (그때 fd는 호출한 쪽이 닫는다)
// 이유: 연결마다 따로여야 하는 상태(버퍼 · 파서 · 요청 수)를 한 객체에 모아 워커끼리 섞이지 않게 한다
struct conn *conn_new(int fd, const char *ip, const struct server_config *cfg)
{
    struct conn *c = malloc(sizeof *c);
    if (c == NULL)
        return NULL;
    if (net_set_timeout(fd, cfg->client_timeout_ms, cfg->client_timeout_ms) < 0) {
        free(c);
        return NULL;
    }
    c->fd = fd;
    snprintf(c->ip, sizeof c->ip, "%s", ip);
    c->cfg = cfg;
    buf_init(&c->in);
    http_parser_init(&c->parser, PARSE_REQUEST);
    c->requests = 0;
    c->keep = 0;
    c->linger = 0;
    return c;
}

// 요청 하나가 완성될 때까지 읽고 파싱한다. DONE / ERROR / CLOSED / IDLE(기다리다 끝) / TIMEOUT
// 이유: read 한 번 5초(소켓 타임아웃)와 요청 전체 10초(마감)를 함께 걸어 느린 손님이 워커를 붙잡지 못하게 한다
static int conn_read_request(struct conn *c)
{
    struct timespec start;
    int started = c->in.len > 0;        /* 파이프라이닝으로 남은 바이트가 있으면 이미 시작된 요청 */
    if (started)
        clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (c->in.len > 0) {
            enum parse_result res = http_parse(&c->parser, c->in.data, c->in.len);
            if (res != PARSE_NEED_MORE)
                return res == PARSE_DONE ? READ_DONE : READ_ERROR;
        }
        if (started && elapsed_ms(&start) > c->cfg->request_deadline_ms)
            return READ_TIMEOUT;
        char chunk[READ_CHUNK];
        ssize_t r = net_read(c->fd, chunk, sizeof chunk);
        // 다음 요청을 기다리다 시간이 지난 것(버퍼 비어 있음)은 조용히 닫고, 요청 도중이면 408 (Q5)
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return started ? READ_TIMEOUT : READ_IDLE;
        if (r <= 0 || buf_append(&c->in, chunk, (size_t)r) < 0)
            return READ_CLOSED;
        if (!started) {
            started = 1;
            clock_gettime(CLOCK_MONOTONIC, &start);
        }
    }
}

// 접근 로그 한 줄을 stderr에 남긴다: 시각 IP "메서드 경로" 상태 본문바이트 걸린ms [백엔드]
// 이유: 어느 요청이 어디로 가서 무엇을 받았는지 서버 쪽에서 보이게 한다 (Q5 지도 Q3)
static void conn_log(const struct conn *c, int status, size_t bytes, const char *upstream, long ms)
{
    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
    const struct http_message *m = &c->parser.msg;
    // fprintf 한 번은 stdio가 스트림을 잠근 채 쓰므로 여러 워커의 줄이 섞이지 않는다
    fprintf(stderr, "%s %s \"%s %s\" %d %zu %ldms%s%s\n", ts, c->ip, m->method, m->target, status,
            bytes, ms, upstream[0] ? " " : "", upstream);
}

// 오류 응답을 보내고, 닫을 때 lingering close를 하도록 표시한다
// 이유: 오류 뒤에는 요청 경계를 믿을 수 없어 닫는다. HEAD면 본문을 빼야 한다 (Q4)
static void conn_send_error(struct conn *c, int status)
{
    struct buf out;
    buf_init(&out);
    int head = strcmp(c->parser.msg.method, "HEAD") == 0;
    int rc = head ? http_build_header(&out, status, NULL, 0, 0, NULL)
                  : http_build_error(&out, status, 0);
    c->linger = rc == 0 && net_write_all(c->fd, out.data, out.len) == 0;
    buf_free(&out);
    conn_log(c, status, 0, "", 0);
}

// request_ctx를 채우고 라우터가 고른 핸들러를 호출한다. 핸들러 반환값 (0 계속 가능, -1 닫기)
// 이유: conn은 어떤 핸들러인지 모르고 같은 모양으로 부른다. keep-alive 여부는 응답 헤더를 쓰기 전에 정한다
static int conn_dispatch(struct conn *c)
{
    const struct http_message *m = &c->parser.msg;
    const struct route *r = router_match(c->cfg, m->target);
    c->keep = m->keep_alive && c->requests + 1 < MAX_REQUESTS;
    if (r == NULL) {
        conn_send_error(c, 404);
        return -1;
    }
    // 이름 붙인 초기화: 적지 않은 칸(status · bytes · upstream)은 0으로 채워진다
    struct request_ctx ctx = {.fd = c->fd, .ip = c->ip, .msg = m, .raw = c->in.data,
                              .raw_len = m->total_len, .keep_alive = c->keep, .route = r,
                              .cfg = c->cfg};
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int rc = router_handler(r)(&ctx);
    conn_log(c, ctx.status, ctx.bytes, ctx.upstream, elapsed_ms(&start));
    return rc;
}

// 핸들러 결과와 keep-alive 결정으로 연결을 계속 쓸지 정한다. 계속이면 1
// 이유: 핸들러가 실패했거나(쓰기 오류 등) 손님 · 요청 수 상한이 닫기를 원하면 끝낸다
static int conn_should_continue(const struct conn *c, int handler_rc)
{
    return handler_rc == 0 && c->keep;
}

// 연결이 끝날 때까지 "요청 읽기 → 처리 → keep-alive 판단"을 반복한다
// 이유: 연결 하나에서 요청을 여러 번 받아 매번 연결을 맺는 비용을 없앤다 (keep-alive · 파이프라이닝)
void conn_serve(struct conn *c)
{
    for (;;) {
        int rr = conn_read_request(c);
        if (rr == READ_IDLE || rr == READ_CLOSED)
            return;
        if (rr != READ_DONE) {
            // 요청 경계를 잃었으므로 keep-alive여도 닫는다
            conn_send_error(c, rr == READ_TIMEOUT ? 408 : c->parser.error_status);
            return;
        }
        int rc = conn_dispatch(c);
        c->requests++;
        buf_consume(&c->in, c->parser.msg.total_len);
        http_parser_init(&c->parser, PARSE_REQUEST);
        if (!conn_should_continue(c, rc)) {
            c->linger = c->linger || c->in.len > 0; /* 읽지 않은 다음 요청이 남았으면 (Q2) */
            return;
        }
    }
}

// 쓰기만 닫아 FIN을 보내고, 남은 입력을 최대 1초 · 64KB 읽어 버린다
// 이유: 읽지 않은 바이트가 남은 채 close하면 RST가 나가 손님이 마지막 응답을 버린다 (Q2)
static void lingering_close(int fd)
{
    char sink[4096];
    size_t total = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    if (shutdown(fd, SHUT_WR) < 0 || net_set_timeout(fd, LINGER_MS, LINGER_MS) < 0)
        return;
    while (total < LINGER_BYTES && elapsed_ms(&start) < LINGER_MS) {
        ssize_t r = net_read(fd, sink, sizeof sink);
        if (r <= 0)
            break;
        total += (size_t)r;
    }
}

// 필요하면 lingering close 후 fd를 닫고 버퍼와 객체 메모리를 해제한다
// 이유: 연결 fd는 여기 한 곳에서만 닫는다 (fd 규칙)
void conn_free(struct conn *c)
{
    if (c->linger)
        lingering_close(c->fd);
    close(c->fd);
    c->fd = -1;
    buf_free(&c->in);
    free(c);
}
