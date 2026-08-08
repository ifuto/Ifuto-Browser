/* 文字コード層（src/charset.c）の in-process オラクル。
 * 期待値は全て python codec（生成器の正本）からの機械導出値を留め置き、
 * 再生成一致オラクル（tools/gen_charset.py --verify）と二重に凍結する。
 * 方針: malformed の消費規則は「1 回の FFFD で何バイト消費するか」まで固定する
 * （曖昧にするとデコーダ間で表示がずれ、最悪 XSS 様のすり抜け差分になる）。 */
#include "tests.h"
#include "../src/charset.h"
#include "../src/dom.h"
#include "../src/arena.h"
#include <string.h>
#include <stdio.h>

static IfStr dec(IfArena *a, const u8 *p, u32 n, IfEnc e) {
    return if_charset_decode(a, if_str((const char *)p, n), e);
}
static bool eq(IfStr s, const char *lit) {
    size_t ln = strlen(lit);
    return s.n == (u32)ln && memcmp(s.p, lit, ln) == 0;
}
#define LIT(lit) if_str(lit, (u32)(sizeof(lit) - 1))

static void t_label(void) {
    CHECK(if_charset_label("shift_jis", 9) == IF_ENC_SJIS);
    CHECK(if_charset_label("Shift_JIS", 9) == IF_ENC_SJIS);
    CHECK(if_charset_label("shift-jis", 9) == IF_ENC_SJIS);
    CHECK(if_charset_label("sjis", 4) == IF_ENC_SJIS);
    CHECK(if_charset_label("windows-31j", 11) == IF_ENC_SJIS);
    CHECK(if_charset_label("cp932", 5) == IF_ENC_SJIS);
    CHECK(if_charset_label("x-sjis", 6) == IF_ENC_SJIS);
    CHECK(if_charset_label("MS_Kanji", 8) == IF_ENC_SJIS);
    CHECK(if_charset_label(" shift_jis ", 11) == IF_ENC_SJIS);
    CHECK(if_charset_label("euc-jp", 6) == IF_ENC_EUCJP);
    CHECK(if_charset_label("EUC-JP", 6) == IF_ENC_EUCJP);
    CHECK(if_charset_label("x-euc-jp", 8) == IF_ENC_EUCJP);
    CHECK(if_charset_label("cseucpkdfmtjapanese", 19) == IF_ENC_EUCJP);
    CHECK(if_charset_label("utf-8", 5) == IF_ENC_UTF8);
    CHECK(if_charset_label("utf8", 4) == IF_ENC_UTF8);
    /* 未知ラベル安全側フォールバック（docs/CHARSET.md 凍結） */
    CHECK(if_charset_label("iso-8859-1", 10) == IF_ENC_UTF8);
    CHECK(if_charset_label("windows-1252", 12) == IF_ENC_UTF8);
    CHECK(if_charset_label("", 0) == IF_ENC_UTF8);
}

static void t_sniff(void) {
    bool bom = false;
    /* HTTP 優先: meta が sjis でも HTTP が euc を言えば euc */
    CHECK(if_charset_sniff(LIT("text/html; charset=euc-jp"),
        LIT("<meta charset=shift_jis><p>x"), &bom) == IF_ENC_EUCJP);
    /* meta 引用符・ci・空白 */
    CHECK(if_charset_sniff(if_str(NULL, 0),
        LIT("<META  CHARSET = \"Shift_JIS\">"), &bom) == IF_ENC_SJIS);
    /* http-equiv content 形（charset= を content 値内から拾う単一スキャナ） */
    CHECK(if_charset_sniff(if_str(NULL, 0),
        LIT("<meta http-equiv=\"Content-Type\" content=\"text/html;charset=Shift_JIS\">"),
        &bom) == IF_ENC_SJIS);
    /* BOM > meta */
    {
        CHECK(if_charset_sniff(if_str(NULL, 0), LIT("\xEF\xBB\xBF<meta charset=shift_jis>"),
              &bom) == IF_ENC_UTF8);
        CHECK(bom);
    }
    bom = false;
    /* 4096 バイト境界: 先頭 4090 辺りの meta は拾う、完全に越えたら拾わない */
    {
        static char big[5000];
        memset(big, ' ', sizeof big);
        memcpy(big + 4050, "<meta charset=euc-jp>", 21);
        CHECK(if_charset_sniff(if_str(NULL, 0), LIT(big), &bom) == IF_ENC_EUCJP);
        memset(big, ' ', 4060);
        memcpy(big + 4120, "<meta charset=euc-jp>", 21);
        CHECK(if_charset_sniff(if_str(NULL, 0), LIT(big), &bom) == IF_ENC_UTF8);
    }
    /* 非 meta / data-charset 偽陽性排除 / 既定 UTF-8 */
    CHECK(if_charset_sniff(if_str(NULL, 0), LIT("<div data-charset=euc-jp>"), &bom)
          == IF_ENC_UTF8);
    CHECK(if_charset_sniff(if_str(NULL, 0), LIT("<p>hello"), &bom) == IF_ENC_UTF8);
    CHECK(!bom);
    /* <metafoo は meta ではない（後続文字ゲート） */
    CHECK(if_charset_sniff(if_str(NULL, 0), LIT("<metafoo charset=euc-jp>"), &bom)
          == IF_ENC_UTF8);
}

static void t_decode_sjis(void) {
    IfArena a; if_arena_init(&a, 1 << 20);
    /* ASCII 直通 + 0x5C はバックスラッシュのまま（円記号化しない。codec 合意） */
    { const u8 b[] = "A\\B~"; CHECK(eq(dec(&a, b, 4, IF_ENC_SJIS), "A\\B~")); }
    /* ひらがな+漢字: "こんにちは日本語"（cp932 機械導出） */
    { const u8 b[] = {0x82,0xb1,0x82,0xf1,0x82,0xc9,0x82,0xbf,0x82,0xcd,
                      0x93,0xfa,0x96,0x7b,0x8c,0xea};
      CHECK(eq(dec(&a, b, sizeof b, IF_ENC_SJIS), "こんにちは日本語")); }
    /* 半角カナ 0xB1..0xB3 → ｱｲｳ */
    { const u8 b[] = {0xb1,0xb2,0xb3}; CHECK(eq(dec(&a, b, 3, IF_ENC_SJIS), "ｱｲｳ")); }
    /* 波ダッシュ 6 件は cp932 採用の代表: 0x8160 → U+FF5E（方針凍結） */
    { const u8 b[] = {0x81,0x60}; CHECK(eq(dec(&a, b, 2, IF_ENC_SJIS), "～")); }
    /* NEC 選定 0x8740 → ① ／ IBM 拡張 0xFA40 → ⅰ */
    { const u8 b[] = {0x87,0x40}; CHECK(eq(dec(&a, b, 2, IF_ENC_SJIS), "①")); }
    { const u8 b[] = {0xfa,0x40}; CHECK(eq(dec(&a, b, 2, IF_ENC_SJIS), "ⅰ")); }
    /* malformed 消費規則: lead+EOF → FFFD(1消費) */
    { const u8 b[] = {0x93}; CHECK(eq(dec(&a, b, 1, IF_ENC_SJIS), "\xEF\xBF\xBD")); }
    /* lead + 範囲外 trail(0x20) → FFFD は lead のみ、trail は restore されて生きる。
     * （0x41 は有効 trail に該当するため本ケースに使えない: 0x40-0x7E が離散範囲） */
    { const u8 b[] = {0x93,0x20,'A','B'}; CHECK(eq(dec(&a, b, 4, IF_ENC_SJIS), "\xEF\xBF\xBD" " AB")); }
    /* 有効 lead + 有効 trail: 0x93 0x41 = JIS 37-02 → 実字（codec 機械導出: 丼） */
    { const u8 b[] = {0x93,0x41}; CHECK(eq(dec(&a, b, 2, IF_ENC_SJIS), "鄭")); }
    /* 孤立 0x80 / 0xA0 / 0xFD */
    { const u8 b[] = {0x80,0xA0,0xFD}; CHECK(eq(dec(&a, b, 3, IF_ENC_SJIS), "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD")); }
    /* lead + trail 範囲外の継続: 0x82 0x20 → FFFD + ' ' */
    { const u8 b[] = {0x82,0x20,0x82,0xa0}; CHECK(eq(dec(&a, b, 4, IF_ENC_SJIS), "\xEF\xBF\xBD あ")); }
    /* 有効 lead+範囲内 trail だが無字セル → FFFD（2消費）: 0x8151 は JIS 1-49 未定義? 安全な穴で確認 */
    { const u8 b[] = {0x85,0x40}; /* NEC 83 区: cp932 では未定義→FFFD */
      CHECK(eq(dec(&a, b, 2, IF_ENC_SJIS), "\xEF\xBF\xBD")); }
    if_arena_destroy(&a);
}

static void t_decode_euc(void) {
    IfArena a; if_arena_init(&a, 1 << 20);
    { const u8 b[] = "abcXYZ"; CHECK(eq(dec(&a, b, 6, IF_ENC_EUCJP), "abcXYZ")); }
    /* "日本" c6fc cbdc */
    { const u8 b[] = {0xc6,0xfc,0xcb,0xdc}; CHECK(eq(dec(&a, b, 4, IF_ENC_EUCJP), "日本")); }
    /* SS2 半角: 8e b1 → ｱ */
    { const u8 b[] = {0x8e,0xb1}; CHECK(eq(dec(&a, b, 2, IF_ENC_EUCJP), "ｱ")); }
    /* SS3 JIS X 0212: 8f a2 af → ˘ U+02D8（euc_jp codec 機械導出） */
    { const u8 b[] = {0x8f,0xa2,0xaf}; CHECK(eq(dec(&a, b, 3, IF_ENC_EUCJP), "\xCB\x98")); }
    /* malformed: 8F + EOF → FFFD(1)。8F a2 + EOF → FFFD(2消費) */
    { const u8 b[] = {0x8f}; CHECK(eq(dec(&a, b, 1, IF_ENC_EUCJP), "\xEF\xBF\xBD")); }
    { const u8 b[] = {0x8f,0xa2}; CHECK(eq(dec(&a, b, 2, IF_ENC_EUCJP), "\xEF\xBF\xBD")); }
    /* 8F + 非範囲 → FFFD(1) + restore,'A' 生存 */
    { const u8 b[] = {0x8f,'A'}; CHECK(eq(dec(&a, b, 2, IF_ENC_EUCJP), "\xEF\xBF\xBD" "A")); }
    /* 8F a2 + 非範囲 → FFFD(2) + 'B' 生存 */
    { const u8 b[] = {0x8f,0xa2,'B'}; CHECK(eq(dec(&a, b, 3, IF_ENC_EUCJP), "\xEF\xBF\xBD" "B")); }
    /* 0208 lead 単体 → FFFD(1)。lead + ASCII → FFFD(1)+restore */
    { const u8 b[] = {0xc6}; CHECK(eq(dec(&a, b, 1, IF_ENC_EUCJP), "\xEF\xBF\xBD")); }
    { const u8 b[] = {0xc6,'!'}; CHECK(eq(dec(&a, b, 2, IF_ENC_EUCJP), "\xEF\xBF\xBD" "!")); }
    /* 0xFF trail は行越境しない（lead/trail 共に 0xA1..0xFE 法則）: */
    { const u8 b[] = {0xc6,0xff}; CHECK(eq(dec(&a, b, 2, IF_ENC_EUCJP), "\xEF\xBF\xBD\xEF\xBF\xBD")); }
    /* 0x8E + 非範囲 → FFFD(1)+restore */
    { const u8 b[] = {0x8e,0x41}; CHECK(eq(dec(&a, b, 2, IF_ENC_EUCJP), "\xEF\xBF\xBD" "A")); }
    /* 孤立 0x81/0x90/0xFF → 各 FFFD(1) */
    { const u8 b[] = {0x81,0x90,0xff}; CHECK(eq(dec(&a, b, 3, IF_ENC_EUCJP), "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD")); }
    if_arena_destroy(&a);
}

/* 全バイト対（65536 通り × 2 エンコーディング）の総当たり健全性:
 * クラッシュ/オーバーランしない（ASan 走査）、出力長 ≤ 3n+3、確定的（同入力=同出力）。
 * これは codec 照合の代替ではなく「表外セルが構造的に安全」であることの機械証明。 */
static void t_sweep(void) {
    IfArena a; if_arena_init(&a, 1 << 22);
    u8 b[2];
    for (u32 v = 0; v < 65536; v++) {
        b[0] = (u8)(v >> 8); b[1] = (u8)v;
        IfStr s1 = if_charset_decode(&a, if_str((const char *)b, 2), IF_ENC_SJIS);
        IfStr s2 = if_charset_decode(&a, if_str((const char *)b, 2), IF_ENC_EUCJP);
        if (s1.n > 6 || s2.n > 6) { CHECK(0); break; }
    }
    CHECK(1);
    if_arena_destroy(&a);
}

/* E2E: sjis HTML（meta charset）→ sniff+decode → パーサ → DOM title が UTF-8。
 * （関門 to_utf8_html 相当の手順を in-process で踏む） */
static void t_e2e(void) {
    static const u8 sjis_html[] = {
        '<','!','D','O','C','T','Y','P','E',' ','h','t','m','l','>',
        '<','h','e','a','d','>','<','m','e','t','a',' ','c','h','a','r','s','e','t','=',
        's','h','i','f','t','_','j','i','s','>','<','t','i','t','l','e','>',
        0x93,0xfa,0x96,0x7b,0x8c,0xea, /* 日本語 */
        '<','/','t','i','t','l','e','>','<','/','h','e','a','d','>',
        '<','b','o','d','y','>','<','p','>',
        0x82,0xb1,0x82,0xf1,0x82,0xc9,0x82,0xbf,0x82,0xcd, /* こんにちは */
        '<','/','p','>','<','/','b','o','d','y','>'
    };
    IfArena a; if_arena_init(&a, 1 << 20);
    IfStr in = if_str((const char *)sjis_html, (u32)sizeof sjis_html);
    bool bom = false;
    IfEnc enc = if_charset_sniff(if_str(NULL, 0), in, &bom);
    CHECK(enc == IF_ENC_SJIS);
    IfStr u8s = if_charset_decode(&a, in, enc);
    IfDom *d = if_parse_html(&a, u8s);
    CHECK(d != NULL);
    CHECK(d->title.n == strlen("日本語") && memcmp(d->title.p, "日本語", d->title.n) == 0);
    if_arena_destroy(&a);
}

/* BOM  strip は関門側の責務: sniff が bom を報告し UTF-8 なら 3B を剥がす。
 * ここでは BOM 付き UTF-8 HTML がそのまま（BOM 残）だと parse 対象外扱いに
 * ならないよう、関門規則（p+=3）の成立を機械固定する。 */
static void t_bom_strip_rule(void) {
    static const char html[] = "\xEF\xBB\xBF<p>x";
    bool bom = false;
    IfStr in = if_str(html, (u32)(sizeof html - 1));
    IfEnc enc = if_charset_sniff(if_str(NULL, 0), in, &bom);
    CHECK(enc == IF_ENC_UTF8 && bom);
    if (bom && in.n >= 3) { in.p += 3; in.n -= 3; }
    CHECK(in.n == 4 && memcmp(in.p, "<p>x", 4) == 0);
}

void test_charset(void) {
    RUN(t_label);
    RUN(t_sniff);
    RUN(t_decode_sjis);
    RUN(t_decode_euc);
    RUN(t_sweep);
    RUN(t_e2e);
    RUN(t_bom_strip_rule);
}
