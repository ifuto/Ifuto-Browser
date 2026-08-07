/* Markdown 変換器 (src/md.c) のテスト。出力は決定的なので文字列完全一致で検証。
 * 安全性（生 HTML 非透過・敵対深度はクラッシュせず規定の飽和化）もここで固める。 */
#define _GNU_SOURCE /* setenv/unsetenv/open_memstream（slice 差分オラクル） */
#include "tests.h"
#include "../src/md.h"
#include "../src/dom.h"   /* if_parse_html（多層防御: MD 出力の HTML 妥当性） */
#include <string.h>

static IfArena g_a;

static void md_eq(const char *src, const char *want) {
    CHECK(1);
    IfStr out;
    if_md_to_html(&g_a, if_str(src, (u32)strlen(src)), &out);
    if (out.n != strlen(want) || memcmp(out.p, want, strlen(want)) != 0) {
        fprintf(stderr, "  md mismatch\n  src : %s\n  want: %s\n  got : %.*s\n",
                src, want, (int)out.n, out.p);
        g_if_test_failures++;
    }
    /* MD 出力は必ず spec 準拠パーサを通す（変換器が出す HTML の well-formed 性を多層保証） */
    IfArena a2;
    if_arena_init(&a2, 1 << 16);
    if_parse_html(&a2, out);
    if_arena_destroy(&a2);
}

void test_md(void) {
    if_arena_init(&g_a, 1 << 16);

    /* 見出し（レベル・閉じ # ・先頭空行） */
    md_eq("# Title", "<h1>Title</h1>\n");
    md_eq("### H3 ###", "<h3>H3</h3>\n");
    md_eq("x\n##H2 (not heading)", "<p>x ##H2 (not heading)</p>\n");
    /* 段落・段落連結・ハードブレーク */
    md_eq("alpha\nbeta", "<p>alpha beta</p>\n");
    md_eq("alpha  \nbeta", "<p>alpha<br>beta</p>\n");
    md_eq("#t 非見出し\n# A\n#A?", "<p>#t 非見出し</p>\n<h1>A</h1>\n<p>#A?</p>\n");
    /* inline: strong/em/del/code/escape */
    md_eq("a **b** and *c* ~~d~~ `e<f`",
          "<p>a <strong>b</strong> and <em>c</em> <del>d</del> <code>e&lt;f</code></p>\n");
    md_eq("\\*no fmt\\*", "<p>*no fmt*</p>\n");
    md_eq("**unclosed", "<p>**unclosed</p>\n");
    md_eq("C#は無傷", "<p>C#は無傷</p>\n");
    /* リンク/画像/自動リンク + 属性 escape（XSS 非透過） */
    md_eq("[t](u?v=1&k=2)", "<p><a href=\"u?v=1&amp;k=2\">t</a></p>\n");
    md_eq("[a *b*](d)", "<p><a href=\"d\">a <em>b</em></a></p>\n");
    md_eq("![alt x](p.png)", "<p><img src=\"p.png\" alt=\"alt x\"></p>\n");
    md_eq("<https://example.jp/?a=1&b=2>",
          "<p><a href=\"https://example.jp/?a=1&amp;b=2\">https://example.jp/?a=1&amp;b=2</a></p>\n");
    /* 生 HTML は通さない（重要: 全テキストは escape される） */
    md_eq("<script>alert(1)</script>", "<p>&lt;script&gt;alert(1)&lt;/script&gt;</p>\n");
    md_eq("<img src=x onerror=alert(1)>", "<p>&lt;img src=x onerror=alert(1)&gt;</p>\n");
    /* 引用（連続・ネスト・フォーマット） */
    md_eq("> q **b**\n> line2", "<blockquote>\n<p>q <strong>b</strong> line2</p>\n</blockquote>\n");
    md_eq("> outer\n> > inner", "<blockquote>\n<p>outer</p>\n<blockquote>\n<p>inner</p>\n</blockquote>\n</blockquote>\n");
    /* リスト（ul/ol・入れ子・中断） */
    md_eq("- a\n- b", "<ul>\n<li>a</li>\n<li>b</li>\n</ul>\n");
    md_eq("1. a\n2. b", "<ol>\n<li>a</li>\n<li>b</li>\n</ol>\n");
    md_eq("- a\n  - x\n  - y\n- b",
          "<ul>\n<li>a<ul>\n<li>x</li>\n<li>y</li>\n</ul>\n</li>\n<li>b</li>\n</ul>\n");
    md_eq("- a\n\ntail", "<ul>\n<li>a</li>\n</ul>\n<p>tail</p>\n");
    /* フェンスコード（言語・escape・未閉鎖 EOF） */
    md_eq("```c\nint x = 1 < 2;\n```", "<pre><code class=\"lang-c\">int x = 1 &lt; 2;\n</code></pre>\n");
    md_eq("```\n&raw\n", "<pre><code>&amp;raw\n</code></pre>\n");
    /* hr */
    md_eq("---", "<hr>\n");
    md_eq("***", "<hr>\n");
    /* "- - text": CommonMark では入れ子 ul だが v0.2md は「項目テキストが
     * マーカーで始まる」のみ近似可視化（構造差分は台帳の偏差として受理） */
    md_eq("- - text", "<ul>\n<li>- text</li>\n</ul>\n");
    md_eq("--- a", "<p>--- a</p>\n"); /* テキスト同行なら hr でない */
    /* GFM 表（inline 展開・セル数差異は素通し=少ないセルは欠損） */
    md_eq("| a | b |\n| --- | --- |\n| *1* | 2 |",
          "<table>\n<thead><tr><th>a</th><th>b</th></tr></thead>\n<tbody>\n<tr><td><em>1</em></td><td>2</td></tr>\n</tbody>\n</table>\n");
    md_eq("a|b\n--|--\n1|2", "<p>a|b --|-- 1|2</p>\n"); /* 3 ハイフン未満 → 表でない */
    /* 脚注（参照順 numbering・多重参照は id 一意） */
    md_eq("text[^a] more[^b] again[^a]\n\n[^a]: first **n**\n[^b]: second",
          "<p>text<sup><a href=\"#fn-a\" id=\"fr-a\">1</a></sup> more<sup><a href=\"#fn-b\" id=\"fr-b\">2</a></sup> "
          "again<sup><a href=\"#fn-a\" id=\"fr-a-2\">1</a></sup></p>\n"
          "<section class=\"footnotes\">\n<hr>\n<ol>\n"
          "<li id=\"fn-a\">first <strong>n</strong> <a href=\"#fr-a\">↩</a></li>\n"
          "<li id=\"fn-b\">second <a href=\"#fr-b\">↩</a></li>\n"
          "</ol>\n</section>\n");
    /* CRLF 正規化 */
    md_eq("a\r\nb\r\n\r\nc", "<p>a b</p>\n<p>c</p>\n");
    /* 敵対深度: 引用 100 段は飽和して必ず終了（=クラッシュしない） */
    {
        char deep[1024];
        u32 n = 0;
        for (int i = 0; i < 100; i++) { deep[n++] = '>'; deep[n++] = ' '; }
        memcpy(deep + n, "x", 2);
        n += 1;
        IfStr out;
        if_md_to_html(&g_a, if_str(deep, n), &out);
        CHECK(out.n > 0 && memchr(out.p, 'x', out.n) != NULL);
        /* 8 段で飽和している（'blockquote' の出現回数は有限） */
        u32 cnt = 0;
        for (u32 i = 0; i + 11 < out.n; i++)
            if (memcmp(out.p + i, "<blockquote>", 12) == 0) cnt++;
        CHECK(cnt <= 9);
    }
    /* 拡張子判定 */
    CHECK(if_path_is_md("README.md"));
    CHECK(if_path_is_md("/a/b/Guide.MARKDOWN"));
    CHECK(!if_path_is_md("index.html"));
    CHECK(!if_path_is_md("noext"));

    if_arena_destroy(&g_a);
}

/* ---- md 2-slice 並列経路の差分オラクル（serial ≡ sliced の機械固定） ----
 * ≥1MB・NUL/CR なしの入力で md_parse_fast_f は 2-slice 並列経路（md_par_scan が
 * 分割点を見つけた場合）に入る。この経路の「生成 DOM/フラグは単走査と逐語同値」
 * を機械固定する:
 *   1. IF_MD_PAR=0（強制 serial）と既定（並列）で if_dom_dump 全バイト一致
 *      （dump の末尾行が nodes/errors/title メタを含むため同値性はメタまで及ぶ）
 *   2. 並列側は md_body_mid != NULL（sliced 経路が本当に発動したことの証明。
 *      分割点が見つからず serial に落ちた場合はテストが空転するのを防ぐ）
 *   3. n_nodes / md_ws_stripped 一致
 * 入力は fence（# や - の囮行入り）・GFM 表・入れ子リスト・CJK・引用・リンクを
 * 混ぜ、fence 状態機械が囮を境界候補から外す経路を必ず通す。NUL/CR/"[^" は
 * 一切含めない（含むと規約どおり sliced が不発する）。 */
void test_md_slice_oracle(void) {
    /* 決定的生成: ブロック循環ローテーションで ~1.2MB */
    size_t cap = 1400000, n = 0;
    char *buf = (char *)malloc(cap);
    CHECK(buf != NULL);
    if (!buf) return;
    u32 blk = 0;
    while (n < 1100000) {
        int m = 0;
        switch (blk % 7) {
        case 0: m = snprintf(buf + n, cap - n, "# Heading %u\n\nparagraph **b%u** and *e%u* with [link%u](http://x/%u) tail\n\n", blk, blk, blk, blk, blk); break;
        case 1: m = snprintf(buf + n, cap - n, "- item %ua\n- item %ub\n  - nested %u\n\nplain after list %u\n\n", blk, blk, blk, blk); break;
        case 2: m = snprintf(buf + n, cap - n, "```c\n# decoy heading in fence %u\n- decoy item in fence %u\n| decoy | table %u |\n```\n\n", blk, blk, blk); break;
        case 3: m = snprintf(buf + n, cap - n, "| col a%u | col b%u |\n|---|---|\n| v%u | w%u |\n\n", blk, blk, blk, blk); break;
        case 4: m = snprintf(buf + n, cap - n, "> quote level %u continues here\n>\n> second para %u\n\n", blk, blk); break;
        case 5: m = snprintf(buf + n, cap - n, "日本語テキスト%u番の段落。漢字かな交じり文で幅計算経路を通す。もう一文。\n\n", blk); break;
        default: m = snprintf(buf + n, cap - n, "para %u with `inline code` and ~~strike~~ and ==mark== end\n\n", blk); break;
        }
        CHECK(m > 0 && (size_t)m < cap - n);
        if (m <= 0) break;
        n += (size_t)m; blk++;
    }

    const char *old_env = getenv("IF_MD_PAR");
    setenv("IF_MD_PAR", "0", 1);
    IfArena sa; if_arena_init(&sa, 1 << 22);
    IfDom *ds = NULL;
    bool ok_s = if_md_parse_fast_f(&sa, if_str(buf, (u32)n), &ds, IF_MD_F_SLIM_ATTRS);
    if (old_env) setenv("IF_MD_PAR", old_env, 1); else unsetenv("IF_MD_PAR");
    IfArena pa; if_arena_init(&pa, 1 << 22);
    IfDom *dp = NULL;
    bool ok_p = if_md_parse_fast_f(&pa, if_str(buf, (u32)n), &dp, IF_MD_F_SLIM_ATTRS);
    /* 環境は両評価後に復旧済み（sliced 評価は 2 回目 getenv で既定経路） */

    CHECK(ok_s && ok_p && ds && dp);
    if (ok_s && ok_p && ds && dp) {
        CHECK(dp->md_body_mid != NULL);           /* sliced 発動の機械証明（空転防止） */
        CHECK(ds->md_body_mid == NULL);           /* serial は決して設定しない */
        CHECK(ds->n_nodes == dp->n_nodes);
        CHECK(ds->md_ws_stripped == dp->md_ws_stripped);
        char *bs = NULL, *bp = NULL; size_t ns = 0, np = 0;
        FILE *os = open_memstream(&bs, &ns), *op = open_memstream(&bp, &np);
        CHECK(os && op);
        if (os && op) {
            if_dom_dump(ds, os); fclose(os);
            if_dom_dump(dp, op); fclose(op);
            CHECK(ns == np && memcmp(bs, bp, ns) == 0); /* dump 末尾行（nodes/errors/title）まで含む全バイト一致 */
            free(bs); free(bp);
        }
    }
    if_arena_destroy(&sa);
    if_arena_destroy(&pa);
    free(buf);
}
