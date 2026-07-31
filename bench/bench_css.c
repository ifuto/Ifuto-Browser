/* 合成 CSS カスケードベンチ: naive 全走査 vs RuleSet 索引（Blink RuleSet 戦略相当）。
 * 同一バイナリで if_css_set_naive_matching を切り替えて両経路を計測し、
 * 全ノードの計算済みスタイルがビット一致することも同時に検査する（推定値なし・実測のみ）。
 *
 * ワークロード（代表的 Web 構造の類似）: 2500 ルール（class/tag/id/universal/複合混在）
 * × 3000 要素（class/id 部分集合、浅いネスト）。 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/css.h"

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static u32 rng32(u32 *st) { u32 x = *st; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *st = x; }

int main(void) {
    enum { NRULES = 2500, NELEM = 3000 };
    static char doc[1 << 20];
    u32 st = 0xC0FFEE;
    u32 dp = 0;
    dp += (u32)sprintf(doc + dp, "<style>");
    /* 実在類似の分布: class 主体、tag 次点、id 少数、universal/複合少数 */
    for (u32 r = 0; r < NRULES; r++) {
        u32 kind = rng32(&st) % 100;
        if (kind < 62)      dp += (u32)sprintf(doc + dp, ".c%u", rng32(&st) % 400);
        else if (kind < 80) dp += (u32)sprintf(doc + dp, "%s", (rng32(&st) & 1) ? "div" : "span");
        else if (kind < 86) dp += (u32)sprintf(doc + dp, "#i%u", rng32(&st) % 60);
        else if (kind < 90) dp += (u32)sprintf(doc + dp, "*");
        else if (kind < 96) dp += (u32)sprintf(doc + dp, "div .c%u", rng32(&st) % 400);
        else                dp += (u32)sprintf(doc + dp, ".c%u>.c%u", rng32(&st) % 400, rng32(&st) % 400);
        if ((rng32(&st) % 4) == 0) dp += (u32)sprintf(doc + dp, ",.c%u", rng32(&st) % 400);
        dp += (u32)sprintf(doc + dp, "{color:rgb(%u,%u,%u);background-color:rgb(%u,%u,%u)",
            rng32(&st) % 256, rng32(&st) % 256, rng32(&st) % 256,
            rng32(&st) % 256, rng32(&st) % 256, rng32(&st) % 256);
        dp += (u32)sprintf(doc + dp, "}");
    }
    dp += (u32)sprintf(doc + dp, "</style><body>");
    for (u32 e = 0; e < NELEM; e++) {
        const char *tg = (rng32(&st) & 1) ? "div" : "span";
        dp += (u32)sprintf(doc + dp, "<%s class=\"c%u c%u\"%s>x</%s>",
            tg, rng32(&st) % 400, rng32(&st) % 400,
            (rng32(&st) % 20) == 0 ? " id=i0" : "", tg);
    }
    dp += (u32)sprintf(doc + dp, "</body>");

    double t_naive = 1e9, t_index = 1e9;
    for (int round = 0; round < 5; round++) {
        for (int mode = 0; mode < 2; mode++) {
            IfArena a; if_arena_init(&a, 1u << 24);
            double t0 = now_s();
            IfDom *d = if_parse_html(&a, if_str(doc, (u32)strlen(doc)));
            if_css_set_naive_matching(mode == 0);
            if_style_apply(&a, d);
            double dt = now_s() - t0;
            if (mode == 0 && dt < t_naive) t_naive = dt;
            if (mode == 1 && dt < t_index) t_index = dt;
            /* （正しさの恒常監査は tests/test_css.c の on/off oracle が担う。bench は速度計） */
            if_arena_destroy(&a);
        }
    }
    if_css_set_naive_matching(0);
    printf("css cascade bench: %d rules x %d elements (synthetic, measured locally)\n", NRULES, NELEM);
    printf("  naive full-scan : %9.3f ms\n", t_naive * 1e3);
    printf("  ruleset index   : %9.3f ms\n", t_index * 1e3);
    printf("  speedup         : %8.2fx\n", t_naive / t_index);
    return 0;
}
