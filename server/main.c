// 임시 서버: accept → 요청 하나를 파싱 → 라우터가 고른 핸들러 → close. 스텝 5에서 교체한다
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "common/buf.h"
#include "common/http_parser.h"
#include "common/http_response.h"
#include "common/net.h"
#include "server/config.h"
#include "server/handler.h"
#include "server/router.h"

#define LISTEN_BACKLOG 128
#define READ_CHUNK 4096
#define EMFILE_BACKOFF_NS 100000000L /* 100ms */

// 파싱 결과에 따라 라우터가 고른 핸들러를 부르거나 오류 응답을 보낸다
// 이유: 요청 경로 → 라우트 → 핸들러 연결을 한 곳에 두고, 핸들러는 서로를 모르게 한다
static void dispatch(int fd, const char *ip, const struct server_config *cfg,
                     const struct http_parser *p, const struct buf *in, enum parse_result res)
{
    int status = p->error_status;
    if (res == PARSE_DONE) {
        const struct route *r = router_match(cfg, p->msg.target);
        if (r != NULL) {
            struct request_ctx ctx = {fd, ip, &p->msg, in->data, p->msg.total_len,
                                      cfg->root, 0, r, cfg};
            router_handler(r)(&ctx);
            return;
        }
        status = 404; /* 일치하는 라우트 없음 */
    }
    struct buf out;
    buf_init(&out);
    if (http_build_error(&out, status, 0) < 0 || net_write_all(fd, out.data, out.len) < 0)
        perror("error response");
    buf_free(&out);
}

// 요청 하나가 완성되거나 규칙 위반이 나올 때까지 읽고 파싱한 뒤 응답한다
// 이유: read 조각을 버퍼에 쌓고, 파서가 "다 왔다(DONE)"고 할 때만 응답해야 요청 경계가 맞는다
static void handle_client(int fd, const char *ip, const struct server_config *cfg)
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
        dispatch(fd, ip, cfg, &p, &in, res);
        break;
    }
    buf_free(&in);
}

// 설정을 읽고, 리스닝 소켓을 연 뒤 "accept → 처리 → close"를 반복한다
// 이유: 설정 오류 · 루트 없음은 손님을 받기 전에 알려 주고, 이후에는 연결을 하나씩 처리한다
int main(int argc, char **argv)
{
    static struct server_config cfg;
    char err[512];
    struct stat st;
    config_defaults(&cfg);
    if (config_apply_args(&cfg, argc, argv, err, sizeof err) < 0) {
        fprintf(stderr, "설정 오류: %s\n사용법: %s [-c 설정파일] [-p 포트] [-r 루트] [-t 스레드수]\n",
                err, argv[0]);
        return 1;
    }
    // 루트가 없으면 요청마다 500이 나므로 시작할 때 알린다 (Q16)
    if (stat(cfg.root, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "루트 폴더가 없음: %s\n", cfg.root);
        return 1;
    }

    int lfd = net_listen((uint16_t)cfg.port, LISTEN_BACKLOG);
    if (lfd < 0) {
        perror("listen");
        return 1;
    }
    fprintf(stderr, "포트 %d에서 대기 중 (루트 %s, 라우트 %d개)\n", cfg.port, cfg.root, cfg.route_count);

    for (;;) {
        char ip[INET_ADDRSTRLEN];
        int cfd = net_accept(lfd, ip, sizeof ip);
        if (cfd < 0) {
            int err_no = errno;
            perror("accept");
            // fd가 바닥나면 accept가 즉시 계속 실패하므로 잠깐 쉬어 CPU 100% 반복을 막는다 (Q3)
            if (err_no == EMFILE || err_no == ENFILE) {
                struct timespec ts = {0, EMFILE_BACKOFF_NS};
                nanosleep(&ts, NULL);
            }
            continue;
        }
        fprintf(stderr, "연결: %s\n", ip);
        handle_client(cfd, ip, &cfg);
        close(cfd);
    }
}
