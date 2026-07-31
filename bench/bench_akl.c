/* Akl dispatch 方式の実測比較ベンチ（computed-goto vs switch）。
 * Makefile で 2 バイナリ（bench_akl / bench_akl_switch）に分けてビルドし、
 * 同一ワークロードの中央値を BENCH.md に公開する。これは「ユーザに dispatch 選択が
 * 委任された」ことへの回答の根拠データであり、推定値は書かない。 */
#define _POSIX_C_SOURCE 200809L
#include "../src/akl/akl.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

typedef struct { const char *name; const char *src; double want; } Wk;
static const Wk W[] = {
    { "fib(22) recursive",
      "function fib(n){ if (n<2) return n; return fib(n-1)+fib(n-2); } fib(22)",
      17711 },
    { "arith loop 100k",
      "var s = 0; for (var i = 0; i < 100000; i = i+1) { s = (s + i*3 + 1) % 1000003; } s",
      905003 },
    { "mixed stmt loop 20k",
      "var acc = 0; for (var i = 0; i < 20000; i = i+1) {"
      " var t = i * 2; if (t % 5 == 0) { acc = acc + t; } else { acc = acc + 1; } } acc",
      79996000 },
    { "str concat 2k",
      "var s = ''; for (var i = 0; i < 2000; i = i+1) { s = 'x' + i; } s;",
      -2 /* 文字列戻り: 値検査は oknum=false でスキップ */ },
};
#define REPS 7

int main(void) {
#ifdef AKL_TEST_SWITCH_DISPATCH
    printf("dispatch: switch\n");
#else
    printf("dispatch: computed-goto\n");
#endif
    for (unsigned w = 0; w < sizeof W / sizeof *W; w++) {
        double ts[REPS];
        double d = 0;
        bool oknum = true;
        for (int r = 0; r < REPS; r++) {
            AklRT *rt = akl_new();
            if (!rt) return 2;
            AklVal v;
            double t0 = now_ms();
            bool ok = akl_eval(rt, W[w].src, &v);
            ts[r] = now_ms() - t0;
            if (!ok) { fprintf(stderr, "eval failed: %s\n", akl_error(rt)); akl_free(rt); return 1; }
            oknum = akl_as_num(v, &d);
            if (r == 0 && W[w].want != -2 && (!oknum || d != W[w].want)) {
                fprintf(stderr, "want mismatch for %s: got %g want %g (fix want or engine)\n",
                        W[w].name, oknum ? d : -9999.0, W[w].want);
                akl_free(rt);
                return 1;
            }
            akl_free(rt);
        }
        qsort(ts, REPS, sizeof *ts, cmp_d);
        printf("%-22s median %8.3f ms  min %8.3f  max %8.3f  (value %g)\n",
               W[w].name, ts[REPS / 2], ts[0], ts[REPS - 1], oknum ? d : -1.0);
    }
    return 0;
}
