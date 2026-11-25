#ifndef GATE_H
#define GATE_H

#define GATE_TICKET_LEN 32              /* 순번 8자 + 비밀 24자 */
#define GATE_CHAIRS_MAX 256

enum gate_result { GATE_PASS, GATE_WAIT, GATE_BUSY, GATE_FULL };

// 줄의 규칙. 요청마다 설정과 지금 멀쩡한 서버 수로 채운다
struct gate_rule {
    int slots;                          /* 동시에 들이는 수 */
    int chairs;                         /* 연결을 쥔 채 기다리는 수 */
    int overbook;                       /* 부르는 속도 (%). 100 = 처리하는 만큼만 부름 */
    int grace_ms;                       /* 오라고 한 시각보다 이만큼 늦으면 떠난 것 */
    int keep;                           /* 1이면 한 과목이 끝나도 순번을 유지 (이어서) */
    int course_slots;                   /* 한 과목이 동시에 쓰는 자리. 0 = 제한 없음 */
    int soldout_ms;                     /* 마감 답을 기억하는 시간. 0 = 기억 안 함 */
};

// 줄에서 받은 것: 번호표, 과목, 기다릴 때의 안내
struct gate_pass {
    char ticket[GATE_TICKET_LEN + 1];
    int course;                         /* 과목 번호. 모르면 -1 */
    int fresh;                          /* 이번에 새로 발급한 번호표면 1 */
    int ahead;                          /* 앞에 남은 순번 */
    int wait_ms;                        /* 이만큼 뒤에 다시 오라는 안내 */
};

// 번호표(NULL이나 모르는 것이면 새로 발급)로 들어간다. 부른 순번이면 의자에서 자리를 기다린다(최대 2초)
// PASS 들어감 · WAIT 나중에 다시 · BUSY 같은 번호표가 이미 안에 있음 · FULL 마감으로 기억한 과목
enum gate_result gate_enter(const char *ticket, int course, const struct gate_rule *rule, long now,
                            struct gate_pass *out);

// 들어갔던 일이 끝났음을 알린다. status는 백엔드의 답(0 = 답 없음), took_ms는 걸린 시간
void gate_leave(const struct gate_pass *p, const struct gate_rule *rule, int status, int took_ms,
                long now);

// 줄을 비운다 (테스트용)
void gate_reset(void);

#endif
