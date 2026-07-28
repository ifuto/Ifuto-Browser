#include "tests.h"
#include "../src/css.h"
#include <string.h>

static IfNode *find_tag(IfNode *n, u16 tag) {
    if (!n) return NULL;
    if (n->kind == IF_NODE_ELEMENT && n->tag == tag) return n;
    for (IfNode *c = n->first_child; c; c = c->next_sibling) {
        IfNode *r = find_tag(c, tag);
        if (r) return r;
    }
    return NULL;
}

static IfDom *parse_doc(IfArena *a, const char *html) {
    IfDom *d = if_parse_html(a, if_str(html, (u32)strlen(html)));
    if_style_apply(a, d);
    return d;
}

void test_css(void) {
    IfArena a; if_arena_init(&a, 1 << 18);

    /* 色パース */
    {
        u32 c;
        CHECK(if_css_color(IF_S("#fff"), &c) && c == 0xFFFFFFFF);
        CHECK(if_css_color(IF_S("#ff0000"), &c) && c == 0xFF0000FF);
        CHECK(if_css_color(IF_S("#0000ff80"), &c) && c == 0x0000FF80);
        CHECK(if_css_color(IF_S("red"), &c) && c == 0xFF0000FF);
        CHECK(if_css_color(IF_S("BLUE"), &c) && c == 0x0000FFFF);
        CHECK(if_css_color(IF_S("rgb(1,2,3)"), &c) && c == 0x010203FF);
        CHECK(if_css_color(IF_S("rgba(1,2,3,4)"), &c) && c == 0x01020304);
        CHECK(if_css_color(IF_S("transparent"), &c) && c == 0);
        CHECK(!if_css_color(IF_S("#ff"), &c));
        CHECK(!if_css_color(IF_S("rgb(1,2)"), &c));
        CHECK(!if_css_color(IF_S("nosuchcolor"), &c));
    }

    /* 基本カスケード: UA デフォルト（h1 は block/bold/2em） */
    {
        IfDom *d = parse_doc(&a, "<h1>x</h1>");
        IfNode *h1 = find_tag(d->root, IF_TAG_H1);
        CHECK(h1 && h1->style);
        CHECK(h1->style->display == IF_D_BLOCK);
        CHECK(h1->style->bold);
        CHECK(h1->style->font_size > 31.0f && h1->style->font_size < 33.0f);
    }

    /* author sheet が UA を上書き + class セレクタ + specificity */
    {
        const char *doc =
            "<style>"
            "p { color: red }"
            ".a { color: #00ff00 }"
            "p.a { color: blue }"
            "#x { color: rgb(9,9,9) }"
            "</style>"
            "<p class=a id=x>hello</p><p>two</p>";
        IfDom *d = parse_doc(&a, doc);
        IfNode *p1 = NULL;
        for (IfNode *c = find_tag(d->root, IF_TAG_BODY)->first_child; c; c = c->next_sibling)
            if (c->tag == IF_TAG_P && !p1) p1 = c;
        CHECK(p1 != NULL);
        /* id セレクタが最強 (spec 0x10000) → rgb(9,9,9) */
        CHECK(p1->style->color == 0x090909FF);
        IfNode *p2 = p1->next_sibling;
        CHECK(p2 && p2->tag == IF_TAG_P);
        CHECK(p2->style->color == 0xFF0000FF); /*素の p は red */
    }

    /* !important は inline を破り、子孫結合子・子結合子の違い */
    {
        const char *doc =
            "<style>"
            "div p { color: #111111 }"
            "div > p { color: #222222 !important }"
            "</style>"
            "<div><p style=\"color: #333333\">a</p><span><p>b</p></span></div>";
        IfDom *d = parse_doc(&a, doc);
        IfNode *div = find_tag(d->root, IF_TAG_DIV);
        IfNode *p1 = NULL, *p2 = NULL;
        for (IfNode *c = div->first_child; c; c = c->next_sibling) {
            if (c->tag == IF_TAG_P) p1 = c;
            if (c->tag == IF_TAG_SPAN) p2 = find_tag(c, IF_TAG_P);
        }
        CHECK(p1 && p2);
        /* p1: inline #333 より author !important #222 が勝つ */
        CHECK(p1->style->color == 0x222222FF);
        /* p2: div > p は不適格（親は span）、div p が効く → #111 */
        CHECK(p2->style->color == 0x111111FF);
    }

    /* shorthand 展開 + 継承 + display none */
    {
        const char *doc =
            "<style>div{margin:1px 2px 3px 4px;border:1px solid #123456;padding:8px}</style>"
            "<div><em style=\"display:none\">hide</em><b>show</b></div>";
        IfDom *d = parse_doc(&a, doc);
        IfNode *div = find_tag(d->root, IF_TAG_DIV);
        CHECK(div->style->margin[0].v == 1.0f && div->style->margin[1].v == 2.0f &&
              div->style->margin[2].v == 3.0f && div->style->margin[3].v == 4.0f);
        CHECK(div->style->border_w[0] == 1.0f && div->style->border_w[3] == 1.0f);
        CHECK(div->style->border_color == 0x123456FF);
        CHECK(div->style->padding[0].v == 8.0f && div->style->padding[3].v == 8.0f);
        IfNode *em = find_tag(div, IF_TAG_EM);
        CHECK(em->style->display == IF_D_NONE);
        IfNode *b = find_tag(div, IF_TAG_B);
        CHECK(b && b->style->display == IF_D_INLINE);
        CHECK(b->style->bold);
    }

    /* 未対応構文（pseudo/属性/兄弟）のセレクタは捨て、他は生きる */
    {
        const char *doc =
            "<style>"
            "p:hover { color: red; font-size: 99px }"
            "p[title] { color: red }"
            "span + p { color: red }"
            "p { color: #00ff00 }"
            "@media screen { p { color: red } }"
            "</style><p>x</p>";
        IfDom *d = parse_doc(&a, doc);
        IfNode *p = find_tag(d->root, IF_TAG_P);
        CHECK(p->style->color == 0x00FF00FF);          /* 正常ルールのみ適用 */
        CHECK(p->style->font_size == 16.0f);           /* :hover ルールは死んでいる */
    }

    /* 敵対的 CSS: 止まらず落ちないことが要件 */
    {
        static const char *bad_css[] = {
            "", "}", "{", "p{", "p{color", "p{color:}", "}{", "p;;;{;;;}",
            "@media{", "@import 'x", "p{color:#f}", "p{@#!$%}", "\\", "p\\{a:b}",
            "p{a:1;a:2;a:3;a:4;a:5}", "{color:red}", "p{color:red", "/* unclosed",
            "p{color:" "rgb(300,-5,x)}", "p{margin:1px 2px 3px 4px 5px}",
        };
        for (u32 i = 0; i < sizeof(bad_css) / sizeof(bad_css[0]); i++) {
            IfArena ha; if_arena_init(&ha, 1 << 14);
            IfStyleSheet *sh = if_css_parse(&ha, if_str(bad_css[i], (u32)strlen(bad_css[i])), 0);
            CHECK(sh != NULL);
            if_arena_destroy(&ha);
        }
    }

    /* セレクタMatcher 単体: 子孫バックトラック */
    {
        IfArena ha; if_arena_init(&ha, 1 << 15);
        const char *doc =
            "<section><div class=x><p><span><b class=y>t</b></span></p></div></section>";
        IfDom *d = if_parse_html(&ha, if_str(doc, (u32)strlen(doc)));
        IfStyleSheet *sh = if_css_parse(&ha, IF_S("section div p span .y{}"), 0);
        /* 空宣言はルールごと捨てられるので宣言つきで */
        sh = if_css_parse(&ha, IF_S("section div p span .y{color:red}"), 0);
        IfNode *b = find_tag(d->root, IF_TAG_B);
        CHECK(b != NULL);
        CHECK(sh->n_rules == 1);
        CHECK(if_css_match_selector(b, &sh->rules[0].sels[0]));
        IfStyleSheet *sh2 = if_css_parse(&ha, IF_S("div > b{color:red}"), 0);
        CHECK(!if_css_match_selector(b, &sh2->rules[0].sels[0]));
        if_arena_destroy(&ha);
    }

    if_arena_destroy(&a);
}
