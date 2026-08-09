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

static void test_script_dom_binding_v03(void);
static void test_script_selector_edge(void);

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


static void test_script_regex(void) {
    TScr t;
    tscr_begin(&t, "<!DOCTYPE html><title>R0</title><div id=a>start</div>"
                   "<script>"
                   "var s = 'hello 42 world';"
                   "console.log('rx1', /\\d+/.exec(s)[0]);"
                   "console.log('rx2', s.replace(/\\w+/g, function(w){ return w.toUpperCase(); }));"
                   "console.log('rx3', 'a,b,c'.split(/,/).length);"
                   "console.log('rx4', 'user@example.com'.replace(/^(.+)@(.+)$/, '$2.$1'));"
                   "document.getElementById('a').textContent = /42/.test(s) ? 'yes' : 'no';"
                   "</script>");
    tscr_run(&t);
    CHECK(t.rep.n_run == 1 && t.rep.n_errors == 0 && t.rep.n_skipped == 0);
    CHECK(strstr(t.logbuf, "[script:console] rx1 42\n") != NULL);
    CHECK(strstr(t.logbuf, "[script:console] rx2 HELLO 42 WORLD\n") != NULL);
    CHECK(strstr(t.logbuf, "[script:console] rx3 3\n") != NULL);
    CHECK(strstr(t.logbuf, "[script:console] rx4 example.com.user\n") != NULL);
    IfNode *d = if_dom_find_by_id(t.dom, if_str("a", 1));
    CHECK(d != NULL);
    IfStr txt = if_dom_text_content(&t.a, d);
    CHECK(txt.n == 3 && memcmp(txt.p, "yes", 3) == 0);
    tscr_end(&t);
}


static void test_script_class_extends(void) {
    TScr t;
    tscr_begin(&t, "<!DOCTYPE html><title>E0</title><div id=a>start</div>"
                   "<script>"
                   "class Shape { constructor(w, h) { this.w = w; this.h = h; } area() { return this.w * this.h; } }"
                   "class Rect extends Shape { constructor(w, h) { super(w, h); } describe() { return 'rect:' + this.area(); } }"
                   "class Square extends Rect { constructor(s) { super(s, s); } }"
                   "var sq = new Square(5);"
                   "console.log('ex1', sq.area());"
                   "document.getElementById('a').textContent = sq.describe();"
                   "</script>");
    tscr_run(&t);
    CHECK(t.rep.n_run == 1 && t.rep.n_errors == 0 && t.rep.n_skipped == 0);
    CHECK(strstr(t.logbuf, "[script:console] ex1 25\n") != NULL);
    IfNode *d = if_dom_find_by_id(t.dom, if_str("a", 1));
    CHECK(d != NULL);
    IfStr txt = if_dom_text_content(&t.a, d);
    CHECK(txt.n == 7 && memcmp(txt.p, "rect:25", 7) == 0);
    tscr_end(&t);
}


static void test_script_spread_rest(void) {
    TScr t;
    tscr_begin(&t, "<!DOCTYPE html><title>S0</title><div id=a>start</div>"
                   "<script>"
                   "var base = {w: 100, h: 50};"
                   "var opts = {...base, h: 60};"
                   "console.log('sp1', opts.w + opts.h);"
                   "var o = {sum: function(a, b, c) { return a + b + c; }};"
                   "var args = [1, 2];"
                   "console.log('sp2', o.sum(...args, 3));"
                   "var rest;"
                   "[rest] = [9, 8, 7];"
                   "var rest2;"
                   "var {first, ...rest2} = {first: 1, second: 2, third: 3};"
                   "console.log('sp3', rest2.second + rest2.third);"
                   "document.getElementById('a').textContent = rest + ':' + first;"
                   "</script>");
    tscr_run(&t);
    CHECK(t.rep.n_run == 1 && t.rep.n_errors == 0 && t.rep.n_skipped == 0);
    CHECK(strstr(t.logbuf, "[script:console] sp1 160\n") != NULL);
    CHECK(strstr(t.logbuf, "[script:console] sp2 6\n") != NULL);
    CHECK(strstr(t.logbuf, "[script:console] sp3 5\n") != NULL);
    IfNode *d = if_dom_find_by_id(t.dom, if_str("a", 1));
    CHECK(d != NULL);
    IfStr txt = if_dom_text_content(&t.a, d);
    CHECK(txt.n == 3 && memcmp(txt.p, "9:1", 3) == 0);
    tscr_end(&t);
}


static void test_script_fields_length(void) {
    TScr t;
    tscr_begin(&t, "<!DOCTYPE html><title>F0</title><div id=a>start</div>"
                   "<script>"
                   "class Item { name = 'unnamed'; count = 0; }"
                   "var it = new Item();"
                   "it.name = 'pen'; it.count = 3;"
                   "console.log('fl1', it.name + ':' + it.count);"
                   "var arr = [1, 2, 3, 4];"
                   "arr.length = 2;"
                   "console.log('fl2', arr.length);"
                   "document.getElementById('a').textContent = it.name + arr.length;"
                   "</script>");
    tscr_run(&t);
    CHECK(t.rep.n_run == 1 && t.rep.n_errors == 0 && t.rep.n_skipped == 0);
    CHECK(strstr(t.logbuf, "[script:console] fl1 pen:3\n") != NULL);
    CHECK(strstr(t.logbuf, "[script:console] fl2 2\n") != NULL);
    IfNode *d = if_dom_find_by_id(t.dom, if_str("a", 1));
    CHECK(d != NULL);
    IfStr txt = if_dom_text_content(&t.a, d);
    CHECK(txt.n == 4 && memcmp(txt.p, "pen2", 4) == 0);
    tscr_end(&t);
}


static void test_script_objlit_ext(void) {
    TScr t;
    tscr_begin(&t, "<!DOCTYPE html><title>X0</title><div id=a>start</div>"
                   "<script>"
                   "var cfg = {theme: 'dark', count: 0};"
                   "cfg.count ||= 5;"
                   "cfg.missing ?\?= 'default';"
                   "var key = 'extra';"
                   "cfg[key] = 42;"
                   "var math = { value: 10, get double() { return this.value * 2; }, set double(v) { this.value = v / 2; } };"
                   "math.double = 20;"
                   "console.log('ol1', cfg.theme + cfg.count + cfg.missing);"
                   "console.log('ol2', cfg.extra);"
                   "console.log('ol3', math.double + ':' + math.value);"
                   "document.getElementById('a').textContent = cfg.count + ':' + math.double;"
                   "</script>");
    tscr_run(&t);
    CHECK(t.rep.n_run == 1 && t.rep.n_errors == 0 && t.rep.n_skipped == 0);
    CHECK(strstr(t.logbuf, "[script:console] ol1 dark5default\n") != NULL);
    CHECK(strstr(t.logbuf, "[script:console] ol2 42\n") != NULL);
    CHECK(strstr(t.logbuf, "[script:console] ol3 20:10\n") != NULL);
    IfNode *d = if_dom_find_by_id(t.dom, if_str("a", 1));
    CHECK(d != NULL);
    IfStr txt = if_dom_text_content(&t.a, d);
    CHECK(txt.n == 4 && memcmp(txt.p, "5:20", 4) == 0);
    tscr_end(&t);
}

void test_script(void) {
    test_script_objlit_ext();
    test_script_fields_length();
    test_script_spread_rest();
    test_script_class_extends();
    test_script_regex();
    test_script_mutation();
    test_script_gc_churn();
    test_script_failure_isolation();
    test_script_skip_rules();
    test_script_kill_and_zero_cost();
    test_script_svg_and_document_shape();
    test_script_textcontent_and_globals();
    test_script_count_cap();
    test_script_dom_binding_v03();
    test_script_selector_edge();
}

static void test_script_dom_binding_v03(void) {
    /* v0.3: querySelector / getElementsByTagName / getAttribute / setAttribute / style */
    TScr t;
    tscr_begin(&t, "<div id=main class=content><p class=first>Hello</p><p>World</p></div>"
                   "<script>"
                   "var el = document.querySelector('#main p.first');"
                   "document.title = 'qs:' + el.textContent;"
                   "var ps = document.getElementsByTagName('p');"
                   "document.title = document.title + ':' + ps.length;"
                   "var p0 = document.querySelector('p');"
                   "document.title = document.title + ':' + p0.getAttribute('class');"
                   "p0.setAttribute('data-x', '42');"
                   "document.title = document.title + ':' + p0.getAttribute('data-x');"
                   "var st = document.querySelector('p').style;"
                   "st.color = 'blue';"
                   "document.title = document.title + ':' + st.color;"
                   "</script>");
    tscr_run(&t);
    CHECK(t.rep.n_run == 1 && t.rep.n_errors == 0);
    CHECK(t.dom->title.n == 24 && memcmp(t.dom->title.p, "qs:Hello:2:first:42:blue", 24) == 0);
    /* setAttribute が DOM に反映 */
    IfNode *p0 = if_dom_query_selector(t.dom, "p");
    CHECK(p0 != NULL);
    IfStr dv = if_dom_attr(p0, "data-x");
    CHECK(dv.n == 2 && memcmp(dv.p, "42", 2) == 0);
    /* style 属性が "color:blue" を含む */
    IfStr sv = if_dom_attr(p0, "style");
    CHECK(sv.n >= 10 && memmem(sv.p, sv.n, "color:blue", 10) != NULL);
    tscr_end(&t);
}

static void test_script_selector_edge(void) {
    /* セレクタの境界: 非対応形は null（明示拒否）、子孫は祖先を辿る */
    TScr t;
    tscr_begin(&t, "<div id=a><section><p class=x>deep</p></section></div>"
                   "<script>"
                   "var a = document.querySelector('div section p');"
                   "var b = document.querySelector('div > p');"      /* > は非対応 → null */
                   "var c = document.querySelector('.x');"
                   "var d = document.querySelector('p.x');"
                   "document.title = (a ? a.textContent : 'na') + ':' + (b ? 'b' : 'nb')"
                   "             + ':' + (c ? c.textContent : 'nc') + ':' + (d ? 'd' : 'nd');"
                   "</script>");
    tscr_run(&t);
    CHECK(t.rep.n_run == 1 && t.rep.n_errors == 0);
    CHECK(t.dom->title.n == 14 && memcmp(t.dom->title.p, "deep:nb:deep:d", 14) == 0);
    tscr_end(&t);
}


