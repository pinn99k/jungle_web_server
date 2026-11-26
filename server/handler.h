#ifndef HANDLER_H
#define HANDLER_H

#include <stddef.h>

#include "common/http_parser.h"
#include "server/config.h"

// 핸들러 하나가 요청 하나를 처리할 때 필요한 것. conn(스텝 5 전까지는 main)이 채운다
struct request_ctx {
    int fd;                             /* 클라이언트 연결. 핸들러는 쓰기만 하고 닫지 않는다 */
    const char *ip;                     /* 클라이언트 IP 문자열 */
    const struct http_message *msg;     /* 파싱한 요청 */
    const char *raw;                    /* 원본 요청 바이트 (본문 포함, 프록시가 쓴다) */
    size_t raw_len;                     /* = msg->total_len */
    int keep_alive;                     /* 응답의 Connection 헤더 값 */
    const struct route *route;          /* 라우터가 고른 라우트 */
    const struct server_config *cfg;    /* 설정 (루트 · 타임아웃 등, 읽기 전용) */
    const char *extra;                  /* 백엔드 응답에 덧붙일 헤더 줄. 없으면 NULL */
    /* 핸들러가 채워 돌려주는 칸 (접근 로그용, Q5 지도 Q3) */
    int status;                         /* 클라이언트에게 보낸 상태 코드 */
    size_t bytes;                       /* 클라이언트에게 보낸 본문 바이트 수 */
    char upstream[24];                  /* 프록시가 고른 백엔드 "IP:포트" (정적이면 빈 문자열) */
};

// 핸들러: 요청 하나를 처리해 응답을 보낸다. 0 = 계속 가능, -1 = 연결을 닫아야 함
typedef int (*handler_fn)(struct request_ctx *ctx);

#endif
