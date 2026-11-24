// 서버 시작: 설정 → SIGPIPE 무시 → 리스닝 → 스레드 풀 → accept 루프 (종료 처리는 스텝 8)
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "common/buf.h"
#include "common/http_response.h"
#include "common/net.h"
#include "server/config.h"
#include "server/conn.h"
#include "server/thread_pool.h"

#define LISTEN_BACKLOG 128
#define QUEUE_CAP 128                   /* 작업 큐 크기 (5장 기본값) */
#define EMFILE_BACKOFF_NS 100000000L    /* 100ms */

// 워커가 실행하는 작업: 연결 하나를 처음부터 끝까지 처리한다
// 이유: 스레드 풀은 무엇을 하는지 모르고 이 함수만 부른다. 연결의 수명은 conn이 책임진다
static void on_connection(int fd, const char *ip, void *arg)
{
    struct conn *c = conn_new(fd, ip, arg);
    if (c == NULL) {
        close(fd);
        return;
    }
    conn_serve(c);
    conn_free(c);
}

// 작업 큐가 가득 찼을 때 503을 보내고 닫는다
// 이유: 대기 줄이 무한히 길어지지 않게 하고, 손님에게 "지금은 바쁨"을 알린다 (lingering 없음, Q10)
static void reject_busy(int fd)
{
    struct buf out;
    buf_init(&out);
    if (http_build_error(&out, 503, 0) < 0 || net_write_all(fd, out.data, out.len) < 0)
        perror("503 response");
    buf_free(&out);
    close(fd);
}

// 설정을 읽고, 리스닝 소켓과 스레드 풀을 만든 뒤 accept한 연결을 큐에 넣는 것을 반복한다
// 이유: main 스레드는 손님을 받기만 하고, 처리는 워커들이 동시에 한다 (D1 스레드 풀)
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
    if (stat(cfg.root, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "루트 폴더가 없음: %s\n", cfg.root);
        return 1;
    }
    // 손님이 먼저 끊은 연결에 쓰면 SIGPIPE가 프로세스 전체를 죽이므로 무시하고 EPIPE로 받는다 (Q3)
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    int lfd = net_listen((uint16_t)cfg.port, LISTEN_BACKLOG);
    if (lfd < 0) {
        perror("listen");
        return 1;
    }
    struct thread_pool *pool = pool_create(cfg.threads, QUEUE_CAP, on_connection, &cfg);
    if (pool == NULL) {
        fprintf(stderr, "스레드 풀을 만들 수 없음\n");
        close(lfd);
        return 1;
    }
    fprintf(stderr, "포트 %d에서 대기 중 (루트 %s, 라우트 %d개, 워커 %d)\n", cfg.port, cfg.root,
            cfg.route_count, cfg.threads);

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
        if (pool_submit(pool, cfd, ip) < 0)
            reject_busy(cfd);
    }
}
