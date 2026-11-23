// 스텝 1 임시 서버: accept → 헤더 끝까지 읽기 → 고정 응답 → close. 스텝 2~5에서 교체한다
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "common/buf.h"
#include "common/net.h"

#define DEFAULT_PORT 8080
#define LISTEN_BACKLOG 128
#define HEADER_MAX 8192
#define READ_CHUNK 4096
#define EMFILE_BACKOFF_NS 100000000L /* 100ms */

static const char RESP_HELLO[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 6\r\n"
    "Connection: close\r\n"
    "\r\n"
    "hello\n";

static const char RESP_431[] =
    "HTTP/1.1 431 Request Header Fields Too Large\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n"
    "\r\n";

// 버퍼에서 \r\n\r\n을 찾아 헤더 길이(빈 줄 포함)를 반환한다. 없으면 -1
// 이유: TCP는 메시지 경계가 없어서, 요청 헤더가 다 왔는지 빈 줄로 판단해야 한다. 스텝 2에서 파서로 교체
static long find_header_end(const struct buf *b)
{
    // 본문에 NUL이 있을 수 있어 strstr 대신 길이 기준으로 직접 찾는다
    for (size_t i = 0; i + 3 < b->len; i++) {
        if (b->data[i] == '\r' && b->data[i + 1] == '\n' &&
            b->data[i + 2] == '\r' && b->data[i + 3] == '\n')
            return (long)(i + 4);
    }
    return -1;
}

// 헤더 끝까지 읽은 뒤 고정 응답 "hello"를 보낸다. 헤더가 8KB를 넘으면 431을 보낸다
// 이유: 파싱 없이 "읽기 → 경계 찾기 → 응답" 흐름만 먼저 돌려 본다. 상한은 끝없이 쌓여 메모리가 터지는 것을 막는다
static void handle_client(int fd)
{
    struct buf in;
    buf_init(&in);
    char chunk[READ_CHUNK];

    for (;;) {
        ssize_t r = net_read(fd, chunk, sizeof chunk);
        if (r <= 0)
            break; /* 0: 손님이 닫음, 음수: 연결 오류. 응답 없이 정리 */
        if (buf_append(&in, chunk, (size_t)r) < 0) {
            fprintf(stderr, "buf_append: 메모리 부족\n");
            break;
        }
        long end = find_header_end(&in);
        int too_big = (end < 0) ? (in.len > HEADER_MAX) : (end > HEADER_MAX);
        if (too_big) {
            if (net_write_all(fd, RESP_431, sizeof RESP_431 - 1) < 0)
                perror("write");
            break;
        }
        if (end >= 0) {
            if (net_write_all(fd, RESP_HELLO, sizeof RESP_HELLO - 1) < 0)
                perror("write");
            break;
        }
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
