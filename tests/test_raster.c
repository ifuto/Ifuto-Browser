/* raster backend 選定の正しさ:
 *  1. 全候補 kernel は scalar 基準と bit-exact に一致する（任意オフセット・任意長）
 *  2. 選択 dispatch if_fill32 も基準と一致する
 *  3. autodetect は冪等（2 回目は同じ決定を即返す） */
#include "tests.h"
#include "../src/raster.h"
#include <string.h>
#include <stdlib.h>

static void ref_fill(u32 *dst, u64 n, u32 v) {
    for (u64 i = 0; i < n; i++) dst[i] = v;
}

void test_raster(void) {
    static u32 buf[128];
    static const u32 VALS[] = { 0u, 0xFFFFFFFFu, 0xFFFFFFu, 0x111111u,
                                0x3D5AF1u, 0xF5F5F0u, 0x00FF00u, 0x80000001u };
    static const u64 NS[] = { 0, 1, 2, 3, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 65 };
    int nk = if_raster_n_kernels();
    CHECK(nk >= 2 && nk <= IF_RASTER_MAX_CAND);

    /* 1: 全 kernel × 全オフセット × 全長 × 全色 で scalar 基準とビット一致 + カナリア無傷 */
    for (int k = 0; k < nk; k++) {
        for (u64 off = 0; off < 9; off++) {
            for (u64 ni = 0; ni < sizeof NS / sizeof NS[0]; ni++) {
                for (u64 vi = 0; vi < sizeof VALS / sizeof VALS[0]; vi++) {
                    u64 n = NS[ni];
                    u32 v = VALS[vi];
                    memset(buf, 0xA5, sizeof buf);
                    if_fill32_kernel(k, buf + off, n, v);
                    for (u64 i = 0; i < 128; i++) {
                        u32 want = (i >= off && i < off + n) ? v : 0xA5A5A5A5u;
                        CHECK(buf[i] == want);
                    }
                }
            }
        }
    }

    /* 2: 選択 dispatch も基準と一致（決定的 LCG で 512 ケース） */
    (void)if_raster_autodetect(); /* 選択を確定させる（冪等） */
    u64 seed = 0x9E3779B97F4A7C15ull;
    for (int t = 0; t < 512; t++) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        u64 off = (seed >> 33) % 8;
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        u64 n = (seed >> 33) % 100;
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        u32 v = (u32)(seed >> 32);
        memset(buf, 0xA5, sizeof buf);
        static u32 wantbuf[128];
        memset(wantbuf, 0xA5, sizeof wantbuf);
        if_fill32(buf + off, n, v);
        ref_fill(wantbuf + off, n, v);
        CHECK(memcmp(buf, wantbuf, sizeof buf) == 0);
    }

    /* 3: 冪等（同一ポインタ・同一決定・done フラグ） */
    const IfRasterPick *p1 = if_raster_autodetect();
    const IfRasterPick *p2 = if_raster_autodetect();
    CHECK(p1 == p2 && p1->done);
    CHECK(p1->selected >= 0 && p1->selected < p1->n_cand);
    CHECK(p1->n_cand == nk);
    /* フォールバック（bench 不能）のとき score は全て 0 になり得るので、
     * bench 成功時だけ >0 を要請する */
    if (p1->bench_us > 0) {
        CHECK(p1->mb_uniform[p1->selected] > 0.0);
        CHECK(p1->mb_mixed[p1->selected] > 0.0);
    }
}
