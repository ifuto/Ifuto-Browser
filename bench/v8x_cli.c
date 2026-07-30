/* v8x_cli — V8x をファイルから実行する最小 CLI（比較ベンチ・手動検証用）。
 *
 * 用途:
 *   v8x_cli FILE          ... 最後の式文の値を stdout に出力（chromium 系 qjs の print 相当）
 *   v8x_cli FILE REPS     ... REPS 回 fresh-engine で実行し、EVAL_MS 統計を stderr へ
 *
 * 計測の公平性の根拠（BENCH.md の採用方式と一致させること）:
 *   - REPS>=1 の内部ループは「エンジン生成→eval→破棄」を 1 単位とし、
 *     起動定数を含めない純エンジン時間を得る。
 *   - プロセス wall / peak RSS は外部ランナ（bench/vsx.py）が測る。
 */
#define _POSIX_C_SOURCE 200809L
#include "../src/v8x/v8x.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* 値を「JS の print 相当」で出す。整数値ドットなし / 非整数は %.17g。
 * qjs/node 側との stdout 突合は vsx.py が数値正規化して行う。 */
static void print_val(V8xRT *rt, V8xVal v) {
    double d;
    bool b;
    uint32_t len;
    if (v8x_as_num(v, &d)) {
        if (isfinite(d) && d == (double)(long long)d && fabs(d) < 9007199254740992.0)
            printf("%.0f\n", d);
        else
            printf("%.17g\n", d);
        return;
    }
    if (v8x_as_bool(v, &b)) { printf("%s\n", b ? "true" : "false"); return; }
    if (v8x_is_null(v)) { printf("null\n"); return; }
    if (v8x_is_undefined(v)) { printf("undefined\n"); return; }
    const char *s = v8x_as_str(rt, v, &len);
    if (s) { fwrite(s, 1, len, stdout); fputc('\n', stdout); return; }
    printf("[function]\n");
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: v8x_cli FILE [REPS]\n");
        return 2;
    }
    /* 既定は製品値のまま。比較ベンチなど「打ち切りなし」で収束させたい時だけ
     * 環境変数で引上げる（組込側責任 API v8x_tune を CLI から踏む）。
     * V8X_TUNE=INSNS,HEAP_MB,OBJS  例: V8X_TUNE=200000000,256,2000000 */
    unsigned long long t_insn = 0, t_heap = 0, t_objs = 0;
    const char *tn = getenv("V8X_TUNE");
    if (tn && tn[0]) sscanf(tn, "%llu,%llu,%llu", &t_insn, &t_heap, &t_objs);
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    if (fseek(f, 0, SEEK_END) != 0) return 2;
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) return 2;
    char *src = (char *)malloc((size_t)n + 1);
    if (!src) return 2;
    if (n && fread(src, 1, (size_t)n, f) != (size_t)n) return 2;
    src[n] = 0;
    fclose(f);

    int reps = argc == 3 ? atoi(argv[2]) : 1;
    if (reps < 1) reps = 1;
    double *ts = (double *)malloc(sizeof(double) * (size_t)reps);
    if (!ts) return 2;

    V8xVal v = 0;
    for (int r = 0; r < reps; r++) {
        V8xRT *rt = v8x_new();
        if (!rt) return 2;
        if (tn && tn[0]) v8x_tune(rt, (uint64_t)t_insn, (uint32_t)t_heap, (uint32_t)t_objs);
        double t0 = now_ms();
        bool ok = v8x_eval(rt, src, &v);
        ts[r] = now_ms() - t0;
        if (!ok) {
            fprintf(stderr, "error: %s\n", v8x_error(rt));
            v8x_free(rt);
            return 1;
        }
        if (r == reps - 1) print_val(rt, v);
        v8x_free(rt);
    }
    if (reps > 1) {
        qsort(ts, (size_t)reps, sizeof *ts, cmp_d);
        double sum = 0;
        for (int r = 0; r < reps; r++) sum += ts[r];
        fprintf(stderr, "EVAL_MS n=%d median=%.3f min=%.3f max=%.3f mean=%.3f\n",
                reps, ts[reps / 2], ts[0], ts[reps - 1], sum / reps);
    } else {
        fprintf(stderr, "EVAL_MS n=1 median=%.3f\n", ts[0]);
    }
    free(ts);
    free(src);
    return 0;
}
