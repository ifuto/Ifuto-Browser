/* akl_regex.c — Ifuto 軽量正規表現エンジン
 *
 * 実装: パターンを命令列（RexIns）にコンパイルし、バックトラッキング再帰
 * VM（rx_run）でマッチする。キャプチャ復元は SPLIT 分岐時の全退避/リストア。
 *
 * 命令の分岐は 2 種:
 *   RI_SPLIT   — 次命令を優先、失敗で alt（貪欲）
 *   RI_SPLIT_L — alt を優先、失敗で次命令（非貪欲）
 * 量詞は「atom の前に分岐を挿入する」方式（rc_ins_insert）で正しくコンパイル
 * する（末尾追加だけでは a?b が "b" にマッチしない誤りになるため）。
 *
 * 対応構文:
 *   リテラル文字（UTF-8）、. ^ $ [class] [^class] [a-z]
 *   (group) (?:non-capt)  * + ? {n} {n,} {n,m}（+ 非貪欲 ?? *? +? {..}?）
 *   |  \d \D \w \W \s \S \b \B \n \t \r \f \v \0 \xHH \uHHHH \cX
 *   メタ文字エスケープ
 * 非対応（コンパイル時エラー・明白な失敗）:
 *   (?=..) (?!..) (?<=..) (?<!..) (?<name>..) \1..（バックリファレンス）
 *   文字クラス内の非 ASCII 文字・範囲、\D \W \S、\u{...}
 * フラグ: i（ASCII のみ）g m s y（u は受け付け、\u{...} のみ非対応）
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "akl_regex.h"

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t i32;

#define RX_MAX_PAT 4096u        /* パターン最大バイト長 */
#define RX_MAX_INS 65536u       /* 命令列最大長 */
#define RX_MAX_CAP 32u          /* キャプチャ最大数 */
#define RX_MAX_REPEAT 1000u     /* {n} 展開上限 */
#define RX_STEP_LIMIT 5000000u  /* 実行ステップ上限 */
#define RX_DEPTH_LIMIT 2048u    /* 再帰深さ上限 */

/* ---- 命令列 ---- */
enum {
    RI_CHAR = 0,   /* 1 バイトリテラル: u32v = byte（i フラグ時 tolower 比較） */
    RI_CHAR8,      /* マルチバイトリテラル: u64v = バイト列（下位から）、u32v = 長さ */
    RI_CLASS,      /* ビットセット: u32v = classes[] index（256 ビット = 32B） */
    RI_ANY,        /* 任意 1 コードポイント（s フラグでなければ行終端除外） */
    RI_ISSPACE,    /* 1 コードポイントが Unicode 空白か */
    RI_NOTSPACE,   /* 1 コードポイントが Unicode 空白でないか */
    RI_ANCHOR,     /* u32v: 0=^ 1=$ 2=\b 3=\B */
    RI_SAVE_BEG,   /* u32v = キャプチャ番号。cap_beg を記録して継続、失敗で復元 */
    RI_SAVE_END,   /* u32v = キャプチャ番号。cap_end を記録して継続、失敗で復元 */
    RI_SPLIT,      /* u32v = alt。次命令を優先し、失敗で alt（貪欲） */
    RI_SPLIT_L,    /* u32v = alt。alt を優先し、失敗で次命令（非貪欲） */
    RI_JMP,        /* u32v = ターゲット */
    RI_MATCH
};

typedef struct {
    u8 op;
    u8 pad[3];
    u32 u32v;
    u64 u64v;
} RexIns; /* 24B */

struct AklRex {
    u8 *pat;
    u32 pat_len;
    u32 flags;
    RexIns *ins;
    u32 n_ins;
    u8 *classes;   /* 32B × n_classes のビットセット配列 */
    u32 n_classes;
    u32 n_cap;
};

/* ---- コンパイラ状態 ---- */
typedef struct {
    const u8 *pat;
    u32 len;
    u32 flags;
    u32 pos;
    RexIns *ins;
    u32 n_ins, cap_ins;
    u8 *classes;
    u32 n_classes, cap_classes;
    u32 n_cap;
    u32 depth;
    char *err;
    u32 err_cap;
    bool failed;
} RexC;

/* ================= UTF-8 補助 ================= */

static bool rx_utf8_get(const u8 *s, u32 n, u32 pos, u32 *cp, u32 *clen) {
    if (pos >= n) return false;
    u8 c = s[pos];
    if (c < 0x80) { *cp = c; *clen = 1; return true; }
    u32 len, v, min;
    if ((c & 0xE0) == 0xC0)      { len = 2; v = c & 0x1Fu; min = 0x80u; }
    else if ((c & 0xF0) == 0xE0) { len = 3; v = c & 0x0Fu; min = 0x800u; }
    else if ((c & 0xF8) == 0xF0) { len = 4; v = c & 0x07u; min = 0x10000u; }
    else return false;
    if (pos + len > n) return false;
    for (u32 i = 1; i < len; i++) {
        if ((s[pos + i] & 0xC0) != 0x80) return false;
        v = (v << 6) | (s[pos + i] & 0x3Fu);
    }
    if (v < min || v > 0x10FFFFu || (v >= 0xD800u && v <= 0xDFFFu)) return false;
    *cp = v; *clen = len;
    return true;
}

static u32 rx_utf8_enc(u32 cp, u8 *out) {
    if (cp < 0x80) { out[0] = (u8)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (u8)(0xC0 | (cp >> 6)); out[1] = (u8)(0x80 | (cp & 0x3F)); return 2;
    }
    if (cp < 0x10000) {
        out[0] = (u8)(0xE0 | (cp >> 12)); out[1] = (u8)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (u8)(0x80 | (cp & 0x3F)); return 3;
    }
    out[0] = (u8)(0xF0 | (cp >> 18)); out[1] = (u8)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (u8)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (u8)(0x80 | (cp & 0x3F));
    return 4;
}

static u8 rx_lc(u8 c) { return (c >= 'A' && c <= 'Z') ? (u8)(c + 32) : c; }

static bool rx_is_word_byte(u8 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static bool rx_is_ws_cp(u32 cp) {
    return cp == 0x20u || cp == 0x09u || cp == 0x0Au || cp == 0x0Bu ||
           cp == 0x0Cu || cp == 0x0Du || cp == 0xA0u || cp == 0x1680u ||
           (cp >= 0x2000u && cp <= 0x200Au) || cp == 0x2028u || cp == 0x2029u ||
           cp == 0x202Fu || cp == 0x205Fu || cp == 0x3000u || cp == 0xFEFFu;
}

/* 対象文字列の位置 pos（clen バイトの文字）が行終端か */
static bool rx_is_line_term(const u8 *s, u32 n, u32 pos, u32 clen) {
    if (clen == 1) return s[pos] == 0x0A || s[pos] == 0x0D;
    if (clen == 3 && pos + 3 <= n) {
        return s[pos] == 0xE2 && s[pos + 1] == 0x80 &&
               (s[pos + 2] == 0xA8 || s[pos + 2] == 0xA9);
    }
    return false;
}

/* ================= コンパイラ ================= */

static void rc_fail(RexC *rc, const char *msg) {
    if (!rc->failed) {
        rc->failed = true;
        if (rc->err && rc->err_cap) {
            int wn = snprintf(rc->err, rc->err_cap, "invalid regexp: %s", msg);
            if (wn < 0 || (u32)wn >= rc->err_cap) rc->err[rc->err_cap - 1] = 0;
        }
    }
}

static bool rc_grow(RexC *rc, u32 need) {
    if (need <= rc->cap_ins) return true;
    u32 nc = rc->cap_ins ? rc->cap_ins * 2 : 64;
    if (nc < need) nc = need;
    RexIns *ni = (RexIns *)realloc(rc->ins, (u64)nc * sizeof(RexIns));
    if (!ni) { rc_fail(rc, "out of memory"); return false; }
    rc->ins = ni; rc->cap_ins = nc;
    return true;
}

static bool rc_emit(RexC *rc, RexIns in) {
    if (rc->n_ins >= RX_MAX_INS) { rc_fail(rc, "pattern too complex"); return false; }
    if (!rc_grow(rc, rc->n_ins + 1)) return false;
    rc->ins[rc->n_ins++] = in;
    return true;
}

static bool rc_emit_op(RexC *rc, u8 op) {
    RexIns in;
    memset(&in, 0, sizeof in);
    in.op = op;
    return rc_emit(rc, in);
}

/* 命令を pos に挿入。既存のジャンプ参照（u32v >= pos）を +1 シフト */
static bool rc_ins_insert(RexC *rc, u32 pos, RexIns in) {
    if (rc->n_ins >= RX_MAX_INS) { rc_fail(rc, "pattern too complex"); return false; }
    if (!rc_grow(rc, rc->n_ins + 1)) return false;
    memmove(&rc->ins[pos + 1], &rc->ins[pos], (u64)(rc->n_ins - pos) * sizeof(RexIns));
    rc->ins[pos] = in;
    rc->n_ins++;
    /* 参照シフト: 挿入位置より「後ろ」を指す参照は +1（挿入により 1 命令ずれる）。
     * 挿入位置「ちょうど pos」を指す参照はシフトしない — コンパイラは
     * 「これから挿入される命令」を指す意図で pos を設定する（例: 量詞の
     * スキップ先 = 次に置かれる SPLIT の位置）。 */
    for (u32 i = 0; i < rc->n_ins; i++) {
        if (i == pos) continue;
        u8 op = rc->ins[i].op;
        if ((op == RI_JMP || op == RI_SPLIT || op == RI_SPLIT_L) &&
            rc->ins[i].u32v > pos) rc->ins[i].u32v++;
    }
    return true;
}

/* ビットセット（256 ビット = 32B）を取得。既存を探し、無ければ追加 */
static u32 rc_class_get(RexC *rc, const u8 *bits) {
    for (u32 i = 0; i < rc->n_classes; i++)
        if (memcmp(rc->classes + i * 32, bits, 32) == 0) return i;
    if (rc->n_classes == rc->cap_classes) {
        u32 nc = rc->cap_classes ? rc->cap_classes * 2 : 8;
        u8 *nb = (u8 *)realloc(rc->classes, (u64)nc * 32);
        if (!nb) { rc_fail(rc, "out of memory"); return 0; }
        rc->classes = nb; rc->cap_classes = nc;
    }
    memcpy(rc->classes + rc->n_classes * 32, bits, 32);
    return rc->n_classes++;
}

static void bits_set(u8 *bits, u8 c) { bits[c >> 3] |= (u8)(1u << (c & 7)); }
static void bits_clr_all(u8 *bits) { memset(bits, 0, 32); }
static void bits_neg(u8 *bits) { for (u32 i = 0; i < 32; i++) bits[i] = (u8)~bits[i]; }
static bool bits_has(const u8 *bits, u8 c) { return (bits[c >> 3] >> (c & 7)) & 1u; }

static void bits_add_digit(u8 *bits) { for (u8 c = '0'; c <= '9'; c++) bits_set(bits, c); }
static void bits_add_word(u8 *bits) {
    for (u8 c = 'a'; c <= 'z'; c++) bits_set(bits, c);
    for (u8 c = 'A'; c <= 'Z'; c++) bits_set(bits, c);
    for (u8 c = '0'; c <= '9'; c++) bits_set(bits, c);
    bits_set(bits, '_');
}
static void bits_add_ascii_space(u8 *bits) {
    bits_set(bits, 0x20); bits_set(bits, 0x09); bits_set(bits, 0x0A);
    bits_set(bits, 0x0D); bits_set(bits, 0x0C); bits_set(bits, 0x0B);
}

/* i フラグ時: セットに大文字/小文字の両方を立てる */
static void bits_ci_expand(u8 *bits) {
    u8 tmp[32];
    memcpy(tmp, bits, 32);
    for (u32 c = 0; c < 256; c++) {
        if (bits_has(tmp, (u8)c)) {
            if (c >= 'A' && c <= 'Z') bits_set(bits, (u8)(c + 32));
            else if (c >= 'a' && c <= 'z') bits_set(bits, (u8)(c - 32));
        }
    }
}

/* エスケープ \x を 1 つ処理。
 * kind: ESC_CHAR（cp にコードポイント）/ ESC_CLASS（cp = クラス index, neg）
 *      / ESC_SPACE（\s \S。neg で判定）/ ESC_ANCHOR（cp = アンカー種別） */
enum { ESC_CHAR = 0, ESC_CLASS, ESC_SPACE, ESC_ANCHOR };

static bool rc_escape(RexC *rc, u32 *cp, int *kind, bool *neg) {
    rc->pos++; /* '\' を越える */
    if (rc->pos >= rc->len) { rc_fail(rc, "trailing backslash"); return false; }
    u8 c = rc->pat[rc->pos];
    switch (c) {
    case 'd': case 'D': {
        u8 bits[32]; bits_clr_all(bits); bits_add_digit(bits);
        if (c == 'D') bits_neg(bits);
        if (rc->flags & AKL_RX_F_IGNORE) bits_ci_expand(bits);
        u32 ci = rc_class_get(rc, bits);
        rc->pos++;
        *cp = ci; *kind = ESC_CLASS; *neg = false;
        return true;
    }
    case 'w': case 'W': {
        u8 bits[32]; bits_clr_all(bits); bits_add_word(bits);
        if (c == 'W') bits_neg(bits);
        if (rc->flags & AKL_RX_F_IGNORE) bits_ci_expand(bits);
        u32 ci = rc_class_get(rc, bits);
        rc->pos++;
        *cp = ci; *kind = ESC_CLASS; *neg = false;
        return true;
    }
    case 's': case 'S':
        rc->pos++;
        *cp = 0; *kind = ESC_SPACE; *neg = (c == 'S');
        return true;
    case 'b': case 'B':
        rc->pos++;
        *cp = (c == 'b') ? 2u : 3u; *kind = ESC_ANCHOR; *neg = false;
        return true;
    case 'n': rc->pos++; *cp = 0x0A; *kind = ESC_CHAR; *neg = false; return true;
    case 't': rc->pos++; *cp = 0x09; *kind = ESC_CHAR; *neg = false; return true;
    case 'r': rc->pos++; *cp = 0x0D; *kind = ESC_CHAR; *neg = false; return true;
    case 'f': rc->pos++; *cp = 0x0C; *kind = ESC_CHAR; *neg = false; return true;
    case 'v': rc->pos++; *cp = 0x0B; *kind = ESC_CHAR; *neg = false; return true;
    case '0': {
        if (rc->pos + 1 < rc->len && rc->pat[rc->pos + 1] >= '0' &&
            rc->pat[rc->pos + 1] <= '9') {
            rc_fail(rc, "octal escape not supported");
            return false;
        }
        rc->pos++; *cp = 0; *kind = ESC_CHAR; *neg = false;
        return true;
    }
    case 'x': {
        u32 v = 0;
        for (u32 k = 0; k < 2; k++) {
            if (rc->pos + 1 >= rc->len) { rc_fail(rc, "bad \\x escape"); return false; }
            u8 h = rc->pat[rc->pos + 1];
            int d = (h >= '0' && h <= '9') ? h - '0'
                  : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                  : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
            if (d < 0) { rc_fail(rc, "bad \\x escape"); return false; }
            v = v * 16 + (u32)d;
            rc->pos++;
        }
        rc->pos++;
        *cp = v; *kind = ESC_CHAR; *neg = false;
        return true;
    }
    case 'u': {
        if (rc->pos + 1 < rc->len && rc->pat[rc->pos + 1] == '{') {
            rc_fail(rc, "\\u{...} not supported");
            return false;
        }
        u32 v = 0;
        for (u32 k = 0; k < 4; k++) {
            if (rc->pos + 1 >= rc->len) { rc_fail(rc, "bad \\u escape"); return false; }
            u8 h = rc->pat[rc->pos + 1];
            int d = (h >= '0' && h <= '9') ? h - '0'
                  : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                  : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
            if (d < 0) { rc_fail(rc, "bad \\u escape"); return false; }
            v = v * 16 + (u32)d;
            rc->pos++;
        }
        rc->pos++;
        *cp = v; *kind = ESC_CHAR; *neg = false;
        return true;
    }
    case 'c': {
        if (rc->pos + 1 >= rc->len) { rc_fail(rc, "bad \\c escape"); return false; }
        u8 l = rx_lc(rc->pat[rc->pos + 1]);
        if (l < 'a' || l > 'z') { rc_fail(rc, "bad \\c escape"); return false; }
        *cp = (u32)(rc->pat[rc->pos + 1] & 0x1F);
        rc->pos += 2;
        *kind = ESC_CHAR; *neg = false;
        return true;
    }
    default:
        if (c >= '1' && c <= '9') {
            rc_fail(rc, "backreference not supported");
            return false;
        }
        rc->pos++;
        *cp = c; *kind = ESC_CHAR; *neg = false;
        return true;
    }
}

static bool rc_emit_char(RexC *rc, u32 cp) {
    if (cp < 0x80) {
        RexIns in;
        memset(&in, 0, sizeof in);
        in.op = RI_CHAR; in.u32v = cp;
        return rc_emit(rc, in);
    }
    u8 enc[4];
    u32 el = rx_utf8_enc(cp, enc);
    RexIns in;
    memset(&in, 0, sizeof in);
    in.op = RI_CHAR8;
    u64 v = 0;
    for (u32 i = 0; i < el; i++) v |= (u64)enc[i] << (i * 8);
    in.u64v = v;
    in.u32v = el;
    return rc_emit(rc, in);
}

/* 量詞 {n,m} を読む。読めたら 0、リテラル '{' 扱いなら -1、壊れは -2 */
static int rc_quant_parse(RexC *rc, u32 *lo, u32 *hi, bool *has_hi) {
    u32 save = rc->pos;
    rc->pos++;
    u32 a = 0, b = 0;
    bool digit = false;
    while (rc->pos < rc->len && rc->pat[rc->pos] >= '0' && rc->pat[rc->pos] <= '9') {
        if (a > 100000000) return -2;
        a = a * 10 + (u32)(rc->pat[rc->pos] - '0');
        rc->pos++; digit = true;
    }
    if (!digit) { rc->pos = save; return -1; }
    if (rc->pos < rc->len && rc->pat[rc->pos] == ',') {
        rc->pos++;
        bool bdigit = false;
        while (rc->pos < rc->len && rc->pat[rc->pos] >= '0' && rc->pat[rc->pos] <= '9') {
            if (b > 100000000) return -2;
            b = b * 10 + (u32)(rc->pat[rc->pos] - '0');
            rc->pos++; bdigit = true;
        }
        *has_hi = bdigit;
    } else {
        *has_hi = true; b = a;
    }
    if (rc->pos >= rc->len || rc->pat[rc->pos] != '}') { rc->pos = save; return -1; }
    rc->pos++;
    if (*has_hi && b < a) return -2;
    *lo = a; *hi = b;
    return 0;
}

static bool rc_compile_alt(RexC *rc);

/* アトム 1 つをコンパイル。 */
static bool rc_compile_atom(RexC *rc, u32 *s0, u32 *e0) {
    if (rc->failed) return false;
    if (++rc->depth > RX_DEPTH_LIMIT) { rc_fail(rc, "pattern too deeply nested"); return false; }
    *s0 = *e0 = rc->n_ins;
    if (rc->pos >= rc->len) {
        rc_fail(rc, "unexpected end of pattern");
        rc->depth--;
        return false;
    }
    u8 c = rc->pat[rc->pos];
    if (c == '(') {
        rc->pos++;
        bool cap = true;
        if (rc->pos + 1 < rc->len && rc->pat[rc->pos] == '?') {
            if (rc->pat[rc->pos + 1] == ':') {
                cap = false;
                rc->pos += 2;
            } else if (rc->pat[rc->pos + 1] == '=' || rc->pat[rc->pos + 1] == '!' ||
                       rc->pat[rc->pos + 1] == '<') {
                rc_fail(rc, "lookahead/lookbehind not supported");
                rc->depth--;
                return false;
            } else {
                rc_fail(rc, "unknown group syntax");
                rc->depth--;
                return false;
            }
        }
        u32 grp = 0;
        if (cap) {
            if (rc->n_cap >= RX_MAX_CAP) {
                rc_fail(rc, "too many capture groups");
                rc->depth--;
                return false;
            }
            grp = ++rc->n_cap;
            RexIns in;
            memset(&in, 0, sizeof in);
            in.op = RI_SAVE_BEG; in.u32v = grp;
            if (!rc_emit(rc, in)) { rc->depth--; return false; }
        }
        if (!rc_compile_alt(rc)) { rc->depth--; return false; }
        if (rc->pos >= rc->len || rc->pat[rc->pos] != ')') {
            rc_fail(rc, "missing ')'");
            rc->depth--;
            return false;
        }
        rc->pos++;
        if (cap) {
            RexIns in;
            memset(&in, 0, sizeof in);
            in.op = RI_SAVE_END; in.u32v = grp;
            if (!rc_emit(rc, in)) { rc->depth--; return false; }
        }
        rc->depth--; *e0 = rc->n_ins;
        return true;
    }
    if (c == '[') {
        rc->pos++;
        bool neg = false;
        if (rc->pos < rc->len && rc->pat[rc->pos] == '^') { neg = true; rc->pos++; }
        u8 bits[32];
        bits_clr_all(bits);
        bool first = true;
        u32 prev_lo = 0;
        bool in_range = false;
        for (;;) {
            if (rc->pos >= rc->len) { rc_fail(rc, "missing ']'"); rc->depth--; return false; }
            u8 cc = rc->pat[rc->pos];
            if (cc == ']' && !first) { rc->pos++; break; }
            first = false;
            u32 cp = 0;
            if (cc == '\\') {
                u32 esc_cp;
                int kind;
                bool eneg;
                if (!rc_escape(rc, &esc_cp, &kind, &eneg)) { rc->depth--; return false; }
                if (kind == ESC_CLASS) {
                    if (eneg) {
                        rc_fail(rc, "\\D \\W inside class not supported");
                        rc->depth--; return false;
                    }
                    const u8 *cb = rc->classes + esc_cp * 32;
                    for (u32 b = 0; b < 32; b++) bits[b] |= cb[b];
                    continue;
                }
                if (kind == ESC_SPACE) {
                    if (eneg) {
                        rc_fail(rc, "\\S inside class not supported");
                        rc->depth--; return false;
                    }
                    bits_add_ascii_space(bits);
                    continue;
                }
                if (kind == ESC_ANCHOR) {
                    rc_fail(rc, "\\b inside class not supported");
                    rc->depth--; return false;
                }
                cp = esc_cp;
            } else {
                u32 clen;
                if (cc >= 0x80) {
                    u32 pcp;
                    if (!rx_utf8_get(rc->pat, rc->len, rc->pos, &pcp, &clen) || pcp >= 0x80) {
                        rc_fail(rc, "non-ASCII char in class not supported");
                        rc->depth--; return false;
                    }
                    cp = pcp;
                } else {
                    clen = 1;
                    cp = cc;
                }
                for (u32 i = 0; i < clen; i++) rc->pos++;
            }
            if (in_range) {
                if (prev_lo > cp || prev_lo >= 0x80 || cp >= 0x80) {
                    rc_fail(rc, "bad range in class");
                    rc->depth--; return false;
                }
                for (u32 v = prev_lo; v <= cp; v++) bits_set(bits, (u8)v);
                in_range = false;
            } else if (cp < 0x80) {
                if (rc->pos + 1 < rc->len && rc->pat[rc->pos] == '-' &&
                    rc->pat[rc->pos + 1] != ']') {
                    rc->pos++;
                    prev_lo = cp;
                    in_range = true;
                } else {
                    bits_set(bits, (u8)cp);
                }
            } else {
                rc_fail(rc, "non-ASCII char in class not supported");
                rc->depth--; return false;
            }
        }
        if (in_range) { rc_fail(rc, "bad range in class"); rc->depth--; return false; }
        if (neg) bits_neg(bits);
        if (rc->flags & AKL_RX_F_IGNORE) bits_ci_expand(bits);
        u32 ci = rc_class_get(rc, bits);
        RexIns in;
        memset(&in, 0, sizeof in);
        in.op = RI_CLASS; in.u32v = ci;
        if (!rc_emit(rc, in)) { rc->depth--; return false; }
        rc->depth--; *e0 = rc->n_ins;
        return true;
    }
    if (c == '.') {
        rc->pos++;
        if (!rc_emit_op(rc, RI_ANY)) { rc->depth--; return false; }
        rc->depth--; *e0 = rc->n_ins;
        return true;
    }
    if (c == '^' || c == '$') {
        rc->pos++;
        RexIns in;
        memset(&in, 0, sizeof in);
        in.op = RI_ANCHOR; in.u32v = (c == '^') ? 0u : 1u;
        if (!rc_emit(rc, in)) { rc->depth--; return false; }
        rc->depth--; *e0 = rc->n_ins;
        return true;
    }
    if (c == '\\') {
        u32 cp;
        int kind;
        bool neg;
        if (!rc_escape(rc, &cp, &kind, &neg)) { rc->depth--; return false; }
        if (kind == ESC_CLASS) {
            RexIns in;
            memset(&in, 0, sizeof in);
            in.op = RI_CLASS; in.u32v = cp;
            if (!rc_emit(rc, in)) { rc->depth--; return false; }
            rc->depth--; *e0 = rc->n_ins;
            return true;
        }
        if (kind == ESC_SPACE) {
            if (!rc_emit_op(rc, neg ? RI_NOTSPACE : RI_ISSPACE)) { rc->depth--; return false; }
            rc->depth--; *e0 = rc->n_ins;
            return true;
        }
        if (kind == ESC_ANCHOR) {
            RexIns in;
            memset(&in, 0, sizeof in);
            in.op = RI_ANCHOR; in.u32v = cp;
            if (!rc_emit(rc, in)) { rc->depth--; return false; }
            rc->depth--; *e0 = rc->n_ins;
            return true;
        }
        if (!rc_emit_char(rc, cp)) { rc->depth--; return false; }
        rc->depth--; *e0 = rc->n_ins;
        return true;
    }
    if (c == ')' || c == '|') {
        rc_fail(rc, "unexpected ')' or '|'");
        rc->depth--;
        return false;
    }
    if (c == '*' || c == '+' || c == '?') {
        rc_fail(rc, "nothing to repeat");
        rc->depth--;
        return false;
    }
    if (c == '{') {
        /* アトム先頭の { は常にリテラル（JS 非 u モードと同じ扱い） */
        rc->pos++;
        if (!rc_emit_char(rc, '{')) { rc->depth--; return false; }
        rc->depth--; *e0 = rc->n_ins;
        return true;
    }
    /* 通常文字（UTF-8） */
    {
        u32 cp, clen;
        if (!rx_utf8_get(rc->pat, rc->len, rc->pos, &cp, &clen)) {
            rc_fail(rc, "bad utf-8 in pattern");
            rc->depth--;
            return false;
        }
        for (u32 i = 0; i < clen; i++) rc->pos++;
        if (!rc_emit_char(rc, cp)) { rc->depth--; return false; }
    }
    rc->depth--; *e0 = rc->n_ins;
    return true;
}

/* コピーされた命令列（copy_start..末尾）のキャプチャ番号を、元の番号にマップする。
 * コピーは rc->n_cap = 0 でコンパイルされるため、コピー内のグループ番号は
 * 1..grp_n の相対番号になっている。これを元のパターンでの番号
 * （grp_before + 1 .. grp_before + grp_n）に変換する。 */
static void rc_remap_grp(RexC *rc, u32 copy_start, u32 grp_before, u32 grp_n) {
    for (u32 k = copy_start; k < rc->n_ins; k++) {
        u8 op = rc->ins[k].op;
        if (op == RI_SAVE_BEG || op == RI_SAVE_END) {
            u32 g = rc->ins[k].u32v;
            if (g >= 1 && g <= grp_n) rc->ins[k].u32v = grp_before + g;
        }
    }
}

/* 連結（0 個以上の atom + 量詞） */
static bool rc_compile_seq(RexC *rc) {
    for (;;) {
        if (rc->failed) return false;
        if (rc->pos >= rc->len) return true;
        u8 c = rc->pat[rc->pos];
        if (c == '|' || c == ')') return true;
        u32 pat_pos_atom = rc->pos;
        u32 grp_before = rc->n_cap;
        u32 s0, e0;
        if (!rc_compile_atom(rc, &s0, &e0)) return false;
        u32 grp_n = rc->n_cap - grp_before; /* この atom 内で増えたキャプチャ数 */
        /* 量詞を読む */
        bool lazy = false;
        bool has_q = false;
        u32 lo = 0, hi = 0;
        bool has_hi = false;
        char q = 0;
        if (rc->pos < rc->len) {
            u8 qc = rc->pat[rc->pos];
            if (qc == '*' || qc == '+' || qc == '?') {
                has_q = true; q = (char)qc; rc->pos++;
                if (rc->pos < rc->len && rc->pat[rc->pos] == '?') { lazy = true; rc->pos++; }
            } else if (qc == '{') {
                int r = rc_quant_parse(rc, &lo, &hi, &has_hi);
                if (r == 0) {
                    has_q = true; q = '{';
                    if (rc->pos < rc->len && rc->pat[rc->pos] == '?') { lazy = true; rc->pos++; }
                } else if (r == -2) {
                    rc_fail(rc, "bad repetition range");
                    return false;
                }
            }
        }
        if (!has_q) continue;
        /* ===== 量詞の展開 ===== */
        /* 各量詞は「atom の前」に分岐命令を挿入して正しくコンパイルする。
         * 貪欲 = RI_SPLIT（次命令優先）、非貪欲 = RI_SPLIT_L（alt 優先）。 */
        if (q == '*') {
            /* 貪欲:  [SPLIT(alt=end)] / atom / JMP(L)
             * 非貪欲: [SPLIT_L(alt=end)] / atom / JMP(L)  （alt=end 優先 = スキップ優先） */
            if (lazy) {
                RexIns sp;
                memset(&sp, 0, sizeof sp);
                sp.op = RI_SPLIT_L;
                if (!rc_ins_insert(rc, s0, sp)) return false;
                RexIns jm;
                memset(&jm, 0, sizeof jm);
                jm.op = RI_JMP; jm.u32v = s0;
                if (!rc_emit(rc, jm)) return false;
                rc->ins[s0].u32v = rc->n_ins;   /* alt = end */
            } else {
                RexIns sp;
                memset(&sp, 0, sizeof sp);
                sp.op = RI_SPLIT;
                if (!rc_ins_insert(rc, s0, sp)) return false;
                RexIns jm;
                memset(&jm, 0, sizeof jm);
                jm.op = RI_JMP; jm.u32v = s0;
                if (!rc_emit(rc, jm)) return false;
                rc->ins[s0].u32v = rc->n_ins;   /* alt = end */
            }
            continue;
        }
        if (q == '+') {
            /* atom / SPLIT(alt=end) / JMP(atom)  （非貪欲: atom / SPLIT_L(alt=atom)） */
            if (lazy) {
                RexIns sp;
                memset(&sp, 0, sizeof sp);
                sp.op = RI_SPLIT_L; sp.u32v = s0; /* alt = 再実行が後回し = スキップ優先 */
                if (!rc_emit(rc, sp)) return false;
            } else {
                u32 sp_pos = rc->n_ins;
                RexIns sp;
                memset(&sp, 0, sizeof sp);
                sp.op = RI_SPLIT;
                if (!rc_emit(rc, sp)) return false;
                RexIns jm;
                memset(&jm, 0, sizeof jm);
                jm.op = RI_JMP; jm.u32v = s0;
                if (!rc_emit(rc, jm)) return false;
                rc->ins[sp_pos].u32v = rc->n_ins; /* alt = end */
            }
            continue;
        }
        if (q == '?') {
            /* SPLIT(alt=スキップ) / atom  （非貪欲: SPLIT_L(alt=スキップ) / atom） */
            RexIns sp;
            memset(&sp, 0, sizeof sp);
            sp.op = lazy ? RI_SPLIT_L : RI_SPLIT;
            if (!rc_ins_insert(rc, s0, sp)) return false;
            rc->ins[s0].u32v = e0 + 1; /* 挿入後は atom が 1 命令ずれる */
            continue;
        }
        /* {n} {n,} {n,m}: 元の atom（s0..e0）を 1 回分として使い、
         * 不足分をパターン位置 pat_pos_atom から再コンパイルして追加する。 */
        if (lo > RX_MAX_REPEAT || (has_hi && hi > RX_MAX_REPEAT)) {
            rc_fail(rc, "repetition count too large");
            return false;
        }
        u32 save_pos = rc->pos;
        u32 last_s = s0;
        /* 追加必須コピー: (lo - 1) 回（元の 1 回と合わせて lo 回） */
        for (u32 i = 1; i < lo; i++) {
            rc->pos = pat_pos_atom;
            u32 copy_start = rc->n_ins;
            u32 cap_copy0 = rc->n_cap;
            rc->n_cap = 0; /* 相対番号で割り当て */
            u32 s2, e2;
            if (!rc_compile_atom(rc, &s2, &e2)) { rc->pos = save_pos; return false; }
            rc->pos = save_pos;
            rc_remap_grp(rc, copy_start, grp_before, grp_n);
            rc->n_cap = cap_copy0;
            last_s = s2;
        }
        if (!has_hi) {
            /* {n,} */
            if (lo == 0) {
                /* {0,} = * と同じ構造（元の atom をそのまま使う） */
                if (lazy) {
                    RexIns sp;
                    memset(&sp, 0, sizeof sp);
                    sp.op = RI_SPLIT_L;
                    if (!rc_ins_insert(rc, s0, sp)) return false;
                    RexIns jm;
                    memset(&jm, 0, sizeof jm);
                    jm.op = RI_JMP; jm.u32v = s0;
                    if (!rc_emit(rc, jm)) return false;
                    rc->ins[s0].u32v = rc->n_ins;
                } else {
                    RexIns sp;
                    memset(&sp, 0, sizeof sp);
                    sp.op = RI_SPLIT;
                    if (!rc_ins_insert(rc, s0, sp)) return false;
                    RexIns jm;
                    memset(&jm, 0, sizeof jm);
                    jm.op = RI_JMP; jm.u32v = s0;
                    if (!rc_emit(rc, jm)) return false;
                    rc->ins[s0].u32v = rc->n_ins;
                }
            } else {
                /* {n,} = atom×n + SPLIT ループ（+ と同じ） */
                if (lazy) {
                    RexIns sp;
                    memset(&sp, 0, sizeof sp);
                    sp.op = RI_SPLIT_L; sp.u32v = last_s;
                    if (!rc_emit(rc, sp)) return false;
                } else {
                    u32 sp_pos = rc->n_ins;
                    RexIns sp;
                    memset(&sp, 0, sizeof sp);
                    sp.op = RI_SPLIT;
                    if (!rc_emit(rc, sp)) return false;
                    RexIns jm;
                    memset(&jm, 0, sizeof jm);
                    jm.op = RI_JMP; jm.u32v = last_s;
                    if (!rc_emit(rc, jm)) return false;
                    rc->ins[sp_pos].u32v = rc->n_ins;
                }
            }
            continue;
        }
        /* {n,m} */
        if (lo == 0) {
            /* {0,m}: 元の atom を最初のオプショナルとして使う */
            RexIns sp;
            memset(&sp, 0, sizeof sp);
            sp.op = lazy ? RI_SPLIT_L : RI_SPLIT;
            if (!rc_ins_insert(rc, s0, sp)) return false;
            rc->ins[s0].u32v = e0 + 1;
            last_s = s0 + 1; /* 挿入後は atom 開始が +1 */
        }
        for (u32 i = lo; i < hi; i++) {
            if (i == 0) continue; /* 元の atom は 1 個目のオプショナルとして使用済み */
            rc->pos = pat_pos_atom;
            u32 copy_start = rc->n_ins;
            u32 cap_copy0 = rc->n_cap;
            rc->n_cap = 0; /* 相対番号で割り当て */
            u32 s2, e2;
            if (!rc_compile_atom(rc, &s2, &e2)) { rc->pos = save_pos; return false; }
            rc->pos = save_pos;
            rc_remap_grp(rc, copy_start, grp_before, grp_n);
            rc->n_cap = cap_copy0;
            RexIns sp;
            memset(&sp, 0, sizeof sp);
            sp.op = lazy ? RI_SPLIT_L : RI_SPLIT;
            if (!rc_ins_insert(rc, s2, sp)) return false;
            rc->ins[s2].u32v = e2 + 1;
        }
        if (lo == 0 && hi == 0) {
            /* {0,0}: atom をスキップ（常に空マッチ） */
            RexIns jm;
            memset(&jm, 0, sizeof jm);
            jm.op = RI_JMP; jm.u32v = e0 + 1;
            if (!rc_ins_insert(rc, s0, jm)) return false;
        }
    }
}

/* 選択（| 区切り）。
 * 構造:
 *   [SPLIT(alt=S1)][seq0][JMP(end)][SPLIT(alt=S2)][seq1][JMP(end)]...[seqN] end:
 * 各分岐は成功したら JMP(end) で共通の継続へ。失敗したら SPLIT の alt で次の
 * 分岐へ。最後の分岐には SPLIT/JMP を置かない（失敗はそのまま false）。
 * 先頭 SPLIT は alt の開始位置（alt_base）に挿入する（グループ内では SAVE の後）。 */
static bool rc_compile_alt(RexC *rc) {
#define RX_MAX_ALT 64u
    if (rc->failed) return false;
    u32 alt_base = rc->n_ins;
    u32 jmps[RX_MAX_ALT];
    u32 n_jmps = 0;
    if (!rc_compile_seq(rc)) return false;
    if (rc->failed || rc->pos >= rc->len || rc->pat[rc->pos] != '|') return !rc->failed;
    RexIns sp0;
    memset(&sp0, 0, sizeof sp0);
    sp0.op = RI_SPLIT;
    if (!rc_ins_insert(rc, alt_base, sp0)) return false;
    u32 split_slot = alt_base; /* パッチ待ち SPLIT（alt 未定） */
    for (;;) {
        if (rc->failed) return false;
        if (rc->pos >= rc->len || rc->pat[rc->pos] != '|') break;
        rc->pos++; /* | */
        u32 seq_start = rc->n_ins; /* この分岐の開始位置（挿入される JMP の位置） */
        if (n_jmps >= RX_MAX_ALT) { rc_fail(rc, "too many alternatives"); return false; }
        /* 前の分岐の末尾に JMP(end) を置く（= この分岐の直前） */
        RexIns jm;
        memset(&jm, 0, sizeof jm);
        jm.op = RI_JMP;
        if (!rc_ins_insert(rc, seq_start, jm)) return false;
        jmps[n_jmps++] = seq_start;
        rc->ins[split_slot].u32v = seq_start + 1; /* 前の分岐の alt = この分岐本体 */
        if (!rc_compile_seq(rc)) return false;
        if (rc->pos < rc->len && rc->pat[rc->pos] == '|') {
            /* この分岐にも SPLIT を置く（次の分岐用） */
            RexIns sp;
            memset(&sp, 0, sizeof sp);
            sp.op = RI_SPLIT;
            if (!rc_ins_insert(rc, seq_start + 1, sp)) return false;
            split_slot = seq_start + 1;
        } else {
            split_slot = UINT32_MAX;
        }
    }
    /* JMP(end) 群のターゲット = 現在の末尾（共通継続） */
    for (u32 i = 0; i < n_jmps; i++) rc->ins[jmps[i]].u32v = rc->n_ins;
    return true;
#undef RX_MAX_ALT
}

/* ================= 実行 ================= */

typedef struct {
    const AklRex *rx;
    const u8 *s;
    u32 s_len;
    u32 *cap_beg;
    u32 *cap_end;
    u32 ncap;
    u32 steps;
    bool lim;
} RxRun;

static bool rx_run(RxRun *rr, u32 pos, u32 ip, u32 depth) {
    const RexIns *ins = rr->rx->ins;
    if (++rr->steps > RX_STEP_LIMIT) { rr->lim = true; return false; }
    if (depth > RX_DEPTH_LIMIT) { rr->lim = true; return false; }
    for (;;) {
        if (ip >= rr->rx->n_ins) return false;
        const RexIns *in = &ins[ip];
        switch (in->op) {
        case RI_MATCH:
            rr->cap_end[0] = pos;
            return true;
        case RI_CHAR: {
            if (pos >= rr->s_len) return false;
            u8 c = rr->s[pos];
            if (rr->rx->flags & AKL_RX_F_IGNORE) {
                if (rx_lc(c) != rx_lc((u8)in->u32v)) return false;
            } else {
                if (c != (u8)in->u32v) return false;
            }
            pos++;
            ip++;
            break;
        }
        case RI_CHAR8: {
            u32 cl = in->u32v;
            if (cl == 0 || cl > 8 || pos + cl > rr->s_len) return false;
            u64 v = 0;
            for (u32 i = 0; i < cl; i++) v |= (u64)rr->s[pos + i] << (i * 8);
            if (v != in->u64v) return false;
            pos += cl;
            ip++;
            break;
        }
        case RI_CLASS: {
            if (pos >= rr->s_len) return false;
            u8 c = rr->s[pos];
            if (!bits_has(rr->rx->classes + in->u32v * 32, c)) return false;
            pos++;
            ip++;
            break;
        }
        case RI_ANY: {
            if (pos >= rr->s_len) return false;
            u32 cp, clen;
            if (!rx_utf8_get(rr->s, rr->s_len, pos, &cp, &clen)) return false;
            if (!(rr->rx->flags & AKL_RX_F_DOTALL) &&
                rx_is_line_term(rr->s, rr->s_len, pos, clen))
                return false;
            pos += clen;
            ip++;
            break;
        }
        case RI_ISSPACE: case RI_NOTSPACE: {
            if (pos >= rr->s_len) return false;
            u32 cp, clen;
            if (!rx_utf8_get(rr->s, rr->s_len, pos, &cp, &clen)) return false;
            bool ws = rx_is_ws_cp(cp);
            if ((in->op == RI_ISSPACE) != ws) return false;
            pos += clen;
            ip++;
            break;
        }
        case RI_ANCHOR: {
            bool ok = false;
            switch (in->u32v) {
            case 0: { /* ^ */
                if (pos == 0) ok = true;
                else if (rr->rx->flags & AKL_RX_F_MULTI) {
                    u8 pc = rr->s[pos - 1];
                    if (pc == 0x0A) ok = true;
                    else if (pc == 0x0D) ok = !(pos >= 2 && rr->s[pos - 2] == 0x0A);
                    else if (pos >= 3 && rr->s[pos - 3] == 0xE2 && rr->s[pos - 2] == 0x80 &&
                             (rr->s[pos - 1] == 0xA8 || rr->s[pos - 1] == 0xA9))
                        ok = true;
                }
                break;
            }
            case 1: { /* $ */
                if (pos == rr->s_len) ok = true;
                else if (rr->rx->flags & AKL_RX_F_MULTI) {
                    u8 c = rr->s[pos];
                    if (c == 0x0A) ok = true;
                    else if (c == 0x0D) ok = !(pos + 1 < rr->s_len && rr->s[pos + 1] == 0x0A);
                    else if (pos + 3 <= rr->s_len && rr->s[pos] == 0xE2 &&
                             rr->s[pos + 1] == 0x80 &&
                             (rr->s[pos + 2] == 0xA8 || rr->s[pos + 2] == 0xA9))
                        ok = true;
                } else if (pos == rr->s_len - 1) {
                    /* m なし: 末尾の 1 行終端の直前にもマッチ */
                    u8 c = rr->s[pos];
                    ok = c == 0x0A || c == 0x0D ||
                         (pos + 3 <= rr->s_len && rr->s[pos] == 0xE2 &&
                          rr->s[pos + 1] == 0x80 &&
                          (rr->s[pos + 2] == 0xA8 || rr->s[pos + 2] == 0xA9));
                }
                break;
            }
            case 2: case 3: { /* \b \B */
                bool l = pos > 0 && rx_is_word_byte(rr->s[pos - 1]);
                bool r = pos < rr->s_len && rx_is_word_byte(rr->s[pos]);
                bool b = l != r;
                ok = (in->u32v == 2) ? b : !b;
                break;
            }
            default:
                return false;
            }
            if (!ok) return false;
            ip++;
            break;
        }
        case RI_SAVE_BEG: case RI_SAVE_END: {
            u32 g = in->u32v;
            if (g > rr->ncap) return false;
            u32 ob = rr->cap_beg[g], oe = rr->cap_end[g];
            if (in->op == RI_SAVE_BEG) rr->cap_beg[g] = pos;
            else rr->cap_end[g] = pos;
            if (rx_run(rr, pos, ip + 1, depth + 1)) return true;
            rr->cap_beg[g] = ob;
            rr->cap_end[g] = oe;
            return false;
        }
        case RI_SPLIT: case RI_SPLIT_L: {
            u32 alt = in->u32v;
            u32 save_beg[RX_MAX_CAP + 1], save_end[RX_MAX_CAP + 1];
            u32 nc = rr->ncap < RX_MAX_CAP ? rr->ncap : RX_MAX_CAP;
            memcpy(save_beg, rr->cap_beg, (u64)(nc + 1) * sizeof(u32));
            memcpy(save_end, rr->cap_end, (u64)(nc + 1) * sizeof(u32));
            if (in->op == RI_SPLIT) {
                if (rx_run(rr, pos, ip + 1, depth + 1)) return true;
                memcpy(rr->cap_beg, save_beg, (u64)(nc + 1) * sizeof(u32));
                memcpy(rr->cap_end, save_end, (u64)(nc + 1) * sizeof(u32));
                if (alt >= rr->rx->n_ins) return false;
                return rx_run(rr, pos, alt, depth + 1);
            }
            if (alt >= rr->rx->n_ins) { if (rx_run(rr, pos, ip + 1, depth + 1)) return true; return false; }
            if (rx_run(rr, pos, alt, depth + 1)) return true;
            memcpy(rr->cap_beg, save_beg, (u64)(nc + 1) * sizeof(u32));
            memcpy(rr->cap_end, save_end, (u64)(nc + 1) * sizeof(u32));
            return rx_run(rr, pos, ip + 1, depth + 1);
        }
        case RI_JMP:
            ip = in->u32v;
            break;
        default:
            return false;
        }
    }
}

/* ================= 公開 API ================= */

AklRex *akl_rex_compile(const uint8_t *pat, uint32_t pat_len, uint32_t flags,
                        char *err_buf, uint32_t err_cap) {
    if (err_cap) err_buf[0] = 0;
    if (pat_len > RX_MAX_PAT) {
        if (err_cap) snprintf(err_buf, err_cap, "invalid regexp: pattern too long");
        return NULL;
    }
    RexC rc;
    memset(&rc, 0, sizeof rc);
    rc.pat = pat; rc.len = pat_len; rc.flags = flags;
    rc.err = err_buf; rc.err_cap = err_cap;
    bool ok = rc_compile_alt(&rc);
    if (ok && !rc.failed && rc.pos < rc.len) {
        rc_fail(&rc, "unexpected character");
        ok = false;
    }
    if (!ok || rc.failed) {
        free(rc.ins);
        free(rc.classes);
        return NULL;
    }
    RexIns m;
    memset(&m, 0, sizeof m);
    m.op = RI_MATCH;
    rc_emit(&rc, m);
    AklRex *rx = (AklRex *)malloc(sizeof(AklRex));
    if (!rx) { free(rc.ins); free(rc.classes); return NULL; }
    rx->pat = (u8 *)malloc(pat_len ? pat_len : 1);
    if (!rx->pat) { free(rx); free(rc.ins); free(rc.classes); return NULL; }
    memcpy(rx->pat, pat, pat_len);
    rx->pat_len = pat_len;
    rx->flags = flags;
    rx->ins = rc.ins;
    rx->n_ins = rc.n_ins;
    rx->classes = rc.classes;
    rx->n_classes = rc.n_classes;
    rx->n_cap = rc.n_cap;
    return rx;
}

void akl_rex_free(AklRex *rx) {
    if (!rx) return;
    free(rx->pat);
    free(rx->ins);
    free(rx->classes);
    free(rx);
}

uint32_t akl_rex_ncap(const AklRex *rx) { return rx ? rx->n_cap : 0; }
const uint8_t *akl_rex_pat(const AklRex *rx, uint32_t *len) {
    if (!rx) return NULL;
    if (len) *len = rx->pat_len;
    return rx->pat;
}
uint32_t akl_rex_flags(const AklRex *rx) { return rx ? rx->flags : 0; }

bool akl_rex_match(const AklRex *rx, const uint8_t *s, uint32_t s_len, uint32_t start,
                   uint32_t *cap_beg, uint32_t *cap_end, uint32_t ncap, bool *lim) {
    if (lim) *lim = false;
    if (!rx) return false;
    u32 nc = rx->n_cap < ncap ? rx->n_cap : ncap;
    RxRun rr;
    memset(&rr, 0, sizeof rr);
    rr.rx = rx; rr.s = s; rr.s_len = s_len;
    rr.cap_beg = cap_beg; rr.cap_end = cap_end; rr.ncap = nc;
    if (start > s_len) start = s_len;
    for (u32 pos = start; ; ) {
        for (u32 k = 0; k <= nc; k++) { cap_beg[k] = UINT32_MAX; cap_end[k] = UINT32_MAX; }
        rr.steps = 0;
        rr.cap_beg = cap_beg; rr.cap_end = cap_end;
        rr.cap_beg[0] = pos;
        bool ok = rx_run(&rr, pos, 0, 0);
        if (ok) {
            if (cap_end[0] == UINT32_MAX) cap_end[0] = pos;
            return true;
        }
        if (rr.lim) { if (lim) *lim = true; return false; }
        if (rx->flags & AKL_RX_F_STICKY) return false;
        if (pos >= s_len) break;
        u32 cp, clen;
        if (!rx_utf8_get(s, s_len, pos, &cp, &clen)) { pos++; continue; }
        pos += clen;
    }
    return false;
}
