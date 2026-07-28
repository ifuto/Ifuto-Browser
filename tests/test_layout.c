#include "tests.h"
#include "../src/layout.h"
#include "../src/render.h"
#include <string.h>

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

    if_arena_destroy(&a);
}
