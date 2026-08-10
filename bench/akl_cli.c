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
#include "../src/sandbox.h"
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
/* モジュール構文（import/export の単語）を含むか。文字列/コメント中の偽陽性は
 * 「より安全側」= サンドボックス無効化方向のみに作用する。 */
static bool src_has_module_syntax(const char *s) {
    while (*s) {
        while (*s && !((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_')) s++;
        if (!*s) break;
        const char *w = s;
        while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_' || (*s >= '0' && *s <= '9')) s++;
        size_t ln = (size_t)(s - w);
        if ((ln == 6 && memcmp(w, "import", 6) == 0) || (ln == 6 && memcmp(w, "export", 6) == 0)) return true;
    }
    return false;
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
    int times = 1, want_rss = 0, want_sandbox = 1, use_cojit = 1;
    /* CLI 製品既定 budget: 500M ops（実測で bench/js 最大は arith の <=100M。
     * 5x の余裕を持たせつつ、暴走スクリプトは数秒で確実に死ぬ上限）。
     * 旧来の CLI 既定 1e12 は「while(1){} が実質無限に CPU を焼く」状態で、
     * fail-stop 規則と CPU 非酷使規則の双方に違反していた（ユーザ報告で検出）。
     * --budget N で明示変更。--budget 0 はエンジン既定（現在 10M ops、埋込向け）。 */
    unsigned long long budget = 500000000ull;
    const char *file = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--times") && i + 1 < argc) times = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rss")) want_rss = 1;
        else if (!strcmp(argv[i], "--no-sandbox")) want_sandbox = 0;
        else if (!strcmp(argv[i], "--no-cojit")) use_cojit = 0;
        else if (!strcmp(argv[i], "--budget") && i + 1 < argc) budget = strtoull(argv[++i], 0, 10);
        else if (!file) file = argv[i];
        else {
            int r2 = atoi(argv[i]); /* 兄弟規約の位置引数 REPS */
            if (r2 > 0) times = r2;
        }
    }
    if (!file || times < 1) { fprintf(stderr, "usage: akl [--times N] [--rss] [--budget N] [--no-sandbox] [--no-cojit] file.js\n"); return 2; }
    if (times > 256) times = 256;
    char *src = slurp(file);
    if (!src) { fprintf(stderr, "cannot read %s\n", file); return 2; }
    /* v0.5: ファイルベースのモジュールローダ（import 解決）。エントリはモジュールとして
     * も評価される（top-level import/export を含む場合。通常は classic と同一動作） */
    extern void cli_module_loader(AklRT *rt, const char *spec, const char *base,
                                  void *udata, char **out_src, char **out_id);
    /* モジュール構文を含むソースは、ローダのファイル I/O がサンドボックスの
     * open 禁止と構造的に衝突するため、サンドボックスを適用しない（明示通知） */
    if (src_has_module_syntax(src)) {
        if (want_sandbox) {
            fprintf(stderr, "[cli] module syntax detected: sandbox disabled (module loader needs file I/O)\n");
            want_sandbox = 0;
        }
    }
    /* ハッキング耐性（ユーザ要求）: 入力の解釈・実行は不可逆サンドボックスの内側で行う。
     * ファイルは lock 前に読み込み済み（allowlist は open を含まない）。
     * --no-sandbox 指定時のみ明示的に素通し（デバッグ用。規定は ON） */
    if (want_sandbox) {
        if (if_sandbox_apply(IF_SB_AKL) != 0) {
            fprintf(stderr, "sandbox unavailable on this kernel (use --no-sandbox to bypass, 安全側のため既定では終了)\n");
            free(src);
            return 2;
        }
    }
    double ts[256];
    /* 兄弟 CLI 規約: AKL_TUNE=INSNS,HEAP_MB,OBJS で上限引上げ（--budget は insn 側の別経路） */
    unsigned long long t_insn = 0, t_heap = 0, t_objs = 0;
    const char *tn = getenv("AKL_TUNE");
    if (tn && tn[0]) sscanf(tn, "%llu,%llu,%llu", &t_insn, &t_heap, &t_objs);
    for (int t = 0; t < times; t++) {
        AklRT *rt = akl_new();
        if (!rt) { fprintf(stderr, "akl_new failed\n"); return 1; }
        akl_tune(rt, (uint64_t)t_insn, (uint32_t)t_heap, (uint32_t)t_objs);
        if (!tn || !tn[0]) { if (budget) akl_set_insn_budget(rt, budget); }
        if (!use_cojit) akl_set_cojit(rt, 0);
        AklVal v;
        akl_set_module_loader(rt, cli_module_loader, NULL);
        akl_set_module_base(rt, file);
        double t0 = now_ms();
        bool ok = akl_eval(rt, src, &v);
        if (!ok) {
            /* モジュール専用構文（import/export 宣言）が原因ならモジュールとして再試行。
             * パース失敗は実行前に確定するため副作用なし（安全な再試行）。 */
            const char *er = akl_error(rt);
            if (er && (strstr(er, "only allowed in modules"))) {
                ok = akl_eval_module(rt, src, file, &v);
            }
        }
        ts[t] = now_ms() - t0;
        if (!ok) { fprintf(stderr, "eval failed: %s\n", akl_error(rt)); akl_free(rt); free(src); return 1; }
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
