// 좌석 장부: 백엔드 여러 개가 함께 보는 공유 메모리. 과목마다 잠금 하나 (DB의 행 잠금 흉내)
#include "backend/ledger.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

struct course {
    pthread_mutex_t lock;
    int seats;
    int granted;
    int winners[LEDGER_SEATS];          /* 좌석을 받은 순서대로 */
    unsigned char taken[LEDGER_STUDENTS / 8];
};

struct ledger {
    int courses;
    long open_at_ms;
    struct course course[LEDGER_COURSES];
};

// 부팅 뒤 지난 밀리초를 반환한다
// 이유: 백엔드 셋과 학생 클라이언트가 같은 "여는 시각"을 봐야 한다
long ledger_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

// 장부 파일을 메모리에 붙인다. 실패 NULL
// 이유: 만들 때와 열 때가 같은 크기 · 같은 방식으로 붙여야 한다
static struct ledger *map_file(const char *path, int flags)
{
    int fd = open(path, flags, 0644);
    if (fd < 0)
        return NULL;
    void *p = MAP_FAILED;
    if (ftruncate(fd, (off_t)sizeof(struct ledger)) == 0)
        p = mmap(NULL, sizeof(struct ledger), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? NULL : p;
}

// 좌석 장부 파일을 새로 만든다. 성공 0, 실패 -1
// 이유: 잠금을 프로세스끼리 쓰고, 쥔 프로세스가 죽어도 풀리게(ROBUST) 만들어야 고장 실험을 할 수 있다
int ledger_create(const char *path, const int *seats, int courses, long open_at_ms)
{
    if (courses < 1 || courses > LEDGER_COURSES)
        return -1;
    struct ledger *l = map_file(path, O_RDWR | O_CREAT | O_TRUNC);
    pthread_mutexattr_t attr;
    if (l == NULL || pthread_mutexattr_init(&attr) != 0)
        return -1;
    int rc = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0
             && pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0 ? 0 : -1;
    l->courses = courses;
    l->open_at_ms = open_at_ms;
    for (int i = 0; rc == 0 && i < courses; i++) {
        l->course[i].seats = seats[i] < LEDGER_SEATS ? seats[i] : LEDGER_SEATS;
        rc = pthread_mutex_init(&l->course[i].lock, &attr) == 0 ? 0 : -1;
    }
    pthread_mutexattr_destroy(&attr);
    munmap(l, sizeof *l);
    return rc;
}

// 이미 있는 장부를 연다. 실패 NULL
// 이유: 백엔드 프로세스마다 한 번 열고 끝까지 쓴다
struct ledger *ledger_open(const char *path)
{
    return map_file(path, O_RDWR);
}

// 아직 여는 시각 전이면 1
// 이유: 시작 전에 누른 학생에게 자리를 쓰지 않고 바로 답한다
int ledger_closed(const struct ledger *l)
{
    return ledger_now() < l->open_at_ms;
}

// 과목의 잠금을 쥔다. 쥔 채 죽은 프로세스가 있었으면 이어받아 고친다. 성공 0, 실패 -1
// 이유: 백엔드가 잠금을 쥔 채 죽어도 그 과목이 영원히 막히지 않아야 한다
static int lock_course(struct course *c)
{
    int rc = pthread_mutex_lock(&c->lock);
    if (rc == EOWNERDEAD)
        rc = pthread_mutex_consistent(&c->lock);
    return rc == 0 ? 0 : -1;
}

// 과목에 학생을 신청한다. 잠금을 쥔 채 lock_ms 동안 일한다
// 이유: 같은 과목은 한 명씩, 다른 과목은 동시에 처리되는 DB의 행 잠금을 흉내 낸다
enum ledger_result ledger_apply(struct ledger *l, int course, int student, int lock_ms)
{
    if (course < 0 || course >= l->courses || student < 0 || student >= LEDGER_STUDENTS)
        return LEDGER_BAD;
    if (ledger_closed(l))
        return LEDGER_CLOSED;
    struct course *c = &l->course[course];
    unsigned char bit = (unsigned char)(1u << (student % 8));
    if (lock_course(c) < 0)
        return LEDGER_BAD;
    struct timespec ts = {lock_ms / 1000, (lock_ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    enum ledger_result r = LEDGER_OK;
    if (c->taken[student / 8] & bit) {
        r = LEDGER_ALREADY;
    } else if (c->granted >= c->seats) {
        r = LEDGER_FULL;
    } else {
        // 기록을 먼저 쓰고 개수를 늘린다. 그 사이에 죽어도 개수가 기록보다 앞서지 않는다
        c->winners[c->granted] = student;
        c->taken[student / 8] |= bit;
        c->granted++;
    }
    pthread_mutex_unlock(&c->lock);
    return r;
}

// 장부가 맞는지 검사하고 과목별 배정을 적는다. 정원 초과 · 중복 수를 반환한다
// 이유: 판정 기준 G2(정원 초과 0 · 중복 0)를 장부에서 직접 확인한다
int ledger_dump(struct ledger *l)
{
    int bad = 0;
    for (int i = 0; i < l->courses; i++) {
        struct course *c = &l->course[i];
        bad += c->granted > c->seats;
        printf("course %d seats %d granted %d :", i, c->seats, c->granted);
        for (int k = 0; k < c->granted; k++) {
            printf(" %d", c->winners[k]);
            for (int j = 0; j < k; j++)
                bad += c->winners[j] == c->winners[k];
        }
        printf("\n");
    }
    return bad;
}
