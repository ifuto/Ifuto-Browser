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
#include "entities_gen.h"

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
    t->strip_lf = 0; /* LF-skip は tree の skip_lf が正本（二重 skip 防止） */
}

/* ---- 文字参照 ---- */

/* windows-1252 マッピング（WHATWG 数値文字参照の C1 補正表） */
static const u16 IF_C1_MAP[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};



/* 名前参照の 2 コードポイント可能性を含む参照解決（WHATWG named character references） */
typedef struct { u32 cp[2]; u8 n; } IfCps; /* n=0 で解決失敗 */

static u32 ent_cp1(const IfEntNamed *e) {
    return e->cp1 == 0xFFFFu ? IF_ENT_AUX32[e->cp2] : e->cp1;
}
static u32 ent_cp2(const IfEntNamed *e) {
    /* cp2==0: 単一 cp。cp1==0xFFFF: astral 単一(cp2 は aux index)。2-cp は両方 BMP。 */
    return (e->cp1 == 0xFFFFu || e->cp2 == 0) ? 0 : e->cp2;
}

static IfCps if_named_ref(IfHtmlTok *t, u32 amp_next, bool in_attr, u32 *out_pos) {
    IfCps r = { {0, 0}, 0 };
    u32 i = amp_next;
    if (i >= t->len || !if_alnum(t->src[i])) return r;
    /* alnum ランを収集（最長エンティティ名は 31 文字） */
    u32 run = 0;
    char buf[48];
    while (i + run < t->len && if_alnum(t->src[i + run]) && run + 1 < sizeof buf) {
        buf[run] = (char)t->src[i + run];
        run++;
    }
    /* 1) 正式形: 長い n から下ろして "name;" 完全一致（flags bit0） */
    for (u32 n = run; n >= 2; n--) {
        i32 ei = if_ent_find(buf, n);
        if (ei < 0) continue;
        const IfEntNamed *e = &IF_ENT_NAMED[ei];
        if (!(e->flags & 1)) continue;
        if (i + n >= t->len || t->src[i + n] != ';') continue;
        r.cp[0] = ent_cp1(e);
        r.cp[1] = ent_cp2(e);
        r.n = r.cp[1] ? 2 : 1;
        *out_pos = i + n + 1;
        return r;
    }
    /* 2) legacy（セミコロン無し）: 最長 prefix。属性値の中では後続が alnum か '='
     *    なら不可とする（WHATWG の ambiguous-amp 規則に対応した確定的規則） */
    {
        i32 ei = if_ent_longest_legacy(buf, run);
        if (ei >= 0) {
            const IfEntNamed *e = &IF_ENT_NAMED[ei];
            u8 next = (i + e->name_len < t->len) ? t->src[i + e->name_len] : 0;
            bool blocked = in_attr && ((next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z')
                                       || (next >= '0' && next <= '9') || next == '=');
            if (!blocked) {
                r.cp[0] = ent_cp1(e);
                r.cp[1] = ent_cp2(e);
                r.n = r.cp[1] ? 2 : 1;
                *out_pos = i + e->name_len;
                return r;
            }
        }
    }
    return r;
}

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
            if (v > 0x110000) v = 0x110000; /* 飽和。数字自体は最後まで消費する（仕様: 可能な限り消費） */
            j++;
        }
        if (j == start) { t->errors++; return 0; }
        if (j < t->len && t->src[j] == ';') j++;
        else t->errors++; /* セミコロン欠落は回復可能エラー */
        u32 cp = (u32)v;
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = IF_CP_REPLACEMENT;
        else if (cp >= 0x80 && cp <= 0x9F) cp = IF_C1_MAP[cp - 0x80];
        *out_pos = j;
        return cp;
    }

    if (if_alnum(t->src[i])) {
        u32 np = i;
        IfCps cps = if_named_ref(t, i, t->in_attr_ctx != 0, &np);
        if (cps.n) {
            *out_pos = np;
            return cps.cp[0]; /* 2cp エントリの第 2 要素は if_named_ref_cps で直接取得する経路 */
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
            u8 tmp[4];
            if (i + 1 < end && if_alnum(t->src[i + 1])) {
                /* 名前参照: 2 コードポイント形まで正確に数える（&NotEqualTilde; 系） */
                IfCps cps = if_named_ref(t, i + 1, t->in_attr_ctx != 0, &np);
                if (cps.n) {
                    *had_ref = true;
                    out += if_utf8_encode(cps.cp[0], tmp);
                    if (cps.n > 1) out += if_utf8_encode(cps.cp[1], tmp);
                    i = np;
                    continue;
                }
            } else {
                u32 cp = if_charref(t, i + 1, &np);
                if (cp) {
                    *had_ref = true;
                    out += if_utf8_encode(cp, tmp);
                    i = np;
                    continue;
                }
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
            if (i + 1 < end && if_alnum(t->src[i + 1])) {
                IfCps cps = if_named_ref(t, i + 1, t->in_attr_ctx != 0, &np);
                if (cps.n) {
                    w += if_utf8_encode(cps.cp[0], dst + w);
                    if (cps.n > 1) w += if_utf8_encode(cps.cp[1], dst + w);
                    i = np;
                    continue;
                }
            } else {
                u32 cp = if_charref(t, i + 1, &np);
                if (cp) { w += if_utf8_encode(cp, dst + w); i = np; continue; }
            }
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

/* U+0000 の規格処理（WHATWG の parse error つき規則）:
 *   to_fffd=false: バイトを除去（in-body の data text は "ignore"）
 *   to_fffd=true : U+FFFD ("\xEF\xBF\xBD") に置換（rawtext/script/plaintext/属性値）
 * NUL が無ければ入力をそのまま返す（arena 確保なし）。 */
/* frameset-ok 判定用: 「空白（TAB/LF/FF/CR/SP）でも U+0000 でもない」実文字を含むか。
 * rawtext/plaintext（tokenizer が U+0000 を U+FFFD 化する経路）では NUL も実文字なので
 * そちらは if_str_is_ws_only ベースで判定し、本 helper は NUL が tree に「U+0000 のまま」
 * 届く経路（DATA text / CDATA）専用にする。 */
static u8 if_tok_real_text(IfStr s) {
    for (u32 i = 0; i < s.n; i++) {
        u8 c = (u8)s.p[i];
        if (c == 0 || c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20) continue;
        return 1;
    }
    return 0;
}

static IfStr if_fix_nul(IfHtmlTok *t, IfStr s, bool to_fffd) {
    bool has_nul = false;
    for (u32 i = 0; i < s.n; i++)
        if (s.p[i] == 0) { has_nul = true; break; }
    if (!has_nul) return s;
    u32 extra = to_fffd ? 0 : 0;
    (void)extra;
    u32 cap = s.n * (to_fffd ? 3 : 1) + 1;
    char *buf = (char *)if_arena_alloc(t->arena, cap);
    u32 w = 0;
    for (u32 i = 0; i < s.n; i++) {
        if (s.p[i] == 0) {
            if (to_fffd) {
                buf[w++] = (char)0xEF;
                buf[w++] = (char)0xBF;
                buf[w++] = (char)0xBD;
            }
            /* drop 時は何も書かない */
        } else {
            buf[w++] = s.p[i];
        }
    }
    return if_str(buf, w);
}


/* ---- rawtext ---- */

/* i に「<script」「</script」「<!--」「-->」のいずれかが始まるかを、
 * 後続文字 (ws / '/' / '>' / EOF) まで含めて判定する補助。EOF は終端として認めない方針。 */
static bool raw_at(const IfHtmlTok *t, u32 i, const char *lit, bool need_term) {
    u32 n = (u32)strlen(lit);
    if (i + n > t->len) return false;
    for (u32 k = 0; k < n; k++)
        if (if_ascii_lower(t->src[i + k]) != (u8)lit[k]) return false;
    if (!need_term) return true;
    u32 after = i + n;
    if (after >= t->len) return false; /* EOF 直前は終端として認めない（tests16 採点規則と整合） */
    return if_hws(t->src[after]) || t->src[after] == '/' || t->src[after] == '>';
}

/* script data のエスケープ状態機械（WHATWG 8.2.26-35 の実用形）:
 *   DATA: '<!--' で ESC へ、'</script' + 区切りで終端（開始タグ放出済みなので
 *         appropriate end tag と合致する）。
 *   ESC:  '-->' で DATA へ、'<script' + 区切りで DBL へ、'</script' + 区切りで終端
 *         （fragment/raw_frag（開始タグ未放出）時は if_raw_token が本関数を
 *           呼ばず EOF 走査にするので、ここに載るのは常に「appropriate 合致可」）。
 *   DBL:  '</script' + 区切りで ESC へ（終端ではない）。
 * 戻り値は真の終了タグの '<' の位置。見つからなければ len。 */
static u32 if_find_script_end(IfHtmlTok *t) {
    enum { S_DATA, S_ESC, S_DBL } st = S_DATA;
    u32 i = t->pos;
    while (i < t->len) {
        if (st == S_DATA) {
            if (raw_at(t, i, "<!--", false)) { st = S_ESC; i += 4; continue; }
            if (raw_at(t, i, "</script", true)) return i;
            i++;
        } else if (st == S_ESC) {
            if (raw_at(t, i, "-->", false)) { st = S_DATA; i += 3; continue; }
            if (raw_at(t, i, "<script", true)) { st = S_DBL; i += 7; continue; }
            if (raw_at(t, i, "</script", true)) return i;
            i++;
        } else {
            if (raw_at(t, i, "</script", true)) { st = S_ESC; i += 8; continue; }
            if (raw_at(t, i, "<script", true)) { i += 7; continue; }
            if (raw_at(t, i, "-->", false)) { st = S_ESC; i += 3; continue; }
            i++;
        }
    }
    return t->len;
}

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
    /* raw_frag: 終端を探さず EOF まで（appropriate end tag 非合致、13.4） */
    u32 end = t->raw_frag ? t->len
                          : ((t->raw_tag == IF_TAG_SCRIPT) ? if_find_script_end(t)
                                                           : if_find_raw_end(t));
    if (end > t->pos) {
        tok.kind = TOK_TEXT;
        /* RCDATA(title/textarea) は文字参照を解決する。rawtext(style/script) は生のまま。
         * どちらも U+0000 は U+FFFD に置換する（WHATWG rawtext/rcdata 規則） */
        tok.text = t->raw_rcdata ? if_resolved(t, t->pos, end)
                                 : if_str((const char *)t->src + t->pos, end - t->pos);
        tok.text = if_fix_nul(t, tok.text, true);
        /* rawtext/rcdata: tokenizer が U+0000 を U+FFFD 化するので NUL も実文字 */
        tok.text_had_real = !if_str_is_ws_only(tok.text);
        t->pos = end;
        return tok;
    }
    t->raw_tag = 0; /* 終了タグは通常の字句解析へ */
    return if_tok_next(t);
}

/* ---- 本体 ---- */

/* 確定時に属性配列を 1 回だけ arena に正確確保するための最終コピー。
 * 旧構造は if_arena_grow でトークンごとに中間配列を確保し、成長元ブロックが
 * ページ arena に残留し続けた（実測で parse stage の ~150MB 存在分の主因。
 * 巨大文書では ~100-190MB が死蔵していた） */
#define IF_ATTR_STACK_CAP 32u
static IfAttr *attrs_finish(IfHtmlTok *t, const IfAttr *attrs, u32 n, bool spilled) {
    if (spilled) return (IfAttr *)attrs; /* 稀な巨大属性タグの grow 路はそのまま所有 */
    if (!n) return NULL;
    IfAttr *fin = (IfAttr *)if_arena_alloc(t->arena, (u64)n * sizeof(IfAttr));
    memcpy(fin, attrs, (u64)n * sizeof(IfAttr));
    return fin;
}

static IfTok if_tag_token(IfHtmlTok *t, bool is_end) {
    IfTok tok = { .kind = TOK_EOF };
    tok.kind = is_end ? TOK_END : TOK_START;

    IfAttr sbuf[IF_ATTR_STACK_CAP];
    IfAttr *attrs = sbuf;
    u32 n_attrs = 0;
    u32 cap = IF_ATTR_STACK_CAP;
    u64 acap = 0;          /* spill 路の arena 容量（if_arena_grow 規約） */
    bool spilled = false;

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
        if (c == '>') { t->pos++; tok.attrs = attrs_finish(t, attrs, n_attrs, spilled); tok.n_attrs = n_attrs; return tok; }
        if (c == '/') {
            t->pos++;
            if (t->pos < t->len && t->src[t->pos] == '>') {
                t->pos++;
                tok.self_closing = true;
                tok.attrs = attrs_finish(t, attrs, n_attrs, spilled);
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
        /* 属性名は tokenizer の attribute-name 章則どおり ASCII lowercase に正規化。
         * （不変なら arena 割当を避けて原スライスのままにする省メモリ経路） */
        for (u32 li = 0; li < aname.n; li++) {
            u8 ac = (u8)aname.p[li];
            if (ac >= 'A' && ac <= 'Z') {
                char *lc = (char *)if_arena_alloc(t->arena, aname.n);
                for (u32 lj = 0; lj < aname.n; lj++) lc[lj] = (char)if_ascii_lower((u8)aname.p[lj]);
                aname = if_str(lc, aname.n);
                break;
            }
        }
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
                    t->in_attr_ctx = 1;
                    aval = if_fix_nul(t, if_resolved(t, vs, t->pos), true);
                    t->in_attr_ctx = 0;
                    if (t->pos < t->len) t->pos++; /* 閉じクォート */
                    else t->errors++;
                } else {
                    u32 vs = t->pos;
                    while (t->pos < t->len && !if_hws(t->src[t->pos]) && t->src[t->pos] != '>') t->pos++;
                    t->in_attr_ctx = 1;
                    aval = if_fix_nul(t, if_resolved(t, vs, t->pos), true);
                    t->in_attr_ctx = 0;
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
        if (n_attrs >= cap) {
            if (!spilled) {
                /* スタック枠 (32) を超える稀なタグのみ arena grow 路へ移行
                 * （中間ブロックは残留するが、攻撃者も属性バイトを支払うため増幅は線形に閉じる） */
                spilled = true;
                acap = 64;
                IfAttr *na = (IfAttr *)if_arena_alloc(t->arena, acap * sizeof(IfAttr));
                memcpy(na, attrs, (u64)n_attrs * sizeof(IfAttr));
                attrs = na;
                cap = 64;
            } else {
                attrs = (IfAttr *)if_arena_grow(t->arena, attrs, &acap, n_attrs + 1, sizeof(IfAttr));
                cap = (u32)(acap < 0xFFFFFFFFu ? acap : 0xFFFFFFFFu);
            }
        }
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
        /* "-->" または abrupt な "--!>"（abrupt-closing-of-empty-comment）を探す
         * （j+2/j+3 は len-1 までに制限して境界外アクセスを防ぐ） */
        u32 start = t->pos;
        u32 j = start;
        while (j + 2 < t->len) {
            if (t->src[j] == '-' && t->src[j + 1] == '-') {
                if (t->src[j + 2] == '>') {
                    tok.kind = TOK_COMMENT;
                    tok.text = if_fix_nul(t, if_str((const char *)t->src + start, j - start), true);
                    t->pos = j + 3;
                    return tok;
                }
                if (t->src[j + 2] == '!' && j + 3 < t->len && t->src[j + 3] == '>') {
                    t->errors++; /* comment end bang state の '>' → abrupt close */
                    tok.kind = TOK_COMMENT;
                    tok.text = if_fix_nul(t, if_str((const char *)t->src + start, j - start), true);
                    t->pos = j + 4;
                    return tok;
                }
            }
            j++;
        }
        t->errors++; /* 閉じられないコメント EOF: comment end / end dash state の
                      * 「保留ダッシュ」（末尾連続 '-' の先頭側から最大 2 個）は
                      * data に付かない（spec: その状態の EOF は保留分を吐かず emit） */
        u32 cend = t->len;
        if (cend > start && t->src[cend - 1] == '-') {
            cend--;
            if (cend > start && t->src[cend - 1] == '-') cend--;
        }
        tok.kind = TOK_COMMENT;
        tok.text = if_fix_nul(t, if_str((const char *)t->src + start, cend - start), true);
        t->pos = t->len;
        return tok;
    }

    /* "<![CDATA[": adjusted current node が非 HTML 名前空間なら CDATA section（テキスト）、
     * そうでなければ bogus comment（spec markup declaration open state 厳密:
     * integration point（svg title/foreignObject/desc, math mtext 等）は node 自体が
     * 非 HTML ns なので CDATA になる点に注意 — tree が立てる adcn_foreign 旗を見る） */
    if (t->adcn_foreign && t->pos + 7 <= t->len &&
        memcmp(t->src + t->pos, "[CDATA[", 7) == 0) {
        u32 start = t->pos + 7;
        u32 j = start;
        while (j + 2 < t->len &&
               !(t->src[j] == ']' && t->src[j + 1] == ']' && t->src[j + 2] == '>')) j++;
        IfTok cdt = { .kind = TOK_TEXT };
        if (j + 2 < t->len) {
            IfStr raw = if_str((const char *)t->src + start, j - start);
            cdt.text_had_real = if_tok_real_text(raw); /* NUL は U+0000 のまま届く経路 */
            cdt.text = if_fix_nul(t, raw, true);
            t->pos = j + 3;
        } else {
            t->errors++; /* 閉じられない CDATA: 残り全部をテキストに */
            IfStr raw = if_str((const char *)t->src + start, t->len - start);
            cdt.text_had_real = if_tok_real_text(raw);
            cdt.text = if_fix_nul(t, raw, true);
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
    tok.text = if_fix_nul(t, body, true); /* U+0000 → U+FFFD（bogus comment 規則） */
    return tok;
}

IfTok if_tok_next(IfHtmlTok *t) {
    IfTok tok = { .kind = TOK_EOF };

    /* <plaintext>: 残り全入力を 1 個の TEXT として返す（文字参照も解決しない。
     * U+0000 は U+FFFD へ置換（WHATWG plaintext 規則）） */
    if (t->plaintext && t->pos < t->len) {
        tok.kind = TOK_TEXT;
        tok.text = if_str((const char *)t->src + t->pos, t->len - t->pos);
        tok.text = if_fix_nul(t, tok.text, true);
        /* plaintext: tokenizer が U+0000 を U+FFFD 化するので NUL も実文字 */
        tok.text_had_real = !if_str_is_ws_only(tok.text);
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
        /* data text の U+0000: HTML content では "ignore"（in-body の parse error
         * 規則）だが、foreign content では U+FFFD 挿入が規則。tree が立てる
         * cdata_foreign 旗（foreign 内 text/CDATA で 1）で切り替える。 */
        /* text_had_real は「変換前」の解決済み列から計算する: foreign で FFFD 化
         * される生 NUL は frameset-ok を倒さないのが spec（&#0; 由来の FFFD や
         * ソース中の実 FFFD は実文字＝倒れる；変換後判定ではこの区別が不可能） */
        IfStr rs = if_resolved(t, start, t->pos);
        tok.text_had_real = if_tok_real_text(rs);
        tok.text = if_fix_nul(t, rs, t->cdata_foreign != 0);
        return tok;
    }

    /* '<' の処理 */
    if (t->pos + 1 >= t->len) {
        /* 孤立 '<' で終端 → テキストとして返す */
        tok.kind = TOK_TEXT;
        tok.text = if_str("<", 1);
        tok.text_had_real = 1;
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
        if (t->pos + 2 >= t->len) {
            /* "</" で EOF: end tag open state の EOF 規則 — parse error で
             * テキスト "</" を吐く（bogus comment にはしない: tests1#37） */
            tok.kind = TOK_TEXT;
            tok.text = if_str("</", 2);
            tok.text_had_real = 1;
            t->pos = t->len;
            t->errors++;
            return tok;
        }
        if (if_alpha(t->src[t->pos + 2])) {
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
        tok.text = if_fix_nul(t, if_str((const char *)t->src + start, t->pos - start), true);
        if (t->pos < t->len) t->pos++;
        t->errors++;
        return tok;
    }

    /* '<' + その他 → '<' はリテラルテキスト */
    tok.kind = TOK_TEXT;
    tok.text = if_str("<", 1);
    tok.text_had_real = 1;
    t->pos += 1;
    t->errors++;
    return tok;
}
