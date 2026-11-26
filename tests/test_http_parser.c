// http_parser 단위 테스트. 소켓 없이 바이트만 넣어 본다
#include "common/http_parser.h"
#include "tests/test.h"

// 구조체가 커서(약 27KB) 테스트마다 스택에 두지 않고 하나를 같이 쓴다
static struct http_parser P;

// 문자열 하나를 통째로 넣어 해석한다. 해석 결과를 반환
// 이유: 대부분의 테스트는 "한 번에 다 온 요청"이라 초기화와 해석을 한 줄로 줄인다
static enum parse_result parse_str(enum parse_mode mode, const char *s)
{
    http_parser_init(&P, mode);
    return http_parse(&P, s, strlen(s));
}

// 요청을 해석하면 오류가 나고 상태 코드가 status인지 확인한다
// 이유: 실패했을 때 어느 요청이었는지 앞부분을 함께 보여 주기 위해서다
static void expect_error(const char *req, int status)
{
    if (parse_str(PARSE_REQUEST, req) != PARSE_ERROR || P.error_status != status) {
        test_failures++;
        printf("  실패: 기대 %d, 결과 %d · 요청 \"%.40s\"\n", status, P.error_status, req);
    }
}

// head + 같은 글자 n개 + tail을 out에 만든다
// 이유: 상한(8KB 등)을 넘는 긴 요청을 손으로 쓰지 않고 만들기 위해서다
static void build(char *out, const char *head, char fill, size_t n, const char *tail)
{
    size_t h = strlen(head);
    memcpy(out, head, h);
    memset(out + h, fill, n);
    memcpy(out + h + n, tail, strlen(tail) + 1);
}

// 가장 단순한 GET이 메서드 · 대상 · 버전 · 헤더 · 길이로 나뉘는지 본다
// 이유: 다른 테스트의 기준이 되는 정상 경로
static void test_simple_get(void)
{
    const char *s = "GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK_INT(parse_str(PARSE_REQUEST, s), PARSE_DONE);
    CHECK_STR(P.msg.method, "GET");
    CHECK_STR(P.msg.target, "/index.html");
    CHECK_INT(P.msg.minor_version, 1);
    CHECK_STR(http_header_get(&P.msg, "host"), "x");
    CHECK_INT(P.msg.keep_alive, 1);
    CHECK_INT(P.msg.total_len, strlen(s));
}

// 1바이트씩 늘려 넣어도 마지막 바이트에서만 DONE이 나는지 본다
// 이유: read()가 아무 데서나 끊어 주는 상황을 가장 잘게 재현한다 (증분 파싱)
static void test_byte_by_byte(void)
{
    const char *s = "POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello";
    size_t n = strlen(s);
    http_parser_init(&P, PARSE_REQUEST);
    for (size_t i = 1; i < n; i++)
        CHECK_INT(http_parse(&P, s, i), PARSE_NEED_MORE);
    CHECK_INT(http_parse(&P, s, n), PARSE_DONE);
    CHECK_INT(P.msg.content_length, 5);
    CHECK_INT(P.msg.total_len, n);
}

// 요청 두 개가 붙어 와도 total_len으로 첫 요청의 끝을 정확히 찾는지 본다
// 이유: keep-alive에서 다음 요청은 total_len 뒤에서 시작한다 (스텝 5 buf_consume)
static void test_pipelining(void)
{
    const char *s = "GET /a HTTP/1.1\r\nHost: x\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK_INT(parse_str(PARSE_REQUEST, s), PARSE_DONE);
    size_t first = P.msg.total_len;
    CHECK_STR(P.msg.target, "/a");
    http_parser_init(&P, PARSE_REQUEST);
    CHECK_INT(http_parse(&P, s + first, strlen(s) - first), PARSE_DONE);
    CHECK_STR(P.msg.target, "/b");
}

// 잘못된 요청마다 정해진 상태 코드가 나오는지 본다
// 이유: design 8장 엣지 케이스와 오늘 정한 Q1~Q5를 한 곳에서 확인한다
static void test_errors(void)
{
    expect_error("GET / HTTP/1.1\r\n\r\n", 400);                              /* Host 없음 */
    expect_error("GET / HTTP/1.1\r\nHost : x\r\n\r\n", 400);                  /* 콜론 앞 공백 */
    expect_error("GET / HTTP/1.1\nHost: x\r\n\r\n", 400);                     /* \n만 (Q2) */
    expect_error("GET / HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n", 400);        /* 이어 쓰기 (Q5) */
    expect_error("get / HTTP/1.1\r\nHost: x\r\n\r\n", 400);                   /* 소문자 메서드 */
    expect_error("GET http://a/ HTTP/1.1\r\nHost: x\r\n\r\n", 400);           /* 경로 형식만 (Q4) */
    expect_error("GET / HTPP/1.1\r\nHost: x\r\n\r\n", 400);                   /* 버전 형식 */
    expect_error("GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505);
    expect_error("ABCDEFGHIJKLMNOPQ / HTTP/1.1\r\nHost: x\r\n\r\n", 501);     /* 17자 메서드 */
    expect_error("POST / HTTP/1.1\r\nHost: x\r\n\r\n", 411);
    expect_error("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\n\r\n", 413);
    expect_error("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 12a\r\n\r\n", 400);
    expect_error("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n"
                 "Content-Length: 1\r\n\r\na", 400);                          /* CL 중복 (Q3) */
    expect_error("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n"
                 "Transfer-Encoding: chunked\r\n\r\n", 400);                  /* CL + TE */
    expect_error("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", 501);
}

// 크기 상한(요청 줄 · 헤더 전체 · 값 · 이름 · 개수)을 넘으면 414 / 431이 나는지 본다
// 이유: 상한이 없으면 손님 하나가 서버 메모리를 끝없이 쓰게 할 수 있다
static void test_limits(void)
{
    static char s[16384];
    build(s, "GET /", 'a', 9000, " HTTP/1.1\r\n");
    expect_error(s, 414);
    build(s, "GET / HTTP/1.1\r\nHost: x\r\nX: ", 'a', 9000, "");
    expect_error(s, 431);                                                     /* 헤더 전체 8KB 초과 */
    build(s, "GET / HTTP/1.1\r\nHost: x\r\nX: ", 'a', 600, "\r\n\r\n");
    expect_error(s, 431);                                                     /* 값 512B 초과 */
    build(s, "GET / HTTP/1.1\r\nHost: x\r\n", 'N', 70, ": v\r\n\r\n");
    expect_error(s, 431);                                                     /* 이름 64B 초과 */

    char many[2048];
    int off = snprintf(many, sizeof many, "GET / HTTP/1.1\r\nHost: x\r\n");
    for (int i = 0; i < 32; i++)
        off += snprintf(many + off, sizeof many - (size_t)off, "A: b\r\n");
    snprintf(many + off, sizeof many - (size_t)off, "\r\n");
    expect_error(many, 431);                                                  /* 33개 */
}

// 처음 보는 메서드도 통과하고, GET 본문도 요청 경계에 들어가는지 본다
// 이유: Q1(처리 여부는 핸들러가)과 Q7(GET 본문은 길이만큼 읽음) 확인
static void test_methods_and_body(void)
{
    CHECK_INT(parse_str(PARSE_REQUEST, "DELETE /api/x HTTP/1.1\r\nHost: x\r\n\r\n"), PARSE_DONE);
    CHECK_STR(P.msg.method, "DELETE");
    const char *g = "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nab";
    CHECK_INT(parse_str(PARSE_REQUEST, g), PARSE_DONE);
    CHECK_INT(P.msg.total_len, strlen(g));
}

// 버전과 Connection 헤더 조합으로 keep-alive가 맞게 정해지는지 본다
// 이유: 1.0은 기본 닫기, 1.1은 기본 유지라서 네 경우가 다 다르다
static void test_keep_alive(void)
{
    parse_str(PARSE_REQUEST, "GET / HTTP/1.0\r\n\r\n");
    CHECK_INT(P.msg.keep_alive, 0);
    parse_str(PARSE_REQUEST, "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    CHECK_INT(P.msg.keep_alive, 1);
    parse_str(PARSE_REQUEST, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK_INT(P.msg.keep_alive, 0);
    parse_str(PARSE_REQUEST,
              "GET / HTTP/1.1\r\nHost: x\r\nConnection: Upgrade, Keep-Alive\r\n\r\n");
    CHECK_INT(P.msg.keep_alive, 1);
}

// 응답 모드에서 상태 코드와 본문 길이(없으면 연결 끝까지)를 맞게 다루는지 본다
// 이유: 스텝 7 프록시가 백엔드 응답을 이 모드로 해석한다
static void test_response_mode(void)
{
    const char *r = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc";
    CHECK_INT(parse_str(PARSE_RESPONSE, r), PARSE_DONE);
    CHECK_INT(P.msg.status, 200);
    CHECK_INT(P.msg.total_len, strlen(r));
    const char *nolen = "HTTP/1.0 502 Bad Gateway\r\n\r\npartial";
    CHECK_INT(parse_str(PARSE_RESPONSE, nolen), PARSE_DONE);
    CHECK_INT(P.msg.content_length, -1);
    CHECK_INT(P.msg.total_len, P.msg.header_len);
    CHECK_INT(parse_str(PARSE_RESPONSE, "HTTP/1.1 2x0 OK\r\n\r\n"), PARSE_ERROR);
}

// 모든 테스트를 실행하고, 하나라도 실패하면 종료 코드 1을 반환한다
// 이유: make test가 종료 코드로 성공 · 실패를 판단한다
int main(void)
{
    RUN(test_simple_get);
    RUN(test_byte_by_byte);
    RUN(test_pipelining);
    RUN(test_errors);
    RUN(test_limits);
    RUN(test_methods_and_body);
    RUN(test_keep_alive);
    RUN(test_response_mode);
    return TEST_EXIT();
}
