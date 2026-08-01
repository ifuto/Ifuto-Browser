/* Ifuto — セルグリッドレンダラ（ソフトウェアラスタ第 1 弾）
 *
 * 設計: GPU バックエンド（Vulkan/D3D12/Metal）に差し替えるときの境界として、
 *   「ボックスツリー → セルグリッド」の単方向データフローを固定する。
 *   将来の GPU 版はこのグリッドの代わりに頂点バッファを吐く backend を実装すればよく、
 *   レイアウトは触らない（ただし px 座標系レイアウトに差し替える）。
 */
#ifndef IFUTO_RENDER_H
#define IFUTO_RENDER_H

#include "common.h"
#include "arena.h"
#include "layout.h"
#include <stdio.h>

#define IF_CELL_DEFAULT 255u /* fg/bg の「端末既定」値 */
enum { IF_F_BOLD = 1, IF_F_ITALIC = 2, IF_F_ULINE = 4, IF_F_STRIKE = 8 };

typedef struct {
    u32 cp;   /* コードポイント（0 = 全角 2 セル目の継続セル） */
    u8 fg, bg;
    u8 flags;
} IfCell;

typedef struct {
    i32 w, h;
    i32 y_off;          /* 窓グリッドの文書行オフセット（フルグリッドは 0） */
    IfCell *cells;      /* cells[(y - y_off)*w + x]（y は文書行） */
} IfGrid;

IfGrid *if_render_grid(IfArena *arena, const IfLayout *lay);

/* viewport 相対グリッド（v0.2 メモリ則の中核）: row0..row1 の範囲だけを埋める。
 * cells は呼出側が (row1-row0) * lay->width 分確保する（本関数はアロケートしない）。
 * AI チャット型の長文書で全面グリッドを保持しないための規約:
 * 「表示しないメモリは持たない」（grid bytes は viewport に比例、文書長に非比例）。 */
void if_render_grid_rows_into(const IfLayout *lay, i32 row0, i32 row1, IfGrid *out);

/* 発行: ansi=1 で 256 色 SGR つき、0 でプレーンテキスト。戻り値は arena 文字列。 */
IfStr if_render_emit(IfArena *arena, const IfGrid *grid, int ansi);

/* ---- 巨大文書の定数メモリ発行（2026-07-31 巨大 IDM 計測からの構造修正） ----
 * 文書行列 extent のみ歩く（グリッド確保なし）。 */
void if_render_extent(const IfLayout *lay, i32 *mx, i32 *my);
/* grid を out へ行単位で逐次書き出す（出力全体の巨大バッファを作らない）。
 * 発行バイト列は if_render_emit と完全一致（ペン規約は行頭 DEFAULT リセット同一） */
void if_render_emit_rows(FILE *out, const IfGrid *grid, int ansi);

u8 if_rgba_to_ansi(u32 rgba); /* RGBA8 → ANSI 256 色 or IF_CELL_DEFAULT */

#endif
