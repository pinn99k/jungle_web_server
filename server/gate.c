// 줄 서기: 번호판(밖에서 기다림)과 의자(안에서 기다림). 자리가 비자마자 순번이 빠른 사람을 들인다 (스텝 18)
// 번호표는 순번과 비밀 값일 뿐이라 누구인지 모른다. 누구인지 판단하는 것은 백엔드의 일이다
#include "server/gate.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common/net.h"

#define TICKETS 65536                   /* 순번을 이 수로 나눈 나머지가 배열 칸 */
#define COURSES 128
#define ORDER_LEN 8
#define SECRET_LEN (GATE_TICKET_LEN - ORDER_LEN)
#define CHAIR_WAIT_MS 2000
#define MIN_WAIT_MS 200
#define MAX_WAIT_MS 10000
#define FIRST_SERVICE_MS 100            /* 아직 재지 못했을 때의 한 건 시간 */
#define CHAIR_SLOTS (GATE_CHAIRS_MAX * 2) /* 부름 받고 아직 일어나는 중인 의자까지 */

enum state { NONE, OUTSIDE, SEATED, RUNNING };

// 번호표 한 장
struct entry {
    enum state state;
    unsigned order;
    char secret[SECRET_LEN + 1];
    long due;                           /* 다시 오라고 한 시각 */
};

// 의자 하나. 앉은 워커는 cond에서 잔다
struct chair {
    int used;
    int granted;
    unsigned order;
    int course;
    pthread_cond_t cond;
};

// 줄 전체의 상태. 여러 값을 함께 바꾸므로 lock 하나로 지킨다
static struct {
    unsigned issued;                    /* 지금까지 발급한 순번 */
    double board;                       /* 번호판: 이 순번까지 불렀다 */
    long moved_at;
    double service_ms;                  /* 한 건에 걸린 시간의 이동 평균 */
    int running;
    int seated;
    int course_running[COURSES];
    int course_seated[COURSES];
    long soldout_until[COURSES];
} g;
static struct entry tickets[TICKETS];
static struct chair chairs[CHAIR_SLOTS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;

// 의자의 조건 변수를 거꾸로 가지 않는 시계로 만든다
// 이유: 시스템 시계가 바뀌어도 "최대 2초"가 어긋나지 않아야 한다
static void init_chairs(void)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    for (int i = 0; i < CHAIR_SLOTS; i++)
        pthread_cond_init(&chairs[i].cond, &attr);
    pthread_condattr_destroy(&attr);
}

// 추측할 수 없는 글자 SECRET_LEN개를 out에 적는다. 성공 0, 실패 -1
// 이유: 순번은 짐작할 수 있으므로, 남의 순번을 쓰지 못하게 비밀 값을 붙인다
static int make_secret(char out[SECRET_LEN + 1])
{
    unsigned char raw[SECRET_LEN / 2];
    size_t got = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    while (fd >= 0 && got < sizeof raw) {
        ssize_t r = net_read(fd, raw + got, sizeof raw - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    if (fd >= 0)
        close(fd);
    for (size_t i = 0; i < sizeof raw; i++)
        snprintf(out + i * 2, 3, "%02x", raw[i]);
    return got == sizeof raw ? 0 : -1;
}

// 번호표로 칸을 찾는다. 없거나 비밀 값이 다르거나 너무 늦게 왔으면 NULL
// 이유: "떠났으면 다음 사람 것". 오라고 한 시각에서 grace_ms가 지나면 순번을 잃는다 (핑구 규칙)
static struct entry *find(const char *ticket, const struct gate_rule *rule, long now)
{
    char head[ORDER_LEN + 1];
    if (ticket == NULL || strlen(ticket) != GATE_TICKET_LEN)
        return NULL;
    memcpy(head, ticket, ORDER_LEN);
    head[ORDER_LEN] = '\0';
    unsigned order = (unsigned)strtoul(head, NULL, 16);
    struct entry *e = &tickets[order % TICKETS];
    if (e->state == NONE || e->order != order || strcmp(e->secret, ticket + ORDER_LEN) != 0)
        return NULL;
    if (e->state == OUTSIDE && now - e->due > rule->grace_ms) {
        e->state = NONE;
        return NULL;
    }
    return e;
}

// 다음 순번의 번호표를 발급한다. 그 칸의 옛 번호표가 아직 안에 있으면 NULL
// 이유: 발급한 순간의 순서가 앞뒤를 정한다. 칸을 다시 쓸 때는 65,536장 전의 번호표만 지워진다
static struct entry *issue(const char *secret, long now)
{
    struct entry *e = &tickets[(g.issued + 1) % TICKETS];
    if (e->state == SEATED || e->state == RUNNING)
        return NULL;
    e->order = ++g.issued;
    memcpy(e->secret, secret, sizeof e->secret);
    e->state = OUTSIDE;
    e->due = now;
    return e;
}

// 1밀리초에 부를 순번 수를 반환한다
// 이유: 자리 ÷ 한 건 시간이 처리량이고, 안 오는 사람만큼 더 빨리 부른다(overbook)
static double call_rate(const struct gate_rule *rule)
{
    return rule->slots * (rule->overbook / 100.0) / g.service_ms;
}

// 흐른 시간만큼 번호판을 올린다. 의자가 꽉 찼으면 멈추고, 빈 곳보다 앞서 부르지 않는다
// 이유: 너무 많이 부르면 연결이 넘치고, 덜 부르면 자리가 논다
static void move_board(const struct gate_rule *rule, long now)
{
    int room = rule->slots + rule->chairs - g.running - g.seated;
    double top = g.issued + (double)(room > 0 ? room : 0);
    if ((g.seated < rule->chairs || g.running < rule->slots) && now > g.moved_at)
        g.board += (double)(now - g.moved_at) * call_rate(rule);
    if (g.board > top)
        g.board = top;
    if (now > g.moved_at)
        g.moved_at = now;
}

// 이 과목이 지금 자리를 하나 더 써도 되면 1
// 이유: 같은 과목은 백엔드에서 한 명씩 처리되므로, 한 과목이 자리를 다 붙잡으면 다른 과목이 막힌다
static int course_ok(const struct gate_rule *rule, int course)
{
    return rule->course_slots == 0 || course < 0 || g.course_running[course] < rule->course_slots;
}

// 앉을 수 있으면 1. 과목별 자리를 켰으면 한 과목이 의자의 절반까지만 쓴다
// 이유: 자리만 나누고 의자를 안 나누면 한 과목이 의자를 다 채워 똑같이 막힌다
static int chair_ok(const struct gate_rule *rule, int course)
{
    if (g.seated >= rule->chairs)
        return g.seated == 0 && g.running < rule->slots;
    return rule->course_slots == 0 || course < 0 || g.course_seated[course] < rule->chairs / 2 + 1;
}

// 빈 자리가 있는 동안 앉은 사람 중 순번이 가장 빠른 사람을 들인다
// 이유: 자리가 비는 순간 바로 채워야 자리가 놀지 않는다. 순번대로라 다시 눌러도 유리하지 않다
static void dispatch(const struct gate_rule *rule)
{
    while (g.running < rule->slots) {
        struct chair *best = NULL;
        for (int i = 0; i < CHAIR_SLOTS; i++) {
            struct chair *c = &chairs[i];
            if (c->used && !c->granted && course_ok(rule, c->course)
                && (best == NULL || c->order < best->order))
                best = c;
        }
        if (best == NULL)
            return;
        best->granted = 1;
        g.seated--;
        g.running++;
        if (best->course >= 0) {
            g.course_seated[best->course]--;
            g.course_running[best->course]++;
        }
        pthread_cond_signal(&best->cond);
    }
}

// 의자에 앉아 자리를 기다린다(락을 쥔 채 부름). 들어갔으면 1, 2초가 지났으면 0
// 이유: 부른 사람은 연결을 쥐고 기다려야 자리가 빈 순간 바로 들어간다
static int sit(struct entry *e, int course, const struct gate_rule *rule)
{
    struct chair *c = chairs;
    while (c < chairs + CHAIR_SLOTS && c->used)
        c++;
    if (c == chairs + CHAIR_SLOTS)
        return 0;
    struct timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += CHAIR_WAIT_MS / 1000;
    c->used = 1;
    c->granted = 0;
    c->order = e->order;
    c->course = course;
    e->state = SEATED;
    g.seated++;
    if (course >= 0)
        g.course_seated[course]++;
    dispatch(rule);
    int rc = 0;
    while (!c->granted && rc != ETIMEDOUT)
        rc = pthread_cond_timedwait(&c->cond, &lock, &until);
    c->used = 0;
    if (!c->granted) {
        g.seated--;
        if (course >= 0)
            g.course_seated[course]--;
    }
    e->state = c->granted ? RUNNING : OUTSIDE;
    return c->granted;
}

// 기다리라는 안내(앞에 남은 순번 · 다시 올 때까지의 시간)를 정한다
// 이유: 언제 다시 올지 알려 줘야 계속 누를 이유가 없고, 오라고 한 시각이 있어야 늦었는지 안다
static void tell_wait(struct entry *e, const struct gate_rule *rule, long now, struct gate_pass *out)
{
    double ahead = e->order - g.board;
    out->ahead = ahead > 0 ? (int)ahead : 0;
    double ms = out->ahead / call_rate(rule);
    out->wait_ms = ms < MIN_WAIT_MS ? MIN_WAIT_MS : ms > MAX_WAIT_MS ? MAX_WAIT_MS : (int)ms;
    e->due = now + out->wait_ms;
}

// 번호표를 확인하거나 발급하고, 부른 순번이면 들인다. 락을 쥔 채 부른다
// 이유: gate_enter에서 잠그고 푸는 일을 떼어 내, 나가는 길마다 풀기를 빠뜨리지 않게 한다
static enum gate_result enter_locked(const char *ticket, const char *secret, int course,
                                     const struct gate_rule *rule, long now, struct gate_pass *out)
{
    if (g.service_ms <= 0) {            /* 처음: 빈 곳만큼은 이미 부른 것으로 시작 */
        g.service_ms = FIRST_SERVICE_MS;
        g.board = rule->slots + rule->chairs;
    }
    move_board(rule, now);
    if (course >= 0 && now < g.soldout_until[course])
        return GATE_FULL;
    struct entry *e = find(ticket, rule, now);
    out->fresh = e == NULL;
    if (e == NULL)
        e = issue(secret, now);
    if (e == NULL) {
        out->wait_ms = MAX_WAIT_MS;
        return GATE_WAIT;
    }
    snprintf(out->ticket, sizeof out->ticket, "%08x%s", e->order, e->secret);
    if (e->state != OUTSIDE)
        return GATE_BUSY;
    if (e->order <= g.board) {
        if (chair_ok(rule, course) && sit(e, course, rule))
            return GATE_PASS;
        // 이어서는 번호표 1장이 여러 건이라 너무 많이 부르게 된다. 부른 사람이 앉지 못하면 번호판을 잠깐 멈춘다
        if (rule->keep && g.moved_at < now + MIN_WAIT_MS)
            g.moved_at = now + MIN_WAIT_MS;
    }
    // 의자에서 2초를 보냈을 수 있으므로 시각은 늦은 쪽으로 다시 맞춘다
    tell_wait(e, rule, now > g.moved_at ? now : g.moved_at, out);
    return GATE_WAIT;
}

// 번호표(NULL이나 모르는 것이면 새로 발급)로 들어간다. PASS · WAIT · BUSY · FULL
// 이유: 밖에서 기다리는 사람은 연결도 워커도 쓰지 않고, 부른 사람만 의자에서 연결을 쥐고 기다린다
enum gate_result gate_enter(const char *ticket, int course, const struct gate_rule *rule, long now,
                            struct gate_pass *out)
{
    char secret[SECRET_LEN + 1];
    memset(out, 0, sizeof *out);
    out->course = course >= 0 && course < COURSES ? course : -1;
    out->wait_ms = MAX_WAIT_MS;
    pthread_once(&once, init_chairs);
    if (make_secret(secret) < 0)
        return GATE_WAIT;
    pthread_mutex_lock(&lock);
    enum gate_result r = enter_locked(ticket, secret, out->course, rule, now, out);
    pthread_mutex_unlock(&lock);
    return r;
}

// 들어갔던 일이 끝났음을 알린다. 자리를 돌려주고 평균 시간 · 마감 기억을 고친 뒤 다음 사람을 들인다
// 이유: 서버 잘못으로 답을 못 받은 사람은 순번을 지켜 주고, 끝난 사람은 규칙(줄 끝부터 · 이어서)을 따른다
void gate_leave(const struct gate_pass *p, const struct gate_rule *rule, int status, int took_ms,
                long now)
{
    pthread_mutex_lock(&lock);
    g.running--;
    if (p->course >= 0)
        g.course_running[p->course]--;
    if (status == 200 || status == 409)
        g.service_ms = g.service_ms * 0.95 + (took_ms > 0 ? took_ms : 1) * 0.05;
    if (status == 409 && p->course >= 0 && rule->soldout_ms > 0)
        g.soldout_until[p->course] = now + rule->soldout_ms;
    struct entry *e = find(p->ticket, rule, now);
    if (e != NULL && e->state == RUNNING) {
        int answered = status >= 200 && status < 500;
        e->state = answered && !rule->keep ? NONE : OUTSIDE;
        e->due = now;
    }
    dispatch(rule);
    pthread_mutex_unlock(&lock);
}

// 줄을 비운다 (테스트용)
// 이유: 단위 테스트마다 빈 줄에서 시작한다
void gate_reset(void)
{
    pthread_mutex_lock(&lock);
    memset(&g, 0, sizeof g);
    memset(tickets, 0, sizeof tickets);
    pthread_mutex_unlock(&lock);
}
