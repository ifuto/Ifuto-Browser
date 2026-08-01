/* Ifuto — raster backend 自動判定（起動時マイクロベンチ）
 *
 * 目的: GUI の画素 fill（fb_rect 系）に使う 32bpp fill kernel を、
 * 「この端末で実測」して選ぶ。理屈で決めない（-O2 の auto-vec や
 * glibc memset の ifunc 解決は機種依存なので、候補を全部測って最速を採る）。
 *
 * スコープの正直な枠組み:
 *   - この製品は ldd = linux-vdso/libc/ld (+libm) 縛り。DRM/GPU ドライバとの
 *     ioctl 接続は依存追加なしには不可能で、現行アーキは純 CPU raster のみ。
 *     GPU ノードの有無は「検出して表示」するが、描画経路には使わない。
 *   - ゆえに backend 選択 = CPU fill kernel の選択 + GPU 非利用の明示、
 *     という形で「CPU/GPU 自動判定」の約束（ifuto://settings）を果たす。
 *
 * 契約:
 *   - if_raster_autodetect() は冪等（2 回目以降は結果を返すだけ）。
 *   - if_fill32(dst,n,v) は選択 kernel への薄い dispatch。dst は 4B aligned、
 *     任意オフセット可（kernel 側が 8B 境界合わせの prologue を持つ）。
 *   - 全 kernel は「dst[0..n) = v」と bit-exact に等しい（テストで相互証明）。 */
#ifndef IFUTO_RASTER_H
#define IFUTO_RASTER_H

#include "common.h"

#define IF_RASTER_MAX_CAND 4

typedef struct {
    int      selected;                      /* [0, n_cand) */
    int      n_cand;
    const char *name[IF_RASTER_MAX_CAND];
    double   mb_uniform[IF_RASTER_MAX_CAND];  /* 均一色（白 bg 相当）MB/s */
    double   mb_mixed[IF_RASTER_MAX_CAND];    /* 非均一色 MB/s */
    double   score[IF_RASTER_MAX_CAND];       /* 0.7*uniform + 0.3*mixed */
    u64      bench_us;                        /* microbench 総時間 */
    bool     gpu_node;                        /* /dev/dri 実ノードの有無（情報表示のみ） */
    char     gpu_note[160];
    bool     done;
} IfRasterPick;

/* 起動時 1 度呼ぶ（以後は cached）。戻り値はプロセス寿命で不変。 */
const IfRasterPick *if_raster_autodetect(void);

/* 候補 kernel を ID 指定で直接実行（テスト/ベンチ相互検定用） */
int         if_raster_n_kernels(void);
const char *if_raster_kernel_name(int kid);
void        if_fill32_kernel(int kid, u32 *dst, u64 n, u32 v);

/* 選択 kernel への dispatch（描画ホットパスはこちらを呼ぶ） */
void if_fill32(u32 *dst, u64 n, u32 v);

#endif
