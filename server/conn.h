#ifndef CONN_H
#define CONN_H

#include "server/config.h"

struct conn;

// 연결 객체를 만들고 소켓 읽기 · 쓰기 타임아웃을 건다. 실패 NULL (그때 fd는 호출한 쪽이 닫는다)
struct conn *conn_new(int fd, const char *ip, const struct server_config *cfg);

// 연결이 끝날 때까지 "요청 읽기 → 처리 → keep-alive 판단"을 반복한다
void conn_serve(struct conn *c);

// 필요하면 lingering close 후 fd를 닫고 버퍼와 객체 메모리를 해제한다
void conn_free(struct conn *c);

#endif
