#ifndef LEDGER_H
#define LEDGER_H

#define LEDGER_COURSES 128
#define LEDGER_STUDENTS 16384
#define LEDGER_SEATS 1024               /* 과목 하나의 최대 정원 */

enum ledger_result { LEDGER_OK, LEDGER_FULL, LEDGER_ALREADY, LEDGER_CLOSED, LEDGER_BAD };

struct ledger;

// 좌석 장부 파일을 새로 만든다. 과목 i의 정원은 seats[i], open_at은 신청을 여는 시각. 성공 0, 실패 -1
int ledger_create(const char *path, const int *seats, int courses, long open_at_ms);

// 이미 있는 장부를 연다 (백엔드 여러 개가 같은 것을 연다). 실패 NULL
struct ledger *ledger_open(const char *path);

// 과목에 학생을 신청한다. 잠금을 쥔 채 lock_ms 동안 일한다
enum ledger_result ledger_apply(struct ledger *l, int course, int student, int lock_ms);

// 아직 여는 시각 전이면 1
int ledger_closed(const struct ledger *l);

// 장부가 맞는지 검사하고 과목별 배정을 stdout에 적는다. 정원 초과 · 중복 수를 반환한다
int ledger_dump(struct ledger *l);

// 부팅 뒤 지난 밀리초 (프로세스끼리 같은 시계)
long ledger_now(void);

#endif
