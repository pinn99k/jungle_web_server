// gate 단위 테스트: 순서 · 안내 시간 · 늦음 · 의자 · 과목별 자리 · 마감 기억
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "server/gate.h"
#include "tests/test.h"

static struct gate_rule R;

// 규칙과 줄을 처음 상태로 되돌린다
// 이유: 앞 테스트의 줄이 다음 테스트에 남지 않게 한다
static void reset(int slots, int chairs)
{
    struct gate_rule r = {slots, chairs, 100, 1000, 0, 0, 0};
    R = r;
    gate_reset();
}

// 번호표 p로 들어가 본다. 처음이면 p를 비워 부른다
// 이유: 테스트 한 줄에 "누가 · 언제 · 결과"만 보이게 한다
static enum gate_result enter(struct gate_pass *p, int course, long now)
{
    char t[GATE_TICKET_LEN + 1];
    strcpy(t, p->ticket);
    return gate_enter(t[0] ? t : NULL, course, &R, now, p);
}

// 자리가 비어 있으면 바로, 다 차면 안내 시간과 함께 기다리게 하는지 본다
// 이유: 안내 시간 = 앞에 남은 순번 ÷ (자리 ÷ 한 건 시간)
static void test_direct_and_wait(void)
{
    struct gate_pass p[12];
    memset(p, 0, sizeof p);
    reset(2, 0);
    CHECK_INT(enter(&p[0], -1, 1000), GATE_PASS);
    CHECK_INT(enter(&p[1], -1, 1000), GATE_PASS);
    for (int i = 2; i < 12; i++)
        CHECK_INT(enter(&p[i], -1, 1000), GATE_WAIT);
    CHECK_INT(p[11].ahead, 10);
    CHECK_INT(p[11].wait_ms, 500);      /* 10명 ÷ (자리 2 ÷ 100ms) */
    CHECK_INT(p[2].wait_ms, 200);
}

// 다시 눌러도 순번이 그대로이고, 앞사람보다 먼저 들어가지 못하는지 본다
// 이유: 다시 누르는 것이 유리하지도 불리하지도 않아야 한다
static void test_press_no_gain(void)
{
    struct gate_pass a = {0}, b = {0}, c = {0};
    char mine[GATE_TICKET_LEN + 1];
    reset(1, 0);
    CHECK_INT(enter(&a, -1, 0), GATE_PASS);
    enter(&b, -1, 0);
    enter(&c, -1, 0);
    strcpy(mine, c.ticket);
    for (int i = 0; i < 20; i++)
        CHECK_INT(enter(&c, -1, 1), GATE_WAIT);
    CHECK_STR(c.ticket, mine);
    gate_leave(&a, &R, 200, 100, 101);
    CHECK_INT(enter(&c, -1, 101), GATE_WAIT);
    CHECK_INT(enter(&b, -1, 102), GATE_PASS);
}

// 오라고 한 시각보다 grace_ms 넘게 늦으면 새 순번을 받는지 본다
// 이유: 핑구 규칙 "떠났으면 다음 사람 것"
static void test_late(void)
{
    struct gate_pass a = {0}, b = {0}, c = {0};
    char old[GATE_TICKET_LEN + 1];
    reset(1, 0);
    enter(&a, -1, 0);
    enter(&b, -1, 0);
    enter(&c, -1, 0);
    strcpy(old, c.ticket);
    enter(&c, -1, c.wait_ms + 1000);
    CHECK_STR(c.ticket, old);
    strcpy(old, b.ticket);
    enter(&b, -1, b.wait_ms + 1001);
    CHECK(strcmp(old, b.ticket) != 0);
    CHECK_INT(b.fresh, 1);
}

// 안에 있는 번호표는 BUSY, 끝나면 줄 끝부터 / 이어서 / 서버 실패면 순번 유지인지 본다
// 이유: 두 번 처리하지 않고, 실패한 사람은 순번을 지켜 준다
static void test_after_leave(void)
{
    struct gate_pass a = {0};
    char old[GATE_TICKET_LEN + 1];
    reset(2, 0);
    enter(&a, -1, 0);
    strcpy(old, a.ticket);
    struct gate_pass dup = a;
    CHECK_INT(enter(&dup, -1, 1), GATE_BUSY);
    gate_leave(&a, &R, 0, 100, 10);
    CHECK_INT(enter(&a, -1, 20), GATE_PASS);
    CHECK_STR(a.ticket, old);
    gate_leave(&a, &R, 200, 100, 30);
    CHECK_INT(enter(&a, -1, 40), GATE_PASS);
    CHECK(strcmp(old, a.ticket) != 0);
    R.keep = 1;
    strcpy(old, a.ticket);
    gate_leave(&a, &R, 200, 100, 50);
    CHECK_INT(enter(&a, -1, 60), GATE_PASS);
    CHECK_STR(a.ticket, old);
}

// 지어낸 번호표로는 남의 순번을 쓸 수 없는지 본다
// 이유: 순번은 짐작할 수 있으므로 비밀 값이 지켜야 한다
static void test_fake(void)
{
    struct gate_pass a = {0}, fake = {0};
    reset(1, 0);
    enter(&a, -1, 0);
    strcpy(fake.ticket, "00000001aaaaaaaaaaaaaaaaaaaaaaaa");
    CHECK_INT(enter(&fake, -1, 0), GATE_WAIT);
    CHECK_INT(fake.fresh, 1);
    CHECK(strncmp(fake.ticket, "00000002", 8) == 0);
}

// 마감을 기억하는 동안 백엔드에 보내지 않고 FULL, 지나면 다시 보내는지 본다
// 이유: 마감된 과목에 자리를 쓰지 않는다 (X4에서 켜고 끈다)
static void test_soldout(void)
{
    struct gate_pass a = {0}, b = {0};
    reset(2, 0);
    R.soldout_ms = 1000;
    enter(&a, 3, 0);
    gate_leave(&a, &R, 409, 100, 100);
    CHECK_INT(enter(&b, 3, 200), GATE_FULL);
    CHECK_INT(enter(&b, 4, 200), GATE_PASS);
    gate_leave(&b, &R, 200, 100, 300);
    CHECK_INT(enter(&b, 3, 1100), GATE_PASS);
}

static struct gate_pass sitter[3];
static enum gate_result sitter_result[3];

// 의자에 앉는 손님 한 명
// 이유: 의자는 다른 스레드가 자리를 돌려줘야 풀리므로 스레드가 필요하다
static void *sit_thread(void *arg)
{
    int i = (int)((struct gate_pass *)arg - sitter);
    sitter_result[i] = enter(&sitter[i], i == 0 ? 7 : 8, 0);
    return NULL;
}

// 밀리초만큼 쉰다
// 이유: 스레드가 의자에 앉을 시간을 준다
static void nap(int ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

// 의자에 앉은 사람이 자리가 비자마자 순번대로 들어가고, 과목별 자리가 찬 과목은 건너뛰는지 본다
// 이유: 자리가 놀지 않아야 하고, 한 과목이 다른 과목을 막지 않아야 한다
static void test_chairs(void)
{
    pthread_t t[3];
    struct gate_pass a = {0}, b = {0};
    reset(2, 3);
    R.course_slots = 1;
    memset(sitter, 0, sizeof sitter);
    CHECK_INT(enter(&a, 7, 0), GATE_PASS);
    CHECK_INT(enter(&b, 9, 0), GATE_PASS);
    for (int i = 0; i < 2; i++) {
        pthread_create(&t[i], NULL, sit_thread, &sitter[i]);
        nap(50);
    }
    gate_leave(&b, &R, 200, 100, 100);  /* 과목 7은 이미 1명 처리 중 → 뒤에 앉은 과목 8이 들어감 */
    pthread_join(t[1], NULL);
    CHECK_INT(sitter_result[1], GATE_PASS);
    gate_leave(&a, &R, 200, 100, 120);
    pthread_join(t[0], NULL);
    CHECK_INT(sitter_result[0], GATE_PASS);
}

// 자리가 안 나면 2초 뒤 WAIT으로 돌려보내는지 본다
// 이유: 손님의 3초 새로고침보다 먼저 답해야 한다
static void test_chair_timeout(void)
{
    struct gate_pass a = {0}, b = {0};
    reset(1, 1);
    enter(&a, -1, 0);
    struct timespec s, e;
    clock_gettime(CLOCK_MONOTONIC, &s);
    CHECK_INT(enter(&b, -1, 0), GATE_WAIT);
    clock_gettime(CLOCK_MONOTONIC, &e);
    long ms = (e.tv_sec - s.tv_sec) * 1000 + (e.tv_nsec - s.tv_nsec) / 1000000;
    CHECK(ms >= 1990 && ms < 2500);
}

// 모든 테스트를 실행하고, 하나라도 실패하면 종료 코드 1을 반환한다
// 이유: make test가 종료 코드로 성공 · 실패를 판단한다
int main(void)
{
    RUN(test_direct_and_wait);
    RUN(test_press_no_gain);
    RUN(test_late);
    RUN(test_after_leave);
    RUN(test_fake);
    RUN(test_soldout);
    RUN(test_chairs);
    RUN(test_chair_timeout);
    return TEST_EXIT();
}
