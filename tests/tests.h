/* 最小テストフレームワーク。fail 時は即座に行番号付きで報告。 */
#ifndef IFUTO_TESTS_H
#define IFUTO_TESTS_H

#include <stdio.h>
#include <stdlib.h>

extern int g_if_test_failures;
extern int g_if_test_checks;

#define CHECK(cond) do { \
    g_if_test_checks++; \
    if (!(cond)) { \
        g_if_test_failures++; \
        fprintf(stderr, "FAIL %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define RUN(fn) do { \
    fprintf(stderr, "  %-40s", #fn); \
    fn(); \
    fprintf(stderr, "ok\n"); \
} while (0)

#endif
