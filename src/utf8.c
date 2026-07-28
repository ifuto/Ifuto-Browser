#include "utf8.h"

u32 if_utf8_decode(const u8 *s, u32 n, u32 *pos) {
    u32 i = *pos;
    if (i >= n) return 0;
    u8 b0 = s[i];

    if (b0 < 0x80) { *pos = i + 1; return b0; }

    u32 need, cp;
    u8 lo = 0x80, hi = 0xBF;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        need = 1; cp = b0 & 0x1Fu;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        need = 2; cp = b0 & 0x0Fu;
        if (b0 == 0xE0) lo = 0xA0;        /* overlong 排除 */
        else if (b0 == 0xED) hi = 0x9F;   /* サロゲート排除 */
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        need = 3; cp = b0 & 0x07u;
        if (b0 == 0xF0) lo = 0x90;        /* overlong 排除 */
        else if (b0 == 0xF4) hi = 0x8F;   /* U+10FFFF 上限 */
    } else {
        *pos = i + 1;                      /* 孤立継続バイト / C0,C1,F5..FF */
        return IF_CP_REPLACEMENT;
    }

    for (u32 j = 1; j <= need; j++) {
        if (i + j >= n) { *pos = n; return IF_CP_REPLACEMENT; } /* 末尾切断: 残りを全部消費 */
        u8 bj = s[i + j];
        u8 l = (j == 1) ? lo : 0x80, h = (j == 1) ? hi : 0xBF;
        if (bj < l || bj > h) { *pos = i + j; return IF_CP_REPLACEMENT; } /* 不正バイトは次回再解釈 */
        cp = (cp << 6) | (u32)(bj & 0x3F);
    }
    *pos = i + need + 1;
    return cp;
}

u32 if_utf8_encode(u32 cp, u8 out[4]) {
    if (cp < 0x80) {
        out[0] = (u8)cp; return 1;
    } else if (cp < 0x800) {
        out[0] = (u8)(0xC0 | (cp >> 6));
        out[1] = (u8)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = IF_CP_REPLACEMENT; /* 念のため */
        out[0] = (u8)(0xE0 | (cp >> 12));
        out[1] = (u8)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (u8)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        if (cp > 0x10FFFF) cp = IF_CP_REPLACEMENT;
        out[0] = (u8)(0xF0 | (cp >> 18));
        out[1] = (u8)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (u8)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (u8)(0x80 | (cp & 0x3F));
        return 4;
    }
}

int if_glyph_width(u32 cp) {
    if (cp < 0x20) return 0;
    if (cp >= 0x7F && cp <= 0x9F) return 0;
    /* 結合文字（表示幅 0） */
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
        (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0xFE20 && cp <= 0xFE2F)) return 0;
    /* East Asian Wide/Fullwidth の主要レンジ（省略版・表示不能時も安全側に倒れるだけ） */
    if ((cp >= 0x1100 && cp <= 0x115F) ||                           /* Hangul Jamo */
        (cp >= 0x2E80 && cp <= 0x303E) ||                           /* CJK Radicals..CJK Symbols */
        (cp >= 0x3041 && cp <= 0x33FF) ||                           /* ひらがな・カタカナ・CJK 記号 */
        (cp >= 0x3400 && cp <= 0x4DBF) ||                           /* CJK Ext A */
        (cp >= 0x4E00 && cp <= 0x9FFF) ||                           /* CJK 統合漢字 */
        (cp >= 0xA000 && cp <= 0xA4CF) ||                           /* Yi */
        (cp >= 0xAC00 && cp <= 0xD7A3) ||                           /* Hangul Syllables */
        (cp >= 0xF900 && cp <= 0xFAFF) ||                           /* CJK 互換漢字 */
        (cp >= 0xFE30 && cp <= 0xFE6F) ||                           /* CJK 互換形・小字型 */
        (cp >= 0xFF00 && cp <= 0xFF60) ||                           /* 全角 ASCII・半角カナ境界 */
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||                           /* 全角記号 */
        (cp >= 0x1F300 && cp <= 0x1F64F) ||                         /* Emoji 主要 */
        (cp >= 0x20000 && cp <= 0x3FFFD)) {                         /* CJK Ext B+ */
        return 2;
    }
    return 1;
}
