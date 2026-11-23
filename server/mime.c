#include "server/mime.h"

#include <string.h>
#include <strings.h>

struct mime_entry {
    const char *ext;
    const char *type;
};

static const struct mime_entry MIME_TABLE[] = {
    {"html", "text/html; charset=utf-8"},
    {"htm", "text/html; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"js", "text/javascript; charset=utf-8"},
    {"json", "application/json; charset=utf-8"},
    {"txt", "text/plain; charset=utf-8"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"svg", "image/svg+xml"},
    {"ico", "image/x-icon"},
};

// 파일 경로의 확장자로 Content-Type을 정한다. 모르면 "application/octet-stream"
// 이유: 타입이 틀리면 브라우저가 CSS를 적용하지 않거나 그림을 내려받기로 처리한다
const char *mime_from_path(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(path, '.');
    // 마지막 / 뒤에 있는 점만 확장자로 본다 ("/a.b/c"의 점은 폴더 이름의 일부)
    if (dot != NULL && (slash == NULL || dot > slash)) {
        for (size_t i = 0; i < sizeof MIME_TABLE / sizeof MIME_TABLE[0]; i++) {
            if (strcasecmp(dot + 1, MIME_TABLE[i].ext) == 0)
                return MIME_TABLE[i].type;
        }
    }
    return "application/octet-stream";
}
