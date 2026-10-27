/* 极简单元测试框架：无依赖，失败不中断，最后汇总。 */
#ifndef TINYTEST_H
#define TINYTEST_H

#include <stdio.h>

extern int tt_checks;
extern int tt_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        tt_checks++;                                                         \
        if (!(cond)) {                                                       \
            tt_failures++;                                                   \
            printf("  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                             \
    do {                                                                           \
        long long tt_a_ = (long long)(a), tt_b_ = (long long)(b);                  \
        tt_checks++;                                                               \
        if (tt_a_ != tt_b_) {                                                      \
            tt_failures++;                                                         \
            printf("  %s:%d: CHECK_EQ(%s, %s) failed: %lld != %lld\n", __FILE__,   \
                   __LINE__, #a, #b, tt_a_, tt_b_);                                \
        }                                                                          \
    } while (0)

#define CHECK_MEM(a, b, n) CHECK(memcmp((a), (b), (n)) == 0)

#define RUN_TEST(fn)                       \
    do {                                   \
        int tt_before_ = tt_failures;      \
        fn();                              \
        printf("[%s] %s\n", tt_failures == tt_before_ ? " OK " : "FAIL", #fn); \
    } while (0)

#endif /* TINYTEST_H */
