#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include "common/buf.h"

// 상태 코드에 맞는 사유구를 반환한다 (200 → "OK"). 모르는 코드면 "Unknown"
const char *http_reason(int status);

// 상태 줄과 표준 헤더(Date, Server, Content-Type, Content-Length, Connection)를 out에 붙인다. 성공 0, 실패 -1
// content_type이 NULL이거나 content_length가 음수면 그 헤더는 뺀다. extra는 "이름: 값\r\n" 줄들 (없으면 NULL)
int http_build_header(struct buf *out, int status, const char *content_type, long content_length,
                      int keep_alive, const char *extra);

// 짧은 HTML 본문을 포함한 오류 응답 전체를 out에 붙인다. 405면 Allow 헤더를 넣는다. 성공 0, 실패 -1
int http_build_error(struct buf *out, int status, int keep_alive);

#endif
