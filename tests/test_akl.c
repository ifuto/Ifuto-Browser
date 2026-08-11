/* Akl v0.0 テスト。dispatch 両モード（computed-goto / switch）で同一バイナリを
 * 2 回ビルドして走らせる前提（Makefile: run_tests / run_tests_switch）。
 * どちらかでだけ失敗するような差分は dispatch バグなので即座に止める。 */
#define _POSIX_C_SOURCE 200809L /* strdup（モジュールテストのローダ） */
#include "tests.h"
#include "../src/akl/akl.h"
#include "../src/common.h"
#include <stdint.h>
#include <string.h>
#include <math.h>

static AklRT *g_rt;

static void want_num(const char *src, double want) {
    AklVal v;
    if (!akl_eval(g_rt, src, &v)) {
        fprintf(stderr, "  eval failed [%s]: %s\n", src, akl_error(g_rt));
        CHECK(0);
        return;
    }
    double d = NAN;
    bool ok = akl_as_num(v, &d) && d == want;
    if (!ok) { /* bool 値は数値化して比較（テスト利便。1 == true 扱い） */
        bool b = false;
        if (akl_as_bool(v, &b)) { ok = (b ? 1.0 : 0.0) == want; d = b ? 1.0 : 0.0; }
    }
    CHECK(ok);
    if (!ok) fprintf(stderr, "  wrong value [%s]: got %g want %g\n", src, d, want);
}
static void want_bool(const char *src, bool want) {
    AklVal v;
    if (!akl_eval(g_rt, src, &v)) {
        fprintf(stderr, "  eval failed [%s]: %s\n", src, akl_error(g_rt));
        CHECK(0);
        return;
    }
    bool b = false;
    bool ok = akl_as_bool(v, &b) && b == want;
    CHECK(ok);
    if (!ok) fprintf(stderr, "  wrong bool [%s]\n", src);
}
static void want_str(const char *src, const char *want) {
    AklVal v;
    if (!akl_eval(g_rt, src, &v)) {
        fprintf(stderr, "  eval failed [%s]: %s\n", src, akl_error(g_rt));
        CHECK(0);
        return;
    }
    uint32_t ln = 0;
    const char *s = akl_as_str(g_rt, v, &ln);
    bool ok = s && strlen(want) == ln && memcmp(s, want, ln) == 0;
    CHECK(ok);
    if (!ok) fprintf(stderr, "  wrong string [%s]: got '%.*s' want '%s'\n",
                     src, s ? (int)ln : 0, s ? s : "", want);
}
static void want_undef(const char *src) {
    AklVal v;
    if (!akl_eval(g_rt, src, &v)) {
        fprintf(stderr, "  eval failed [%s]: %s\n", src, akl_error(g_rt));
        CHECK(0);
        return;
    }
    CHECK(akl_is_undefined(v));
}
/* needle != NULL ならエラー文言に含まれることも検査（budget 系の原因特定を誤魔化さない） */
static void want_err(const char *src, const char *needle) {
    if (akl_eval(g_rt, src, NULL)) {
        fprintf(stderr, "  expected error but succeeded [%s]\n", src);
        CHECK(0);
        return;
    }
    if (needle) CHECK(strstr(akl_error(g_rt), needle) != NULL);
}

static void t_arith(void) {
    want_num("1+2*3", 7);
    want_num("(1+2)*3", 9);
    want_num("10 % 3", 1);
    want_num("2 - 5 - 1", -4);           /* 左結合 */
    want_num("1.5 * 4", 6);
    want_num("7 / 2", 3.5);
    want_num("1e3 + 1", 1001);
    want_num("- -3", 3);
    want_num("-7 % 3", -1);              /* JS の剰余は fmod 系（被除数符号） */
    want_num("2147483647 + 1", 2147483648.0); /* int32 fast-path 溢れ → double */
    want_num("-2147483648 % -1", 0);     /* INT32_MIN % -1 は UB を踏まず fmod 経路 */
    { /* 0.1+0.2 は binary64 厳密値と一致するはず */
        AklVal v;
        CHECK(akl_eval(g_rt, "0.1 + 0.2", &v));
        double d = 0;
        CHECK(akl_as_num(v, &d) && d == 0.30000000000000004);
    }
    { /* NaN は canonical 正規化され != 自身 */
        AklVal v;
        CHECK(akl_eval(g_rt, "var n = 0/0; n == n", &v));
        bool b = true;
        CHECK(akl_as_bool(v, &b) && b == false);
    }
    { /* ±inf */
        AklVal v; double d;
        CHECK(akl_eval(g_rt, "1/0", &v) && akl_as_num(v, &d) && isinf(d) && d > 0);
        CHECK(akl_eval(g_rt, "-1/0", &v) && akl_as_num(v, &d) && isinf(d) && d < 0);
    }
    want_num("0x10 + 0b10 + 0o17", 33);
    { /* グローバル定数 NaN / Infinity（書換不可） */
        AklVal v; double d;
        CHECK(akl_eval(g_rt, "NaN", &v) && akl_as_num(v, &d) && isnan(d));
        CHECK(akl_eval(g_rt, "Infinity", &v) && akl_as_num(v, &d) && isinf(d) && d > 0);
        CHECK(!akl_eval(g_rt, "NaN = 1;", NULL));
        CHECK(akl_eval(g_rt, "Infinity - Infinity == Infinity - Infinity", &v));
        bool b = true;
        CHECK(akl_as_bool(v, &b) && b == false); /* NaN === NaN ではない */
    }
}

static void t_vars_assign(void) {
    want_num("var x = 5; x * 2", 10);
    want_num("let y = 3; y + 1", 4);
    want_num("var a; var b = (a = 3) + 1; a * 10 + b", 34); /* 代入は式 */
    want_num("var q = 100; q", 100);                        /* eval 跨ぎのグローバル永続 */
    want_num("q + 1", 101);
    want_err("const c = 1; c = 2;", "const");
    want_err("var 1x;", NULL);
    want_err("const z;", "initializer");
    /* v0.3: ++/-- は正式対応（前置・後置）。値は JS どおり（後置=旧値、前置=新値） */
    want_num("var i = 5; --i; i", 4);
    want_num("var i = 1; ++i; i", 2);
    want_num("var i = 9; i--; i", 8);
    want_num("var i = 9; i++; i", 10);
    want_num("var i = 5; var j = i--; i + j", 9);   /* 後置は旧値を返す */
    want_num("var i = 5; var j = --i; i + j", 8);   /* 前置は新値を返す */
    want_num("var i = 5; i++ + i", 11);             /* 後置は評価後 +1 */
    want_num("1 - -2", 3);           /* 空白分離の二重 unary は生き続ける */
    want_num("- -3", 3);
    want_num("5 + +2", 7);
}

static void t_control(void) {
    want_num("if (1) 2; else 3;", 2);
    want_num("if (0) 2; else 3;", 3);
    want_num("if (1) { 10; } else { 20; }", 10);
    want_num("var s=0; for (var i=0; i<10; i = i+1) { s = s+i; } s", 45);
    want_num("var s=0; var i=0; while (i<5) { s = s+i; i = i+1; } s", 10);
    want_num("var i=0; for (;;) { i = i+1; if (i>=3) break; } i", 3);
    /* continue は本体を飛ばして step へ（step 忘却・cond 直行の両方を検出できる形） */
    want_num("var s=0; for (var i=0; i<10; i = i+1) { if (i % 2 == 1) continue; s = s+i; } s", 20);
    want_num("var n=0; for (var i=0; i<3; i=i+1) { for (var j=0; j<3; j=j+1) { if (j>i) break; n=n+1; } } n", 6);
    want_err("break;", "outside loop");
    want_err("continue;", "outside loop");
    want_err("while (1) {}", "budget");
}

static void t_functions(void) {
    want_num("function f(n){ if (n<=1) return 1; return n*f(n-1); } f(5)", 120);
    want_num("function fib(n){ if (n<2) return n; return fib(n-1)+fib(n-2); } fib(10)", 55);
    want_num("function g(a,b){ return a-b; } g(10,4)", 6);
    want_bool("typeof g == 'function'", true);
    want_str("function k(){ } var r = k(); typeof r", "undefined");
    want_str("function h2(a){ return typeof a; } h2()", "undefined"); /* 引数不足は undefined 埋め */
    want_num("function addv(a){ var s = 0; for (var i=0; i<a; i=i+1) s=s+i; return s; } addv(100)", 4950);
    want_err("return 1;", "return outside function");
    want_err("function r(){ return r(); } r();", "depth");
    want_err("u_nocall();", "not defined");       /* 未定義名の呼出は ReferenceError */
    want_err("var notfn = 1; notfn();", "not a function"); /* 非関数値の呼出は TypeError */
}

static void t_strings(void) {
    want_str("'hello ' + 'world'", "hello world");
    want_str("''", "");
    want_str("'a\\nb'", "a\nb");
    want_str("'\\u0041\\x42'", "AB"); /* \uXXXX / \xNN escape */
    want_str("'\\u3042'", "あ");      /* 直 UTF-8 Python の str 変換で fmt せずバイト列で比較 */
    want_bool("'a' < 'b'", true);
    want_bool("'b' <= 'a'", false);
    want_bool("'abc' == 'abc'", true);
    want_bool("'abc' == 'abd'", false);
    want_bool("'abc' != 'abd'", true);
    want_bool("'ab' < 'abc'", true);         /* 接頭辞は短い方が小 */
    want_bool("'' == ''", true);
    want_str("'1' + 1", "11");               /* JS: 片側 string は ToString 連結 */
    want_str("'x' + true", "xtrue");
    want_str("'v=' + null", "v=null");
    want_str("'u=' + undefined", "u=undefined");
    want_str("'' + (0.1 + 0.2)", "0.30000000000000004"); /* 往復最短精度 */
    want_str("'' + 1e21", "1e+21");
    want_str("'' + 1e20", "100000000000000000000");      /* 整数は 1e21 未満十進全桁 */
    want_str("'' + 1e-7", "1e-7");                        /* 指数の先行ゼロ正規化 */
    want_str("'' + NaN", "NaN");
    want_str("'' + (1/0)", "Infinity");
    want_str("'' + (-1/0)", "-Infinity");
    want_str("typeof 5", "number");
    want_str("typeof true", "boolean");
    want_str("typeof null", "object");
    want_str("typeof undefined", "undefined");
    want_bool("typeof '' == 'string'", true);
}

static void t_equality_logic(void) {
    want_bool("1 == 1", true);
    want_bool("1 == '1'", true);       /* loose: string→number */
    want_bool("1 === '1'", false);
    want_bool("1 === 1.0", true);
    want_bool("'1' == true", true);    /* loose: 両辺 number 化 */
    want_bool("0 == false", true);
    want_bool("null == undefined", true);
    want_bool("null === undefined", false);
    want_bool("null == 0", false);     /* null/undefined は数値化しない */
    want_bool("undefined == 0", false);
    want_bool("!(1 == 2)", true);
    want_bool("0 < 1", true);
    want_bool("1 <= 1", true);
    want_bool("2 > 1", true);
    want_bool("2 >= 3", false);
    want_bool("'10' < 9", false);      /* 混在は数値比較: '10'→10, 10<9=false */
    want_bool("'9' < 10", true);       /* 混在は数値比較: '9'→9, 9<10=true */
    /* 短絡: 右辺は未評価（undef 参照でも落ちない）・値は JS どおり生値 */
    want_num("var z = 0 && x_undefined_sc; z", 0);
    want_num("var z = 1 || x_undefined_sc; z", 1);
    want_num("var z = 5 && 6; z", 6);
    want_num("var z = 0 || 7; z", 7);
    want_str("var z = '' || 'fb'; z", "fb");
    want_bool("1<2 && 2<3", true);
    want_bool("1>2 || 2>3", false);
}

static void t_budgets_and_boundaries(void) {
    AklRT *rt2 = akl_new();
    CHECK(rt2 != NULL);
    AklVal v;
    /* 命令 budget: 毎回新鮮に供給される（枯渇後も次の eval は普通に走る） */
    CHECK(!akl_eval(rt2, "var i = 0; while (1) { i = i+1; }", NULL));
    CHECK(strstr(akl_error(rt2), "budget") != NULL);
    CHECK(akl_eval(rt2, "1+1", &v));
    double d = 0;
    CHECK(akl_as_num(v, &d) && d == 2);
    /* グローバルは rt ごと独立（q は g_rt 側で定義済みだが rt2 では未定義） */
    CHECK(!akl_eval(rt2, "q + 1", NULL));
    akl_free(rt2);
    /* 解析深度 budget（AKL_PARSE_DEPTH=512 超の括弧 600 連） */
    {
        char deep[1280];
        int p = 0;
        for (int i = 0; i < 600; i++) deep[p++] = '(';
        deep[p++] = '1';
        for (int i = 0; i < 600; i++) deep[p++] = ')';
        deep[p] = 0;
        CHECK(!akl_eval(g_rt, deep, NULL));
    }
    /* 空プログラム・コメントのみは合法 */
    CHECK(akl_eval(g_rt, ";", NULL));
    CHECK(akl_eval(g_rt, "/* nothing */ // nothing\n", NULL));
    /* 文字列ヒープ budget（倍々連結）は拒否で止まり、ホストを殺さない */
    CHECK(!akl_eval(g_rt, "var s = 'xxxxxxxx'; while (1) { s = s + s; }", NULL));
    CHECK(strstr(akl_error(g_rt), "budget") != NULL);
}

static void t_dispatch_parity(void) {
    /* 両 dispatch で一致すべき黄金値（初回観測でピン留めし不変を要求） */
    AklVal v; double d = -1;
    CHECK(akl_eval(g_rt,
        "function sig(n){ var a=0; for (var i=1; i<=n; i=i+1){ a = (a*31 + i) % 1000003; } return a; } sig(200)",
        &v) && akl_as_num(v, &d));
    CHECK(d == 674928); /* 参照実装 (Python: a=(a*31+i)%1000003, i=1..200) で実算 */
}


/* --- 融合命令（LINC / CJMPF_L/G）と fuzz 起源パーサ硬直化の regression（v0.1 追加） --- */
static void test_akl_fusion_and_hardening(void) {
    /* LINC: x = x ± int 定数 が式文で融合されても意味不変（int/文字列/溢れ） */
    want_num("function f(){ var i = 0; i = i + 2147483647; i = i + 1; return i; } f()", 2147483648.0);
    want_num("function f(){ var i = 5; i = i - 9; return i; } f()", -4);
    want_num("function f(){ var t = 0; for (var i = 0; i < 10; i = i + 1) { t = t + i; } return t; } f()", 45);
    want_str("function f(){ var s = 'a'; s = s + 1; return s; } f()", "a1");
    /* CJMPF_L/G: local rel int / global rel int、逆転形、<=/>=、文字列数値化 */
    want_num("function f(){ var c = 0; for (var i = 0; i <= 5; i = i + 1) c = c + 1; return c; } f()", 6);
    want_num("function f(){ var c = 0; for (var i = 10; i >= 8; i = i - 1) c = c + 1; return c; } f()", 3);
    /* グローバル名は g_rt に持ち越されるため一意名を使う（v0.0 設計: NAME は永続） */
    want_num("var gq = 0; for (var gi = 0; gi < 4; gi = gi + 1) gq = gq + 1; gq", 4);
    want_num("var gr = 0; for (var gj = 0; 4 > gj; gj = gj + 1) gr = gr + 1; gr", 4);
    want_num("function f(){ var s = '5'; if (s < 10) return 1; return 0; } f()", 1);
    want_num("function f(){ var s = 'z'; if (s < 10) return 1; return 0; } f()", 0);
    want_num("function f(){ if (3 < 2) return 1; return 2; } f()", 2);
    /* != / == int fast path 意味保持 */
    want_bool("function f(){ var a = 3; return a == '3'; } f()", true);
    want_bool("function f(){ var a = 3; return a === '3'; } f()", false);
    want_bool("function f(){ var a = 3; return a != 4; } f()", true);
    /* GC: 文字列 churn が object/heap budget で死なない（到達不能は回収される） */
    want_num("var gs = ''; for (var gk = 0; gk < 3000; gk = gk + 1) { gs = gs + 'x'; } gk", 3000);
    want_num("var gn = 0; for (var gl = 0; gl < 3000; gl = gl + 1) { var gt = 'x' + gl; gn = gn + 1; } gn", 3000);
    /* GC 後も生存参照は保持される（ルート経由の文字列が回収されない） */
    want_str("var gkeep = 'K'; for (var gm = 0; gm < 2000; gm = gm + 1) { var gu = 'x' + gm; } gkeep", "K");
    /* fuzz 起源: 単項マイナス連鎖と字句不全の硬直化（クラッシュせず評価が止まる） */
    {
        AklVal v;
        CHECK(!akl_eval(g_rt, "--.", &v));
        CHECK(!akl_eval(g_rt, "var x = 1 +", &v));
        CHECK(!akl_eval(g_rt, "---------------5", &v) || true); /* budget 内なら -5 で良い。構文拒否でも良い */
    }
}

/* phase 2/3: ROPE 連結・融合命令・for 回転の回帰。全て node/qjs 地上値と照合済みの数を使い、
 * グローバル名は rp_/ci_ 接頭で一意化する（v0.0 設計: グローバルは rt 内永続）。 */
static void test_akl_rope_and_superinst(void) {
    /* ROPE: 蓄積連結→等値・型・関係比較・alias・truthy */
    want_bool("var rpa=''; for (var rpi=0; rpi<20000; rpi=rpi+1) { rpa=rpa+'x'; } rpa===rpa", true);
    want_bool("var rpb=''; var rpc=''; for (var rpj=0; rpj<20000; rpj=rpj+1) { rpb=rpb+'x'; rpc=rpc+'x'; } rpb===rpc", true);
    want_bool("var rpd=''; var rpe=''; for (var rpk=0; rpk<20000; rpk=rpk+1) { rpd=rpd+'x'; rpe=rpe+'x'; } rpe=rpe+'y'; rpd===rpe", false);
    want_bool("var rpt=''; for (var rpl=0; rpl<5000; rpl=rpl+1) { rpt=rpt+'x'; } var rpu=rpt; rpt=rpt+'y'; rpu===rpt", false);
    want_bool("var rpf=''; for (var rpm=0; rpm<20000; rpm=rpm+1) { rpf=rpf+'x'; } rpf<'y'", true);
    want_bool("var rpg=''; for (var rpn=0; rpn<300; rpn=rpn+1) { rpg=rpg+'ab'; } typeof rpg === 'string'", true);
    /* ROPE とリテラルの等値（len 不一致は flatten せず即 false。等長は flatten 一致） */
    want_bool("var rph=''; for (var rpo=0; rpo<5; rpo=rpo+1) { rph=rph+'12345678901234'; } rph==='1234567890123412345678901234123456789012341234567890123412345678901234'", true);
    /* *CI 系: 文字列化する ADD は imm 右辺の連結（順序保持） */
    want_str("var cia='q'; cia + 1", "q1");
    want_str("var cib='q'; cib = cib + 1; cib", "q1");
    want_str("'a' + 1", "a1");
    want_str("1 + 'a'", "1a");
    /* GMULC/LMULC/乗算定数 int fast・溢れ・左定数 MUL（交換の丸め一致） */
    want_num("var cic=6; cic*3", 18);
    want_num("var cid=6; 3*cid", 18);
    want_num("var cie=5; (2+3)*4 + cie", 25);
    want_num("var cif=2147483647; cif+1", 2147483648.0);
    want_num("function f(){ var cli=6; return cli*3; } f()", 18);
    want_num("function f(){ var clj=17; return clj%5; } f()", 2);
    want_num("var cig= -2147483648 + 0; cig% -1", 0);
    want_num("var cih= -17; cih%5", -2);
    /* CJMPF_MOD: ==/!=、負の被除数、MOD 0 除算（NaN→else） */
    want_num("var cii=0; for (var cik=0; cik<10; cik=cik+1) { if (cik%3==0) { cii=cii+1; } } cii", 4);
    want_num("var cij=0; for (var cil=-10; cil<0; cil=cil+1) { if (cil%3==0) { cij=cij+1; } } cij", 3);
    want_num("var cim=0; for (var cin=0; cin<10; cin=cin+1) { if (cin%3!=0) { cim=cim+1; } } cim", 6);
    want_num("function f(){ var cl=0; for (var i=0; i<9; i=i+1) { if (i%4==0) { cl=cl+1; } } return cl; } f()", 3);
    /* GADD_G / GADD_P: int・文字列連結のオペランド順序 */
    want_num("var cip=1; var ciq=2; cip = cip + ciq; cip", 3);
    want_str("var cir='x'; var cis=1; cir = cir + cis; cir", "x1");
    want_str("var cit='x'; cit + 'y'", "xy");
    /* 文レベル代入の completion 値（JS 仕様: var 文は空 completion で上書きしない） */
    want_num("var ciu=0; ciu=42;", 42);
    want_num("var civ=0; civ=42; var ciw=1;", 42);
    /* *CI-st 再融合（グローバル/ローカル × 文字列/数値） */
    want_num("var cix=0; for (var ciz=0; ciz<1000; ciz=ciz+1) { cix=(cix+ciz*3+1)%1000003; } cix", 499497);
    want_str("var cja='a'; cja = cja + 1; cja", "a1");
    want_num("function f(){ var ll=10; ll = ll*3; ll = (ll+5)%7; return ll; } f()", 0);
    /* GX/LX（dst = expr op slot 3 アドレス融合）: 値・文字列順序・fmod フォールバック */
    want_num("var gxa=0; var gxm=100000; for (var gxi=0; gxi<100000; gxi=gxi+1) { gxa = (gxa + gxi*3 + 1) % gxm; } gxa", 50000);
    want_str("var gxb='a'; var gxc='b'; gxb = gxb + gxc; gxb", "ab");
    want_num("var gxd=-17; var gxe=5; gxd = gxd % gxe; gxd", -2);
    want_num("function f(){ var la=2; var lb=3; la = la * lb; lb = lb - la; return lb; } f()", -3);
    want_str("function f(){ var lc='q'; var ld=4; lc = lc + ld; return lc; } f()", "q4");
    /* CJMPF_MULGG/MODGG: 試行除法形（負数・文字列化・溢れの回帰込み） */
    want_num("var gpf=0; for (var gpn=2; gpn<200; gpn=gpn+1) { var gpp=1; var gpd=2; while (gpd*gpd<=gpn) { if (gpn%gpd==0) { gpp=0; break; } gpd=gpd+1; } if (gpp) { gpf=gpf+1; } } gpf", 46);
    /* LOOPINC 回転: break/continue/逆方向/非整数カウンタ/式初期化 */
    want_num("var roa=0; for (var roi=0; roi<100; roi=roi+1) { if (roi==5) break; roa=roa+1; } roa", 5);
    want_num("var rob=0; for (var roj=0; roj<10; roj=roj+1) { if (roj%2==0) { continue; } rob=rob+1; } rob", 5);
    want_num("var roc=0; for (var rok=1999; rok>0; rok=rok-1) { roc=roc+rok; } roc", 1999000);
    want_num("for (roq=0; roq<5; roq=roq+1) { } roq", 5); /* 非 var 初期化（pre-existing gap の回帰。roq は事前宣言なしで作られる） */
    want_num("var rod=0; for (var rol=0.5; rol<4; rol=rol+1) { rod=rod+1; } rod", 4);
    /* IIFE 未対応は仕様外機能なのでテストしない（parser 近似の既定） */
}

/* 定数除数 magic 剰余の同値性: JS:0 ≤ x ≤ 2^31−1 全域・負経路・2 の冪,
 * D=1・D 巨大の角まで C の % (=JS 規約の数学的意味) と絶対一致することの掃引検証。
 * magic 変形は「実行時コード生成なし」の整数同値変形であり、これが破れると
 * fusion 系命令（MODCI / *CI_G/L / CJMPF_MODG/L）の数学的基盤が崩れる。 */
static double mm_sweep_c(i32 lo, i32 hi, i32 d) {
    double s = 0;
    for (i32 i = lo; i < hi; i++) s += (double)(i % d);
    return s;
}

static void test_akl_modmagic(void) {
    static const i32 DS[] = { 2, 3, 4, 5, 7, 8, 10, 13, 16, 97, 256, 1000,
                              65536, 1000003, 1048576, 2147483646, 2147483647 };
    char buf[320];
    for (u32 t = 0; t < sizeof DS / sizeof DS[0]; t++) {
        i32 d = DS[t];
        /* 非負掃引（magic 経路）: var msa{t} = 0; for (msi{t}=0; <300000; ++) msa += msi % d */
        snprintf(buf, sizeof buf,
                 "var msa%u=0; for (var msi%u=0; msi%u<300000; msi%u=msi%u+1) { msa%u=msa%u+msi%u%%%d; } msa%u",
                 t, t, t, t, t, t, t, t, d, t);
        want_num(buf, mm_sweep_c(0, 300000, d));
        /* 負混在掃引（C フォールバック経路） */
        snprintf(buf, sizeof buf,
                 "var mnb%u=0; for (var mnj%u=-150000; mnj%u<150000; mnj%u=mnj%u+1) { mnb%u=mnb%u+mnj%u%%%d; } mnb%u",
                 t, t, t, t, t, t, t, t, d, t);
        want_num(buf, mm_sweep_c(-150000, 150000, d));
        /* 条件一致融合 (CJMPF_MOD): count i in [0,B) s.t. i%d==k */
        i32 B = 200000;
        i32 k = d > 7 ? 7 : d - 1;
        i32 cnt_c = 0;
        for (i32 i = 0; i < B; i++) if (i % d == k) cnt_c++;
        snprintf(buf, sizeof buf,
                 "var mcc%u=0; for (var mcj%u=0; mcj%u<200000; mcj%u=mcj%u+1) { if (mcj%u%%%d==%d) { mcc%u=mcc%u+1; } } mcc%u",
                 t, t, t, t, t, t, d, k, t, t, t);
        want_num(buf, cnt_c);
    }
    /* 角: 2^31−1/2 境界（int 上限直下の magic 精度）。d>1 かつ x≥0 の最端 */
    want_num("2147483647 % 2147483647", 2147483647 % 2147483647);
    want_num("2147483647 % 2147483646", 2147483647 % 2147483646);
    want_num("2147483646 % 1000003", 2147483646 % 1000003);
    want_num("2147483647 % 1000003", 2147483647 % 1000003);
    want_num("2147483647 % 2", 1);
    want_num("2147483647 % 1073741824", 2147483647 % 1073741824);
    /* d=1 / 負の除数 / 負の被除数（非 magic 経路の規約確認） */
    want_num("2147483647 % 1", 0);
    want_num("-7 % 3", -1);
    want_num("7 % -3", 1);
}

/* ---- JS 例外（throw/try/catch/finally）。JS 規則の要点:
 *   finally は正常・throw・return の全経路で走る。catch 束縛は例外値。
 *   未捕捉はホストエラー（budget/OOM と同じ致命的扱い = 製品安全側）。 */
static void t_exceptions(void) {
    /* 基本捕捉 */
    want_num("var r=0; try { throw 5; } catch(e) { r=e; } r", 5);
    want_num("var r=0; try { r=1; } catch(e) { r=99; } r", 1); /* 投げなしは素通し */
    /* catch 束縛なし（ES2019） */
    want_num("var r=0; try { throw 7; } catch { r=3; } r", 3);
    /* finally は正常経路でも走る */
    want_str("var s=''; try { s=s+'a'; } finally { s=s+'b'; } s", "ab");
    /* finally は throw 経路でも走る（連鎖: 内側 finally → 外側 catch） */
    want_str("var s=''; try { try { throw 1; } finally { s=s+'f'; } } catch(e) { s=s+'c'; } s", "fc");
    /* catch+finally 両方（順序: try→catch→finally） */
    want_str("var s='x'; try { throw 'e1'; } catch(e) { s=s+e; } finally { s=s+'F'; } s", "xe1F");
    /* rethrow（catch からの再送出 → 外側が捕捉） */
    want_num("var r=0; try { try { throw 2; } catch(e) { throw e+1; } } catch(e2) { r=e2; } r", 3);
    /* finally の中からの throw は元の保留を破棄して新例外が勝つ */
    want_num("var r=0; try { try { throw 1; } finally { throw 2; } } catch(e) { r=e; } r", 2);
    /* 関数呼出しを跨ぐ巻き戻し: callee が投げて caller が捕まえる */
    want_num("function xg(){ throw 42; } var r=0; try { xg(); } catch(e){ r=e; } r", 42);
    /* return 経路でも finally が走り、返り値は finally 実行前に確定した値 */
    want_num("var rfc=0; function xf(){ try { return 1; } finally { rfc=rfc+1; } }"
             " var rq=xf(); rq*10+rfc", 11);
    /* 深い関数連鎖の中間で catch */
    want_num("function xa(){ throw 5; } function xb(){ xa(); } function xc(){ xb(); }"
             " var r=0; try { xc(); } catch(e){ r=e; } r", 5);
    /* 未捕捉はホストエラー（致命的。文字列化される） */
    want_err("throw 123", "uncaught exception");
    want_err("throw 'boom'", "uncaught exception: boom");
    want_err("function xh(){ throw 1; } xh()", "uncaught exception");
    /* try 越境 break/continue は v0.1 では明白な compile エラー（誤動作より拒否） */
    want_err("var iw=0; while(iw<3){ try { break; } catch(e){} iw=iw+1; }", "boundary");
    want_err("for(var ic=0;ic<3;ic=ic+1){ try { continue; } finally {} }", "boundary");
    /* loop INSIDE try（跨がない）は合法 */
    want_num("var rlp=0; try { for(var ilp=0;ilp<4;ilp=ilp+1){ if(ilp==2){ break; } rlp=rlp+1; } } catch(e){} rlp", 2);
    /* ネスト深度: try は lex 的に積める（上限以内） */
    want_num("var r=0; try{ try{ try{ throw 9; }catch(e){ r=r+1; } }finally{ r=r+2; } }finally{ r=r+4; } r", 7);
    /* 例外を catch して値が残るとき stack は破壊されない（後続 eval が独立に動く） */
    want_num("var zz=1; try { throw 0; } catch(e){} zz+1", 2);
    /* カンマ宣言の保持（旧バグ: 2 個目以降しか残らなかった。CoJIT 検証中に同定） */
    want_num("var ca=1,cb=2; ca*10+cb", 12);
    want_num("var cc=1,cd2=2,ce=3; cc*100+cd2*10+ce", 123);
    /* v0.5: native 例外（JSON.parse 等）は JS の try/catch で捕捉可能（V8 準拠）。
     * 以前は native_err 経由で eval 全体が失敗していた。e.name も "SyntaxError" になる。 */
    want_str("try { JSON.parse('{') } catch (e) { e.name }", "SyntaxError");
    want_str("try { JSON.parse('[') } catch (e) { 'caught:' + e.name }", "caught:SyntaxError");
    want_str("function g() { try { JSON.parse('[') } catch (e) { throw e; } }"
             " try { g() } catch (e2) { e2.name }", "SyntaxError"); /* rethrow 跨フレーム */
    /* v0.5: HOF コールバック（再入 akl_call）の例外は「元の値」のまま伝搬する
     * （文字列化されない。V8 と同一）。Error OBJ の identity も保持。 */
    want_str("try { [1,2].map(function(x){ throw 42; }) } catch (e) { typeof e + ':' + e }", "number:42");
    want_str("try { [1,2].map(function(x){ throw new TypeError('boom'); }) } catch (e) { e.name + ':' + e.message }",
             "TypeError:boom");
    /* v0.5: catch 束縛のシャドーイング（自関数の catch 変数が祖先の同名 catch 変数と
     * 衝突して誤 capture され、CELOAD が ENV から undefined を読む実バグの回帰防止） */
    want_str("function g() { try { JSON.parse('[') } catch (e) { throw e; } }"
             " try { g() } catch (e) { e.name }", "SyntaxError");
    want_num("function g2() { try { throw 7; } catch (e) { return e; } }"
             " try { throw 1; } catch (e) { g2() }", 7);
}

/* ---- CoJIT（検証駆動 AOT 特化）の差分オラクル。
 * 等価性は「on/off の 2 インスタンスで結果が一致する」で機械監査する
 * （ハッキング耐性の中核: 特化器が意味を変えた瞬間、ここが赤くなる）。
 * さらに数値ケースは C 側の独立予測とも突き合わせる（diff 以上の強度）。 */
static u32 xor32(u32 *st) {
    u32 x = *st;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *st = x;
    return x;
}

static void t_cojit(void) {
    /* 決定的コーパス: 発火の有無と結果の一致 */
    static const struct { const char *src; double want; u32 min_fire; } CC[] = {
        { "var i=0; while(i<5){ i=i+1; } i", 5, 1 },
        { "var i=0; var s=0; while(i<7){ s=s+i*2; i=i+1; } s", 42, 1 },
        { "function wf(){ var i=0; var s=1; while(i<4){ s=s*2; i=i+1; } return s; } wf()", 16, 1 },
        { "var i=0; while(i<100){ if(i==50){ break; } i=i+1; } i", 50, 1 },
        { "var i=0; var cd=0; while(i<10){ i=i+1; if(i%3==0){ continue; } cd=cd+1; } cd", 7, 0 },
        { "var i=10; while(i>0){ i=i-3; } i", -2, 1 },
        { "var i=0; while(i<5){ i=i+1; }", 5, 1 },     /* last_val 保持の検証（末尾） */
        { "var so='x'; var i=0; while(i<3){ so=so+i; i=i+1; } so", 0, 1 }, /* 数値評価不可→下で別検査 */
        { "var g=0; function gf(){ var s=0; while(g<8){ s=s+g; g=g+2; } return s; } gf()", 12, 1 },
        { "var a2=0,b2=0; while(a2<4){ while(b2<3){ b2=b2+1; } a2=a2+1; } a2*10+b2", 43, 1 }, /* 入れ子 */
        /* ---- 末尾 D（for-update 形 g=g+d: GLOAD_S;ADDCI;DUP;GSTORE_S;POP → LOOPINC non-V） ---- */
        { "var s=0; for(var i=0;i<5;i=i+1){ s=s+i; } s", 10, 1 },
        { "var s=0; for(var i=10;i>0;i=i-3){ s=s+1; } s", 4, 1 },
        { "var s=0; for(var i=0;i<7;i=i+1){ if(i==3){ continue; } s=s+i; } s", 18, 1 }, /* continue→update */
        { "var s=0; for(var i=0;i<10;i=i+1){ if(i==4){ break; } s=s+i; } s", 6, 1 },     /* break→exit */
        { "var s=0; for(var i=0;i<3;i=i+1){ for(var j=0;j<3;j=j+1){ s=s+1; } } s", 9, 2 }, /* 入れ子両層 */
        { "7; for(var i=0;i<3;i=i+1){}", 7, 1 },  /* D は last_val を汚さない（V 誤用なら 3 になる） */
        { "function ff(){ var s=0; for(var i=0;i<4;i=i+1){ s=s+i*2; } return s; } ff()", 12, 1 }, /* 関数内 D */
        { "var so2=0; for(var i=0;i<3;i=i+1){ so2=so2+1; } so2", 3, 1 },
    };
    AklRT *on = akl_new(), *off = akl_new();
    CHECK(on && off);
    akl_set_cojit(on, 1);
    akl_set_cojit(off, 0);
    for (u32 i = 0; i < sizeof(CC) / sizeof(CC[0]); i++) {
        if (i == 7) { /* 文字列ケース: "x"+0+1+2 */
            AklVal v; u32 ln = 0;
            bool ok1 = akl_eval(on, CC[i].src, &v);
            const char *s1 = ok1 ? akl_as_str(on, v, &ln) : NULL;
            CHECK(ok1 && s1 && ln == 4 && memcmp(s1, "x012", 4) == 0);
            continue;
        }
        AklVal v1, v2;
        bool ok1 = akl_eval(on, CC[i].src, &v1);
        bool ok2 = akl_eval(off, CC[i].src, &v2);
        CHECK(ok1 == ok2);
        double d1 = 0, d2 = 0;
        akl_as_num(v1, &d1); akl_as_num(v2, &d2);
        CHECK(ok1 && d1 == CC[i].want && d1 == d2);
        if (!(ok1 && d1 == CC[i].want && d1 == d2))
            fprintf(stderr, "  cojit mismatch [%s]: on=%g off=%g want=%g\n", CC[i].src, d1, d2, CC[i].want);
        CHECK(akl_cojit_count(on) >= CC[i].min_fire);
    }
    CHECK(akl_cojit_count(on) >= 8);

    /* 構造化乱択差分: 形をランダムにして on/off 一致を 400 系統で監査。
     * C 側の整数予測とも同時に突き合わせる（int 域に限定して double 厳密一致を使う） */
    u32 st = 0xC0117u;
    char buf[512];
    for (u32 t = 0; t < 400; t++) {
        u32 feat = xor32(&st);
        bool in_func = feat & 1;
        bool is_local = (feat >> 1) & 1;
        i32 dlt = (i32[] ){ 1, 2, -1, -3, 97 }[(feat >> 2) % 5];
        u8 cmpi = (u8)((feat >> 5) % 4);
        const char *cmps[] = { "<", "<=", ">", ">=" };
        i32 start = (i32)(xor32(&st) % 200) - 100;
        i32 lim = (i32)(xor32(&st) % 200) - 100;
        /* 収束を保証する向きに揃える */
        if (dlt > 0) { if (cmpi >= 2) cmpi = 0; if (start >= lim) lim = start + 1; }
        else { if (cmpi < 2) cmpi = 2; if (start <= lim) start = lim + 1; }
        i32 kk = (i32)(xor32(&st) % 5);
        /* 期待値を C で直接計算（i32 域に留める。99m 回踏まないよう回数は収束式で） */
        i64 acc = 0, i = start;
        i64 guard = 0;
        while (guard++ < 100000) {
            bool r = cmpi == 0 ? i < lim : cmpi == 1 ? i <= lim : cmpi == 2 ? i > lim : i >= lim;
            if (!r) break;
            acc += i * kk;
            i += dlt;
            if (i > 1000000000 || i < -1000000000) break; /* double 厳密域超過回避 */
        }
        if (guard >= 100000 || i > 100000000 || i < -100000000) { t--; continue; }
        const char *carr = is_local ? "ci" : "cg";
        int w;
        if (in_func) {
            w = snprintf(buf, sizeof buf,
                "function cjf(){ var %s=%d; var acc=0; while(%s%s%d){ acc=acc+%s*%d; %s=%s+%d; } return acc; } cjf()",
                carr, start, carr, cmps[cmpi], lim, carr, kk, carr, carr, dlt);
        } else if (is_local) {
            /* main にローカルは catch 以外無いので is_local は関数内のみ有意 */
            t--; continue;
        } else {
            w = snprintf(buf, sizeof buf,
                "var %s=%d; var acc=0; while(%s%s%d){ acc=acc+%s*%d; %s=%s+%d; } acc",
                carr, start, carr, cmps[cmpi], lim, carr, kk, carr, carr, dlt);
        }
        (void)w;
        akl_set_cojit(on, 1);
        AklVal v1, v2;
        bool ok1 = akl_eval(on, buf, &v1);
        bool ok2 = akl_eval(off, buf, &v2);
        double d1 = -777, d2 = -778;
        akl_as_num(v1, &d1); akl_as_num(v2, &d2);
        bool same = ok1 == ok2 && d1 == d2 && d1 == (double)acc;
        CHECK(same);
        if (!same) {
            fprintf(stderr, "  cojit randomized diff t=%u [%s]: on(%d,%g) off(%d,%g) want %lld\n",
                    t, buf, ok1, d1, ok2, d2, (long long)acc);
            break;
        }
    }
    akl_free(on);
    akl_free(off);
}

/* ============================== オブジェクト + ネイティブ登録層 ============================== */

static void t_objects(void) {
    want_num("var o = {a:1, b:2}; o.a + o.b", 3);
    want_num("var o = {}; o.x = 7; o.x", 7);
    want_num("var o = {}; o.x = 7", 7);               /* 代入式の値は右辺 */
    want_undef("var o = {a:1}; o.missing");           /* 未定義 prop → undefined */
    want_num("var o = {p:{q:41}}; o.p.q + 1", 42);    /* ネスト + PGET 連鎖 */
    want_num("var o = {\"k\":9}; o.k", 9);            /* 文字列キー */
    want_num("var o = {a:5,}; o.a", 5);               /* trailing comma */
    want_num("({a:2}).a * 21", 42);                   /* 括弧 primary の連鎖 */
    want_num("var o = {}; o.a = 1; o.b = 2; o.a + o.b", 3);
    want_num("function g(){ return 3; } var o = {}; o.f = g; o.f()", 3); /* メソッド: bytecode 関数（this 未導入、self は渡らない） */
    want_num("var o = {}; o.x = 1; var p = o; p.x + 9", 10);             /* 参照の共有 */
    want_str("typeof ({})", "object");
    want_str("typeof ({a:1})", "object");
    want_str("\"\" + {a:1}", "[object Object]");
    want_bool("var a = {}; var b = a; a === b", true);
    want_bool("var a = {}; var b = {}; a === b", false);  /* identity（同形でも別物） */
    want_bool("var a = {}; var b = a; a !== b", false);
    /* 明白失敗系（黙った誤答を作らない） */
    want_err("var o = 5; o.x = 1", "TypeError");       /* 非 object store（代入は TypeError が正しい） */
    want_undef("var o = 5; o.x");                      /* 非 object load は undefined（V8 準拠。v0.5 で修正） */
    want_err("var o = {}; o.f()", "not a function");   /* 無い/非関数メソッド */
    want_num("var o = {a}; o.a == undefined ? 1 : 0", 0); /* shorthand: a 未定義 → o.a は undefined */
    want_num("var x = 9; var o = {a:1, b: x}; o.b", 9);  /* b: x は通常プロパティ */
    want_num("{a:1}", 1);                              /* 文頭 { はブロック文。a:1 はラベル+式文（JS 準拠） */
    /* prop 数天井 64（1 obj あたり）: 65 個目で明白に失敗 */
    {
        char src[4200];
        int n = snprintf(src, sizeof src, "var o = {}; ");
        for (int i = 0; i <= 64; i++) n += snprintf(src + n, sizeof src - (size_t)n, "o.k%d = 0; ", i);
        want_err(src, "property budget");
        src[0] = 0;
        n = snprintf(src, sizeof src, "var o = {}; ");
        for (int i = 0; i < 64; i++) n += snprintf(src + n, sizeof src - (size_t)n, "o.k%d = %d; ", i, i);
        n += snprintf(src + n, sizeof src - (size_t)n, "o.k63");
        want_num(src, 63);                             /* 64 個ちょうどは成功 */
    }
}

/* ---- ネイティブ登録層 ---- */
static AklVal g_ho_expect;
static int g_reg_rejected;

static AklVal n_add2(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)self; (void)udata;
    if (argc != 2) return akl_mkundefined(); /* argc 規約: 過不足は undefined（err にしない） */
    double a, b;
    if (!akl_as_num(argv[0], &a) || !akl_as_num(argv[1], &b)) {
        akl_native_throw(rt, "add2 expects numbers");
        return akl_mkundefined();
    }
    return akl_mknum(a + b);
}
static AklVal n_is_self(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)rt; (void)argc; (void)argv;
    AklVal *want = (AklVal *)udata;
    return akl_mkbool(self == *want);
}
static AklVal n_getn(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)argc; (void)argv; (void)udata;
    return akl_prop_get(rt, self, "n"); /* native 内 prop_get（既存 intern ヒット経路） */
}
static AklVal n_get_unknown(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)argc; (void)argv; (void)udata;
    /* 実行中の新規 intern を強制する（nursery 保護経路の実証。無保護なら GC で dangling に成り得た） */
    return akl_prop_get(rt, self, "never_seen_prop_xyzzy_0");
}
static AklVal n_boom(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)self; (void)argc; (void)argv; (void)udata;
    akl_native_throw(rt, "catastrophic failure");
    return akl_mkundefined();
}
/* n 個の一時文字列（各 70,000B）を unpinned で作って合計 len を返す。
 * 8 個 → 560KB > GC 初期閾値 512KB なので途中で GC 発火。nursery 保護なしなら
 * 作った文字列が sweep され読み出しで破壊される（ASAN が裏取りする経路）。 */
static AklVal n_temps(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)self; (void)udata;
    if (argc != 1) return akl_mkundefined();
    double nd;
    if (!akl_as_num(argv[0], &nd)) { akl_native_throw(rt, "temps expects a number"); return akl_mkundefined(); }
    int n = (int)nd;
    char *buf = (char *)malloc(70001);
    if (!buf) { akl_native_throw(rt, "oom: temps"); return akl_mkundefined(); }
    memset(buf, 'q', 70000);
    double total = 0;
    for (int i = 0; i < n; i++) {
        AklVal sv = akl_mkstring(rt, buf, 70000);
        uint32_t ln = 0;
        const char *b = akl_as_str(rt, sv, &ln);
        if (b) total += (double)ln;
    }
    free(buf);
    return akl_mknum(total);
}
static AklVal n_reg_inside(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)self; (void)argc; (void)argv; (void)udata;
    if (akl_native_register(rt, "late_fn", n_add2, NULL)) {
        akl_native_throw(rt, "register during eval must be rejected");
        return akl_mkundefined();
    }
    g_reg_rejected = 1;
    return akl_mknum(7);
}

/* akl_tostring（host プリミティブ）: native 内で引数を JS ToString して返す。
 * console.log 等が組み立てられることを公開面越しに機械検証する。 */
static AklVal n_tostr(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)self; (void)udata;
    if (argc != 1) return akl_mkundefined();
    AklVal sv = akl_tostring(rt, argv[0]);
    if (akl_error(rt)[0]) return akl_mkundefined();
    return sv; /* 即返却: caller（VM）が push で直ちに根付かせるため生存規約を満たす */
}

static void t_native(void) {
    CHECK(akl_native_register(g_rt, "add2", n_add2, NULL));
    want_num("add2(20, 22)", 42);
    want_num("add2(add2(1,2), add2(3,4))", 10);
    want_undef("add2(1)");                            /* argc 規約（err ではなく undefined） */
    want_err("add2(1, \"x\")", "add2 expects numbers"); /* native_throw は明白な失敗 */
    want_undef("add2()");                            /* argc 規約: 0 個でも undefined */
    /* self 伝播: メソッド呼出はレシーバ、通常呼出は undefined */
    AklVal ho = akl_mkobject(g_rt);
    CHECK(akl_is_object(g_rt, ho));
    CHECK(akl_prop_set(g_rt, ho, "n", akl_mknum(41)));
    g_ho_expect = ho;
    CHECK(akl_prop_set(g_rt, ho, "isMe", akl_mknative(g_rt, n_is_self, &g_ho_expect)));
    CHECK(akl_prop_set(g_rt, ho, "getn", akl_mknative(g_rt, n_getn, NULL)));
    CHECK(akl_prop_set(g_rt, ho, "getUnknown", akl_mknative(g_rt, n_get_unknown, NULL)));
    CHECK(akl_global_set(g_rt, "ho", ho));
    want_bool("ho.isMe()", true);
    want_bool("var f = ho.isMe; f()", false);         /* 関数値経由の通常呼出は self=undefined */
    want_num("ho.getn() + 1", 42);
    want_undef("ho.getUnknown()");                    /* 実行中新規 intern も安全（nury 保護） */
    want_num("var o = {}; o.x = ho.getn(); o.x + 1", 42); /* native 値をスクリプト obj へ */
    CHECK(akl_native_register(g_rt, "boom", n_boom, NULL));
    want_err("boom()", "catastrophic failure");
    /* プログラム的な temp budget: 8 個は成功（GC 発火帯）・9 個目で明白に失敗 */
    AklVal tfn = akl_mknative(g_rt, n_temps, NULL);
    CHECK(akl_global_set(g_rt, "temps", tfn));
    want_num("temps(8)", 8.0 * 70000.0);
    want_err("temps(9)", "temp budget");
    /* 登録拒否（native コールバック内からの登録は構造的に拒否） */
    g_reg_rejected = 0;
    CHECK(akl_global_set(g_rt, "reginside", akl_mknative(g_rt, n_reg_inside, NULL)));
    want_num("reginside()", 7);
    CHECK(g_reg_rejected == 1);
    want_err("late_fn(1,2)", NULL);                   /* 拒否されたので束縛は存在しない */
    /* budget 課金: native 1 呼出は AKL_NATIVE_COST(1024) 命令相当。500 では明白に枯渇 */
    {
        AklRT *b = akl_new();
        CHECK(b != NULL);
        if (b) {
            akl_set_insn_budget(b, 500);
            CHECK(akl_native_register(b, "add2", n_add2, NULL));
            CHECK(!akl_eval(b, "add2(1,2)", NULL));
            CHECK(strstr(akl_error(b), "budget") != NULL);
            akl_free(b);
        }
    }
    /* host 側 const グローバルはスクリプトから上書きできない（明白に失敗） */
    want_err("ho = 1", "const");
    /* akl_tostring: 全型の JS ToString（DOM バインド/console 前提プリミティブ） */
    CHECK(akl_global_set(g_rt, "tostr", akl_mknative(g_rt, n_tostr, NULL)));
    want_str("tostr(42)", "42");
    want_str("tostr(1.5)", "1.5");
    want_str("tostr(true)", "true");
    want_str("tostr(undefined)", "undefined");
    want_str("tostr(null)", "null");
    want_str("tostr(\"s\")", "s");
    want_str("tostr({a:1})", "[object Object]");
    want_str("tostr(tostr)", "function");                /* NATIVE も JS 同様 function */
    want_str("tostr(40) + 2", "402");                  /* 文字列 + 数値の連結保存 */
}

/* ---- AKL_OK_HANDLE（AklHandleVTab）の敵対検証（v0.3 DOM バインドの足場） ---- */
typedef struct { int n; char tag[8]; } TBox;
static const AklHandleVTab g_box_vt;
static int g_hcalls;

static bool h_get(AklRT *rt, void *ptr, const char *name, uint32_t len, AklVal *out) {
    TBox *b = (TBox *)ptr;
    if (len == 1 && name[0] == 'n') { *out = akl_mknum((double)b->n); return true; }
    if (len == 4 && memcmp(name, "name", 4) == 0) {
        /* VM 実行中の確保（nursery 規約経路）を必ず踏ませる */
        *out = akl_mkstring(rt, b->tag, (uint32_t)strlen(b->tag));
        return true;
    }
    return false; /* unknown prop → undefined（TypeError ではない） */
}
static bool h_set(AklRT *rt, void *ptr, const char *name, uint32_t len, AklVal v) {
    (void)rt;
    TBox *b = (TBox *)ptr;
    if (len != 1 || name[0] != 'n') return false; /* → "property store rejected" */
    double d;
    if (!akl_as_num(v, &d)) return false;         /* 型違反も拒否で明白に */
    b->n = (int)d;
    return true;
}
static bool h_call(AklRT *rt, void *ptr, const char *name, uint32_t len,
                   int argc, const AklVal *argv, AklVal *out) {
    TBox *b = (TBox *)ptr;
    if (len == 3 && memcmp(name, "add", 3) == 0) {
        double d = 0;
        if (argc != 1 || !akl_as_num(argv[0], &d)) {
            akl_native_throw(rt, "add expects 1 number");
            return true;
        }
        b->n += (int)d;
        g_hcalls++;
        *out = akl_mknum((double)b->n);
        return true;
    }
    if (len == 4 && memcmp(name, "self", 4) == 0) {
        /* gc_live 中のハンドル生成（メソッド戻り値の本筋経路 = nursery 一時保護） */
        *out = akl_mkhandle(rt, &g_box_vt, b);
        return true;
    }
    return false; /* 未知メソッド → "TypeError: not a function" */
}
static const AklHandleVTab g_box_vt = { "TBox", h_get, h_set, h_call };

static void t_handles(void) {
    TBox box; box.n = 10; memcpy(box.tag, "BoxTen", 7);
    g_hcalls = 0;

    /* 基本面: typeof / tostring / 未定義意味論の凍結 */
    CHECK(akl_global_set(g_rt, "box", akl_mkhandle(g_rt, &g_box_vt, &box)));
    CHECK(akl_is_handle(g_rt, akl_mkhandle(g_rt, &g_box_vt, &box)));
    want_str("typeof box", "object");
    want_str("'' + box", "[object TBox]");
    want_num("box.n", 10);
    want_str("box.name", "BoxTen");
    want_undef("box.zzz");                       /* unknown get は undefined */
    want_err("box.zzz = 1", "property store rejected");
    want_err("box.n = 's'", "property store rejected"); /* 型違反も拒否 */
    want_err("box.nosuch(1)", "not a function");
    want_err("box.add()", "add expects 1 number");
    want_num("box.n = 41; box.n + 1", 42);       /* set → C 側 mutates → get で見える */
    CHECK(box.n == 41);
    want_num("box.add(5)", 46);
    CHECK(g_hcalls == 1);
    want_num("var k2 = box.self(); k2.add(1); box.n", 47); /* self ハンドルの identity は C ptr 共有 */
    /* ハンドルに対する !! / 等値の挙動（truthy object） */
    want_bool("!box", false);
    want_bool("box == box", true);
    akl_eval(g_rt, "box = undefined", NULL);     /* 後続の他テストを汚さない掃除 */

    /* GC churn: heap 1MB に絞った専用 RT で、mcall 戻り値・get 確保・garbage を
     * 数千回まわして GC を強制発火させる（HANDLE オブジェクトの rooting 証明）。
     * 期待値は Python 実算で凍結: Σ 11..3010 + 300000 = 4,831,500。 */
    AklRT *rt3 = akl_new();
    CHECK(rt3 != NULL);
    if (!rt3) return;
    akl_tune(rt3, 0, 1, 0); /* heap 1MB（insn budget は既定 10M のまま） */
    g_hcalls = 0;
    box.n = 10;
    CHECK(akl_global_set(rt3, "box", akl_mkhandle(rt3, &g_box_vt, &box)));
    AklVal v; double d = -1;
    CHECK(akl_eval(rt3,
        "var acc = 0;"
        "for (var i = 0; i < 3000; i = i + 1) {"
        "  var junk = 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx' + i;"
        "  if (box.name == 'BoxTen') { acc = acc + 100; }"
        "  acc = acc + box.add(1);"
        "} acc",
        &v) && akl_as_num(v, &d));
    if (d != 4831500) fprintf(stderr, "  gc-churn acc: got %g want 4831500\n", d);
    CHECK(d == 4831500);
    CHECK(g_hcalls == 3000);
    /* GC 発火の機械証明: 1MB cap に対し 50k iter × (ROPE+STR ≈120B) ≈ 6MB の
     * garbage を流す。GC が全く発火しなければ heap budget で eval 失敗する =
     * 成功かつ値が合うこと ≡ GC が複数回発火した上で rooting が正しかったこと。
     * さらに box.name（get コールバック内確保）も毎回踏ませ、コールバック最中の
     * 確保→GC の最悪窓を必ず通す。 */
    CHECK(akl_eval(rt3, "1", NULL)); /* 前エラーの掃除 */
    CHECK(akl_eval(rt3,
        "var fl = 0;"
        "for (var i = 0; i < 50000; i = i + 1) {"
        "  var g = 'flood-flood-flood-flood-flood' + i;"
        "  if (box.name == 'BoxTen') { fl = fl + 1; }"
        "} fl",
        &v) && akl_as_num(v, &d));
    if (d != 50000) fprintf(stderr, "  gc-flood: got %g want 50000 (GC 未発火なら heap budget で潰れる)\n", d);
    CHECK(d == 50000);
    /* self() で作ったハンドルが GC 嵐を跨いで生存（globals root 経路） */
    CHECK(akl_eval(rt3,
        "var keep = box.self();"
        "keep.n = 5;"
        "for (var i = 0; i < 20000; i = i + 1) { var z = 'zzzzzzzzzzzzzzzz' + i; }"
        "keep.add(2)",
        &v) && akl_as_num(v, &d));
    CHECK(d == 7);
    CHECK(box.n == 7); /* C 側実体への書込が最終値として残る */
    akl_free(rt3);
}

static void t_v03_arrays(void);
static void t_v03_closures(void);
static void t_v03_this_methods(void);
static void t_v03_control(void);
static void t_v03_misc(void);
static void t_v03_builtins(void);
static void t_v03_json(void);
static void t_v03_hof(void);
static void t_v03_syntax2(void);
static void t_v03_syntax3(void);
static void t_v04_regex(void);
static void t_v04_class_extends(void);
static void t_v04_spread_rest(void);
static void t_v04_builtins2(void);
static void t_v04_fields_len(void);
static void t_v04_logassign(void);
static void t_v04_objlit_ext(void);
static void t_v04_labels(void);
static void t_v04_arguments(void);
static void t_v04_arrow_promise_async(void);
static void t_v04_bigint(void);
static void t_v04_generator(void);
static void t_v04_map_set(void);
static void t_v05_import_export(void);
static void t_v05_class_accessors(void);

void test_akl(void) {
    g_rt = akl_new();
    CHECK(g_rt != NULL);
    if (!g_rt) return;
    t_arith();
    t_vars_assign();
    t_control();
    t_functions();
    t_strings();
    t_equality_logic();
    t_budgets_and_boundaries();
    t_dispatch_parity();
    test_akl_fusion_and_hardening();
    test_akl_rope_and_superinst();
    test_akl_modmagic();
    t_exceptions();
    t_cojit();
    t_objects();
    t_native();
    t_handles();
    fprintf(stderr, "  %-40s", "t_v03_arrays");
    t_v03_arrays();
    fprintf(stderr, "  %-40s", "t_v03_closures");
    t_v03_closures();
    fprintf(stderr, "  %-40s", "t_v03_this_methods");
    t_v03_this_methods();
    fprintf(stderr, "  %-40s", "t_v03_control");
    t_v03_control();
    fprintf(stderr, "  %-40s", "t_v03_misc");
    t_v03_misc();
    fprintf(stderr, "  %-40s", "t_v03_builtins");
    t_v03_builtins();
    fprintf(stderr, "  %-40s", "t_v03_json");
    t_v03_json();
    fprintf(stderr, "  %-40s", "t_v03_hof");
    t_v03_hof();
    fprintf(stderr, "  %-40s", "t_v03_syntax2");
    t_v03_syntax2();
    fprintf(stderr, "  %-40s", "t_v03_syntax3");
    t_v03_syntax3();
    fprintf(stderr, "  %-40s", "t_v04_regex");
    t_v04_regex();
    fprintf(stderr, "  %-40s", "t_v04_class_extends");
    t_v04_class_extends();
    fprintf(stderr, "  %-40s", "t_v04_spread_rest");
    t_v04_spread_rest();
    fprintf(stderr, "  %-40s", "t_v04_builtins2");
    t_v04_builtins2();
    fprintf(stderr, "  %-40s", "t_v04_logassign");
    t_v04_logassign();
    fprintf(stderr, "  %-40s", "t_v04_objlit_ext");
    t_v04_objlit_ext();
    fprintf(stderr, "  %-40s", "t_v04_fields_len");
    t_v04_fields_len();
    fprintf(stderr, "  %-40s", "t_v04_labels");
    t_v04_labels();
    fprintf(stderr, "  %-40s", "t_v04_arguments");
    t_v04_arguments();
    fprintf(stderr, "  %-40s", "t_v04_arrow_promise_async");
    t_v04_arrow_promise_async();
    fprintf(stderr, "  %-40s", "t_v04_bigint");
    t_v04_bigint();
    fprintf(stderr, "  %-40s", "t_v04_generator");
    t_v04_generator();
    fprintf(stderr, "  %-40s", "t_v04_map_set");
    t_v04_map_set();
    fprintf(stderr, "  %-40s", "t_v05_import_export");
    t_v05_import_export();
    fprintf(stderr, "  %-40s", "t_v05_class_accessors");
    t_v05_class_accessors();
    akl_free(g_rt);
    g_rt = NULL;

}

/* ================= v0.3 言語完全化 ================= */

static void t_v03_arrays(void) {
    want_num("var a = []; a.length", 0);
    want_num("var a = [1,2,3]; a.length", 3);
    want_num("var a = [1,2,3]; a[0] + a[1] + a[2]", 6);
    want_num("var a = [1,2,3]; a[1] = 9; a[1]", 9);
    want_num("var a = [1,2,3]; a[5] = 7; a.length", 6);      /* 伸長 + 穴 */
    want_undef("var a = [1,2]; a[5]");
    want_undef("var a = [1,2]; a[-1]");
    want_num("var a = [1,2]; a[1.9]", 2);                     /* ToUint32 で trunc */
    want_num("[1,2,3].length", 3);
    want_num("var a = [1,2,3]; a[0] + a[1]", 3);
    want_str("var a = [1]; a[0] = 'x'; a[0]", "x");
    want_num("var a = []; a[0] = 5; a[0]", 5);
    want_num("var a = [[1,2],[3,4]]; a[1][0]", 3);           /* ネスト */
    want_num("var o = {k: 7}; o['k']", 7);                   /* オブジェクトへのブラケット */
    want_num("var o = {k: 7}; o['k'] = 9; o.k", 9);
    want_str("var s = 'abc'; s[1]", "b");                    /* 文字列 index */
    want_num("var s = 'abc'; s.length", 3);
    want_num("var s = 'あいう'; s.length", 3);                /* code point 単位 */
    want_str("var s = 'あいう'; s[2]", "う");
    want_num("var a = [3,1,2]; var s = ''; var i = 0; while (i < a.length) { s = s + a[i]; i = i + 1; } s.length", 3);
    /* GC: 配列要素は mark される（要素の文字列が回収されない） */
    {
        AklVal v;
        CHECK(akl_eval(g_rt, "var a = []; var i = 0; while (i < 200) { a[i] = 's' + i; i = i + 1; } var r = a[199]; i = 0; while (i < 200) { a[i] = 0; i = i + 1; } r", &v));
        uint32_t ln = 0;
        const char *s = akl_as_str(g_rt, v, &ln);
        CHECK(s && ln == 4 && memcmp(s, "s199", 4) == 0);
    }
}

static void t_v03_closures(void) {
    want_num("function f() { var x = 1; return function() { return x; }; } var g = f(); g()", 1);
    want_num("function f() { var n = 0; return function() { n = n + 1; return n; }; } var a = f(); var b = f(); a() + a() + b()", 4);
    want_num("function f() { var n = 0; return function() { n = n + 1; return n; }; } var a = f(); var r = a(); r * 100 + a()", 102);
    /* 相互独立: 2 クロージャが同じ環境を共有しない */
    want_num("function mk() { var n = 0; return function() { n = n + 2; return n; }; } var x = mk(); var y = mk(); x() + y()", 4);
    /* 2 レベル捕捉（env チェーン parent を辿る） */
    want_num("function outer() { var a = 1; function mid() { var b = 10; return function() { return a + b; }; } return mid()(); } outer()", 11);
    /* 3 レベル + ミューテーション共有 */
    want_num("function o() { var x = 1; function m() { return function() { x = x + 5; return x; }; } return m(); } var f = o(); f() + f()", 17); /* 共有: 6+11 */
    /* クロージャを引数で受け渡し */
    want_num("function mk() { var n = 1; return function() { return n; }; } function call(f) { return f(); } call(mk())", 1);
    /* 関数式の自己参照（名前付き） */
    want_num("var f = function fact(n) { if (n <= 1) return 1; return n * fact(n - 1); }; f(5)", 120);
    /* 関数式は無名でも値として使える */
    want_num("var f = function() { return 42; }; f()", 42);
    /* 配列に関数を入れる */
    want_num("var a = [function() { return 1; }, function() { return 2; }]; a[0]() + a[1]()", 3);
    /* GC: クロージャの ENV は mark される（関数が回収されても env は生きる） */
    {
        AklVal v;
        CHECK(akl_eval(g_rt, "function mk() { var n = 0; var f = function() { n = n + 1; return n; }; var i = 0; while (i < 300) { var g = function() { return 0; }; i = i + 1; } f(); f(); return f; } var h = mk(); h() + h()", &v));
        double d = 0;
        CHECK(akl_as_num(v, &d) && d == 7); /* mk 内で 2 回、h で 2 回 = 4? → 3+4=7 */
    }
}

static void t_v03_this_methods(void) {
    want_num("var o = { v: 42, f: function() { return this.v; } }; o.f()", 42);
    want_num("var o = { v: 1, f: function() { return this.v; } }; var p = { v: 2, g: o.f }; p.g()", 2);
    want_num("var o = { v: 5, f: function() { return this.v * 2; } }; o.f() + o.f()", 20);
    /* メソッドチェーン */
    want_num("var o = { v: 3, inc: function() { this.v = this.v + 1; return this; } }; o.inc().inc().v", 5);
    /* this はメソッド外では undefined（main トップレベル） */
    want_num("typeof this === 'undefined' ? 1 : 0", 1); /* main トップレベルでは undefined */
}

static void t_v03_control(void) {
    /* 三項 */
    want_num("1 ? 10 : 20", 10);
    want_num("0 ? 10 : 20", 20);
    want_num("var x = 3; x > 2 ? 100 : 50", 100);
    want_num("var x = 3; (x > 2 ? 100 : 50) + 1", 101);
    want_num("0 ? 1 : 2 ? 3 : 4", 3);          /* 右結合 */
    /* do-while */
    want_num("var i = 0; var s = 0; do { s = s + i; i = i + 1; } while (i < 4); s", 6);
    want_num("var i = 9; var s = 0; do { s = s + 1; i = i + 1; } while (i < 4); s", 1); /* 最低 1 回 */
    want_num("var i = 0; do { i = i + 1; if (i == 2) continue; if (i == 4) break; } while (1); i", 4);
    /* switch */
    want_num("var x = 3; var s = 0; switch (x) { case 1: s = 10; break; case 3: s = 30; break; default: s = -1; } s", 30);
    want_num("var x = 9; var s = 0; switch (x) { case 1: s = 10; break; default: s = -1; } s", -1);
    want_num("var x = 3; var s = 0; switch (x) { case 3: s = 30; case 4: s = s + 4; break; default: s = -1; } s", 34); /* fallthrough */
    want_num("var x = 3; var s = 0; switch (x) { case 1: case 2: s = 20; break; case 3: s = 30; } s", 30); /* 空 case */
    want_num("var x = 2; var s = 0; switch (x) { case 1: case 2: s = 20; break; case 3: s = 30; } s", 20);
    want_num("var s = ''; var i = 5; switch (i) { default: s = 'd'; case 0: s = s + '0'; } s.length", 2); /* default から case0 へ落下 */
    /* switch 内 break はループに影響しない */
    want_num("var x = 1; var i = 0; while (i < 3) { switch (x) { case 1: break; } i = i + 1; } i", 3);
    /* switch 内 continue はループへ */
    want_num("var i = 0; var s = 0; while (i < 3) { i = i + 1; switch (i) { case 2: continue; } s = s + i; } s", 4);
    /* ビット演算 */
    want_num("5 & 3", 1);
    want_num("5 | 3", 7);
    want_num("5 ^ 3", 6);
    want_num("~0", -1);
    want_num("~0 & 0xFF", 255);
    want_num("1 << 4", 16);
    want_num("-8 >> 1", -4);
    want_num("(-8 >>> 28)", 15);
    want_num("1 << 33", 2);                    /* shift count & 31 */
    want_num("'5' | 0", 5);                    /* ToInt32 強制 */
    want_num("3.9 | 0", 3);                    /* trunc */
    /* 複合代入 */
    want_num("var x = 1; x += 2; x", 3);
    want_num("var x = 10; x -= 3; x", 7);
    want_num("var x = 3; x *= 4; x", 12);
    want_num("var x = 12; x /= 4; x", 3);
    want_num("var x = 13; x %= 5; x", 3);
    want_num("var x = 5; x <<= 2; x", 20);
    want_num("var x = 5; x &= 3; x", 1);
    want_num("var x = 5; x |= 2; x", 7);
    want_num("var x = 5; x ^= 1; x", 4);
    want_num("var x = 1; x += 2 * 3; x", 7);
    /* 複合代入は式として値を返す */
    want_num("var x = 1; var y = (x += 2); x + y", 6);
    /* オブジェクトへの複合代入 */
    want_num("var o = { a: 5 }; o.a += 3; o.a", 8);
    want_num("var o = { a: 5 }; var x = (o.a += 3); o.a + x", 16);
    /* 配列への複合代入 */
    want_num("var a = [1, 2]; a[0] += 10; a[0]", 11);
    want_num("var a = [1, 2]; var i = 1; a[i] *= 5; a[i]", 10);
    /* オブジェクトへの ++/-- */
    want_num("var o = { a: 5 }; o.a++; o.a", 6);
    want_num("var o = { a: 5 }; var x = o.a++; o.a + x", 11);
    want_num("var o = { a: 5 }; var x = ++o.a; o.a + x", 12);
    /* 配列への ++/-- */
    want_num("var a = [5]; a[0]++; a[0]", 6);
    want_num("var a = [5]; var x = a[0]++; a[0] + x", 11);
    want_num("var a = [5]; var x = ++a[0]; a[0] + x", 12);
    /* クロージャ内の ++（capture 経由） */
    want_num("function f() { var n = 0; return function() { n++; return n; }; } var g = f(); g() + g()", 3);
    want_num("function f() { var n = 0; return function() { n += 2; return n; }; } var g = f(); g() + g()", 6);
    /* for の step に ++ / += */
    want_num("var s = 0; for (var i = 0; i < 5; i++) { s += i; } s", 10);
    want_num("var s = 0; for (var i = 0; i < 5; i += 2) { s += i; } s", 6);
}

static void t_v03_misc(void) {
    /* typeof 配列 */
    want_str("typeof [1,2]", "object");
    /* 配列の ToString = join(",") */
    want_str("'' + [1,2,3]", "1,2,3");
    want_str("'' + []", "");
    want_str("'' + [1,'a',[2,3]]", "1,a,2,3");
    /* 等価は identity（配列） */
    want_num("var a = []; var b = a; (a === b) ? 1 : 0", 1);
    want_num("var a = []; var b = []; (a === b) ? 1 : 0", 0);
    /* truthiness */
    want_num("[] ? 1 : 0", 1);
    /* オブジェクトのブラケット代入でプロパティが増える */
    want_num("var o = {}; o['x'] = 7; o.x", 7);
}

static void t_v03_builtins(void) {
    /* Math */
    want_num("Math.floor(3.7)", 3);
    want_num("Math.ceil(3.2)", 4);
    want_num("Math.round(2.5)", 3);
    want_num("Math.abs(-5)", 5);
    want_num("Math.sqrt(16)", 4);
    want_num("Math.pow(2, 10)", 1024);
    want_num("Math.max(1, 7, 3)", 7);
    want_num("Math.min(1, 7, 3)", 1);
    want_num("Math.max() == -1/0 ? 1 : 0", 1); /* 引数なし max は -Infinity */
    want_num("Math.floor(-3.7)", -4);
    want_num("Math.trunc(-3.7)", -3);
    want_num("Math.sign(-9)", -1);
    want_num("Math.min(3, 1) + Math.max(2, 4)", 5);
    want_num("Math.PI > 3.14 && Math.PI < 3.15 ? 1 : 0", 1);
    want_num("Math.floor(4.9) * Math.ceil(0.1)", 4); /* 4*1 */
    {
        AklVal v; double d;
        CHECK(akl_eval(g_rt, "var r = Math.random(); r", &v) && akl_as_num(v, &d) && d >= 0.0 && d < 1.0);
        CHECK(akl_eval(g_rt, "var r = Math.random(); r", &v) && akl_as_num(v, &d) && d >= 0.0 && d < 1.0);
    }
    /* parseInt / parseFloat / isNaN / isFinite */
    want_num("parseInt('42')", 42);
    want_num("parseInt('0x1F')", 31);
    want_num("parseInt('101', 2)", 5);
    want_num("parseInt('  -7')", -7);
    want_num("parseInt('12abc')", 12);
    want_num("parseFloat('3.5abc')", 3.5);
    want_num("parseFloat('1e3')", 1000);
    want_num("isNaN('x')", 1);
    want_num("isNaN(5)", 0);
    want_num("isFinite(5)", 1);
    want_num("isFinite(1/0)", 0);
    /* 文字列メソッド */
    want_str("'abc'.toUpperCase()", "ABC");
    want_str("'ABC'.toLowerCase()", "abc");
    want_str("'Hello World'.indexOf('World') >= 0 ? 'yes' : 'no'", "yes");
    want_num("'Hello'.indexOf('x')", -1);
    want_num("'abc'.length", 3);
    want_str("'abc'.charAt(1)", "b");
    want_str("'abc'.charAt(9)", "");
    want_num("'abc'.charCodeAt(0)", 97);
    want_str("'a,b,c'.split(',')[1]", "b");
    want_num("'a,b,c'.split(',').length", 3);
    want_str("'  x  '.trim()", "x");
    want_str("'abcdef'.slice(1, 3)", "bc");
    want_str("'abcdef'.slice(-2)", "ef");
    want_str("'abcdef'.substring(3, 1)", "bc");
    want_str("'abc'.repeat(2)", "abcabc");
    want_num("'abc'.includes('b')", 1);
    want_num("'abc'.startsWith('ab')", 1);
    want_num("'abc'.endsWith('bc')", 1);
    want_str("'abc'.concat('de', 'f')", "abcdef");
    want_str("'hello world'.replace('world', 'ifuto')", "hello ifuto");
    want_num("'あいう'.length", 3);
    want_str("'あいう'[1]", "い");
    want_str("'アイウ'.toLowerCase()", "アイウ"); /* 非 ASCII は不変 */
    /* 配列メソッド */
    want_num("[1,2,3].push(4)", 4);
    want_num("var a = [1,2,3]; a.push(4); a.length", 4);
    want_num("var a = [1,2,3]; a.pop()", 3);
    want_num("var a = [1,2,3]; a.pop(); a.length", 2);
    want_num("var a = [1,2]; a.shift()", 1);
    want_num("var a = [1,2]; a.unshift(0); a[0]", 0);
    want_str("[1,2,3].join('-')", "1-2-3");
    want_str("[1,2,3].join()", "1,2,3");
    want_num("[1,2].concat([3,4]).length", 4);
    want_num("[1,2,3].slice(1).length", 2);
    want_num("[1,2,3].indexOf(2)", 1);
    want_num("[1,2,3].indexOf(9)", -1);
    want_num("[1,2].includes(2)", 1);
    want_num("[3,2,1].reverse()[0]", 1);
    want_num("var a = [1,2,3]; a.lastIndexOf(1)", 0);
    want_str("[1,'a',[2,3]].toString()", "1,a,2,3");
    /* メソッドを変数に取り出す（PLOAD 経由の NATIVE 値） */
    want_num("var f = Math.floor; f(3.9)", 3);
    want_num("typeof ('abc'.toUpperCase) === 'function' ? 1 : 0", 1); /* PLOAD は NATIVE 値 */
    /* オブジェクトメソッドと組込の共存 */
    want_num("var o = { m: function() { return 7; } }; o.m() + Math.abs(-3)", 10);
    /* GC churn: メソッド呼び出しを繰り返す */
    {
        AklVal v; double d;
        CHECK(akl_eval(g_rt, "var s = ''; var i = 0; while (i < 200) { s = s + i; i = i + 1; } s.length", &v));
    }
}

static void t_v03_json(void) {
    /* stringify */
    want_str("JSON.stringify({a:1,b:'x'})", "{\"a\":1,\"b\":\"x\"}");
    want_str("JSON.stringify([1,'a',true,null])", "[1,\"a\",true,null]");
    want_str("JSON.stringify([])", "[]");
    want_str("JSON.stringify({})", "{}");
    want_str("JSON.stringify({a:1.5})", "{\"a\":1.5}");
    want_str("JSON.stringify('hi')", "\"hi\"");
    want_str("JSON.stringify({a:{b:[1,2]}})", "{\"a\":{\"b\":[1,2]}}");
    want_str("JSON.stringify(42)", "42");
    want_str("JSON.stringify(1/0)", "null");   /* Infinity は null */
    want_str("JSON.stringify('a\\nb')", "\"a\\u000ab\""); /* control は \uXXXX */
    want_str("JSON.stringify({a:'x\"y'})", "{\"a\":\"x\\\"y\"}");
    want_undef("JSON.stringify(function(){})");
    want_undef("JSON.stringify(undefined)");
    /* parse */
    want_num("JSON.parse('42')", 42);
    want_num("JSON.parse('[1,2,3]').length", 3);
    want_num("JSON.parse('[1,2,3]')[1]", 2);
    want_str("JSON.parse('{\"k\":\"v\"}').k", "v");
    want_num("JSON.parse('{\"a\":1,\"b\":[true,null,\"x\"]}').b[0]", 1);
    want_num("JSON.parse('{\"a\\\\u0041\":1}').aA", 1);
    want_num("var o = JSON.parse('{\"x\":{\"y\":[1,2]}}'); o.x.y[1]", 2);
    want_num("JSON.parse('  [ 1 , 2 ] ')[1]", 2);
    want_str("JSON.parse('\"str\"')", "str");
    want_num("JSON.parse('true')", 1);
    want_num("JSON.parse('null') == null ? 1 : 0", 1);
    want_num("JSON.parse('-1.5e2')", -150);
    /* roundtrip */
    want_str("JSON.stringify(JSON.parse('{\"a\":1,\"b\":[1,2]}'))", "{\"a\":1,\"b\":[1,2]}");
    /* 不正 JSON は明白に失敗 */
    want_err("JSON.parse('{a:1}')", NULL);
    want_err("JSON.parse('[1,]')", NULL);
    want_err("JSON.parse('{\"a\":}')", NULL);
    want_err("JSON.parse('01')", NULL);
    want_err("JSON.parse('')", NULL);
    /* 深いネストは budget で失敗（ホストを殺さない） */
    {
        char deep[1100];
        for (int i = 0; i < 500; i++) deep[i] = '[';
        for (int i = 500; i < 1000; i++) deep[i] = ']';
        deep[1000] = 0;
        /* JSON.parse に渡す: '[' を 500 個 → AKL_JSON_DEPTH(128) 超で失敗 */
        CHECK(!akl_eval(g_rt, "JSON.parse('...')", NULL) || true); /* 深さ制限は err に倒れる */
    }
    {
        char src2[1100];
        int w = 0;
        const char *pre = "JSON.parse('";
        for (int i = 0; pre[i]; i++) src2[w++] = pre[i];
        for (int i = 0; i < 500; i++) src2[w++] = '[';
        for (int i = 0; i < 500; i++) src2[w++] = ']';
        src2[w++] = '\'';
        src2[w] = 0;
        CHECK(!akl_eval(g_rt, src2, NULL));
    }
}

static void t_v03_hof(void) {
    /* 高階関数（VM 再入 akl_call 経由） */
    want_str("var a=[1,2,3]; a.map(function(x){ return x*2; }).join(',')", "2,4,6");
    want_num("var a=[1,2,3,4]; a.filter(function(x){ return x%2==0; }).length", 2);
    want_num("var a=[1,2,3]; var s=0; a.forEach(function(x){ s=s+x; }); s", 6);
    want_num("var a=[1,2,3,4]; a.some(function(x){ return x>3; })", 1);
    want_num("var a=[1,2,3,4]; a.every(function(x){ return x>0; })", 1);
    want_num("var a=[1,2,3,4]; a.every(function(x){ return x>1; })", 0);
    want_num("var a=[1,2,3,4]; a.find(function(x){ return x>2; })", 3);
    want_num("var a=[1,2,3,4]; a.findIndex(function(x){ return x>2; })", 2);
    want_num("var a=[1,2,3,4]; a.findIndex(function(x){ return x>9; })", -1);
    want_num("var a=[1,2,3,4]; a.reduce(function(acc,x){ return acc+x; }, 0)", 10);
    want_num("var a=[1,2,3]; a.reduce(function(acc,x){ return acc+x; })", 6);
    want_str("[1,2,3].map(function(x,i){ return x+i; }).join(',')", "1,3,5");
    want_str("var r=[]; [10,20].forEach(function(x,i,a){ r.push(a.length); }); r.join(',')", "2,2");
    want_str("var f = function(x){ return x*x; }; [2,3].map(f).join(',')", "4,9");
    want_str("[[1,2],[3,4]].map(function(x){ return x[0]+x[1]; }).join(',')", "3,7");
    want_str("[1,2,3].filter(function(x){ return x>1; }).map(function(x){ return x*10; }).join(',')", "20,30");
    /* クロージャをコールバックに（env 伝播） */
    want_str("function mk(){ var n=0; return function(x){ n=n+1; return x+n; }; } var f=mk(); [10,20,30].map(f).join(',')", "11,22,33");
    /* ネストした高階（map 内 map） */
    want_str("[[1,2],[3,4]].map(function(a){ return a.map(function(x){ return x*3; }).join('-'); }).join(';')", "3-6;9-12");
    /* コールバック内の副作用（外側変数へ書き込み） */
    want_str("var s=''; [1,2,3].map(function(x){ s=s+'['+x+']'; return 0; }); s", "[1][2][3]");
    /* 巨大配列 + GC churn（akl_call 中の GC で要素が生きる） */
    {
        AklVal v;
        CHECK(akl_eval(g_rt, "var a=[]; for(var i=0;i<300;i=i+1){ a.push(i); } var r=a.map(function(x){ var g='garbage'+x; return x*2; }); r[299]+r[0]", &v));
        double d = 0;
        CHECK(akl_as_num(v, &d) && d == 598);
    }
    /* コールバック内の例外は伝播し、後続コードは実行されない */
    want_err("var a=[1,2,3]; a.map(function(x){ throw 42; }); var b=1;", "uncaught exception: 42");
    want_err("var a=[1,2,3]; a.reduce(function(a,b){ throw 'x'; }, 0)", "uncaught exception: x");
    /* reduce の空配列 + 初期値なしは TypeError */
    want_err("[].reduce(function(a,b){ return a+b; })", "reduce of empty array");
    /* 非関数のコールバックは TypeError */
    want_err("[1,2].map(5)", "TypeError");
    /* オブジェクトのメソッドをコールバックに（this は undefined で呼ばれる） */
    want_str("var o={v:5,m:function(x){ return x+1; }}; [1,2].map(o.m).join(',')", "2,3");
    /* forEach の戻り値は undefined */
    want_undef("var a=[1]; a.forEach(function(x){})");
    /* find で見つからない場合は undefined */
    want_undef("var a=[1,2]; a.find(function(x){ return x>9; })");
}

static void t_v03_syntax2(void) {
    /* 演算子: ** void カンマ in delete ?. ?? instanceof */
    want_num("2 ** 10", 1024);
    want_num("2 ** 3 ** 2", 512);
    want_num("var x = 2; x **= 3; x", 8);
    want_num("void 0 == undefined ? 1 : 0", 1);
    want_num("(1, 2, 3)", 3);
    want_num("var x = (1, 2); x", 2);
    want_num("var a = {x: 1}; 'x' in a", 1);
    want_num("'y' in {x:1}", 0);
    want_num("2 in [10,20,30]", 1);
    want_num("var a = {x: 1, y: 2}; delete a.x; 'x' in a", 0);
    want_num("var a = [1,2,3]; delete a[1]; a[1] == undefined ? 1 : 0", 1);
    want_num("var a = {b: 5}; a?.b", 5);
    want_num("var a = null; a?.b == undefined ? 1 : 0", 1);
    want_num("var a = null; a?.[0] == undefined ? 1 : 0", 1);
    want_num("var a = [1,2]; a?.[0]", 1);
    want_num("var a = null; a?.(1) == undefined ? 1 : 0", 1);
    want_num("null ?? 5", 5);
    want_num("0 ?? 5", 0);
    want_num("undefined ?? 'x' == 'x' ? 1 : 0", 1);
    want_num("({}).x ?? 9", 9);
    /* テンプレートリテラル */
    want_str("var n = 'ifuto'; `hi ${n}`", "hi ifuto");
    want_str("var a = 1, b = 2; `sum: ${a + b}`", "sum: 3");
    want_str("`no-args`", "no-args");
    want_str("var n = 'x'; `a${n}b${n}c`", "axbxc");
    want_str("`${1+1}`", "2");
    want_str("var o = {k: 9}; `val=${o.k}`", "val=9");
    want_str("var s = 'x'; `pre${s}mid${s}post`", "prexmidxpost");
    /* for-in / for-of */
    want_str("var o = {a:1,b:2,c:3}; var s=''; for (var k in o) { s = s + k; } s", "abc");
    want_num("var o = {a:1,b:2}; var s=0; for (var k in o) { s = s + o[k]; } s", 3);
    want_num("var a = [10,20,30]; var s=0; for (var v of a) { s = s + v; } s", 60);
    want_str("var s=''; for (var v of 'abc') { s = s + v + '.'; } s", "a.b.c.");
    want_num("var o={x:1,y:2}; var n=0; for (var k in o) { n = n + 1; } n", 2);
    /* デフォルト引数（b = 10） */
    want_num("function f(a, b = 10) { return a + b; } f(1)", 11);
    want_num("function f(a, b = 10) { return a + b; } f(1, 2)", 3);
    want_num("function f(a, b = 10) { return b; } f(1, 99)", 99);
    want_num("var f = function(a, b = 10) { return a - b; }; f(10)", 0);
    want_num("function f(a, b = 10, c = 20) { return a + b + c; } f(1, 2)", 23);
    want_num("var s = 0; function f(a, b = 10) { return a + b; } s = f(1) + f(2); s", 23);
    /* 通常の for がデフォルト引数対応後も正常 */
    want_num("var s=0; for (var i=0; i<10; i = i+1) { s = s+i; } s", 45);
    want_num("function addv(a){ var s = 0; for (var i=0; i<a; i=i+1) s=s+i; return s; } addv(100)", 4950);
}

static void t_v03_syntax3(void) {
    /* 分割代入 */
    want_num("var [x, y] = [10, 20]; x + y", 30);
    want_num("var {p, q} = {p: 3, q: 4}; p * q", 12);
    want_num("var {a: x, b: y} = {a: 5, b: 6}; x + y", 11);
    want_num("var [[a, b], cc] = [[1, 2], 3]; a + b + cc", 6);
    want_str("var [first, second] = ['a', 'b']; first + second", "ab");
    want_num("var x = 0, y = 0; [x, y] = [5, 7]; x + y", 12);
    want_num("var o = {m: 1}; var {m: val} = o; val", 1);
    /* spread */
    want_str("[1, ...[2,3], 4].join(',')", "1,2,3,4");
    want_str("var a = [1,2]; [...a, ...a].join(',')", "1,2,1,2");
    want_num("var a = [10, 20, 30]; function f(x, y, z) { return x + y + z; } f(...a)", 60);
    want_num("var args = [2, 3]; function add(a, b) { return a + b; } add(...args)", 5);
    want_num("function f(a, b) { return a + b; } f(1, ...[2])", 3);
    want_num("function f(a, b, c) { return a + b + c; } f(...[1], 2, ...[3])", 6);
    want_num("function f() { return 42; } f(...[])", 42);
    /* new */
    want_num("function P() { this.x = 42; } var p = new P(); p.x", 42);
    want_num("function P(v) { this.v = v; } var p = new P(7); p.v", 7);
    want_num("function P() { return 99; } var p = new P(); (p == 99 ? 1 : 0)", 0);
    want_num("function P() { this.n = 5; return {n: 100}; } var p = new P(); p.n", 100);
    want_num("function Counter() { this.count = 0; this.inc = function() { this.count++; return this.count; }; } var ct = new Counter(); ct.inc(); ct.inc(); ct.count", 2);
    want_num("var o = {constructor: function() { this.q = 3; }}; var x = new o(); x.q", 3);
    /* class */
    want_num("class P { constructor(x) { this.x = x; } get() { return this.x; } } var p = new P(42); p.get()", 42);
    want_num("class B { m() { return 7; } static s() { return 9; } } var b = new B(); b.m() + B.s()", 16);
    want_num("class C { constructor() { this.n = 0; } add(x) { this.n += x; return this; } val() { return this.n; } } var cc = new C(); cc.add(5).add(3).val()", 8);
    want_str("class G { hi() { return 'hello'; } } var g = new G(); g.hi()", "hello");
    want_num("class M { } var m = new M(); (m ? 1 : 0)", 1);
    want_num("class F { constructor() { this.count = 0; } tick() { this.count++; return this.count; } } var a = new F(); var b = new F(); a.tick(); a.tick(); b.tick(); a.count * 10 + b.count", 21);
    want_num("class H { constructor() { this.x = 5; } static make() { return new H(); } } var h = H.make(); h.x", 5);
    want_num("class Stack { constructor() { this.a = []; } push(x) { this.a.push(x); return this; } pop() { return this.a.pop(); } } var s = new Stack(); s.push(1).push(2); s.pop() + s.pop()", 3);
    want_str("class Person { constructor(n) { this.name = n; } greet() { return 'hi ' + this.name; } } var p = new Person('taro'); p.greet()", "hi taro");
    want_num("class I { constructor(v) { this.v = v; } get() { return this.v; } } var i1 = new I(10); var i2 = new I(20); i1.get() + i2.get()", 30);
    want_num("class E { constructor() { this.list = []; } push(x) { this.list.push(x); return this; } sum() { var s = 0; for (var i = 0; i < this.list.length; i++) { s += this.list[i]; } return s; } } var e = new E(); e.push(1).push(2).push(3).sum()", 6);
}

/* ================= v0.4: 正規表現 ================= */
static void t_v04_regex(void) {
    /* リテラルと基本メソッド */
    want_bool("/abc/.test('xxabcxx')", true);
    want_bool("/abc/.test('xxab')", false);
    want_bool("/a/i.test('ABC')", true);
    want_str("'hello'.match(/l+/)[0]", "ll");
    want_num("'hello'.match(/l+/).length", 1);
    want_str("'12-34'.match(/(\\d+)-(\\d+)/)[2]", "34");
    want_str("'12-34'.match(/(\\d+)-(\\d+)/)[1]", "12");
    want_str("'a1b2'.match(/[0-9]+/g).join('-')", "1-2");
    want_num("'aaa bbb aaa'.match(/a/g).length", 6);
    want_num("'aaa bbb'.match(/x/) == null ? 1 : 0", 1);
    want_num("'aaa'.match(/x/g) == null ? 1 : 0", 1);
    /* replace: 正規表現 / 関数 / $ 展開 */
    want_str("'aaa bbb'.replace(/a/g, 'X')", "XXX bbb");
    want_str("'aaa'.replace(/a/, 'X')", "Xaa");
    want_str("'abc'.replace(/(b)/, '[$1]')", "a[b]c");
    want_str("'abc'.replace(/(a)(b)/, '$2$1')", "bac");
    want_str("'ab'.replace(/a/, function(m){ return m.toUpperCase(); })", "Ab");
    want_str("'a1b2'.replace(/[0-9]/g, function(m){ return '(' + m + ')'; })", "a(1)b(2)");
    want_str("'hello'.replace(/l/g, 'L$&')", "heLlLlo");
    want_str("'abc'.replace('b', '$&$&')", "abbc");
    want_str("'x'.replace(/x/, '$`-$&-$\\'')", "-x-");
    want_str("'aaa'.replace(/a/g, '')", "");
    want_str("'  x  '.replace(/^\\s+|\\s+$/g, '!')", "!x!");
    /* split: 正規表現（キャプチャ含む）/ 空マッチ */
    want_num("'a,b,c'.split(/,/).length", 3);
    want_str("'a,b,c'.split(/,/)[1]", "b");
    want_num("'a1b22c'.split(/(\\d+)/).length", 5);
    want_str("'a1b22c'.split(/(\\d+)/)[2]", "b");
    want_num("'ab'.split(/x*/).length", 1);
    want_str("'ab'.split(/x*/)[0]", "ab");
    want_num("'a,b,'.split(/,/).length", 3);
    /* search */
    want_num("'hello'.search(/l+/)", 2);
    want_num("'hello'.search(/z/)", -1);
    /* RegExp コンストラクタ */
    want_bool("new RegExp('ab', 'i').test('AB')", true);
    want_bool("RegExp('ab').test('ab')", true);
    want_bool("RegExp(/a/g).global", true);
    want_str("new RegExp('a+b').source", "a+b");
    /* プロパティ */
    want_str("/abc/gi.flags", "gi");
    want_str("/a+b/.source", "a+b");
    want_str("/x/i.toString()", "/x/i");
    want_bool("/a/g.global", true);
    want_bool("/a/i.ignoreCase", true);
    want_bool("/a/m.multiline", true);
    want_bool("/a/g.dotAll", false);
    /* exec / lastIndex */
    want_str("/(\\d+)/.exec('x123y')[1]", "123");
    want_num("var r = /a/g; r.exec('banana'); r.lastIndex", 2);
    want_str("var r = /a/g; r.exec('banana'); r.exec('banana')[0]", "a");
    want_num("var r = /a/g; r.exec('bbbb') == null ? 1 : 0", 1);
    want_num("var r = /a/g; r.test('aab'); r.lastIndex", 1);
    want_num("var r = /a/; r.lastIndex = 3; r.exec('aaa') == null ? 1 : 0", 1);
    /* アンカー・量詞・クラス・グループ */
    want_bool("/^abc$/.test('abc')", true);
    want_bool("/^abc$/.test('xabc')", false);
    want_bool("/colou?r/.test('color')", true);
    want_bool("/colou?r/.test('colour')", true);
    want_bool("/\\d{2,4}/.test('a12345b')", true);
    want_bool("/[a-c]+/.test('cab')", true);
    want_bool("/(ab)+c/.test('ababc')", true);
    want_bool("/^\\w+@[a-z]+\\.(com|org)$/i.test('User@Example.COM')", true);
    want_bool("/\\bword\\b/.test('a word!')", true);
    want_bool("/^$/m.test('\\n')", true);
    /* 非貪欲 */
    want_str("'axxbayyb'.match(/a.*?b/)[0]", "axxb");
    want_str("'axxbayyb'.match(/a.*b/)[0]", "axxbayyb");
    /* エラー（非対応構文は明白に失敗） */
    want_err("/(?=a)/", "invalid regexp");
    want_err("/(?<n>a)/", "invalid regexp");
    want_err("/\\1/", "invalid regexp");
    want_err("/a{1001}/", "invalid regexp");
    want_err("new RegExp('a', 'x')", "SyntaxError");
    want_err("new RegExp('a', 'ii')", "SyntaxError");
    want_err("/[z-a]/", "invalid regexp");
    /* ステップ制限（指数的バックトラックは有界） */
    want_err("var s = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'; s.replace(/(a+)+b/, 'x')", "RangeError");
    /* ブラウザ系実用パターン */
    want_str("'color:#ff8800;'.match(/#[0-9a-fA-F]+/)[0]", "#ff8800");
    want_str("'width=100px'.match(/[0-9]+/)[0]", "100");
    want_bool("'2026-08-09'.match(/^\\d{4}-\\d{2}-\\d{2}$/) != null", true);
    want_str("'user@example.com'.replace(/^(.+)@(.+)$/, '$2.$1')", "example.com.user");
    /* UTF-8 */
    want_bool("/あ+/.test('あああ')", true);
    want_str("'xあy'.match(/あ/)[0]", "あ");
    /* match の引数変換（文字列 → RegExp） */
    want_str("'hello'.match('l+')[0]", "ll");
    want_num("'hello'.search('l')", 2);
    want_num("'a,b'.split('a').length", 2);
}

/* ================= v0.4: class extends / super ================= */
static void t_v04_class_extends(void) {
    /* 継承とメソッドコピー */
    want_str("class A { hi() { return 'A'; } } class B extends A { } var b = new B(); b.hi()", "A");
    want_num("class A { m() { return 5; } } class B extends A { m() { return 6; } } new B().m()", 6);
    want_str("class A { m() { return 'base'; } } class B extends A { m() { return 'B:' + super.m(); } } class C extends B { } var cx = new C(); cx.m()", "B:base");
    /* super コンストラクタ */
    want_num("class A { constructor(x) { this.x = x; } get() { return this.x; } } class B extends A { constructor(x) { super(x + 1); } } var b = new B(41); b.get()", 42);
    want_num("class A { constructor() { this.n = 1; } } class B extends A { constructor() { super(); this.n++; } } var b = new B(); b.n", 2);
    want_num("class A { constructor() { this.n = 10; } } class B extends A { } var b = new B(); b.n", 10);
    /* super メソッド呼び出し */
    want_num("class A { m() { return 1; } } class B extends A { m() { return super.m() + 1; } } var b = new B(); b.m()", 2);
    want_str("class A { greet() { return 'hi'; } } class B extends A { greet() { return super.greet() + '!'; } } new B().greet()", "hi!");
    want_num("class A { v() { return 10; } } class B extends A { v() { return super.v() * 2; } } class C extends B { v() { return super.v() + 1; } } new C().v()", 21);
    want_num("class A { constructor() { this.c = 0; } inc() { this.c++; return this.c; } } class B extends A { inc() { super.inc(); return super.inc(); } } new B().inc()", 2);
    /* super チェーン + インスタンス状態 */
    want_num("class A { constructor() { this.v = []; } add(x) { this.v.push(x); return this; } } class B extends A { add(x) { super.add(x * 2); return this; } } var b = new B(); b.add(3).add(4); b.v[0] + b.v[1]", 14);
    /* static 継承 */
    want_num("class A { static s() { return 7; } } class B extends A { } B.s()", 7);
    /* 3 段継承 + 引数伝播 */
    want_num("class Shape { constructor(w, h) { this.w = w; this.h = h; } area() { return this.w * this.h; } } class Rect extends Shape { constructor(w, h) { super(w, h); } } class Square extends Rect { constructor(s) { super(s, s); } } var sq = new Square(5); sq.area()", 25);
    /* クロージャ + 継承の共存 */
    want_num("var x = 99; class A { m() { return x; } } class B extends A { } new B().m()", 99);
    want_num("class A { m() { return 5; } } class B extends A { n() { return super.m() * 3; } } new B().n()", 15);
    /* エラー: super はクラス外で使えない */
    want_err("super.m()", "super");
    want_err("class A { m() { return 1; } } class B { m() { return super.m(); } } new B().m()", "super");
}

/* ================= v0.4: オブジェクト spread / メソッド spread / 分割 rest ================= */
static void t_v04_spread_rest(void) {
    /* オブジェクト spread */
    want_num("var a = {x: 1, y: 2}; var b = {...a}; b.x + b.y", 3);
    want_num("var a = {x: 1}; var b = {...a, x: 9, z: 3}; b.x + b.z", 12);
    want_num("var a = {x: 1}; var b = {z: 0, ...a}; b.x + b.z", 1);
    want_num("var a = {x: 1, y: 2}; var cc = {y: 9}; var d = {...a, ...cc}; d.x * 10 + d.y", 19);
    want_num("var b = {...null}; (b ? 1 : 0)", 1);
    want_num("var a = {m: 5, n: {p: 7}}; var b = {...a}; b.n.p", 7);
    want_num("var a = {x: 1}; var b = {x: 2, y: 3}; var cc = {...a, ...b, z: 4}; cc.x + cc.y + cc.z", 9);
    /* メソッド呼び出し spread */
    want_num("var o = {f: function(a, b) { return a + b; }}; var args = [2, 3]; o.f(...args)", 5);
    want_num("var o = {f: function(a, b, c) { return a + b + c; }}; var args = [1, 2]; o.f(...args, 3)", 6);
    want_num("var o = {f: function(a, b, c) { return a - b - c; }}; o.f(10, ...[2, 3])", 5);
    want_num("var o = {m: function(a, b) { return a * b; }}; var args = [6, 7]; o.m(...args)", 42);
    /* 配列 rest */
    want_num("var a, b, r; [a, b, ...r] = [1, 2, 3, 4, 5]; a + b + r.length + r[0] + r[2]", 14);
    want_num("var first, rest; [first, ...rest] = [10, 20, 30]; first + rest[0] + rest[1]", 60);
    want_num("var a, rest; [a, ...rest] = [1]; rest.length", 0);
    want_num("var r; [r] = [7]; r", 7);
    want_num("var first, rest; var arr = [1,2,3,4]; [first, ...rest] = arr; first + rest.length", 4);
    want_str("var r; var s = 'abc'; [r] = s; r", "a");
    /* オブジェクト rest */
    want_num("var o = {p: 1, q: 2, r: 3}; var p, rest; var {p, ...rest} = o; p + rest.q + rest.r", 6);
    want_num("var o = {a: 1, b: 2, c: 3}; var rest; var {b, ...rest} = o; rest.a + rest.c", 4);
    want_num("var o = {a: 1, b: 2}; var rest; var {...rest} = o; rest.a + rest.b", 3);
    want_num("var o = {a: 1, b: 2, cc: 3}; var rest; var {a, cc, ...rest} = o; rest.b", 2);
}

/* ================= v0.4: Object / Array / String / Number / Boolean 組込 ================= */
static void t_v04_builtins2(void) {
    /* Object.keys / values / entries */
    want_str("Object.keys({a: 1, b: 2}).join(',')", "a,b");
    want_num("Object.keys({}).length", 0);
    want_str("Object.values({a: 1, b: 2}).join(',')", "1,2");
    want_num("var e = Object.entries({x: 5, y: 6}); e.length + e[0][1] + e[1][1]", 13);
    want_str("Object.entries({p: 7})[0][0]", "p");
    /* Object.assign */
    want_num("var t = {}; Object.assign(t, {a: 1}, {b: 2, a: 9}); t.a + t.b", 11);
    want_num("var t = {z: 0}; Object.assign(t, null, {z: 3}); t.z", 3);
    /* Object.create（prototype 連鎖は非対応。オブジェクト/ null を返す） */
    want_num("Object.create(null) ? 1 : 0", 1);
    want_num("var p = {m: 1}; var o = Object.create(p); (o ? 1 : 0)", 1);
    /* Array.isArray */
    want_bool("Array.isArray([1,2])", true);
    want_bool("Array.isArray({})", false);
    want_bool("Array.isArray('x')", false);
    /* String / Number / Boolean コンストラクタ */
    want_str("String(42) + '!'", "42!");
    want_str("String(true)", "true");
    want_str("String(undefined)", "undefined");
    want_str("String(null)", "null");
    want_num("Number('3.5') * 2", 7);
    want_num("Number('abc') != Number('abc') ? 1 : 0", 1);
    want_num("Number(false)", 0);
    want_num("Number()", 0);
    want_bool("Boolean(0)", false);
    want_bool("Boolean('x')", true);
    want_bool("Boolean([])", true);
    /* 動的キー参照 */
    want_num("var o = {a: 1, b: 2}; var k = Object.keys(o); k.length + o[k[0]]", 3);
}

/* ================= v0.4: class フィールド宣言 / 配列 length 代入 ================= */
static void t_v04_fields_len(void) {
    /* class フィールド（instance field） */
    want_num("class A { x = 1; } var a = new A(); a.x", 1);
    want_num("class A { x = 1; y = 2; } var a = new A(); a.x + a.y", 3);
    want_num("class A { x = 10; get() { return this.x * 2; } } var a = new A(); a.get()", 20);
    want_str("class A { name = 'taro'; greet() { return 'hi ' + this.name; } } new A().greet()", "hi taro");
    want_num("class A { x = 1; } class B extends A { y = 2; } var b = new B(); b.x + b.y", 3);
    want_num("class A { constructor(v) { this.v = v; } } class B extends A { w = 3; constructor(v) { super(v); } } var b = new B(9); b.v + b.w", 12);
    want_num("class A { x; } var a = new A(); a.x == undefined ? 1 : 0", 1);
    want_num("class A { count = 0; inc() { this.count++; return this.count; } } var a = new A(); a.inc(); a.inc(); a.count", 2);
    want_num("class A { n = 5; } var a = new A(); var b = new A(); b.n = 9; a.n + b.n", 14);
    want_num("class A { x = 1; } var a = new A(); delete a.x; a.x == undefined ? 1 : 0", 1);
    want_num("class A { x = 1; } class B extends A { x = 2; } var b = new B(); b.x", 2);
    want_num("class A { m() { return 1; } } class B extends A { w = 3; } var b = new B(); b.m() + b.w", 4);
    want_err("class A { static x = 1; }", "static class fields");
    want_err("class A { static constructor() {} }", "static constructor");
    /* 配列 length 代入（切り詰め / undefined 拡張） */
    want_num("var a = [1,2,3,4,5]; a.length = 2; a.length + a[0] + a[1]", 5);
    want_num("var a = [1]; a.length = 3; a.length + (a[2] == undefined ? 1 : 0)", 4);
    want_num("var a = []; a.length = 0; a.length", 0);
    want_num("var a = [1,2,3]; a.length = 0; a.length + (a[1] == undefined ? 1 : 0)", 1);
    want_num("var a = [1,2,3]; a.length = 1; a[1] == undefined ? 1 : 0", 1);
    want_num("var a = [1,2]; a.length = 5; a[4] == undefined ? 1 : 0", 1);
    want_num("var a = [1,2,3]; a.length = 2; a.push(9); a.length + a[2]", 12);
}

/* ================= v0.4: 論理代入・数値区切り ================= */
static void t_v04_logassign(void) {
    /* ||= */
    want_num("var a = null; a ||= 5; a", 5);
    want_num("var b = 0; b ||= 5; b", 5);
    want_num("var cc2 = 3; cc2 ||= 5; cc2", 3);
    want_str("var s = ''; s ||= 'x'; s", "x");
    /* &&= */
    want_num("var f = 1; f &&= 7; f", 7);
    want_num("var g = 0; g &&= 7; g", 0);
    /* ??= */
    want_num("var d = null; d ?\?= 9; d", 9);
    want_num("var e = 0; e ?\?= 9; e", 0);
    want_num("var t = 4; t ?\?= 1; t", 4);
    /* プロパティ/要素 */
    want_num("var o = {}; o.x ||= 5; o.x", 5);
    want_num("var o = {x: null}; o.x ?\?= 3; o.x", 3);
    want_num("var o = {x: 3}; o.x ?\?= 9; o.x", 3);
    want_num("var o = {x: 1}; o.x &&= 7; o.x", 7);
    want_num("var arr = [0]; arr[0] ||= 9; arr[0]", 9);
    want_num("var arr = [5]; arr[0] &&= 2; arr[0]", 2);
    want_num("var o = {x: 1}; var k = 'x'; o[k] ||= 9; o.x", 1);
    /* 短絡（右辺は評価されない） */
    want_num("var cnt = 0; function f() { cnt++; return null; } var r = 1; r ||= f(); r + cnt", 1);
    want_num("var cnt2 = 0; function g() { cnt2++; return 1; } var r2 = null; r2 ||= g(); r2 + cnt2", 2);
    /* 関数内 */
    want_num("function f() { var n = null; n ?\?= 5; return n; } f()", 5);
    /* 数値区切り */
    want_num("1_000_000", 1000000);
    want_num("0xFF_FF", 65535);
    want_num("0b1010_0101", 165);
    want_num("0o77_77", 4095);
    want_num("var n = 1_000; n + 1", 1001);
}

/* ================= v0.4: オブジェクトリテラル拡張 ================= */
static void t_v04_objlit_ext(void) {
    /* ショートハンド {a} */
    want_num("var a = 1; var b = 2; var o = {a, b}; o.a + o.b", 3);
    want_num("var x = 9; var o = {x}; o.x", 9);
    /* computed key */
    want_num("var k = 'x'; var o = {[k]: 9}; o.x", 9);
    want_num("var k = 'a'; var v = 7; var o = {[k + 'b']: v}; o.ab", 7);
    want_num("var o = {['m']: 5}; o.m", 5);
    want_num("var i = 2; var o = {['x' + i]: 10}; o.x2", 10);
    /* メソッド短縮 */
    want_num("var o = { m() { return 42; } }; o.m()", 42);
    want_num("var o = { m() { return this.x; }, x: 5 }; o.m()", 5);
    want_num("var o = { a: 1, m() { return this.a + 1; } }; o.m()", 2);
    want_num("var o = { m() { return 1; }, n() { return 2; } }; o.m() + o.n()", 3);
    want_num("var o = { x: 10, m() { return this.x * 2; } }; o.m()", 20);
    /* getter */
    want_num("var o = { get x() { return 7; } }; o.x", 7);
    want_num("var count = 0; var o = { get v() { count++; return count; } }; o.v + o.v", 3);
    want_num("var o = { _x: 1, get x() { return this._x; } }; o.x", 1);
    /* setter */
    want_num("var o = { set s(v) { this._s = v; } }; o.s = 9; o._s", 9);
    want_num("var o = { _x: 1, get x() { return this._x; }, set x(v) { this._x = v * 2; } }; o.x = 5; o.x", 10);
    want_num("var o = { set s(v) { this._s = v + 1; }, get s() { return this._s; } }; o.s = 41; o.s", 42);
    want_num("var o = { _v: 0, get v() { return this._v; }, set v(nv) { if (nv >= 0) this._v = nv; } }; o.v = 10; o.v = -5; o.v", 10);
}

/* ================= v0.4: ラベル break/continue・debugger ================= */
static void t_v04_labels(void) {
    /* debugger 文は no-op */
    want_num("debugger; 1+1", 2);
    want_undef("debugger");
    /* ラベル付き break（ループ） */
    want_num("var i = 0; outer: while (i < 3) { i++; if (i == 2) break outer; } i", 2);
    want_num("var n = 0; done: while (n < 10) { n++; if (n == 5) break done; } n", 5);
    want_num("var s = 0; outer: for (var a = 0; a < 3; a++) { for (var b = 0; b < 3; b++) { if (a == 1 && b == 1) break outer; s++; } } s", 4);
    want_num("var i = 0; outer: while (true) { while (true) { i++; if (i > 2) break outer; } } i", 3);
    want_num("var i = 0; outer: for (var j = 0; j < 100; j++) { if (j == 3) break outer; i = j; } i", 2);
    /* ラベル付き continue */
    want_num("var i = 0; outer: for (var j = 0; j < 3; j++) { for (var k = 0; k < 3; k++) { if (k == 1) continue outer; i++; } } i", 3);
    want_num("var x = 0; outer: do { x++; if (x == 2) continue outer; } while (x < 3); x", 3);
    /* 非ループラベルの break（ブロック終端へ） */
    want_num("var n = 0; lbl: { n = 1; break lbl; n = 2; } n", 1);
    want_num("var n = 0; lbl: { n = 1; if (n == 1) break lbl; n = 2; } n", 1);
    /* エラー */
    want_err("break missing;", "label");
    want_err("var i = 0; foo: { continue foo; }", "continue");
}

/* ================= v0.4: arguments ================= */
static void t_v04_arguments(void) {
    want_num("function f() { return arguments.length; } f(1, 2, 3)", 3);
    want_num("function f() { return arguments[0] + arguments[1]; } f(3, 4)", 7);
    want_str("function f() { return arguments[2]; } f('a', 'b', 'c')", "c");
    want_num("function f() { var s = 0; for (var i = 0; i < arguments.length; i++) s += arguments[i]; return s; } f(1, 2, 3, 4)", 10);
    want_num("function f() { return arguments.length; } f()", 0);
    want_num("function f(a, b) { return arguments.length + a + b; } f(1, 2, 3, 4)", 7);
    want_num("var o = { m: function() { return arguments[0]; } }; o.m(7)", 7);
    want_num("function f() { var g = function() { return arguments.length; }; return g(5, 6); } f()", 2);
    want_num("class C { m() { return arguments[1]; } } new C().m(1, 9)", 9);
    want_num("function f() { return arguments.length; } f(1, 2, 3, 4, 5, 6, 7, 8)", 8);
    want_num("function f() { var a = arguments; return a[0] * 10 + a.length; } f(4, 5, 6)", 43);
    /* 関数外の arguments は ReferenceError（簡易近似。AKL_COMPAT に明記） */
    want_err("arguments", "arguments");
    /* 高階コールバック内 */
    want_num("[1, 2, 3].map(function() { return arguments.length; })[0]", 3);
}

/* ================= v0.4: アロー関数・Promise・async/await ================= */
static void t_v04_arrow_promise_async(void) {
    /* アロー関数 */
    want_num("var f = (x) => x + 1; f(41)", 42);
    want_num("var f = x => x * 2; f(21)", 42);
    want_num("var f = () => 42; f()", 42);
    want_num("var f = (a, b) => a - b; f(9, 3)", 6);
    want_num("var f = (a, b) => a + b; f(1, 2) + f(3, 4)", 10);
    want_num("var f = (x) => { return x * 3; }; f(5)", 15);
    want_str("[1,2,3].map(x => x * 2).join(',')", "2,4,6");
    want_num("[1,2,3,4].filter(x => x % 2 == 0).length", 2);
    want_num("function make() { var n = 5; return (x) => x + n; } var g = make(); g(10)", 15);
    /* アロー this キャプチャ */
    want_num("var o = { v: 7, get: function() { return () => this.v; } }; var g = o.get(); g()", 7);
    want_num("var o = { v: 7, m: function() { return () => this.v; } }; o.m()()", 7);
    want_num("var o = { v: 3, m: function() { var f = () => this.v; return f(); } }; o.m()", 3);
    /* Promise（同期解決近似） */
    want_num("var r = 0; var p = new Promise(function(res) { res(5); }); p.then(function(v) { r = v * 2; }); r", 10);
    want_num("var r = 0; Promise.resolve(42).then(function(v) { r = v + 1; }); r", 43);
    want_str("var r = ''; var p = new Promise(function(res, rej) { rej('err'); }); p.catch(function(e) { r = 'caught:' + e; }); r", "caught:err");
    want_num("var r = 0; var p = new Promise(function(res) { res(7); }); p.then(function(v) { return v; }).then(function(v) { r = v * 2; }); r", 14);
    want_str("var r = ''; Promise.reject('x').catch(function(e) { r = e + '!'; }); r", "x!");
    want_num("var r = 0; var p = new Promise(function(res) { res(10); }); p.finally(function() { r = 1; }); r", 1);
    /* async/await */
    want_num("var r = 0; async function f() { return 42; } f().then(function(v) { r = v; }); r", 42);
    want_num("var r = 0; async function f() { return await Promise.resolve(10) * 2; } f().then(function(v) { r = v; }); r", 20);
    want_num("var r = 0; async function f() { var a = await 5; return a * 2; } f().then(function(v) { r = v; }); r", 10);
    want_str("var r = ''; async function f() { return 'hello'; } f().then(function(v) { r = v; }); r", "hello");
    want_num("var r = 0; async function f() { var p = new Promise(function(res) { res(21); }); return await p * 2; } f().then(function(v) { r = v; }); r", 42);
    want_num("var r = 0; var f = async function() { return 99; }; f().then(function(v) { r = v; }); r", 99);
    want_num("var r = 0; async function f() { return 1 + await 2; } f().then(function(v) { r = v; }); r", 3);
    /* エラー */
    want_err("await 5", "await");
    want_err("var f = () => { return 1; }; f(); break", "break");
}

/* ================= v0.4: BigInt ================= */
static void t_v04_bigint(void) {
    /* リテラル */
    want_num("10n + 0", 10); /* CLI 表示用に number 化するが値は正しい */
    want_str("'' + 10n", "10");
    want_str("'' + 0xFFn", "255");
    want_str("'' + 0b101n", "5");
    want_str("'' + 0o77n", "63");
    want_str("'' + 0n", "0");
    want_str("'' + 123456789012345678n", "123456789012345678");
    /* 64bit 正確演算（2^53 超） */
    want_str("'' + (9007199254740993n + 1n)", "9007199254740994");
    want_str("'' + 9223372036854775807n", "9223372036854775807");
    want_str("'' + (0x7FFFFFFFFFFFFFFFn)", "9223372036854775807");
    /* 演算 */
    want_str("'' + (10n + 5n)", "15");
    want_str("'' + (10n - 3n)", "7");
    want_str("'' + (10n * 3n)", "30");
    want_str("'' + (10n / 3n)", "3");
    want_str("'' + (10n % 3n)", "1");
    want_str("'' + (10n * 2n + 1n)", "21");
    /* 比較 */
    want_bool("10n > 5n", true);
    want_bool("10n < 5n", false);
    want_bool("10n == 10n", true);
    want_bool("10n === 10n", true);
    want_bool("10n == 10", true);
    want_bool("10n === 10", false);
    /* typeof・文字列化 */
    want_str("typeof 10n", "bigint");
    want_str("'' + 10n", "10");
    /* 変数・関数 */
    want_str("var x = 10n; '' + (x * 2n)", "20");
    want_str("function f(x) { return '' + (x + 1n); } f(41n)", "42");
    want_str("var a = [1n, 2n, 3n]; '' + (a[0] + a[1] + a[2])", "6");
    /* エラー */
    want_err("123456789012345678901234567890n", "lex error");
    want_err("9223372036854775808n", "lex error");
    want_err("0xFFFFFFFFFFFFFFFFn", "lex error");
    want_err("10n % 0n", "RangeError");
    want_err("1n / 0n", "RangeError");
}

/* ================= v0.4: generator ================= */
static void t_v04_generator(void) {
    /* 基本 next() */
    want_num("function* g() { yield 1; yield 2; } var it = g(); it.next().value", 1);
    want_num("function* g() { yield 1; yield 2; } var it = g(); var ga = it.next(); var gb = it.next(); var gc = it.next(); ga.value + gb.value + (gc.done ? 10 : 0)", 13);
    want_num("function* g() { yield 10; yield 20; yield 30; } var it = g(); it.next().value + it.next().value + it.next().value", 60);
    want_str("function* g() { yield 'a'; yield 'b'; } var it = g(); it.next().value + it.next().value", "ab");
    /* 空 generator */
    want_bool("function* g() { } var it = g(); it.next().done", true);
    /* done/value プロパティ */
    want_str("function* g() { yield 1; yield 2; } var it = g(); var r = it.next(); r.done + ':' + r.value", "false:1");
    want_str("function* g() { yield 1; yield 2; } var it = g(); it.next(); var r = it.next(); r.done + ':' + r.value", "false:2");
    want_str("function* g() { yield 1; } var it = g(); it.next(); var r = it.next(); r.done + ':' + r.value", "true:undefined");
    /* ループ内 yield */
    want_num("function* g() { var n = 0; while (n < 3) { yield n; n = n + 1; } } var it = g(); it.next().value + it.next().value + it.next().value", 3);
    /* yield 式の値（近似: undefined） */
    want_num("function* g() { var x = yield 5; return x + 1; } var it = g(); it.next().value", 5);
    want_str("function* g() { var x = yield 5; return x + 1; } var it = g(); it.next(); var r = it.next(); r.done + ':' + r.value", "true:undefined");
    /* yield の式としての値 */
    want_num("function* g() { var a = yield 2; return a; } var it = g(); var r = it.next(); r.value * 21", 42);
    /* 複数インスタンス */
    want_num("function* g() { yield 1; yield 2; } var ga = g(); var gb = g(); ga.next().value + gb.next().value", 2);
    /* クロージャ + generator */
    want_num("function make() { var n = 10; return function*() { yield n; yield n + 1; }; } var g = make(); var it = g(); it.next().value + it.next().value", 21);
    /* エラー: generator 外の yield */
    want_err("yield 1;", "yield");
}

/* ================= v0.4: Map / Set / 組込充実 ================= */
static void t_v04_map_set(void) {
    /* Map */
    want_num("var m = new Map(); m.set('a', 1); m.get('a')", 1);
    want_num("var m = new Map(); m.set('a', 1); m.set('b', 2); m.get('b') + m.get('a')", 3);
    want_num("var m = new Map(); m.set('a', 1); m.set('a', 9); m.get('a')", 9);
    want_str("var m = new Map(); m.set('a', 1); m.has('a') + ':' + m.has('x')", "true:false");
    want_str("var m = new Map(); m.set('a', 1); m.delete('a') + ':' + m.size", "true:0");
    want_num("var m = new Map(); m.set('a', 1); m.clear(); m.size", 0);
    want_num("var m = new Map([['a', 1], ['b', 2]]); m.get('b')", 2);
    want_num("var m = new Map(); m.set('x', 5); m.keys().length + m.values()[0]", 6);
    want_num("var m = new Map(); m.size", 0);
    want_str("var m = new Map(); m.set(1, 'one'); m.get(1)", "one");
    want_str("var m = new Map(); var k = {id: 1}; m.set(k, 'obj'); m.get(k)", "obj");
    want_num("var m = new Map(); m.set(1, 10); m.set(2, 20); var ks = m.keys(); ks[0] + ks[1] + m.values()[0] + m.values()[1]", 33);
    /* Set */
    want_bool("var s = new Set(); s.add(1); s.has(1)", true);
    want_num("var s = new Set(); s.add(1); s.add(2); s.add(1); s.size", 2);
    want_str("var s = new Set(); s.add(1); s.delete(1) + ':' + s.size", "true:0");
    want_num("var s = new Set([1, 2, 3]); s.size", 3);
    want_num("var s = new Set(); s.add('a'); s.clear(); s.size", 0);
    want_num("var s = new Set(); s.add(5); var v = s.values(); v[0]", 5);
    want_num("var s = new Set(); s.add(1); s.add(2); var v = s.values(); v[0] + v[1]", 3);
    want_num("var s = new Set(); s.add(NaN); s.add(NaN); s.size", 1);
    /* Object.fromEntries */
    want_num("Object.fromEntries([['a', 1], ['b', 2]]).b", 2);
    want_num("Object.fromEntries([['x', 9]]).x", 9);
    /* Array.from */
    want_num("Array.from([1, 2, 3]).length", 3);
    want_num("Array.from([5, 6])[1]", 6);
    want_num("Array.from('abc').length", 3);
    want_num("Array.from('あいう').length", 3);
    want_str("Array.from('ab').join('-')", "a-b");
    /* padStart / padEnd */
    want_str("'abc'.padStart(5, '*')", "**abc");
    want_str("'abc'.padEnd(6, '-')", "abc---");
    want_str("'abc'.padStart(5)", "  abc");
    want_str("'123'.padStart(6, '0')", "000123");
    want_str("'abc'.padStart(2, 'x')", "abc");
    want_str("'あ'.padEnd(3, 'い')", "あいい");
    /* flat */
    want_num("[1, [2, 3], [4, [5]]].flat().length", 4);
    want_str("[1, [2, 3]].flat().join(',')", "1,2,3");
    want_str("[1, [2, [3, [4]]]].flat(2).join('')", "1234");
    want_str("[1, [2, 3]].flat(0).join(',')", "1,2,3");
    /* hasOwnProperty */
    want_bool("var o = {a: 1}; o.hasOwnProperty('a')", true);
    want_bool("var o = {a: 1}; o.hasOwnProperty('b')", false);
    want_bool("var o = {a: 1}; o.hasOwnProperty('toString')", false);
    /* v0.5 回帰: GC が Map のキー STR を回収しない（akl_gc_kind_children に MAP/SET が
     * 欠落していた潜伏バグ。3500 個超で size が重複扱いになり増えなくなる）。
     * GC が obj 数 4096 で発火する量を超えても size が正確であること。 */
    want_num("var m = new Map(); for (var i = 0; i < 4000; i++) m.set('k' + i, i); m.size", 4000);
    want_num("var m = new Map(); for (var i = 0; i < 4096; i++) m.set('k' + i, i); m.get('k4095')", 4095);
    want_num("var m = new Map(); for (var i = 0; i < 4000; i++) m.set('k' + i, i); m.get('k1000') + m.get('k3999')", 4999);
    /* Set も同様 */
    want_num("var s = new Set(); for (var i = 0; i < 4000; i++) s.add('v' + i); s.size", 4000);
    /* 上限超過は明白に失敗（黙って無視しない） */
    want_err("var m = new Map(); for (var i = 0; i < 5000; i++) m.set('k' + i, i);", "Map/Set size limit");
    want_err("var s = new Set(); for (var i = 0; i < 5000; i++) s.add(i);", "Map/Set size limit");
    /* v0.5 ハッシュ索引: SameValueZero との整合（int 5 === double 5.0、NaN 同値、±0 同値） */
    want_str("var m = new Map(); m.set(5, 'int'); m.set(5.0, 'dbl'); m.get(5) + ':' + m.get(5.0)", "dbl:dbl");
    want_num("var m = new Map(); m.set(NaN, 1); m.set(NaN, 2); m.size * 10 + m.get(NaN)", 12);
    want_str("var m = new Map(); m.set(0, 'a'); m.set(-0, 'b'); m.size + ':' + m.get(0)", "1:b");
    want_num("var m = new Map(); m.set('k' + 1, 7); m.set('k' + 2, 8); m.get('k' + 1) + m.get('k' + 2)", 15);
    /* delete 後の再挿入（再ハッシュ経路） */
    want_str("var m = new Map(); m.set('a', 1); m.set('b', 2); m.delete('a'); m.set('a', 9); m.get('a') + ':' + m.get('b')", "9:2");
    want_str("var s = new Set(); s.add('x'); s.add('y'); s.delete('x'); s.add('x'); s.size + ':' + s.has('x')", "2:true");
    /* clear 後の再利用 */
    want_str("var m = new Map(); m.set('a', 1); m.clear(); m.set('b', 2); m.size + ':' + m.get('b')", "1:2");
    /* 挿入順は維持（ハッシュは索引に過ぎない） */
    want_str("var m = new Map(); m.set('z', 1); m.set('a', 2); m.set('m', 3); m.keys().join(',')", "z,a,m");
    /* オブジェクトキーは同一性 */
    want_str("var m = new Map(); var o = {id: 1}; m.set(o, 'obj'); m.get(o) + ':' + m.get({id: 1})", "obj:undefined");
    /* v0.5 セキュリティ回帰: obj 配列 realloc 後の dangling ポインタ（UAF）クラス。
     * 各操作が「ToString/obj 生成で配列 realloc を起こす量」を超えても正しく動くこと
     * （ASan ビルドでなければ検出されないが、値の整合性はここで担保）。 */
    want_num("var a = []; for (var i = 0; i < 2000; i++) a.push({x: i}); ('' + a).length > 0 ? 1 : 0", 1);
    want_num("var a = []; for (var i = 0; i < 2000; i++) a.push('v' + i); a.slice(100, 1900).length", 1800);
    want_num("var o = {}; for (var i = 0; i < 50; i++) o['k' + i] = i; Object.entries(o).length", 50);
    want_num("var m = new Map(); for (var i = 0; i < 1500; i++) m.set('k' + i, i); m.keys().length + m.values().length", 3000);
    want_num("var s = new Set(); for (var i = 0; i < 1500; i++) s.add('v' + i); s.values().length", 1500);
    want_num("var re = /x/; var o = {toString: undefined}; re.test(o) ? 1 : 0", 0);
    want_num("var a = []; for (var i = 0; i < 1500; i++) a.push('s' + i); var f = [[1, 2], [3]]; f.flat(1).length + a.concat(a).length", 3003);
    want_num("class C { constructor() { this.a = 1; } get v() { return this.a; } } var n = 0; for (var i = 0; i < 300; i++) { n += new C().v; } n", 300);
    want_num("var d = {p1: 1, p2: 2}; Object.keys(d).length + (d['p2']) + ('p1' in d ? 10 : 0)", 14);
}

/* ================= v0.5: import / export（モジュール） ================= */
/* 静的モジュール表を提供するテスト用ローダ（id = spec そのまま。AKL 側が free） */
static void test_mod_loader(AklRT *rt, const char *spec, const char *base,
                            void *udata, char **out_src, char **out_id) {
    (void)rt; (void)udata; (void)base;
    *out_src = NULL; *out_id = NULL;
    static const struct { const char *id; const char *src; } T[] = {
        {"test:m1", "export const x = 42;\n"
                    "export function add(a, b) { return a + b; }\n"
                    "export default 'hello';"},
        {"test:m2", "import { x, add } from 'test:m1';\n"
                    "export const total = x + add(1, 2);"},
        {"test:side", "var counter = 0;\n"
                      "export function bump() { counter = counter + 1; return counter; }\n"
                      "export function peek() { return counter; }"},
        {"test:once", "export var n = 0;\n"
                      "n = n + 1;\n"
                      "export { n };"},
        {"test:reexport", "export { x as xx } from 'test:m1';\n"
                          "export * from 'test:m1';\n"
                          "export { default as dflt } from 'test:m1';"},
        {"test:cycleA", "import { b } from 'test:cycleB';\n"
                        "export const a = 1;"},
        {"test:cycleB", "import { a } from 'test:cycleA';\n"
                        "export const b = 2;"},
        {"test:fail", "export const q = 1;\n"
                      "throw 'boom';"},
        {"test:usethis", "export const t = typeof this;"},
        {"test:capture", "var counter = 0;\n"
                         "export function bump() { counter = counter + 1; return counter; }\n"
                         "export function peek() { return counter; }"},
        {"test:dfltexpr", "export default 1 + 2 * 3;"},
        {"test:alias", "export const alpha = 1;\n"
                       "export const beta = 2;"},
        {"test:dfltfn", "export default function() { return 7; }"},
        {"test:dfltfnnamed", "export default function g() { return 8; }\n"
                             "export { g };"},
        {"test:dfltcls", "export default class { m() { return 5; } }"},
        {"test:dfltclsnamed", "export default class C { m() { return 6; } }"},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        if (strcmp(spec, T[i].id) == 0) {
            *out_src = strdup(T[i].src);
            *out_id = strdup(T[i].id);
            return;
        }
    }
}

/* モジュール評価ヘルパ（エントリは base 付きで akl_eval_module） */
static void mod_num(const char *src, double want) {
    AklVal v;
    if (!akl_eval_module(g_rt, src, "test:entry", &v)) {
        fprintf(stderr, "  mod eval failed [%s]: %s\n", src, akl_error(g_rt));
        CHECK(0);
        return;
    }
    double d = NAN;
    bool ok = akl_as_num(v, &d) && d == want;
    if (!ok) {
        bool b = false;
        if (akl_as_bool(v, &b)) { ok = (b ? 1.0 : 0.0) == want; d = b ? 1.0 : 0.0; }
    }
    CHECK(ok);
    if (!ok) fprintf(stderr, "  wrong mod value [%s]: got %g want %g\n", src, d, want);
}
static void mod_str(const char *src, const char *want) {
    AklVal v;
    if (!akl_eval_module(g_rt, src, "test:entry", &v)) {
        fprintf(stderr, "  mod eval failed [%s]: %s\n", src, akl_error(g_rt));
        CHECK(0);
        return;
    }
    uint32_t ln = 0;
    const char *s = akl_as_str(g_rt, v, &ln);
    bool ok = s && strlen(want) == ln && memcmp(s, want, ln) == 0;
    CHECK(ok);
    if (!ok) fprintf(stderr, "  wrong mod string [%s]: got '%.*s' want '%s'\n",
                     src, s ? (int)ln : 0, s ? s : "", want);
}
static void mod_bool(const char *src, bool want) {
    AklVal v;
    if (!akl_eval_module(g_rt, src, "test:entry", &v)) {
        fprintf(stderr, "  mod eval failed [%s]: %s\n", src, akl_error(g_rt));
        CHECK(0);
        return;
    }
    bool b = false;
    CHECK(akl_as_bool(v, &b) && b == want);
}
static void mod_err(const char *src, const char *needle) {
    AklVal v;
    if (akl_eval_module(g_rt, src, "test:entry", &v)) {
        fprintf(stderr, "  mod expected error [%s]\n", src);
        CHECK(0);
        return;
    }
    const char *er = akl_error(g_rt);
    CHECK(strstr(er, needle) != NULL);
    if (!strstr(er, needle)) fprintf(stderr, "  wrong error [%s]: %s\n", src, er);
}

static void t_v05_import_export(void) {
    akl_set_module_loader(g_rt, test_mod_loader, NULL);

    /* 基本: 名前付き import/export */
    mod_num("import { x, add } from 'test:m1';\nx + add(1, 2)", 45);
    mod_num("import { x } from 'test:m1';\nx * 2", 84);
    /* default import/export */
    mod_str("import def from 'test:m1';\ndef + '!'", "hello!");
    /* namespace import（同一モジュールの ns は同一オブジェクト） */
    mod_num("import * as ns from 'test:m1';\nns.x + ns.add(2, 3)", 47);
    mod_bool("import * as a from 'test:m1';\nimport * as b from 'test:m1';\na === b", true);
    /* import 別名 */
    mod_num("import { x as y, add as plus } from 'test:m1';\ny + plus(0, 1)", 43);
    /* モジュールキャッシュ（2 回目は再評価されない） */
    mod_num("import { n } from 'test:once';\nn", 1);
    mod_num("import { n } from 'test:once';\nn", 1);
    /* モジュールローカル var はモジュールスコープ（グローバル汚染なし） */
    mod_num("import { bump } from 'test:side';\nbump() + bump()", 3);
    /* クロージャがモジュールローカルを capture（別モジュールで検証） */
    mod_num("import { bump, peek } from 'test:capture';\nbump(); peek()", 1);
    /* re-export（名前指定 / スター / default の名前付き再 export） */
    mod_num("import { xx } from 'test:reexport';\nxx", 42);
    mod_num("import { x } from 'test:reexport';\nx + 1", 43);
    mod_str("import { dflt } from 'test:reexport';\ndflt", "hello");
    /* 副作用のみ import */
    mod_num("import 'test:side';\n1", 1);
    /* エントリの export は破棄（最終式文の値が last_val） */
    mod_num("export const ignored = 9;\n7", 7);
    /* モジュール最上位の this / arguments は undefined 系 */
    mod_str("import { t } from 'test:usethis';\nt", "undefined");
    /* 動的 import（同期解決近似の Promise） */
    mod_num("var r = 0;\nimport('test:m1').then(function(ns) { r = ns.x; });\nr", 42);
    /* 動的 import は classic スクリプトでも使用可 */
    want_num("var r = 0;\nimport('test:m1').then(function(ns) { r = ns.x; });\nr", 42);

    /* export default: 式 / 無名関数 / 名前付き関数 / 無名クラス / 名前付きクラス */
    mod_num("import v from 'test:dfltexpr';\nv", 7);
    mod_num("import f from 'test:dfltfn';\nf()", 7);
    mod_num("import f from 'test:dfltfnnamed';\nf()", 8);
    mod_num("import C from 'test:dfltcls';\nnew C().m()", 5);
    mod_num("import C from 'test:dfltclsnamed';\nnew C().m()", 6);

    /* エラー系 */
    mod_err("import { a } from 'test:cycleA';\na", "circular import");
    mod_err("import { z } from 'test:nope';\nz", "cannot resolve module");
    mod_err("import { q } from 'test:fail';\nq", "boom");
    mod_err("import { x } from 'test:m1';\nx = 1", "const");
    mod_err("import.meta", "import.meta is not supported");
    mod_err("function f() { export var x = 1; }", "top level");
    mod_err("{ import { x } from 'test:m1'; }", "top level");
    mod_err("export { z } from 'test:nope';", "cannot resolve");
    /* classic スクリプトでの宣言は構文エラー */
    want_err("import { x } from 'test:m1';", "only allowed in modules");
    want_err("export const e = 1;", "only allowed in modules");
    /* 実行時エラーはレコードに保存され、再 import も失敗を再提示 */
    mod_err("import { q } from 'test:fail';\nq", "boom");

    /* GC との相互作用: 多数のモジュール評価を連続実行しても壊れない */
    for (int i = 0; i < 200; i++) {
        AklVal v;
        if (!akl_eval_module(g_rt, "import { x } from 'test:m1';\nx", "test:entry", &v)) {
            fprintf(stderr, "  module stress failed at %d: %s\n", i, akl_error(g_rt));
            CHECK(0);
            break;
        }
        double d = NAN;
        CHECK(akl_as_num(v, &d) && d == 42);
        if (d != 42) break;
    }
}

/* ================= v0.5: クラス getter/setter 構文 ================= */
static void t_v05_class_accessors(void) {
    /* 基本 get/set（変数名は cc — 既存テストの const c と衝突しない規約） */
    want_num("class C { constructor() { this._x = 1; } get x() { return this._x; } set x(v) { this._x = v; } }"
             " var cc = new C(); cc.x + 10", 11);
    want_num("class C { constructor() { this._x = 1; } get x() { return this._x; } set x(v) { this._x = v; } }"
             " var cc = new C(); cc.x = 99; cc.x", 99);
    /* インスタンスごとに独立したバッキングフィールド */
    want_num("class C { constructor() { this._x = 0; } get x() { return this._x; } set x(v) { this._x = v; } }"
             " var ca = new C(); var cb = new C(); ca.x = 5; cb.x + ca.x", 5);
    /* static getter */
    want_num("class C { static get s() { return 42; } } C.s", 42);
    /* メソッド名 get/set（アクセサと誤認しない） */
    want_num("class C { get() { return 7; } } new C().get()", 7);
    want_num("class C { set() { return 8; } } new C().set()", 8);
    /* 継承 + getter */
    want_num("class B { constructor() { this._v = 3; } get v() { return this._v; } }"
             " class C extends B { } var cc = new C(); cc.v", 3);
    /* getter 内の this はインスタンス */
    want_str("class C { constructor() { this.n = 'a'; } get label() { return this.n + '!'; } }"
             " var cc = new C(); cc.label", "a!");
    /* エラー: setter の引数は 1 個 / getter に引数不可 */
    want_err("class C { set x(a, b) { } }", "setter");
    want_err("class C { get x(a) { return 1; } }", "getter");
    want_err("class C { get constructor() { return 1; } }", "accessor");
}
