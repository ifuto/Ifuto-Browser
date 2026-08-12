/* akl_regex 単体テストハーネス */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../src/akl/akl_regex.h"

static int fails = 0, total = 0;

static void t(const char *pat, uint32_t flags, const char *s, int expect_match,
              int expect_beg, int expect_end) {
    total++;
    char err[128];
    AklRex *rx = akl_rex_compile((const uint8_t *)pat, (uint32_t)strlen(pat), flags, err, sizeof err);
    if (!rx) {
        printf("FAIL compile %s: %s\n", pat, err);
        fails++;
        return;
    }
    uint32_t ncap = akl_rex_ncap(rx);
    uint32_t *cb = (uint32_t *)malloc((ncap + 1) * sizeof(uint32_t) * 2);
    uint32_t *ce = cb + ncap + 1;
    bool lim = false;
    bool m = akl_rex_match(rx, (const uint8_t *)s, (uint32_t)strlen(s), 0, cb, ce, ncap, &lim);
    if (m != (expect_match != 0)) {
        printf("FAIL match %s / %s => got %d want %d%s\n", pat, s, m, expect_match, lim ? " (limit)" : "");
        fails++;
    } else if (m) {
        if ((int)cb[0] != expect_beg || (int)ce[0] != expect_end) {
            printf("FAIL span %s / %s => [%u,%u) want [%d,%d)\n", pat, s, cb[0], ce[0], expect_beg, expect_end);
            fails++;
        }
    }
    free(cb);
    akl_rex_free(rx);
}

static void t_err(const char *pat) {
    total++;
    char err[128] = {0};
    AklRex *rx = akl_rex_compile((const uint8_t *)pat, (uint32_t)strlen(pat), 0, err, sizeof err);
    if (rx) {
        printf("FAIL compile should error: %s\n", pat);
        fails++;
        akl_rex_free(rx);
    }
}

static void t_caps(const char *pat, const char *s, int want_cap1_beg, int want_cap1_end) {
    total++;
    char err[128];
    AklRex *rx = akl_rex_compile((const uint8_t *)pat, (uint32_t)strlen(pat), 0, err, sizeof err);
    if (!rx) { printf("FAIL compile %s: %s\n", pat, err); fails++; return; }
    uint32_t ncap = akl_rex_ncap(rx);
    uint32_t *cb = (uint32_t *)malloc((ncap + 1) * sizeof(uint32_t) * 2);
    uint32_t *ce = cb + ncap + 1;
    bool lim = false;
    bool m = akl_rex_match(rx, (const uint8_t *)s, (uint32_t)strlen(s), 0, cb, ce, ncap, &lim);
    if (!m) { printf("FAIL caps match %s / %s\n", pat, s); fails++; }
    else if ((int)cb[1] != want_cap1_beg || (int)ce[1] != want_cap1_end) {
        printf("FAIL caps %s / %s => cap1=[%u,%u) want [%d,%d)\n", pat, s, cb[1], ce[1], want_cap1_beg, want_cap1_end);
        fails++;
    }
    free(cb);
    akl_rex_free(rx);
}

int main(void) {
    /* リテラル */
    t("abc", 0, "xxabcyy", 1, 2, 5);
    t("abc", 0, "xxab", 0, 0, 0);
    t("", 0, "xyz", 1, 0, 0);
    /* アンカー */
    t("^abc", 0, "abcdef", 1, 0, 3);
    t("^abc", 0, "zabcdef", 0, 0, 0);
    t("abc$", 0, "zzabc", 1, 2, 5);
    t("abc$", 0, "abcx", 0, 0, 0);
    t("^$", 0, "", 1, 0, 0);
    t("^$", 0, "a", 0, 0, 0);
    /* $ は末尾改行の直前にもマッチ（m なし） */
    t("a$", 0, "a\n", 1, 0, 1);
    t("a$", 0, "a\nb", 0, 0, 0);
    /* m フラグ */
    t("^b", 4, "a\nb", 1, 2, 3);
    t("^b", 0, "a\nb", 0, 0, 0);
    t("b$", 4, "b\na", 1, 0, 1);
    t("b$", 0, "b\na", 0, 0, 0);
    /* ドット */
    t("a.c", 0, "abc", 1, 0, 3);
    t("a.c", 0, "a\nc", 0, 0, 0);
    t("a.c", 8, "a\nc", 1, 0, 3); /* s フラグ */
    t("a.c", 0, "a\nc", 0, 0, 0); /* 行終端は除く */
    /* クラス */
    t("[abc]+", 0, "xxcabayy", 1, 2, 6);
    t("[^abc]+", 0, "abcXXabc", 1, 3, 5);
    t("[a-z]+", 0, "ABCdef", 1, 3, 6);
    t("[a-cx-z]+", 0, "abyz", 1, 0, 4);
    t("[]]", 0, "]", 1, 0, 1);
    t("[0-9]{3}", 0, "ab123c", 1, 2, 5);
    t("[\\d]+", 0, "ab123c", 1, 2, 5);
    t("[\\w]+", 0, "a-b_c", 1, 0, 1);
    t("[\\s]+", 0, "a \t\nb", 1, 1, 4);
    /* 量詞 */
    t("a*", 0, "bbaaa", 1, 0, 0);
    t("a*", 0, "bbb", 1, 0, 0);
    t("a+", 0, "bbaaa", 1, 2, 5);
    t("a+", 0, "bbb", 0, 0, 0);
    t("a?", 0, "bbb", 1, 0, 0);
    t("a?b", 0, "b", 1, 0, 1);
    t("a?b", 0, "ab", 1, 0, 2);
    t("a{2}", 0, "baaab", 1, 1, 3);
    t("a{2}", 0, "baab", 1, 1, 3);
    t("a{2,}", 0, "baaaab", 1, 1, 5);
    t("a{2,3}", 0, "baaaab", 1, 1, 4);
    t("a{2,3}", 0, "baaaaab", 1, 1, 4);
    t("a{0,2}", 0, "bb", 1, 0, 0);
    t("a{0,}", 0, "bb", 1, 0, 0);
    /* 非貪欲 */
    t("a.*?b", 0, "axxbayyb", 1, 0, 4);
    t("a.*b", 0, "axxbayyb", 1, 0, 8);
    t("a+?b", 0, "aaab", 1, 0, 4);
    t("a??b", 0, "ab", 1, 0, 2);
    t("a??b", 0, "b", 1, 0, 1);
    t("a{2,4}?b", 0, "aaab", 1, 0, 4);
    /* グループと選択 */
    t("(ab)+", 0, "ababx", 1, 0, 4);
    t("(a|b)c", 0, "bc", 1, 0, 2);
    t("(a|ab)c", 0, "abc", 1, 0, 3);
    t("(ab|a)c", 0, "abc", 1, 0, 3);
    t("(?:ab)+", 0, "ababx", 1, 0, 4);
    t("a|b|c", 0, "xb", 1, 1, 2);
    t("x|", 0, "abc", 1, 0, 0);
    t("(|a)", 0, "b", 1, 0, 0);
    t("(a|b)*", 0, "ababx", 1, 0, 4);
    /* キャプチャ */
    t_caps("(ab)+", "xxabab", 4, 6);
    t_caps("(a|b)(c|d)", "xxbd", 2, 3);
    t_caps("((a)b)", "xab", 1, 3);
    /* エスケープ */
    t("\\d+", 0, "ab123cd", 1, 2, 5);
    t("\\D+", 0, "123ab", 1, 3, 5);
    t("\\w+", 0, "a-b_c", 1, 0, 1);
    t("\\W+", 0, "ab-", 1, 2, 3);
    t("\\s+", 0, "ab \tcd", 1, 2, 4);
    t("\\S+", 0, " a", 1, 1, 2);
    t("\\bword\\b", 0, "a word b", 1, 2, 6);
    t("\\bword", 0, "sword", 0, 0, 0);
    t("word\\b", 0, "words", 0, 0, 0);
    t("\\Bword", 0, "sword", 1, 1, 5);
    t("\\x41", 0, "xA", 1, 1, 2);
    t("\\u0041", 0, "xA", 1, 1, 2);
    t("\\cA", 0, "\x01", 1, 0, 1);
    t("\\*", 0, "a*b", 1, 1, 2);
    t("\\n", 0, "a\nb", 1, 1, 2);
    t("\\t", 0, "a\tb", 1, 1, 2);
    t("\\.", 0, "a.b", 1, 1, 2);
    /* i フラグ */
    t("abc", 1, "xABCy", 1, 1, 4);
    t("[a-z]+", 1, "ABC", 1, 0, 3);
    t("\\w+", 1, "___", 1, 0, 3);
    /* UTF-8 */
    t("あ", 0, "xあy", 1, 1, 4);
    t("あ+", 0, "あああx", 1, 0, 9);
    t("\\u3042", 0, "xあ", 1, 1, 4);
    t(".", 0, "あ", 1, 0, 3);
    t("^.$", 0, "あ", 1, 0, 3);
    /* バックトラック複合 */
    t("(a+)+b", 0, "aaaaaab", 1, 0, 7);
    t("(a|ab)*c", 0, "ababc", 1, 0, 5);
    /* コンパイルエラー */
    t_err("(");
    t_err("a)");
    t_err("(a");
    t_err("a{2,1}");
    t_err("[");
    t_err("[z-a]");
    t("(?=a)a", 0, "ab", 1, 0, 1);      /* v0.6: 先読み（位置確認して消費） */
    t("a(?=b)b", 0, "ab", 1, 0, 2);
    t("(?=a)b", 0, "ab", 0, 0, 0);      /* a の直後に b は無い */
    t("(?!b)a", 0, "a", 1, 0, 1);
    t("(?!a)a", 0, "a", 0, 0, 0);      /* 否定先読みはマッチしない */
    t("(?!x)a", 0, "a", 1, 0, 1);      /* 否定先読み成功 */
    t_err("(?<n>a)");
    t_err("\\1");                      /* 単独バックリファレンスは未定義グループでエラー */
    t_err("a**");
    t_err("*a");
    t("\\u3042", 0, "あ", 1, 0, 3);    /* v0.6: 非 ASCII リテラル */
    t_err("\\u{1F600}");
    t_err("a\\");
    /* ネスト量詞（挿入シフトの整合性） */
    t("(a*)*", 0, "aaa", 1, 0, 3);
    t("(a*)*b", 0, "aaab", 1, 0, 4);
    t("(a+)+b", 0, "aaaaaab", 1, 0, 7);
    t("(ab*)*c", 0, "abbbbc", 1, 0, 6);
    t("(a?)+b", 0, "aaab", 1, 0, 4);
    t("(a{1,3}){2}", 0, "aaaaaa", 1, 0, 6);
    t("(a|b*)*", 0, "bbabb", 1, 0, 5);
    t("a{0,1}b", 0, "b", 1, 0, 1);
    t("a{0,1}b", 0, "ab", 1, 0, 2);
    t("a{2}?", 0, "aaa", 1, 0, 2);
    /* ステップ制限 */
    {
        total++;
        char err[128];
        AklRex *rx = akl_rex_compile((const uint8_t *)"(a+)+b", (uint32_t)strlen("(a+)+b"), 0, err, sizeof err);
        if (!rx) { printf("FAIL compile (a+)+b\n"); fails++; }
        else {
            const char *s = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
            uint32_t ncap = akl_rex_ncap(rx);
            uint32_t cb[8], ce[8];
            bool lim = false;
            bool m = akl_rex_match(rx, (const uint8_t *)s, (uint32_t)strlen(s), 0, cb, ce, ncap, &lim);
            if (m || !lim) { printf("FAIL step limit (a+)+b => m=%d lim=%d\n", m, lim); fails++; }
            akl_rex_free(rx);
        }
    }
    printf("regex tests: %d total, %d fails\n", total, fails);
    return fails ? 1 : 0;
}
