#include "server/config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX_LEN 1024

// 문자열 s를 [min, max] 범위의 정수로 바꾼다. 성공 0, 숫자가 아니거나 범위 밖이면 -1
// 이유: 포트 · 스레드 수 · 타임아웃이 모두 "범위가 있는 정수"라 검사를 한 곳에 모은다
static int parse_int(const char *s, long min, long max, long *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max)
        return -1;
    *out = v;
    return 0;
}

// "127.0.0.1:8001"을 숫자 IPv4와 포트로 나눈다. 성공 0, 형식 오류 -1
// 이유: 주소를 시작할 때 검사해 두면 요청마다 실패하는 대신 설정 단계에서 바로 알 수 있다 (Q18)
static int parse_backend(const char *s, struct backend *out)
{
    const char *colon = strrchr(s, ':');
    struct in_addr addr;
    long port;
    if (colon == NULL || (size_t)(colon - s) >= sizeof out->host)
        return -1;
    memcpy(out->host, s, (size_t)(colon - s));
    out->host[colon - s] = '\0';
    if (inet_pton(AF_INET, out->host, &addr) != 1 || parse_int(colon + 1, 1, 65535, &port) < 0)
        return -1;
    out->port = (int)port;
    return 0;
}

// strtok_r로 이어서 남은 토큰을 모두 백엔드 주소로 읽어 r에 넣는다. 성공 NULL, 실패 시 이유
// 이유: proxy 라우트의 "백엔드 목록 검사"를 라우트 줄 해석과 나눠, 각각 한 가지 일만 하게 한다
static const char *parse_backends(struct route *r, char **save)
{
    for (char *b = strtok_r(NULL, " \t", save); b != NULL; b = strtok_r(NULL, " \t", save)) {
        if (r->backend_count >= CONFIG_MAX_BACKENDS)
            return "백엔드가 너무 많음 (최대 8)";
        if (parse_backend(b, &r->backends[r->backend_count++]) < 0)
            return "백엔드 주소는 숫자 IPv4:포트";
    }
    return r->backend_count == 0 ? "proxy 뒤에 백엔드가 하나 이상 필요" : NULL;
}

// "<prefix> static" 또는 "<prefix> proxy <IPv4:포트>..."를 라우트로 추가한다. 성공 NULL, 실패 시 이유
// 이유: 라우팅 규칙을 설정으로 받으라는 명세 요구. 잘못된 규칙은 서버를 켜기 전에 거부한다
static const char *parse_route_line(struct server_config *c, char *line)
{
    char *save = NULL;
    char *prefix = strtok_r(line, " \t", &save);
    char *type = strtok_r(NULL, " \t", &save);
    if (prefix == NULL || type == NULL || prefix[0] != '/' || strlen(prefix) >= CONFIG_PREFIX_MAX)
        return "형식: route </경로> static | proxy <IPv4:포트>...";
    if (c->route_count >= CONFIG_MAX_ROUTES)
        return "라우트가 너무 많음 (최대 16)";
    for (int i = 0; i < c->route_count; i++) {
        if (strcmp(c->routes[i].prefix, prefix) == 0)
            return "같은 prefix가 두 번 나옴";
    }
    struct route *r = &c->routes[c->route_count];
    snprintf(r->prefix, sizeof r->prefix, "%s", prefix);
    r->backend_count = 0;
    if (strcmp(type, "static") == 0) {
        r->type = ROUTE_STATIC;
        if (strtok_r(NULL, " \t", &save) != NULL)
            return "static 뒤에는 아무것도 오지 않음";
    } else if (strcmp(type, "proxy") == 0) {
        r->type = ROUTE_PROXY;
        const char *msg = parse_backends(r, &save);
        if (msg != NULL)
            return msg;
    } else {
        return "라우트 종류는 static 또는 proxy";
    }
    c->route_count++;
    return NULL;
}

// 키 하나에 값 하나를 적용한다. 성공 NULL, 실패 시 이유
// 이유: 설정 파일과 CLI 인자가 같은 키 · 같은 범위 검사를 쓰게 한다
static const char *apply_key(struct server_config *c, const char *key, const char *val)
{
    if (strcmp(key, "root") == 0) {
        int n = snprintf(c->root, sizeof c->root, "%s", val);
        return (n < 0 || (size_t)n >= sizeof c->root) ? "root 경로가 너무 김" : NULL;
    }
    const struct { const char *name; long min; long max; int *dst; } keys[] = {
        {"port", 1, 65535, &c->port},
        {"threads", 1, 256, &c->threads},
        {"client_timeout_ms", 1, 600000, &c->client_timeout_ms},
        {"request_deadline_ms", 1, 600000, &c->request_deadline_ms},
        {"backend_connect_timeout_ms", 1, 600000, &c->backend_connect_timeout_ms},
        {"backend_timeout_ms", 1, 600000, &c->backend_timeout_ms},
    };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        long v;
        if (strcmp(key, keys[i].name) != 0)
            continue;
        if (parse_int(val, keys[i].min, keys[i].max, &v) < 0)
            return "숫자가 아니거나 범위 밖";
        *keys[i].dst = (int)v;
        return NULL;
    }
    return "모르는 키";
}

// 5장의 기본값으로 채운다. 기본 라우트는 "/ static" 하나
// 이유: 설정 파일 없이도 정적 서버로 바로 동작하게 한다 (Q15)
void config_defaults(struct server_config *c)
{
    memset(c, 0, sizeof *c);
    c->port = 8080;
    snprintf(c->root, sizeof c->root, "./www");
    c->threads = 8;
    c->client_timeout_ms = 5000;
    c->request_deadline_ms = 10000;
    c->backend_connect_timeout_ms = 1000;
    c->backend_timeout_ms = 3000;
    char def[] = "/ static";
    parse_route_line(c, def);
}

// 설정 파일 한 줄을 해석해 적용한다. 성공 NULL(주석 · 빈 줄 포함), 실패 시 이유
// 이유: "키 값"과 "route …" 두 형식을 한 곳에서 나눠 보낸다
static const char *apply_line(struct server_config *c, char *line)
{
    char *end = line + strlen(line);
    char *save = NULL;
    char *key = strtok_r(line, " \t", &save);
    if (key == NULL || key[0] == '#')
        return NULL;
    if (strcmp(key, "route") == 0) {
        // save의 내용은 POSIX가 정하지 않으므로, 키 뒤의 나머지는 직접 계산한다
        char *rest = key + strlen(key);
        return parse_route_line(c, rest < end ? rest + 1 : rest);
    }
    char *val = strtok_r(NULL, " \t", &save);
    if (val == NULL || strtok_r(NULL, " \t", &save) != NULL)
        return "형식: 키 값";
    return apply_key(c, key, val);
}

// 설정 파일을 한 줄씩 읽어 값을 채운다. 성공 0, 실패하면 "파일:줄: 이유"를 err에 적고 -1
// 이유: 잘못된 설정은 서버를 켜기 전에 어느 줄이 왜 틀렸는지 알려 줘야 고칠 수 있다
int config_load_file(struct server_config *c, const char *path, char *err, size_t err_len)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        snprintf(err, err_len, "%s: %s", path, strerror(errno));
        return -1;
    }
    // 파일에 route가 하나라도 있으면 기본 라우트 대신 그것만 쓴다 (없으면 끝에서 기본값을 다시 넣음)
    c->route_count = 0;
    char line[LINE_MAX_LEN];
    int lineno = 0;
    const char *msg = NULL;
    while (msg == NULL && fgets(line, sizeof line, f) != NULL) {
        lineno++;
        size_t n = strcspn(line, "\r\n");
        if (line[n] == '\0' && !feof(f)) {
            msg = "줄이 너무 김 (최대 1023자)";
        } else {
            line[n] = '\0';
            msg = apply_line(c, line);
        }
    }
    fclose(f);
    if (msg != NULL) {
        snprintf(err, err_len, "%s:%d: %s", path, lineno, msg);
        return -1;
    }
    if (c->route_count == 0) {
        char def[] = "/ static";
        parse_route_line(c, def);
    }
    return 0;
}

static const struct {
    const char *flag;
    const char *key;                    /* NULL = 설정 키가 아님 (-c는 따로 처리) */
} FLAGS[] = {
    {"-p", "port"},
    {"-r", "root"},
    {"-t", "threads"},
    {"-c", NULL},
};

// 인자 이름으로 FLAGS 표의 칸 번호를 찾는다. 모르는 인자면 -1
// 이유: 인자와 설정 키의 짝을 표 하나에 두어, 인자를 추가할 때 표만 고치면 되게 한다
static int find_flag(const char *a)
{
    for (size_t i = 0; i < sizeof FLAGS / sizeof FLAGS[0]; i++) {
        if (strcmp(a, FLAGS[i].flag) == 0)
            return (int)i;
    }
    return -1;
}

// -c 파일을 먼저 읽고 -p · -r · -t로 덮어쓴다. 성공 0, 실패하면 이유를 err에 적고 -1
// 이유: 인자 순서와 상관없이 CLI가 파일을 이기게 한다 (Q14: -p 9090 -c f도 9090)
int config_apply_args(struct server_config *c, int argc, char **argv, char *err, size_t err_len)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && config_load_file(c, argv[i + 1], err, err_len) < 0)
            return -1;
    }
    for (int i = 1; i < argc; i += 2) {
        const char *a = argv[i];
        int f = find_flag(a);
        if (f < 0) {
            snprintf(err, err_len, "모르는 인자: %s", a);
            return -1;
        }
        const char *key = FLAGS[f].key;
        if (i + 1 >= argc) {
            snprintf(err, err_len, "%s 뒤에 값이 필요", a);
            return -1;
        }
        const char *msg = key ? apply_key(c, key, argv[i + 1]) : NULL;
        if (msg != NULL) {
            snprintf(err, err_len, "%s %s: %s", a, argv[i + 1], msg);
            return -1;
        }
    }
    return 0;
}
