/* Ifuto — UTF-8 デコーダ（WHATWG 流儀のエラー回復）
 *
 * 不変条件:
 *   - どんなバイト列でも停止せず、不正列は U+FFFD に置換して前進する。
 *   - サロゲート・overlong・U+10FFFF 超過はすべて不正。
 *   - pos は常に単調増加し、n を超えない（無限ループ不能）。
 */
#ifndef IFUTO_UTF8_H
#define IFUTO_UTF8_H

#include "common.h"

#define IF_CP_REPLACEMENT 0xFFFDu

/* s[pos] から 1 コードポイントをデコードし、pos を進める。pos >= n なら 0 を返す。 */
u32 if_utf8_decode(const u8 *s, u32 n, u32 *pos);

/* cp を UTF-8 にエンコード。戻り値はバイト数(1..4)。out は 4B 必要。 */
u32 if_utf8_encode(u32 cp, u8 out[4]);

/* 端末セル幅: 0=制御/結合, 1=通常, 2=全角 */
int if_glyph_width(u32 cp);

#endif
