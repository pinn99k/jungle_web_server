#include "common/http_parser.h"

#include <limits.h>
#include <string.h>
#include <strings.h>

enum { ST_START_LINE, ST_HEADERS, ST_BODY, ST_DONE, ST_ERROR };

// 요청 모드 또는 응답 모드로 파서를 초기화한다
// 이유: 연결 하나에서 요청이 이어 올 때마다 같은 파서를 깨끗한 상태로 다시 쓰기 위해서다
void http_parser_init(struct http_parser *p, enum parse_mode mode)
{
    memset(p, 0, sizeof *p);
    p->mode = mode;
    p->state = ST_START_LINE;
    p->msg.content_length = -1;
}

// 파서를 오류 상태로 바꾸고 응답할 상태 코드를 기록한다. 항상 PARSE_ERROR
// 이유: 오류가 나는 곳이 여러 군데라 "상태 바꾸기 + 코드 기록"을 한 곳에서 같게 처리한다
static enum parse_result fail(struct http_parser *p, int status)
{
    p->state = ST_ERROR;
    p->error_status = status;
    return PARSE_ERROR;
}

// from부터 \r\n 위치(\r의 자리)를 찾는다. 아직 없으면 -1, \r이나 \n이 따로 나오면 -2
// 이유: 줄 끝을 \r\n으로만 인정해야 앞뒤 서버가 같은 경계를 본다 (Q2: bare LF 거부)
static long find_crlf(const char *buf, size_t from, size_t len)
{
    for (size_t i = from; i < len; i++) {
        if (buf[i] == '\n')
            return -2;
        if (buf[i] == '\r') {
            if (i + 1 == len)
                return -1;
            return buf[i + 1] == '\n' ? (long)i : -2;
        }
    }
    return -1;
}

// "HTTP/x.y" 8글자를 읽어 minor 버전을 적는다. 성공 0, 형식 오류 400, 지원 안 하는 버전 505
// 이유: 요청 줄과 응답 상태 줄이 같은 버전 형식을 쓰므로 한 곳에서 검사한다
static int parse_version(const char *s, size_t n, int *minor)
{
    if (n != 8 || memcmp(s, "HTTP/", 5) != 0 || s[6] != '.' ||
        s[5] < '0' || s[5] > '9' || s[7] < '0' || s[7] > '9')
        return 400;
    if (s[5] != '1' || (s[7] != '0' && s[7] != '1'))
        return 505;
    *minor = s[7] - '0';
    return 0;
}

// 메서드, 요청 대상, 버전을 꺼낸다. 성공 0, 실패 시 상태 코드(400/414/501/505)
// 이유: 메서드는 형식만 보고 처리 여부는 핸들러가 정한다 (Q1). 대상은 /로 시작하는 경로만 받는다 (Q4)
static int parse_request_line(struct http_message *m, const char *line, size_t n)
{
    const char *sp1 = memchr(line, ' ', n);
    if (sp1 == NULL || sp1 == line)
        return 400;
    size_t mlen = (size_t)(sp1 - line);
    for (size_t i = 0; i < mlen; i++) {
        if (line[i] < 'A' || line[i] > 'Z')
            return 400;
    }
    if (mlen > HTTP_METHOD_MAX)
        return 501;

    const char *t = sp1 + 1;
    size_t rest = n - mlen - 1;
    const char *sp2 = memchr(t, ' ', rest);
    if (sp2 == NULL || sp2 == t || t[0] != '/')
        return 400;
    size_t tlen = (size_t)(sp2 - t);
    if (tlen >= sizeof m->target)
        return 414;
    for (size_t i = 0; i < tlen; i++) {
        if ((unsigned char)t[i] <= 0x20 || t[i] == 0x7f)
            return 400;
    }
    int rc = parse_version(sp2 + 1, rest - tlen - 1, &m->minor_version);
    if (rc != 0)
        return rc;

    memcpy(m->method, line, mlen);
    m->method[mlen] = '\0';
    memcpy(m->target, t, tlen);
    m->target[tlen] = '\0';
    return 0;
}

// 응답 모드에서 "HTTP/1.x 200 사유" 줄의 버전과 상태 코드를 꺼낸다. 성공 0, 실패 400
// 이유: 프록시가 백엔드 응답을 해석할 때 상태 코드와 버전이 필요하다 (스텝 7)
static int parse_status_line(struct http_message *m, const char *line, size_t n)
{
    if (n < 12 || line[8] != ' ' || parse_version(line, 8, &m->minor_version) != 0)
        return 400;
    int status = 0;
    for (size_t i = 9; i < 12; i++) {
        if (line[i] < '0' || line[i] > '9')
            return 400;
        status = status * 10 + (line[i] - '0');
    }
    if (n > 12 && line[12] != ' ')
        return 400;
    m->status = status;
    return 0;
}

// "이름: 값" 한 줄을 나눠 헤더 배열에 복사한다. 성공 0, 실패 시 상태 코드(400/431)
// 이유: 수신 버퍼는 realloc으로 옮겨질 수 있어서, 포인터 대신 고정 배열에 복사해 둔다
static int parse_header_line(struct http_message *m, const char *line, size_t n)
{
    // 줄 앞 공백 = 앞 헤더에 이어 쓰는 옛 형식. RFC 7230 권고대로 거부한다 (Q5)
    if (line[0] == ' ' || line[0] == '\t')
        return 400;
    const char *colon = memchr(line, ':', n);
    if (colon == NULL || colon == line)
        return 400;
    size_t nlen = (size_t)(colon - line);
    for (size_t i = 0; i < nlen; i++) {
        if ((unsigned char)line[i] <= 0x20 || line[i] == 0x7f)
            return 400;
    }

    const char *v = colon + 1;
    const char *end = line + n;
    while (v < end && (*v == ' ' || *v == '\t'))
        v++;
    while (end > v && (end[-1] == ' ' || end[-1] == '\t'))
        end--;
    size_t vlen = (size_t)(end - v);
    for (size_t i = 0; i < vlen; i++) {
        unsigned char c = (unsigned char)v[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f)
            return 400;
    }
    if (nlen >= HTTP_NAME_MAX || vlen >= HTTP_VALUE_MAX || m->header_count >= HTTP_MAX_HEADERS)
        return 431;

    struct http_header *h = &m->headers[m->header_count++];
    memcpy(h->name, line, nlen);
    h->name[nlen] = '\0';
    memcpy(h->value, v, vlen);
    h->value[vlen] = '\0';
    return 0;
}

// 숫자로만 된 문자열을 넘침 없이 정수로 바꾼다. 성공 0, 실패 -1
// 이유: atoi는 "12a"를 12로, 너무 큰 수를 엉뚱한 값으로 바꿔 본문 길이를 속일 수 있다
static int parse_content_length(const char *s, long *out)
{
    if (*s == '\0')
        return -1;
    long v = 0;
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9')
            return -1;
        int d = *s - '0';
        if (v > (LONG_MAX - d) / 10)
            return -1;
        v = v * 10 + d;
    }
    *out = v;
    return 0;
}

// Content-Length와 Transfer-Encoding으로 본문 길이를 정한다. 성공 0, 실패 시 상태 코드(400/411/413/501)
// 이유: 본문 길이가 곧 요청 경계라서, 해석이 갈릴 수 있는 조합(중복 CL, CL+TE)은 거부한다 (Q3)
static int resolve_body_length(struct http_parser *p)
{
    struct http_message *m = &p->msg;
    int cl_count = 0;
    int has_te = 0;
    const char *cl = NULL;
    for (int i = 0; i < m->header_count; i++) {
        if (strcasecmp(m->headers[i].name, "Content-Length") == 0) {
            cl_count++;
            cl = m->headers[i].value;
        } else if (strcasecmp(m->headers[i].name, "Transfer-Encoding") == 0) {
            has_te = 1;
        }
    }
    if (cl_count > 1)
        return 400;
    if (has_te)
        return cl_count ? 400 : 501;
    if (cl == NULL) {
        int is_post = p->mode == PARSE_REQUEST && strcmp(m->method, "POST") == 0;
        return is_post ? 411 : 0;
    }
    long v;
    if (parse_content_length(cl, &v) < 0)
        return 400;
    if (p->mode == PARSE_REQUEST && v > HTTP_BODY_MAX)
        return 413;
    m->content_length = v;
    return 0;
}

// 쉼표로 나뉜 목록 list에 token이 있는지 대소문자 무시로 확인한다. 있으면 1
// 이유: "Connection: keep-alive, Upgrade"처럼 값이 여러 개일 수 있다
static int has_token(const char *list, const char *token)
{
    size_t tlen = strlen(token);
    const char *s = list;
    while (*s != '\0') {
        while (*s == ' ' || *s == '\t' || *s == ',')
            s++;
        const char *e = s;
        while (*e != '\0' && *e != ',')
            e++;
        const char *t = e;
        while (t > s && (t[-1] == ' ' || t[-1] == '\t'))
            t--;
        if ((size_t)(t - s) == tlen && strncasecmp(s, token, tlen) == 0)
            return 1;
        s = e;
    }
    return 0;
}

// 버전과 Connection 헤더로 연결 유지 여부를 정한다
// 이유: HTTP/1.1은 기본이 유지, 1.0은 기본이 닫기라서 둘을 함께 봐야 한다
static void decide_keep_alive(struct http_message *m)
{
    const char *c = http_header_get(m, "Connection");
    int keep = m->minor_version == 1;
    if (c != NULL && has_token(c, "close"))
        keep = 0;
    else if (c != NULL && has_token(c, "keep-alive"))
        keep = 1;
    m->keep_alive = keep;
}

// 빈 줄을 만났을 때 헤더 전체를 검사하고 본문 길이 · keep-alive를 정한다. 성공 0, 실패 시 상태 코드
// 이유: Host 필수 · 본문 길이 · 연결 유지는 헤더가 모두 모여야 판단할 수 있다
static int end_of_headers(struct http_parser *p, size_t header_len)
{
    struct http_message *m = &p->msg;
    if (header_len - p->headers_start > HTTP_HEADERS_MAX)
        return 431;
    if (p->mode == PARSE_REQUEST && m->minor_version == 1 && http_header_get(m, "Host") == NULL)
        return 400;
    int rc = resolve_body_length(p);
    if (rc != 0)
        return rc;
    decide_keep_alive(m);
    m->header_len = header_len;
    return 0;
}

// 줄 끝이 아직 안 왔을 때 지금까지 쌓인 양이 상한을 넘었는지 본다. 넘으면 ERROR, 아니면 NEED_MORE
// 이유: 빈 줄 없이 끝없이 보내는 손님 때문에 버퍼가 무한히 커지는 것을 막는다 (페이즈 1 Q2)
static enum parse_result pending_limit(struct http_parser *p, size_t len)
{
    if (p->state == ST_START_LINE && len - p->pos > HTTP_LINE_MAX)
        return fail(p, 414);
    if (p->state == ST_HEADERS && len - p->headers_start > HTTP_HEADERS_MAX)
        return fail(p, 431);
    return PARSE_NEED_MORE;
}

// 메시지 시작부터 쌓인 buf[0..len)을 지난번 위치부터 이어서 해석한다. NEED_MORE / DONE / ERROR
// 이유: read()는 아무 데서나 끊어 주므로, 매번 처음부터 다시 보지 않고 멈춘 줄부터 이어 간다
enum parse_result http_parse(struct http_parser *p, const char *buf, size_t len)
{
    if (p->state == ST_ERROR)
        return PARSE_ERROR;
    struct http_message *m = &p->msg;

    while (p->state == ST_START_LINE || p->state == ST_HEADERS) {
        long eol = find_crlf(buf, p->pos, len);
        if (eol == -2)
            return fail(p, 400);
        if (eol < 0)
            return pending_limit(p, len);
        const char *line = buf + p->pos;
        size_t n = (size_t)eol - p->pos;
        size_t next = (size_t)eol + 2;
        int rc;
        if (p->state == ST_START_LINE) {
            if (n > HTTP_LINE_MAX)
                rc = 414;
            else if (p->mode == PARSE_REQUEST)
                rc = parse_request_line(m, line, n);
            else
                rc = parse_status_line(m, line, n);
            p->headers_start = next;
            p->state = ST_HEADERS;
        } else if (n == 0) {
            rc = end_of_headers(p, next);
            p->state = ST_BODY;
        } else {
            rc = parse_header_line(m, line, n);
        }
        if (rc != 0)
            return fail(p, rc);
        p->pos = next;
    }

    // 응답 모드에서 Content-Length가 없으면 본문은 연결이 닫힐 때까지라 헤더까지만 해석한다
    size_t body = m->content_length < 0 ? 0 : (size_t)m->content_length;
    if (len - m->header_len < body)
        return PARSE_NEED_MORE;
    m->total_len = m->header_len + body;
    p->state = ST_DONE;
    return PARSE_DONE;
}

// 헤더 이름으로 값을 찾는다. 대소문자 무시. 없으면 NULL
// 이유: HTTP 헤더 이름은 대소문자를 구분하지 않는다 (host, Host, HOST 모두 같음)
const char *http_header_get(const struct http_message *m, const char *name)
{
    for (int i = 0; i < m->header_count; i++) {
        if (strcasecmp(m->headers[i].name, name) == 0)
            return m->headers[i].value;
    }
    return NULL;
}
