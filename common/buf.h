#ifndef BUF_H
#define BUF_H

#include <stddef.h>

// 크기가 늘어나는 바이트 버퍼. data[0..len)이 채워진 부분이고 cap은 할당한 크기다
struct buf {
    char *data;
    size_t len;
    size_t cap;
};

// 빈 버퍼로 초기화한다
void buf_init(struct buf *b);

// 뒤에 n바이트를 붙이고, 공간이 모자라면 2배로 늘린다. 성공 0, 실패 -1
int buf_append(struct buf *b, const char *data, size_t n);

// 앞에서 n바이트를 지우고 나머지를 앞으로 당긴다
void buf_consume(struct buf *b, size_t n);

// 메모리를 해제하고 빈 상태로 되돌린다
void buf_free(struct buf *b);

#endif
