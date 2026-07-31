/* V8x v0.0 — Ifuto 自作 JS エンジンの実装（C11 + GCC/Clang computed-goto 拡張）。
 *
 * 構成: lexer → recursive-descent parser（AST 配列プール）→ one-pass codegen
 *       → bytecode verifier → スタックマシン VM。JIT は持たない（方針は v8x.h 冒頭）。
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
#include "v8x.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <stdarg.h>

/* ============================== NaN-box 値 ============================== */

#define V8X_TAG_MASK  0xFFFF000000000000ull
#define V8X_VAL_UNDEF (V8X_TAG_MASK | 0u)
#define V8X_VAL_NULL  (V8X_TAG_MASK | 1u)
#define V8X_VAL_FALSE (V8X_TAG_MASK | 2u)
#define V8X_VAL_TRUE  (V8X_TAG_MASK | 3u)
#define V8X_MK_INT(i) ((V8X_TAG_MASK | (1ull << 32)) | (uint32_t)(i))
#define V8X_MK_OBJ(i) ((V8X_TAG_MASK | (2ull << 32)) | (uint32_t)(i))

static bool v8x_tagged(V8xVal v) { return (v & V8X_TAG_MASK) == V8X_TAG_MASK; }
static bool v8x_is_intv(V8xVal v) {
    return (v & 0xFFFFFFFF00000000ull) == (V8X_TAG_MASK | (1ull << 32));
}
static bool v8x_is_objv(V8xVal v) {
    return (v & 0xFFFFFFFF00000000ull) == (V8X_TAG_MASK | (2ull << 32));
}
static i32 v8x_get_int(V8xVal v) { return (i32)(uint32_t)v; }
static u32 v8x_get_obj(V8xVal v) { return (uint32_t)v; }

static V8xVal v8x_from_double(double d) {
    V8xVal v;
    if (isnan(d)) return 0x7FF8000000000000ull; /* canonical NaN（タグ空間 0xFFFF 帯と非衝突） */
    memcpy(&v, &d, 8);
    /* canonical NaN 以外にタグ空間上位 0xFFFF 帯は算術結果から来ない（v8x.h 不変条件） */
    return v;
}
static V8xVal v8x_num(double d) {
    if (isnan(d)) { static const double CN = 0.0 / 0.0; (void)CN; }
    double cd = d;
    if (isnan(cd)) { /* canonical NaN へ正規化（タグ空間衝突の構造的排除） */
        V8xVal v = 0x7FF8000000000000ull;
        double out;
        memcpy(&out, &v, 8);
        cd = out;
    }
    return v8x_from_double(cd);
}
static double v8x_as_double_raw(V8xVal v) {
    double d;
    memcpy(&d, &v, 8);
    return d;
}
/* 数値として読めるか（INT or double）。obj/特殊値は false */
static bool v8x_numv(V8xVal v, double *out) {
    if (v8x_is_intv(v)) { *out = (double)v8x_get_int(v); return true; }
    if (!v8x_tagged(v)) { *out = v8x_as_double_raw(v); return true; }
    return false;
}

/* ============================== ヒープオブジェクト ============================== */

enum { V8X_OK_STR = 1, V8X_OK_FUNC = 2 };

typedef struct {
    u8 kind;
    u8 _p[3];
    u32 len;      /* STR: バイト長 */
    u8 *bytes;    /* STR: malloc 所有 */
    u32 code_off; /* FUNC */
    u32 name;     /* FUNC: 名前 STR の obj index（呼出名診断用） */
    u16 n_params; /* FUNC */
    u16 n_locals; /* FUNC */
} V8xObj;

/* ============================== runtime ============================== */

typedef struct { u32 name; V8xVal v; u8 is_const; u8 _p[3]; } V8xGlobal;
typedef struct { u32 code_off, code_end; u32 name; u16 n_params, n_locals; } V8xFuncEnt;

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
    OP_HALT,
    OP_COUNT
};

struct V8xRT {
    /* ヒープ（obj index 参照。mark-sweep GC: スイープは index 不変・穴再利用。
     * 構造的安全: コンパイル生成物（< pin_mark）はゴミになり得ない構成で、
     * 到達不能は実行時生成物のみ。根は VM スタック＋globals＋nursery の3系のみ） */
    V8xObj *objs; u32 n_objs, cap_objs;
    u64 heap_bytes; /* 生存 STR bytes 累計（GC が減算。上限は「生存分」で裁く） */
    u32 pin_mark;   /* objs[0..pin_mark): コンパイル由来。スイープ対象外 */
    u32 *free_objs; u32 n_free, cap_free; /* スイープで空いたスロット（LIFO 再利用） */
    u64 gc_next;    /* 適応 GC 閾値（bytes）。硬上限 V8X_MAX_HEAP_MB とは別に、
                     * 前回 GC 後 live ×2（下限 512KB）で発火し定常 RSS を live 漸近に抑える */
    u32 gc_next_objs; /* 同上（オブジェクト数。スロット配列の高水位を live 漸近に） */
    u32 gc_sp;      /* VM が alloc サイト直前に同期するスタック深さスナップショット */
    u32 nury[4]; u32 n_nury; /* C 側一時ルート（concat 中の to_string 一時 obj） */
    bool gc_live;   /* vm_exec 実行中のみ true（GC はこの時だけ発火） */
    /* GLOAD/GSTORE の O(1) 化: name(u32 intern id) -> global slot の脱 Salt ハッシュ
     * （globals は append-only。n_globals 変化検知でのみ全再構築） */
    u32 *ghash; u32 ghash_cap; u32 ghash_sync;
    /* コード＋関数表（eval ごとに追記。関数は以後の eval から呼べる） */
    u8 *code; u32 code_len, code_cap;
    V8xFuncEnt *funcs; u32 n_funcs, cap_funcs;
    /* グローバル（name は intern 済み STR obj index） */
    V8xGlobal *globals; u32 n_globals, cap_globals;
    /* VM 作業領域 */
    V8xVal *stk; u32 cap_stk;
    u64 insn_budget_def;
    u32 heap_mb;    /* 硬上限 = heap_mb<<20。既定 V8X_MAX_HEAP_MB。v8x_tune で組込側責任で引上げ可 */
    u32 max_objs;   /* オブジェクト数硬上限。既定 V8X_MAX_OBJECTS。同上 */
    V8xVal last_val;
    char err[256];
    /* parse 作業の再利用バッファは eval ローカルで確保（再入安全） */
};

enum {
    V8X_MAX_OBJECTS  = 100000,
    V8X_MAX_HEAP_MB  = 16,
    V8X_MAX_SRC      = 4u << 20,
    V8X_MAX_NODES    = 200000,
    V8X_MAX_DEPTH    = 256,   /* call 深度 */
    V8X_PARSE_DEPTH  = 512,
    V8X_STK_INIT     = 1024,
    V8X_STK_MAX      = 1u << 16,
    V8X_MAX_LOCALS   = 1024
};

/* ============================== 診断/確保 ============================== */

static void v8x_errf(V8xRT *rt, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rt->err, sizeof rt->err, fmt, ap);
    va_end(ap);
}

/* ---- mark-sweep GC。ルートは VM スタック(gc_sp スナップショット)＋globals＋nursery＋last_val。
 * スイープは index を動かさない（バイトコード中の CONST_STR/MAKEF オペランド = obj index を
 * 無効化しないための構造的条件）。空いたスロットは free_objs で再利用する。 */
static void v8x_gc_mark_val(V8xRT *rt, V8xVal v, u8 *mk) {
    if (v8x_is_objv(v)) {
        u32 i = v8x_get_obj(v);
        if (i >= rt->pin_mark && i < rt->n_objs) mk[i - rt->pin_mark] = 1;
    }
}
static u32 v8x_gc(V8xRT *rt) {
    if (!rt->gc_live) return 0;
    u32 span = rt->n_objs - rt->pin_mark;
    if (!span) return 0;
    u8 *mk = (u8 *)calloc(span, 1);
    if (!mk) return 0; /* OOM 時は回収不能扱い（呼び出し側が budget エラーに倒す。安全側） */
    if (rt->stk) {
        u32 lim = rt->gc_sp < rt->cap_stk ? rt->gc_sp : rt->cap_stk;
        for (u32 i = 0; i < lim; i++) v8x_gc_mark_val(rt, rt->stk[i], mk);
    }
    v8x_gc_mark_val(rt, rt->last_val, mk);
    for (u32 i = 0; i < rt->n_globals; i++) v8x_gc_mark_val(rt, rt->globals[i].v, mk);
    for (u32 i = 0; i < rt->n_nury; i++) {
        u32 oi = rt->nury[i];
        if (oi >= rt->pin_mark && oi < rt->n_objs) mk[oi - rt->pin_mark] = 1;
    }
    u32 got = 0;
    for (u32 i = rt->pin_mark; i < rt->n_objs; i++) {
        V8xObj *o = &rt->objs[i];
        if (o->kind == 0 || mk[i - rt->pin_mark]) continue;
        if (o->kind == V8X_OK_STR && o->bytes) { rt->heap_bytes -= o->len; free(o->bytes); }
        o->kind = 0; o->bytes = NULL; o->len = 0;
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
static void obj_free_rollback(V8xRT *rt, u32 idx) {
    memset(&rt->objs[idx], 0, sizeof(V8xObj));
    if (idx + 1 == rt->n_objs) rt->n_objs--;
    else if (rt->n_free < rt->cap_free) rt->free_objs[rt->n_free++] = idx;
    /* free リスト満杯なら kind==0 のまま放置（GC が再度拾う。リークはスロットのみで安全側） */
}

static u32 v8x_obj_new(V8xRT *rt) {
    if (rt->n_objs - rt->n_free >= rt->gc_next_objs) v8x_gc(rt); /* 生存オブジェクト数の適応閾値
        * （n_objs は free-list 再利用で単調にしか見えないので live = n_objs - n_free で裁う。
        *  これを誤ると freelist 充填後に毎回 GC する常勤化バグになる — 実測 9ms→265ms 退行） */
    if (rt->n_free) {
        u32 idx = rt->free_objs[--rt->n_free];
        memset(&rt->objs[idx], 0, sizeof(V8xObj));
        return idx;
    }
    if (rt->n_objs >= rt->max_objs) {
        v8x_gc(rt);
        if (rt->n_free) {
            u32 idx = rt->free_objs[--rt->n_free];
            memset(&rt->objs[idx], 0, sizeof(V8xObj));
            return idx;
        }
        v8x_errf(rt, "object budget exhausted");
        return UINT32_MAX;
    }
    if (rt->n_objs == rt->cap_objs) {
        u32 nc = rt->cap_objs ? rt->cap_objs * 2 : 64;
        V8xObj *no = (V8xObj *)realloc(rt->objs, (u64)nc * sizeof(V8xObj));
        if (!no) { v8x_errf(rt, "oom: objects"); return UINT32_MAX; }
        rt->objs = no; rt->cap_objs = nc;
    }
    V8xObj *o = &rt->objs[rt->n_objs];
    memset(o, 0, sizeof *o);
    return rt->n_objs++;
}

/* STR obj を新規作成（bytes はコピー）。失敗時 UINT32_MAX（err 設定済み） */
static u32 v8x_mkstr(V8xRT *rt, const u8 *p, u32 n) {
    if ((u64)rt->heap_bytes + n > rt->gc_next) v8x_gc(rt); /* 適応閾値（RSS 漸近抑制） */
    if ((u64)rt->heap_bytes + n > (u64)rt->heap_mb << 20) {
        v8x_gc(rt); /* 生存分だけで再判定（上限は live bytes に対して適用） */
        if ((u64)rt->heap_bytes + n > (u64)V8X_MAX_HEAP_MB << 20) {
            v8x_errf(rt, "heap bytes budget exhausted");
            return UINT32_MAX;
        }
    }
    u32 idx = v8x_obj_new(rt);
    if (idx == UINT32_MAX) return UINT32_MAX;
    u8 *cp = (u8 *)malloc(n ? n : 1);
    if (!cp) { v8x_errf(rt, "oom: string"); return UINT32_MAX; }
    if (n) memcpy(cp, p, n);
    V8xObj *o = &rt->objs[idx];
    o->kind = V8X_OK_STR; o->len = n; o->bytes = cp;
    rt->heap_bytes += n;
    return idx;
}

static const u8 *v8x_str(V8xRT *rt, u32 idx, u32 *len) {
    if (idx >= rt->n_objs || rt->objs[idx].kind != V8X_OK_STR) { *len = 0; return (const u8 *)""; }
    *len = rt->objs[idx].len;
    return rt->objs[idx].bytes;
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
       KW_N };
static const char *const V8X_KWS[KW_N] = {
    "var", "let", "const", "function", "return", "if", "else", "while",
    "for", "break", "continue", "true", "false", "null", "undefined", "typeof"
};

enum { P_LP, P_RP, P_LC, P_RC, P_SEMI, P_COMMA, P_ASSIGN, P_PLUS, P_MINUS, P_STAR,
       P_SLASH, P_PCT, P_BANG, P_LT, P_LE, P_GT, P_GE, P_EQEQ, P_NEQ, P_SEQ, P_SNE,
       P_ANDAND, P_OROR, P_N };
static const char *const V8X_PUNCTS[P_N] = {
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
            if (strlen(V8X_KWS[k]) == ln && memcmp(lx->s + st, V8X_KWS[k], ln) == 0) {
                lx->kind = TK_KW; lx->pk = (u8)k;
                return 0;
            }
        }
        lx->kind = TK_IDENT; lx->str_p = lx->s + st; lx->str_len = ln;
        return 0;
    }
    /* punct 複数文字（最長一致。テーブル順序に依存しない: "==" が "===" を潰さない） */
    int best = -1; u32 bestl = 0;
    for (int k = 0; k < P_N; k++) {
        u32 pl = (u32)strlen(V8X_PUNCTS[k]);
        if (pl >= 2 && pl > bestl && lx->pos + pl <= lx->n &&
            memcmp(lx->s + lx->pos, V8X_PUNCTS[k], pl) == 0) {
            best = k; bestl = pl;
        }
    }
    if (best >= 0) {
        lx->pos += bestl; lx->kind = TK_PUNCT; lx->pk = (u8)best;
        return 0;
    }
    for (int k = 0; k < P_N; k++) {
        if (V8X_PUNCTS[k][1] == 0 && V8X_PUNCTS[k][0] == (char)c) {
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
    N_NONE = 0xFFFFFFFFu
};

typedef struct { u8 kind; u8 op; u8 flags; u8 _p; u32 a, b, c, d; } V8xNode; /* 20B */

typedef struct {
    V8xRT *rt;
    Lex lx;
    V8xNode *nodes; u32 n_nodes, cap_nodes;
    u32 *list; u32 n_list, cap_list;     /* 引数・パラメータ・文の並び */
    u32 depth;
    const char *fail;                    /* 構文エラーの原因（短い固定文） */
} P;

static u32 p_node(P *p, u8 kind) {
    if (p->n_nodes >= V8X_MAX_NODES) { p->fail = "node budget exhausted"; return N_NONE; }
    if (p->n_nodes == p->cap_nodes) {
        u32 nc = p->cap_nodes ? p->cap_nodes * 2 : 256;
        V8xNode *nn = (V8xNode *)realloc(p->nodes, (u64)nc * sizeof(V8xNode));
        if (!nn) { p->fail = "oom: nodes"; return N_NONE; }
        p->nodes = nn; p->cap_nodes = nc;
    }
    V8xNode *n = &p->nodes[p->n_nodes];
    memset(n, 0, sizeof *n);
    n->kind = kind;
    n->a = n->b = n->c = n->d = N_NONE;
    return p->n_nodes++;
}

static u32 p_list_push(P *p, u32 v) {
    if (p->n_list >= V8X_MAX_NODES * 2) { p->fail = "list budget exhausted"; return N_NONE; }
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
    V8xRT *rt = p->rt;
    for (u32 i = 0; i < rt->n_objs; i++) {
        if (rt->objs[i].kind != V8X_OK_STR) continue;
        if (rt->objs[i].len == n && (n == 0 || memcmp(rt->objs[i].bytes, s, n) == 0))
            return i;
    }
    u32 idx = v8x_mkstr(rt, s, n);
    if (idx == UINT32_MAX) p->fail = "intern failed";
    return idx;
}

/* ---- 式（再帰下降、優先順位段ごと） ---- */
static u32 p_expr(P *p);

static u32 p_primary(P *p) {
    if (p->fail) return N_NONE;
    if (++p->depth > V8X_PARSE_DEPTH) { p->depth--; p->fail = "parse depth exhausted"; return N_NONE; }
    Lex *lx = &p->lx;
    if (lx->kind == TK_NUM) {
        u32 ni = p_node(p, N_NUM);
        if (ni != N_NONE) {
            p->nodes[ni].op = lx->num_is_int ? 1 : 0;
            if (lx->num_is_int) p->nodes[ni].a = (u32)lx->num_i;
            else {
                V8xVal bits;
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
    if (++p->depth > V8X_PARSE_DEPTH) { p->depth--; p->fail = "parse depth exhausted"; return N_NONE; }
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
    if (++p->depth > V8X_PARSE_DEPTH) { p->depth--; p->fail = "parse depth exhausted"; return N_NONE; }
    u32 ni = N_NONE;
    if (p_eat_punct(p, P_SEMI)) { p->depth--; return p_node(p, N_BLOCK); /* 空文 */ }
    if (p_is_kw(p, KW_VAR) || p_is_kw(p, KW_LET) || p_is_kw(p, KW_CONST)) {
        u8 is_const = p->lx.pk == KW_CONST;
        lex_next(&p->lx);
        for (;;) {
            if (p->lx.kind != TK_IDENT) { p->fail = "expected variable name"; goto out; }
            u32 name = p_intern(p, p->lx.str_p, p->lx.str_len);
            if (name == UINT32_MAX) goto out;
            lex_next(&p->lx);
            u32 init = N_NONE;
            if (p_eat_punct(p, P_ASSIGN)) {
                init = p_expr(p);
                if (init == N_NONE) goto out;
            } else if (is_const) { p->fail = "const declaration requires initializer"; goto out; }
            ni = p_node(p, N_VAR);
            if (ni == N_NONE) goto out;
            p->nodes[ni].flags = is_const;
            p->nodes[ni].a = name;
            p->nodes[ni].b = init;
            if (!p_eat_punct(p, P_COMMA)) break;
        }
        if (!p_eat_punct(p, P_SEMI) && p->lx.kind != TK_EOF && !p_is_punct(p, P_RC)) { p->fail = "expected ';'"; ni = N_NONE; }
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
    V8xRT *rt;
    P *p;
    LocalEnt *locals; u32 n_locals, cap_locals;
    u16 fn_slot_base;      /* この関数の frame 内ローカル数（codegen で確定） */
    i32 cur_fn;            /* codegen 中の関数 index（main=0） */
    u32 in_func_depth;     /* 0=main */
    /* loop の break/continue パッチ連鎖（pos のリストを逆方向リンク: buf[pos]=prev head） */
    u32 brk_head[64], cont_head[64], cont_kind[64]; u32 n_loops;
    bool fail;
} Cg;

static u32 cg_push_byte(Cg *cg, u8 b) {
    V8xRT *rt = cg->rt;
    if (rt->code_len == rt->code_cap) {
        u32 nc = rt->code_cap ? rt->code_cap * 2 : 1024;
        u8 *ncp = (u8 *)realloc(rt->code, nc);
        if (!ncp) { v8x_errf(rt, "oom: code"); cg->fail = true; return UINT32_MAX; }
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
        if (cg->locals[at].is_const) { v8x_errf(cg->rt, "reassignment of const binding"); cg->fail = true; return -1; }
        return at;
    }
    if (cg->n_locals >= V8X_MAX_LOCALS) { v8x_errf(cg->rt, "too many locals"); cg->fail = true; return -1; }
    if (cg->n_locals == cg->cap_locals) {
        u32 nc = cg->cap_locals ? cg->cap_locals * 2 : 32;
        LocalEnt *nl = (LocalEnt *)realloc(cg->locals, (u64)nc * sizeof(LocalEnt));
        if (!nl) { v8x_errf(cg->rt, "oom: locals"); cg->fail = true; return -1; }
        cg->locals = nl; cg->cap_locals = nc;
    }
    cg->locals[cg->n_locals].name = name;
    cg->locals[cg->n_locals].is_const = is_const;
    return (i32)cg->n_locals++;
}

/* name(u32 intern id) → global slot の O(1) 検索（脱 Salt: name は攻撃者文字列ではなく
 * エンジン内部の連番 intern id のため DoS 耐性問題なし。globals は append-only で、
 * ベイク時点とのズレは n_globals 一致で検知して全再構築） */
static bool ghash_build(V8xRT *rt) {
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
static u32 cg_global_find(V8xRT *rt, u32 name) {
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
static u32 cg_global_add(V8xRT *rt, u32 name, u8 is_const) {
    u32 at = cg_global_find(rt, name);
    if (at != UINT32_MAX) {
        if (rt->globals[at].is_const) { v8x_errf(rt, "reassignment of const binding"); return UINT32_MAX; }
        return at;
    }
    if (rt->n_globals == rt->cap_globals) {
        u32 nc = rt->cap_globals ? rt->cap_globals * 2 : 32;
        V8xGlobal *ng = (V8xGlobal *)realloc(rt->globals, (u64)nc * sizeof(V8xGlobal));
        if (!ng) { v8x_errf(rt, "oom: globals"); return UINT32_MAX; }
        rt->globals = ng; rt->cap_globals = nc;
    }
    rt->globals[rt->n_globals].name = name;
    rt->globals[rt->n_globals].is_const = is_const;
    rt->globals[rt->n_globals].v = V8X_VAL_UNDEF;
    return rt->n_globals++;
}

/* name のストア命令を出す（main では G、関数内では L 解決→見つからなければ G） */
static bool cg_store(Cg *cg, u32 name, u8 decl_const, bool decl) {
    if (cg->in_func_depth == 0) {
        u32 gi = cg_global_add(cg->rt, name, decl ? decl_const : 0);
        if (gi == UINT32_MAX) { cg->fail = true; return false; }
        V8xGlobal *g = &cg->rt->globals[gi];
        if (!decl && g->is_const) { v8x_errf(cg->rt, "assignment to const global"); cg->fail = true; return false; }
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
    if (!decl && cg->locals[slot].is_const) { v8x_errf(cg->rt, "assignment to const local"); cg->fail = true; return false; }
    cg_op(cg, OP_LSTORE);
    cg_u32(cg, (u32)slot);
    return !cg->fail;
}
static bool cg_load(Cg *cg, u32 name) {
    if (cg->in_func_depth != 0) {
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
    V8xNode *n = &cg->p->nodes[ni];
    u8 cmp = 0xFF;
    if (n->kind == N_BIN) {
        if (n->op == OP_LT) cmp = 0;
        else if (n->op == OP_LE) cmp = 1;
        else if (n->op == OP_GT) cmp = 2;
        else if (n->op == OP_GE) cmp = 3;
    }
    if (cmp != 0xFF && n->a != N_NONE && n->b != N_NONE) {
        V8xNode *L = &cg->p->nodes[n->a], *R = &cg->p->nodes[n->b];
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
    cg_expr(cg, ni);
    return cg_jmp_op(cg, OP_JMPF);
}

static void cg_expr(Cg *cg, u32 ni) {
    if (cg->fail) return;
    V8xNode *n = &cg->p->nodes[ni];
    switch (n->kind) {
    case N_NUM:
        if (n->op) {
            cg_op(cg, OP_CONST_I);
            cg_u32(cg, n->a);
        } else {
            V8xVal bits = ((V8xVal)n->b << 32) | n->a;
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
        v8x_errf(cg->rt, "internal: bad expr node %u", n->kind);
        cg->fail = true;
        break;
    }
}

static void cg_stmt(Cg *cg, u32 ni) {
    if (cg->fail) return;
    V8xNode *n = &cg->p->nodes[ni];
    switch (n->kind) {
    case N_BLOCK:
        if (n->a == N_NONE) break; /* 空文 */
        for (u32 i = 0; i < n->c; i++) cg_stmt(cg, cg->p->list[n->a + i]);
        break;
    case N_EXPRSTMT: {
        /* 文脈限定の LINC 融合: `x = x + 定数int;` / `x = x - 定数int;` を 1 命令化する。
         * 式文としての最終値破棄は VM 側が last_val を更新するので意味差なし。
         * rhs の変数が左辺と同一スロットに解決されるときのみ（別名・グローバルは対象外）。 */
        V8xNode *e = &cg->p->nodes[n->a];
        bool fused = false;
        if (e->kind == N_ASSIGN && e->b != N_NONE) {
            V8xNode *rhs = &cg->p->nodes[e->b];
            if (rhs->kind == N_BIN && (rhs->op == OP_ADD || rhs->op == OP_SUB) &&
                rhs->a != N_NONE && rhs->b != N_NONE) {
                V8xNode *L = &cg->p->nodes[rhs->a], *R = &cg->p->nodes[rhs->b];
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
        }
        if (!fused) {
            cg_expr(cg, n->a);
            if (!cg->fail) cg_op(cg, OP_POPV);
        }
        break;
    }
    case N_VAR:
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
        if (cg->n_loops >= 64) { v8x_errf(cg->rt, "loop nesting budget exhausted"); cg->fail = true; break; }
        u32 cond = cg_target_here(cg);
        u32 s_end = cg_cond_jmpf(cg, n->a);
        u32 li = cg->n_loops++;
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
        if (cg->n_loops >= 64) { v8x_errf(cg->rt, "loop nesting budget exhausted"); cg->fail = true; break; }
        if (n->a != N_NONE) cg_stmt(cg, n->a);
        u32 cond = cg_target_here(cg);
        u32 s_end = N_NONE;
        if (n->b != N_NONE) {
            s_end = cg_cond_jmpf(cg, n->b);
        }
        u32 li = cg->n_loops++;
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
        if (!cg->n_loops) { v8x_errf(cg->rt, "break outside loop"); cg->fail = true; break; }
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
        if (!cg->n_loops) { v8x_errf(cg->rt, "continue outside loop"); cg->fail = true; break; }
        {
            u32 site = cg_jmp_op(cg, OP_JMP);
            u32 li = cg->n_loops - 1;
            u32 prev = cg->cont_head[li];
            memcpy(&cg->rt->code[site], &prev, 4);
            cg->cont_head[li] = site;
        }
        break;
    case N_RET:
        if (!cg->in_func_depth) { v8x_errf(cg->rt, "return outside function"); cg->fail = true; break; }
        if (n->a != N_NONE) cg_expr(cg, n->a);
        else cg_op(cg, OP_UNDEF_T);
        cg_op(cg, OP_RET);
        break;
    case N_FUNC: {
        /* 関数エントリを作成して本体を別領域に出力し、宣言点は JMP で飛ばす */
        V8xRT *rt = cg->rt;
        if (rt->n_funcs == rt->cap_funcs) {
            u32 nc = rt->cap_funcs ? rt->cap_funcs * 2 : 32;
            V8xFuncEnt *nf = (V8xFuncEnt *)realloc(rt->funcs, (u64)nc * sizeof(V8xFuncEnt));
            if (!nf) { v8x_errf(rt, "oom: funcs"); cg->fail = true; break; }
            rt->funcs = nf; rt->cap_funcs = nc;
        }
        if (n->c > 255) { v8x_errf(rt, "too many parameters"); cg->fail = true; break; }
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
        v8x_errf(cg->rt, "internal: bad stmt node %u", n->kind);
        cg->fail = true;
        break;
    }
}

#ifdef V8X_AST_DUMP
/* -DV8X_AST_DUMP ビルド時のみ有効な診断出力（通常ビルドからは完全に消える） */
static void v8x_ast_dump(P *p) {
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

static bool v8x_verify(V8xRT *rt, u32 code_from) {
    u32 len = rt->code_len;
    if (code_from > len) { v8x_errf(rt, "verify: range"); return false; }
    u8 *ins = (u8 *)calloc(len ? len : 1, 1); /* 命令開始 bitmap */
    u32 *jt = (u32 *)malloc(((u64)len + 8) * sizeof(u32));
    u32 n_jt = 0;
    if (!ins || !jt) { free(ins); free(jt); v8x_errf(rt, "oom: verify"); return false; }
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
            if (n_f >= 256) { v8x_errf(rt, "verify: func nesting"); goto done; }
            fstk[n_f++] = (i32)fi;
            fi++;
        }
        cur = n_f ? fstk[n_f - 1] : -1;
        if (cur < 0) { v8x_errf(rt, "verify: outside function at %u", pc); goto done; }
        u32 foff = rt->funcs[cur].code_off, fend = rt->funcs[cur].code_end;
        if (pc < foff || pc >= fend) { v8x_errf(rt, "verify: region"); goto done; }
        ins[pc] = 1;
        u8 op = rt->code[pc];
        if (op >= OP_COUNT) { v8x_errf(rt, "verify: bad opcode %u at %u", op, pc); goto done; }
        pc++;
        u32 imm_len = 0;
        switch (op) {
        case OP_CONST_I: case OP_CONST_STR:
        case OP_GLOAD: case OP_GSTORE:
        case OP_MAKEF:
            imm_len = 4; break;
        case OP_LLOAD: case OP_LSTORE:
            imm_len = 4; break; /* レイアウトは u32 に統一（codegen と一致するか下で確認） */
        case OP_CONST_D: imm_len = 8; break;
        case OP_JMP: case OP_JMPF: case OP_JMPT: imm_len = 4; break;
        case OP_CALL: imm_len = 1; break;
        case OP_LINC: case OP_GINC: imm_len = 8; break;
        case OP_CJMPF_L: case OP_CJMPF_G: imm_len = 13; break;
        case OP_GLOAD_S: case OP_GSTORE_S: imm_len = 4; break;
        default: imm_len = 0; break;
        }
        if (pc + imm_len > len) { v8x_errf(rt, "verify: operand overrun at %u", pc - 1); goto done; }
        /* 内容検査 */
        if (op == OP_JMP || op == OP_JMPF || op == OP_JMPT) {
            u32 tgt;
            memcpy(&tgt, &rt->code[pc], 4);
            if (tgt >= len) { v8x_errf(rt, "verify: jump out of range %u", tgt); goto done; }
            if (n_jt) { } /* counting */
            jt[n_jt++] = tgt;
        } else if (op == OP_CJMPF_L || op == OP_CJMPF_G) {
            u32 tgt;
            memcpy(&tgt, &rt->code[pc + 9], 4);
            if (tgt >= len) { v8x_errf(rt, "verify: jump out of range %u", tgt); goto done; }
            jt[n_jt++] = tgt;
            if (op == OP_CJMPF_L) {
                u32 slot;
                memcpy(&slot, &rt->code[pc], 4);
                if (slot >= rt->funcs[cur].n_locals) {
                    v8x_errf(rt, "verify: local slot %u >= %u", slot, rt->funcs[cur].n_locals);
                    goto done;
                }
            }
        } else if (op == OP_LINC) {
            u32 slot;
            memcpy(&slot, &rt->code[pc], 4);
            if (slot >= rt->funcs[cur].n_locals) {
                v8x_errf(rt, "verify: local slot %u >= %u", slot, rt->funcs[cur].n_locals);
                goto done;
            }
        } else if (op == OP_GLOAD_S || op == OP_GSTORE_S || op == OP_GINC) {
            u32 slot;
            memcpy(&slot, &rt->code[pc], 4);
            if (slot >= rt->n_globals) {
                v8x_errf(rt, "verify: global slot %u >= %u", slot, rt->n_globals);
                goto done;
            }
        } else if (op == OP_LLOAD || op == OP_LSTORE) {
            u32 slot;
            memcpy(&slot, &rt->code[pc], 4);
            if (slot >= rt->funcs[cur].n_locals) {
                v8x_errf(rt, "verify: local slot %u >= %u", slot, rt->funcs[cur].n_locals);
                goto done;
            }
        } else if (op == OP_CONST_STR) {
            u32 idx;
            memcpy(&idx, &rt->code[pc], 4);
            if (idx >= rt->n_objs || rt->objs[idx].kind != V8X_OK_STR) {
                v8x_errf(rt, "verify: bad string ref %u", idx);
                goto done;
            }
        } else if (op == OP_MAKEF) {
            u32 fidx;
            memcpy(&fidx, &rt->code[pc], 4);
            if (fidx >= rt->n_funcs || rt->funcs[fidx].code_off >= len) {
                v8x_errf(rt, "verify: bad func ref %u", fidx);
                goto done;
            }
        } else if (op == OP_GLOAD || op == OP_GSTORE) {
            u32 name;
            memcpy(&name, &rt->code[pc], 4);
            if (name >= rt->n_objs || rt->objs[name].kind != V8X_OK_STR) {
                v8x_errf(rt, "verify: bad global name ref %u", name);
                goto done;
            }
        }
        pc += imm_len;
    }
    /* ジャンプ先が命令開始か（bitmap で） */
    for (u32 i = 0; i < n_jt; i++) {
        if (!ins[jt[i]]) { v8x_errf(rt, "verify: jump target %u not an instruction", jt[i]); goto done; }
    }
    ok = true;
done:
    free(ins);
    free(jt);
    return ok;
}

/* ============================== 数値/文字列の変換 ============================== */

static double v8x_canon(double d) {
    if (isnan(d)) {
        V8xVal bits = 0x7FF8000000000000ull;
        double out;
        memcpy(&out, &bits, 8);
        return out;
    }
    return d;
}

/* ToNumber 近似（v0.0 のプリミティブ集合で JS 整合） */
static double v8x_to_number(V8xRT *rt, V8xVal v) {
    double d;
    if (v8x_numv(v, &d)) return d;
    if (v == V8X_VAL_TRUE) return 1.0;
    if (v == V8X_VAL_FALSE) return 0.0;
    if (v == V8X_VAL_NULL) return 0.0;
    if (v == V8X_VAL_UNDEF) return v8x_canon(0.0 / 0.0);
    if (v8x_is_objv(v)) {
        V8xObj *o = &rt->objs[v8x_get_obj(v)];
        if (o->kind == V8X_OK_STR) {
            u32 i = 0;
            while (i < o->len && (o->bytes[i] == ' ' || o->bytes[i] == '\t' || o->bytes[i] == '\n' ||
                                  o->bytes[i] == '\r' || o->bytes[i] == '\f')) i++;
            u32 e = o->len;
            while (e > i && (o->bytes[e - 1] == ' ' || o->bytes[e - 1] == '\t' || o->bytes[e - 1] == '\n' ||
                             o->bytes[e - 1] == '\r' || o->bytes[e - 1] == '\f')) e--;
            if (i == e) return 0.0;
            char tmp[64];
            u32 m = e - i < 63 ? e - i : 63;
            memcpy(tmp, o->bytes + i, m);
            tmp[m] = 0;
            char *endp = NULL;
            double dv = strtod(tmp, &endp);
            if (endp == tmp || *endp != 0) return v8x_canon(0.0 / 0.0);
            return dv;
        }
    }
    return v8x_canon(0.0 / 0.0);
}

static bool v8x_truthy(V8xRT *rt, V8xVal v) {
    double d;
    if (v == V8X_VAL_UNDEF || v == V8X_VAL_NULL || v == V8X_VAL_FALSE) return false;
    if (v == V8X_VAL_TRUE) return true;
    if (v8x_is_intv(v)) return v8x_get_int(v) != 0;
    if (v8x_numv(v, &d)) return d != 0.0 && !isnan(d);
    if (v8x_is_objv(v)) {
        V8xObj *o = &rt->objs[v8x_get_obj(v)];
        if (o->kind == V8X_OK_STR) return o->len != 0;
    }
    return true;
}

/* ToString（obj index を返す。失敗時 UINT32_MAX で err 設定） */
static u32 v8x_to_string(V8xRT *rt, V8xVal v) {
    char tmp[40];
    if (v == V8X_VAL_UNDEF) return v8x_mkstr(rt, (const u8 *)"undefined", 9);
    if (v == V8X_VAL_NULL)  return v8x_mkstr(rt, (const u8 *)"null", 4);
    if (v == V8X_VAL_TRUE)  return v8x_mkstr(rt, (const u8 *)"true", 4);
    if (v == V8X_VAL_FALSE) return v8x_mkstr(rt, (const u8 *)"false", 5);
    if (v8x_is_intv(v)) {
        int n = snprintf(tmp, sizeof tmp, "%d", v8x_get_int(v));
        return v8x_mkstr(rt, (const u8 *)tmp, (u32)n);
    }
    double d;
    if (v8x_numv(v, &d)) {
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
        return v8x_mkstr(rt, (const u8 *)tmp, (u32)n);
    }
    if (v8x_is_objv(v)) {
        V8xObj *o = &rt->objs[v8x_get_obj(v)];
        if (o->kind == V8X_OK_STR) return v8x_get_obj(v);
        if (o->kind == V8X_OK_FUNC) return v8x_mkstr(rt, (const u8 *)"function", 8);
    }
    return v8x_mkstr(rt, (const u8 *)"[unknown]", 9);
}

static bool v8x_strict_eq(V8xRT *rt, V8xVal a, V8xVal b) {
    double da, db;
    bool na = v8x_numv(a, &da), nb = v8x_numv(b, &db);
    if (na && nb) return da == db;
    if (na != nb) return false;
    if (a == b) return true; /* UNDEF/NULL/bool 同値, 同一 obj idx */
    if (v8x_is_objv(a) && v8x_is_objv(b)) {
        V8xObj *oa = &rt->objs[v8x_get_obj(a)], *ob = &rt->objs[v8x_get_obj(b)];
        if (oa->kind == V8X_OK_STR && ob->kind == V8X_OK_STR)
            return oa->len == ob->len && (oa->len == 0 || memcmp(oa->bytes, ob->bytes, oa->len) == 0);
    }
    return false;
}
static bool v8x_loose_eq(V8xRT *rt, V8xVal a, V8xVal b) {
    if (v8x_strict_eq(rt, a, b)) return true;
    if ((a == V8X_VAL_NULL && b == V8X_VAL_UNDEF) || (a == V8X_VAL_UNDEF && b == V8X_VAL_NULL)) return true;
    if (v8x_is_objv(a)) {
        V8xObj *oa = &rt->objs[v8x_get_obj(a)];
        if (oa->kind == V8X_OK_STR) {
            double db;
            if (v8x_numv(b, &db)) return v8x_to_number(rt, a) == db;
            if (b == V8X_VAL_TRUE || b == V8X_VAL_FALSE) return v8x_to_number(rt, a) == (b == V8X_VAL_TRUE);
        }
    }
    if (v8x_is_objv(b)) {
        V8xObj *ob = &rt->objs[v8x_get_obj(b)];
        if (ob->kind == V8X_OK_STR) {
            double da;
            if (v8x_numv(a, &da)) return da == v8x_to_number(rt, b);
            if (a == V8X_VAL_TRUE || a == V8X_VAL_FALSE) return (a == V8X_VAL_TRUE) == v8x_to_number(rt, b);
        }
    }
    if (a == V8X_VAL_TRUE || a == V8X_VAL_FALSE) {
        double da = a == V8X_VAL_TRUE ? 1.0 : 0.0;
        return da == v8x_to_number(rt, b);
    }
    if (b == V8X_VAL_TRUE || b == V8X_VAL_FALSE) {
        double db = b == V8X_VAL_TRUE ? 1.0 : 0.0;
        return v8x_to_number(rt, a) == db;
    }
    return false;
}

/* `+` 演算の完全実装（int fast path / 文字列連結 / 数値加算）。
 * ADD 命令と LINC 非 int フォールバックで共有するため分離。
 * sp は GC ルート深さ（文字列経路のみ使用）。失敗時 false（rt->err 設定済み）。 */
static bool v8x_bin_add(V8xRT *rt, V8xVal va, V8xVal vb, u32 sp, V8xVal *out) {
    if (v8x_is_intv(va) && v8x_is_intv(vb)) {
        i64 r = (i64)v8x_get_int(va) + (i64)v8x_get_int(vb);
        if (r >= -2147483648ll && r <= 2147483647ll) { *out = V8X_MK_INT((i32)r); return true; }
        *out = v8x_num((double)r);
        return true;
    }
    bool sa = v8x_is_objv(va) && rt->objs[v8x_get_obj(va)].kind == V8X_OK_STR;
    bool sb = v8x_is_objv(vb) && rt->objs[v8x_get_obj(vb)].kind == V8X_OK_STR;
    if (sa || sb) {
        rt->gc_sp = sp; /* GC 発火点: ルート深さを同期 */
        u32 nur0 = rt->n_nury;
        u32 ia = v8x_to_string(rt, va);
        if (ia == UINT32_MAX) return false;
        if (rt->n_nury < 4) rt->nury[rt->n_nury++] = ia; /* ib 変換の GC から ia を守る */
        u32 ib = v8x_to_string(rt, vb);
        if (ib == UINT32_MAX) { rt->n_nury = nur0; return false; }
        /* ib も obj_new 前に root 化: obj_new 内の適応 GC は pa/pb の根拠を殺し得る
         * （ASan 実検出: memcpy 時点の heap-use-after-free）。順序は here-document で固定。 */
        if (ib >= rt->pin_mark && rt->n_nury < 4) rt->nury[rt->n_nury++] = ib;
        u32 la, lb;
        const u8 *pa = v8x_str(rt, ia, &la), *pb = v8x_str(rt, ib, &lb);
        if ((u64)la + lb > (u64)rt->heap_mb << 20) { rt->n_nury = nur0; v8x_errf(rt, "heap bytes budget exhausted"); return false; }
        /* 中間バッファなし: obj を先に確保して bytes に直接書き込む（1 alloc/concat） */
        u32 ic = v8x_obj_new(rt);
        if (ic == UINT32_MAX) { rt->n_nury = nur0; return false; }
        u8 *cat = (u8 *)malloc((u64)la + lb + 1);
        if (!cat) {
            obj_free_rollback(rt, ic); /* append/再利用を種別に応じて巻き戻す */
            rt->n_nury = nur0;
            v8x_errf(rt, "oom: concat");
            return false;
        }
        memcpy(cat, pa, la);
        memcpy(cat + la, pb, lb);
        /* heap 上限は live bytes で裁く（mkstr と同一規則）。
         * GC は memcpy 後・co 投入前のこの順序が安全: cat は独立所有、ic は kind==0 で
         * スイープに拾われない、to_string 一時 obj は消費済みなので回収されて良い。 */
        if (rt->heap_bytes + (u64)la + lb > rt->gc_next) v8x_gc(rt);
        if (rt->heap_bytes + (u64)la + lb > (u64)rt->heap_mb << 20) {
            v8x_gc(rt);
            if (rt->heap_bytes + (u64)la + lb > (u64)rt->heap_mb << 20) {
                obj_free_rollback(rt, ic);
                free(cat);
                rt->n_nury = nur0;
                v8x_errf(rt, "heap bytes budget exhausted");
                return false;
            }
        }
        V8xObj *co = &rt->objs[ic];
        co->kind = V8X_OK_STR; co->len = la + lb; co->bytes = cat;
        rt->heap_bytes += (u64)la + lb;
        rt->n_nury = nur0;
        *out = V8X_MK_OBJ(ic);
        return true;
    }
    *out = v8x_num(v8x_canon(v8x_to_number(rt, va) + v8x_to_number(rt, vb)));
    return true;
}

/* ============================== VM ============================== */

typedef struct { u32 ret_off; u32 base; u32 func; } V8xFrame;

/* dispatch: GCC/Clang では computed-goto（分岐予測局所化）、他は switch。
 * V8X_TEST_SWITCH_DISPATCH で強制的に switch 側をビルド（検証・bench_v8x の差分測定用）。 */
#if defined(__GNUC__) && !defined(V8X_TEST_SWITCH_DISPATCH)
#define V8X_THREADED 1
#endif

static bool vm_exec(V8xRT *rt, u32 entry) {
    if (entry >= rt->n_funcs) { v8x_errf(rt, "internal: bad entry"); return false; }
    V8xFrame *frames = (V8xFrame *)malloc((u64)V8X_MAX_DEPTH * sizeof(V8xFrame));
    if (!frames) { v8x_errf(rt, "oom: frames"); return false; }
    u32 nframes = 0;
    V8xVal *stk = rt->stk;
    u32 sp = 0;
    u32 cap = rt->cap_stk;
    u8 *code = rt->code;
    u32 cur = entry;
    const u8 *pc = code + rt->funcs[cur].code_off;
    u32 base = 0;
    bool dead = false; /* defense-in-depth: 下記 V8X_POP 下限突破で立つ（verifier 通過後は発火しない設計） */
    u64 budget = rt->insn_budget_def;

/* stk を top 要素数まで収容できるよう倍々で拡張（rt->stk と共有。V8X_STK_MAX で fail-fast） */
#define V8X_GROW_TO(top) do { \
    u32 top_ = (top); \
    while (cap < top_) { \
        if (cap >= V8X_STK_MAX) { v8x_errf(rt, "stack capacity budget exhausted"); free(frames); return false; } \
        u32 ncap_ = cap * 2 <= V8X_STK_MAX ? cap * 2 : V8X_STK_MAX; \
        V8xVal *ns_ = (V8xVal *)realloc(rt->stk, (u64)ncap_ * sizeof(V8xVal)); \
        if (!ns_) { v8x_errf(rt, "oom: stack grow"); free(frames); return false; } \
        rt->stk = stk = ns_; rt->cap_stk = cap = ncap_; \
    } } while (0)
#define V8X_PUSH(v) do { V8X_GROW_TO(sp + 1); stk[sp++] = (v); } while (0)
/* 下限突破は verifier 済みコードでは到達不能。到達したら dead を立てて次の V8X_NEXT で停止する */
#define V8X_POP() (sp > base ? stk[--sp] : (dead = true, v8x_errf(rt, "stack underflow"), V8X_VAL_UNDEF))
#define V8X_PEEK() stk[sp - 1]
#define V8X_BUDGET() do { if (!--budget) { v8x_errf(rt, "instruction budget exhausted"); free(frames); return false; } } while (0)

    /* メイン locals 窓 */
    {
        u32 nl = rt->funcs[cur].n_locals;
        V8X_GROW_TO(nl);
        for (u32 i = 0; i < nl; i++) stk[sp++] = V8X_VAL_UNDEF;
    }
/* 規則: マクロ内で V8X_NEXT を使わない（switch 側では do-while(0) がマクロ内 break を
 * 攫って case 貫通事故になる。裸ブロックにし NEXT は必ず呼び出し側の case 末で行う） */
#define V8X_BINOP_NUM(COMBINE) { \
        V8xVal vb_ = V8X_POP(); V8xVal va_ = V8X_POP(); \
        double da_ = v8x_to_number(rt, va_), db_ = v8x_to_number(rt, vb_); \
        V8X_PUSH(v8x_num(v8x_canon(COMBINE))); }

#if V8X_THREADED
    static const void *const v8x_jt[OP_COUNT] = {
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
        [OP_HALT] = &&l_HALT,
    };
#define V8X_NEXT() do { if (dead) { free(frames); return false; } V8X_BUDGET(); goto *v8x_jt[*pc++]; } while (0)
#define V8X_L(name) l_##name
    goto *v8x_jt[*pc++];
#else
/* switch 側は do-while(0) で包むと break がループを抜けるだけで case を貫通するため
 * 必ず裸ブロック＋素の break にする（-Wimplicit-fallthrough の警告もここ由来） */
#define V8X_NEXT() { if (dead) { free(frames); return false; } if (!--budget) { v8x_errf(rt, "instruction budget exhausted"); free(frames); return false; } break; }
#define V8X_L(name) case OP_##name
    for (;;) switch (*pc++) {
#endif

    V8X_L(CONST_I): {
        u32 imm;
        memcpy(&imm, pc, 4); pc += 4;
        V8X_PUSH(V8X_MK_INT((i32)imm));
        V8X_NEXT();
    }
    V8X_L(CONST_D): {
        V8xVal bits;
        memcpy(&bits, pc, 8); pc += 8;
        V8X_PUSH(bits); /* double bits はそのまま値（非タグ） */
        V8X_NEXT();
    }
    V8X_L(CONST_STR): {
        u32 idx;
        memcpy(&idx, pc, 4); pc += 4;
        V8X_PUSH(V8X_MK_OBJ(idx));
        V8X_NEXT();
    }
    V8X_L(TRUE_T):  { V8X_PUSH(V8X_VAL_TRUE);  V8X_NEXT(); }
    V8X_L(FALSE_T): { V8X_PUSH(V8X_VAL_FALSE); V8X_NEXT(); }
    V8X_L(NULL_T):  { V8X_PUSH(V8X_VAL_NULL);  V8X_NEXT(); }
    V8X_L(UNDEF_T): { V8X_PUSH(V8X_VAL_UNDEF); V8X_NEXT(); }

    V8X_L(ADD): {
        V8xVal vb = V8X_POP(), va = V8X_POP();
        V8xVal out;
        if (!v8x_bin_add(rt, va, vb, sp, &out)) { free(frames); return false; }
        V8X_PUSH(out);
        V8X_NEXT();
    }
    V8X_L(SUB): {
        V8xVal vb = V8X_POP(), va = V8X_POP();
        if (v8x_is_intv(va) && v8x_is_intv(vb)) {
            i64 r = (i64)v8x_get_int(va) - (i64)v8x_get_int(vb);
            if (r >= -2147483648ll && r <= 2147483647ll) { V8X_PUSH(V8X_MK_INT((i32)r)); V8X_NEXT(); }
            V8X_PUSH(v8x_num((double)r));
            V8X_NEXT();
        }
        V8X_PUSH(v8x_num(v8x_canon(v8x_to_number(rt, va) - v8x_to_number(rt, vb))));
        V8X_NEXT();
    }
    V8X_L(MUL): {
        V8xVal vb = V8X_POP(), va = V8X_POP();
        if (v8x_is_intv(va) && v8x_is_intv(vb)) {
            i64 r = (i64)v8x_get_int(va) * (i64)v8x_get_int(vb);
            if (r >= -2147483648ll && r <= 2147483647ll) { V8X_PUSH(V8X_MK_INT((i32)r)); V8X_NEXT(); }
            V8X_PUSH(v8x_num((double)r));
            V8X_NEXT();
        }
        V8X_PUSH(v8x_num(v8x_canon(v8x_to_number(rt, va) * v8x_to_number(rt, vb))));
        V8X_NEXT();
    }
    V8X_L(DIV): { V8X_BINOP_NUM(da_ / db_); V8X_NEXT(); }
    V8X_L(MOD): {
        V8xVal vb = V8X_POP(), va = V8X_POP();
        if (v8x_is_intv(va) && v8x_is_intv(vb)) {
            i32 ia = v8x_get_int(va), ib = v8x_get_int(vb);
            if (ib != 0 && !(ia == INT32_MIN && ib == -1)) { V8X_PUSH(V8X_MK_INT(ia % ib)); V8X_NEXT(); }
        }
        double da = v8x_to_number(rt, va), db = v8x_to_number(rt, vb);
        V8X_PUSH(v8x_num(v8x_canon(fmod(da, db))));
        V8X_NEXT();
    }
    /* 比較 4 命令は独立本体（dispatch 両モードで壊れないマクロ展開。文字列辞書式は両辺 string のみ） */
#define V8X_REL(NUMCMP, STRCMP, INTCMP) { \
        V8xVal rb_ = V8X_POP(), ra_ = V8X_POP(); \
        if (v8x_is_intv(ra_) && v8x_is_intv(rb_)) { \
            i32 ia_ = v8x_get_int(ra_), ib_ = v8x_get_int(rb_); \
            V8X_PUSH((INTCMP) ? V8X_VAL_TRUE : V8X_VAL_FALSE); \
            V8X_NEXT(); \
        } \
        bool sa_ = v8x_is_objv(ra_) && rt->objs[v8x_get_obj(ra_)].kind == V8X_OK_STR; \
        bool sb_ = v8x_is_objv(rb_) && rt->objs[v8x_get_obj(rb_)].kind == V8X_OK_STR; \
        if (sa_ && sb_) { \
            V8xObj *oa_ = &rt->objs[v8x_get_obj(ra_)], *ob_ = &rt->objs[v8x_get_obj(rb_)]; \
            u32 m_ = oa_->len < ob_->len ? oa_->len : ob_->len; \
            int cmp_ = m_ ? memcmp(oa_->bytes, ob_->bytes, m_) : 0; \
            if (!cmp_) cmp_ = oa_->len < ob_->len ? -1 : oa_->len > ob_->len ? 1 : 0; \
            V8X_PUSH((STRCMP) ? V8X_VAL_TRUE : V8X_VAL_FALSE); \
        } else { \
            double da_ = v8x_to_number(rt, ra_), db_ = v8x_to_number(rt, rb_); \
            bool r_ = !isnan(da_) && !isnan(db_) && (NUMCMP); \
            V8X_PUSH(r_ ? V8X_VAL_TRUE : V8X_VAL_FALSE); \
        } \
        V8X_NEXT(); }
    V8X_L(LT): { V8X_REL(da_ <  db_, cmp_ <  0, ia_ <  ib_); }
    V8X_L(LE): { V8X_REL(da_ <= db_, cmp_ <= 0, ia_ <= ib_); }
    V8X_L(GT): { V8X_REL(da_ >  db_, cmp_ >  0, ia_ >  ib_); }
    V8X_L(GE): { V8X_REL(da_ >= db_, cmp_ >= 0, ia_ >= ib_); }
#undef V8X_REL
    V8X_L(EQ):  { V8xVal vb = V8X_POP(), va = V8X_POP(); V8X_PUSH(v8x_loose_eq(rt, va, vb) ? V8X_VAL_TRUE : V8X_VAL_FALSE);  V8X_NEXT(); }
    V8X_L(NE):  { V8xVal vb = V8X_POP(), va = V8X_POP(); V8X_PUSH(!v8x_loose_eq(rt, va, vb) ? V8X_VAL_TRUE : V8X_VAL_FALSE); V8X_NEXT(); }
    V8X_L(SEQ): { V8xVal vb = V8X_POP(), va = V8X_POP(); V8X_PUSH(v8x_strict_eq(rt, va, vb) ? V8X_VAL_TRUE : V8X_VAL_FALSE);  V8X_NEXT(); }
    V8X_L(SNE): { V8xVal vb = V8X_POP(), va = V8X_POP(); V8X_PUSH(!v8x_strict_eq(rt, va, vb) ? V8X_VAL_TRUE : V8X_VAL_FALSE); V8X_NEXT(); }

    V8X_L(NOT): { V8xVal v = V8X_POP(); V8X_PUSH(v8x_truthy(rt, v) ? V8X_VAL_FALSE : V8X_VAL_TRUE); V8X_NEXT(); }
    V8X_L(NEG): {
        V8xVal v = V8X_POP();
        if (v8x_is_intv(v)) {
            i32 i2 = v8x_get_int(v);
            if (i2 != INT32_MIN) { V8X_PUSH(V8X_MK_INT(-i2)); V8X_NEXT(); }
        }
        V8X_PUSH(v8x_num(v8x_canon(-v8x_to_number(rt, v))));
        V8X_NEXT();
    }
    V8X_L(POS): { V8xVal v = V8X_POP(); V8X_PUSH(v8x_num(v8x_canon(v8x_to_number(rt, v)))); V8X_NEXT(); }
    V8X_L(TYPEOF): {
        V8xVal v = V8X_POP();
        const char *s;
        double d;
        if (v == V8X_VAL_UNDEF) s = "undefined";
        else if (v == V8X_VAL_NULL) s = "object";
        else if (v == V8X_VAL_TRUE || v == V8X_VAL_FALSE) s = "boolean";
        else if (v8x_numv(v, &d)) s = "number";
        else if (v8x_is_objv(v)) s = rt->objs[v8x_get_obj(v)].kind == V8X_OK_STR ? "string" : "function";
        else s = "undefined";
        rt->gc_sp = sp; /* mkstr の GC 発火に備えてルート深さを同期 */
        u32 idx = v8x_mkstr(rt, (const u8 *)s, (u32)strlen(s));
        if (idx == UINT32_MAX) { free(frames); return false; }
        V8X_PUSH(V8X_MK_OBJ(idx));
        V8X_NEXT();
    }

    V8X_L(POP):  { (void)V8X_POP(); V8X_NEXT(); }
    V8X_L(POPV): { rt->last_val = V8X_POP(); V8X_NEXT(); }
    V8X_L(DUP):  { if (sp <= base) { v8x_errf(rt, "stack underflow"); free(frames); return false; } V8xVal t_ = stk[sp - 1]; V8X_PUSH(t_); V8X_NEXT(); }

    V8X_L(LLOAD): {
        u32 slot;
        memcpy(&slot, pc, 4); pc += 4;
        if (base + slot >= sp) { v8x_errf(rt, "local OOB read"); free(frames); return false; }
        V8X_PUSH(stk[base + slot]);
        V8X_NEXT();
    }
    V8X_L(LSTORE): {
        u32 slot;
        memcpy(&slot, pc, 4); pc += 4;
        if (base + slot >= sp) { v8x_errf(rt, "local OOB write"); free(frames); return false; }
        stk[base + slot] = V8X_POP();
        V8X_NEXT();
    }
    V8X_L(GLOAD): {
        u32 name;
        memcpy(&name, pc, 4); pc += 4;
        u32 gi = cg_global_find(rt, name);
        if (gi == UINT32_MAX) {
            u32 ln;
            const u8 *np = v8x_str(rt, name, &ln);
            v8x_errf(rt, "ReferenceError: %.*s is not defined", (int)ln, np);
            free(frames);
            return false;
        }
        V8X_PUSH(rt->globals[gi].v);
        V8X_NEXT();
    }
    V8X_L(GSTORE): {
        u32 name;
        memcpy(&name, pc, 4); pc += 4;
        u32 gi = cg_global_find(rt, name);
        if (gi == UINT32_MAX) { v8x_errf(rt, "internal: global slot missing"); free(frames); return false; }
        if (rt->globals[gi].is_const) { v8x_errf(rt, "TypeError: assignment to const"); free(frames); return false; }
        rt->globals[gi].v = V8X_POP();
        V8X_NEXT();
    }
    V8X_L(JMP): {
        u32 tgt;
        memcpy(&tgt, pc, 4);
        pc = code + tgt;
        V8X_NEXT();
    }
    V8X_L(JMPF): {
        u32 tgt;
        memcpy(&tgt, pc, 4); pc += 4;
        V8xVal v = V8X_POP();
        if (!v8x_truthy(rt, v)) pc = code + tgt;
        V8X_NEXT();
    }
    V8X_L(JMPT): {
        u32 tgt;
        memcpy(&tgt, pc, 4); pc += 4;
        V8xVal v = V8X_POP();
        if (v8x_truthy(rt, v)) pc = code + tgt;
        V8X_NEXT();
    }
    V8X_L(CALL): {
        u8 argc = *pc++;
        if (argc > 250 || sp < base + argc + 1) { v8x_errf(rt, "stack underflow: call"); free(frames); return false; }
        V8xVal fv = stk[sp - argc - 1];
        if (!(v8x_is_objv(fv) && rt->objs[v8x_get_obj(fv)].kind == V8X_OK_FUNC)) {
            v8x_errf(rt, "TypeError: not a function");
            free(frames);
            return false;
        }
        u32 fidx = v8x_get_obj(fv);
        u32 fe_i = rt->objs[fidx].code_off; /* FUNC obj の code_off は funcs[] の index */
        if (fe_i >= rt->n_funcs) { v8x_errf(rt, "internal: bad func ref"); free(frames); return false; }
        V8xFuncEnt *fe = &rt->funcs[fe_i];
        if (nframes >= V8X_MAX_DEPTH) { v8x_errf(rt, "call depth budget exhausted"); free(frames); return false; }
        /* 引数ウィンドウ: fn 値の 1 個分を潰して locals 窓にする */
        u32 win = sp - argc - 1;
        u32 nloc = fe->n_locals, npar = fe->n_params;
        V8X_GROW_TO(win + nloc); /* 書き込み前に確保（padding が cap を踏まないよう順序固定） */
        u32 keep = argc < npar ? argc : npar;
        /* keep 引数を win.. にずらす（src=win+1+i は win+i より後なので前方コピーで安全） */
        for (u32 i = 0; i < keep; i++) stk[win + i] = stk[win + 1 + i];
        for (u32 i = keep; i < nloc; i++) stk[win + i] = V8X_VAL_UNDEF;
        sp = win + nloc;
        frames[nframes].ret_off = (u32)(pc - code);
        frames[nframes].base = base;
        frames[nframes].func = cur;
        nframes++;
        base = win;
        cur = fe_i;
        pc = code + rt->funcs[cur].code_off;
        V8X_NEXT();
    }
    V8X_L(RET): {
        if (sp <= base) { v8x_errf(rt, "stack underflow: ret"); free(frames); return false; }
        V8xVal v = V8X_POP();
        sp = base;
        if (!nframes) { free(frames); v8x_errf(rt, "internal: ret at top level"); return false; }
        nframes--;
        base = frames[nframes].base;
        cur = frames[nframes].func;
        pc = code + frames[nframes].ret_off;
        V8X_PUSH(v);
        V8X_NEXT();
    }
    V8X_L(MAKEF): {
        u32 fidx;
        memcpy(&fidx, pc, 4); pc += 4;
        rt->gc_sp = sp; /* obj_new の GC 発火に備えてルート深さを同期 */
        u32 oi = v8x_obj_new(rt);
        if (oi == UINT32_MAX) { free(frames); return false; }
        V8xObj *o = &rt->objs[oi];
        o->kind = V8X_OK_FUNC;
        o->code_off = fidx; /* FUNC obj の code_off は「func 表 index」を指す（名前の再利用） */
        V8X_PUSH(V8X_MK_OBJ(oi));
        V8X_NEXT();
    }
    V8X_L(LINC): {
        u32 slot;
        memcpy(&slot, pc, 4); pc += 4;
        i32 d;
        memcpy(&d, pc, 4); pc += 4;
        if (base + slot >= sp) { v8x_errf(rt, "local OOB write"); free(frames); return false; }
        V8xVal lv = stk[base + slot];
        V8xVal nv;
        if (v8x_is_intv(lv)) {
            i64 r = (i64)v8x_get_int(lv) + (i64)d;
            if (r >= -2147483648ll && r <= 2147483647ll) nv = V8X_MK_INT((i32)r);
            else nv = v8x_num((double)r);
        } else {
            /* x = x + d の汎用経路（文字列連結を含む。オペランド順序は左=x, 右=d で保持） */
            if (!v8x_bin_add(rt, lv, V8X_MK_INT(d), sp, &nv)) { free(frames); return false; }
        }
        stk[base + slot] = nv;
        rt->last_val = nv; /* 式文の POPV と同義（N_EXPRSTMT 融合との整合） */
        V8X_NEXT();
    }
    V8X_L(GLOAD_S): {
        u32 gi;
        memcpy(&gi, pc, 4); pc += 4;
        if (gi >= rt->n_globals) { v8x_errf(rt, "internal: global slot OOB"); free(frames); return false; }
        V8X_PUSH(rt->globals[gi].v);
        V8X_NEXT();
    }
    V8X_L(GSTORE_S): {
        u32 gi;
        memcpy(&gi, pc, 4); pc += 4;
        if (gi >= rt->n_globals) { v8x_errf(rt, "internal: global slot OOB"); free(frames); return false; }
        rt->globals[gi].v = V8X_POP();
        V8X_NEXT();
    }
    V8X_L(GINC): {
        u32 gi;
        memcpy(&gi, pc, 4); pc += 4;
        i32 d;
        memcpy(&d, pc, 4); pc += 4;
        if (gi >= rt->n_globals) { v8x_errf(rt, "internal: global slot OOB"); free(frames); return false; }
        V8xVal lv = rt->globals[gi].v;
        V8xVal nv;
        if (v8x_is_intv(lv)) {
            i64 r = (i64)v8x_get_int(lv) + (i64)d;
            if (r >= -2147483648ll && r <= 2147483647ll) nv = V8X_MK_INT((i32)r);
            else nv = v8x_num((double)r);
        } else {
            if (!v8x_bin_add(rt, lv, V8X_MK_INT(d), sp, &nv)) { free(frames); return false; }
        }
        rt->globals[gi].v = nv;
        rt->last_val = nv;
        V8X_NEXT();
    }
    V8X_L(CJMPF_L): {
        u32 slot;
        memcpy(&slot, pc, 4); pc += 4;
        i32 imm;
        memcpy(&imm, pc, 4); pc += 4;
        u8 cmp = *pc++;
        u32 tgt;
        memcpy(&tgt, pc, 4); pc += 4;
        if (base + slot >= sp) { v8x_errf(rt, "local OOB read"); free(frames); return false; }
        V8xVal lv = stk[base + slot];
        bool r;
        if (v8x_is_intv(lv)) {
            i32 il = v8x_get_int(lv);
            r = cmp == 0 ? il < imm : cmp == 1 ? il <= imm : cmp == 2 ? il > imm : il >= imm;
        } else {
            /* 文字列×数値は数値化（汎用 LT と同一経路。"5" < 10 等を保持）。NaN なら false */
            double dl = v8x_to_number(rt, lv), dm = (double)imm;
            r = !isnan(dl) && (cmp == 0 ? dl < dm : cmp == 1 ? dl <= dm : cmp == 2 ? dl > dm : dl >= dm);
        }
        if (!r) pc = code + tgt;
        V8X_NEXT();
    }
    V8X_L(CJMPF_G): {
        u32 gi; /* 事前解決スロット（verifier が n_globals 未満を保証） */
        memcpy(&gi, pc, 4); pc += 4;
        i32 imm;
        memcpy(&imm, pc, 4); pc += 4;
        u8 cmp = *pc++;
        u32 tgt;
        memcpy(&tgt, pc, 4); pc += 4;
        if (gi >= rt->n_globals) { v8x_errf(rt, "internal: global slot OOB"); free(frames); return false; }
        V8xVal lv = rt->globals[gi].v;
        bool r;
        if (v8x_is_intv(lv)) {
            i32 il = v8x_get_int(lv);
            r = cmp == 0 ? il < imm : cmp == 1 ? il <= imm : cmp == 2 ? il > imm : il >= imm;
        } else {
            double dl = v8x_to_number(rt, lv), dm = (double)imm;
            r = !isnan(dl) && (cmp == 0 ? dl < dm : cmp == 1 ? dl <= dm : cmp == 2 ? dl > dm : dl >= dm);
        }
        if (!r) pc = code + tgt;
        V8X_NEXT();
    }
    V8X_L(HALT): {
        free(frames);
        return true;
    }

#if !V8X_THREADED
    }
#endif
#undef V8X_GROW_TO
#undef V8X_PUSH
#undef V8X_POP
#undef V8X_PEEK
#undef V8X_BUDGET
#undef V8X_BINOP_NUM
#undef V8X_NEXT
#undef V8X_L
}

/* ============================== 公開 API ============================== */

V8xRT *v8x_new(void) {
    V8xRT *rt = (V8xRT *)calloc(1, sizeof(V8xRT));
    if (!rt) return NULL;
    rt->gc_next = 512u << 10;
    rt->gc_next_objs = 4096;
    rt->heap_mb = V8X_MAX_HEAP_MB;
    rt->max_objs = V8X_MAX_OBJECTS;
    rt->stk = (V8xVal *)malloc((u64)V8X_STK_INIT * sizeof(V8xVal));
    if (!rt->stk) { free(rt); return NULL; }
    rt->cap_stk = V8X_STK_INIT;
    if (rt->cap_stk > V8X_STK_MAX) rt->cap_stk = V8X_STK_MAX;
    rt->insn_budget_def = 10000000;
    rt->last_val = V8X_VAL_UNDEF;
    /* main 関数エントリ（entry 0。code 範囲は eval ごとの末尾まで） */
    if (v8x_obj_new(rt) == UINT32_MAX) { free(rt->stk); free(rt); return NULL; }
    /* obj0 = 予約（壊れ index 検出を容易に） */
    /* JS グローバル定数（書換不可）: NaN, Infinity */
    {
        u32 n_nan = v8x_mkstr(rt, (const u8 *)"NaN", 3);
        u32 n_inf = v8x_mkstr(rt, (const u8 *)"Infinity", 8);
        u32 g1 = n_nan == UINT32_MAX ? UINT32_MAX : cg_global_add(rt, n_nan, 1);
        u32 g2 = n_inf == UINT32_MAX ? UINT32_MAX : cg_global_add(rt, n_inf, 1);
        if (g1 == UINT32_MAX || g2 == UINT32_MAX) {
            v8x_free(rt);
            return NULL;
        }
        rt->globals[g1].v = v8x_num(0.0 / 0.0);
        rt->globals[g2].v = v8x_num(1.0 / 0.0);
    }
    return rt;
}

void v8x_free(V8xRT *rt) {
    if (!rt) return;
    for (u32 i = 0; i < rt->n_objs; i++)
        if (rt->objs[i].kind == V8X_OK_STR) free(rt->objs[i].bytes);
    free(rt->objs);
    free(rt->free_objs);
    free(rt->ghash);
    free(rt->code);
    free(rt->funcs);
    free(rt->globals);
    free(rt->stk);
    free(rt);
}

bool v8x_eval(V8xRT *rt, const char *src, V8xVal *out) {
    rt->err[0] = 0;
    if (!src) { v8x_errf(rt, "null source"); return false; }
    u64 slen = strlen(src);
    if (slen > V8X_MAX_SRC) { v8x_errf(rt, "source budget exhausted"); return false; }

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
    if (lex_next(&p.lx) < 0) { v8x_errf(rt, "lex error at line %u", p.lx.line); free(p.lx.esc); return false; }

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
        v8x_errf(rt, "SyntaxError: %s (line %u)", why, p.lx.line);
        free(p.nodes); free(p.list); free(p.lx.esc);
        return false;
    }

    /* codegen（main 関数 = entry 0 を eval ごとに新規作成…ではなく funcs[0] は
     * 「直近 eval の main」として使い回す。過去 eval のコードは funcs 参照が無い
     * ため到達不能（verify は全体を走査するが incoherent にはならない）…としたいが、
     * 過去コードの GLOAD 等は obj 表が永続なので依然 valid。検査のため verify 全体を通す） */
    V8xFuncEnt *f0;
    if (rt->n_funcs == rt->cap_funcs) {
        u32 nc = rt->cap_funcs ? rt->cap_funcs * 2 : 32;
        V8xFuncEnt *nf = (V8xFuncEnt *)realloc(rt->funcs, (u64)nc * sizeof(V8xFuncEnt));
        if (!nf) { v8x_errf(rt, "oom: funcs"); free(p.nodes); free(p.list); free(p.lx.esc); return false; }
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
#ifdef V8X_AST_DUMP
    v8x_ast_dump(&p);
#endif
    cg_stmt(&cg, prog);
    cg_op(&cg, OP_HALT);
    rt->funcs[main_idx].n_locals = (u16)cg.n_locals;
    rt->funcs[main_idx].code_end = rt->code_len;
    f0 = &rt->funcs[main_idx];
    free(cg.locals);
    free(p.nodes); free(p.list); free(p.lx.esc);
    if (cg.fail) { rt->n_funcs--; rt->code_len = code_from; return false; }

    if (!v8x_verify(rt, f0->code_off)) return false;

    rt->pin_mark = rt->n_objs; /* コンパイル由来はスイープ対象外。実行時生成物のみ集める */
    rt->gc_live = true;        /* GC は VM ルートが生きている実行中に限る */
    bool vm_ok = vm_exec(rt, main_idx);
    rt->gc_live = false;
    rt->gc_sp = 0;
    if (!vm_ok) return false;

    if (out) *out = rt->last_val;
    return true;
}

const char *v8x_error(V8xRT *rt) { return rt && rt->err[0] ? rt->err : ""; }

void v8x_set_insn_budget(V8xRT *rt, uint64_t budget) {
    if (rt) rt->insn_budget_def = budget;
}

bool v8x_as_num(V8xVal v, double *out) { return v8x_numv(v, out); }
bool v8x_as_bool(V8xVal v, bool *out) {
    if (v == V8X_VAL_TRUE) { *out = true; return true; }
    if (v == V8X_VAL_FALSE) { *out = false; return true; }
    return false;
}
bool v8x_is_null(V8xVal v) { return v == V8X_VAL_NULL; }
bool v8x_is_undefined(V8xVal v) { return v == V8X_VAL_UNDEF; }
const char *v8x_as_str(V8xRT *rt, V8xVal v, uint32_t *len) {
    if (!rt || !v8x_is_objv(v)) return NULL;
    V8xObj *o = &rt->objs[v8x_get_obj(v)];
    if (o->kind != V8X_OK_STR) return NULL;
    /* NUL 終端を API として約束するための 1 バイト余裕は mkstr で確保していない。
     * len 参照 API なので NUL は返さない（bytes は len まで有効） */
    if (len) *len = o->len;
    return (const char *)o->bytes;
}

void v8x_tune(V8xRT *rt, uint64_t insn, uint32_t heap_mb, uint32_t max_objs) {
    if (!rt) return;
    if (insn) v8x_set_insn_budget(rt, insn);
    if (heap_mb) rt->heap_mb = heap_mb;
    if (max_objs) rt->max_objs = max_objs;
}
