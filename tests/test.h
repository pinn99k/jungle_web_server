#ifndef TEST_H
#define TEST_H

// 아주 작은 테스트 틀. 테스트 파일 하나 = 실행 파일 하나라서 카운터를 static으로 둔다
#include <stdio.h>
#include <string.h>

static int test_failures;
static int test_runs;

// 조건이 거짓이면 파일 · 줄 번호와 함께 실패를 기록한다
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            test_failures++;                                                     \
            printf("  실패 %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                        \
    } while (0)

// 두 정수가 같은지 확인하고, 다르면 두 값을 출력한다
#define CHECK_INT(a, b)                                                          \
    do {                                                                         \
        long long a_ = (long long)(a), b_ = (long long)(b);                      \
        if (a_ != b_) {                                                          \
            test_failures++;                                                     \
            printf("  실패 %s:%d: %s = %lld, 기대 %lld\n", __FILE__, __LINE__,    \
                   #a, a_, b_);                                                  \
        }                                                                        \
    } while (0)

// 두 문자열이 같은지 확인하고, 다르면 두 값을 출력한다 (NULL도 비교)
#define CHECK_STR(a, b)                                                          \
    do {                                                                         \
        const char *a_ = (a), *b_ = (b);                                         \
        int same_ = (a_ == NULL || b_ == NULL) ? a_ == b_ : strcmp(a_, b_) == 0; \
        if (!same_) {                                                            \
            test_failures++;                                                     \
            printf("  실패 %s:%d: %s = \"%s\", 기대 \"%s\"\n", __FILE__, __LINE__, \
                   #a, a_ ? a_ : "(NULL)", b_ ? b_ : "(NULL)");                  \
        }                                                                        \
    } while (0)

// 테스트 함수를 실행하고 통과 / 실패를 출력한다
#define RUN(fn)                                                                  \
    do {                                                                         \
        int before_ = test_failures;                                             \
        test_runs++;                                                             \
        fn();                                                                    \
        printf("%s %s\n", test_failures == before_ ? "통과" : "실패", #fn);       \
    } while (0)

// 결과를 요약하고 main의 종료 코드를 돌려준다. 실패가 있으면 1
#define TEST_EXIT() \
    (printf("테스트 %d개, 실패 %d건\n", test_runs, test_failures), test_failures ? 1 : 0)

#endif
