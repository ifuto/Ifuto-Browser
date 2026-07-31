#include "fb.h"
#include "font5x7.h"
#include <string.h>
#include <stdlib.h>

void fb_init(IfFbStrip *s, u32 window_w_px) {
    s->w_px = window_w_px ? window_w_px : 8;
    s->h_px = GUI_CELL_H * GUI_STRIP_CELLS;
    s->cap_px = (u64)s->w_px * s->h_px;
    s->px = (u32 *)malloc(s->cap_px * sizeof(u32));
    if (!s->px) if_fatal("oom: fb strip");
}

/* X11 レガシ深度 24（ZPixmap, BGRXレイアウト相当）は fb→線形転送側で並べる。
 * ここでの画素値は 0x00RRGGBB と固定し変換責任を転送側に限定する。 */

void fb_rect(IfFbStrip *s, i32 x, i32 y, i32 w, i32 h, u32 rgb) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (i32)s->w_px) w = (i32)s->w_px - x;
    if (y + h > (i32)s->h_px) h = (i32)s->h_px - y;
    if (w <= 0 || h <= 0) return;
    for (i32 yy = y; yy < y + h; yy++) {
        u32 *row = s->px + (u64)yy * s->w_px + (u32)x;
        for (i32 xx = 0; xx < w; xx++) row[xx] = rgb;
    }
}

void fb_glyph(IfFbStrip *s, i32 x_px, i32 y_px, u8 ch, u32 fg, u32 bg,
              bool bold, bool underline) {
    fb_rect(s, x_px, y_px, GUI_CELL_W, GUI_CELL_H, bg);
    const uint8_t *g = f5x7_glyph(ch);
    for (i32 r = 0; r < F5X7_ROWS; r++) {
        i32 yy = y_px + 4 + r; /* 縦オフセット 4（7 行 = 中央寄せ） */
        if (yy < 0 || yy >= (i32)s->h_px) continue;
        for (i32 c = 0; c < F5X7_COLS; c++) {
            /* 表記規約: 各バイトの MSB(bit4) が左列（font5x7 表の設計規約） */
            if (!(g[r] & (0x10u >> c))) continue;
            i32 xx = x_px + 2 + c; /* 横オフセット 2 */
            if (xx >= 0 && xx < (i32)s->w_px) s->px[(u64)yy * s->w_px + (u32)xx] = fg;
            if (bold) {
                i32 x2 = xx + 1;
                if (x2 >= 0 && x2 < (i32)s->w_px && x2 - xx <= 1)
                    s->px[(u64)yy * s->w_px + (u32)x2] = fg;
            }
        }
    }
    if (underline) {
        i32 uy = y_px + 13;
        if (uy >= 0 && uy < (i32)s->h_px)
            for (i32 c = 0; c < GUI_CELL_W; c++) {
                i32 xx = x_px + c;
                if (xx >= 0 && xx < (i32)s->w_px) s->px[(u64)uy * s->w_px + (u32)xx] = fg;
            }
    }
}

void fb_text(IfFbStrip *s, i32 x_px, i32 y_px, const u8 *str, u32 n, u32 fg, u32 bg,
             bool bold, bool underline) {
    for (u32 i = 0; i < n; i++) {
        u8 ch = str[i];
        if (ch >= 0x80) ch = '?'; /* 非 ASCII → 豆腐欄（'?')。表記安全性の規約 */
        fb_glyph(s, x_px + (i32)i * GUI_CELL_W, y_px, ch, fg, bg, bold, underline);
    }
}
