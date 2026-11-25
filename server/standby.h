#ifndef STANDBY_H
#define STANDBY_H

#include "server/config.h"

// 메인이 될 때까지 기다린다. 메인이 되면 잠금 fd(끝까지 열어 둔다), 종료 신호를 받으면 -2, 실패 -1
int standby_acquire(const struct server_config *c);

#endif
