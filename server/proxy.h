#ifndef PROXY_H
#define PROXY_H

#include "server/handler.h"

// 프록시 핸들러: 백엔드를 골라 요청을 전달하고 응답을 중계한다 (실패 502, 시간 초과 504). 0 = 계속 가능, -1 = 닫아야 함
int proxy_handle(struct request_ctx *ctx);

#endif
