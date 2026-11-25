#ifndef GATE_H
#define GATE_H

#define GATE_TICKET_LEN 32

enum gate_result { GATE_PASS, GATE_WAIT, GATE_BUSY, GATE_DONE };

// 줄의 규칙. slots명까지 동시에 들이고, grace_ms 동안 소식이 없으면 떠난 것으로 본다
struct gate_rule {
    int slots;
    int grace_ms;
    int lottery;                        /* 1이면 온 순서 대신 추첨 순서 */
};

// 번호표(NULL이나 모르는 것이면 새로 발급)로 줄에 선다. 앞에 있는 수를 ahead에, 번호표를 out에 적는다
// PASS 들어가도 됨 · WAIT 기다림 · BUSY 이미 처리 중 · DONE 이미 성공함
enum gate_result gate_visit(const char *ticket, const struct gate_rule *rule, long now,
                            char out[GATE_TICKET_LEN + 1], int *ahead);

// 들어갔던 번호표의 일이 끝났음을 알린다. ok면 성공으로 기억한다
void gate_finish(const char *ticket, int ok);

// 줄을 비운다 (테스트용)
void gate_reset(void);

#endif
