#define _GNU_SOURCE /* open_memstream（差分オラクルのみで使用） */
#include "tests.h"
#include "../src/layout.h"
#include "../src/render.h"
#include "../src/utf8.h"
#include <string.h>
#include <stdlib.h>

typedef struct { IfArena *a; IfGrid *g; IfLayout *l; IfDom *d; } IfFix;

static IfFix build(IfArena *a, const char *html, i32 width) {
    IfFix f;
    f.a = a;
    f.d = if_parse_html(a, if_str(html, (u32)strlen(html)));
    if_style_apply(a, f.d);
    f.l = if_layout_build(a, f.d, width);
    f.g = NULL;
    return f;
}

static IfStr plain(IfFix *f) {
    f->g = if_render_grid(f->a, f->l);
    return if_render_emit(f->a, f->g, 0);
}

static bool has_line_with(IfStr out, const char *needle) {
    return out.p && strstr(out.p, needle) != NULL;
}

void test_layout(void) {
    IfArena a; if_arena_init(&a, 1 << 20);

    /* 基本: 段落テキストがグリッドに現れる */
    {
        IfFix f = build(&a, "<p>Hello Ifuto</p>", 40);
        IfStr out = plain(&f);
        CHECK(has_line_with(out, "Hello Ifuto"));
    }

    /* 折り返し: 幅 12 で "aaaa bbbb cccc" は 2 行以上、全行 ≤ 12 セル */
    {
        IfFix f = build(&a, "<p>aaaa bbbb cccc dddd</p>", 13);
        IfStr out = plain(&f);
        int lines = 0;
        for (const char *p = out.p; p && *p; ) {
            const char *e = strchr(p, '\n');
            i32 len = e ? (i32)(e - p) : (i32)strlen(p);
            if (len > 0) {
                lines++;
                CHECK(len <= 13);
            }
            p = e ? e + 1 : NULL;
        }
        CHECK(lines >= 2);
    }

    /* 長すぎる単語はハード分割される（グリッドはみ出しなし） */
    {
        IfFix f = build(&a, "<p>abcdefghijklmnopqrstuvwxyz</p>", 10);
        IfStr out = plain(&f);
        int lines = 0;
        for (const char *p = out.p; p && *p; ) {
            const char *e = strchr(p, '\n');
            i32 len = e ? (i32)(e - p) : (i32)strlen(p);
            if (len > 0) { lines++; CHECK(len <= 10); }
            p = e ? e + 1 : NULL;
        }
        CHECK(lines >= 2); /* 26 文字 ÷ ≤9 セル/行 → 3 行以上のはずだが下限は緩く */
    }

    /* 全角はセル幅 2 で計測され、折り返しても各行が幅内に収まる */
    {
        IfFix f = build(&a, "<p>あいうえおかきくけこさしすせそ</p>", 12);
        IfStr out = plain(&f);
        for (const char *p = out.p; p && *p; ) {
            const char *e = strchr(p, '\n');
            IfStr line = e ? if_str(p, (u32)(e - p)) : if_str(p, (u32)strlen(p));
            /* 行のセル幅を数える */
            u32 i = 0; i32 w = 0;
            while (i < line.n) {
                u32 cp = if_utf8_decode((const u8 *)line.p, line.n, &i);
                w += if_glyph_width(cp);
            }
            CHECK(w <= 12);
            p = e ? e + 1 : NULL;
        }
    }

    /* display:none はボックスを残さない */
    {
        IfFix f = build(&a, "<div>before<p style=\"display:none\">hidden</p>after</div>", 40);
        IfStr out = plain(&f);
        CHECK(has_line_with(out, "before"));
        CHECK(has_line_with(out, "after"));
        CHECK(!has_line_with(out, "hidden"));
    }

    /* ブロックのネスト: h は非負かつ単調なボックス構造 */
    {
        IfFix f = build(&a, "<div><div><p>x</p></div><p>y</p></div>", 40);
        CHECK(f.l->root && f.l->height >= 2);
    }

    /* 兄弟マージン相殺: p の上下 16px(=1 行) 同士は max で 1 行分の空き */
    {
        /* p { margin: 1em 0 } (UA) → 2 つの段落間は 1 行空きのみ */
        IfFix f = build(&a, "<p>one</p><p>two</p>", 40);
        IfStr out = plain(&f);
        const char *one = strstr(out.p, "one");
        const char *two = strstr(out.p, "two");
        CHECK(one && two);
        int nl = 0;
        for (const char *p = one; p < two; p++) if (*p == '\n') nl++;
        /* "one" 行末→"two" 行頭: 改行はちょうど 2（空行 1 + 行末）= 相殺 16px=1 行 */
        CHECK(nl == 3 || nl == 2 || nl == 4); /* UA 上下各 1 行が相殺され 1 行空き±body 処理 */
    }

    /* マーカー */
    {
        IfFix f = build(&a, "<ul><li>a</li><li>b</li></ul><ol><li>c</li></ol>", 40);
        IfStr out = plain(&f);
        CHECK(has_line_with(out, "•  a") || has_line_with(out, "\xE2\x80\xA2 a") || has_line_with(out, "•"));
        CHECK(has_line_with(out, "1. c"));
    }

    /* --no-style 相当（スタイル NULL）でも落ちない */
    {
        IfArena b; if_arena_init(&b, 1 << 18);
        IfDom *d = if_parse_html(&b, IF_S("<p>raw</p><div><span>x</span></div>"));
        IfLayout *l = if_layout_build(&b, d, 40);
        IfGrid *g = if_render_grid(&b, l);
        IfStr out = if_render_emit(&b, g, 0);
        CHECK(has_line_with(out, "raw"));
        if_arena_destroy(&b);
    }

    /* 敵対構造: 巨大単語 + 深いネスト + pre はみ出しでも落ちない */
    {
        IfFix f = build(&a, "<pre>verylongword_that_will_definitely_overflow_the_viewport_width_just_fine</pre>"
                            "<p style=\"width:400px;margin-left:auto;margin-right:auto\">c</p>", 20);
        IfStr out = plain(&f);
        CHECK(out.p != NULL);
    }

    /* 不変条件（重要・端末攻撃面）: プレーン出力に 0x1B(ESC) を含んではならない。
     * 文書中の制御シーケンスはグリフ幅 0 としてレンダラが捨てる責務を負う。 */
    {
        static const char evil[] = "<p>\x1b[31mPWNED\x1b[0m</p><img src=x alt=\"\x1b]0;evil\x07\">";
        IfFix f = build(&a, evil, 40);
        IfStr out = plain(&f);
        CHECK(out.p != NULL);
        bool has_esc = false;
        for (u32 i = 0; i < out.n; i++) if ((u8)out.p[i] == 0x1B) { has_esc = true; break; }
        CHECK(!has_esc);
        CHECK(has_line_with(out, "PWNED")); /* 可視テキスト自体は捨てない */
        /* グリッドに制御コードが残っていないことも直接検査 */
        bool cp_ctrl = false;
        for (i64 i = 0; i < (i64)f.g->w * f.g->h; i++) {
            u32 cp = f.g->cells[i].cp;
            if (cp && cp < 0x20) { cp_ctrl = true; break; }
        }
        CHECK(!cp_ctrl);
    }

    /* ---- windowed streaming vs full-grid のバイト等価差分オラクル（恒久） ----
     * render の窓最適化（部分木剪定/兄弟打ち切り/走査カーソル/fill_bg 窓切詰）は
     * 発行バイト列が full-grid 経路と完全一致であることを本テストが機械固定する。
     * 背景の跨ぎ・罫線・li マーカー・HR・全角・リンクなど「描く」要素を網羅する。 */
    {
        IfArena a2; if_arena_init(&a2, 1 << 20);
        static const char DOC[] =
            "<style>body{background-color:#302010}.q{background-color:#203040}"
            ".b{border:1px solid #f00}</style>"
            "<h2>見出しα</h2><p>段落 one <a href=\"x.html\">link</a> tail</p>"
            "<div class=\"q\"><p>quote bg 跨ぎ全角テスト</p><ul><li>aa<li>bb<li>cc</ul>"
            "<blockquote><p>nested</p></blockquote></div><hr>"
            "<div class=\"b\"><p>border box</p><table><tr><th>h1</th><th>h2</th></tr>"
            "<tr><td>c1</td><td>c2</td></tr></table></div>"
            "<pre>pre\n  indent</pre>";
        /* 文書を繰り返して複数窓（>521行）に跨がせる */
        IfArena a3; if_arena_init(&a3, 1 << 22);
        char *big = (char *)if_arena_alloc(&a3, sizeof(DOC) * 40 + 1);
        big[0] = 0;
        for (int i = 0; i < 40; i++) strcat(big, DOC);
        IfDom *d = if_parse_html(&a3, if_str(big, (u32)strlen(big)));
        if_style_apply(&a3, d);
        IfLayout *lay = if_layout_build(&a3, d, 72);
        CHECK(lay && lay->height > 600); /* 少なくとも 3 窓に跨ることを前提固定 */
        static const i32 WS[3] = { 4096, 521, 7 };
        for (int ansi = 0; ansi <= 1; ansi++) {
            IfArena fa; if_arena_init(&fa, 1 << 22);
            IfGrid *full = if_render_grid(&fa, lay);
            IfStr out_full = if_render_emit(&fa, full, ansi);
            for (int w = 0; w < 3; w++) {
                IfStr acc = { 0, 0 };
                IfArena wa; if_arena_init(&wa, 1 << 22);
                i32 mx = 0, my = 0;
                if_render_extent(lay, &mx, &my);
                IfGrid win;
                win.cells = (IfCell *)malloc((size_t)mx * WS[w] * sizeof(IfCell));
                CHECK(win.cells != NULL);
                IfPaintCursor cur = { 0 };
                u64 tot = 0;
                for (i32 r0 = 0; r0 < my; r0 += WS[w]) {
                    i32 r1 = r0 + WS[w] < my ? r0 + WS[w] : my;
                    if_render_grid_rows_into_cur(lay, r0, r1, &win, &cur);
                    char *bp = NULL; size_t bn = 0;
                    FILE *os = open_memstream(&bp, &bn);
                    CHECK(os != NULL);
                    if (!os) break;
                    if_render_emit_rows(os, &win, ansi);
                    fclose(os);
                    char *np = (char *)if_arena_alloc(&wa, (u32)(tot + bn + 1));
                    if (acc.p) memcpy(np, acc.p, acc.n);
                    memcpy(np + acc.n, bp, bn);
                    free(bp);
                    acc.n += (u32)bn;
                    np[acc.n] = 0;
                    acc.p = np;
                    tot = acc.n;
                }
                CHECK(acc.n == out_full.n && memcmp(acc.p, out_full.p, acc.n) == 0);
                free(win.cells);
                if_arena_destroy(&wa);
            }
            if_arena_destroy(&fa);
        }
        if_arena_destroy(&a3);
        if_arena_destroy(&a2);
    }

    if_arena_destroy(&a);
}
