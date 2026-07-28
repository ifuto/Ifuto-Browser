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

#define IF_CELL_DEFAULT 255u /* fg/bg の「端末既定」値 */
enum { IF_F_BOLD = 1, IF_F_ITALIC = 2, IF_F_ULINE = 4, IF_F_STRIKE = 8 };

typedef struct {
    u32 cp;   /* コードポイント（0 = 全角 2 セル目の継続セル） */
    u8 fg, bg;
    u8 flags;
} IfCell;

typedef struct {
    i32 w, h;
    IfCell *cells; /* cells[y*w+x] */
} IfGrid;

IfGrid *if_render_grid(IfArena *arena, const IfLayout *lay);

/* 発行: ansi=1 で 256 色 SGR つき、0 でプレーンテキスト。戻り値は arena 文字列。 */
IfStr if_render_emit(IfArena *arena, const IfGrid *grid, int ansi);

u8 if_rgba_to_ansi(u32 rgba); /* RGBA8 → ANSI 256 色 or IF_CELL_DEFAULT */

#endif
