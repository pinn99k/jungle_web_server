// 임시 서버: accept → 요청 하나를 파싱 → 고정 응답 → close. 스텝 3~5에서 교체한다
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "common/buf.h"
#include "common/http_parser.h"
#include "common/net.h"

#define DEFAULT_PORT 8080
#define LISTEN_BACKLOG 128
#define READ_CHUNK 4096
#define EMFILE_BACKOFF_NS 100000000L /* 100ms */

/* 본문 길이는 %d, 본문은 %s. 사유구 표(http_reason)는 스텝 3에서 생긴다 */
#define RESP_HELLO_FMT \
    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s"
#define RESP_ERROR_FMT \
    "HTTP/1.1 %d Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"

// 파싱 결과에 맞는 응답을 보낸다. 성공이면 "hello 메서드 경로", 오류면 상태 코드만
// 이유: 파서가 무엇을 알아냈는지 curl로 바로 보이게 한다. 스텝 3 http_response가 생기면 교체
static void send_result(int fd, const struct http_parser *p, enum parse_result res)
{
    char body[HTTP_LINE_MAX + 64];
    char out[sizeof body + 128];
    int n;
    if (res == PARSE_DONE) {
        int blen = snprintf(body, sizeof body, "hello %s %s\n", p->msg.method, p->msg.target);
        n = snprintf(out, sizeof out, RESP_HELLO_FMT, blen, body);
    } else {
        n = snprintf(out, sizeof out, RESP_ERROR_FMT, p->error_status);
    }
    if (n < 0 || (size_t)n >= sizeof out) {
        fprintf(stderr, "응답이 버퍼보다 큼\n");
        return;
    }
    if (net_write_all(fd, out, (size_t)n) < 0)
        perror("write");
}

// 요청 하나가 완성되거나 규칙 위반이 나올 때까지 읽고 파싱한 뒤 응답한다
// 이유: read 조각을 버퍼에 쌓고, 파서가 "다 왔다(DONE)"고 할 때만 응답해야 요청 경계가 맞는다
static void handle_client(int fd)
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
        send_result(fd, &p, res);
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
        handle_client(cfd);
        close(cfd);
    }
}
