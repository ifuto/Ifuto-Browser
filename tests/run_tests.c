#include "tests.h"

int g_if_test_failures = 0;
int g_if_test_checks = 0;

void test_arena(void);
void test_utf8(void);
void test_html(void);
void test_css(void);
void test_layout(void);
void test_uichrome(void);
void test_v8x(void);
void test_md(void);

int main(void) {
    fprintf(stderr, "[arena]\n");
    RUN(test_arena);
    fprintf(stderr, "[utf8]\n");
    RUN(test_utf8);
    fprintf(stderr, "[html]\n");
    RUN(test_html);
    fprintf(stderr, "[css]\n");
    RUN(test_css);
    fprintf(stderr, "[layout]\n");
    RUN(test_layout);
    RUN(test_uichrome);
    fprintf(stderr, "[v8x]\n");
    RUN(test_v8x);
    fprintf(stderr, "[md]\n");
    RUN(test_md);
    fprintf(stderr, "----\nchecks: %d, failures: %d\n", g_if_test_checks, g_if_test_failures);
    return g_if_test_failures ? 1 : 0;
}
