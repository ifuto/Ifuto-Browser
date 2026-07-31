/* Akl 単体 CLI（比較ベンチ・guard 用。ブラウザ本体バイナリには不加入）。
 * 使い方: akl_cli [--times N] [--rss] file.js
 *   --times N : 新規ランタイムで N 回評価し中央値/min/max を出力（N<=256）
 *   --rss     : 終了時に ru_maxrss(KB) を出力
 * 評価失敗なら exit 1。これはエンジン契約の一部（guard が異常検知に使う）。 */
#define _POSIX_C_SOURCE 200809L
#include "../src/akl/akl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <sys/resource.h>

/* 値を「JS の print 相当」で出力（兄弟 harness bench/vsx.py との stdout 突合規約）。
 * 整数値はドットなし / 非整数は %.17g。vsx 側は数値正規化して照合する。 */
static void print_val(AklRT *rt, AklVal v) {
    double d;
    bool b;
    uint32_t len;
    if (akl_as_num(v, &d)) {
        if (isnan(d)) { printf("NaN\n"); return; }
        if (isinf(d)) { printf("%sInfinity\n", d > 0 ? "" : "-"); return; }
        if (d == (double)(long long)d && fabs(d) < 9007199254740992.0)
            printf("%.0f\n", d);
        else
            printf("%.17g\n", d);
        return;
    }
    if (akl_as_bool(v, &b)) { printf("%s\n", b ? "true" : "false"); return; }
    if (akl_is_null(v)) { printf("null\n"); return; }
    if (akl_is_undefined(v)) { printf("undefined\n"); return; }
    const char *s = akl_as_str(rt, v, &len);
    if (s) { fwrite(s, 1, len, stdout); fputc('\n', stdout); return; }
    printf("[function]\n");
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
static int cmpd(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}
static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    char *b = (char *)malloc((size_t)sz + 1);
    if (!b) { fclose(f); return 0; }
    if (sz && fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return 0; }
    b[sz] = 0;
    fclose(f);
    return b;
}

int main(int argc, char **argv) {
    int times = 1, want_rss = 0;
    unsigned long long budget = 1000000000000ull; /* bench 用途: 既定は実質無制限。--budget で縮めて枯渇挙動も検査可 */
    const char *file = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--times") && i + 1 < argc) times = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rss")) want_rss = 1;
        else if (!strcmp(argv[i], "--budget") && i + 1 < argc) budget = strtoull(argv[++i], 0, 10);
        else if (!file) file = argv[i];
        else {
            int r2 = atoi(argv[i]); /* 兄弟規約の位置引数 REPS */
            if (r2 > 0) times = r2;
        }
    }
    if (!file || times < 1) { fprintf(stderr, "usage: akl_cli [--times N] [--rss] file.js\n"); return 2; }
    if (times > 256) times = 256;
    char *src = slurp(file);
    if (!src) { fprintf(stderr, "cannot read %s\n", file); return 2; }
    double ts[256];
    /* 兄弟 CLI 規約: AKL_TUNE=INSNS,HEAP_MB,OBJS で上限引上げ（--budget は insn 側の別経路） */
    unsigned long long t_insn = 0, t_heap = 0, t_objs = 0;
    const char *tn = getenv("AKL_TUNE");
    if (tn && tn[0]) sscanf(tn, "%llu,%llu,%llu", &t_insn, &t_heap, &t_objs);
    for (int t = 0; t < times; t++) {
        AklRT *rt = akl_new();
        if (!rt) { fprintf(stderr, "akl_new failed\n"); return 1; }
        akl_tune(rt, (uint64_t)t_insn, (uint32_t)t_heap, (uint32_t)t_objs);
        if (!tn || !tn[0]) akl_set_insn_budget(rt, budget);
        AklVal v;
        double t0 = now_ms();
        bool ok = akl_eval(rt, src, &v);
        ts[t] = now_ms() - t0;
        if (!ok) { fprintf(stderr, "eval failed: %s\n", akl_error(rt)); akl_free(rt); return 1; }
        if (t == times - 1) print_val(rt, v); /* 最終 rep の完了値（vsx 互換の print 規約） */
        akl_free(rt);
    }
    qsort(ts, (size_t)times, sizeof *ts, cmpd);
    fprintf(stderr, "EVAL_MS n=%d median %.3f min %.3f max %.3f\n", times, ts[times / 2], ts[0], ts[times - 1]);
    if (want_rss) {
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        fprintf(stderr, "maxrss %ld KB\n", ru.ru_maxrss);
    }
    free(src);
    return 0;
}
