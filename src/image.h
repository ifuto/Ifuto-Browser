/* image.h — Ifuto 軽量画像デコード（BMP / PNG）
 *
 * 設計方針:
 *  - 戻り値はピクセル配列（RGBA8888）。デコード失敗は NULL で明白に失敗
 *    （「嘘をつかない」: 部分デコードを返さない）。
 *  - PNG: 8bit のみ（16bit は拒否）。カラータイプ 0(グレー) 2(RGB) 4(グレー+α) 6(RGBA)。
 *    パレット(3)・インターレース(Adam7) は拒否。
 *    zlib インフレート（ストアド/固定/動的ハフマン）を内蔵。
 *  - BMP: 無圧縮 24/32bpp（BITMAPINFOHEADER のみ）。RLE・16bpp・パレットは拒否。
 *  - メモリ上限: 1 画像 64MB まで（ホストを殺さない）。
 */
#ifndef IFUTO_IMAGE_H
#define IFUTO_IMAGE_H

#include "common.h"

typedef struct {
    u32 w, h;
    u8 *px; /* w*h*4。RGBA8888（リトルエンディアンで 0xAABBGGRR の u32 としても読める） */
} IfImage;

/* PNG / BMP をデコード。len はデータ長。失敗は NULL（err_buf に理由。err_cap>0 時）。 */
IfImage *if_img_decode(const u8 *data, u32 len, char *err_buf, u32 err_cap);
void if_img_free(IfImage *img);

#endif
