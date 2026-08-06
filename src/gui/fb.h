/* Ifuto GUI — ピクセルフレームバッファ（ストリップ保持、全面バックバッファなし）
 *
 * メモリ則: 画面全体の画素を保持しない。描画は 8 セル行（=128px 行）の
 * ストリップ（幅×128×4bytes。1000px 幅で 512KB）を使い回し、
 * 転送（XPutImage）または PPM ダンプに流し切る。
 * セル幾何: 8x16 px（layout.c のセルと 1:1 IF_CHAR_W_PX/IF_ROW_H_PX）。 */
#ifndef IFUTO_GUI_FB_H
#define IFUTO_GUI_FB_H

#include "../common.h"

#define GUI_CELL_W 8
#define GUI_CELL_H 16
#define GUI_STRIP_CELLS 8 /* 1 ストリップ = 8 セル行 = 128 px 行 */

typedef struct {
    u32 *px;        /* GUI_STRIP_CELLS 行分（cap_px 要素） */
    u32 w_px, h_px; /* ストリップ物理サイズ（h_px = GUI_CELL_H * GUI_STRIP_CELLS） */
    u64 cap_px;
} IfFbStrip;

void fb_init(IfFbStrip *s, u32 window_w_px);

/* 矩形塗り（clip 済。色は 0xRRGGBB） */
void fb_rect(IfFbStrip *s, i32 x, i32 y, i32 w, i32 h, u32 rgb);

/* 1 グリフ描画。fg/bg は 0xRRGGBB。bold は +1px 重ね、underline は最下行 */
void fb_glyph(IfFbStrip *s, i32 x_px, i32 y_px, u8 ch, u32 fg, u32 bg,
              bool bold, bool underline);

/* 全角 16x16 グリフ描画（2 セルぶん。rows16 は F16_ROWS エントリ、bit15=最左列）。
 * bg は 2 セル幅の矩形で塗る（cp=0 の全角継続セルもここで覆われる規約） */
void fb_glyph16(IfFbStrip *s, i32 x_px, i32 y_px, const void *rows16, u32 fg, u32 bg,
                bool bold, bool underline);

/* コードポイント → グリフ選択の一点化（ASCII/box-drawing/全角互換形/font16/明示豆腐）。
 * 戻り値は進んだセル幅（1 or 2） */
i32 fb_glyph_cp(IfFbStrip *s, i32 x_px, i32 y_px, u32 cp, u32 fg, u32 bg,
                bool bold, bool underline);

/* 文字列（セル列数ならセル制約でクリップされるのではなく呼出側責任。UTF-8 は
 * 非 ASCII バイトを tofu に落とす規約） */
void fb_text(IfFbStrip *s, i32 x_px, i32 y_px, const u8 *str, u32 n, u32 fg, u32 bg,
             bool bold, bool underline);

#endif
