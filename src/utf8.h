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

/* 「妥当 3 バイト列 ∧ if_glyph_width==2」を lead/継続バイトだけで確定できる帯（高速述語）。
 * 真 ⟹ decode 成功 ∧ 幅 2（逆は言えない: 幅 2 の帯はこれより広い。縮小側は安全）。
 * 帯の構成（if_glyph_width の幅 2 レンジの byte 空間写像）:
 *   0x3000-0x33FF: E3 b1∈[0x80,0x8F] ただし U+303F(E3 80 BF, 幅 1) と U+3040(E3 81 80, 幅 1) は除外
 *   0x4E00-0x9FFF: E4 b1∈[0xB8,0xBF]（= [0x4E00,0x4FFF]）| E5..E9（= [0x5000,0x9FFF]）
 * E0/ED の過長・サロゲート除外はこれらの lead には非該当。 */
static inline __attribute__((always_inline)) bool if_utf8_band_w2(u8 b0, u8 b1, u8 b2) {
    if ((b2 & 0xC0) != 0x80) return false;
    /* 0x3000-0x303E（Ø303F=幅 ??? → 1 を除外）、0x3041-0x33FF（0x3040=幅 1 を除外）:
     * E3 b1∈[0x80,0x8F] から (b1==0x80 && b2==0xBF)=U+303F と (b1==0x81 && b2==0x80)=U+3040 を抜く */
    if (b0 == 0xE3) return b1 >= 0x80 && b1 <= 0x8F &&
                          !(b1 == 0x80 && b2 == 0xBF) && !(b1 == 0x81 && b2 == 0x80);
    if (b0 == 0xE4) return b1 >= 0xB8 && b1 <= 0xBF;
    return b0 >= 0xE5 && b0 <= 0xE9 && (b1 & 0xC0) == 0x80;
}

#endif
