/* <script> akl 実行配線（src/script.c）の in-process オラクル。
 * 実 DOM に対し if_script_run を直接呼び、DOM 変更・失敗隔離・スキップ規則・
 * kill switch・走査スイッチ（has_script）・console 出力行を検査する。
 * 大域の殺し方: 失敗は全部明白に数える（実ブラウザで曖昧に落ちるのは最悪手）。 */
#define _GNU_SOURCE
#include "tests.h"
#include "../src/dom.h"
#include "../src/script.h"
#include "../src/arena.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* parse → run → teardown の定型。log は open_memstream で捕捉（行規約のオラクル）。 */
typedef struct {
    IfArena a;
    IfDom *dom;
    IfScriptReport rep;
    char *logbuf;
    size_t logn;
    FILE *logf;
} TScr;

static void tscr_begin(TScr *t, const char *html) {
    if_arena_init(&t->a, 1 << 20);
    t->dom = if_parse_html(&t->a, if_str(html, (u32)strlen(html)));
    CHECK(t->dom != NULL);
    t->logbuf = NULL; t->logn = 0;
    t->logf = open_memstream(&t->logbuf, &t->logn);
    CHECK(t->logf != NULL);
}
static void tscr_run(TScr *t) {
    t->rep = if_script_run(&t->a, t->dom, t->logf);
    fclose(t->logf); t->logf = NULL;
}
static void tscr_end(TScr *t) {
    free(t->logbuf);
    if_arena_destroy(&t->a);
}

static void test_script_mutation(void) {
    TScr t;
    tscr_begin(&t, "<!DOCTYPE html><title>T0</title><div id=a>Hello</div>"
                   "<script>"
                   "var d=document.getElementById('a');"
                   "d.textContent='CHANGED';"
                   "document.title='T1';"
                   "console.log('tc', 1+2, (''+document.title));"
                   "</script>");
    CHECK(t.dom->has_script == 1);
    tscr_run(&t);
    CHECK(t.rep.n_run == 1 && t.rep.n_errors == 0 && t.rep.n_skipped == 0);
    IfNode *d = if_dom_find_by_id(t.dom, if_str("a", 1));
    CHECK(d != NULL);
    IfStr txt = if_dom_text_content(&t.a, d);
    CHECK(txt.n == 7 && memcmp(txt.p, "CHANGED", 7) == 0); /* 描画そのものが変わる */
    CHECK(t.dom->title.n == 2 && memcmp(t.dom->title.p, "T1", 2) == 0);
    CHECK(strstr(t.logbuf, "[script:console] tc 3 T1\n") != NULL); /* 1 行・空白結合規約 */
    tscr_end(&t);
}

static void test_script_failure_isolation(void) {
    TScr t;
    tscr_begin(&t, "<div id=a>X</div>"
                   "<script>var g=41; broken syntax !!!</script>"
                   "<script>document.getElementById('a').textContent='ok';</script>"
                   "<script>while(1){}</script>"
                   "<script>console.log('alive');</script>");
    tscr_run(&t);
    CHECK(t.rep.n_run == 4);
    CHECK(t.rep.n_errors == 2); /* syntax 1 + budget 1（失敗 script の var も無効 = 本家 spec 同型） */
    IfNode *d = if_dom_find_by_id(t.dom, if_str("a", 1));
    IfStr txt = if_dom_text_content(&t.a, d);
    CHECK(txt.n == 2 && memcmp(txt.p, "ok", 2) == 0); /* 失敗後も後続は走る */
    CHECK(strstr(t.logbuf, "[script] FAILED: SyntaxError:") != NULL);
    CHECK(strstr(t.logbuf, "[script] FAILED: instruction budget exhausted") != NULL);
    CHECK(strstr(t.logbuf, "alive") != NULL);
    /* FAILED 行は 1 行規約（改行畳み）: 行頭 "[script]" で始まる行以外に script 由来の行がない */
    for (char *p = t.logbuf; p && *p;) {
        char *e = strchr(p, '\n');
        CHECK(strncmp(p, "[script", 7) == 0);
        if (!e) break;
        p = e + 1;
    }
    tscr_end(&t);
}

static void test_script_skip_rules(void) {
    TScr t;
    tscr_begin(&t, "<div id=a>X</div>"
                   "<script src=\"http://example.invalid/x.js\">var bad=1;</script>"
                   "<script type=\"text/vbscript\">vb junk</script>"
                   "<script type=\"module\">console.log('mod');</script>" /* type!=text/javascript は明白スキップ */
                   "<script></script>"
                   "<script type=\"TEXT/JAVASCRIPT\">document.getElementById('a').textContent='run';</script>");
    tscr_run(&t);
    CHECK(t.rep.n_run == 1);      /* type 値の大文字小文字は無視（HTML 規則） */
    CHECK(t.rep.n_skipped == 4);  /* src / vbscript / module / 空 */
    CHECK(t.rep.n_errors == 0);
    IfNode *d = if_dom_find_by_id(t.dom, if_str("a", 1));
    IfStr txt = if_dom_text_content(&t.a, d);
    CHECK(txt.n == 3 && memcmp(txt.p, "run", 3) == 0);
    tscr_end(&t);
}

static void test_script_kill_and_zero_cost(void) {
    TScr t;
    /* IF_SCRIPT=0: 完全 no-op（DOM 不変・report 全零） */
    setenv("IF_SCRIPT", "0", 1);
    tscr_begin(&t, "<div id=a>Keep</div><script>document.getElementById('a').textContent='BAD';</script>");
    tscr_run(&t);
    unsetenv("IF_SCRIPT");
    CHECK(t.rep.n_run == 0 && t.rep.n_errors == 0 && t.rep.n_skipped == 0);
    IfNode *d = if_dom_find_by_id(t.dom, if_str("a", 1));
    IfStr txt = if_dom_text_content(&t.a, d);
    CHECK(txt.n == 4 && memcmp(txt.p, "Keep", 4) == 0);
    CHECK(t.logn == 0); /* ログにも出さない（完全 no-op） */
    tscr_end(&t);

    /* has_script=0 の文書は走査スイッチで即リターン（走査コスト構造的ゼロ） */
    tscr_begin(&t, "<div id=a>NoScripts</div>");
    CHECK(t.dom->has_script == 0);
    tscr_run(&t);
    CHECK(t.rep.n_run == 0 && t.logn == 0);
    tscr_end(&t);
}

static void test_script_svg_and_document_shape(void) {
    TScr t;
    /* svg 内 script は対象外（HTML ns のみ）。document の形も同時に検査。 */
    tscr_begin(&t, "<svg><script>document.title='SVGSET'</script></svg>"
                   "<script>"
                   "console.log('t='+(''+document.title));"       /* 取得: trim 済み title */
                   "console.log('u='+(''+document.nosuchprop));"    /* unknown prop → undefined */
                   "console.log('b='+(''+document.body));"          /* HANDLE tostring 規約 */
                   "document.body.textContent='BC';"                 /* body 経由の書換 */
                   "</script>");
    tscr_run(&t);
    CHECK(t.rep.n_run == 1); /* svg script は実行もエラーもしない（収集対象外） */
    CHECK(t.rep.n_errors == 0);
    IfNode *body = if_dom_find_tag_dfs(t.dom, IF_TAG_BODY);
    CHECK(body != NULL);
    IfStr bt = if_dom_text_content(&t.a, body);
    CHECK(bt.n == 2 && memcmp(bt.p, "BC", 2) == 0);
    CHECK(strstr(t.logbuf, "u=undefined") != NULL);
    CHECK(strstr(t.logbuf, "b=[object HTMLElement]") != NULL);
    CHECK(t.dom->title.n == 0); /* svg script の書換は無効 title 不変 */
    tscr_end(&t);
}

static void test_script_textcontent_and_globals(void) {
    TScr t;
    /* textContent: ToString 経由・子孫連結取得・script 間グローバル共有（1 RT 意味論） */
    tscr_begin(&t, "<div id=a>ab<span>cd</span>ef</div><div id=b></div>"
                   "<script>var got=document.getElementById('a').textContent;</script>"
                   "<script>document.getElementById('b').textContent=got+('_'+7);</script>");
    tscr_run(&t);
    CHECK(t.rep.n_run == 2 && t.rep.n_errors == 0);
    IfNode *b = if_dom_find_by_id(t.dom, if_str("b", 1));
    IfStr bt = if_dom_text_content(&t.a, b);
    CHECK(bt.n == 8 && memcmp(bt.p, "abcdef_7", 8) == 0);
    tscr_end(&t);
}

static void test_script_count_cap(void) {
    /* 上限 128: 超過分は打切り + 明白な 1 行警告（母数も正確に） */
    char buf[64 * 140];
    char *w = buf;
    w += sprintf(w, "<div id=a>X</div>");
    for (int i = 0; i < 140; i++) w += sprintf(w, "<script>var v%d=%d;</script>", i, i);
    TScr t;
    tscr_begin(&t, buf);
    tscr_run(&t);
    CHECK(t.rep.n_run == 128);
    CHECK(t.rep.n_errors == 0);
    CHECK(strstr(t.logbuf, "script count truncated at 128 (found 140)") != NULL);
    tscr_end(&t);
}

static void test_script_gc_churn(void) {
    /* 敵対: ハンドル（elem/document の IfNode* 値）を生かしたまま akl GC を強制発火。
     * GC 発火の機械証明: 300k iter × (ROPE+STR ≈123B) ≈ 37MB の garbage を
     * 16MB 既定ヒープに流す → GC 不発火なら heap budget で eval 失敗する（成功 ≡
     * GC 複数回発火 + globals ルート経由で handle オブジェクト生存 + IfNode* ptr
     * （GC 非管理）が一貫有効）。insn≈3M < 既定 budget 10M、live は微小。 */
    TScr t;
    tscr_begin(&t, "<div id=a>X</div>"
                   "<script>"
                   "var d = document.getElementById('a');"
                   "for (var i = 0; i < 300000; i = i + 1) { var g = 'garbage-garbage-garbage' + i; }"
                   "d.textContent = 'post-gc';"
                   "document.title = 'gc-ok';"
                   "</script>");
    tscr_run(&t);
    CHECK(t.rep.n_run == 1 && t.rep.n_errors == 0);
    IfNode *d = if_dom_find_by_id(t.dom, if_str("a", 1));
    IfStr txt = if_dom_text_content(&t.a, d);
    CHECK(txt.n == 7 && memcmp(txt.p, "post-gc", 7) == 0);
    CHECK(t.dom->title.n == 5 && memcmp(t.dom->title.p, "gc-ok", 5) == 0);
    tscr_end(&t);
}

void test_script(void) {
    test_script_mutation();
    test_script_gc_churn();
    test_script_failure_isolation();
    test_script_skip_rules();
    test_script_kill_and_zero_cost();
    test_script_svg_and_document_shape();
    test_script_textcontent_and_globals();
    test_script_count_cap();
}
