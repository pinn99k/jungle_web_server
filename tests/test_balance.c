// balance 단위 테스트. 상태가 전역이라 테스트마다 다른 라우트 번호를 쓴다
#include <string.h>

#include "server/balance.h"
#include "tests/test.h"

static struct server_config C;

// n번 골라 백엔드별로 뽑힌 수를 count에 센다. 고른 요청은 바로 끝난 것으로 한다
// 이유: "고르게 나뉘었나"를 한 줄로 확인한다
static void deal(int route, int n, int times, int *count)
{
    for (int i = 0; i < times; i++) {
        int b = balance_pick(&C, route, n, 0, 1000);
        balance_begin(route, b);
        balance_end(&C, route, b, 0, 1000);
        count[b]++;
    }
}

// 설정을 기본값으로 되돌린다
// 이유: 앞 테스트의 정책이 다음 테스트에 남지 않게 한다
static void reset(enum balance_policy p)
{
    config_defaults(&C);
    C.balance = (int)p;
}

// 번갈아 · 고르게는 서버 셋에 똑같이 나누는지 본다
// 이유: 평소 상황의 기본 동작이다
static void test_round_robin(void)
{
    int count[3] = {0};
    reset(BALANCE_RR);
    deal(0, 3, 300, count);
    CHECK_INT(count[0], 100);
    CHECK_INT(count[2], 100);
}

// rr은 실패한 몫을 다음 한 대로, even은 남은 서버에 나누는지 본다
// 이유: 서버가 죽었을 때 한 대에 몰리느냐가 갈린다
static void test_spill(void)
{
    reset(BALANCE_RR);
    CHECK_INT(balance_pick(&C, 1, 3, 1u << 0, 0), 1);
    CHECK_INT(balance_pick(&C, 1, 3, 1u << 2, 0), 0);
    reset(BALANCE_EVEN);
    int count[3] = {0};
    for (int i = 0; i < 200; i++)
        count[balance_pick(&C, 2, 3, 1u << 0, 0)]++;
    CHECK_INT(count[0], 0);
    CHECK_INT(count[1], 100);
    CHECK_INT(count[2], 100);
}

// 처리 중인 수가 가장 적은 서버를 고르는지 본다
// 이유: 무거운 요청을 쥔 서버로 더 보내지 않게 한다
static void test_least(void)
{
    reset(BALANCE_LEAST);
    balance_begin(3, 0);
    balance_begin(3, 0);
    balance_begin(3, 1);
    for (int i = 0; i < 3; i++)
        CHECK_INT(balance_pick(&C, 3, 3, 0, 0), 2);
    balance_begin(3, 2);
    CHECK(balance_pick(&C, 3, 3, 0, 0) != 0);
}

// 이어서 실패한 서버를 빼고, 시간이 지나면 다시 쓰는지 본다
// 이유: 아픈 서버에 요청마다 부딪히지 않게 한다
static void test_eject(void)
{
    reset(BALANCE_EVEN);
    C.eject_fails = 2;
    C.eject_ms = 1000;
    balance_begin(4, 1);
    balance_end(&C, 4, 1, 1, 5000);
    int count[3] = {0};
    for (int i = 0; i < 30; i++)
        count[balance_pick(&C, 4, 3, 0, 5000)]++;
    CHECK(count[1] > 0);
    balance_begin(4, 1);
    balance_end(&C, 4, 1, 1, 5000);
    for (int i = 0; i < 30; i++)
        CHECK(balance_pick(&C, 4, 3, 0, 5500) != 1);
    memset(count, 0, sizeof count);
    for (int i = 0; i < 30; i++)
        count[balance_pick(&C, 4, 3, 0, 6100)]++;
    CHECK(count[1] > 0);
    // 모두 아프면 그래도 하나는 고른다
    for (int b = 0; b < 3; b++) {
        balance_begin(4, b);
        balance_end(&C, 4, b, 1, 7000);
        balance_begin(4, b);
        balance_end(&C, 4, b, 1, 7000);
    }
    CHECK(balance_pick(&C, 4, 3, 0, 7100) >= 0);
}

// 실패했다 돌아온 서버는 처음에는 조금만 받는지 본다
// 이유: 막 켜진 서버에 한꺼번에 몰리지 않게 한다
static void test_slow_start(void)
{
    reset(BALANCE_EVEN);
    C.slow_start_ms = 10000;
    balance_begin(5, 0);
    balance_end(&C, 5, 0, 1, 0);
    balance_begin(5, 0);
    balance_end(&C, 5, 0, 0, 1000);
    int count[2] = {0};
    for (int i = 0; i < 200; i++)
        count[balance_pick(&C, 5, 2, 0, 1100)]++;
    CHECK(count[0] < 30);
    memset(count, 0, sizeof count);
    for (int i = 0; i < 200; i++)
        count[balance_pick(&C, 5, 2, 0, 20000)]++;
    CHECK_INT(count[0], 100);
}

// 모든 테스트를 실행하고, 하나라도 실패하면 종료 코드 1을 반환한다
// 이유: make test가 종료 코드로 성공 · 실패를 판단한다
int main(void)
{
    RUN(test_round_robin);
    RUN(test_spill);
    RUN(test_least);
    RUN(test_eject);
    RUN(test_slow_start);
    return TEST_EXIT();
}
