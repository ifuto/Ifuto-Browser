/* Ifuto — bump arena allocator.
 *
 * 不変条件:
 *   - arena から返ったポインタは if_arena_destroy/reset まで常に有効。
 *   - 個別 free は存在しない（dangling を構造的に不可能にする）。
 *   - OOM・オーバーフロー・異常サイズは if_fatal で即死（黙った破壊より fast-fail）。
 *
 * 計算量: alloc は償却 O(1)、free-all は O(blocks)。free-list も compaction も持たない。
 */
#ifndef IFUTO_ARENA_H
#define IFUTO_ARENA_H

#include "common.h"

typedef struct IfArenaBlock {
    struct IfArenaBlock *next;
    u64 cap;
    u64 used;
    u64 pad; /* ヘッダを 32B に揃え、後続データの 16B アラインを構造的に保証する */
    /* data follows (16B aligned: malloc は 16B 整列、header は 32B) */
} IfArenaBlock;

typedef struct {
    IfArenaBlock *head;
    u64 default_cap;
    u64 total_reserved; /* 計測のための正確な予約量 */
} IfArena;

void  if_arena_init(IfArena *a, u64 default_cap);
void *if_arena_alloc(IfArena *a, u64 size);            /* 未初期化, 16B aligned */
void *if_arena_calloc(IfArena *a, u64 size);           /* ゼロ初期化 */
void  if_arena_destroy(IfArena *a);
u64   if_arena_reserved(const IfArena *a);
/* 実際に使用中の合計バイト（ブロック used の総和。reserved と併せて
 * 「予約は多いが使っていない」の監視に使う。省メモリ台帳の計測点） */
u64   if_arena_used(const IfArena *a);
/* 指定アライン（2 のべき乗 ≤ 16）で確保。デフォルト 16B 保証を崩さず、
 * odd サイズ構造体（IfNode 69B 等）を 16B に切り上げないための 8B 経路
 * （2026-08-10 省メモリ: 16B アラインでは 69B→80B 消費。8B なら 72B） */
void *if_arena_alloc_a(IfArena *a, u64 size, u64 align);
void *if_arena_calloc_a(IfArena *a, u64 size, u64 align); /* ゼロ初期化 + align */
/* src の全ブロックを dst の解放チェーンへ寄贈（src は空に。dst の bump 現端は不変）。
 * 並列 parse のスレッド別 arena を join 時に主 arena へ畳み込み、
 * 「if_arena_destroy で一括解放」の寿命規約を維持する。 */
void  if_arena_absorb(IfArena *dst, IfArena *src);

/* 可変長配列用の小さなヘルパ: *ptr (*cap 要素, elem サイズ esz) を need まで倍増。
 * arena は realloc できないので、コピー移動し旧領域は捨てる（ページ寿命内なら損失は軽微）。 */
void *if_arena_grow(IfArena *a, void *ptr, u64 *cap, u64 need, u64 esz);

/* arena.c の IF_ARENA_ALIGN と同値（inline 経路との整合のため公開） */
#define IF_ARENA_ALIGN_I 16u
/* ---- inline bump 高速経路（2026-08-01 性能本丸） ----
 * if_arena_alloc と同一の不変条件（16B アライン・bump・fast-fail）を保つ。
 * 先頭ブロックに収まる通常経路のみ inline 化し、新規ブロック系は従来関数へ。 */
static inline void *if_arena_bump(IfArena *a, u64 size) {
    IfArenaBlock *b = a->head;
    if (__builtin_expect(b != NULL, 1)) {
        u64 off = (b->used + (IF_ARENA_ALIGN_I - 1)) & ~(u64)(IF_ARENA_ALIGN_I - 1);
        if (__builtin_expect(off <= b->cap && size <= b->cap - off, 1)) {
            b->used = off + size;
            return (u8 *)b + sizeof(IfArenaBlock) + off;
        }
    }
    return if_arena_alloc(a, size);
}

/* 「直前の bump/alloc が ptr で size だった」場合に限り取り消す（seg pop の巻き戻し）。
 * 条件が崩れていれば何もしない（arena は不可逆が原則、これは最新端のみの最適化）。 */
static inline void if_arena_rewind_last(IfArena *a, void *ptr, u64 size) {
    IfArenaBlock *b = a->head;
    if (!b || !ptr) return;
    u64 off = (b->used + (IF_ARENA_ALIGN_I - 1)) & ~(u64)(IF_ARENA_ALIGN_I - 1);
    (void)off;
    if ((u8 *)ptr >= (u8 *)b + sizeof(IfArenaBlock) &&
        (u8 *)ptr + size == (u8 *)b + sizeof(IfArenaBlock) + b->used)
        b->used = (u64)((u8 *)ptr - ((u8 *)b + sizeof(IfArenaBlock)));
}

#endif
