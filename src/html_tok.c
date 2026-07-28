/* Ifuto — HTML トークナイザ（WHATWG サブセット + 仕様流儀のエラー回復）
 *
 * 防御的設計:
 *   - すべてのループは pos の単調増加でしか回らない（無限ループ不能）。
 *   - 不正バイト列・切断タグ・NUL・孤立 '<' をすべてトークン化エラーとして回収し、crash しない。
 *   - 属性は first-wins 重複除去・個数上限つき。
 *   - 文字参照は二段階（長さ計算 → arena 確保 → 書き込み）で無駄を出さない。
 */
#include "html_int.h"
#include "utf8.h"

#define IF_MAX_ATTRS 256u

static bool if_hws(u8 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }
static bool if_alpha(u8 c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool if_alnum(u8 c) { return if_alpha(c) || (c >= '0' && c <= '9'); }

void if_tok_init(IfHtmlTok *t, IfArena *arena, IfStr input) {
    t->src = (const u8 *)input.p;
    t->len = input.n;
    t->pos = 0;
    t->arena = arena;
    t->raw_tag = 0;
    t->errors = 0;
}

void if_tok_set_raw(IfHtmlTok *t, u16 tag) {
    t->raw_tag = tag;
    t->raw_rcdata = if_tag_is_rcdata(tag) ? 1 : 0;
    t->strip_lf = (tag == IF_TAG_TEXTAREA) ? 1 : 0;
}

/* ---- 文字参照 ---- */

/* windows-1252 マッピング（WHATWG 数値文字参照の C1 補正表） */
static const u16 IF_C1_MAP[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

static const struct { const char *name; u8 n; u16 cp; } IF_NAMED_REFS[] = {
    {"amp", 3, 0x26}, {"lt", 2, 0x3C}, {"gt", 2, 0x3E}, {"quot", 4, 0x22},
    {"apos", 4, 0x27}, {"nbsp", 4, 0xA0}, {"copy", 4, 0xA9}, {"reg", 3, 0xAE},
    {"trade", 5, 0x2122}, {"hellip", 6, 0x2026}, {"mdash", 5, 0x2014},
    {"ndash", 5, 0x2013}, {"laquo", 5, 0xAB}, {"raquo", 5, 0xBB},
    {"times", 5, 0xD7}, {"divide", 6, 0xF7}, {"middot", 6, 0xB7},
    {"bull", 4, 0x2022}, {"dagger", 6, 0x2020}, {"Dagger", 6, 0x2021},
    {"permil", 6, 0x2030}, {"prime", 5, 0x2032}, {"Prime", 5, 0x2033},
    {"lsaquo", 6, 0x2039}, {"rsaquo", 6, 0x203A}, {"oline", 5, 0x203E},
    {"frasl", 5, 0x2044}, {"euro", 4, 0x20AC}, {"deg", 3, 0xB0},
    {"plusmn", 6, 0xB1}, {"para", 4, 0xB6}, {"sect", 4, 0xA7},
    {"larr", 4, 0x2190}, {"uarr", 4, 0x2191}, {"rarr", 4, 0x2192},
    {"darr", 4, 0x2193}, {"harr", 4, 0x2194},
};

/* '&' の直後（pos は '&' の次）の参照をデコード。
 * 参照として成立すれば cp（または先頭 cp）を返し *out_pos を参照の次へ。
 * 不成立なら 0 を返し *out_pos は '&' の次のまま（呼び出し側で '&' をリテラルに）。 */
static u32 if_charref(IfHtmlTok *t, u32 amp_next, u32 *out_pos) {
    u32 i = amp_next;
    if (i >= t->len) return 0;

    if (t->src[i] == '#') {
        u32 j = i + 1;
        bool hex = false;
        if (j < t->len && (t->src[j] == 'x' || t->src[j] == 'X')) { hex = true; j++; }
        u32 start = j;
        u64 v = 0;
        while (j < t->len) {
            u8 c = t->src[j];
            if (!hex && c >= '0' && c <= '9') v = v * 10 + (u64)(c - '0');
            else if (hex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                v = v * 16 + (u64)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
            else break;
            if (v > 0x7FFFFFFF) { v = 0x110000; break; } /* clamp: 以後の桁も消費のため継続 */
            j++;
        }
        if (j == start) return 0; /* '#' の後に桁がない: リテラル */
        /* 残りの同種の桁を消費（clamp 時） */
        while (j < t->len) {
            u8 c = t->src[j];
            bool dig = hex ? ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
                           : (c >= '0' && c <= '9');
            if (!dig) break;
            j++;
        }
        if (j < t->len && t->src[j] == ';') j++;
        else t->errors++; /* セミコロン欠落は回復可能エラー */
        u32 cp = (u32)v;
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = IF_CP_REPLACEMENT;
        else if (cp >= 0x80 && cp <= 0x9F) cp = IF_C1_MAP[cp - 0x80];
        *out_pos = j;
        return cp;
    }

    if (if_alnum(t->src[i])) {
        /* 最長一致（前方一致でテーブルを線形走査。件数が小さいので分木不要） */
        u32 best_len = 0, best_cp = 0;
        for (u64 k = 0; k < sizeof(IF_NAMED_REFS) / sizeof(IF_NAMED_REFS[0]); k++) {
            u32 n = IF_NAMED_REFS[k].n;
            if (n <= best_len || i + n > t->len) continue;
            if (memcmp(t->src + i, IF_NAMED_REFS[k].name, n) != 0) continue;
            bool semi = (i + n < t->len && t->src[i + n] == ';');
            if (!semi) continue; /* v0.1: 名前参照はセミコロン必須（回復は '&' リテラル側に倒す） */
            best_len = n; best_cp = IF_NAMED_REFS[k].cp;
        }
        if (best_len) {
            *out_pos = i + best_len + 1; /* ';' 込み */
            return best_cp;
        }
    }
    return 0;
}

/* 文字参照を含まないことを確認しながら [start,end) をデコード長だけ返す */
static u32 if_decoded_len(IfHtmlTok *t, u32 start, u32 end, bool *had_ref) {
    u32 out = 0;
    u32 i = start;
    *had_ref = false;
    while (i < end) {
        if (t->src[i] == '&') {
            u32 np = i + 1;
            u32 cp = if_charref(t, i + 1, &np);
            if (cp) {
                *had_ref = true;
                u8 tmp[4];
                out += if_utf8_encode(cp, tmp);
                i = np;
                continue;
            }
        }
        /* 不正バイト → FFFD(3B) など、デコード後 UTF-8 長は生長と一致しないので再エンコードして数える */
        u32 np = i;
        u32 cp2 = if_utf8_decode(t->src, end, &np);
        u8 tmp[4];
        out += if_utf8_encode(cp2, tmp);
        i = np;
    }
    return out;
}

static void if_decode_into(IfHtmlTok *t, u32 start, u32 end, u8 *dst) {
    u32 w = 0;
    u32 i = start;
    while (i < end) {
        if (t->src[i] == '&') {
            u32 np = i + 1;
            u32 cp = if_charref(t, i + 1, &np);
            if (cp) { w += if_utf8_encode(cp, dst + w); i = np; continue; }
        }
        u32 np = i;
        u32 cp = if_utf8_decode(t->src, end, &np);
        w += if_utf8_encode(cp, dst + w);
        i = np;
    }
}

/* 文字参照を解決した文字列を返す。参照が無ければ入力スライス（ゼロコピー）。 */
static IfStr if_resolved(IfHtmlTok *t, u32 start, u32 end) {
    bool had_ref = false;
    u32 olen = if_decoded_len(t, start, end, &had_ref);
    if (!had_ref) return if_str((const char *)t->src + start, end - start);
    u8 *buf = (u8 *)if_arena_alloc(t->arena, olen ? olen : 1);
    if_decode_into(t, start, end, buf);
    return if_str((const char *)buf, olen);
}

/* ---- rawtext ---- */

/* raw_tag の終了タグ "</name" (後続は ws, '/', '>') を探す。見つからなければ len を返す */
static u32 if_find_raw_end(IfHtmlTok *t) {
    IfStr name = if_str(if_tag_name(t->raw_tag), 0);
    name.n = name.p ? (u32)strlen(name.p) : 0;
    if (name.n == 0) return t->len;
    u32 i = t->pos;
    while (i + 2 + name.n <= t->len) {
        if (t->src[i] == '<' && t->src[i + 1] == '/') {
            bool match = true;
            for (u32 k = 0; k < name.n; k++) {
                if (if_ascii_lower(t->src[i + 2 + k]) != (u8)name.p[k]) { match = false; break; }
            }
            if (match) {
                u32 after = i + 2 + name.n;
                /* EOF は終端として認めない（仕様: rawtext 終了タグ名の後ろが
                 * ファイル終端なら、それはテキスト。tests16 の採点結果で確認） */
                if (after < t->len &&
                    (if_hws(t->src[after]) || t->src[after] == '/' || t->src[after] == '>'))
                    return i;
            }
        }
        i++;
    }
    return t->len;
}

static IfTok if_raw_token(IfHtmlTok *t) {
    IfTok tok = { .kind = TOK_EOF };
    /* textarea の仕様: 開始タグ直後の LF 1 個は無視する */
    if (t->strip_lf) {
        t->strip_lf = 0;
        if (t->pos < t->len && t->src[t->pos] == '\n') t->pos++;
    }
    u32 end = if_find_raw_end(t);
    if (end > t->pos) {
        tok.kind = TOK_TEXT;
        /* RCDATA(title/textarea) は文字参照を解決する。rawtext(style/script) は生のまま。 */
        tok.text = t->raw_rcdata ? if_resolved(t, t->pos, end)
                                 : if_str((const char *)t->src + t->pos, end - t->pos);
        t->pos = end;
        return tok;
    }
    t->raw_tag = 0; /* 終了タグは通常の字句解析へ */
    return if_tok_next(t);
}

/* ---- 本体 ---- */

static IfTok if_tag_token(IfHtmlTok *t, bool is_end) {
    IfTok tok = { .kind = TOK_EOF };
    tok.kind = is_end ? TOK_END : TOK_START;

    IfAttr *attrs = NULL;
    u32 n_attrs = 0;
    u64 cap = 0;

    /* タグ名 */
    u32 name_start = t->pos;
    while (t->pos < t->len && !if_hws(t->src[t->pos]) && t->src[t->pos] != '/' &&
           t->src[t->pos] != '>' && t->src[t->pos] != 0) {
        t->pos++;
    }
    IfStr name = if_str((const char *)t->src + name_start, t->pos - name_start);
    tok.tag_raw = name;
    tok.tag = if_tag_id(name);

    for (;;) {
        /* 空白スキップ */
        while (t->pos < t->len && if_hws(t->src[t->pos])) t->pos++;
        if (t->pos >= t->len) { t->errors++; return (IfTok){ .kind = TOK_EOF }; }
        u8 c = t->src[t->pos];
        if (c == '>') { t->pos++; tok.attrs = attrs; tok.n_attrs = n_attrs; return tok; }
        if (c == '/') {
            t->pos++;
            if (t->pos < t->len && t->src[t->pos] == '>') {
                t->pos++;
                tok.self_closing = true;
                tok.attrs = attrs;
                tok.n_attrs = n_attrs;
                return tok;
            }
            t->errors++; /* 迷いの '/' — 読み飛ばして継続 */
            continue;
        }
        /* 属性名 */
        u32 as = t->pos;
        while (t->pos < t->len) {
            u8 a = t->src[t->pos];
            if (if_hws(a) || a == '=' || a == '>' || a == '/') break;
            if (a == 0) { t->errors++; }
            t->pos++;
        }
        IfStr aname = if_str((const char *)t->src + as, t->pos - as);
        while (t->pos < t->len && if_hws(t->src[t->pos])) t->pos++;
        IfStr aval = { NULL, 0 };
        if (t->pos < t->len && t->src[t->pos] == '=') {
            t->pos++;
            while (t->pos < t->len && if_hws(t->src[t->pos])) t->pos++;
            if (t->pos < t->len) {
                u8 q = t->src[t->pos];
                if (q == '"' || q == '\'') {
                    t->pos++;
                    u32 vs = t->pos;
                    while (t->pos < t->len && t->src[t->pos] != q) t->pos++;
                    aval = if_resolved(t, vs, t->pos);
                    if (t->pos < t->len) t->pos++; /* 閉じクォート */
                    else t->errors++;
                } else {
                    u32 vs = t->pos;
                    while (t->pos < t->len && !if_hws(t->src[t->pos]) && t->src[t->pos] != '>') t->pos++;
                    aval = if_resolved(t, vs, t->pos);
                }
            }
        }
        if (aname.n == 0) { t->errors++; if (t->pos < t->len) t->pos++; continue; }

        /* 重複属性: first-wins（仕様どおり後勝ちではない） */
        bool dup = false;
        for (u32 k = 0; k < n_attrs; k++)
            if (if_str_eq_ci(attrs[k].name, aname)) { dup = true; t->errors++; break; }
        if (dup) continue;

        if (n_attrs >= IF_MAX_ATTRS) { t->errors++; continue; }
        attrs = (IfAttr *)if_arena_grow(t->arena, attrs, &cap, n_attrs + 1, sizeof(IfAttr));
        attrs[n_attrs].name = aname;
        attrs[n_attrs].value = aval;
        n_attrs++;
    }
}


/* doctype 本体（"doctype" より後ろ）の解析。
 * 形式: name [PUBLIC "pub" ["sys"] | SYSTEM "sys"] 。
 * HTML 本流仕様より単純化しているが、tree-construction 期待串の生成に必要な
 * 「name の lowercase 化 / pub・sys の有無と値」は仕様どおり拾う。 */
static u8 if_dt_ws(u8 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

static bool if_dt_kw(IfStr rest, u32 off, const char *kw) {
    u32 k = 0;
    while (kw[k]) k++;
    if (rest.n < off + k) return false;
    return if_str_eq_ci(if_str(rest.p + off, k), if_str(kw, k));
}

static u32 if_dt_quoted(IfStr rest, u32 off, IfStr *out) {
    u8 q = (u8)rest.p[off++];
    u32 vs = off;
    while (off < rest.n && (u8)rest.p[off] != q) off++;
    *out = if_str(rest.p + vs, off - vs);
    if (off < rest.n) off++; /* 閉じ引用符 */
    return off;
}

static void if_parse_doctype_rest(IfHtmlTok *t, IfTok *tok, IfStr rest) {
    u32 p = 0;
    while (p < rest.n && if_dt_ws((u8)rest.p[p])) p++;
    u32 ns = p;
    while (p < rest.n && !if_dt_ws((u8)rest.p[p])) p++;
    if (p > ns) {
        char *lc = (char *)if_arena_alloc(t->arena, (u64)(p - ns));
        for (u32 i = ns; i < p; i++) lc[i - ns] = (char)if_ascii_lower((u8)rest.p[i]);
        tok->text = if_str(lc, p - ns);
        tok->dt_has_name = 1;
    }
    while (p < rest.n && if_dt_ws((u8)rest.p[p])) p++;
    if (p >= rest.n) return;
    if (if_dt_kw(rest, p, "public")) {
        p += 6;
        while (p < rest.n && if_dt_ws((u8)rest.p[p])) p++;
        if (p < rest.n && (rest.p[p] == '"' || rest.p[p] == '\'')) {
            p = if_dt_quoted(rest, p, &tok->dt_pub);
            tok->dt_has_pub = 1;
            while (p < rest.n && if_dt_ws((u8)rest.p[p])) p++;
            if (p < rest.n && (rest.p[p] == '"' || rest.p[p] == '\'')) {
                p = if_dt_quoted(rest, p, &tok->dt_sys);
                tok->dt_has_sys = 1;
            }
        } else {
            t->errors++; /* missing public id */
        }
        return;
    }
    if (if_dt_kw(rest, p, "system")) {
        p += 6;
        while (p < rest.n && if_dt_ws((u8)rest.p[p])) p++;
        if (p < rest.n && (rest.p[p] == '"' || rest.p[p] == '\'')) {
            p = if_dt_quoted(rest, p, &tok->dt_sys);
            tok->dt_has_sys = 1;
        } else {
            t->errors++; /* missing system id */
        }
        return;
    }
}

/* "<!--" の後ろ。コメントまたは doctype/bogus */
static IfTok if_markup_decl(IfHtmlTok *t) {
    IfTok tok = { .kind = TOK_EOF };

    if (t->pos + 1 < t->len && t->src[t->pos] == '-' && t->src[t->pos + 1] == '-') {
        t->pos += 2;
        /* "<!-->" / "<!--->" の短絡形 */
        u32 i = t->pos;
        while (i < t->len && t->src[i] == '-') i++;
        if (i >= t->len) { t->errors++; tok.kind = TOK_COMMENT; tok.text = if_str("", 0); return tok; }
        if (t->src[i] == '>' && i - t->pos <= 1) { t->pos = i + 1; t->errors++; tok.kind = TOK_COMMENT; tok.text = if_str("", 0); return tok; }
        /* "-->" を探す（j+2 は len-1 までに制限して境界外アクセスを防ぐ） */
        u32 start = t->pos;
        u32 j = start;
        while (j + 2 < t->len) {
            if (t->src[j] == '-' && t->src[j + 1] == '-' && t->src[j + 2] == '>') {
                tok.kind = TOK_COMMENT;
                tok.text = if_str((const char *)t->src + start, j - start);
                t->pos = j + 3;
                return tok;
            }
            j++;
        }
        t->errors++; /* 閉じられないコメント: 残り全部をコメントに */
        tok.kind = TOK_COMMENT;
        tok.text = if_str((const char *)t->src + start, t->len - start);
        t->pos = t->len;
        return tok;
    }

    /* "<![CDATA[": foreign content 内ではテキスト、外では bogus comment */
    if (t->cdata_foreign && t->pos + 7 <= t->len &&
        memcmp(t->src + t->pos, "[CDATA[", 7) == 0) {
        u32 start = t->pos + 7;
        u32 j = start;
        while (j + 2 < t->len &&
               !(t->src[j] == ']' && t->src[j + 1] == ']' && t->src[j + 2] == '>')) j++;
        IfTok cdt = { .kind = TOK_TEXT };
        if (j + 2 < t->len) {
            cdt.text = if_str((const char *)t->src + start, j - start);
            t->pos = j + 3;
        } else {
            t->errors++; /* 閉じられない CDATA: 残り全部をテキストに */
            cdt.text = if_str((const char *)t->src + start, t->len - start);
            t->pos = t->len;
        }
        if (cdt.text.n == 0) return if_tok_next(t);
        return cdt;
    }

    /* doctype or bogus */
    u32 start = t->pos;
    while (t->pos < t->len && t->src[t->pos] != '>') t->pos++;
    IfStr body = if_str((const char *)t->src + start, t->pos - start);
    if (t->pos < t->len) t->pos++; /* '>' */
    IfStr b7 = body.n > 7 ? if_str(body.p, 7) : body;
    if (if_str_eq_ci(b7, IF_S("doctype"))) {
        tok.kind = TOK_DOCTYPE;
        IfStr rest = body.n > 7 ? if_str(body.p + 7, body.n - 7) : if_str("", 0);
        if_parse_doctype_rest(t, &tok, rest);
        return tok;
    }
    t->errors++;
    tok.kind = TOK_COMMENT; /* bogus comment */
    tok.text = body;
    return tok;
}

IfTok if_tok_next(IfHtmlTok *t) {
    IfTok tok = { .kind = TOK_EOF };

    /* <plaintext>: 残り全入力を 1 個の TEXT として返す（文字参照も解決しない） */
    if (t->plaintext && t->pos < t->len) {
        tok.kind = TOK_TEXT;
        tok.text = if_str((const char *)t->src + t->pos, t->len - t->pos);
        t->pos = t->len;
        return tok;
    }

    if (t->raw_tag && t->pos < t->len) return if_raw_token(t);
    if (t->pos >= t->len) return tok;

    /* テキスト走査: '<' まで */
    if (t->src[t->pos] != '<') {
        u32 start = t->pos;
        while (t->pos < t->len && t->src[t->pos] != '<') {
            if (t->src[t->pos] == 0) t->errors++;
            t->pos++;
        }
        tok.kind = TOK_TEXT;
        tok.text = if_resolved(t, start, t->pos);
        return tok;
    }

    /* '<' の処理 */
    if (t->pos + 1 >= t->len) {
        /* 孤立 '<' で終端 → テキストとして返す */
        tok.kind = TOK_TEXT;
        tok.text = if_str("<", 1);
        t->pos = t->len;
        t->errors++;
        return tok;
    }
    u8 c1 = t->src[t->pos + 1];

    if (if_alpha(c1)) {
        t->pos += 1; /* タグ名の先頭文字へ（if_tag_token は pos から名前を読む） */
        return if_tag_token(t, false);
    }
    if (c1 == '/') {
        if (t->pos + 2 < t->len && if_alpha(t->src[t->pos + 2])) {
            t->pos += 2; /* "</" を消費し、タグ名先頭へ（if_tag_token は pos から名前を読む） */
            return if_tag_token(t, true);
        }
        if (t->pos + 2 < t->len && t->src[t->pos + 2] == '>') {
            t->pos += 3; /* "</>" は捨てる */
            t->errors++;
            return if_tok_next(t);
        }
        /* "</" + その他 → bogus comment */
        t->pos += 2;
        u32 start = t->pos;
        while (t->pos < t->len && t->src[t->pos] != '>') t->pos++;
        tok.kind = TOK_COMMENT;
        tok.text = if_str((const char *)t->src + start, t->pos - start);
        if (t->pos < t->len) t->pos++;
        t->errors++;
        return tok;
    }
    if (c1 == '!') {
        t->pos += 2;
        return if_markup_decl(t);
    }
    if (c1 == '?') {
        /* Processing Instruction（WPT processing-instructions.dat が定義する文法）:
         *   <? target ws* data >  target = [A-Za-z_] [A-Za-z0-9_-]* 、先頭が xml(CI) で始まらない
         *   - data の終端は最初の '>'（"?>" 対である必要はない。'?' は data に残る）
         *   - EOF で閉じられなかった PI は**トークンごと捨てる**（実データで確認済み）
         *   - ターゲット妥当性違反は bogus comment（data は '?' を含め '>' まで。EOF でも出す）
         */
        u32 q = t->pos + 1; /* '?' の位置（bogus comment の data 先頭） */
        u32 p = t->pos + 2;
        if (p >= t->len) { /* "<?" で EOF: 何も出さない */
            t->pos = t->len;
            t->errors++;
            return if_tok_next(t);
        }
        u8 c = t->src[p];
        bool can_start = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        if (can_start) {
            u32 ts = p;
            while (p < t->len) {
                u8 d = t->src[p];
                if ((d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z') ||
                    (d >= '0' && d <= '9') || d == '_' || d == '-') p++;
                else break;
            }
            if (p >= t->len) { /* EOF in target: 捨てる */
                t->pos = t->len;
                t->errors++;
                return if_tok_next(t);
            }
            IfStr target = if_str((const char *)t->src + ts, p - ts);
            bool is_xml = target.n >= 3 &&
                if_ascii_lower((u8)target.p[0]) == 'x' &&
                if_ascii_lower((u8)target.p[1]) == 'm' &&
                if_ascii_lower((u8)target.p[2]) == 'l';
            u8 next = t->src[p];
            bool term_ok = next == '>' || next == '?' || if_hws(next);
            if (!is_xml && term_ok) {
                while (p < t->len && if_hws(t->src[p])) p++;
                u32 ds = p;
                while (p < t->len && t->src[p] != '>') p++;
                if (p >= t->len) { /* EOF in data: 捨てる */
                    t->pos = t->len;
                    t->errors++;
                    return if_tok_next(t);
                }
                /* 終端が "?>" なら '?' は data に含めない（"? >" のように空白挟みは含める） */
                u32 de = p;
                if (de > ds && t->src[de - 1] == '?') de--;
                tok.kind = TOK_COMMENT;
                tok.is_pi = 1;
                tok.pi_target = target;
                tok.text = if_str((const char *)t->src + ds, de - ds);
                t->pos = p + 1; /* '>' */
                return tok;
            }
        }
        /* bogus comment: data は '?' から '>' の手前まで。EOF なら残り全部で出す */
        t->pos = q;
        u32 start = t->pos;
        while (t->pos < t->len && t->src[t->pos] != '>') t->pos++;
        tok.kind = TOK_COMMENT;
        tok.text = if_str((const char *)t->src + start, t->pos - start);
        if (t->pos < t->len) t->pos++;
        t->errors++;
        return tok;
    }

    /* '<' + その他 → '<' はリテラルテキスト */
    tok.kind = TOK_TEXT;
    tok.text = if_str("<", 1);
    t->pos += 1;
    t->errors++;
    return tok;
}
