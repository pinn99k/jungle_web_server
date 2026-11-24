#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// 모든 IPv4 주소의 port에서 연결을 기다리는 리스닝 fd를 반환한다. 실패 -1
int net_listen(uint16_t port, int backlog);

// 연결 하나를 받아 fd를 반환하고, 클라이언트 IP 문자열을 ip에 적는다. 실패 -1 (시그널로 깨면 errno = EINTR)
int net_accept(int lfd, char *ip, size_t ip_len);

// read()를 감싼다. EINTR이면 다시 읽는다. 반환값 의미는 read()와 같다 (0 = 상대가 닫음)
ssize_t net_read(int fd, void *buf, size_t n);

// n바이트를 모두 보낼 때까지 write()를 반복한다. 성공 0, 실패 -1
int net_write_all(int fd, const void *data, size_t n);

// 숫자 IPv4 host:port에 timeout_ms 안에 연결해 fd(블로킹)를 반환한다. 실패 -1 (시간 초과면 errno = ETIMEDOUT)
int net_connect(const char *host, uint16_t port, int timeout_ms);

// 작은 패킷을 모아 보내는 Nagle 알고리즘을 끈다 (TCP_NODELAY). 성공 0, 실패 -1
int net_set_nodelay(int fd);

// 소켓의 읽기 · 쓰기 타임아웃을 밀리초로 건다. 넘으면 read · write가 EAGAIN으로 실패한다. 성공 0, 실패 -1
int net_set_timeout(int fd, int recv_ms, int send_ms);

#endif
