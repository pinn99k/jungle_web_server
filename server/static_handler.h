#ifndef STATIC_HANDLER_H
#define STATIC_HANDLER_H

#include <stddef.h>

#include "server/handler.h"

// 정적 파일 핸들러. 메서드 확인 → 경로 변환 → 파일 열기 → 헤더 · 본문 전송. 0 = 계속 가능, -1 = 닫아야 함
int static_handle(struct request_ctx *ctx);

// 요청 경로를 루트 안의 실제 파일 경로로 바꾼다. 성공 0, 실패 시 상태 코드(400/403/404/414/500)
// out은 PATH_MAX 이상이어야 한다
int static_resolve_path(const char *root, const char *target, char *out, size_t out_len);

#endif
