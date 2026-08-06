#include "fb.h"
#include "font5x7.h"
#include "font16.h"
#include "../raster.h"
#include "../utf8.h"
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
    /* fill kernel は起動時 microbench 選定（raster.c）。bit-exact 同一性は
     * tests/test_raster.c が全候補について scalar 基準と相互証明済み */
    for (i32 yy = y; yy < y + h; yy++)
        if_fill32(s->px + (u64)yy * s->w_px + (u32)x, (u64)w, rgb);
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

void fb_glyph16(IfFbStrip *s, i32 x_px, i32 y_px, const void *rows16_, u32 fg, u32 bg,
                bool bold, bool underline) {
    const uint16_t *rows16 = (const uint16_t *)rows16_;
    fb_rect(s, x_px, y_px, 2 * GUI_CELL_W, GUI_CELL_H, bg);
    for (i32 r = 0; r < 16; r++) {
        i32 yy = y_px + r;
        if (yy < 0 || yy >= (i32)s->h_px) continue;
        u32 row = rows16[r];
        for (i32 c = 0; c < 16; c++) {
            if (!(row & (0x8000u >> c))) continue;
            i32 xx = x_px + c;
            if (xx >= 0 && xx < (i32)s->w_px) s->px[(u64)yy * s->w_px + (u32)xx] = fg;
            if (bold) {
                i32 x2 = xx + 1;
                if (x2 >= 0 && x2 < (i32)s->w_px)
                    s->px[(u64)yy * s->w_px + (u32)x2] = fg;
            }
        }
    }
    if (underline) {
        i32 uy = y_px + 13;
        if (uy >= 0 && uy < (i32)s->h_px)
            for (i32 c = 0; c < 2 * GUI_CELL_W; c++) {
                i32 xx = x_px + c;
                if (xx >= 0 && xx < (i32)s->w_px) s->px[(u64)uy * s->w_px + (u32)xx] = fg;
            }
    }
}

/* グリフ選択の一点化: コードポイント → 描画。
 *   ASCII / box-drawing 外形等価 / 全角互換形（半角グリフ中央配置）/
 *   font16（かな・カナ・記号）/ F16_TOFU（未収録全角の明示印。'?' 潰れ禁止）。
 * cp==0x3000/空白は bg 矩形のみ。戻り値: 進んだセル幅（1 or 2）。 */
i32 fb_glyph_cp(IfFbStrip *s, i32 x_px, i32 y_px, u32 cp, u32 fg, u32 bg,
                bool bold, bool underline) {
    if (cp == ' ') { fb_rect(s, x_px, y_px, GUI_CELL_W, GUI_CELL_H, bg); return 1; }
    if (cp == 0x3000) { fb_rect(s, x_px, y_px, 2 * GUI_CELL_W, GUI_CELL_H, bg); return 2; }
    if (cp >= 0x20 && cp <= 0x7E) { fb_glyph(s, x_px, y_px, (u8)cp, fg, bg, bold, underline); return 1; }
    /* ASCII 外形付け替え: box-drawing 系は「構図の等価物」に落とす（豆腐回避） */
    if ((cp >= 0x2500 && cp <= 0x257F) || cp == 0x2014 || cp == 0x2013) {
        fb_glyph(s, x_px, y_px, '-', fg, bg, bold, underline); return 1;
    }
    if (cp == 0x2022) { /* 箇条書き小点（● は font16 に実字形があるため除外） */
        fb_glyph(s, x_px, y_px, '*', fg, bg, bold, underline); return 1;
    }
    /* 全角 ASCII 互換形（ＡＢＣ１２３…）は半角グリフを 2 セル中央に */
    if (cp >= 0xFF01 && cp <= 0xFF5E) {
        fb_rect(s, x_px, y_px, 2 * GUI_CELL_W, GUI_CELL_H, bg);
        fb_glyph(s, x_px + GUI_CELL_W / 2, y_px, (u8)(cp - 0xFEE0), fg, bg, bold, underline);
        return 2;
    }
    const void *g16 = f16_lookup(cp);
    if (!g16) g16 = F16_TOFU;
    fb_glyph16(s, x_px, y_px, g16, fg, bg, bold, underline);
    return 2;
}

void fb_text(IfFbStrip *s, i32 x_px, i32 y_px, const u8 *str, u32 n, u32 fg, u32 bg,
             bool bold, bool underline) {
    u32 pos = 0, col = 0;
    while (pos < n) {
        u32 cp = if_utf8_decode(str, n, &pos);
        if (!cp) break;
        col += (u32)fb_glyph_cp(s, x_px + (i32)col * GUI_CELL_W, y_px, cp, fg, bg,
                                bold, underline);
    }
}
