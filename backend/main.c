// 테스트용 백엔드: 요청 하나를 받아 경로별 고정 기능으로 응답하고 닫는다 (기본 단일 스레드, common/만 사용)
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
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
#define WORK_MAX_MS 10000
#define COURSES_MS 10
#define APPLY_MS 100
#define MAX_THREADS 256

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

// ms밀리초 기다린다
// 이유: CPU를 쓰지 않고 기다려 DB를 기다리는 백엔드를 흉내 낸다 (코어를 서버와 나눠 쓰지 않게)
static void wait_ms(int ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

// {"port":..,"time":"UTC 시각"}으로 응답한다
// 이유: 응답에 자기 포트를 넣어 Round-Robin을 눈으로 확인한다
static void do_time(int fd, int port, const struct http_message *m, const char *raw)
{
    (void)m;
    (void)raw;
    char body[128];
    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
    int n = snprintf(body, sizeof body, "{\"port\":%d,\"time\":\"%s\"}\n", port, ts);
    respond(fd, 200, "application/json", body, (size_t)n);
}

// 받은 본문을 그대로 돌려준다
// 이유: 프록시가 본문을 한 바이트도 바꾸지 않고 전달하는지 확인한다
static void do_echo(int fd, int port, const struct http_message *m, const char *raw)
{
    (void)port;
    const char *type = http_header_get(m, "Content-Type");
    respond(fd, 200, type ? type : "application/octet-stream", raw + m->header_len,
            m->total_len - m->header_len);
}

// 5초 기다린 뒤 응답한다
// 이유: 프록시의 응답 제한(3초)을 넘겨 504를 일으키려고 일부러 늦게 답한다
static void do_slow(int fd, int port, const struct http_message *m, const char *raw)
{
    (void)m;
    (void)raw;
    char body[64];
    sleep(SLOW_SECONDS);
    int n = snprintf(body, sizeof body, "{\"port\":%d,\"slow\":%d}\n", port, SLOW_SECONDS);
    respond(fd, 200, "application/json", body, (size_t)n);
}

// "/api/work?ms=N"의 N(0~10000)을 반환한다. 없거나 범위 밖이면 -1
// 이유: 측정할 때 백엔드가 걸리는 시간을 요청마다 정해야 느린 요청 · 빠른 요청을 섞을 수 있다
static int work_ms(const char *target)
{
    const char *q = strstr(target, "?ms=");
    char *end;
    if (q == NULL || q[4] < '0' || q[4] > '9')
        return -1;
    long ms = strtol(q + 4, &end, 10);
    return (*end != '\0' || ms > WORK_MAX_MS) ? -1 : (int)ms;
}

// N밀리초 기다린 뒤 {"port":..,"ms":N}으로 응답한다. N이 잘못되면 400
// 이유: 걸리는 시간을 아는 백엔드가 있어야 용량(초당 처리 수)을 계산하고 잴 수 있다
static void do_work(int fd, int port, const struct http_message *m, const char *raw)
{
    (void)raw;
    char body[64];
    int ms = work_ms(m->target);
    if (ms < 0) {
        respond_error(fd, 400);
        return;
    }
    wait_ms(ms);
    int n = snprintf(body, sizeof body, "{\"port\":%d,\"ms\":%d}\n", port, ms);
    respond(fd, 200, "application/json", body, (size_t)n);
}

// 10ms 뒤 과목 목록을 돌려준다
// 이유: 신청보다 가벼운 조회 요청이 있어야 무거운 요청과 섞인 상황을 만든다
static void do_courses(int fd, int port, const struct http_message *m, const char *raw)
{
    (void)m;
    (void)raw;
    char body[256];
    wait_ms(COURSES_MS);
    int n = snprintf(body, sizeof body,
                     "{\"port\":%d,\"courses\":[\"운영체제\",\"네트워크\",\"데이터베이스\"]}\n", port);
    respond(fd, 200, "application/json", body, (size_t)n);
}

// 본문 "student=N"의 N(0 이상)을 반환한다. 형식이 다르면 -1
// 이유: 본문은 NUL로 끝나지 않으므로 길이를 재서 복사한 뒤에만 문자열 함수를 쓴다
static long student_id(const char *body, size_t len)
{
    static const char KEY[] = "student=";
    char num[16];
    char *end;
    if (len < sizeof KEY || len - (sizeof KEY - 1) >= sizeof num
        || memcmp(body, KEY, sizeof KEY - 1) != 0)
        return -1;
    size_t n = len - (sizeof KEY - 1);
    memcpy(num, body + sizeof KEY - 1, n);
    num[n] = '\0';
    long id = strtol(num, &end, 10);
    return (num[0] < '0' || num[0] > '9' || *end != '\0') ? -1 : id;
}

// 신청을 처리(100ms)하고 "apply N"을 stderr에 남긴 뒤 응답한다. 본문이 잘못되면 400
// 이유: 기록을 응답보다 먼저 남겨야 "처리는 됐는데 손님은 실패로 아는" 신청을 나중에 셀 수 있다
static void do_apply(int fd, int port, const struct http_message *m, const char *raw)
{
    char body[96];
    long id = student_id(raw + m->header_len, m->total_len - m->header_len);
    if (id < 0) {
        respond_error(fd, 400);
        return;
    }
    wait_ms(APPLY_MS);
    fprintf(stderr, "apply %ld\n", id);
    int n = snprintf(body, sizeof body, "{\"port\":%d,\"student\":%ld,\"ok\":true}\n", port, id);
    respond(fd, 200, "application/json", body, (size_t)n);
}

static const struct {
    const char *method;
    const char *path;
    void (*handle)(int fd, int port, const struct http_message *m, const char *raw);
} ROUTES[] = {
    {"GET", "/api/time", do_time},       {"POST", "/api/echo", do_echo},
    {"GET", "/api/slow", do_slow},       {"GET", "/api/work", do_work},
    {"GET", "/api/courses", do_courses}, {"POST", "/api/apply", do_apply},
};

// 메서드 · 경로에 맞는 기능을 표에서 찾아 실행한다. 없으면 404
// 이유: 경로마다 모양이 같으므로 표에 한 줄을 더하면 기능이 늘어난다
static void route_request(int fd, int port, const struct http_message *m, const char *raw)
{
    for (size_t i = 0; i < sizeof ROUTES / sizeof ROUTES[0]; i++) {
        if (strcmp(m->method, ROUTES[i].method) == 0 && path_is(m->target, ROUTES[i].path)) {
            ROUTES[i].handle(fd, port, m, raw);
            return;
        }
    }
    respond_error(fd, 404);
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

struct listener {
    int fd;
    int port;
};

// 리스닝 소켓에서 연결을 하나씩 받아 처리하고 닫는 것을 반복한다. 돌아오지 않는다
// 이유: 스레드 하나가 한 번에 한 건만 처리하므로, 스레드 수가 곧 이 백엔드의 동시 처리 수가 된다
static void *serve_loop(void *arg)
{
    const struct listener *l = arg;
    for (;;) {
        char ip[INET_ADDRSTRLEN];
        int cfd = net_accept(l->fd, ip, sizeof ip);
        if (cfd < 0) {
            perror("accept");
            continue;
        }
        handle_conn(cfd, l->port);
        close(cfd);
    }
    return NULL;
}

// 인자 문자열을 1~max 정수로 바꾼다. 숫자가 아니거나 범위 밖이면 -1
// 이유: 포트와 스레드 수가 같은 검사를 쓴다
static long parse_arg(const char *s, long max)
{
    char *end;
    long v = strtol(s, &end, 10);
    return (end == s || *end != '\0' || v < 1 || v > max) ? -1 : v;
}

// 포트 인자로 리스닝하고, 처리 스레드 수(기본 1)만큼 serve_loop를 돌린다
// 이유: 기본은 단일 스레드라 /api/slow 5초 동안 다른 요청을 못 받고, 수를 주면 여러 건을 동시에 받는다
int main(int argc, char **argv)
{
    long port = argc >= 2 ? parse_arg(argv[1], 65535) : -1;
    long threads = argc >= 3 ? parse_arg(argv[2], MAX_THREADS) : 1;
    if (argc > 3 || port < 0 || threads < 0) {
        fprintf(stderr, "사용법: %s <포트> [처리 스레드 수 1~%d]\n", argv[0], MAX_THREADS);
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
    static struct listener l;
    l.fd = lfd;
    l.port = (int)port;
    fprintf(stderr, "backend: 포트 %ld에서 대기 중 (스레드 %ld)\n", port, threads);
    for (long i = 1; i < threads; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, serve_loop, &l) != 0) {
            perror("pthread_create");
            return 1;
        }
    }
    serve_loop(&l);
    return 0;
}
