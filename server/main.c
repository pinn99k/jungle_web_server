// 서버 시작과 종료: 설정 → 시그널 → 리스닝 → 스레드 풀 → accept 루프 → Ctrl+C면 마무리 후 종료
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
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
#include "server/standby.h"
#include "server/thread_pool.h"

#define LISTEN_BACKLOG 128
#define EMFILE_BACKOFF_NS 100000000L    /* 100ms */
#define LISTEN_RETRY 40                 /* 이어받을 때 포트가 풀리길 기다리는 횟수 (50ms씩 2초) */
#define LISTEN_RETRY_NS 50000000L

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

// SIGINT · SIGTERM이 오면 종료 플래그만 세운다
// 이유: 시그널 핸들러는 아무 지점에서나 끼어들므로 async-signal-safe한 일(원자적 쓰기)만 한다
static void on_signal(int sig)
{
    (void)sig;
    atomic_store(&conn_stopping, 1);
}

// SIGPIPE는 무시하고, SIGINT · SIGTERM에 on_signal을 건다. 성공 0, 실패 -1
// 이유: SA_RESTART를 쓰지 않아야 accept가 EINTR로 깨어나 main이 종료 플래그를 본다 (Q6 지도 Q1)
static int install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    // 손님이 먼저 끊은 연결에 쓰면 SIGPIPE가 프로세스 전체를 죽이므로 무시하고 EPIPE로 받는다 (Q4 지도 Q3)
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) < 0)
        return -1;
    sa.sa_handler = on_signal;
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;
    return 0;
}

// 워커가 SIGINT · SIGTERM을 받지 않게 막은 채 스레드 풀을 만들고, main에서만 다시 받는다. 실패 NULL
// 이유: 커널은 프로세스 시그널을 아무 스레드에게나 보낸다. 워커가 받으면 main의 accept가 깨지 않는다 (Q3)
static struct thread_pool *start_pool(struct server_config *cfg)
{
    sigset_t set;
    sigset_t old;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &set, &old) != 0)
        return NULL;
    struct thread_pool *pool = pool_create(cfg->threads, cfg->queue, on_connection, cfg);
    pthread_sigmask(SIG_SETMASK, &old, NULL); /* 워커는 막힌 상태를 물려받았고, main만 다시 받음 */
    return pool;
}

// 종료 플래그가 설 때까지 연결을 받아 큐에 넣는다. 큐가 가득 차면 503
// 이유: Ctrl+C로 accept가 EINTR로 깨면 루프 조건에서 플래그를 보고 빠져나온다
static void accept_loop(int lfd, struct thread_pool *pool)
{
    while (!atomic_load(&conn_stopping)) {
        char ip[INET_ADDRSTRLEN];
        // 빈 곳을 찾아서 연결함
        int cfd = net_accept(lfd, ip, sizeof ip);
        if (cfd >= 0) {
            if (pool_submit(pool, cfd, ip) < 0)
                reject_busy(cfd);
            continue;
        }
        int err_no = errno;
        if (err_no == EINTR)
            continue; /* 시그널로 깸: 루프 조건에서 종료 플래그를 본다 */
        perror("accept");
        // fd가 바닥나면 accept가 즉시 계속 실패하므로 잠깐 쉬어 CPU 100% 반복을 막는다 (Q1 지도 Q3)
        if (err_no == EMFILE || err_no == ENFILE) {
            struct timespec ts = {0, EMFILE_BACKOFF_NS};
            nanosleep(&ts, NULL);
        }
    }
}

// 리스닝 소켓을 연다. 이중화 중이면 포트가 풀릴 때까지 잠깐 다시 시도한다. 성공 fd, 실패 -1
// 이유: 메인이 끝날 때 커널은 잠금과 포트를 같은 순간에 풀지 않는다. 그 틈에 실패했다고 끝나면 이어받을 쪽이 없다
static int open_listener(const struct server_config *cfg)
{
    int tries = cfg->lock_file[0] != '\0' ? LISTEN_RETRY : 0;
    int lfd = net_listen((uint16_t)cfg->port, LISTEN_BACKLOG);
    while (lfd < 0 && errno == EADDRINUSE && tries-- > 0) {
        struct timespec ts = {0, LISTEN_RETRY_NS};
        nanosleep(&ts, NULL);
        lfd = net_listen((uint16_t)cfg->port, LISTEN_BACKLOG);
    }
    return lfd;
}

// 설정 → 시그널 → 리스닝 → 스레드 풀 → accept 루프 → 종료 정리
// 이유: 종료는 "새 연결 막기 → 리스닝 닫기 → 진행 중 · 대기 중 요청 마무리 → 워커 회수" 순서여야 요청이 잘리지 않는다
int main(int argc, char **argv)
{
    static struct server_config cfg;
    char err[512];
    struct stat st;
    config_defaults(&cfg);
    // 설정파일 읽기 8080번, www/, 라우트 /api/ → 8001·8002, / → 정적
    if (config_apply_args(&cfg, argc, argv, err, sizeof err) < 0) {
        fprintf(stderr, "설정 오류: %s\n사용법: %s [-c 설정파일] [-p 포트] [-r 루트] [-t 스레드수]\n",
                err, argv[0]);
        return 1;
    }
    if (stat(cfg.root, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "루트 폴더가 없음: %s\n", cfg.root);
        return 1;
    }
    if (install_signals() < 0) {
        perror("sigaction");
        return 1;
    }
    // 이중화를 켰으면 메인이 될 때까지 여기서 기다린다. 잠금은 프로세스가 끝날 때 커널이 푼다 (ADR 0003)
    if (cfg.lock_file[0] != '\0') {
        int lock = standby_acquire(&cfg);
        if (lock < 0) {
            if (lock == -1)
                perror("lock_file");
            return lock == -1;
        }
    }
    // 커널 시작
    int lfd = open_listener(&cfg);
    if (lfd < 0) {
        perror("listen");
        return 1;
    }
    // 풀 만들기
    struct thread_pool *pool = start_pool(&cfg);
    if (pool == NULL) {
        fprintf(stderr, "스레드 풀을 만들 수 없음\n");
        close(lfd);
        return 1;
    }
    fprintf(stderr, "포트 %d에서 대기 중 (루트 %s, 라우트 %d개, 워커 %d)\n", cfg.port, cfg.root,
            cfg.route_count, cfg.threads);

    // 연결 루프 시작 
    accept_loop(lfd, pool);
    fprintf(stderr, "종료 중: 새 연결을 받지 않고 진행 중인 요청을 마무리합니다\n");
    close(lfd);
    pool_shutdown(pool);
    fprintf(stderr, "종료\n");
    return 0;
}
