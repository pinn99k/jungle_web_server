// 백엔드 고르기: 번갈아 · 고르게 · 덜 바쁜 쪽, 아픈 서버 잠깐 빼기, 막 돌아온 서버 천천히 늘리기 (스텝 13)
// 여러 워커가 함께 보므로 모든 상태는 원자적 변수다. 설정(c)은 읽기만 한다
#include "server/balance.h"

#include <stdatomic.h>
#include <time.h>

static atomic_uint turns[CONFIG_MAX_ROUTES];
static atomic_int inflight[CONFIG_MAX_ROUTES][CONFIG_MAX_BACKENDS];  /* 보내 놓고 안 끝난 수 */
static atomic_int fails[CONFIG_MAX_ROUTES][CONFIG_MAX_BACKENDS];     /* 연속으로 실패한 횟수 */
static atomic_long down_until[CONFIG_MAX_ROUTES][CONFIG_MAX_BACKENDS]; /* 이 시각까지 뺀다 */
static atomic_long warm_from[CONFIG_MAX_ROUTES][CONFIG_MAX_BACKENDS];  /* 돌아온 시각. 0 = 평소 */

// 부팅 뒤 지난 밀리초를 반환한다
// 이유: 시스템 시계가 바뀌어도 거꾸로 가지 않아야 "언제까지 뺀다"가 어긋나지 않는다
long balance_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

// 돌아온 지 얼마 안 된 서버에 이번 차례(turn)를 줘도 되면 1
// 이유: 막 켜진 서버는 한가해 보여 몰리기 쉬우므로, 시간이 지난 만큼만 차례를 준다
static int warm_enough(const struct server_config *c, int route, int b, unsigned turn, long now)
{
    long from = atomic_load(&warm_from[route][b]);
    if (c->slow_start_ms == 0 || from == 0)
        return 1;
    long elapsed = now - from;
    if (elapsed >= c->slow_start_ms) {
        atomic_store(&warm_from[route][b], 0);
        return 1;
    }
    long percent = elapsed * 100 / c->slow_start_ms;
    return (long)(turn % 100) < (percent < 5 ? 5 : percent);
}

// 백엔드의 처리 슬롯(slots)이 꽉 찼으면 1
// 이유: 한 대가 견딜 만큼만 보내야 안에서 줄이 생기지 않는다. 남는 줄은 프록시가 세운다
static int is_full(const struct server_config *c, int route, int b)
{
    int slots = c->routes[route].slots;
    return slots > 0 && atomic_load(&inflight[route][b]) >= slots;
}

// 시도하지 않은 백엔드 중 조건에 맞는 것을 cand에 담고 개수를 반환한다. 처리 슬롯이 꽉 차서 뺀 수는 *full에
// 이유: 아픈 서버 · 막 돌아온 서버를 빼고도 하나도 안 남으면 조건을 풀어 다시 모아야 한다
static int collect(const struct server_config *c, int route, int n, unsigned tried, unsigned turn,
                   long now, int level, int *cand, int *full)
{
    int m = 0;
    *full = 0;
    for (int b = 0; b < n; b++) {
        if (tried & (1u << b))
            continue;
        if (level < 2 && atomic_load(&down_until[route][b]) > now)
            continue;
        if (level < 1 && !warm_enough(c, route, b, turn, now))
            continue;
        if (is_full(c, route, b))
            (*full)++;
        else
            cand[m++] = b;
    }
    return m;
}

// 후보 중 처리 중인 요청이 가장 적은 백엔드를 고른다 (같으면 차례가 빠른 쪽)
// 이유: 무거운 요청이 섞이면 번갈아 보내는 것만으로는 한 서버 뒤에 줄이 생긴다
static int least_busy(int route, const int *cand, int m, unsigned turn)
{
    int best = cand[turn % (unsigned)m];
    int best_n = atomic_load(&inflight[route][best]);
    for (int i = 0; i < m; i++) {
        int b = cand[(turn + (unsigned)i) % (unsigned)m];
        int n = atomic_load(&inflight[route][b]);
        if (n < best_n) {
            best = b;
            best_n = n;
        }
    }
    return best;
}

// 이미 시도한 서버 바로 다음 번호를 고른다 (예전 방식). 없으면 -1
// 이유: balance rr은 실패한 몫을 늘 다음 한 대로 넘기던 예전 동작을 그대로 재현한다
static int next_after_tried(int n, unsigned tried)
{
    for (int b = 0; b < n; b++) {
        if (!(tried & (1u << b)))
            continue;
        for (int i = 1; i < n; i++) {
            int next = (b + i) % n;
            if (!(tried & (1u << next)))
                return next;
        }
    }
    return -1;
}

// n대 중 이번 요청을 보낼 백엔드 번호를 고른다. 고를 것이 없으면 -1
// 이유: 정책은 설정으로 바꾸고, 워커 여럿이 동시에 불러도 상태가 깨지지 않게 한 곳에서 고른다
int balance_pick(const struct server_config *c, int route, int n, unsigned tried, long now)
{
    if (c->balance == BALANCE_RR && tried != 0)
        return next_after_tried(n, tried);
    unsigned turn = atomic_fetch_add(&turns[route], 1);
    int cand[CONFIG_MAX_BACKENDS];
    int m = 0;
    int full = 0;
    // 0: 멀쩡하고 데워진 서버 → 1: 멀쩡한 서버 → 2: 아프더라도 남은 서버
    for (int level = 0; level <= 2 && m == 0; level++) {
        // 멀쩡한 서버가 있는데 꽉 찼을 뿐이면 아픈 서버로 넘치지 않고 기다리게 한다
        if (level == 2 && full > 0)
            break;
        m = collect(c, route, n, tried, turn, now, level, cand, &full);
    }
    if (m == 0)
        return -1;
    return c->balance == BALANCE_LEAST ? least_busy(route, cand, m, turn) : cand[turn % (unsigned)m];
}

// 백엔드에 요청을 보내기 시작했음을 센다. 0, 처리 슬롯이 이미 꽉 찼으면 되돌리고 -1
// 이유: 고르는 사이에 다른 워커가 마지막 처리 슬롯을 가져갈 수 있어, 먼저 세고 넘쳤으면 되돌린다
int balance_begin(const struct server_config *c, int route, int b)
{
    int slots = c->routes[route].slots;
    int before = atomic_fetch_add(&inflight[route][b], 1);
    if (slots == 0 || before < slots)
        return 0;
    atomic_fetch_sub(&inflight[route][b], 1);
    return -1;
}

// 지금 이 라우트가 동시에 처리할 수 있는 수를 반환한다. 제한이 없으면 0
// 이유: 서버가 죽으면 들이는 사람도 그만큼 줄여야 남은 서버가 넘치지 않는다
int balance_capacity(const struct server_config *c, int route, long now)
{
    const struct route *r = &c->routes[route];
    int up = 0;
    for (int b = 0; b < r->backend_count; b++)
        up += atomic_load(&down_until[route][b]) <= now;
    return (up > 0 ? up : r->backend_count) * r->slots;
}

// 요청이 끝났음을 센다. 연속으로 eject_fails번 실패하면 eject_ms 동안 빼고, 성공하면 실패를 잊는다
// 이유: 아픈 서버에 요청마다 다시 부딪히지 않게 한다. 실패 뒤 첫 성공은 "돌아온 시각"이 된다
void balance_end(const struct server_config *c, int route, int b, int failed, long now)
{
    atomic_fetch_sub(&inflight[route][b], 1);
    if (failed) {
        int f = atomic_fetch_add(&fails[route][b], 1) + 1;
        if (c->eject_fails > 0 && f >= c->eject_fails)
            atomic_store(&down_until[route][b], now + c->eject_ms);
        return;
    }
    if (atomic_exchange(&fails[route][b], 0) > 0)
        atomic_store(&warm_from[route][b], now);
}
