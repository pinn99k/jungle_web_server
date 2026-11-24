#include "common/buf.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BUF_INIT_CAP 4096

// 빈 버퍼로 초기화한다
// 이유: 메모리는 처음 붙일 때 빌린다. 빈 연결마다 4KB를 미리 잡지 않기 위해서다
void buf_init(struct buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

// 뒤에 n바이트를 붙이고, 공간이 모자라면 2배로 늘린다. 성공 0, 실패 -1
// 이유: read()는 아무 데서나 끊어 주므로 조각을 이어 붙여야 요청 하나를 볼 수 있다
int buf_append(struct buf *b, const char *data, size_t n)
{
    if (n == 0)
        return 0;
    // len + n이 size_t 최댓값을 넘으면 작은 수로 되돌아가, 작은 버퍼에 큰 데이터를 쓰게 된다 (넘침 방어)
    if (n > SIZE_MAX - b->len)
        return -1;
    size_t need = b->len + n;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : BUF_INIT_CAP;
        // 핵심: 2배씩 늘리면 복사가 가끔만 일어나 붙이기 한 번의 평균 비용이 일정하다
        //       (모자란 만큼만 늘리면 붙일 때마다 전체를 복사하게 됨)
        while (cap < need)
            cap = (cap > SIZE_MAX / 2) ? need : cap * 2;
        // realloc이 실패해도 기존 data는 그대로 남으므로 새 포인터로 받는다.
        // 핵심: realloc은 메모리를 다른 곳으로 옮길 수 있다 → 전에 b->data 안을 가리키던 포인터는 모두
        //       무효가 된다. 그래서 파서는 위치를 포인터가 아니라 "시작부터 몇 번째"(pos)로 기억한다 (스텝 2)
        char *p = realloc(b->data, cap);
        if (p == NULL)
            return -1;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, n);
    b->len = need;
    return 0;
}

// 앞에서 n바이트를 지우고 나머지를 앞으로 당긴다
// 이유: 처리한 요청만 지우고, 뒤에 붙어 온 다음 요청 바이트는 남겨 두기 위해서다
void buf_consume(struct buf *b, size_t n)
{
    if (n >= b->len) {
        b->len = 0;
        return;
    }
    // 옮기는 앞뒤 영역이 겹치므로 memcpy가 아니라 memmove (memcpy는 겹치면 결과가 정해지지 않음)
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

// 메모리를 해제하고 빈 상태로 되돌린다
// 이유: 해제 뒤에 다시 써도 빈 버퍼로 동작하게 해서 해제한 메모리를 건드리는 실수를 막는다
void buf_free(struct buf *b)
{
    free(b->data);
    buf_init(b);
}
