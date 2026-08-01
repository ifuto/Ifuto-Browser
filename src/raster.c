/* 実装は raster.h を参照。基準は scalar(u32x1) で、全候補はそれと
 * bit-exact に一致しないと採用しない（tests/test_raster.c で相互証明）。 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime(CLOCK_MONOTONIC) */
#include "raster.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>

/* ---- 候補 kernel（dst 任意オフセット安全: 8B 境界は prologue で揃える） ---- */

static void fill_u32x1(u32 *dst, u64 n, u32 v) { /* 基準（O2 は SSE2 化しうる。それも実測の一部） */
    for (u64 i = 0; i < n; i++) dst[i] = v;
}

static void fill_u64x2(u32 *dst, u64 n, u32 v) { /* 8B store ×2 unroll */
    if (((uintptr_t)dst & 7u) && n) { *dst++ = v; n--; }
    u64 w = ((u64)v << 32) | v;
    u64 *p = (u64 *)dst; /* -fno-strict-aliasing 下で明示 */
    u64 q = n >> 1;
    u64 i = 0;
    for (; i + 2 <= q; i += 2) { p[i] = w; p[i + 1] = w; }
    for (; i < q; i++) p[i] = w;
    if (n & 1u) ((u32 *)p)[n & ~(u64)1] = v; /* 端数 1 px */
}

static void fill_u64x8(u32 *dst, u64 n, u32 v) { /* 64B/iter（現行 x86-64 の store port 飽和狙い） */
    if (((uintptr_t)dst & 7u) && n) { *dst++ = v; n--; }
    u64 w = ((u64)v << 32) | v;
    u64 *p = (u64 *)dst;
    u64 q = n >> 1;
    u64 i = 0;
    for (; i + 8 <= q; i += 8) {
        p[i] = w; p[i+1] = w; p[i+2] = w; p[i+3] = w;
        p[i+4] = w; p[i+5] = w; p[i+6] = w; p[i+7] = w;
    }
    for (; i < q; i++) p[i] = w;
    if (n & 1u) ((u32 *)p)[n & ~(u64)1] = v;
}

/* 白 0xFFFFFF 等の「4 byte 均一色」なら glibc memset（AVX2 ifunc）に直行。
 * ページ bg 塗りは事実上このパターンが支配的、という実務観測に基づく特化。 */
static void fill_smart(u32 *dst, u64 n, u32 v) {
    /* v の 4 バイトが全て同値 ⇔ 上位3バイト==下位3バイト かつ byte0==byte1 */
    if ((v >> 8) == (v & 0xFFFFFFu)) {
        memset(dst, (int)(v & 0xFFu), n * 4);
        return;
    }
    fill_u64x2(dst, n, v);
}

static void (*const K[IF_RASTER_MAX_CAND])(u32 *, u64, u32) = {
    fill_u32x1, fill_u64x2, fill_u64x8, fill_smart
};
static const char *const KNAME[IF_RASTER_MAX_CAND] = {
    "u32x1(scalar)", "u64x2(8B)", "u64x8(64B)", "smart(memset)"
};
#define N_K 4

int         if_raster_n_kernels(void) { return N_K; }
const char *if_raster_kernel_name(int kid) {
    return (kid >= 0 && kid < N_K) ? KNAME[kid] : "?";
}
void if_fill32_kernel(int kid, u32 *dst, u64 n, u32 v) {
    if (kid >= 0 && kid < N_K) K[kid](dst, n, v);
}

/* ---- 選択（プロセス 1 回） ---- */
static int g_sel = 0;
void if_fill32(u32 *dst, u64 n, u32 v) { K[g_sel](dst, n, v); }

static u64 now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static IfRasterPick g_pick;

static void gpu_probe(IfRasterPick *p) {
    struct stat st;
    if (stat("/dev/dri/renderD128", &st) == 0 || stat("/dev/dri/card0", &st) == 0) {
        p->gpu_node = true;
        snprintf(p->gpu_note, sizeof p->gpu_note,
                 "GPU ノード検出 (/dev/dri)。純 libc 方針により DRM ioctl 非接続"
                 "（GPU は描画経路に使わない。選択対象は CPU kernel）");
    } else {
        p->gpu_node = false;
        snprintf(p->gpu_note, sizeof p->gpu_note,
                 "GPU ノード未検出。CPU raster のみ（ldd = libc+libm の製品法則）");
    }
}

/* 1 測定: 512KiB バッファ（GUI strip 実サイズ相当、再利用＝warm cache も本番態勢どおり）
 * を reps 回 fill した最小 ns から MB/s を返す。 */
static double bench_one(void (*fn)(u32 *, u64, u32),
                        u32 *buf, u64 n, u32 color) {
    /* ウォーム + キャリブレーション（測定区間 ~0.4ms/trial × 2 trial。
     * 起動コストを守るため microbench 総量は ~8ms 以内に抑える） */
    fn(buf, n, color);
    u64 t0 = now_ns();
    fn(buf, n, color);
    u64 rep_ns = now_ns() - t0;
    if (rep_ns < 1000) rep_ns = 1000;
    u64 reps = 400000ull / rep_ns;
    if (reps < 4) reps = 4;
    if (reps > 512) reps = 512;
    u64 best = ~(u64)0;
    for (int trial = 0; trial < 2; trial++) {
        u64 a = now_ns();
        for (u64 r = 0; r < reps; r++) fn(buf, n, color);
        u64 b = now_ns() - a;
        if (b < best) best = b;
    }
    double bytes = (double)(n * 4ull) * (double)reps;
    return bytes * 1000.0 / (double)best; /* MB/s（10^6 基準） */
}

const IfRasterPick *if_raster_autodetect(void) {
    if (g_pick.done) return &g_pick;
    IfRasterPick *p = &g_pick;
    memset(p, 0, sizeof *p);
    u64 all0 = now_ns();
    const u64 N = 512 * 1024 / 4; /* px 数（=512KiB/4B） */
    u32 *buf = (u32 *)aligned_alloc(64, N * 4);
    if (!buf) { /* フォールバック: ベンチ不能でも安全側の既定で動く */
        p->selected = 1; /* u64x2 */
        p->n_cand = N_K;
        p->done = true;
        g_sel = 1;
        gpu_probe(p);
        return p;
    }
    const u32 UNIFORM = 0xFFFFFFu;   /* ページ bg 支配色 */
    const u32 MIXED   = 0x3D5AF1u;   /* 非均一（accent 相当） */
    p->n_cand = N_K;
    for (int k = 0; k < N_K; k++) {
        p->name[k] = KNAME[k];
        p->mb_uniform[k] = bench_one(K[k], buf, N, UNIFORM);
        p->mb_mixed[k]   = bench_one(K[k], buf, N, MIXED);
        /* bg 塗りが支配（各行の全面塗り）なので 7:3 加重 */
        p->score[k] = 0.7 * p->mb_uniform[k] + 0.3 * p->mb_mixed[k];
    }
    int sel = 0;
    for (int k = 1; k < N_K; k++)
        if (p->score[k] > p->score[sel]) sel = k;
    p->selected = sel;
    g_sel = sel;
    free(buf);
    p->bench_us = (now_ns() - all0) / 1000;
    gpu_probe(p);
    p->done = true;
    return p;
}
