#ifndef ROUTER_H
#define ROUTER_H

#include "server/config.h"
#include "server/handler.h"

// 요청 경로와 가장 길게 일치하는 라우트를 반환한다 (경로 조각 단위). 없으면 NULL
const struct route *router_match(const struct server_config *c, const char *target);

// 라우트 종류에 맞는 핸들러 함수를 반환한다 (정적 → static_handle, 프록시 → proxy_handle)
handler_fn router_handler(const struct route *r);

#endif
