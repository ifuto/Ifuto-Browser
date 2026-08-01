/* Ifuto — デコ op（装飾イベント）。layout が DFS 順（= paint 順）に追記し、
 * render_ansi.c の行スイーパが消費する。lst は layout/render 共通の内部構造。 */
#ifndef IFUTO_DECO_H
#define IFUTO_DECO_H

#include "common.h"

enum {
    IF_DECO_BG,       /* box 背景塗り（cp/fg/flags は保持して bg のみ） */
    IF_DECO_BORDER,   /* box 4 辺 + 角の罫線（cp/fg のみ上書き、bg/flags 保持） */
    IF_DECO_HLINE,    /* HR 相当: NULL style の ─ ラン（フルペン既定で上書き） */
    IF_DECO_MARKER    /* li マーカー: テキスト + style のフルペン上書き */
};

typedef struct IfDeco {
    u8 kind;
    u8 tlen;           /* MARKER: text の有効長 */
    char text[12];     /* MARKER: "• " / "12." (u32 最大 10 桁 + '.') */
    i32 x, y, w, h;
    u32 argb;          /* BG: bg RGBA / BORDER: border_color */
    const struct IfStyle *st; /* MARKER のペン */
} IfDeco;

#endif
