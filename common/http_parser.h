#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>

#define HTTP_METHOD_MAX 16              /* 메서드 이름 최대 글자 수 (넘으면 501) */
#define HTTP_LINE_MAX 8192              /* 요청 줄 상한 (넘으면 414) */
#define HTTP_HEADERS_MAX 8192           /* 헤더 전체 상한 (넘으면 431) */
#define HTTP_MAX_HEADERS 32             /* 헤더 개수 상한 (넘으면 431) */
#define HTTP_NAME_MAX 64                /* 헤더 이름 버퍼 크기 (NUL 포함) */
#define HTTP_VALUE_MAX 512              /* 헤더 값 버퍼 크기 (NUL 포함) */
#define HTTP_BODY_MAX (1024L * 1024L)   /* 요청 본문 상한 (넘으면 413) */

enum parse_mode { PARSE_REQUEST, PARSE_RESPONSE };
enum parse_result { PARSE_NEED_MORE, PARSE_DONE, PARSE_ERROR };

// 헤더 한 줄. 이름과 값은 수신 버퍼가 아니라 여기에 복사해 둔다
struct http_header {
    char name[HTTP_NAME_MAX];
    char value[HTTP_VALUE_MAX];
};

// 해석한 메시지 하나. 요청이면 method · target, 응답이면 status를 채운다
struct http_message {
    char method[HTTP_METHOD_MAX + 1];
    char target[HTTP_LINE_MAX];
    int status;
    int minor_version;                  /* HTTP/1.0 → 0, HTTP/1.1 → 1 */
    struct http_header headers[HTTP_MAX_HEADERS];
    int header_count;
    long content_length;                /* -1 = Content-Length 없음 */
    size_t header_len;                  /* 시작 줄부터 빈 줄까지의 길이 */
    size_t total_len;                   /* header_len + 본문 길이 = 메시지 하나의 전체 길이 */
    int keep_alive;
};

// 파서 상태. pos · headers_start는 메시지 시작부터의 거리라서 버퍼가 재할당돼도 유효하다
struct http_parser {
    enum parse_mode mode;
    int state;
    size_t pos;
    size_t headers_start;
    int error_status;                   /* PARSE_ERROR일 때 응답할 상태 코드 */
    struct http_message msg;
};

// 요청 모드 또는 응답 모드로 파서를 초기화한다
void http_parser_init(struct http_parser *p, enum parse_mode mode);

// 메시지 시작부터 쌓인 buf[0..len)을 지난번 위치부터 이어서 해석한다. NEED_MORE / DONE / ERROR
enum parse_result http_parse(struct http_parser *p, const char *buf, size_t len);

// 헤더 이름으로 값을 찾는다. 대소문자 무시. 없으면 NULL
const char *http_header_get(const struct http_message *m, const char *name);

#endif
