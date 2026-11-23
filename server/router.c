#include "server/router.h"

#include <string.h>

#include "server/proxy.h"
#include "server/static_handler.h"

// 요청 경로와 가장 길게 일치하는 라우트를 반환한다 (경로 조각 단위). 없으면 NULL
// 이유: "/api/"와 "/"가 둘 다 맞을 때 더 구체적인 쪽을 고르고, "/api"가 "/apix"를 잡지 않게 한다 (Q13)
const struct route *router_match(const struct server_config *c, const char *target)
{
    size_t plen = strcspn(target, "?");
    const struct route *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < c->route_count; i++) {
        const struct route *r = &c->routes[i];
        size_t n = strlen(r->prefix);
        if (n > plen || strncmp(target, r->prefix, n) != 0)
            continue;
        // prefix가 /로 끝나지 않으면, 다음 글자가 경로의 끝이거나 /여야 같은 경로 조각이다
        if (r->prefix[n - 1] != '/' && n < plen && target[n] != '/')
            continue;
        if (best == NULL || n > best_len) {
            best = r;
            best_len = n;
        }
    }
    return best;
}

// 라우트 종류에 맞는 핸들러 함수를 반환한다 (정적 → static_handle, 프록시 → proxy_handle)
// 이유: 호출하는 쪽(conn)이 어떤 핸들러인지 몰라도 같은 모양으로 부를 수 있게 한다
handler_fn router_handler(const struct route *r)
{
    return r->type == ROUTE_PROXY ? proxy_handle : static_handle;
}
