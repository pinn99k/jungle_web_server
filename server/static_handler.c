#include "server/static_handler.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/buf.h"
#include "common/http_response.h"
#include "common/net.h"
#include "server/mime.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define FILE_CHUNK 65536

// 16진수 글자 하나를 값(0~15)으로 바꾼다. 16진수가 아니면 -1
// 이유: %2e 같은 퍼센트 인코딩의 두 글자를 읽기 위해서다
static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// in[0..n)의 %XX 인코딩을 풀어 out에 NUL로 끝나게 적는다. 성공 0, 잘못된 인코딩 · %00 · 넘침 -1
// 이유: %2e%2e처럼 인코딩해서 숨긴 ..도 풀고 나서 검사해야 경로 탐색을 막을 수 있다
static int percent_decode(const char *in, size_t n, char *out, size_t out_len)
{
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c == '%') {
            if (i + 2 >= n)
                return -1;
            int hi = hex_value(in[i + 1]);
            int lo = hex_value(in[i + 2]);
            if (hi < 0 || lo < 0 || (hi == 0 && lo == 0))
                return -1;
            c = (char)(hi * 16 + lo);
            i += 2;
        }
        if (w + 1 >= out_len)
            return -1;
        out[w++] = c;
    }
    out[w] = '\0';
    return 0;
}

// /로 시작하는 경로에서 //, ., ..을 정리한다 (끝의 /는 남김). 성공 0, 루트 위로 올라가면 -1
// 이유: "/a/../../etc"처럼 글자로 루트 밖을 가리키는 경로를 파일을 열기 전에 막는다
static int normalize_path(char *p)
{
    size_t n = strlen(p);
    int trailing = n > 0 && p[n - 1] == '/';
    size_t r = 0;
    size_t w = 0;
    while (r < n) {
        while (r < n && p[r] == '/')
            r++;
        size_t s = r;
        while (r < n && p[r] != '/')
            r++;
        size_t len = r - s;
        if (len == 0 || (len == 1 && p[s] == '.'))
            continue;
        if (len == 2 && p[s] == '.' && p[s + 1] == '.') {
            if (w == 0)
                return -1;
            while (p[w - 1] != '/')
                w--;
            w--;
            continue;
        }
        p[w++] = '/';
        memmove(p + w, p + s, len);
        w += len;
    }
    if (w == 0 || trailing)
        p[w++] = '/';
    p[w] = '\0';
    return 0;
}

// 요청 경로를 루트 안의 실제 파일 경로로 바꾼다. 성공 0, 실패 시 상태 코드(400/403/404/414/500)
// 이유: 글자 검사(..)와 실제 위치 검사(realpath, 링크 탈출 Q1)를 둘 다 통과한 경로만 연다
int static_resolve_path(const char *root, const char *target, char *out, size_t out_len)
{
    char path[PATH_MAX];
    if (percent_decode(target, strcspn(target, "?"), path, sizeof path) < 0)
        return 400;
    if (normalize_path(path) < 0)
        return 403;
    // 점 파일 · 점 폴더는 있다는 사실도 숨긴다 (Q3)
    if (strstr(path, "/.") != NULL)
        return 404;

    char root_real[PATH_MAX];
    char joined[2 * PATH_MAX];
    if (out_len < PATH_MAX || realpath(root, root_real) == NULL)
        return 500;
    int n = snprintf(joined, sizeof joined, "%s%s", root_real, path);
    if (n < 0 || (size_t)n >= sizeof joined)
        return 414;
    if (realpath(joined, out) == NULL)
        return errno == EACCES ? 403 : 404;
    // "/work/www-secret"처럼 앞부분만 같은 경로를 걸러내려고 뒤 글자까지 본다
    size_t rl = strlen(root_real);
    if (strncmp(out, root_real, rl) != 0 || (out[rl] != '\0' && out[rl] != '/'))
        return 403;
    return 0;
}

// 파일을 열고 크기를 얻는다. 디렉토리면 index.html을 붙인다. 성공 0, 실패 시 상태 코드(301/403/404/500)
// 이유: 디렉토리 · 일반 파일 · 권한 없음 · 없음을 구분해 맞는 상태 코드로 답하기 위해서다
static int open_file(char *path, size_t cap, int slash, int *fd, off_t *size)
{
    struct stat st;
    if (stat(path, &st) < 0)
        return errno == EACCES ? 403 : 404;
    int is_dir = S_ISDIR(st.st_mode);
    if (is_dir) {
        if (!slash)
            return 301; /* Q2: /docs → /docs/ */
        size_t len = strlen(path);
        int n = snprintf(path + len, cap - len, "/index.html");
        if (n < 0 || (size_t)n >= cap - len)
            return 500;
    }
    // 마지막 조각이 링크면 열지 않는다. index.html이 루트 밖을 가리키는 링크일 수 있다 (Q1)
    int f = open(path, O_RDONLY | O_NOFOLLOW);
    if (f < 0) {
        if (errno == ENOENT)
            return is_dir ? 403 : 404; /* index.html 없는 폴더는 403 (Q6) */
        return (errno == EACCES || errno == ELOOP) ? 403 : 500;
    }
    if (fstat(f, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(f);
        return 403;
    }
    *fd = f;
    *size = st.st_size;
    return 0;
}

// 파일을 64KB씩 읽어 소켓으로 보낸다. 성공 0, 실패 -1
// 이유: 큰 파일을 통째로 메모리에 올리지 않고, 연결마다 쓰는 메모리를 64KB로 묶는다
static int send_file_body(int sock, int file_fd, off_t size)
{
    char chunk[FILE_CHUNK];
    off_t left = size;
    while (left > 0) {
        size_t want = left < (off_t)sizeof chunk ? (size_t)left : sizeof chunk;
        ssize_t r = net_read(file_fd, chunk, want);
        // 보내는 중 파일이 줄었으면 Content-Length를 채울 수 없으므로 연결을 닫게 한다 (Q10)
        if (r <= 0 || net_write_all(sock, chunk, (size_t)r) < 0)
            return -1;
        left -= r;
    }
    return 0;
}

// 버퍼에 만든 응답을 보내고 버퍼를 해제한다. build_rc가 실패(-1)면 보내지 않는다. 성공 0, 실패 -1
// 이유: 오류 · 리다이렉트 응답이 "만들기 → 보내기 → 해제"를 같은 순서로 빠짐없이 하게 한다
static int send_built(int fd, struct buf *out, int build_rc)
{
    int rc = build_rc == 0 ? net_write_all(fd, out->data, out->len) : -1;
    buf_free(out);
    return rc;
}

// 정적 파일 핸들러. 메서드 확인 → 경로 변환 → 파일 열기 → 헤더 · 본문 전송. 0 = 계속 가능, -1 = 닫아야 함
// 이유: 라우터가 정적 라우트로 고른 요청을 처음부터 끝까지 이 함수 하나가 책임진다
int static_handle(struct request_ctx *ctx)
{
    struct buf out;
    buf_init(&out);
    const char *method = ctx->msg->method;
    int head = strcmp(method, "HEAD") == 0;
    if (!head && strcmp(method, "GET") != 0) {
        ctx->status = 405;
        return send_built(ctx->fd, &out, http_build_error(&out, 405, ctx->keep_alive));
    }

    const char *target = ctx->msg->target;
    size_t plen = strcspn(target, "?");
    char path[PATH_MAX + 16];
    int fd = -1;
    off_t size = 0;
    int rc = static_resolve_path(ctx->cfg->root, target, path, sizeof path);
    if (rc == 0)
        rc = open_file(path, sizeof path, target[plen - 1] == '/', &fd, &size);
    ctx->status = rc == 0 ? 200 : rc; /* 접근 로그용 (Q5 지도 Q3) */
    if (rc == 301) {
        char loc[HTTP_LINE_MAX + 32];
        // Location = 경로 + "/" + 원래 쿼리 (target은 파서가 제어 문자를 걸렀으므로 헤더에 그대로 넣어도 안전)
        int n = snprintf(loc, sizeof loc, "Location: %.*s/%s\r\n", (int)plen, target,
                         target + plen);
        int built = -1;
        if (n > 0 && (size_t)n < sizeof loc)
            built = http_build_header(&out, 301, NULL, 0, ctx->keep_alive, loc);
        return send_built(ctx->fd, &out, built);
    }
    if (rc != 0 && head) /* HEAD 응답에는 본문이 없어야 하므로 오류여도 헤더만 (Q4) */
        return send_built(ctx->fd, &out,
                          http_build_header(&out, rc, NULL, 0, ctx->keep_alive, NULL));
    if (rc != 0)
        return send_built(ctx->fd, &out, http_build_error(&out, rc, ctx->keep_alive));

    const char *type = mime_from_path(path);
    int ok = http_build_header(&out, 200, type, (long)size, ctx->keep_alive, NULL) == 0
             && net_write_all(ctx->fd, out.data, out.len) == 0
             && (head || send_file_body(ctx->fd, fd, size) == 0);
    ctx->bytes = ok && !head ? (size_t)size : 0;
    buf_free(&out);
    close(fd);
    return ok ? 0 : -1;
}
