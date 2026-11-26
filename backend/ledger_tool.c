// 좌석 장부 도구: 만들기(init)와 검사(dump)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend/ledger.h"

// "ledger init <파일> <초인기 정원> <인기 정원> <보통 정원> <몇 ms 뒤 열지>" 또는 "ledger dump <파일>"
// 이유: 과목 100개(초인기 1 · 인기 9 · 보통 90)를 측정 스크립트가 한 줄로 만들고 검사하게 한다
int main(int argc, char **argv)
{
    if (argc == 7 && strcmp(argv[1], "init") == 0) {
        int seats[100];
        for (int i = 0; i < 100; i++)
            seats[i] = atoi(argv[i == 0 ? 3 : i < 10 ? 4 : 5]);
        long open_at = ledger_now() + atol(argv[6]);
        if (ledger_create(argv[2], seats, 100, open_at) < 0) {
            perror("ledger init");
            return 1;
        }
        printf("%ld\n", open_at);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "dump") == 0) {
        struct ledger *l = ledger_open(argv[2]);
        if (l == NULL) {
            perror("ledger dump");
            return 1;
        }
        int bad = ledger_dump(l);
        fprintf(stderr, "정원 초과 · 중복 %d\n", bad);
        return bad != 0;
    }
    fprintf(stderr, "사용법: %s init <파일> <초인기> <인기> <보통 정원> <ms 뒤 열기> | dump <파일>\n", argv[0]);
    return 1;
}
