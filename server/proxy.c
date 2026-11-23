#include "server/proxy.h"

#include "common/buf.h"
#include "common/http_response.h"
#include "common/net.h"

// 프록시 핸들러. 스텝 7 전까지는 502를 보내는 임시 함수. 0 = 계속 가능, -1 = 닫아야 함
// 이유: 라우터가 프록시 라우트를 고르는 흐름을 백엔드 없이 먼저 확인한다. 스텝 7에서 본문을 채운다
int proxy_handle(struct request_ctx *ctx)
{
    struct buf out;
    buf_init(&out);
    int rc = -1;
    if (http_build_error(&out, 502, ctx->keep_alive) == 0)
        rc = net_write_all(ctx->fd, out.data, out.len);
    buf_free(&out);
    return rc;
}
