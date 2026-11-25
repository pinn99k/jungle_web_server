#include "common/http_response.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const struct {
    int code;
    const char *reason;
} REASONS[] = {
    {200, "OK"},
    {301, "Moved Permanently"},
    {400, "Bad Request"},
    {403, "Forbidden"},
    {404, "Not Found"},
    {405, "Method Not Allowed"},
    {408, "Request Timeout"},
    {409, "Conflict"},
    {411, "Length Required"},
    {413, "Content Too Large"},
    {414, "URI Too Long"},
    {431, "Request Header Fields Too Large"},
    {500, "Internal Server Error"},
    {501, "Not Implemented"},
    {502, "Bad Gateway"},
    {503, "Service Unavailable"},
    {504, "Gateway Timeout"},
    {505, "HTTP Version Not Supported"},
};

// 상태 코드에 맞는 사유구를 반환한다 (200 → "OK"). 모르는 코드면 "Unknown"
// 이유: 상태 줄 "HTTP/1.1 404 Not Found"의 뒷부분을 서버 · 백엔드가 같은 표로 쓰게 한다
const char *http_reason(int status)
{
    for (size_t i = 0; i < sizeof REASONS / sizeof REASONS[0]; i++) {
        if (REASONS[i].code == status)
            return REASONS[i].reason;
    }
    return "Unknown";
}

// printf 형식으로 만든 한 줄을 out 뒤에 붙인다. 성공 0, 실패(1KB 초과 · 메모리 부족) -1
// 이유: 헤더마다 "snprintf → 잘림 검사 → 붙이기"를 반복하지 않고, 잘림 검사를 한 곳에서 빠짐없이 한다
static int appendf(struct buf *out, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof line)
        return -1;
    return buf_append(out, line, (size_t)n);
}

// 상태 줄과 표준 헤더를 out에 붙인다. 성공 0, 실패 -1
// 이유: 모든 응답이 같은 헤더 순서 · 날짜 형식을 쓰게 해서 핸들러마다 헤더를 손으로 쓰지 않게 한다
int http_build_header(struct buf *out, int status, const char *content_type, long content_length,
                      int keep_alive, const char *extra)
{
    char date[64];
    time_t now = time(NULL);
    struct tm tm;
    // HTTP 날짜는 항상 GMT (RFC 7231 형식: Sun, 06 Nov 1994 08:49:37 GMT)
    if (gmtime_r(&now, &tm) == NULL
        || strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &tm) == 0)
        return -1;

    if (appendf(out, "HTTP/1.1 %d %s\r\nDate: %s\r\nServer: mini_web_server\r\n", status,
                http_reason(status), date) < 0)
        return -1;
    if (content_type != NULL && appendf(out, "Content-Type: %s\r\n", content_type) < 0)
        return -1;
    if (content_length >= 0 && appendf(out, "Content-Length: %ld\r\n", content_length) < 0)
        return -1;
    if (appendf(out, "Connection: %s\r\n", keep_alive ? "keep-alive" : "close") < 0)
        return -1;
    // extra는 Location처럼 1KB를 넘을 수 있어 형식 없이 그대로 붙인다
    if (extra != NULL && buf_append(out, extra, strlen(extra)) < 0)
        return -1;
    return buf_append(out, "\r\n", 2);
}

// 짧은 HTML 본문을 포함한 오류 응답 전체를 out에 붙인다. 405면 Allow 헤더를 넣는다. 성공 0, 실패 -1
// 이유: 브라우저 화면에 "404 Not Found"가 보이게 하고, 405에는 RFC가 요구하는 Allow를 빠뜨리지 않는다
int http_build_error(struct buf *out, int status, int keep_alive)
{
    char body[256];
    int blen = snprintf(body, sizeof body, "<html><body><h1>%d %s</h1></body></html>\n", status,
                        http_reason(status));
    if (blen < 0 || (size_t)blen >= sizeof body)
        return -1;
    const char *extra = status == 405 ? "Allow: GET, HEAD\r\n" : NULL;
    if (http_build_header(out, status, "text/html; charset=utf-8", blen, keep_alive, extra) < 0)
        return -1;
    return buf_append(out, body, (size_t)blen);
}
