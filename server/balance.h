#ifndef BALANCE_H
#define BALANCE_H

#include "server/config.h"

// n대 중 이번 요청을 보낼 백엔드 번호를 고른다. tried는 이미 시도한 번호의 비트. 고를 것이 없으면 -1
int balance_pick(const struct server_config *c, int route, int n, unsigned tried, long now);

// 백엔드에 요청을 보내기 시작했음을 센다
void balance_begin(int route, int b);

// 요청이 끝났음을 센다. failed면 아픈 것으로 기록해 정해진 횟수가 되면 잠깐 뺀다
void balance_end(const struct server_config *c, int route, int b, int failed, long now);

// 부팅 뒤 지난 밀리초를 반환한다 (위 함수들의 now에 쓴다)
long balance_now(void);

#endif
