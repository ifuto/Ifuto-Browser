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
        CHECK(t && t->kind == IF_NODE_TEXT && if_str_eq(t->text, IF_S("Hello")));
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
        CHECK(t && if_str_eq(t->text, IF_S("p > a { color: red }")));
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

    if_arena_destroy(&a);
}
