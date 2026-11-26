// 프록시 경로 검사 단위 테스트
#include "server/proxy.h"
#include "tests/test.h"

// 상위로 올라가는 조각을 인코딩 · 세미콜론 · 역슬래시로 숨겨도 찾는지 본다
// 이유: 라우터와 백엔드가 경로를 다르게 읽는 틈을 막는다
static void test_escapes(void)
{
    CHECK_INT(proxy_path_escapes("/api/../admin"), 1);
    CHECK_INT(proxy_path_escapes("/api/.."), 1);
    CHECK_INT(proxy_path_escapes("/api/%2e%2E/admin"), 1);
    CHECK_INT(proxy_path_escapes("/api/..%2fadmin"), 1);
    CHECK_INT(proxy_path_escapes("/api/..%5cadmin"), 1);
    CHECK_INT(proxy_path_escapes("/api/..;a=1/admin"), 1);
    CHECK_INT(proxy_path_escapes("/api/..;"), 1);
    CHECK_INT(proxy_path_escapes("/../api/x?y"), 1);
}

// 이름이나 쿼리에 점이 있을 뿐인 경로는 통과하는지 본다
// 이유: 정상 요청을 거부하면 안 된다
static void test_normal(void)
{
    CHECK_INT(proxy_path_escapes("/api/time"), 0);
    CHECK_INT(proxy_path_escapes("/api/a..b"), 0);
    CHECK_INT(proxy_path_escapes("/api/..."), 0);
    CHECK_INT(proxy_path_escapes("/api/./x"), 0);
    CHECK_INT(proxy_path_escapes("/api/x?q=../y"), 0);
    CHECK_INT(proxy_path_escapes("/api/x;..;/y"), 0);
    CHECK_INT(proxy_path_escapes("/api/%zz%2"), 0);
    CHECK_INT(proxy_path_escapes("/"), 0);
}

// 모든 테스트를 실행하고, 하나라도 실패하면 종료 코드 1을 반환한다
// 이유: make test가 종료 코드로 성공 · 실패를 판단한다
int main(void)
{
    RUN(test_escapes);
    RUN(test_normal);
    return TEST_EXIT();
}
