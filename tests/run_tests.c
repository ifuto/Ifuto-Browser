#include "tests.h"

int g_if_test_failures = 0;
int g_if_test_checks = 0;

void test_arena(void);
void test_utf8(void);
void test_html(void);
void test_css(void);
void test_css_ruleset_oracle(void);
void test_layout(void);
void test_layout_linkspans(void);
void test_font16_lookup(void);
void test_uichrome(void);
void test_ifuto_pages(void);
void test_akl(void);
void test_md(void);
void test_raster(void);
void test_http(void);

int main(void) {
    fprintf(stderr, "[arena]\n");
    RUN(test_arena);
    fprintf(stderr, "[utf8]\n");
    RUN(test_utf8);
    fprintf(stderr, "[html]\n");
    RUN(test_html);
    fprintf(stderr, "[css]\n");
    RUN(test_css);
    RUN(test_css_ruleset_oracle);
    fprintf(stderr, "[layout]\n");
    RUN(test_layout);
    RUN(test_layout_linkspans);
    RUN(test_font16_lookup);
    RUN(test_uichrome);
    RUN(test_ifuto_pages);
    fprintf(stderr, "[akl]\n");
    RUN(test_akl);
    fprintf(stderr, "[md]\n");
    RUN(test_md);
    fprintf(stderr, "[raster]\n");
    RUN(test_raster);
    fprintf(stderr, "[http]\n");
    RUN(test_http);
    fprintf(stderr, "----\nchecks: %d, failures: %d\n", g_if_test_checks, g_if_test_failures);
    return g_if_test_failures ? 1 : 0;
}
