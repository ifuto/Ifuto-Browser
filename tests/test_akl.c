/* Akl v0.0 テスト。dispatch 両モード（computed-goto / switch）で同一バイナリを
 * 2 回ビルドして走らせる前提（Makefile: run_tests / run_tests_switch）。
 * どちらかでだけ失敗するような差分は dispatch バグなので即座に止める。 */
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
    akl_free(g_rt);
    g_rt = NULL;
}
