#ifndef PROXY_H
#define PROXY_H

#include "server/handler.h"

// 프록시 핸들러. 스텝 7 전까지는 502를 보내는 임시 함수. 0 = 계속 가능, -1 = 닫아야 함
int proxy_handle(struct request_ctx *ctx);

#endif
