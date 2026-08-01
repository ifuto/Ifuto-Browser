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
#include <string.h>
#include <stdlib.h>
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

enum { AKL_OK_STR = 1, AKL_OK_FUNC = 2, AKL_OK_ROPE = 3 };
/* ROPE: code_off=左 obj idx, name=右 obj idx, n_params=深さ(最大4096), len=全長。
 * 不変条件: 子の index は親より小さい必要は「ない」（free-list 再利用で逆転し得る）。
 * よって GC の伝播は添字順に依らない明示ワークリストで行う。文字列は不変。 */

/* nursery（C 側一時ルート）容量。最大同時ピンは eq/rel 系の入れ子で
 * 2(ハンドラ) + 2(loose/strict) + 1(flatten) = 5。余裕を見て 8。 */
#define AKL_NURY_CAP 8

typedef struct {
    u8 kind;
    u8 _p[3];
    u32 len;      /* STR: バイト長 */
    u8 *bytes;    /* STR: malloc 所有 */
    u32 code_off; /* FUNC */
    u32 name;     /* FUNC: 名前 STR の obj index（呼出名診断用） */
    u16 n_params; /* FUNC */
    u16 n_locals; /* FUNC */
} AklObj;

/* ============================== runtime ============================== */

typedef struct { u32 name; AklVal v; u8 is_const; u8 _p[3]; } AklGlobal;
typedef struct { u32 code_off, code_end; u32 name; u16 n_params, n_locals; } AklFuncEnt;

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
    OP_HALT,
    OP_COUNT
};

/* JS 例外の実行時エントリ。kind: TRY（捕捉候補）/ FIN（finally 実行中＝保留状態）。
 * in_catch: TRY で catch 本体実行中（この状態への throw では finally だけを残して pop）。
 * mode（FIN）: 0=normal, 1=throw, 2=return。pending に例外/返り値を保持（GC ルート）。 */
enum { AKL_TE_TRY = 0, AKL_TE_FIN = 1 };
#define AKL_PC_NONE 0xFFFFFFFFu
#define AKL_SLOT_NONE 0xFFFFFFFFu
typedef struct {
    AklVal pending;
    u32 frame;       /* 属する呼出し深さ（main=0, 1回目の callee=1, ...） */
    u32 sp;
    u32 catch_pc, finally_pc, catch_slot;
    u32 resume_pc;   /* FIN: mode 0 の継続先 */
    u8 kind, mode, in_catch, _p;
} AklTryEnt;

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
    char err[256];
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
    AKL_MAX_LOCALS   = 1024
};

/* ============================== 診断/確保 ============================== */

static void akl_errf(AklRT *rt, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rt->err, sizeof rt->err, fmt, ap);
    va_end(ap);
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
    akl_gc_mark_val(rt, rt->last_val, mk);
    for (u32 i = 0; i < rt->n_tries; i++)
        if (rt->tries[i].kind == AKL_TE_FIN) akl_gc_mark_val(rt, rt->tries[i].pending, mk);
    for (u32 i = 0; i < rt->n_globals; i++) akl_gc_mark_val(rt, rt->globals[i].v, mk);
    for (u32 i = 0; i < rt->n_nury; i++) {
        u32 oi = rt->nury[i];
        if (oi >= rt->pin_mark && oi < rt->n_objs) mk[oi - rt->pin_mark] = 1;
    }
    /* ROPE の伝播: free-list 再利用で子 index > 親 index があり得るため
     * 添字順走査では閉包が取れない。明示ワークリスト（深さ上限 4096 由来の有界）で辿る。 */
    {
        u32 *wl = (u32 *)malloc((u64)span * sizeof(u32));
        if (wl) {
            u32 wn = 0;
            for (u32 i = rt->pin_mark; i < rt->n_objs; i++)
                if (mk[i - rt->pin_mark] && rt->objs[i].kind == AKL_OK_ROPE) wl[wn++] = i;
            while (wn) {
                u32 ri = wl[--wn];
                AklObj *ro = &rt->objs[ri];
                u32 kids[2] = { ro->code_off, ro->name };
                for (int k = 0; k < 2; k++) {
                    u32 ci = kids[k];
                    if (ci >= rt->pin_mark && ci < rt->n_objs && !mk[ci - rt->pin_mark]) {
                        mk[ci - rt->pin_mark] = 1;
                        if (rt->objs[ci].kind == AKL_OK_ROPE && wn < span) wl[wn++] = ci;
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
        o->kind = 0; o->bytes = NULL; o->len = 0; o->code_off = 0; o->name = 0;
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
static u32 akl_str_flatten(AklRT *rt, u32 idx, u8 **out_bytes); /* 前方宣言 */
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
    TK_KW
};

typedef struct {
    const u8 *s; u32 n, pos, line;
    /* 現在トークン */
    u8 kind; u8 pk; /* KW: kw id / PUNCT: punct id */
    double num; bool num_is_int; i32 num_i;
    u32 str_len; const u8 *str_p; /* デコード済みは decode バッファに */
    u8 *esc; u32 esc_n, esc_cap;  /* 文字列デコード用（rt 外で確保）
*/
} Lex;

enum { KW_VAR, KW_LET, KW_CONST, KW_FUNCTION, KW_RETURN, KW_IF, KW_ELSE, KW_WHILE,
       KW_FOR, KW_BREAK, KW_CONTINUE, KW_TRUE, KW_FALSE, KW_NULL, KW_UNDEFINED, KW_TYPEOF,
       KW_THROW, KW_TRY, KW_CATCH, KW_FINALLY,
       KW_N };
static const char *const AKL_KWS[KW_N] = {
    "var", "let", "const", "function", "return", "if", "else", "while",
    "for", "break", "continue", "true", "false", "null", "undefined", "typeof",
    "throw", "try", "catch", "finally"
};

enum { P_LP, P_RP, P_LC, P_RC, P_SEMI, P_COMMA, P_ASSIGN, P_PLUS, P_MINUS, P_STAR,
       P_SLASH, P_PCT, P_BANG, P_LT, P_LE, P_GT, P_GE, P_EQEQ, P_NEQ, P_SEQ, P_SNE,
       P_ANDAND, P_OROR, P_N };
static const char *const AKL_PUNCTS[P_N] = {
    "(", ")", "{", "}", ";", ",", "=", "+", "-", "*", "/", "%", "!", "<", "<=", ">", ">=",
    "==", "!=", "===", "!==", "&&", "||"
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
static int lex_string(Lex *lx) {
    u8 q = lx->s[lx->pos++];
    lx->esc_n = 0;
    for (;;) {
        if (lex_eof(lx) || lex_cur(lx) == '\n') return -1;
        u8 c = lx->s[lx->pos++];
        if (c == q) break;
        if (c != '\\') { lex_utf8_emit(lx, c); continue; }
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
static int lex_next(Lex *lx) {
    lex_skip_ws(lx);
    if (lex_eof(lx)) { lx->kind = TK_EOF; return 0; }
    u8 c = lex_cur(lx);
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
            while (hex_dig(lex_cur(lx)) >= 0) {
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
            while (lex_cur(lx) == '0' || lex_cur(lx) == '1') {
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
            while (lex_cur(lx) >= '0' && lex_cur(lx) <= '7') {
                ov = ov * 8 + (double)(lex_cur(lx) - '0');
                lx->pos++;
                if (++guard > 16) return -1; /* 8^16 = 2^48 まで正確 */
            }
            lx->num = ov;
        } else {
            while (lex_cur(lx) >= '0' && lex_cur(lx) <= '9') {
                v = v * 10 + (double)(lex_cur(lx) - '0');
                lx->pos++;
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
    /* ++ / -- は未対応。通すと二重 unary（+ (+x) / - (-x)）に落ちて「静かに
     * 間違った答え」を返す（実測: var i=5; --i; i → 5、本来は 4）。
     * 規則「未対応は構文エラーで明白に落ちる」に従い、隣接は lex で拒否する。
     * JS 側も a--b は SyntaxError なので互換方向でも安全側。
     * 空白ありの 1 - -2 は従来通り 3 に評価される（分離は失われない）。 */
    if ((c == '+' && lex_at(lx, 1) == '+') || (c == '-' && lex_at(lx, 1) == '-'))
        return -1;
    /* punct 複数文字（最長一致。テーブル順序に依存しない: "==" が "===" を潰さない） */
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
    N_NONE = 0xFFFFFFFFu
};

typedef struct { u8 kind; u8 op; u8 flags; u8 _p; u32 a, b, c, d; } AklNode; /* 20B */

typedef struct {
    AklRT *rt;
    Lex lx;
    AklNode *nodes; u32 n_nodes, cap_nodes;
    u32 *list; u32 n_list, cap_list;     /* 引数・パラメータ・文の並び */
    u32 depth;
    const char *fail;                    /* 構文エラーの原因（短い固定文） */
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
typedef struct { u32 *v; u32 n, cap; } U32Vec;
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
static u32 p_intern(P *p, const u8 *s, u32 n) {
    AklRT *rt = p->rt;
    for (u32 i = 0; i < rt->n_objs; i++) {
        if (rt->objs[i].kind != AKL_OK_STR) continue;
        if (rt->objs[i].len == n && (n == 0 || memcmp(rt->objs[i].bytes, s, n) == 0))
            return i;
    }
    u32 idx = akl_mkstr(rt, s, n);
    if (idx == UINT32_MAX) p->fail = "intern failed";
    return idx;
}

/* ---- 式（再帰下降、優先順位段ごと） ---- */
static u32 p_expr(P *p);

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
        /* 呼び出し連鎖 */
        while (p_eat_punct(p, P_LP)) {
            U32Vec sc = { NULL, 0, 0 };
            if (!p_is_punct(p, P_RP)) {
                for (;;) {
                    u32 arg = p_expr(p);
                    if (arg == N_NONE || p_scratch(p, &sc, arg) < 0) { free(sc.v); p->depth--; return N_NONE; }
                    if (!p_eat_punct(p, P_COMMA)) break;
                }
            }
            if (!p_expect_punct(p, P_RP, "expected ')'")) { free(sc.v); p->depth--; return N_NONE; }
            if (sc.n > 250) { p->fail = "too many arguments"; free(sc.v); p->depth--; return N_NONE; }
            u32 first = p_list_commit(p, &sc);
            u32 cnt = sc.n;
            free(sc.v);
            if (cnt && first == N_NONE) { p->depth--; return N_NONE; }
            u32 call = p_node(p, N_CALL);
            if (call == N_NONE) { p->depth--; return N_NONE; }
            p->nodes[call].a = ni;
            p->nodes[call].b = first;
            p->nodes[call].c = cnt;
            ni = call;
        }
        p->depth--;
        return ni;
    }
    if (p_eat_punct(p, P_LP)) {
        u32 e = p_expr(p);
        if (e == N_NONE) { p->depth--; return N_NONE; }
        if (!p_expect_punct(p, P_RP, "expected ')'")) { p->depth--; return N_NONE; }
        /* 括弧化式も呼び出し連鎖しうる */
        while (p_eat_punct(p, P_LP)) {
            U32Vec sc = { NULL, 0, 0 };
            if (!p_is_punct(p, P_RP)) {
                for (;;) {
                    u32 arg = p_expr(p);
                    if (arg == N_NONE || p_scratch(p, &sc, arg) < 0) { free(sc.v); p->depth--; return N_NONE; }
                    if (!p_eat_punct(p, P_COMMA)) break;
                }
            }
            if (!p_expect_punct(p, P_RP, "expected ')'")) { free(sc.v); p->depth--; return N_NONE; }
            if (sc.n > 250) { p->fail = "too many arguments"; free(sc.v); p->depth--; return N_NONE; }
            u32 first = p_list_commit(p, &sc);
            u32 cnt = sc.n;
            free(sc.v);
            if (cnt && first == N_NONE) { p->depth--; return N_NONE; }
            u32 call = p_node(p, N_CALL);
            if (call == N_NONE) { p->depth--; return N_NONE; }
            p->nodes[call].a = e;
            p->nodes[call].b = first;
            p->nodes[call].c = cnt;
            e = call;
        }
        p->depth--;
        return e;
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
    u8 uop = 0;
    if (p_eat_punct(p, P_BANG)) uop = OP_NOT;
    else if (p_eat_punct(p, P_MINUS)) uop = OP_NEG;
    else if (p_eat_punct(p, P_PLUS)) uop = OP_POS;
    else if (p_is_kw(p, KW_TYPEOF)) { if (lex_next(&p->lx) < 0) p->fail = "lex error"; uop = OP_TYPEOF; }
    u32 ni;
    if (!uop) ni = p_primary(p);
    else if (p->fail) ni = N_NONE;
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
    while (p_is_punct(p, mk_op) || (mk_op == P_EQEQ && (p_is_punct(p, P_NEQ) || p_is_punct(p, P_SEQ) || p_is_punct(p, P_SNE)))
           || (mk_op == P_LT && (p_is_punct(p, P_LE) || p_is_punct(p, P_GT) || p_is_punct(p, P_GE)))) {
        u8 pk = p->lx.pk;
        lex_next(&p->lx);
        u32 rhs = next(p);
        if (rhs == N_NONE) return N_NONE;
        u8 op;
        switch (pk) {
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
static u32 p_mul(P *p) {
    u32 lhs = p_unary(p);
    if (lhs == N_NONE) return N_NONE;
    while (p_is_punct(p, P_STAR) || p_is_punct(p, P_SLASH) || p_is_punct(p, P_PCT)) {
        u8 pk = p->lx.pk;
        lex_next(&p->lx);
        u32 rhs = p_unary(p);
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
static u32 p_rel(P *p)   { return p_bin_rhs(p, P_LT, p_add); }
static u32 p_eq(P *p)    { return p_bin_rhs(p, P_EQEQ, p_rel); }

static u32 p_logical_or(P *p);

static u32 p_expr(P *p) {
    u32 lhs = p_logical_or(p);
    if (lhs == N_NONE) return N_NONE;
    if (p_is_punct(p, P_ASSIGN)) {
        if (p->nodes[lhs].kind != N_IDENT) { p->fail = "invalid assignment target"; return N_NONE; }
        lex_next(&p->lx);
        u32 rhs = p_expr(p);
        if (rhs == N_NONE) return N_NONE;
        u32 ni = p_node(p, N_ASSIGN);
        if (ni == N_NONE) return N_NONE;
        p->nodes[ni].a = p->nodes[lhs].a; /* name idx */
        p->nodes[ni].b = rhs;
        return ni;
    }
    return lhs;
}

static u32 p_logical_and(P *p) {
    u32 lhs = p_eq(p);
    if (lhs == N_NONE) return N_NONE;
    while (p_is_punct(p, P_ANDAND)) {
        lex_next(&p->lx);
        u32 rhs = p_eq(p);
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
    /* '(' の後のパラメータ名列（scratch 収集 → commit） */
    U32Vec sc = { NULL, 0, 0 };
    if (!p_is_punct(p, P_RP)) {
        for (;;) {
            if (p->lx.kind != TK_IDENT) { p->fail = "expected parameter name"; free(sc.v); return N_NONE; }
            u32 idx = p_intern(p, p->lx.str_p, p->lx.str_len);
            if (idx == UINT32_MAX || p_scratch(p, &sc, idx) < 0) { free(sc.v); return N_NONE; }
            lex_next(&p->lx);
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
    if (p_is_kw(p, KW_VAR) || p_is_kw(p, KW_LET) || p_is_kw(p, KW_CONST)) {
        u8 is_const = p->lx.pk == KW_CONST;
        lex_next(&p->lx);
        /* カンマ宣言は全宣言を保持する（旧実装は最後の宣言しか返さず、
         * `var a=0,b=0; a` が ReferenceError になる実在バグだった。テストで同定） */
        U32Vec sc = { NULL, 0, 0 };
        for (;;) {
            if (p->lx.kind != TK_IDENT) { p->fail = "expected variable name"; goto out_fail; }
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
            e = p_expr(p);
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
    if (p_is_kw(p, KW_FOR)) {
        lex_next(&p->lx);
        if (!p_expect_punct(p, P_LP, "expected '('")) goto out;
        u32 init = N_NONE, cond = N_NONE, step = N_NONE;
        if (!p_is_punct(p, P_SEMI)) {
            if (p_is_kw(p, KW_VAR) || p_is_kw(p, KW_LET) || p_is_kw(p, KW_CONST)) {
                /* for 内の var 宣言: 末尾 ';' まで 1 個だけ許す簡易形（仕様の for(var i=0;..)） */
                u8 is_const = p->lx.pk == KW_CONST;
                lex_next(&p->lx);
                if (p->lx.kind != TK_IDENT) { p->fail = "expected variable name"; goto out; }
                u32 name = p_intern(p, p->lx.str_p, p->lx.str_len);
                if (name == UINT32_MAX) goto out;
                lex_next(&p->lx);
                u32 iv = N_NONE;
                if (p_eat_punct(p, P_ASSIGN)) { iv = p_expr(p); if (iv == N_NONE) goto out; }
                else if (is_const) { p->fail = "const declaration requires initializer"; goto out; }
                init = p_node(p, N_VAR);
                if (init != N_NONE) { p->nodes[init].flags = is_const; p->nodes[init].a = name; p->nodes[init].b = iv; }
            } else {
                init = p_expr(p);
                if (init == N_NONE) goto out;
            }
        }
        if (!p_expect_punct(p, P_SEMI, "expected ';'")) goto out;
        if (!p_is_punct(p, P_SEMI)) { cond = p_expr(p); if (cond == N_NONE) goto out; }
        if (!p_expect_punct(p, P_SEMI, "expected ';'")) goto out;
        if (!p_is_punct(p, P_RP)) {
            /* step は代入式まで許す（N_ASSIGN 化は p_expr が行う） */
            step = p_expr(p);
            if (step == N_NONE) goto out;
        }
        if (!p_expect_punct(p, P_RP, "expected ')'")) goto out;
        u32 body = p_stmt(p);
        if (body == N_NONE) goto out;
        ni = p_node(p, N_FOR);
        if (ni != N_NONE) { p->nodes[ni].a = init; p->nodes[ni].b = cond; p->nodes[ni].c = step; p->nodes[ni].d = body; }
        goto out;
    }
    if (p_is_kw(p, KW_BREAK))    { lex_next(&p->lx); p_eat_punct(p, P_SEMI); ni = p_node(p, N_BREAK); goto out; }
    if (p_is_kw(p, KW_CONTINUE)) { lex_next(&p->lx); p_eat_punct(p, P_SEMI); ni = p_node(p, N_CONTINUE); goto out; }
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
    {
        u32 e = p_expr(p);
        if (e == N_NONE) goto out;
        if (!p_eat_punct(p, P_SEMI) && p->lx.kind != TK_EOF && !p_is_punct(p, P_RC)) { p->fail = "expected ';'"; goto out; }
        ni = p_node(p, N_EXPRSTMT);
        if (ni != N_NONE) p->nodes[ni].a = e;
    }
out:
    p->depth--;
    return ni;
}

/* ============================== codegen ============================== */

typedef struct { u32 name; u8 is_const; u8 _p[3]; } LocalEnt;

typedef struct {
    AklRT *rt;
    P *p;
    LocalEnt *locals; u32 n_locals, cap_locals;
    u16 fn_slot_base;      /* この関数の frame 内ローカル数（codegen で確定） */
    i32 cur_fn;            /* codegen 中の関数 index（main=0） */
    u32 in_func_depth;     /* 0=main */
    /* loop の break/continue パッチ連鎖（pos のリストを逆方向リンク: buf[pos]=prev head） */
    u32 brk_head[64], cont_head[64], cont_kind[64]; u32 n_loops;
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

/* ローカル名 → slot。新規なら追加。is_const_out に宣言種別 */
static i32 cg_local_find(Cg *cg, u32 name) {
    for (u32 i = 0; i < cg->n_locals; i++)
        if (cg->locals[i].name == name) return (i32)i;
    return -1;
}
static i32 cg_local_add(Cg *cg, u32 name, u8 is_const) {
    i32 at = cg_local_find(cg, name);
    if (at >= 0) {
        if (cg->locals[at].is_const) { akl_errf(cg->rt, "reassignment of const binding"); cg->fail = true; return -1; }
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
    return (i32)cg->n_locals++;
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
        if (rt->globals[at].is_const) { akl_errf(rt, "reassignment of const binding"); return UINT32_MAX; }
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

/* name のストア命令を出す（main では G、関数内では L 解決→見つからなければ G） */
static bool cg_store(Cg *cg, u32 name, u8 decl_const, bool decl) {
    if (cg->in_func_depth == 0) {
        /* main 固有ローカル（catch 束縛）があればローカルを優先。var 宣言は従来通りグローバル */
        i32 lslot = decl ? -1 : cg_local_find(cg, name);
        if (lslot >= 0) {
            if (cg->locals[lslot].is_const) { akl_errf(cg->rt, "assignment to const local"); cg->fail = true; return false; }
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
    cg_op(cg, OP_LSTORE);
    cg_u32(cg, (u32)slot);
    return !cg->fail;
}
static bool cg_load(Cg *cg, u32 name) {
    /* main 深でもローカルを先に見る（catch 束縛は main にも存在する）。
     * main 固有ローカルが作られるのは catch 束縛のみなので旧経路との衝突は構造的に無い */
    {
        i32 slot = cg_local_find(cg, name);
        if (slot >= 0) {
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
                if (slot >= 0) {
                    cg_op(cg, OP_CJMPF_L);
                    cg_u32(cg, (u32)slot);
                    cg_u32(cg, imm);
                    cg_op(cg, cmp);
                    return cg_u32(cg, 0);
                }
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
                    if (slot >= 0) {
                        cg_op(cg, OP_CJMPF_MODL);
                        cg_u32(cg, (u32)slot);
                        cg_u32(cg, m->a);
                        cg_u32(cg, kn->a);
                        cg_op(cg, mcmp);
                        return cg_u32(cg, 0);
                    }
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
                        if (slot >= 0) { cg_op(cg, OP_LMULC); cg_u32(cg, (u32)slot); cg_u32(cg, imm); break; }
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
                        if (slot >= 0) { cg_op(cg, OP_LMULC); cg_u32(cg, (u32)slot); cg_u32(cg, imm); break; }
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
                if (slot >= 0) { cg_expr(cg, n->b); cg_op(cg, OP_LADD_P); cg_u32(cg, (u32)slot); break; }
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
        for (u32 i = 0; i < n->c; i++) cg_expr(cg, cg->p->list[n->b + i]);
        cg_op(cg, OP_CALL);
        cg_push_byte(cg, (u8)(n->c & 0xFF));
        break;
    }
    default:
        akl_errf(cg->rt, "internal: bad expr node %u", n->kind);
        cg->fail = true;
        break;
    }
}

static void cg_stmt(Cg *cg, u32 ni) {
    if (cg->fail) return;
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
        if (e->kind == N_ASSIGN && e->b != N_NONE) {
            AklNode *rhs = &cg->p->nodes[e->b];
            if (rhs->kind == N_BIN && (rhs->op == OP_ADD || rhs->op == OP_SUB) &&
                rhs->a != N_NONE && rhs->b != N_NONE) {
                AklNode *L = &cg->p->nodes[rhs->a], *R = &cg->p->nodes[rhs->b];
                if (L->kind == N_IDENT && L->a == e->a && R->kind == N_NUM && R->op == 1) {
                    i64 dd = (i64)(i32)R->a;
                    if (rhs->op == OP_SUB) dd = -dd;
                    if (dd >= -2147483648ll && dd <= 2147483647ll) {
                        i32 slot = cg->in_func_depth != 0 ? cg_local_find(cg, e->a) : -1;
                        if (slot >= 0) {
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
        if (!fused && e->kind == N_ASSIGN && e->b != N_NONE) {
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
                    if (slot >= 0) {
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
                    if (dslot >= 0 && !cg->locals[dslot].is_const) {
                        i32 sslot = cg_local_find(cg, R2->a);
                        if (sslot >= 0) {
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
        if (!fused && e->kind == N_ASSIGN && e->b != N_NONE) {
            /* (3) 文レベル代入: rhs; STORE_PV の 2 命令化。rhs; DUP; STORE; POPV と効果同一。
             * const 検査は cg_store の経路と同じメッセージで compile 時に行う。 */
            cg_expr(cg, e->b);
            if (!cg->fail) {
                if (cg->in_func_depth != 0) {
                    i32 slot = cg_local_find(cg, e->a);
                    if (slot >= 0) {
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
    case N_VAR:
        /* `var t = x + y`（全ローカル）の 1 命令化。store 規約は cg_store と同一（last_val 不変、
         * decl 経路の cg_local_add を同じ引数で呼ぶので重複登録エラー等の挙動も一致） */
        if (cg->in_func_depth != 0 && n->b != N_NONE) {
            AklNode *rhs = &cg->p->nodes[n->b];
            if (rhs->kind == N_BIN && rhs->op == OP_ADD && rhs->a != N_NONE && rhs->b != N_NONE &&
                cg->p->nodes[rhs->a].kind == N_IDENT && cg->p->nodes[rhs->b].kind == N_IDENT) {
                i32 s1 = cg_local_find(cg, cg->p->nodes[rhs->a].a);
                i32 s2 = cg_local_find(cg, cg->p->nodes[rhs->b].a);
                if (s1 >= 0 && s2 >= 0) {
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
            /* 非 var 初期化は生の式ノード（parser が N_EXPRSTMT で包まない）なので式として捨てる */
            if (cg->p->nodes[n->a].kind == N_VAR) {
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
        if (!cg->n_loops) { akl_errf(cg->rt, "continue outside loop"); cg->fail = true; break; }
        if ((u32)cg->try_depth != cg->try_at_loop[cg->n_loops - 1]) {
            akl_errf(cg->rt, "continue across try boundary is unsupported in v0.1");
            cg->fail = true; break;
        }
        {
            u32 site = cg_jmp_op(cg, OP_JMP);
            u32 li = cg->n_loops - 1;
            u32 prev = cg->cont_head[li];
            memcpy(&cg->rt->code[site], &prev, 4);
            cg->cont_head[li] = site;
        }
        break;
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
            if (slot >= 0) {
                cg_op(cg, OP_RET_L);
                cg_u32(cg, (u32)slot);
                break;
            }
        }
        if (n->a != N_NONE) cg_expr(cg, n->a);
        else cg_op(cg, OP_UNDEF_T);
        cg_op(cg, OP_RET);
        break;
    case N_FUNC: {
        /* 関数エントリを作成して本体を別領域に出力し、宣言点は JMP で飛ばす */
        AklRT *rt = cg->rt;
        if (rt->n_funcs == rt->cap_funcs) {
            u32 nc = rt->cap_funcs ? rt->cap_funcs * 2 : 32;
            AklFuncEnt *nf = (AklFuncEnt *)realloc(rt->funcs, (u64)nc * sizeof(AklFuncEnt));
            if (!nf) { akl_errf(rt, "oom: funcs"); cg->fail = true; break; }
            rt->funcs = nf; rt->cap_funcs = nc;
        }
        if (n->c > 255) { akl_errf(rt, "too many parameters"); cg->fail = true; break; }
        u32 fidx = rt->n_funcs++;
        u32 site = cg_jmp_op(cg, OP_JMP); /* 本体を飛ばす */
        rt->funcs[fidx].code_off = cg_target_here(cg);
        rt->funcs[fidx].name = n->a;
        rt->funcs[fidx].n_params = (u16)n->c;
        /* 新しい codegen スコープ */
        LocalEnt *saved = cg->locals;
        u32 save_n = cg->n_locals, save_cap = cg->cap_locals;
        i32 save_fn = cg->cur_fn;
        u32 save_depth = cg->in_func_depth;
        cg->locals = NULL; cg->n_locals = 0; cg->cap_locals = 0;
        cg->cur_fn = (i32)fidx;
        cg->in_func_depth = save_depth + 1;
        for (u32 i = 0; i < n->c; i++) {
            i32 s = cg_local_add(cg, cg->p->list[n->b + i], 0);
            if (s < 0) break;
        }
        if (!cg->fail) cg_stmt(cg, n->d);
        cg_op(cg, OP_UNDEF_T);
        cg_op(cg, OP_RET);
        rt->funcs[fidx].n_locals = (u16)cg->n_locals;
        rt->funcs[fidx].code_end = cg_target_here(cg);
        free(cg->locals);
        cg->locals = saved; cg->n_locals = save_n; cg->cap_locals = save_cap;
        cg->cur_fn = save_fn;
        cg->in_func_depth = save_depth;
        if (cg->fail) break;
        cg_patch_u32(cg, site, cg_target_here(cg));
        cg_op(cg, OP_MAKEF); cg_u32(cg, fidx);
        /* 宣言 = 束縛（main はグローバル、関数内はローカル） */
        if (cg->in_func_depth == 0) {
            u32 gi = cg_global_add(rt, n->a, 0);
            if (gi == UINT32_MAX) { cg->fail = true; break; }
            cg_op(cg, OP_GSTORE); cg_u32(cg, n->a);
        } else {
            i32 s2 = cg_local_add(cg, n->a, 0);
            if (s2 < 0) break;
            cg_op(cg, OP_LSTORE); cg_u32(cg, (u32)s2);
        }
        break;
    }
    default:
        akl_errf(cg->rt, "internal: bad stmt node %u", n->kind);
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
    case OP_MAKEF:
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
        return 1;
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
        if (o->kind == AKL_OK_FUNC) return akl_mkstr(rt, (const u8 *)"function", 8);
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

/* ============================== VM ============================== */

typedef struct { u32 ret_off; u32 base; u32 func; } AklFrame;

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

static bool vm_exec(AklRT *rt, u32 entry) {
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

    /* メイン locals 窓 */
    {
        u32 nl = rt->funcs[cur].n_locals;
        AKL_GROW_TO(nl);
        for (u32 i = 0; i < nl; i++) stk[sp++] = AKL_VAL_UNDEF;
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
        else if (akl_is_objv(v)) s = akl_is_strly(rt, v) ? "string" : "function";
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
        if (!(akl_is_objv(fv) && rt->objs[akl_get_obj(fv)].kind == AKL_OK_FUNC)) {
            akl_errf(rt, "TypeError: not a function");
            free(frames);
            return false;
        }
        u32 fidx = akl_get_obj(fv);
        u32 fe_i = rt->objs[fidx].code_off; /* FUNC obj の code_off は funcs[] の index */
        if (fe_i >= rt->n_funcs) { akl_errf(rt, "internal: bad func ref"); free(frames); return false; }
        AklFuncEnt *fe = &rt->funcs[fe_i];
        if (nframes >= AKL_MAX_DEPTH) { akl_errf(rt, "call depth budget exhausted"); free(frames); return false; }
        /* 引数ウィンドウ: fn 値の 1 個分を潰して locals 窓にする */
        u32 win = sp - argc - 1;
        u32 nloc = fe->n_locals, npar = fe->n_params;
        AKL_GROW_TO(win + nloc); /* 書き込み前に確保（padding が cap を踏まないよう順序固定） */
        u32 keep = argc < npar ? argc : npar;
        /* keep 引数を win.. にずらす（src=win+1+i は win+i より後なので前方コピーで安全） */
        for (u32 i = 0; i < keep; i++) stk[win + i] = stk[win + 1 + i];
        for (u32 i = keep; i < nloc; i++) stk[win + i] = AKL_VAL_UNDEF;
        sp = win + nloc;
        frames[nframes].ret_off = (u32)(pc - code);
        frames[nframes].base = base;
        frames[nframes].func = cur;
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
        if (!nframes) { free(frames); akl_errf(rt, "internal: ret at top level"); return false; }
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
        rt->gc_sp = sp; /* obj_new の GC 発火に備えてルート深さを同期 */
        u32 oi = akl_obj_new(rt);
        if (oi == UINT32_MAX) { free(frames); return false; }
        AklObj *o = &rt->objs[oi];
        o->kind = AKL_OK_FUNC;
        o->code_off = fidx; /* FUNC obj の code_off は「func 表 index」を指す（名前の再利用） */
        AKL_PUSH(AKL_MK_OBJ(oi));
        AKL_NEXT();
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
        if (!nframes) { free(frames); akl_errf(rt, "internal: ret at top level"); return false; }
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

    AKL_L(NOP): { AKL_NEXT(); } /* CoJIT 埋め。通常到達不能、到達しても透過 */

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
    return rt;
}

void akl_free(AklRT *rt) {
    if (!rt) return;
    for (u32 i = 0; i < rt->n_objs; i++)
        if (rt->objs[i].kind == AKL_OK_STR) free(rt->objs[i].bytes);
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
    if (!src) { akl_errf(rt, "null source"); return false; }
    u64 slen = strlen(src);
    if (slen > AKL_MAX_SRC) { akl_errf(rt, "source budget exhausted"); return false; }

    P p;
    memset(&p, 0, sizeof p);
    p.rt = rt;
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
    /* パラメータなし・locals は stmt の var で追加される */
    rt->funcs[main_idx].code_off = code_from;
    rt->funcs[main_idx].name = 0;
    rt->funcs[main_idx].n_params = 0;
    cg.in_func_depth = 0;
#ifdef AKL_AST_DUMP
    akl_ast_dump(&p);
#endif
    cg_stmt(&cg, prog);
    cg_op(&cg, OP_HALT);
    rt->funcs[main_idx].n_locals = (u16)cg.n_locals;
    rt->funcs[main_idx].code_end = rt->code_len;
    f0 = &rt->funcs[main_idx];
    free(cg.locals);
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
    bool vm_ok = vm_exec(rt, main_idx);
    rt->gc_live = false;
    rt->gc_sp = 0;
    if (!vm_ok) return false;

    if (out) *out = rt->last_val;
    return true;
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
       GC 未発火でも budget 超過時のみ err を返す安全設計（内部 mkstr と同一経路） */
    u32 oi = akl_mkstr(rt, (const u8 *)s, len);
    if (oi == UINT32_MAX) return AKL_VAL_UNDEF;
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
