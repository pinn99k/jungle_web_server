// 수강신청 학생 흉내: poll 하나로 학생 수천 명이 화면 → 미리담기 → 5과목 신청 → 결과 조회를 한다
// 서버와 같은 오해를 하지 않도록 서버 코드를 쓰지 않는다 (ADR 0002 예외). 조건은 Q10 6장
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define COURSES 5
#define STEPS (4 + COURSES + 1)         /* 화면 3 · 미리담기 · 신청 5 · 결과 조회 */
#define CART 3
#define RESULT (STEPS - 1)
#define PATIENCE_MS 3000                /* 답이 없으면 새로고침하는 시간 */
#define PRESS_PATIENCE_MS 1000           /* 계속 누름 · 매크로는 더 빨리 새로고침한다 */
#define LIMIT_MS 60000                  /* 열린 뒤 이만큼까지 잰다 */
#define CUT_MS 5000
#define BUF_MAX 2048                    /* 헤더와 짧은 본문만 남기고 나머지는 세기만 한다 */

enum kind { CALM, PRESS, MACRO, TABS, CUT, LEAVE, KINDS };
enum state { SLEEP, CONNECTING, WAITING, FINISHED };

// 창 하나. 학생은 보통 창 하나, TABS만 여러 개
struct tab {
    int student, index, count;          /* 몇 번째 창인지 · 이 학생의 창 수 */
    enum kind kind;
    enum state state;
    int rtt;
    int step;
    int fd;
    int reused;                         /* 이미 한 번 이상 답을 받은 연결이면 1 */
    int was_cut;
    long start;                         /* 화면을 여는 시각 */
    long wake;                          /* SLEEP이면 깰 시각, 아니면 포기할 시각 */
    long sent;                          /* 이번 단계를 처음 보낸 시각 (다시 보내도 그대로) */
    char ticket[64];
    char buf[BUF_MAX + 1];
    size_t got;                         /* 받은 바이트 (버퍼보다 많을 수 있음) */
};

// 학생의 과목 하나
struct pick {
    int course;
    long first, done;                   /* 처음 누른 시각 · 결과를 안 시각 (열린 때 기준 ms). -1 = 없음 */
    char result;                        /* '?' 모름 · 'o' 성공 · 'x' 마감 */
};

static const char *const KIND_NAME[KINDS] = {"calm", "press", "macro", "tabs", "cut", "leave"};
static struct sockaddr_in addr;
static long open_at;
static struct tab *tabs;
static int tab_count;
static struct pick (*picks)[COURSES];
static unsigned rng = 1;
static long *lat[2];                    /* 0 화면, 1 조회(미리담기 · 결과) */
static int lat_n[2], lat_max;
static long sent_total, s503, s5xx, timeouts, errors, refused;

// 부팅 뒤 지난 밀리초를 반환한다
// 이유: 장부의 여는 시각과 같은 시계(CLOCK_MONOTONIC)를 쓴다
static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

// 0 이상 n 미만의 난수를 반환한다
// 이유: 씨앗을 고정해 설정을 바꿔도 같은 학생들이 같은 때 온다
static int dice(int n)
{
    rng = rng * 1103515245u + 12345u;
    return (int)((rng >> 8) % (unsigned)n);
}

// 이 학생이 다시 누르기까지 기다리는 시간을 반환한다 (안내가 없을 때)
// 이유: 종류마다 다른 것은 얼마나 급하게 다시 누르는가뿐이다
static int press_ms(enum kind k)
{
    return k == PRESS ? 200 : k == MACRO ? 50 : 1000;
}

// t가 다음에 할 신청 순번(0~4)을 반환한다. step이 신청이 아니면 -1
// 이유: 창이 여러 개면 과목을 나눠 맡는다 (창 k는 k, k+창수, ... 번째 과목)
static int apply_index(const struct tab *t)
{
    return t->step > CART && t->step < RESULT ? t->step - CART - 1 : -1;
}

// 연결을 닫는다
// 이유: fd는 한 곳에서만 닫고 -1로 둔다
static void hang_up(struct tab *t)
{
    if (t->fd >= 0)
        close(t->fd);
    t->fd = -1;
    t->reused = 0;
}

// ms 뒤에 지금 단계를 다시(또는 처음) 보내도록 재운다
// 이유: 네트워크 지연과 사람이 기다리는 시간을 한 가지 방법으로 흉내 낸다
static void sleep_for(struct tab *t, long now, int ms)
{
    t->state = SLEEP;
    t->wake = now + ms;
}

// 다음 단계로 넘어간다. 남의 창이 맡은 과목은 건너뛰고, 끝났으면 FINISHED
// 이유: 창 여러 개인 학생은 과목을 나눠 동시에 신청한다
static void next_step(struct tab *t, long now)
{
    do {
        t->step++;
    } while (apply_index(t) >= 0 && apply_index(t) % t->count != t->index);
    t->sent = -1;
    if (t->step >= STEPS) {
        hang_up(t);
        t->state = FINISHED;
        return;
    }
    sleep_for(t, now, t->rtt);
}

// 지금 단계의 요청을 만들어 보낸다. 다 보냈으면 0, 실패 -1
// 이유: 요청이 짧아 연결된 소켓의 버퍼에 한 번에 들어간다
static int send_request(struct tab *t, long now)
{
    static const char *const PAGE[CART] = {"/", "/style.css", "/app.js"};
    char req[512], body[32] = "", path[32], cookie[96] = "";
    int k = apply_index(t), n;
    if (k >= 0) {
        snprintf(path, sizeof path, "/api/apply/%d", picks[t->student][k].course);
        snprintf(body, sizeof body, "student=%d", t->student);
        if (t->ticket[0] != '\0')
            snprintf(cookie, sizeof cookie, "Cookie: qticket=%s\r\n", t->ticket);
    } else {
        snprintf(path, sizeof path, "%s", t->step < CART ? PAGE[t->step]
                 : t->step == CART ? "/api/cart" : "/api/result");
    }
    n = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\nHost: course\r\n%s", k >= 0 ? "POST" : "GET",
                 path, cookie);
    n += k < 0 ? snprintf(req + n, sizeof req - (size_t)n, "\r\n")
               : snprintf(req + n, sizeof req - (size_t)n, "Content-Length: %zu\r\n\r\n%s",
                          strlen(body), body);
    if (send(t->fd, req, (size_t)n, 0) != n)
        return -1;
    sent_total++;
    if (t->sent < 0) {
        t->sent = now;
        if (k >= 0)
            picks[t->student][k].first = now - open_at;
    }
    t->got = 0;
    t->state = WAITING;
    // 급한 학생은 답이 없으면 1초 만에 다시 누른다 (0.05초마다 끊으면 어떤 서버의 답도 볼 수 없다)
    t->wake = now + (t->kind == PRESS || t->kind == MACRO ? PRESS_PATIENCE_MS : PATIENCE_MS);
    return 0;
}

// 새 연결을 시작한다
// 이유: 연결이 맺어지는 것도 poll로 기다려야 한 명이 모두를 막지 않는다
static void dial(struct tab *t, long now)
{
    int on = 1;
    t->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (t->fd < 0 || fcntl(t->fd, F_SETFL, O_NONBLOCK) < 0) {
        perror("socket");
        exit(1);
    }
    setsockopt(t->fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    t->state = CONNECTING;
    t->wake = now + PATIENCE_MS;
    if (connect(t->fd, (struct sockaddr *)&addr, sizeof addr) < 0 && errno != EINPROGRESS) {
        refused++;
        hang_up(t);
        sleep_for(t, now, press_ms(t->kind));
    }
}

// 응답 버퍼에서 헤더 값을 찾아 out에 적는다. 있으면 1
// 이유: 필요한 헤더는 넷뿐이라 줄을 훑어 찾는 것으로 충분하다
static int header(const char *buf, const char *name, char *out, size_t n)
{
    size_t len = strlen(name);
    for (const char *p = strstr(buf, "\r\n"); p != NULL && strncmp(p, "\r\n\r\n", 4) != 0;
         p = strstr(p + 2, "\r\n")) {
        if (strncasecmp(p + 2, name, len) == 0 && p[2 + len] == ':') {
            const char *v = p + 3 + len + strspn(p + 3 + len, " ");
            snprintf(out, n, "%.*s", (int)strcspn(v, "\r\n"), v);
            return 1;
        }
    }
    return 0;
}

// 신청의 답을 해석한다. 결과를 알았으면 1, 다시 해야 하면 기다릴 밀리초를 *ms에 적고 0
// 이유: 성공 · 이미 됨 · 마감만이 "결과를 안 것"이고 나머지는 모두 다시 눌러야 한다
static int apply_answer(struct tab *t, int status, const char *body, long now, int *ms)
{
    struct pick *p = &picks[t->student][apply_index(t)];
    char v[64];
    if (status == 200 || (status == 409 && strstr(body, "full") != NULL)) {
        p->result = status == 200 ? 'o' : 'x';
        p->done = now - open_at;
        return 1;
    }
    *ms = press_ms(t->kind);
    if (status == 403) {                /* 아직 안 열림: 곧 다시. 온 순서는 열린 뒤 처음 누른 때로 센다 */
        *ms /= 2;
        t->sent = -1;
    }
    s503 += status == 503;
    s5xx += status >= 500 && status != 503;
    if (status == 503 && t->kind == LEAVE) {
        hang_up(t);
        t->state = FINISHED;
        return 0;
    }
    // 안내를 받으면 그때까지 기다린다. 계속 누르는 사람과 매크로는 안내를 무시한다
    if (status == 503 && t->kind != PRESS && t->kind != MACRO
        && header(t->buf, "X-Wait-Ms", v, sizeof v))
        *ms = atoi(v) + dice(50);
    return 0;
}

// 완성된 응답을 처리하고 다음 행동을 정한다
// 이유: 화면 · 조회는 200이면 다음으로, 신청은 답의 뜻에 따라 다음 과목 또는 다시
static void on_response(struct tab *t, long now)
{
    char v[128];
    int status = atoi(t->buf + 9), ms = press_ms(t->kind);
    const char *body = strstr(t->buf, "\r\n\r\n") + 4;
    t->reused = 1;
    // 쿠키는 어느 답에서든 받아 둔다 (브라우저처럼)
    if (header(t->buf, "Set-Cookie", v, sizeof v) && strncmp(v, "qticket=", 8) == 0)
        snprintf(t->ticket, sizeof t->ticket, "%.*s", (int)strcspn(v + 8, ";"), v + 8);
    if (!header(t->buf, "Connection", v, sizeof v) || strcasecmp(v, "close") == 0)
        hang_up(t);
    int known;
    if (apply_index(t) >= 0) {
        known = apply_answer(t, status, body, now, &ms);
    } else {
        known = status == 200;
        s503 += status == 503;
        s5xx += status >= 500 && status != 503;
        int kind = t->step >= CART;
        if (known && lat_n[kind] < lat_max)
            lat[kind][lat_n[kind]++] = now - t->sent;
    }
    if (t->state == FINISHED)
        return;
    if (known)
        next_step(t, now);
    else
        sleep_for(t, now, ms);
}

// 응답이 다 왔으면 1 (헤더가 끝나고 Content-Length만큼 받음)
// 이유: 본문이 커도 앞부분만 남기고 나머지는 세기만 하면 된다
static int complete(const struct tab *t)
{
    char v[32];
    const char *end = strstr(t->buf, "\r\n\r\n");
    return end != NULL && header(t->buf, "Content-Length", v, sizeof v)
           && t->got >= (size_t)(end + 4 - t->buf) + (size_t)atol(v);
}

// 읽을 수 있는 만큼 읽는다. 응답이 완성되면 처리하고, 끊겼으면 다시 보낸다
// 이유: 다시 쓴 연결이 답 없이 닫힌 것은 서버가 쉬던 연결을 닫은 것이라 오류가 아니다
static void on_readable(struct tab *t, long now)
{
    char chunk[16384];
    ssize_t r = recv(t->fd, chunk, sizeof chunk, 0);
    if (r < 0 && (errno == EAGAIN || errno == EINTR))
        return;
    if (r <= 0) {
        int stale = t->reused && t->got == 0;
        hang_up(t);
        errors += !stale;
        sleep_for(t, now, stale ? 0 : press_ms(t->kind));
        return;
    }
    if (t->got < BUF_MAX) {
        size_t keep = (size_t)r < BUF_MAX - t->got ? (size_t)r : BUF_MAX - t->got;
        memcpy(t->buf + t->got, chunk, keep);
        t->buf[t->got + keep] = '\0';
    }
    t->got += (size_t)r;
    if (complete(t))
        on_response(t, now);
}

// 연결이 맺어졌는지 확인하고 요청을 보낸다
// 이유: 거절된 연결은 잠시 뒤 다시 건다
static void on_connected(struct tab *t, long now)
{
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(t->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0
        || send_request(t, now) < 0) {
        refused++;
        hang_up(t);
        sleep_for(t, now, press_ms(t->kind));
    }
}

// 시각이 된 창을 움직인다: 깨면 보내고, 답이 없으면 새로고침, 끊김은 5초 쉰다
// 이유: 사람의 행동(기다림 · 새로고침 · 끊김)은 모두 시각으로 정해진다
static void on_timer(struct tab *t, long now)
{
    if (t->kind == CUT && !t->was_cut && now - t->start > 1000 && t->step > 0) {
        t->was_cut = 1;
        hang_up(t);
        sleep_for(t, now, CUT_MS);
        return;
    }
    if (now < t->wake)
        return;
    if (t->state == SLEEP) {
        if (t->fd < 0) {
            dial(t, now);
        } else if (send_request(t, now) < 0) {
            hang_up(t);
            dial(t, now);
        }
        return;
    }
    // 답이 없다: 연결을 버리고 새로고침한다. 떠나는 사람은 여기서 떠난다
    timeouts++;
    hang_up(t);
    if (t->kind == LEAVE && apply_index(t) >= 0)
        t->state = FINISHED;
    else
        sleep_for(t, now, 0);
}

// 화면을 여는 시각(열린 때 기준 ms)을 정한다: 10% 전 3초 · 70% 첫 1초 · 15% 10초 · 5% 60초 안(30초까지)
// 이유: 정각에 몰리는 모양이 이 실험의 핵심 조건이다
static int arrival_ms(void)
{
    int d = dice(100);
    return d < 10 ? -dice(3000) - 1 : d < 80 ? dice(1000) : d < 95 ? 1000 + dice(9000)
                                                                   : 10000 + dice(20000);
}

// 학생 종류를 정한다. share[k]는 종류별 %
// 이유: 종류별 비율을 실험마다 바꿀 수 있어야 한다 (X1 탭, X2 떠남)
static enum kind pick_kind(const int *share)
{
    int d = dice(100);
    for (int k = PRESS; k < KINDS; k++) {
        if (d < share[k])
            return (enum kind)k;
        d -= share[k];
    }
    return CALM;
}

// 학생 n명과 창 · 과목을 만든다. 첫 과목이 초인기일 확률 super_pct%, 인기 과목 수 hot
// 이유: 시작하기 전에 모두 정해 두면 설정을 바꿔도 같은 학생들로 비교할 수 있다
static void build(int n, const int *share, int tab_n, int super_pct, int hot)
{
    tabs = calloc((size_t)(n * tab_n), sizeof *tabs);
    picks = calloc((size_t)n, sizeof *picks);
    lat_max = n * 8;
    lat[0] = calloc((size_t)lat_max, sizeof *lat[0]);
    lat[1] = calloc((size_t)lat_max, sizeof *lat[1]);
    if (tabs == NULL || picks == NULL || lat[0] == NULL || lat[1] == NULL)
        exit(1);
    for (int s = 0; s < n; s++) {
        enum kind k = pick_kind(share);
        int rtt = 5 + dice(76), when = arrival_ms();
        int base = dice(90), popular = dice(9);
        for (int i = 0; i < COURSES; i++)   /* 앞의 hot개는 인기, 나머지는 서로 다른 보통 과목 */
            picks[s][i].course = i < hot ? 1 + (popular + i) % 9 : 10 + (base + i * 7) % 90;
        if (dice(100) < super_pct)
            picks[s][0].course = 0;
        for (int i = 0; i < COURSES; i++) {
            picks[s][i].first = picks[s][i].done = -1;
            picks[s][i].result = '?';
        }
        for (int i = 0; i < (k == TABS ? tab_n : 1); i++) {
            struct tab *t = &tabs[tab_count++];
            t->student = s;
            t->index = i;
            t->count = k == TABS ? tab_n : 1;
            t->kind = k;
            t->rtt = rtt;
            t->fd = -1;
            t->sent = -1;
            t->start = t->wake = open_at + when;
        }
    }
}

// 모든 창이 끝나거나 시간이 다 될 때까지 돌린다
// 이유: 쉬는 창은 poll에 넣지 않아, 5,000명이어도 움직이는 연결만 본다
static void run(void)
{
    struct pollfd *pf = calloc((size_t)tab_count, sizeof *pf);
    int *who = calloc((size_t)tab_count, sizeof *who);
    int alive = 1;
    while (alive && pf != NULL && who != NULL && now_ms() < open_at + LIMIT_MS) {
        int m = 0;
        long now = now_ms();
        alive = 0;
        for (int i = 0; i < tab_count; i++) {
            struct tab *t = &tabs[i];
            if (t->state == FINISHED)
                continue;
            alive = 1;
            on_timer(t, now);
            if (t->state == CONNECTING || t->state == WAITING) {
                pf[m] = (struct pollfd){t->fd, t->state == CONNECTING ? POLLOUT : POLLIN, 0};
                who[m++] = i;
            }
        }
        if (poll(pf, (nfds_t)m, 2) <= 0)
            continue;
        now = now_ms();
        for (int j = 0; j < m; j++) {
            if (pf[j].revents == 0)
                continue;
            if (tabs[who[j]].state == CONNECTING)
                on_connected(&tabs[who[j]], now);
            else
                on_readable(&tabs[who[j]], now);
        }
    }
    free(pf);
    free(who);
}

// long 두 개를 비교한다 (qsort용)
// 이유: 백분위를 구하려면 정렬해야 한다
static int by_value(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return (x > y) - (x < y);
}

// 신청마다 한 줄 CSV를 쓰고, 화면 · 조회 시간과 센 것을 stdout에 적는다
// 이유: 판정(G1~G6)은 장부와 합쳐 summary.awk가 한다
static void report(FILE *out)
{
    for (int i = 0; i < tab_count; i++) {
        struct tab *t = &tabs[i];
        for (int k = 0; t->index == 0 && k < COURSES; k++) {
            struct pick *p = &picks[t->student][k];
            fprintf(out, "%d,%s,%d,%d,%c,%ld,%ld\n", t->student, KIND_NAME[t->kind], k, p->course,
                    p->result, p->first, p->done);
        }
    }
    for (int k = 0; k < 2; k++) {
        int n = lat_n[k];
        qsort(lat[k], (size_t)n, sizeof *lat[k], by_value);
        printf("%s %d건 p50 %ldms p99 %ldms\n", k == 0 ? "화면" : "조회", n,
               n ? lat[k][n / 2] : -1, n ? lat[k][n * 99 / 100] : -1);
    }
    printf("보낸요청 %ld 기다림503 %ld 실패5xx %ld 시간초과 %ld 끊김 %ld 연결실패 %ld\n", sent_total,
           s503, s5xx, timeouts, errors, refused);
}

// 인자를 읽어 학생을 만들고 돌린다
// 이유: 종류 비율 · 창 수 · 경쟁률을 실험마다 바꾸는 손잡이로 둔다
int main(int argc, char **argv)
{
    int share[KINDS] = {0, 10, 2, 10, 5, 5}, tab_n = 3, super_pct = 20, hot = 1;
    if (argc < 5) {
        fprintf(stderr, "사용법: %s <포트> <여는 시각> <학생 수> <결과 CSV> "
                "[press= macro= tabs= cut= leave= tabn= super= hot= seed=]\n", argv[0]);
        return 1;
    }
    const struct { const char *name; int *dst; } keys[] = {
        {"press=", &share[PRESS]}, {"macro=", &share[MACRO]}, {"tabs=", &share[TABS]},
        {"cut=", &share[CUT]}, {"leave=", &share[LEAVE]}, {"tabn=", &tab_n},
        {"super=", &super_pct}, {"hot=", &hot}, {"seed=", (int *)&rng},
    };
    for (int i = 5; i < argc; i++) {
        for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
            if (strncmp(argv[i], keys[k].name, strlen(keys[k].name)) == 0)
                *keys[k].dst = atoi(argv[i] + strlen(keys[k].name));
        }
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)atoi(argv[1]));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    open_at = atol(argv[2]);
    FILE *out = fopen(argv[4], "w");
    if (out == NULL || tab_n < 1 || hot > COURSES) {
        perror("students");
        return 1;
    }
    build(atoi(argv[3]), share, tab_n, super_pct, hot);
    run();
    report(out);
    return fclose(out) == 0 ? 0 : 1;
}
