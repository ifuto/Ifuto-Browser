#include "tests.h"
#include "../src/utf8.h"
#include <string.h>

static u32 dec1(const char *s, u32 *pos) {
    return if_utf8_decode((const u8 *)s, (u32)strlen(s), pos);
}

void test_utf8(void) {
    u32 pos;

    /* ASCII */
    pos = 0;
    CHECK(dec1("ABC", &pos) == 'A' && pos == 1);

    /* 2バイト: あ…ではなく確認用に é (U+00E9 = C3 A9) */
    pos = 0;
    CHECK(dec1("\xC3\xA9!", &pos) == 0xE9 && pos == 2);

    /* 3バイト: U+3042 (E3 81 82) */
    pos = 0;
    CHECK(dec1("\xE3\x81\x82", &pos) == 0x3042 && pos == 3);

    /* 4バイト: U+1F600 (F0 9F 98 80) */
    pos = 0;
    CHECK(dec1("\xF0\x9F\x98\x80", &pos) == 0x1F600 && pos == 4);

    /* 不正: 孤立継続バイト → FFFD, 1バイト前進 */
    {
        const u8 b[] = {0x80, 0x80, 0x41};
        pos = 0;
        CHECK(if_utf8_decode(b, 3, &pos) == IF_CP_REPLACEMENT && pos == 1);
        CHECK(if_utf8_decode(b, 3, &pos) == IF_CP_REPLACEMENT && pos == 2);
        CHECK(if_utf8_decode(b, 3, &pos) == 0x41 && pos == 3);
    }

    /* overlong 排除: C0 81 は不正 */
    pos = 0;
    CHECK(dec1("\xC0\x81", &pos) == IF_CP_REPLACEMENT && pos == 1);

    /* 2バイト頭 + 不正継続 → FFFD、不正バイトから再解釈 */
    {
        const u8 b[] = {0xC3, 0x28};
        pos = 0;
        CHECK(if_utf8_decode(b, 2, &pos) == IF_CP_REPLACEMENT && pos == 1);
        CHECK(if_utf8_decode(b, 2, &pos) == 0x28 && pos == 2);
    }

    /* 末尾切断: 3バイト列の 2 バイト目で終端 → 残り消費 */
    {
        const u8 b[] = {0xE3, 0x81};
        pos = 0;
        CHECK(if_utf8_decode(b, 2, &pos) == IF_CP_REPLACEMENT && pos == 2);
    }

    /* サロゲート ED A0 80 → 不正 */
    pos = 0;
    CHECK(dec1("\xED\xA0\x80", &pos) == IF_CP_REPLACEMENT);

    /* U+10FFFF 上限: F4 8F BF BF は合法、F5 以降は不正 */
    {
        const u8 max_ok[] = {0xF4, 0x8F, 0xBF, 0xBF};
        pos = 0;
        CHECK(if_utf8_decode(max_ok, 4, &pos) == 0x10FFFF && pos == 4);
    }
    pos = 0;
    CHECK(dec1("\xF5\x80\x80\x80", &pos) == IF_CP_REPLACEMENT && pos == 1);

    /* encoder round-trip */
    {
        u8 buf[4];
        u32 cps[] = {0x41, 0xE9, 0x3042, 0x1F600, 0xFFFD};
        for (int i = 0; i < 5; i++) {
            u32 len = if_utf8_encode(cps[i], buf);
            u32 p = 0;
            CHECK(if_utf8_decode(buf, len, &p) == cps[i]);
            CHECK(p == len);
        }
    }

    /* セル幅 */
    CHECK(if_glyph_width('A') == 1);
    CHECK(if_glyph_width(0x3042) == 2);   /* あ */
    CHECK(if_glyph_width(0xFF21) == 2);   /* Ａ 全角 */
    CHECK(if_glyph_width(0x0301) == 0);   /* 結合アクセント */
    CHECK(if_glyph_width(0x07) == 0);     /* BEL */
}
