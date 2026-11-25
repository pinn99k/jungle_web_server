// gate 단위 테스트: 순서 · 떠남 · 다시 누름 · 두 번 신청
#include <string.h>

#include "server/gate.h"
#include "tests/test.h"

static struct gate_rule R = {1, 1000, 0};

// 번호표로 한 번 들른다. 번호표는 t에 남긴다 (처음이면 빈 문자열을 준다)
// 이유: 테스트 한 줄에 "누가 · 언제 · 결과"만 보이게 한다
static enum gate_result visit(char t[GATE_TICKET_LEN + 1], long now, int *ahead)
{
    char out[GATE_TICKET_LEN + 1];
    enum gate_result r = gate_visit(t[0] ? t : NULL, &R, now, out, ahead);
    strcpy(t, out);
    return r;
}

// 처음 온 사람은 번호표만 받고, 다시 와서 들어가는지 본다
// 이유: 번호표를 실제로 들고 온 사람만 들인다
static void test_order(void)
{
    char a[33] = "", b[33] = "";
    int ahead;
    gate_reset();
    CHECK_INT(visit(a, 0, &ahead), GATE_WAIT);
    CHECK_INT((int)strlen(a), GATE_TICKET_LEN);
    CHECK_INT(visit(b, 10, &ahead), GATE_WAIT);
    CHECK_INT(ahead, 1);
    CHECK_INT(visit(b, 20, &ahead), GATE_WAIT);
    CHECK_INT(visit(a, 30, &ahead), GATE_PASS);
    CHECK_INT(visit(b, 40, &ahead), GATE_WAIT);
    gate_finish(a, 1);
    CHECK_INT(visit(b, 50, &ahead), GATE_PASS);
}

// 여러 번 다시 눌러도 앞사람보다 먼저 들어가지 못하는지 본다
// 이유: 다시 누르는 것이 유리하지 않아야 한다
static void test_press_no_gain(void)
{
    char calm[33] = "", press[33] = "";
    int ahead;
    gate_reset();
    visit(calm, 0, &ahead);
    visit(press, 1, &ahead);
    for (int i = 0; i < 20; i++)
        CHECK_INT(visit(press, 2 + i, &ahead), GATE_WAIT);
    CHECK_INT(visit(calm, 100, &ahead), GATE_PASS);
}

// 소식이 끊긴 사람의 자리는 다음 사람에게 가고, 돌아오면 새로 줄을 서는지 본다
// 이유: 핑구 규칙 "떠났으면 다음 사람 것"
static void test_leave(void)
{
    char a[33] = "", b[33] = "", old[33];
    int ahead;
    gate_reset();
    visit(a, 0, &ahead);
    visit(b, 0, &ahead);
    CHECK_INT(visit(b, 900, &ahead), GATE_WAIT);
    CHECK_INT(visit(b, 1500, &ahead), GATE_PASS);
    strcpy(old, a);
    CHECK_INT(visit(a, 2000, &ahead), GATE_WAIT);
    CHECK(strcmp(old, a) != 0);
}

// 처리 중이면 BUSY, 성공한 뒤면 DONE, 실패한 뒤면 새로 줄을 서는지 본다
// 이유: 두 번 처리되지 않게 하되, 실패한 사람은 다시 시도할 수 있어야 한다
static void test_busy_done(void)
{
    char a[33] = "", old[33];
    int ahead;
    gate_reset();
    visit(a, 0, &ahead);
    CHECK_INT(visit(a, 1, &ahead), GATE_PASS);
    CHECK_INT(visit(a, 2, &ahead), GATE_BUSY);
    gate_finish(a, 1);
    CHECK_INT(visit(a, 3, &ahead), GATE_DONE);
    gate_reset();
    a[0] = '\0';
    visit(a, 0, &ahead);
    visit(a, 1, &ahead);
    gate_finish(a, 0);
    strcpy(old, a);
    CHECK_INT(visit(a, 2, &ahead), GATE_WAIT);
    CHECK(strcmp(old, a) != 0);
}

// 자리(slots)만큼만 동시에 들이고, 모르는 번호표는 새로 발급하는지 본다
// 이유: 지어낸 번호표로 앞지르지 못해야 한다
static void test_slots_and_fake(void)
{
    char t[3][33] = {"", "", ""};
    char fake[33] = "0123456789abcdef0123456789abcdef";
    int ahead;
    gate_reset();
    R.slots = 2;
    for (int i = 0; i < 3; i++)
        visit(t[i], i, &ahead);
    CHECK_INT(visit(t[0], 10, &ahead), GATE_PASS);
    CHECK_INT(visit(t[1], 11, &ahead), GATE_PASS);
    CHECK_INT(visit(t[2], 12, &ahead), GATE_WAIT);
    CHECK_INT(visit(fake, 13, &ahead), GATE_WAIT);
    CHECK(strcmp(fake, "0123456789abcdef0123456789abcdef") != 0);
    R.slots = 1;
}

// 모든 테스트를 실행하고, 하나라도 실패하면 종료 코드 1을 반환한다
// 이유: make test가 종료 코드로 성공 · 실패를 판단한다
int main(void)
{
    RUN(test_order);
    RUN(test_press_no_gain);
    RUN(test_leave);
    RUN(test_busy_done);
    RUN(test_slots_and_fake);
    return TEST_EXIT();
}
