// 줄 서기: 웹서버가 누구인지 모르는 번호표만 발급하고, 번호표 순서대로 들인다 (스텝 14)
// 기다리는 손님은 연결도 워커도 붙잡지 않는다. 누구인지 판단하는 것은 백엔드의 일이다
#include "server/gate.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "common/net.h"

#define GATE_MAX 2048

enum state { FREE, WAITING, RUNNING, DONE };

struct entry {
    enum state state;
    char ticket[GATE_TICKET_LEN + 1];
    unsigned long order;                /* 작을수록 먼저 */
    long seen;                          /* 마지막으로 소식이 온 시각 */
};

// 번호표 목록. 순서 · 개수를 함께 바꾸므로 원자적 변수로는 안 되고 락 하나로 지킨다
static struct entry table[GATE_MAX];
static unsigned long next_order;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// 추측할 수 없는 난수 n바이트를 채운다. 성공 0, 실패 -1
// 이유: 번호표를 짐작해 남의 순서를 훔치지 못하게 한다
static int random_bytes(unsigned char *out, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t got = 0;
    while (got < n) {
        ssize_t r = net_read(fd, out + got, n - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    return got == n ? 0 : -1;
}

// 번호표로 목록의 칸을 찾는다. 없으면 NULL
// 이유: 칸 수가 2048개라 처음부터 훑어도 요청 하나의 비용에 비해 작다
static struct entry *find(const char *ticket)
{
    for (int i = 0; ticket != NULL && i < GATE_MAX; i++) {
        if (table[i].state != FREE && strcmp(table[i].ticket, ticket) == 0)
            return &table[i];
    }
    return NULL;
}

// 빈 칸을 찾는다. 없으면 가장 오래된 DONE을 비워 쓴다. 그것도 없으면 NULL
// 이유: 이미 성공한 기억은 잊어도 되지만 기다리는 사람의 자리는 잊으면 안 된다
static struct entry *take_free(void)
{
    struct entry *old = NULL;
    for (int i = 0; i < GATE_MAX; i++) {
        if (table[i].state == FREE)
            return &table[i];
        if (table[i].state == DONE && (old == NULL || table[i].order < old->order))
            old = &table[i];
    }
    return old;
}

// 새 번호표를 만들어 줄에 세운다. 난수를 못 얻거나 자리가 없으면 NULL
// 이유: 번호표를 발급하는 순간의 순서가 앞뒤를 정한다. 추첨이면 순서 자체를 난수로 정한다
static struct entry *join(const struct gate_rule *rule, long now)
{
    unsigned char raw[GATE_TICKET_LEN / 2 + sizeof(unsigned long)];
    struct entry *e = take_free();
    if (e == NULL || random_bytes(raw, sizeof raw) < 0)
        return NULL;
    for (int i = 0; i < GATE_TICKET_LEN / 2; i++)
        snprintf(e->ticket + i * 2, 3, "%02x", raw[i]);
    unsigned long dice;
    memcpy(&dice, raw + GATE_TICKET_LEN / 2, sizeof dice);
    e->state = WAITING;
    e->order = rule->lottery ? dice : ++next_order;
    e->seen = now;
    return e;
}

// 오래 소식이 없는 대기자를 지우고, e보다 앞서 기다리는 수를 *ahead에, 처리 중인 수를 반환한다
// 이유: 떠난 사람은 다음 사람에게 자리를 넘겨야 한다 (핑구 규칙)
static int count_ahead(const struct gate_rule *rule, const struct entry *e, long now, int *ahead)
{
    int running = 0;
    *ahead = 0;
    for (int i = 0; i < GATE_MAX; i++) {
        struct entry *o = &table[i];
        if (o->state == RUNNING)
            running++;
        if (o->state != WAITING)
            continue;
        if (o != e && now - o->seen > rule->grace_ms)
            o->state = FREE;
        else if (o->order < e->order)
            (*ahead)++;
    }
    return running;
}

// 번호표로 줄에 선다. 처음이면 번호표를 발급하고 기다리라고 한다
// 이유: 거절해도 순서를 기억해, 다시 눌러도 유리하지 않고 기다려도 불리하지 않게 한다
enum gate_result gate_visit(const char *ticket, const struct gate_rule *rule, long now,
                            char out[GATE_TICKET_LEN + 1], int *ahead)
{
    enum gate_result result = GATE_WAIT;
    *ahead = 0;
    out[0] = '\0';
    pthread_mutex_lock(&lock);
    struct entry *e = find(ticket);
    if (e != NULL && e->state == WAITING && now - e->seen > rule->grace_ms) {
        e->state = FREE; /* 너무 늦게 돌아왔다. 자리는 이미 다음 사람 것 */
        e = NULL;
    }
    int fresh = e == NULL;
    if (fresh)
        e = join(rule, now);
    if (e != NULL) {
        memcpy(out, e->ticket, sizeof e->ticket);
        if (e->state == RUNNING) {
            result = GATE_BUSY;
        } else if (e->state == DONE) {
            result = GATE_DONE;
        } else {
            e->seen = now;
            int running = count_ahead(rule, e, now, ahead);
            // 번호표를 막 받은 사람은 한 번 더 와서 들어간다 (번호표를 실제로 들고 있는지 확인)
            if (!fresh && running + *ahead < rule->slots) {
                e->state = RUNNING;
                result = GATE_PASS;
            }
        }
    }
    pthread_mutex_unlock(&lock);
    return result;
}

// 들어갔던 번호표의 일이 끝났음을 알린다. 성공이면 기억하고, 아니면 줄에서 뺀다
// 이유: 성공한 번호표로 다시 오면 또 처리하지 않고, 실패했으면 새로 줄을 서게 한다
void gate_finish(const char *ticket, int ok)
{
    pthread_mutex_lock(&lock);
    struct entry *e = find(ticket);
    if (e != NULL && e->state == RUNNING)
        e->state = ok ? DONE : FREE;
    pthread_mutex_unlock(&lock);
}

// 줄을 비운다 (테스트용)
// 이유: 단위 테스트마다 빈 줄에서 시작한다
void gate_reset(void)
{
    pthread_mutex_lock(&lock);
    memset(table, 0, sizeof table);
    next_order = 0;
    pthread_mutex_unlock(&lock);
}
