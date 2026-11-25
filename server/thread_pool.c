#include "server/thread_pool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

struct job {
    int fd;
    char ip[16];
};

// 작업 큐(원형 배열)와 워커들. 큐는 lock으로 지키고, 비었을 때 워커는 not_empty에서 잠든다
struct thread_pool {
    pthread_t *threads;
    int nthreads;
    struct job *queue;
    int cap;
    int head;                           /* 다음에 꺼낼 칸 */
    int count;                          /* 들어 있는 작업 수 */
    int stopping;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pool_work_fn work;
    void *arg;
    atomic_int *waiting;                /* count를 락 없이 보여 주는 칸 */
};

// 큐에서 작업을 꺼내 work를 실행하는 것을 반복한다. 종료 중이고 큐가 비면 끝낸다. 항상 NULL
// 이유: 워커는 손님 수와 상관없이 고정된 수만 만들고, 각자 큐에서 다음 일을 스스로 가져간다
static void *worker_main(void *arg)
{
    struct thread_pool *p = arg;
    for (;;) {
        pthread_mutex_lock(&p->lock);
        // 깨어나도 다른 워커가 먼저 가져갔을 수 있으므로 조건을 다시 확인한다 (spurious wakeup 포함)
        while (p->count == 0 && !p->stopping)
            pthread_cond_wait(&p->not_empty, &p->lock);
        if (p->count == 0) {
            pthread_mutex_unlock(&p->lock);
            return NULL;
        }
        struct job j = p->queue[p->head];
        p->head = (p->head + 1) % p->cap;
        p->count--;
        atomic_store(p->waiting, p->count);
        pthread_mutex_unlock(&p->lock);
        p->work(j.fd, j.ip, p->arg); /* 락 밖에서 실행해야 다른 워커가 동시에 꺼낼 수 있다 */
    }
}

// 워커 스레드를 모두 깨워 끝내고 회수한 뒤 메모리를 해제한다. started는 실제로 만든 스레드 수
// 이유: 정상 종료와 pool_create 중간 실패가 같은 정리 순서를 쓰게 한다
static void pool_destroy(struct thread_pool *p, int started)
{
    pthread_mutex_lock(&p->lock);
    p->stopping = 1;
    pthread_cond_broadcast(&p->not_empty);
    pthread_mutex_unlock(&p->lock);
    for (int i = 0; i < started; i++)
        pthread_join(p->threads[i], NULL);
    pthread_cond_destroy(&p->not_empty);
    pthread_mutex_destroy(&p->lock);
    free(p->threads);
    free(p->queue);
    free(p);
}

// 워커 스레드 nthreads개와 크기 queue_cap의 빈 작업 큐를 만든다. 실패 NULL
// 이유: 스레드를 요청마다 만들지 않고 미리 만들어 두어, 생성 비용과 동시 스레드 수를 고정한다
struct thread_pool *pool_create(int nthreads, int queue_cap, pool_work_fn work, void *arg,
                                atomic_int *waiting)
{
    struct thread_pool *p = calloc(1, sizeof *p);
    if (p == NULL)
        return NULL;
    p->threads = calloc((size_t)nthreads, sizeof *p->threads);
    p->queue = calloc((size_t)queue_cap, sizeof *p->queue);
    p->nthreads = nthreads;
    p->cap = queue_cap;
    p->work = work;
    p->arg = arg;
    p->waiting = waiting;
    if (p->threads == NULL || p->queue == NULL || pthread_mutex_init(&p->lock, NULL) != 0) {
        free(p->threads);
        free(p->queue);
        free(p);
        return NULL;
    }
    pthread_cond_init(&p->not_empty, NULL);
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&p->threads[i], NULL, worker_main, p) != 0) {
            pool_destroy(p, i);
            return NULL;
        }
    }
    return p;
}

// 작업 큐에 연결을 넣고 워커 하나를 깨운다. 성공 0, 큐가 가득 차거나 종료 중이면 -1 (호출한 쪽이 503)
// 이유: accept 스레드는 기다리지 않고 바로 다음 손님을 받아야 하므로, 자리가 없으면 즉시 거절한다
int pool_submit(struct thread_pool *p, int fd, const char *ip)
{
    pthread_mutex_lock(&p->lock);
    if (p->stopping || p->count == p->cap) {
        pthread_mutex_unlock(&p->lock);
        return -1;
    }
    struct job *j = &p->queue[(p->head + p->count) % p->cap];
    j->fd = fd;
    snprintf(j->ip, sizeof j->ip, "%s", ip);
    p->count++;
    atomic_store(p->waiting, p->count);
    pthread_cond_signal(&p->not_empty);
    pthread_mutex_unlock(&p->lock);
    return 0;
}

// 새 작업을 막고, 남은 작업을 다 처리한 뒤 워커를 모두 회수하고 해제한다
// 이유: 큐에 남은 손님을 버리지 않고 끝까지 응답한 뒤 끝내기 위해서다 (스텝 8 안전 종료)
void pool_shutdown(struct thread_pool *p)
{
    pool_destroy(p, p->nthreads);
}
