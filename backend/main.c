// 테스트용 백엔드: 요청 하나를 받아 경로별 고정 기능으로 응답하고 닫는다 (단일 스레드, common/만 사용)
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common/buf.h"
#include "common/http_parser.h"
#include "common/http_response.h"
#include "common/net.h"

#define LISTEN_BACKLOG 64
#define READ_CHUNK 4096
#define CLIENT_TIMEOUT_MS 5000
#define SLOW_SECONDS 5

// 헤더와 본문을 조립해 보낸다 (항상 Connection: close)
// 이유: 프록시가 요청마다 새 연결을 쓰고 Connection: close로 보내므로, 응답 하나 뒤 닫는 게 약속이다
static void respond(int fd, int status, const char *type, const char *body, size_t len)
{
    struct buf out;
    buf_init(&out);
    if (http_build_header(&out, status, type, (long)len, 0, NULL) < 0
        || buf_append(&out, body, len) < 0 || net_write_all(fd, out.data, out.len) < 0)
        perror("respond");
    buf_free(&out);
}

// 오류 응답(짧은 HTML)을 보낸다
// 이유: 모르는 경로 · 잘못된 요청도 서버와 같은 모양의 오류로 답해 프록시가 그대로 전달하게 한다
static void respond_error(int fd, int status)
{
    struct buf out;
    buf_init(&out);
    if (http_build_error(&out, status, 0) < 0 || net_write_all(fd, out.data, out.len) < 0)
        perror("respond_error");
    buf_free(&out);
}

// 요청 대상의 경로 부분(? 앞)이 path와 정확히 같으면 1
// 이유: /api/time?x=1 같은 쿼리가 붙어도 같은 기능으로 보이게 한다
static int path_is(const char *target, const char *path)
{
    size_t n = strcspn(target, "?");
    return strlen(path) == n && strncmp(target, path, n) == 0;
}

// 메서드 · 경로에 맞는 기능을 실행해 응답한다. 그 외는 404
// 이유: 각 경로가 프록시의 한 기능을 눈에 보이게 한다 — time: Round-Robin(포트), echo: 본문 전달, slow: 504
static void route_request(int fd, int port, const struct http_message *m, const char *raw)
{
    char body[128];
    int is_get = strcmp(m->method, "GET") == 0;
    if (is_get && path_is(m->target, "/api/time")) {
        char ts[32];
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
        int n = snprintf(body, sizeof body, "{\"port\":%d,\"time\":\"%s\"}\n", port, ts);
        respond(fd, 200, "application/json", body, (size_t)n);
    } else if (strcmp(m->method, "POST") == 0 && path_is(m->target, "/api/echo")) {
        const char *type = http_header_get(m, "Content-Type");
        respond(fd, 200, type ? type : "application/octet-stream", raw + m->header_len,
                m->total_len - m->header_len);
    } else if (is_get && path_is(m->target, "/api/slow")) {
        sleep(SLOW_SECONDS); /* 프록시의 응답 제한(3초)을 넘겨 504를 일으키려고 일부러 늦게 */
        int n = snprintf(body, sizeof body, "{\"port\":%d,\"slow\":%d}\n", port, SLOW_SECONDS);
        respond(fd, 200, "application/json", body, (size_t)n);
    } else {
        respond_error(fd, 404);
    }
}

// 요청 하나를 읽고 파싱한 뒤 경로에 맞는 응답을 보낸다 (닫는 것은 main)
// 이유: 백엔드는 프록시의 상대역이라 단순하게 "연결 하나 = 요청 하나"만 처리한다
static void handle_conn(int fd, int port)
{
    if (net_set_timeout(fd, CLIENT_TIMEOUT_MS, CLIENT_TIMEOUT_MS) < 0)
        return;
    struct buf in;
    buf_init(&in);
    struct http_parser p;
    http_parser_init(&p, PARSE_REQUEST);
    char chunk[READ_CHUNK];
    enum parse_result res = PARSE_NEED_MORE;
    while (res == PARSE_NEED_MORE) {
        ssize_t r = net_read(fd, chunk, sizeof chunk);
        if (r <= 0 || buf_append(&in, chunk, (size_t)r) < 0)
            break;
        res = http_parse(&p, in.data, in.len);
    }
    if (res == PARSE_DONE) {
        fprintf(stderr, "backend %d: %s %s\n", port, p.msg.method, p.msg.target);
        route_request(fd, port, &p.msg, in.data);
    } else if (res == PARSE_ERROR) {
        respond_error(fd, p.error_status);
    }
    buf_free(&in);
}

// 포트 인자로 리스닝하고, 연결을 하나씩 받아 처리하고 닫는 것을 반복한다
// 이유: 단일 스레드라서 /api/slow를 처리하는 5초 동안 이 백엔드는 다른 요청을 못 받는다 (Round-Robin 확인에 씀)
int main(int argc, char **argv)
{
    char *end;
    long port = argc == 2 ? strtol(argv[1], &end, 10) : 0;
    if (argc != 2 || *end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "사용법: %s <포트>\n", argv[0]);
        return 1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    int lfd = sigaction(SIGPIPE, &sa, NULL) < 0 ? -1 : net_listen((uint16_t)port, LISTEN_BACKLOG);
    if (lfd < 0) {
        perror("backend 시작");
        return 1;
    }
    fprintf(stderr, "backend: 포트 %ld에서 대기 중\n", port);
    for (;;) {
        char ip[INET_ADDRSTRLEN];
        int cfd = net_accept(lfd, ip, sizeof ip);
        if (cfd < 0) {
            perror("accept");
            continue;
        }
        handle_conn(cfd, (int)port);
        close(cfd);
    }
}
