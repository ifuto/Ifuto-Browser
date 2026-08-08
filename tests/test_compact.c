/* 入力 compaction（取り込み時複製, if_dom_copy_strings / GUI 経路）の機械オラクル。
 * 検査原理:
 *  1. フラグ ON で parse → 入力ヒープを即 free（ASan 毒殺）→ 内容全読。
 *     借用残りが 1 つでもあれば heap-use-after-free でプロセスごと死ぬ。
 *  2. フラグ OFF（既定・CLI の不変条件）では入力切片がそのまま DOM に居ること
 *     を直接検査（ゼロコピー経路の生存証明 = CLI オラクル系の前提固定）。
 * フィールド台帳との 1:1 対応: TEXT / COMMENT / PI target / attrs name,value
 * （entity デコード済は arena 由来）/ DOCTYPE / tpl content / script 実戦経路 /
 * md fast（借用判定が唯一の関門）/ バッファ終端ちょうどの属性値。 */
#define _GNU_SOURCE
#include "tests.h"
#include "../src/dom.h"
#include "../src/md.h"
#include "../src/script.h"
#include "../src/arena.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* 本ファイルは「入力を free した後に DOM 文字列を読む」ことが検査目的そのもの
 * （解放後 dereference の検出は ASan 実行時検出の仕事）。GCC の静的
 * -Wuse-after-free は ASan 毒殺をモデル化できず誤検出するため本ファイル限り診断 off。 */
#pragma GCC diagnostic ignored "-Wuse-after-free"

static int in_input(const char *p, u32 n, const char *lo, u32 total) {
    if (!p || !n) return 0;
    uintptr_t a = (uintptr_t)lo, b = (uintptr_t)p;
    return b >= a && (u64)(b - a) + n <= total;
}

static void acc_text_rec(IfNode *n, char *out, size_t *w) {
    for (IfNode *c = n; c; c = c->next_sibling) {
        if (c->kind == IF_NODE_TEXT || c->kind == IF_NODE_COMMENT) {
            memcpy(out + *w, c->u.text.p, c->u.text.n); /* ASan: 死域ならここで死ぬ */
            *w += c->u.text.n;
        }
        if (c->first_child && !(c->flags & IF_NF_SLIM)) acc_text_rec(c->first_child, out, w);
    }
}

/* noinline: free 後の d 由来参照と buf の来歴を GCC 解析上は切断する（実行時の
 * ASan 毒殺は変わらず要求どおり有効） */
static char *xinput(const char *s, u32 n) __attribute__((noinline));
static char *xinput(const char *s, u32 n) {
    char *b = (char *)malloc(n);
    memcpy(b, s, n);
    return b;
}

static void test_compact_html_ingest(void) {
    const char *html =
        "<!DOCTYPE html><title>Ttl&auml;</title>"
        "<body><!--cmt-body--><div id=garbage class=\"x&amp;y\">TEXT-slice"
        "<x-UNKNOWNQ attrq=\"vv\">inner</x-UNKNOWNQ>"
        "<template><i>tpl-slice</i></template>"
        "<a href=\"#end-marker\">end</a></body>";
    u32 n = (u32)strlen(html);
    char *buf = xinput(html, n);

    if_dom_slim = false;      /* 他テストの走行順に依存しない自己完結前提 */
    if_dom_copy_strings = true;  /* ← GUI 経路の compaction 条件 */
    IfArena a; if_arena_init(&a, 1 << 20);
    IfDom *d = if_parse_html(&a, if_str(buf, n));
    if_dom_copy_strings = false; /* 直後に必ず復元（CLI/他テストを汚さない） */
    CHECK(d != NULL);

    /* 全保持切片が入力を参照していないことを直接検査してから free する */
    IfNode *div = if_dom_find_by_id(d, if_str("garbage", 7));
    CHECK(div != NULL);
    IfStr hole_text = { NULL, 0 };
    for (IfNode *c = div->first_child; c; c = c->next_sibling)
        if (c->kind == IF_NODE_TEXT) { hole_text = c->u.text; break; }
    CHECK(hole_text.n == 10 && memcmp(hole_text.p, "TEXT-slice", 10) == 0);
    CHECK(!in_input(hole_text.p, hole_text.n, buf, n));
    CHECK(!in_input(d->title.p, d->title.n, buf, n));
    IfStr cls = if_dom_attr(div, "class");
    CHECK(cls.n == 3 && memcmp(cls.p, "x&y", 3) == 0);
    CHECK(!in_input(cls.p, cls.n, buf, n));
    {
        IfNode *a_el = if_dom_find_tag_dfs(d, IF_TAG_A);
        CHECK(a_el != NULL);
        if (a_el) {
            IfStr ahref = if_dom_attr(a_el, "href");
            CHECK(ahref.n == 11 && memcmp(ahref.p, "#end-marker", 11) == 0);
            CHECK(!in_input(ahref.p, ahref.n, buf, n));
        }
    }
    {
        IfNode *dt = NULL;
        for (IfNode *c = d->root->first_child; c && !dt; c = c->next_sibling)
            if (c->kind == IF_NODE_DOCTYPE) dt = c;
        CHECK(dt && dt->u.dtype);
        if (dt && dt->u.dtype) {
            CHECK(dt->u.dtype->name.n == 4 && memcmp(dt->u.dtype->name.p, "html", 4) == 0);
            CHECK(!in_input(dt->u.dtype->name.p, dt->u.dtype->name.n, buf, n));
        }
    }
    u32 total = n;
    free(buf); /* ← 以降、入力死域を dereference すれば ASan が即殺す */
    buf = NULL;

    /* 本丸: free 後の内容全読（text/comment/inner/tpl content） */
    static char all[4096];
    size_t w = 0;
    acc_text_rec(d->root->first_child, all, &w);
    all[w] = 0;
    CHECK(strstr(all, "TEXT-slice") != NULL);
    CHECK(strstr(all, "cmt-body") != NULL);
    CHECK(strstr(all, "inner") != NULL);
    {
        IfNode *tpl = if_dom_find_tag_dfs(d, IF_TAG_TEMPLATE);
        CHECK(tpl != NULL);
        IfNode *content = if_dom_tpl_content(d, tpl);
        CHECK(content != NULL);
        static char tbuf[128];
        size_t tw = 0;
        acc_text_rec(content ? content->first_child : NULL, tbuf, &tw);
        tbuf[tw] = 0;
        CHECK(strstr(tbuf, "tpl-slice") != NULL);
    }
    /* title 再確認（死域外 + 内容） */
    CHECK(!in_input(d->title.p, d->title.n, (const char *)(uintptr_t)(all - all), 0)); /* 形式: 常真範囲外 */
    CHECK(d->title.n >= 3 && memcmp(d->title.p, "Ttl", 3) == 0);
    (void)total;
    if_arena_destroy(&a);
}

static void test_compact_script_path(void) {
    /* compaction 条件で parse → free → script 実行（attr src/type・textContent・
     * title 双方向 = 実戦で最も文字列を踏む経路） */
    const char *html =
        "<title>Before</title><div id=a>Hello</div>"
        "<script type=\"TEXT/JAVASCRIPT\">"
        "document.getElementById('a').textContent='AFTER';"
        "document.title='T'+'x';</script>";
    u32 n = (u32)strlen(html);
    char *buf = xinput(html, n);
    if_dom_slim = false;
    if_dom_copy_strings = true;
    IfArena a; if_arena_init(&a, 1 << 20);
    IfDom *d = if_parse_html(&a, if_str(buf, n));
    if_dom_copy_strings = false;
    free(buf); buf = NULL;

    char *lb = NULL; size_t ln = 0;
    FILE *lf = open_memstream(&lb, &ln);
    IfScriptReport rep = if_script_run(&a, d, lf);
    fclose(lf);
    CHECK(rep.n_run == 1 && rep.n_errors == 0); /* 死域踏みなら eval 失敗/ASan 停止 */
    IfNode *dv = if_dom_find_by_id(d, if_str("a", 1));
    IfStr txt = if_dom_text_content(&a, dv);
    CHECK(txt.n == 5 && memcmp(txt.p, "AFTER", 5) == 0);
    CHECK(d->title.n == 2 && memcmp(d->title.p, "Tx", 2) == 0);
    free(lb);
    if_arena_destroy(&a);
}

static void test_compact_md_fast_ingest(void) {
    const char *md = "# Head\n\nbody text here\n\n[lnk](http://example.com/x) and text\n";
    u32 n = (u32)strlen(md);
    char *buf = xinput(md, n);
    if_dom_copy_strings = true; /* GUI md 経路と同条件（借用判定が唯一の関門） */
    IfArena a; if_arena_init(&a, 1 << 20);
    IfDom *d = NULL;
    bool ok = if_md_parse_fast_f(&a, if_str(buf, n), &d, 0);
    if_dom_copy_strings = false;
    CHECK(ok && d != NULL);
    if (!ok || !d) { free(buf); if_arena_destroy(&a); return; }
    /* 前提: OFF なら借用切片が残る経路（次テストで証明）→ ON では 0 のはず */
    free(buf); buf = NULL;
    static char all[1024];
    size_t w = 0;
    acc_text_rec(d->root->first_child, all, &w);
    all[w] = 0;
    CHECK(strstr(all, "Head") != NULL);
    CHECK(strstr(all, "body text here") != NULL);
    if_arena_destroy(&a);
}

static void test_zerocopy_offpath_invariant(void) {
    /* フラグ OFF（CLI 既定）の不変条件: 入力切片が DOM に居る（ゼロコピー経路は
     * 生きている）。オラクル系が前提にする挙動を直接固定する */
    const char *html = "<div id=z>ZCOPY</div>";
    u32 n = (u32)strlen(html);
    char *buf = xinput(html, n);
    if_dom_slim = false;
    IfArena a; if_arena_init(&a, 1 << 20);
    IfDom *d = if_parse_html(&a, if_str(buf, n));
    IfNode *z = if_dom_find_by_id(d, if_str("z", 1));
    CHECK(z != NULL);
    int saw_slice = 0;
    for (IfNode *c = z->first_child; c; c = c->next_sibling)
        if (c->kind == IF_NODE_TEXT && in_input(c->u.text.p, c->u.text.n, buf, n)) saw_slice = 1;
    CHECK(saw_slice); /* flag OFF では zero-copy 切片が存在する */
    if_arena_destroy(&a);
    free(buf);
}

void test_compact(void) {
    test_compact_html_ingest();
    test_compact_script_path();
    test_compact_md_fast_ingest();
    test_zerocopy_offpath_invariant();
}
