// 임시 서버: accept → 요청 하나를 파싱 → 정적 파일 응답 → close. 스텝 4~5에서 교체한다
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "common/buf.h"
#include "common/http_parser.h"
#include "common/http_response.h"
#include "common/net.h"
#include "server/handler.h"
#include "server/static_handler.h"

#define DEFAULT_PORT 8080
#define LISTEN_BACKLOG 128
#define READ_CHUNK 4096
#define EMFILE_BACKOFF_NS 100000000L /* 100ms */
#define STATIC_ROOT "./www"          /* 스텝 4에서 설정 파일 · -r 인자로 바뀐다 */

// 파싱 결과에 따라 정적 핸들러를 부르거나 오류 응답을 보낸다
// 이유: 라우터(스텝 4)가 생기기 전이라 모든 요청을 정적 핸들러로 보낸다
static void dispatch(int fd, const char *ip, const struct http_parser *p, const struct buf *in,
                     enum parse_result res)
{
    if (res == PARSE_DONE) {
        struct request_ctx ctx = {fd, ip, &p->msg, in->data, p->msg.total_len, STATIC_ROOT, 0};
        static_handle(&ctx);
        return;
    }
    struct buf out;
    buf_init(&out);
    if (http_build_error(&out, p->error_status, 0) < 0 || net_write_all(fd, out.data, out.len) < 0)
        perror("error response");
    buf_free(&out);
}

// 요청 하나가 완성되거나 규칙 위반이 나올 때까지 읽고 파싱한 뒤 응답한다
// 이유: read 조각을 버퍼에 쌓고, 파서가 "다 왔다(DONE)"고 할 때만 응답해야 요청 경계가 맞는다
static void handle_client(int fd, const char *ip)
{
    struct buf in;
    buf_init(&in);
    struct http_parser p;
    http_parser_init(&p, PARSE_REQUEST);
    char chunk[READ_CHUNK];

    for (;;) {
        ssize_t r = net_read(fd, chunk, sizeof chunk);
        if (r <= 0)
            break; /* 0: 손님이 닫음, 음수: 연결 오류. 응답 없이 정리 */
        if (buf_append(&in, chunk, (size_t)r) < 0) {
            fprintf(stderr, "buf_append: 메모리 부족\n");
            break;
        }
        enum parse_result res = http_parse(&p, in.data, in.len);
        if (res == PARSE_NEED_MORE)
            continue;
        dispatch(fd, ip, &p, &in, res);
        break;
    }
    buf_free(&in);
}

// 포트 인자를 읽고, 리스닝 소켓을 연 뒤 "accept → 처리 → close"를 반복한다
// 이유: 연결을 하나씩 순서대로 처리하는 가장 단순한 서버로 소켓 흐름 전체를 먼저 확인한다
int main(int argc, char **argv)
{
    long port = DEFAULT_PORT;
    if (argc > 2) {
        fprintf(stderr, "사용법: %s [포트]\n", argv[0]);
        return 1;
    }
    if (argc == 2) {
        char *end;
        errno = 0;
        port = strtol(argv[1], &end, 10);
        if (errno != 0 || end == argv[1] || *end != '\0' || port < 1 || port > 65535) {
            fprintf(stderr, "잘못된 포트: %s\n", argv[1]);
            return 1;
        }
    }

    int lfd = net_listen((uint16_t)port, LISTEN_BACKLOG);
    if (lfd < 0) {
        perror("listen");
        return 1;
    }
    fprintf(stderr, "포트 %ld에서 대기 중\n", port);

    for (;;) {
        char ip[INET_ADDRSTRLEN];
        int cfd = net_accept(lfd, ip, sizeof ip);
        if (cfd < 0) {
            int err = errno;
            perror("accept");
            // fd가 바닥나면 accept가 즉시 계속 실패하므로 잠깐 쉬어 CPU 100% 반복을 막는다 (Q3)
            if (err == EMFILE || err == ENFILE) {
                struct timespec ts = {0, EMFILE_BACKOFF_NS};
                nanosleep(&ts, NULL);
            }
            continue;
        }
        fprintf(stderr, "연결: %s\n", ip);
        handle_client(cfd, ip);
        close(cfd);
    }
}
