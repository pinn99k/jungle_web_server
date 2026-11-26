// mime_from_path 단위 테스트
#include "server/mime.h"
#include "tests/test.h"

// 명세가 이름을 든 다섯 종류(HTML, CSS, JS, PNG, JPG)가 맞는 타입으로 나오는지 본다
// 이유: 타입이 틀리면 브라우저가 파일을 다르게 처리한다
static void test_known_types(void)
{
    CHECK_STR(mime_from_path("/a/index.html"), "text/html; charset=utf-8");
    CHECK_STR(mime_from_path("/style.css"), "text/css; charset=utf-8");
    CHECK_STR(mime_from_path("/app.js"), "text/javascript; charset=utf-8");
    CHECK_STR(mime_from_path("/img.png"), "image/png");
    CHECK_STR(mime_from_path("/photo.jpg"), "image/jpeg");
    CHECK_STR(mime_from_path("/photo.JPEG"), "image/jpeg");
}

// 확장자가 없거나 모르는 경우, 점이 폴더 이름에만 있는 경우를 본다
// 이유: 모르면 내려받기 타입(octet-stream)으로 안전하게 처리해야 한다
static void test_unknown(void)
{
    CHECK_STR(mime_from_path("/README"), "application/octet-stream");
    CHECK_STR(mime_from_path("/a.zzz"), "application/octet-stream");
    CHECK_STR(mime_from_path("/v1.2/file"), "application/octet-stream");
}

// 모든 테스트를 실행하고, 하나라도 실패하면 종료 코드 1을 반환한다
// 이유: make test가 종료 코드로 성공 · 실패를 판단한다
int main(void)
{
    RUN(test_known_types);
    RUN(test_unknown);
    return TEST_EXIT();
}
