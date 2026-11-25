#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

#define CONFIG_MAX_ROUTES 16
#define CONFIG_MAX_BACKENDS 8
#define CONFIG_PREFIX_MAX 256
#define CONFIG_ROOT_MAX 1024

enum route_type { ROUTE_STATIC, ROUTE_PROXY };

// 백엔드를 고르는 방법. RR은 실패한 몫을 늘 다음 한 대로 넘기는 예전 방식, EVEN은 고르게
enum balance_policy { BALANCE_RR, BALANCE_EVEN, BALANCE_LEAST };

// 백엔드 하나의 주소. 숫자 IPv4만 받는다 (Q18)
struct backend {
    char host[16];
    int port;
};

// 라우트 하나: 이 prefix로 시작하는 요청을 어디로 보낼지
struct route {
    char prefix[CONFIG_PREFIX_MAX];
    enum route_type type;
    struct backend backends[CONFIG_MAX_BACKENDS];
    int backend_count;
};

// 서버 설정 전체. 시작할 때 한 번 채우고 이후에는 읽기만 한다
struct server_config {
    int port;
    char root[CONFIG_ROOT_MAX];
    int threads;
    int client_timeout_ms;
    int request_deadline_ms;
    int backend_connect_timeout_ms;
    int backend_timeout_ms;
    char lock_file[CONFIG_ROOT_MAX];    /* 메인 · 보조 잠금 파일. 빈 문자열이면 이중화 안 함 */
    int standby_timeout_ms;             /* 보조가 메인의 응답을 기다리는 시간 */
    int standby_fails;                  /* 연속으로 이만큼 답이 없으면 이어받음 */
    int queue;                          /* 작업 큐 칸 수 (넘치면 503) */
    int balance;                        /* enum balance_policy */
    int eject_fails;                    /* 이어서 이만큼 실패하면 뺌. 0 = 안 뺌 */
    int eject_ms;                       /* 빼 두는 시간 */
    int slow_start_ms;                  /* 돌아온 서버를 이 시간에 걸쳐 천천히 늘림. 0 = 끔 */
    int retry_safe;                     /* 1이면 GET · HEAD는 실패 뒤 다른 서버에 한 번 더 */
    char gate_path[CONFIG_ROOT_MAX];    /* 줄을 세울 경로. 빈 문자열이면 줄 서기 안 함 */
    int gate_slots;                     /* 동시에 들이는 수 */
    int gate_grace_ms;                  /* 이만큼 소식이 없으면 떠난 것으로 봄 */
    int gate_lottery;                   /* 1이면 온 순서 대신 추첨 */
    struct route routes[CONFIG_MAX_ROUTES];
    int route_count;
};

// 5장의 기본값으로 채운다. 기본 라우트는 "/ static" 하나
void config_defaults(struct server_config *c);

// 설정 파일을 한 줄씩 읽어 값을 채운다. 성공 0, 실패하면 "파일:줄: 이유"를 err에 적고 -1
int config_load_file(struct server_config *c, const char *path, char *err, size_t err_len);

// -c 파일을 먼저 읽고 -p · -r · -t로 덮어쓴다. 성공 0, 실패하면 이유를 err에 적고 -1
int config_apply_args(struct server_config *c, int argc, char **argv, char *err, size_t err_len);

#endif
