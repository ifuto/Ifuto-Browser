#define _POSIX_C_SOURCE 200809L /* clock_gettime（Math.random フォールバック） */
/* Akl v0.0 — Ifuto 自作 JS エンジンの実装（C11 + GCC/Clang computed-goto 拡張）。
 *
 * 構成: lexer → recursive-descent parser（AST 配列プール）→ one-pass codegen
 *       → bytecode verifier → スタックマシン VM。JIT は持たない（方針は akl.h 冒頭）。
 *
 * 安全性設計（「Rust レベル」を C で担保する内訳）:
 *   1. verifier が codegen 出力を全検査する:
 *      - 全 opcode < OP_COUNT（dangling dispatch の経路を閉塞）
 *      - 全即値が code 範囲内、全ジャンプ先が「命令境界かつ code 範囲内」
 *      - LLOAD/LSTORE の slot < 当該関数の n_locals（局所窓の範囲保証）
 *      - CONST_STR/MAKEF の index がオブジェクト表/関数表に有効
 *      これにより VM の dispatch は検証済み入力にのみ作用する（入力は外部文字列=JS
 *      ソースであり bytecode ではない。bytecode を外部から注入する経路は API に無い）。
 *   2. VM の動的アクセスは全て境界検査つき: 値スタック push/pop、フレーム深度、
 *      ローカル窓も verifier の裏付けに加え push/pop では常時検査（fail-stop 方針）。
 *   3. UB 排除: double↔bits は memcpy のみ、alloc は全検査、再帰・反復は budget 管理。
 *   4. 値表現は NaN-box 8B。算術結果の NaN は canonical 正規化し、タグ空間
 *      （上位16bit=0xFFFF）に衝突する double の生成経路を API 面から排除する。
 *
 * v0.0 範囲: number/string/boolean/null/undefined、var/let/const（関数スコープ近似）、
 *   if/else、while、for、break/continue、function 宣言・呼び出し・再帰、return、
 *   ==/!=/===/!==、</<=/>/>=、+ - * / %、! - +(単項) typeof、&& || =（代入は式）。
 *   既知の v0.0 境界（台帳）: 配列/オブジェクトリテラル無し、クロージャの自由変数は
 *   グローバルのみ解決（ネスト関数の再帰はトップレベル宣言経由のみ）、ASI は最小
 *   （';' 必須、ただし '}'/EOF 直前は許容）、let/const のブロックスコープは関数
 *   スコープに潰す、arguments/this/new/prototype は未。
 */
#include "../common.h"
#include "akl.h"
#include "akl_regex.h"
#include <string.h>
#include <stdlib.h>
#include <sys/random.h>
#include <time.h>
#include <stdio.h>
#include <math.h>
#include <stdarg.h>

/* ============================== NaN-box 値 ============================== */

#define AKL_TAG_MASK  0xFFFF000000000000ull
#define AKL_VAL_UNDEF (AKL_TAG_MASK | 0u)
#define AKL_VAL_NULL  (AKL_TAG_MASK | 1u)
#define AKL_VAL_FALSE (AKL_TAG_MASK | 2u)
#define AKL_VAL_TRUE  (AKL_TAG_MASK | 3u)
#define AKL_MK_INT(i) ((AKL_TAG_MASK | (1ull << 32)) | (uint32_t)(i))
#define AKL_MK_OBJ(i) ((AKL_TAG_MASK | (2ull << 32)) | (uint32_t)(i))

static bool akl_tagged(AklVal v) { return (v & AKL_TAG_MASK) == AKL_TAG_MASK; }
static bool akl_is_intv(AklVal v) {
    return (v & 0xFFFFFFFF00000000ull) == (AKL_TAG_MASK | (1ull << 32));
}
static bool akl_is_objv(AklVal v) {
    return (v & 0xFFFFFFFF00000000ull) == (AKL_TAG_MASK | (2ull << 32));
}
static i32 akl_get_int(AklVal v) { return (i32)(uint32_t)v; }
static u32 akl_get_obj(AklVal v) { return (uint32_t)v; }

static AklVal akl_from_double(double d) {
    AklVal v;
    if (isnan(d)) return 0x7FF8000000000000ull; /* canonical NaN（タグ空間 0xFFFF 帯と非衝突） */
    memcpy(&v, &d, 8);
    /* canonical NaN 以外にタグ空間上位 0xFFFF 帯は算術結果から来ない（akl.h 不変条件） */
    return v;
}
static AklVal akl_num(double d) {
    if (isnan(d)) { static const double CN = 0.0 / 0.0; (void)CN; }
    double cd = d;
    if (isnan(cd)) { /* canonical NaN へ正規化（タグ空間衝突の構造的排除） */
        AklVal v = 0x7FF8000000000000ull;
        double out;
        memcpy(&out, &v, 8);
        cd = out;
    }
    return akl_from_double(cd);
}
static double akl_as_double_raw(AklVal v) {
    double d;
    memcpy(&d, &v, 8);
    return d;
}
/* 数値として読めるか（INT or double）。obj/特殊値は false */
static bool akl_numv(AklVal v, double *out) {
    if (akl_is_intv(v)) { *out = (double)akl_get_int(v); return true; }
    if (!akl_tagged(v)) { *out = akl_as_double_raw(v); return true; }
    return false;
}

/* ============================== ヒープオブジェクト ============================== */

enum { AKL_OK_STR = 1, AKL_OK_FUNC = 2, AKL_OK_ROPE = 3, AKL_OK_NATIVE = 4, AKL_OK_OBJ = 5, AKL_OK_HANDLE = 6,
       AKL_OK_ARR = 7, AKL_OK_ENV = 8, AKL_OK_REGEX = 9 };
/* ROPE: code_off=左 obj idx, name=右 obj idx, n_params=深さ(最大4096), len=全長。
 * 不変条件: 子の index は親より小さい必要は「ない」（free-list 再利用で逆転し得る）。
 * よって GC の伝播は添字順に依らない明示ワークリストで行う。文字列は不変。
 * NATIVE: u.nat（C fn + udata。heap 不算入・GC 子なし）。
 * OBJ:   u.po（props malloc 所有。heap_bytes に cap*sizeof(AklProp) を生存課金。
 *        prop の name は intern STR obj index → GC 伝播で mark 必須）。
 * ARR:   u.arr（要素列 malloc 所有。heap_bytes に cap*8 生存課金。length は u.arr.n）。
 *        index は u32（ToUint32 後）。負・非整数・範囲外は undefined（名前付きプロパティ非対応）。
 * ENV:   u.env（クロージャ環境チェーン。vals は関数の capture 済みローカル。
 *        parent は生成元の環境チェーン（なければ UINT32_MAX）。GC 伝播必須）。
 * FUNC:  env フィールド = クロージャ生成時に捕捉した環境 obj index（無ければ UINT32_MAX）。 */

typedef struct { u32 name; AklVal v; } AklProp;
#define AKL_OBJ_MAX_PROPS 64u /* 1 オブジェクトの prop 数上限（線形走査の有界化） */

/* nursery（C 側一時ルート）容量。最大同時ピンは eq/rel 系の入れ子で
 * 2(ハンドラ) + 2(loose/strict) + 1(flatten) = 5。余裕を見て 8。 */
#define AKL_NURY_CAP 8

typedef struct {
    u8 kind;
    u8 _p[3];
    u32 len;      /* STR: バイト長 */
    u8 *bytes;    /* STR: malloc 所有 */
    u32 code_off; /* FUNC: funcs[] index */
    u32 name;     /* FUNC: 名前 STR の obj index（呼出名診断用） */
    u16 n_params; /* FUNC */
    u16 n_locals; /* FUNC */
    u32 env;      /* FUNC: クロージャ捕捉環境 obj index（UINT32_MAX=無し） */
    union {
        struct { AklNativeFn fn; void *udata; } nat;   /* NATIVE */
        struct { AklProp *props; u32 n, cap; } po;     /* OBJ */
        struct { const AklHandleVTab *vt; void *ptr; } hd; /* HANDLE（C 側所有。GC 非管理） */
        struct { AklVal *v; u32 n, cap; } arr;         /* ARR */
        struct { AklVal *vals; u32 n; u32 parent; } env; /* ENV */
        struct { AklRex *rx; u32 flags; i32 last_index; } rex; /* REGEX（16B） */
    } u;
} AklObj; /* 56B（v0.3: ARR/ENV/FUNC.env 追加で 48B から +8B。ARCH 台帳記録） */

/* ============================== runtime ============================== */

typedef struct { u32 name; AklVal v; u8 is_const; u8 _p[3]; } AklGlobal;
typedef struct { u32 code_off, code_end; u32 name; u16 n_params, n_locals; u16 n_env; u16 n_cap; } AklFuncEnt;

enum { /* VM 命令（追加は末尾に。verifier/jumptable も同期させること） */
    OP_CONST_I = 0, OP_CONST_D, OP_CONST_STR,
    OP_TRUE_T, OP_FALSE_T, OP_NULL_T, OP_UNDEF_T,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_LT, OP_LE, OP_GT, OP_GE,
    OP_EQ, OP_NE, OP_SEQ, OP_SNE,
    OP_NOT, OP_NEG, OP_POS, OP_TYPEOF,
    OP_POP, OP_POPV, OP_DUP,
    OP_LLOAD, OP_LSTORE, OP_GLOAD, OP_GSTORE,
    OP_JMP, OP_JMPF, OP_JMPT,
    OP_CALL, OP_RET, OP_MAKEF,
    OP_LINC,                /* slot u32 | delta i32 : x = x + d（スタック中立・last_val 更新） */
    OP_CJMPF_L,             /* slot u32 | imm i32 | cmp u8 | tgt u32 : 比較+条件分岐の融合 */
    OP_CJMPF_G,             /* name u32 | imm i32 | cmp u8 | tgt u32 : 同上（グローバル） */
    OP_GLOAD_S,             /* slot u32 : 直結グローバル load（compile 時登録済み保証は cg_store 不変条件） */
    OP_GSTORE_S,            /* slot u32 : 直結グローバル store（const 検査は compile 時済み） */
    OP_GINC,                /* slot u32 | delta i32 : グローバル版 LINC */
    OP_GMULC,               /* gslot u32 | imm i32 : push(globals[gslot] * imm) */
    OP_LMULC,               /* lslot u32 | imm i32 : push(locals[lslot] * imm) */
    OP_MULCI,               /* imm i32 : TOS = TOS * imm */
    OP_ADDCI,               /* imm i32 : TOS = TOS + imm（TOS が文字列なら imm を右辺とする連結） */
    OP_SUBCI,               /* imm i32 : TOS = TOS - imm */
    OP_MODCI,               /* imm i32 : TOS = TOS % imm */
    OP_GADD_P,              /* gslot u32 : TOS = globals[gslot] + TOS（左辺=g で連結順序保持） */
    OP_LADD_P,              /* lslot u32 : TOS = locals[lslot] + TOS */
    OP_GADD_G,              /* gdst u32 | gsrc u32 : gdst = gdst + gsrc（文脈限定・スタック中立・last_val 更新） */
    OP_CJMPF_MODG,          /* gslot u32 | mod i32 | k i32 | cmp u8 | tgt u32 : (x%mod == k / != k) で分岐 */
    OP_CJMPF_MODL,          /* lslot 同上 */
    OP_GSTORE_SPV,          /* gslot u32 : pop -> g。last_val = v（文レベル代入の融合版） */
    OP_LSTORE_PV,           /* lslot u32 : pop -> l。last_val = v */
    OP_LOOPINC_G,           /* gslot u32 | delta i32 | lim i32 | cmp u8 | tgt u32 : g+=delta 後 g rel lim なら tgt へ（for 回転） */
    OP_ADDCI_G, OP_SUBCI_G, OP_MULCI_G, OP_MODCI_G, /* gslot u32 | imm i32 : g = TOS op imm + last_val（*CI+STORE_PV 再融合） */
    OP_ADDCI_L, OP_SUBCI_L, OP_MULCI_L, OP_MODCI_L, /* lslot u32 | imm i32 : 同上ローカル */
    OP_CJMPF_MULGG,         /* g1 g2 g3 u32x3 | cmp u8 | tgt u32 : (g1*g2) rel g3 が偽なら tgt（試行除法形） */
    OP_CJMPF_MODGG,         /* g1 u32 | g2 u32 | k i32 | cmp u8 | tgt u32 : (g1 % g2 == k / != k) が偽なら tgt */
    OP_LADD_LL,             /* dst s1 s2 u32x3 : locals[dst] = locals[s1] + locals[s2]（var t = x + y 融合・last_val 不変） */
    OP_RET_L,               /* slot u32 : return locals[slot]（LLOAD;RET 融合） */
    OP_ADD_GX, OP_SUB_GX, OP_MUL_GX, OP_MOD_GX, /* gdst u32 | gsrc u32 : gdst = TOS op globals[gsrc] + last_val */
    OP_ADD_LX, OP_SUB_LX, OP_MUL_LX, OP_MOD_LX, /* ldst u32 | lsrc u32 : ldst = TOS op locals[lsrc] + last_val */
    OP_TRY_PUSH,            /* catch_pc u32 | finally_pc u32 | catch_slot u32（0xFFFFFFFF=無）: try 入口 */
    OP_TRY_LEAVE,           /* resume u32 : try/catch の正常完了 → finally 連鎖 or resume へ */
    OP_FIN_END,             /* finally 末端: mode で normal/throw/return に分岐 */
    OP_THROW,               /* TOS の値を投げる（巻き戻し → 捕捉 or uncaught 致命的エラー） */
    OP_LOOPINC_L,           /* lslot u32 | delta i32 | lim i32 | cmp u8 | tgt u32 : LOOPINC_G のローカル版 */
    OP_LOOPINC_GV,          /* LOOPINC_G + last_val 更新（回転元の「文としての更新」の意味を正確に保持。CoJIT 由来） */
    OP_LOOPINC_LV,          /* LOOPINC_L 同上 */
    OP_NOP,                 /* CoJIT の書換埋め（セマンティクス的到達不能のみ出現。実行時は透過） */
    OP_OBJNEW,              /* imm なし : push 新規 AKL_OK_OBJ（props 空） */
    OP_PLOAD,               /* name u32 : pop obj → push obj[name]（無ければ undefined） */
    OP_PSTORE,              /* name u32 : pop val, pop obj → obj[name]=val, push val（代入式の値） */
    OP_MCALL,               /* argc u8 | name u32 : stk[..-argc-1]=obj メソッド呼出（native は self=obj） */
    /* ---- v0.3: 配列・ブラケット・this・クロージャ環境・ビット演算 ---- */
    OP_THIS,                /* imm なし : push 現在 frame の this（frame slot 0。main は undefined） */
    OP_ELOAD,               /* idx u32 : push 自前環境（frame slot 1）の vals[idx] */
    OP_ESTORE,              /* idx u32 : pop → 自前環境 vals[idx]（push しない。文脈は代入式側が DUP） */
    OP_CELOAD,              /* capslot u8 | depth u8 | idx u32 : cap 環境を depth 回 parent で辿り vals[idx] */
    OP_CESTORE,             /* capslot u8 | depth u8 | idx u32 : 同上ストア */
    OP_ANEW,                /* count u32 : 直近 count 個を pop して新規 ARR（順序保持）を push */
    OP_AGET,                /* imm なし : pop idx, pop obj/arr/str → push 要素（範囲外 undefined） */
    OP_ASET,                /* imm なし : pop val, pop idx, pop arr → arr[idx]=val, push val */
    OP_BNOT,                /* pop → ~ToInt32 を push */
    OP_BAND, OP_BOR, OP_BXOR, OP_BSHL, OP_BSHR, OP_BUSHR, /* pop b, pop a → a op b（ToInt32/ToUint32） */
    OP_POW,                 /* pop b, pop a → push a**b（pow） */
    OP_KEYSOF,              /* pop obj → push キー配列（OBJ のプロパティ名 / ARR の index / 文字列の index） */
    OP_TOARR,               /* pop v → push 配列化（ARR はそのまま、それ以外は [v]） */
    OP_ARRPUSH,             /* pop val, pop arr → arr に val を追加 → push arr */
    OP_ARRPUSHALL,          /* pop src, pop arr → arr に src の全要素を追加 → push arr */
    OP_ARRSPREADC,          /* slot u32 : pop arr → 要素を順に push + 個数を locals[slot] に加算 */
    OP_CALLN,               /* slot u32 : argc = locals[slot]、fn = stk[sp-argc-1] で呼ぶ（動的引数） */
    OP_IN,                  /* pop obj, pop key → push (key in obj) */
    OP_PDEL,                /* name u32 : pop obj → obj[name] を削除 → push true（無ければ false） */
    OP_IDEL,                /* pop idx, pop obj → obj[idx] を削除 → push true（配列は undefined 化） */
    OP_INSTANCEOF,          /* pop obj, pop f → push (obj は f のインスタンスか) */
    OP_NEW,                 /* argc u8 : new 呼び出し（this=新 OBJ、戻りが obj でなければ this） */
    OP_NEWREGEX,            /* pat STR idx u32 | flags u32 : RegExp オブジェクト生成 */
    OP_CALLT,               /* argc u8 : [this][fn][args...] を this で呼ぶ（super 用） */
    OP_MAKEFS,              /* fidx u32 | srcslot u8 : [親][fn] → fn の env 先頭に親をバインド */
    OP_SUPERGET,            /* name u32 : 親クラス（関数 env の vals[0]）の name を push */
    OP_OBJSPREAD,           /* pop src → TOS の OBJ に全 props コピー（オブジェクト spread） */
    OP_PSETDYN,             /* pop val, pop key, pop obj → obj[key]=val, push val（computed） */
    OP_MCALLN,              /* slot u32 | name u32 : argc = locals[slot] の動的 MCALL（メソッド spread） */
    OP_ARRREST,             /* pop start, pop arr → 新規配列 [start..n)（分割 rest） */
    OP_OBJREST,             /* pop src_obj → 新規 OBJ（全 props コピー。除外は PDEL で） */
    OP_HALT,
    OP_COUNT
};

/* JS 例外の実行時エントリ。kind: TRY（捕捉候補）/ FIN（finally 実行中＝保留状態）。
 * in_catch: TRY で catch 本体実行中（この状態への throw では finally だけを残して pop）。
 * mode（FIN）: 0=normal, 1=throw, 2=return。pending に例外/返り値を保持（GC ルート）。 */
enum { AKL_TE_TRY = 0, AKL_TE_FIN = 1 };
#define AKL_PC_NONE 0xFFFFFFFFu
#define AKL_SLOT_NONE 0xFFFFFFFFu
#define AKL_STR_METH_N 20u    /* v0.3 組込: 文字列メソッド数（v0.4: match/search 追加） */
#define AKL_ARR_METH_N 20u    /* v0.3 組込: 配列メソッド数（v0.4: 高階 8 種追加） */
#define AKL_REGEX_METH_N 3u    /* v0.4: 正規表現メソッド数（test/exec/toString） */

typedef struct {
    AklVal pending;
    u32 frame;       /* 属する呼出し深さ（main=0, 1回目の callee=1, ...） */
    u32 sp;
    u32 catch_pc, finally_pc, catch_slot;
    u32 resume_pc;   /* FIN: mode 0 の継続先 */
    u8 kind, mode, in_catch, _p;
} AklTryEnt;

/* v0.3 再入（高階関数）: akl_call が退避した outer VM スタックの GC ルート登録。
 * inner 実行中の GC は rt->stk（inner）＋root_stks（outer 群）を根としてマークする。 */
typedef struct AklRootStk {
    AklVal *stk;
    u32 sp;              /* gc_sp 相当（ルート深さ） */
    struct AklRootStk *next;
} AklRootStk;

struct AklRT {
    /* ヒープ（obj index 参照。mark-sweep GC: スイープは index 不変・穴再利用。
     * 構造的安全: コンパイル生成物（< pin_mark）はゴミになり得ない構成で、
     * 到達不能は実行時生成物のみ。根は VM スタック＋globals＋nursery の3系のみ） */
    AklObj *objs; u32 n_objs, cap_objs;
    u64 heap_bytes; /* 生存 STR bytes 累計（GC が減算。上限は「生存分」で裁く） */
    u32 pin_mark;   /* objs[0..pin_mark): コンパイル由来。スイープ対象外 */
    u32 *free_objs; u32 n_free, cap_free; /* スイープで空いたスロット（LIFO 再利用） */
    u64 gc_next;    /* 適応 GC 閾値（bytes）。硬上限 AKL_MAX_HEAP_MB とは別に、
                     * 前回 GC 後 live ×2（下限 512KB）で発火し定常 RSS を live 漸近に抑える */
    u32 gc_next_objs; /* 同上（オブジェクト数。スロット配列の高水位を live 漸近に） */
    u32 gc_sp;      /* VM が alloc サイト直前に同期するスタック深さスナップショット */
    u32 nury[AKL_NURY_CAP]; u32 n_nury; /* C 側一時ルート（concat 一時 obj / eq・rel の flatten 対象ピン） */
    bool gc_live;   /* vm_exec 実行中のみ true（GC はこの時だけ発火） */
    /* GLOAD/GSTORE の O(1) 化: name(u32 intern id) -> global slot の脱 Salt ハッシュ
     * （globals は append-only。n_globals 変化検知でのみ全再構築） */
    u32 *ghash; u32 ghash_cap; u32 ghash_sync;
    /* コード＋関数表（eval ごとに追記。関数は以後の eval から呼べる） */
    u8 *code; u32 code_len, code_cap;
    AklFuncEnt *funcs; u32 n_funcs, cap_funcs;
    /* グローバル（name は intern 済み STR obj index） */
    AklGlobal *globals; u32 n_globals, cap_globals;
    /* VM 作業領域 */
    AklVal *stk; u32 cap_stk;
    u64 insn_budget_def;
    u32 heap_mb;    /* 硬上限 = heap_mb<<20。既定 AKL_MAX_HEAP_MB。akl_tune で組込側責任で引上げ可 */
    u32 max_objs;   /* オブジェクト数硬上限。既定 AKL_MAX_OBJECTS。同上 */
    /* JS 例外スタック（frames とは別系。push/pop は lex 領域に LIFO 対応） */
    AklTryEnt *tries; u32 n_tries, cap_tries;
    bool cojit_off;     /* CoJIT（検証駆動 AOT 特化）の kill-switch。既定 ON */
    u32 cojit_applied;  /* S1 回転の累積適用数（観測用） */
    AklVal last_val;
    /* v0.3 再入（高階関数）: akl_call 用 */
    AklRootStk *root_stks;  /* 退避済み outer スタックの GC ルート（連結リスト） */
    u32 call_depth;         /* akl_call の再入深さ（上限 AKL_MAX_REENTRY） */
    /* v0.3 組込: 文字列/配列メソッドの NATIVE キャッシュ（akl_new で生成。PLOAD が返す） */
    AklVal str_meth_vals[AKL_STR_METH_N];
    AklVal arr_meth_vals[AKL_ARR_METH_N];
    AklVal regex_meth_vals[AKL_REGEX_METH_N];
    char err[256];
    bool native_err;    /* native が akl_native_throw した（VM はこれを見て eval を失敗に） */
    /* 定数除数の剰余を magic-multiply に強度削減する直写メモ（8 エントリ）。
     * 実行時コード生成は一切行わない（禁止事項の JIT ではない）: m,p は d にのみ
     * 依存する数学的定数で、純粋な整数同値変形を d ごとに 1 度だけ計算する。
     * d=0 は「空」の番兵（規約上 d>1 のみ登録するので衝突しない） */
    struct { u64 m; u32 d; u8 p; } mm[8];
    /* parse 作業の再利用バッファは eval ローカルで確保（再入安全） */
};

enum {
    AKL_MAX_OBJECTS  = 100000,
    AKL_MAX_HEAP_MB  = 16,
    AKL_MAX_SRC      = 4u << 20,
    AKL_MAX_NODES    = 200000,
    AKL_MAX_DEPTH    = 256,   /* call 深度 */
    AKL_PARSE_DEPTH  = 512,
    AKL_STK_INIT     = 1024,
    AKL_STK_MAX      = 1u << 16,
    AKL_MAX_LOCALS   = 1024,
    AKL_NATIVE_COST  = 1024,  /* native 呼出 1 回の insn budget 課金（既定 10M で ~9765 呼まで） */
    AKL_MAX_REENTRY  = 64     /* akl_call の再入深さ上限（スタック枯渇の構造的防止） */
};


/* ============================== 診断/確保 ============================== */

static void akl_errf(AklRT *rt, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rt->err, sizeof rt->err, fmt, ap);
    va_end(ap);
}

/* GC 伝播が必要なオブジェクト種別（子参照を持つ） */
static bool akl_gc_kind_children(u8 k) {
    return k == AKL_OK_ROPE || k == AKL_OK_OBJ || k == AKL_OK_ARR ||
           k == AKL_OK_ENV || k == AKL_OK_FUNC;
}

/* ---- mark-sweep GC。ルートは VM スタック(gc_sp スナップショット)＋globals＋nursery＋last_val。
 * スイープは index を動かさない（バイトコード中の CONST_STR/MAKEF オペランド = obj index を
 * 無効化しないための構造的条件）。空いたスロットは free_objs で再利用する。 */
static void akl_gc_mark_val(AklRT *rt, AklVal v, u8 *mk) {
    if (akl_is_objv(v)) {
        u32 i = akl_get_obj(v);
        if (i >= rt->pin_mark && i < rt->n_objs) mk[i - rt->pin_mark] = 1;
    }
}
static u32 akl_gc(AklRT *rt) {
    if (!rt->gc_live) return 0;
    u32 span = rt->n_objs - rt->pin_mark;
    if (!span) return 0;
    u8 *mk = (u8 *)calloc(span, 1);
    if (!mk) return 0; /* OOM 時は回収不能扱い（呼び出し側が budget エラーに倒す。安全側） */
    if (rt->stk) {
        u32 lim = rt->gc_sp < rt->cap_stk ? rt->gc_sp : rt->cap_stk;
        for (u32 i = 0; i < lim; i++) akl_gc_mark_val(rt, rt->stk[i], mk);
    }
    /* 再入（高階関数）: 退避済み outer スタックも根としてマーク（inner 実行中の
     * GC が outer のローカル値を回収しないための構造保証） */
    for (const AklRootStk *rs = rt->root_stks; rs; rs = rs->next)
        for (u32 i = 0; i < rs->sp; i++) akl_gc_mark_val(rt, rs->stk[i], mk);
    akl_gc_mark_val(rt, rt->last_val, mk);
    for (u32 i = 0; i < rt->n_tries; i++)
        if (rt->tries[i].kind == AKL_TE_FIN) akl_gc_mark_val(rt, rt->tries[i].pending, mk);
    for (u32 i = 0; i < rt->n_globals; i++) akl_gc_mark_val(rt, rt->globals[i].v, mk);
    for (u32 i = 0; i < rt->n_nury; i++) {
        u32 oi = rt->nury[i];
        if (oi >= rt->pin_mark && oi < rt->n_objs) mk[oi - rt->pin_mark] = 1;
    }
    /* ROPE/OBJ/ARR/ENV/FUNC の伝播: free-list 再利用で子 index > 親 index があり得るため
     * 添字順走査では閉包が取れない。明示ワークリスト（深さ上限 4096 由来の有界）で辿る。
     * OBJ は prop 値（obj 参照）と prop 名（intern STR index）の両方を子として mark する
     *（名の生存を obj に連動させ、ホスト側 akl_prop_set の intern 規約を不要にする）。
     * ARR は要素全部、ENV は vals 全部 + parent、FUNC は env（クロージャ捕捉環境）を子とする。 */
    {
        u32 *wl = (u32 *)malloc((u64)span * sizeof(u32));
        if (wl) {
            u32 wn = 0;
            for (u32 i = rt->pin_mark; i < rt->n_objs; i++)
                if (mk[i - rt->pin_mark] && akl_gc_kind_children(rt->objs[i].kind)) wl[wn++] = i;
            while (wn) {
                u32 ri = wl[--wn];
                AklObj *ro = &rt->objs[ri];
                if (ro->kind == AKL_OK_OBJ) {
                    for (u32 k = 0; k < ro->u.po.n; k++) {
                        u32 kids[2] = { ro->u.po.props[k].name,
                                        akl_is_objv(ro->u.po.props[k].v) ? akl_get_obj(ro->u.po.props[k].v)
                                                                         : rt->n_objs };
                        for (int j = 0; j < 2; j++) {
                            u32 ci = kids[j];
                            if (ci >= rt->pin_mark && ci < rt->n_objs && !mk[ci - rt->pin_mark]) {
                                mk[ci - rt->pin_mark] = 1;
                                if (akl_gc_kind_children(rt->objs[ci].kind) && wn < span) wl[wn++] = ci;
                            }
                        }
                    }
                } else if (ro->kind == AKL_OK_ROPE) { /* ROPE */
                    u32 kids[2] = { ro->code_off, ro->name };
                    for (int k = 0; k < 2; k++) {
                        u32 ci = kids[k];
                        if (ci >= rt->pin_mark && ci < rt->n_objs && !mk[ci - rt->pin_mark]) {
                            mk[ci - rt->pin_mark] = 1;
                            if (akl_gc_kind_children(rt->objs[ci].kind) && wn < span) wl[wn++] = ci;
                        }
                    }
                } else if (ro->kind == AKL_OK_ARR) { /* ARR: 要素全部 */
                    for (u32 k = 0; k < ro->u.arr.n; k++) {
                        AklVal ev = ro->u.arr.v[k];
                        if (!akl_is_objv(ev)) continue;
                        u32 ci = akl_get_obj(ev);
                        if (ci >= rt->pin_mark && ci < rt->n_objs && !mk[ci - rt->pin_mark]) {
                            mk[ci - rt->pin_mark] = 1;
                            if (akl_gc_kind_children(rt->objs[ci].kind) && wn < span) wl[wn++] = ci;
                        }
                    }
                } else if (ro->kind == AKL_OK_ENV) { /* ENV: vals + parent */
                    for (u32 k = 0; k < ro->u.env.n; k++) {
                        AklVal ev = ro->u.env.vals[k];
                        if (!akl_is_objv(ev)) continue;
                        u32 ci = akl_get_obj(ev);
                        if (ci >= rt->pin_mark && ci < rt->n_objs && !mk[ci - rt->pin_mark]) {
                            mk[ci - rt->pin_mark] = 1;
                            if (akl_gc_kind_children(rt->objs[ci].kind) && wn < span) wl[wn++] = ci;
                        }
                    }
                    if (ro->u.env.parent != UINT32_MAX) {
                        u32 ci = ro->u.env.parent;
                        if (ci >= rt->pin_mark && ci < rt->n_objs && !mk[ci - rt->pin_mark]) {
                            mk[ci - rt->pin_mark] = 1;
                            if (akl_gc_kind_children(rt->objs[ci].kind) && wn < span) wl[wn++] = ci;
                        }
                    }
                } else if (ro->kind == AKL_OK_FUNC) { /* FUNC: クロージャ捕捉環境 */
                    if (ro->env != UINT32_MAX) {
                        u32 ci = ro->env;
                        if (ci >= rt->pin_mark && ci < rt->n_objs && !mk[ci - rt->pin_mark]) {
                            mk[ci - rt->pin_mark] = 1;
                            if (akl_gc_kind_children(rt->objs[ci].kind) && wn < span) wl[wn++] = ci;
                        }
                    }
                }
            }
            free(wl);
        }
        /* OOM で wl 取れない時は伝播を諦める = 保守方向でなく危険方向になるが、
         * その場合 mk[] 確保も先に失敗している想定なので実質到達不能。 */
    }
    u32 got = 0;
    for (u32 i = rt->pin_mark; i < rt->n_objs; i++) {
        AklObj *o = &rt->objs[i];
        if (o->kind == 0 || mk[i - rt->pin_mark]) continue;
        if (o->kind == AKL_OK_STR && o->bytes) { rt->heap_bytes -= o->len; free(o->bytes); }
        if (o->kind == AKL_OK_OBJ && o->u.po.props) {
            rt->heap_bytes -= (u64)o->u.po.cap * sizeof(AklProp);
            free(o->u.po.props);
        }
        if (o->kind == AKL_OK_ARR && o->u.arr.v) {
            rt->heap_bytes -= (u64)o->u.arr.cap * sizeof(AklVal);
            free(o->u.arr.v);
        }
        if (o->kind == AKL_OK_ENV && o->u.env.vals) {
            rt->heap_bytes -= (u64)o->u.env.n * sizeof(AklVal);
            free(o->u.env.vals);
        }
        if (o->kind == AKL_OK_REGEX && o->u.rex.rx) {
            akl_rex_free(o->u.rex.rx);
            rt->heap_bytes -= o->len; /* len = コンパイル済みサイズ概算 */
        }
        o->kind = 0; o->bytes = NULL; o->len = 0; o->code_off = 0; o->name = 0;
        o->env = UINT32_MAX;
        o->u.nat.fn = NULL; o->u.nat.udata = NULL; /* union 全域クリア（fn/udata と props/n/cap は同地址） */
        if (rt->n_free == rt->cap_free) {
            u32 nc = rt->cap_free ? rt->cap_free * 2 : 128;
            u32 *nf = (u32 *)realloc(rt->free_objs, (u64)nc * sizeof(u32));
            if (!nf) break; /* リスト拡張失敗: 解放済みスロットの再利用だけ諦める */
            rt->free_objs = nf; rt->cap_free = nc;
        }
        rt->free_objs[rt->n_free++] = i;
        got++;
    }
    free(mk);
    u64 next = rt->heap_bytes * 2;
    u64 floor_ = 512u << 10, ceil_ = (u64)rt->heap_mb << 20;
    rt->gc_next = next < floor_ ? floor_ : next > ceil_ ? ceil_ : next;
    u32 nobjs_live = rt->n_objs - rt->n_free;
    u64 nobj = (u64)nobjs_live * 2 + 1024;
    rt->gc_next_objs = nobj > rt->max_objs ? rt->max_objs : (u32)nobj;
    return got;
}

static i32 obj_prop_find(const AklObj *o, u32 name);
static const u8 *akl_str(AklRT *rt, u32 idx, u32 *len);
static bool obj_prop_set(AklRT *rt, AklObj *o, u32 name, AklVal v);
/* クラス継承のメソッドコピー: __super チェーンを親 → 子の順で辿り、constructor 以外の
 * メソッド/static をインスタンスにコピーする（子が親を上書き）。深さ制限 64。 */
static bool akl_new_copy_chain(AklRT *rt, AklObj *cls, AklObj *inst, u32 sup_name, u32 depth) {
    if (depth >= 64 || cls->kind != AKL_OK_OBJ) return true;
    i32 pi = obj_prop_find(cls, sup_name);
    if (pi >= 0) {
        AklVal pv = cls->u.po.props[pi].v;
        if (akl_is_objv(pv)) {
            AklObj *po = &rt->objs[akl_get_obj(pv)];
            if (po->kind == AKL_OK_OBJ) {
                if (!akl_new_copy_chain(rt, po, inst, sup_name, depth + 1)) return false;
            }
        }
    }
    for (u32 i = 0; i < cls->u.po.n; i++) {
        u32 pname = cls->u.po.props[i].name;
        if (pname == sup_name) continue;
        u32 kl;
        const u8 *kp = akl_str(rt, pname, &kl);
        if (rt->err[0]) return false;
        if (kl == 11 && memcmp(kp, "constructor", 11) == 0) continue;
        if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = pname;
        if (!obj_prop_set(rt, inst, pname, cls->u.po.props[i].v)) return false;
    }
    return true;
}

/* obj_new 直後に後続処理が失敗したときの巻き戻し。
 * idx が最終スロットなら append を戻し、それ以外（free-list 由来）は穴に返す。 */
static void obj_free_rollback(AklRT *rt, u32 idx) {
    memset(&rt->objs[idx], 0, sizeof(AklObj));
    if (idx + 1 == rt->n_objs) rt->n_objs--;
    else if (rt->n_free < rt->cap_free) rt->free_objs[rt->n_free++] = idx;
    /* free リスト満杯なら kind==0 のまま放置（GC が再度拾う。リークはスロットのみで安全側） */
}

static u32 akl_obj_new(AklRT *rt) {
    if (rt->n_objs - rt->n_free >= rt->gc_next_objs) akl_gc(rt); /* 生存オブジェクト数の適応閾値
        * （n_objs は free-list 再利用で単調にしか見えないので live = n_objs - n_free で裁う。
        *  これを誤ると freelist 充填後に毎回 GC する常勤化バグになる — 実測 9ms→265ms 退行） */
    if (rt->n_free) {
        u32 idx = rt->free_objs[--rt->n_free];
        memset(&rt->objs[idx], 0, sizeof(AklObj));
        return idx;
    }
    if (rt->n_objs >= rt->max_objs) {
        akl_gc(rt);
        if (rt->n_free) {
            u32 idx = rt->free_objs[--rt->n_free];
            memset(&rt->objs[idx], 0, sizeof(AklObj));
            return idx;
        }
        akl_errf(rt, "object budget exhausted");
        return UINT32_MAX;
    }
    if (rt->n_objs == rt->cap_objs) {
        u32 nc = rt->cap_objs ? rt->cap_objs * 2 : 64;
        AklObj *no = (AklObj *)realloc(rt->objs, (u64)nc * sizeof(AklObj));
        if (!no) { akl_errf(rt, "oom: objects"); return UINT32_MAX; }
        rt->objs = no; rt->cap_objs = nc;
    }
    AklObj *o = &rt->objs[rt->n_objs];
    memset(o, 0, sizeof *o);
    return rt->n_objs++;
}

/* STR obj を新規作成（bytes はコピー）。失敗時 UINT32_MAX（err 設定済み） */
static u32 akl_mkstr(AklRT *rt, const u8 *p, u32 n) {
    if ((u64)rt->heap_bytes + n > rt->gc_next) akl_gc(rt); /* 適応閾値（RSS 漸近抑制） */
    if ((u64)rt->heap_bytes + n > (u64)rt->heap_mb << 20) {
        akl_gc(rt); /* 生存分だけで再判定（上限は live bytes に対して適用） */
        if ((u64)rt->heap_bytes + n > (u64)AKL_MAX_HEAP_MB << 20) {
            akl_errf(rt, "heap bytes budget exhausted");
            return UINT32_MAX;
        }
    }
    u32 idx = akl_obj_new(rt);
    if (idx == UINT32_MAX) return UINT32_MAX;
    u8 *cp = (u8 *)malloc(n ? n : 1);
    if (!cp) { akl_errf(rt, "oom: string"); return UINT32_MAX; }
    if (n) memcpy(cp, p, n);
    AklObj *o = &rt->objs[idx];
    o->kind = AKL_OK_STR; o->len = n; o->bytes = cp;
    rt->heap_bytes += n;
    return idx;
}

/* 文字列アクセスの唯一の入口。ROPE はここで初回だけ平坦化して STR に置換する。
 * 失敗（OOM/budget）は rt->err を立てて空文字を返すので、VM 側の呼出部は
 * 直後に rt->err[0] を検査すること（検査しない経路では誤って空同士の一致になり得る）。 */
/* ---- OBJ プロパティ（線形・上限 AKL_OBJ_MAX_PROPS）+ NATIVE 生成 ---- */
static i32 obj_prop_find(const AklObj *o, u32 name) {
    if (o->kind != AKL_OK_OBJ) return -1;
    for (u32 i = 0; i < o->u.po.n; i++)
        if (o->u.po.props[i].name == name) return (i32)i;
    return -1;
}
/* heap_bytes 課金は cap 差分で正確に追う（sweep が同単位で減算する対） */
static bool obj_prop_set(AklRT *rt, AklObj *o, u32 name, AklVal v) {
    i32 i = obj_prop_find(o, name);
    if (i >= 0) { o->u.po.props[i].v = v; return true; }
    if (o->u.po.n >= AKL_OBJ_MAX_PROPS) { akl_errf(rt, "property budget exhausted"); return false; }
    if (o->u.po.n == o->u.po.cap) {
        u32 nc = o->u.po.cap ? o->u.po.cap * 2 : 4;
        if (nc > AKL_OBJ_MAX_PROPS) nc = AKL_OBJ_MAX_PROPS;
        AklProp *np = (AklProp *)realloc(o->u.po.props, (u64)nc * sizeof(AklProp));
        if (!np) { akl_errf(rt, "oom: props"); return false; }
        rt->heap_bytes += (u64)(nc - o->u.po.cap) * sizeof(AklProp);
        o->u.po.props = np; o->u.po.cap = nc;
    }
    o->u.po.props[o->u.po.n].name = name;
    o->u.po.props[o->u.po.n].v = v;
    o->u.po.n++;
    return true;
}

static u32 akl_str_flatten(AklRT *rt, u32 idx, u8 **out_bytes); /* 前方宣言 */

/* UTF-8 バイト列のコードポイント数（s[i] / .length の単位。非 BMP は 1 と数える
 * 既知偏差 — UTF-16 code unit ではなく code point 単位。AKL_COMPAT に明記） */
static u32 akl_str_cp_count(const u8 *s, u32 n) {
    u32 cnt = 0;
    for (u32 i = 0; i < n; cnt++) {
        u8 c = s[i];
        i += c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
           : (c & 0xF8) == 0xF0 ? 4 : 1;
    }
    return cnt;
}
/* 位置 i のコードポイントのバイト長（範囲外は 0） */
static u32 akl_str_cp_len_at(const u8 *s, u32 n, u32 i) {
    u32 pos = 0;
    for (u32 k = 0; k < i; k++) {
        if (pos >= n) return 0;
        u8 c = s[pos];
        pos += c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
             : (c & 0xF8) == 0xF0 ? 4 : 1;
    }
    if (pos >= n) return 0;
    u8 c = s[pos];
    u32 cl = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
          : (c & 0xF8) == 0xF0 ? 4 : 1;
    return pos + cl <= n ? cl : n - pos;
}
static const u8 *akl_str(AklRT *rt, u32 idx, u32 *len) {
    if (idx >= rt->n_objs) { *len = 0; return (const u8 *)""; }
    AklObj *o = &rt->objs[idx];
    if (o->kind == AKL_OK_STR) { *len = o->len; return o->bytes; }
    if (o->kind == AKL_OK_ROPE) {
        u8 *fb = NULL;
        if (akl_str_flatten(rt, idx, &fb) != UINT32_MAX) { *len = rt->objs[idx].len; return rt->objs[idx].bytes; }
        *len = 0;
        return (const u8 *)"";
    }
    *len = 0; return (const u8 *)"";
}

/* stringly（STR or ROPE）判定。比較・連結・typeof・truthy で共通。 */
static bool akl_is_strly(AklRT *rt, AklVal v) {
    if (!akl_is_objv(v)) return false;
    u32 i = akl_get_obj(v);
    return i < rt->n_objs && (rt->objs[i].kind == AKL_OK_STR || rt->objs[i].kind == AKL_OK_ROPE);
}

/* 深さ（STR=0, ROPE=max(子)+1）。生成時に n_params へ格納済みのものを読むだけ。 */
static u32 akl_str_depth(AklRT *rt, u32 idx) { return rt->objs[idx].kind == AKL_OK_ROPE ? rt->objs[idx].n_params : 0; }

/* ROPE idx を平坦化し obj を同一 index の STR に置換する。
 * ルート: 自身を nursery に載せてから budget/GC を処理（子も到達可能に保持される）。
 * 深さは生成側で 4096 上限、ただし free-list 再構成耐性のため反復 DFS（C 再帰なし）。 */
static u32 akl_str_flatten(AklRT *rt, u32 idx, u8 **out_bytes) {
    AklObj *o = &rt->objs[idx];
    if (o->kind != AKL_OK_ROPE) { *out_bytes = o->kind == AKL_OK_STR ? o->bytes : NULL; return idx; }
    u32 total = o->len;
    if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = idx; /* GC から idx 一時保護 */
    while (rt->heap_bytes + total > rt->gc_next || rt->heap_bytes + total > ((u64)rt->heap_mb << 20)) {
        u32 before = (u32)(rt->heap_bytes & 0xFFFFFFFFu);
        akl_gc(rt);
        if ((u32)(rt->heap_bytes & 0xFFFFFFFFu) == before) break; /* 進捗なし: ループ防止 */
        if (rt->heap_bytes + total <= ((u64)rt->heap_mb << 20) && rt->heap_bytes + total <= rt->gc_next) break;
    }
    if (rt->heap_bytes + total > ((u64)rt->heap_mb << 20)) {
        if (rt->n_nury) rt->n_nury--;
        akl_errf(rt, "heap bytes budget exhausted");
        return UINT32_MAX;
    }
    /* 反復 DFS: 訪問順にセグメント逆順スタックを積む。最大深さ 4096 で有界。 */
    typedef struct { u32 idx; u32 pos; } Seg;
    Seg *stk = (Seg *)malloc(sizeof(Seg) * 8192 + 64);
    if (!stk) { if (rt->n_nury) rt->n_nury--; akl_errf(rt, "oom: flatten scratch"); return UINT32_MAX; }
    u8 *fb = (u8 *)malloc((u64)total + 1);
    if (!fb) { free(stk); if (rt->n_nury) rt->n_nury--; akl_errf(rt, "oom: flatten"); return UINT32_MAX; }
    stk[0].idx = idx; stk[0].pos = 0;
    u32 sn = 1, w = 0;
    while (sn) {
        Seg cur = stk[--sn];
        AklObj *no = &rt->objs[cur.idx];
        if (no->kind == AKL_OK_STR) {
            if (w + no->len > total) { free(stk); free(fb); if (rt->n_nury) rt->n_nury--; akl_errf(rt, "rope integrity"); return UINT32_MAX; }
            memcpy(fb + w, no->bytes, no->len);
            w += no->len;
        } else if (no->kind == AKL_OK_ROPE) {
            if (sn + 2 > 8192) { free(stk); free(fb); if (rt->n_nury) rt->n_nury--; akl_errf(rt, "rope depth integrity"); return UINT32_MAX; }
            /* 右→左の順で積み、左から処理（cur.pos は可視化用の予約。左右一体で網羅） */
            stk[sn].idx = no->name;  stk[sn].pos = 0; sn++;
            stk[sn].idx = no->code_off; stk[sn].pos = 0; sn++;
        } else {
            /* GC 競合や整合違反: 空セグメント扱い（伝播で守られているので到達不能設計） */
        }
    }
    free(stk);
    /* obj を同一 index で STR 化（子は以後到達不能になり次回 GC で回収される） */
    o->kind = AKL_OK_STR;
    o->bytes = fb;
    rt->heap_bytes += total;
    if (rt->n_nury) rt->n_nury--;
    *out_bytes = fb;
    return idx;
}

/* ============================== lexer ============================== */

enum {
    TK_EOF = 0, TK_IDENT, TK_NUM, TK_STR,
    TK_PUNCT,       /* ch = 1 文字、または2-3文字演算子を op 文字列 index で */
    TK_KW, TK_TPL   /* テンプレートリテラルの文字列断片（tpl_mid で ${ 後かどうか） */
};

typedef struct {
    const u8 *s; u32 n, pos, line;
    /* 現在トークン */
    u8 kind; u8 pk; /* KW: kw id / PUNCT: punct id */
    double num; bool num_is_int; i32 num_i;
    u32 str_len; const u8 *str_p; /* デコード済みは decode バッファに */
    u8 *esc; u32 esc_n, esc_cap;  /* 文字列デコード用（rt 外で確保） */
    u8 tpl_mid;   /* TK_TPL が ${...} の前（= 式が続く）か */
} Lex;

enum { KW_VAR, KW_LET, KW_CONST, KW_FUNCTION, KW_RETURN, KW_IF, KW_ELSE, KW_WHILE,
       KW_FOR, KW_BREAK, KW_CONTINUE, KW_TRUE, KW_FALSE, KW_NULL, KW_UNDEFINED, KW_TYPEOF,
       KW_THROW, KW_TRY, KW_CATCH, KW_FINALLY,
       KW_DO, KW_SWITCH, KW_CASE, KW_DEFAULT, KW_THIS,
       KW_VOID, KW_DELETE, KW_IN, KW_NEW, KW_OF, KW_INSTANCEOF,
       KW_CLASS, KW_EXTENDS, KW_STATIC, KW_SUPER,
       KW_DEBUGGER,
       KW_N };
static const char *const AKL_KWS[KW_N] = {
    "var", "let", "const", "function", "return", "if", "else", "while",
    "for", "break", "continue", "true", "false", "null", "undefined", "typeof",
    "throw", "try", "catch", "finally",
    "do", "switch", "case", "default", "this",
    "void", "delete", "in", "new", "of", "instanceof",
    "class", "extends", "static", "super", "debugger"
};

enum { P_LP, P_RP, P_LC, P_RC, P_SEMI, P_COMMA, P_ASSIGN, P_PLUS, P_MINUS, P_STAR,
       P_SLASH, P_PCT, P_BANG, P_LT, P_LE, P_GT, P_GE, P_EQEQ, P_NEQ, P_SEQ, P_SNE,
       P_ANDAND, P_OROR, P_DOT, P_COLON,
       /* v0.3: 配列・ブラケット・三項・増減・複合代入・ビット演算 */
       P_LBR, P_RBR, P_QMARK, P_INC, P_DEC,
       P_ADDASS, P_SUBASS, P_MULASS, P_DIVASS, P_MODASS,
       P_SHL, P_SHR, P_USHR, P_BAND, P_BOR, P_BXOR, P_BNOT,
       P_SHLASS, P_SHRASS, P_USHRASS, P_ANDASS, P_ORASS, P_XORASS,
       /* v0.3 JS 全構文: 冪乗・オプショナルチェーン・ヌル合体・rest/spread・テンプレート */
       P_POW, P_POWASS, P_QMARKDOT, P_QQ, P_ELLIPSIS, P_BACKTICK,
       /* v0.4: 論理代入 */
       P_ANDANDASS, P_ORORASS, P_QQASS,
       P_N };
static const char *const AKL_PUNCTS[P_N] = {
    "(", ")", "{", "}", ";", ",", "=", "+", "-", "*", "/", "%", "!", "<", "<=", ">", ">=",
    "==", "!=", "===", "!==", "&&", "||", ".", ":",
    "[", "]", "?", "++", "--",
    "+=", "-=", "*=", "/=", "%=",
    "<<", ">>", ">>>", "&", "|", "^", "~",
    "<<=", ">>=", ">>>=", "&=", "|=", "^=",
    "**", "**=", "?.", "??", "...", "`",
    "&&=", "||=", "?\?="
};

static bool lex_eof(Lex *lx) { return lx->pos >= lx->n; }
static u8 lex_cur(Lex *lx) { return lex_eof(lx) ? 0 : lx->s[lx->pos]; }
static u8 lex_at(Lex *lx, u32 off) { return lx->pos + off >= lx->n ? 0 : lx->s[lx->pos + off]; }

static u8 ascii_lc(u8 c) { return (c >= 'A' && c <= 'Z') ? (u8)(c + 32) : c; }

static void lex_skip_ws(Lex *lx) {
    for (;;) {
        u8 c = lex_cur(lx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { lx->pos++; continue; }
        if (c == '\n') { lx->pos++; lx->line++; continue; }
        if (c == '/' && lex_at(lx, 1) == '/') {
            while (!lex_eof(lx) && lex_cur(lx) != '\n') lx->pos++;
            continue;
        }
        if (c == '/' && lex_at(lx, 1) == '*') {
            lx->pos += 2;
            while (lx->pos + 1 < lx->n && !(lx->s[lx->pos] == '*' && lx->s[lx->pos + 1] == '/')) {
                if (lx->s[lx->pos] == '\n') lx->line++;
                lx->pos++;
            }
            lx->pos = lx->pos + 2 <= lx->n ? lx->pos + 2 : lx->n;
            continue;
        }
        return;
    }
}

static int hex_dig(u8 c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = ascii_lc(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static u32 lex_utf8_emit(Lex *lx, u32 cp) {
    u8 tmp[4]; u32 k = 0;
    if (cp < 0x80) tmp[k++] = (u8)cp;
    else if (cp < 0x800) { tmp[k++] = (u8)(0xC0 | (cp >> 6)); tmp[k++] = (u8)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { tmp[k++] = (u8)(0xE0 | (cp >> 12)); tmp[k++] = (u8)(0x80 | ((cp >> 6) & 63)); tmp[k++] = (u8)(0x80 | (cp & 63)); }
    else { tmp[k++] = (u8)(0xF0 | (cp >> 18)); tmp[k++] = (u8)(0x80 | ((cp >> 12) & 63)); tmp[k++] = (u8)(0x80 | ((cp >> 6) & 63)); tmp[k++] = (u8)(0x80 | (cp & 63)); }
    if (lx->esc_n + k > lx->esc_cap) {
        u32 nc = lx->esc_cap ? lx->esc_cap * 2 : 64;
        while (lx->esc_n + k > nc) nc *= 2;
        u8 *nb = (u8 *)realloc(lx->esc, nc);
        if (!nb) return 0;
        lx->esc = nb; lx->esc_cap = nc;
    }
    memcpy(lx->esc + lx->esc_n, tmp, k);
    lx->esc_n += k;
    return k;
}

/* 文字列リテラル（引用符の中身を esc バッファにデコード）。-1=エラー */
/* 生バイト列を esc バッファへ追記（UTF-8 文字列リテラルのマルチバイト列は
 * バイト単位の再エンコードをせず原列を保持する。v0.3 実測: 各バイトをコード
 * ポイントとして lex_utf8_emit すると CJK リテラルが二重エンコード化する） */
static u32 lex_emit_raw(Lex *lx, const u8 *p, u32 k) {
    if (lx->esc_n + k > lx->esc_cap) {
        u32 nc = lx->esc_cap ? lx->esc_cap * 2 : 64;
        while (lx->esc_n + k > nc) nc *= 2;
        u8 *nb = (u8 *)realloc(lx->esc, nc);
        if (!nb) return 0;
        lx->esc = nb; lx->esc_cap = nc;
    }
    memcpy(lx->esc + lx->esc_n, p, k);
    lx->esc_n += k;
    return k;
}

static int lex_string(Lex *lx) {
    u8 q = lx->s[lx->pos++];
    lx->esc_n = 0;
    for (;;) {
        if (lex_eof(lx) || lex_cur(lx) == '\n') return -1;
        u8 c = lx->s[lx->pos++];
        if (c == q) break;
        if (c != '\\') {
            if (c >= 0x80) {
                /* UTF-8 マルチバイト列: lead から列長を決めて原列を複製 */
                u32 seq = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
                u32 avail = lx->n - lx->pos;
                if (avail + 1 < seq) return -1; /* 途中 EOF は構文エラー */
                if (!lex_emit_raw(lx, lx->s + lx->pos - 1, seq)) return -1;
                lx->pos += seq - 1;
            } else {
                lex_utf8_emit(lx, c);
            }
            continue;
        }
        if (lex_eof(lx)) return -1;
        u8 e = lx->s[lx->pos++];
        switch (e) {
        case 'n': lex_utf8_emit(lx, '\n'); break;
        case 't': lex_utf8_emit(lx, '\t'); break;
        case 'r': lex_utf8_emit(lx, '\r'); break;
        case 'b': lex_utf8_emit(lx, '\b'); break;
        case 'f': lex_utf8_emit(lx, '\f'); break;
        case 'v': lex_utf8_emit(lx, '\v'); break;
        case '0': lex_utf8_emit(lx, 0); break;
        case '\\': case '\'': case '"': case '/': lex_utf8_emit(lx, e); break;
        case 'x': {
            int h1 = hex_dig(lex_cur(lx)), h2 = lex_eof(lx) ? -1 : hex_dig(lx->s[lx->pos + 1]);
            if (h1 < 0 || h2 < 0) return -1;
            lx->pos += 2;
            lex_utf8_emit(lx, (u32)(h1 * 16 + h2));
            break;
        }
        case 'u': {
            u32 cp = 0;
            for (int i = 0; i < 4; i++) {
                int h = hex_dig(lex_cur(lx));
                if (h < 0) return -1;
                cp = cp * 16 + (u32)h;
                lx->pos++;
            }
            lex_utf8_emit(lx, cp);
            break;
        }
        case '\n': lx->line++; break; /* line continuation */
        default: lex_utf8_emit(lx, e); break; /* 未知 escape は文字そのまま（寛容） */
        }
    }
    lx->str_p = lx->esc;
    lx->str_len = lx->esc_n;
    return 0;
}

/* 1 トークン読む。-1=エラー（u8 をわずかなので int） */
/* テンプレート断片: backtick または } の直後から、${ または閉じ backtick まで。
 * エスケープ: \` \$ \\ は次文字をそのまま。他は raw 保持（\n 等はそのまま）。 */
static int lex_template(Lex *lx) {
    u8 *buf = (u8 *)malloc(64);
    if (!buf) return -1;
    u32 bl = 0, cap = 64;
    bool mid = false;
    for (;;) {
        if (lex_eof(lx)) { free(buf); return -1; }
        u8 c = lx->s[lx->pos++];
        if (c == '`') break;
        if (c == '$' && lex_cur(lx) == '{') { lx->pos++; mid = true; break; }
        if (c == '\\' && !lex_eof(lx)) {
            u8 e = lx->s[lx->pos++];
            if (bl + 1 > cap) {
                cap *= 2;
                u8 *nb = (u8 *)realloc(buf, cap);
                if (!nb) { free(buf); return -1; }
                buf = nb;
            }
            buf[bl++] = e;
            continue;
        }
        if (bl + 1 > cap) {
            cap *= 2;
            u8 *nb = (u8 *)realloc(buf, cap);
            if (!nb) { free(buf); return -1; }
            buf = nb;
        }
        buf[bl++] = c;
    }
    free(lx->esc);
    lx->esc = buf;
    lx->esc_n = bl;
    lx->esc_cap = cap;
    lx->str_p = buf;
    lx->str_len = bl;
    lx->kind = TK_TPL;
    lx->tpl_mid = mid;
    return 0;
}

/* 数値リテラルの数字列を読み進める。_ は数字間の区切りとして許容（JS ES2021）。 */
static bool lex_digits(Lex *lx) {
    bool any = false;
    for (;;) {
        u8 c = lex_cur(lx);
        if (c >= '0' && c <= '9') { lx->pos++; any = true; continue; }
        if (c == '_') {
            if (!any || lex_at(lx, 1) < '0' || lex_at(lx, 1) > '9') return false;
            lx->pos++;
            continue;
        }
        break;
    }
    return any;
}

static int lex_next(Lex *lx) {
    lex_skip_ws(lx);
    if (lex_eof(lx)) { lx->kind = TK_EOF; return 0; }
    u8 c = lex_cur(lx);
    if (c == '`') { lx->pos++; return lex_template(lx); }
    if (c == '"' || c == '\'') {
        if (lex_string(lx) < 0) return -1;
        lx->kind = TK_STR;
        return 0;
    }
    if ((c >= '0' && c <= '9') || (c == '.' && lex_at(lx, 1) >= '0' && lex_at(lx, 1) <= '9')) {
        bool is_int = true;
        double v = 0;
        if (c == '0' && (lex_at(lx, 1) == 'x' || lex_at(lx, 1) == 'X')) {
            lx->pos += 2;
            if (hex_dig(lex_cur(lx)) < 0) return -1;
            double hv = 0;
            u32 guard = 0;
            while (hex_dig(lex_cur(lx)) >= 0 || lex_cur(lx) == '_') {
                if (lex_cur(lx) == '_') {
                    if (hex_dig(lex_at(lx, 1)) < 0) return -1;
                    lx->pos++;
                    continue;
                }
                hv = hv * 16 + (double)hex_dig(lex_cur(lx));
                lx->pos++;
                if (++guard > 16) return -1; /* 実効桁上限（巨大 16 進は拒否） */
            }
            lx->num = hv;
        } else if (c == '0' && (lex_at(lx, 1) == 'b' || lex_at(lx, 1) == 'B')) {
            lx->pos += 2;
            if (lex_cur(lx) != '0' && lex_cur(lx) != '1') return -1;
            double bv = 0;
            u32 guard = 0;
            while (lex_cur(lx) == '0' || lex_cur(lx) == '1' || lex_cur(lx) == '_') {
                if (lex_cur(lx) == '_') {
                    u8 nx = lex_at(lx, 1);
                    if (nx != '0' && nx != '1') return -1;
                    lx->pos++;
                    continue;
                }
                bv = bv * 2 + (double)(lex_cur(lx) - '0');
                lx->pos++;
                if (++guard > 48) return -1; /* 2^48 まで double で正確 */
            }
            lx->num = bv;
        } else if (c == '0' && (lex_at(lx, 1) == 'o' || lex_at(lx, 1) == 'O')) {
            lx->pos += 2;
            if (lex_cur(lx) < '0' || lex_cur(lx) > '7') return -1;
            double ov = 0;
            u32 guard = 0;
            while ((lex_cur(lx) >= '0' && lex_cur(lx) <= '7') || lex_cur(lx) == '_') {
                if (lex_cur(lx) == '_') {
                    u8 nx = lex_at(lx, 1);
                    if (nx < '0' || nx > '7') return -1;
                    lx->pos++;
                    continue;
                }
                ov = ov * 8 + (double)(lex_cur(lx) - '0');
                lx->pos++;
                if (++guard > 16) return -1; /* 8^16 = 2^48 まで正確 */
            }
            lx->num = ov;
        } else {
            {
                u32 dpos0 = lx->pos;
                if (!lex_digits(lx)) return -1;
                lx->pos = dpos0;
                while (lex_cur(lx) >= '0' && lex_cur(lx) <= '9') {
                    v = v * 10 + (double)(lex_cur(lx) - '0');
                    lx->pos++;
                }
                while (lex_cur(lx) == '_') { /* 区切りは数値計算では無視 */
                    lx->pos++;
                    while (lex_cur(lx) >= '0' && lex_cur(lx) <= '9') {
                        v = v * 10 + (double)(lex_cur(lx) - '0');
                        lx->pos++;
                    }
                }
            }
            if (lex_cur(lx) == '.') {
                is_int = false;
                lx->pos++;
                double frac = 0, scale = 1;
                while (lex_cur(lx) >= '0' && lex_cur(lx) <= '9') {
                    frac = frac * 10 + (double)(lex_cur(lx) - '0');
                    scale *= 10;
                    lx->pos++;
                }
                v += frac / scale;
            }
            if (lex_cur(lx) == 'e' || lex_cur(lx) == 'E') {
                is_int = false;
                lx->pos++;
                int sign = 1;
                if (lex_cur(lx) == '+' || lex_cur(lx) == '-') { sign = lex_cur(lx) == '-' ? -1 : 1; lx->pos++; }
                if (lex_cur(lx) < '0' || lex_cur(lx) > '9') return -1;
                int ex = 0;
                while (lex_cur(lx) >= '0' && lex_cur(lx) <= '9') {
                    if (ex < 10000) ex = ex * 10 + (lex_cur(lx) - '0');
                    lx->pos++;
                }
                v *= pow(10.0, (double)(sign * ex));
            }
            lx->num = v;
        }
        if (is_int) {
            double ad = fabs(lx->num);
            if (ad <= 2147483647.0) { lx->num_is_int = true; lx->num_i = (i32)lx->num; }
            else lx->num_is_int = false;
        } else lx->num_is_int = false;
        lx->kind = TK_NUM;
        return 0;
    }
    if (c == '_' || c == '$' || (ascii_lc(c) >= 'a' && ascii_lc(c) <= 'z')) {
        u32 st = lx->pos;
        while (!lex_eof(lx)) {
            u8 d = lex_cur(lx);
            if (d == '_' || d == '$' || (d >= '0' && d <= '9') || (ascii_lc(d) >= 'a' && ascii_lc(d) <= 'z')) lx->pos++;
            else break;
        }
        u32 ln = lx->pos - st;
        for (int k = 0; k < KW_N; k++) {
            if (strlen(AKL_KWS[k]) == ln && memcmp(lx->s + st, AKL_KWS[k], ln) == 0) {
                lx->kind = TK_KW; lx->pk = (u8)k;
                return 0;
            }
        }
        lx->kind = TK_IDENT; lx->str_p = lx->s + st; lx->str_len = ln;
        return 0;
    }
    /* punct 複数文字（最長一致。テーブル順序に依存しない: "==" が "===" を潰さない）。
     * v0.3: "++"/"--" は正式トークン化（二重 unary 誤読の危険は lex 最長一致で排除） */
    int best = -1; u32 bestl = 0;
    for (int k = 0; k < P_N; k++) {
        u32 pl = (u32)strlen(AKL_PUNCTS[k]);
        if (pl >= 2 && pl > bestl && lx->pos + pl <= lx->n &&
            memcmp(lx->s + lx->pos, AKL_PUNCTS[k], pl) == 0) {
            best = k; bestl = pl;
        }
    }
    if (best >= 0) {
        lx->pos += bestl; lx->kind = TK_PUNCT; lx->pk = (u8)best;
        return 0;
    }
    for (int k = 0; k < P_N; k++) {
        if (AKL_PUNCTS[k][1] == 0 && AKL_PUNCTS[k][0] == (char)c) {
            lx->pos++; lx->kind = TK_PUNCT; lx->pk = (u8)k;
            return 0;
        }
    }
    return -1;
}

/* ============================== AST ============================== */

enum {
    N_NUM, N_STR, N_BOOL, N_NULL, N_UNDEF, N_IDENT,
    N_UNARY, N_BIN, N_ASSIGN,
    N_CALL, N_FUNC, N_VAR,
    N_EXPRSTMT, N_IF, N_WHILE, N_FOR, N_BLOCK, N_RET, N_BREAK, N_CONTINUE, N_PROG,
    N_THROW, N_TRY,
    N_PGET,     /* a=obj node, b=name STR obj idx */
    N_PSET,     /* a=obj node, b=val node, c=name STR obj idx */
    N_MCALL,    /* a=obj node, b=args first(list), c=argc, d=name STR obj idx */
    N_OBJLIT,   /* a=names first(list), b=values first(list), c=count */
    /* v0.3 JS 全構文 */
    N_COMMA,    /* a=左, b=右（カンマ演算子） */
    N_OPTCHAIN, /* a=base, b=name/idx/args, c=0:prop 1:index 2:call, d=argc(call) */
    N_QQ,       /* a=左, b=右（?? ヌル合体。null/undefined でなければ左） */
    N_DELETE,   /* a=target（N_PGET|N_INDEX|N_IDENT） */
    N_IN,       /* a=key, b=obj（in 演算子） */
    N_NEW,      /* a=callee, b=args first(list), c=argc */
    N_TPL,      /* テンプレートリテラル: 断片は list（N_STR or 式）、c=断片数。
                 * list[n->b] = 先頭テキスト STR、以降 交互に 式, テキスト STR。
                 * 偶数 index がテキスト、奇数 index が式（式は N_NONE=無し不可）。 */
    N_SPREAD,   /* a=expr（spread 要素/引数） */
    N_FORIN,    /* a=ループ変数名（N_VAR 内包: a=name, b=init=NONE）, b=対象式, c=body */
    N_DESTR,    /* 分割代入: a=要素 list first, c=count, b=右辺（var 宣言時）, flags bit0=オブジェクト */
    N_DSTR_EL,  /* 要素: a=名前 idx, b=プロパティ名 idx（オブジェクト時）, c=ネスト N_DESTR idx,
                 *   flags: 0=配列 1=オブジェクト 2=ネスト */
    N_DSTR_REST,/* rest 要素: a=名前 idx */
    N_CLASS,    /* a=クラス名 idx, b=メンバー list first, c=count */
    N_CLASSMETH,/* a=メソッド名 idx, b=params first, c=params count, d=body,
                 * flags bit0=static bit1=constructor */
    N_DEFPARAM, /* デフォルト引数: a=name idx, b=default expr */
    N_RESTPARAM,/* rest 引数: a=name idx */
    /* v0.3 追加 */
    N_THIS,     /* this キーワード */
    N_ARRLIT,   /* b=elements first(list), c=count */
    N_INDEX,    /* a=obj node, b=index expr node（読み出し） */
    N_INDEXSET, /* a=obj node, b=index node, c=val node（代入） */
    N_TERN,     /* a=cond, b=then, c=else */
    N_ASSIGNOP, /* a=target node（N_IDENT|N_PGET|N_INDEX）, b=rhs node, op=二項演算 opcode */
    N_INCDEC,   /* a=target node, op=0(++) / 1(--), flags bit0=後置 */
    N_DOWHILE,  /* a=body, b=cond */
    N_SWITCH,   /* a=disc node, b=case list first, c=count */
    N_CASE,     /* a=expr（N_NONE=default）, b=body stmt */
    N_FUNCEXPR, /* 関数式: N_FUNC と同形（d=body）。a=name idx（無名は UINT32_MAX） */
    N_LABEL,    /* ラベル文: a=ラベル名 idx, b=本体文, flags bit0=本体がループ */
    N_OBJKEY,   /* オブジェクト computed キー: a=キー式, b=値（N_OBJLIT の要素） */
    N_LOGASSIGN,/* 論理代入: a=target, b=rhs, op=0(||=) 1(&&=) 2(??=) */
    N_REGEX,    /* 正規表現リテラル: a=pattern STR idx, b=flags（AKL_RX_F_*） */
    N_SUPER,    /* super: 疑似識別子（親クラスオブジェクト = SUPER_NAME ローカル） */
    N_SUPERGET, /* super.name: b=name STR idx */
    N_SUPERMCALL, /* super.name(...): b=name, c=args first, d=argc */
    N_SUPERCALL,  /* super(...): b=args first, c=argc（親 constructor を this で） */
    N_NONE = 0xFFFFFFFFu
};

typedef struct { u8 kind; u8 op; u8 flags; u8 _p; u32 a, b, c, d; } AklNode; /* 20B */

typedef struct { u32 *v; u32 n, cap; } U32Vec;

/* 関数ごとのクロージャ捕捉解析結果（akl_analyze が確保。codegen が参照） */
typedef struct {
    u32 *cap_names;  /* この関数の capture 対象ローカル名。位置 = ENV 内 idx */
    u16 n_cap;       /* cap_names 数 = 自前 ENV スロット数（n_env） */
    u16 needs_cap;   /* CELOAD/CESTORE を発行する、または内側関数へ env を中継する */
    U32Vec decls;    /* この関数が宣言する名前（解析中のみ使用） */
    u16 done;
} AklFnInfo;

typedef struct {
    AklRT *rt;
    Lex lx;
    AklNode *nodes; u32 n_nodes, cap_nodes;
    u32 *list; u32 n_list, cap_list;     /* 引数・パラメータ・文の並び */
    AklFnInfo *fninfo;                   /* 関数ノードごとの捕捉解析結果（akl_analyze が確保） */
    u32 fninfo_n;                        /* fninfo 配列の長さ（合成ノードは範囲外） */
    AklFnInfo main_fi;                   /* main 擬似関数の捕捉情報 */
    u32 depth;
    const char *fail;                    /* 構文エラーの原因（短い固定文） */
    char fail_buf[128];                  /* 詳細メッセージ用（regex 等） */
    u32 super_ctx;                       /* class メソッド body のパース中 > 0（super 許可） */
    u32 super_name;                      /* 親クラス保持ローカルの疑似名（intern id） */
    u32 super_prop;                      /* クラスオブジェクトの親参照プロパティ名（intern id） */
} P;

static u32 p_node(P *p, u8 kind) {
    if (p->n_nodes >= AKL_MAX_NODES) { p->fail = "node budget exhausted"; return N_NONE; }
    if (p->n_nodes == p->cap_nodes) {
        u32 nc = p->cap_nodes ? p->cap_nodes * 2 : 256;
        AklNode *nn = (AklNode *)realloc(p->nodes, (u64)nc * sizeof(AklNode));
        if (!nn) { p->fail = "oom: nodes"; return N_NONE; }
        p->nodes = nn; p->cap_nodes = nc;
    }
    AklNode *n = &p->nodes[p->n_nodes];
    memset(n, 0, sizeof *n);
    n->kind = kind;
    n->a = n->b = n->c = n->d = N_NONE;
    return p->n_nodes++;
}

static u32 p_list_push(P *p, u32 v) {
    if (p->n_list >= AKL_MAX_NODES * 2) { p->fail = "list budget exhausted"; return N_NONE; }
    if (p->n_list == p->cap_list) {
        u32 nc = p->cap_list ? p->cap_list * 2 : 128;
        u32 *nl = (u32 *)realloc(p->list, (u64)nc * sizeof(u32));
        if (!nl) { p->fail = "oom: list"; return N_NONE; }
        p->list = nl; p->cap_list = nc;
    }
    p->list[p->n_list++] = v;
    return p->n_list - 1;
}

/* parse scratch: 収集中のアイテム列。commit まで p->list を穢さない。
 * 「first = n_list を捕って push し続ける」方式は、ネストした収集（call 引数・params・
 * 内側ブロック）の中間 push が親区画に混入するため構造的に破綻する。全収集サイトは
 * scratch に集めてから commit で一括連結する（これで区画 [first, first+cnt) は常に不純物なし） */
static int p_scratch(P *p, U32Vec *t, u32 x) {
    if (t->n == t->cap) {
        u32 nc = t->cap ? t->cap * 2 : 8;
        u32 *nv = (u32 *)realloc(t->v, (u64)nc * sizeof(u32));
        if (!nv) { p->fail = "oom: list"; return -1; }
        t->v = nv; t->cap = nc;
    }
    t->v[t->n++] = x;
    return 0;
}
/* scratch を p->list 末尾へ連結。先頭 index を返す（失敗時 N_NONE、p->fail 設定済み） */
static u32 p_list_commit(P *p, const U32Vec *t) {
    u32 first = p->n_list;
    for (u32 i = 0; i < t->n; i++)
        if (p_list_push(p, t->v[i]) == N_NONE) return N_NONE;
    return first;
}

static bool p_is_punct(P *p, u8 pk) { return p->lx.kind == TK_PUNCT && p->lx.pk == pk; }
static bool p_is_kw(P *p, u8 kw) { return p->lx.kind == TK_KW && p->lx.pk == kw; }
static bool p_eat_punct(P *p, u8 pk) {
    if (!p_is_punct(p, pk)) return false;
    /* lex_next の失敗（未知文字等）を握り潰すと lx.kind が陳腐化して同じトークンを
     * 食い続ける無限再帰になる（fuzz: "--." で p_unary が C スタック枯渇）。
     * 必ず失敗を上流へ伝播させる。 */
    if (lex_next(&p->lx) < 0) { p->fail = "lex error"; return true; }
    return true;
}
static bool p_expect_punct(P *p, u8 pk, const char *what) {
    if (p_eat_punct(p, pk)) return true;
    p->fail = what;
    return false;
}

/* 識別子を intern して obj index を返す（e dedupe: STR 実体の線形一致検査） */
/* intern: 実行時/コンパイル時共通（公開 API の akl_prop_set もこれを使う）。
 * mkstr は GC を発火し得るため、呼出側は VM 停止中 or ルート同期済みの文脈限定。
 * created != NULL は「新規に STR を作ったか」を返す： VM 実行中（native 内）に
 * 新規作成した STR はルートに届くまで sweep され得るため、呼出側が nursery に積む
 * 責任を持つ（akl_prop_get/set が遵守する規約）。 */
static u32 akl_intern(AklRT *rt, const u8 *s, u32 n, bool *created) {
    if (created) *created = false;
    for (u32 i = 0; i < rt->n_objs; i++) {
        if (rt->objs[i].kind != AKL_OK_STR) continue;
        if (rt->objs[i].len == n && (n == 0 || memcmp(rt->objs[i].bytes, s, n) == 0))
            return i;
    }
    u32 k = akl_mkstr(rt, s, n);
    if (created && k != UINT32_MAX) *created = true;
    return k;
}
static u32 p_intern(P *p, const u8 *s, u32 n) {
    u32 idx = akl_intern(p->rt, s, n, NULL);
    if (idx == UINT32_MAX) p->fail = "intern failed";
    return idx;
}

/* ---- 式（再帰下降、優先順位段ごと） ---- */
static u32 p_expr(P *p);
static u32 p_expr_comma(P *p);
static u32 p_new_callee(P *p);
static u32 p_primary(P *p);
static u32 p_regex_literal(P *p);
static u32 p_params(P *p, u32 *first, u32 *cnt);
static u32 p_block_tail(P *p);

/* 呼出し arg 列: 現在トークンが '(' である前提で '(' ... ')' を消費し list commit する。
 * 失敗時 false（p->fail 設定済み）。depth 中立。 */
static bool p_args(P *p, u32 *first, u32 *cnt) {
    if (!p_expect_punct(p, P_LP, "expected '('")) return false;
    U32Vec sc = { NULL, 0, 0 };
    if (!p_is_punct(p, P_RP)) {
        for (;;) {
            u32 arg;
            if (p_eat_punct(p, P_ELLIPSIS)) { /* spread 引数 */
                u32 s = p_expr(p);
                if (s == N_NONE) { free(sc.v); return false; }
                arg = p_node(p, N_SPREAD);
                if (arg == N_NONE) { free(sc.v); return false; }
                p->nodes[arg].a = s;
            } else {
                arg = p_expr(p);
                if (arg == N_NONE) { free(sc.v); return false; }
            }
            if (p_scratch(p, &sc, arg) < 0) { free(sc.v); return false; }
            if (!p_eat_punct(p, P_COMMA)) break;
        }
    }
    if (!p_expect_punct(p, P_RP, "expected ')'")) { free(sc.v); return false; }
    if (sc.n > 250) { p->fail = "too many arguments"; free(sc.v); return false; }
    u32 f = p_list_commit(p, &sc);
    *cnt = sc.n;
    free(sc.v);
    if (*cnt && f == N_NONE) return false;
    *first = f;
    return true;
}

/* 後置連鎖: ( args ) 呼出し / . name / . name ( args ) メソッド呼出しを貪欲に畳む。
 * depth 中立（arg 内の式再帰は p_expr 側で会計済み）。 */
static u32 p_postfix(P *p, u32 base) {
    for (;;) {
        if (p_is_punct(p, P_LP)) {
            u32 first, cnt;
            if (!p_args(p, &first, &cnt)) return N_NONE;
            u32 call = p_node(p, N_CALL);
            if (call == N_NONE) return N_NONE;
            p->nodes[call].a = base;
            p->nodes[call].b = first;
            p->nodes[call].c = cnt;
            base = call;
        } else if (p_eat_punct(p, P_DOT)) {
            if (p->lx.kind != TK_IDENT && p->lx.kind != TK_KW) { p->fail = "expected property name after '.'"; return N_NONE; }
            u32 name = p_intern(p, p->lx.str_p, p->lx.str_len);
            if (name == UINT32_MAX) return N_NONE;
            lex_next(&p->lx);
            if (p_is_punct(p, P_LP)) {
                u32 first, cnt;
                if (!p_args(p, &first, &cnt)) return N_NONE;
                u32 mc = p_node(p, N_MCALL);
                if (mc == N_NONE) return N_NONE;
                p->nodes[mc].a = base;
                p->nodes[mc].b = first;
                p->nodes[mc].c = cnt;
                p->nodes[mc].d = name;
                base = mc;
            } else {
                u32 pg = p_node(p, N_PGET);
                if (pg == N_NONE) return N_NONE;
                p->nodes[pg].a = base;
                p->nodes[pg].b = name;
                base = pg;
            }
        } else if (p_is_punct(p, P_QMARKDOT)) { /* オプショナルチェーン a?.b / a?.[i] / a?.(args) */
            lex_next(&p->lx);
            if (p_is_punct(p, P_LBR)) { /* a?.[i] */
                lex_next(&p->lx);
                u32 idx = p_expr(p);
                if (idx == N_NONE) return N_NONE;
                if (!p_expect_punct(p, P_RBR, "expected ']'")) return N_NONE;
                u32 oc = p_node(p, N_OPTCHAIN);
                if (oc == N_NONE) return N_NONE;
                p->nodes[oc].a = base;
                p->nodes[oc].b = idx;
                p->nodes[oc].c = 1; /* 1 = index アクセス */
                base = oc;
            } else if (p_is_punct(p, P_LP)) { /* a?.(args): 短絡呼び出し */
                u32 first, cnt;
                if (!p_args(p, &first, &cnt)) return N_NONE;
                u32 oc = p_node(p, N_OPTCHAIN);
                if (oc == N_NONE) return N_NONE;
                p->nodes[oc].a = base;
                p->nodes[oc].b = first;
                p->nodes[oc].c = 2; /* 2 = call */
                p->nodes[oc].d = cnt;
                base = oc;
            } else { /* a?.name */
                if (p->lx.kind != TK_IDENT && p->lx.kind != TK_KW) { p->fail = "expected property name after '?.'"; return N_NONE; }
                u32 name = p_intern(p, p->lx.str_p, p->lx.str_len);
                if (name == UINT32_MAX) return N_NONE;
                lex_next(&p->lx);
                u32 oc = p_node(p, N_OPTCHAIN);
                if (oc == N_NONE) return N_NONE;
                p->nodes[oc].a = base;
                p->nodes[oc].b = name;
                p->nodes[oc].c = 0; /* 0 = prop アクセス */
                base = oc;
            }
        } else if (p_eat_punct(p, P_LBR)) { /* ブラケットアクセス obj[expr] */
            u32 idx = p_expr(p);
            if (idx == N_NONE) return N_NONE;
            if (!p_expect_punct(p, P_RBR, "expected ']'")) return N_NONE;
            u32 ix = p_node(p, N_INDEX);
            if (ix == N_NONE) return N_NONE;
            p->nodes[ix].a = base;
            p->nodes[ix].b = idx;
            base = ix;
        } else if (p_is_punct(p, P_INC) || p_is_punct(p, P_DEC)) {
            /* 後置 ++/--（前置は p_unary が先に掴む。ここに来るのは p_postfix 経由のみ） */
            bool is_inc = p->lx.pk == P_INC;
            lex_next(&p->lx);
            u32 ni = p_node(p, N_INCDEC);
            if (ni == N_NONE) return N_NONE;
            p->nodes[ni].a = base;
            p->nodes[ni].op = is_inc ? 0 : 1;
            p->nodes[ni].flags = 1; /* 後置 */
            base = ni;
        } else break;
    }
    return base;
}

/* オブジェクトリテラル: `{ k: v, ... }` 現在トークンは '{'（式文脈のみ到達。
 * 文頭 '{' は p_stmt のブロックが優先して消費＝JS と同一の曖昧性解決）。
 * key は IDENT/KW/STR。shorthand {a} / method shorthand / computed key は非対応（明白に拒否）。 */
/* getter/setter の非公開プロパティ名（ソースに現れない NUL 入り）。PLOAD/PSTORE が
 * 通常名に失敗したら "get:\x01name" / "set:\x01name" を探索して関数を呼ぶ。 */
static u32 p_special_prop_name(P *p, const char *kind, u32 name) {
    u32 kn = (u32)strlen(kind);
    u32 nl = 0;
    const u8 *np = akl_str(p->rt, name, &nl);
    if (!np) return UINT32_MAX;
    u8 buf[256];
    if (kn + 1 + nl > sizeof buf) return UINT32_MAX;
    memcpy(buf, kind, kn);
    buf[kn] = 0x01;
    memcpy(buf + kn + 1, np, nl);
    return akl_intern(p->rt, buf, kn + 1 + nl, NULL);
}

static u32 p_lit_object(P *p) {
    if (!p_expect_punct(p, P_LC, "expected '{'")) return N_NONE;
    U32Vec ks = { NULL, 0, 0 }, vs = { NULL, 0, 0 };
    if (!p_is_punct(p, P_RC)) {
        for (;;) {
            Lex *lx = &p->lx;
            u32 key;
            if (p_eat_punct(p, P_ELLIPSIS)) { /* オブジェクト spread: {...a, k: v} */
                u32 s = p_expr(p);
                if (s == N_NONE) goto fail;
                u32 spn = p_node(p, N_SPREAD);
                if (spn == N_NONE) goto fail;
                p->nodes[spn].a = s;
                if (p_scratch(p, &ks, UINT32_MAX) < 0 || p_scratch(p, &vs, spn) < 0) goto fail;
                if (p_eat_punct(p, P_COMMA)) {
                    if (p_is_punct(p, P_RC)) break;
                    continue;
                }
                break;
            }
            if (p_is_punct(p, P_LBR)) { /* computed key: { [expr]: v } */
                lex_next(&p->lx);
                u32 ke = p_expr(p);
                if (ke == N_NONE) goto fail;
                if (!p_expect_punct(p, P_RBR, "expected ']'")) goto fail;
                u32 okn = p_node(p, N_OBJKEY);
                if (okn == N_NONE) goto fail;
                p->nodes[okn].a = ke;
                if (p_is_punct(p, P_LP)) { /* computed メソッドは非対応（明示） */
                    p->fail = "computed method names are not supported";
                    goto fail;
                }
                if (!p_expect_punct(p, P_COLON, "expected ':' in object literal")) goto fail;
                u32 val = p_expr(p);
                if (val == N_NONE) goto fail;
                p->nodes[okn].b = val;
                if (p_scratch(p, &ks, UINT32_MAX) < 0 || p_scratch(p, &vs, okn) < 0) goto fail;
                if (p_eat_punct(p, P_COMMA)) {
                    if (p_is_punct(p, P_RC)) break;
                    continue;
                }
                break;
            }
            /* getter/setter 判定: 'get'/'set' + 名前 + '(' を先読み（Lex 全体退避） */
            bool is_acc = false, is_get = false, is_set = false;
            if (lx->kind == TK_IDENT && lx->str_len == 3 &&
                (memcmp(lx->str_p, "get", 3) == 0 || memcmp(lx->str_p, "set", 3) == 0)) {
                Lex saved = *lx;
                lex_next(lx);
                if (lx->kind == TK_IDENT) {
                    Lex saved2 = *lx;
                    lex_next(lx);
                    if (lx->kind == TK_PUNCT && lx->pk == P_LP) {
                        is_acc = true;
                        is_get = memcmp(saved.str_p, "get", 3) == 0;
                        *lx = saved2; /* 名前トークンで復元 */
                    } else {
                        *lx = saved;
                    }
                } else {
                    *lx = saved;
                }
            }
            if (is_acc) {
                /* アクセサ: 現在トークンは名前（復元済み） */
                if (lx->kind != TK_IDENT) { p->fail = "expected accessor name"; goto fail; }
                key = p_intern(p, lx->str_p, lx->str_len);
                if (key == UINT32_MAX) goto fail;
                lex_next(lx);
                if (!p_expect_punct(p, P_LP, "expected '('")) goto fail;
                u32 pf = 0, pc2 = 0;
                p_params(p, &pf, &pc2);
                if (p->fail) goto fail;
                if (is_set && pc2 > 1) { p->fail = "setter takes exactly one argument"; goto fail; }
                if (!p_expect_punct(p, P_LC, "expected '{'")) goto fail;
                p->super_ctx++;
                u32 body = p_block_tail(p);
                p->super_ctx--;
                if (body == N_NONE) goto fail;
                u32 cm = p_node(p, N_CLASSMETH);
                if (cm == N_NONE) goto fail;
                p->nodes[cm].a = key;
                p->nodes[cm].b = pf;
                p->nodes[cm].c = pc2;
                p->nodes[cm].d = body;
                p->nodes[cm].flags = is_get ? 4u : 8u;
                u32 skey = p_special_prop_name(p, is_get ? "get" : "set", key);
                if (skey == UINT32_MAX) goto fail;
                if (p_scratch(p, &ks, skey) < 0 || p_scratch(p, &vs, cm) < 0) goto fail;
                if (p_eat_punct(p, P_COMMA)) {
                    if (p_is_punct(p, P_RC)) break;
                    continue;
                }
                break;
            }
            /* 通常キー（IDENT / KW / STR） */
            if (lx->kind != TK_IDENT && lx->kind != TK_KW && lx->kind != TK_STR) {
                p->fail = "expected property key";
                goto fail;
            }
            key = p_intern(p, lx->str_p, lx->str_len);
            if (key == UINT32_MAX) goto fail;
            lex_next(lx);
            if (p_is_punct(p, P_LP)) { /* メソッド短縮: { m() {} } */
                lex_next(&p->lx); /* '(' を消費 */
                u32 pf = 0, pc2 = 0;
                p_params(p, &pf, &pc2);
                if (p->fail) goto fail;
                if (!p_expect_punct(p, P_LC, "expected '{'")) goto fail;
                p->super_ctx++;
                u32 body = p_block_tail(p);
                p->super_ctx--;
                if (body == N_NONE) goto fail;
                u32 cm = p_node(p, N_CLASSMETH);
                if (cm == N_NONE) goto fail;
                p->nodes[cm].a = key;
                p->nodes[cm].b = pf;
                p->nodes[cm].c = pc2;
                p->nodes[cm].d = body;
                if (p_scratch(p, &ks, key) < 0 || p_scratch(p, &vs, cm) < 0) goto fail;
                if (p_eat_punct(p, P_COMMA)) {
                    if (p_is_punct(p, P_RC)) break;
                    continue;
                }
                break;
            }
            if (p_is_punct(p, P_COMMA) || p_is_punct(p, P_RC)) { /* ショートハンド {a} */
                u32 val = p_node(p, N_IDENT);
                if (val == N_NONE) goto fail;
                p->nodes[val].a = key;
                if (p_scratch(p, &ks, key) < 0 || p_scratch(p, &vs, val) < 0) goto fail;
                if (p_eat_punct(p, P_COMMA)) {
                    if (p_is_punct(p, P_RC)) break;
                    continue;
                }
                break;
            }
            if (!p_expect_punct(p, P_COLON, "expected ':' in object literal")) goto fail;
            u32 val = p_expr(p);
            if (val == N_NONE) goto fail;
            if (p_scratch(p, &ks, key) < 0 || p_scratch(p, &vs, val) < 0) goto fail;
            if (p_eat_punct(p, P_COMMA)) {
                if (p_is_punct(p, P_RC)) break; /* trailing comma 許容 */
                continue;
            }
            break;
        }
    }
    if (!p_expect_punct(p, P_RC, "expected '}'")) goto fail;
    {
        u32 kf = p_list_commit(p, &ks);
        u32 vf = p_list_commit(p, &vs);
        u32 n = ks.n;
        free(ks.v); free(vs.v);
        if (n && (kf == N_NONE || vf == N_NONE)) { if (!p->fail) p->fail = "oom: object literal"; return N_NONE; }
        u32 ni = p_node(p, N_OBJLIT);
        if (ni != N_NONE) { p->nodes[ni].a = kf; p->nodes[ni].b = vf; p->nodes[ni].c = n; }
        return ni;
    }
fail:
    free(ks.v); free(vs.v);
    return N_NONE;
}

/* new の callee 専用パース。p_primary は IDENT ハンドラ内で p_postfix を呼ぶため、
 * `new P()` が `new (P())` になる（実測で特定）。ここではメンバーアクセス（. [ ]）だけを
 * 畳み、'(' は消費しない。 */
static u32 p_new_callee(P *p) {
    if (++p->depth > AKL_PARSE_DEPTH) { p->depth--; p->fail = "parse depth exhausted"; return N_NONE; }
    u32 base;
    if (p->lx.kind == TK_IDENT) {
        u32 idx = p_intern(p, p->lx.str_p, p->lx.str_len);
        base = idx == UINT32_MAX ? N_NONE : p_node(p, N_IDENT);
        if (base != N_NONE) p->nodes[base].a = idx;
        lex_next(&p->lx);
    } else if (p_is_kw(p, KW_NEW)) { /* new の再帰 */
        base = p_primary(p);
    } else if (p_is_punct(p, P_LP)) { /* (expr) の callee */
        lex_next(&p->lx);
        base = p_expr(p);
        if (base == N_NONE || !p_expect_punct(p, P_RP, "expected ')'")) { p->depth--; return N_NONE; }
    } else {
        p->fail = "expected constructor";
        p->depth--;
        return N_NONE;
    }
    if (base == N_NONE) { p->depth--; return N_NONE; }
    for (;;) {
        if (p_eat_punct(p, P_DOT)) {
            if (p->lx.kind != TK_IDENT && p->lx.kind != TK_KW) { p->fail = "expected property name"; p->depth--; return N_NONE; }
            u32 name = p_intern(p, p->lx.str_p, p->lx.str_len);
            if (name == UINT32_MAX) { p->depth--; return N_NONE; }
            lex_next(&p->lx);
            u32 pg = p_node(p, N_PGET);
            if (pg == N_NONE) { p->depth--; return N_NONE; }
            p->nodes[pg].a = base;
            p->nodes[pg].b = name;
            base = pg;
        } else if (p_eat_punct(p, P_LBR)) {
            u32 idx = p_expr(p);
            if (idx == N_NONE || !p_expect_punct(p, P_RBR, "expected ']'")) { p->depth--; return N_NONE; }
            u32 ix = p_node(p, N_INDEX);
            if (ix == N_NONE) { p->depth--; return N_NONE; }
            p->nodes[ix].a = base;
            p->nodes[ix].b = idx;
            base = ix;
        } else break;
    }
    p->depth--;
    return base;
}

/* 正規表現リテラル /pat/flags をスキャンして N_REGEX ノードを生成する。
 * 呼び出し時、p->lx は TK_PUNCT/P_SLASH を保持し、lx->pos は '/' の直後を指す。 */
static u32 p_regex_literal(P *p) {
    Lex *lx = &p->lx;
    u32 st = lx->pos - 1; /* '/' の位置 */
    u32 pos = st + 1;
    bool in_cls = false;
    for (;;) {
        if (pos >= lx->n) { p->fail = "unterminated regexp literal"; return N_NONE; }
        u8 c = lx->s[pos];
        if (c == '\n' || c == '\r') { p->fail = "unterminated regexp literal"; return N_NONE; }
        if (c == '\\') {
            if (pos + 1 >= lx->n || lx->s[pos + 1] == '\n' || lx->s[pos + 1] == '\r') {
                p->fail = "unterminated regexp literal";
                return N_NONE;
            }
            pos += 2;
            continue;
        }
        if (c == '[') in_cls = true;
        else if (c == ']') in_cls = false;
        else if (c == '/' && !in_cls) break;
        pos++;
    }
    u32 pat_st = st + 1;
    u32 pat_len = pos - pat_st;
    u32 pos2 = pos + 1;
    u32 flags = 0;
    while (pos2 < lx->n) {
        u8 c = lx->s[pos2];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) break;
        u32 bit;
        switch (c) {
        case 'i': bit = AKL_RX_F_IGNORE; break;
        case 'g': bit = AKL_RX_F_GLOBAL; break;
        case 'm': bit = AKL_RX_F_MULTI; break;
        case 's': bit = AKL_RX_F_DOTALL; break;
        case 'y': bit = AKL_RX_F_STICKY; break;
        case 'u': bit = AKL_RX_F_UNICODE; break;
        default: p->fail = "invalid regexp flag"; return N_NONE;
        }
        if (flags & bit) { p->fail = "duplicate regexp flag"; return N_NONE; }
        flags |= bit;
        pos2++;
    }
    /* パターンをコンパイル検証（実行時にも再コンパイルされる） */
    char rerr[96];
    AklRex *rx = akl_rex_compile(lx->s + pat_st, pat_len, flags, rerr, sizeof rerr);
    if (!rx) {
        snprintf(p->fail_buf, sizeof p->fail_buf, "%s", rerr);
        p->fail = p->fail_buf;
        return N_NONE;
    }
    akl_rex_free(rx);
    u32 idx = p_intern(p, lx->s + pat_st, pat_len);
    if (idx == UINT32_MAX) return N_NONE;
    u32 ni = p_node(p, N_REGEX);
    if (ni == N_NONE) return N_NONE;
    p->nodes[ni].a = idx;
    p->nodes[ni].b = flags;
    lx->pos = pos2;
    lex_next(lx);
    return ni;
}

static u32 p_primary(P *p) {
    if (p->fail) return N_NONE;
    if (++p->depth > AKL_PARSE_DEPTH) { p->depth--; p->fail = "parse depth exhausted"; return N_NONE; }
    Lex *lx = &p->lx;
    if (lx->kind == TK_NUM) {
        u32 ni = p_node(p, N_NUM);
        if (ni != N_NONE) {
            p->nodes[ni].op = lx->num_is_int ? 1 : 0;
            if (lx->num_is_int) p->nodes[ni].a = (u32)lx->num_i;
            else {
                AklVal bits;
                double d = lx->num;
                memcpy(&bits, &d, 8);
                p->nodes[ni].b = (u32)(bits >> 32);
                p->nodes[ni].a = (u32)bits;
            }
        }
        lex_next(lx);
        p->depth--;
        return ni;
    }
    if (lx->kind == TK_STR) {
        u32 idx = p_intern(p, lx->str_p, lx->str_len);
        u32 ni = idx == UINT32_MAX ? N_NONE : p_node(p, N_STR);
        if (ni != N_NONE) p->nodes[ni].a = idx;
        lex_next(lx);
        if (ni != N_NONE) ni = p_postfix(p, ni); /* 'abc'.length 等 */
        p->depth--;
        return ni;
    }
    if (p_is_kw(p, KW_TRUE))  { lex_next(lx); p->depth--; u32 ni = p_node(p, N_BOOL); if (ni != N_NONE) p->nodes[ni].a = 1; return ni; }
    if (p_is_kw(p, KW_FALSE)) { lex_next(lx); p->depth--; u32 ni = p_node(p, N_BOOL); if (ni != N_NONE) p->nodes[ni].a = 0; return ni; }
    if (p_is_kw(p, KW_NULL))  { lex_next(lx); p->depth--; return p_node(p, N_NULL); }
    if (p_is_kw(p, KW_UNDEFINED)) { lex_next(lx); p->depth--; return p_node(p, N_UNDEF); }
    if (lx->kind == TK_IDENT) {
        u32 idx = p_intern(p, lx->str_p, lx->str_len);
        u32 ni = idx == UINT32_MAX ? N_NONE : p_node(p, N_IDENT);
        if (ni != N_NONE) p->nodes[ni].a = idx;
        lex_next(lx);
        if (ni != N_NONE) ni = p_postfix(p, ni);
        p->depth--;
        return ni;
    }
    if (p_eat_punct(p, P_LP)) {
        u32 e = p_expr_comma(p); /* JS: (a, b) はカンマ式 */
        if (e == N_NONE) { p->depth--; return N_NONE; }
        if (!p_expect_punct(p, P_RP, "expected ')'")) { p->depth--; return N_NONE; }
        e = p_postfix(p, e); /* 括弧化式も呼出/プロパティ連鎖しうる */
        p->depth--;
        return e;
    }
    if (p_is_punct(p, P_LC)) { /* オブジェクトリテラル（式文脈のみ。文頭は p_stmt のブロック優先） */
        u32 ni = p_lit_object(p);
        if (ni != N_NONE) ni = p_postfix(p, ni);
        p->depth--;
        return ni;
    }
    if (p_is_punct(p, P_SLASH)) { /* 正規表現リテラル（式の開始位置のみ。除算は p_expr 側で処理） */
        u32 ni = p_regex_literal(p);
        if (ni != N_NONE) ni = p_postfix(p, ni);
        p->depth--;
        return ni;
    }
    if (p_is_punct(p, P_LBR)) { /* 配列リテラル: [ e, ... ]（trailing comma 許容。空可） */
        lex_next(&p->lx);
        U32Vec sc = { NULL, 0, 0 };
        if (!p_is_punct(p, P_RBR)) {
            for (;;) {
                u32 e;
                if (p_eat_punct(p, P_ELLIPSIS)) { /* spread 要素 */
                    u32 s = p_expr(p);
                    if (s == N_NONE) { free(sc.v); p->depth--; return N_NONE; }
                    e = p_node(p, N_SPREAD);
                    if (e == N_NONE) { free(sc.v); p->depth--; return N_NONE; }
                    p->nodes[e].a = s;
                } else {
                    e = p_expr(p);
                    if (e == N_NONE) { free(sc.v); p->depth--; return N_NONE; }
                }
                if (p_scratch(p, &sc, e) < 0) { free(sc.v); p->depth--; return N_NONE; }
                if (!p_eat_punct(p, P_COMMA)) break;
                if (p_is_punct(p, P_RBR)) break; /* trailing comma */
            }
        }
        if (!p_expect_punct(p, P_RBR, "expected ']'")) { free(sc.v); p->depth--; return N_NONE; }
        u32 f = p_list_commit(p, &sc);
        u32 cnt = sc.n;
        free(sc.v);
        u32 ni = (cnt && f == N_NONE) ? N_NONE : p_node(p, N_ARRLIT);
        if (ni != N_NONE) { p->nodes[ni].b = f; p->nodes[ni].c = cnt; }
        if (ni != N_NONE) ni = p_postfix(p, ni);
        p->depth--;
        return ni;
    }
    if (p_is_kw(p, KW_FUNCTION)) { /* 関数式: function [name] (params) { body }（名前は省略可） */
        lex_next(&p->lx);
        u32 name = UINT32_MAX;
        if (p->lx.kind == TK_IDENT) {
            name = p_intern(p, p->lx.str_p, p->lx.str_len);
            if (name == UINT32_MAX) { p->depth--; return N_NONE; }
            lex_next(&p->lx);
        }
        if (!p_expect_punct(p, P_LP, "expected '('")) { p->depth--; return N_NONE; }
        u32 pf = 0, pc2 = 0;
        p_params(p, &pf, &pc2);
        if (p->fail) { p->depth--; return N_NONE; }
        if (!p_expect_punct(p, P_LC, "expected '{'")) { p->depth--; return N_NONE; }
        u32 body = p_block_tail(p);
        if (body == N_NONE) { p->depth--; return N_NONE; }
        u32 ni = p_node(p, N_FUNCEXPR);
        if (ni == N_NONE) { p->depth--; return N_NONE; }
        p->nodes[ni].a = name;
        p->nodes[ni].b = pf;
        p->nodes[ni].c = pc2;
        p->nodes[ni].d = body;
        ni = p_postfix(p, ni);
        p->depth--;
        return ni;
    }
    if (p_is_kw(p, KW_THIS)) {
        lex_next(&p->lx);
        u32 ni = p_node(p, N_THIS);
        if (ni != N_NONE) ni = p_postfix(p, ni);
        p->depth--;
        return ni;
    }
    if (p_is_kw(p, KW_SUPER)) {
        /* super は class メソッド body 内のみ（p_class が super_ctx を立てる） */
        lex_next(&p->lx);
        if (!p->super_ctx) { p->fail = "super is not allowed here"; p->depth--; return N_NONE; }
        if (p_is_punct(p, P_LP)) { /* super(...) 親 constructor を this で呼ぶ */
            u32 af, ac;
            if (!p_args(p, &af, &ac)) { p->depth--; return N_NONE; }
            u32 ni = p_node(p, N_SUPERCALL);
            if (ni != N_NONE) { p->nodes[ni].b = af; p->nodes[ni].c = ac; }
            p->depth--;
            return ni;
        }
        if (p_eat_punct(p, P_DOT)) {
            if (p->lx.kind != TK_IDENT) { p->fail = "expected property name after super"; p->depth--; return N_NONE; }
            u32 name = p_intern(p, p->lx.str_p, p->lx.str_len);
            if (name == UINT32_MAX) { p->depth--; return N_NONE; }
            lex_next(&p->lx);
            if (p_is_punct(p, P_LP)) { /* super.m(...) */
                u32 af, ac;
                if (!p_args(p, &af, &ac)) { p->depth--; return N_NONE; }
                u32 ni = p_node(p, N_SUPERMCALL);
                if (ni != N_NONE) { p->nodes[ni].b = name; p->nodes[ni].c = af; p->nodes[ni].d = ac; }
                p->depth--;
                return ni;
            }
            u32 ni = p_node(p, N_SUPERGET);
            if (ni != N_NONE) p->nodes[ni].b = name;
            p->depth--;
            return ni;
        }
        p->fail = "expected '(' or '.' after super";
        p->depth--;
        return N_NONE;
    }
    if (p_is_kw(p, KW_NEW)) { /* new 演算子: new F(args) / new F().m() */
        lex_next(&p->lx);
        u32 callee = p_new_callee(p); /* callee は primary + メンバー（呼び出しは消費しない） */
        if (callee == N_NONE) { p->depth--; return N_NONE; }
        for (;;) {
            if (p_eat_punct(p, P_DOT)) {
                if (p->lx.kind != TK_IDENT && p->lx.kind != TK_KW) { p->fail = "expected property name after '.'"; p->depth--; return N_NONE; }
                u32 name = p_intern(p, p->lx.str_p, p->lx.str_len);
                if (name == UINT32_MAX) { p->depth--; return N_NONE; }
                lex_next(&p->lx);
                u32 pg = p_node(p, N_PGET);
                if (pg == N_NONE) { p->depth--; return N_NONE; }
                p->nodes[pg].a = callee;
                p->nodes[pg].b = name;
                callee = pg;
            } else if (p_eat_punct(p, P_LBR)) {
                u32 idx = p_expr(p);
                if (idx == N_NONE || !p_expect_punct(p, P_RBR, "expected ']'")) { p->depth--; return N_NONE; }
                u32 ix = p_node(p, N_INDEX);
                if (ix == N_NONE) { p->depth--; return N_NONE; }
                p->nodes[ix].a = callee;
                p->nodes[ix].b = idx;
                callee = ix;
            } else break;
        }
        u32 first = 0, cnt = 0;
        if (p_is_punct(p, P_LP)) {
            if (!p_args(p, &first, &cnt)) { p->depth--; return N_NONE; }
        }
        u32 ni = p_node(p, N_NEW);
        if (ni == N_NONE) { p->depth--; return N_NONE; }
        p->nodes[ni].a = callee;
        p->nodes[ni].b = first;
        p->nodes[ni].c = cnt;
        ni = p_postfix(p, ni); /* new F().m() の連鎖 */
        p->depth--;
        return ni;
    }
    if (p->lx.kind == TK_TPL) { /* テンプレートリテラル `...${expr}...` */
        U32Vec parts = { NULL, 0, 0 };
        bool ok = true;
        for (;;) {
            /* 現在 TK_TPL: テキスト断片（空でも intern） */
            u32 tname = p_intern(p, p->lx.str_p, p->lx.str_len);
            if (tname == UINT32_MAX) { ok = false; break; }
            u32 tn = p_node(p, N_STR);
            if (tn == N_NONE) { ok = false; break; }
            p->nodes[tn].a = tname;
            if (p_scratch(p, &parts, tn) < 0) { ok = false; break; }
            bool mid = p->lx.tpl_mid != 0;
            if (!mid) break; /* 閉じ backtick で終了 */
            /* ${ は lexer が消費済み: 式をパース */
            if (lex_next(&p->lx) < 0) { p->fail = "lex error"; ok = false; break; }
            u32 e = p_expr(p);
            if (e == N_NONE) { ok = false; break; }
            if (p_scratch(p, &parts, e) < 0) { ok = false; break; }
            /* } は lex_next が既に読んでいる（pos は } の直後）。
             * p_eat_punct は使わない（lex_next で次トークンまで読んでしまい、継続断片の
             * 開始位置がずれる — 実測で特定）。pos++ もしない（1 文字飛ばす）。 */
            if (!p_is_punct(p, P_RC)) { p->fail = "expected '}' in template"; ok = false; break; }
            /* } の直後からテンプレート断片を直接読む（lex_next は backtick 起点のため不可） */
            if (lex_template(&p->lx) < 0) { p->fail = "lex error"; ok = false; break; }
            if (p->lx.kind != TK_TPL) { p->fail = "expected template continuation"; ok = false; break; }
        }
        u32 ni = N_NONE;
        if (ok) {
            u32 first = p_list_commit(p, &parts);
            u32 cnt = parts.n;
            ni = (cnt && first == N_NONE) ? N_NONE : p_node(p, N_TPL);
            if (ni != N_NONE) { p->nodes[ni].b = first; p->nodes[ni].c = cnt; }
        }
        free(parts.v);
        if (lex_next(&p->lx) < 0) { p->fail = "lex error"; p->depth--; return N_NONE; } /* 次トークンへ */
        if (ni != N_NONE) ni = p_postfix(p, ni);
        p->depth--;
        return ni;
    }
    p->fail = "unexpected token in expression";
    p->depth--;
    return N_NONE;
}

static u32 p_unary(P *p) {
    if (p->fail) return N_NONE;
    /* 深さ会計必須: 単項演算子の連鎖（"-----x" 等）も C 再帰する。budget 節約で
     * 省略するとトークン不全時の再帰競走と相まって宿主プロセスを殺す（fuzz 由来）。 */
    if (++p->depth > AKL_PARSE_DEPTH) { p->depth--; p->fail = "parse depth exhausted"; return N_NONE; }
    int uop = 0;
    if (p_eat_punct(p, P_BANG)) uop = OP_NOT;
    else if (p_eat_punct(p, P_MINUS)) uop = OP_NEG;
    else if (p_eat_punct(p, P_PLUS)) uop = OP_POS;
    else if (p_eat_punct(p, P_BNOT)) uop = OP_BNOT;
    else if (p_is_kw(p, KW_TYPEOF)) { if (lex_next(&p->lx) < 0) p->fail = "lex error"; uop = OP_TYPEOF; }
    else if (p_is_kw(p, KW_VOID)) { if (lex_next(&p->lx) < 0) p->fail = "lex error"; uop = OP_POP + 0x100; } /* void: 後で POP;UNDEF */
    else if (p_is_kw(p, KW_DELETE)) {
        lex_next(&p->lx);
        u32 t = p_unary(p);
        if (t == N_NONE) { p->depth--; return N_NONE; }
        u8 tk = p->nodes[t].kind;
        if (tk == N_PGET || tk == N_INDEX || tk == N_IDENT) {
            u32 ni = p_node(p, N_DELETE);
            if (ni == N_NONE) { p->depth--; return N_NONE; }
            p->nodes[ni].a = t;
            p->depth--;
            return ni;
        }
        /* 対象外（リテラル等）: JS は true を返す。評価して捨てて true */
        p->depth--;
        return t; /* 呼出側で扱う: N_DELETE に包まない = 値は評価のみ（true 近似） */
    }
    u32 ni;
    if (!uop) {
        if (p_is_punct(p, P_INC) || p_is_punct(p, P_DEC)) { /* 前置 ++/--: 後置より高い優先度で掴む */
            bool is_inc = p->lx.pk == P_INC;
            lex_next(&p->lx);
            u32 t = p_unary(p);
            ni = t == N_NONE ? N_NONE : p_node(p, N_INCDEC);
            if (ni != N_NONE) { p->nodes[ni].a = t; p->nodes[ni].op = is_inc ? 0 : 1; p->nodes[ni].flags = 0; }
            p->depth--;
            return ni;
        }
        ni = p_primary(p);
    } else if (p->fail) ni = N_NONE;
    else if (uop == OP_POP + 0x100) { /* void expr: 評価して捨て undefined */
        u32 x = p_unary(p);
        if (x == N_NONE) ni = N_NONE;
        else {
            u32 vn = p_node(p, N_UNARY);
            if (vn != N_NONE) { p->nodes[vn].op = OP_POP; p->nodes[vn].a = x; } /* POP マーカ */
            u32 un = p_node(p, N_UNDEF);
            ni = (vn != N_NONE && un != N_NONE) ? p_node(p, N_COMMA) : N_NONE;
            if (ni != N_NONE) { p->nodes[ni].a = vn; p->nodes[ni].b = un; }
        }
    }
    else {
        u32 x = p_unary(p);
        ni = x == N_NONE ? N_NONE : p_node(p, N_UNARY);
        if (ni != N_NONE) { p->nodes[ni].op = uop; p->nodes[ni].a = x; }
    }
    p->depth--;
    return ni;
}

static u32 p_bin_rhs(P *p, u8 mk_op, u32 (*next)(P *)) {
    u32 lhs = next(p);
    if (lhs == N_NONE) return N_NONE;
    for (;;) {
        u8 pk = p->lx.pk;
        bool is_in = p_is_kw(p, KW_IN) && mk_op == P_LT;
        bool is_inst = p_is_kw(p, KW_INSTANCEOF) && mk_op == P_LT;
        if (!(p_is_punct(p, mk_op) ||
              (mk_op == P_EQEQ && (p_is_punct(p, P_NEQ) || p_is_punct(p, P_SEQ) || p_is_punct(p, P_SNE))) ||
              (mk_op == P_LT && (p_is_punct(p, P_LE) || p_is_punct(p, P_GT) || p_is_punct(p, P_GE) ||
                                 is_in || is_inst)))) break;
        lex_next(&p->lx);
        u32 rhs = next(p);
        if (rhs == N_NONE) return N_NONE;
        u8 op;
        if (is_in) op = OP_IN;
        else if (is_inst) op = OP_INSTANCEOF;
        else switch (pk) {
        case P_STAR: op = OP_MUL; break;
        case P_SLASH: op = OP_DIV; break;
        case P_PCT: op = OP_MOD; break;
        case P_PLUS: op = OP_ADD; break;
        case P_MINUS: op = OP_SUB; break;
        case P_LT: op = OP_LT; break;
        case P_LE: op = OP_LE; break;
        case P_GT: op = OP_GT; break;
        case P_GE: op = OP_GE; break;
        case P_EQEQ: op = OP_EQ; break;
        case P_NEQ: op = OP_NE; break;
        case P_SEQ: op = OP_SEQ; break;
        default: op = OP_SNE; break;
        }
        u32 ni = p_node(p, N_BIN);
        if (ni == N_NONE) return N_NONE;
        p->nodes[ni].op = op;
        p->nodes[ni].a = lhs;
        p->nodes[ni].b = rhs;
        lhs = ni;
    }
    return lhs;
}
static u32 p_pow(P *p) { /* ** 右結合（unary の下） */
    u32 lhs = p_unary(p);
    if (lhs == N_NONE) return N_NONE;
    if (p_is_punct(p, P_POW)) {
        lex_next(&p->lx);
        u32 rhs = p_pow(p); /* 右結合 */
        if (rhs == N_NONE) return N_NONE;
        u32 ni = p_node(p, N_BIN);
        if (ni == N_NONE) return N_NONE;
        p->nodes[ni].op = OP_POW;
        p->nodes[ni].a = lhs;
        p->nodes[ni].b = rhs;
        return ni;
    }
    return lhs;
}
static u32 p_mul(P *p) {
    u32 lhs = p_pow(p);
    if (lhs == N_NONE) return N_NONE;
    while (p_is_punct(p, P_STAR) || p_is_punct(p, P_SLASH) || p_is_punct(p, P_PCT)) {
        u8 pk = p->lx.pk;
        lex_next(&p->lx);
        u32 rhs = p_pow(p);
        if (rhs == N_NONE) return N_NONE;
        u32 ni = p_node(p, N_BIN);
        if (ni == N_NONE) return N_NONE;
        p->nodes[ni].op = pk == P_STAR ? OP_MUL : pk == P_SLASH ? OP_DIV : OP_MOD;
        p->nodes[ni].a = lhs;
        p->nodes[ni].b = rhs;
        lhs = ni;
    }
    return lhs;
}
static u32 p_add(P *p) {
    u32 lhs = p_mul(p);
    if (lhs == N_NONE) return N_NONE;
    while (p_is_punct(p, P_PLUS) || p_is_punct(p, P_MINUS)) {
        u8 pk = p->lx.pk;
        lex_next(&p->lx);
        u32 rhs = p_mul(p);
        if (rhs == N_NONE) return N_NONE;
        u32 ni = p_node(p, N_BIN);
        if (ni == N_NONE) return N_NONE;
        p->nodes[ni].op = pk == P_PLUS ? OP_ADD : OP_SUB;
        p->nodes[ni].a = lhs;
        p->nodes[ni].b = rhs;
        lhs = ni;
    }
    return lhs;
}
static u32 p_shift(P *p) { /* << >> >>> : p_add と p_rel の間 */
    u32 lhs = p_add(p);
    if (lhs == N_NONE) return N_NONE;
    while (p_is_punct(p, P_SHL) || p_is_punct(p, P_SHR) || p_is_punct(p, P_USHR)) {
        u8 pk = p->lx.pk;
        lex_next(&p->lx);
        u32 rhs = p_add(p);
        if (rhs == N_NONE) return N_NONE;
        u32 ni = p_node(p, N_BIN);
        if (ni == N_NONE) return N_NONE;
        p->nodes[ni].op = pk == P_SHL ? OP_BSHL : pk == P_SHR ? OP_BSHR : OP_BUSHR;
        p->nodes[ni].a = lhs;
        p->nodes[ni].b = rhs;
        lhs = ni;
    }
    return lhs;
}
static u32 p_rel(P *p)   { return p_bin_rhs(p, P_LT, p_shift); }
static u32 p_eq(P *p)    { return p_bin_rhs(p, P_EQEQ, p_rel); }
static u32 p_bitand(P *p) { /* & : p_eq と p_bitxor の間 */
    u32 lhs = p_eq(p);
    if (lhs == N_NONE) return N_NONE;
    while (p_is_punct(p, P_BAND)) {
        lex_next(&p->lx);
        u32 rhs = p_eq(p);
        if (rhs == N_NONE) return N_NONE;
        u32 ni = p_node(p, N_BIN);
        if (ni == N_NONE) return N_NONE;
        p->nodes[ni].op = OP_BAND;
        p->nodes[ni].a = lhs;
        p->nodes[ni].b = rhs;
        lhs = ni;
    }
    return lhs;
}
static u32 p_bitxor(P *p) { /* ^ */
    u32 lhs = p_bitand(p);
    if (lhs == N_NONE) return N_NONE;
    while (p_is_punct(p, P_BXOR)) {
        lex_next(&p->lx);
        u32 rhs = p_bitand(p);
        if (rhs == N_NONE) return N_NONE;
        u32 ni = p_node(p, N_BIN);
        if (ni == N_NONE) return N_NONE;
        p->nodes[ni].op = OP_BXOR;
        p->nodes[ni].a = lhs;
        p->nodes[ni].b = rhs;
        lhs = ni;
    }
    return lhs;
}
static u32 p_bitor(P *p) { /* | */
    u32 lhs = p_bitxor(p);
    if (lhs == N_NONE) return N_NONE;
    while (p_is_punct(p, P_BOR)) {
        lex_next(&p->lx);
        u32 rhs = p_bitxor(p);
        if (rhs == N_NONE) return N_NONE;
        u32 ni = p_node(p, N_BIN);
        if (ni == N_NONE) return N_NONE;
        p->nodes[ni].op = OP_BOR;
        p->nodes[ni].a = lhs;
        p->nodes[ni].b = rhs;
        lhs = ni;
    }
    return lhs;
}

static u32 p_logical_or(P *p);

/* カンマ演算子（式文・for の init/step・return・括弧式のみ。引数リストでは無効） */
static u32 p_expr_comma(P *p) {
    u32 lhs = p_expr(p);
    if (lhs == N_NONE) return N_NONE;
    while (p_is_punct(p, P_COMMA)) {
        lex_next(&p->lx);
        u32 rhs = p_expr(p);
        if (rhs == N_NONE) return N_NONE;
        u32 ni = p_node(p, N_COMMA);
        if (ni == N_NONE) return N_NONE;
        p->nodes[ni].a = lhs;
        p->nodes[ni].b = rhs;
        lhs = ni;
    }
    return lhs;
}

static u32 p_expr(P *p) {
    u32 lhs = p_logical_or(p);
    if (lhs == N_NONE) return N_NONE;
    if (p_is_punct(p, P_QQ)) { /* ?? ヌル合体: a ?? b = a が null/undefined でなければ a */
        lex_next(&p->lx);
        u32 rhs = p_expr(p); /* 右結合 */
        if (rhs == N_NONE) return N_NONE;
        u32 ni = p_node(p, N_QQ);
        if (ni == N_NONE) return N_NONE;
        p->nodes[ni].a = lhs;
        p->nodes[ni].b = rhs;
        return ni;
    }
    if (p_eat_punct(p, P_QMARK)) { /* 三項: cond ? then : else（右結合） */
        u32 th = p_expr(p);
        if (th == N_NONE) return N_NONE;
        if (!p_expect_punct(p, P_COLON, "expected ':' in conditional")) return N_NONE;
        u32 el = p_expr(p);
        if (el == N_NONE) return N_NONE;
        u32 ni = p_node(p, N_TERN);
        if (ni == N_NONE) return N_NONE;
        p->nodes[ni].a = lhs;
        p->nodes[ni].b = th;
        p->nodes[ni].c = el;
        return ni;
    }
    if (p_is_punct(p, P_ASSIGN) || p_is_punct(p, P_ADDASS) || p_is_punct(p, P_SUBASS) ||
        p_is_punct(p, P_MULASS) || p_is_punct(p, P_DIVASS) || p_is_punct(p, P_MODASS) ||
        p_is_punct(p, P_SHLASS) || p_is_punct(p, P_SHRASS) || p_is_punct(p, P_USHRASS) ||
        p_is_punct(p, P_ANDASS) || p_is_punct(p, P_ORASS) || p_is_punct(p, P_XORASS) ||
        p_is_punct(p, P_POWASS)) {
        u8 pk = p->lx.pk;
        u8 lk = p->nodes[lhs].kind;
        if (lk == N_DESTR) { /* 代入の左辺が分割パターン: [a,b] = expr */
            lex_next(&p->lx);
            u32 rhs = p_expr(p);
            if (rhs == N_NONE) return N_NONE;
            p->nodes[lhs].b = rhs;
            return lhs;
        }
        if (lk != N_IDENT && lk != N_PGET && lk != N_INDEX) { p->fail = "invalid assignment target"; return N_NONE; }
        lex_next(&p->lx);
        u32 rhs = p_expr(p);
        if (rhs == N_NONE) return N_NONE;
        if (pk == P_ASSIGN) {
            if (lk == N_PGET) {
                u32 ni = p_node(p, N_PSET);
                if (ni == N_NONE) return N_NONE;
                p->nodes[ni].a = p->nodes[lhs].a; /* obj node */
                p->nodes[ni].b = rhs;           /* val node */
                p->nodes[ni].c = p->nodes[lhs].b; /* name STR idx */
                return ni;
            }
            if (lk == N_INDEX) {
                u32 ni = p_node(p, N_INDEXSET);
                if (ni == N_NONE) return N_NONE;
                p->nodes[ni].a = p->nodes[lhs].a;
                p->nodes[ni].b = p->nodes[lhs].b;
                p->nodes[ni].c = rhs;
                return ni;
            }
            u32 ni = p_node(p, N_ASSIGN);
            if (ni == N_NONE) return N_NONE;
            p->nodes[ni].a = p->nodes[lhs].a; /* name idx */
            p->nodes[ni].b = rhs;
            return ni;
        }
        /* 複合代入（op は二項演算 opcode に写像） */
        u8 op;
        switch (pk) {
        case P_ADDASS: op = OP_ADD; break;
        case P_SUBASS: op = OP_SUB; break;
        case P_MULASS: op = OP_MUL; break;
        case P_DIVASS: op = OP_DIV; break;
        case P_MODASS: op = OP_MOD; break;
        case P_SHLASS: op = OP_BSHL; break;
        case P_SHRASS: op = OP_BSHR; break;
        case P_USHRASS: op = OP_BUSHR; break;
        case P_ANDASS: op = OP_BAND; break;
        case P_ORASS: op = OP_BOR; break;
        case P_POWASS: op = OP_POW; break;
        default: op = OP_BXOR; break;
        }
        u32 ni = p_node(p, N_ASSIGNOP);
        if (ni == N_NONE) return N_NONE;
        p->nodes[ni].a = lhs; /* target node（N_IDENT | N_PGET | N_INDEX） */
        p->nodes[ni].b = rhs;
        p->nodes[ni].op = op;
        return ni;
    }
    if (p_is_punct(p, P_ANDANDASS) || p_is_punct(p, P_ORORASS) || p_is_punct(p, P_QQASS)) {
        /* 論理代入: a ||= b / a &&= b / a ??= b（短絡。式の値 = 新値 or 元値） */
        u8 pk = p->lx.pk;
        u8 lk = p->nodes[lhs].kind;
        if (lk != N_IDENT && lk != N_PGET && lk != N_INDEX) { p->fail = "invalid assignment target"; return N_NONE; }
        lex_next(&p->lx);
        u32 rhs = p_expr(p);
        if (rhs == N_NONE) return N_NONE;
        u32 ni = p_node(p, N_LOGASSIGN);
        if (ni == N_NONE) return N_NONE;
        p->nodes[ni].a = lhs;
        p->nodes[ni].b = rhs;
        p->nodes[ni].op = (u8)(pk == P_ANDANDASS ? 1 : (pk == P_QQASS ? 2 : 0));
        return ni;
    }
    return lhs;
}

static u32 p_logical_and(P *p) {
    u32 lhs = p_bitor(p);
    if (lhs == N_NONE) return N_NONE;
    while (p_is_punct(p, P_ANDAND)) {
        lex_next(&p->lx);
        u32 rhs = p_bitor(p);
        if (rhs == N_NONE) return N_NONE;
        u32 ni = p_node(p, N_BIN);
        if (ni == N_NONE) return N_NONE;
        p->nodes[ni].op = OP_JMPF; /* マーカ: a が falsy なら rhs を飛ばす（&& 短絡） */
        p->nodes[ni].a = lhs;
        p->nodes[ni].b = rhs;
        lhs = ni;
    }
    return lhs;
}
static u32 p_logical_or(P *p) {
    u32 lhs = p_logical_and(p);
    if (lhs == N_NONE) return N_NONE;
    while (p_is_punct(p, P_OROR)) {
        lex_next(&p->lx);
        u32 rhs = p_logical_and(p);
        if (rhs == N_NONE) return N_NONE;
        u32 ni = p_node(p, N_BIN);
        if (ni == N_NONE) return N_NONE;
        p->nodes[ni].op = OP_JMPT; /* マーカ: a が truthy なら rhs を飛ばす（|| 短絡） */
        p->nodes[ni].a = lhs;
        p->nodes[ni].b = rhs;
        lhs = ni;
    }
    return lhs;
}

/* ---- 文 ---- */
static u32 p_stmt(P *p);

/* 分割代入パターンのパース。現在トークンは P_LBR（配列）または P_LC（オブジェクト）。
 * 返り値は N_DESTR ノード（右辺なし）。 */
static u32 p_destructure(P *p) {
    bool is_obj = p_is_punct(p, P_LC);
    lex_next(&p->lx);
    U32Vec sc = { NULL, 0, 0 };
    u32 idx = 0; /* 配列要素位置（スキップ分も数える） */
    if (!p_is_punct(p, is_obj ? P_RC : P_RBR)) {
        for (;;) {
            if (is_obj) {
                /* オブジェクトパターン: name | name: target | 'str': target | ...rest */
                if (p_eat_punct(p, P_ELLIPSIS)) {
                    if (p->lx.kind != TK_IDENT) { p->fail = "expected rest target"; free(sc.v); return N_NONE; }
                    u32 rname = p_intern(p, p->lx.str_p, p->lx.str_len);
                    if (rname == UINT32_MAX) { free(sc.v); return N_NONE; }
                    lex_next(&p->lx);
                    u32 rn = p_node(p, N_DSTR_REST);
                    if (rn == N_NONE) { free(sc.v); return N_NONE; }
                    p->nodes[rn].a = rname;
                    p->nodes[rn].flags = 1; /* オブジェクト rest */
                    if (p_scratch(p, &sc, rn) < 0) { free(sc.v); return N_NONE; }
                    if (!p_is_punct(p, P_RC)) { p->fail = "rest must be last"; free(sc.v); return N_NONE; }
                    break;
                }
                u32 key;
                if (p->lx.kind == TK_IDENT || p->lx.kind == TK_KW || p->lx.kind == TK_STR) {
                    key = p_intern(p, p->lx.str_p, p->lx.str_len);
                    if (key == UINT32_MAX) { free(sc.v); return N_NONE; }
                    lex_next(&p->lx);
                } else { p->fail = "expected property name"; free(sc.v); return N_NONE; }
                u32 target = key; /* shorthand: {a} は同名 */
                if (p_eat_punct(p, P_COLON)) {
                    if (p->lx.kind == TK_IDENT) {
                        target = p_intern(p, p->lx.str_p, p->lx.str_len);
                        if (target == UINT32_MAX) { free(sc.v); return N_NONE; }
                        lex_next(&p->lx);
                    } else if (p_is_punct(p, P_LBR) || p_is_punct(p, P_LC)) {
                        u32 nest = p_destructure(p);
                        if (nest == N_NONE) { free(sc.v); return N_NONE; }
                        u32 ne = p_node(p, N_DSTR_EL);
                        if (ne == N_NONE) { free(sc.v); return N_NONE; }
                        p->nodes[ne].b = key;
                        p->nodes[ne].c = nest;
                        p->nodes[ne].flags = 3; /* オブジェクト + ネスト */
                        if (p_scratch(p, &sc, ne) < 0) { free(sc.v); return N_NONE; }
                        if (!p_eat_punct(p, P_COMMA)) break;
                        if (p_is_punct(p, P_RC)) break;
                        continue;
                    } else { p->fail = "expected target"; free(sc.v); return N_NONE; }
                }
                u32 el = p_node(p, N_DSTR_EL);
                if (el == N_NONE) { free(sc.v); return N_NONE; }
                p->nodes[el].a = target;
                p->nodes[el].b = key;
                p->nodes[el].flags = 1; /* オブジェクト要素 */
                if (p_scratch(p, &sc, el) < 0) { free(sc.v); return N_NONE; }
            } else {
                /* 配列パターン: [a, b, , ...rest] */
                if (p_eat_punct(p, P_ELLIPSIS)) {
                    if (p->lx.kind != TK_IDENT) { p->fail = "expected rest target"; free(sc.v); return N_NONE; }
                    u32 rname = p_intern(p, p->lx.str_p, p->lx.str_len);
                    if (rname == UINT32_MAX) { free(sc.v); return N_NONE; }
                    lex_next(&p->lx);
                    u32 rn = p_node(p, N_DSTR_REST);
                    if (rn == N_NONE) { free(sc.v); return N_NONE; }
                    p->nodes[rn].a = rname;
                    p->nodes[rn].flags = 0;
                    p->nodes[rn].b = idx; /* 開始 index */
                    if (p_scratch(p, &sc, rn) < 0) { free(sc.v); return N_NONE; }
                    if (!p_is_punct(p, P_RBR)) { p->fail = "rest must be last"; free(sc.v); return N_NONE; }
                    break;
                }
                if (p_is_punct(p, P_COMMA)) { /* 空スロット（スキップ） */
                    idx++;
                    if (!p_eat_punct(p, P_COMMA)) break;
                    if (p_is_punct(p, P_RBR)) break;
                    continue;
                }
                u32 target;
                if (p->lx.kind == TK_IDENT) {
                    target = p_intern(p, p->lx.str_p, p->lx.str_len);
                    if (target == UINT32_MAX) { free(sc.v); return N_NONE; }
                    lex_next(&p->lx);
                } else if (p_is_punct(p, P_LBR) || p_is_punct(p, P_LC)) {
                    u32 nest = p_destructure(p);
                    if (nest == N_NONE) { free(sc.v); return N_NONE; }
                    u32 ne = p_node(p, N_DSTR_EL);
                    if (ne == N_NONE) { free(sc.v); return N_NONE; }
                    p->nodes[ne].b = idx;
                    p->nodes[ne].c = nest;
                    p->nodes[ne].flags = 2; /* 配列 + ネスト */
                    if (p_scratch(p, &sc, ne) < 0) { free(sc.v); return N_NONE; }
                    idx++;
                    if (!p_eat_punct(p, P_COMMA)) break;
                    if (p_is_punct(p, P_RBR)) break;
                    continue;
                } else { p->fail = "expected target"; free(sc.v); return N_NONE; }
                u32 el = p_node(p, N_DSTR_EL);
                if (el == N_NONE) { free(sc.v); return N_NONE; }
                p->nodes[el].a = target;
                p->nodes[el].b = idx;
                p->nodes[el].flags = 0;
                if (p_scratch(p, &sc, el) < 0) { free(sc.v); return N_NONE; }
            }
            idx++;
            if (!p_eat_punct(p, P_COMMA)) break;
            if (p_is_punct(p, is_obj ? P_RC : P_RBR)) break;
        }
    }
    if (!p_expect_punct(p, is_obj ? P_RC : P_RBR, is_obj ? "expected '}'" : "expected ']'")) { free(sc.v); return N_NONE; }
    u32 first = p_list_commit(p, &sc);
    u32 cnt = sc.n;
    free(sc.v);
    u32 ni = (cnt && first == N_NONE) ? N_NONE : p_node(p, N_DESTR);
    if (ni != N_NONE) {
        p->nodes[ni].a = first;
        p->nodes[ni].c = cnt;
        if (is_obj) p->nodes[ni].flags |= 1;
    }
    return ni;
}

static u32 p_block_tail(P *p) {
    /* '{' の後: 文の列を scratch に集めてから commit し、N_BLOCK を返す */
    U32Vec sc = { NULL, 0, 0 };
    while (p->lx.kind != TK_EOF && !p_is_punct(p, P_RC)) {
        u32 s = p_stmt(p);
        if (s == N_NONE || p_scratch(p, &sc, s) < 0) { free(sc.v); return N_NONE; }
    }
    if (!p_expect_punct(p, P_RC, "expected '}'")) { free(sc.v); return N_NONE; }
    u32 first = p_list_commit(p, &sc);
    u32 cnt = sc.n;
    free(sc.v);
    if (cnt && first == N_NONE) return N_NONE;
    u32 ni = p_node(p, N_BLOCK);
    if (ni == N_NONE) return N_NONE;
    p->nodes[ni].a = first;
    p->nodes[ni].c = cnt;
    return ni;
}

static u32 p_params(P *p, u32 *first, u32 *cnt) {
    /* '(' の後のパラメータ名列（scratch 収集 → commit）。デフォルト値（name = expr）
     * は N_DEFPARAM ノード、rest（...name）は N_RESTPARAM ノードとして積む。 */
    U32Vec sc = { NULL, 0, 0 };
    if (!p_is_punct(p, P_RP)) {
        for (;;) {
            if (p->lx.kind != TK_IDENT) { p->fail = "expected parameter name"; free(sc.v); return N_NONE; }
            u32 idx = p_intern(p, p->lx.str_p, p->lx.str_len);
            if (idx == UINT32_MAX) { free(sc.v); return N_NONE; }
            lex_next(&p->lx);
            u32 ent = idx; /* 通常パラメータは名前 idx のまま（従来互換） */
            if (p_eat_punct(p, P_ASSIGN)) { /* デフォルト引数 */
                u32 d = p_expr(p);
                if (d == N_NONE) { free(sc.v); return N_NONE; }
                ent = p_node(p, N_DEFPARAM);
                if (ent == N_NONE) { free(sc.v); return N_NONE; }
                p->nodes[ent].a = idx;
                p->nodes[ent].b = d;
            } else if (p_eat_punct(p, P_ELLIPSIS)) { /* rest 引数 */
                if (p->lx.kind != TK_IDENT) { p->fail = "expected rest parameter name"; free(sc.v); return N_NONE; }
                idx = p_intern(p, p->lx.str_p, p->lx.str_len);
                if (idx == UINT32_MAX) { free(sc.v); return N_NONE; }
                lex_next(&p->lx);
                ent = p_node(p, N_RESTPARAM);
                if (ent == N_NONE) { free(sc.v); return N_NONE; }
                p->nodes[ent].a = idx;
                if (!p_is_punct(p, P_RP)) { p->fail = "rest parameter must be last"; free(sc.v); return N_NONE; }
            }
            if (p_scratch(p, &sc, ent) < 0) { free(sc.v); return N_NONE; }
            if (!p_eat_punct(p, P_COMMA)) break;
        }
    }
    if (!p_expect_punct(p, P_RP, "expected ')'")) { free(sc.v); return N_NONE; }
    u32 f = p_list_commit(p, &sc);
    *first = f;
    *cnt = sc.n;
    free(sc.v);
    if (*cnt && f == N_NONE) return N_NONE;
    return 0;
}

static u32 p_stmt(P *p) {
    if (p->fail) return N_NONE;
    if (++p->depth > AKL_PARSE_DEPTH) { p->depth--; p->fail = "parse depth exhausted"; return N_NONE; }
    u32 ni = N_NONE;
    if (p_eat_punct(p, P_SEMI)) { p->depth--; return p_node(p, N_BLOCK); /* 空文 */ }
    if (p_is_kw(p, KW_DEBUGGER)) { /* debugger 文は no-op（デバッガは持たない） */
        lex_next(&p->lx);
        p_eat_punct(p, P_SEMI);
        p->depth--;
        return p_node(p, N_BLOCK);
    }
    /* ラベル検出: IDENT の直後が ':' ならラベル文 */
    if (p->lx.kind == TK_IDENT) {
        Lex saved = p->lx;
        lex_next(&p->lx);
        if (p_is_punct(p, P_COLON)) {
            u32 lname = p_intern(p, saved.str_p, saved.str_len);
            if (lname == UINT32_MAX) { p->depth--; return N_NONE; }
            lex_next(&p->lx); /* ':' を消費 */
            u32 body = p_stmt(p);
            if (body == N_NONE) { p->depth--; return N_NONE; }
            u32 lb = p_node(p, N_LABEL);
            if (lb == N_NONE) { p->depth--; return N_NONE; }
            p->nodes[lb].a = lname;
            p->nodes[lb].b = body;
            /* 本体がループ文ならラベルはループ付き（break/continue 両方可） */
            if (body < p->n_nodes) {
                u8 bk = p->nodes[body].kind;
                if (bk == N_WHILE || bk == N_FOR || bk == N_DOWHILE) p->nodes[lb].flags |= 1;
            }
            p->depth--;
            return lb;
        }
        p->lx = saved; /* ラベルでなければ戻す */
    }
    if (p_is_kw(p, KW_VAR) || p_is_kw(p, KW_LET) || p_is_kw(p, KW_CONST)) {
        u8 is_const = p->lx.pk == KW_CONST;
        lex_next(&p->lx);
        /* カンマ宣言は全宣言を保持する（旧実装は最後の宣言しか返さず、
         * `var a=0,b=0; a` が ReferenceError になる実在バグだった。テストで同定） */
        U32Vec sc = { NULL, 0, 0 };
        for (;;) {
            if (p->lx.kind == TK_IDENT) {
                u32 name = p_intern(p, p->lx.str_p, p->lx.str_len);
                if (name == UINT32_MAX) goto out_fail;
                lex_next(&p->lx);
                u32 init = N_NONE;
                if (p_eat_punct(p, P_ASSIGN)) {
                    init = p_expr(p);
                    if (init == N_NONE) goto out_fail;
                } else if (is_const) { p->fail = "const declaration requires initializer"; goto out_fail; }
                u32 vn = p_node(p, N_VAR);
                if (vn == N_NONE) goto out_fail;
                p->nodes[vn].flags = is_const;
                p->nodes[vn].a = name;
                p->nodes[vn].b = init;
                if (p_scratch(p, &sc, vn) < 0) goto out_fail;
            } else if (p_is_punct(p, P_LBR) || p_is_punct(p, P_LC)) {
                /* 分割代入パターン: var [a, b] = expr */
                u32 dest = p_destructure(p);
                if (dest == N_NONE) goto out_fail;
                if (!p_eat_punct(p, P_ASSIGN)) { p->fail = "destructuring requires initializer"; goto out_fail; }
                u32 init = p_expr(p);
                if (init == N_NONE) goto out_fail;
                p->nodes[dest].b = init;
                if (p_scratch(p, &sc, dest) < 0) goto out_fail;
            } else { p->fail = "expected variable name"; goto out_fail; }
            if (!p_eat_punct(p, P_COMMA)) break;
        }
        if (!p_eat_punct(p, P_SEMI) && p->lx.kind != TK_EOF && !p_is_punct(p, P_RC)) {
            p->fail = "expected ';'";
            goto out_fail;
        }
        if (sc.n == 1) {
            ni = sc.v[0];
        } else {
            u32 first = p_list_commit(p, &sc);
            ni = p_node(p, N_BLOCK);
            if (ni != N_NONE && first != N_NONE) { p->nodes[ni].a = first; p->nodes[ni].c = sc.n; }
            else { free(sc.v); goto out; }
        }
        free(sc.v);
        goto out;
    out_fail:
        free(sc.v);
        ni = N_NONE;
        goto out;
    }
    if (p_is_kw(p, KW_CLASS)) {
        lex_next(&p->lx);
        if (p->lx.kind != TK_IDENT) { p->fail = "expected class name"; goto out; }
        u32 name = p_intern(p, p->lx.str_p, p->lx.str_len);
        if (name == UINT32_MAX) goto out;
        lex_next(&p->lx);
        u32 parent = N_NONE;
        if (p_is_kw(p, KW_EXTENDS)) {
            lex_next(&p->lx);
            parent = p_expr(p); /* extends 式（class 本体の { は式の続きでない） */
            if (parent == N_NONE) goto out;
        }
        if (!p_expect_punct(p, P_LC, "expected '{' after class name")) goto out;
        U32Vec mb = { NULL, 0, 0 };
        U32Vec flds = { NULL, 0, 0 };
        while (!p_is_punct(p, P_RC) && p->lx.kind != TK_EOF) {
            bool is_static = false;
            if (p_is_kw(p, KW_STATIC)) { is_static = true; lex_next(&p->lx); }
            u32 mname;
            bool is_ctor = false;
            if (p->lx.kind == TK_IDENT || p->lx.kind == TK_KW) {
                if (p->lx.kind == TK_IDENT && p->lx.str_len == 11 &&
                    memcmp(p->lx.str_p, "constructor", 11) == 0) is_ctor = true;
                mname = p_intern(p, p->lx.str_p, p->lx.str_len);
                if (mname == UINT32_MAX) { free(mb.v); goto out; }
                lex_next(&p->lx);
            } else { p->fail = "expected method name"; free(mb.v); goto out; }
            if (is_static && is_ctor) {
                p->fail = "static constructor is not supported (would shadow class constructor)";
                free(mb.v);
                goto out;
            }
            if (!p_is_punct(p, P_LP)) {
                /* フィールド宣言: name [= expr] [;]（instance field）。
                 * constructor の先頭で this.name = expr を実行する文に変換する。 */
                if (is_static) {
                    p->fail = "static class fields are not supported yet";
                    free(mb.v);
                    goto out;
                }
                u32 fexpr = N_NONE;
                if (p_eat_punct(p, P_ASSIGN)) {
                    fexpr = p_expr(p);
                    if (fexpr == N_NONE) { free(mb.v); goto out; }
                } else {
                    fexpr = p_node(p, N_UNDEF);
                    if (fexpr == N_NONE) { free(mb.v); goto out; }
                }
                u32 th = p_node(p, N_THIS);
                u32 ps = p_node(p, N_PSET);
                u32 es = p_node(p, N_EXPRSTMT);
                if (th == N_NONE || ps == N_NONE || es == N_NONE) { free(mb.v); goto out; }
                p->nodes[ps].a = th;
                p->nodes[ps].b = fexpr;
                p->nodes[ps].c = mname;
                p->nodes[es].a = ps;
                if (p_scratch(p, &flds, es) < 0) { free(mb.v); goto out; }
                p_eat_punct(p, P_SEMI);
                continue;
            }
            lex_next(&p->lx); /* '(' */
            u32 pf = 0, pc2 = 0;
            p_params(p, &pf, &pc2);
            if (p->fail) { free(mb.v); goto out; }
            if (!p_expect_punct(p, P_LC, "expected '{'")) { free(mb.v); goto out; }
            if (parent != N_NONE) p->super_ctx++;
            u32 body = p_block_tail(p);
            if (parent != N_NONE) p->super_ctx--;
            if (body == N_NONE) { free(mb.v); goto out; }
            u32 cm = p_node(p, N_CLASSMETH);
            if (cm == N_NONE) { free(mb.v); goto out; }
            p->nodes[cm].a = mname;
            p->nodes[cm].b = pf;
            p->nodes[cm].c = pc2;
            p->nodes[cm].d = body;
            if (is_static) p->nodes[cm].flags |= 1;
            if (is_ctor && !is_static) p->nodes[cm].flags |= 2;
            if (p_scratch(p, &mb, cm) < 0) { free(mb.v); goto out; }
            /* メソッド間のセミコロンは任意 */
            p_eat_punct(p, P_SEMI);
        }
        if (!p_expect_punct(p, P_RC, "expected '}' after class")) { free(mb.v); goto out; }
        /* constructor が無い場合、合成コンストラクタをここで追加する（パース時に生成
         * することで capture 解析 an_walk の対象になる。cg 時生成だと needs_cap が
         * 解析されず super 環境が壊れる — 実測で特定）。
         * extends あり: body = super()（JS では派生クラスの ctor は super 必須）。
         * フィールド文（flds）は constructor body の先頭に挿入する（super() の後）。 */
        {
            bool has_ctor = false;
            for (u32 i = 0; i < mb.n; i++)
                if ((p->nodes[mb.v[i]].flags & 2) != 0) { has_ctor = true; break; }
            if (!has_ctor) {
                u32 cm = p_node(p, N_CLASSMETH);
                if (cm == N_NONE) { free(mb.v); free(flds.v); goto out; }
                p->nodes[cm].a = p_intern(p, (const u8 *)"constructor", 11);
                p->nodes[cm].b = N_NONE;
                p->nodes[cm].c = 0;
                p->nodes[cm].flags = 2;
                u32 nstmts = flds.n + (parent != N_NONE ? 1u : 0u);
                if (nstmts > 0) {
                    u32 *stmts = (u32 *)malloc((u64)nstmts * sizeof(u32));
                    if (!stmts) { p->fail = "oom: class fields"; free(mb.v); free(flds.v); goto out; }
                    u32 w = 0;
                    if (parent != N_NONE) {
                        u32 sc = p_node(p, N_SUPERCALL);
                        u32 es = p_node(p, N_EXPRSTMT);
                        if (sc == N_NONE || es == N_NONE) { free(stmts); free(mb.v); free(flds.v); goto out; }
                        p->nodes[sc].b = N_NONE;
                        p->nodes[sc].c = 0;
                        p->nodes[es].a = sc;
                        stmts[w++] = es;
                    }
                    for (u32 i = 0; i < flds.n; i++) stmts[w++] = flds.v[i];
                    U32Vec tmp = { NULL, 0, 0 };
                    for (u32 i = 0; i < nstmts; i++) {
                        if (p_scratch(p, &tmp, stmts[i]) < 0) { free(stmts); free(tmp.v); free(mb.v); free(flds.v); goto out; }
                    }
                    u32 bfirst = p_list_commit(p, &tmp);
                    free(tmp.v);
                    free(stmts);
                    if (bfirst == N_NONE) { free(mb.v); free(flds.v); goto out; }
                    u32 body = p_node(p, N_BLOCK);
                    if (body == N_NONE) { free(mb.v); free(flds.v); goto out; }
                    p->nodes[body].a = bfirst;
                    p->nodes[body].c = nstmts;
                    p->nodes[cm].d = body;
                } else {
                    p->nodes[cm].d = N_NONE;
                }
                if (p_scratch(p, &mb, cm) < 0) { free(mb.v); free(flds.v); goto out; }
            } else if (flds.n > 0) {
                /* 既存 constructor の body 末尾にフィールド文を追加（list 領域の直後） */
                u32 ctor = UINT32_MAX;
                for (u32 i = 0; i < mb.n; i++)
                    if ((p->nodes[mb.v[i]].flags & 2) != 0) { ctor = mb.v[i]; break; }
                u32 bd = p->nodes[ctor].d;
                if (bd == N_NONE || p->nodes[bd].kind != N_BLOCK) {
                    p->fail = "constructor body must be a block for fields";
                    free(mb.v);
                    free(flds.v);
                    goto out;
                }
                if (p->nodes[bd].a + p->nodes[bd].c != p->n_list) {
                    p->fail = "internal: block region not contiguous";
                    free(mb.v);
                    free(flds.v);
                    goto out;
                }
                for (u32 i = 0; i < flds.n; i++) {
                    if (p_list_push(p, flds.v[i]) == N_NONE) { free(mb.v); free(flds.v); goto out; }
                }
                p->nodes[bd].c += flds.n;
            }
        }
        free(flds.v);
        u32 mfirst = p_list_commit(p, &mb);
        u32 mcnt = mb.n;
        free(mb.v);
        ni = (mcnt && mfirst == N_NONE) ? N_NONE : p_node(p, N_CLASS);
        if (ni != N_NONE) {
            p->nodes[ni].a = name;
            p->nodes[ni].b = mfirst;
            p->nodes[ni].c = mcnt;
            p->nodes[ni].d = parent;
        }
        goto out;
    }
    if (p_is_kw(p, KW_FUNCTION)) {
        lex_next(&p->lx);
        if (p->lx.kind != TK_IDENT) { p->fail = "expected function name"; goto out; }
        u32 name = p_intern(p, p->lx.str_p, p->lx.str_len);
        if (name == UINT32_MAX) goto out;
        lex_next(&p->lx);
        if (!p_expect_punct(p, P_LP, "expected '('")) goto out;
        u32 pf = 0, pc2 = 0;
        p_params(p, &pf, &pc2);
        if (p->fail) goto out;
        if (!p_expect_punct(p, P_LC, "expected '{'")) goto out;
        u32 body = p_block_tail(p);
        if (body == N_NONE) goto out;
        ni = p_node(p, N_FUNC);
        if (ni == N_NONE) goto out;
        p->nodes[ni].a = name;
        p->nodes[ni].b = pf;
        p->nodes[ni].c = pc2;
        p->nodes[ni].d = body;
        goto out;
    }
    if (p_is_kw(p, KW_RETURN)) {
        lex_next(&p->lx);
        u32 e = N_NONE;
        if (!p_is_punct(p, P_SEMI) && !p_is_punct(p, P_RC) && p->lx.kind != TK_EOF) {
            e = p_expr_comma(p);
            if (e == N_NONE) goto out;
        }
        p_eat_punct(p, P_SEMI);
        ni = p_node(p, N_RET);
        if (ni != N_NONE) p->nodes[ni].a = e;
        goto out;
    }
    if (p_is_kw(p, KW_IF)) {
        lex_next(&p->lx);
        if (!p_expect_punct(p, P_LP, "expected '('")) goto out;
        u32 c = p_expr(p);
        if (c == N_NONE || !p_expect_punct(p, P_RP, "expected ')'")) goto out;
        u32 th = p_stmt(p);
        if (th == N_NONE) goto out;
        u32 el = N_NONE;
        if (p_is_kw(p, KW_ELSE)) {
            lex_next(&p->lx);
            el = p_stmt(p);
            if (el == N_NONE) goto out;
        }
        ni = p_node(p, N_IF);
        if (ni != N_NONE) { p->nodes[ni].a = c; p->nodes[ni].b = th; p->nodes[ni].c = el; }
        goto out;
    }
    if (p_is_kw(p, KW_WHILE)) {
        lex_next(&p->lx);
        if (!p_expect_punct(p, P_LP, "expected '('")) goto out;
        u32 c = p_expr(p);
        if (c == N_NONE || !p_expect_punct(p, P_RP, "expected ')'")) goto out;
        u32 body = p_stmt(p);
        if (body == N_NONE) goto out;
        ni = p_node(p, N_WHILE);
        if (ni != N_NONE) { p->nodes[ni].a = c; p->nodes[ni].b = body; }
        goto out;
    }
    if (p_is_kw(p, KW_DO)) {
        lex_next(&p->lx);
        u32 body = p_stmt(p);
        if (body == N_NONE) goto out;
        if (!p_is_kw(p, KW_WHILE)) { p->fail = "expected 'while' after do body"; goto out; }
        lex_next(&p->lx);
        if (!p_expect_punct(p, P_LP, "expected '('")) goto out;
        u32 c = p_expr(p);
        if (c == N_NONE || !p_expect_punct(p, P_RP, "expected ')'")) goto out;
        p_eat_punct(p, P_SEMI); /* JS: do..while(cond); の ; は任意 */
        ni = p_node(p, N_DOWHILE);
        if (ni != N_NONE) { p->nodes[ni].a = body; p->nodes[ni].b = c; }
        goto out;
    }
    if (p_is_kw(p, KW_SWITCH)) {
        lex_next(&p->lx);
        if (!p_expect_punct(p, P_LP, "expected '('")) goto out;
        u32 disc = p_expr(p);
        if (disc == N_NONE || !p_expect_punct(p, P_RP, "expected ')'")) goto out;
        if (!p_expect_punct(p, P_LC, "expected '{' after switch")) goto out;
        U32Vec sc = { NULL, 0, 0 };
        bool saw_default = false;
        while (!p_is_punct(p, P_RC) && p->lx.kind != TK_EOF) {
            u32 ce = N_NONE;
            if (p_is_kw(p, KW_CASE)) {
                lex_next(&p->lx);
                ce = p_expr(p);
                if (ce == N_NONE) { free(sc.v); goto out; }
            } else if (p_is_kw(p, KW_DEFAULT)) {
                if (saw_default) { p->fail = "duplicate default in switch"; free(sc.v); goto out; }
                saw_default = true;
                lex_next(&p->lx);
            } else { p->fail = "expected 'case' or 'default' in switch"; free(sc.v); goto out; }
            if (!p_expect_punct(p, P_COLON, "expected ':' after case")) { free(sc.v); goto out; }
            /* case 本体: 次の case/default/} まで（空 case は N_BLOCK 空でよい） */
            U32Vec bs = { NULL, 0, 0 };
            while (!p_is_kw(p, KW_CASE) && !p_is_kw(p, KW_DEFAULT) &&
                   !p_is_punct(p, P_RC) && p->lx.kind != TK_EOF) {
                u32 s = p_stmt(p);
                if (s == N_NONE) { free(bs.v); free(sc.v); goto out; }
                if (p_scratch(p, &bs, s) < 0) { free(bs.v); free(sc.v); goto out; }
            }
            u32 bfirst = p_list_commit(p, &bs);
            u32 bcnt = bs.n;
            free(bs.v);
            u32 body = N_NONE;
            if (bcnt == 1) body = p->list[bfirst]; /* 実ノードへ解決（bfirst は list index） */
            else if (bcnt > 1) {
                body = p_node(p, N_BLOCK);
                if (body != N_NONE) { p->nodes[body].a = bfirst; p->nodes[body].c = bcnt; }
            } else if (bcnt == 0) body = p_node(p, N_BLOCK); /* 空 case */
            if (body == N_NONE) { free(sc.v); goto out; }
            u32 cs = p_node(p, N_CASE);
            if (cs == N_NONE) { free(sc.v); goto out; }
            p->nodes[cs].a = ce;
            p->nodes[cs].b = body;
            if (p_scratch(p, &sc, cs) < 0) { free(sc.v); goto out; }
        }
        if (!p_expect_punct(p, P_RC, "expected '}' after switch")) { free(sc.v); goto out; }
        u32 sfirst = p_list_commit(p, &sc);
        u32 scnt = sc.n;
        free(sc.v);
        ni = (scnt && sfirst == N_NONE) ? N_NONE : p_node(p, N_SWITCH);
        if (ni != N_NONE) { p->nodes[ni].a = disc; p->nodes[ni].b = sfirst; p->nodes[ni].c = scnt; }
        goto out;
    }
    if (p_is_kw(p, KW_FOR)) {
        lex_next(&p->lx);
        if (!p_expect_punct(p, P_LP, "expected '('")) goto out;
        u32 init = N_NONE, cond = N_NONE, step = N_NONE;
        /* for (var k in obj) / for (var v of arr): 単一変数のみ（JS 基本形）。
         * var 宣言後に in/of が直接来る場合のみ。それ以外は通常の for へ。 */
        if (p_is_kw(p, KW_VAR) || p_is_kw(p, KW_LET) || p_is_kw(p, KW_CONST)) {
            u8 is_const = p->lx.pk == KW_CONST;
            lex_next(&p->lx);
            if (p->lx.kind != TK_IDENT) { p->fail = "expected variable name"; goto out; }
            u32 name = p_intern(p, p->lx.str_p, p->lx.str_len);
            if (name == UINT32_MAX) goto out;
            lex_next(&p->lx);
            if (p_is_kw(p, KW_IN) || p_is_kw(p, KW_OF)) {
                bool is_in = p->lx.pk == KW_IN;
                lex_next(&p->lx);
                u32 obj = p_expr(p);
                if (obj == N_NONE || !p_expect_punct(p, P_RP, "expected ')'")) goto out;
                u32 body = p_stmt(p);
                if (body == N_NONE) goto out;
                ni = p_node(p, N_FORIN);
                if (ni != N_NONE) {
                    p->nodes[ni].a = name;
                    p->nodes[ni].b = obj;
                    p->nodes[ni].c = body;
                    p->nodes[ni].flags = is_in ? 0 : 1; /* 0=in 1=of */
                    p->nodes[ni].d = is_const;
                }
                goto out;
            }
            /* 通常の for: ここで宣言済み name を使って var init を再構成する。
             * 現在トークンは '=' か ';' なので、従来の var 処理を続行する。
             * name を戻す: 一時的にループ変数として N_VAR を作る */
            if (!p_is_punct(p, P_ASSIGN) && !p_is_punct(p, P_SEMI)) {
                p->fail = "expected '=' or ';' after for variable";
                goto out;
            }
            u32 iv2 = N_NONE;
            if (p_eat_punct(p, P_ASSIGN)) { iv2 = p_expr_comma(p); if (iv2 == N_NONE) goto out; }
            else if (is_const) { p->fail = "const declaration requires initializer"; goto out; }
            init = p_node(p, N_VAR);
            if (init != N_NONE) { p->nodes[init].flags = is_const; p->nodes[init].a = name; p->nodes[init].b = iv2; }
            /* 残りは通常の for へ（init は確定済み） */
            if (!p_expect_punct(p, P_SEMI, "expected ';'")) goto out;
            if (!p_is_punct(p, P_SEMI)) { cond = p_expr(p); if (cond == N_NONE) goto out; }
            if (!p_expect_punct(p, P_SEMI, "expected ';'")) goto out;
            if (!p_is_punct(p, P_RP)) {
                step = p_expr_comma(p);
                if (step == N_NONE) goto out;
            }
            if (!p_expect_punct(p, P_RP, "expected ')'")) goto out;
            u32 body2 = p_stmt(p);
            if (body2 == N_NONE) goto out;
            ni = p_node(p, N_FOR);
            if (ni != N_NONE) { p->nodes[ni].a = init; p->nodes[ni].b = cond; p->nodes[ni].c = step; p->nodes[ni].d = body2; }
            goto out;
        }
        if (!p_is_punct(p, P_SEMI)) {
            if (p_is_kw(p, KW_VAR) || p_is_kw(p, KW_LET) || p_is_kw(p, KW_CONST)) {
                /* for 内の var 宣言: カンマ区切り複数（JS 準拠） */
                u8 is_const = p->lx.pk == KW_CONST;
                lex_next(&p->lx);
                U32Vec vd = { NULL, 0, 0 };
                for (;;) {
                    if (p->lx.kind != TK_IDENT) { p->fail = "expected variable name"; free(vd.v); goto out; }
                    u32 name = p_intern(p, p->lx.str_p, p->lx.str_len);
                    if (name == UINT32_MAX) { free(vd.v); goto out; }
                    lex_next(&p->lx);
                    u32 iv = N_NONE;
                    if (p_eat_punct(p, P_ASSIGN)) { iv = p_expr(p); if (iv == N_NONE) { free(vd.v); goto out; } }
                    else if (is_const) { p->fail = "const declaration requires initializer"; free(vd.v); goto out; }
                    u32 vn = p_node(p, N_VAR);
                    if (vn == N_NONE) { free(vd.v); goto out; }
                    p->nodes[vn].flags = is_const;
                    p->nodes[vn].a = name;
                    p->nodes[vn].b = iv;
                    if (p_scratch(p, &vd, vn) < 0) { free(vd.v); goto out; }
                    if (!p_eat_punct(p, P_COMMA)) break;
                }
                if (vd.n == 1) {
                    init = vd.v[0];
                } else {
                    u32 first = p_list_commit(p, &vd);
                    init = p_node(p, N_BLOCK);
                    if (init != N_NONE && first != N_NONE) { p->nodes[init].a = first; p->nodes[init].c = vd.n; }
                    else { free(vd.v); goto out; }
                }
                free(vd.v);
            } else {
                init = p_expr_comma(p);
                if (init == N_NONE) goto out;
            }
        }
        if (!p_expect_punct(p, P_SEMI, "expected ';'")) goto out;
        if (!p_is_punct(p, P_SEMI)) { cond = p_expr(p); if (cond == N_NONE) goto out; }
        if (!p_expect_punct(p, P_SEMI, "expected ';'")) goto out;
        if (!p_is_punct(p, P_RP)) {
            /* step は代入式まで許す（N_ASSIGN 化は p_expr が行う） */
            step = p_expr_comma(p);
            if (step == N_NONE) goto out;
        }
        if (!p_expect_punct(p, P_RP, "expected ')'")) goto out;
        u32 body = p_stmt(p);
        if (body == N_NONE) goto out;
        ni = p_node(p, N_FOR);
        if (ni != N_NONE) { p->nodes[ni].a = init; p->nodes[ni].b = cond; p->nodes[ni].c = step; p->nodes[ni].d = body; }
        goto out;
    }
    if (p_is_kw(p, KW_BREAK)) {
        lex_next(&p->lx);
        u32 lname = N_NONE;
        if (p->lx.kind == TK_IDENT) { /* break label; */
            lname = p_intern(p, p->lx.str_p, p->lx.str_len);
            if (lname == UINT32_MAX) goto out;
            lex_next(&p->lx);
        }
        p_eat_punct(p, P_SEMI);
        ni = p_node(p, N_BREAK);
        if (ni != N_NONE) p->nodes[ni].a = lname;
        goto out;
    }
    if (p_is_kw(p, KW_CONTINUE)) {
        lex_next(&p->lx);
        u32 lname = N_NONE;
        if (p->lx.kind == TK_IDENT) {
            lname = p_intern(p, p->lx.str_p, p->lx.str_len);
            if (lname == UINT32_MAX) goto out;
            lex_next(&p->lx);
        }
        p_eat_punct(p, P_SEMI);
        ni = p_node(p, N_CONTINUE);
        if (ni != N_NONE) p->nodes[ni].a = lname;
        goto out;
    }
    if (p_is_kw(p, KW_THROW)) {
        lex_next(&p->lx);
        u32 e = p_expr(p);
        if (e == N_NONE) goto out;
        if (!p_eat_punct(p, P_SEMI) && p->lx.kind != TK_EOF && !p_is_punct(p, P_RC)) { p->fail = "expected ';'"; goto out; }
        ni = p_node(p, N_THROW);
        if (ni != N_NONE) p->nodes[ni].a = e;
        goto out;
    }
    if (p_is_kw(p, KW_TRY)) {
        lex_next(&p->lx);
        if (!p_expect_punct(p, P_LC, "expected '{' after try")) goto out;
        u32 body = p_block_tail(p);
        if (body == N_NONE) goto out;
        u32 cname = UINT32_MAX; /* catch 束縛名（無ければ NONE 扱い） */
        u32 cbody = N_NONE, fbody = N_NONE;
        bool has_catch = p_is_kw(p, KW_CATCH);
        if (!has_catch && !p_is_kw(p, KW_FINALLY)) { p->fail = "try requires catch or finally"; goto out; }
        if (has_catch) {
            lex_next(&p->lx);
            if (p_eat_punct(p, P_LP)) { /* ES2019 束縛なし catch も許容 */
                if (p->lx.kind != TK_IDENT) { p->fail = "expected catch binding name"; goto out; }
                cname = p_intern(p, p->lx.str_p, p->lx.str_len);
                if (cname == UINT32_MAX) goto out;
                lex_next(&p->lx);
                if (!p_expect_punct(p, P_RP, "expected ')'")) goto out;
            }
            if (!p_expect_punct(p, P_LC, "expected '{' after catch")) goto out;
            cbody = p_block_tail(p);
            if (cbody == N_NONE) goto out;
        }
        if (p_is_kw(p, KW_FINALLY)) {
            lex_next(&p->lx);
            if (!p_expect_punct(p, P_LC, "expected '{' after finally")) goto out;
            fbody = p_block_tail(p);
            if (fbody == N_NONE) goto out;
        }
        ni = p_node(p, N_TRY);
        if (ni != N_NONE) { p->nodes[ni].a = body; p->nodes[ni].b = cname; p->nodes[ni].c = cbody; p->nodes[ni].d = fbody; }
        goto out;
    }
    if (p_eat_punct(p, P_LC)) { ni = p_block_tail(p); goto out; }
    if (p_is_punct(p, P_LBR)) { /* 文頭の分割代入: [a, b] = expr（式文と曖昧 — ロールバックで解決） */
        Lex saved = p->lx;
        u32 dest = p_destructure(p);
        if (dest != N_NONE && p_eat_punct(p, P_ASSIGN)) {
            u32 rhs = p_expr_comma(p);
            if (rhs == N_NONE) goto out;
            p->nodes[dest].b = rhs;
            if (!p_eat_punct(p, P_SEMI) && p->lx.kind != TK_EOF && !p_is_punct(p, P_RC)) { p->fail = "expected ';'"; goto out; }
            ni = p_node(p, N_EXPRSTMT);
            if (ni != N_NONE) p->nodes[ni].a = dest;
            goto out;
        }
        /* 分割代入でない（配列リテラル式など）: トークン位置を復元して式文として処理 */
        p->lx = saved;
        p->fail = NULL;
    }
    {
        u32 e = p_expr_comma(p);
        if (e == N_NONE) goto out;
        if (!p_eat_punct(p, P_SEMI) && p->lx.kind != TK_EOF && !p_is_punct(p, P_RC)) { p->fail = "expected ';'"; goto out; }
        ni = p_node(p, N_EXPRSTMT);
        if (ni != N_NONE) p->nodes[ni].a = e;
    }
out:
    p->depth--;
    return ni;
}

/* ============================== クロージャ捕捉解析 ==============================
 * 関数 F が祖先 G のローカル x を参照するとき、x は G の「capture 済みローカル」に
 * なり G の実行時環境（ENV obj）に置かれる。F と G の間の全関数は env を中継する
 * （needs_cap）。ENV はチェーン（parent=生成元の env チェーン）を成し、
 * CELOAD(depth, idx) の depth = 中継関数のうち自前 env を持つ数（= parent ホップ数）。
 * 解析は codegen に先立って全関数の capture 情報を確定させる（単一 pass では
 * 「外側関数のローカルが後で宣言される」順序問題を解決できないため）。 */

/* ノード部分木の識別子参照を収集（ネストした関数本体はスキップ）。 */
static bool an_add(U32Vec *v, u32 name) {
    for (u32 i = 0; i < v->n; i++) if (v->v[i] == name) return true;
    if (v->n == v->cap) {
        u32 nc = v->cap ? v->cap * 2 : 16;
        u32 *nv = (u32 *)realloc(v->v, (u64)nc * sizeof(u32));
        if (!nv) return false;
        v->v = nv; v->cap = nc;
    }
    v->v[v->n++] = name;
    return true;
}

static void an_refs(P *p, u32 ni, U32Vec *out) {
    if (ni == N_NONE || p->fail) return;
    AklNode *n = &p->nodes[ni];
    switch (n->kind) {
    case N_IDENT: an_add(out, n->a); break;
    case N_ASSIGN: an_add(out, n->a); an_refs(p, n->b, out); break;
    case N_PGET: an_refs(p, n->a, out); break;   /* b は prop 名（識別子でない） */
    case N_PSET: an_refs(p, n->a, out); an_refs(p, n->b, out); break;
    case N_MCALL: an_refs(p, n->a, out); for (u32 i = 0; i < n->c; i++) an_refs(p, p->list[n->b + i], out); break;
    case N_INDEX: an_refs(p, n->a, out); an_refs(p, n->b, out); break;
    case N_INDEXSET: an_refs(p, n->a, out); an_refs(p, n->b, out); an_refs(p, n->c, out); break;
    case N_ASSIGNOP: an_refs(p, n->a, out); an_refs(p, n->b, out); break;
    case N_INCDEC: an_refs(p, n->a, out); break;
    case N_TERN: an_refs(p, n->a, out); an_refs(p, n->b, out); an_refs(p, n->c, out); break;
    case N_ARRLIT: for (u32 i = 0; i < n->c; i++) an_refs(p, p->list[n->b + i], out); break;
    case N_CALL: an_refs(p, n->a, out); for (u32 i = 0; i < n->c; i++) an_refs(p, p->list[n->b + i], out); break;
    case N_VAR: an_refs(p, n->b, out); break;
    case N_UNARY: case N_RET: case N_THROW: an_refs(p, n->a, out); break;
    case N_BIN: an_refs(p, n->a, out); an_refs(p, n->b, out); break;
    case N_EXPRSTMT: an_refs(p, n->a, out); break;
    case N_IF: an_refs(p, n->a, out); an_refs(p, n->b, out); an_refs(p, n->c, out); break;
    case N_WHILE: case N_DOWHILE: an_refs(p, n->a, out); an_refs(p, n->b, out); break;
    case N_FOR: an_refs(p, n->a, out); an_refs(p, n->b, out); an_refs(p, n->c, out); an_refs(p, n->d, out); break;
    case N_SWITCH:
        an_refs(p, n->a, out);
        for (u32 i = 0; i < n->c; i++) an_refs(p, p->list[n->b + i], out);
        break;
    case N_CASE: an_refs(p, n->a, out); an_refs(p, n->b, out); break;
    case N_BLOCK: if (n->a != N_NONE) for (u32 i = 0; i < n->c; i++) an_refs(p, p->list[n->a + i], out); break;
    case N_TRY: an_refs(p, n->a, out); an_refs(p, n->c, out); an_refs(p, n->d, out); break;
    case N_OBJLIT: for (u32 i = 0; i < n->c; i++) an_refs(p, p->list[n->b + i], out); break;
    case N_LABEL: an_refs(p, n->b, out); break;
    case N_OBJKEY: an_refs(p, n->a, out); an_refs(p, n->b, out); break;
    case N_LOGASSIGN: an_refs(p, n->a, out); an_refs(p, n->b, out); break;
    case N_SPREAD: an_refs(p, n->a, out); break;
    case N_SUPERMCALL:
        an_add(out, p->super_name);
        for (u32 i = 0; i < n->d; i++) an_refs(p, p->list[n->c + i], out);
        break;
    case N_SUPERCALL:
        an_add(out, p->super_name);
        for (u32 i = 0; i < n->c; i++) an_refs(p, p->list[n->b + i], out);
        break;
    case N_SUPERGET:
        an_add(out, p->super_name);
        break;
    /* N_FUNC / N_FUNCEXPR / N_CLASSMETH: 本体は別関数（スキップ）。N_THIS 等: なし */
    default: break;
    }
}

/* この関数自身の宣言名を収集: var/let/const + catch 束縛 + 関数宣言名。
 * ネスト関数の本体はスキップ（N_FUNC/N_FUNCEXPR は名前だけをこの関数の宣言とし、
 * 名前付き関数式の名前は生成元スコープの宣言として扱う近似 — AKL_COMPAT に明記）。 */
static void an_decls(P *p, u32 ni, U32Vec *out) {
    if (ni == N_NONE || p->fail) return;
    AklNode *n = &p->nodes[ni];
    switch (n->kind) {
    case N_VAR: an_add(out, n->a); break;
    case N_FUNC: case N_FUNCEXPR:
        if (n->a != UINT32_MAX) an_add(out, n->a);
        break;
    case N_TRY:
        if (n->b != UINT32_MAX) an_add(out, n->b);
        an_decls(p, n->a, out);
        an_decls(p, n->c, out);
        an_decls(p, n->d, out);
        break;
    case N_FOR: an_decls(p, n->a, out); an_decls(p, n->d, out); break;
    case N_BLOCK: if (n->a != N_NONE) for (u32 i = 0; i < n->c; i++) an_decls(p, p->list[n->a + i], out); break;
    case N_IF: an_decls(p, n->b, out); an_decls(p, n->c, out); break;
    case N_WHILE: case N_DOWHILE: an_decls(p, n->a, out); break;
    case N_SWITCH: for (u32 i = 0; i < n->c; i++) an_decls(p, p->list[n->b + i], out); break;
    case N_CASE: an_decls(p, n->b, out); break;
    case N_EXPRSTMT: an_decls(p, n->a, out); break;
    case N_TERN: an_decls(p, n->b, out); an_decls(p, n->c, out); break;
    case N_ASSIGNOP: an_decls(p, n->a, out); an_decls(p, n->b, out); break;
    case N_INCDEC: an_decls(p, n->a, out); break;
    case N_INDEXSET: an_decls(p, n->a, out); an_decls(p, n->b, out); an_decls(p, n->c, out); break;
    case N_PSET: an_decls(p, n->a, out); an_decls(p, n->b, out); break;
    case N_BIN: case N_UNARY: case N_RET: case N_THROW: case N_INDEX:
        an_decls(p, n->a, out);
        an_decls(p, n->b, out);
        an_decls(p, n->c, out);
        break;
    case N_PGET: /* b は name（STR idx）でノードでない */
        an_decls(p, n->a, out);
        break;
    case N_CALL: case N_MCALL:
        an_decls(p, n->a, out);
        for (u32 i = 0; i < n->c; i++) an_decls(p, p->list[n->b + i], out);
        break;
    case N_ARRLIT: case N_OBJLIT:
        for (u32 i = 0; i < n->c; i++) an_decls(p, p->list[n->b + i], out);
        break;
    case N_LABEL:
        an_decls(p, n->b, out);
        break;
    case N_OBJKEY:
        an_decls(p, n->a, out);
        an_decls(p, n->b, out);
        break;
    case N_LOGASSIGN:
        an_decls(p, n->a, out);
        an_decls(p, n->b, out);
        break;
    case N_SPREAD:
        an_decls(p, n->a, out);
        break;
    default: break;
    }
}

/* cap_names への追加（重複排除・env idx 順序維持）。false = OOM */
static bool an_capture(P *p, AklFnInfo *fi, u32 name) {
    for (u32 i = 0; i < fi->n_cap; i++) if (fi->cap_names[i] == name) return true;
    u32 nc = fi->n_cap + 1;
    u32 *nv = (u32 *)realloc(fi->cap_names, (u64)nc * sizeof(u32));
    if (!nv) { p->fail = "oom: capture analysis"; return false; }
    fi->cap_names = nv;
    fi->cap_names[fi->n_cap++] = name;
    return true;
}

static void an_walk(P *p, u32 ni, u32 *anc, u32 anc_n);

/* 関数ノードの解析（anc[0..anc_n) = 祖先関数ノード列。anc[anc_n] = この関数。
 * main 擬似関数は anc[0] = N_NONE で表現し、P.main_fi がその fninfo）。 */
static void an_fn(P *p, u32 ni, u32 *anc, u32 anc_n) {
    AklNode *n = &p->nodes[ni];
    AklFnInfo *fi = &p->fninfo[ni];
    if (fi->done) return;
    fi->done = 1;
    for (u32 i = 0; i < n->c; i++) an_add(&fi->decls, p->list[n->b + i]); /* params */
    an_decls(p, n->d, &fi->decls);
    U32Vec refs = { NULL, 0, 0 };
    an_refs(p, n->d, &refs);
    anc[anc_n] = ni;
    /* free(F) ∩ decls(G): 祖先を innermost から走査し capture / needs_cap を marking */
    for (u32 a = anc_n; a-- > 0;) {
        AklFnInfo *gfi = (anc[a] == N_NONE) ? &p->main_fi : &p->fninfo[anc[a]];
        for (u32 i = 0; i < refs.n; i++) {
            /* SUPER_NAME は祖先のローカルではなく「関数自身の cap env 先頭」
             * （OP_MAKEFS がバインド）。cap env slot を確保するため needs_cap を立てる。 */
            if (refs.v[i] == p->super_name) {
                p->fninfo[ni].needs_cap = 1;
                continue;
            }
            for (u32 j = 0; j < gfi->decls.n; j++) {
                if (refs.v[i] == gfi->decls.v[j]) {
                    if (!an_capture(p, gfi, refs.v[i])) { free(refs.v); return; }
                    p->fninfo[ni].needs_cap = 1;
                    for (u32 m = a + 1; m < anc_n; m++) { /* 中継関数（a と ni の間） */
                        u32 mid = anc[m];
                        if (mid != N_NONE) p->fninfo[mid].needs_cap = 1;
                    }
                    break;
                }
            }
        }
    }
    free(refs.v);
    an_walk(p, n->d, anc, anc_n + 1);
}

/* 部分木を DFS して関数ノードを an_fn へ */
static void an_walk(P *p, u32 ni, u32 *anc, u32 anc_n) {
    if (ni == N_NONE || p->fail) return;
    AklNode *n = &p->nodes[ni];
    switch (n->kind) {
    case N_FUNC: case N_FUNCEXPR: case N_CLASSMETH:
        an_fn(p, ni, anc, anc_n);
        return;
    case N_CLASS: /* a=名前, b=メンバー list, c=count, d=親ノード */
        if (n->d != N_NONE) an_walk(p, n->d, anc, anc_n);
        for (u32 i = 0; i < n->c; i++) an_walk(p, p->list[n->b + i], anc, anc_n);
        return;
    case N_BLOCK: /* a=first, c=count（N_BLOCK は a が list 先頭 — 他と異なる）。
                    * 空文 N_BLOCK は a=c=N_NONE（cg_stmt の規約と同じガード） */
        if (n->a != N_NONE) for (u32 i = 0; i < n->c; i++) an_walk(p, p->list[n->a + i], anc, anc_n);
        return;
    case N_CALL: case N_MCALL:
        an_walk(p, n->a, anc, anc_n); /* callee / レシーバ */
        for (u32 i = 0; i < n->c; i++) an_walk(p, p->list[n->b + i], anc, anc_n);
        return;
    case N_ARRLIT: case N_OBJLIT: /* a は list index（name 列）でノードでない */
        for (u32 i = 0; i < n->c; i++) an_walk(p, p->list[n->b + i], anc, anc_n);
        return;
    case N_LABEL:
        an_walk(p, n->b, anc, anc_n);
        return;
    case N_OBJKEY:
        an_walk(p, n->a, anc, anc_n);
        an_walk(p, n->b, anc, anc_n);
        return;
    case N_LOGASSIGN:
        an_walk(p, n->a, anc, anc_n);
        an_walk(p, n->b, anc, anc_n);
        return;
    case N_SPREAD:
        an_walk(p, n->a, anc, anc_n);
        return;
    case N_IF: case N_WHILE: case N_DOWHILE: case N_TERN:
        an_walk(p, n->a, anc, anc_n);
        an_walk(p, n->b, anc, anc_n);
        an_walk(p, n->c, anc, anc_n);
        return;
    case N_FOR:
        an_walk(p, n->a, anc, anc_n);
        an_walk(p, n->b, anc, anc_n);
        an_walk(p, n->c, anc, anc_n);
        an_walk(p, n->d, anc, anc_n);
        return;
    case N_SWITCH:
        an_walk(p, n->a, anc, anc_n);
        for (u32 i = 0; i < n->c; i++) an_walk(p, p->list[n->b + i], anc, anc_n);
        return;
    case N_CASE:
        an_walk(p, n->a, anc, anc_n);
        an_walk(p, n->b, anc, anc_n);
        return;
    case N_UNARY: case N_RET: case N_THROW: case N_PGET: case N_INDEX:
        an_walk(p, n->a, anc, anc_n);
        return;
    case N_BIN: case N_INDEXSET: case N_ASSIGNOP:
        an_walk(p, n->a, anc, anc_n);
        an_walk(p, n->b, anc, anc_n);
        an_walk(p, n->c, anc, anc_n);
        return;
    case N_PSET: /* c は name（STR idx）でノードでない */
        an_walk(p, n->a, anc, anc_n);
        an_walk(p, n->b, anc, anc_n);
        return;
    case N_EXPRSTMT: case N_INCDEC:
        an_walk(p, n->a, anc, anc_n);
        an_walk(p, n->b, anc, anc_n);
        return;
    case N_VAR: case N_ASSIGN: /* a は name（識別子）でノードでない */
        an_walk(p, n->b, anc, anc_n);
        return;
    case N_TRY:
        an_walk(p, n->a, anc, anc_n);
        an_walk(p, n->c, anc, anc_n);
        an_walk(p, n->d, anc, anc_n);
        return;
    default: return; /* N_IDENT / リテラル / N_ASSIGN(a=name) 等: 子なし */
    }
}


/* main 擬似関数の decls: トップレベルの var/関数宣言はグローバル化するため、
 * main の「ローカル」は catch 束縛のみ（cg_store の depth 0 経路と一致）。
 * これ以外を入れるとグローバル名が誤って capture される（実バグとして検出済み）。 */
static void an_main_decls(P *p, u32 ni, U32Vec *out) {
    if (ni == N_NONE || p->fail) return;
    AklNode *n = &p->nodes[ni];
    switch (n->kind) {
    case N_TRY:
        if (n->b != UINT32_MAX) an_add(out, n->b);
        an_main_decls(p, n->a, out);
        an_main_decls(p, n->c, out);
        an_main_decls(p, n->d, out);
        break;
    case N_BLOCK:
        if (n->a != N_NONE) for (u32 i = 0; i < n->c; i++) an_main_decls(p, p->list[n->a + i], out);
        break;
    case N_FOR:
        an_main_decls(p, n->a, out);
        an_main_decls(p, n->d, out);
        break;
    case N_IF:
        an_main_decls(p, n->b, out);
        an_main_decls(p, n->c, out);
        break;
    case N_WHILE: case N_DOWHILE:
        an_main_decls(p, n->a, out);
        an_main_decls(p, n->b, out);
        break;
    case N_SWITCH:
        for (u32 i = 0; i < n->c; i++) an_main_decls(p, p->list[n->b + i], out);
        break;
    case N_CASE:
        an_main_decls(p, n->b, out);
        break;
    case N_EXPRSTMT: case N_VAR: case N_ASSIGN: case N_INCDEC:
        an_main_decls(p, n->b, out);
        break;
    case N_ASSIGNOP: case N_INDEXSET:
        an_main_decls(p, n->a, out);
        an_main_decls(p, n->b, out);
        an_main_decls(p, n->c, out);
        break;
    case N_PSET: /* c は name（STR idx）でノードでない */
        an_main_decls(p, n->a, out);
        an_main_decls(p, n->b, out);
        break;
    case N_TERN: case N_BIN: case N_UNARY:
        an_main_decls(p, n->a, out);
        an_main_decls(p, n->b, out);
        an_main_decls(p, n->c, out);
        break;
    case N_CALL: case N_MCALL:
        an_main_decls(p, n->a, out);
        for (u32 i = 0; i < n->c; i++) an_main_decls(p, p->list[n->b + i], out);
        break;
    case N_ARRLIT: case N_OBJLIT: /* a は list index（name 列）でノードでない */
        for (u32 i = 0; i < n->c; i++) an_main_decls(p, p->list[n->b + i], out);
        break;
    case N_LABEL:
        an_main_decls(p, n->b, out);
        break;
    case N_OBJKEY:
        an_main_decls(p, n->a, out);
        an_main_decls(p, n->b, out);
        break;
    case N_LOGASSIGN:
        an_main_decls(p, n->a, out);
        an_main_decls(p, n->b, out);
        break;
    case N_SPREAD:
        an_main_decls(p, n->a, out);
        break;
    default: break;
    }
}

/* プログラム全体の解析。main 擬似関数の decls = トップレベル catch 束縛のみ。 */
static bool akl_analyze(P *p, u32 prog) {
    if (p->n_nodes > 0) {
        p->fninfo = (AklFnInfo *)calloc(p->n_nodes, sizeof(AklFnInfo));
        if (!p->fninfo) { p->fail = "oom: capture analysis"; return false; }
        p->fninfo_n = p->n_nodes;
    }
    memset(&p->main_fi, 0, sizeof p->main_fi);
    an_main_decls(p, prog, &p->main_fi.decls);
    u32 *anc = (u32 *)malloc((u64)(AKL_PARSE_DEPTH + 2) * sizeof(u32));
    if (!anc) { p->fail = "oom: capture analysis"; return false; }
    anc[0] = N_NONE; /* main 擬似 */
    an_walk(p, prog, anc, 1);
    free(anc);
    return !p->fail;
}

/* ============================== codegen ============================== */

typedef struct { u32 name; u8 is_const; u8 captured; u8 env_idx; u8 _p; } LocalEnt;

/* 祖先関数の codegen 状態（ネスト関数 compile 中の退避先）。
 * 現在関数の scope は Cg 直持ち（locals/n_locals/cap_locals/cur_n_env/cur_needs_cap/
 * cur_fi）。outer[0] が main 相当。 */
typedef struct {
    LocalEnt *locals; u32 n_locals, cap_locals;
    u32 n_env;             /* 自前 ENV スロット数（= capture 済みローカル数） */
    u16 needs_cap;         /* env 中継が必要 */
    i32 fn_idx;            /* funcs 表 index（main=0） */
    const AklFnInfo *fi;   /* 解析結果（無ければ NULL） */
} CgScope;

typedef struct {
    AklRT *rt;
    P *p;
    LocalEnt *locals; u32 n_locals, cap_locals;
    u32 cur_n_env;         /* 現在関数の capture 済みローカル数（frame 隠し slot 1 の実体数） */
    u16 cur_needs_cap;     /* 現在関数が cap env を要する（frame 隠し slot 2） */
    const AklFnInfo *cur_fi; /* 現在関数の捕捉解析結果 */
    u16 fn_slot_base;      /* この関数の frame 内ローカル数（codegen で確定） */
    i32 cur_fn;            /* codegen 中の関数 index（main=0） */
    u32 in_func_depth;     /* 0=main */
    CgScope outer[AKL_MAX_DEPTH]; /* 祖先 scope（0=main）。n_outer が深さ */
    u32 n_outer;
    /* loop の break/continue パッチ連鎖（pos のリストを逆方向リンク: buf[pos]=prev head） */
    u32 brk_head[64], cont_head[64], cont_kind[64]; u32 n_loops;
    /* ラベル文: 名前 / 対応ループ li（未確定 UINT32_MAX）/ 非ループ break パッチリスト */
    u32 lbl_name[64], lbl_li[64], lbl_brk_head[64];
    u8 lbl_is_loop[64];
    u32 n_lbl;
    u32 lbl_pending;   /* 次に開設されるループに確定すべきラベル index（UINT32_MAX=無し） */
    u8 loop_kind[64];           /* 0=loop / 1=switch（continue の解決で switch を飛ばす） */
    u32 tmp_seq;                /* 匿名一時ローカル名の採番（0xFFFFFFFE - n 系列） */
    bool super_pending;         /* 次に生成する N_CLASSMETH 関数は親（スタック）を env にバインド */
    u16 try_depth;              /* lex 上の try 領域の深さ（catch 本体含む） */
    u8 try_at_loop[64];         /* 各 loop 開設時の try_depth（try 越境 brk/cont の検出用） */
    bool fail;
} Cg;

static u32 cg_push_byte(Cg *cg, u8 b) {
    AklRT *rt = cg->rt;
    if (rt->code_len == rt->code_cap) {
        u32 nc = rt->code_cap ? rt->code_cap * 2 : 1024;
        u8 *ncp = (u8 *)realloc(rt->code, nc);
        if (!ncp) { akl_errf(rt, "oom: code"); cg->fail = true; return UINT32_MAX; }
        rt->code = ncp; rt->code_cap = nc;
    }
    rt->code[rt->code_len++] = b;
    return rt->code_len - 1;
}
static u32 cg_u32(Cg *cg, u32 v) {
    u32 at = cg->rt->code_len;
    for (int i = 0; i < 4; i++)
        if (cg_push_byte(cg, (u8)(v >> (8 * i))) == UINT32_MAX) return UINT32_MAX;
    return at;
}
static u8 cg_op(Cg *cg, u8 op) { return (u8)cg_push_byte(cg, op); }
static u32 cg_target_here(Cg *cg) { return cg->rt->code_len; }
static void cg_patch_u32(Cg *cg, u32 at, u32 v) {
    for (int i = 0; i < 4; i++) cg->rt->code[at + (u32)i] = (u8)(v >> (8 * i));
}

/* ローカル名 → slot。新規なら追加。is_const_out に宣言種別。
 * capture 済み（解析で確定）なら captured/env_idx を付与（frame の dummies 分は
 * 含まれない: dummies は関数 scope 開設時に予約されるため実ローカルはその後ろ）。 */
static i32 cg_local_find(Cg *cg, u32 name) {
    for (u32 i = 0; i < cg->n_locals; i++)
        if (cg->locals[i].name == name) return (i32)i;
    return -1;
}
static i32 cg_local_add(Cg *cg, u32 name, u8 is_const) {
    i32 at = cg_local_find(cg, name);
    if (at >= 0) {
        if (cg->locals[at].is_const) {
            akl_errf(cg->rt, "reassignment of const binding"); cg->fail = true; return -1;
        }
        return at;
    }
    if (cg->n_locals >= AKL_MAX_LOCALS) { akl_errf(cg->rt, "too many locals"); cg->fail = true; return -1; }
    if (cg->n_locals == cg->cap_locals) {
        u32 nc = cg->cap_locals ? cg->cap_locals * 2 : 32;
        LocalEnt *nl = (LocalEnt *)realloc(cg->locals, (u64)nc * sizeof(LocalEnt));
        if (!nl) { akl_errf(cg->rt, "oom: locals"); cg->fail = true; return -1; }
        cg->locals = nl; cg->cap_locals = nc;
    }
    cg->locals[cg->n_locals].name = name;
    cg->locals[cg->n_locals].is_const = is_const;
    cg->locals[cg->n_locals].captured = 0;
    cg->locals[cg->n_locals].env_idx = 0xFF;
    /* 解析済み capture 名と突合（名前 → ENV idx = cap_names 内位置） */
    if (cg->cur_fi && name != UINT32_MAX) {
        for (u16 k = 0; k < cg->cur_fi->n_cap; k++) {
            if (cg->cur_fi->cap_names[k] == name) {
                cg->locals[cg->n_locals].captured = 1;
                cg->locals[cg->n_locals].env_idx = (u8)k;
                break;
            }
        }
    }
    return (i32)cg->n_locals++;
}
/* 隠し slot（this / 自前 ENV / cap ENV）のダミー予約。同名 UINT32_MAX を複数回
 * 追加するため cg_local_add（find 経由・const 衝突検査）は使えない。 */
static i32 cg_local_add_dummy(Cg *cg) {
    if (cg->n_locals >= AKL_MAX_LOCALS) { akl_errf(cg->rt, "too many locals"); cg->fail = true; return -1; }
    if (cg->n_locals == cg->cap_locals) {
        u32 nc = cg->cap_locals ? cg->cap_locals * 2 : 32;
        LocalEnt *nl = (LocalEnt *)realloc(cg->locals, (u64)nc * sizeof(LocalEnt));
        if (!nl) { akl_errf(cg->rt, "oom: locals"); cg->fail = true; return -1; }
        cg->locals = nl; cg->cap_locals = nc;
    }
    cg->locals[cg->n_locals].name = UINT32_MAX;
    cg->locals[cg->n_locals].is_const = 1;
    cg->locals[cg->n_locals].captured = 0;
    cg->locals[cg->n_locals].env_idx = 0xFF;
    return (i32)cg->n_locals++;
}

/* ラベル → ループ確定: ループ開設時に、直前に開いたラベル（ループ付き）があれば紐付ける */
static void cg_lbl_bind(Cg *cg, u32 li) {
    if (cg->lbl_pending != UINT32_MAX) {
        cg->lbl_li[cg->lbl_pending] = li;
        cg->lbl_pending = UINT32_MAX;
    }
}

static bool cg_captured(Cg *cg, u32 name, bool store, u32 *env_idx_out);
/* 代入ターゲット（N_IDENT）が外側関数の capture 済み変数か。capture 済みは ENV 経由
 * なので、slot/global 直結の融合（LINC/GSTORE_SPV 等）は全て不適格。 */
static bool cg_tgt_is_captured(Cg *cg, u32 ni) {
    if (ni == N_NONE || cg->p->nodes[ni].kind != N_IDENT) return false;
    return cg_captured(cg, cg->p->nodes[ni].a, false, NULL);
}

/* 融合命令の対象として「素のローカル（capture されていない）」か。capture 済み
 * ローカルは実体が ENV にあるため、slot 直接アクセス系の融合は全て不適格。 */
static bool cg_local_plain(Cg *cg, i32 slot) {
    return slot >= 0 && (u32)slot < cg->n_locals && !cg->locals[slot].captured;
}
/* 現在関数の隠し slot 数（this + 自前 env + cap env の順で固定） */
static u32 cg_hidden(const Cg *cg) {
    return 1 + (cg->cur_n_env ? 1u : 0u) + (cg->cur_needs_cap ? 1u : 0u);
}

/* name(u32 intern id) → global slot の O(1) 検索（脱 Salt: name は攻撃者文字列ではなく
 * エンジン内部の連番 intern id のため DoS 耐性問題なし。globals は append-only で、
 * ベイク時点とのズレは n_globals 一致で検知して全再構築） */
static bool ghash_build(AklRT *rt) {
    u32 need = 16;
    while (need < rt->n_globals * 2 + 8) need <<= 1;
    u32 *g = (u32 *)calloc(need, sizeof(u32));
    if (!g) return false;
    for (u32 i = 0; i < rt->n_globals; i++) {
        u32 h = (rt->globals[i].name * 2654435761u) & (need - 1);
        while (g[h]) h = (h + 1) & (need - 1);
        g[h] = i + 1; /* 1-based。0 は空 */
    }
    free(rt->ghash);
    rt->ghash = g; rt->ghash_cap = need; rt->ghash_sync = rt->n_globals;
    return true;
}
static u32 cg_global_find(AklRT *rt, u32 name) {
    if (!rt->n_globals) return UINT32_MAX;
    if (rt->ghash_sync != rt->n_globals) {
        if (!ghash_build(rt)) {
            for (u32 i = 0; i < rt->n_globals; i++) /* メモリ逼迫時のフォールバック */
                if (rt->globals[i].name == name) return i;
            return UINT32_MAX;
        }
    }
    u32 h = (name * 2654435761u) & (rt->ghash_cap - 1);
    while (rt->ghash[h]) {
        u32 gi = rt->ghash[h] - 1;
        if (rt->globals[gi].name == name) return gi;
        h = (h + 1) & (rt->ghash_cap - 1);
    }
    return UINT32_MAX;
}
static u32 cg_global_add(AklRT *rt, u32 name, u8 is_const) {
    u32 at = cg_global_find(rt, name);
    if (at != UINT32_MAX) {
        if (rt->globals[at].is_const) {
            akl_errf(rt, "reassignment of const binding"); return UINT32_MAX;
        }
        return at;
    }
    if (rt->n_globals == rt->cap_globals) {
        u32 nc = rt->cap_globals ? rt->cap_globals * 2 : 32;
        AklGlobal *ng = (AklGlobal *)realloc(rt->globals, (u64)nc * sizeof(AklGlobal));
        if (!ng) { akl_errf(rt, "oom: globals"); return UINT32_MAX; }
        rt->globals = ng; rt->cap_globals = nc;
    }
    rt->globals[rt->n_globals].name = name;
    rt->globals[rt->n_globals].is_const = is_const;
    rt->globals[rt->n_globals].v = AKL_VAL_UNDEF;
    return rt->n_globals++;
}

/* 祖先 scope への capture 解決: 見つかれば CELOAD/CESTORE を発行し true。
 * depth = 現在関数と祖先 a の間で自前 ENV を持つ関数の数（= ENV チェーンの parent ホップ数）。
 * 発行される cap slot は現在関数の frame 隠し slot（codegen 時に確定）。 */
static bool cg_captured(Cg *cg, u32 name, bool store, u32 *env_idx_out) {
    for (u32 a = cg->n_outer; a-- > 0;) {
        const CgScope *os = &cg->outer[a];
        if (os->fi) {
            for (u16 k = 0; k < os->fi->n_cap; k++) {
                if (os->fi->cap_names[k] == name) {
                    u32 depth = 0;
                    for (u32 m = a + 1; m < cg->n_outer; m++)
                        if (cg->outer[m].n_env) depth++;
                    u8 cap_slot = (u8)(1 + (cg->cur_n_env ? 1u : 0u));
                    cg_op(cg, store ? OP_CESTORE : OP_CELOAD);
                    cg_push_byte(cg, cap_slot);
                    cg_push_byte(cg, (u8)depth);
                    cg_u32(cg, k);
                    if (env_idx_out) *env_idx_out = k;
                    return true;
                }
            }
        }
    }
    return false;
}

/* name のストア命令を出す（main では G、関数内では L 解決→capture→見つからなければ G） */
static bool cg_store(Cg *cg, u32 name, u8 decl_const, bool decl) {
    /* capture 解決が先（関数内からの代入は全て CESTORE 経由。const は解析対象外:
     * 解析は名前ベースなので const ローカルも capture され得る — const は不変なので
     * 代入はそもそも compile 時エラーになる（外側の const への代入も同様に拒否） */
    if (cg->in_func_depth > 0 && !decl && cg_captured(cg, name, true, NULL))
        return !cg->fail;
    if (cg->in_func_depth == 0) {
        /* main 固有ローカル（catch 束縛）があればローカルを優先。var 宣言は従来通りグローバル */
        i32 lslot = decl ? -1 : cg_local_find(cg, name);
        if (lslot >= 0) {
            if (cg->locals[lslot].is_const) { akl_errf(cg->rt, "assignment to const local"); cg->fail = true; return false; }
            if (cg->locals[lslot].captured) { /* main の capture 済みローカル（catch 束縛） */
                cg_op(cg, OP_ESTORE);
                cg_u32(cg, cg->locals[lslot].env_idx);
                return !cg->fail;
            }
            cg_op(cg, OP_LSTORE);
            cg_u32(cg, (u32)lslot);
            return !cg->fail;
        }
        u32 gi = cg_global_add(cg->rt, name, decl ? decl_const : 0);
        if (gi == UINT32_MAX) { cg->fail = true; return false; }
        AklGlobal *g = &cg->rt->globals[gi];
        if (!decl && g->is_const) { akl_errf(cg->rt, "assignment to const global"); cg->fail = true; return false; }
        (void)g;
        cg_op(cg, OP_GSTORE_S); /* compile 時登録不変条件により name 解決不要 */
        cg_u32(cg, gi);
        return !cg->fail;
    }
    i32 slot = decl ? cg_local_add(cg, name, decl_const) : cg_local_find(cg, name);
    if (slot < 0 && !cg->fail) {
        /* 未定義への代入はグローバル生成（非 strict 近似） */
        u32 gi = cg_global_add(cg->rt, name, 0);
        if (gi == UINT32_MAX) { cg->fail = true; return false; }
        cg_op(cg, OP_GSTORE_S);
        cg_u32(cg, gi);
        return true;
    }
    if (cg->fail) return false;
    if (!decl && cg->locals[slot].is_const) { akl_errf(cg->rt, "assignment to const local"); cg->fail = true; return false; }
    if (cg->locals[slot].captured) {
        cg_op(cg, OP_ESTORE);
        cg_u32(cg, cg->locals[slot].env_idx);
        return !cg->fail;
    }
    cg_op(cg, OP_LSTORE);
    cg_u32(cg, (u32)slot);
    return !cg->fail;
}
static bool cg_load(Cg *cg, u32 name) {
    /* capture 解決が先（関数内からの参照は CELOAD 経由） */
    if (cg->in_func_depth > 0 && cg_captured(cg, name, false, NULL))
        return !cg->fail;
    /* main 深でもローカルを先に見る（catch 束縛は main にも存在する）。
     * main 固有ローカルが作られるのは catch 束縛のみなので旧経路との衝突は構造的に無い */
    {
        i32 slot = cg_local_find(cg, name);
        if (slot >= 0) {
            if (cg->locals[slot].captured) {
                cg_op(cg, OP_ELOAD);
                cg_u32(cg, cg->locals[slot].env_idx);
                return !cg->fail;
            }
            cg_op(cg, OP_LLOAD);
            cg_u32(cg, (u32)slot);
            return !cg->fail;
        }
    }
    /* 名前が compile 時点で登録済みなら直結版。未登録は前方参照（実行時解決に倒す） */
    u32 gi = cg_global_find(cg->rt, name);
    if (gi != UINT32_MAX) {
        cg_op(cg, OP_GLOAD_S);
        cg_u32(cg, gi);
        return !cg->fail;
    }
    cg_op(cg, OP_GLOAD);
    cg_u32(cg, name);
    return !cg->fail;
}

static void cg_expr(Cg *cg, u32 ni);
static void cg_stmt(Cg *cg, u32 ni);
static void cg_fn(Cg *cg, u32 ni);

/* ジャンプ出力ヘルパ。patch site(pos of u32 imm)を返す */
static u32 cg_jmp_op(Cg *cg, u8 op) {
    cg_op(cg, op);
    return cg_u32(cg, 0);
}

/* 条件式の融合吐き: `local/global rel int定数` の形だけ CJMPF_L/G 1命令にし、
 * それ以外は通常経路（expr + JMPF）。戻り値は分岐先 patch site（両経路で同一規約）。
 * 意味保持の証明: 融合側も一般側も 'lhs rel rhs が偽なら tgt へ' のみ。スタック効果は
 * 一般形が +1+1-1-1=0、融合形が 0 で一致。NaN 時 false も同一（VM コメント参照）。 */
static u32 cg_cond_jmpf(Cg *cg, u32 ni) {
    AklNode *n = &cg->p->nodes[ni];
    u8 cmp = 0xFF;
    if (n->kind == N_BIN) {
        if (n->op == OP_LT) cmp = 0;
        else if (n->op == OP_LE) cmp = 1;
        else if (n->op == OP_GT) cmp = 2;
        else if (n->op == OP_GE) cmp = 3;
    }
    if (cmp != 0xFF && n->a != N_NONE && n->b != N_NONE) {
        AklNode *L = &cg->p->nodes[n->a], *R = &cg->p->nodes[n->b];
        int var_side = (L->kind == N_IDENT && R->kind == N_NUM && R->op == 1) ? 0
                     : (R->kind == N_IDENT && L->kind == N_NUM && L->op == 1) ? 1 : -1;
        if (var_side >= 0) {
            u32 name = var_side ? R->a : L->a;
            u32 imm = var_side ? L->a : R->a;
            /* var 右辺形はオペランド交換で cmp を反転（LT<->GT, LE<->GE） */
            if (var_side) cmp = cmp == 0 ? 2 : cmp == 1 ? 3 : cmp == 2 ? 0 : 1;
            if (cg->in_func_depth != 0) {
                i32 slot = cg_local_find(cg, name);
                if (cg_local_plain(cg, slot)) {
                    cg_op(cg, OP_CJMPF_L);
                    cg_u32(cg, (u32)slot);
                    cg_u32(cg, imm);
                    cg_op(cg, cmp);
                    return cg_u32(cg, 0);
                }
            }
            if (cg->in_func_depth > 0 && cg_captured(cg, name, false, NULL)) {
                cg_expr(cg, ni);
                return cg_jmp_op(cg, OP_JMPF); /* capture は汎用経路へ */
            }
            /* 前方参照（compile 時未登録）は汎用経路に倒す。登録済みなら slot 直結 */
            u32 gi = cg_global_find(cg->rt, name);
            if (gi == UINT32_MAX) {
                cg_expr(cg, ni);
                return cg_jmp_op(cg, OP_JMPF);
            }
            cg_op(cg, OP_CJMPF_G);
            cg_u32(cg, gi);
            cg_u32(cg, imm);
            cg_op(cg, cmp);
            return cg_u32(cg, 0);
        }
    }
    /* `(x * y) rel z` ネスト乗算比較の融合（試行除法ループの条件形。全項グローバル:
     * グローバル値の読み出しに副作用はなく、非融合の「GLOAD;GLOAD;MUL;GLOAD;REL;JMPF」と
     * 丸め・分岐が一致する。int 積の溢れは i64→double 一回丸めで MUL 汎用路と同一） */
    if (cmp != 0xFF && n->a != N_NONE && n->b != N_NONE && cg->in_func_depth == 0) {
        AklNode *L = &cg->p->nodes[n->a], *R = &cg->p->nodes[n->b];
        AklNode *muln = NULL, *zn = NULL;
        u8 mcmp = cmp;
        if (L->kind == N_BIN && L->op == OP_MUL && R->kind == N_IDENT) { muln = L; zn = R; }
        else if (R->kind == N_BIN && R->op == OP_MUL && L->kind == N_IDENT) {
            muln = R; zn = L;
            mcmp = mcmp == 0 ? 2 : mcmp == 1 ? 3 : mcmp == 2 ? 0 : 1; /* 左右交換で rel 反転 */
        }
        if (muln && muln->a != N_NONE && muln->b != N_NONE &&
            cg->p->nodes[muln->a].kind == N_IDENT && cg->p->nodes[muln->b].kind == N_IDENT) {
            u32 g1 = cg_global_find(cg->rt, cg->p->nodes[muln->a].a);
            u32 g2 = cg_global_find(cg->rt, cg->p->nodes[muln->b].a);
            u32 g3 = cg_global_find(cg->rt, zn->a);
            if (g1 != UINT32_MAX && g2 != UINT32_MAX && g3 != UINT32_MAX) {
                cg_op(cg, OP_CJMPF_MULGG);
                cg_u32(cg, g1);
                cg_u32(cg, g2);
                cg_u32(cg, g3);
                cg_op(cg, mcmp);
                return cg_u32(cg, 0);
            }
        }
    }
    /* `(x % m) == k` / `!= k` の融合。EQ/NE は対称なので左右ミラーは cmp 値で吸収する。
     * 意味は「MOD; CONST_I; EQ/NE; JMPF」と逐語同一（ハンドラの int/fmod 分岐が MOD と同型）。 */
    if (n->kind == N_BIN && (n->op == OP_EQ || n->op == OP_NE) && n->a != N_NONE && n->b != N_NONE) {
        AklNode *L = &cg->p->nodes[n->a], *R = &cg->p->nodes[n->b];
        AklNode *modn = NULL, *kn = NULL;
        if (L->kind == N_BIN && L->op == OP_MOD && R->kind == N_NUM && R->op == 1) { modn = L; kn = R; }
        else if (R->kind == N_BIN && R->op == OP_MOD && L->kind == N_NUM && L->op == 1) { modn = R; kn = L; }
        if (modn && modn->a != N_NONE && modn->b != N_NONE) {
            AklNode *v = &cg->p->nodes[modn->a], *m = &cg->p->nodes[modn->b];
            if (v->kind == N_IDENT && m->kind == N_IDENT && cg->in_func_depth == 0) {
                /* 除数が変数の経路: MODGG（k は引き続き int 定数限定） */
                u8 mcmp = n->op == OP_EQ ? 0 : 1;
                u32 gx = cg_global_find(cg->rt, v->a);
                u32 gm = cg_global_find(cg->rt, m->a);
                if (gx != UINT32_MAX && gm != UINT32_MAX) {
                    cg_op(cg, OP_CJMPF_MODGG);
                    cg_u32(cg, gx);
                    cg_u32(cg, gm);
                    cg_u32(cg, kn->a);
                    cg_op(cg, mcmp);
                    return cg_u32(cg, 0);
                }
            }
            if (v->kind == N_IDENT && m->kind == N_NUM && m->op == 1) {
                u8 mcmp = n->op == OP_EQ ? 0 : 1;
                if (cg->in_func_depth != 0) {
                    i32 slot = cg_local_find(cg, v->a);
                    if (cg_local_plain(cg, slot)) {
                        cg_op(cg, OP_CJMPF_MODL);
                        cg_u32(cg, (u32)slot);
                        cg_u32(cg, m->a);
                        cg_u32(cg, kn->a);
                        cg_op(cg, mcmp);
                        return cg_u32(cg, 0);
                    }
                }
                if (cg->in_func_depth > 0 && cg_captured(cg, v->a, false, NULL)) {
                    cg_expr(cg, ni);
                    return cg_jmp_op(cg, OP_JMPF); /* capture は汎用経路へ */
                }
                u32 gi = cg_global_find(cg->rt, v->a);
                if (gi != UINT32_MAX) {
                    cg_op(cg, OP_CJMPF_MODG);
                    cg_u32(cg, gi);
                    cg_u32(cg, m->a);
                    cg_u32(cg, kn->a);
                    cg_op(cg, mcmp);
                    return cg_u32(cg, 0);
                }
            }
        }
    }
    cg_expr(cg, ni);
    return cg_jmp_op(cg, OP_JMPF);
}

static void cg_expr(Cg *cg, u32 ni) {
    if (cg->fail) return;
    AklNode *n = &cg->p->nodes[ni];
    switch (n->kind) {
    case N_NUM:
        if (n->op) {
            cg_op(cg, OP_CONST_I);
            cg_u32(cg, n->a);
        } else {
            AklVal bits = ((AklVal)n->b << 32) | n->a;
            double d;
            memcpy(&d, &bits, 8);
            cg_op(cg, OP_CONST_D);
            for (int i = 0; i < 8; i++) cg_push_byte(cg, (u8)(bits >> (8 * i)));
        }
        break;
    case N_STR:   cg_op(cg, OP_CONST_STR); cg_u32(cg, n->a); break;
    case N_REGEX: cg_op(cg, OP_NEWREGEX); cg_u32(cg, n->a); cg_u32(cg, n->b); break;
    case N_SUPERGET: /* super.name → 親クラス（関数 env の vals[0]）の name */
        cg_op(cg, OP_SUPERGET);
        cg_u32(cg, n->b);
        break;
    case N_SUPERMCALL: /* super.name(args) → [this][親.name][args] CALLT */
        cg_op(cg, OP_THIS);
        cg_op(cg, OP_SUPERGET);
        cg_u32(cg, n->b);
        for (u32 i = 0; i < n->d; i++) cg_expr(cg, cg->p->list[n->c + i]);
        cg_op(cg, OP_CALLT);
        cg_push_byte(cg, (u8)(n->d & 0xFF));
        break;
    case N_SUPERCALL: { /* super(args) → [this][親.constructor][args] CALLT */
        u32 cname = p_intern(cg->p, (const u8 *)"constructor", 11);
        cg_op(cg, OP_THIS);
        cg_op(cg, OP_SUPERGET);
        cg_u32(cg, cname);
        for (u32 i = 0; i < n->c; i++) cg_expr(cg, cg->p->list[n->b + i]);
        cg_op(cg, OP_CALLT);
        cg_push_byte(cg, (u8)(n->c & 0xFF));
        break;
    }
    case N_BOOL:  cg_op(cg, n->a ? OP_TRUE_T : OP_FALSE_T); break;
    case N_NULL:  cg_op(cg, OP_NULL_T); break;
    case N_UNDEF: cg_op(cg, OP_UNDEF_T); break;
    case N_IDENT: cg_load(cg, n->a); break;
    case N_UNARY:
        cg_expr(cg, n->a);
        cg_op(cg, n->op);
        break;
    case N_ASSIGN:
        cg_expr(cg, n->b);
        cg_op(cg, OP_DUP);
        cg_store(cg, n->a, 0, false);
        break;
    case N_BIN:
        if (n->op == OP_JMPT || n->op == OP_JMPF) { /* && / || 短絡 */
            cg_expr(cg, n->a);
            cg_op(cg, OP_DUP);
            u32 site = cg_jmp_op(cg, n->op == OP_JMPT ? OP_JMPT : OP_JMPF);
            cg_op(cg, OP_POP);
            cg_expr(cg, n->b);
            cg_patch_u32(cg, site, cg_target_here(cg));
            break;
        }
        /* ==== 融合: rhs が int 定数（*CI 系）。意味は「lhs; CONST_I; OP」と逐語同一。
         * 根拠: 各 *CI ハンドラの汎用路が同じ関数（bin_add/to_number/fmod）を同順序で呼ぶ。
         * 文字列化し得る ADD/SUB/MOD は const を右辺に限る（順序に意味があるため）。 */
        if ((n->op == OP_ADD || n->op == OP_SUB || n->op == OP_MUL || n->op == OP_MOD) && n->b != N_NONE) {
            AklNode *R = &cg->p->nodes[n->b];
            if (R->kind == N_NUM && R->op == 1) {
                u32 imm = R->a;
                if (n->op == OP_MUL && n->a != N_NONE && cg->p->nodes[n->a].kind == N_IDENT) {
                    u32 name = cg->p->nodes[n->a].a;
                    if (cg->in_func_depth != 0) {
                        i32 slot = cg_local_find(cg, name);
                        if (cg_local_plain(cg, slot)) { cg_op(cg, OP_LMULC); cg_u32(cg, (u32)slot); cg_u32(cg, imm); break; }
                    }
                    if (cg->in_func_depth > 0 && cg_captured(cg, name, false, NULL)) {
                        cg_expr(cg, n->a);
                        cg_expr(cg, n->b);
                        cg_op(cg, n->op);
                        break;
                    }
                    u32 gi = cg_global_find(cg->rt, name);
                    if (gi != UINT32_MAX) { cg_op(cg, OP_GMULC); cg_u32(cg, gi); cg_u32(cg, imm); break; }
                }
                cg_expr(cg, n->a);
                cg_op(cg, n->op == OP_ADD ? OP_ADDCI : n->op == OP_SUB ? OP_SUBCI :
                        n->op == OP_MUL ? OP_MULCI : OP_MODCI);
                cg_u32(cg, imm);
                break;
            }
        }
        /* c * x の左定数: MUL は交換可能（int 同値・double は同一引数の一回丸めで一致・
         * 文字列は N_STR が対象外なので N_NUM 定数のみ＝無副作用 to_number のみ）。 */
        if (n->op == OP_MUL && n->a != N_NONE && n->b != N_NONE) {
            AklNode *L2 = &cg->p->nodes[n->a];
            if (L2->kind == N_NUM && L2->op == 1) {
                u32 imm = L2->a;
                if (cg->p->nodes[n->b].kind == N_IDENT) {
                    u32 name = cg->p->nodes[n->b].a;
                    if (cg->in_func_depth != 0) {
                        i32 slot = cg_local_find(cg, name);
                        if (cg_local_plain(cg, slot)) { cg_op(cg, OP_LMULC); cg_u32(cg, (u32)slot); cg_u32(cg, imm); break; }
                    }
                    if (cg->in_func_depth > 0 && cg_captured(cg, name, false, NULL)) {
                        cg_expr(cg, n->a);
                        cg_expr(cg, n->b);
                        cg_op(cg, n->op);
                        break;
                    }
                    u32 gi = cg_global_find(cg->rt, name);
                    if (gi != UINT32_MAX) { cg_op(cg, OP_GMULC); cg_u32(cg, gi); cg_u32(cg, imm); break; }
                }
                cg_expr(cg, n->b);
                cg_op(cg, OP_MULCI);
                cg_u32(cg, imm);
                break;
            }
        }
        /* x + rhs（rhs 非定数）: TOS = x + TOS。bin_add のオペランド順序は 左=x で保持
         * （文字列連結を含めて非融合経路と同一結果）。 */
        if (n->op == OP_ADD && n->a != N_NONE && cg->p->nodes[n->a].kind == N_IDENT) {
            u32 name = cg->p->nodes[n->a].a;
            if (cg->in_func_depth != 0) {
                i32 slot = cg_local_find(cg, name);
                if (cg_local_plain(cg, slot)) { cg_expr(cg, n->b); cg_op(cg, OP_LADD_P); cg_u32(cg, (u32)slot); break; }
            }
            if (cg->in_func_depth > 0 && cg_captured(cg, name, false, NULL)) {
                cg_expr(cg, n->a);
                cg_expr(cg, n->b);
                cg_op(cg, n->op);
                break;
            }
            u32 gi = cg_global_find(cg->rt, name);
            if (gi != UINT32_MAX) {
                cg_expr(cg, n->b);
                cg_op(cg, OP_GADD_P);
                cg_u32(cg, gi);
                break;
            }
        }
        cg_expr(cg, n->a);
        cg_expr(cg, n->b);
        cg_op(cg, n->op);
        break;
    case N_CALL: {
        cg_expr(cg, n->a);
        bool has_spread = false;
        for (u32 i = 0; i < n->c; i++)
            if (cg->p->nodes[cg->p->list[n->b + i]].kind == N_SPREAD) { has_spread = true; break; }
        if (has_spread) {
            i32 t = cg_local_add(cg, 0xFFFFFFFEu - (u32)cg->tmp_seq++, 0);
            if (t < 0) return;
            cg_op(cg, OP_CONST_I); cg_u32(cg, 0);
            cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)t); /* 引数カウンタ */
            for (u32 i = 0; i < n->c; i++) {
                u32 el = cg->p->list[n->b + i];
                if (cg->p->nodes[el].kind == N_SPREAD) {
                    cg_expr(cg, cg->p->nodes[el].a);
                    cg_op(cg, OP_ARRSPREADC); cg_u32(cg, (u32)t);
                } else {
                    cg_expr(cg, el);
                    cg_op(cg, OP_LINC); cg_u32(cg, (u32)t); cg_u32(cg, 1);
                }
                if (cg->fail) return;
            }
            cg_op(cg, OP_CALLN); cg_u32(cg, (u32)t);
        } else {
            for (u32 i = 0; i < n->c; i++) cg_expr(cg, cg->p->list[n->b + i]);
            cg_op(cg, OP_CALL);
            cg_push_byte(cg, (u8)(n->c & 0xFF));
        }
        break;
    }
    case N_OBJLIT: {
        /* [obj] → (dup,val,PSTORE,POP)* の直線形。最後に obj が TOS に残る。
         * 要素が N_SPREAD なら {...src} を OBJSPREAD でマージ（後勝ち） */
        cg_op(cg, OP_OBJNEW);
        for (u32 i = 0; i < n->c; i++) {
            u32 ve = cg->p->list[n->b + i];
            if (ve < cg->p->n_nodes && cg->p->nodes[ve].kind == N_SPREAD) {
                cg_expr(cg, cg->p->nodes[ve].a);
                cg_op(cg, OP_OBJSPREAD);
                continue;
            }
            if (ve < cg->p->n_nodes && cg->p->nodes[ve].kind == N_OBJKEY) {
                /* computed: [obj] → DUP; val; key; PSETDYN; POP */
                cg_op(cg, OP_DUP);
                cg_expr(cg, cg->p->nodes[ve].b); /* 値 */
                cg_expr(cg, cg->p->nodes[ve].a); /* キー式 */
                cg_op(cg, OP_PSETDYN);
                cg_op(cg, OP_POP);
                if (cg->fail) return;
                continue;
            }
            if (ve < cg->p->n_nodes && cg->p->nodes[ve].kind == N_CLASSMETH) {
                /* メソッド短縮: cg_fn で関数を作って PSTORE */
                cg_op(cg, OP_DUP);
                cg_fn(cg, ve);
                cg_op(cg, OP_PSTORE);
                cg_u32(cg, cg->p->list[n->a + i]);
                cg_op(cg, OP_POP);
                if (cg->fail) return;
                continue;
            }
            cg_op(cg, OP_DUP);
            cg_expr(cg, ve);
            cg_op(cg, OP_PSTORE);
            cg_u32(cg, cg->p->list[n->a + i]);
            cg_op(cg, OP_POP);
            if (cg->fail) return;
        }
        break;
    }
    case N_PGET:
        cg_expr(cg, n->a);
        cg_op(cg, OP_PLOAD);
        cg_u32(cg, n->b);
        break;
    case N_PSET:
        cg_expr(cg, n->a);
        cg_expr(cg, n->b);
        cg_op(cg, OP_PSTORE);
        cg_u32(cg, n->c);
        break;
    case N_MCALL: {
        cg_expr(cg, n->a); /* レシーバ（this）が先に積まれる */
        bool has_spread = false;
        for (u32 i = 0; i < n->c; i++)
            if (cg->p->nodes[cg->p->list[n->b + i]].kind == N_SPREAD) { has_spread = true; break; }
        if (has_spread) {
            /* レイアウト [obj][args...]。spread の個数を一時ローカルに累積し、
             * OP_MCALLN が locals[slot] を argc として使う（関数呼び出し spread と同形） */
            i32 t = cg_local_add(cg, 0xFFFFFFFEu - (u32)cg->tmp_seq++, 0);
            if (t < 0) return;
            cg_op(cg, OP_CONST_I); cg_u32(cg, 0);
            cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)t);
            for (u32 i = 0; i < n->c; i++) {
                u32 el = cg->p->list[n->b + i];
                if (cg->p->nodes[el].kind == N_SPREAD) {
                    cg_expr(cg, cg->p->nodes[el].a);
                    cg_op(cg, OP_ARRSPREADC); cg_u32(cg, (u32)t);
                } else {
                    cg_expr(cg, el);
                    cg_op(cg, OP_LINC); cg_u32(cg, (u32)t); cg_u32(cg, 1);
                }
                if (cg->fail) return;
            }
            cg_op(cg, OP_MCALLN);
            cg_u32(cg, (u32)t);
            cg_u32(cg, n->d);
        } else {
            for (u32 i = 0; i < n->c; i++) cg_expr(cg, cg->p->list[n->b + i]);
            cg_op(cg, OP_MCALL);
            cg_push_byte(cg, (u8)(n->c & 0xFF));
            cg_u32(cg, n->d);
        }
        break;
    }
    case N_DESTR: {
        /* 右辺を評価して temp に退避し、各要素を store。
         * 式の値は右辺（JS: 分割代入式の値は右辺） */
        i32 t = cg_local_add(cg, 0xFFFFFFFEu - (u32)cg->tmp_seq++, 0);
        if (t < 0) break;
        if (n->b != N_NONE) {
            cg_expr(cg, n->b);
            cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)t);
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t); /* 式の値として右辺を残す */
        } else {
            cg_op(cg, OP_UNDEF_T);
            break; /* 右辺なし（var でない代入文では来ない） */
        }
        u32 idx = 0; /* 配列要素の現在位置（rest 用） */
        for (u32 i = 0; i < n->c; i++) {
            u32 el = cg->p->list[n->a + i];
            u8 ek = cg->p->nodes[el].kind;
            if (ek == N_DSTR_EL) {
                u8 fl = cg->p->nodes[el].flags;
                u32 name = cg->p->nodes[el].a;
                if (fl == 2) { /* ネスト */
                    /* 部分値を取得してネスト N_DESTR の b に相当する temp を用意:
                     * ネストの要素を LOAD t; idx; AGET/PLOAD で展開する簡易実装。
                     * ここではネスト N_DESTR を「その b に部分値」として再帰処理 */
                    u32 nest = cg->p->nodes[el].c;
                    i32 t2 = cg_local_add(cg, 0xFFFFFFFEu - (u32)cg->tmp_seq++, 0);
                    if (t2 < 0) break;
                    if (fl & 1) { /* オブジェクトネスト */
                        cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t);
                        cg_op(cg, OP_PLOAD); cg_u32(cg, cg->p->nodes[el].b);
                    } else {
                        cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t);
                        cg_op(cg, OP_CONST_I); cg_u32(cg, cg->p->nodes[el].b);
                        cg_op(cg, OP_AGET);
                    }
                    cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)t2);
                    /* ネスト要素の処理（N_DESTR の c をループ） */
                    AklNode *nn = &cg->p->nodes[nest];
                    for (u32 j = 0; j < nn->c; j++) {
                        u32 nel = cg->p->list[nn->a + j];
                        if (cg->p->nodes[nel].kind != N_DSTR_EL) continue;
                        u8 nfl = cg->p->nodes[nel].flags;
                        if (nfl == 3) continue; /* ネストのネストは非対応（明示） */
                        cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t2);
                        if (nfl & 1) { cg_op(cg, OP_PLOAD); cg_u32(cg, cg->p->nodes[nel].b); }
                        else { cg_op(cg, OP_CONST_I); cg_u32(cg, cg->p->nodes[nel].b); cg_op(cg, OP_AGET); }
                        if (!cg->fail) cg_store(cg, cg->p->nodes[nel].a, 0, true);
                    }
                    continue;
                }
                if (fl & 1) { /* オブジェクト: prop 名 */
                    cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t);
                    cg_op(cg, OP_PLOAD); cg_u32(cg, cg->p->nodes[el].b);
                } else { /* 配列: index */
                    cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t);
                    cg_op(cg, OP_CONST_I); cg_u32(cg, cg->p->nodes[el].b);
                    cg_op(cg, OP_AGET);
                    idx = cg->p->nodes[el].b + 1;
                }
                if (!cg->fail) cg_store(cg, name, 0, true);
            } else if (ek == N_DSTR_REST) {
                u32 rname = cg->p->nodes[el].a;
                if (cg->p->nodes[el].flags & 1) {
                    /* オブジェクト rest: {a, ...rest} = obj → OBJREST で全コピー後、
                     * この N_DESTR で既に取り出したキーを PDEL で除外 */
                    cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t);
                    cg_op(cg, OP_OBJREST);
                    for (u32 j = 0; j < n->c; j++) {
                        u32 oel = cg->p->list[n->a + j];
                        if (cg->p->nodes[oel].kind != N_DSTR_EL) continue;
                        if (cg->p->nodes[oel].flags & 2) continue; /* ネスト要素は除外不可（明示） */
                        cg_op(cg, OP_DUP);
                        cg_op(cg, OP_PDEL); cg_u32(cg, cg->p->nodes[oel].b);
                        cg_op(cg, OP_POP);
                    }
                    if (!cg->fail) cg_store(cg, rname, 0, true);
                } else {
                    /* 配列 rest: [a, b, ...rest] = arr → 現在 index から末尾までを配列化 */
                    cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t);
                    cg_op(cg, OP_CONST_I); cg_u32(cg, idx); /* 現在位置 */
                    cg_op(cg, OP_ARRREST);
                    if (!cg->fail) cg_store(cg, rname, 0, true);
                }
            }
        }
        break;
    }
    case N_TPL: {
        /* 断片: [テキスト, 式, テキスト, 式, ...] を連結。
         * 式の後のテキストが無い場合（末尾 ${x}）は空文字を補完（パーサが必ず
         * テキストを積むので通常は不要 — 防御的に空 N_STR を積む） */
        u32 cnt = n->c;
        if (cnt == 0) { cg_op(cg, OP_CONST_STR); cg_u32(cg, p_intern(cg->p, (const u8 *)"", 0)); break; }
        /* 断片を先頭から順に push し、2 個目以降は毎回 ADD（先頭テキストが
         * 未使用のまま残るバグを修正: 1 断片式テンプレートで先頭が消えていた） */
        cg_expr(cg, cg->p->list[n->b]); /* 先頭テキスト */
        for (u32 i = 1; i < cnt; i++) {
            cg_expr(cg, cg->p->list[n->b + i]);
            cg_op(cg, OP_ADD);
        }
        break;
    }
    case N_OPTCHAIN: {
        /* a?.b → T=base; LOAD T; DUP; NULL; EQ; JMPT Lnull; POP; LOAD T; PLOAD name; JMP Lend;
         *         Lnull: POP; UNDEF; Lend:
         * a?.[i] は LOAD T; LOAD T; i; AGET に、a?.(args) は LOAD T; args; CALL に展開。
         * null ガードは「null または undefined」（EQ は null==undefined を true にする）。 */
        i32 t = cg_local_add(cg, 0xFFFFFFFEu - (u32)cg->tmp_seq++, 0);
        if (t < 0) break;
        cg_expr(cg, n->a);
        cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)t);
        cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t);
        cg_op(cg, OP_DUP);
        cg_op(cg, OP_NULL_T);
        cg_op(cg, OP_EQ); /* null == undefined は true（loose eq） */
        u32 s_null = cg_jmp_op(cg, OP_JMPT);
        cg_op(cg, OP_POP); /* null でない: 元の base 値を捨てる */
        cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t);
        if (n->c == 0) {
            cg_op(cg, OP_PLOAD); cg_u32(cg, n->b);
        } else if (n->c == 1) {
            cg_expr(cg, n->b);
            cg_op(cg, OP_AGET);
        } else {
            for (u32 i = 0; i < n->d; i++) cg_expr(cg, cg->p->list[n->b + i]);
            cg_op(cg, OP_CALL);
            cg_push_byte(cg, (u8)(n->d & 0xFF));
        }
        u32 s_end = cg_jmp_op(cg, OP_JMP);
        cg_patch_u32(cg, s_null, cg_target_here(cg));
        cg_op(cg, OP_POP); /* スタックの base を捨てる */
        cg_op(cg, OP_UNDEF_T);
        cg_patch_u32(cg, s_end, cg_target_here(cg));
        break;
    }
    case N_THIS:
        cg_op(cg, OP_THIS);
        break;
    case N_FUNCEXPR:
        cg_fn(cg, ni);
        break;
    case N_COMMA:
        cg_expr(cg, n->a);
        cg_op(cg, OP_POP);
        cg_expr(cg, n->b);
        break;
    case N_QQ: {
        /* a ?? b: expr a; DUP; NULL_T; EQ; JMPT Lb; JMP Lend;
         *          Lb: POP; expr b; Lend:
         *   a が null/undefined でなければ a が TOS に残る（DUP の分） */
        cg_expr(cg, n->a);
        cg_op(cg, OP_DUP);
        cg_op(cg, OP_NULL_T);
        cg_op(cg, OP_EQ);
        u32 s_b = cg_jmp_op(cg, OP_JMPT);
        u32 s_end = cg_jmp_op(cg, OP_JMP);
        cg_patch_u32(cg, s_b, cg_target_here(cg));
        cg_op(cg, OP_POP); /* a を捨てる */
        cg_expr(cg, n->b);
        cg_patch_u32(cg, s_end, cg_target_here(cg));
        break;
    }
    case N_LOGASSIGN: {
        /* a ||= b / a &&= b / a ??= b（target は N_IDENT | N_PGET | N_INDEX）。
         * 展開: LOAD a; DUP; 判定ジャンプ Lend; POP; rhs; DUP; store; Lend:
         * 判定: ||= は JMPT（truthy なら a のまま end）、&&= は JMPF（falsy なら a のまま）、
         *       ??= は DUP;NULL_T;EQ;JMPF（null でない = EQ false なら a のまま end。
         *       null なら EQ true で続行 → b を代入）。
         * JMPX は TOS を pop するため end には a のコピーが残る。代入経路は POP で
         * a を捨て rhs → DUP → store → 式の値 = b。
         * obj/index は一時ローカルに退避（評価 1 回・ゴミを残さない）。 */
        AklNode *tn = &cg->p->nodes[n->a];
        u8 mode = n->op;
        if (tn->kind == N_IDENT) {
            cg_load(cg, tn->a);
            cg_op(cg, OP_DUP);
            u32 s_end;
            if (mode == 0) s_end = cg_jmp_op(cg, OP_JMPT);
            else if (mode == 1) s_end = cg_jmp_op(cg, OP_JMPF);
            else {
                cg_op(cg, OP_NULL_T);
                cg_op(cg, OP_EQ);
                s_end = cg_jmp_op(cg, OP_JMPF);
            }
            cg_op(cg, OP_POP);
            cg_expr(cg, n->b);
            cg_op(cg, OP_DUP);
            if (!cg->fail) cg_store(cg, tn->a, 0, false);
            cg_patch_u32(cg, s_end, cg_target_here(cg));
            break;
        }
        if (tn->kind == N_PGET) {
            u32 on = tn->a;
            u32 nm = tn->b;
            i32 t = cg_local_add(cg, 0xFFFFFFFEu - (u32)cg->tmp_seq++, 0);
            if (t < 0) break;
            cg_expr(cg, on);
            cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)t);
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t);
            cg_op(cg, OP_PLOAD); cg_u32(cg, nm);
            cg_op(cg, OP_DUP);
            u32 s_end;
            if (mode == 0) s_end = cg_jmp_op(cg, OP_JMPT);
            else if (mode == 1) s_end = cg_jmp_op(cg, OP_JMPF);
            else {
                cg_op(cg, OP_NULL_T);
                cg_op(cg, OP_EQ);
                s_end = cg_jmp_op(cg, OP_JMPF);
            }
            cg_op(cg, OP_POP); /* 元値を捨てる */
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t);
            cg_expr(cg, n->b);
            cg_op(cg, OP_PSTORE); cg_u32(cg, nm); /* [obj, b] → PSTORE が b を push */
            cg_patch_u32(cg, s_end, cg_target_here(cg));
            break;
        }
        /* N_INDEX */
        {
            u32 on = tn->a;
            u32 ix = tn->b;
            i32 t = cg_local_add(cg, 0xFFFFFFFEu - (u32)cg->tmp_seq++, 0);
            i32 u = cg_local_add(cg, 0xFFFFFFFEu - (u32)cg->tmp_seq++, 0);
            if (t < 0 || u < 0) break;
            cg_expr(cg, on);
            cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)t);
            cg_expr(cg, ix);
            cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)u);
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t);
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)u);
            cg_op(cg, OP_AGET);
            cg_op(cg, OP_DUP);
            u32 s_end;
            if (mode == 0) s_end = cg_jmp_op(cg, OP_JMPT);
            else if (mode == 1) s_end = cg_jmp_op(cg, OP_JMPF);
            else {
                cg_op(cg, OP_NULL_T);
                cg_op(cg, OP_EQ);
                s_end = cg_jmp_op(cg, OP_JMPF);
            }
            cg_op(cg, OP_POP); /* 元値を捨てる */
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t);
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)u);
            cg_expr(cg, n->b);
            cg_op(cg, OP_ASET); /* [obj, idx, b] → ASET が b を push */
            cg_patch_u32(cg, s_end, cg_target_here(cg));
            break;
        }
        break;
    }
    case N_DELETE: {
        u32 t = n->a;
        u8 tk = cg->p->nodes[t].kind;
        if (tk == N_PGET) {
            cg_expr(cg, cg->p->nodes[t].a);
            cg_op(cg, OP_PDEL); cg_u32(cg, cg->p->nodes[t].b);
        } else if (tk == N_INDEX) {
            cg_expr(cg, cg->p->nodes[t].a);
            cg_expr(cg, cg->p->nodes[t].b);
            cg_op(cg, OP_IDEL);
        } else if (tk == N_IDENT) {
            /* delete 変数: 非 strict では true。無害化 */
            cg_op(cg, OP_TRUE_T);
        } else {
            cg_expr(cg, t);
            cg_op(cg, OP_POP);
            cg_op(cg, OP_TRUE_T);
        }
        break;
    }
    case N_IN:
        cg_expr(cg, n->a);
        cg_expr(cg, n->b);
        cg_op(cg, OP_IN);
        break;
    case N_NEW:
        cg_expr(cg, n->a);
        for (u32 i = 0; i < n->c; i++) cg_expr(cg, cg->p->list[n->b + i]);
        cg_op(cg, OP_NEW);
        cg_push_byte(cg, (u8)(n->c & 0xFF));
        break;
    case N_ARRLIT: {
        /* spread 要素があれば push 方式（OP_ARRPUSH/ARRPUSHALL）、なければ一括 ANEW */
        bool has_spread = false;
        for (u32 i = 0; i < n->c; i++)
            if (cg->p->nodes[cg->p->list[n->b + i]].kind == N_SPREAD) { has_spread = true; break; }
        if (has_spread) {
            cg_op(cg, OP_ANEW);
            cg_u32(cg, 0);
            for (u32 i = 0; i < n->c; i++) {
                u32 el = cg->p->list[n->b + i];
                if (cg->p->nodes[el].kind == N_SPREAD) {
                    cg_op(cg, OP_DUP);
                    cg_expr(cg, cg->p->nodes[el].a);
                    cg_op(cg, OP_ARRPUSHALL);
                } else {
                    cg_op(cg, OP_DUP);
                    cg_expr(cg, el);
                    cg_op(cg, OP_ARRPUSH);
                }
                if (cg->fail) return;
            }
        } else {
            for (u32 i = 0; i < n->c; i++) {
                cg_expr(cg, cg->p->list[n->b + i]);
                if (cg->fail) return;
            }
            cg_op(cg, OP_ANEW);
            cg_u32(cg, n->c);
        }
        break;
    }
    case N_INDEX:
        cg_expr(cg, n->a);
        cg_expr(cg, n->b);
        cg_op(cg, OP_AGET);
        break;
    case N_INDEXSET:
        cg_expr(cg, n->a);
        cg_expr(cg, n->b);
        cg_expr(cg, n->c);
        cg_op(cg, OP_ASET); /* pop val, idx, arr → push val */
        break;
    case N_TERN: {
        cg_expr(cg, n->a); /* cond を先に評価（JMPF が pop する） */
        u32 s_else = cg_jmp_op(cg, OP_JMPF);
        cg_expr(cg, n->b);
        u32 s_end = cg_jmp_op(cg, OP_JMP);
        cg_patch_u32(cg, s_else, cg_target_here(cg));
        cg_expr(cg, n->c);
        cg_patch_u32(cg, s_end, cg_target_here(cg));
        break;
    }
    case N_ASSIGNOP: {
        /* ターゲット種別ごとに評価順を固定（冒頭の無条件評価は二重評価になるため禁止） */
        AklNode *tn = &cg->p->nodes[n->a];
        if (tn->kind == N_IDENT) {
            /* lhs 値 → rhs → op → DUP → store（式の値 = 新値。capture/global も
             * cg_load/cg_store が正しく処理） */
            cg_load(cg, tn->a);
            cg_expr(cg, n->b);
            cg_op(cg, n->op);
            cg_op(cg, OP_DUP);
            if (!cg->fail) cg_store(cg, tn->a, 0, false);
            break;
        }
        if (tn->kind == N_PGET) {
            /* obj; DUP; PLOAD; rhs; op; PSTORE → [new]（obj は DUP で保持、副作用 1 回） */
            u32 on = tn->a;
            u32 nm = tn->b;
            cg_expr(cg, on);
            cg_op(cg, OP_DUP);
            cg_op(cg, OP_PLOAD); cg_u32(cg, nm);
            cg_expr(cg, n->b);
            cg_op(cg, n->op);
            cg_op(cg, OP_PSTORE); cg_u32(cg, nm);
            break;
        }
        if (tn->kind == N_INDEX) {
            /* 一時ローカル T,U,W 使用: T=arr; U=idx; W=新値。
             * ASET は [arr, idx, val]（val が TOS）を要求するため、計算値を W に
             * 退避してから LOAD T; LOAD U; LOAD W; ASET の順に積む。 */
            u32 on = tn->a;
            u32 ix = tn->b;
            i32 t = cg_local_add(cg, 0xFFFFFFFEu - (u32)cg->tmp_seq++, 0);
            i32 u = cg_local_add(cg, 0xFFFFFFFEu - (u32)cg->tmp_seq++, 0);
            i32 w = cg_local_add(cg, 0xFFFFFFFEu - (u32)cg->tmp_seq++, 0);
            if (t < 0 || u < 0 || w < 0) break;
            cg_expr(cg, on);
            cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)t);
            cg_expr(cg, ix);
            cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)u);
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t);
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)u);
            cg_op(cg, OP_AGET);
            cg_expr(cg, n->b);
            cg_op(cg, n->op);
            cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)w);
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t);
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)u);
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)w);
            cg_op(cg, OP_ASET);
            break;
        }
        akl_errf(cg->rt, "internal: bad assignop target");
        cg->fail = true;
        break;
    }
    case N_INCDEC: {
        /* 前置/後置 ++/--。n->op: 0=++, 1=--。flags bit0=後置 */
        u32 target = n->a;
        bool is_post = (n->flags & 1) != 0;
        u8 addop = n->op == 0 ? OP_ADD : OP_SUB;
        AklNode *tn = &cg->p->nodes[target];
        if (tn->kind == N_IDENT) {
            /* cg_load/cg_store が local/captured/global を正しく処理する汎用経路:
             *  後置: LOAD; DUP; PUSH1; ADD/SUB; STORE → [old]
             *  前置: LOAD; PUSH1; ADD/SUB; DUP; STORE → [new] */
            cg_load(cg, tn->a);
            if (is_post) cg_op(cg, OP_DUP);
            cg_op(cg, OP_CONST_I); cg_u32(cg, 1);
            cg_op(cg, addop);
            if (!is_post) cg_op(cg, OP_DUP);
            if (!cg->fail) cg_store(cg, tn->a, 0, false);
            break;
        }
        if (tn->kind == N_PGET) {
            /* T=obj, U=値退避: LOAD T; PLOAD → old; ±1 → new; STORE U;
             * LOAD T; LOAD U; PSTORE → [new]（post は [old, new] → POP で [old] を残す） */
            u32 on = tn->a, nm = tn->b;
            i32 t = cg_local_add(cg, 0xFFFFFFFEu - (u32)cg->tmp_seq++, 0);
            i32 u = cg_local_add(cg, 0xFFFFFFFEu - (u32)cg->tmp_seq++, 0);
            if (t < 0 || u < 0) break;
            cg_expr(cg, on);
            cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)t);
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t);
            cg_op(cg, OP_PLOAD); cg_u32(cg, nm); /* [old] */
            if (is_post) cg_op(cg, OP_DUP);
            cg_op(cg, OP_CONST_I); cg_u32(cg, 1);
            cg_op(cg, addop); /* [new] / [old, new] */
            cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)u); /* [old] / [] */
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t);
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)u);
            cg_op(cg, OP_PSTORE); cg_u32(cg, nm); /* pop val, obj → push val=[new] */
            if (is_post) cg_op(cg, OP_POP); /* [old, new] → [old] */
            break;
        }
        if (tn->kind == N_INDEX) {
            u32 on = tn->a, ix = tn->b;
            i32 t = cg_local_add(cg, 0xFFFFFFFEu - (u32)cg->tmp_seq++, 0);
            i32 u = cg_local_add(cg, 0xFFFFFFFEu - (u32)cg->tmp_seq++, 0);
            i32 w = cg_local_add(cg, 0xFFFFFFFEu - (u32)cg->tmp_seq++, 0);
            if (t < 0 || u < 0 || w < 0) break;
            cg_expr(cg, on);
            cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)t);
            cg_expr(cg, ix);
            cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)u);
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t);
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)u);
            cg_op(cg, OP_AGET); /* [old] */
            if (is_post) cg_op(cg, OP_DUP);
            cg_op(cg, OP_CONST_I); cg_u32(cg, 1);
            cg_op(cg, addop);   /* [new] / [old, new] */
            if (!is_post) cg_op(cg, OP_DUP);
            cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)u); /* u はもう index でない: 値退避に再利用 */
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t);
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)w);
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)u);
            cg_op(cg, OP_ASET); /* pop val, idx, arr → push val */
            if (is_post) cg_op(cg, OP_POP); /* 余分な new を捨て [old] を残す */
            break;
        }
        akl_errf(cg->rt, "internal: bad incdec target");
        cg->fail = true;
        break;
    }
    default:
        akl_errf(cg->rt, "internal: bad expr node %u", n->kind);
        cg->fail = true;
        break;
    }
}

static void cg_fn(Cg *cg, u32 ni) {
    AklNode *n = &cg->p->nodes[ni];
        /* 関数エントリを作成して本体を別領域に出力し、宣言点は JMP で飛ばす。
         * 関数式（N_FUNCEXPR）は名前が無ければ「値を残す」だけ（束縛しない）。
         * スコープ: 現在 scope を outer へ退避し、新 scope を開設。
         * 隠し slot（this / 自前 ENV / cap ENV）はダミー LocalEnt（name=UINT32_MAX）
         * として locals 先頭に予約する — 全 slot 番号がそのまま実フレーム位置になり、
         * 既存の slot 発行コードを一切触らずに済む設計。 */
        AklRT *rt = cg->rt;
        if (rt->n_funcs == rt->cap_funcs) {
            u32 nc = rt->cap_funcs ? rt->cap_funcs * 2 : 32;
            AklFuncEnt *nf = (AklFuncEnt *)realloc(rt->funcs, (u64)nc * sizeof(AklFuncEnt));
            if (!nf) { akl_errf(rt, "oom: funcs"); cg->fail = true; return; }
            rt->funcs = nf; rt->cap_funcs = nc;
        }
        if (n->c > 255) { akl_errf(rt, "too many parameters"); cg->fail = true; return; }
        bool is_expr = n->kind == N_FUNCEXPR || n->kind == N_CLASSMETH;
        u32 fidx = rt->n_funcs++;
        u32 site = cg_jmp_op(cg, OP_JMP); /* 本体を飛ばす */
        rt->funcs[fidx].code_off = cg_target_here(cg);
        rt->funcs[fidx].name = n->a;
        rt->funcs[fidx].n_params = (u16)n->c;
        /* 新しい codegen スコープ（解析結果 fi を参照。無ければ capture なし） */
        const AklFnInfo *fi = (cg->p->fninfo && ni < cg->p->fninfo_n) ? &cg->p->fninfo[ni] : NULL;
        if (cg->n_outer >= AKL_MAX_DEPTH) { akl_errf(rt, "function nesting budget exhausted"); cg->fail = true; return; }
        cg->outer[cg->n_outer].locals = cg->locals;
        cg->outer[cg->n_outer].n_locals = cg->n_locals;
        cg->outer[cg->n_outer].cap_locals = cg->cap_locals;
        cg->outer[cg->n_outer].n_env = cg->cur_n_env;
        cg->outer[cg->n_outer].needs_cap = cg->cur_needs_cap;
        cg->outer[cg->n_outer].fn_idx = cg->cur_fn;
        cg->outer[cg->n_outer].fi = cg->cur_fi;
        cg->n_outer++;
        cg->locals = NULL; cg->n_locals = 0; cg->cap_locals = 0;
        cg->cur_n_env = fi ? fi->n_cap : 0;
        cg->cur_needs_cap = fi ? fi->needs_cap : 0;
        cg->cur_fi = fi;
        cg->cur_fn = (i32)fidx;
        cg->in_func_depth++;
        /* 隠し slot のダミー予約（this は常に 1、自前 ENV / cap ENV は必要時のみ） */
        u32 nh = cg_hidden(cg);
        for (u32 i = 0; i < nh; i++) {
            if (cg_local_add_dummy(cg) < 0) { cg->fail = true; return; }
        }
        if (!cg->fail) {
            for (u32 i = 0; i < n->c; i++) {
                u32 pe = cg->p->list[n->b + i];
                u8 pk = pe < cg->p->n_nodes ? cg->p->nodes[pe].kind : 0xFF;
                u32 name = (pk == N_DEFPARAM || pk == N_RESTPARAM) ? cg->p->nodes[pe].a : pe;
                i32 s = cg_local_add(cg, name, 0);
                if (s < 0) return;
            }
        }
        /* デフォルト引数の評価（本体の前・順次。undefined の場合のみ代入） */
        if (!cg->fail) {
            for (u32 i = 0; i < n->c; i++) {
                u32 pe = cg->p->list[n->b + i];
                if (pe >= cg->p->n_nodes) continue;
                if (cg->p->nodes[pe].kind != N_DEFPARAM) continue;
                u32 name = cg->p->nodes[pe].a;
                i32 slot = cg_local_find(cg, name);
                if (slot < 0) continue;
                cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)slot);
                cg_op(cg, OP_DUP);
                cg_op(cg, OP_UNDEF_T);
                cg_op(cg, OP_SEQ);
                u32 s_skip = cg_jmp_op(cg, OP_JMPF);
                cg_op(cg, OP_POP);
                cg_expr(cg, cg->p->nodes[pe].b);
                cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)slot);
                cg_patch_u32(cg, s_skip, cg_target_here(cg));
            }
        }
        if (!cg->fail) cg_stmt(cg, n->d);
        cg_op(cg, OP_UNDEF_T);
        cg_op(cg, OP_RET);
        rt->funcs[fidx].n_locals = (u16)cg->n_locals;
        rt->funcs[fidx].n_env = (u16)cg->cur_n_env;
        rt->funcs[fidx].n_cap = (u16)cg->cur_needs_cap;
        rt->funcs[fidx].code_end = cg_target_here(cg);
        free(cg->locals);
        cg->n_outer--;
        cg->locals = cg->outer[cg->n_outer].locals;
        cg->n_locals = cg->outer[cg->n_outer].n_locals;
        cg->cap_locals = cg->outer[cg->n_outer].cap_locals;
        cg->cur_n_env = cg->outer[cg->n_outer].n_env;
        cg->cur_needs_cap = cg->outer[cg->n_outer].needs_cap;
        cg->cur_fi = cg->outer[cg->n_outer].fi;
        cg->cur_fn = cg->outer[cg->n_outer].fn_idx;
        cg->in_func_depth--;
        if (cg->fail) return;
        cg_patch_u32(cg, site, cg_target_here(cg));
        /* MAKEF / MAKEFS: 捕捉元 env slot（現在関数の env チェーン先頭 = 自前 ENV or cap ENV）。
         * MAKEFS は N_CLASSMETH で super_pending のとき（スタックに親クラスがある）。
         * ネスト関数（N_FUNC）の MAKEF では super_pending を消費しない。 */
        u8 srcslot = (u8)(cg->cur_n_env ? 1 : (cg->cur_needs_cap ? 1 : 0));
        if (cg->super_pending && n->kind == N_CLASSMETH) {
            cg_op(cg, OP_MAKEFS);
            cg->super_pending = false;
        } else {
            cg_op(cg, OP_MAKEF);
        }
        cg_u32(cg, fidx);
        cg_push_byte(cg, srcslot);
        if (is_expr) {
            /* 関数式: 名前付きなら生成元スコープへ束縛（本体からの自己参照が
             * capture/global 解決で見えるようにする）。DUP で値を残す。無名は値のみ。
             * クラスメソッド（N_CLASSMETH）は値のみ（束縛はクラスオブジェクト側が行う）。 */
            if (n->kind == N_FUNCEXPR && n->a != UINT32_MAX && !cg->fail) {
                cg_op(cg, OP_DUP);
                cg_store(cg, n->a, 0, true);
            }
            return;
        }
        /* 宣言 = 束縛（main はグローバル、関数内はローカル） */
        if (cg->in_func_depth == 0) {
            u32 gi = cg_global_add(rt, n->a, 0);
            if (gi == UINT32_MAX) { cg->fail = true; return; }
            cg_op(cg, OP_GSTORE); cg_u32(cg, n->a);
        } else {
            i32 s2 = cg_local_add(cg, n->a, 0);
            if (s2 < 0) return;
            cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)s2);
        }
        }

static void cg_stmt(Cg *cg, u32 ni) {
    if (cg->fail || ni == N_NONE) return; /* N_NONE = 空本体（合成コンストラクタ等） */
    AklNode *n = &cg->p->nodes[ni];
    switch (n->kind) {
    case N_BLOCK:
        if (n->a == N_NONE) break; /* 空文 */
        for (u32 i = 0; i < n->c; i++) cg_stmt(cg, cg->p->list[n->a + i]);
        break;
    case N_EXPRSTMT: {
        /* 文脈限定の LINC 融合: `x = x + 定数int;` / `x = x - 定数int;` を 1 命令化する。
         * 式文としての最終値破棄は VM 側が last_val を更新するので意味差なし。
         * rhs の変数が左辺と同一スロットに解決されるときのみ（別名・グローバルは対象外）。 */
        AklNode *e = &cg->p->nodes[n->a];
        bool fused = false;
        /* capture 済みターゲットへの代入は全て汎用経路（ENV 経由の cg_store が正しく処理）。
         * N_ASSIGN の a は name（u32）、N_ASSIGNOP/N_INCDEC の a はターゲットノード。 */
        bool tgt_cap = false;
        if (e->kind == N_ASSIGN) tgt_cap = cg_captured(cg, e->a, false, NULL);
        else if (e->kind == N_ASSIGNOP || e->kind == N_INCDEC) tgt_cap = cg_tgt_is_captured(cg, e->a);
        if (!tgt_cap && e->kind == N_ASSIGN && e->b != N_NONE) {
            AklNode *rhs = &cg->p->nodes[e->b];
            if (rhs->kind == N_BIN && (rhs->op == OP_ADD || rhs->op == OP_SUB) &&
                rhs->a != N_NONE && rhs->b != N_NONE) {
                AklNode *L = &cg->p->nodes[rhs->a], *R = &cg->p->nodes[rhs->b];
                if (L->kind == N_IDENT && L->a == e->a && R->kind == N_NUM && R->op == 1) {
                    i64 dd = (i64)(i32)R->a;
                    if (rhs->op == OP_SUB) dd = -dd;
                    if (dd >= -2147483648ll && dd <= 2147483647ll) {
                        i32 slot = cg->in_func_depth != 0 ? cg_local_find(cg, e->a) : -1;
                        if (cg_local_plain(cg, slot)) {
                            cg_op(cg, OP_LINC);
                            cg_u32(cg, (u32)slot);
                            cg_u32(cg, (u32)(i32)dd);
                            fused = true;
                        } else if (cg->in_func_depth == 0) {
                            /* トップレベル: cg_global_add で登録が保証される直結 GINC。
                             * const グローバルへの代入は融合しない（汎用経路で正しく例外化） */
                            u32 gi = cg_global_find(cg->rt, e->a);
                            if (gi != UINT32_MAX && !cg->rt->globals[gi].is_const) {
                                cg_op(cg, OP_GINC);
                                cg_u32(cg, gi);
                                cg_u32(cg, (u32)(i32)dd);
                                fused = true;
                            }
                        }
                    }
                }
            }
            /* (2) `x = x + y;` グローバル同士の融合。意味は rhs; DUP; GSTORE_S; POPV と
             * 同一（スタック中立・last_val 更新込み）。左辺と rhs 左が同一グローバルのみ。 */
            if (!fused && rhs->kind == N_BIN && rhs->op == OP_ADD && rhs->a != N_NONE && rhs->b != N_NONE &&
                cg->in_func_depth == 0) {
                AklNode *L = &cg->p->nodes[rhs->a], *R = &cg->p->nodes[rhs->b];
                if (L->kind == N_IDENT && L->a == e->a && R->kind == N_IDENT) {
                    u32 gd = cg_global_find(cg->rt, e->a);
                    u32 gs = cg_global_find(cg->rt, R->a);
                    if (gd != UINT32_MAX && gs != UINT32_MAX && !cg->rt->globals[gd].is_const) {
                        cg_op(cg, OP_GADD_G);
                        cg_u32(cg, gd);
                        cg_u32(cg, gs);
                        fused = true;
                    }
                }
            }
        }
        /* (3.5) v0.3: `x += int定数;`（N_ASSIGNOP）と `x++; x--;`（N_INCDEC）の LINC/GINC 融合。
         * 意味は N_ASSIGN 融合 (1) と同一（last_val 更新込みのスタック中立命令） */
        if (!fused && !tgt_cap && e->kind == N_ASSIGNOP && e->b != N_NONE &&
            cg->p->nodes[e->a].kind == N_IDENT && (e->op == OP_ADD || e->op == OP_SUB)) {
            AklNode *rhs3 = &cg->p->nodes[e->b];
            if (rhs3->kind == N_NUM && rhs3->op == 1) {
                i64 dd = (i64)(i32)rhs3->a;
                if (e->op == OP_SUB) dd = -dd;
                if (dd >= -2147483648ll && dd <= 2147483647ll) {
                    i32 slot = cg->in_func_depth != 0 ? cg_local_find(cg, cg->p->nodes[e->a].a) : -1;
                    if (cg_local_plain(cg, slot)) {
                        cg_op(cg, OP_LINC);
                        cg_u32(cg, (u32)slot);
                        cg_u32(cg, (u32)(i32)dd);
                        fused = true;
                    } else if (cg->in_func_depth == 0) {
                        u32 gi = cg_global_find(cg->rt, cg->p->nodes[e->a].a);
                        if (gi != UINT32_MAX && !cg->rt->globals[gi].is_const) {
                            cg_op(cg, OP_GINC);
                            cg_u32(cg, gi);
                            cg_u32(cg, (u32)(i32)dd);
                            fused = true;
                        }
                    }
                }
            }
        }
        if (!fused && !tgt_cap && e->kind == N_INCDEC && cg->p->nodes[e->a].kind == N_IDENT) {
            i64 dd = e->op == 0 ? 1 : -1;
            i32 slot = cg->in_func_depth != 0 ? cg_local_find(cg, cg->p->nodes[e->a].a) : -1;
            if (cg_local_plain(cg, slot)) {
                cg_op(cg, OP_LINC);
                cg_u32(cg, (u32)slot);
                cg_u32(cg, (u32)(i32)dd);
                fused = true;
            } else if (cg->in_func_depth == 0) {
                u32 gi = cg_global_find(cg->rt, cg->p->nodes[e->a].a);
                if (gi != UINT32_MAX && !cg->rt->globals[gi].is_const) {
                    cg_op(cg, OP_GINC);
                    cg_u32(cg, gi);
                    cg_u32(cg, (u32)(i32)dd);
                    fused = true;
                }
            }
        }
        if (!fused && !tgt_cap && e->kind == N_ASSIGN && e->b != N_NONE) {
            /* (2.5) `dst = expr op int定数;` を dst 直結の *CI.G/L 1 命令化。
             * （cg_expr(lhs); *CI; STORE_PV の 3 命令を再融合。計算共有で意味同一） */
            AklNode *rhs2 = &cg->p->nodes[e->b];
            if (rhs2->kind == N_BIN && (rhs2->op == OP_ADD || rhs2->op == OP_SUB ||
                rhs2->op == OP_MUL || rhs2->op == OP_MOD) &&
                rhs2->a != N_NONE && rhs2->b != N_NONE) {
                AklNode *R2 = &cg->p->nodes[rhs2->b];
                if (R2->kind == N_NUM && R2->op == 1) {
                    u32 imm = R2->a;
                    i32 slot = cg->in_func_depth != 0 ? cg_local_find(cg, e->a) : -1;
                    if (cg_local_plain(cg, slot)) {
                        if (!cg->locals[slot].is_const) {
                            cg_expr(cg, rhs2->a);
                            cg_op(cg, rhs2->op == OP_ADD ? OP_ADDCI_L : rhs2->op == OP_SUB ? OP_SUBCI_L :
                                    rhs2->op == OP_MUL ? OP_MULCI_L : OP_MODCI_L);
                            cg_u32(cg, (u32)slot);
                            cg_u32(cg, imm);
                            fused = true;
                        }
                    } else if (cg->in_func_depth == 0) {
                        u32 gi = cg_global_find(cg->rt, e->a);
                        if (gi != UINT32_MAX && !cg->rt->globals[gi].is_const) {
                            cg_expr(cg, rhs2->a);
                            cg_op(cg, rhs2->op == OP_ADD ? OP_ADDCI_G : rhs2->op == OP_SUB ? OP_SUBCI_G :
                                    rhs2->op == OP_MUL ? OP_MULCI_G : OP_MODCI_G);
                            cg_u32(cg, gi);
                            cg_u32(cg, imm);
                            fused = true;
                        }
                    }
                } else if (R2->kind == N_IDENT) {
                    /* rhs が変数スロットの *CI-st 対（3 アドレス形 GX/LX）。意味は
                     * 「lhs; LOAD src; OP; STORE_PV dst」逐語。dst/src の種別が同じときのみ
                     * （混在形は汎用経路に倒す。命令爆発を抑える設計判断） */
                    i32 dslot = cg->in_func_depth != 0 ? cg_local_find(cg, e->a) : -1;
                    if (cg_local_plain(cg, dslot) && !cg->locals[dslot].is_const) {
                        i32 sslot = cg_local_find(cg, R2->a);
                        if (cg_local_plain(cg, sslot)) {
                            cg_expr(cg, rhs2->a);
                            cg_op(cg, rhs2->op == OP_ADD ? OP_ADD_LX : rhs2->op == OP_SUB ? OP_SUB_LX :
                                    rhs2->op == OP_MUL ? OP_MUL_LX : OP_MOD_LX);
                            cg_u32(cg, (u32)dslot);
                            cg_u32(cg, (u32)sslot);
                            fused = true;
                        }
                    } else if (cg->in_func_depth == 0) {
                        u32 gd = cg_global_find(cg->rt, e->a);
                        u32 gs = cg_global_find(cg->rt, R2->a);
                        if (gd != UINT32_MAX && gs != UINT32_MAX && !cg->rt->globals[gd].is_const) {
                            cg_expr(cg, rhs2->a);
                            cg_op(cg, rhs2->op == OP_ADD ? OP_ADD_GX : rhs2->op == OP_SUB ? OP_SUB_GX :
                                    rhs2->op == OP_MUL ? OP_MUL_GX : OP_MOD_GX);
                            cg_u32(cg, gd);
                            cg_u32(cg, gs);
                            fused = true;
                        }
                    }
                }
            }
        }
        if (!fused && !tgt_cap && e->kind == N_ASSIGN && e->b != N_NONE) {
            /* (3) 文レベル代入: rhs; STORE_PV の 2 命令化。rhs; DUP; STORE; POPV と効果同一。
             * const 検査は cg_store の経路と同じメッセージで compile 時に行う。 */
            cg_expr(cg, e->b);
            if (!cg->fail) {
                if (cg->in_func_depth != 0) {
                    i32 slot = cg_local_find(cg, e->a);
                    if (slot >= 0 && cg->locals[slot].captured) {
                        /* capture 済みは ENV 経由（rhs; DUP; ESTORE; POPV = 値保持 + last_val 同期） */
                        if (cg->locals[slot].is_const) { akl_errf(cg->rt, "assignment to const local"); cg->fail = true; }
                        else {
                            cg_op(cg, OP_DUP);
                            cg_op(cg, OP_ESTORE); cg_u32(cg, cg->locals[slot].env_idx);
                            cg_op(cg, OP_POPV);
                        }
                    } else if (slot >= 0) {
                        if (cg->locals[slot].is_const) { akl_errf(cg->rt, "assignment to const local"); cg->fail = true; }
                        else { cg_op(cg, OP_LSTORE_PV); cg_u32(cg, (u32)slot); }
                    } else {
                        u32 gi = cg_global_add(cg->rt, e->a, 0);
                        if (gi == UINT32_MAX) cg->fail = true;
                        else if (cg->rt->globals[gi].is_const) { akl_errf(cg->rt, "assignment to const global"); cg->fail = true; }
                        else { cg_op(cg, OP_GSTORE_SPV); cg_u32(cg, gi); }
                    }
                } else {
                    u32 gi = cg_global_add(cg->rt, e->a, 0);
                    if (gi == UINT32_MAX) cg->fail = true;
                    else if (cg->rt->globals[gi].is_const) { akl_errf(cg->rt, "assignment to const global"); cg->fail = true; }
                    else { cg_op(cg, OP_GSTORE_SPV); cg_u32(cg, gi); }
                }
                fused = true; /* fail 時も cg->fail が後続を止めるので二重出力は起きない */
            }
        }
        if (!fused) {
            cg_expr(cg, n->a);
            if (!cg->fail) cg_op(cg, OP_POPV);
        }
        break;
    }
    case N_DESTR:
        cg_expr(cg, ni); /* 分割代入（右辺評価 + 要素 store） */
        cg_op(cg, OP_POP); /* 式文として値は捨てる */
        break;
    case N_CLASS: {
        /* class Name { constructor(...){} m(){} static s(){} }
         * → OBJNEW; (DUP; cg_fn(member); PSTORE name; POP)* ; クラス名に束縛
         * constructor メンバーは "constructor" キーで登録（OP_NEW が参照）。
         * extends がある場合: 親を SUPER_NAME ローカルに保存し（メソッド関数が
         * capture して super 解決に使う）、クラスオブジェクトには __super
         * プロパティとして保持（OP_NEW のメソッド継承コピー用）。 */
        u32 cname = p_intern(cg->p, (const u8 *)"constructor", 11);
        i32 pslot = -1;
        if (n->d != N_NONE) {
            /* 親クラスを一時ローカルに保持（メソッド生成のたびに push して MAKEFS へ） */
            cg_expr(cg, n->d);
            pslot = cg_local_add_dummy(cg);
            if (pslot < 0) break;
            cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)pslot);
        }
        cg_op(cg, OP_OBJNEW);
        for (u32 i = 0; i < n->c; i++) {
            u32 m = cg->p->list[n->b + i];
            u32 mname = cg->p->nodes[m].a;
            bool is_ctor = (cg->p->nodes[m].flags & 2) != 0;
            cg_op(cg, OP_DUP);
            if (pslot >= 0) {
                cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)pslot);
                cg->super_pending = true; /* このメソッドは MAKEFS（親を env に） */
            }
            cg_fn(cg, m); /* 関数値 push（値のみ） */
            cg_op(cg, OP_PSTORE);
            cg_u32(cg, is_ctor ? cname : mname);
            cg_op(cg, OP_POP);
            if (cg->fail) break;
        }
        /* クラスオブジェクトに親を __super プロパティで保存（OP_NEW の継承コピー用） */
        if (pslot >= 0 && !cg->fail) {
            cg_op(cg, OP_DUP);
            cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)pslot);
            cg_op(cg, OP_PSTORE); cg_u32(cg, cg->p->super_prop);
            cg_op(cg, OP_POP);
        }
        /* クラス名に束縛（main はグローバル、関数内はローカル）。
         * cg_store は TOS（クラスオブジェクト）を消費するため OP_POP は不要
         * — 余分な POP が空 pop でスタックを壊す（実測で特定） */
        if (!cg->fail) cg_store(cg, n->a, 0, true);
        break;
    }
    case N_VAR:
        /* `var t = x + y`（全ローカル）の 1 命令化。store 規約は cg_store と同一（last_val 不変、
         * decl 経路の cg_local_add を同じ引数で呼ぶので重複登録エラー等の挙動も一致） */
        if (cg->in_func_depth != 0 && n->b != N_NONE) {
            AklNode *rhs = &cg->p->nodes[n->b];
            if (rhs->kind == N_BIN && rhs->op == OP_ADD && rhs->a != N_NONE && rhs->b != N_NONE &&
                cg->p->nodes[rhs->a].kind == N_IDENT && cg->p->nodes[rhs->b].kind == N_IDENT) {
                i32 s1 = cg_local_find(cg, cg->p->nodes[rhs->a].a);
                i32 s2 = cg_local_find(cg, cg->p->nodes[rhs->b].a);
                if (cg_local_plain(cg, s1) && cg_local_plain(cg, s2)) {
                    i32 dslot = cg_local_add(cg, n->a, n->flags);
                    if (dslot >= 0) {
                        cg_op(cg, OP_LADD_LL);
                        cg_u32(cg, (u32)dslot);
                        cg_u32(cg, (u32)s1);
                        cg_u32(cg, (u32)s2);
                        break;
                    }
                }
            }
        }
        if (n->b != N_NONE) cg_expr(cg, n->b);
        else cg_op(cg, OP_UNDEF_T);
        if (!cg->fail) cg_store(cg, n->a, n->flags, true);
        break;
    case N_IF: {
        u32 s_else = cg_cond_jmpf(cg, n->a);
        cg_stmt(cg, n->b);
        if (n->c != N_NONE) {
            u32 s_end = cg_jmp_op(cg, OP_JMP);
            cg_patch_u32(cg, s_else, cg_target_here(cg));
            cg_stmt(cg, n->c);
            cg_patch_u32(cg, s_end, cg_target_here(cg));
        } else {
            cg_patch_u32(cg, s_else, cg_target_here(cg));
        }
        break;
    }
    case N_WHILE: {
        if (cg->n_loops >= 64) { akl_errf(cg->rt, "loop nesting budget exhausted"); cg->fail = true; break; }
        u32 cond = cg_target_here(cg);
        u32 s_end = cg_cond_jmpf(cg, n->a);
        u32 li = cg->n_loops++;
        cg_lbl_bind(cg, li);
        cg->loop_kind[li] = 0;
        cg->try_at_loop[li] = (u8)cg->try_depth;
        cg->brk_head[li] = N_NONE; cg->cont_head[li] = N_NONE; cg->cont_kind[li] = cond;
        cg_stmt(cg, n->b);
        cg_op(cg, OP_JMP); cg_u32(cg, cond);
        u32 end = cg_target_here(cg);
        cg_patch_u32(cg, s_end, end);
        for (u32 s2 = cg->brk_head[li]; s2 != N_NONE;) {
            u32 nxt; memcpy(&nxt, &cg->rt->code[s2], 4);
            cg_patch_u32(cg, s2, end);
            s2 = nxt;
        }
        for (u32 s3 = cg->cont_head[li]; s3 != N_NONE;) {
            u32 nxt2; memcpy(&nxt2, &cg->rt->code[s3], 4);
            cg_patch_u32(cg, s3, (u32)cg->cont_kind[li]);
            s3 = nxt2;
        }
        cg->n_loops--;
        break;
    }
    case N_FORIN: {
        /* for (var k in obj): キー配列を作って index ループ。
         * for (var v of arr): 要素を配列として同様（obj が配列でなければ要素1個） */
        if (cg->n_loops >= 64) { akl_errf(cg->rt, "loop nesting budget exhausted"); cg->fail = true; break; }
        i32 t_arr = cg_local_add(cg, 0xFFFFFFFEu - (u32)cg->tmp_seq++, 0);
        i32 t_i = cg_local_add(cg, 0xFFFFFFFEu - (u32)cg->tmp_seq++, 0);
        if (t_arr < 0 || t_i < 0) break;
        u32 li = cg->n_loops++;
        cg_lbl_bind(cg, li);
        cg->loop_kind[li] = 0;
        cg->try_at_loop[li] = (u8)cg->try_depth;
        cg->brk_head[li] = N_NONE; cg->cont_head[li] = N_NONE; cg->cont_kind[li] = N_NONE;
        /* 対象式 → 配列化 */
        cg_expr(cg, n->b);
        if (n->flags == 0) { /* in: キー配列 */
            cg_op(cg, OP_KEYSOF);
        } else { /* of: 配列化（ARR はそのまま、文字列は index 列挙、それ以外は [v]） */
            cg_op(cg, OP_TOARR);
        }
        cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)t_arr);
        cg_op(cg, OP_CONST_I); cg_u32(cg, 0);
        cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)t_i);
        u32 cond = cg_target_here(cg);
        cg->cont_kind[li] = cond;
        /* i < arr.length */
        cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t_i);
        cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t_arr);
        cg_op(cg, OP_PLOAD); cg_u32(cg, p_intern(cg->p, (const u8 *)"length", 6));
        cg_op(cg, OP_LT);
        u32 s_end = cg_jmp_op(cg, OP_JMPF);
        /* k = arr[i] */
        cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t_arr);
        cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t_i);
        cg_op(cg, OP_AGET);
        if (!cg->fail) cg_store(cg, n->a, (u8)n->d, true);
        cg_stmt(cg, n->c);
        /* i++ */
        cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t_i);
        cg_op(cg, OP_CONST_I); cg_u32(cg, 1);
        cg_op(cg, OP_ADD);
        cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)t_i);
        cg_op(cg, OP_JMP); cg_u32(cg, cond);
        u32 end = cg_target_here(cg);
        cg_patch_u32(cg, s_end, end);
        for (u32 s2 = cg->brk_head[li]; s2 != N_NONE;) {
            u32 nxt; memcpy(&nxt, &cg->rt->code[s2], 4);
            cg_patch_u32(cg, s2, end);
            s2 = nxt;
        }
        for (u32 s3 = cg->cont_head[li]; s3 != N_NONE;) {
            u32 nxt2; memcpy(&nxt2, &cg->rt->code[s3], 4);
            cg_patch_u32(cg, s3, (u32)cg->cont_kind[li]);
            s3 = nxt2;
        }
        cg->n_loops--;
        break;
    }
    case N_DOWHILE: {
        /* do body while(cond): body を必ず 1 回実行。continue → cond 検査へ */
        if (cg->n_loops >= 64) { akl_errf(cg->rt, "loop nesting budget exhausted"); cg->fail = true; break; }
        u32 li = cg->n_loops++;
        cg_lbl_bind(cg, li);
        cg->loop_kind[li] = 0;
        cg->try_at_loop[li] = (u8)cg->try_depth;
        cg->brk_head[li] = N_NONE; cg->cont_head[li] = N_NONE; cg->cont_kind[li] = N_NONE;
        u32 body = cg_target_here(cg);
        cg_stmt(cg, n->a);
        u32 cond = cg_target_here(cg);
        cg->cont_kind[li] = cond;
        if (!cg->fail) {
            cg_expr(cg, n->b);
            cg_op(cg, OP_JMPT);
            cg_u32(cg, body);
        }
        u32 end = cg_target_here(cg);
        for (u32 s2 = cg->brk_head[li]; s2 != N_NONE;) {
            u32 nxt; memcpy(&nxt, &cg->rt->code[s2], 4);
            cg_patch_u32(cg, s2, end);
            s2 = nxt;
        }
        for (u32 s3 = cg->cont_head[li]; s3 != N_NONE;) {
            u32 nxt2; memcpy(&nxt2, &cg->rt->code[s3], 4);
            cg_patch_u32(cg, s3, (u32)cg->cont_kind[li]);
            s3 = nxt2;
        }
        cg->n_loops--;
        break;
    }
    case N_SWITCH: {
        /* switch (disc) { case e1: b1; case e2: b2; default: bd; }
         *  T=disc; (LOAD T; ei; SEQ; JMPT Li)*; JMP Ldef_or_end; L1: b1; L2: b2; ...; Ldef: bd; Lend:
         *  fallthrough は自然落下。break → Lend（loop 連鎖を switch 専用エントリで開設）。
         *  continue は loop_kind==0 の実ループまで飛ばす。 */
        if (cg->n_loops >= 64) { akl_errf(cg->rt, "loop nesting budget exhausted"); cg->fail = true; break; }
        u32 li = cg->n_loops++;
        cg_lbl_bind(cg, li);
        cg->loop_kind[li] = 1;
        cg->try_at_loop[li] = (u8)cg->try_depth;
        cg->brk_head[li] = N_NONE; cg->cont_head[li] = N_NONE; cg->cont_kind[li] = N_NONE;
        i32 t = cg_local_add(cg, 0xFFFFFFFEu - (u32)cg->tmp_seq++, 0);
        if (t < 0) break;
        cg_expr(cg, n->a);
        cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)t);
        /* case 判定列（L_i は後 patch。default は最後） */
        u32 *sites = (u32 *)malloc((n->c ? n->c : 1) * sizeof(u32));
        if (!sites) { akl_errf(cg->rt, "oom: switch"); cg->fail = true; break; }
        u32 ncases = n->c;
        for (u32 i = 0; i < ncases; i++) {
            AklNode *cs = &cg->p->nodes[cg->p->list[n->b + i]];
            if (cs->a != N_NONE) {
                cg_op(cg, OP_LLOAD); cg_u32(cg, (u32)t);
                cg_expr(cg, cs->a);
                cg_op(cg, OP_SEQ);
                sites[i] = cg_jmp_op(cg, OP_JMPT);
            } else sites[i] = N_NONE;
        }
        u32 s_default = cg_jmp_op(cg, OP_JMP); /* 不整合: 後で default/end へ */
        /* case 本体列 */
        u32 default_pc = N_NONE;
        for (u32 i = 0; i < ncases; i++) {
            AklNode *cs = &cg->p->nodes[cg->p->list[n->b + i]];
            if (cs->a == N_NONE) default_pc = cg_target_here(cg);
            else cg_patch_u32(cg, sites[i], cg_target_here(cg));
            cg_stmt(cg, cs->b);
            if (cg->fail) break;
        }
        free(sites);
        if (cg->fail) break;
        if (cg->fail) break;
        u32 end = cg_target_here(cg);
        cg_patch_u32(cg, s_default, default_pc != N_NONE ? default_pc : end);
        for (u32 s2 = cg->brk_head[li]; s2 != N_NONE;) {
            u32 nxt; memcpy(&nxt, &cg->rt->code[s2], 4);
            cg_patch_u32(cg, s2, end);
            s2 = nxt;
        }
        cg->n_loops--;
        break;
    }
    case N_FOR: {
        if (cg->n_loops >= 64) { akl_errf(cg->rt, "loop nesting budget exhausted"); cg->fail = true; break; }
        /* ループ回転 + LOOPINC 融合の適格判定:
         *   cond = `x rel int定数`（lhs が変数側の 4 rel のみ）、step = `x = x ± int定数`
         *   x は同一の非 const グローバル、トップレベルのみ。
         * 意味保持: 元の動的列は check; body; step; check; body; ...
         *           回転後は     check; body; step+check; body; step+check; ... で逐語同一。
         *           （check の比較は CJMPF_G と LOOPINC_G で同じ cmp 規約を共有する）
         * break→end / continue→step の規約は通常経路と同一。 */
        u32 rot_gi = UINT32_MAX, rot_lim = 0; i32 rot_delta = 0; u8 rot_cmp = 0;
        if (cg->in_func_depth == 0 && n->b != N_NONE && n->c != N_NONE) {
            AklNode *cn = &cg->p->nodes[n->b];
            if (cn->kind == N_BIN && (cn->op == OP_LT || cn->op == OP_LE || cn->op == OP_GT || cn->op == OP_GE) &&
                cn->a != N_NONE && cn->b != N_NONE) {
                AklNode *L = &cg->p->nodes[cn->a], *R = &cg->p->nodes[cn->b];
                if (L->kind == N_IDENT && R->kind == N_NUM && R->op == 1) {
                    AklNode *st = &cg->p->nodes[n->c];
                    if (st->kind == N_ASSIGN && st->a == L->a && st->b != N_NONE) {
                        AklNode *rhs = &cg->p->nodes[st->b];
                        if (rhs->kind == N_BIN && (rhs->op == OP_ADD || rhs->op == OP_SUB) &&
                            rhs->a != N_NONE && rhs->b != N_NONE) {
                            AklNode *sL = &cg->p->nodes[rhs->a], *sR = &cg->p->nodes[rhs->b];
                            if (sL->kind == N_IDENT && sL->a == st->a && sR->kind == N_NUM && sR->op == 1) {
                                i64 dd = (i64)(i32)sR->a;
                                if (rhs->op == OP_SUB) dd = -dd;
                                u32 gi = dd >= -2147483648ll && dd <= 2147483647ll
                                       ? cg_global_find(cg->rt, st->a) : UINT32_MAX;
                                if (gi != UINT32_MAX && !cg->rt->globals[gi].is_const) {
                                    rot_gi = gi; rot_delta = (i32)dd; rot_lim = R->a;
                                    rot_cmp = cn->op == OP_LT ? 0 : cn->op == OP_LE ? 1 : cn->op == OP_GT ? 2 : 3;
                                }
                            }
                        }
                    }
                }
            }
        }
        if (n->a != N_NONE) {
            /* 非 var 初期化は生の式ノード（parser が N_EXPRSTMT で包まない）なので式として捨てる。
             * N_BLOCK = 複数 var 宣言（for (var a=0, b=0; ...)）は文として処理 */
            u8 ak = cg->p->nodes[n->a].kind;
            if (ak == N_VAR || ak == N_BLOCK) {
                cg_stmt(cg, n->a);
            } else {
                cg_expr(cg, n->a);
                cg_op(cg, OP_POP);
            }
        }
        if (rot_gi != UINT32_MAX) {
            /* 初回 pre-check は CJMPF_G と同一命令形（比較意味を共有） */
            cg_op(cg, OP_CJMPF_G);
            cg_u32(cg, rot_gi);
            cg_u32(cg, rot_lim);
            cg_op(cg, rot_cmp);
            u32 s_pre = cg_u32(cg, 0);
            u32 body_top = cg_target_here(cg);
            u32 li = cg->n_loops++;
            cg_lbl_bind(cg, li);
            cg->loop_kind[li] = 0;
            cg->try_at_loop[li] = (u8)cg->try_depth;
            cg->brk_head[li] = N_NONE; cg->cont_head[li] = N_NONE; cg->cont_kind[li] = N_NONE;
            cg_stmt(cg, n->d);
            u32 step_addr = cg_target_here(cg);
            cg->cont_kind[li] = step_addr;
            cg_op(cg, OP_LOOPINC_G);
            cg_u32(cg, rot_gi);
            cg_u32(cg, (u32)rot_delta);
            cg_u32(cg, rot_lim);
            cg_op(cg, rot_cmp);
            cg_u32(cg, (u32)body_top);
            u32 end = cg_target_here(cg);
            cg_patch_u32(cg, s_pre, end);
            for (u32 s2 = cg->brk_head[li]; s2 != N_NONE;) {
                u32 nxt; memcpy(&nxt, &cg->rt->code[s2], 4);
                cg_patch_u32(cg, s2, end);
                s2 = nxt;
            }
            for (u32 s3 = cg->cont_head[li]; s3 != N_NONE;) {
                u32 nxt2; memcpy(&nxt2, &cg->rt->code[s3], 4);
                cg_patch_u32(cg, s3, (u32)cg->cont_kind[li]);
                s3 = nxt2;
            }
            cg->n_loops--;
            break;
        }
        u32 cond = cg_target_here(cg);
        u32 s_end = N_NONE;
        if (n->b != N_NONE) {
            s_end = cg_cond_jmpf(cg, n->b);
        }
        u32 li = cg->n_loops++;
        cg_lbl_bind(cg, li);
        cg->loop_kind[li] = 0;
        cg->try_at_loop[li] = (u8)cg->try_depth;
        cg->brk_head[li] = N_NONE; cg->cont_head[li] = N_NONE; cg->cont_kind[li] = N_NONE; /* step addr 後決め */
        cg_stmt(cg, n->d);
        u32 step_addr = cg_target_here(cg);
        cg->cont_kind[li] = step_addr;
        if (n->c != N_NONE) { cg_expr(cg, n->c); cg_op(cg, OP_POP); }
        cg_op(cg, OP_JMP); cg_u32(cg, cond);
        u32 end = cg_target_here(cg);
        if (s_end != N_NONE) cg_patch_u32(cg, s_end, end);
        for (u32 s2 = cg->brk_head[li]; s2 != N_NONE;) {
            u32 nxt; memcpy(&nxt, &cg->rt->code[s2], 4);
            cg_patch_u32(cg, s2, end);
            s2 = nxt;
        }
        for (u32 s3 = cg->cont_head[li]; s3 != N_NONE;) {
            u32 nxt2; memcpy(&nxt2, &cg->rt->code[s3], 4);
            cg_patch_u32(cg, s3, (u32)cg->cont_kind[li]);
            s3 = nxt2;
        }
        cg->n_loops--;
        break;
    }
    case N_BREAK:
        if (n->a != N_NONE) { /* break label; */
            i32 found = -1;
            for (i32 k = (i32)cg->n_lbl - 1; k >= 0; k--)
                if (cg->lbl_name[k] == n->a) { found = k; break; }
            if (found < 0) { akl_errf(cg->rt, "undefined label"); cg->fail = true; break; }
            if (cg->lbl_is_loop[found] && cg->lbl_li[found] != UINT32_MAX) {
                u32 li = cg->lbl_li[found];
                u32 site = cg_jmp_op(cg, OP_JMP);
                u32 prev = cg->brk_head[li];
                memcpy(&cg->rt->code[site], &prev, 4);
                cg->brk_head[li] = site;
            } else {
                /* 非ループラベル: N_LABEL 終端へ（N_LABEL 処理完了時に一括パッチ） */
                u32 site = cg_jmp_op(cg, OP_JMP);
                u32 prev = cg->lbl_brk_head[found];
                memcpy(&cg->rt->code[site], &prev, 4);
                cg->lbl_brk_head[found] = site;
            }
            break;
        }
        if (!cg->n_loops) { akl_errf(cg->rt, "break outside loop"); cg->fail = true; break; }
        /* try 領域を跨ぐ制御移動は finally 連鎖が必要。v0.1 は未対応で明白に拒否（台帳） */
        if ((u32)cg->try_depth != cg->try_at_loop[cg->n_loops - 1]) {
            akl_errf(cg->rt, "break across try boundary is unsupported in v0.1");
            cg->fail = true; break;
        }
        {
            u32 site = cg_jmp_op(cg, OP_JMP);
            /* 連鎖: imm に prev head を埋め、あとで一括 patch */
            u32 li = cg->n_loops - 1;
            u32 prev = cg->brk_head[li];
            memcpy(&cg->rt->code[site], &prev, 4);
            cg->brk_head[li] = site;
        }
        break;
    case N_CONTINUE:
        if (n->a != N_NONE) { /* continue label; */
            i32 found = -1;
            for (i32 k = (i32)cg->n_lbl - 1; k >= 0; k--)
                if (cg->lbl_name[k] == n->a) { found = k; break; }
            if (found < 0) { akl_errf(cg->rt, "undefined label"); cg->fail = true; break; }
            if (!cg->lbl_is_loop[found] || cg->lbl_li[found] == UINT32_MAX) {
                akl_errf(cg->rt, "continue target is not a loop");
                cg->fail = true;
                break;
            }
            {
                u32 li = cg->lbl_li[found];
                u32 site = cg_jmp_op(cg, OP_JMP);
                u32 prev = cg->cont_head[li];
                memcpy(&cg->rt->code[site], &prev, 4);
                cg->cont_head[li] = site;
            }
            break;
        }
        {
            /* switch は continue の対象でない（JS: 最も内側の実ループへ）。loop_kind==0 を探す */
            u32 li = cg->n_loops;
            while (li > 0 && cg->loop_kind[li - 1] != 0) li--;
            if (!li) { akl_errf(cg->rt, "continue outside loop"); cg->fail = true; break; }
            li--;
            if ((u32)cg->try_depth != cg->try_at_loop[li]) {
                akl_errf(cg->rt, "continue across try boundary is unsupported in v0.1");
                cg->fail = true; break;
            }
            u32 site = cg_jmp_op(cg, OP_JMP);
            u32 prev = cg->cont_head[li];
            memcpy(&cg->rt->code[site], &prev, 4);
            cg->cont_head[li] = site;
        }
        break;
    case N_LABEL: {
        /* label: stmt。ループ付きラベル（flags bit0）は次に開設されるループに
         * 紐付け（cg_lbl_bind）。非ループは break label を文終端へパッチ。 */
        if (cg->n_lbl >= 64) { akl_errf(cg->rt, "label nesting budget exhausted"); cg->fail = true; break; }
        u32 my = cg->n_lbl++;
        cg->lbl_name[my] = n->a;
        cg->lbl_li[my] = UINT32_MAX;
        cg->lbl_brk_head[my] = N_NONE;
        cg->lbl_is_loop[my] = (n->flags & 1) != 0;
        if (cg->lbl_is_loop[my]) cg->lbl_pending = my;
        cg_stmt(cg, n->b);
        if (cg->fail) break;
        /* 文終端: 非ループラベルの break をここにパッチ */
        if (cg->lbl_brk_head[my] != N_NONE) {
            u32 end = cg_target_here(cg);
            u32 s2 = cg->lbl_brk_head[my];
            while (s2 != N_NONE) {
                u32 nxt; memcpy(&nxt, &cg->rt->code[s2], 4);
                cg_patch_u32(cg, s2, end);
                s2 = nxt;
            }
        }
        cg->n_lbl--;
        cg->lbl_pending = UINT32_MAX;
        break;
    }
    case N_THROW:
        cg_expr(cg, n->a);
        cg_op(cg, OP_THROW);
        break;
    case N_TRY: {
        /* レイアウト（制御は TRY_LEAVE が常に転移するので catch/finally への落下なし）:
         *   TRY_PUSH catch_pc, finally_pc, catch_slot
         *   <try 本体>
         *   TRY_LEAVE Ldone          ; 正常完了（unwinder 未介入時のみここを通る）
         *  Lcatch: <catch 本体>      ; unwinder が束縛代入→in_catch=1 でここに来る
         *   TRY_LEAVE Ldone          ; catch 完了も finally 連鎖が要る
         *  Lfin: <finally 本体>
         *   FIN_END
         *  Ldone: 以後の文 */
        bool has_catch = n->c != N_NONE, has_fin = n->d != N_NONE;
        cg_op(cg, OP_TRY_PUSH);
        u32 p_catch = cg_u32(cg, 0);
        u32 p_fin = cg_u32(cg, 0);
        u32 cslot = AKL_SLOT_NONE;
        if (has_catch && n->b != UINT32_MAX) {
            /* 束縛は現 frame のローカルとして割付（既存名は共有。ブロックスコープ厳密分離は台帳） */
            i32 sl = cg_local_add(cg, n->b, 0);
            if (sl < 0) break;
            cslot = (u32)sl;
        }
        cg_u32(cg, cslot);
        cg->try_depth++;
        cg_stmt(cg, n->a);
        cg_op(cg, OP_TRY_LEAVE);
        u32 leave1 = cg_u32(cg, 0);
        cg->try_depth--;
        if (cg->fail) break;
        u32 catch_pc = cg_target_here(cg);
        u32 leave2 = N_NONE;
        if (has_catch) {
            cg->try_depth++; /* catch 本体内の throw でも finally は走る（entry を残す設計） */
            cg_stmt(cg, n->c);
            cg_op(cg, OP_TRY_LEAVE);
            leave2 = cg_u32(cg, 0);
            cg->try_depth--;
        }
        if (cg->fail) break;
        u32 fin_pc = cg_target_here(cg);
        if (has_fin) {
            /* finally の内側は try 領域ではない（throw は unwinder の FIN 規則で外へ） */
            cg_stmt(cg, n->d);
            cg_op(cg, OP_FIN_END);
        }
        if (cg->fail) break;
        /* finally 本体に break/continue を書くと領域外に出る JS 複雑規則になるため、
         * v0.1 では loop が try_depth==0 でない限り loop を開設できない既存ゲートで十分 */
        u32 done = cg_target_here(cg);
        cg_patch_u32(cg, p_catch, has_catch ? catch_pc : AKL_PC_NONE);
        cg_patch_u32(cg, p_fin, has_fin ? fin_pc : AKL_PC_NONE);
        cg_patch_u32(cg, leave1, done);
        if (leave2 != N_NONE) cg_patch_u32(cg, leave2, done);
        break;
    }
    case N_RET:
        if (!cg->in_func_depth) { akl_errf(cg->rt, "return outside function"); cg->fail = true; break; }
        /* return <local> の LLOAD;RET 融合 */
        if (n->a != N_NONE && cg->p->nodes[n->a].kind == N_IDENT) {
            i32 slot = cg_local_find(cg, cg->p->nodes[n->a].a);
            if (cg_local_plain(cg, slot)) {
                cg_op(cg, OP_RET_L);
                cg_u32(cg, (u32)slot);
                break;
            }
        }
        if (n->a != N_NONE) cg_expr(cg, n->a);
        else cg_op(cg, OP_UNDEF_T);
        cg_op(cg, OP_RET);
        break;
    case N_FUNC: case N_FUNCEXPR:
        cg_fn(cg, ni);
        break;
    default:
        akl_errf(cg->rt, "internal: bad stmt node %u (idx %u a=%u b=%u c=%u d=%u)", n->kind, ni, n->a, n->b, n->c, n->d);
        cg->fail = true;
        break;
    }
}

#ifdef AKL_AST_DUMP
/* -DAKL_AST_DUMP ビルド時のみ有効な診断出力（通常ビルドからは完全に消える） */
static void akl_ast_dump(P *p) {
    static const char *const NK[] = {
        "NUM","STR","BOOL","NULL","UNDEF","IDENT","UNARY","BIN","ASSIGN",
        "CALL","FUNC","VAR","EXPRSTMT","IF","WHILE","FOR","BLOCK","RET","BREAK","CONTINUE","PROG"
    };
    for (u32 i = 0; i < p->n_nodes; i++)
        fprintf(stderr, "node[%u] %-8s a=%u b=%u c=%u d=%u op=%u fl=%u\n",
                i, p->nodes[i].kind <= 20 ? NK[p->nodes[i].kind] : "??",
                p->nodes[i].a, p->nodes[i].b, p->nodes[i].c, p->nodes[i].d,
                p->nodes[i].op, p->nodes[i].flags);
    for (u32 i = 0; i < p->n_list; i++) fprintf(stderr, "list[%u]=%u\n", i, p->list[i]);
}
#endif

/* ============================== verifier ============================== */

/* linear decode: 全命令の開始位置・即値範囲・ジャンプ先・locals 参照を検査。
 * ここを通った code は VM が（dispatch を含めて）信頼してよい、が
 * VM はそれに依らず push/pop/深度を動的にも検査する（defense in depth）。 */
typedef struct { u32 pc; u8 fn; } VfyFrame;

static u32 akl_op_imm_len(u8 op) {
    switch (op) {
    case OP_CONST_I: case OP_CONST_STR:
    case OP_GLOAD: case OP_GSTORE:
    case OP_LLOAD: case OP_LSTORE:
    case OP_JMP: case OP_JMPF: case OP_JMPT:
    case OP_MULCI: case OP_ADDCI: case OP_SUBCI: case OP_MODCI:
    case OP_GADD_P: case OP_LADD_P:
    case OP_GSTORE_SPV: case OP_LSTORE_PV:
    case OP_RET_L:
    case OP_TRY_LEAVE:
    case OP_GLOAD_S: case OP_GSTORE_S:
        return 4;
    case OP_CONST_D:
    case OP_LINC: case OP_GINC:
    case OP_GMULC: case OP_LMULC:
    case OP_GADD_G:
    case OP_ADDCI_G: case OP_SUBCI_G: case OP_MULCI_G: case OP_MODCI_G:
    case OP_ADDCI_L: case OP_SUBCI_L: case OP_MULCI_L: case OP_MODCI_L:
    case OP_ADD_GX: case OP_SUB_GX: case OP_MUL_GX: case OP_MOD_GX:
    case OP_ADD_LX: case OP_SUB_LX: case OP_MUL_LX: case OP_MOD_LX:
        return 8;
    case OP_LADD_LL:
    case OP_TRY_PUSH:
        return 12;
    case OP_CJMPF_L: case OP_CJMPF_G:
        return 13;
    case OP_CJMPF_MODG: case OP_CJMPF_MODL:
    case OP_LOOPINC_G: case OP_LOOPINC_L:
    case OP_LOOPINC_GV: case OP_LOOPINC_LV:
    case OP_CJMPF_MULGG:
    case OP_CJMPF_MODGG:
        return 17;
    case OP_CALL:
    case OP_NEW:
    case OP_CALLT:
        return 1;
    case OP_NEWREGEX:
        return 8;
    case OP_PLOAD: case OP_PSTORE:
        return 4;
    case OP_MCALL:
        return 5;
    case OP_MAKEF:
    case OP_MAKEFS:
        return 5;   /* fidx u32 | srcslot u8 */
    case OP_SUPERGET:
        return 4;
    case OP_MCALLN:
        return 8;   /* slot u32 | name u32 */
    case OP_PSETDYN:
        return 0;
    case OP_ELOAD: case OP_ESTORE:
        return 4;
    case OP_PDEL:
        return 4;
    case OP_ARRSPREADC:
    case OP_CALLN:
        return 4;   /* slot u32 */
    case OP_CELOAD: case OP_CESTORE:
        return 6;   /* capslot u8 | depth u8 | idx u32 */
    case OP_ANEW:
        return 4;
    default: return 0;
    }
}


/* ============================== CoJIT（静的検証駆動 AOT 特化） ==============================
 * 「静的検証で、必要な部分のみ最適化する」層。実行時コード生成（禁止事項の JIT）ではなく、
 * compile 直後のバイト列から意味保持が構造的に自明な形だけを検出し、証明済み
 * スーパー命令へ畳む。異常を見つけたら一切無変更（汎用実行が既定＝安全側）。
 * 特化後ストリームは必ず akl_verify の whole-scan を通す（事後セルフチェック）。
 *
 * S1: キャリア回転（while 形・関数内 for 形＝codegen の AST 回転が届かない経路）
 *   P:  CJMPF_{G,L} slot, k, cmp, exit        (pre-check)
 *       body（任意の命令列）
 *   T:  LINC|GINC slot, d
 *       JMP P
 *   exit:
 * を
 *   T:  JMP Lx / NOP×9      Lx: LOOPINC_{G,L}V slot, d, k, cmp, P+14 / JMP exit
 * に書き換える。
 * 等価性（逐語確認済みの要点）:
 *  - LOOPINC_V は [LINC;CJMPF] の細部まで同一の分岐網（int/i64/double/bin_add/
 *    to_number/isnan/fmod なし）＋ last_val 更新（LINC/GINC の文脈意味を保持）。
 *  - 実行順序は原列 body;inc;check;body;... と回転後で逐語同一（inc は既に末尾）。
 *  - continue→T は JMP Lx に着地し同一 update+check を通る。break→exit は不変。
 *  - 受理条件は形適合のみ（T+14==exit、slot 一致、JMP 背逆）。body 中の op は
 *    不問（移動しないため）。交叉ジャンプは構造化 codegen の不変条件で存在しない。
 */

/* pc を内包する最も内側（最小 span）の funcs エントリ index を返す。無ければ main 相当の 0…
 * ではなく UINT32_MAX（呼出側は funcs[] 参照前に検査せよ） → cojit では必ず見つかる想定 */
static u32 akl_func_owner_of(const AklRT *rt, u32 pc) {
    u32 best = UINT32_MAX, best_span = UINT32_MAX;
    for (u32 i = 0; i < rt->n_funcs; i++) {
        u32 off = rt->funcs[i].code_off, end = rt->funcs[i].code_end;
        if (pc >= off && pc < end && end - off <= best_span) {
            best = i; best_span = end - off;
        }
    }
    return best;
}

static u32 akl_cojit(AklRT *rt, u32 code_from) {
    u32 applied = 0;
    u8 *code = rt->code;
    const u32 end = rt->code_len; /* 追記分は走査しない */
    u32 pc = code_from;
    while (pc < end) {
        u8 op = code[pc];
        if (op >= OP_COUNT) break; /* 入力は codegen 直後（正当）。防御として中止 */
        if (op == OP_CJMPF_G || op == OP_CJMPF_L) {
            bool is_g = op == OP_CJMPF_G;
            u32 slot, exit_t; i32 k; u8 cmp;
            memcpy(&slot, code + pc + 1, 4);
            memcpy(&k, code + pc + 5, 4);
            cmp = code[pc + 9];
            memcpy(&exit_t, code + pc + 10, 4);
            /* 末尾候補:
             *   A: [LINC|GINC slot d][JMP pc]        …14B（文末融合形）
             *   B: [GLOAD_S slot][ADDCI d][GSTORE_SPV slot][JMP pc]  …20B（関数内グローバル）
             *   C: [LLOAD slot][ADDCI d][LSTORE_PV slot][JMP pc]     …20B（関数内ローカル）
             *   D: [GLOAD_S|LLOAD slot][ADDCI d][DUP][GSTORE_S|LSTORE slot][POP][JMP pc] …22B
             *      （main トップレベルの非融合形: g = g + d の評価値を DUP/POP で捨てる。
             *       last_val 不変）
             * かつ T+{14,20,22}==exit_t。A/B/C は 「slot += d（+last_val 更新）」で LOOPINC_V
             * と、D は「slot += d（last_val 不変）」で LOOPINC（non-V）と、それぞれ逐語等価
             * （int fast/i64 域/bin_add/to_number/isnan/cmp 表まで同一）。 */
            bool found = false;
            u32 t_tail = 0, t_len = 14; i32 d = 0;
            bool lv_update = true; /* 末尾形が last_val を更新するか（A/B/C=更新, D=不変） */
            u32 q = pc + 14;
            for (u32 steps = 0; q < end && steps < 10000 && !found; steps++) {
                u8 o2 = code[q];
                if (o2 >= OP_COUNT) break;
                u32 stride = 1 + akl_op_imm_len(o2);
                if (q + stride > end) break;
                if ((o2 == OP_LINC && !is_g) || (o2 == OP_GINC && is_g)) {
                    u32 s2; i32 d2;
                    memcpy(&s2, code + q + 1, 4);
                    memcpy(&d2, code + q + 5, 4);
                    if (s2 == slot && code[q + 9] == OP_JMP) {
                        u32 bt;
                        memcpy(&bt, code + q + 10, 4);
                        if (bt == pc && q + 14 == exit_t) {
                            found = true; t_tail = q; t_len = 14; d = d2;
                        }
                    }
                } else if (is_g && o2 == OP_GLOAD_S && q + 22 <= end) {
                    u32 s2; i32 d2; u32 s3;
                    memcpy(&s2, code + q + 1, 4);
                    memcpy(&d2, code + q + 6, 4);
                    memcpy(&s3, code + q + 11, 4);
                    if (s2 == slot && s3 == slot &&
                        code[q + 5] == OP_ADDCI && code[q + 10] == OP_GSTORE_SPV &&
                        code[q + 15] == OP_JMP) {
                        u32 bt;
                        memcpy(&bt, code + q + 16, 4);
                        if (bt == pc && q + 20 == exit_t) {
                            found = true; t_tail = q; t_len = 20; d = d2;
                            lv_update = true;
                        }
                    } else {
                        /* 末尾 D（文形: g = g + d の評価値を DUP/POP で捨てる形）:
                         *   [GLOAD_S slot][ADDCI d][DUP][GSTORE_S slot][POP][JMP pc] …22B
                         * net = g += d, stack 中立, last_val 不変。
                         * 各部分の逐語手続き: GLOAD_S+ADDCI ≡ LOOPINC_G の nv 計算
                         * （int fast=i64 域一致 / overflow=(double)s2 / 文字列は同じ
                         *  akl_bin_add(lv, MK_INT(d))。比較は CJMPF_G と同一 cmp 表）、
                         * DUP+GSTORE_S+POP ≡ store のみで stack/last_val 不変条件一致。
                         * → LOOPINC_G（non-V 版。last_val を動かさない）へ写像する */
                        u32 s4; u32 bt;
                        memcpy(&s4, code + q + 12, 4);
                        memcpy(&bt, code + q + 18, 4);
                        if (s2 == slot && s4 == slot &&
                            code[q + 5] == OP_ADDCI && code[q + 10] == OP_DUP &&
                            code[q + 11] == OP_GSTORE_S && code[q + 16] == OP_POP &&
                            code[q + 17] == OP_JMP && bt == pc && q + 22 == exit_t) {
                            found = true; t_tail = q; t_len = 22; d = d2;
                            lv_update = false;
                        }
                    }
                } else if (!is_g && o2 == OP_LLOAD && q + 22 <= end) {
                    u32 s2; i32 d2; u32 s3;
                    memcpy(&s2, code + q + 1, 4);
                    memcpy(&d2, code + q + 6, 4);
                    memcpy(&s3, code + q + 11, 4);
                    if (s2 == slot && s3 == slot &&
                        code[q + 5] == OP_ADDCI && code[q + 10] == OP_LSTORE_PV &&
                        code[q + 15] == OP_JMP) {
                        u32 bt;
                        memcpy(&bt, code + q + 16, 4);
                        if (bt == pc && q + 20 == exit_t) {
                            found = true; t_tail = q; t_len = 20; d = d2;
                            lv_update = true;
                        }
                    } else {
                        /* 末尾 D のローカル版:
                         *   [LLOAD slot][ADDCI d][DUP][LSTORE slot][POP][JMP pc] …22B
                         * → LOOPINC_L（non-V）へ写像（等価性はグローバル版と同じ証明） */
                        u32 s4; u32 bt;
                        memcpy(&s4, code + q + 12, 4);
                        memcpy(&bt, code + q + 18, 4);
                        if (s2 == slot && s4 == slot &&
                            code[q + 5] == OP_ADDCI && code[q + 10] == OP_DUP &&
                            code[q + 11] == OP_LSTORE && code[q + 16] == OP_POP &&
                            code[q + 17] == OP_JMP && bt == pc && q + 22 == exit_t) {
                            found = true; t_tail = q; t_len = 22; d = d2;
                            lv_update = false;
                        }
                    }
                }
                q += stride;
            }
            if (found) {
                /* 発生元関数（内側優先の最小 span）を求める: ローカル版の trampoline は
                 * その関数と同じ n_locals の領域に属さないと verify が健全に通せない */
                u32 owner = akl_func_owner_of(rt, pc);
                if (!is_g && owner == UINT32_MAX) { /* 防御層: 設計上到達不能 */
                    pc += 14;
                    continue;
                }
                /* 変更は全て容量確保の後（OOM では無変更＝従来路実行に留まる） */
                bool cap_ok = true;
                if (rt->code_len + 22 > rt->code_cap) {
                    u32 nc = rt->code_cap ? rt->code_cap * 2 : 1024;
                    while (nc < rt->code_len + 22) nc *= 2;
                    u8 *ncp = (u8 *)realloc(rt->code, nc);
                    if (!ncp) { cap_ok = false; code = rt->code; }
                    else { rt->code = ncp; rt->code_cap = nc; code = ncp; }
                }
                if (cap_ok && !is_g && rt->n_funcs == rt->cap_funcs) {
                    /* 領域エントリ追加のための funcs 拡張 */
                    u32 nc = rt->cap_funcs ? rt->cap_funcs * 2 : 32;
                    AklFuncEnt *nf = (AklFuncEnt *)realloc(rt->funcs, (u64)nc * sizeof(AklFuncEnt));
                    if (!nf) cap_ok = false;
                    else { rt->funcs = nf; rt->cap_funcs = nc; }
                }
                if (cap_ok) {
                    u32 lx = rt->code_len;
                    if (!is_g) {
                        /* trampoline 領域を発生元関数の n_locals で登録（verifier の
                         * ローカルスロット照査を健全なまま通す） */
                        u32 fe = rt->n_funcs++;
                        rt->funcs[fe].code_off = lx;
                        rt->funcs[fe].code_end = lx + 22;
                        rt->funcs[fe].name = 0;
                        rt->funcs[fe].n_params = 0;
                        rt->funcs[fe].n_locals = rt->funcs[owner].n_locals;
                    }
                    code[rt->code_len++] = is_g ? (lv_update ? OP_LOOPINC_GV : OP_LOOPINC_G)
                                                : (lv_update ? OP_LOOPINC_LV : OP_LOOPINC_L);
                    memcpy(code + rt->code_len, &slot, 4); rt->code_len += 4;
                    memcpy(code + rt->code_len, &d, 4);    rt->code_len += 4;
                    memcpy(code + rt->code_len, &k, 4);    rt->code_len += 4;
                    code[rt->code_len++] = cmp;
                    u32 body_top = pc + 14; /* 回転後のループ先頭は body（pre-check 済み） */
                    memcpy(code + rt->code_len, &body_top, 4); rt->code_len += 4;
                    code[rt->code_len++] = OP_JMP;
                    memcpy(code + rt->code_len, &exit_t, 4); rt->code_len += 4;
                    code[t_tail] = OP_JMP;
                    memcpy(code + t_tail + 1, &lx, 4);
                    for (u32 z = t_tail + 5; z < t_tail + t_len; z++) code[z] = OP_NOP;
                    applied++;
                    pc += 14;
                    continue;
                }
                /* 容量失敗: 無変更で次へ */
                pc += 14;
                continue;
            }
        }
        pc += 1 + akl_op_imm_len(op);
    }
    rt->cojit_applied += applied;
    return applied;
}

static bool akl_verify(AklRT *rt, u32 code_from) {
    u32 len = rt->code_len;
    if (code_from > len) { akl_errf(rt, "verify: range"); return false; }
    u8 *ins = (u8 *)calloc(len ? len : 1, 1); /* 命令開始 bitmap */
    u32 *jt = (u32 *)malloc(((u64)len + 8) * sizeof(u32));
    u32 n_jt = 0;
    if (!ins || !jt) { free(ins); free(jt); akl_errf(rt, "oom: verify"); return false; }
    /* 関数区間で「現在の関数」を管理（main=0 は [0,len)） */
    /* eval ごとの追記なので、code_end <= code_from の過去区間は走査対象外として
     * fi を前倒しする（funcs[] は code_off 昇順: codegen が main→ネストの順に発行） */
    u32 fi = 0;
    while (fi < rt->n_funcs && rt->funcs[fi].code_end <= code_from) fi++;
    i32 cur = 0;
    i32 fstk[256]; u32 n_f = 0;
    u32 pc = code_from;
    bool ok = false;
    while (pc < len) {
        while (n_f && pc >= rt->funcs[fstk[n_f - 1]].code_end) n_f--;
        while (fi < rt->n_funcs && rt->funcs[fi].code_off == pc) {
            if (n_f >= 256) { akl_errf(rt, "verify: func nesting"); goto done; }
            fstk[n_f++] = (i32)fi;
            fi++;
        }
        cur = n_f ? fstk[n_f - 1] : -1;
        if (cur < 0) { akl_errf(rt, "verify: outside function at %u", pc); goto done; }
        u32 foff = rt->funcs[cur].code_off, fend = rt->funcs[cur].code_end;
        if (pc < foff || pc >= fend) { akl_errf(rt, "verify: region"); goto done; }
        ins[pc] = 1;
        u8 op = rt->code[pc];
        if (op >= OP_COUNT) { akl_errf(rt, "verify: bad opcode %u at %u", op, pc); goto done; }
        pc++;
        u32 imm_len = akl_op_imm_len(op); /* 表は cojit と共有（drift 防止） */
        if (pc + imm_len > len) { akl_errf(rt, "verify: operand overrun at %u", pc - 1); goto done; }
        /* 内容検査 */
        if (op == OP_JMP || op == OP_JMPF || op == OP_JMPT) {
            u32 tgt;
            memcpy(&tgt, &rt->code[pc], 4);
            if (tgt >= len) { akl_errf(rt, "verify: jump out of range %u", tgt); goto done; }
            if (n_jt) { } /* counting */
            jt[n_jt++] = tgt;
        } else if (op == OP_CJMPF_MULGG || op == OP_CJMPF_MODGG) {
            u32 tgt;
            memcpy(&tgt, &rt->code[pc + 13], 4);
            if (tgt >= len) { akl_errf(rt, "verify: jump out of range %u", tgt); goto done; }
            jt[n_jt++] = tgt;
            u32 g1, g2;
            memcpy(&g1, &rt->code[pc], 4);
            memcpy(&g2, &rt->code[pc + 4], 4);
            if (g1 >= rt->n_globals || g2 >= rt->n_globals) {
                akl_errf(rt, "verify: global slot %u/%u >= %u", g1, g2, rt->n_globals);
                goto done;
            }
            if (op == OP_CJMPF_MULGG) {
                u32 g3;
                memcpy(&g3, &rt->code[pc + 8], 4);
                if (g3 >= rt->n_globals) {
                    akl_errf(rt, "verify: global slot %u >= %u", g3, rt->n_globals);
                    goto done;
                }
            }
        } else if (op == OP_CJMPF_MODG || op == OP_CJMPF_MODL || op == OP_LOOPINC_G ||
                   op == OP_LOOPINC_L || op == OP_LOOPINC_GV || op == OP_LOOPINC_LV) {
            u32 tgt;
            memcpy(&tgt, &rt->code[pc + 13], 4);
            if (tgt >= len) { akl_errf(rt, "verify: jump out of range %u", tgt); goto done; }
            jt[n_jt++] = tgt;
            u32 slot;
            memcpy(&slot, &rt->code[pc], 4);
            if (op == OP_CJMPF_MODL || op == OP_LOOPINC_L || op == OP_LOOPINC_LV) {
                if (slot >= rt->funcs[cur].n_locals) {
                    akl_errf(rt, "verify: local slot %u >= %u", slot, rt->funcs[cur].n_locals);
                    goto done;
                }
            } else {
                if (slot >= rt->n_globals) {
                    akl_errf(rt, "verify: global slot %u >= %u", slot, rt->n_globals);
                    goto done;
                }
            }
        } else if (op == OP_CJMPF_L || op == OP_CJMPF_G) {
            u32 tgt;
            memcpy(&tgt, &rt->code[pc + 9], 4);
            if (tgt >= len) { akl_errf(rt, "verify: jump out of range %u", tgt); goto done; }
            jt[n_jt++] = tgt;
            if (op == OP_CJMPF_L) {
                u32 slot;
                memcpy(&slot, &rt->code[pc], 4);
                if (slot >= rt->funcs[cur].n_locals) {
                    akl_errf(rt, "verify: local slot %u >= %u", slot, rt->funcs[cur].n_locals);
                    goto done;
                }
            }
        } else if (op == OP_LINC) {
            u32 slot;
            memcpy(&slot, &rt->code[pc], 4);
            if (slot >= rt->funcs[cur].n_locals) {
                akl_errf(rt, "verify: local slot %u >= %u", slot, rt->funcs[cur].n_locals);
                goto done;
            }
        } else if (op == OP_GADD_G || op == OP_ADD_GX || op == OP_SUB_GX ||
                   op == OP_MUL_GX || op == OP_MOD_GX) {
            u32 gd, gs;
            memcpy(&gd, &rt->code[pc], 4);
            memcpy(&gs, &rt->code[pc + 4], 4);
            if (gd >= rt->n_globals || gs >= rt->n_globals) {
                akl_errf(rt, "verify: global slot %u/%u >= %u", gd, gs, rt->n_globals);
                goto done;
            }
        } else if (op == OP_GLOAD_S || op == OP_GSTORE_S || op == OP_GINC ||
                   op == OP_GMULC || op == OP_GADD_P || op == OP_GSTORE_SPV ||
                   op == OP_ADDCI_G || op == OP_SUBCI_G || op == OP_MULCI_G || op == OP_MODCI_G) {
            u32 slot;
            memcpy(&slot, &rt->code[pc], 4);
            if (slot >= rt->n_globals) {
                akl_errf(rt, "verify: global slot %u >= %u", slot, rt->n_globals);
                goto done;
            }
        } else if (op == OP_ADD_LX || op == OP_SUB_LX || op == OP_MUL_LX || op == OP_MOD_LX) {
            u32 d0;
            memcpy(&d0, &rt->code[pc], 4);
            u32 s1_;
            memcpy(&s1_, &rt->code[pc + 4], 4);
            if (d0 >= rt->funcs[cur].n_locals || s1_ >= rt->funcs[cur].n_locals) {
                akl_errf(rt, "verify: local slot out of range");
                goto done;
            }
        } else if (op == OP_LADD_LL) {
            u32 d0;
            memcpy(&d0, &rt->code[pc], 4);
            u32 s1_, s2_;
            memcpy(&s1_, &rt->code[pc + 4], 4);
            memcpy(&s2_, &rt->code[pc + 8], 4);
            if (d0 >= rt->funcs[cur].n_locals || s1_ >= rt->funcs[cur].n_locals || s2_ >= rt->funcs[cur].n_locals) {
                akl_errf(rt, "verify: local slot out of range");
                goto done;
            }
        } else if (op == OP_LLOAD || op == OP_LSTORE || op == OP_LMULC ||
                   op == OP_LADD_P || op == OP_LSTORE_PV || op == OP_RET_L ||
                   op == OP_ADDCI_L || op == OP_SUBCI_L || op == OP_MULCI_L || op == OP_MODCI_L) {
            u32 slot;
            memcpy(&slot, &rt->code[pc], 4);
            if (slot >= rt->funcs[cur].n_locals) {
                akl_errf(rt, "verify: local slot %u >= %u", slot, rt->funcs[cur].n_locals);
                goto done;
            }
        } else if (op == OP_CONST_STR) {
            u32 idx;
            memcpy(&idx, &rt->code[pc], 4);
            if (idx >= rt->n_objs || rt->objs[idx].kind != AKL_OK_STR) {
                akl_errf(rt, "verify: bad string ref %u", idx);
                goto done;
            }
        } else if (op == OP_TRY_PUSH) {
            /* catch/finally pc: NONE か len 内の命令開始。slot: NONE か現関数 locals 内 */
            for (int w = 0; w < 2; w++) {
                u32 t;
                memcpy(&t, &rt->code[pc + (u32)w * 4], 4);
                if (t != AKL_PC_NONE) {
                    if (t >= len) { akl_errf(rt, "verify: try target out of range %u", t); goto done; }
                    jt[n_jt++] = t;
                }
            }
            u32 cslot;
            memcpy(&cslot, &rt->code[pc + 8], 4);
            if (cslot != AKL_SLOT_NONE && cslot >= rt->funcs[cur].n_locals) {
                akl_errf(rt, "verify: catch slot %u >= %u", cslot, rt->funcs[cur].n_locals);
                goto done;
            }
        } else if (op == OP_TRY_LEAVE) {
            u32 t;
            memcpy(&t, &rt->code[pc], 4);
            if (t >= len) { akl_errf(rt, "verify: try_leave target out of range %u", t); goto done; }
            jt[n_jt++] = t;
        } else if (op == OP_MAKEF) {
            u32 fidx;
            memcpy(&fidx, &rt->code[pc], 4);
            if (fidx >= rt->n_funcs || rt->funcs[fidx].code_off >= len) {
                akl_errf(rt, "verify: bad func ref %u", fidx);
                goto done;
            }
        } else if (op == OP_GLOAD || op == OP_GSTORE) {
            u32 name;
            memcpy(&name, &rt->code[pc], 4);
            if (name >= rt->n_objs || rt->objs[name].kind != AKL_OK_STR) {
                akl_errf(rt, "verify: bad global name ref %u", name);
                goto done;
            }
        } else if (op == OP_PLOAD || op == OP_PSTORE) {
            u32 name;
            memcpy(&name, &rt->code[pc], 4);
            if (name >= rt->n_objs || rt->objs[name].kind != AKL_OK_STR) {
                akl_errf(rt, "verify: bad prop name ref %u", name);
                goto done;
            }
        } else if (op == OP_MCALL) {
            if (rt->code[pc] > 250) { akl_errf(rt, "verify: mcall argc %u", (u32)rt->code[pc]); goto done; }
            u32 name;
            memcpy(&name, &rt->code[pc + 1], 4);
            if (name >= rt->n_objs || rt->objs[name].kind != AKL_OK_STR) {
                akl_errf(rt, "verify: bad mcall name ref %u", name);
                goto done;
            }
        }
        pc += imm_len;
    }
    /* ジャンプ先が命令開始か（bitmap で） */
    for (u32 i = 0; i < n_jt; i++) {
        if (!ins[jt[i]]) { akl_errf(rt, "verify: jump target %u not an instruction", jt[i]); goto done; }
    }
    ok = true;
done:
    free(ins);
    free(jt);
    return ok;
}

/* ============================== 数値/文字列の変換 ============================== */

static double akl_canon(double d) {
    if (isnan(d)) {
        AklVal bits = 0x7FF8000000000000ull;
        double out;
        memcpy(&out, &bits, 8);
        return out;
    }
    return d;
}

/* ToNumber 近似（v0.0 のプリミティブ集合で JS 整合） */
static double akl_to_number(AklRT *rt, AklVal v) {
    double d;
    if (akl_numv(v, &d)) return d;
    if (v == AKL_VAL_TRUE) return 1.0;
    if (v == AKL_VAL_FALSE) return 0.0;
    if (v == AKL_VAL_NULL) return 0.0;
    if (v == AKL_VAL_UNDEF) return akl_canon(0.0 / 0.0);
    if (akl_is_objv(v)) {
        if (akl_is_strly(rt, v)) {
            u32 sl;
            const u8 *sb = akl_str(rt, akl_get_obj(v), &sl); /* ROPE は初回のみ平坦化 */
            if (rt->err[0]) return akl_canon(0.0 / 0.0);
            u32 i = 0;
            while (i < sl && (sb[i] == ' ' || sb[i] == '\t' || sb[i] == '\n' ||
                              sb[i] == '\r' || sb[i] == '\f')) i++;
            u32 e = sl;
            while (e > i && (sb[e - 1] == ' ' || sb[e - 1] == '\t' || sb[e - 1] == '\n' ||
                             sb[e - 1] == '\r' || sb[e - 1] == '\f')) e--;
            if (i == e) return 0.0;
            char tmp[64];
            u32 m = e - i < 63 ? e - i : 63;
            memcpy(tmp, sb + i, m);
            tmp[m] = 0;
            char *endp = NULL;
            double dv = strtod(tmp, &endp);
            if (endp == tmp || *endp != 0) return akl_canon(0.0 / 0.0);
            return dv;
        }
    }
    return akl_canon(0.0 / 0.0);
}

/* JS ToInt32（ビット演算・配列 index の統一変換）。obj/文字列は ToNumber 経由。 */
static i32 akl_to_int32(AklRT *rt, AklVal v) {
    if (akl_is_intv(v)) return akl_get_int(v);
    double d = akl_to_number(rt, v);
    if (isnan(d) || isinf(d)) return 0;
    double t = fmod(trunc(d), 4294967296.0);
    if (t < 0) t += 4294967296.0;
    return (i32)(u32)t;
}

/* 二項演算の被演算子ペアをまとめて数値化する。pop 済みの値は GC ルート外なので、
 * 片方の flatten が起こす GC で他方の obj が掃かれる（index 再利用まで含む）のを
 * 防ぐため、両方を nursery にピンしてから変換する。 */
static void akl_to_number2(AklRT *rt, AklVal a, AklVal b, double *da, double *db) {
    u32 nur0 = rt->n_nury;
    if (akl_is_objv(a) && rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = akl_get_obj(a);
    if (akl_is_objv(b) && rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = akl_get_obj(b);
    *da = akl_to_number(rt, a);
    *db = akl_to_number(rt, b);
    rt->n_nury = nur0;
}

static bool akl_truthy(AklRT *rt, AklVal v) {
    double d;
    if (v == AKL_VAL_UNDEF || v == AKL_VAL_NULL || v == AKL_VAL_FALSE) return false;
    if (v == AKL_VAL_TRUE) return true;
    if (akl_is_intv(v)) return akl_get_int(v) != 0;
    if (akl_numv(v, &d)) return d != 0.0 && !isnan(d);
    if (akl_is_objv(v)) {
        AklObj *o = &rt->objs[akl_get_obj(v)];
        if (o->kind == AKL_OK_STR || o->kind == AKL_OK_ROPE) return o->len != 0; /* ROPE の len は全長。flatten 不要 */
    }
    return true;
}

/* i32 → 十進バイト列。JS ToString(int) と同一（snprintf("%d") 互換・ロケール非依存）。 */
static u32 akl_fmt_i32(char *out, i32 v) {
    char tmp[12];
    u32 n = 0;
    u32 u = v < 0 ? (u32)(-(i64)v) : (u32)v;
    do { tmp[n++] = (char)('0' + (u % 10)); u /= 10; } while (u);
    u32 w = 0;
    if (v < 0) out[w++] = '-';
    while (n) out[w++] = tmp[--n];
    return w;
}

/* ToString（obj index を返す。失敗時 UINT32_MAX で err 設定） */
static u32 akl_to_string(AklRT *rt, AklVal v) {
    char tmp[40];
    if (v == AKL_VAL_UNDEF) return akl_mkstr(rt, (const u8 *)"undefined", 9);
    if (v == AKL_VAL_NULL)  return akl_mkstr(rt, (const u8 *)"null", 4);
    if (v == AKL_VAL_TRUE)  return akl_mkstr(rt, (const u8 *)"true", 4);
    if (v == AKL_VAL_FALSE) return akl_mkstr(rt, (const u8 *)"false", 5);
    if (akl_is_intv(v)) {
        u32 n = akl_fmt_i32(tmp, akl_get_int(v));
        return akl_mkstr(rt, (const u8 *)tmp, n);
    }
    double d;
    if (akl_numv(v, &d)) {
        int n;
        if (isnan(d)) n = snprintf(tmp, sizeof tmp, "NaN");
        else if (isinf(d)) n = snprintf(tmp, sizeof tmp, d > 0 ? "Infinity" : "-Infinity");
        else if (d == floor(d) && fabs(d) < 1e21) {
            n = snprintf(tmp, sizeof tmp, "%.0f", d); /* 整数は 1e21 未満まで十進全桁（JS 準拠） */
        } else {
            /* 往復最短精度: 15→17 桁で strtod が厳密に元へ戻る最初を採用（Grisu -lite） */
            n = 0;
            for (int prec = 15; prec <= 17; prec++) {
                n = snprintf(tmp, sizeof tmp, "%.*g", prec, d);
                if (strtod(tmp, NULL) == d) break;
            }
            /* C 指数の先行ゼロを JS 形へ正規化（e-07 → e-7, e+09 → e+9） */
            for (int i = 0; i + 3 < n; i++) {
                if (tmp[i] == 'e' && (tmp[i + 1] == '+' || tmp[i + 1] == '-') && tmp[i + 2] == '0' &&
                    tmp[i + 3] >= '0' && tmp[i + 3] <= '9') {
                    memmove(tmp + i + 2, tmp + i + 3, (u32)(n - (i + 2)));
                    n--;
                    break;
                }
            }
        }
        return akl_mkstr(rt, (const u8 *)tmp, (u32)n);
    }
    if (akl_is_objv(v)) {
        AklObj *o = &rt->objs[akl_get_obj(v)];
        if (o->kind == AKL_OK_STR || o->kind == AKL_OK_ROPE) return akl_get_obj(v); /* ROPE は文字列そのもの。flatten は読み出し時に遅延 */
        if (o->kind == AKL_OK_FUNC || o->kind == AKL_OK_NATIVE) return akl_mkstr(rt, (const u8 *)"function", 8);
        if (o->kind == AKL_OK_OBJ) return akl_mkstr(rt, (const u8 *)"[object Object]", 15);
        if (o->kind == AKL_OK_ARR) {
            /* JS ToString(arr) = Array.prototype.toString = join(",") */
            u32 n = o->u.arr.n;
            u8 *buf = (u8 *)malloc(64);
            if (!buf) { akl_errf(rt, "oom: array tostring"); return UINT32_MAX; }
            u32 bl = 0, cap = 64;
            bool ok_ = true;
            for (u32 i = 0; i < n && ok_; i++) {
                if (i) {
                    if (bl + 1 > cap) {
                        u32 nc2 = cap * 2;
                        u8 *nb2 = (u8 *)realloc(buf, nc2);
                        if (!nb2) { free(buf); akl_errf(rt, "oom: array tostring"); return UINT32_MAX; }
                        buf = nb2; cap = nc2;
                    }
                    buf[bl++] = ',';
                }
                AklVal ev = o->u.arr.v[i];
                u32 si = akl_to_string(rt, ev);
                if (si == UINT32_MAX) { ok_ = false; break; }
                if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = si;
                u32 el;
                const u8 *ep = akl_str(rt, si, &el);
                if (rt->err[0]) { ok_ = false; break; }
                if (bl + el > cap) {
                    u32 nc2 = cap * 2;
                    while (nc2 < bl + el) nc2 *= 2;
                    u8 *nb2 = (u8 *)realloc(buf, nc2);
                    if (!nb2) { free(buf); akl_errf(rt, "oom: array tostring"); return UINT32_MAX; }
                    buf = nb2; cap = nc2;
                }
                memcpy(buf + bl, ep, el);
                bl += el;
                if (rt->n_nury) rt->n_nury--;
            }
            if (!ok_) { free(buf); return UINT32_MAX; }
            u32 ridx = akl_mkstr(rt, buf, bl);
            free(buf);
            return ridx;
        }
        if (o->kind == AKL_OK_HANDLE) {
            char hbuf[64];
            int hn_ = snprintf(hbuf, sizeof hbuf, "[object %s]",
                               o->u.hd.vt && o->u.hd.vt->tag ? o->u.hd.vt->tag : "Handle");
            return akl_mkstr(rt, (const u8 *)hbuf, (u32)hn_);
        }
    }
    return akl_mkstr(rt, (const u8 *)"[unknown]", 9);
}

static bool akl_strict_eq(AklRT *rt, AklVal a, AklVal b) {
    double da, db;
    bool na = akl_numv(a, &da), nb = akl_numv(b, &db);
    if (na && nb) return da == db;
    if (na != nb) return false;
    if (a == b) return true; /* UNDEF/NULL/bool 同値, 同一 obj idx */
    if (akl_is_strly(rt, a) && akl_is_strly(rt, b)) {
        u32 ia = akl_get_obj(a), ib = akl_get_obj(b);
        if (rt->objs[ia].len != rt->objs[ib].len) return false; /* ROPE でも len は全長。不等長は flatten せず即 false */
        /* pop 済み値の連続 flatten は相互スイープを起こし得るのでペアでピン */
        u32 nur0 = rt->n_nury;
        if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = ia;
        if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = ib;
        u32 la, lb;
        const u8 *pa = akl_str(rt, ia, &la);
        const u8 *pb = rt->err[0] ? (const u8 *)"" : akl_str(rt, ib, &lb);
        bool r = !rt->err[0] && la == lb && (la == 0 || memcmp(pa, pb, la) == 0);
        rt->n_nury = nur0;
        return r;
    }
    return false;
}
static bool akl_loose_eq(AklRT *rt, AklVal a, AklVal b) {
    /* 全体を通じて a,b をピン（strict_eq 内の flatten と to_number 家族の GC から守る） */
    u32 nur0 = rt->n_nury;
    if (akl_is_objv(a) && rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = akl_get_obj(a);
    if (akl_is_objv(b) && rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = akl_get_obj(b);
    bool r = false;
    if (akl_strict_eq(rt, a, b)) r = true;
    else if ((a == AKL_VAL_NULL && b == AKL_VAL_UNDEF) || (a == AKL_VAL_UNDEF && b == AKL_VAL_NULL)) r = true;
    else if (akl_is_strly(rt, a)) {
        double db;
        if (akl_numv(b, &db)) r = akl_to_number(rt, a) == db;
        else if (b == AKL_VAL_TRUE || b == AKL_VAL_FALSE) r = akl_to_number(rt, a) == (b == AKL_VAL_TRUE);
    } else if (akl_is_strly(rt, b)) {
        double da;
        if (akl_numv(a, &da)) r = da == akl_to_number(rt, b);
        else if (a == AKL_VAL_TRUE || a == AKL_VAL_FALSE) r = (a == AKL_VAL_TRUE) == akl_to_number(rt, b);
    } else if (a == AKL_VAL_TRUE || a == AKL_VAL_FALSE) {
        double da = a == AKL_VAL_TRUE ? 1.0 : 0.0;
        r = da == akl_to_number(rt, b);
    } else if (b == AKL_VAL_TRUE || b == AKL_VAL_FALSE) {
        double db = b == AKL_VAL_TRUE ? 1.0 : 0.0;
        r = akl_to_number(rt, a) == db;
    }
    rt->n_nury = nur0;
    return r;
}

/* `+` 演算の完全実装（int fast path / 文字列連結 / 数値加算）。
 * ADD 命令と LINC 非 int フォールバックで共有するため分離。
 * sp は GC ルート深さ（文字列経路のみ使用）。失敗時 false（rt->err 設定済み）。 */
static bool akl_bin_add(AklRT *rt, AklVal va, AklVal vb, u32 sp, AklVal *out) {
    if (akl_is_intv(va) && akl_is_intv(vb)) {
        i64 r = (i64)akl_get_int(va) + (i64)akl_get_int(vb);
        if (r >= -2147483648ll && r <= 2147483647ll) { *out = AKL_MK_INT((i32)r); return true; }
        *out = akl_num((double)r);
        return true;
    }
    bool sa = akl_is_strly(rt, va), sb = akl_is_strly(rt, vb);
    if (sa || sb) {
        rt->gc_sp = sp; /* GC 発火点: ルート深さを同期 */
        u32 nur0 = rt->n_nury;
        u32 ia = sa ? akl_get_obj(va) : akl_to_string(rt, va); /* STR/ROPE は変換不要 */
        if (ia == UINT32_MAX) return false;
        if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = ia;
        /* 右辺 int の直接桁化: ToString(int) と同一十進バイトを中間 obj なしで連結する
         * （'x' + i 型のホット路。obj 生成・malloc・GC 圧を反復あたり 1 回分削る） */
        char dbuf[16];
        u32 ib = UINT32_MAX, lb;
        const u8 *pb_inline = NULL;
        if (!sb && akl_is_intv(vb)) {
            lb = akl_fmt_i32(dbuf, akl_get_int(vb));
            pb_inline = (const u8 *)dbuf;
        } else {
            ib = akl_to_string(rt, vb);
            if (ib == UINT32_MAX) { rt->n_nury = nur0; return false; }
            if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = ib;
            lb = rt->objs[ib].len;
        }
        /* 平坦化せずオブジェクトの len を読む（ROPE は len が全長）。これが O(1) 連結の要。 */
        u32 la = rt->objs[ia].len;
        if ((u64)la + lb > (u64)rt->heap_mb << 20) { rt->n_nury = nur0; akl_errf(rt, "heap bytes budget exhausted"); return false; }
        u32 ic;
        if ((u64)la + lb <= 64) {
            /* 小片フラット路: 1 alloc 直接書き（ROPE ノードの固定費を避ける）。 */
            u32 ta;
            const u8 *pa = akl_str(rt, ia, &ta);
            if (rt->err[0]) { rt->n_nury = nur0; return false; }
            (void)ta;
            const u8 *pb = pb_inline;
            if (!pb) {
                u32 tb;
                pb = akl_str(rt, ib, &tb);
                if (rt->err[0]) { rt->n_nury = nur0; return false; }
            }
            ic = akl_obj_new(rt);
            if (ic == UINT32_MAX) { rt->n_nury = nur0; return false; }
            u8 *cat = (u8 *)malloc((u64)la + lb + 1);
            if (!cat) {
                obj_free_rollback(rt, ic);
                rt->n_nury = nur0;
                akl_errf(rt, "oom: concat");
                return false;
            }
            memcpy(cat, pa, la);
            memcpy(cat + la, pb, lb);
            /* GC は memcpy 後・co 投入前のこの順序が安全: cat は独立所有、ic は kind==0 で
             * スイープに拾われない、to_string 一時 obj は消費済みなので回収されて良い。 */
            if (rt->heap_bytes + (u64)la + lb > rt->gc_next) akl_gc(rt);
            if (rt->heap_bytes + (u64)la + lb > (u64)rt->heap_mb << 20) {
                akl_gc(rt);
                if (rt->heap_bytes + (u64)la + lb > (u64)rt->heap_mb << 20) {
                    obj_free_rollback(rt, ic);
                    free(cat);
                    rt->n_nury = nur0;
                    akl_errf(rt, "heap bytes budget exhausted");
                    return false;
                }
            }
            AklObj *co = &rt->objs[ic];
            co->kind = AKL_OK_STR; co->len = la + lb; co->bytes = cat;
            rt->heap_bytes += (u64)la + lb;
        } else {
            /* ROPE 路: バイトコピーなし・heap_bytes 不変の O(1) 連結。
             * 深さ 4096 を超える時は深い側だけ akl_str で in-place 平坦化（idx 不変）してから継ぐ。
             * 左背骨蓄積パターンでは 4096 concat に 1 回 O(len) なので償却 O(1)。 */
            u32 rib = ib;
            if (pb_inline) { /* 右辺 int は ROPE ノードの子にするため obj 化が要る（稀な路） */
                rib = akl_mkstr(rt, pb_inline, lb);
                if (rib == UINT32_MAX) { rt->n_nury = nur0; return false; }
                if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = rib;
            }
            u32 da = akl_str_depth(rt, ia), db = akl_str_depth(rt, rib);
            if ((da > db ? da : db) + 1 > 4096) {
                u32 deep = da >= db ? ia : rib;
                u32 tl;
                (void)akl_str(rt, deep, &tl);
                if (rt->err[0]) { rt->n_nury = nur0; return false; }
                da = akl_str_depth(rt, ia);
                db = akl_str_depth(rt, rib);
            }
            ic = akl_obj_new(rt);
            if (ic == UINT32_MAX) { rt->n_nury = nur0; return false; }
            AklObj *co = &rt->objs[ic];
            co->kind = AKL_OK_ROPE; co->bytes = NULL; co->len = la + lb;
            co->code_off = ia; co->name = rib;
            co->n_params = (u16)((da > db ? da : db) + 1);
        }
        rt->n_nury = nur0;
        *out = AKL_MK_OBJ(ic);
        return true;
    }
    *out = akl_num(akl_canon(akl_to_number(rt, va) + akl_to_number(rt, vb)));
    return true;
}

/* 定数除数の剰余（JS の % 規約）を magic-multiply で計算。imul64+shr+msub ≈ 5 cycle
 * （idiv は 20〜45 cycle）。経路は x ≥ 0 かつ d > 1 に限定し、それ以外は C の %。
 *
 * 数学的正当性: p = 32 + floor(log2 d), m = ceil(2^p / d)（d が 2 の冪なら移位）と
 * 取ると、0 ≤ x ≤ 2^31−1 の全域で floor(m·x / 2^p) == floor(x / d) が厳密に成立。
 * 証明: 2^p = m·d − e（0 < e ≤ d）と書ける（m は天井）ので
 *   m·x/2^p = x/d + e·x/(d·2^p)。
 * e·x ≤ (d−1)(2^31−1) < d·2^31 ≤ d·2^32 ≤ 2^p（p ≥ 32 + log2 d ⟹ 2^p ≥ d·2^32）
 * より付加項は 1/d 未満。floor(x/d) と次の整数のgapは 1/d 以上なので整数部は跨がない。
 * 溢れなし: m ≤ ceil(2^(32+fl)/2^fl) = 2^32、x ≤ 2^31−1 ⟹ m·x < 2^63。
 * （d 非冪で p = 32+fl ≤ 62、d 冪は q = x >> fl） */
#if defined(__GNUC__)
__attribute__((always_inline)) inline
#endif
static i32 akl_mod_imm(AklRT *rt, i32 x, i32 d) {
    if (x >= 0 && d > 1) {
        u32 ud = (u32)d;
        u32 slot = ud & 7u;
        if (rt->mm[slot].d != ud) {
            u32 d2 = ud;
            u8 fl = 31;
            while (!(d2 & 0x80000000u)) { fl--; d2 <<= 1; } /* floor(log2 d)（clz 代替） */
            if ((ud & (ud - 1)) == 0) { /* 2 の冪: 移位のみ */
                rt->mm[slot].m = 0; rt->mm[slot].p = fl;
            } else {
                u8 p = (u8)(32 + fl);
                rt->mm[slot].m = ((((u64)1) << p) + ud - 1) / ud;
                rt->mm[slot].p = p;
            }
            rt->mm[slot].d = ud;
        }
        u64 m = rt->mm[slot].m; u8 p = rt->mm[slot].p;
        u64 xu = (u64)(u32)x;
        u64 q = m ? ((m * xu) >> p) : (xu >> p);
        return (i32)(xu - q * (u64)ud);
    }
    return x % d; /* x<0 / d∈{0,±1}（INT32_MIN%-1 は呼出側で除外済）→ C 準拠 = JS 規約 */
}

/* *CI 計算の共有体。各演算は対応する単独命令（ADDCI/SUBCI/MULCI/MODCI) と
 * 同一手続き・同一丸め（int は i64 経由、溢れは (double)r、文字列 ADD は右辺連結）。
 * 単独 *CI 命令と *CI+store 再融合命令がこの 1 実装を共有することで
 * 「再融合は *CI; STORE_PV と逐語同一」の証明が構造的に成立する。 */
#if defined(__GNUC__)
__attribute__((always_inline)) inline
#endif
static bool akl_cist_compute(AklRT *rt, u8 baseop, AklVal lv, i32 imm, u32 sp, AklVal *out) {
    if (akl_is_intv(lv)) {
        i32 iv = akl_get_int(lv);
        if (baseop == OP_MOD) {
            if (imm != 0 && !(iv == INT32_MIN && imm == -1)) {
                *out = AKL_MK_INT(akl_mod_imm(rt, iv, imm)); return true; /* iv≥0&&imm>1 なら magic */
            }
        } else {
            i64 r = baseop == OP_ADD ? (i64)iv + (i64)imm
                  : baseop == OP_SUB ? (i64)iv - (i64)imm
                  : (i64)iv * (i64)imm;
            *out = (r >= -2147483648ll && r <= 2147483647ll) ? AKL_MK_INT((i32)r) : akl_num((double)r);
            return true;
        }
    }
    if (baseop == OP_ADD) { /* TOS が文字列なら imm を右辺とする連結（ADD 二項規約） */
        rt->gc_sp = sp;
        return akl_bin_add(rt, lv, AKL_MK_INT(imm), sp, out);
    }
    rt->gc_sp = sp;
    double dl = akl_to_number(rt, lv);
    if (rt->err[0]) return false;
    double dm = (double)imm;
    *out = akl_num(akl_canon(baseop == OP_SUB ? dl - dm : baseop == OP_MUL ? dl * dm : fmod(dl, dm)));
    return true;
}

/* 二項 (TOS op slot値) の共有体。*CI（定数右辺）の akl_cist_compute と対になる。
 * 演算手続きは対応する二項命令（ADD/SUB/MUL/MOD ハンドラ）と同一:
 * both-int fast（i64 経由・溢れは (double)r）、MOD の fmod/INT32_MIN フォールバック、
 * ADD のみ文字列連結（bin_add・オペランド順序は 左=TOS で保持）。 */
#if defined(__GNUC__)
__attribute__((always_inline)) inline
#endif
static bool akl_binfv_compute(AklRT *rt, u8 baseop, AklVal lv, AklVal rv, u32 sp, AklVal *out) {
    if (akl_is_intv(lv) && akl_is_intv(rv)) {
        i32 a = akl_get_int(lv), b = akl_get_int(rv);
        if (baseop == OP_MOD) {
            if (b != 0 && !(a == INT32_MIN && b == -1)) { *out = AKL_MK_INT(a % b); return true; }
        } else {
            i64 r = baseop == OP_ADD ? (i64)a + (i64)b
                  : baseop == OP_SUB ? (i64)a - (i64)b
                  : (i64)a * (i64)b;
            *out = (r >= -2147483648ll && r <= 2147483647ll) ? AKL_MK_INT((i32)r) : akl_num((double)r);
            return true;
        }
    }
    if (baseop == OP_ADD) {
        rt->gc_sp = sp;
        return akl_bin_add(rt, lv, rv, sp, out);
    }
    rt->gc_sp = sp;
    double da, db;
    akl_to_number2(rt, lv, rv, &da, &db);
    if (rt->err[0]) return false;
    *out = akl_num(akl_canon(baseop == OP_SUB ? da - db : baseop == OP_MUL ? da * db : fmod(da, db)));
    return true;
}

typedef struct { const char *name; AklNativeFn fn; } AklMethEntry;

/* 公開 API は VM の後に定義されるため、組込 native からの前方宣言 */
AklVal akl_mkstring(AklRT *rt, const char *s, uint32_t len);
AklVal akl_mknum(double d);
AklVal akl_mkbool(bool b);
AklVal akl_mkundefined(void);
void akl_native_throw(AklRT *rt, const char *msg);

/* ============================== v0.3 組込（Math / String / Array / グローバル関数） ==============================
 * 方針: メソッド表は静的テーブル + MCALL 直結（PLOAD はキャッシュ済み NATIVE を返す）。
 * 全 native は「明白に失敗」（akl_native_throw）で不正 self を拒否する。 */

/* ---- 文字列ヘルパ（コードポイント単位。s[i]/.length と同一規約） ---- */
static u32 akl_str_cp_to_byte(const u8 *s, u32 n, u32 cp_i) {
    u32 pos = 0;
    for (u32 k = 0; k < cp_i; k++) {
        if (pos >= n) return n;
        u8 c = s[pos];
        pos += c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
             : (c & 0xF8) == 0xF0 ? 4 : 1;
    }
    return pos;
}
/* JS ToInteger（trunc）。NaN/Inf はそのまま返す */
static double akl_to_integer(AklRT *rt, AklVal v) {
    double d = akl_to_number(rt, v);
    return (isnan(d) || isinf(d)) ? d : trunc(d);
}

static bool akl_arr_grow(AklRT *rt, AklObj *o, u32 need); /* 定義は配列メソッド節（前方参照） */

static i32 akl_self_str(AklRT *rt, AklVal self) {
    if (!akl_is_objv(self)) return -1;
    u32 oi = akl_get_obj(self);
    if (oi >= rt->n_objs) return -1;
    u8 k = rt->objs[oi].kind;
    return (k == AKL_OK_STR || k == AKL_OK_ROPE) ? (i32)oi : -1;
}
static i32 akl_self_arr(AklRT *rt, AklVal self) {
    if (!akl_is_objv(self)) return -1;
    u32 oi = akl_get_obj(self);
    if (oi >= rt->n_objs) return -1;
    return rt->objs[oi].kind == AKL_OK_ARR ? (i32)oi : -1;
}
static AklVal akl_native_typeerr(AklRT *rt, const char *what) {
    akl_native_throw(rt, what);
    return akl_mkundefined();
}

/* ================= 文字列メソッド ================= */
static AklVal akl_m_str_charAt(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 si = akl_self_str(rt, self);
    if (si < 0) return akl_native_typeerr(rt, "TypeError: not a string");
    u32 ln;
    const u8 *bp = akl_str(rt, (u32)si, &ln);
    if (rt->err[0]) return akl_mkundefined();
    double d = argc > 0 ? akl_to_integer(rt, argv[0]) : 0.0;
    if (rt->err[0]) return akl_mkundefined();
    u32 nc = akl_str_cp_count(bp, ln);
    if (d < 0 || isnan(d) || d >= (double)nc) return akl_mkstring(rt, "", 0);
    u32 cp = (u32)d;
    u32 pos = akl_str_cp_to_byte(bp, ln, cp);
    u32 cl = akl_str_cp_len_at(bp, ln, cp);
    return akl_mkstring(rt, (const char *)bp + pos, cl);
}
static AklVal akl_m_str_charCodeAt(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 si = akl_self_str(rt, self);
    if (si < 0) return akl_native_typeerr(rt, "TypeError: not a string");
    u32 ln;
    const u8 *bp = akl_str(rt, (u32)si, &ln);
    if (rt->err[0]) return akl_mkundefined();
    double d = argc > 0 ? akl_to_integer(rt, argv[0]) : 0.0;
    if (rt->err[0]) return akl_mkundefined();
    u32 nc = akl_str_cp_count(bp, ln);
    if (d < 0 || isnan(d) || d >= (double)nc) return akl_mknum(NAN);
    u32 pos = akl_str_cp_to_byte(bp, ln, (u32)d);
    u8 c = bp[pos];
    u32 cl = akl_str_cp_len_at(bp, ln, (u32)d);
    u32 cp;
    if (cl == 1) cp = c;
    else if (cl == 2) cp = ((u32)(c & 0x1F) << 6) | (bp[pos + 1] & 0x3F);
    else if (cl == 3) cp = ((u32)(c & 0x0F) << 12) | ((u32)(bp[pos + 1] & 0x3F) << 6) | (bp[pos + 2] & 0x3F);
    else cp = ((u32)(c & 0x07) << 18) | ((u32)(bp[pos + 1] & 0x3F) << 12) | ((u32)(bp[pos + 2] & 0x3F) << 6) | (bp[pos + 3] & 0x3F);
    return akl_mknum((double)cp);
}
static AklVal akl_m_str_codePointAt(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    return akl_m_str_charCodeAt(rt, self, argc, argv, udata); /* コードポイント単位（既知偏差: UTF-16 ではない） */
}
/* 部分文字列の位置を探す（code point 単位）。fromIndex は先頭からの cp 位置 */
static double akl_str_index_of(AklRT *rt, i32 hay, i32 needle, double from) {
    u32 hn, nn;
    const u8 *hp = akl_str(rt, (u32)hay, &hn);
    if (rt->err[0]) return NAN;
    const u8 *np = akl_str(rt, (u32)needle, &nn);
    if (rt->err[0]) return NAN;
    u32 hc = akl_str_cp_count(hp, hn), nc = akl_str_cp_count(np, nn);
    if (nc == 0) return from < 0 ? 0 : from <= (double)hc ? from : (double)hc;
    if (from < 0) from = 0;
    if (from > (double)hc) return -1.0;
    u32 f = (u32)from;
    for (u32 i = f; i + nc <= hc; i++) {
        u32 p1 = akl_str_cp_to_byte(hp, hn, i);
        u32 p2 = akl_str_cp_to_byte(hp, hn, i + nc);
        if (p2 - p1 == nn && (nn == 0 || memcmp(hp + p1, np, nn) == 0)) return (double)i;
    }
    return -1.0;
}
static AklVal akl_m_str_indexOf(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 si = akl_self_str(rt, self);
    if (si < 0) return akl_native_typeerr(rt, "TypeError: not a string");
    AklVal sub = argc > 0 ? argv[0] : akl_mkundefined();
    u32 sidx = akl_to_string(rt, sub);
    if (sidx == UINT32_MAX) return akl_mkundefined();
    if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = sidx;
    double from = argc > 1 ? akl_to_integer(rt, argv[1]) : 0.0;
    if (rt->err[0]) return akl_mkundefined();
    double r = akl_str_index_of(rt, si, (i32)sidx, from);
    if (rt->n_nury) rt->n_nury--;
    return akl_mknum(r);
}
static AklVal akl_m_str_lastIndexOf(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 si = akl_self_str(rt, self);
    if (si < 0) return akl_native_typeerr(rt, "TypeError: not a string");
    AklVal sub = argc > 0 ? argv[0] : akl_mkundefined();
    u32 sidx = akl_to_string(rt, sub);
    if (sidx == UINT32_MAX) return akl_mkundefined();
    if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = sidx;
    u32 hn, nn;
    const u8 *hp = akl_str(rt, (u32)si, &hn);
    if (rt->err[0]) return akl_mkundefined();
    const u8 *np = akl_str(rt, sidx, &nn);
    if (rt->err[0]) return akl_mkundefined();
    double from = argc > 1 ? akl_to_integer(rt, argv[1]) : 1.0e18;
    if (rt->err[0]) return akl_mkundefined();
    u32 hc = akl_str_cp_count(hp, hn), nc = akl_str_cp_count(np, nn);
    double res = -1.0;
    if (nc == 0) {
        res = from < 0 ? -1.0 : from > (double)hc ? (double)hc : from;
    } else {
        double f = from < 0 ? -1.0 : from > (double)hc ? (double)hc : from;
        for (double i = f; i >= 0; i -= 1.0) {
            u32 p1 = akl_str_cp_to_byte(hp, hn, (u32)i);
            u32 p2 = akl_str_cp_to_byte(hp, hn, (u32)i + nc);
            if (p2 - p1 == nn && (nn == 0 || memcmp(hp + p1, np, nn) == 0)) { res = i; break; }
        }
    }
    if (rt->n_nury) rt->n_nury--;
    return akl_mknum(res);
}
/* slice(start, end): 負は末尾から。end 省略は末尾。 */
static AklVal akl_m_str_slice(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 si = akl_self_str(rt, self);
    if (si < 0) return akl_native_typeerr(rt, "TypeError: not a string");
    u32 ln;
    const u8 *bp = akl_str(rt, (u32)si, &ln);
    if (rt->err[0]) return akl_mkundefined();
    double nc = (double)akl_str_cp_count(bp, ln);
    double s = argc > 0 ? akl_to_integer(rt, argv[0]) : 0.0;
    if (rt->err[0]) return akl_mkundefined();
    double e = argc > 1 ? akl_to_integer(rt, argv[1]) : nc;
    if (rt->err[0]) return akl_mkundefined();
    if (isnan(s)) s = 0;
    if (isnan(e)) e = 0;
    if (s < 0) s = nc + s;
    if (e < 0) e = nc + e;
    if (s < 0) s = 0;
    if (e < 0) e = 0;
    if (s > nc) s = nc;
    if (e > nc) e = nc;
    if (e < s) e = s;
    u32 p0 = akl_str_cp_to_byte(bp, ln, (u32)s);
    u32 p1 = akl_str_cp_to_byte(bp, ln, (u32)e);
    return akl_mkstring(rt, (const char *)bp + p0, p1 - p0);
}
static AklVal akl_m_str_substring(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 si = akl_self_str(rt, self);
    if (si < 0) return akl_native_typeerr(rt, "TypeError: not a string");
    u32 ln;
    const u8 *bp = akl_str(rt, (u32)si, &ln);
    if (rt->err[0]) return akl_mkundefined();
    double nc = (double)akl_str_cp_count(bp, ln);
    double s = argc > 0 ? akl_to_integer(rt, argv[0]) : 0.0;
    if (rt->err[0]) return akl_mkundefined();
    double e = argc > 1 ? akl_to_integer(rt, argv[1]) : nc;
    if (rt->err[0]) return akl_mkundefined();
    if (isnan(s)) s = 0;
    if (isnan(e)) e = 0;
    if (s < 0) s = 0;
    if (e < 0) e = 0;
    if (s > nc) s = nc;
    if (e > nc) e = nc;
    if (e < s) { double t = s; s = e; e = t; } /* substring は swap する（slice と異なる） */
    u32 p0 = akl_str_cp_to_byte(bp, ln, (u32)s);
    u32 p1 = akl_str_cp_to_byte(bp, ln, (u32)e);
    return akl_mkstring(rt, (const char *)bp + p0, p1 - p0);
}
static AklVal akl_m_str_substr(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 si = akl_self_str(rt, self);
    if (si < 0) return akl_native_typeerr(rt, "TypeError: not a string");
    u32 ln;
    const u8 *bp = akl_str(rt, (u32)si, &ln);
    if (rt->err[0]) return akl_mkundefined();
    double nc = (double)akl_str_cp_count(bp, ln);
    double s = argc > 0 ? akl_to_integer(rt, argv[0]) : 0.0;
    if (rt->err[0]) return akl_mkundefined();
    double len = argc > 1 ? akl_to_integer(rt, argv[1]) : nc;
    if (rt->err[0]) return akl_mkundefined();
    if (isnan(s)) s = 0;
    if (isnan(len)) len = 0;
    if (s < 0) s = nc + s;
    if (s < 0) s = 0;
    if (s > nc) s = nc;
    if (len < 0) len = 0;
    double e = s + len;
    if (e > nc) e = nc;
    u32 p0 = akl_str_cp_to_byte(bp, ln, (u32)s);
    u32 p1 = akl_str_cp_to_byte(bp, ln, (u32)e);
    return akl_mkstring(rt, (const char *)bp + p0, p1 - p0);
}
/* ASCII のみの大文字/小文字化（非 ASCII は不変。既知偏差として AKL_COMPAT に明記） */
static AklVal akl_m_str_case(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata, bool upper) {
    (void)argc; (void)argv; (void)udata;
    i32 si = akl_self_str(rt, self);
    if (si < 0) return akl_native_typeerr(rt, "TypeError: not a string");
    u32 ln;
    const u8 *bp = akl_str(rt, (u32)si, &ln);
    if (rt->err[0]) return akl_mkundefined();
    u8 *nb = (u8 *)malloc(ln ? ln : 1);
    if (!nb) { akl_errf(rt, "oom: string case"); return akl_mkundefined(); }
    for (u32 i = 0; i < ln; i++) {
        u8 c = bp[i];
        nb[i] = upper ? (c >= 'a' && c <= 'z' ? (u8)(c - 32) : c)
                      : (c >= 'A' && c <= 'Z' ? (u8)(c + 32) : c);
    }
    rt->gc_sp = rt->gc_sp; /* mkstr の GC に備える（gc_sp は VM が同期済み） */
    u32 oi = akl_mkstr(rt, nb, ln);
    free(nb);
    if (oi == UINT32_MAX) return akl_mkundefined();
    return AKL_MK_OBJ(oi);
}
static AklVal akl_m_str_toUpperCase(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    return akl_m_str_case(rt, self, argc, argv, udata, true);
}
static AklVal akl_m_str_toLowerCase(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    return akl_m_str_case(rt, self, argc, argv, udata, false);
}
static bool akl_is_ascii_ws(u8 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
static AklVal akl_m_str_trim(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)argc; (void)argv; (void)udata;
    i32 si = akl_self_str(rt, self);
    if (si < 0) return akl_native_typeerr(rt, "TypeError: not a string");
    u32 ln;
    const u8 *bp = akl_str(rt, (u32)si, &ln);
    if (rt->err[0]) return akl_mkundefined();
    u32 a = 0, b = ln;
    while (a < b && akl_is_ascii_ws(bp[a])) a++;
    while (b > a && akl_is_ascii_ws(bp[b - 1])) b--;
    return akl_mkstring(rt, (const char *)bp + a, b - a);
}
/* split(sep): 文字列区切りのみ。結果は ARR（要素は新規 STR） */
static AklVal akl_m_str_split(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 si = akl_self_str(rt, self);
    if (si < 0) return akl_native_typeerr(rt, "TypeError: not a string");
    u32 ln;
    const u8 *bp = akl_str(rt, (u32)si, &ln);
    if (rt->err[0]) return akl_mkundefined();
    if (argc == 0 || akl_is_undefined(argv[0])) {
        /* sep なし: [全体] */
        rt->gc_sp = rt->gc_sp;
        u32 oi = akl_obj_new(rt);
        if (oi == UINT32_MAX) return akl_mkundefined();
        rt->objs[oi].kind = AKL_OK_ARR;
        rt->objs[oi].u.arr.v = (AklVal *)malloc(sizeof(AklVal));
        if (!rt->objs[oi].u.arr.v) { rt->objs[oi].kind = 0; akl_errf(rt, "oom: split"); return akl_mkundefined(); }
        u32 st = akl_mkstr(rt, bp, ln);
        if (st == UINT32_MAX) { free(rt->objs[oi].u.arr.v); rt->objs[oi].kind = 0; return akl_mkundefined(); }
        rt->objs[oi].u.arr.v[0] = AKL_MK_OBJ(st);
        rt->objs[oi].u.arr.n = rt->objs[oi].u.arr.cap = 1;
        rt->heap_bytes += sizeof(AklVal);
        return AKL_MK_OBJ(oi);
    }
    if (akl_is_objv(argv[0]) && rt->objs[akl_get_obj(argv[0])].kind == AKL_OK_REGEX) {
        /* 正規表現分割（キャプチャを含める。空マッチはスキップ） */
        AklObj *ro = &rt->objs[akl_get_obj(argv[0])];
        AklRex *rx = ro->u.rex.rx;
        u32 ncap = akl_rex_ncap(rx);
        if (ncap > 32) ncap = 32;
        u32 cap_beg[33], cap_end[33];
        bool lim = false;
        u32 oi = akl_obj_new(rt);
        if (oi == UINT32_MAX) return akl_mkundefined();
        rt->objs[oi].kind = AKL_OK_ARR;
        u32 pos = 0;
        u32 cnt = 0;
        bool any = false;
        for (;;) {
            if (pos >= ln) break;
            bool m = akl_rex_match(rx, bp, ln, pos, cap_beg, cap_end, ncap, &lim);
            if (lim) { akl_errf(rt, "RangeError: regexp execution limit exceeded"); akl_native_throw(rt, rt->err); rt->objs[oi].kind = 0; return akl_mkundefined(); }
            if (!m) break;
            if (cap_beg[0] == cap_end[0]) { /* 空マッチは区切りにしない（位置のみ進める） */
                pos = cap_beg[0] + 1;
                continue;
            }
            any = true;
            u32 e0 = pos, e1 = cap_beg[0];
            if (!akl_arr_grow(rt, &rt->objs[oi], cnt + 1 + ncap)) { rt->objs[oi].kind = 0; return akl_mkundefined(); }
            rt->objs[oi].u.arr.v[cnt++] = akl_mkstring(rt, (const char *)(bp + e0), e1 - e0);
            for (u32 k = 1; k <= ncap; k++) {
                if (cap_beg[k] == UINT32_MAX) rt->objs[oi].u.arr.v[cnt++] = akl_mkundefined();
                else rt->objs[oi].u.arr.v[cnt++] = akl_mkstring(rt, (const char *)(bp + cap_beg[k]), cap_end[k] - cap_beg[k]);
            }
            pos = cap_end[0];
        }
        if (any) {
            if (!akl_arr_grow(rt, &rt->objs[oi], cnt + 1)) { rt->objs[oi].kind = 0; return akl_mkundefined(); }
            rt->objs[oi].u.arr.v[cnt++] = akl_mkstring(rt, (const char *)(bp + pos), ln - pos);
        } else {
            if (!akl_arr_grow(rt, &rt->objs[oi], cnt + 1)) { rt->objs[oi].kind = 0; return akl_mkundefined(); }
            rt->objs[oi].u.arr.v[cnt++] = akl_mkstring(rt, (const char *)bp, ln);
        }
        rt->objs[oi].u.arr.n = cnt;
        return AKL_MK_OBJ(oi);
    }
    u32 sep = akl_to_string(rt, argv[0]);
    if (sep == UINT32_MAX) return akl_mkundefined();
    if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = sep;
    u32 sn;
    const u8 *sp = akl_str(rt, sep, &sn);
    if (rt->err[0]) return akl_mkundefined();
    if (sn == 0) { if (rt->n_nury) rt->n_nury--; return akl_native_typeerr(rt, "TypeError: split with empty separator is not supported"); }
    /* 区切り位置（sep_at）を列挙 → 要素は [start, sep_at[k]) / [sep_at[k]+sn, ...) の交互。
     * 例 'a,b,c' / ',' → sep_at=[1,3]、要素 [0,1)='a', [2,3)='b', [4,5)='c' */
    u32 nparts = 1;
    for (u32 i = 0; i + sn <= ln; ) {
        if (memcmp(bp + i, sp, sn) == 0) { nparts++; i += sn; }
        else i++;
    }
    u32 *sep_at = (u32 *)malloc((u64)(nparts ? nparts : 1) * sizeof(u32));
    if (!sep_at) { akl_errf(rt, "oom: split"); return akl_mkundefined(); }
    u32 m = 0;
    for (u32 i = 0; i + sn <= ln; ) {
        if (memcmp(bp + i, sp, sn) == 0) { sep_at[m++] = i; i += sn; }
        else i++;
    }
    /* ARR を先に作り、nursery でピン → 要素を順に格納（ARR がルートになる） */
    rt->gc_sp = rt->gc_sp;
    u32 oi = akl_obj_new(rt);
    u32 n = m + 1; /* 要素数 = 区切り数 + 1 */
    if (oi == UINT32_MAX) { free(sep_at); return akl_mkundefined(); }
    rt->objs[oi].kind = AKL_OK_ARR;
    rt->objs[oi].u.arr.v = (AklVal *)malloc((u64)n * sizeof(AklVal));
    if (!rt->objs[oi].u.arr.v) { rt->objs[oi].kind = 0; free(sep_at); akl_errf(rt, "oom: split"); return akl_mkundefined(); }
    rt->objs[oi].u.arr.n = rt->objs[oi].u.arr.cap = n;
    rt->heap_bytes += (u64)n * sizeof(AklVal);
    if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = oi; /* ARR ピン */
    bool ok = true;
    u32 start = 0;
    for (u32 i = 0; i < n && ok; i++) {
        u32 end = (i < m) ? sep_at[i] : ln;
        u32 st = akl_mkstr(rt, bp + start, end - start);
        if (st == UINT32_MAX) { ok = false; break; }
        rt->objs[oi].u.arr.v[i] = AKL_MK_OBJ(st);
        if (i < m) start = sep_at[i] + sn;
    }
    free(sep_at);
    if (!ok) { rt->objs[oi].kind = 0; free(rt->objs[oi].u.arr.v); return akl_mkundefined(); }
    return AKL_MK_OBJ(oi);
}
static AklVal akl_m_str_concat(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 si = akl_self_str(rt, self);
    if (si < 0) return akl_native_typeerr(rt, "TypeError: not a string");
    /* 全引数を ToString して長さ合計 → 一括確保 */
    u32 *parts = (u32 *)malloc((u64)(argc + 1) * sizeof(u32));
    if (!parts) { akl_errf(rt, "oom: concat"); return akl_mkundefined(); }
    parts[0] = (u32)si;
    u64 total = 0;
    {
        u32 l0;
        akl_str(rt, (u32)si, &l0);
        if (rt->err[0]) { free(parts); return akl_mkundefined(); }
        total = l0;
    }
    bool ok = true;
    for (int i = 0; i < argc && ok; i++) {
        u32 st = akl_to_string(rt, argv[i]);
        if (st == UINT32_MAX) { ok = false; break; }
        if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = st;
        parts[i + 1] = st;
        total += rt->objs[st].len;
    }
    if (!ok) { free(parts); return akl_mkundefined(); }
    if (total > (u64)rt->heap_mb << 20) { free(parts); akl_errf(rt, "heap bytes budget exhausted"); return akl_mkundefined(); }
    u8 *buf = (u8 *)malloc(total ? total : 1);
    if (!buf) { free(parts); akl_errf(rt, "oom: concat"); return akl_mkundefined(); }
    u32 w = 0;
    for (int i = 0; i <= argc; i++) {
        u32 pl;
        const u8 *pp = akl_str(rt, parts[i], &pl);
        if (rt->err[0]) { free(buf); free(parts); return akl_mkundefined(); }
        memcpy(buf + w, pp, pl);
        w += pl;
    }
    u32 oi = akl_mkstr(rt, buf, w);
    free(buf);
    free(parts);
    if (oi == UINT32_MAX) return akl_mkundefined();
    return AKL_MK_OBJ(oi);
}
static AklVal akl_m_str_includes(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 si = akl_self_str(rt, self);
    if (si < 0) return akl_native_typeerr(rt, "TypeError: not a string");
    AklVal sub = argc > 0 ? argv[0] : akl_mkundefined();
    u32 sidx = akl_to_string(rt, sub);
    if (sidx == UINT32_MAX) return akl_mkundefined();
    if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = sidx;
    double from = argc > 1 ? akl_to_integer(rt, argv[1]) : 0.0;
    if (rt->err[0]) return akl_mkundefined();
    double r = akl_str_index_of(rt, si, (i32)sidx, from);
    if (rt->n_nury) rt->n_nury--;
    return akl_mkbool(r >= 0.0);
}
static AklVal akl_m_str_startsWith(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 si = akl_self_str(rt, self);
    if (si < 0) return akl_native_typeerr(rt, "TypeError: not a string");
    AklVal sub = argc > 0 ? argv[0] : akl_mkundefined();
    u32 sidx = akl_to_string(rt, sub);
    if (sidx == UINT32_MAX) return akl_mkundefined();
    if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = sidx;
    double from = argc > 1 ? akl_to_integer(rt, argv[1]) : 0.0;
    if (rt->err[0]) return akl_mkundefined();
    if (from < 0) from = 0;
    u32 hn, nn;
    const u8 *hp = akl_str(rt, (u32)si, &hn);
    if (rt->err[0]) return akl_mkundefined();
    const u8 *np = akl_str(rt, sidx, &nn);
    if (rt->err[0]) return akl_mkundefined();
    u32 hc = akl_str_cp_count(hp, hn), nc = akl_str_cp_count(np, nn);
    if (from + (double)nc > (double)hc) { if (rt->n_nury) rt->n_nury--; return akl_mkbool(false); }
    u32 p0 = akl_str_cp_to_byte(hp, hn, (u32)from);
    u32 p1 = akl_str_cp_to_byte(hp, hn, (u32)from + nc);
    bool r = (p1 - p0 == nn) && (nn == 0 || memcmp(hp + p0, np, nn) == 0);
    if (rt->n_nury) rt->n_nury--;
    return akl_mkbool(r);
}
static AklVal akl_m_str_endsWith(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 si = akl_self_str(rt, self);
    if (si < 0) return akl_native_typeerr(rt, "TypeError: not a string");
    AklVal sub = argc > 0 ? argv[0] : akl_mkundefined();
    u32 sidx = akl_to_string(rt, sub);
    if (sidx == UINT32_MAX) return akl_mkundefined();
    if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = sidx;
    double end = argc > 1 ? akl_to_integer(rt, argv[1]) : 1.0e18;
    if (rt->err[0]) return akl_mkundefined();
    u32 hn, nn;
    const u8 *hp = akl_str(rt, (u32)si, &hn);
    if (rt->err[0]) return akl_mkundefined();
    const u8 *np = akl_str(rt, sidx, &nn);
    if (rt->err[0]) return akl_mkundefined();
    u32 hc = akl_str_cp_count(hp, hn), nc = akl_str_cp_count(np, nn);
    if (end < 0) end = 0;
    if (end > (double)hc) end = (double)hc;
    if ((double)nc > end) { if (rt->n_nury) rt->n_nury--; return akl_mkbool(false); }
    u32 p0 = akl_str_cp_to_byte(hp, hn, (u32)end - nc);
    u32 p1 = akl_str_cp_to_byte(hp, hn, (u32)end);
    bool r = (p1 - p0 == nn) && (nn == 0 || memcmp(hp + p0, np, nn) == 0);
    if (rt->n_nury) rt->n_nury--;
    return akl_mkbool(r);
}
/* replace(search, repl): 文字列 search の最初の出現を repl に置換（正規表現・$ パターンは非対応） */
static AklVal akl_m_str_replace(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 si = akl_self_str(rt, self);
    if (si < 0) return akl_native_typeerr(rt, "TypeError: not a string");
    if (argc < 1) return akl_native_typeerr(rt, "TypeError: replace requires search string");
    u32 ln;
    const u8 *bp = akl_str(rt, (u32)si, &ln);
    if (rt->err[0]) return akl_mkundefined();
    bool is_regex = akl_is_objv(argv[0]) && rt->objs[akl_get_obj(argv[0])].kind == AKL_OK_REGEX;
    bool fn_repl = argc > 1 && akl_is_objv(argv[1]) &&
                   (rt->objs[akl_get_obj(argv[1])].kind == AKL_OK_FUNC ||
                    rt->objs[akl_get_obj(argv[1])].kind == AKL_OK_NATIVE);
    bool global = is_regex && (rt->objs[akl_get_obj(argv[0])].u.rex.flags & AKL_RX_F_GLOBAL);
    u32 self_idx = (u32)si;
    u8 *out = NULL;
    u32 out_len = 0, out_cap = 0;
    bool oom = false;
#define RX_OUT_APPEND(p, n) do { \
        u32 rxn = (u32)(n); \
        if (rxn) { \
            if ((u64)out_len + rxn > out_cap) { \
                u32 nc = out_cap ? out_cap * 2 : 64; \
                while (nc < out_len + rxn) nc *= 2; \
                if ((u64)nc > (u64)rt->heap_mb << 20) { oom = true; break; } \
                u8 *nb = (u8 *)realloc(out, nc ? nc : 1); \
                if (!nb) { oom = true; break; } \
                out = nb; out_cap = nc; \
            } \
            memcpy(out + out_len, (p), rxn); out_len += rxn; \
        } \
    } while (0)
    u32 search_from = 0;
    u32 prev_end = 0;
    bool any = false;
    u32 cap_beg[33], cap_end[33];
    for (;;) {
        u32 ncap = 0;
        bool m = false;
        bool lim = false;
        if (is_regex) {
            AklObj *ro = &rt->objs[akl_get_obj(argv[0])];
            ncap = akl_rex_ncap(ro->u.rex.rx);
            if (ncap > 32) ncap = 32;
            m = akl_rex_match(ro->u.rex.rx, bp, ln, search_from, cap_beg, cap_end, ncap, &lim);
            if (lim) { free(out); akl_errf(rt, "RangeError: regexp execution limit exceeded"); akl_native_throw(rt, rt->err); return akl_mkundefined(); }
        } else {
            u32 sidx = akl_to_string(rt, argv[0]);
            if (sidx == UINT32_MAX) { free(out); return akl_mkundefined(); }
            u32 sn;
            const u8 *sp = akl_str(rt, sidx, &sn);
            if (rt->err[0]) { free(out); return akl_mkundefined(); }
            if (sn == 0) { m = true; cap_beg[0] = search_from; cap_end[0] = search_from; }
            else {
                for (u32 i = search_from; i + sn <= ln; i++) {
                    if (memcmp(bp + i, sp, sn) == 0) { m = true; cap_beg[0] = i; cap_end[0] = i + sn; break; }
                }
            }
        }
        if (!m) break;
        any = true;
        RX_OUT_APPEND(bp + prev_end, cap_beg[0] - prev_end);
        if (oom) break;
        if (fn_repl) {
            /* replacer(match, p1..pn, offset, string) */
            AklVal args[36];
            u32 na = 0;
            u32 nur0 = rt->n_nury;
            args[na++] = akl_mkstring(rt, (const char *)(bp + cap_beg[0]), cap_end[0] - cap_beg[0]);
            if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = akl_get_obj(args[na - 1]);
            for (u32 k = 1; k <= ncap; k++) {
                if (cap_beg[k] == UINT32_MAX) args[na++] = akl_mkundefined();
                else {
                    args[na++] = akl_mkstring(rt, (const char *)(bp + cap_beg[k]), cap_end[k] - cap_beg[k]);
                    if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = akl_get_obj(args[na - 1]);
                }
            }
            args[na++] = akl_mknum((double)cap_beg[0]);
            args[na++] = AKL_MK_OBJ(self_idx);
            if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = self_idx;
            AklVal rv = AKL_VAL_UNDEF;
            if (!akl_call(rt, argv[1], (int)na, args, &rv)) { free(out); return akl_mkundefined(); }
            rt->n_nury = nur0;
            u32 ridx = akl_to_string(rt, rv);
            if (ridx == UINT32_MAX) { free(out); return akl_mkundefined(); }
            u32 rn;
            const u8 *rp = akl_str(rt, ridx, &rn);
            if (rt->err[0]) { free(out); return akl_mkundefined(); }
            RX_OUT_APPEND(rp, rn);
        } else {
            /* 文字列 replacer（$ 展開: $$ $& $` $' $1..$99） */
            u32 ridx = akl_to_string(rt, argc > 1 ? argv[1] : akl_mkundefined());
            if (ridx == UINT32_MAX) { free(out); return akl_mkundefined(); }
            u32 rn;
            const u8 *rp = akl_str(rt, ridx, &rn);
            if (rt->err[0]) { free(out); return akl_mkundefined(); }
            for (u32 i = 0; i < rn; i++) {
                if (oom) break;
                if (rp[i] != '$' || i + 1 >= rn) { RX_OUT_APPEND(rp + i, 1); continue; }
                u8 c = rp[i + 1];
                if (c == '$') { RX_OUT_APPEND((const u8 *)"$", 1); i++; }
                else if (c == '&') { RX_OUT_APPEND(bp + cap_beg[0], cap_end[0] - cap_beg[0]); i++; }
                else if (c == '`') { RX_OUT_APPEND(bp, cap_beg[0]); i++; }
                else if (c == '\'') { RX_OUT_APPEND(bp + cap_end[0], ln - cap_end[0]); i++; }
                else if (c >= '1' && c <= '9') {
                    u32 g = (u32)(c - '0');
                    u32 used = 1;
                    if (i + 2 < rn && rp[i + 2] >= '0' && rp[i + 2] <= '9') {
                        u32 g2 = g * 10 + (u32)(rp[i + 2] - '0');
                        if (g2 >= 1 && g2 <= ncap) { g = g2; used = 2; }
                    }
                    if (g >= 1 && g <= ncap && cap_beg[g] != UINT32_MAX) {
                        RX_OUT_APPEND(bp + cap_beg[g], cap_end[g] - cap_beg[g]);
                        i += used;
                    } else {
                        RX_OUT_APPEND((const u8 *)"$", 1);
                    }
                } else {
                    RX_OUT_APPEND((const u8 *)"$", 1);
                }
            }
        }
        if (oom) break;
        prev_end = cap_end[0];
        search_from = (cap_end[0] == cap_beg[0]) ? cap_beg[0] + 1 : cap_end[0];
        if (!global) break;
        if (search_from > ln) break;
    }
    if (oom) { free(out); akl_errf(rt, "heap bytes budget exhausted"); return akl_mkundefined(); }
    if (!any) { free(out); return akl_mkstring(rt, (const char *)bp, ln); }
    RX_OUT_APPEND(bp + prev_end, ln - prev_end);
    if (oom) { free(out); akl_errf(rt, "heap bytes budget exhausted"); return akl_mkundefined(); }
    u32 oi = akl_mkstr(rt, out, out_len);
    free(out);
    if (oi == UINT32_MAX) return akl_mkundefined();
    return AKL_MK_OBJ(oi);
#undef RX_OUT_APPEND
}

static AklVal akl_m_str_repeat(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 si = akl_self_str(rt, self);
    if (si < 0) return akl_native_typeerr(rt, "TypeError: not a string");
    double n = argc > 0 ? akl_to_integer(rt, argv[0]) : 0.0;
    if (rt->err[0]) return akl_mkundefined();
    if (n < 0 || isnan(n)) return akl_native_typeerr(rt, "RangeError: negative repeat count");
    if (n > 1.0e7) return akl_native_typeerr(rt, "RangeError: repeat count too large");
    u32 ln;
    const u8 *bp = akl_str(rt, (u32)si, &ln);
    if (rt->err[0]) return akl_mkundefined();
    u64 total = (u64)ln * (u64)n;
    if (total > (u64)rt->heap_mb << 20) { akl_errf(rt, "heap bytes budget exhausted"); return akl_mkundefined(); }
    u8 *buf = (u8 *)malloc(total ? total : 1);
    if (!buf) { akl_errf(rt, "oom: repeat"); return akl_mkundefined(); }
    for (u64 i = 0; i < (u64)n; i++) memcpy(buf + i * ln, bp, ln);
    u32 oi = akl_mkstr(rt, buf, (u32)total);
    free(buf);
    if (oi == UINT32_MAX) return akl_mkundefined();
    return AKL_MK_OBJ(oi);
}


/* ================= RegExp（正規表現） ================= */

/* REGEX オブジェクト生成（rx は所有権移転）。heap_bytes に概算課金（GC の
 * free で o->len を減算）。 */
static AklVal akl_regex_make(AklRT *rt, AklRex *rx, u32 flags) {
    u32 pl = 0;
    akl_rex_pat(rx, &pl);
    u64 sz = 128 + (u64)pl * 24; /* 命令 24B × 概算 */
    if (rt->heap_bytes + sz > (u64)rt->heap_mb << 20) {
        akl_rex_free(rx);
        akl_errf(rt, "heap bytes budget exhausted");
        return akl_mkundefined();
    }
    u32 oi = akl_obj_new(rt);
    if (oi == UINT32_MAX) { akl_rex_free(rx); return akl_mkundefined(); }
    AklObj *o = &rt->objs[oi];
    o->kind = AKL_OK_REGEX;
    o->u.rex.rx = rx;
    o->u.rex.flags = flags;
    o->u.rex.last_index = 0;
    o->len = (u32)(sz > UINT32_MAX ? UINT32_MAX : sz);
    rt->heap_bytes += o->len;
    return AKL_MK_OBJ(oi);
}

/* マッチ試行共通部。成功時 true で cap 配列を埋める。g/y フラグ時 lastIndex を更新 */
static bool akl_regex_try(AklRT *rt, AklObj *o, const u8 *sp, u32 sl,
                          u32 *cap_beg, u32 *cap_end, u32 ncap) {
    AklRex *rx = o->u.rex.rx;
    bool lim = false;
    i32 last = o->u.rex.last_index;
    if (last < 0) last = 0;
    bool m = akl_rex_match(rx, sp, sl, (u32)last, cap_beg, cap_end, ncap, &lim);
    if (lim) {
        akl_errf(rt, "RangeError: regexp execution limit exceeded"); akl_native_throw(rt, rt->err);
        return false;
    }
    if (m && (o->u.rex.flags & (AKL_RX_F_GLOBAL | AKL_RX_F_STICKY))) {
        o->u.rex.last_index = (i32)cap_end[0];
    }
    return m;
}

/* exec の配列組み立て（cap 配列から） */
static AklVal akl_regex_result_arr(AklRT *rt, const u8 *bp, u32 *cap_beg, u32 *cap_end, u32 ncap) {
    u32 oi = akl_obj_new(rt);
    if (oi == UINT32_MAX) return akl_mkundefined();
    rt->objs[oi].kind = AKL_OK_ARR;
    u32 n = ncap + 1;
    if (!akl_arr_grow(rt, &rt->objs[oi], n)) { rt->objs[oi].kind = 0; return akl_mkundefined(); }
    for (u32 k = 0; k <= ncap; k++) {
        if (cap_beg[k] == UINT32_MAX) rt->objs[oi].u.arr.v[k] = akl_mkundefined();
        else rt->objs[oi].u.arr.v[k] = akl_mkstring(rt, (const char *)(bp + cap_beg[k]), cap_end[k] - cap_beg[k]);
    }
    rt->objs[oi].u.arr.n = n;
    return AKL_MK_OBJ(oi);
}

static AklVal akl_m_regex_exec(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    if (!akl_is_objv(self)) return akl_native_typeerr(rt, "TypeError: not a RegExp");
    AklObj *o = &rt->objs[akl_get_obj(self)];
    if (o->kind != AKL_OK_REGEX) return akl_native_typeerr(rt, "TypeError: not a RegExp");
    u32 sidx = akl_to_string(rt, argc > 0 ? argv[0] : akl_mkundefined());
    if (sidx == UINT32_MAX) return akl_mkundefined();
    if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = sidx;
    u32 sl;
    const u8 *sp = akl_str(rt, sidx, &sl);
    if (rt->err[0]) return akl_mkundefined();
    u32 ncap = akl_rex_ncap(o->u.rex.rx);
    if (ncap > 32) ncap = 32;
    u32 cap_beg[33], cap_end[33];
    bool m = akl_regex_try(rt, o, sp, sl, cap_beg, cap_end, ncap);
    if (rt->err[0]) return akl_mkundefined();
    if (!m) return AKL_VAL_NULL;
    return akl_regex_result_arr(rt, sp, cap_beg, cap_end, ncap);
}

static AklVal akl_m_regex_test(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    if (!akl_is_objv(self)) return akl_native_typeerr(rt, "TypeError: not a RegExp");
    AklObj *o = &rt->objs[akl_get_obj(self)];
    if (o->kind != AKL_OK_REGEX) return akl_native_typeerr(rt, "TypeError: not a RegExp");
    u32 sidx = akl_to_string(rt, argc > 0 ? argv[0] : akl_mkundefined());
    if (sidx == UINT32_MAX) return akl_mkundefined();
    if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = sidx;
    u32 sl;
    const u8 *sp = akl_str(rt, sidx, &sl);
    if (rt->err[0]) return akl_mkundefined();
    u32 ncap = akl_rex_ncap(o->u.rex.rx);
    if (ncap > 32) ncap = 32;
    u32 cap_beg[33], cap_end[33];
    bool m = akl_regex_try(rt, o, sp, sl, cap_beg, cap_end, ncap);
    if (rt->err[0]) return akl_mkundefined();
    return akl_mkbool(m);
}

static AklVal akl_m_regex_toString(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata; (void)argc; (void)argv;
    if (!akl_is_objv(self)) return akl_native_typeerr(rt, "TypeError: not a RegExp");
    AklObj *o = &rt->objs[akl_get_obj(self)];
    if (o->kind != AKL_OK_REGEX) return akl_native_typeerr(rt, "TypeError: not a RegExp");
    u32 pl = 0;
    const u8 *pp = akl_rex_pat(o->u.rex.rx, &pl);
    char flags[8];
    u32 fw = 0;
    u32 f = o->u.rex.flags;
    if (f & AKL_RX_F_GLOBAL) flags[fw++] = 'g';
    if (f & AKL_RX_F_IGNORE) flags[fw++] = 'i';
    if (f & AKL_RX_F_MULTI) flags[fw++] = 'm';
    if (f & AKL_RX_F_DOTALL) flags[fw++] = 's';
    if (f & AKL_RX_F_UNICODE) flags[fw++] = 'u';
    if (f & AKL_RX_F_STICKY) flags[fw++] = 'y';
    flags[fw] = 0;
    u64 total = (u64)pl + 2 + fw;
    if (total > (u64)rt->heap_mb << 20) { akl_errf(rt, "heap bytes budget exhausted"); return akl_mkundefined(); }
    u8 *buf = (u8 *)malloc(total ? total : 1);
    if (!buf) { akl_errf(rt, "oom: regexp toString"); return akl_mkundefined(); }
    u32 w = 0;
    buf[w++] = '/';
    memcpy(buf + w, pp, pl); w += pl;
    buf[w++] = '/';
    memcpy(buf + w, flags, fw); w += fw;
    u32 oi = akl_mkstr(rt, buf, w);
    free(buf);
    if (oi == UINT32_MAX) return akl_mkundefined();
    return AKL_MK_OBJ(oi);
}

static const AklMethEntry AKL_REGEX_METHODS[AKL_REGEX_METH_N] = {
    {"test", akl_m_regex_test}, {"exec", akl_m_regex_exec},
    {"toString", akl_m_regex_toString}
};

/* グローバル RegExp コンストラクタ（new でも呼び出しでも同じ） */
static AklVal akl_m_regexp_ctor(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata; (void)self;
    u32 flags = 0;
    bool have_flags = false;
    if (argc > 1 && !akl_is_undefined(argv[1])) {
        u32 fidx = akl_to_string(rt, argv[1]);
        if (fidx == UINT32_MAX) return akl_mkundefined();
        u32 fl;
        const u8 *fp = akl_str(rt, fidx, &fl);
        if (rt->err[0]) return akl_mkundefined();
        for (u32 i = 0; i < fl; i++) {
            u32 bit;
            switch (fp[i]) {
            case 'i': bit = AKL_RX_F_IGNORE; break;
            case 'g': bit = AKL_RX_F_GLOBAL; break;
            case 'm': bit = AKL_RX_F_MULTI; break;
            case 's': bit = AKL_RX_F_DOTALL; break;
            case 'u': bit = AKL_RX_F_UNICODE; break;
            case 'y': bit = AKL_RX_F_STICKY; break;
            default: return akl_native_typeerr(rt, "SyntaxError: invalid regexp flags");
            }
            if (flags & bit) return akl_native_typeerr(rt, "SyntaxError: duplicate regexp flag");
            flags |= bit;
        }
        have_flags = true;
    }
    const u8 *pp = NULL;
    u32 pn = 0;
    if (argc > 0 && akl_is_objv(argv[0]) && rt->objs[akl_get_obj(argv[0])].kind == AKL_OK_REGEX) {
        AklObj *ro = &rt->objs[akl_get_obj(argv[0])];
        if (!have_flags) {
            /* RegExp オブジェクトの複製 */
            u32 sl = 0;
            const u8 *sr = akl_rex_pat(ro->u.rex.rx, &sl);
            char rerr[96];
            AklRex *rx = akl_rex_compile(sr, sl, ro->u.rex.flags, rerr, sizeof rerr);
            if (!rx) { akl_errf(rt, "SyntaxError: %s", rerr); akl_native_throw(rt, rt->err); return akl_mkundefined(); }
            return akl_regex_make(rt, rx, ro->u.rex.flags);
        }
        pp = akl_rex_pat(ro->u.rex.rx, &pn);
    } else {
        u32 sidx = akl_to_string(rt, argc > 0 ? argv[0] : akl_mkundefined());
        if (sidx == UINT32_MAX) return akl_mkundefined();
        pp = akl_str(rt, sidx, &pn);
        if (rt->err[0]) return akl_mkundefined();
    }
    char rerr[96];
    AklRex *rx = akl_rex_compile(pp, pn, flags, rerr, sizeof rerr);
    if (!rx) { akl_errf(rt, "SyntaxError: %s", rerr); akl_native_throw(rt, rt->err); return akl_mkundefined(); }
    return akl_regex_make(rt, rx, flags);
}

/* String.prototype.match（正規表現。g フラグで全マッチ配列、非 g でキャプチャ配列。
 * マッチなしは null。index/input プロパティは非対応（AKL_COMPAT に明記）） */
static AklVal akl_m_str_match(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 si = akl_self_str(rt, self);
    if (si < 0) return akl_native_typeerr(rt, "TypeError: not a string");
    u32 ln;
    const u8 *bp = akl_str(rt, (u32)si, &ln);
    if (rt->err[0]) return akl_mkundefined();
    AklRex *rx = NULL;
    u32 rflags = 0;
    bool own = false;
    if (argc > 0 && akl_is_objv(argv[0]) && rt->objs[akl_get_obj(argv[0])].kind == AKL_OK_REGEX) {
        AklObj *ro = &rt->objs[akl_get_obj(argv[0])];
        rx = ro->u.rex.rx;
        rflags = ro->u.rex.flags;
    } else {
        u32 sidx = akl_to_string(rt, argc > 0 ? argv[0] : akl_mkundefined());
        if (sidx == UINT32_MAX) return akl_mkundefined();
        u32 sn;
        const u8 *sp = akl_str(rt, sidx, &sn);
        if (rt->err[0]) return akl_mkundefined();
        char rerr[96];
        rx = akl_rex_compile(sp, sn, 0, rerr, sizeof rerr);
        if (!rx) { akl_errf(rt, "SyntaxError: %s", rerr); akl_native_throw(rt, rt->err); return akl_mkundefined(); }
        own = true;
    }
    u32 cap_beg[33], cap_end[33];
    bool lim = false;
    if (rflags & AKL_RX_F_GLOBAL) {
        u32 oi = akl_obj_new(rt);
        if (oi == UINT32_MAX) { if (own) akl_rex_free(rx); return akl_mkundefined(); }
        rt->objs[oi].kind = AKL_OK_ARR;
        u32 pos = 0;
        u32 cnt = 0;
        for (;;) {
            bool m = akl_rex_match(rx, bp, ln, pos, cap_beg, cap_end, 0, &lim);
            if (lim) { akl_errf(rt, "RangeError: regexp execution limit exceeded"); akl_native_throw(rt, rt->err); if (own) akl_rex_free(rx); rt->objs[oi].kind = 0; return akl_mkundefined(); }
            if (!m) break;
            if (!akl_arr_grow(rt, &rt->objs[oi], cnt + 1)) { if (own) akl_rex_free(rx); rt->objs[oi].kind = 0; return akl_mkundefined(); }
            rt->objs[oi].u.arr.v[cnt++] = akl_mkstring(rt, (const char *)(bp + cap_beg[0]), cap_end[0] - cap_beg[0]);
            pos = (cap_end[0] == pos) ? cap_end[0] + 1 : cap_end[0];
            if (pos > ln) break;
        }
        rt->objs[oi].u.arr.n = cnt;
        if (own) akl_rex_free(rx);
        if (cnt == 0) return AKL_VAL_NULL;
        return AKL_MK_OBJ(oi);
    }
    u32 ncap = akl_rex_ncap(rx);
    if (ncap > 32) ncap = 32;
    bool m = akl_rex_match(rx, bp, ln, 0, cap_beg, cap_end, ncap, &lim);
    if (own) akl_rex_free(rx);
    if (lim) { akl_errf(rt, "RangeError: regexp execution limit exceeded"); akl_native_throw(rt, rt->err); return akl_mkundefined(); }
    if (!m) return AKL_VAL_NULL;
    return akl_regex_result_arr(rt, bp, cap_beg, cap_end, ncap);
}

/* String.prototype.search: 最初のマッチ位置。無ければ -1 */
static AklVal akl_m_str_search(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 si = akl_self_str(rt, self);
    if (si < 0) return akl_native_typeerr(rt, "TypeError: not a string");
    u32 ln;
    const u8 *bp = akl_str(rt, (u32)si, &ln);
    if (rt->err[0]) return akl_mkundefined();
    AklRex *rx = NULL;
    bool own = false;
    if (argc > 0 && akl_is_objv(argv[0]) && rt->objs[akl_get_obj(argv[0])].kind == AKL_OK_REGEX) {
        AklObj *ro = &rt->objs[akl_get_obj(argv[0])];
        rx = ro->u.rex.rx;
    } else {
        u32 sidx = akl_to_string(rt, argc > 0 ? argv[0] : akl_mkundefined());
        if (sidx == UINT32_MAX) return akl_mkundefined();
        u32 sn;
        const u8 *sp = akl_str(rt, sidx, &sn);
        if (rt->err[0]) return akl_mkundefined();
        char rerr[96];
        rx = akl_rex_compile(sp, sn, 0, rerr, sizeof rerr);
        if (!rx) { akl_errf(rt, "SyntaxError: %s", rerr); akl_native_throw(rt, rt->err); return akl_mkundefined(); }
        own = true;
    }
    u32 cap_beg[2], cap_end[2];
    bool lim = false;
    bool m = akl_rex_match(rx, bp, ln, 0, cap_beg, cap_end, 0, &lim);
    if (own) akl_rex_free(rx);
    if (lim) { akl_errf(rt, "RangeError: regexp execution limit exceeded"); akl_native_throw(rt, rt->err); return akl_mkundefined(); }
    return akl_mknum(m ? (double)cap_beg[0] : -1.0);
}

static const AklMethEntry AKL_STR_METHODS[AKL_STR_METH_N] = {
    {"charAt", akl_m_str_charAt}, {"charCodeAt", akl_m_str_charCodeAt},
    {"codePointAt", akl_m_str_codePointAt}, {"indexOf", akl_m_str_indexOf},
    {"lastIndexOf", akl_m_str_lastIndexOf}, {"slice", akl_m_str_slice},
    {"substring", akl_m_str_substring}, {"substr", akl_m_str_substr},
    {"toUpperCase", akl_m_str_toUpperCase}, {"toLowerCase", akl_m_str_toLowerCase},
    {"trim", akl_m_str_trim}, {"split", akl_m_str_split},
    {"concat", akl_m_str_concat}, {"includes", akl_m_str_includes},
    {"startsWith", akl_m_str_startsWith}, {"endsWith", akl_m_str_endsWith},
    {"replace", akl_m_str_replace}, {"repeat", akl_m_str_repeat},
    {"match", akl_m_str_match}, {"search", akl_m_str_search}
};


/* ================= 配列メソッド ================= */
/* ARR を成長させる共通ヘルパ（budget は heap_mb が壁） */
static bool akl_arr_grow(AklRT *rt, AklObj *o, u32 need) {
    if (need <= o->u.arr.cap) return true;
    u64 needb = (u64)need * sizeof(AklVal);
    if (needb > (u64)rt->heap_mb << 20) { akl_errf(rt, "heap bytes budget exhausted"); return false; }
    u32 ncap = o->u.arr.cap ? o->u.arr.cap * 2 : 4;
    u32 lim = (u32)(((u64)rt->heap_mb << 20) / sizeof(AklVal));
    while (ncap < need) ncap *= 2;
    if (ncap > lim) ncap = lim;
    if (ncap < need) { akl_errf(rt, "heap bytes budget exhausted"); return false; }
    AklVal *nv = (AklVal *)realloc(o->u.arr.v, (u64)ncap * sizeof(AklVal));
    if (!nv) { akl_errf(rt, "oom: array grow"); return false; }
    rt->heap_bytes += (u64)(ncap - o->u.arr.cap) * sizeof(AklVal);
    o->u.arr.v = nv;
    o->u.arr.cap = ncap;
    return true;
}
static AklVal akl_m_arr_push(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 ai = akl_self_arr(rt, self);
    if (ai < 0) return akl_native_typeerr(rt, "TypeError: not an array");
    AklObj *o = &rt->objs[(u32)ai];
    u32 need = o->u.arr.n + (u32)argc;
    if (!akl_arr_grow(rt, o, need)) return akl_mkundefined();
    for (int i = 0; i < argc; i++) o->u.arr.v[o->u.arr.n++] = argv[i];
    return akl_mknum((double)o->u.arr.n);
}
static AklVal akl_m_arr_pop(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)argc; (void)argv; (void)udata;
    i32 ai = akl_self_arr(rt, self);
    if (ai < 0) return akl_native_typeerr(rt, "TypeError: not an array");
    AklObj *o = &rt->objs[(u32)ai];
    if (o->u.arr.n == 0) return akl_mkundefined();
    return o->u.arr.v[--o->u.arr.n];
}
static AklVal akl_m_arr_shift(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)argc; (void)argv; (void)udata;
    i32 ai = akl_self_arr(rt, self);
    if (ai < 0) return akl_native_typeerr(rt, "TypeError: not an array");
    AklObj *o = &rt->objs[(u32)ai];
    if (o->u.arr.n == 0) return akl_mkundefined();
    AklVal r = o->u.arr.v[0];
    for (u32 i = 1; i < o->u.arr.n; i++) o->u.arr.v[i - 1] = o->u.arr.v[i];
    o->u.arr.n--;
    return r;
}
static AklVal akl_m_arr_unshift(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 ai = akl_self_arr(rt, self);
    if (ai < 0) return akl_native_typeerr(rt, "TypeError: not an array");
    AklObj *o = &rt->objs[(u32)ai];
    u32 need = o->u.arr.n + (u32)argc;
    if (!akl_arr_grow(rt, o, need)) return akl_mkundefined();
    for (u32 i = o->u.arr.n; i-- > 0;) o->u.arr.v[i + (u32)argc] = o->u.arr.v[i];
    for (int i = 0; i < argc; i++) o->u.arr.v[i] = argv[i];
    o->u.arr.n = need;
    return akl_mknum((double)need);
}
static AklVal akl_m_arr_join(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 ai = akl_self_arr(rt, self);
    if (ai < 0) return akl_native_typeerr(rt, "TypeError: not an array");
    AklObj *o = &rt->objs[(u32)ai];
    u32 sep_len = 0;
    const u8 *sep = (const u8 *)",";
    if (argc > 0 && !akl_is_undefined(argv[0])) {
        u32 sidx = akl_to_string(rt, argv[0]);
        if (sidx == UINT32_MAX) return akl_mkundefined();
        if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = sidx;
        sep = akl_str(rt, sidx, &sep_len);
        if (rt->err[0]) return akl_mkundefined();
    } else sep_len = 1;
    u64 total = 0;
    u32 n = o->u.arr.n;
    u32 *parts = (u32 *)malloc((u64)(n ? n : 1) * sizeof(u32));
    if (!parts) { akl_errf(rt, "oom: join"); return akl_mkundefined(); }
    for (u32 i = 0; i < n; i++) {
        u32 si = akl_to_string(rt, o->u.arr.v[i]);
        if (si == UINT32_MAX) { free(parts); return akl_mkundefined(); }
        if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = si;
        parts[i] = si;
        total += rt->objs[si].len;
        if (i + 1 < n) total += sep_len;
    }
    if (total > (u64)rt->heap_mb << 20) { free(parts); akl_errf(rt, "heap bytes budget exhausted"); return akl_mkundefined(); }
    u8 *buf = (u8 *)malloc(total ? total : 1);
    if (!buf) { free(parts); akl_errf(rt, "oom: join"); return akl_mkundefined(); }
    u32 w = 0;
    for (u32 i = 0; i < n; i++) {
        u32 pl;
        const u8 *pp = akl_str(rt, parts[i], &pl);
        if (rt->err[0]) { free(buf); free(parts); return akl_mkundefined(); }
        memcpy(buf + w, pp, pl);
        w += pl;
        if (i + 1 < n) { memcpy(buf + w, sep, sep_len); w += sep_len; }
    }
    free(parts);
    u32 oi = akl_mkstr(rt, buf, w);
    free(buf);
    if (oi == UINT32_MAX) return akl_mkundefined();
    return AKL_MK_OBJ(oi);
}
static AklVal akl_m_arr_concat(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 ai = akl_self_arr(rt, self);
    if (ai < 0) return akl_native_typeerr(rt, "TypeError: not an array");
    AklObj *o = &rt->objs[(u32)ai];
    u64 need = o->u.arr.n;
    for (int i = 0; i < argc; i++) {
        if (akl_self_arr(rt, argv[i]) >= 0) need += rt->objs[akl_get_obj(argv[i])].u.arr.n;
        else need += 1;
    }
    if (need > (u64)rt->heap_mb << 20) { akl_errf(rt, "heap bytes budget exhausted"); return akl_mkundefined(); }
    rt->gc_sp = rt->gc_sp;
    u32 oi = akl_obj_new(rt);
    if (oi == UINT32_MAX) return akl_mkundefined();
    rt->objs[oi].kind = AKL_OK_ARR;
    rt->objs[oi].u.arr.v = (AklVal *)malloc((need ? need : 1) * sizeof(AklVal));
    if (!rt->objs[oi].u.arr.v) { rt->objs[oi].kind = 0; akl_errf(rt, "oom: concat"); return akl_mkundefined(); }
    rt->objs[oi].u.arr.cap = (u32)need;
    if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = oi;
    u32 w = 0;
    for (u32 i = 0; i < o->u.arr.n; i++) rt->objs[oi].u.arr.v[w++] = o->u.arr.v[i];
    for (int i = 0; i < argc; i++) {
        i32 x = akl_self_arr(rt, argv[i]);
        if (x >= 0) {
            AklObj *xo = &rt->objs[(u32)x];
            for (u32 k = 0; k < xo->u.arr.n; k++) rt->objs[oi].u.arr.v[w++] = xo->u.arr.v[k];
        } else rt->objs[oi].u.arr.v[w++] = argv[i];
    }
    rt->objs[oi].u.arr.n = w;
    rt->heap_bytes += (u64)w * sizeof(AklVal);
    return AKL_MK_OBJ(oi);
}
static AklVal akl_m_arr_slice(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 ai = akl_self_arr(rt, self);
    if (ai < 0) return akl_native_typeerr(rt, "TypeError: not an array");
    AklObj *o = &rt->objs[(u32)ai];
    double nc = (double)o->u.arr.n;
    double s = argc > 0 ? akl_to_integer(rt, argv[0]) : 0.0;
    if (rt->err[0]) return akl_mkundefined();
    double e = argc > 1 ? akl_to_integer(rt, argv[1]) : nc;
    if (rt->err[0]) return akl_mkundefined();
    if (isnan(s)) s = 0;
    if (isnan(e)) e = 0;
    if (s < 0) s = nc + s;
    if (e < 0) e = nc + e;
    if (s < 0) s = 0;
    if (e < 0) e = 0;
    if (s > nc) s = nc;
    if (e > nc) e = nc;
    if (e < s) e = s;
    u32 cnt = (u32)(e - s);
    rt->gc_sp = rt->gc_sp;
    u32 oi = akl_obj_new(rt);
    if (oi == UINT32_MAX) return akl_mkundefined();
    rt->objs[oi].kind = AKL_OK_ARR;
    if (cnt) {
        rt->objs[oi].u.arr.v = (AklVal *)malloc((u64)cnt * sizeof(AklVal));
        if (!rt->objs[oi].u.arr.v) { rt->objs[oi].kind = 0; akl_errf(rt, "oom: slice"); return akl_mkundefined(); }
        for (u32 i = 0; i < cnt; i++) rt->objs[oi].u.arr.v[i] = o->u.arr.v[(u32)s + i];
        rt->objs[oi].u.arr.n = rt->objs[oi].u.arr.cap = cnt;
        rt->heap_bytes += (u64)cnt * sizeof(AklVal);
    }
    return AKL_MK_OBJ(oi);
}
/* 要素の strict 等価スキャン（-1 = 不在） */
static i32 akl_arr_index_of(AklRT *rt, AklObj *o, AklVal needle) {
    for (u32 i = 0; i < o->u.arr.n; i++)
        if (akl_strict_eq(rt, o->u.arr.v[i], needle)) return (i32)i;
    return -1;
}
static AklVal akl_m_arr_indexOf(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 ai = akl_self_arr(rt, self);
    if (ai < 0) return akl_native_typeerr(rt, "TypeError: not an array");
    if (argc == 0) return akl_mknum(-1.0);
    return akl_mknum((double)akl_arr_index_of(rt, &rt->objs[(u32)ai], argv[0]));
}
static AklVal akl_m_arr_includes(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 ai = akl_self_arr(rt, self);
    if (ai < 0) return akl_native_typeerr(rt, "TypeError: not an array");
    if (argc == 0) return akl_mkbool(false);
    return akl_mkbool(akl_arr_index_of(rt, &rt->objs[(u32)ai], argv[0]) >= 0);
}
static AklVal akl_m_arr_toString(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    return akl_m_arr_join(rt, self, argc, argv, udata);
}
static AklVal akl_m_arr_reverse(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)argc; (void)argv; (void)udata;
    i32 ai = akl_self_arr(rt, self);
    if (ai < 0) return akl_native_typeerr(rt, "TypeError: not an array");
    AklObj *o = &rt->objs[(u32)ai];
    for (u32 i = 0, j = o->u.arr.n; i + 1 < j; i++, j--) {
        AklVal t = o->u.arr.v[i];
        o->u.arr.v[i] = o->u.arr.v[j - 1];
        o->u.arr.v[j - 1] = t;
    }
    return self;
}
static AklVal akl_m_arr_lastIndexOf(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 ai = akl_self_arr(rt, self);
    if (ai < 0) return akl_native_typeerr(rt, "TypeError: not an array");
    if (argc == 0) return akl_mknum(-1.0);
    AklObj *o = &rt->objs[(u32)ai];
    for (u32 i = o->u.arr.n; i-- > 0;)
        if (akl_strict_eq(rt, o->u.arr.v[i], argv[0])) return akl_mknum((double)i);
    return akl_mknum(-1.0);
}



/* ================= v0.4: 高階関数（VM 再入 akl_call 経由） =================
 * コールバック呼び出し規約: fn(element, index, array)。this は undefined。
 * reduce のみ fn(acc, element, index, array)。
 * コールバック内の例外・budget 枯渇は akl_call が false を返し、native_throw で
 * eval 失敗に倒れる（fail-stop 粒度 = メソッド呼び出し全体）。 */

/* 呼び出し可能（FUNC / NATIVE）なら true */
static bool akl_is_callable(AklRT *rt, AklVal v) {
    if (!akl_is_objv(v)) return false;
    u32 oi = akl_get_obj(v);
    if (oi >= rt->n_objs) return false;
    u8 k = rt->objs[oi].kind;
    return k == AKL_OK_FUNC || k == AKL_OK_NATIVE;
}

/* 新規 ARR を作り nursery でピンして返す（要素書込中も GC から保護される）。
 * 成功で *oi に obj index、*outv に要素バッファ（n 要素分確保済み）。 */
static AklVal akl_hof_newarr(AklRT *rt, u32 n, u32 *oi, AklVal **outv) {
    *outv = NULL;
    u32 o = akl_obj_new(rt);
    if (o == UINT32_MAX) return akl_mkundefined();
    rt->objs[o].kind = AKL_OK_ARR;
    if (n) {
        rt->objs[o].u.arr.v = (AklVal *)malloc((u64)n * sizeof(AklVal));
        if (!rt->objs[o].u.arr.v) {
            rt->objs[o].kind = 0;
            akl_errf(rt, "oom: array");
            return akl_mkundefined();
        }
        rt->objs[o].u.arr.cap = n;
        rt->heap_bytes += (u64)n * sizeof(AklVal);
        *outv = rt->objs[o].u.arr.v;
    }
    if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = o;
    *oi = o;
    return AKL_MK_OBJ(o);
}
/* 失敗後始末: 配列スロットを解放（kind=0 で GC から外す） */
static void akl_hof_abort(AklRT *rt, u32 oi) {
    if (oi != UINT32_MAX && oi < rt->n_objs && rt->objs[oi].kind == AKL_OK_ARR) {
        free(rt->objs[oi].u.arr.v);
        rt->objs[oi].kind = 0;
    }
}



static AklVal akl_m_arr_map(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 ai = akl_self_arr(rt, self);
    if (ai < 0) return akl_native_typeerr(rt, "TypeError: not an array");
    if (argc < 1 || !akl_is_callable(rt, argv[0]))
        return akl_native_typeerr(rt, "TypeError: map requires a function");
    AklVal fn = argv[0];
    AklObj *o = &rt->objs[(u32)ai];
    u32 n = o->u.arr.n;
    u32 nur0 = rt->n_nury;
    u32 oi = UINT32_MAX;
    AklVal *outv = NULL;
    AklVal res = akl_hof_newarr(rt, n, &oi, &outv);
    if (akl_is_undefined(res) && rt->err[0]) { rt->n_nury = nur0; return akl_mkundefined(); }
    bool ok = true;
    for (u32 i = 0; i < n; i++) {
        AklVal args[3];
        args[0] = o->u.arr.v[i];
        args[1] = akl_mknum((double)i);
        args[2] = self;
        AklVal r;
        if (!akl_call(rt, fn, 3, args, &r)) {
            ok = false; break;
        }
        outv[i] = r;
        rt->objs[oi].u.arr.n = i + 1; /* GC マーク範囲を進める（途中発火対策） */
    }
    rt->n_nury = nur0;
    if (!ok) {
        akl_hof_abort(rt, oi);
        if (rt->err[0]) akl_native_throw(rt, rt->err);
        return akl_mkundefined();
    }
    rt->objs[oi].u.arr.n = n;
    return res;
}

static AklVal akl_m_arr_filter(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 ai = akl_self_arr(rt, self);
    if (ai < 0) return akl_native_typeerr(rt, "TypeError: not an array");
    if (argc < 1 || !akl_is_callable(rt, argv[0]))
        return akl_native_typeerr(rt, "TypeError: filter requires a function");
    AklVal fn = argv[0];
    AklObj *o = &rt->objs[(u32)ai];
    u32 n = o->u.arr.n;
    u32 nur0 = rt->n_nury;
    u32 oi = UINT32_MAX;
    AklVal *outv = NULL;
    AklVal res = akl_hof_newarr(rt, n, &oi, &outv);
    if (akl_is_undefined(res) && rt->err[0]) { rt->n_nury = nur0; return akl_mkundefined(); }
    bool ok = true;
    u32 w = 0;
    for (u32 i = 0; i < n; i++) {
        AklVal args[3];
        args[0] = o->u.arr.v[i];
        args[1] = akl_mknum((double)i);
        args[2] = self;
        AklVal r;
        if (!akl_call(rt, fn, 3, args, &r)) { ok = false; break; }
        if (akl_truthy(rt, r)) outv[w++] = o->u.arr.v[i];
        rt->objs[oi].u.arr.n = w;
    }
    rt->n_nury = nur0;
    if (!ok) {
        akl_hof_abort(rt, oi);
        if (rt->err[0]) akl_native_throw(rt, rt->err);
        return akl_mkundefined();
    }
    rt->objs[oi].u.arr.n = w;
    return res;
}

static AklVal akl_m_arr_forEach(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 ai = akl_self_arr(rt, self);
    if (ai < 0) return akl_native_typeerr(rt, "TypeError: not an array");
    if (argc < 1 || !akl_is_callable(rt, argv[0]))
        return akl_native_typeerr(rt, "TypeError: forEach requires a function");
    AklVal fn = argv[0];
    AklObj *o = &rt->objs[(u32)ai];
    u32 n = o->u.arr.n;
    u32 nur0 = rt->n_nury;
    for (u32 i = 0; i < n; i++) {
        AklVal args[3];
        args[0] = o->u.arr.v[i];
        args[1] = akl_mknum((double)i);
        args[2] = self;
        AklVal r;
        if (!akl_call(rt, fn, 3, args, &r)) {
            rt->n_nury = nur0;
            if (rt->err[0]) akl_native_throw(rt, rt->err);
            return akl_mkundefined();
        }
    }
    rt->n_nury = nur0;
    return akl_mkundefined();
}

/* some / every / find / findIndex の共通スキャン。mode: 0=some 1=every 2=find 3=findIndex */
static AklVal akl_m_arr_scan(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata, u8 mode) {
    (void)udata;
    i32 ai = akl_self_arr(rt, self);
    if (ai < 0) return akl_native_typeerr(rt, "TypeError: not an array");
    if (argc < 1 || !akl_is_callable(rt, argv[0]))
        return akl_native_typeerr(rt, "TypeError: requires a function");
    AklVal fn = argv[0];
    AklObj *o = &rt->objs[(u32)ai];
    u32 n = o->u.arr.n;
    u32 nur0 = rt->n_nury;
    for (u32 i = 0; i < n; i++) {
        AklVal args[3];
        args[0] = o->u.arr.v[i];
        args[1] = akl_mknum((double)i);
        args[2] = self;
        AklVal r;
        if (!akl_call(rt, fn, 3, args, &r)) {
            rt->n_nury = nur0;
            if (rt->err[0]) akl_native_throw(rt, rt->err);
            return akl_mkundefined();
        }
        bool t = akl_truthy(rt, r);
        if (mode == 0 && t) { rt->n_nury = nur0; return akl_mkbool(true); }   /* some */
        if (mode == 1 && !t) { rt->n_nury = nur0; return akl_mkbool(false); }  /* every */
        if (mode == 2 && t) { rt->n_nury = nur0; return o->u.arr.v[i]; }       /* find */
        if (mode == 3 && t) { rt->n_nury = nur0; return akl_mknum((double)i); } /* findIndex */
    }
    rt->n_nury = nur0;
    if (mode == 0) return akl_mkbool(false);
    if (mode == 1) return akl_mkbool(true);
    if (mode == 2) return akl_mkundefined();
    return akl_mknum(-1.0);
}
static AklVal akl_m_arr_some(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    return akl_m_arr_scan(rt, self, argc, argv, udata, 0);
}
static AklVal akl_m_arr_every(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    return akl_m_arr_scan(rt, self, argc, argv, udata, 1);
}
static AklVal akl_m_arr_find(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    return akl_m_arr_scan(rt, self, argc, argv, udata, 2);
}
static AklVal akl_m_arr_findIndex(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    return akl_m_arr_scan(rt, self, argc, argv, udata, 3);
}

static AklVal akl_m_arr_reduce(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata;
    i32 ai = akl_self_arr(rt, self);
    if (ai < 0) return akl_native_typeerr(rt, "TypeError: not an array");
    if (argc < 1 || !akl_is_callable(rt, argv[0]))
        return akl_native_typeerr(rt, "TypeError: reduce requires a function");
    AklVal fn = argv[0];
    AklObj *o = &rt->objs[(u32)ai];
    u32 n = o->u.arr.n;
    u32 nur0 = rt->n_nury;
    AklVal acc;
    u32 start;
    if (argc >= 2) {
        acc = argv[1];
        start = 0;
    } else {
        if (n == 0) {
            rt->n_nury = nur0;
            akl_native_throw(rt, "TypeError: reduce of empty array with no initial value");
            return akl_mkundefined();
        }
        acc = o->u.arr.v[0];
        start = 1;
    }
    for (u32 i = start; i < n; i++) {
        AklVal args[4];
        args[0] = acc;
        args[1] = o->u.arr.v[i];
        args[2] = akl_mknum((double)i);
        args[3] = self;
        AklVal r;
        if (!akl_call(rt, fn, 4, args, &r)) {
            rt->n_nury = nur0;
            if (rt->err[0]) akl_native_throw(rt, rt->err);
            return akl_mkundefined();
        }
        acc = r;
    }
    rt->n_nury = nur0;
    return acc;
}

/* ================= JSON ================= */
/* 循環検出用の深さ上限（JSON は木構造。深さ 128 で打ち切り = 明白な失敗） */
#define AKL_JSON_DEPTH 128

typedef struct {
    u8 *p;
    u32 n, cap;
    AklRT *rt;
} JsonBuf;

static bool jb_grow(JsonBuf *b, u32 need) {
    if (b->n + need <= b->cap) return true;
    u32 nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->n + need) nc *= 2;
    u8 *np = (u8 *)realloc(b->p, nc);
    if (!np) { akl_errf(b->rt, "oom: json"); return false; }
    b->p = np;
    b->cap = nc;
    return true;
}
static bool jb_put(JsonBuf *b, const char *s) {
    u32 l = (u32)strlen(s);
    if (!jb_grow(b, l)) return false;
    memcpy(b->p + b->n, s, l);
    b->n += l;
    return true;
}
static bool jb_putn(JsonBuf *b, const u8 *s, u32 l) {
    if (!jb_grow(b, l)) return false;
    memcpy(b->p + b->n, s, l);
    b->n += l;
    return true;
}
/* 文字列の JSON エスケープ（" \ control は \uXXXX） */
static bool jb_str(JsonBuf *b, const u8 *s, u32 l) {
    if (!jb_put(b, "\"")) return false;
    for (u32 i = 0; i < l; i++) {
        u8 c = s[i];
        if (c == '"' || c == '\\') {
            char t[2] = { '\\', (char)c };
            if (!jb_putn(b, (const u8 *)t, 2)) return false;
        } else if (c < 0x20) {
            char t[8];
            int n = snprintf(t, sizeof t, "\\u%04x", c);
            if (!jb_putn(b, (const u8 *)t, (u32)n)) return false;
        } else {
            if (!jb_putn(b, &c, 1)) return false;
        }
    }
    return jb_put(b, "\"");
}
/* 数値の JSON 形式（JS ToString とほぼ同一。整数は小数点なし） */
static bool jb_num(JsonBuf *b, double d) {
    char tmp[40];
    int n;
    if (isnan(d) || isinf(d)) return jb_put(b, "null"); /* JSON 非対応値 */
    if (d == floor(d) && fabs(d) < 1e21) {
        n = snprintf(tmp, sizeof tmp, "%.0f", d);
    } else {
        n = 0;
        for (int prec = 15; prec <= 17; prec++) {
            n = snprintf(tmp, sizeof tmp, "%.*g", prec, d);
            if (strtod(tmp, NULL) == d) break;
        }
        for (int i = 0; i + 3 < n; i++) {
            if (tmp[i] == 'e' && (tmp[i + 1] == '+' || tmp[i + 1] == '-') && tmp[i + 2] == '0' &&
                tmp[i + 3] >= '0' && tmp[i + 3] <= '9') {
                memmove(tmp + i + 2, tmp + i + 3, (u32)(n - (i + 2)));
                n--;
                break;
            }
        }
    }
    return jb_putn(b, (const u8 *)tmp, (u32)n);
}

static bool jb_value(JsonBuf *b, AklVal v, u32 depth) {
    if (depth > AKL_JSON_DEPTH) { akl_errf(b->rt, "TypeError: JSON stringify depth exceeded"); return false; }
    double d;
    bool bl;
    uint32_t ln;
    if (v == AKL_VAL_NULL) return jb_put(b, "null");
    if (v == AKL_VAL_TRUE) return jb_put(b, "true");
    if (v == AKL_VAL_FALSE) return jb_put(b, "false");
    if (akl_numv(v, &d)) return jb_num(b, d);
    if (akl_as_bool(v, &bl)) return jb_put(b, bl ? "true" : "false");
    if (!akl_is_objv(v)) return jb_put(b, "null"); /* undefined 等 */
    u32 oi = akl_get_obj(v);
    u8 k = b->rt->objs[oi].kind;
    if (k == AKL_OK_STR || k == AKL_OK_ROPE) {
        const u8 *sp = akl_str(b->rt, oi, &ln);
        if (b->rt->err[0]) return false;
        return jb_str(b, sp, ln);
    }
    if (k == AKL_OK_ARR) {
        AklObj *o = &b->rt->objs[oi];
        if (!jb_put(b, "[")) return false;
        for (u32 i = 0; i < o->u.arr.n; i++) {
            if (i) if (!jb_put(b, ",")) return false;
            AklVal ev = o->u.arr.v[i];
            if (akl_is_undefined(ev)) { if (!jb_put(b, "null")) return false; }
            else if (!jb_value(b, ev, depth + 1)) return false;
        }
        return jb_put(b, "]");
    }
    if (k == AKL_OK_OBJ) {
        AklObj *o = &b->rt->objs[oi];
        if (!jb_put(b, "{")) return false;
        for (u32 i = 0; i < o->u.po.n; i++) {
            if (i) if (!jb_put(b, ",")) return false;
            u32 kl;
            const u8 *kp = akl_str(b->rt, o->u.po.props[i].name, &kl);
            if (b->rt->err[0]) return false;
            if (!jb_str(b, kp, kl)) return false;
            if (!jb_put(b, ":")) return false;
            if (!jb_value(b, o->u.po.props[i].v, depth + 1)) return false;
        }
        return jb_put(b, "}");
    }
    return jb_put(b, "null"); /* function / native / handle は null */
}

static AklVal akl_m_json_stringify(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)self; (void)udata;
    if (argc == 0 || akl_is_undefined(argv[0]) ||
        (akl_is_objv(argv[0]) && rt->objs[akl_get_obj(argv[0])].kind == AKL_OK_FUNC)) {
        return akl_mkundefined(); /* トップレベル function/undefined は undefined */
    }
    JsonBuf b;
    memset(&b, 0, sizeof b);
    b.rt = rt;
    if (!jb_value(&b, argv[0], 0)) {
        free(b.p);
        if (rt->err[0]) akl_native_throw(rt, rt->err); /* 深さ超過等を eval 失敗に */
        return akl_mkundefined();
    }
    u32 oi = akl_mkstr(rt, b.p, b.n);
    free(b.p);
    if (oi == UINT32_MAX) return akl_mkundefined();
    return AKL_MK_OBJ(oi);
}

/* ---- JSON.parse の再帰下降 ---- */
typedef struct {
    const u8 *s;
    u32 n, pos;
    AklRT *rt;
} JsonP;

static void jp_ws(JsonP *p) {
    while (p->pos < p->n) {
        u8 c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}
static AklVal jp_value(JsonP *p, u32 depth);
static bool jp_lit(JsonP *p, const char *lit) {
    u32 l = (u32)strlen(lit);
    if (p->pos + l > p->n || memcmp(p->s + p->pos, lit, l) != 0) return false;
    p->pos += l;
    return true;
}
static bool jp_hex4(JsonP *p, u32 *out) {
    if (p->pos + 4 > p->n) return false;
    u32 v = 0;
    for (u32 i = 0; i < 4; i++) {
        u8 c = p->s[p->pos + i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = v * 16 + (u32)d;
    }
    p->pos += 4;
    *out = v;
    return true;
}
/* 文字列（"..." エスケープ込み。UTF-8 出力） */
static bool jp_string(JsonP *p, u32 *out_idx, bool do_intern) {
    if (p->pos >= p->n || p->s[p->pos] != '"') return false;
    p->pos++;
    u8 *buf = (u8 *)malloc(64);
    if (!buf) { akl_errf(p->rt, "oom: json parse"); return false; }
    u32 bl = 0, cap = 64;
    bool ok = false;
    while (p->pos < p->n) {
        u8 c = p->s[p->pos++];
        if (c == '"') { ok = true; break; }
        if (c == '\\') {
            if (p->pos >= p->n) break;
            u8 e = p->s[p->pos++];
            u32 cp;
            switch (e) {
            case '"': cp = '"'; break;
            case '\\': cp = '\\'; break;
            case '/': cp = '/'; break;
            case 'b': cp = 8; break;
            case 'f': cp = 12; break;
            case 'n': cp = 10; break;
            case 'r': cp = 13; break;
            case 't': cp = 9; break;
            case 'u': {
                u32 h;
                if (!jp_hex4(p, &h)) { ok = false; break; }
                cp = h;
                ok = true; /* エスケープ成功（continue まで ok を true に保つ） */
                break;
            }
            default: ok = false; break;
            }
            if (!ok) break;
            if (bl + 4 > cap) {
                cap *= 2;
                u8 *nb = (u8 *)realloc(buf, cap);
                if (!nb) { free(buf); akl_errf(p->rt, "oom: json parse"); return false; }
                buf = nb;
            }
            if (cp < 0x80) buf[bl++] = (u8)cp;
            else if (cp < 0x800) {
                buf[bl++] = (u8)(0xC0 | (cp >> 6));
                buf[bl++] = (u8)(0x80 | (cp & 63));
            } else if (cp < 0x10000) {
                buf[bl++] = (u8)(0xE0 | (cp >> 12));
                buf[bl++] = (u8)(0x80 | ((cp >> 6) & 63));
                buf[bl++] = (u8)(0x80 | (cp & 63));
            } else {
                buf[bl++] = (u8)(0xF0 | (cp >> 18));
                buf[bl++] = (u8)(0x80 | ((cp >> 12) & 63));
                buf[bl++] = (u8)(0x80 | ((cp >> 6) & 63));
                buf[bl++] = (u8)(0x80 | (cp & 63));
            }
            continue; /* エスケープ処理完了。次の文字へ（ok はまだ false なので break しない） */
        } else if (c < 0x20) {
            break;
        } else {
            if (bl + 1 > cap) {
                cap *= 2;
                u8 *nb = (u8 *)realloc(buf, cap);
                if (!nb) { free(buf); akl_errf(p->rt, "oom: json parse"); return false; }
                buf = nb;
            }
            buf[bl++] = c;
        }
    }
    if (!ok) { free(buf); return false; }
    /* キー（do_intern=true）は intern 済みで作る: akl_mkstr で uninterned な STR を
     * 作ると、akl_intern の線形走査が「先頭優先」でそれを既存と誤認し、コンパイル時
     * intern の STR と別の id になる（実測で .key が undefined に化ける）。 */
    u32 oi;
    if (do_intern) oi = akl_intern(p->rt, buf, bl, NULL);
    else oi = akl_mkstr(p->rt, buf, bl);
    free(buf);
    if (oi == UINT32_MAX) return false;
    *out_idx = oi;
    return true;
}
static bool jp_num(JsonP *p, AklVal *out) {
    u32 st = p->pos;
    if (p->pos < p->n && p->s[p->pos] == '-') p->pos++;
    u32 intst = p->pos; /* 整数部の開始（符号の後） */
    bool any = false;
    while (p->pos < p->n && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') { p->pos++; any = true; }
    if (any && p->pos - intst > 1 && p->s[intst] == '0') { p->pos = st; return false; } /* 01 / -01 は JSON 不正 */
    if (p->pos < p->n && p->s[p->pos] == '.') {
        p->pos++;
        while (p->pos < p->n && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') p->pos++;
    }
    if (p->pos < p->n && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->n && (p->s[p->pos] == '+' || p->s[p->pos] == '-')) p->pos++;
        while (p->pos < p->n && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') p->pos++;
    }
    if (!any) { p->pos = st; return false; }
    char tmp[64];
    u32 l = p->pos - st;
    if (l >= sizeof tmp) { p->pos = st; return false; }
    memcpy(tmp, p->s + st, l);
    tmp[l] = 0;
    *out = akl_mknum(strtod(tmp, NULL));
    return true;
}
static AklVal jp_value(JsonP *p, u32 depth) {
    if (depth > AKL_JSON_DEPTH) { akl_errf(p->rt, "SyntaxError: json depth exceeded"); return AKL_VAL_UNDEF; }
    jp_ws(p);
    if (p->pos >= p->n) return AKL_VAL_UNDEF;
    u8 c = p->s[p->pos];
    if (c == '{') {
        p->pos++;
        u32 oi = akl_obj_new(p->rt);
        if (oi == UINT32_MAX) return AKL_VAL_UNDEF;
        p->rt->objs[oi].kind = AKL_OK_OBJ;
        AklVal obj = AKL_MK_OBJ(oi);
        if (p->rt->n_nury < AKL_NURY_CAP) p->rt->nury[p->rt->n_nury++] = oi;
        jp_ws(p);
        if (p->pos < p->n && p->s[p->pos] == '}') { p->pos++; return obj; }
        for (;;) {
            jp_ws(p);
            u32 ki;
            if (!jp_string(p, &ki, true)) { akl_errf(p->rt, "SyntaxError: invalid json"); return AKL_VAL_UNDEF; }
            if (p->rt->n_nury < AKL_NURY_CAP) p->rt->nury[p->rt->n_nury++] = ki;
            /* キーは intern 必須（PLOAD は intern idx で照合する。未 intern のまま
             * prop_set すると名前不一致になり .k が undefined になる — 実測で特定） */
            u32 klen;
            const u8 *kp = akl_str(p->rt, ki, &klen);
            if (p->rt->err[0]) return AKL_VAL_UNDEF;
            u32 kni = akl_intern(p->rt, kp, klen, NULL);
            if (kni == UINT32_MAX) { akl_errf(p->rt, "oom: json parse"); return AKL_VAL_UNDEF; }
            if (p->rt->n_nury < AKL_NURY_CAP) p->rt->nury[p->rt->n_nury++] = kni;
            jp_ws(p);
            if (p->pos >= p->n || p->s[p->pos] != ':') { akl_errf(p->rt, "SyntaxError: invalid json"); return AKL_VAL_UNDEF; }
            p->pos++;
            AklVal v = jp_value(p, depth + 1);
            if (p->rt->err[0]) return AKL_VAL_UNDEF;
            if (!obj_prop_set(p->rt, &p->rt->objs[oi], kni, v)) return AKL_VAL_UNDEF;
            jp_ws(p);
            if (p->pos < p->n && p->s[p->pos] == ',') { p->pos++; continue; }
            if (p->pos < p->n && p->s[p->pos] == '}') { p->pos++; return obj; }
            akl_errf(p->rt, "SyntaxError: invalid json");
            return AKL_VAL_UNDEF;
        }
    }
    if (c == '[') {
        p->pos++;
        u32 oi = akl_obj_new(p->rt);
        if (oi == UINT32_MAX) return AKL_VAL_UNDEF;
        p->rt->objs[oi].kind = AKL_OK_ARR;
        AklVal arr = AKL_MK_OBJ(oi);
        if (p->rt->n_nury < AKL_NURY_CAP) p->rt->nury[p->rt->n_nury++] = oi;
        jp_ws(p);
        if (p->pos < p->n && p->s[p->pos] == ']') { p->pos++; return arr; }
        for (;;) {
            jp_ws(p);
            AklVal v = jp_value(p, depth + 1);
            if (p->rt->err[0]) return AKL_VAL_UNDEF;
            AklObj *ao = &p->rt->objs[oi];
            u32 need = ao->u.arr.n + 1;
            if (need > ao->u.arr.cap) {
                u32 nc = ao->u.arr.cap ? ao->u.arr.cap * 2 : 4;
                AklVal *nv = (AklVal *)realloc(ao->u.arr.v, (u64)nc * sizeof(AklVal));
                if (!nv) { akl_errf(p->rt, "oom: json parse"); return AKL_VAL_UNDEF; }
                p->rt->heap_bytes += (u64)(nc - ao->u.arr.cap) * sizeof(AklVal);
                ao->u.arr.v = nv;
                ao->u.arr.cap = nc;
            }
            ao->u.arr.v[ao->u.arr.n++] = v;
            jp_ws(p);
            if (p->pos < p->n && p->s[p->pos] == ',') { p->pos++; continue; }
            if (p->pos < p->n && p->s[p->pos] == ']') { p->pos++; return arr; }
            akl_errf(p->rt, "SyntaxError: invalid json");
            return AKL_VAL_UNDEF;
        }
    }
    if (c == '"') {
        u32 si;
        if (!jp_string(p, &si, false)) { akl_errf(p->rt, "SyntaxError: invalid json"); return AKL_VAL_UNDEF; }
        return AKL_MK_OBJ(si);
    }
    if (c == 't' && jp_lit(p, "true")) return AKL_VAL_TRUE;
    if (c == 'f' && jp_lit(p, "false")) return AKL_VAL_FALSE;
    if (c == 'n' && jp_lit(p, "null")) return AKL_VAL_NULL;
    {
        AklVal nv;
        if (jp_num(p, &nv)) return nv;
    }
    akl_errf(p->rt, "SyntaxError: invalid json");
    return AKL_VAL_UNDEF;
}

static AklVal akl_m_json_parse(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)self; (void)udata;
    if (argc == 0) { akl_native_throw(rt, "TypeError: JSON.parse requires a string"); return akl_mkundefined(); }
    u32 si = akl_to_string(rt, argv[0]);
    if (si == UINT32_MAX) return akl_mkundefined();
    u32 ln;
    const u8 *sp = akl_str(rt, si, &ln);
    if (rt->err[0]) return akl_mkundefined();
    JsonP p;
    p.s = sp;
    p.n = ln;
    p.pos = 0;
    p.rt = rt;
    AklVal v = jp_value(&p, 0);
    if (rt->err[0]) {
        akl_native_throw(rt, rt->err); /* 不正 JSON は「明白に失敗」（undefined を黙って返さない） */
        return akl_mkundefined();
    }
    jp_ws(&p);
    if (p.pos != p.n || akl_is_undefined(v)) {
        akl_native_throw(rt, "SyntaxError: invalid json"); /* 空入力・値なし・trailing */
        return akl_mkundefined();
    }
    return v;
}


/* ================= グローバル関数 ================= */
/* JS parseInt: 空白除去 → 符号 → radix 進数（radix 0/省略 = 自動: 0x で 16 進） */
static const AklMethEntry AKL_ARR_METHODS[AKL_ARR_METH_N] = {
    {"push", akl_m_arr_push}, {"pop", akl_m_arr_pop},
    {"shift", akl_m_arr_shift}, {"unshift", akl_m_arr_unshift},
    {"join", akl_m_arr_join}, {"concat", akl_m_arr_concat},
    {"slice", akl_m_arr_slice}, {"indexOf", akl_m_arr_indexOf},
    {"includes", akl_m_arr_includes}, {"toString", akl_m_arr_toString},
    {"reverse", akl_m_arr_reverse}, {"lastIndexOf", akl_m_arr_lastIndexOf},
    /* v0.4: 高階（VM 再入 akl_call 経由） */
    {"map", akl_m_arr_map}, {"filter", akl_m_arr_filter},
    {"forEach", akl_m_arr_forEach}, {"some", akl_m_arr_some},
    {"every", akl_m_arr_every}, {"find", akl_m_arr_find},
    {"findIndex", akl_m_arr_findIndex}, {"reduce", akl_m_arr_reduce}
};


static AklVal akl_m_parseInt(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)self; (void)udata;
    if (argc == 0) return akl_mknum(NAN);
    u32 sidx = akl_to_string(rt, argv[0]);
    if (sidx == UINT32_MAX) return akl_mkundefined();
    u32 ln;
    const u8 *bp = akl_str(rt, sidx, &ln);
    if (rt->err[0]) return akl_mkundefined();
    u32 i = 0;
    while (i < ln && akl_is_ascii_ws(bp[i])) i++;
    bool neg = false;
    if (i < ln && (bp[i] == '+' || bp[i] == '-')) { neg = bp[i] == '-'; i++; }
    int radix = 10;
    bool auto_radix = true;
    if (argc > 1 && !akl_is_undefined(argv[1])) {
        double rv = akl_to_number(rt, argv[1]);
        if (rt->err[0]) return akl_mkundefined();
        if (isnan(rv)) { auto_radix = true; }
        else {
            radix = (int)trunc(rv);
            auto_radix = radix == 0;
            if (!auto_radix && (radix < 2 || radix > 36)) return akl_mknum(NAN);
        }
    }
    if (auto_radix && i + 1 < ln && bp[i] == '0' && (bp[i + 1] == 'x' || bp[i + 1] == 'X')) {
        radix = 16;
        i += 2;
    }
    if (radix == 16 && i + 1 < ln && bp[i] == '0' && (bp[i + 1] == 'x' || bp[i + 1] == 'X')) {
        i += 2;
    }
    if (i >= ln) return akl_mknum(NAN);
    double acc = 0.0;
    bool any = false;
    while (i < ln) {
        u8 c = bp[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;
        if (d >= radix) break;
        acc = acc * radix + d;
        any = true;
        i++;
    }
    if (!any) return akl_mknum(NAN);
    return akl_mknum(neg ? -acc : acc);
}
static AklVal akl_m_parseFloat(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)self; (void)udata;
    if (argc == 0) return akl_mknum(NAN);
    u32 sidx = akl_to_string(rt, argv[0]);
    if (sidx == UINT32_MAX) return akl_mkundefined();
    u32 ln;
    const u8 *bp = akl_str(rt, sidx, &ln);
    if (rt->err[0]) return akl_mkundefined();
    u32 i = 0;
    while (i < ln && akl_is_ascii_ws(bp[i])) i++;
    u32 st = i;
    bool neg = false;
    if (i < ln && (bp[i] == '+' || bp[i] == '-')) { neg = bp[i] == '-'; i++; }
    u32 ds = i;
    while (i < ln && bp[i] >= '0' && bp[i] <= '9') i++;
    if (i < ln && bp[i] == '.') {
        i++;
        while (i < ln && bp[i] >= '0' && bp[i] <= '9') i++;
    }
    if (i == ds) return akl_mknum(NAN);
    if (i < ln && (bp[i] == 'e' || bp[i] == 'E')) {
        u32 es = i;
        i++;
        if (i < ln && (bp[i] == '+' || bp[i] == '-')) i++;
        u32 ed = i;
        while (i < ln && bp[i] >= '0' && bp[i] <= '9') i++;
        if (i == ed) i = es; /* 指数なし → e は含めない */
    }
    char tmp[64];
    u32 tl = i - st;
    if (tl >= sizeof tmp) tl = sizeof tmp - 1;
    memcpy(tmp, bp + st, tl);
    tmp[tl] = 0;
    return akl_mknum(strtod(tmp, NULL) * (neg ? -1.0 : 1.0));
}
static AklVal akl_m_isNaN(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)self; (void)udata;
    if (argc == 0) return akl_mkbool(true);
    double d = akl_to_number(rt, argv[0]);
    if (rt->err[0]) return akl_mkundefined();
    return akl_mkbool(isnan(d));
}
static AklVal akl_m_isFinite(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)self; (void)udata;
    if (argc == 0) return akl_mkbool(false);
    double d = akl_to_number(rt, argv[0]);
    if (rt->err[0]) return akl_mkundefined();
    return akl_mkbool(!isnan(d) && !isinf(d));
}

/* ================= Math ================= */
static AklVal akl_m_math_abs(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)self; (void)udata;
    double d = argc > 0 ? akl_to_number(rt, argv[0]) : NAN;
    if (rt->err[0]) return akl_mkundefined();
    return akl_mknum(fabs(d));
}
#define AKL_MATH1(NAME, EXPR) \
static AklVal NAME(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) { \
    (void)self; (void)udata; \
    double d = argc > 0 ? akl_to_number(rt, argv[0]) : NAN; \
    if (rt->err[0]) return akl_mkundefined(); \
    return akl_mknum(EXPR); \
}
AKL_MATH1(akl_m_math_floor, floor(d))
AKL_MATH1(akl_m_math_ceil, ceil(d))
AKL_MATH1(akl_m_math_round, floor(d + 0.5))
AKL_MATH1(akl_m_math_trunc, trunc(d))
AKL_MATH1(akl_m_math_sqrt, sqrt(d))
AKL_MATH1(akl_m_math_cbrt, cbrt(d))
AKL_MATH1(akl_m_math_exp, exp(d))
AKL_MATH1(akl_m_math_log, log(d))
AKL_MATH1(akl_m_math_log2, log2(d))
AKL_MATH1(akl_m_math_log10, log10(d))
AKL_MATH1(akl_m_math_sin, sin(d))
AKL_MATH1(akl_m_math_cos, cos(d))
AKL_MATH1(akl_m_math_tan, tan(d))
AKL_MATH1(akl_m_math_asin, asin(d))
AKL_MATH1(akl_m_math_acos, acos(d))
AKL_MATH1(akl_m_math_atan, atan(d))
AKL_MATH1(akl_m_math_sign, (d > 0 ? 1.0 : d < 0 ? -1.0 : d))
#undef AKL_MATH1
static AklVal akl_m_math_pow(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)self; (void)udata;
    double a = argc > 0 ? akl_to_number(rt, argv[0]) : NAN;
    double b = argc > 1 ? akl_to_number(rt, argv[1]) : NAN;
    if (rt->err[0]) return akl_mkundefined();
    return akl_mknum(pow(a, b));
}
static AklVal akl_m_math_atan2(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)self; (void)udata;
    double a = argc > 0 ? akl_to_number(rt, argv[0]) : NAN;
    double b = argc > 1 ? akl_to_number(rt, argv[1]) : NAN;
    if (rt->err[0]) return akl_mkundefined();
    return akl_mknum(atan2(a, b));
}
static AklVal akl_m_math_min(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)self; (void)udata;
    double r = argc ? akl_to_number(rt, argv[0]) : INFINITY;
    for (int i = 1; i < argc; i++) {
        double d = akl_to_number(rt, argv[i]);
        if (isnan(d)) return akl_mknum(NAN);
        if (d < r) r = d;
    }
    return akl_mknum(r);
}
static AklVal akl_m_math_max(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)self; (void)udata;
    double r = argc ? akl_to_number(rt, argv[0]) : -INFINITY;
    for (int i = 1; i < argc; i++) {
        double d = akl_to_number(rt, argv[i]);
        if (isnan(d)) return akl_mknum(NAN);
        if (d > r) r = d;
    }
    return akl_mknum(r);
}
static AklVal akl_m_math_hypot(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)self; (void)udata;
    double s = 0.0;
    for (int i = 0; i < argc; i++) {
        double d = akl_to_number(rt, argv[i]);
        if (isinf(d)) return akl_mknum(INFINITY);
        s += d * d;
    }
    return akl_mknum(sqrt(s));
}
static AklVal akl_m_math_random(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)self; (void)argc; (void)argv; (void)udata;
    u64 x = 0;
    if (getrandom(&x, sizeof x, 0) != (ssize_t)sizeof x) { /* 非対応カーネルは時間系にフォールバック */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        x = (u64)ts.tv_nsec ^ ((u64)ts.tv_sec << 21) ^ (u64)(uintptr_t)rt;
    }
    return akl_mknum((double)(x >> 11) / 9007199254740992.0);
}

/* ================= 登録 ================= */
static AklVal akl_builtin_mkmath(AklRT *rt) {
    u32 oi = akl_obj_new(rt);
    if (oi == UINT32_MAX) return AKL_VAL_UNDEF;
    rt->objs[oi].kind = AKL_OK_OBJ;
    AklVal obj = AKL_MK_OBJ(oi);
    if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = oi;
    struct { const char *n; AklNativeFn f; } fn[] = {
        {"abs", akl_m_math_abs}, {"floor", akl_m_math_floor}, {"ceil", akl_m_math_ceil},
        {"round", akl_m_math_round}, {"trunc", akl_m_math_trunc}, {"sqrt", akl_m_math_sqrt},
        {"cbrt", akl_m_math_cbrt}, {"pow", akl_m_math_pow}, {"exp", akl_m_math_exp},
        {"log", akl_m_math_log}, {"log2", akl_m_math_log2}, {"log10", akl_m_math_log10},
        {"min", akl_m_math_min}, {"max", akl_m_math_max}, {"hypot", akl_m_math_hypot},
        {"random", akl_m_math_random}, {"sign", akl_m_math_sign},
        {"sin", akl_m_math_sin}, {"cos", akl_m_math_cos}, {"tan", akl_m_math_tan},
        {"asin", akl_m_math_asin}, {"acos", akl_m_math_acos}, {"atan", akl_m_math_atan},
        {"atan2", akl_m_math_atan2}
    };
    for (u32 i = 0; i < sizeof fn / sizeof fn[0]; i++) {
        u32 nm = akl_intern(rt, (const u8 *)fn[i].n, (u32)strlen(fn[i].n), NULL);
        if (nm == UINT32_MAX) return AKL_VAL_UNDEF;
        AklVal fv = akl_mknative(rt, fn[i].f, NULL);
        if (!obj_prop_set(rt, &rt->objs[oi], nm, fv)) return AKL_VAL_UNDEF;
    }
    struct { const char *n; double v; } cn[] = {
        {"PI", 3.141592653589793}, {"E", 2.718281828459045},
        {"LN2", 0.6931471805599453}, {"LN10", 2.302585092994046},
        {"LOG2E", 1.4426950408889634}, {"LOG10E", 0.4342944819032518},
        {"SQRT1_2", 0.7071067811865476}, {"SQRT2", 1.4142135623730951}
    };
    for (u32 i = 0; i < sizeof cn / sizeof cn[0]; i++) {
        u32 nm = akl_intern(rt, (const u8 *)cn[i].n, (u32)strlen(cn[i].n), NULL);
        if (nm == UINT32_MAX) return AKL_VAL_UNDEF;
        if (!obj_prop_set(rt, &rt->objs[oi], nm, akl_mknum(cn[i].v))) return AKL_VAL_UNDEF;
    }
    return obj;
}

/* akl_new から呼ばれる組込登録（VM 停止中・gc_live==false 前提） */

/* ================= Object / Array / String / Number / Boolean 組込 ================= */

/* Object.keys(obj): キー名の配列 */
static AklVal akl_m_obj_keys(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata; (void)self;
    AklVal tgt = argc > 0 ? argv[0] : akl_mkundefined();
    u32 oi = akl_obj_new(rt);
    if (oi == UINT32_MAX) return akl_mkundefined();
    rt->objs[oi].kind = AKL_OK_ARR;
    u32 cnt = 0;
    if (akl_is_objv(tgt)) {
        AklObj *o = &rt->objs[akl_get_obj(tgt)];
        if (o->kind == AKL_OK_OBJ) {
            for (u32 i = 0; i < o->u.po.n; i++) {
                if (!akl_arr_grow(rt, &rt->objs[oi], cnt + 1)) { rt->objs[oi].kind = 0; return akl_mkundefined(); }
                rt->objs[oi].u.arr.v[cnt++] = AKL_MK_OBJ(o->u.po.props[i].name);
            }
        }
    }
    rt->objs[oi].u.arr.n = cnt;
    return AKL_MK_OBJ(oi);
}

/* Object.values(obj): 値の配列 */
static AklVal akl_m_obj_values(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata; (void)self;
    AklVal tgt = argc > 0 ? argv[0] : akl_mkundefined();
    u32 oi = akl_obj_new(rt);
    if (oi == UINT32_MAX) return akl_mkundefined();
    rt->objs[oi].kind = AKL_OK_ARR;
    u32 cnt = 0;
    if (akl_is_objv(tgt)) {
        AklObj *o = &rt->objs[akl_get_obj(tgt)];
        if (o->kind == AKL_OK_OBJ) {
            for (u32 i = 0; i < o->u.po.n; i++) {
                if (!akl_arr_grow(rt, &rt->objs[oi], cnt + 1)) { rt->objs[oi].kind = 0; return akl_mkundefined(); }
                rt->objs[oi].u.arr.v[cnt++] = o->u.po.props[i].v;
            }
        }
    }
    rt->objs[oi].u.arr.n = cnt;
    return AKL_MK_OBJ(oi);
}

/* Object.entries(obj): [key, value] ペアの配列 */
static AklVal akl_m_obj_entries(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata; (void)self;
    AklVal tgt = argc > 0 ? argv[0] : akl_mkundefined();
    u32 oi = akl_obj_new(rt);
    if (oi == UINT32_MAX) return akl_mkundefined();
    rt->objs[oi].kind = AKL_OK_ARR;
    u32 cnt = 0;
    if (akl_is_objv(tgt)) {
        AklObj *o = &rt->objs[akl_get_obj(tgt)];
        if (o->kind == AKL_OK_OBJ) {
            for (u32 i = 0; i < o->u.po.n; i++) {
                if (!akl_arr_grow(rt, &rt->objs[oi], cnt + 1)) { rt->objs[oi].kind = 0; return akl_mkundefined(); }
                u32 po = akl_obj_new(rt);
                if (po == UINT32_MAX) { rt->objs[oi].kind = 0; return akl_mkundefined(); }
                rt->objs[po].kind = AKL_OK_ARR;
                if (!akl_arr_grow(rt, &rt->objs[po], 2)) { rt->objs[oi].kind = 0; rt->objs[po].kind = 0; return akl_mkundefined(); }
                rt->objs[po].u.arr.v[0] = AKL_MK_OBJ(o->u.po.props[i].name);
                rt->objs[po].u.arr.v[1] = o->u.po.props[i].v;
                rt->objs[po].u.arr.n = 2;
                rt->objs[oi].u.arr.v[cnt++] = AKL_MK_OBJ(po);
            }
        }
    }
    rt->objs[oi].u.arr.n = cnt;
    return AKL_MK_OBJ(oi);
}

/* Object.assign(tgt, ...srcs): src の列挙プロパティを tgt へコピー（後勝ち） */
static AklVal akl_m_obj_assign(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata; (void)self;
    AklVal tgt = argc > 0 ? argv[0] : akl_mkundefined();
    if (!akl_is_objv(tgt) || rt->objs[akl_get_obj(tgt)].kind != AKL_OK_OBJ)
        return akl_native_typeerr(rt, "TypeError: Object.assign target must be an object");
    AklObj *to = &rt->objs[akl_get_obj(tgt)];
    for (int a = 1; a < argc; a++) {
        AklVal sv = argv[a];
        if (!akl_is_objv(sv)) continue; /* undefined/null は無視（JS 同様） */
        AklObj *so = &rt->objs[akl_get_obj(sv)];
        if (so->kind != AKL_OK_OBJ) continue;
        for (u32 i = 0; i < so->u.po.n; i++) {
            if (to->u.po.n >= AKL_OBJ_MAX_PROPS) {
                akl_errf(rt, "object property limit exceeded");
                return akl_mkundefined();
            }
            if (!obj_prop_set(rt, to, so->u.po.props[i].name, so->u.po.props[i].v))
                return akl_mkundefined();
        }
    }
    return tgt;
}

/* Object.create(proto): 新規オブジェクト（prototype 連鎖は非対応。値はコピーしない） */
static AklVal akl_m_obj_create(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata; (void)self;
    AklVal proto = argc > 0 ? argv[0] : akl_mkundefined();
    if (!akl_is_objv(proto) && !akl_is_null(proto))
        return akl_native_typeerr(rt, "TypeError: Object.create prototype must be an object or null");
    u32 oi = akl_obj_new(rt);
    if (oi == UINT32_MAX) return akl_mkundefined();
    rt->objs[oi].kind = AKL_OK_OBJ;
    return AKL_MK_OBJ(oi);
}

/* Array.isArray(x) */
static AklVal akl_m_arr_isArray(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata; (void)self;
    bool is = argc > 0 && akl_is_objv(argv[0]) && rt->objs[akl_get_obj(argv[0])].kind == AKL_OK_ARR;
    return akl_mkbool(is);
}

/* String(x): 文字列化（new String も同じ値。ラッパーオブジェクトは簡易近似） */
static AklVal akl_m_String(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata; (void)self;
    u32 sidx = akl_to_string(rt, argc > 0 ? argv[0] : akl_mkundefined());
    if (sidx == UINT32_MAX) return akl_mkundefined();
    return AKL_MK_OBJ(sidx);
}

/* Number(x): 数値化（NaN は canonical） */
static AklVal akl_m_Number(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata; (void)self;
    if (argc == 0) return akl_mknum(0.0);
    double d = akl_to_number(rt, argv[0]);
    if (rt->err[0]) return akl_mkundefined();
    return akl_mknum(d);
}

/* Boolean(x): 真理値化（JS ToBoolean: falsy は false/0/''/null/undefined/NaN） */
static AklVal akl_m_Boolean(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)udata; (void)self;
    if (argc == 0) return AKL_VAL_FALSE;
    return akl_truthy(rt, argv[0]) ? AKL_VAL_TRUE : AKL_VAL_FALSE;
}

static bool akl_builtins_install(AklRT *rt) {
    u32 n_nan = akl_mkstr(rt, (const u8 *)"Math", 4);
    u32 n_parseInt = akl_mkstr(rt, (const u8 *)"parseInt", 8);
    u32 n_parseFloat = akl_mkstr(rt, (const u8 *)"parseFloat", 10);
    u32 n_isNaN = akl_mkstr(rt, (const u8 *)"isNaN", 5);
    u32 n_isFinite = akl_mkstr(rt, (const u8 *)"isFinite", 8);
    if (n_nan == UINT32_MAX || n_parseInt == UINT32_MAX || n_parseFloat == UINT32_MAX ||
        n_isNaN == UINT32_MAX || n_isFinite == UINT32_MAX) return false;
    AklVal math = akl_builtin_mkmath(rt);
    if (akl_is_undefined(math)) return false;
    if (!akl_global_set(rt, "Math", math)) return false;
    /* JSON オブジェクト（stringify / parse） */
    {
        u32 jo = akl_obj_new(rt);
        if (jo == UINT32_MAX) return false;
        rt->objs[jo].kind = AKL_OK_OBJ;
        AklVal jv = AKL_MK_OBJ(jo);
        if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = jo;
        struct { const char *n; AklNativeFn f; } jf[] = {
            {"stringify", akl_m_json_stringify}, {"parse", akl_m_json_parse}
        };
        for (u32 i = 0; i < sizeof jf / sizeof jf[0]; i++) {
            u32 nm = akl_intern(rt, (const u8 *)jf[i].n, (u32)strlen(jf[i].n), NULL);
            if (nm == UINT32_MAX) return false;
            if (!obj_prop_set(rt, &rt->objs[jo], nm, akl_mknative(rt, jf[i].f, NULL))) return false;
        }
        if (!akl_global_set(rt, "JSON", jv)) return false;
    }
    /* RegExp グローバル（new でも呼び出しでもオブジェクトを返す） */
    {
        char *buf = (char *)malloc(7);
        if (!buf) { akl_errf(rt, "oom: builtins"); return false; }
        memcpy(buf, "RegExp", 6);
        buf[6] = 0;
        bool ok = akl_native_register(rt, buf, akl_m_regexp_ctor, NULL);
        free(buf);
        if (!ok) return false;
    }
    /* Object オブジェクト（keys/values/entries/assign/create） */
    {
        u32 oo = akl_obj_new(rt);
        if (oo == UINT32_MAX) return false;
        rt->objs[oo].kind = AKL_OK_OBJ;
        AklVal ov = AKL_MK_OBJ(oo);
        if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = oo;
        struct { const char *n; AklNativeFn f; } of[] = {
            {"keys", akl_m_obj_keys}, {"values", akl_m_obj_values},
            {"entries", akl_m_obj_entries}, {"assign", akl_m_obj_assign},
            {"create", akl_m_obj_create}
        };
        for (u32 i = 0; i < sizeof of / sizeof of[0]; i++) {
            u32 nm = akl_intern(rt, (const u8 *)of[i].n, (u32)strlen(of[i].n), NULL);
            if (nm == UINT32_MAX) return false;
            if (!obj_prop_set(rt, &rt->objs[oo], nm, akl_mknative(rt, of[i].f, NULL))) return false;
        }
        if (!akl_global_set(rt, "Object", ov)) return false;
    }
    /* Array オブジェクト（isArray） */
    {
        u32 ao = akl_obj_new(rt);
        if (ao == UINT32_MAX) return false;
        rt->objs[ao].kind = AKL_OK_OBJ;
        AklVal av = AKL_MK_OBJ(ao);
        if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = ao;
        u32 nm = akl_intern(rt, (const u8 *)"isArray", 7, NULL);
        if (nm == UINT32_MAX) return false;
        if (!obj_prop_set(rt, &rt->objs[ao], nm, akl_mknative(rt, akl_m_arr_isArray, NULL))) return false;
        if (!akl_global_set(rt, "Array", av)) return false;
    }
    struct { u32 name; AklNativeFn f; } g[] = {
        {n_parseInt, akl_m_parseInt}, {n_parseFloat, akl_m_parseFloat},
        {n_isNaN, akl_m_isNaN}, {n_isFinite, akl_m_isFinite}
    };
    /* String / Number / Boolean コンストラクタ（呼び出し = 変換。new は空オブジェクト近似） */
    {
        struct { const char *n; AklNativeFn f; } ct[] = {
            {"String", akl_m_String}, {"Number", akl_m_Number}, {"Boolean", akl_m_Boolean}
        };
        for (u32 i = 0; i < sizeof ct / sizeof ct[0]; i++) {
            char *buf = (char *)malloc(strlen(ct[i].n) + 1);
            if (!buf) { akl_errf(rt, "oom: builtins"); return false; }
            memcpy(buf, ct[i].n, strlen(ct[i].n) + 1);
            bool ok = akl_native_register(rt, buf, ct[i].f, NULL);
            free(buf);
            if (!ok) return false;
        }
    }
    for (u32 i = 0; i < sizeof g / sizeof g[0]; i++) {
        u32 gn = 0;
        const u8 *gp = akl_str(rt, g[i].name, &gn);
        if (!gp) return false;
        char *buf = (char *)malloc(gn + 1);
        if (!buf) { akl_errf(rt, "oom: builtins"); return false; }
        memcpy(buf, gp, gn);
        buf[gn] = 0;
        bool ok = akl_native_register(rt, buf, g[i].f, NULL);
        free(buf);
        if (!ok) return false;
    }
    /* 文字列/配列メソッドの NATIVE キャッシュ */
    for (u32 i = 0; i < AKL_STR_METH_N; i++) {
        rt->str_meth_vals[i] = akl_mknative(rt, AKL_STR_METHODS[i].fn, NULL);
        if (akl_is_undefined(rt->str_meth_vals[i])) return false;
    }
    for (u32 i = 0; i < AKL_ARR_METH_N; i++) {
        rt->arr_meth_vals[i] = akl_mknative(rt, AKL_ARR_METHODS[i].fn, NULL);
        if (akl_is_undefined(rt->arr_meth_vals[i])) return false;
    }
    for (u32 i = 0; i < AKL_REGEX_METH_N; i++) {
        rt->regex_meth_vals[i] = akl_mknative(rt, AKL_REGEX_METHODS[i].fn, NULL);
        if (akl_is_undefined(rt->regex_meth_vals[i])) return false;
    }
    return true;
}


/* ============================== VM ============================== */

typedef struct { u32 ret_off; u32 base; u32 func; u8 is_new; } AklFrame; /* 16B（new 呼び出しの this 復元用） */

/* ---- JS 例外: cold 経路の out-of-line 化 ----
 * 例外機構はホット数値ループでは「存在しない」ため、そのコードを dispatch
 * 関数の中に展開したままだと I$ / 分岐予測 / インライン予算を静かに食い潰す
 * （2026-07-31 実測: inline 展開のままでは arith +24% / branch +42% / fib30 +11%
 * 悪化。バイトコード列は新旧で完全同一だったため、原因は機械語レイアウト悪化と
 * 同定。cold 関数に隔離してホット経路を復元する。意味論は旧マクロと 1 行対応）。 */
#if defined(__GNUC__)
#  define AKL_COLDFN __attribute__((noinline, cold))
#else
#  define AKL_COLDFN
#endif

/* native 呼出（CALL/MCALL 共通 cold path）: fn 実行と native_err 判定のみ。
 * GC ルート同期（gc_sp への sp 反映）は呼出側で完了済みの前提。
 * 失敗（native_throw / budget 系）は fn 内が err を設定し、return false で eval を失敗に倒す。
 * cold 隔離: ホット数値ループの I$ / 分岐予測を native 機構の機械語で汚さない
 * （2026-08-08 実測: インライン展開のままでは arith +5.4% / fib +1.7% 退行、隔離で復元） */
static AKL_COLDFN bool akl_vm_native_call(AklRT *rt, AklObj *fo, AklVal self, u8 argc,
                                          AklVal *argv, AklVal *out) {
    *out = fo->u.nat.fn(rt, self, (int)argc, argv, fo->u.nat.udata);
    return !rt->native_err;
}

/* vm_exec の制御ローカルのスナップショット。cold fn が読み書きし、呼出し側の
 * case が AKL_VMST_OUT/IN で同期する（stk/cap は grow で更新され得るのでポインタ渡し） */
typedef struct {
    AklVal **pstk;
    u32 *pcap;
    u8 *code;
    u32 pc_off, sp, base, nframes, cur, entry;
    AklFrame *frames;
} AklVmSt;

static AKL_COLDFN bool akl_vm_stk_grow1(AklRT *rt, AklVmSt *st) {
    if (*st->pcap > st->sp) return true;
    u32 cap = *st->pcap;
    if (cap >= AKL_STK_MAX) { akl_errf(rt, "stack capacity budget exhausted"); return false; }
    u32 ncap = cap * 2 <= AKL_STK_MAX ? cap * 2 : AKL_STK_MAX;
    AklVal *ns = (AklVal *)realloc(rt->stk, (u64)ncap * sizeof(AklVal));
    if (!ns) { akl_errf(rt, "oom: stack grow"); return false; }
    rt->stk = *st->pstk = ns; *st->pcap = ncap;
    return true;
}

/* 指定深さまで frame を引き剥がす（値の伝播なし）。main(深さ0) は base=0, cur=entry で復帰 */
static AKL_COLDFN void akl_vm_teardown_to(AklVmSt *st, u32 fdepth) {
    while (st->nframes > fdepth) {
        st->nframes--;
        st->base = st->frames[st->nframes].base;
        st->cur = st->frames[st->nframes].func;
    }
    if (st->nframes == 0) { st->base = 0; st->cur = st->entry; }
}

/* 例外の巻き戻し: FIN(top) → pop して継続。TRY: catch があれば束縛代入して捕捉、
 * 無ければ finally があれば FIN(mode=1) に化けて実行、両方無ければ pop。
 * true なら pc_off 確定済み（捕捉 or finally 突入）、false なら uncaught/異常（err 設定済） */
static AKL_COLDFN bool akl_vm_unwind(AklRT *rt, AklVmSt *st, AklVal uv) {
    for (;;) {
        if (!rt->n_tries) {
            if (!akl_vm_stk_grow1(rt, st)) return false; /* OOM 系は err 設定済 */
            (*st->pstk)[st->sp++] = uv;
            rt->gc_sp = st->sp; /* 文字列化の GC から値を保護 */
            u32 s_ = akl_to_string(rt, uv);
            u32 sl_ = 0;
            const u8 *bp_ = s_ != UINT32_MAX ? akl_str(rt, s_, &sl_) : NULL;
            if (bp_) akl_errf(rt, "uncaught exception: %.*s", sl_ > 96 ? 96 : (int)sl_, (const char *)bp_);
            else akl_errf(rt, "uncaught exception");
            return false;
        }
        AklTryEnt *te_ = &rt->tries[rt->n_tries - 1];
        if (te_->kind == AKL_TE_FIN) {
            /* 保留中の return/throw は新例外で破棄（JS 規則）。無害化して pop */
            te_->pending = AKL_VAL_UNDEF;
            rt->n_tries--;
            continue;
        }
        akl_vm_teardown_to(st, te_->frame);
        st->sp = te_->sp;
        if (!te_->in_catch && te_->catch_pc != AKL_PC_NONE) {
            if (te_->catch_slot != AKL_SLOT_NONE) {
                if (st->base + te_->catch_slot >= st->sp) {
                    akl_errf(rt, "internal: catch slot OOB"); return false; }
                (*st->pstk)[st->base + te_->catch_slot] = uv;
            }
            te_->in_catch = 1;
            te_->sp = st->sp;
            st->pc_off = te_->catch_pc;
            return true;
        }
        if (te_->finally_pc != AKL_PC_NONE) {
            te_->kind = AKL_TE_FIN; te_->mode = 1; te_->pending = uv; te_->resume_pc = AKL_PC_NONE;
            st->pc_off = te_->finally_pc;
            return true;
        }
        te_->pending = AKL_VAL_UNDEF;
        rt->n_tries--;
    }
}

/* return の finally 連鎖（mode=2 ペンディングに返り値を保持）後に frame 解体。
 * true なら pc_off 確定済み（finally 転移 or 解体+push 完了）、false なら致命的 */
static AKL_COLDFN bool akl_vm_ret_step(AklRT *rt, AklVmSt *st, AklVal rv) {
    for (;;) {
        if (!rt->n_tries) break;
        AklTryEnt *te_ = &rt->tries[rt->n_tries - 1];
        if (te_->frame > st->nframes) {
            /* 直前の RET 連鎖で深さ管理が崩れない限り到達不能（防御層） */
            akl_errf(rt, "internal: try/frame skew"); return false; }
        if (te_->frame < st->nframes) break;
        if (te_->kind == AKL_TE_FIN) {
            te_->pending = AKL_VAL_UNDEF; rt->n_tries--; continue; }
        if (te_->finally_pc != AKL_PC_NONE) {
            te_->kind = AKL_TE_FIN; te_->mode = 2; te_->pending = rv; te_->resume_pc = AKL_PC_NONE;
            st->sp = te_->sp;
            st->pc_off = te_->finally_pc;
            return true;
        }
        rt->n_tries--;
    }
    st->sp = st->base;
    if (!st->nframes) { akl_errf(rt, "internal: ret at top level"); return false; }
    st->nframes--;
    st->base = st->frames[st->nframes].base;
    st->cur = st->frames[st->nframes].func;
    st->pc_off = st->frames[st->nframes].ret_off;
    if (!akl_vm_stk_grow1(rt, st)) return false;
    (*st->pstk)[st->sp++] = rv;
    return true;
}

static AKL_COLDFN bool akl_vm_try_push(AklRT *rt, AklVmSt *st, u32 cpc, u32 fpc, u32 cslot) {
    if (rt->n_tries >= 1024) { akl_errf(rt, "try depth budget exhausted"); return false; }
    if (rt->n_tries == rt->cap_tries) {
        u32 nc = rt->cap_tries ? rt->cap_tries * 2 : 16;
        if (nc > 1024) nc = 1024;
        AklTryEnt *nt_ = (AklTryEnt *)realloc(rt->tries, (u64)nc * sizeof(AklTryEnt));
        if (!nt_) { akl_errf(rt, "oom: tries"); return false; }
        rt->tries = nt_; rt->cap_tries = nc;
    }
    AklTryEnt *e_ = &rt->tries[rt->n_tries++];
    memset(e_, 0, sizeof *e_);
    e_->frame = st->nframes; e_->sp = st->sp;
    e_->catch_pc = cpc; e_->finally_pc = fpc; e_->catch_slot = cslot;
    return true;
}

static AKL_COLDFN bool akl_vm_try_leave(AklRT *rt, AklVmSt *st, u32 resume) {
    if (!rt->n_tries) { akl_errf(rt, "internal: try_leave without entry"); return false; }
    AklTryEnt *e_ = &rt->tries[rt->n_tries - 1];
    if (e_->kind != AKL_TE_TRY || e_->frame != st->nframes) {
        akl_errf(rt, "internal: try_leave entry mismatch"); return false; }
    if (e_->finally_pc != AKL_PC_NONE) {
        u32 fpc = e_->finally_pc;
        e_->kind = AKL_TE_FIN; e_->mode = 0; e_->resume_pc = resume; e_->sp = st->sp;
        st->pc_off = fpc;
    } else {
        rt->n_tries--;
        st->pc_off = resume;
    }
    return true;
}

static AKL_COLDFN bool akl_vm_fin_end(AklRT *rt, AklVmSt *st) {
    if (!rt->n_tries) { akl_errf(rt, "internal: fin_end without entry"); return false; }
    AklTryEnt *e_ = &rt->tries[rt->n_tries - 1];
    if (e_->kind != AKL_TE_FIN || e_->frame != st->nframes) {
        akl_errf(rt, "internal: fin_end entry mismatch"); return false; }
    u8 mode_ = e_->mode;
    AklVal pv_ = e_->pending;
    u32 resume_ = e_->resume_pc;
    e_->pending = AKL_VAL_UNDEF;
    rt->n_tries--;
    if (mode_ == 0) { st->pc_off = resume_; return true; }
    if (mode_ == 1) return akl_vm_unwind(rt, st, pv_);
    return akl_vm_ret_step(rt, st, pv_);
}

/* dispatch: GCC/Clang では computed-goto（分岐予測局所化）、他は switch。
 * AKL_TEST_SWITCH_DISPATCH で強制的に switch 側をビルド（検証・bench_akl の差分測定用）。 */
#if defined(__GNUC__) && !defined(AKL_TEST_SWITCH_DISPATCH)
#define AKL_THREADED 1
#endif

/* frame の隠し slot（this / 自前 ENV / cap ENV）初期化。CALL/MCALL/main 共通。
 * stk[base..base+nloc) は確保済み前提。ENV 生成は GC 発火し得るため、呼出側は
 * gc_sp をルート済み深さに同期してから呼ぶこと（本関数は同期しない）。 */
static AKL_COLDFN bool akl_vm_frame_hidden(AklRT *rt, AklVal *stk, u32 base, u32 nloc,
                                           const AklFuncEnt *fe, AklVal this_v, AklVal capenv) {
    u32 nh = 1 + (fe->n_env ? 1u : 0u) + (fe->n_cap ? 1u : 0u);
    if (nh < 1 || nloc < nh) { akl_errf(rt, "internal: frame hidden slots"); return false; }
    stk[base] = this_v;
    if (fe->n_env) {
        u32 oi = akl_obj_new(rt);
        if (oi == UINT32_MAX) return false;
        AklObj *o = &rt->objs[oi];
        o->kind = AKL_OK_ENV;
        o->u.env.n = fe->n_env;
        o->u.env.parent = akl_is_objv(capenv) ? akl_get_obj(capenv) : UINT32_MAX;
        o->u.env.vals = (AklVal *)malloc((u64)fe->n_env * sizeof(AklVal));
        if (!o->u.env.vals) {
            rt->objs[oi].kind = 0;
            akl_errf(rt, "oom: env");
            return false;
        }
        for (u32 i = 0; i < fe->n_env; i++) o->u.env.vals[i] = AKL_VAL_UNDEF;
        rt->heap_bytes += (u64)fe->n_env * sizeof(AklVal);
        stk[base + 1] = AKL_MK_OBJ(oi);
    }
    if (fe->n_cap) stk[base + 1 + (fe->n_env ? 1u : 0u)] = capenv;
    return true;
}

/* init_args/init_argc/init_this: 再入呼び出し（akl_call）のための最初のフレーム引数。
 * akl_eval は NULL/0/UNDEF を渡す（従来どおり引数なし）。 */
static bool vm_exec(AklRT *rt, u32 entry, const AklVal *init_args, u32 init_argc,
                    AklVal init_this, AklVal init_capenv, bool top_ret_ok) {
    if (entry >= rt->n_funcs) { akl_errf(rt, "internal: bad entry"); return false; }
    AklFrame *frames = (AklFrame *)malloc((u64)AKL_MAX_DEPTH * sizeof(AklFrame));
    if (!frames) { akl_errf(rt, "oom: frames"); return false; }
    u32 nframes = 0;
    AklVal *stk = rt->stk;
    u32 sp = 0;
    u32 cap = rt->cap_stk;
    u8 *code = rt->code;
    u32 cur = entry;
    const u8 *pc = code + rt->funcs[cur].code_off;
    u32 base = 0;
    bool dead = false; /* defense-in-depth: 下記 AKL_POP 下限突破で立つ（verifier 通過後は発火しない設計） */
    u64 budget = rt->insn_budget_def;
    /* MCALL/MCALLN 共通化のための共有引数（goto をまたぐため関数先頭で確保） */
    i32 mcall_argc = 0;
    u32 mcall_name = 0;

/* stk を top 要素数まで収容できるよう倍々で拡張（rt->stk と共有。AKL_STK_MAX で fail-fast） */
#define AKL_GROW_TO(top) do { \
    u32 top_ = (top); \
    while (cap < top_) { \
        if (cap >= AKL_STK_MAX) { akl_errf(rt, "stack capacity budget exhausted"); free(frames); return false; } \
        u32 ncap_ = cap * 2 <= AKL_STK_MAX ? cap * 2 : AKL_STK_MAX; \
        AklVal *ns_ = (AklVal *)realloc(rt->stk, (u64)ncap_ * sizeof(AklVal)); \
        if (!ns_) { akl_errf(rt, "oom: stack grow"); free(frames); return false; } \
        rt->stk = stk = ns_; rt->cap_stk = cap = ncap_; \
    } } while (0)
#define AKL_PUSH(v) do { AKL_GROW_TO(sp + 1); stk[sp++] = (v); } while (0)
/* 下限突破は verifier 済みコードでは到達不能。到達したら dead を立てて次の AKL_NEXT で停止する */
#define AKL_POP() (sp > base ? stk[--sp] : (dead = true, akl_errf(rt, "stack underflow"), AKL_VAL_UNDEF))
#define AKL_PEEK() stk[sp - 1]
#define AKL_BUDGET() do { if (!--budget) { akl_errf(rt, "instruction budget exhausted"); free(frames); return false; } } while (0)

    /* cold 例外経路との同期窓（ホット経路では一切触れない。case 内で OUT→cold call→IN） */
    AklVmSt vmst_;
#define AKL_VMST_OUT() do { \
    vmst_.pstk = &stk; vmst_.pcap = &cap; vmst_.code = code; \
    vmst_.pc_off = (u32)(pc - code); vmst_.sp = sp; vmst_.base = base; \
    vmst_.nframes = nframes; vmst_.cur = cur; vmst_.entry = entry; vmst_.frames = frames; \
} while (0)
#define AKL_VMST_IN() do { \
    pc = code + vmst_.pc_off; sp = vmst_.sp; base = vmst_.base; \
    nframes = vmst_.nframes; cur = vmst_.cur; \
} while (0)

    /* メイン locals 窓（v0.3: 隠し slot の初期化込み。this / 自前 ENV / cap ENV。
     * 再入時は init_args をパラメータへコピー（hidden の後ろ。超過引数は JS 同様無視）） */
    {
        AklFuncEnt *fe0 = &rt->funcs[cur];
        u32 nl = fe0->n_locals;
        AKL_GROW_TO(nl + 4);
        u32 nh = 1 + (fe0->n_env ? 1u : 0u) + (fe0->n_cap ? 1u : 0u);
        for (u32 i = 0; i < nl; i++) stk[i] = AKL_VAL_UNDEF;
        if (nh > 1) {
            rt->gc_sp = nl;
            if (!akl_vm_frame_hidden(rt, stk, 0, nl, fe0, init_this, init_capenv)) {
                free(frames);
                return false;
            }
        } else {
            stk[0] = init_this;
        }
        u32 keep = init_argc < fe0->n_params ? init_argc : fe0->n_params;
        for (u32 i = 0; i < keep; i++) stk[nh + i] = init_args[i];
        sp = nl;
    }
/* 規則: マクロ内で AKL_NEXT を使わない（switch 側では do-while(0) がマクロ内 break を
 * 攫って case 貫通事故になる。裸ブロックにし NEXT は必ず呼び出し側の case 末で行う） */
#define AKL_BINOP_NUM(COMBINE) { \
        AklVal vb_ = AKL_POP(), va_ = AKL_POP(); \
        rt->gc_sp = sp; /* flatten 発火に備えてルート深さを同期 */ \
        double da_, db_; \
        akl_to_number2(rt, va_, vb_, &da_, &db_); \
        AKL_PUSH(akl_num(akl_canon(COMBINE))); }

#if AKL_THREADED
    static const void *const akl_jt[OP_COUNT] = {
        [OP_CONST_I] = &&l_CONST_I, [OP_CONST_D] = &&l_CONST_D, [OP_CONST_STR] = &&l_CONST_STR,
        [OP_TRUE_T] = &&l_TRUE_T, [OP_FALSE_T] = &&l_FALSE_T, [OP_NULL_T] = &&l_NULL_T,
        [OP_UNDEF_T] = &&l_UNDEF_T,
        [OP_ADD] = &&l_ADD, [OP_SUB] = &&l_SUB, [OP_MUL] = &&l_MUL, [OP_DIV] = &&l_DIV,
        [OP_MOD] = &&l_MOD,
        [OP_LT] = &&l_LT, [OP_LE] = &&l_LE, [OP_GT] = &&l_GT, [OP_GE] = &&l_GE,
        [OP_EQ] = &&l_EQ, [OP_NE] = &&l_NE, [OP_SEQ] = &&l_SEQ, [OP_SNE] = &&l_SNE,
        [OP_NOT] = &&l_NOT, [OP_NEG] = &&l_NEG, [OP_POS] = &&l_POS, [OP_TYPEOF] = &&l_TYPEOF,
        [OP_POP] = &&l_POP, [OP_POPV] = &&l_POPV, [OP_DUP] = &&l_DUP,
        [OP_LLOAD] = &&l_LLOAD, [OP_LSTORE] = &&l_LSTORE,
        [OP_GLOAD] = &&l_GLOAD, [OP_GSTORE] = &&l_GSTORE,
        [OP_JMP] = &&l_JMP, [OP_JMPF] = &&l_JMPF, [OP_JMPT] = &&l_JMPT,
        [OP_CALL] = &&l_CALL, [OP_RET] = &&l_RET, [OP_MAKEF] = &&l_MAKEF,
        [OP_LINC] = &&l_LINC, [OP_CJMPF_L] = &&l_CJMPF_L, [OP_CJMPF_G] = &&l_CJMPF_G,
        [OP_GLOAD_S] = &&l_GLOAD_S, [OP_GSTORE_S] = &&l_GSTORE_S, [OP_GINC] = &&l_GINC,
        [OP_GMULC] = &&l_GMULC, [OP_LMULC] = &&l_LMULC, [OP_MULCI] = &&l_MULCI,
        [OP_ADDCI] = &&l_ADDCI, [OP_SUBCI] = &&l_SUBCI, [OP_MODCI] = &&l_MODCI,
        [OP_GADD_P] = &&l_GADD_P, [OP_LADD_P] = &&l_LADD_P, [OP_GADD_G] = &&l_GADD_G,
        [OP_CJMPF_MODG] = &&l_CJMPF_MODG, [OP_CJMPF_MODL] = &&l_CJMPF_MODL,
        [OP_GSTORE_SPV] = &&l_GSTORE_SPV, [OP_LSTORE_PV] = &&l_LSTORE_PV,
        [OP_LOOPINC_G] = &&l_LOOPINC_G,
        [OP_ADDCI_G] = &&l_ADDCI_G, [OP_SUBCI_G] = &&l_SUBCI_G,
        [OP_MULCI_G] = &&l_MULCI_G, [OP_MODCI_G] = &&l_MODCI_G,
        [OP_ADDCI_L] = &&l_ADDCI_L, [OP_SUBCI_L] = &&l_SUBCI_L,
        [OP_MULCI_L] = &&l_MULCI_L, [OP_MODCI_L] = &&l_MODCI_L,
        [OP_CJMPF_MULGG] = &&l_CJMPF_MULGG, [OP_CJMPF_MODGG] = &&l_CJMPF_MODGG,
        [OP_LADD_LL] = &&l_LADD_LL, [OP_RET_L] = &&l_RET_L,
        [OP_ADD_GX] = &&l_ADD_GX, [OP_SUB_GX] = &&l_SUB_GX,
        [OP_MUL_GX] = &&l_MUL_GX, [OP_MOD_GX] = &&l_MOD_GX,
        [OP_ADD_LX] = &&l_ADD_LX, [OP_SUB_LX] = &&l_SUB_LX,
        [OP_MUL_LX] = &&l_MUL_LX, [OP_MOD_LX] = &&l_MOD_LX,
        [OP_TRY_PUSH] = &&l_TRY_PUSH, [OP_TRY_LEAVE] = &&l_TRY_LEAVE,
        [OP_FIN_END] = &&l_FIN_END, [OP_THROW] = &&l_THROW,
        [OP_LOOPINC_L] = &&l_LOOPINC_L, [OP_NOP] = &&l_NOP,
        [OP_LOOPINC_GV] = &&l_LOOPINC_GV, [OP_LOOPINC_LV] = &&l_LOOPINC_LV,
        [OP_OBJNEW] = &&l_OBJNEW, [OP_PLOAD] = &&l_PLOAD,
        [OP_PSTORE] = &&l_PSTORE, [OP_MCALL] = &&l_MCALL,
        [OP_THIS] = &&l_THIS, [OP_ELOAD] = &&l_ELOAD, [OP_ESTORE] = &&l_ESTORE,
        [OP_CELOAD] = &&l_CELOAD, [OP_CESTORE] = &&l_CESTORE,
        [OP_ANEW] = &&l_ANEW, [OP_AGET] = &&l_AGET, [OP_ASET] = &&l_ASET,
        [OP_BNOT] = &&l_BNOT, [OP_BAND] = &&l_BAND, [OP_BOR] = &&l_BOR,
        [OP_BXOR] = &&l_BXOR, [OP_BSHL] = &&l_BSHL, [OP_BSHR] = &&l_BSHR,
        [OP_BUSHR] = &&l_BUSHR,
        [OP_POW] = &&l_POW, [OP_IN] = &&l_IN,
        [OP_KEYSOF] = &&l_KEYSOF, [OP_TOARR] = &&l_TOARR,
        [OP_ARRPUSH] = &&l_ARRPUSH, [OP_ARRPUSHALL] = &&l_ARRPUSHALL,
        [OP_ARRSPREADC] = &&l_ARRSPREADC, [OP_CALLN] = &&l_CALLN,
        [OP_PDEL] = &&l_PDEL, [OP_IDEL] = &&l_IDEL, [OP_INSTANCEOF] = &&l_INSTANCEOF,
        [OP_NEW] = &&l_NEW,
        [OP_NEWREGEX] = &&l_NEWREGEX,
        [OP_CALLT] = &&l_CALLT,
        [OP_MAKEFS] = &&l_MAKEFS,
        [OP_SUPERGET] = &&l_SUPERGET,
        [OP_OBJSPREAD] = &&l_OBJSPREAD,
        [OP_PSETDYN] = &&l_PSETDYN,
        [OP_MCALLN] = &&l_MCALLN,
        [OP_ARRREST] = &&l_ARRREST,
        [OP_OBJREST] = &&l_OBJREST,
        [OP_HALT] = &&l_HALT,
    };
#define AKL_NEXT() do { if (dead) { free(frames); return false; } AKL_BUDGET(); goto *akl_jt[*pc++]; } while (0)
#define AKL_L(name) l_##name
    goto *akl_jt[*pc++];
#else
/* switch 側は do-while(0) で包むと break がループを抜けるだけで case を貫通するため
 * 必ず裸ブロック＋素の break にする（-Wimplicit-fallthrough の警告もここ由来） */
#define AKL_NEXT() { if (dead) { free(frames); return false; } if (!--budget) { akl_errf(rt, "instruction budget exhausted"); free(frames); return false; } break; }
#define AKL_L(name) case OP_##name
    for (;;) switch (*pc++) {
#endif

    AKL_L(CONST_I): {
        u32 imm;
        memcpy(&imm, pc, 4); pc += 4;
        AKL_PUSH(AKL_MK_INT((i32)imm));
        AKL_NEXT();
    }
    AKL_L(CONST_D): {
        AklVal bits;
        memcpy(&bits, pc, 8); pc += 8;
        AKL_PUSH(bits); /* double bits はそのまま値（非タグ） */
        AKL_NEXT();
    }
    AKL_L(CONST_STR): {
        u32 idx;
        memcpy(&idx, pc, 4); pc += 4;
        AKL_PUSH(AKL_MK_OBJ(idx));
        AKL_NEXT();
    }
    AKL_L(TRUE_T):  { AKL_PUSH(AKL_VAL_TRUE);  AKL_NEXT(); }
    AKL_L(FALSE_T): { AKL_PUSH(AKL_VAL_FALSE); AKL_NEXT(); }
    AKL_L(NULL_T):  { AKL_PUSH(AKL_VAL_NULL);  AKL_NEXT(); }
    AKL_L(UNDEF_T): { AKL_PUSH(AKL_VAL_UNDEF); AKL_NEXT(); }

    AKL_L(ADD): {
        AklVal vb = AKL_POP(), va = AKL_POP();
        AklVal out;
        if (!akl_bin_add(rt, va, vb, sp, &out)) { free(frames); return false; }
        AKL_PUSH(out);
        AKL_NEXT();
    }
    AKL_L(SUB): {
        AklVal vb = AKL_POP(), va = AKL_POP();
        if (akl_is_intv(va) && akl_is_intv(vb)) {
            i64 r = (i64)akl_get_int(va) - (i64)akl_get_int(vb);
            if (r >= -2147483648ll && r <= 2147483647ll) { AKL_PUSH(AKL_MK_INT((i32)r)); AKL_NEXT(); }
            AKL_PUSH(akl_num((double)r));
            AKL_NEXT();
        }
        rt->gc_sp = sp;
        double da_, db_;
        akl_to_number2(rt, va, vb, &da_, &db_);
        AKL_PUSH(akl_num(akl_canon(da_ - db_)));
        AKL_NEXT();
    }
    AKL_L(MUL): {
        AklVal vb = AKL_POP(), va = AKL_POP();
        if (akl_is_intv(va) && akl_is_intv(vb)) {
            i64 r = (i64)akl_get_int(va) * (i64)akl_get_int(vb);
            if (r >= -2147483648ll && r <= 2147483647ll) { AKL_PUSH(AKL_MK_INT((i32)r)); AKL_NEXT(); }
            AKL_PUSH(akl_num((double)r));
            AKL_NEXT();
        }
        rt->gc_sp = sp;
        double da_, db_;
        akl_to_number2(rt, va, vb, &da_, &db_);
        AKL_PUSH(akl_num(akl_canon(da_ * db_)));
        AKL_NEXT();
    }
    AKL_L(DIV): { AKL_BINOP_NUM(da_ / db_); AKL_NEXT(); }
    AKL_L(MOD): {
        AklVal vb = AKL_POP(), va = AKL_POP();
        if (akl_is_intv(va) && akl_is_intv(vb)) {
            i32 ia = akl_get_int(va), ib = akl_get_int(vb);
            if (ib != 0 && !(ia == INT32_MIN && ib == -1)) { AKL_PUSH(AKL_MK_INT(ia % ib)); AKL_NEXT(); }
        }
        rt->gc_sp = sp;
        double da, db;
        akl_to_number2(rt, va, vb, &da, &db);
        AKL_PUSH(akl_num(akl_canon(fmod(da, db))));
        AKL_NEXT();
    }
    /* 比較 4 命令は独立本体（dispatch 両モードで壊れないマクロ展開。文字列辞書式は両辺 string のみ） */
#define AKL_REL(NUMCMP, STRCMP, INTCMP) { \
        AklVal rb_ = AKL_POP(), ra_ = AKL_POP(); \
        if (akl_is_intv(ra_) && akl_is_intv(rb_)) { \
            i32 ia_ = akl_get_int(ra_), ib_ = akl_get_int(rb_); \
            AKL_PUSH((INTCMP) ? AKL_VAL_TRUE : AKL_VAL_FALSE); \
            AKL_NEXT(); \
        } \
        rt->gc_sp = sp; /* flatten 発火に備えてルート深さを同期 */ \
        if (akl_is_strly(rt, ra_) && akl_is_strly(rt, rb_)) { \
            u32 oia_ = akl_get_obj(ra_), oib_ = akl_get_obj(rb_); \
            u32 nur0_ = rt->n_nury; /* ペアピン: 相互スイープ防止 */ \
            if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = oia_; \
            if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = oib_; \
            u32 la_, lb_; \
            const u8 *pa_ = akl_str(rt, oia_, &la_); \
            const u8 *pb_ = rt->err[0] ? (const u8 *)"" : akl_str(rt, oib_, &lb_); \
            if (rt->err[0]) { free(frames); return false; } \
            u32 m_ = la_ < lb_ ? la_ : lb_; \
            int cmp_ = m_ ? memcmp(pa_, pb_, m_) : 0; \
            if (!cmp_) cmp_ = la_ < lb_ ? -1 : la_ > lb_ ? 1 : 0; \
            rt->n_nury = nur0_; \
            AKL_PUSH((STRCMP) ? AKL_VAL_TRUE : AKL_VAL_FALSE); \
        } else { \
            double da_, db_; \
            akl_to_number2(rt, ra_, rb_, &da_, &db_); \
            bool r_ = !isnan(da_) && !isnan(db_) && (NUMCMP); \
            AKL_PUSH(r_ ? AKL_VAL_TRUE : AKL_VAL_FALSE); \
        } \
        AKL_NEXT(); }
    AKL_L(LT): { AKL_REL(da_ <  db_, cmp_ <  0, ia_ <  ib_); }
    AKL_L(LE): { AKL_REL(da_ <= db_, cmp_ <= 0, ia_ <= ib_); }
    AKL_L(GT): { AKL_REL(da_ >  db_, cmp_ >  0, ia_ >  ib_); }
    AKL_L(GE): { AKL_REL(da_ >= db_, cmp_ >= 0, ia_ >= ib_); }
#undef AKL_REL
    AKL_L(EQ):  { AklVal vb = AKL_POP(), va = AKL_POP(); rt->gc_sp = sp; bool r_ = akl_loose_eq(rt, va, vb);   if (rt->err[0]) { free(frames); return false; } AKL_PUSH(r_  ? AKL_VAL_TRUE : AKL_VAL_FALSE); AKL_NEXT(); }
    AKL_L(NE):  { AklVal vb = AKL_POP(), va = AKL_POP(); rt->gc_sp = sp; bool r_ = akl_loose_eq(rt, va, vb);   if (rt->err[0]) { free(frames); return false; } AKL_PUSH(!r_ ? AKL_VAL_TRUE : AKL_VAL_FALSE); AKL_NEXT(); }
    AKL_L(SEQ): { AklVal vb = AKL_POP(), va = AKL_POP(); rt->gc_sp = sp; bool r_ = akl_strict_eq(rt, va, vb);  if (rt->err[0]) { free(frames); return false; } AKL_PUSH(r_  ? AKL_VAL_TRUE : AKL_VAL_FALSE); AKL_NEXT(); }
    AKL_L(SNE): { AklVal vb = AKL_POP(), va = AKL_POP(); rt->gc_sp = sp; bool r_ = akl_strict_eq(rt, va, vb);  if (rt->err[0]) { free(frames); return false; } AKL_PUSH(!r_ ? AKL_VAL_TRUE : AKL_VAL_FALSE); AKL_NEXT(); }

    AKL_L(NOT): { AklVal v = AKL_POP(); AKL_PUSH(akl_truthy(rt, v) ? AKL_VAL_FALSE : AKL_VAL_TRUE); AKL_NEXT(); }
    AKL_L(NEG): {
        AklVal v = AKL_POP();
        if (akl_is_intv(v)) {
            i32 i2 = akl_get_int(v);
            if (i2 != INT32_MIN) { AKL_PUSH(AKL_MK_INT(-i2)); AKL_NEXT(); }
        }
        rt->gc_sp = sp; /* 単項なので他被演算子のピンは不要（to_number が自己ピン） */
        AKL_PUSH(akl_num(akl_canon(-akl_to_number(rt, v))));
        AKL_NEXT();
    }
    AKL_L(POS): { AklVal v = AKL_POP(); rt->gc_sp = sp; AKL_PUSH(akl_num(akl_canon(akl_to_number(rt, v)))); AKL_NEXT(); }
    AKL_L(TYPEOF): {
        AklVal v = AKL_POP();
        const char *s;
        double d;
        if (v == AKL_VAL_UNDEF) s = "undefined";
        else if (v == AKL_VAL_NULL) s = "object";
        else if (v == AKL_VAL_TRUE || v == AKL_VAL_FALSE) s = "boolean";
        else if (akl_numv(v, &d)) s = "number";
        else if (akl_is_objv(v)) {
            u8 ok_ = rt->objs[akl_get_obj(v)].kind;
            s = (ok_ == AKL_OK_STR || ok_ == AKL_OK_ROPE) ? "string"
              : (ok_ == AKL_OK_OBJ || ok_ == AKL_OK_HANDLE || ok_ == AKL_OK_ARR) ? "object"
              : "function"; /* FUNC / NATIVE */
        }
        else s = "undefined";
        rt->gc_sp = sp; /* mkstr の GC 発火に備えてルート深さを同期 */
        u32 idx = akl_mkstr(rt, (const u8 *)s, (u32)strlen(s));
        if (idx == UINT32_MAX) { free(frames); return false; }
        AKL_PUSH(AKL_MK_OBJ(idx));
        AKL_NEXT();
    }

    AKL_L(POP):  { (void)AKL_POP(); AKL_NEXT(); }
    AKL_L(POPV): { rt->last_val = AKL_POP(); AKL_NEXT(); }
    AKL_L(DUP):  { if (sp <= base) { akl_errf(rt, "stack underflow"); free(frames); return false; } AklVal t_ = stk[sp - 1]; AKL_PUSH(t_); AKL_NEXT(); }

    AKL_L(LLOAD): {
        u32 slot;
        memcpy(&slot, pc, 4); pc += 4;
        if (base + slot >= sp) { akl_errf(rt, "local OOB read"); free(frames); return false; }
        AKL_PUSH(stk[base + slot]);
        AKL_NEXT();
    }
    AKL_L(LSTORE): {
        u32 slot;
        memcpy(&slot, pc, 4); pc += 4;
        if (base + slot >= sp) { akl_errf(rt, "local OOB write"); free(frames); return false; }
        stk[base + slot] = AKL_POP();
        AKL_NEXT();
    }
    AKL_L(GLOAD): {
        u32 name;
        memcpy(&name, pc, 4); pc += 4;
        u32 gi = cg_global_find(rt, name);
        if (gi == UINT32_MAX) {
            u32 ln;
            const u8 *np = akl_str(rt, name, &ln);
            akl_errf(rt, "ReferenceError: %.*s is not defined", (int)ln, np);
            free(frames);
            return false;
        }
        AKL_PUSH(rt->globals[gi].v);
        AKL_NEXT();
    }
    AKL_L(GSTORE): {
        u32 name;
        memcpy(&name, pc, 4); pc += 4;
        u32 gi = cg_global_find(rt, name);
        if (gi == UINT32_MAX) { akl_errf(rt, "internal: global slot missing"); free(frames); return false; }
        if (rt->globals[gi].is_const) { akl_errf(rt, "TypeError: assignment to const"); free(frames); return false; }
        rt->globals[gi].v = AKL_POP();
        AKL_NEXT();
    }
    AKL_L(JMP): {
        u32 tgt;
        memcpy(&tgt, pc, 4);
        pc = code + tgt;
        AKL_NEXT();
    }
    AKL_L(JMPF): {
        u32 tgt;
        memcpy(&tgt, pc, 4); pc += 4;
        AklVal v = AKL_POP();
        if (!akl_truthy(rt, v)) pc = code + tgt;
        AKL_NEXT();
    }
    AKL_L(JMPT): {
        u32 tgt;
        memcpy(&tgt, pc, 4); pc += 4;
        AklVal v = AKL_POP();
        if (akl_truthy(rt, v)) pc = code + tgt;
        AKL_NEXT();
    }
    AKL_L(CALL): {
        u8 argc = *pc++;
        if (argc > 250 || sp < base + argc + 1) { akl_errf(rt, "stack underflow: call"); free(frames); return false; }
        AklVal fv = stk[sp - argc - 1];
        if (!(akl_is_objv(fv))) {
            akl_errf(rt, "TypeError: not a function");
            free(frames);
            return false;
        }
        AklObj *fo = &rt->objs[akl_get_obj(fv)];
        if (fo->kind == AKL_OK_NATIVE) { /* is_objv 二重評価を避けるため kind 一本化（fib 実測 +2.8% の退行を解消） */
            if (budget < AKL_NATIVE_COST) { akl_errf(rt, "instruction budget exhausted"); free(frames); return false; }
            budget -= AKL_NATIVE_COST;
            rt->gc_sp = sp; /* native 内の mkstring 等が GC 発火し得る → ルート同期（MAKEF 同型） */
            u32 nur0 = rt->n_nury;
            AklVal r;
            if (!akl_vm_native_call(rt, fo, AKL_VAL_UNDEF, argc, stk + sp - argc, &r)) { free(frames); return false; }
            rt->n_nury = nur0; /* 返り値 r は直後にスタックへ根付く（復元〜書込の間に GC 契機なし） */
            sp -= argc;
            stk[sp - 1] = r;   /* 関数値スロットを結果で潰す（pop argc+1 / push r 等価） */
            AKL_NEXT();
        }
        if (fo->kind != AKL_OK_FUNC) {
            akl_errf(rt, "TypeError: not a function");
            free(frames);
            return false;
        }
        u32 fidx = akl_get_obj(fv);
        u32 fe_i = rt->objs[fidx].code_off; /* FUNC obj の code_off は funcs[] の index */
        if (fe_i >= rt->n_funcs) { akl_errf(rt, "internal: bad func ref"); free(frames); return false; }
        AklFuncEnt *fe = &rt->funcs[fe_i];
        if (nframes >= AKL_MAX_DEPTH) { akl_errf(rt, "call depth budget exhausted"); free(frames); return false; }
        /* 引数ウィンドウ: fn 値の 1 個分を this スロットに潰し、引数を隠し slot 分だけ
         * 右へシフトして locals 窓にする（v0.3 の this/ENV 対応フレーム）。
         * nh=1（大半）はシフト不要 = 旧実装より 1 移動少ない。 */
        u32 nh = 1 + (fe->n_env ? 1u : 0u) + (fe->n_cap ? 1u : 0u);
        u32 win = sp - argc - 1;
        u32 nloc = fe->n_locals, npar = fe->n_params;
        u32 k = nh - 1;
        AKL_GROW_TO(win + k + argc + 8); /* シフト先（sp より上）の安全マージン込み */
        if (k) { /* 右へ k シフト（高位から: dest=src+k で「既読の src」しか潰さない） */
            for (i32 i = (i32)argc - 1; i >= 0; i--) stk[win + k + i + 1] = stk[win + i + 1];
        }
        if (nh > 1) {
            /* hidden 初期化（自前 ENV 生成は GC 発火し得る → 呼出側 sp をルート同期） */
            rt->gc_sp = sp;
            if (!akl_vm_frame_hidden(rt, stk, win, nloc, fe, AKL_VAL_UNDEF,
                                     fo->env == UINT32_MAX ? AKL_VAL_UNDEF : AKL_MK_OBJ(fo->env))) {
                free(frames);
                return false;
            }
        } else {
            stk[win] = AKL_VAL_UNDEF; /* this */
        }
        u32 keep = argc < npar ? argc : npar;
        for (u32 i = nh + keep; i < nloc; i++) stk[win + i] = AKL_VAL_UNDEF;
        sp = win + nloc;
        frames[nframes].ret_off = (u32)(pc - code);
        frames[nframes].base = base;
        frames[nframes].func = cur;
        frames[nframes].is_new = 0;
        nframes++;
        base = win;
        cur = fe_i;
        pc = code + rt->funcs[cur].code_off;
        AKL_NEXT();
    }
    AKL_L(CALLT): {
        /* レイアウト: [this][fn][args...]（sp-argc-3 = this, sp-argc-2 = fn）。
         * fn を this で呼ぶ（super の親コンストラクタ/メソッド呼び出し用）。 */
        u8 argc = *pc++;
        if (argc > 250 || sp < base + argc + 2) { akl_errf(rt, "stack underflow: callt"); free(frames); return false; }
        AklVal thisv = stk[sp - argc - 2];
        AklVal fv = stk[sp - argc - 1];
        if (!(akl_is_objv(fv))) {
            akl_errf(rt, "TypeError: not a function");
            free(frames);
            return false;
        }
        AklObj *fo = &rt->objs[akl_get_obj(fv)];
        if (fo->kind == AKL_OK_NATIVE) {
            if (budget < AKL_NATIVE_COST) { akl_errf(rt, "instruction budget exhausted"); free(frames); return false; }
            budget -= AKL_NATIVE_COST;
            rt->gc_sp = sp;
            u32 nur0 = rt->n_nury;
            AklVal r;
            if (!akl_vm_native_call(rt, fo, thisv, argc, stk + sp - argc, &r)) { free(frames); return false; }
            rt->n_nury = nur0;
            sp -= argc + 1; /* fn + args を潰す */
            stk[sp - 1] = r;
            AKL_NEXT();
        }
        if (fo->kind != AKL_OK_FUNC) {
            akl_errf(rt, "TypeError: not a function");
            free(frames);
            return false;
        }
        u32 fidx = akl_get_obj(fv);
        u32 fe_i = rt->objs[fidx].code_off;
        if (fe_i >= rt->n_funcs) { akl_errf(rt, "internal: bad func ref"); free(frames); return false; }
        AklFuncEnt *fe = &rt->funcs[fe_i];
        if (nframes >= AKL_MAX_DEPTH) { akl_errf(rt, "call depth budget exhausted"); free(frames); return false; }
        u32 nh = 1 + (fe->n_env ? 1u : 0u) + (fe->n_cap ? 1u : 0u);
        /* this の位置をフレーム base にする（fn スロットは消費）。
         * RET 後 sp = base+1 = [result] だけが残り、this は消える（CALL と同形）。 */
        u32 win = sp - argc - 2;
        u32 nloc = fe->n_locals, npar = fe->n_params;
        u32 k = nh - 1;
        AKL_GROW_TO(win + k + argc + 8);
        if (k) { /* args を hidden 分シフト（win+1 は fn スロット。args の元位置は win+2 から） */
            for (i32 i = (i32)argc - 1; i >= 0; i--) stk[win + k + i + 1] = stk[win + i + 2];
        } else if (argc) { /* fn スロット（win+1）を args[0] で潰す */
            for (i32 i = (i32)argc - 1; i >= 0; i--) stk[win + i + 1] = stk[win + i + 2];
        }
        if (nh > 1) {
            rt->gc_sp = sp;
            if (!akl_vm_frame_hidden(rt, stk, win, nloc, fe, thisv,
                                     fo->env == UINT32_MAX ? AKL_VAL_UNDEF : AKL_MK_OBJ(fo->env))) {
                free(frames);
                return false;
            }
        } else {
            stk[win] = thisv;
        }
        u32 keep = argc < npar ? argc : npar;
        for (u32 i = nh + keep; i < nloc; i++) stk[win + i] = AKL_VAL_UNDEF;
        sp = win + nloc;
        frames[nframes].ret_off = (u32)(pc - code);
        frames[nframes].base = base;
        frames[nframes].func = cur;
        frames[nframes].is_new = 0;
        nframes++;
        base = win;
        cur = fe_i;
        pc = code + rt->funcs[cur].code_off;
        AKL_NEXT();
    }
    AKL_L(RET): {
        if (sp <= base) { akl_errf(rt, "stack underflow: ret"); free(frames); return false; }
        AklVal v = AKL_POP();
        if (rt->n_tries) { /* try 存在時のみ cold 連鎖（finally）。非存在は旧来の完全 inline */
            AKL_VMST_OUT();
            bool ok_ = akl_vm_ret_step(rt, &vmst_, v);
            AKL_VMST_IN();
            if (!ok_) { free(frames); return false; }
            AKL_NEXT();
        }
        sp = base;
        if (!nframes) {
            if (top_ret_ok) { /* エントリ呼び出し（akl_call）: 関数の return = 成功終了 */
                free(frames);
                rt->last_val = v;
                return true;
            }
            free(frames); akl_errf(rt, "internal: ret at top level"); return false;
        }
        /* new 呼び出し: コンストラクタが obj を返さなければ this を返す（JS）。
         * 現在の callee のフレームは frames[nframes-1]（frames[nframes] は未初期化
         * — 実測でゴミ is_new により return 値が this に化けた） */
        if (nframes > 0 && frames[nframes - 1].is_new && !akl_is_objv(v)) v = stk[base];
        nframes--;
        base = frames[nframes].base;
        cur = frames[nframes].func;
        pc = code + frames[nframes].ret_off;
        AKL_PUSH(v);
        AKL_NEXT();
    }
    AKL_L(MAKEF): {
        u32 fidx;
        memcpy(&fidx, pc, 4); pc += 4;
        u8 srcslot = *pc++;
        rt->gc_sp = sp; /* obj_new の GC 発火に備えてルート深さを同期 */
        u32 oi = akl_obj_new(rt);
        if (oi == UINT32_MAX) { free(frames); return false; }
        AklObj *o = &rt->objs[oi];
        o->kind = AKL_OK_FUNC;
        o->code_off = fidx; /* FUNC obj の code_off は「func 表 index」を指す（名前の再利用） */
        /* クロージャ捕捉環境: 現在 frame の env チェーン先頭（自前 ENV or cap ENV） */
        o->env = UINT32_MAX;
        if (srcslot && sp > base) {
            AklVal ev = stk[base + srcslot];
            if (akl_is_objv(ev)) o->env = akl_get_obj(ev);
        }
        AKL_PUSH(AKL_MK_OBJ(oi));
        AKL_NEXT();
    }
    AKL_L(MAKEFS): {
        u32 fidx;
        memcpy(&fidx, pc, 4); pc += 4;
        u8 srcslot = *pc++;
        if (sp < base + 2 || fidx >= rt->n_funcs) { akl_errf(rt, "stack underflow: makefs"); free(frames); return false; }
        AklVal parent = stk[sp - 1]; /* レイアウト [class][class][parent] の最上位が親 */
        rt->gc_sp = sp;
        u32 oi = akl_obj_new(rt);
        if (oi == UINT32_MAX) { free(frames); return false; }
        AklObj *o = &rt->objs[oi];
        o->kind = AKL_OK_FUNC;
        o->code_off = fidx;
        /* 従来の捕捉環境（外側ローカルの capture） */
        o->env = UINT32_MAX;
        if (srcslot && sp > base) {
            AklVal ev = stk[base + srcslot];
            if (akl_is_objv(ev)) o->env = akl_get_obj(ev);
        }
        /* 親 ENV を先頭に挿入: 新 ENV(vals=[parent], parent=捕捉環境)。
         * SUPERGET はこの vals[0] を親クラスとして読む。 */
        u32 eo = akl_obj_new(rt);
        if (eo == UINT32_MAX) { free(frames); return false; }
        AklObj *en = &rt->objs[eo];
        en->kind = AKL_OK_ENV;
        en->u.env.vals = (AklVal *)malloc(sizeof(AklVal));
        if (!en->u.env.vals) { akl_errf(rt, "oom: makefs env"); free(frames); return false; }
        en->u.env.vals[0] = parent;
        en->u.env.n = 1;
        en->u.env.parent = o->env;
        rt->heap_bytes += sizeof(AklVal);
        o->env = eo;
        /* レイアウト: cg は [class][class][parent] で MAKEFS を発行（DUP 後 LLOAD）。
         * parent の位置を fn で置換する（sp 不変）→ [class][class][fn]。
         * 続く PSTORE が class を 1 個消費し、[class] が残る。 */
        stk[sp - 1] = AKL_MK_OBJ(oi);
        AKL_NEXT();
    }
    AKL_L(OBJNEW): {
        rt->gc_sp = sp; /* obj_new の GC 発火に備えてルート深さを同期 */
        u32 oi = akl_obj_new(rt);
        if (oi == UINT32_MAX) { free(frames); return false; }
        rt->objs[oi].kind = AKL_OK_OBJ;
        AKL_PUSH(AKL_MK_OBJ(oi));
        AKL_NEXT();
    }
    AKL_L(PLOAD): {
        u32 name;
        memcpy(&name, pc, 4); pc += 4;
        AklVal ov = AKL_POP();
        if (akl_is_objv(ov) && rt->objs[akl_get_obj(ov)].kind == AKL_OK_HANDLE) {
            AklObj *ho = &rt->objs[akl_get_obj(ov)];
            u32 nl;
            const u8 *nb = akl_str(rt, name, &nl);
            if (!nb) { free(frames); return false; }
            AklVal out = AKL_VAL_UNDEF;
            if (ho->u.hd.vt && ho->u.hd.vt->get) {
                rt->gc_sp = sp + 1; /* pop 後でも ov は stk[sp] に残存値として mark されるよう同期 */
                u32 nur0 = rt->n_nury;
                bool ok_h = ho->u.hd.vt->get(rt, ho->u.hd.ptr, (const char *)nb, nl, &out);
                if (rt->native_err) { free(frames); return false; }
                rt->n_nury = nur0; /* out は直後 PUSH で根付く（間に GC 契機なし） */
                if (!ok_h) out = AKL_VAL_UNDEF;
            }
            AKL_PUSH(out);
            AKL_NEXT();
        }
        if (akl_is_objv(ov)) {
            AklObj *oo = &rt->objs[akl_get_obj(ov)];
            if (oo->kind == AKL_OK_ARR) {
                u32 nl;
                const u8 *nb = akl_str(rt, name, &nl);
                if (!nb) { free(frames); return false; }
                if (nl == 6 && memcmp(nb, "length", 6) == 0) {
                    AKL_PUSH(AKL_MK_INT((i32)oo->u.arr.n));
                    AKL_NEXT();
                }
                u32 hit_a = UINT32_MAX;
                for (u32 i = 0; i < AKL_ARR_METH_N; i++)
                    if (strlen(AKL_ARR_METHODS[i].name) == nl &&
                        memcmp(AKL_ARR_METHODS[i].name, nb, nl) == 0) { hit_a = i; break; }
                if (hit_a != UINT32_MAX) { AKL_PUSH(rt->arr_meth_vals[hit_a]); AKL_NEXT(); }
                AKL_PUSH(AKL_VAL_UNDEF);
                AKL_NEXT();
            }
            if (oo->kind == AKL_OK_STR || oo->kind == AKL_OK_ROPE) {
                u32 nl;
                const u8 *nb = akl_str(rt, name, &nl);
                if (!nb) { free(frames); return false; }
                if (nl == 6 && memcmp(nb, "length", 6) == 0) {
                    u32 ln;
                    const u8 *bp = akl_str(rt, akl_get_obj(ov), &ln);
                    if (rt->err[0]) { free(frames); return false; }
                    AKL_PUSH(AKL_MK_INT((i32)akl_str_cp_count(bp, ln)));
                    AKL_NEXT();
                }
                u32 hit_s = UINT32_MAX;
                for (u32 i = 0; i < AKL_STR_METH_N; i++)
                    if (strlen(AKL_STR_METHODS[i].name) == nl &&
                        memcmp(AKL_STR_METHODS[i].name, nb, nl) == 0) { hit_s = i; break; }
                if (hit_s != UINT32_MAX) { AKL_PUSH(rt->str_meth_vals[hit_s]); AKL_NEXT(); }
                AKL_PUSH(AKL_VAL_UNDEF);
                AKL_NEXT();
            }
            if (oo->kind == AKL_OK_REGEX) {
                u32 nl;
                const u8 *nb = akl_str(rt, name, &nl);
                if (!nb) { free(frames); return false; }
                AklRex *rx = oo->u.rex.rx;
                u32 rf = oo->u.rex.flags;
                AklVal out = AKL_VAL_UNDEF;
                if (nl == 6 && memcmp(nb, "source", 6) == 0) {
                    u32 pl = 0;
                    const u8 *pp = akl_rex_pat(rx, &pl);
                    out = akl_mkstring(rt, (const char *)pp, pl);
                } else if (nl == 5 && memcmp(nb, "flags", 5) == 0) {
                    char fb[8];
                    u32 fw = 0;
                    if (rf & AKL_RX_F_GLOBAL) fb[fw++] = 'g';
                    if (rf & AKL_RX_F_IGNORE) fb[fw++] = 'i';
                    if (rf & AKL_RX_F_MULTI) fb[fw++] = 'm';
                    if (rf & AKL_RX_F_DOTALL) fb[fw++] = 's';
                    if (rf & AKL_RX_F_UNICODE) fb[fw++] = 'u';
                    if (rf & AKL_RX_F_STICKY) fb[fw++] = 'y';
                    out = akl_mkstring(rt, fb, fw);
                } else if (nl == 6 && memcmp(nb, "global", 6) == 0) {
                    out = akl_mkbool((rf & AKL_RX_F_GLOBAL) != 0);
                } else if (nl == 10 && memcmp(nb, "ignoreCase", 10) == 0) {
                    out = akl_mkbool((rf & AKL_RX_F_IGNORE) != 0);
                } else if (nl == 9 && memcmp(nb, "multiline", 9) == 0) {
                    out = akl_mkbool((rf & AKL_RX_F_MULTI) != 0);
                } else if (nl == 6 && memcmp(nb, "dotAll", 6) == 0) {
                    out = akl_mkbool((rf & AKL_RX_F_DOTALL) != 0);
                } else if (nl == 6 && memcmp(nb, "sticky", 6) == 0) {
                    out = akl_mkbool((rf & AKL_RX_F_STICKY) != 0);
                } else if (nl == 7 && memcmp(nb, "unicode", 7) == 0) {
                    out = akl_mkbool((rf & AKL_RX_F_UNICODE) != 0);
                } else if (nl == 9 && memcmp(nb, "lastIndex", 9) == 0) {
                    out = AKL_MK_INT(oo->u.rex.last_index);
                } else {
                    u32 hit_r = UINT32_MAX;
                    for (u32 i = 0; i < AKL_REGEX_METH_N; i++)
                        if (strlen(AKL_REGEX_METHODS[i].name) == nl &&
                            memcmp(AKL_REGEX_METHODS[i].name, nb, nl) == 0) { hit_r = i; break; }
                    if (hit_r != UINT32_MAX) { AKL_PUSH(rt->regex_meth_vals[hit_r]); AKL_NEXT(); }
                }
                AKL_PUSH(out);
                AKL_NEXT();
            }
        }
        if (!akl_is_objv(ov) || rt->objs[akl_get_obj(ov)].kind != AKL_OK_OBJ) {
            akl_errf(rt, "TypeError: property access on non-object value");
            free(frames); return false;
        }
        AklObj *o = &rt->objs[akl_get_obj(ov)];
        i32 pi = obj_prop_find(o, name);
        if (pi < 0 && o->kind == AKL_OK_OBJ) {
            /* クラス継承の static: __super チェーンを辿って解決（深さ制限） */
            u32 sup_name = akl_intern(rt, (const u8 *)"\x00super", 7, NULL);
            AklObj *co = o;
            u32 cdepth = 0;
            while (pi < 0 && co->kind == AKL_OK_OBJ && cdepth++ < 64) {
                i32 spi = obj_prop_find(co, sup_name);
                if (spi < 0) break;
                AklVal pv = co->u.po.props[spi].v;
                if (!akl_is_objv(pv)) break;
                co = &rt->objs[akl_get_obj(pv)];
                pi = obj_prop_find(co, name);
            }
            if (pi >= 0) { AKL_PUSH(co->u.po.props[pi].v); AKL_NEXT(); }
        }
        if (pi >= 0) { AKL_PUSH(o->u.po.props[pi].v); AKL_NEXT(); }
        /* getter 自動呼び出し: 通常 prop が無ければ "get:\x01name" を探して this=obj で呼ぶ */
        if (o->kind == AKL_OK_OBJ) {
            u32 nl2;
            const u8 *nb2 = akl_str(rt, name, &nl2);
            if (!nb2) { free(frames); return false; }
            u8 gbuf[260];
            if (nl2 + 5 <= sizeof gbuf) {
                memcpy(gbuf, "get", 3);
                gbuf[3] = 0x01;
                memcpy(gbuf + 4, nb2, nl2);
                rt->gc_sp = sp + 1; /* ov は stk[sp] に残存（GC ルート） */
                u32 gname = akl_intern(rt, gbuf, 4 + nl2, NULL);
                if (gname != UINT32_MAX) {
                    i32 gi = obj_prop_find(o, gname);
                    if (gi >= 0) {
                        AklVal fn = o->u.po.props[gi].v;
                        if (akl_is_objv(fn)) {
                            AklObj *gfo = &rt->objs[akl_get_obj(fn)];
                            if (gfo->kind == AKL_OK_NATIVE) {
                                if (budget < AKL_NATIVE_COST) { akl_errf(rt, "instruction budget exhausted"); free(frames); return false; }
                                budget -= AKL_NATIVE_COST;
                                u32 nur0 = rt->n_nury;
                                AklVal gout;
                                if (!akl_vm_native_call(rt, gfo, ov, 0, NULL, &gout)) { free(frames); return false; }
                                rt->n_nury = nur0;
                                AKL_PUSH(gout);
                                AKL_NEXT();
                            }
                            if (gfo->kind == AKL_OK_FUNC) {
                                u32 gfid = akl_get_obj(fn);
                                u32 gfei = rt->objs[gfid].code_off;
                                if (gfei >= rt->n_funcs) { akl_errf(rt, "internal: bad func ref"); free(frames); return false; }
                                AklFuncEnt *gfe = &rt->funcs[gfei];
                                if (nframes >= AKL_MAX_DEPTH) { akl_errf(rt, "call depth budget exhausted"); free(frames); return false; }
                                u32 gnh = 1 + (gfe->n_env ? 1u : 0u) + (gfe->n_cap ? 1u : 0u);
                                u32 gwin = sp; /* ov pop 済み。ここからフレーム */
                                u32 gnloc = gfe->n_locals;
                                AKL_GROW_TO(gwin + gnloc + 4);
                                if (gnh > 1) {
                                    rt->gc_sp = sp;
                                    if (!akl_vm_frame_hidden(rt, stk, gwin, gnloc, gfe, ov,
                                        gfo->env == UINT32_MAX ? AKL_VAL_UNDEF : AKL_MK_OBJ(gfo->env))) {
                                        free(frames);
                                        return false;
                                    }
                                } else {
                                    stk[gwin] = ov;
                                }
                                for (u32 gi2 = gnh; gi2 < gnloc; gi2++) stk[gwin + gi2] = AKL_VAL_UNDEF;
                                sp = gwin + gnloc;
                                frames[nframes].ret_off = (u32)(pc - code);
                                frames[nframes].base = base;
                                frames[nframes].func = cur;
                                frames[nframes].is_new = 0;
                                nframes++;
                                base = gwin;
                                cur = gfei;
                                pc = code + rt->funcs[cur].code_off;
                                AKL_NEXT();
                            }
                        }
                    }
                }
            }
        }
        AKL_PUSH(AKL_VAL_UNDEF); /* 未定義 prop → undefined（JS 同様） */
        AKL_NEXT();
    }
    AKL_L(PSTORE): {
        u32 name;
        memcpy(&name, pc, 4); pc += 4;
        AklVal v = AKL_POP();
        AklVal ov = AKL_POP();
        if (akl_is_objv(ov) && rt->objs[akl_get_obj(ov)].kind == AKL_OK_HANDLE) {
            AklObj *ho = &rt->objs[akl_get_obj(ov)];
            u32 nl;
            const u8 *nb = akl_str(rt, name, &nl);
            if (!nb) { free(frames); return false; }
            if (!ho->u.hd.vt || !ho->u.hd.vt->set) {
                akl_errf(rt, "TypeError: property store on handle without setter");
                free(frames); return false;
            }
            rt->gc_sp = sp + 2; /* pop 済み ov/v も stk 残存値として mark（set 内 GC 発火対策） */
            u32 nur0 = rt->n_nury;
            bool ok_h = ho->u.hd.vt->set(rt, ho->u.hd.ptr, (const char *)nb, nl, v);
            if (rt->native_err) { free(frames); return false; }
            rt->n_nury = nur0;
            if (!ok_h) { akl_errf(rt, "TypeError: property store rejected"); free(frames); return false; }
            AKL_PUSH(v); /* 代入式の値は右辺（JS 同様） */
            AKL_NEXT();
        }
        if (akl_is_objv(ov)) {
            AklObj *oo = &rt->objs[akl_get_obj(ov)];
            if (oo->kind == AKL_OK_REGEX) {
                u32 nl;
                const u8 *nb = akl_str(rt, name, &nl);
                if (!nb) { free(frames); return false; }
                if (nl == 9 && memcmp(nb, "lastIndex", 9) == 0) {
                    double d = akl_to_integer(rt, v);
                    if (rt->err[0]) { free(frames); return false; }
                    oo->u.rex.last_index = (i32)d;
                }
                /* その他のプロパティ代入は静かに無視（独自プロパティ非対応。AKL_COMPAT） */
                AKL_PUSH(v);
                AKL_NEXT();
            }
            if (oo->kind == AKL_OK_ARR) {
                u32 nl;
                const u8 *nb = akl_str(rt, name, &nl);
                if (!nb) { free(frames); return false; }
                if (nl == 6 && memcmp(nb, "length", 6) == 0) {
                    /* arr.length = n: 切り詰め / undefined で拡張（JS 準拠） */
                    double d = akl_to_integer(rt, v);
                    if (rt->err[0]) { free(frames); return false; }
                    if (d < 0 || isnan(d)) { akl_errf(rt, "RangeError: invalid array length"); free(frames); return false; }
                    u32 n = (u32)d;
                    if (n < oo->u.arr.n) {
                        oo->u.arr.n = n; /* 切り詰め（要素は残骸。GC mark 範囲は n のみ） */
                    } else if (n > oo->u.arr.n) {
                        if (!akl_arr_grow(rt, oo, n)) { free(frames); return false; }
                        for (u32 i = oo->u.arr.n; i < n; i++) oo->u.arr.v[i] = AKL_VAL_UNDEF;
                        oo->u.arr.n = n;
                    }
                    AKL_PUSH(v);
                    AKL_NEXT();
                }
                /* 他のプロパティ代入は静かに無視（名前付きプロパティ非対応。AKL_COMPAT） */
                AKL_PUSH(v);
                AKL_NEXT();
            }
        }
        if (!akl_is_objv(ov) || rt->objs[akl_get_obj(ov)].kind != AKL_OK_OBJ) {
            akl_errf(rt, "TypeError: property store on non-object value");
            free(frames); return false;
        }
        AklObj *so = &rt->objs[akl_get_obj(ov)];
        if (so->kind == AKL_OK_OBJ) {
            /* setter 自動呼び出し: 通常 prop に代入する前に "set:\x01name" を探す */
            u32 nl3;
            const u8 *nb3 = akl_str(rt, name, &nl3);
            if (!nb3) { free(frames); return false; }
            u8 sbuf[260];
            if (nl3 + 5 <= sizeof sbuf) {
                memcpy(sbuf, "set", 3);
                sbuf[3] = 0x01;
                memcpy(sbuf + 4, nb3, nl3);
                rt->gc_sp = sp + 2; /* ov/v は stk 残存（GC ルート） */
                u32 sname = akl_intern(rt, sbuf, 4 + nl3, NULL);
                if (sname != UINT32_MAX) {
                    i32 si = obj_prop_find(so, sname);
                    if (si >= 0) {
                        AklVal fn = so->u.po.props[si].v;
                        if (akl_is_objv(fn)) {
                            AklObj *sfo = &rt->objs[akl_get_obj(fn)];
                            if (sfo->kind == AKL_OK_NATIVE) {
                                if (budget < AKL_NATIVE_COST) { akl_errf(rt, "instruction budget exhausted"); free(frames); return false; }
                                budget -= AKL_NATIVE_COST;
                                u32 nur0 = rt->n_nury;
                                AklVal sout;
                                if (!akl_vm_native_call(rt, sfo, ov, 1, &v, &sout)) { free(frames); return false; }
                                rt->n_nury = nur0;
                                AKL_PUSH(v); /* 代入式の値は右辺 */
                                AKL_NEXT();
                            }
                            if (sfo->kind == AKL_OK_FUNC) {
                                u32 sfid = akl_get_obj(fn);
                                u32 sfei = rt->objs[sfid].code_off;
                                if (sfei >= rt->n_funcs) { akl_errf(rt, "internal: bad func ref"); free(frames); return false; }
                                AklFuncEnt *sfe = &rt->funcs[sfei];
                                if (nframes >= AKL_MAX_DEPTH) { akl_errf(rt, "call depth budget exhausted"); free(frames); return false; }
                                u32 snh = 1 + (sfe->n_env ? 1u : 0u) + (sfe->n_cap ? 1u : 0u);
                                u32 swin = sp; /* ov/v pop 済み。フレームはここから */
                                u32 snloc = sfe->n_locals;
                                u32 snpar = sfe->n_params;
                                AKL_GROW_TO(swin + snloc + 4);
                                if (snh > 1) {
                                    rt->gc_sp = sp;
                                    if (!akl_vm_frame_hidden(rt, stk, swin, snloc, sfe, ov,
                                        sfo->env == UINT32_MAX ? AKL_VAL_UNDEF : AKL_MK_OBJ(sfo->env))) {
                                        free(frames);
                                        return false;
                                    }
                                } else {
                                    stk[swin] = ov;
                                }
                                if (snpar > 0) stk[swin + snh] = v; /* 第 1 引数 = 代入値 */
                                for (u32 si2 = snh + (snpar > 0 ? 1u : 0u); si2 < snloc; si2++) stk[swin + si2] = AKL_VAL_UNDEF;
                                sp = swin + snloc;
                                frames[nframes].ret_off = (u32)(pc - code);
                                frames[nframes].base = base;
                                frames[nframes].func = cur;
                                frames[nframes].is_new = 0;
                                nframes++;
                                base = swin;
                                cur = sfei;
                                pc = code + rt->funcs[cur].code_off;
                                AKL_NEXT();
                            }
                        }
                    }
                }
            }
        }
        if (!obj_prop_set(rt, so, name, v)) { free(frames); return false; }
        AKL_PUSH(v); /* 代入式の値は右辺（JS 同様） */
        AKL_NEXT();
    }
    AKL_L(MCALL): {
        u8 argc8 = *pc++;
        memcpy(&mcall_name, pc, 4); pc += 4;
        mcall_argc = argc8;
        goto mcall_common;
    }
    AKL_L(MCALLN): {
        u32 slot;
        memcpy(&slot, pc, 4); pc += 4;
        memcpy(&mcall_name, pc, 4); pc += 4;
        if (sp <= base + slot) { akl_errf(rt, "local OOB read"); free(frames); return false; }
        mcall_argc = akl_get_int(stk[base + slot]);
        if (mcall_argc < 0) mcall_argc = 0;
        goto mcall_common;
    }
    mcall_common: {
        i32 argc = mcall_argc;
        u32 name = mcall_name;
        if (argc > 250 || sp < base + argc + 1) { akl_errf(rt, "stack underflow: mcall"); free(frames); return false; }
        AklVal ov = stk[sp - argc - 1];
        if (akl_is_objv(ov) && rt->objs[akl_get_obj(ov)].kind == AKL_OK_HANDLE) {
            AklObj *ho = &rt->objs[akl_get_obj(ov)];
            u32 nl;
            const u8 *nb = akl_str(rt, name, &nl);
            if (!nb) { free(frames); return false; }
            if (!ho->u.hd.vt || !ho->u.hd.vt->call) { akl_errf(rt, "TypeError: not a function"); free(frames); return false; }
            if (budget < AKL_NATIVE_COST) { akl_errf(rt, "instruction budget exhausted"); free(frames); return false; }
            budget -= AKL_NATIVE_COST;
            rt->gc_sp = sp; /* argv/ov は stk 上 = mark 済み */
            u32 nur0 = rt->n_nury;
            AklVal out = AKL_VAL_UNDEF;
            bool ok_h = ho->u.hd.vt->call(rt, ho->u.hd.ptr, (const char *)nb, nl, (int)argc, stk + sp - argc, &out);
            if (rt->native_err) { free(frames); return false; }
            rt->n_nury = nur0;
            if (!ok_h) { akl_errf(rt, "TypeError: not a function"); free(frames); return false; }
            sp -= argc;
            stk[sp - 1] = out; /* obj スロットを結果で潰す */
            AKL_NEXT();
        }
        if (akl_is_objv(ov)) { /* v0.3 組込: 文字列/配列メソッドの直結ディスパッチ */
            AklObj *oo = &rt->objs[akl_get_obj(ov)];
            const AklMethEntry *tbl = NULL;
            u32 tn = 0;
            if (oo->kind == AKL_OK_STR || oo->kind == AKL_OK_ROPE) {
                tbl = AKL_STR_METHODS; tn = AKL_STR_METH_N;
            } else if (oo->kind == AKL_OK_ARR) {
                tbl = AKL_ARR_METHODS; tn = AKL_ARR_METH_N;
            } else if (oo->kind == AKL_OK_REGEX) {
                tbl = AKL_REGEX_METHODS; tn = AKL_REGEX_METH_N;
            }
            if (tbl) {
                u32 nl;
                const u8 *nb = akl_str(rt, name, &nl);
                if (!nb) { free(frames); return false; }
                u32 hit = UINT32_MAX;
                for (u32 i = 0; i < tn; i++)
                    if (strlen(tbl[i].name) == nl && memcmp(tbl[i].name, nb, nl) == 0) { hit = i; break; }
                if (hit != UINT32_MAX) {
                    if (budget < AKL_NATIVE_COST) { akl_errf(rt, "instruction budget exhausted"); free(frames); return false; }
                    budget -= AKL_NATIVE_COST;
                    rt->gc_sp = sp; /* argv/ov は stk 上 = mark 済み */
                    u32 nur0 = rt->n_nury;
                    AklVal r = tbl[hit].fn(rt, ov, (int)argc, stk + sp - argc, (void *)(uintptr_t)hit);
                    if (rt->native_err) { free(frames); return false; }
                    rt->n_nury = nur0;
                    sp -= argc;
                    stk[sp - 1] = r;
                    AKL_NEXT();
                }
            }
        }
        AklVal fv = AKL_VAL_UNDEF;
        if (akl_is_objv(ov)) {
            AklObj *o = &rt->objs[akl_get_obj(ov)];
            if (o->kind == AKL_OK_OBJ) {
                i32 pi = obj_prop_find(o, name);
                if (pi < 0 && o->kind == AKL_OK_OBJ) {
                    /* クラス継承の static: __super チェーンを辿って解決（深さ制限） */
                    u32 sup_name = akl_intern(rt, (const u8 *)"\x00super", 7, NULL);
                    AklObj *co = o;
                    u32 cdepth = 0;
                    while (pi < 0 && co->kind == AKL_OK_OBJ && cdepth++ < 64) {
                        i32 spi = obj_prop_find(co, sup_name);
                        if (spi < 0) break;
                        AklVal pv = co->u.po.props[spi].v;
                        if (!akl_is_objv(pv)) break;
                        co = &rt->objs[akl_get_obj(pv)];
                        pi = obj_prop_find(co, name);
                    }
                    if (pi >= 0) fv = co->u.po.props[pi].v;
                } else if (pi >= 0) {
                    fv = o->u.po.props[pi].v;
                }
            }
        }
        if (!akl_is_objv(fv)) { akl_errf(rt, "TypeError: not a function"); free(frames); return false; }
        AklObj *fo = &rt->objs[akl_get_obj(fv)];
        if (fo->kind == AKL_OK_NATIVE) {
            if (budget < AKL_NATIVE_COST) { akl_errf(rt, "instruction budget exhausted"); free(frames); return false; }
            budget -= AKL_NATIVE_COST;
            rt->gc_sp = sp;
            u32 nur0 = rt->n_nury;
            AklVal r;
            if (!akl_vm_native_call(rt, fo, ov, argc, stk + sp - argc, &r)) { free(frames); return false; }
            rt->n_nury = nur0;
            sp -= argc;
            stk[sp - 1] = r;
            AKL_NEXT();
        }
        if (fo->kind != AKL_OK_FUNC) { akl_errf(rt, "TypeError: not a function"); free(frames); return false; }
        /* バイトコード関数のメソッド呼出: this = レシーバ（frame hidden slot 0） */
        u32 fidx = akl_get_obj(fv);
        {
            u32 fe_i = rt->objs[fidx].code_off;
            if (fe_i >= rt->n_funcs) { akl_errf(rt, "internal: bad func ref"); free(frames); return false; }
            AklFuncEnt *fe = &rt->funcs[fe_i];
            if (nframes >= AKL_MAX_DEPTH) { akl_errf(rt, "call depth budget exhausted"); free(frames); return false; }
            u32 nh = 1 + (fe->n_env ? 1u : 0u) + (fe->n_cap ? 1u : 0u);
            u32 win = sp - argc - 1;
            u32 nloc = fe->n_locals, npar = fe->n_params;
            u32 k = nh - 1;
            AKL_GROW_TO(win + k + argc + 8);
            if (k) {
                for (i32 i = (i32)argc - 1; i >= 0; i--) stk[win + k + i + 1] = stk[win + i + 1];
            }
            if (nh > 1) {
                rt->gc_sp = sp;
                if (!akl_vm_frame_hidden(rt, stk, win, nloc, fe, ov,
                                         fo->env == UINT32_MAX ? AKL_VAL_UNDEF : AKL_MK_OBJ(fo->env))) {
                    free(frames);
                    return false;
                }
            } else {
                stk[win] = ov; /* this = レシーバ */
            }
            u32 keep = argc < npar ? argc : npar;
            for (u32 i = nh + keep; i < nloc; i++) stk[win + i] = AKL_VAL_UNDEF;
            sp = win + nloc;
            frames[nframes].ret_off = (u32)(pc - code);
            frames[nframes].base = base;
            frames[nframes].func = cur;
            frames[nframes].is_new = 0;
            nframes++;
            base = win;
            cur = fe_i;
            pc = code + rt->funcs[cur].code_off;
            AKL_NEXT();
        }
    }
    AKL_L(LINC): {
        u32 slot;
        memcpy(&slot, pc, 4); pc += 4;
        i32 d;
        memcpy(&d, pc, 4); pc += 4;
        if (base + slot >= sp) { akl_errf(rt, "local OOB write"); free(frames); return false; }
        AklVal lv = stk[base + slot];
        AklVal nv;
        if (akl_is_intv(lv)) {
            i64 r = (i64)akl_get_int(lv) + (i64)d;
            if (r >= -2147483648ll && r <= 2147483647ll) nv = AKL_MK_INT((i32)r);
            else nv = akl_num((double)r);
        } else {
            /* x = x + d の汎用経路（文字列連結を含む。オペランド順序は左=x, 右=d で保持） */
            if (!akl_bin_add(rt, lv, AKL_MK_INT(d), sp, &nv)) { free(frames); return false; }
        }
        stk[base + slot] = nv;
        rt->last_val = nv; /* 式文の POPV と同義（N_EXPRSTMT 融合との整合） */
        AKL_NEXT();
    }
    AKL_L(GLOAD_S): {
        u32 gi;
        memcpy(&gi, pc, 4); pc += 4;
        if (gi >= rt->n_globals) { akl_errf(rt, "internal: global slot OOB"); free(frames); return false; }
        AKL_PUSH(rt->globals[gi].v);
        AKL_NEXT();
    }
    AKL_L(GSTORE_S): {
        u32 gi;
        memcpy(&gi, pc, 4); pc += 4;
        if (gi >= rt->n_globals) { akl_errf(rt, "internal: global slot OOB"); free(frames); return false; }
        rt->globals[gi].v = AKL_POP();
        AKL_NEXT();
    }
    AKL_L(GINC): {
        u32 gi;
        memcpy(&gi, pc, 4); pc += 4;
        i32 d;
        memcpy(&d, pc, 4); pc += 4;
        if (gi >= rt->n_globals) { akl_errf(rt, "internal: global slot OOB"); free(frames); return false; }
        AklVal lv = rt->globals[gi].v;
        AklVal nv;
        if (akl_is_intv(lv)) {
            i64 r = (i64)akl_get_int(lv) + (i64)d;
            if (r >= -2147483648ll && r <= 2147483647ll) nv = AKL_MK_INT((i32)r);
            else nv = akl_num((double)r);
        } else {
            if (!akl_bin_add(rt, lv, AKL_MK_INT(d), sp, &nv)) { free(frames); return false; }
        }
        rt->globals[gi].v = nv;
        rt->last_val = nv;
        AKL_NEXT();
    }
    /* ==== 融合命令群。各汎用路は非融合の命令列が呼ぶ関数と同じものを同順序で呼び、
     * int fast path の丸めも i64 積/和の一回 double 化で一致させている ==== */
    AKL_L(GMULC): {
        u32 gi; memcpy(&gi, pc, 4); pc += 4;
        i32 imm; memcpy(&imm, pc, 4); pc += 4;
        if (gi >= rt->n_globals) { akl_errf(rt, "internal: global slot OOB"); free(frames); return false; }
        AklVal lv = rt->globals[gi].v;
        if (akl_is_intv(lv)) {
            i64 r = (i64)akl_get_int(lv) * (i64)imm;
            if (r >= -2147483648ll && r <= 2147483647ll) { AKL_PUSH(AKL_MK_INT((i32)r)); AKL_NEXT(); }
            AKL_PUSH(akl_num((double)r)); AKL_NEXT();
        }
        rt->gc_sp = sp; /* 文字列なら to_number が flatten し得る */
        AKL_PUSH(akl_num(akl_canon(akl_to_number(rt, lv) * (double)imm)));
        if (rt->err[0]) { free(frames); return false; }
        AKL_NEXT();
    }
    AKL_L(LMULC): {
        u32 slot; memcpy(&slot, pc, 4); pc += 4;
        i32 imm; memcpy(&imm, pc, 4); pc += 4;
        if (base + slot >= sp) { akl_errf(rt, "local OOB read"); free(frames); return false; }
        AklVal lv = stk[base + slot];
        if (akl_is_intv(lv)) {
            i64 r = (i64)akl_get_int(lv) * (i64)imm;
            if (r >= -2147483648ll && r <= 2147483647ll) { AKL_PUSH(AKL_MK_INT((i32)r)); AKL_NEXT(); }
            AKL_PUSH(akl_num((double)r)); AKL_NEXT();
        }
        rt->gc_sp = sp;
        AKL_PUSH(akl_num(akl_canon(akl_to_number(rt, lv) * (double)imm)));
        if (rt->err[0]) { free(frames); return false; }
        AKL_NEXT();
    }
    AKL_L(MULCI): {
        i32 imm; memcpy(&imm, pc, 4); pc += 4;
        AklVal v = AKL_POP(), out;
        if (!akl_cist_compute(rt, OP_MUL, v, imm, sp, &out)) { free(frames); return false; }
        AKL_PUSH(out);
        AKL_NEXT();
    }
    AKL_L(ADDCI): {
        i32 imm; memcpy(&imm, pc, 4); pc += 4;
        AklVal v = AKL_POP(), out;
        if (!akl_cist_compute(rt, OP_ADD, v, imm, sp, &out)) { free(frames); return false; }
        AKL_PUSH(out);
        AKL_NEXT();
    }
    AKL_L(SUBCI): {
        i32 imm; memcpy(&imm, pc, 4); pc += 4;
        AklVal v = AKL_POP(), out;
        if (!akl_cist_compute(rt, OP_SUB, v, imm, sp, &out)) { free(frames); return false; }
        AKL_PUSH(out);
        AKL_NEXT();
    }
    AKL_L(MODCI): {
        i32 imm; memcpy(&imm, pc, 4); pc += 4;
        AklVal v = AKL_POP(), out;
        if (!akl_cist_compute(rt, OP_MOD, v, imm, sp, &out)) { free(frames); return false; }
        AKL_PUSH(out);
        AKL_NEXT();
    }
    AKL_L(GADD_P): {
        u32 gi; memcpy(&gi, pc, 4); pc += 4;
        if (gi >= rt->n_globals) { akl_errf(rt, "internal: global slot OOB"); free(frames); return false; }
        AklVal lv = rt->globals[gi].v;
        AklVal tv = AKL_POP();
        if (akl_is_intv(lv) && akl_is_intv(tv)) {
            i64 r = (i64)akl_get_int(lv) + (i64)akl_get_int(tv);
            if (r >= -2147483648ll && r <= 2147483647ll) { AKL_PUSH(AKL_MK_INT((i32)r)); AKL_NEXT(); }
            AKL_PUSH(akl_num((double)r)); AKL_NEXT();
        }
        rt->gc_sp = sp;
        AklVal out;
        if (!akl_bin_add(rt, lv, tv, sp, &out)) { free(frames); return false; }
        AKL_PUSH(out);
        AKL_NEXT();
    }
    AKL_L(LADD_P): {
        u32 slot; memcpy(&slot, pc, 4); pc += 4;
        if (base + slot >= sp) { akl_errf(rt, "local OOB read"); free(frames); return false; }
        AklVal lv = stk[base + slot];
        AklVal tv = AKL_POP();
        if (akl_is_intv(lv) && akl_is_intv(tv)) {
            i64 r = (i64)akl_get_int(lv) + (i64)akl_get_int(tv);
            if (r >= -2147483648ll && r <= 2147483647ll) { AKL_PUSH(AKL_MK_INT((i32)r)); AKL_NEXT(); }
            AKL_PUSH(akl_num((double)r)); AKL_NEXT();
        }
        rt->gc_sp = sp;
        AklVal out;
        if (!akl_bin_add(rt, lv, tv, sp, &out)) { free(frames); return false; }
        AKL_PUSH(out);
        AKL_NEXT();
    }
    AKL_L(GADD_G): {
        u32 gd; memcpy(&gd, pc, 4); pc += 4;
        u32 gs; memcpy(&gs, pc, 4); pc += 4;
        if (gd >= rt->n_globals || gs >= rt->n_globals) { akl_errf(rt, "internal: global slot OOB"); free(frames); return false; }
        AklVal lv = rt->globals[gd].v, rv = rt->globals[gs].v, nv;
        if (akl_is_intv(lv) && akl_is_intv(rv)) {
            i64 r = (i64)akl_get_int(lv) + (i64)akl_get_int(rv);
            nv = (r >= -2147483648ll && r <= 2147483647ll) ? AKL_MK_INT((i32)r) : akl_num((double)r);
        } else {
            rt->gc_sp = sp;
            if (!akl_bin_add(rt, lv, rv, sp, &nv)) { free(frames); return false; }
        }
        rt->globals[gd].v = nv;
        rt->last_val = nv; /* 式文の POPV 同義 */
        AKL_NEXT();
    }
    AKL_L(CJMPF_MODG): {
        u32 gi; memcpy(&gi, pc, 4); pc += 4;
        i32 m; memcpy(&m, pc, 4); pc += 4;
        i32 k; memcpy(&k, pc, 4); pc += 4;
        u8 cmp = *pc++;
        u32 tgt; memcpy(&tgt, pc, 4); pc += 4;
        if (gi >= rt->n_globals) { akl_errf(rt, "internal: global slot OOB"); free(frames); return false; }
        AklVal lv = rt->globals[gi].v;
        double dk = (double)k;
        bool eq;
        if (akl_is_intv(lv) && m != 0 && !(akl_get_int(lv) == INT32_MIN && m == -1)) {
            eq = (double)akl_mod_imm(rt, akl_get_int(lv), m) == dk; /* x≥0&&m>1 なら magic */
        } else {
            rt->gc_sp = sp;
            double dv = akl_to_number(rt, lv);
            if (rt->err[0]) { free(frames); return false; }
            eq = !isnan(dv) && fmod(dv, (double)m) == dk;
        }
        bool cond = cmp == 0 ? eq : !eq; /* cmp 0: ==k, 1: !=k（JMPF 融合なので cond 偽で tgt へ） */
        if (!cond) pc = code + tgt;
        AKL_NEXT();
    }
    AKL_L(CJMPF_MODL): {
        u32 slot; memcpy(&slot, pc, 4); pc += 4;
        i32 m; memcpy(&m, pc, 4); pc += 4;
        i32 k; memcpy(&k, pc, 4); pc += 4;
        u8 cmp = *pc++;
        u32 tgt; memcpy(&tgt, pc, 4); pc += 4;
        if (base + slot >= sp) { akl_errf(rt, "local OOB read"); free(frames); return false; }
        AklVal lv = stk[base + slot];
        double dk = (double)k;
        bool eq;
        if (akl_is_intv(lv) && m != 0 && !(akl_get_int(lv) == INT32_MIN && m == -1)) {
            eq = (double)akl_mod_imm(rt, akl_get_int(lv), m) == dk; /* x≥0&&m>1 なら magic */
        } else {
            rt->gc_sp = sp;
            double dv = akl_to_number(rt, lv);
            if (rt->err[0]) { free(frames); return false; }
            eq = !isnan(dv) && fmod(dv, (double)m) == dk;
        }
        bool cond = cmp == 0 ? eq : !eq;
        if (!cond) pc = code + tgt;
        AKL_NEXT();
    }
    AKL_L(GSTORE_SPV): {
        u32 gi; memcpy(&gi, pc, 4); pc += 4;
        if (gi >= rt->n_globals) { akl_errf(rt, "internal: global slot OOB"); free(frames); return false; }
        AklVal v = AKL_POP();
        rt->globals[gi].v = v;
        rt->last_val = v; /* POPV 同義 */
        AKL_NEXT();
    }
    AKL_L(LSTORE_PV): {
        u32 slot; memcpy(&slot, pc, 4); pc += 4;
        if (base + slot >= sp) { akl_errf(rt, "local OOB write"); free(frames); return false; }
        AklVal v = AKL_POP();
        stk[base + slot] = v;
        rt->last_val = v;
        AKL_NEXT();
    }
    /* *CI + STORE_PV 再融合: `dst = expr op imm;` の 1 命令形。計算は akl_cist_compute
     * 共有なので「*CI; STORE_PV 逐語実行」と結果・副作用順序が一致する */
#define AKL_XCIG(BASEOP) { \
        u32 gi_; memcpy(&gi_, pc, 4); pc += 4; \
        if (gi_ >= rt->n_globals) { akl_errf(rt, "internal: global slot OOB"); free(frames); return false; } \
        i32 imm_; memcpy(&imm_, pc, 4); pc += 4; \
        AklVal lv_ = AKL_POP(), nv_; \
        if (!akl_cist_compute(rt, (BASEOP), lv_, imm_, sp, &nv_)) { free(frames); return false; } \
        rt->globals[gi_].v = nv_; \
        rt->last_val = nv_; \
        AKL_NEXT(); }
#define AKL_XCIL(BASEOP) { \
        u32 sl_; memcpy(&sl_, pc, 4); pc += 4; \
        if (base + sl_ >= sp) { akl_errf(rt, "local OOB write"); free(frames); return false; } \
        i32 imm_; memcpy(&imm_, pc, 4); pc += 4; \
        AklVal lv_ = AKL_POP(), nv_; \
        if (!akl_cist_compute(rt, (BASEOP), lv_, imm_, sp, &nv_)) { free(frames); return false; } \
        stk[base + sl_] = nv_; \
        rt->last_val = nv_; \
        AKL_NEXT(); }
    AKL_L(ADDCI_G): AKL_XCIG(OP_ADD)
    AKL_L(SUBCI_G): AKL_XCIG(OP_SUB)
    AKL_L(MULCI_G): AKL_XCIG(OP_MUL)
    AKL_L(MODCI_G): AKL_XCIG(OP_MOD)
    AKL_L(ADDCI_L): AKL_XCIL(OP_ADD)
    AKL_L(SUBCI_L): AKL_XCIL(OP_SUB)
    AKL_L(MULCI_L): AKL_XCIL(OP_MUL)
    AKL_L(MODCI_L): AKL_XCIL(OP_MOD)
    /* dst = TOS op slot 再融合。計算は akl_binfv_compute 共有で
     * 「GLOAD_S src; OP; STORE_PV dst」（ローカル版は LLOAD）と逐語同一 */
#define AKL_XGX(BASEOP) { \
        u32 gd_; memcpy(&gd_, pc, 4); pc += 4; \
        u32 gs_; memcpy(&gs_, pc, 4); pc += 4; \
        if (gd_ >= rt->n_globals || gs_ >= rt->n_globals) { akl_errf(rt, "internal: global slot OOB"); free(frames); return false; } \
        AklVal lv_ = AKL_POP(), nv_; \
        rt->gc_sp = sp; \
        if (!akl_binfv_compute(rt, (BASEOP), lv_, rt->globals[gs_].v, sp, &nv_)) { free(frames); return false; } \
        rt->globals[gd_].v = nv_; \
        rt->last_val = nv_; \
        AKL_NEXT(); }
#define AKL_XLX(BASEOP) { \
        u32 ld_; memcpy(&ld_, pc, 4); pc += 4; \
        u32 ls_; memcpy(&ls_, pc, 4); pc += 4; \
        if (base + ld_ >= sp || base + ls_ >= sp) { akl_errf(rt, "local OOB"); free(frames); return false; } \
        AklVal lv_ = AKL_POP(), nv_; \
        rt->gc_sp = sp; \
        if (!akl_binfv_compute(rt, (BASEOP), lv_, stk[base + ls_], sp, &nv_)) { free(frames); return false; } \
        stk[base + ld_] = nv_; \
        rt->last_val = nv_; \
        AKL_NEXT(); }
    AKL_L(ADD_GX): AKL_XGX(OP_ADD)
    AKL_L(SUB_GX): AKL_XGX(OP_SUB)
    AKL_L(MUL_GX): AKL_XGX(OP_MUL)
    AKL_L(MOD_GX): AKL_XGX(OP_MOD)
    AKL_L(ADD_LX): AKL_XLX(OP_ADD)
    AKL_L(SUB_LX): AKL_XLX(OP_SUB)
    AKL_L(MUL_LX): AKL_XLX(OP_MUL)
    AKL_L(MOD_LX): AKL_XLX(OP_MOD)
    AKL_L(CJMPF_MULGG): {
        u32 g1; memcpy(&g1, pc, 4); pc += 4;
        u32 g2; memcpy(&g2, pc, 4); pc += 4;
        u32 g3; memcpy(&g3, pc, 4); pc += 4;
        u8 cmp = *pc++;
        u32 tgt; memcpy(&tgt, pc, 4); pc += 4;
        if (g1 >= rt->n_globals || g2 >= rt->n_globals || g3 >= rt->n_globals) { akl_errf(rt, "internal: global slot OOB"); free(frames); return false; }
        AklVal v1 = rt->globals[g1].v, v2 = rt->globals[g2].v, v3 = rt->globals[g3].v;
        bool r;
        if (akl_is_intv(v1) && akl_is_intv(v2) && akl_is_intv(v3)) {
            i64 m = (i64)akl_get_int(v1) * (i64)akl_get_int(v2);
            i32 iz = akl_get_int(v3);
            if (m >= -2147483648ll && m <= 2147483647ll) {
                i32 im = (i32)m;
                r = cmp == 0 ? im < iz : cmp == 1 ? im <= iz : cmp == 2 ? im > iz : im >= iz;
            } else {
                double dm = (double)m, dz = (double)iz;
                r = cmp == 0 ? dm < dz : cmp == 1 ? dm <= dz : cmp == 2 ? dm > dz : dm >= dz;
            }
        } else {
            /* 非融合（GLOAD;GLOAD;MUL;GLOAD;REL;JMPF）と同一手続き。
             * v1,v2 が int で v3 だけ非 int の場合も MUL int fast+REL 数値化と
             * 同じ（double)(i64積) になることを一回丸めの一意性で確認済み */
            rt->gc_sp = sp;
            double da_, db_;
            akl_to_number2(rt, v1, v2, &da_, &db_);
            double dm = da_ * db_;
            double d3 = akl_to_number(rt, v3);
            if (rt->err[0]) { free(frames); return false; }
            r = !isnan(dm) && !isnan(d3) &&
                (cmp == 0 ? dm < d3 : cmp == 1 ? dm <= d3 : cmp == 2 ? dm > d3 : dm >= d3);
        }
        if (!r) pc = code + tgt;
        AKL_NEXT();
    }
    AKL_L(CJMPF_MODGG): {
        u32 g1; memcpy(&g1, pc, 4); pc += 4;
        u32 g2; memcpy(&g2, pc, 4); pc += 4;
        i32 k; memcpy(&k, pc, 4); pc += 4;
        u8 cmp = *pc++;
        u32 tgt; memcpy(&tgt, pc, 4); pc += 4;
        if (g1 >= rt->n_globals || g2 >= rt->n_globals) { akl_errf(rt, "internal: global slot OOB"); free(frames); return false; }
        AklVal lv = rt->globals[g1].v, mv = rt->globals[g2].v;
        double dk = (double)k;
        bool eq;
        if (akl_is_intv(lv) && akl_is_intv(mv) &&
            akl_get_int(mv) != 0 && !(akl_get_int(lv) == INT32_MIN && akl_get_int(mv) == -1)) {
            eq = (double)(akl_get_int(lv) % akl_get_int(mv)) == dk;
        } else {
            /* MOD の汎用（fmod・NaN 伝播）→ EQ/NE と同一。0 除算は fmod=NaN→eq=false で一致 */
            rt->gc_sp = sp;
            double da_, db_;
            akl_to_number2(rt, lv, mv, &da_, &db_);
            if (rt->err[0]) { free(frames); return false; }
            double dm = fmod(da_, db_);
            eq = !isnan(dm) && dm == dk;
        }
        bool cond = cmp == 0 ? eq : !eq;
        if (!cond) pc = code + tgt;
        AKL_NEXT();
    }
    AKL_L(LADD_LL): {
        u32 dslot; memcpy(&dslot, pc, 4); pc += 4;
        u32 s1; memcpy(&s1, pc, 4); pc += 4;
        u32 s2; memcpy(&s2, pc, 4); pc += 4;
        if (base + dslot >= sp || base + s1 >= sp || base + s2 >= sp) { akl_errf(rt, "local OOB"); free(frames); return false; }
        AklVal va = stk[base + s1], vb = stk[base + s2], nv;
        if (akl_is_intv(va) && akl_is_intv(vb)) {
            i64 r = (i64)akl_get_int(va) + (i64)akl_get_int(vb);
            nv = (r >= -2147483648ll && r <= 2147483647ll) ? AKL_MK_INT((i32)r) : akl_num((double)r);
        } else {
            rt->gc_sp = sp;
            if (!akl_bin_add(rt, va, vb, sp, &nv)) { free(frames); return false; }
        }
        stk[base + dslot] = nv;
        AKL_NEXT();
    }
    AKL_L(RET_L): {
        u32 slot; memcpy(&slot, pc, 4); pc += 4;
        if (base + slot >= sp) { akl_errf(rt, "local OOB read"); free(frames); return false; }
        AklVal v = stk[base + slot];
        /* RET（LLOAD;RET の融合）と同一の巻き戻し順序（finally 連鎖込み） */
        if (rt->n_tries) {
            AKL_VMST_OUT();
            bool ok_ = akl_vm_ret_step(rt, &vmst_, v);
            AKL_VMST_IN();
            if (!ok_) { free(frames); return false; }
            AKL_NEXT();
        }
        sp = base;
        if (!nframes) {
            if (top_ret_ok) { /* エントリ呼び出し（akl_call）: 関数の return = 成功終了 */
                free(frames);
                rt->last_val = v;
                return true;
            }
            free(frames); akl_errf(rt, "internal: ret at top level"); return false;
        }
        if (nframes > 0 && frames[nframes - 1].is_new && !akl_is_objv(v)) v = stk[base];
        nframes--;
        base = frames[nframes].base;
        cur = frames[nframes].func;
        pc = code + frames[nframes].ret_off;
        AKL_PUSH(v);
        AKL_NEXT();
    }
    AKL_L(LOOPINC_G): {
        u32 gi; memcpy(&gi, pc, 4); pc += 4;
        i32 d; memcpy(&d, pc, 4); pc += 4;
        i32 lim; memcpy(&lim, pc, 4); pc += 4;
        u8 cmp = *pc++;
        u32 tgt; memcpy(&tgt, pc, 4); pc += 4;
        if (gi >= rt->n_globals) { akl_errf(rt, "internal: global slot OOB"); free(frames); return false; }
        AklVal lv = rt->globals[gi].v, nv;
        bool r;
        if (akl_is_intv(lv)) {
            i64 s2 = (i64)akl_get_int(lv) + (i64)d;
            if (s2 >= -2147483648ll && s2 <= 2147483647ll) {
                i32 il = (i32)s2;
                nv = AKL_MK_INT(il);
                r = cmp == 0 ? il < lim : cmp == 1 ? il <= lim : cmp == 2 ? il > lim : il >= lim;
            } else {
                nv = akl_num((double)s2);
                double ds = (double)s2, dl = (double)lim;
                r = cmp == 0 ? ds < dl : cmp == 1 ? ds <= dl : cmp == 2 ? ds > dl : ds >= dl;
            }
        } else {
            /* 汎用: GINC + CJMPF_G と同じ手続き（文字列なら bin_add で連結→数値化比較） */
            rt->gc_sp = sp;
            if (!akl_bin_add(rt, lv, AKL_MK_INT(d), sp, &nv)) { free(frames); return false; }
            if (akl_is_intv(nv)) {
                i32 il = akl_get_int(nv);
                r = cmp == 0 ? il < lim : cmp == 1 ? il <= lim : cmp == 2 ? il > lim : il >= lim;
            } else {
                rt->gc_sp = sp;
                double dv = akl_to_number(rt, nv), dl = (double)lim;
                if (rt->err[0]) { free(frames); return false; }
                r = !isnan(dv) && (cmp == 0 ? dv < dl : cmp == 1 ? dv <= dl : cmp == 2 ? dv > dl : dv >= dl);
            }
        }
        rt->globals[gi].v = nv;
        /* last_val は更新しない（for-step は文ではない。元経路の expr+POP も last_val 不変） */
        if (r) pc = code + tgt;
        AKL_NEXT();
    }
    AKL_L(CJMPF_L): {
        u32 slot;
        memcpy(&slot, pc, 4); pc += 4;
        i32 imm;
        memcpy(&imm, pc, 4); pc += 4;
        u8 cmp = *pc++;
        u32 tgt;
        memcpy(&tgt, pc, 4); pc += 4;
        if (base + slot >= sp) { akl_errf(rt, "local OOB read"); free(frames); return false; }
        AklVal lv = stk[base + slot];
        bool r;
        if (akl_is_intv(lv)) {
            i32 il = akl_get_int(lv);
            r = cmp == 0 ? il < imm : cmp == 1 ? il <= imm : cmp == 2 ? il > imm : il >= imm;
        } else {
            /* 文字列×数値は数値化（汎用 LT と同一経路。"5" < 10 等を保持）。NaN なら false */
            rt->gc_sp = sp; /* to_number の flatten 発火に備える */
            double dl = akl_to_number(rt, lv), dm = (double)imm;
            r = !isnan(dl) && (cmp == 0 ? dl < dm : cmp == 1 ? dl <= dm : cmp == 2 ? dl > dm : dl >= dm);
        }
        if (!r) pc = code + tgt;
        AKL_NEXT();
    }
    AKL_L(CJMPF_G): {
        u32 gi; /* 事前解決スロット（verifier が n_globals 未満を保証） */
        memcpy(&gi, pc, 4); pc += 4;
        i32 imm;
        memcpy(&imm, pc, 4); pc += 4;
        u8 cmp = *pc++;
        u32 tgt;
        memcpy(&tgt, pc, 4); pc += 4;
        if (gi >= rt->n_globals) { akl_errf(rt, "internal: global slot OOB"); free(frames); return false; }
        AklVal lv = rt->globals[gi].v;
        bool r;
        if (akl_is_intv(lv)) {
            i32 il = akl_get_int(lv);
            r = cmp == 0 ? il < imm : cmp == 1 ? il <= imm : cmp == 2 ? il > imm : il >= imm;
        } else {
            rt->gc_sp = sp; /* to_number の flatten 発火に備える */
            double dl = akl_to_number(rt, lv), dm = (double)imm;
            r = !isnan(dl) && (cmp == 0 ? dl < dm : cmp == 1 ? dl <= dm : cmp == 2 ? dl > dm : dl >= dm);
        }
        if (!r) pc = code + tgt;
        AKL_NEXT();
    }
    /* ---- JS 例外（op 意味論は cg N_TRY コメントと paired） ---- */
    AKL_L(TRY_PUSH): {
        u32 cpc, fpc, cslot;
        memcpy(&cpc, pc, 4); memcpy(&fpc, pc + 4, 4); memcpy(&cslot, pc + 8, 4);
        pc += 12;
        AKL_VMST_OUT();
        bool ok_ = akl_vm_try_push(rt, &vmst_, cpc, fpc, cslot);
        AKL_VMST_IN();
        if (!ok_) { free(frames); return false; }
        AKL_NEXT();
    }
    AKL_L(TRY_LEAVE): {
        u32 resume; memcpy(&resume, pc, 4); pc += 4;
        AKL_VMST_OUT();
        bool ok_ = akl_vm_try_leave(rt, &vmst_, resume);
        AKL_VMST_IN();
        if (!ok_) { free(frames); return false; }
        AKL_NEXT();
    }
    AKL_L(FIN_END): {
        AKL_VMST_OUT();
        bool ok_ = akl_vm_fin_end(rt, &vmst_);
        AKL_VMST_IN();
        if (!ok_) { free(frames); return false; }
        AKL_NEXT();
    }
    AKL_L(THROW): {
        if (sp <= base) { akl_errf(rt, "stack underflow: throw"); free(frames); return false; }
        AklVal tv_ = AKL_POP();
        AKL_VMST_OUT();
        bool ok_ = akl_vm_unwind(rt, &vmst_, tv_);
        AKL_VMST_IN();
        if (!ok_) { free(frames); return false; }
        AKL_NEXT();
    }
    AKL_L(LOOPINC_L): {
        u32 slot; memcpy(&slot, pc, 4); pc += 4;
        i32 d; memcpy(&d, pc, 4); pc += 4;
        i32 lim; memcpy(&lim, pc, 4); pc += 4;
        u8 cmp = *pc++;
        u32 tgt; memcpy(&tgt, pc, 4); pc += 4;
        if (base + slot >= sp) { akl_errf(rt, "local OOB write"); free(frames); return false; }
        AklVal lv = stk[base + slot], nv;
        bool r;
        if (akl_is_intv(lv)) {
            i64 s2 = (i64)akl_get_int(lv) + (i64)d;
            if (s2 >= -2147483648ll && s2 <= 2147483647ll) {
                i32 il = (i32)s2;
                nv = AKL_MK_INT(il);
                r = cmp == 0 ? il < lim : cmp == 1 ? il <= lim : cmp == 2 ? il > lim : il >= lim;
            } else {
                nv = akl_num((double)s2);
                double ds = (double)s2, dl = (double)lim;
                r = cmp == 0 ? ds < dl : cmp == 1 ? ds <= dl : cmp == 2 ? ds > dl : ds >= dl;
            }
        } else {
            rt->gc_sp = sp;
            if (!akl_bin_add(rt, lv, AKL_MK_INT(d), sp, &nv)) { free(frames); return false; }
            if (akl_is_intv(nv)) {
                i32 il = akl_get_int(nv);
                r = cmp == 0 ? il < lim : cmp == 1 ? il <= lim : cmp == 2 ? il > lim : il >= lim;
            } else {
                rt->gc_sp = sp;
                double dv = akl_to_number(rt, nv), dl = (double)lim;
                if (rt->err[0]) { free(frames); return false; }
                r = !isnan(dv) && (cmp == 0 ? dv < dl : cmp == 1 ? dv <= dl : cmp == 2 ? dv > dl : dv >= dl);
            }
        }
        stk[base + slot] = nv;
        if (r) pc = code + tgt;
        AKL_NEXT();
    }
    AKL_L(LOOPINC_GV): {
        u32 gi; memcpy(&gi, pc, 4); pc += 4;
        i32 d; memcpy(&d, pc, 4); pc += 4;
        i32 lim; memcpy(&lim, pc, 4); pc += 4;
        u8 cmp = *pc++;
        u32 tgt; memcpy(&tgt, pc, 4); pc += 4;
        if (gi >= rt->n_globals) { akl_errf(rt, "internal: global slot OOB"); free(frames); return false; }
        AklVal lv = rt->globals[gi].v, nv;
        bool r;
        if (akl_is_intv(lv)) {
            i64 s2 = (i64)akl_get_int(lv) + (i64)d;
            if (s2 >= -2147483648ll && s2 <= 2147483647ll) {
                i32 il = (i32)s2;
                nv = AKL_MK_INT(il);
                r = cmp == 0 ? il < lim : cmp == 1 ? il <= lim : cmp == 2 ? il > lim : il >= lim;
            } else {
                nv = akl_num((double)s2);
                double ds = (double)s2, dl = (double)lim;
                r = cmp == 0 ? ds < dl : cmp == 1 ? ds <= dl : cmp == 2 ? ds > dl : ds >= dl;
            }
        } else {
            /* 汎用: GINC + CJMPF_G と同じ手続き（文字列なら bin_add で連結→数値化比較） */
            rt->gc_sp = sp;
            if (!akl_bin_add(rt, lv, AKL_MK_INT(d), sp, &nv)) { free(frames); return false; }
            if (akl_is_intv(nv)) {
                i32 il = akl_get_int(nv);
                r = cmp == 0 ? il < lim : cmp == 1 ? il <= lim : cmp == 2 ? il > lim : il >= lim;
            } else {
                rt->gc_sp = sp;
                double dv = akl_to_number(rt, nv), dl = (double)lim;
                if (rt->err[0]) { free(frames); return false; }
                r = !isnan(dv) && (cmp == 0 ? dv < dl : cmp == 1 ? dv <= dl : cmp == 2 ? dv > dl : dv >= dl);
            }
        }
        rt->globals[gi].v = nv; rt->last_val = nv; /* LINC/GINC 経路等価（文末更新の保持） */
        if (r) pc = code + tgt;
        AKL_NEXT();
    }

    AKL_L(LOOPINC_LV): {
        u32 slot; memcpy(&slot, pc, 4); pc += 4;
        i32 d; memcpy(&d, pc, 4); pc += 4;
        i32 lim; memcpy(&lim, pc, 4); pc += 4;
        u8 cmp = *pc++;
        u32 tgt; memcpy(&tgt, pc, 4); pc += 4;
        if (base + slot >= sp) { akl_errf(rt, "local OOB write"); free(frames); return false; }
        AklVal lv = stk[base + slot], nv;
        bool r;
        if (akl_is_intv(lv)) {
            i64 s2 = (i64)akl_get_int(lv) + (i64)d;
            if (s2 >= -2147483648ll && s2 <= 2147483647ll) {
                i32 il = (i32)s2;
                nv = AKL_MK_INT(il);
                r = cmp == 0 ? il < lim : cmp == 1 ? il <= lim : cmp == 2 ? il > lim : il >= lim;
            } else {
                nv = akl_num((double)s2);
                double ds = (double)s2, dl = (double)lim;
                r = cmp == 0 ? ds < dl : cmp == 1 ? ds <= dl : cmp == 2 ? ds > dl : ds >= dl;
            }
        } else {
            rt->gc_sp = sp;
            if (!akl_bin_add(rt, lv, AKL_MK_INT(d), sp, &nv)) { free(frames); return false; }
            if (akl_is_intv(nv)) {
                i32 il = akl_get_int(nv);
                r = cmp == 0 ? il < lim : cmp == 1 ? il <= lim : cmp == 2 ? il > lim : il >= lim;
            } else {
                rt->gc_sp = sp;
                double dv = akl_to_number(rt, nv), dl = (double)lim;
                if (rt->err[0]) { free(frames); return false; }
                r = !isnan(dv) && (cmp == 0 ? dv < dl : cmp == 1 ? dv <= dl : cmp == 2 ? dv > dl : dv >= dl);
            }
        }
        stk[base + slot] = nv; rt->last_val = nv;
        if (r) pc = code + tgt;
        AKL_NEXT();
    }

    /* ---- v0.3: this / クロージャ環境 / 配列・ブラケット / ビット演算 ---- */
    AKL_L(THIS): {
        AKL_PUSH(stk[base]); /* frame hidden slot 0（main は undefined） */
        AKL_NEXT();
    }
    AKL_L(ELOAD): {
        u32 idx;
        memcpy(&idx, pc, 4); pc += 4;
        if (sp <= base + 1) { akl_errf(rt, "stack underflow: eload"); free(frames); return false; }
        AklVal ev = stk[base + 1]; /* frame hidden slot 1 = 自前 ENV */
        if (!akl_is_objv(ev) || rt->objs[akl_get_obj(ev)].kind != AKL_OK_ENV ||
            idx >= rt->objs[akl_get_obj(ev)].u.env.n) {
            akl_errf(rt, "internal: env missing"); free(frames); return false;
        }
        AKL_PUSH(rt->objs[akl_get_obj(ev)].u.env.vals[idx]);
        AKL_NEXT();
    }
    AKL_L(ESTORE): {
        u32 idx;
        memcpy(&idx, pc, 4); pc += 4;
        if (sp <= base + 1) { akl_errf(rt, "stack underflow: estore"); free(frames); return false; }
        AklVal ev = stk[base + 1];
        if (!akl_is_objv(ev) || rt->objs[akl_get_obj(ev)].kind != AKL_OK_ENV ||
            idx >= rt->objs[akl_get_obj(ev)].u.env.n) {
            akl_errf(rt, "internal: env missing"); free(frames); return false;
        }
        rt->objs[akl_get_obj(ev)].u.env.vals[idx] = AKL_POP();
        AKL_NEXT();
    }
    AKL_L(SUPERGET): {
        /* 親クラス = 現在関数の cap env の vals[0]（MAKEFS がバインド）。
         * その name プロパティを push（無ければ undefined）。 */
        u32 name;
        memcpy(&name, pc, 4); pc += 4;
        if (cur >= rt->n_funcs) { akl_errf(rt, "internal: bad func"); free(frames); return false; }
        u8 cap_slot = (u8)(1 + (rt->funcs[cur].n_env ? 1u : 0u));
        if (sp <= base + cap_slot) { akl_errf(rt, "stack underflow: superget"); free(frames); return false; }
        AklVal ev = stk[base + cap_slot];
        if (!akl_is_objv(ev)) { akl_errf(rt, "internal: super env missing"); free(frames); return false; }
        AklObj *eo = &rt->objs[akl_get_obj(ev)];
        if (eo->kind != AKL_OK_ENV || eo->u.env.n < 1) {
            akl_errf(rt, "internal: super env broken");
            free(frames);
            return false;
        }
        AklVal parent = eo->u.env.vals[0];
        AklVal out = AKL_VAL_UNDEF;
        if (akl_is_objv(parent)) {
            AklObj *po = &rt->objs[akl_get_obj(parent)];
            if (po->kind == AKL_OK_OBJ) {
                i32 pi = obj_prop_find(po, name);
                if (pi >= 0) out = po->u.po.props[pi].v;
            }
        }
        AKL_PUSH(out);
        AKL_NEXT();
    }
    AKL_L(CELOAD): {
        u8 cap_slot = *pc++;
        u8 depth = *pc++;
        u32 idx;
        memcpy(&idx, pc, 4); pc += 4;
        if (sp <= base + cap_slot) { akl_errf(rt, "stack underflow: ceload"); free(frames); return false; }
        AklVal ev = stk[base + cap_slot];
        if (!akl_is_objv(ev)) { akl_errf(rt, "internal: cap env missing"); free(frames); return false; }
        u32 oi = akl_get_obj(ev);
        for (u8 d = 0; d < depth; d++) {
            AklObj *eo = &rt->objs[oi];
            if (eo->kind != AKL_OK_ENV || eo->u.env.parent == UINT32_MAX) {
                akl_errf(rt, "internal: cap env chain broken"); free(frames); return false;
            }
            oi = eo->u.env.parent;
        }
        if (rt->objs[oi].kind != AKL_OK_ENV || idx >= rt->objs[oi].u.env.n) {
            akl_errf(rt, "internal: cap env missing"); free(frames); return false;
        }
        AKL_PUSH(rt->objs[oi].u.env.vals[idx]);
        AKL_NEXT();
    }
    AKL_L(CESTORE): {
        u8 cap_slot = *pc++;
        u8 depth = *pc++;
        u32 idx;
        memcpy(&idx, pc, 4); pc += 4;
        if (sp <= base + cap_slot) { akl_errf(rt, "stack underflow: cestore"); free(frames); return false; }
        AklVal ev = stk[base + cap_slot];
        if (!akl_is_objv(ev)) { akl_errf(rt, "internal: cap env missing"); free(frames); return false; }
        u32 oi = akl_get_obj(ev);
        for (u8 d = 0; d < depth; d++) {
            AklObj *eo = &rt->objs[oi];
            if (eo->kind != AKL_OK_ENV || eo->u.env.parent == UINT32_MAX) {
                akl_errf(rt, "internal: cap env chain broken"); free(frames); return false;
            }
            oi = eo->u.env.parent;
        }
        if (rt->objs[oi].kind != AKL_OK_ENV || idx >= rt->objs[oi].u.env.n) {
            akl_errf(rt, "internal: cap env missing"); free(frames); return false;
        }
        rt->objs[oi].u.env.vals[idx] = AKL_POP();
        AKL_NEXT();
    }
    AKL_L(ANEW): {
        u32 cnt;
        memcpy(&cnt, pc, 4); pc += 4;
        if (cnt > sp) { akl_errf(rt, "stack underflow: anew"); free(frames); return false; }
        rt->gc_sp = sp; /* 要素はスタック上（ルート済み） */
        u32 oi = akl_obj_new(rt);
        if (oi == UINT32_MAX) { free(frames); return false; }
        AklObj *o = &rt->objs[oi];
        o->kind = AKL_OK_ARR;
        if (cnt) {
            o->u.arr.v = (AklVal *)malloc((u64)cnt * sizeof(AklVal));
            if (!o->u.arr.v) {
                rt->objs[oi].kind = 0;
                akl_errf(rt, "oom: array"); free(frames); return false;
            }
            for (u32 i = 0; i < cnt; i++) o->u.arr.v[i] = stk[sp - cnt + i];
            o->u.arr.n = o->u.arr.cap = cnt;
            rt->heap_bytes += (u64)cnt * sizeof(AklVal);
        }
        sp -= cnt;
        AKL_PUSH(AKL_MK_OBJ(oi));
        AKL_NEXT();
    }
    AKL_L(AGET): {
        AklVal iv = AKL_POP();
        AklVal av = AKL_POP();
        if (akl_is_objv(av)) {
            AklObj *ao = &rt->objs[akl_get_obj(av)];
            if (ao->kind == AKL_OK_ARR) {
                rt->gc_sp = sp + 2; /* pop 済み av/iv は stk 残存値としてルート */
                i32 i = akl_to_int32(rt, iv);
                if (rt->err[0]) { free(frames); return false; }
                if (i >= 0 && (u32)i < ao->u.arr.n) { AKL_PUSH(ao->u.arr.v[(u32)i]); AKL_NEXT(); }
                AKL_PUSH(AKL_VAL_UNDEF);
                AKL_NEXT();
            }
            if (ao->kind == AKL_OK_OBJ) { /* o["k"]: 実行時 intern + prop 検索 */
                rt->gc_sp = sp + 2;
                u32 si = akl_to_string(rt, iv);
                if (si == UINT32_MAX) { free(frames); return false; }
                u32 nur0 = rt->n_nury;
                if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = si;
                u32 el;
                const u8 *ep = akl_str(rt, si, &el);
                if (rt->err[0]) { rt->n_nury = nur0; free(frames); return false; }
                u32 ni2 = akl_intern(rt, ep, el, NULL);
                if (ni2 == UINT32_MAX) { rt->n_nury = nur0; free(frames); return false; }
                if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = ni2;
                i32 pi = obj_prop_find(ao, ni2);
                rt->n_nury = nur0;
                AKL_PUSH(pi >= 0 ? ao->u.po.props[pi].v : AKL_VAL_UNDEF);
                AKL_NEXT();
            }
            if (ao->kind == AKL_OK_STR || ao->kind == AKL_OK_ROPE) { /* s[i]: コードポイント単位 */
                rt->gc_sp = sp + 2;
                u32 ln;
                const u8 *bp = akl_str(rt, akl_get_obj(av), &ln);
                if (rt->err[0]) { free(frames); return false; }
                i32 i = akl_to_int32(rt, iv);
                if (rt->err[0]) { free(frames); return false; }
                if (i < 0) { AKL_PUSH(AKL_VAL_UNDEF); AKL_NEXT(); }
                u32 cl = akl_str_cp_len_at(bp, ln, (u32)i);
                if (!cl) { AKL_PUSH(AKL_VAL_UNDEF); AKL_NEXT(); }
                u32 pos = 0;
                for (u32 k = 0; k < (u32)i; k++) {
                    u8 c = bp[pos];
                    pos += c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
                           : (c & 0xF8) == 0xF0 ? 4 : 1;
                }
                rt->gc_sp = sp;
                u32 si2 = akl_mkstr(rt, bp + pos, cl);
                if (si2 == UINT32_MAX) { free(frames); return false; }
                AKL_PUSH(AKL_MK_OBJ(si2));
                AKL_NEXT();
            }
        }
        AKL_PUSH(AKL_VAL_UNDEF); /* 非対応対象（数値等への index は undefined） */
        AKL_NEXT();
    }
    AKL_L(ASET): {
        AklVal vv = AKL_POP();
        AklVal iv = AKL_POP();
        AklVal av = AKL_POP();
        if (akl_is_objv(av) && rt->objs[akl_get_obj(av)].kind == AKL_OK_ARR) {
            AklObj *ao = &rt->objs[akl_get_obj(av)];
            rt->gc_sp = sp + 3;
            i32 i = akl_to_int32(rt, iv);
            if (rt->err[0]) { free(frames); return false; }
            if (i >= 0) {
                u32 idx = (u32)i;
                if (idx >= ao->u.arr.n) {
                    u64 need = (u64)(idx + 1) * sizeof(AklVal);
                    if (need > (u64)rt->heap_mb << 20) {
                        akl_errf(rt, "heap bytes budget exhausted"); free(frames); return false;
                    }
                    if (idx >= ao->u.arr.cap) {
                        u32 ncap = ao->u.arr.cap ? ao->u.arr.cap * 2 : 4;
                        while (ncap <= idx) ncap *= 2;
                        u32 lim = (u32)(((u64)rt->heap_mb << 20) / sizeof(AklVal));
                        if (ncap > lim) ncap = lim;
                        if (ncap <= idx) {
                            akl_errf(rt, "heap bytes budget exhausted"); free(frames); return false;
                        }
                        AklVal *nv2 = (AklVal *)realloc(ao->u.arr.v, (u64)ncap * sizeof(AklVal));
                        if (!nv2) { akl_errf(rt, "oom: array grow"); free(frames); return false; }
                        rt->heap_bytes += (u64)(ncap - ao->u.arr.cap) * sizeof(AklVal);
                        ao->u.arr.v = nv2;
                        ao->u.arr.cap = ncap;
                    }
                    for (u32 k = ao->u.arr.n; k < idx; k++) ao->u.arr.v[k] = AKL_VAL_UNDEF;
                    ao->u.arr.n = idx + 1;
                }
                ao->u.arr.v[idx] = vv;
            }
            /* 負 index は JS の名前付きプロパティ相当 = 非対応（値は代入式の値として返す） */
            AKL_PUSH(vv);
            AKL_NEXT();
        }
        if (akl_is_objv(av) && rt->objs[akl_get_obj(av)].kind == AKL_OK_OBJ) { /* o["k"]=v */
            AklObj *ao = &rt->objs[akl_get_obj(av)];
            rt->gc_sp = sp + 3;
            u32 si = akl_to_string(rt, iv);
            if (si == UINT32_MAX) { free(frames); return false; }
            u32 nur0 = rt->n_nury;
            if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = si;
            u32 el;
            const u8 *ep = akl_str(rt, si, &el);
            if (rt->err[0]) { rt->n_nury = nur0; free(frames); return false; }
            u32 ni2 = akl_intern(rt, ep, el, NULL);
            if (ni2 == UINT32_MAX) { rt->n_nury = nur0; free(frames); return false; }
            if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = ni2;
            bool ok2 = obj_prop_set(rt, ao, ni2, vv);
            rt->n_nury = nur0;
            if (!ok2) { free(frames); return false; }
            AKL_PUSH(vv);
            AKL_NEXT();
        }
        AKL_PUSH(vv); /* 非対応対象（文字列への代入等）は無視。代入式の値は右辺 */
        AKL_NEXT();
    }
    AKL_L(BNOT): {
        AklVal v = AKL_POP();
        rt->gc_sp = sp;
        i32 i = akl_to_int32(rt, v);
        if (rt->err[0]) { free(frames); return false; }
        AKL_PUSH(AKL_MK_INT(~i));
        AKL_NEXT();
    }
#define AKL_BIT2(EXPR) { \
        AklVal vb = AKL_POP(), va = AKL_POP(); \
        rt->gc_sp = sp; \
        i32 a = akl_to_int32(rt, va), b = akl_to_int32(rt, vb); \
        if (rt->err[0]) { free(frames); return false; } \
        AKL_PUSH(AKL_MK_INT((i32)(EXPR))); \
        AKL_NEXT(); }
    AKL_L(BAND):  { AKL_BIT2(a & b); }
    AKL_L(BOR):   { AKL_BIT2(a | b); }
    AKL_L(BXOR):  { AKL_BIT2(a ^ b); }
    AKL_L(BSHL):  { AKL_BIT2(a << (b & 31)); }
    AKL_L(BSHR):  { AKL_BIT2(a >> (b & 31)); }
    AKL_L(BUSHR): { AKL_BIT2((u32)a >> (b & 31)); }
#undef AKL_BIT2

    AKL_L(POW): {
        AklVal vb = AKL_POP(), va = AKL_POP();
        rt->gc_sp = sp;
        double da, db;
        akl_to_number2(rt, va, vb, &da, &db);
        if (rt->err[0]) { free(frames); return false; }
        AKL_PUSH(akl_num(akl_canon(pow(da, db))));
        AKL_NEXT();
    }
    AKL_L(IN): {
        AklVal obj = AKL_POP();
        AklVal key = AKL_POP();
        if (akl_is_objv(obj)) {
            AklObj *o = &rt->objs[akl_get_obj(obj)];
            if (o->kind == AKL_OK_OBJ) {
                rt->gc_sp = sp + 2;
                u32 si = akl_to_string(rt, key);
                if (si == UINT32_MAX) { free(frames); return false; }
                if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = si;
                u32 el;
                const u8 *ep = akl_str(rt, si, &el);
                if (rt->err[0]) { free(frames); return false; }
                u32 ni2 = akl_intern(rt, ep, el, NULL);
                if (ni2 == UINT32_MAX) { free(frames); return false; }
                AKL_PUSH(obj_prop_find(o, ni2) >= 0 ? AKL_VAL_TRUE : AKL_VAL_FALSE);
                AKL_NEXT();
            }
            if (o->kind == AKL_OK_ARR) {
                rt->gc_sp = sp + 2;
                i32 i = akl_to_int32(rt, key);
                if (rt->err[0]) { free(frames); return false; }
                AKL_PUSH(i >= 0 && (u32)i < o->u.arr.n ? AKL_VAL_TRUE : AKL_VAL_FALSE);
                AKL_NEXT();
            }
            if (o->kind == AKL_OK_STR || o->kind == AKL_OK_ROPE) { /* "x" in "string": index は数値のみ */
                rt->gc_sp = sp + 2;
                i32 i = akl_to_int32(rt, key);
                if (rt->err[0]) { free(frames); return false; }
                u32 ln;
                const u8 *bp = akl_str(rt, akl_get_obj(obj), &ln);
                if (rt->err[0]) { free(frames); return false; }
                u32 nc = akl_str_cp_count(bp, ln);
                AKL_PUSH(i >= 0 && (u32)i < nc ? AKL_VAL_TRUE : AKL_VAL_FALSE);
                AKL_NEXT();
            }
        }
        AKL_PUSH(AKL_VAL_FALSE);
        AKL_NEXT();
    }
    AKL_L(PDEL): {
        u32 name;
        memcpy(&name, pc, 4); pc += 4;
        AklVal ov = AKL_POP();
        if (akl_is_objv(ov) && rt->objs[akl_get_obj(ov)].kind == AKL_OK_OBJ) {
            AklObj *o = &rt->objs[akl_get_obj(ov)];
            i32 pi = obj_prop_find(o, name);
            if (pi >= 0) {
                for (u32 k = (u32)pi; k + 1 < o->u.po.n; k++) o->u.po.props[k] = o->u.po.props[k + 1];
                o->u.po.n--;
                AKL_PUSH(AKL_VAL_TRUE);
                AKL_NEXT();
            }
        }
        AKL_PUSH(AKL_VAL_TRUE); /* delete は存在しなくても true（非 strict） */
        AKL_NEXT();
    }
    AKL_L(IDEL): {
        AklVal iv = AKL_POP();
        AklVal av = AKL_POP();
        if (akl_is_objv(av) && rt->objs[akl_get_obj(av)].kind == AKL_OK_ARR) {
            AklObj *o = &rt->objs[akl_get_obj(av)];
            rt->gc_sp = sp + 2;
            i32 i = akl_to_int32(rt, iv);
            if (rt->err[0]) { free(frames); return false; }
            if (i >= 0 && (u32)i < o->u.arr.n) o->u.arr.v[(u32)i] = AKL_VAL_UNDEF; /* 穴化 */
        }
        AKL_PUSH(AKL_VAL_TRUE);
        AKL_NEXT();
    }
    AKL_L(INSTANCEOF): {
        AklVal obj = AKL_POP();
        AklVal f = AKL_POP();
        /* akl は class 実装時まで「FUNC で生成された this」を追跡しない。
         * 暫定: 両方オブジェクトなら typeof ベースの緩い判定（後で class と共に本格化） */
        bool r = false;
        if (akl_is_objv(obj) && akl_is_objv(f)) {
            u8 ok = rt->objs[akl_get_obj(obj)].kind;
            u8 fk = rt->objs[akl_get_obj(f)].kind;
            if (ok == AKL_OK_OBJ && (fk == AKL_OK_FUNC || fk == AKL_OK_NATIVE)) r = true;
        }
        AKL_PUSH(r ? AKL_VAL_TRUE : AKL_VAL_FALSE);
        AKL_NEXT();
    }

    AKL_L(KEYSOF): {
        AklVal ov = AKL_POP();
        u32 cnt = 0;
        if (akl_is_objv(ov)) {
            AklObj *o = &rt->objs[akl_get_obj(ov)];
            if (o->kind == AKL_OK_OBJ) cnt = o->u.po.n;
            else if (o->kind == AKL_OK_ARR) cnt = o->u.arr.n;
            else if (o->kind == AKL_OK_STR || o->kind == AKL_OK_ROPE) {
                u32 ln;
                const u8 *bp = akl_str(rt, akl_get_obj(ov), &ln);
                if (rt->err[0]) { free(frames); return false; }
                cnt = akl_str_cp_count(bp, ln);
            }
        }
        rt->gc_sp = sp;
        u32 oi = akl_obj_new(rt);
        if (oi == UINT32_MAX) { free(frames); return false; }
        rt->objs[oi].kind = AKL_OK_ARR;
        if (cnt) {
            rt->objs[oi].u.arr.v = (AklVal *)malloc((u64)cnt * sizeof(AklVal));
            if (!rt->objs[oi].u.arr.v) { rt->objs[oi].kind = 0; akl_errf(rt, "oom: keys"); free(frames); return false; }
            rt->objs[oi].u.arr.cap = cnt;
            rt->heap_bytes += (u64)cnt * sizeof(AklVal);
        }
        if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = oi;
        if (akl_is_objv(ov)) {
            AklObj *o = &rt->objs[akl_get_obj(ov)];
            if (o->kind == AKL_OK_OBJ) {
                for (u32 i = 0; i < cnt; i++) {
                    u32 kl;
                    const u8 *kp = akl_str(rt, o->u.po.props[i].name, &kl);
                    if (rt->err[0]) { free(frames); return false; }
                    u32 ks = akl_mkstr(rt, kp, kl);
                    if (ks == UINT32_MAX) { free(frames); return false; }
                    rt->objs[oi].u.arr.v[i] = AKL_MK_OBJ(ks);
                    rt->objs[oi].u.arr.n = i + 1;
                }
            } else if (o->kind == AKL_OK_ARR) {
                for (u32 i = 0; i < cnt; i++) {
                    rt->objs[oi].u.arr.v[i] = akl_mknum((double)i);
                    rt->objs[oi].u.arr.n = i + 1;
                }
            } else if (o->kind == AKL_OK_STR || o->kind == AKL_OK_ROPE) {
                for (u32 i = 0; i < cnt; i++) {
                    rt->objs[oi].u.arr.v[i] = akl_mknum((double)i);
                    rt->objs[oi].u.arr.n = i + 1;
                }
            }
        }
        rt->objs[oi].u.arr.n = cnt;
        AKL_PUSH(AKL_MK_OBJ(oi));
        AKL_NEXT();
    }
    AKL_L(TOARR): {
        AklVal v = AKL_POP();
        if (akl_is_objv(v)) {
            u32 oi2 = akl_get_obj(v);
            if (oi2 < rt->n_objs && rt->objs[oi2].kind == AKL_OK_ARR) { AKL_PUSH(v); AKL_NEXT(); }
            if (oi2 < rt->n_objs && (rt->objs[oi2].kind == AKL_OK_STR || rt->objs[oi2].kind == AKL_OK_ROPE)) {
                /* 文字列: コードポイントの配列に展開 */
                u32 ln;
                const u8 *bp = akl_str(rt, oi2, &ln);
                if (rt->err[0]) { free(frames); return false; }
                u32 cnt = akl_str_cp_count(bp, ln);
                rt->gc_sp = sp;
                u32 oi = akl_obj_new(rt);
                if (oi == UINT32_MAX) { free(frames); return false; }
                rt->objs[oi].kind = AKL_OK_ARR;
                if (cnt) {
                    rt->objs[oi].u.arr.v = (AklVal *)malloc((u64)cnt * sizeof(AklVal));
                    if (!rt->objs[oi].u.arr.v) { rt->objs[oi].kind = 0; akl_errf(rt, "oom: toarr"); free(frames); return false; }
                    rt->objs[oi].u.arr.cap = cnt;
                    rt->heap_bytes += (u64)cnt * sizeof(AklVal);
                }
                if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = oi;
                for (u32 i = 0; i < cnt; i++) {
                    u32 cl = akl_str_cp_len_at(bp, ln, i);
                    u32 pos = akl_str_cp_to_byte(bp, ln, i);
                    u32 st = akl_mkstr(rt, bp + pos, cl);
                    if (st == UINT32_MAX) { free(frames); return false; }
                    rt->objs[oi].u.arr.v[i] = AKL_MK_OBJ(st);
                    rt->objs[oi].u.arr.n = i + 1;
                }
                AKL_PUSH(AKL_MK_OBJ(oi));
                AKL_NEXT();
            }
        }
        /* それ以外: [v] */
        rt->gc_sp = sp;
        u32 oi = akl_obj_new(rt);
        if (oi == UINT32_MAX) { free(frames); return false; }
        rt->objs[oi].kind = AKL_OK_ARR;
        rt->objs[oi].u.arr.v = (AklVal *)malloc(sizeof(AklVal));
        if (!rt->objs[oi].u.arr.v) { rt->objs[oi].kind = 0; akl_errf(rt, "oom: toarr"); free(frames); return false; }
        rt->objs[oi].u.arr.v[0] = v;
        rt->objs[oi].u.arr.n = rt->objs[oi].u.arr.cap = 1;
        rt->heap_bytes += sizeof(AklVal);
        AKL_PUSH(AKL_MK_OBJ(oi));
        AKL_NEXT();
    }

    AKL_L(ARRPUSH): {
        AklVal vv = AKL_POP();
        AklVal av = AKL_POP();
        if (akl_is_objv(av) && rt->objs[akl_get_obj(av)].kind == AKL_OK_ARR) {
            AklObj *o = &rt->objs[akl_get_obj(av)];
            u32 need = o->u.arr.n + 1;
            if (need > o->u.arr.cap) {
                u32 nc = o->u.arr.cap ? o->u.arr.cap * 2 : 4;
                AklVal *nv = (AklVal *)realloc(o->u.arr.v, (u64)nc * sizeof(AklVal));
                if (!nv) { akl_errf(rt, "oom: push"); free(frames); return false; }
                rt->heap_bytes += (u64)(nc - o->u.arr.cap) * sizeof(AklVal);
                o->u.arr.v = nv;
                o->u.arr.cap = nc;
            }
            o->u.arr.v[o->u.arr.n++] = vv;
        }
        AKL_PUSH(av);
        AKL_NEXT();
    }
    AKL_L(ARRPUSHALL): {
        AklVal sv = AKL_POP();
        AklVal av = AKL_POP();
        if (akl_is_objv(av) && rt->objs[akl_get_obj(av)].kind == AKL_OK_ARR &&
            akl_is_objv(sv) && rt->objs[akl_get_obj(sv)].kind == AKL_OK_ARR) {
            AklObj *o = &rt->objs[akl_get_obj(av)];
            AklObj *s = &rt->objs[akl_get_obj(sv)];
            u32 need = o->u.arr.n + s->u.arr.n;
            if (need > o->u.arr.cap) {
                u32 nc = o->u.arr.cap ? o->u.arr.cap * 2 : 4;
                while (nc < need) nc *= 2;
                AklVal *nv = (AklVal *)realloc(o->u.arr.v, (u64)nc * sizeof(AklVal));
                if (!nv) { akl_errf(rt, "oom: pushall"); free(frames); return false; }
                rt->heap_bytes += (u64)(nc - o->u.arr.cap) * sizeof(AklVal);
                o->u.arr.v = nv;
                o->u.arr.cap = nc;
            }
            for (u32 i = 0; i < s->u.arr.n; i++) o->u.arr.v[o->u.arr.n++] = s->u.arr.v[i];
        }
        AKL_PUSH(av);
        AKL_NEXT();
    }
    AKL_L(ARRSPREADC): {
        u32 slot;
        memcpy(&slot, pc, 4); pc += 4;
        AklVal av = AKL_POP();
        if (akl_is_objv(av) && rt->objs[akl_get_obj(av)].kind == AKL_OK_ARR) {
            AklObj *o = &rt->objs[akl_get_obj(av)];
            u32 n2 = o->u.arr.n;
            AKL_GROW_TO(sp + n2);
            for (u32 i = 0; i < n2; i++)
                stk[sp + i] = o->u.arr.v[i]; /* 順序維持（直接書込） */
            sp += n2;
            if (base + slot < sp) {
                AklVal cv = stk[base + slot];
                i32 cc = akl_is_intv(cv) ? akl_get_int(cv) : 0;
                stk[base + slot] = AKL_MK_INT(cc + (i32)n2);
            }
        }
        AKL_NEXT();
    }
    AKL_L(CALLN): {
        /* 動的引数呼び出し（spread 対応）。argc は locals[slot]（引数カウンタ） */
        u32 slot;
        memcpy(&slot, pc, 4); pc += 4;
        if (base + slot >= sp) { akl_errf(rt, "internal: calln counter"); free(frames); return false; }
        AklVal cv = stk[base + slot];
        u32 argc = akl_is_intv(cv) && akl_get_int(cv) >= 0 ? (u32)akl_get_int(cv) : 0;
        if (argc > 4096 || sp < base + argc + 1) { akl_errf(rt, "too many arguments"); free(frames); return false; }
        AklVal fv = stk[sp - argc - 1];
        if (!akl_is_objv(fv)) { akl_errf(rt, "TypeError: not a function"); free(frames); return false; }
        AklObj *fo = &rt->objs[akl_get_obj(fv)];
        if (fo->kind == AKL_OK_NATIVE) {
            if (budget < AKL_NATIVE_COST) { akl_errf(rt, "instruction budget exhausted"); free(frames); return false; }
            budget -= AKL_NATIVE_COST;
            rt->gc_sp = sp;
            u32 nur0 = rt->n_nury;
            AklVal r;
            if (!akl_vm_native_call(rt, fo, AKL_VAL_UNDEF, (u8)argc, stk + sp - argc, &r)) { free(frames); return false; }
            rt->n_nury = nur0;
            sp -= argc;
            stk[sp - 1] = r;
            AKL_NEXT();
        }
        if (fo->kind != AKL_OK_FUNC) { akl_errf(rt, "TypeError: not a function"); free(frames); return false; }
        u32 fidx = akl_get_obj(fv);
        u32 fe_i = rt->objs[fidx].code_off;
        if (fe_i >= rt->n_funcs) { akl_errf(rt, "internal: bad func ref"); free(frames); return false; }
        AklFuncEnt *fe = &rt->funcs[fe_i];
        if (nframes >= AKL_MAX_DEPTH) { akl_errf(rt, "call depth budget exhausted"); free(frames); return false; }
        u32 nh = 1 + (fe->n_env ? 1u : 0u) + (fe->n_cap ? 1u : 0u);
        u32 win = sp - argc - 1;
        u32 nloc = fe->n_locals, npar = fe->n_params;
        u32 k = nh - 1;
        AKL_GROW_TO(win + k + argc + 8);
        if (k) {
            for (i32 i = (i32)argc - 1; i >= 0; i--) stk[win + k + i + 1] = stk[win + i + 1];
        }
        if (nh > 1) {
            rt->gc_sp = sp;
            if (!akl_vm_frame_hidden(rt, stk, win, nloc, fe, AKL_VAL_UNDEF,
                                     fo->env == UINT32_MAX ? AKL_VAL_UNDEF : AKL_MK_OBJ(fo->env))) {
                free(frames);
                return false;
            }
        } else {
            stk[win] = AKL_VAL_UNDEF;
        }
        u32 keep = argc < npar ? argc : npar;
        for (u32 i = nh + keep; i < nloc; i++) stk[win + i] = AKL_VAL_UNDEF;
        sp = win + nloc;
        frames[nframes].ret_off = (u32)(pc - code);
        frames[nframes].base = base;
        frames[nframes].func = cur;
        frames[nframes].is_new = 0;
        nframes++;
        base = win;
        cur = fe_i;
        pc = code + rt->funcs[cur].code_off;
        AKL_NEXT();
    }

    AKL_L(NEW): {
        u8 argc = *pc++;
        if (argc > 250 || sp < base + argc + 1) { akl_errf(rt, "stack underflow: new"); free(frames); return false; }
        AklVal fv = stk[sp - argc - 1];
        if (!akl_is_objv(fv)) { akl_errf(rt, "TypeError: not a constructor"); free(frames); return false; }
        AklObj *fo = &rt->objs[akl_get_obj(fv)];
        AklVal ctor = fv;
        /* class オブジェクト（AKL_OK_OBJ）: constructor プロパティを関数として使用。
         * メソッド（constructor 以外）は new 時にインスタンスへコピーする（後段） */
        AklVal cls_obj = AKL_VAL_UNDEF;
        if (fo->kind == AKL_OK_OBJ) {
            cls_obj = fv;
            i32 pi = obj_prop_find(fo, akl_intern(rt, (const u8 *)"constructor", 11, NULL));
            if (pi < 0) { akl_errf(rt, "TypeError: not a constructor"); free(frames); return false; }
            ctor = fo->u.po.props[pi].v;
            if (!akl_is_objv(ctor) || rt->objs[akl_get_obj(ctor)].kind != AKL_OK_FUNC) {
                akl_errf(rt, "TypeError: not a constructor"); free(frames); return false;
            }
            fo = &rt->objs[akl_get_obj(ctor)];
        }
        if (fo->kind == AKL_OK_NATIVE) {
            if (budget < AKL_NATIVE_COST) { akl_errf(rt, "instruction budget exhausted"); free(frames); return false; }
            budget -= AKL_NATIVE_COST;
            /* native コンストラクタ: 新 OBJ を this として呼ぶ（戻り値は無視） */
            rt->gc_sp = sp;
            u32 oi = akl_obj_new(rt);
            if (oi == UINT32_MAX) { free(frames); return false; }
            rt->objs[oi].kind = AKL_OK_OBJ;
            u32 nur0 = rt->n_nury;
            AklVal r;
            if (!akl_vm_native_call(rt, fo, AKL_MK_OBJ(oi), argc, stk + sp - argc, &r)) { free(frames); return false; }
            rt->n_nury = nur0;
            sp -= argc;
            /* native コンストラクタ: 戻りがオブジェクトならそれを返す（JS 仕様）。
             * プリミティブなら this（新 OBJ）を返す。 */
            stk[sp - 1] = akl_is_objv(r) ? r : AKL_MK_OBJ(oi);
            AKL_NEXT();
        }
        if (fo->kind != AKL_OK_FUNC) { akl_errf(rt, "TypeError: not a constructor"); free(frames); return false; }
        u32 fidx = akl_get_obj(ctor);
        u32 fe_i = rt->objs[fidx].code_off;
        if (fe_i >= rt->n_funcs) { akl_errf(rt, "internal: bad func ref"); free(frames); return false; }
        AklFuncEnt *fe = &rt->funcs[fe_i];
        if (nframes >= AKL_MAX_DEPTH) { akl_errf(rt, "call depth budget exhausted"); free(frames); return false; }
        u32 nh = 1 + (fe->n_env ? 1u : 0u) + (fe->n_cap ? 1u : 0u);
        u32 win = sp - argc - 1;
        u32 nloc = fe->n_locals, npar = fe->n_params;
        u32 k = nh - 1;
        AKL_GROW_TO(win + k + argc + 8);
        if (k) {
            for (i32 i = (i32)argc - 1; i >= 0; i--) stk[win + k + i + 1] = stk[win + i + 1];
        }
        /* this = 新 OBJ（nursery ピン: frame_hidden の env 生成 GC から保護） */
        rt->gc_sp = sp;
        u32 no = akl_obj_new(rt);
        if (no == UINT32_MAX) { free(frames); return false; }
        rt->objs[no].kind = AKL_OK_OBJ;
        AklVal thisv = AKL_MK_OBJ(no);
        u32 nur0 = rt->n_nury;
        if (rt->n_nury < AKL_NURY_CAP) rt->nury[rt->n_nury++] = no;
        if (nh > 1) {
            if (!akl_vm_frame_hidden(rt, stk, win, nloc, fe, thisv,
                                     fo->env == UINT32_MAX ? AKL_VAL_UNDEF : AKL_MK_OBJ(fo->env))) {
                rt->n_nury = nur0;
                free(frames);
                return false;
            }
        } else {
            stk[win] = thisv;
        }
        /* クラスメソッドをインスタンスへコピー（constructor 以外。static 含む — 簡易近似）。
         * extends の場合: __super チェーンを「親 → 子」の順で辿ってコピーする
         * （子クラスのメソッドが親を上書きする = JS の継承セマンティクス） */
        if (akl_is_objv(cls_obj)) {
            AklObj *co = &rt->objs[akl_get_obj(cls_obj)];
            AklObj *no2 = &rt->objs[no];
            u32 sup_name = akl_intern(rt, (const u8 *)"\x00super", 7, NULL);
            if (!akl_new_copy_chain(rt, co, no2, sup_name, 0)) {
                rt->n_nury = nur0;
                free(frames);
                return false;
            }
        }
        rt->n_nury = nur0;
        u32 keep = argc < npar ? argc : npar;
        for (u32 i = nh + keep; i < nloc; i++) stk[win + i] = AKL_VAL_UNDEF;
        sp = win + nloc;
        frames[nframes].ret_off = (u32)(pc - code);
        frames[nframes].base = base;
        frames[nframes].func = cur;
        frames[nframes].is_new = 1;
        nframes++;
        base = win;
        cur = fe_i;
        pc = code + rt->funcs[cur].code_off;
        AKL_NEXT();
    }

    AKL_L(PSETDYN): {
        /* スタック [obj, val, key]（key が TOS） */
        AklVal kv = AKL_POP(); /* key */
        AklVal vv = AKL_POP(); /* val */
        AklVal av = AKL_POP(); /* obj */
        if (!akl_is_objv(av) || rt->objs[akl_get_obj(av)].kind != AKL_OK_OBJ) {
            akl_errf(rt, "TypeError: property store on non-object value");
            free(frames);
            return false;
        }
        u32 sidx = akl_to_string(rt, kv);
        if (sidx == UINT32_MAX) { free(frames); return false; }
        u32 sn;
        const u8 *spn = akl_str(rt, sidx, &sn);
        if (rt->err[0]) { free(frames); return false; }
        u32 name = akl_intern(rt, spn, sn, NULL);
        if (name == UINT32_MAX) { akl_errf(rt, "intern failed"); free(frames); return false; }
        AklObj *ao = &rt->objs[akl_get_obj(av)];
        if (ao->u.po.n >= AKL_OBJ_MAX_PROPS) {
            akl_errf(rt, "object property limit exceeded");
            free(frames);
            return false;
        }
        if (!obj_prop_set(rt, ao, name, vv)) { free(frames); return false; }
        AKL_PUSH(vv);
        AKL_NEXT();
    }
    AKL_L(OBJSPREAD): {
        /* pop src → TOS の OBJ に全 props コピー（{...src, k: v} の後勝ちを保証）。
         * src が OBJ 以外（undefined/null/primitive/ARR/STR）は無視（簡易近似。AKL_COMPAT）。 */
        AklVal srcv = AKL_POP();
        if (sp <= base) { akl_errf(rt, "stack underflow: objspread"); free(frames); return false; }
        AklVal dstv = stk[sp - 1];
        if (!akl_is_objv(srcv) || !akl_is_objv(dstv)) { AKL_NEXT(); }
        AklObj *so = &rt->objs[akl_get_obj(srcv)];
        AklObj *do2 = &rt->objs[akl_get_obj(dstv)];
        if (so->kind != AKL_OK_OBJ || do2->kind != AKL_OK_OBJ) { AKL_NEXT(); }
        for (u32 i = 0; i < so->u.po.n; i++) {
            if (do2->u.po.n >= AKL_OBJ_MAX_PROPS) {
                akl_errf(rt, "object property limit exceeded");
                free(frames);
                return false;
            }
            if (!obj_prop_set(rt, do2, so->u.po.props[i].name, so->u.po.props[i].v)) {
                free(frames);
                return false;
            }
        }
        AKL_NEXT();
    }
    AKL_L(ARRREST): {
        /* pop start, pop arr → 新規配列 [start..n)（配列分割の rest）。 */
        AklVal sv = AKL_POP();
        AklVal av = AKL_POP();
        rt->gc_sp = sp;
        u32 oi = akl_obj_new(rt);
        if (oi == UINT32_MAX) { free(frames); return false; }
        rt->objs[oi].kind = AKL_OK_ARR;
        u32 cnt = 0;
        if (akl_is_objv(av) && rt->objs[akl_get_obj(av)].kind == AKL_OK_ARR) {
            AklObj *ao = &rt->objs[akl_get_obj(av)];
            i32 st = akl_get_int(sv);
            if (st < 0) st = 0;
            if ((u32)st < ao->u.arr.n) {
                cnt = ao->u.arr.n - (u32)st;
                if (!akl_arr_grow(rt, &rt->objs[oi], cnt)) { free(frames); return false; }
                memcpy(rt->objs[oi].u.arr.v, ao->u.arr.v + st, (u64)cnt * sizeof(AklVal));
            }
        }
        rt->objs[oi].u.arr.n = cnt;
        AKL_PUSH(AKL_MK_OBJ(oi));
        AKL_NEXT();
    }
    AKL_L(OBJREST): {
        /* pop src_obj → 新規 OBJ（全 props コピー）。オブジェクト分割の rest。
         * 除外キーは呼出側が DUP;PDEL;POP で削除する。 */
        AklVal srcv = AKL_POP();
        rt->gc_sp = sp;
        u32 oi = akl_obj_new(rt);
        if (oi == UINT32_MAX) { free(frames); return false; }
        rt->objs[oi].kind = AKL_OK_OBJ;
        if (akl_is_objv(srcv) && rt->objs[akl_get_obj(srcv)].kind == AKL_OK_OBJ) {
            AklObj *so = &rt->objs[akl_get_obj(srcv)];
            AklObj *no = &rt->objs[oi];
            for (u32 i = 0; i < so->u.po.n; i++) {
                if (no->u.po.n >= AKL_OBJ_MAX_PROPS) {
                    akl_errf(rt, "object property limit exceeded");
                    free(frames);
                    return false;
                }
                if (!obj_prop_set(rt, no, so->u.po.props[i].name, so->u.po.props[i].v)) {
                    free(frames);
                    return false;
                }
            }
        }
        AKL_PUSH(AKL_MK_OBJ(oi));
        AKL_NEXT();
    }

    AKL_L(NOP): { AKL_NEXT(); } /* CoJIT 埋め。通常到達不能、到達しても透過 */

    AKL_L(NEWREGEX): {
        u32 pat_idx;
        memcpy(&pat_idx, pc, 4); pc += 4;
        u32 rflags;
        memcpy(&rflags, pc, 4); pc += 4;
        u32 pn;
        const u8 *pp = akl_str(rt, pat_idx, &pn);
        if (!pp) { free(frames); return false; }
        if (budget < AKL_NATIVE_COST) { akl_errf(rt, "instruction budget exhausted"); free(frames); return false; }
        budget -= AKL_NATIVE_COST;
        char rerr[96];
        AklRex *rx = akl_rex_compile(pp, pn, rflags, rerr, sizeof rerr);
        if (!rx) { akl_errf(rt, "SyntaxError: %s", rerr); akl_native_throw(rt, rt->err); free(frames); return false; }
        AklVal rv = akl_regex_make(rt, rx, rflags);
        if (rt->err[0]) { free(frames); return false; }
        AKL_PUSH(rv);
        AKL_NEXT();
    }

    AKL_L(HALT): {
        if (rt->n_tries) { /* 防御層: main 正常終了で try が残るのは内部不整合 */
            akl_errf(rt, "internal: try/frame skew at halt"); free(frames); return false; }
        free(frames);
        return true;
    }

#if !AKL_THREADED
    }
#endif
#undef AKL_GROW_TO
#undef AKL_PUSH
#undef AKL_POP
#undef AKL_PEEK
#undef AKL_BUDGET
#undef AKL_BINOP_NUM
#undef AKL_NEXT
#undef AKL_L
}

/* ============================== 公開 API ============================== */

AklRT *akl_new(void) {
    AklRT *rt = (AklRT *)calloc(1, sizeof(AklRT));
    if (!rt) return NULL;
    rt->gc_next = 512u << 10;
    rt->gc_next_objs = 4096;
    rt->heap_mb = AKL_MAX_HEAP_MB;
    rt->max_objs = AKL_MAX_OBJECTS;
    rt->stk = (AklVal *)malloc((u64)AKL_STK_INIT * sizeof(AklVal));
    if (!rt->stk) { free(rt); return NULL; }
    rt->cap_stk = AKL_STK_INIT;
    if (rt->cap_stk > AKL_STK_MAX) rt->cap_stk = AKL_STK_MAX;
    rt->insn_budget_def = 10000000;
    rt->last_val = AKL_VAL_UNDEF;
    /* main 関数エントリ（entry 0。code 範囲は eval ごとの末尾まで） */
    if (akl_obj_new(rt) == UINT32_MAX) { free(rt->stk); free(rt); return NULL; }
    /* obj0 = 予約（壊れ index 検出を容易に） */
    /* JS グローバル定数（書換不可）: NaN, Infinity */
    {
        u32 n_nan = akl_mkstr(rt, (const u8 *)"NaN", 3);
        u32 n_inf = akl_mkstr(rt, (const u8 *)"Infinity", 8);
        u32 g1 = n_nan == UINT32_MAX ? UINT32_MAX : cg_global_add(rt, n_nan, 1);
        u32 g2 = n_inf == UINT32_MAX ? UINT32_MAX : cg_global_add(rt, n_inf, 1);
        if (g1 == UINT32_MAX || g2 == UINT32_MAX) {
            akl_free(rt);
            return NULL;
        }
        rt->globals[g1].v = akl_num(0.0 / 0.0);
        rt->globals[g2].v = akl_num(1.0 / 0.0);
    }
    /* v0.3 組込（Math / parseInt 等 / 文字列・配列メソッド）。失敗は rt を棄却 */
    if (!akl_builtins_install(rt)) {
        akl_free(rt);
        return NULL;
    }
    return rt;
}

void akl_free(AklRT *rt) {
    if (!rt) return;
    for (u32 i = 0; i < rt->n_objs; i++) {
        if (rt->objs[i].kind == AKL_OK_STR) free(rt->objs[i].bytes);
        if (rt->objs[i].kind == AKL_OK_OBJ) free(rt->objs[i].u.po.props);
        if (rt->objs[i].kind == AKL_OK_ARR) free(rt->objs[i].u.arr.v);
        if (rt->objs[i].kind == AKL_OK_ENV) free(rt->objs[i].u.env.vals);
        if (rt->objs[i].kind == AKL_OK_REGEX) akl_rex_free(rt->objs[i].u.rex.rx);
    }
    free(rt->objs);
    free(rt->free_objs);
    free(rt->ghash);
    free(rt->code);
    free(rt->funcs);
    free(rt->globals);
    free(rt->stk);
    free(rt->tries);
    free(rt);
}

void akl_set_cojit(AklRT *rt, int enabled) { if (rt) rt->cojit_off = !enabled; }
uint32_t akl_cojit_count(AklRT *rt) { return rt ? rt->cojit_applied : 0; }

bool akl_eval(AklRT *rt, const char *src, AklVal *out) {
    rt->err[0] = 0;
    rt->n_tries = 0; /* 前回 eval が途中終了（uncaught/budget/誤り）した場合の残留を棄却 */
    rt->n_nury = 0; /* 同上（C 側一時ルートの残留棄却。内部規律は各使用者復元） */
    rt->native_err = false;
    rt->last_val = AKL_VAL_UNDEF; /* 前回 eval の最後の式文の値が残留しないようリセット */
    if (rt->gc_live) { akl_errf(rt, "recursive akl_eval is not supported"); return false; }
    if (!src) { akl_errf(rt, "null source"); return false; }
    u64 slen = strlen(src);
    if (slen > AKL_MAX_SRC) { akl_errf(rt, "source budget exhausted"); return false; }

    P p;
    memset(&p, 0, sizeof p);
    p.rt = rt;
    /* 親クラス保持ローカル・クラス親参照の非公開名（ソースに現れない NUL 入り） */
    p.super_name = akl_intern(rt, (const u8 *)"\x01super", 7, NULL);
    p.super_prop = akl_intern(rt, (const u8 *)"\x00super", 7, NULL);
    p.lx.s = (const u8 *)src;
    p.lx.n = (u32)slen;
    p.lx.pos = 0;
    p.lx.line = 1;
    /* shebang 許容 */
    if (slen >= 2 && src[0] == '#' && src[1] == '!') {
        while (p.lx.pos < p.lx.n && src[p.lx.pos] != '\n') p.lx.pos++;
    }
    if (lex_next(&p.lx) < 0) { akl_errf(rt, "lex error at line %u", p.lx.line); free(p.lx.esc); return false; }

    /* program: 文の列（scratch 収集 → commit） */
    u32 first = N_NONE, cnt = 0;
    {
        U32Vec sc = { NULL, 0, 0 };
        while (p.lx.kind != TK_EOF) {
            u32 s = p_stmt(&p);
            if (s == N_NONE) break;
            if (p_scratch(&p, &sc, s) < 0) break;
        }
        first = p_list_commit(&p, &sc);
        cnt = sc.n;
        free(sc.v);
        if (cnt && first == N_NONE && !p.fail) p.fail = "oom: list";
    }
    bool parse_ok = !p.fail && p.lx.kind == TK_EOF;
    u32 prog = N_NONE;
    if (parse_ok) {
        prog = p_node(&p, N_BLOCK);
        if (prog == N_NONE) parse_ok = false;
        else { p.nodes[prog].a = first; p.nodes[prog].c = cnt; }
    }
    if (!parse_ok) {
        const char *why = p.fail ? p.fail : "syntax error";
        akl_errf(rt, "SyntaxError: %s (line %u)", why, p.lx.line);
        free(p.nodes); free(p.list); free(p.lx.esc);
        return false;
    }
    /* v0.3: クロージャ捕捉解析（codegen に先立って全関数の capture 情報を確定） */
    if (!akl_analyze(&p, prog)) {
        akl_errf(rt, "SyntaxError: %s (line %u)", p.fail ? p.fail : "capture analysis", p.lx.line);
        free(p.nodes); free(p.list); free(p.lx.esc);
        if (p.fninfo) {
            for (u32 i = 0; i < p.fninfo_n; i++) { free(p.fninfo[i].cap_names); free(p.fninfo[i].decls.v); }
            free(p.fninfo);
        }
        return false;
    }

    /* codegen（main 関数 = entry 0 を eval ごとに新規作成…ではなく funcs[0] は
     * 「直近 eval の main」として使い回す。過去 eval のコードは funcs 参照が無い
     * ため到達不能（verify は全体を走査するが incoherent にはならない）…としたいが、
     * 過去コードの GLOAD 等は obj 表が永続なので依然 valid。検査のため verify 全体を通す） */
    AklFuncEnt *f0;
    if (rt->n_funcs == rt->cap_funcs) {
        u32 nc = rt->cap_funcs ? rt->cap_funcs * 2 : 32;
        AklFuncEnt *nf = (AklFuncEnt *)realloc(rt->funcs, (u64)nc * sizeof(AklFuncEnt));
        if (!nf) { akl_errf(rt, "oom: funcs"); free(p.nodes); free(p.list); free(p.lx.esc); return false; }
        rt->funcs = nf; rt->cap_funcs = nc;
    }
    u32 main_idx = rt->n_funcs++;
    u32 code_from = rt->code_len;
    Cg cg;
    memset(&cg, 0, sizeof cg);
    cg.rt = rt;
    cg.p = &p;
    cg.cur_fn = (i32)main_idx;
    cg.cur_fi = &p.main_fi;
    cg.cur_n_env = p.main_fi.n_cap;
    cg.cur_needs_cap = p.main_fi.needs_cap;
    /* パラメータなし・locals は stmt の var で追加される。隠し slot（this 等）は
     * ダミー LocalEnt として先頭に予約（ネスト関数と同一の規約） */
    rt->funcs[main_idx].code_off = code_from;
    rt->funcs[main_idx].name = 0;
    rt->funcs[main_idx].n_params = 0;
    cg.in_func_depth = 0;
    {
        u32 nh0 = cg_hidden(&cg);
        for (u32 i = 0; i < nh0; i++) cg_local_add_dummy(&cg);
    }
#ifdef AKL_AST_DUMP
    akl_ast_dump(&p);
#endif
    cg_stmt(&cg, prog);
    cg_op(&cg, OP_HALT);
    rt->funcs[main_idx].n_locals = (u16)cg.n_locals;
    rt->funcs[main_idx].n_env = (u16)cg.cur_n_env;
    rt->funcs[main_idx].n_cap = (u16)cg.cur_needs_cap;
    rt->funcs[main_idx].code_end = rt->code_len;
    f0 = &rt->funcs[main_idx];
    free(cg.locals);
    if (p.fninfo) {
        /* fninfo は解析時サイズ（fninfo_n）。合成ノード（空コンストラクタ等）で
         * n_nodes が増えても範囲外を free しない */
        for (u32 i = 0; i < p.fninfo_n; i++) { free(p.fninfo[i].cap_names); free(p.fninfo[i].decls.v); }
        free(p.fninfo);
    }
    free(p.main_fi.cap_names);
    free(p.main_fi.decls.v);
    free(p.nodes); free(p.list); free(p.lx.esc);
    /* 失敗時は funcs 表も main_idx まで戻す（途中生成のネスト関数エントリが
     * 区間表に残って次回 eval の region を食い違わせる潜在バグを塞ぐ） */
    if (cg.fail) { rt->n_funcs = main_idx; rt->code_len = code_from; return false; }

    /* CoJIT: 意味保持が構造的に自明な形のみ特化（異常時は無変更）。append したら
     * main 領域を再捺印してから必ず verify の whole-scan を通す（事後セルフチェック） */
    if (!rt->cojit_off) (void)akl_cojit(rt, code_from);
    f0 = &rt->funcs[main_idx];
    f0->code_end = rt->code_len;

    if (!akl_verify(rt, f0->code_off)) return false;

    rt->pin_mark = rt->n_objs; /* コンパイル由来はスイープ対象外。実行時生成物のみ集める */
    rt->gc_live = true;        /* GC は VM ルートが生きている実行中に限る */
    if (getenv("AKL_DUMP")) {
        static const char *const ON[] = {
            "CONST_I",
            "CONST_D",
            "CONST_STR",
            "TRUE_T",
            "FALSE_T",
            "NULL_T",
            "UNDEF_T",
            "ADD",
            "SUB",
            "MUL",
            "DIV",
            "MOD",
            "LT",
            "LE",
            "GT",
            "GE",
            "EQ",
            "NE",
            "SEQ",
            "SNE",
            "NOT",
            "NEG",
            "POS",
            "TYPEOF",
            "POP",
            "POPV",
            "DUP",
            "LLOAD",
            "LSTORE",
            "GLOAD",
            "GSTORE",
            "JMP",
            "JMPF",
            "JMPT",
            "CALL",
            "RET",
            "MAKEF",
            "LINC",
            "CJMPF_L",
            "CJMPF_G",
            "GLOAD_S",
            "GSTORE_S",
            "GINC",
            "GMULC",
            "LMULC",
            "MULCI",
            "ADDCI",
            "SUBCI",
            "MODCI",
            "GADD_P",
            "LADD_P",
            "GADD_G",
            "CJMPF_MODG",
            "CJMPF_MODL",
            "GSTORE_SPV",
            "LSTORE_PV",
            "LOOPINC_G",
            "ADDCI_G",
            "SUBCI_G",
            "MULCI_G",
            "MODCI_G",
            "ADDCI_L",
            "SUBCI_L",
            "MULCI_L",
            "MODCI_L",
            "CJMPF_MULGG",
            "CJMPF_MODGG",
            "LADD_LL",
            "RET_L",
            "ADD_GX",
            "SUB_GX",
            "MUL_GX",
            "MOD_GX",
            "ADD_LX",
            "SUB_LX",
            "MUL_LX",
            "MOD_LX",
            "TRY_PUSH",
            "TRY_LEAVE",
            "FIN_END",
            "THROW",
            "LOOPINC_L",
            "LOOPINC_GV",
            "LOOPINC_LV",
            "NOP",
            "OBJNEW",
            "PLOAD",
            "PSTORE",
            "MCALL",
            "THIS",
            "ELOAD",
            "ESTORE",
            "CELOAD",
            "CESTORE",
            "ANEW",
            "AGET",
            "ASET",
            "BNOT",
            "BAND",
            "BOR",
            "BXOR",
            "BSHL",
            "BSHR",
            "BUSHR",
            "POW",
            "KEYSOF",
            "TOARR",
            "ARRPUSH",
            "ARRPUSHALL",
            "ARRSPREADC",
            "CALLN",
            "IN",
            "PDEL",
            "IDEL",
            "INSTANCEOF",
            "NEW",
            "NEWREGEX",
            "CALLT",
            "MAKEFS",
            "SUPERGET",
            "OBJSPREAD",
            "PSETDYN",
            "MCALLN",
            "ARRREST",
            "OBJREST",
            "HALT"
        };
        fprintf(stderr, "--- main ---\n");
        u8 *cp = rt->code + code_from;
        u32 rem = rt->code_len - code_from;
        while (rem) {
            u8 op = *cp;
            fprintf(stderr, "  %04u: %s\n", (u32)(cp - (rt->code + code_from)),
                    op < (sizeof ON / sizeof ON[0]) ? ON[op] : "??");
            cp += 1 + akl_op_imm_len(op);
            rem -= 1 + akl_op_imm_len(op);
        }
    }
    bool vm_ok = vm_exec(rt, main_idx, NULL, 0, AKL_VAL_UNDEF, AKL_VAL_UNDEF, false);
    /* eval 終了時の残骸回収: 次回 eval の pin_mark が「実行時生成物まで」取り込んで
     * 永久にスイープ対象外にする問題への対処。gc_sp=0（スタックは root にしない）で
     * GC を発火し、ローカルで不要になった実行時オブジェクト（ROPE チェーン等）を
     * 回収する。globals / last_val に残る値は正しく生存する。 */
    rt->gc_sp = 0;
    rt->gc_live = true;
    akl_gc(rt);
    rt->gc_live = false;
    if (!vm_ok) return false;

    if (out) *out = rt->last_val;
    return true;
}

/* v0.3 再入呼び出し（高階関数のコールバック等）: fn（FUNC または NATIVE）を呼ぶ。
 * - FUNC: outer スタックを退避して root_stks に登録（GC ルート）し、新スタックで
 *   vm_exec を再入実行。tries は inner 用にリセットし、終了後に復元。
 * - NATIVE: 直接呼ぶ（VM 再入なし。この場合の AKL_NATIVE_COST 課金は呼出側の
 *   MCALL 課金で賄われる形になる — 高階コールバック自体は inner の既定 budget が裁く）。
 * - 失敗（コールバック内の例外・budget 枯渇）は false で rt->err を設定。
 * - 再入深さは AKL_MAX_REENTRY で fail-stop（スタック枯渇の構造的防止）。 */
bool akl_call(AklRT *rt, AklVal fn, int argc, const AklVal *argv, AklVal *out) {
    if (!rt) return false;
    if (rt->call_depth >= AKL_MAX_REENTRY) {
        akl_errf(rt, "call depth budget exhausted");
        return false;
    }
    if (!akl_is_objv(fn)) { akl_errf(rt, "TypeError: not a function"); return false; }
    u32 oi = akl_get_obj(fn);
    if (oi >= rt->n_objs) { akl_errf(rt, "TypeError: not a function"); return false; }
    AklObj *fo = &rt->objs[oi];
    if (fo->kind == AKL_OK_NATIVE) {
        *out = fo->u.nat.fn(rt, AKL_VAL_UNDEF, argc, argv, fo->u.nat.udata);
        return !rt->native_err;
    }
    if (fo->kind != AKL_OK_FUNC) { akl_errf(rt, "TypeError: not a function"); return false; }
    u32 fe_i = fo->code_off;
    if (fe_i >= rt->n_funcs) { akl_errf(rt, "internal: bad func ref"); return false; }

    /* outer 状態を退避し、inner のための新スタックを用意 */
    AklVal *old_stk = rt->stk;
    u32 old_cap = rt->cap_stk;
    u32 old_gc_sp = rt->gc_sp;
    u32 old_n_tries = rt->n_tries;
    AklRootStk rs;
    rs.stk = old_stk;
    rs.sp = old_gc_sp;
    rs.next = rt->root_stks;
    rt->root_stks = &rs;

    u32 ncap = rt->cap_stk ? rt->cap_stk : 1024;
    AklVal *nstk = (AklVal *)malloc((u64)ncap * sizeof(AklVal));
    if (!nstk) {
        rt->root_stks = rs.next;
        akl_errf(rt, "oom: call stack");
        return false;
    }
    rt->stk = nstk;
    rt->cap_stk = ncap;
    rt->gc_sp = 0;
    rt->n_tries = 0;
    rt->call_depth++;

    AklVal capenv = fo->env == UINT32_MAX ? AKL_VAL_UNDEF : AKL_MK_OBJ(fo->env);
    bool ok = vm_exec(rt, fe_i, argv, (u32)argc, AKL_VAL_UNDEF, capenv, true);

    rt->call_depth--;
    rt->n_tries = old_n_tries;
    free(nstk);
    rt->stk = old_stk;
    rt->cap_stk = old_cap;
    rt->gc_sp = old_gc_sp;
    rt->root_stks = rs.next;

    if (!ok) return false;
    if (out) *out = rt->last_val;
    return true;
}

AklVal akl_mkarray(AklRT *rt, const AklVal *items, uint32_t n) {
    if (!rt) return akl_mkundefined();
    if (n > (u32)(((u64)rt->heap_mb << 20) / sizeof(AklVal))) {
        akl_errf(rt, "heap bytes budget exhausted");
        return akl_mkundefined();
    }
    u32 oi = akl_obj_new(rt);
    if (oi == UINT32_MAX) return akl_mkundefined();
    AklObj *o = &rt->objs[oi];
    o->kind = AKL_OK_ARR;
    if (n) {
        o->u.arr.v = (AklVal *)malloc((u64)n * sizeof(AklVal));
        if (!o->u.arr.v) {
            rt->objs[oi].kind = 0;
            akl_errf(rt, "oom: array");
            return akl_mkundefined();
        }
        memcpy(o->u.arr.v, items, (u64)n * sizeof(AklVal));
        o->u.arr.n = o->u.arr.cap = n;
        rt->heap_bytes += (u64)n * sizeof(AklVal);
    }
    return AKL_MK_OBJ(oi);
}
uint32_t akl_arr_len(AklRT *rt, AklVal arr) {
    if (!rt || !akl_is_objv(arr)) return 0;
    u32 oi = akl_get_obj(arr);
    if (oi >= rt->n_objs || rt->objs[oi].kind != AKL_OK_ARR) return 0;
    return rt->objs[oi].u.arr.n;
}

const char *akl_error(AklRT *rt) { return rt && rt->err[0] ? rt->err : ""; }

/* ---- ホスト側値生成（公開 C ABI。ファサード層向け。契約は akl.h 参照） ---- */
AklVal akl_mknum(double d) { return akl_num(d); }
AklVal akl_mkbool(bool b) { return b ? AKL_VAL_TRUE : AKL_VAL_FALSE; }
AklVal akl_mknull(void) { return AKL_VAL_NULL; }
AklVal akl_mkundefined(void) { return AKL_VAL_UNDEF; }
bool akl_is_string(AklRT *rt, AklVal v) {
    if (!rt || !akl_is_objv(v)) return false;
    u32 oi = akl_get_obj(v);
    if (oi >= rt->n_objs) return false;
    return rt->objs[oi].kind == AKL_OK_STR || rt->objs[oi].kind == AKL_OK_ROPE;
}
AklVal akl_mkstring(AklRT *rt, const char *s, uint32_t len) {
    if (!rt) return AKL_VAL_UNDEF;
    if (!s && len) { akl_errf(rt, "null string data"); return AKL_VAL_UNDEF; }
    if (len > AKL_MAX_SRC) { akl_errf(rt, "string budget exhausted"); return AKL_VAL_UNDEF; }
    /* VM 停止中は akl_gc が no-op（gc_live=false）。nursery 経由の mkstr は
       GC 未発火でも budget 超過時のみ err を返す安全設計（内部 mkstr と同一経路）。
       VM 実行中（native コールバックからの呼出）は nursery に一時ルートを積む：
       これが無いと fn 内で作った値が即 GC 回収され dangling を返し得る（V8 の
       Local handle 問題と同型）。失敗時は eval 全体を明白に失敗させる（budget
       枯渇状況で「静かに undefined」を返す経路を塞ぐ）。 */
    u32 oi = akl_mkstr(rt, (const u8 *)s, len);
    if (oi == UINT32_MAX) { if (rt->gc_live) rt->native_err = true; return AKL_VAL_UNDEF; }
    if (rt->gc_live) {
        if (rt->n_nury >= AKL_NURY_CAP) {
            akl_errf(rt, "native temp budget exhausted");
            rt->native_err = true;
            return AKL_VAL_UNDEF;
        }
        rt->nury[rt->n_nury++] = oi;
    }
    return AKL_MK_OBJ(oi);
}

void akl_set_insn_budget(AklRT *rt, uint64_t budget) {
    if (rt) rt->insn_budget_def = budget;
}

bool akl_as_num(AklVal v, double *out) { return akl_numv(v, out); }
bool akl_as_bool(AklVal v, bool *out) {
    if (v == AKL_VAL_TRUE) { *out = true; return true; }
    if (v == AKL_VAL_FALSE) { *out = false; return true; }
    return false;
}
bool akl_is_null(AklVal v) { return v == AKL_VAL_NULL; }
bool akl_is_undefined(AklVal v) { return v == AKL_VAL_UNDEF; }
const char *akl_as_str(AklRT *rt, AklVal v, uint32_t *len) {
    if (!rt || !akl_is_objv(v)) return NULL;
    u32 oi = akl_get_obj(v);
    if (oi >= rt->n_objs) return NULL;
    if (rt->objs[oi].kind != AKL_OK_STR && rt->objs[oi].kind != AKL_OK_ROPE) return NULL;
    /* NUL 終端を API として約束するための 1 バイト余裕は mkstr で確保していない。
     * len 参照 API なので NUL は返さない（bytes は len まで有効）。ROPE はここで平坦化。
     * 読取 API は自身の成否のみで報告する: 前回 eval の残留 err に黙殺されると
     * ホスト側が恒久的に読めなくなる事前バグ（V8 ファサード持込みで同定） */
    rt->err[0] = 0;
    u32 l;
    const u8 *b = akl_str(rt, oi, &l);
    if (!b) return NULL;
    if (len) *len = l;
    return (const char *)b;
}

void akl_tune(AklRT *rt, uint64_t insn, uint32_t heap_mb, uint32_t max_objs) {
    if (!rt) return;
    if (insn) akl_set_insn_budget(rt, insn);
    if (heap_mb) rt->heap_mb = heap_mb;
    if (max_objs) rt->max_objs = max_objs;
}

/* ============================== ネイティブ登録層（公開 C ABI。契約は akl.h） ============================== */

/* intern で新規作成した STR を VM 実行中に使う場合の保護（nurseryルート行き）。
 * 既存ヒット時は何もしない（nursery は 8 スロットしかないので使い潰さない）。 */
static bool akl_pin_if_created(AklRT *rt, u32 k, bool created) {
    if (!created || !rt->gc_live) return true;
    if (rt->n_nury >= AKL_NURY_CAP) {
        akl_errf(rt, "native temp budget exhausted");
        rt->native_err = true;
        return false;
    }
    rt->nury[rt->n_nury++] = k;
    return true;
}

/* 登録/生成系は VM 停止中限定: native コールバックからの登録は GC/budget 不変条件を
 * 毀損し得るため構造的に拒否する（「明白に失敗」原則） */
static bool akl_host_idle(AklRT *rt) {
    if (rt->gc_live) { akl_errf(rt, "host registration during eval is not supported"); return false; }
    return true;
}

bool akl_is_object(AklRT *rt, AklVal v) {
    return rt && akl_is_objv(v) && akl_get_obj(v) < rt->n_objs &&
           rt->objs[akl_get_obj(v)].kind == AKL_OK_OBJ;
}

bool akl_prop_set(AklRT *rt, AklVal obj, const char *name, AklVal v) {
    if (!rt || !name) return false;
    if (!akl_is_object(rt, obj)) { akl_errf(rt, "prop_set on non-object"); return false; }
    bool created = false;
    u32 k = akl_intern(rt, (const u8 *)name, (u32)strlen(name), &created);
    if (k == UINT32_MAX) { if (rt->gc_live) rt->native_err = true; return false; }
    if (!akl_pin_if_created(rt, k, created)) return false;
    /* 束縛後は OBJ 伝播 mark（prop name も子として mark する設計）で生存が保証される */
    if (!obj_prop_set(rt, &rt->objs[akl_get_obj(obj)], k, v)) {
        if (rt->gc_live) rt->native_err = true;
        return false;
    }
    return true;
}

AklVal akl_prop_get(AklRT *rt, AklVal obj, const char *name) {
    if (!rt || !name || !akl_is_object(rt, obj)) return AKL_VAL_UNDEF;
    bool created = false;
    u32 k = akl_intern(rt, (const u8 *)name, (u32)strlen(name), &created);
    if (k == UINT32_MAX) { if (rt->gc_live) rt->native_err = true; return AKL_VAL_UNDEF; }
    if (!akl_pin_if_created(rt, k, created)) return AKL_VAL_UNDEF;
    i32 i = obj_prop_find(&rt->objs[akl_get_obj(obj)], k);
    return i >= 0 ? rt->objs[akl_get_obj(obj)].u.po.props[i].v : AKL_VAL_UNDEF;
}

AklVal akl_mkobject(AklRT *rt) {
    if (!rt) return AKL_VAL_UNDEF;
    if (!akl_host_idle(rt)) return AKL_VAL_UNDEF;
    u32 oi = akl_obj_new(rt);
    if (oi == UINT32_MAX) return AKL_VAL_UNDEF;
    rt->objs[oi].kind = AKL_OK_OBJ;
    return AKL_MK_OBJ(oi);
}

AklVal akl_mknative(AklRT *rt, AklNativeFn fn, void *udata) {
    if (!rt || !fn) { if (rt) akl_errf(rt, "bad native function handle"); return AKL_VAL_UNDEF; }
    if (!akl_host_idle(rt)) return AKL_VAL_UNDEF;
    u32 oi = akl_obj_new(rt);
    if (oi == UINT32_MAX) return AKL_VAL_UNDEF;
    AklObj *o = &rt->objs[oi];
    o->kind = AKL_OK_NATIVE;
    o->u.nat.fn = fn;
    o->u.nat.udata = udata;
    return AKL_MK_OBJ(oi);
}

/* HANDLE はメソッド戻り値として「実行中生成」が本筋のため mknative とは規約が違う:
 * VM 実行中も生成可（mkstring 同型の nursery 一時保護＋直後に束縛/返却の規約）。
 * ホスト側オブジェクト構築（document 等）は通常通り eval 前（VM 停止中）に行う。 */
AklVal akl_mkhandle(AklRT *rt, const AklHandleVTab *vt, void *ptr) {
    if (!rt || !vt) { if (rt) akl_errf(rt, "bad handle vtab"); return AKL_VAL_UNDEF; }
    u32 oi = akl_obj_new(rt);
    if (oi == UINT32_MAX) { if (rt->gc_live) rt->native_err = true; return AKL_VAL_UNDEF; }
    AklObj *o = &rt->objs[oi];
    o->kind = AKL_OK_HANDLE;
    o->u.hd.vt = vt;
    o->u.hd.ptr = ptr;
    if (rt->gc_live) {
        if (rt->n_nury >= AKL_NURY_CAP) {
            akl_errf(rt, "native temp budget exhausted");
            rt->native_err = true;
            return AKL_VAL_UNDEF;
        }
        rt->nury[rt->n_nury++] = oi;
    }
    return AKL_MK_OBJ(oi);
}

bool akl_is_handle(AklRT *rt, AklVal v) {
    return rt && akl_is_objv(v) && akl_get_obj(v) < rt->n_objs &&
           rt->objs[akl_get_obj(v)].kind == AKL_OK_HANDLE;
}

/* globals への const 束縛（register/global_set の共通内部）。同名は上書き。
 * （ghash は n_globals 変化でのみ再構築なので既存 slot 書換は自動整合） */
static bool akl_bind_global(AklRT *rt, const char *name, AklVal v) {
    u32 k = akl_intern(rt, (const u8 *)name, (u32)strlen(name), NULL);
    if (k == UINT32_MAX) return false;
    for (u32 i = 0; i < rt->n_globals; i++) {
        if (rt->globals[i].name == k) { rt->globals[i].v = v; rt->globals[i].is_const = 1; return true; }
    }
    if (rt->n_globals == rt->cap_globals) {
        u32 nc = rt->cap_globals ? rt->cap_globals * 2 : 64;
        AklGlobal *ng = (AklGlobal *)realloc(rt->globals, (u64)nc * sizeof(AklGlobal));
        if (!ng) { akl_errf(rt, "oom: globals"); return false; }
        rt->globals = ng; rt->cap_globals = nc;
    }
    AklGlobal *g = &rt->globals[rt->n_globals++];
    g->name = k; g->v = v; g->is_const = 1;
    memset(g->_p, 0, sizeof g->_p);
    return true;
}

bool akl_global_set(AklRT *rt, const char *name, AklVal v) {
    if (!rt || !name) return false;
    if (!akl_host_idle(rt)) return false;
    return akl_bind_global(rt, name, v);
}

bool akl_native_register(AklRT *rt, const char *name, AklNativeFn fn, void *udata) {
    if (!rt || !name || !fn) { if (rt) akl_errf(rt, "bad native registration"); return false; }
    if (!akl_host_idle(rt)) return false;
    u32 oi = akl_obj_new(rt);
    if (oi == UINT32_MAX) return false;
    AklObj *o = &rt->objs[oi];
    o->kind = AKL_OK_NATIVE;
    o->u.nat.fn = fn;
    o->u.nat.udata = udata;
    if (!akl_bind_global(rt, name, AKL_MK_OBJ(oi))) { obj_free_rollback(rt, oi); return false; }
    return true;
}

void akl_native_throw(AklRT *rt, const char *msg) {
    if (!rt) return;
    if (msg == rt->err) {
        /* msg が rt->err 自身を指すケース（native が rt->err をそのまま渡す）:
         * errf の %s は src/dst エイリアスでバッファを壊し、エラーメッセージが
         * 空になる（実測で特定: 高階関数のコールバック例外が 'uncaught exception:
         * 42' から '' に化けた）。先に別バッファへ退避してから設定する。 */
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s", rt->err);
        akl_errf(rt, "%s", tmp);
    } else {
        akl_errf(rt, "%s", msg ? msg : "native exception");
    }
    rt->native_err = true;
}

AklVal akl_tostring(AklRT *rt, AklVal v) {
    if (!rt) return AKL_VAL_UNDEF;
    u32 k = akl_to_string(rt, v);
    if (k == UINT32_MAX) { if (rt->gc_live) rt->native_err = true; return AKL_VAL_UNDEF; }
    return AKL_MK_OBJ(k);
}
