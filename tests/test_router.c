// config · router 단위 테스트. 임시 설정 파일을 /tmp에 써서 진짜로 읽어 본다
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "server/config.h"
#include "server/router.h"
#include "tests/test.h"

static struct server_config C;
static char ERR[512];

// text를 임시 파일에 쓰고 그 파일로 설정을 읽는다. config_load_file의 반환값
// 이유: 설정 파일 해석을 실제 파일 입출력과 함께 시험한다
static int load(const char *text)
{
    char path[] = "/tmp/msw_conf.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        exit(1);
    }
    FILE *f = fdopen(fd, "w");
    fputs(text, f);
    fclose(f);
    config_defaults(&C);
    int rc = config_load_file(&C, path, ERR, sizeof ERR);
    unlink(path);
    return rc;
}

// target에 맞는 라우트의 prefix를 반환한다. 없으면 NULL
// 이유: 테스트 한 줄에 "요청 → 고른 prefix"만 보이게 한다
static const char *match(const char *target)
{
    const struct route *r = router_match(&C, target);
    return r ? r->prefix : NULL;
}

// 여러 prefix가 맞을 때 가장 긴 쪽을 고르는지 본다
// 이유: "/api/admin/"이 "/api/"보다, "/api/"가 "/"보다 구체적이다
static void test_longest_prefix(void)
{
    CHECK_INT(load("route / static\nroute /api/ proxy 127.0.0.1:8001\n"
                   "route /api/admin/ static\n"), 0);
    CHECK_STR(match("/index.html"), "/");
    CHECK_STR(match("/api/time"), "/api/");
    CHECK_STR(match("/api/admin/x"), "/api/admin/");
    CHECK_STR(match("/api/time?x=/api/admin/"), "/api/");
    CHECK_INT(router_match(&C, "/api/t")->type, ROUTE_PROXY);
    CHECK_INT(C.routes[1].backends[0].port, 8001);
}

// prefix 끝에 /가 없어도 /apix · /api-old를 잡지 않는지 본다
// 이유: Q13 — 경로 조각 단위 비교
static void test_segment_boundary(void)
{
    CHECK_INT(load("route / static\nroute /api proxy 127.0.0.1:8001\n"), 0);
    CHECK_STR(match("/api"), "/api");
    CHECK_STR(match("/api/time"), "/api");
    CHECK_STR(match("/api?x=1"), "/api");
    CHECK_STR(match("/apix"), "/");
    CHECK_STR(match("/api-old/a"), "/");
}

// 맞는 라우트가 없으면 NULL, 파일에 route가 없으면 기본 "/ static"이 남는지 본다
// 이유: 라우트 없음 → 404, 설정 없이도 정적 서버 (Q15)
static void test_no_match_and_default(void)
{
    CHECK_INT(load("route /api/ proxy 127.0.0.1:8001\n"), 0);
    CHECK(match("/index.html") == NULL);
    CHECK_INT(load("port 9000\n# 주석\n\n"), 0);
    CHECK_INT(C.port, 9000);
    CHECK_STR(match("/a"), "/");
}

// 잘못된 설정을 줄 번호와 함께 거부하는지 본다
// 이유: 설정 실수는 서버를 켜기 전에 잡아야 한다 (Q18 · Q19 · Q20 · Q22)
static void test_config_errors(void)
{
    CHECK_INT(load("port 8080\nporto 1\n"), -1);
    CHECK(strstr(ERR, ":2:") != NULL);
    CHECK_INT(load("port 70000\n"), -1);
    CHECK_INT(load("route /api/ proxy localhost:8001\n"), -1);
    CHECK_INT(load("route / static\nroute / static\n"), -1);
    CHECK_INT(load("route api static\n"), -1);
    CHECK_INT(load("route /a/ proxy\n"), -1);
    CHECK_INT(load("threads\n"), -1);
}

// -c를 뒤에 줘도 CLI 값이 파일 값을 이기는지 본다
// 이유: Q14 — 인자 순서와 상관없이 CLI가 이긴다
static void test_args_override(void)
{
    char path[] = "/tmp/msw_conf.XXXXXX";
    int fd = mkstemp(path);
    FILE *f = fdopen(fd, "w");
    fputs("port 8081\nroot /tmp\n", f);
    fclose(f);
    char *argv[] = {"server", "-p", "9090", "-c", path, NULL};
    config_defaults(&C);
    CHECK_INT(config_apply_args(&C, 5, argv, ERR, sizeof ERR), 0);
    CHECK_INT(C.port, 9090);
    CHECK_STR(C.root, "/tmp");
    char *bad[] = {"server", "-x", "1", NULL};
    CHECK_INT(config_apply_args(&C, 3, bad, ERR, sizeof ERR), -1);
    char *missing[] = {"server", "-p", NULL};
    CHECK_INT(config_apply_args(&C, 2, missing, ERR, sizeof ERR), -1);
    unlink(path);
}

// 모든 테스트를 실행하고, 하나라도 실패하면 종료 코드 1을 반환한다
// 이유: make test가 종료 코드로 성공 · 실패를 판단한다
int main(void)
{
    RUN(test_longest_prefix);
    RUN(test_segment_boundary);
    RUN(test_no_match_and_default);
    RUN(test_config_errors);
    RUN(test_args_override);
    return TEST_EXIT();
}
