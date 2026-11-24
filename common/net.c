#include "common/net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

// 모든 IPv4 주소의 port에서 연결을 기다리는 리스닝 fd를 반환한다. 실패 -1
// 이유: socket → bind → listen 순서와 실패 시 fd 정리를 한 곳에 모아 호출하는 쪽을 단순하게 한다
int net_listen(uint16_t port, int backlog)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    // 서버를 재시작할 때 이전 연결이 TIME_WAIT로 포트를 잡고 있어도 bind가 되게 한다
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) < 0)
        goto fail;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0)
        goto fail;
    if (listen(fd, backlog) < 0)
        goto fail;
    return fd;

fail:;
    // close가 errno를 덮어쓰지 않게 보관해, 호출한 쪽의 perror가 진짜 원인을 출력하게 한다
    int err = errno;
    close(fd);
    errno = err;
    return -1;
}

// 연결 하나를 받아 fd를 반환하고, 클라이언트 IP 문자열을 ip에 적는다. 실패 -1
// 이유: 시그널로 깨어난 실패(EINTR)는 진짜 오류가 아니므로 호출하는 쪽이 신경 쓰지 않게 여기서 다시 시도한다
int net_accept(int lfd, char *ip, size_t ip_len)
{
    struct sockaddr_in addr;
    socklen_t len;
    int fd;
    do {
        len = sizeof addr;
        fd = accept(lfd, (struct sockaddr *)&addr, &len);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return -1;

    if (ip_len > 0 && inet_ntop(AF_INET, &addr.sin_addr, ip, (socklen_t)ip_len) == NULL)
        ip[0] = '\0';
    return fd;
}

// read()를 감싼다. EINTR이면 다시 읽는다. 반환값 의미는 read()와 같다 (0 = 상대가 닫음)
// 이유: 시그널 때문에 읽기가 중간에 실패한 것을 연결 오류로 오해하지 않기 위해서다
ssize_t net_read(int fd, void *buf, size_t n)
{
    ssize_t r;
    do {
        r = read(fd, buf, n);
    } while (r < 0 && errno == EINTR);
    return r;
}

// n바이트를 모두 보낼 때까지 write()를 반복한다. 성공 0, 실패 -1
// 이유: write()는 소켓 송신 버퍼가 차면 일부만 쓰고 돌아올 수 있다 (부분 쓰기)
int net_write_all(int fd, const void *data, size_t n)
{
    const char *p = data;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

// 소켓의 읽기 · 쓰기 타임아웃을 밀리초로 건다. 넘으면 read · write가 EAGAIN으로 실패한다. 성공 0, 실패 -1
// 이유: 블로킹 I/O에서 아무것도 안 보내는 손님 · 안 읽는 손님이 워커를 영원히 붙잡지 못하게 한다
int net_set_timeout(int fd, int recv_ms, int send_ms)
{
    struct timeval rt = {recv_ms / 1000, (recv_ms % 1000) * 1000};
    struct timeval st = {send_ms / 1000, (send_ms % 1000) * 1000};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof rt) < 0)
        return -1;
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof st);
}

// 논블로킹으로 connect를 시작하고, 끝날 때까지 최대 timeout_ms 기다린다. 성공 0, 실패 -1 (errno 설정)
// 이유: 블로킹 connect는 응답 없는 주소에서 수십 초 멈출 수 있어, 백엔드 연결 1초 제한을 지킬 수 없다
static int connect_with_timeout(int fd, const struct sockaddr_in *addr, int timeout_ms)
{
    if (connect(fd, (const struct sockaddr *)addr, sizeof *addr) == 0)
        return 0;
    if (errno != EINPROGRESS)
        return -1;
    struct pollfd pfd = {fd, POLLOUT, 0};
    int pr;
    do {
        pr = poll(&pfd, 1, timeout_ms);
    } while (pr < 0 && errno == EINTR);
    if (pr <= 0) {
        if (pr == 0)
            errno = ETIMEDOUT;
        return -1;
    }
    // 연결이 끝났다는 신호만으로는 성공인지 모르므로, 소켓에 남은 오류를 꺼내 본다
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
        return -1;
    errno = err;
    return err == 0 ? 0 : -1;
}

// 숫자 IPv4 host:port에 timeout_ms 안에 연결해 fd(블로킹)를 반환한다. 실패 -1 (시간 초과면 errno = ETIMEDOUT)
// 이유: 프록시가 백엔드에 거는 "연결 2"를 만든다. 연결 뒤에는 블로킹으로 되돌려 나머지 코드는 그대로 쓴다
int net_connect(const char *host, uint16_t port, int timeout_ms)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0
        || connect_with_timeout(fd, &addr, timeout_ms) < 0 || fcntl(fd, F_SETFL, flags) < 0) {
        int err = errno;
        close(fd);
        errno = err;
        return -1;
    }
    return fd;
}
