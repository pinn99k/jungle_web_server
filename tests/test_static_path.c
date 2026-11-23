// static_resolve_path 단위 테스트. /tmp에 임시 루트를 만들어 진짜 파일 · 링크로 시험한다
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "server/static_handler.h"
#include "tests/test.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static char ROOT[64];

// 루트 기준 상대 경로 rel에 내용을 쓴 파일을 만든다
// 이유: 테스트마다 같은 준비 코드를 반복하지 않기 위해서다
static void make_file(const char *rel, const char *content)
{
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", ROOT, rel);
    FILE *f = fopen(p, "w");
    if (f == NULL) {
        perror(p);
        exit(1);
    }
    fputs(content, f);
    fclose(f);
}

// 임시 루트: a.html, sub/b.txt, .env, link → /etc, s → 루트-secret (앞부분만 같은 폴더)
// 이유: 정상 · 경로 탐색 · 점 파일 · 링크 탈출(Q1)을 모두 실제 파일 시스템으로 확인한다
static void setup(void)
{
    snprintf(ROOT, sizeof ROOT, "/tmp/msw_test.XXXXXX");
    if (mkdtemp(ROOT) == NULL) {
        perror("mkdtemp");
        exit(1);
    }
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/sub", ROOT);
    mkdir(p, 0755);
    make_file("a.html", "a");
    make_file("sub/b.txt", "b");
    make_file(".env", "secret");
    snprintf(p, sizeof p, "%s/link", ROOT);
    if (symlink("/etc", p) < 0)
        perror("symlink link");
    char secret[PATH_MAX];
    snprintf(secret, sizeof secret, "%s-secret", ROOT);
    mkdir(secret, 0755);
    snprintf(p, sizeof p, "%s/s", ROOT);
    if (symlink(secret, p) < 0)
        perror("symlink s");
}

// target을 풀었을 때의 결과 코드를 돌려준다
// 이유: 테스트 한 줄에 "요청 → 기대 코드"만 보이게 한다
static int resolve(const char *target)
{
    char out[PATH_MAX + 16];
    return static_resolve_path(ROOT, target, out, sizeof out);
}

// 정상 경로 · 쿼리 · 겹친 / · 안쪽 .. 이 루트 안 파일로 풀리는지 본다
// 이유: 방어 검사가 정상 요청까지 막지 않는지가 먼저다
static void test_normal(void)
{
    char out[PATH_MAX + 16];
    CHECK_INT(static_resolve_path(ROOT, "/a.html", out, sizeof out), 0);
    CHECK(strstr(out, "/a.html") != NULL);
    CHECK_INT(resolve("/a.html?x=1"), 0);
    CHECK_INT(resolve("//a.html"), 0);
    CHECK_INT(resolve("/sub/../a.html"), 0);
    CHECK_INT(resolve("/sub/b.txt"), 0);
    CHECK_INT(resolve("/sub/"), 0);
    CHECK_INT(resolve("/nope.html"), 404);
}

// 루트 위로 올라가거나 인코딩이 잘못된 경로를 막는지 본다
// 이유: 명세 엣지 케이스 "경로 탐색 (.., %2e%2e, %00)"
static void test_traversal(void)
{
    CHECK_INT(resolve("/../etc/passwd"), 403);
    CHECK_INT(resolve("/%2e%2e/etc/passwd"), 403);
    CHECK_INT(resolve("/sub/../../x"), 403);
    CHECK_INT(resolve("/a%00.html"), 400);
    CHECK_INT(resolve("/%2"), 400);
    CHECK_INT(resolve("/%zz"), 400);
}

// 루트 밖을 가리키는 링크와 앞부분만 같은 폴더로 가는 링크를 막는지 본다
// 이유: Q1 — 문자열 검사만으로는 링크 탈출을 못 막는다
static void test_symlink_escape(void)
{
    CHECK_INT(resolve("/link/hostname"), 403);
    CHECK_INT(resolve("/s/"), 403);
}

// 점 파일이 있어도 404로 숨기는지 본다 (인코딩 · .. 으로 돌아가도)
// 이유: Q3 — 실수로 남긴 .env · .git이 새 나가지 않게
static void test_dotfiles(void)
{
    CHECK_INT(resolve("/.env"), 404);
    CHECK_INT(resolve("/sub/../.env"), 404);
    CHECK_INT(resolve("/%2eenv"), 404);
}

// 준비 후 모든 테스트를 실행하고, 하나라도 실패하면 종료 코드 1을 반환한다
// 이유: make test가 종료 코드로 성공 · 실패를 판단한다 (임시 폴더는 /tmp에 남겨 둔다)
int main(void)
{
    setup();
    RUN(test_normal);
    RUN(test_traversal);
    RUN(test_symlink_escape);
    RUN(test_dotfiles);
    return TEST_EXIT();
}
