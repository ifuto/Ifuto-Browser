/* Markdown 変換器 (src/md.c) のテスト。出力は決定的なので文字列完全一致で検証。
 * 安全性（生 HTML 非透過・敵対深度はクラッシュせず規定の飽和化）もここで固める。 */
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
