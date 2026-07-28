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

/* 可変長配列用の小さなヘルパ: *ptr (*cap 要素, elem サイズ esz) を need まで倍増。
 * arena は realloc できないので、コピー移動し旧領域は捨てる（ページ寿命内なら損失は軽微）。 */
void *if_arena_grow(IfArena *a, void *ptr, u64 *cap, u64 need, u64 esz);

#endif
