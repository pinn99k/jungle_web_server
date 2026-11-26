#ifndef PROXY_H
#define PROXY_H

#include "server/handler.h"

// 요청 경로에 상위로 올라가는 ..  조각이 있으면 1 (퍼센트 인코딩 · 세미콜론으로 숨긴 것 포함)
int proxy_path_escapes(const char *target);

// 프록시 핸들러: 백엔드를 골라 요청을 전달하고 응답을 중계한다 (실패 502, 시간 초과 504). 0 = 계속 가능, -1 = 닫아야 함
int proxy_handle(struct request_ctx *ctx);

#endif
