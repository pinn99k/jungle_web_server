// 메인 · 보조: 잠금 파일을 쥔 프로세스가 메인이다. 보조는 메인이 죽거나 멈추면 이어받는다 (ADR 0003)
#include "server/standby.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common/net.h"
#include "server/conn.h"

#define LOCK_POLL_MS 100                /* 메인이 죽었는지(잠금이 풀렸는지) 보는 간격 */
#define CHECK_EVERY 10                  /* 잠금을 이만큼 볼 때마다 한 번 메인에게 물어봄 (1초) */

// 파일 전체에 대한 쓰기 잠금 요청을 만든다
// 이유: 잠금 시도(F_SETLK)와 쥔 쪽 확인(F_GETLK)이 같은 범위를 봐야 한다
static struct flock whole_file(void)
{
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    return fl;
}

// 잠금을 시도한다. 얻으면 1, 다른 프로세스가 쥐고 있으면 0, 오류 -1
// 이유: 잠금은 프로세스가 어떻게 죽든 커널이 풀어 주므로, 얻었다면 메인이 확실히 없는 것이다
static int try_lock(int fd)
{
    struct flock fl = whole_file();
    if (fcntl(fd, F_SETLK, &fl) == 0)
        return 1;
    return (errno == EACCES || errno == EAGAIN) ? 0 : -1;
}

// 메인에게 GET /를 보내 제한 시간 안에 한 바이트라도 오면 1, 아니면 0
// 이유: 멈춘 메인도 커널이 연결은 받아 주므로, 연결이 아니라 응답이 와야 일하고 있다고 본다
static int main_answers(const struct server_config *c)
{
    static const char REQ[] = "GET / HTTP/1.1\r\nHost: standby\r\nConnection: close\r\n\r\n";
    char byte;
    int fd = net_connect("127.0.0.1", (uint16_t)c->port, c->standby_timeout_ms);
    if (fd < 0)
        return 0;
    int ok = net_set_timeout(fd, c->standby_timeout_ms, c->standby_timeout_ms) == 0
             && net_write_all(fd, REQ, sizeof REQ - 1) == 0 && net_read(fd, &byte, 1) == 1;
    close(fd);
    return ok;
}

// 잠금을 쥔 프로세스를 SIGKILL로 끝낸다
// 이유: 멈춘 메인은 포트를 쥐고 있어 먼저 끝내야 이어받을 수 있다. 번호를 커널에게 물어 엉뚱한 프로세스를 피한다
static void fence_main(int fd)
{
    struct flock fl = whole_file();
    if (fcntl(fd, F_GETLK, &fl) < 0 || fl.l_type == F_UNLCK || fl.l_pid <= 1)
        return; /* 그 사이 풀렸으면 다음 잠금 시도에서 얻는다 */
    fprintf(stderr, "보조: 메인(pid %ld)이 답하지 않아 끝냅니다\n", (long)fl.l_pid);
    if (kill(fl.l_pid, SIGKILL) < 0)
        perror("fence");
}

// 메인이 될 때까지 기다린다. 메인이 되면 잠금 fd, 종료 신호를 받으면 -2, 실패 -1
// 이유: 죽음은 커널이 알려 주지만(잠금) 멈춤은 추측해야 한다(응답). 두 가지를 다른 간격으로 본다
int standby_acquire(const struct server_config *c)
{
    int fd = open(c->lock_file, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    int got;
    int misses = 0;
    for (int tick = 1; (got = try_lock(fd)) == 0 && !atomic_load(&conn_stopping); tick++) {
        if (tick == 1)
            fprintf(stderr, "보조: 메인이 있어 기다립니다 (포트 %d)\n", c->port);
        struct timespec ts = {0, LOCK_POLL_MS * 1000000L};
        nanosleep(&ts, NULL);
        if (tick % CHECK_EVERY != 0)
            continue;
        misses = main_answers(c) ? 0 : misses + 1;
        if (misses >= c->standby_fails) {
            fence_main(fd);
            misses = 0;
        }
    }
    if (got == 1)
        return fd;
    close(fd);
    return got == 0 ? -2 : -1;
}
