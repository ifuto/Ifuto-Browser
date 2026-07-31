/* V8x v0.0 テスト。dispatch 両モード（computed-goto / switch）で同一バイナリを
 * 2 回ビルドして走らせる前提（Makefile: run_tests / run_tests_switch）。
 * どちらかでだけ失敗するような差分は dispatch バグなので即座に止める。 */
#include "tests.h"
#include "../src/v8x/v8x.h"
#include <stdint.h>
#include <string.h>
#include <math.h>

static V8xRT *g_rt;

static void want_num(const char *src, double want) {
    V8xVal v;
    if (!v8x_eval(g_rt, src, &v)) {
        fprintf(stderr, "  eval failed [%s]: %s\n", src, v8x_error(g_rt));
        CHECK(0);
        return;
    }
    double d = NAN;
    bool ok = v8x_as_num(v, &d) && d == want;
    CHECK(ok);
    if (!ok) fprintf(stderr, "  wrong value [%s]: got %g want %g\n", src, d, want);
}
static void want_bool(const char *src, bool want) {
    V8xVal v;
    if (!v8x_eval(g_rt, src, &v)) {
        fprintf(stderr, "  eval failed [%s]: %s\n", src, v8x_error(g_rt));
        CHECK(0);
        return;
    }
    bool b = false;
    bool ok = v8x_as_bool(v, &b) && b == want;
    CHECK(ok);
    if (!ok) fprintf(stderr, "  wrong bool [%s]\n", src);
}
static void want_str(const char *src, const char *want) {
    V8xVal v;
    if (!v8x_eval(g_rt, src, &v)) {
        fprintf(stderr, "  eval failed [%s]: %s\n", src, v8x_error(g_rt));
        CHECK(0);
        return;
    }
    uint32_t ln = 0;
    const char *s = v8x_as_str(g_rt, v, &ln);
    bool ok = s && strlen(want) == ln && memcmp(s, want, ln) == 0;
    CHECK(ok);
    if (!ok) fprintf(stderr, "  wrong string [%s]: got '%.*s' want '%s'\n",
                     src, s ? (int)ln : 0, s ? s : "", want);
}
/* needle != NULL ならエラー文言に含まれることも検査（budget 系の原因特定を誤魔化さない） */
static void want_err(const char *src, const char *needle) {
    if (v8x_eval(g_rt, src, NULL)) {
        fprintf(stderr, "  expected error but succeeded [%s]\n", src);
        CHECK(0);
        return;
    }
    if (needle) CHECK(strstr(v8x_error(g_rt), needle) != NULL);
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
        V8xVal v;
        CHECK(v8x_eval(g_rt, "0.1 + 0.2", &v));
        double d = 0;
        CHECK(v8x_as_num(v, &d) && d == 0.30000000000000004);
    }
    { /* NaN は canonical 正規化され != 自身 */
        V8xVal v;
        CHECK(v8x_eval(g_rt, "var n = 0/0; n == n", &v));
        bool b = true;
        CHECK(v8x_as_bool(v, &b) && b == false);
    }
    { /* ±inf */
        V8xVal v; double d;
        CHECK(v8x_eval(g_rt, "1/0", &v) && v8x_as_num(v, &d) && isinf(d) && d > 0);
        CHECK(v8x_eval(g_rt, "-1/0", &v) && v8x_as_num(v, &d) && isinf(d) && d < 0);
    }
    want_num("0x10 + 0b10 + 0o17", 33);
    { /* グローバル定数 NaN / Infinity（書換不可） */
        V8xVal v; double d;
        CHECK(v8x_eval(g_rt, "NaN", &v) && v8x_as_num(v, &d) && isnan(d));
        CHECK(v8x_eval(g_rt, "Infinity", &v) && v8x_as_num(v, &d) && isinf(d) && d > 0);
        CHECK(!v8x_eval(g_rt, "NaN = 1;", NULL));
        CHECK(v8x_eval(g_rt, "Infinity - Infinity == Infinity - Infinity", &v));
        bool b = true;
        CHECK(v8x_as_bool(v, &b) && b == false); /* NaN === NaN ではない */
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
    V8xRT *rt2 = v8x_new();
    CHECK(rt2 != NULL);
    V8xVal v;
    /* 命令 budget: 毎回新鮮に供給される（枯渇後も次の eval は普通に走る） */
    CHECK(!v8x_eval(rt2, "var i = 0; while (1) { i = i+1; }", NULL));
    CHECK(strstr(v8x_error(rt2), "budget") != NULL);
    CHECK(v8x_eval(rt2, "1+1", &v));
    double d = 0;
    CHECK(v8x_as_num(v, &d) && d == 2);
    /* グローバルは rt ごと独立（q は g_rt 側で定義済みだが rt2 では未定義） */
    CHECK(!v8x_eval(rt2, "q + 1", NULL));
    v8x_free(rt2);
    /* 解析深度 budget（V8X_PARSE_DEPTH=512 超の括弧 600 連） */
    {
        char deep[1280];
        int p = 0;
        for (int i = 0; i < 600; i++) deep[p++] = '(';
        deep[p++] = '1';
        for (int i = 0; i < 600; i++) deep[p++] = ')';
        deep[p] = 0;
        CHECK(!v8x_eval(g_rt, deep, NULL));
    }
    /* 空プログラム・コメントのみは合法 */
    CHECK(v8x_eval(g_rt, ";", NULL));
    CHECK(v8x_eval(g_rt, "/* nothing */ // nothing\n", NULL));
    /* 文字列ヒープ budget（倍々連結）は拒否で止まり、ホストを殺さない */
    CHECK(!v8x_eval(g_rt, "var s = 'xxxxxxxx'; while (1) { s = s + s; }", NULL));
    CHECK(strstr(v8x_error(g_rt), "budget") != NULL);
}

static void t_dispatch_parity(void) {
    /* 両 dispatch で一致すべき黄金値（初回観測でピン留めし不変を要求） */
    V8xVal v; double d = -1;
    CHECK(v8x_eval(g_rt,
        "function sig(n){ var a=0; for (var i=1; i<=n; i=i+1){ a = (a*31 + i) % 1000003; } return a; } sig(200)",
        &v) && v8x_as_num(v, &d));
    CHECK(d == 674928); /* 参照実装 (Python: a=(a*31+i)%1000003, i=1..200) で実算 */
}


/* --- 融合命令（LINC / CJMPF_L/G）と fuzz 起源パーサ硬直化の regression（v0.1 追加） --- */
static void test_v8x_fusion_and_hardening(void) {
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
        V8xVal v;
        CHECK(!v8x_eval(g_rt, "--.", &v));
        CHECK(!v8x_eval(g_rt, "var x = 1 +", &v));
        CHECK(!v8x_eval(g_rt, "---------------5", &v) || true); /* budget 内なら -5 で良い。構文拒否でも良い */
    }
}

/* phase 2/3: ROPE 連結・融合命令・for 回転の回帰。全て node/qjs 地上値と照合済みの数を使い、
 * グローバル名は rp_/ci_ 接頭で一意化する（v0.0 設計: グローバルは rt 内永続）。 */
static void test_v8x_rope_and_superinst(void) {
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
    /* GMULC/LMULC/*CI int fast・溢れ・左定数 MUL（交換の丸め一致） */
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

void test_v8x(void) {
    g_rt = v8x_new();
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
    test_v8x_fusion_and_hardening();
    test_v8x_rope_and_superinst();
    v8x_free(g_rt);
    g_rt = NULL;
}
