#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

#define CONFIG_MAX_ROUTES 16
#define CONFIG_MAX_BACKENDS 8
#define CONFIG_PREFIX_MAX 256
#define CONFIG_ROOT_MAX 1024

enum route_type { ROUTE_STATIC, ROUTE_PROXY };

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
