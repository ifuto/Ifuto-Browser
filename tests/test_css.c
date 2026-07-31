#include "tests.h"
#include "../src/css_blink.h"
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

    /* ---- RuleSet 索引: 決定的ケース ---- */
    {
        IfArena ha; if_arena_init(&ha, 1 << 16);

        /* cascade order の一意化（旧バグ: rule++ 刻みで decl order がルール間衝突し、
         * 同 spec 同重要度のタイで後勝ちが成立しなかった。索引差分監査が炙り出し） */
        {
            const char *doc =
                "<style>.a{background-color:#111;color:red}.b{color:blue}</style>"
                "<p class='a b'>x</p>";
            IfDom *d = if_parse_html(&ha, if_str(doc, (u32)strlen(doc)));
            if_style_apply(&ha, d);
            IfNode *p = find_tag(d->root, IF_TAG_P);
            CHECK(p && p->style);
            CHECK(p->style->color == 0x0000FFFF); /* 後規則 .b の blue が勝つ（旧: 衝突で red） */
        }

        /* 未知タグの CI 照合（selector 大文字 × DOM 小文字、bucket は lowercase 正規化で一致） */
        {
            const char *doc =
                "<style>XFOO{color:rgb(1,2,3)}</style><xfoo>t</xfoo><xbar>u</xbar>";
            IfDom *d = if_parse_html(&ha, if_str(doc, (u32)strlen(doc)));
            if_style_apply(&ha, d);
            IfNode *xf = NULL, *xb = NULL;
            for (IfNode *nd = d->root; nd; nd = NULL) { break; } /* placeholder */
            /* preorder 走査で拾う */
            IfNode *stk[64]; u32 sn = 0; stk[sn++] = d->root;
            while (sn) {
                IfNode *nd = stk[--sn];
                if (nd->kind == IF_NODE_ELEMENT && nd->tag == IF_TAG_UNKNOWN) {
                    if (nd->tag_name.n == 4 && memcmp(nd->tag_name.p, "xfoo", 4) == 0) xf = nd;
                    if (nd->tag_name.n == 4 && memcmp(nd->tag_name.p, "xbar", 4) == 0) xb = nd;
                }
                for (IfNode *c = nd->first_child; c; c = c->next_sibling)
                    if (sn < 64) stk[sn++] = c;
            }
            CHECK(xf && xf->style && xf->style->color == 0x010203FF);
            CHECK(xb && xb->style && xb->style->color != 0x010203FF);
        }

        /* 複数セレクタが別バケツに散る 1 ルール: 両経路で同じ decl が届く */
        {
            const char *doc =
                "<style>div#m, .k1.z2{color:rgb(7,8,9)}</style>"
                "<div id=m>a</div><span class='k1 z2'>b</span><span class=k1>c</span>";
            IfDom *d = if_parse_html(&ha, if_str(doc, (u32)strlen(doc)));
            if_style_apply(&ha, d);
            IfNode *dv = find_tag(d->root, IF_TAG_DIV);
            CHECK(dv && dv->style->color == 0x070809FF);
            /* span: .k1.z2 を持つ方だけ届く（class バケツ経由） */
            u32 nhit = 0, nmiss = 0;
            for (IfNode *nd = d->root; nd;) { break; }
            IfNode *stk2[64]; u32 sn2 = 0; stk2[sn2++] = d->root;
            while (sn2) {
                IfNode *nd = stk2[--sn2];
                if (nd->kind == IF_NODE_ELEMENT && nd->tag == IF_TAG_SPAN && nd->style) {
                    if (nd->style->color == 0x070809FF) nhit++; else nmiss++;
                }
                for (IfNode *c = nd->first_child; c; c = c->next_sibling)
                    if (sn2 < 64) stk2[sn2++] = c;
            }
            CHECK(nhit == 1 && nmiss == 1);
        }

        /* Blink ファサード（概念・形状互換層）の実動: recalc→computed style→照合 */
        {
            const char *doc = "<style>.fx{color:rgb(5,6,7)}</style><p class=fx>t</p>";
            IfutoDocument *d = if_parse_html(&ha, if_str(doc, (u32)strlen(doc)));
            ifuto_style_recalc(&ha, d);
            IfutoElement *p = find_tag(d->root, IF_TAG_P);
            const IfutoComputedStyle *cst = ifuto_computed_style(p);
            CHECK(cst && cst->color == 0x050607FF);
            IfutoStyleSheetContents *one =
                ifuto_stylesheet_create_and_parse(&ha, IF_S(".fx{color:red}"), 0);
            CHECK(one && one->rs.n_pool == 1);
            CHECK(ifuto_selector_matches(p, &one->rules[0].sels[0]));
            ifuto_ruleset_set_naive_matching(0);
        }

        /* universal のみのルールは常時スキャン区画から届く */
        {
            const char *doc = "<style>*{font-weight:bold}</style><p>t</p>";
            IfDom *d = if_parse_html(&ha, if_str(doc, (u32)strlen(doc)));
            if_style_apply(&ha, d);
            IfNode *p = find_tag(d->root, IF_TAG_P);
            CHECK(p && p->style && p->style->bold);
        }
        if_arena_destroy(&ha);
    }

    if_arena_destroy(&a);
}

/* ---- RuleSet 索引の差分オラクル（on/off 機械監査。CoJIT oracle と同型） ----
 * 構造化ランダム sheet × DOM に対し naive 全走査と索引候補走査の計算済みスタイルが
 * 全ノードでビット一致することを検証する。索引が意味を変えた瞬間ここが赤くなる。 */
typedef struct { u32 tag; IfStyle st; } CssSnap;
static u32 css_snapshot(IfNode *n, CssSnap *out, u32 cap, u32 cnt) {
    if (n->kind == IF_NODE_ELEMENT && n->style && cnt < cap) {
        out[cnt].tag = n->tag;
        out[cnt].st = *n->style;
        cnt++;
    }
    for (IfNode *c = n->first_child; c && cnt < cap; c = c->next_sibling)
        cnt = css_snapshot(c, out, cap, cnt);
    return cnt;
}

static u32 css_rng(u32 *st) {
    u32 x = *st;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *st = x;
}

void test_css_ruleset_oracle(void) {
    static const char *tags[]  = { "div", "span", "p", "b" };
    static const char *props[] = {
        "color:rgb(%u,%u,%u)", "background-color:rgb(%u,%u,%u)",
        "font-weight:%s", "font-style:%s", "text-decoration:%s"
    };
    enum { MAXE = 96 };
    CssSnap *sa = (CssSnap *)malloc(sizeof(CssSnap) * MAXE);
    CssSnap *sb = (CssSnap *)malloc(sizeof(CssSnap) * MAXE);
    CHECK(sa && sb);

    for (u32 seed = 1; seed <= 220; seed++) {
        u32 st = seed * 2654435761u + 12345u;
        char sheet[4600]; u32 sp = 0;
        u32 nr = 4 + css_rng(&st) % 24;
        if (seed % 17 == 0) nr = 1;         /* 極小シート系 */
        for (u32 r = 0; r < nr && sp < sizeof(sheet) - 220; r++) {
            u32 comps = 1 + css_rng(&st) % 2;
            for (u32 c = 0; c < comps; c++) {
                u32 feat = css_rng(&st) % 5;
                switch (feat) {
                case 0: sp += (u32)sprintf(sheet + sp, "#i%u", css_rng(&st) % 4); break;
                case 1: sp += (u32)sprintf(sheet + sp, ".c%u", css_rng(&st) % 8); break;
                case 2: sp += (u32)sprintf(sheet + sp, "%s", tags[css_rng(&st) % 4]); break;
                case 3: sp += (u32)sprintf(sheet + sp, "%s.c%u", tags[css_rng(&st) % 4], css_rng(&st) % 8); break;
                default: sp += (u32)sprintf(sheet + sp, "*"); break;
                }
                if (c + 1 < comps)
                    sp += (u32)sprintf(sheet + sp, (css_rng(&st) & 1) ? ">" : " ");
            }
            if ((css_rng(&st) % 3) == 0) sp += (u32)sprintf(sheet + sp, ",.c%u", css_rng(&st) % 8); /* 複合リスト */
            u32 nd = 1 + css_rng(&st) % 3;
            sp += (u32)sprintf(sheet + sp, "{");
            for (u32 dd = 0; dd < nd; dd++) {
                u32 pi = css_rng(&st) % 5;
                switch (pi) {
                case 0: case 1:
                    sp += (u32)sprintf(sheet + sp, props[pi], css_rng(&st) % 256, css_rng(&st) % 256, css_rng(&st) % 256); break;
                case 2: sp += (u32)sprintf(sheet + sp, props[pi], (css_rng(&st) & 1) ? "bold" : "normal"); break;
                case 3: sp += (u32)sprintf(sheet + sp, props[pi], (css_rng(&st) & 1) ? "italic" : "normal"); break;
                default: sp += (u32)sprintf(sheet + sp, props[pi], (css_rng(&st) & 1) ? "underline" : "none"); break;
                }
                sp += (u32)sprintf(sheet + sp, ";");
            }
            sp += (u32)sprintf(sheet + sp, "}");
        }

        char dom[12288]; u32 dp = 0;
        dp += (u32)snprintf(dom + dp, sizeof(dom) - dp, "<style>%s</style>", sheet);
        CHECK(dp < sizeof(dom) - 512);
        u32 ne = 8 + css_rng(&st) % 28;
        u32 open = 0;
        for (u32 e = 0; e < ne && dp < sizeof(dom) - 160; e++) {
            const char *tg = tags[css_rng(&st) % 4];
            dp += (u32)sprintf(dom + dp, "<%s", tg);
            if ((css_rng(&st) % 3) == 0) dp += (u32)sprintf(dom + dp, " id=i%u", css_rng(&st) % 4);
            u32 nc = css_rng(&st) % 3;
            if (nc) {
                dp += (u32)sprintf(dom + dp, " class=\"");
                for (u32 k = 0; k < nc; k++)
                    dp += (u32)sprintf(dom + dp, "%sc%u", k ? " " : "", css_rng(&st) % 8);
                dp += (u32)sprintf(dom + dp, "\"");
            }
            dp += (u32)sprintf(dom + dp, ">x");
            open++;
            if (open > 1 && (css_rng(&st) % 2) == 0) { dp += (u32)sprintf(dom + dp, "</%s>", "div"); open--; } /* 近似クローズ */
        }
        /* 生成 HTML は恣意的に壊れ得るが、parser は敵対的入力に規律で応答する = oracle 対象として適格 */

        IfArena ha; if_arena_init(&ha, 1 << 20);
        IfDom *d = if_parse_html(&ha, if_str(dom, (u32)strlen(dom)));

        if_css_set_naive_matching(1);
        if_style_apply(&ha, d);
        u32 na = css_snapshot(d->root, sa, MAXE, 0);

        if_css_set_naive_matching(0);
        if_style_apply(&ha, d);
        u32 nb = css_snapshot(d->root, sb, MAXE, 0);

        CHECK(na == nb);
        for (u32 i = 0; i < na && i < nb; i++) {
            CHECK(sa[i].tag == sb[i].tag);
            CHECK(memcmp(&sa[i].st, &sb[i].st, sizeof(IfStyle)) == 0);
        }
        if_arena_destroy(&ha);
    }
    if_css_set_naive_matching(0); /* 後続スイートの既定を汚染しない */
    free(sa); free(sb);
}
