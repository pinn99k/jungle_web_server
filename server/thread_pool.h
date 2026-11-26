#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <stdatomic.h>

// 워커가 연결 하나를 맡아 실행할 함수. fd를 닫는 것까지 이 함수의 책임이다
typedef void (*pool_work_fn)(int fd, const char *ip, void *arg);

struct thread_pool;

// 워커 스레드 nthreads개와 크기 queue_cap의 빈 작업 큐를 만든다. 기다리는 작업 수를 waiting에 적는다. 실패 NULL
struct thread_pool *pool_create(int nthreads, int queue_cap, pool_work_fn work, void *arg,
                                atomic_int *waiting);

// 작업 큐에 연결을 넣고 워커 하나를 깨운다. 성공 0, 큐가 가득 차거나 종료 중이면 -1 (호출한 쪽이 503)
int pool_submit(struct thread_pool *p, int fd, const char *ip);

// 새 작업을 막고, 남은 작업을 다 처리한 뒤 워커를 모두 회수하고 해제한다
void pool_shutdown(struct thread_pool *p);

#endif
