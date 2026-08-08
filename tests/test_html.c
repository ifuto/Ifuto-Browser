#define _GNU_SOURCE /* open_memstream（fragment シリアライズ オラクル） */
#include "tests.h"
#include "../src/dom.h"
#include <string.h>

static IfNode *first_elem(IfNode *n) {
    for (IfNode *c = n->first_child; c; c = c->next_sibling)
        if (c->kind == IF_NODE_ELEMENT) return c;
    return NULL;
}
static IfNode *next_elem(IfNode *n) {
    for (IfNode *c = n->next_sibling; c; c = c->next_sibling)
        if (c->kind == IF_NODE_ELEMENT) return c;
    return NULL;
}

static IfDom *parse(IfArena *a, const char *html) {
    return if_parse_html(a, if_str(html, (u32)strlen(html)));
}

/* slim-DOM テスト用: 本文中に needle を含む TEXT ノードが居るか（content 辺も潜る） */
static bool sl_tree_text_has(const IfDom *d, const IfNode *n, const char *needle) {
    size_t nl = strlen(needle);
    if (n->kind == IF_NODE_TEXT && n->u.text.n >= nl) {
        for (u32 i = 0; i + nl <= n->u.text.n; i++)
            if (memcmp(n->u.text.p + i, needle, nl) == 0) return true;
    }
    for (const IfNode *c = n->first_child; c; c = c->next_sibling)
        if (sl_tree_text_has(d, c, needle)) return true;
    IfNode *tc = if_dom_tpl_content(d, n); /* template rare-data 辺 */
    if (tc && sl_tree_text_has(d, tc, needle)) return true;
    return false;
}
static bool sl_tree_tag_exists(const IfDom *d, const IfNode *n, u16 tag) {
    if (n->kind == IF_NODE_ELEMENT && n->tag == tag) return true;
    for (const IfNode *c = n->first_child; c; c = c->next_sibling)
        if (sl_tree_tag_exists(d, c, tag)) return true;
    IfNode *tc = if_dom_tpl_content(d, n); /* template rare-data 辺 */
    if (tc && sl_tree_tag_exists(d, tc, tag)) return true;
    return false;
}

void test_html(void) {
    /* タグ表の宣言長さと strlen の整合（round-trip は n を信じるためこの誤りを検出できない）*/
    CHECK(if_dom_tag_table_sane());
    /* タグ表の整合性（enum とテーブルのズレ検出） */
    for (u16 i = 1; i < IF_TAG_N_TAGS; i++) {
        const char *n = if_tag_name(i);
        CHECK(n != NULL);
        IfStr s = if_str(n, (u32)strlen(n));
        CHECK(if_tag_id(s) == i);
    }
    CHECK(if_tag_id(IF_S("DIV")) == IF_TAG_DIV);   /* CI 照合 */
    CHECK(if_tag_id(IF_S("zznotatag")) == IF_TAG_UNKNOWN);
    CHECK(if_tag_is_void(IF_TAG_BR) && if_tag_is_void(IF_TAG_IMG));
    CHECK(!if_tag_is_void(IF_TAG_DIV));
    CHECK(if_tag_is_rawtext(IF_TAG_STYLE) && if_tag_is_rawtext(IF_TAG_SCRIPT));

    IfArena a; if_arena_init(&a, 1 << 16);

    /* 暗黙 html/head/body + title 回収 */
    {
        IfDom *d = parse(&a, "<title>Hi</title><p>Hello");
        IfNode *html = first_elem(d->root);
        CHECK(html && html->tag == IF_TAG_HTML);
        IfNode *head = first_elem(html);
        CHECK(head && head->tag == IF_TAG_HEAD);
        CHECK(d->title.p && if_str_eq(d->title, IF_S("Hi")));
        IfNode *body = next_elem(head);
        CHECK(body && body->tag == IF_TAG_BODY);
        IfNode *p = first_elem(body);
        CHECK(p && p->tag == IF_TAG_P);
        IfNode *t = p->first_child;
        CHECK(t && t->kind == IF_NODE_TEXT && if_str_eq(t->u.text, IF_S("Hello")));
    }

    /* p の暗黙終了 + 属性（デコード込み） + 重複属性 first-wins */
    {
        IfDom *d = parse(&a, "<p class=a&gt; id=x id=y>one<p>two");
        IfNode *body = first_elem(first_elem(d->root)->next_sibling ? first_elem(d->root) : d->root);
        /* html→body をたどる */
        IfNode *html = first_elem(d->root);
        IfNode *bd = NULL;
        for (IfNode *c = html->first_child; c; c = c->next_sibling)
            if (c->tag == IF_TAG_BODY) bd = c;
        (void)body;
        CHECK(bd != NULL);
        IfNode *p1 = first_elem(bd);
        CHECK(p1 && p1->tag == IF_TAG_P);
        /* class の値は文字参照デコードで "a>"（&gt; → '>' の1文字） */
        IfStr cls = if_dom_attr(p1, "CLASS");
        CHECK(if_str_eq(cls, IF_S("a>")));
        CHECK(if_str_eq(if_dom_attr(p1, "id"), IF_S("x"))); /* 重複は first-wins */
        IfNode *p2 = next_elem(p1);
        CHECK(p2 && p2->tag == IF_TAG_P);
        CHECK(p1->next_sibling != NULL);
    }

    /* rawtext: <style> の中の '<' はタグにならない */
    {
        IfDom *d = parse(&a, "<style>p > a { color: red }</style><p>x</p>");
        IfNode *html = first_elem(d->root);
        IfNode *style = NULL;
        for (IfNode *c = html->first_child; c && !style; c = c->next_sibling)
            for (IfNode *g = c->first_child; g; g = g->next_sibling)
                if (g->tag == IF_TAG_STYLE) style = g;
        CHECK(style != NULL);
        IfNode *t = style->first_child;
        CHECK(t && t->kind == IF_NODE_TEXT);
        CHECK(t && if_str_eq(t->u.text, IF_S("p > a { color: red }")));
    }

    /* li の暗黙終了 */
    {
        IfDom *d = parse(&a, "<ul><li>a<li>b<li>c</ul>");
        IfNode *li1 = NULL;
        for (IfNode *c = d->root->first_child; c; c = c->next_sibling) {
            (void)c;
        }
        /* body から ul を探す */
        IfNode *ul = NULL;
        IfNode *html = first_elem(d->root);
        for (IfNode *c = html->first_child; c; c = c->next_sibling)
            if (c->tag == IF_TAG_BODY)
                for (IfNode *g = c->first_child; g; g = g->next_sibling)
                    if (g->tag == IF_TAG_UL) ul = g;
        CHECK(ul != NULL);
        li1 = first_elem(ul);
        CHECK(li1 && li1->tag == IF_TAG_LI);
        IfNode *li2 = next_elem(li1);
        IfNode *li3 = li2 ? next_elem(li2) : NULL;
        CHECK(li2 && li3);
        CHECK(li3->next_sibling == NULL);
    }

    /* 敵対的入力: すべて crash せず終了することが要件 */
    {
        static const char *hostile[] = {
            "", "<", "<<<", "<!", "<!-", "<!--", "<!-->", "<!--->", "<!-- x",
            "<p", "<p ", "<p a", "<p a=", "<p a='x", "</", "</>", "<?php ?>",
            "<p>\x00\x00</p>", "&#xffffff;&#0;&#xD800;", "&amp &lt &unknown;",
            "<div><div><div><span></div></span>", "<b><i></b></i>",
            "<style>/* never closed", "<title>t<t</title>",
            "a&#65;b&#x41;c", "<p a=b c='d e' f=\"g\">",
        };
        for (u32 i = 0; i < sizeof(hostile) / sizeof(hostile[0]); i++) {
            IfArena ha; if_arena_init(&ha, 1 << 14);
            IfDom *d = if_parse_html(&ha, if_str(hostile[i], (u32)strlen(hostile[i])));
            CHECK(d != NULL);
            if_arena_destroy(&ha);
        }
    }

    /* 深い入れ子（上限内）はOK、上限超過でも死なない */
    {
        IfArena ha; if_arena_init(&ha, 1 << 20);
        u32 depth = 2000;
        u64 sz = (u64)depth * 6 + 16;
        char *buf = (char *)malloc(sz);
        u64 w = 0;
        for (u32 i = 0; i < depth; i++) { memcpy(buf + w, "<div>", 5); w += 5; }
        memcpy(buf + w, "x", 1); w += 1;
        IfDom *d = if_parse_html(&ha, if_str(buf, (u32)w));
        CHECK(d != NULL);
        free(buf);
        if_arena_destroy(&ha);
    }

    /* ---- slim-DOM（法則: 画面描画に関係ないものは DOM しない） ---- */
    {
        const char *doc =
            "<!doctype html><title>KeepMe</title><p>alpha</p>"
            "<script>var x = 'script-body-vanish';</script>"
            "<style>.keep { color: red }</style>"
            "<template><div>t-body-vanish</div></template>"
            "<p>omega <b>bold-stays</b></p>";
        u32 full_nodes;
        { /* full との差分測定 */
            IfArena fa; if_arena_init(&fa, 1 << 16);
            IfDom *d = if_parse_html(&fa, if_str(doc, (u32)strlen(doc)));
            CHECK(d != NULL);
            CHECK(sl_tree_text_has(d, d->root, "script-body-vanish")); /* full では居る */
            CHECK(sl_tree_tag_exists(d, d->root, IF_TAG_DIV));
            full_nodes = d->n_nodes;
            if_arena_destroy(&fa);
        }
        CHECK(!if_dom_slim);
        if_dom_slim = true;
        IfArena sa; if_arena_init(&sa, 1 << 16);
        IfDom *d = if_parse_html(&sa, if_str(doc, (u32)strlen(doc)));
        CHECK(d != NULL);
        /* title は tab 表示情報 → 残る。style 本文は cascade が読む → 残る */
        CHECK(if_str_eq(d->title, IF_S("KeepMe")));
        CHECK(sl_tree_tag_exists(d, d->root, IF_TAG_SCRIPT)); /* root は marker として残る */
        CHECK(sl_tree_tag_exists(d, d->root, IF_TAG_TEMPLATE));
        CHECK(sl_tree_text_has(d, d->root, "alpha") && sl_tree_text_has(d, d->root, "bold-stays"));
        /* script の本文・template の子孫は DOM に存在しない（content 辺も探索済み） */
        CHECK(!sl_tree_text_has(d, d->root, "script-body-vanish"));
        CHECK(!sl_tree_text_has(d, d->root, "t-body-vanish"));
        CHECK(sl_tree_text_has(d, d->root, ".keep"));         /* style 本文は残す設計 */
        CHECK(!sl_tree_tag_exists(d, d->root, IF_TAG_DIV));   /* template 下 div 無し */
        /* ノード数は full より厳密に少ない（剃りの証跡を数値で） */
        CHECK(d->n_nodes < full_nodes);
        if_arena_destroy(&sa);
        if_dom_slim = false;
        { /* グローバル復元後は full と一致することを証明 */
            IfArena ra; if_arena_init(&ra, 1 << 16);
            IfDom *d2 = if_parse_html(&ra, if_str(doc, (u32)strlen(doc)));
            CHECK(d2 && d2->n_nodes == full_nodes);
            CHECK(sl_tree_text_has(d2, d2->root, "script-body-vanish"));
            if_arena_destroy(&ra);
        }
    }

    if_arena_destroy(&a);
}

/* ---- fragment parsing（WHATWG 13.4）固定オラクル ----
 * 既定値は tests/wpt-tree-construction 採点済みの本物ケースから機械抽出した bytes。
 * 目的: python ハーネス非依存で fragment 経路の縮退を機械検出する。 */
void test_frag_parse(void) {
    static const struct { const char *ctx, *data, *want; } FRAG_CASES[] = {
        /* foreign-fragment.dat#0: breakout（svg ctx で nobr は HTML へ） */
        { "svg path", "<nobr>X",
          "| <nobr>\n"
          "|   \"X\"" },
        /* foreign-fragment.dat#1: font color 属性付き breakout */
        { "svg path", "<font color></font>X",
          "| <font>\n"
          "|   color=\"\"\n"
          "| \"X\"" },
        /* foreign-fragment.dat#18: math TIP + 非 void self-closing は push（<ms/>X） */
        { "math ms", "<b></b><mglyph/><i></i><malignmark/><u></u><ms/>X",
          "| <b>\n"
          "| <math mglyph>\n"
          "| <i>\n"
          "| <math malignmark>\n"
          "| <u>\n"
          "| <ms>\n"
          "|   \"X\"" },
        /* foreign-fragment.dat#38: annotation-xml（encoding 無し）で div は breakout */
        { "math annotation-xml", "<div></div>",
          "| <div>" },
        /* foreign-fragment.dat#47: breakout 後の入れ子は HTML 規則 */
        { "svg svg", "<div><h1>X</h1></div>",
          "| <div>\n"
          "|   <h1>\n"
          "|     \"X\"" },
        /* tests4.dat#8: script ctx は EOF までが 1 TEXT（appropriate end tag 非合致） */
        { "script", "<!-- inside </script> -->",
          "| \"<!-- inside </script> -->\"" },
        /* template.dat#108: template ctx（content + 初期 in-template モード） */
        { "template", "<template><form><input name=\"q\"></form><div>second</div></template>",
          "| <template>\n"
          "|   content\n"
          "|     <form>\n"
          "|       <input>\n"
          "|         name=\"q\"\n"
          "|     <div>\n"
          "|       \"second\"" },
        /* tests6.dat#29: frameset ctx で stray </frameset> は無視され frame は挿入 */
        { "frameset", "</frameset><frame>",
          "| <frame>" },
        /* tests_innerHTML_1.dat#19: caption ctx の table はネスト挿入 */
        { "caption", "<table></table><tbody>",
          "| <table>" },
        /* tests_innerHTML_1.dat#63: td ctx で畳める cell 無しの <th> は落ちる */
        { "td", "<th><a>",
          "| <a>" },
        /* tests_innerHTML_1.dat#75: select ctx 直下の <input> は抑止 */
        { "select", "<input><option>",
          "| <option>" },
        /* tests_innerHTML_1.dat#78: </html> は in-body→after-body 規則でコメントは html 末子 */
        { "html", "</html><!--abc-->",
          "| <head>\n"
          "| <body>\n"
          "| <!-- abc -->" },
    };
    for (u32 i = 0; i < sizeof FRAG_CASES / sizeof FRAG_CASES[0]; i++) {
        IfArena a; if_arena_init(&a, 1 << 16);
        IfDom *d = if_parse_html_fragment(&a, if_str(FRAG_CASES[i].data,
                                           (u32)strlen(FRAG_CASES[i].data)),
                                          FRAG_CASES[i].ctx);
        CHECK(d != NULL);
        if (!d) { if_arena_destroy(&a); continue; }
        char *bs = NULL; size_t ns = 0;
        FILE *os = open_memstream(&bs, &ns);
        CHECK(os != NULL);
        if (!os) { if_arena_destroy(&a); continue; }
        if_dom_serialize_wpt_frag(d, os);
        fclose(os);
        /* fragment 期待は「仮想 html root の子群」。実出力の末尾改行 1 個は
         * ハーネス規約どおり落として比較（python 側の rstrip と同じ正規化） */
        size_t got_n = ns;
        while (got_n && bs[got_n - 1] == '\n') got_n--;
        size_t want_n = strlen(FRAG_CASES[i].want);
        if (!(got_n == want_n && memcmp(bs, FRAG_CASES[i].want, got_n) == 0)) {
            fprintf(stderr, "--- frag case #%u (ctx=%s data=%.60s)\n", i,
                    FRAG_CASES[i].ctx, FRAG_CASES[i].data);
            fprintf(stderr, "got:\n%.*s\nwant:\n%s\n", (int)ns, bs, FRAG_CASES[i].want);
            CHECK(0);
        }
        free(bs);
        if_arena_destroy(&a);
    }
}
