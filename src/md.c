/* Ifuto — Markdown → HTML 変換器 v0.2（C11, no deps）
 *
 * 出力は決定的（テストが文字列一致できる）: 各ブロックは 1 行で閉じ、
 * ブロック間に "\n" を 1 つ挟む。
 *
 * メモリ則: 入力は 1 回の線形走査、行はポインタ切片（コピーなし）。脚注の
 * 定義・参照だけが小さい動的配列（arena）を持つ。
 * パースの強靱性: 全関数で入力長チェック、引用の再帰は depth ≤ 8 で打ち切り、
 * リスト入れ子は 16 段で飽和（以後はフラット化、台帳）。破損入力でも無限ループ
 * しない（各行処理は必ず前行消費）。
 */
#include "md.h"
#include "strutil.h"
#include <stdio.h>
#include <string.h>

/* ---- 出力ビルダ ---- */
typedef struct { IfArena *a; char *p; u64 n, cap; } B;

static void b_init(B *b, IfArena *a) { b->a = a; b->p = NULL; b->n = 0; b->cap = 0; }
static void b_putn(B *b, const char *s, u64 n) {
    if (b->n + n + 1 > b->cap) {
        u64 nc = b->cap ? b->cap : 256;
        while (nc < b->n + n + 1) nc *= 2;
        char *np = (char *)if_arena_alloc(b->a, nc);
        if (b->n) memcpy(np, b->p, b->n);
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, s, (size_t)n);
    b->n += n; b->p[b->n] = 0;
}
static void b_puts(B *b, const char *s) { b_putn(b, s, (u64)strlen(s)); }
static void b_putc(B *b, char c) { b_putn(b, &c, 1); }

/* テキスト/属性の escape（必須3文字）。attr は追加で '"' */
static void esc_text(B *b, IfStr s) {
    for (u32 i = 0; i < s.n; i++) {
        char c = s.p[i];
        if (c == '&') b_puts(b, "&amp;");
        else if (c == '<') b_puts(b, "&lt;");
        else if (c == '>') b_puts(b, "&gt;");
        else b_putc(b, c);
    }
}
static void esc_attr(B *b, IfStr s) {
    for (u32 i = 0; i < s.n; i++) {
        char c = s.p[i];
        if (c == '&') b_puts(b, "&amp;");
        else if (c == '<') b_puts(b, "&lt;");
        else if (c == '"') b_puts(b, "&quot;");
        else b_putc(b, c);
    }
}

/* ---- 行走査 ---- */
typedef struct { const char *p; u32 n; } Ln; /* LF を含まない行（CR は前端で剥がす） */

static bool ln_blank(Ln l) {
    for (u32 i = 0; i < l.n; i++)
        if (l.p[i] != ' ' && l.p[i] != '\t') return false;
    return true;
}

bool if_path_is_md(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    if (if_str_eq_ci(if_str(dot, (u32)strlen(dot)), if_str(".md", 3))) return true;
    return if_str_eq_ci(if_str(dot, (u32)strlen(dot)), if_str(".markdown", 9));
}

/* ---- 脚注 ---- */
typedef struct { IfStr id; IfStr text; } FnDef;
typedef struct { IfStr id; } FnRefCtx;
typedef struct {
    IfArena *a;
    FnDef *defs; u32 n_defs, cap_defs;
    IfStr *refs; u32 n_refs, cap_refs; /* 参照順の id（重複なし） */
} Fn;

static u32 fn_find_def(Fn *f, IfStr id) {
    for (u32 i = 0; i < f->n_defs; i++) if (if_str_eq(f->defs[i].id, id)) return i;
    return UINT32_MAX;
}
/* 参照順 numbering: 既出でなければ refs に追加して番号（1-based）を返す */
static u32 fn_ref_number(Fn *f, IfStr id) {
    for (u32 i = 0; i < f->n_refs; i++) if (if_str_eq(f->refs[i], id)) return i + 1;
    if (f->n_refs >= f->cap_refs) {
        u64 cap = f->cap_refs;
        f->refs = (IfStr *)if_arena_grow(f->a, f->refs, &cap, f->n_refs + 1, sizeof(IfStr));
        f->cap_refs = (u32)cap;
    }
    f->refs[f->n_refs++] = id;
    return f->n_refs;
}
static void fn_add_def(Fn *f, IfStr id, IfStr text) {
    if (fn_find_def(f, id) != UINT32_MAX) return; /* 先勝ち（CommonMark） */
    if (f->n_defs >= f->cap_defs) {
        u64 cap = f->cap_defs;
        f->defs = (FnDef *)if_arena_grow(f->a, f->defs, &cap, f->n_defs + 1, sizeof(FnDef));
        f->cap_defs = (u32)cap;
    }
    f->defs[f->n_defs].id = id;
    f->defs[f->n_defs].text = text;
    f->n_defs++;
}

/* ---- inline 展開 ---- */
static void inline_span(B *out, Fn *fn, IfStr s);

/* 閉じ区切りを探す（delim は "**", "*", "__", "_", "~~", "`" 等）。戻り値: 終了位置 or 負 */
static i32 find_close(IfStr s, u32 from, const char *delim, u32 dn) {
    for (u32 i = from; i + dn <= s.n; i++) {
        if (s.p[i] == '\\') { i++; continue; } /* escape 内の区切りは無視 */
        if (memcmp(s.p + i, delim, dn) == 0) return (i32)i;
    }
    return -1;
}

static void emit_fmt(B *out, Fn *fn, const char *tag_open, const char *tag_close,
                     IfStr inner) {
    b_puts(out, tag_open);
    inline_span(out, fn, inner);
    b_puts(out, tag_close);
}

/* "[text](dest)" / "![alt](dest)" / "[^id]" / "<http://>" の判定を 1 箇所で */
static bool try_link(B *out, Fn *fn, IfStr s, u32 *adv) {
    /* s.p[0] == '['（画像なら直前が '!'、呼出側が捌く） */
    u32 i = 1;
    if (i < s.n && s.p[i] == '^') { /* footnote ref */
        u32 is = ++i;
        while (i < s.n && s.p[i] != ']') i++;
        if (i >= s.n) return false;
        IfStr id = if_str(s.p + is, i - is);
        if (fn && id.n) {
            /* 同一脚色への n 回目の参照: id を fr-id-n で一意化（番号は共通） */
            u32 seen = 0;
            for (u32 r = 0; r < fn->n_refs; r++) if (if_str_eq(fn->refs[r], id)) seen++;
            u32 num = fn_ref_number(fn, id);
            char nb[24];
            snprintf(nb, sizeof nb, "%u", num);
            b_puts(out, "<sup><a href=\"#fn-");
            esc_attr(out, id);
            b_puts(out, "\" id=\"fr-");
            esc_attr(out, id);
            if (seen) {
                char sb[24];
                snprintf(sb, sizeof sb, "-%u", seen + 1);
                b_puts(out, sb);
            }
            b_puts(out, "\">");
            b_puts(out, nb);
            b_puts(out, "</a></sup>");
            *adv = i + 1;
            return true;
        }
        return false;
    }
    /* 通常リンク: 対応 ] を探す（入れ子 [ ] は深さ勘定） */
    u32 depth = 1, ts = i;
    while (i < s.n && depth) {
        if (s.p[i] == '\\') { i += 2; continue; }
        if (s.p[i] == '[') depth++;
        else if (s.p[i] == ']') depth--;
        i++;
    }
    if (depth || i >= s.n || s.p[i] != '(') return false;
    IfStr text = if_str(s.p + ts, i - ts - 1);
    u32 ds = ++i;
    while (i < s.n && s.p[i] != ')') i++;
    if (i >= s.n) return false;
    IfStr dest = if_str(s.p + ds, i - ds);
    b_puts(out, "<a href=\"");
    esc_attr(out, dest);
    b_puts(out, "\">");
    inline_span(out, fn, text);
    b_puts(out, "</a>");
    *adv = i + 1;
    return true;
}

static void inline_span(B *out, Fn *fn, IfStr s) {
    for (u32 i = 0; i < s.n;) {
        char c = s.p[i];
        if (c == '\\' && i + 1 < s.n) { /* escape: 後続句読点をリテラル化 */
            char n2 = s.p[i + 1];
            if (strchr("\\`*_{}[]()#+-.!~|<>", n2)) { b_putc(out, n2); i += 2; continue; }
        }
        if (c == '`') { /* インラインコード: 連続バックティックの数だけ要求 */
            u32 run = 1;
            while (i + run < s.n && s.p[i + run] == '`') run++;
            char delim[4] = "``";
            i32 close = -1;
            for (u32 j = i + run; j + run <= s.n; j++) {
                if (memcmp(s.p + j, delim, run <= 2 ? run : 2) == 0 && run <= 2) { close = (i32)j; break; }
                if (s.p[j] == '`' && run > 2) { close = (i32)j; break; } /* 3+ は単一 ` で近似閉鎖（台帳） */
            }
            if (close >= 0) {
                b_puts(out, "<code>");
                esc_text(out, if_str(s.p + i + run, (u32)close - (i + run)));
                b_puts(out, "</code>");
                i = (u32)close + run;
                continue;
            }
        }
        if ((c == '*' || c == '_') && i + 1 < s.n && s.p[i + 1] == c) {
            i32 close = find_close(s, i + 2, c == '*' ? "**" : "__", 2);
            if (close > (i32)i + 2) {
                emit_fmt(out, fn, "<strong>", "</strong>", if_str(s.p + i + 2, (u32)close - i - 2));
                i = (u32)close + 2;
                continue;
            }
        }
        if (c == '*' || c == '_') {
            i32 close = find_close(s, i + 1, c == '*' ? "*" : "_", 1);
            if (close > (i32)i + 1) {
                emit_fmt(out, fn, "<em>", "</em>", if_str(s.p + i + 1, (u32)close - i - 1));
                i = (u32)close + 1;
                continue;
            }
        }
        if (c == '~' && i + 1 < s.n && s.p[i + 1] == '~') {
            i32 close = find_close(s, i + 2, "~~", 2);
            if (close > (i32)i + 2) {
                emit_fmt(out, fn, "<del>", "</del>", if_str(s.p + i + 2, (u32)close - i - 2));
                i = (u32)close + 2;
                continue;
            }
        }
        if (c == '!' && i + 1 < s.n && s.p[i + 1] == '[') {
            /* ![alt](src): alt は装飾展開しないプレーンテキスト */
            IfStr rest = if_str(s.p + i + 1, s.n - i - 1);
            u32 k = 1, adv0 = 0;
            while (k < rest.n) {
                if (rest.p[k] == '\\') { k += 2; continue; }
                if (rest.p[k] == ']') break;
                k++;
            }
            if (k < rest.n && k + 1 < rest.n && rest.p[k + 1] == '(') {
                u32 ds = k + 2, ke = ds;
                while (ke < rest.n && rest.p[ke] != ')') ke++;
                if (ke < rest.n) {
                    b_puts(out, "<img src=\"");
                    esc_attr(out, if_str(rest.p + ds, ke - ds));
                    b_puts(out, "\" alt=\"");
                    esc_attr(out, if_str(rest.p + 1, k - 1));
                    b_puts(out, "\">");
                    adv0 = ke + 1;
                }
            }
            if (adv0) { i = i + 1 + adv0; continue; }
        }
        if (c == '[') {
            u32 adv = 0;
            if (try_link(out, fn, if_str(s.p + i, s.n - i), &adv)) { i += adv; continue; }
        }
        if (c == '<') { /* 自動リンク <http://…> / <https://…> */
            u32 j = i + 1;
            while (j < s.n && s.p[j] != '>') j++;
            if (j < s.n) {
                IfStr url = if_str(s.p + i + 1, j - i - 1);
                if ((url.n > 7 && memcmp(url.p, "http://", 7) == 0) ||
                    (url.n > 8 && memcmp(url.p, "https://", 8) == 0)) {
                    b_puts(out, "<a href=\"");
                    esc_attr(out, url);
                    b_puts(out, "\">");
                    esc_text(out, url);
                    b_puts(out, "</a>");
                    i = j + 1;
                    continue;
                }
            }
        }
        /* 通常文字: escape 3 原則のみ */
        if (c == '&') { b_puts(out, "&amp;"); i++; continue; }
        if (c == '<') { b_puts(out, "&lt;"); i++; continue; }
        if (c == '>') { b_puts(out, "&gt;"); i++; continue; }
        b_putc(out, c);
        i++;
    }
}

/* ---- ブロック判定 ---- */
static int ln_heading(Ln l) { /* ATX: 1..6 個の # + 空白。戻り値レベル or 0 */
    u32 i = 0;
    while (i < l.n && l.p[i] == '#' && i < 6) i++;
    if (i == 0 || i >= l.n || (l.p[i] != ' ' && l.p[i] != '\t')) return 0;
    return (int)i;
}

static bool ln_is_hr(Ln l) {
    u32 i = 0;
    while (i < l.n && (l.p[i] == ' ' || l.p[i] == '\t')) i++;
    if (i >= l.n) return false;
    char c = l.p[i];
    if (c != '-' && c != '*' && c != '_') return false;
    u32 cnt = 0;
    for (; i < l.n; i++) {
        if (l.p[i] == c) cnt++;
        else if (l.p[i] != ' ' && l.p[i] != '\t') return false;
    }
    return cnt >= 3;
}

/* フェンス: ``` または ~~~ で始まる（3 個以上）。終了判定と同じ記号 */
static u32 ln_fence(Ln l, char *sym) {
    u32 i = 0;
    while (i < l.n && (l.p[i] == ' ' || l.p[i] == '\t')) i++;
    if (i >= l.n || (l.p[i] != '`' && l.p[i] != '~')) return 0;
    char c = l.p[i];
    u32 run = 0;
    while (i < l.n && l.p[i] == c) { run++; i++; }
    if (run < 3) return 0;
    *sym = c;
    return run;
}

static u32 ln_quote(Ln l) { /* "> " 剥がし幅（0 = 引用でない） */
    u32 i = 0;
    while (i < l.n && l.p[i] == ' ') i++;
    if (i >= l.n || l.p[i] != '>') return 0;
    i++;
    if (i < l.n && l.p[i] == ' ') i++;
    return i;
}

/* リスト項目: (空白の深さ, マーカー幅, ordered?) を返す。非項目は depth=-1 */
typedef struct { u32 indent; u32 mwidth; bool ordered; } LiMark;
static bool ln_list_item(Ln l, LiMark *m) {
    u32 i = 0;
    while (i < l.n && l.p[i] == ' ') i++;
    if (i >= l.n) return false;
    if (l.p[i] == '-' || l.p[i] == '*' || l.p[i] == '+') {
        if (i + 1 < l.n && (l.p[i + 1] == ' ' || l.p[i + 1] == '\t')) {
            m->indent = i; m->mwidth = i + 2; m->ordered = false;
            return true;
        }
        return false;
    }
    u32 ds = i;
    while (i < l.n && l.p[i] >= '0' && l.p[i] <= '9') i++;
    if (i > ds && i - ds <= 9 && i < l.n && (l.p[i] == '.' || l.p[i] == ')') &&
        i + 1 < l.n && (l.p[i + 1] == ' ' || l.p[i + 1] == '\t')) {
        m->indent = ds; m->mwidth = i + 2; m->ordered = true;
        return true;
    }
    return false;
}

/* 脚注定義行: "[^id]: text" */
static bool ln_fndef(Ln l, IfStr *id, IfStr *text) {
    if (l.n < 5 || l.p[0] != '[' || l.p[1] != '^') return false;
    u32 i = 2;
    while (i < l.n && l.p[i] != ']') i++;
    if (i >= l.n || i + 1 >= l.n || l.p[i + 1] != ':') return false;
    u32 ts = i + 2;
    while (ts < l.n && (l.p[ts] == ' ' || l.p[ts] == '\t')) ts++;
    *id = if_str(l.p + 2, i - 2);
    *text = if_str(l.p + ts, l.n - ts);
    return id->n != 0;
}

/* GFM 表: 現在行 + 区切り行判定用。セル分割は '|'（`|a|b|` と `a|b` 両受け） */
static u32 split_cells(Ln l, IfStr *cells, u32 cap) {
    u32 i = 0, n = 0;
    while (i < l.n && (l.p[i] == ' ' || l.p[i] == '\t')) i++;
    if (i < l.n && l.p[i] == '|') i++;
    u32 st = i;
    for (; i <= l.n; i++) {
        if (i == l.n || l.p[i] == '|') {
            u32 e = i;
            IfStr c = if_str(l.p + st, e - st);
            c = if_str_trim(c);
            if (n < cap) cells[n] = c;
            n++;
            st = i + 1;
        }
    }
    /* 末尾の空セル（終端 '|'）を落とす */
    if (n && cells[n - 1].n == 0) n--;
    return n;
}

static bool ln_is_table_delim(Ln l) {
    /* | --- | :--: | 系。1 つ以上のセル、各セルは :?-+:? のみ */
    IfStr cells[32];
    u32 n = split_cells(l, cells, 32);
    if (!n) return false;
    for (u32 i = 0; i < n; i++) {
        IfStr c = cells[i];
        u32 j = 0;
        if (j < c.n && c.p[j] == ':') j++;
        u32 ds = j;
        while (j < c.n && c.p[j] == '-') j++;
        if (j - ds < 3) return false; /* 最低 3 ハイフン（GFM 規則） */
        if (j < c.n && c.p[j] == ':') j++;
        if (j != c.n) return false;
    }
    return true;
}

/* ---- ブロック生成（行配列 + 窓再帰。引用のみデンデンバッファで再帰） ----
 * 設計: 全行を一次元 Ln 配列に割り切り、[lo,hi) の窓でブロック列を処理する。
 * 入れ子リストは「深い行の連続窓」なのでコピー不要。引用だけは '>' 除去のため
 * デンデン済みテキストを arena に作り (depth+1 で本文 parser を再起動)。 */
static void blocks_win(B *out, Fn *fn, Ln *ls, u32 lo, u32 hi, u32 depth);
static void blocks_str(B *out, Fn *fn, IfStr s, u32 depth);

/* 段落: 連結は「スペース」、前行の末尾 2 空白（ハードブレーク）なら <br> で接続 */
static void emit_para_lines(B *out, Fn *fn, Ln *ls, u32 lo, u32 hi) {
    b_puts(out, "<p>");
    bool prev_hard = false;
    for (u32 i = lo; i < hi; i++) {
        IfStr x = if_str(ls[i].p, ls[i].n);
        u32 trail = 0;
        while (trail < x.n && x.p[x.n - 1 - trail] == ' ') trail++;
        bool hard = trail >= 2;
        if (hard) x.n -= trail;
        if (i > lo) b_puts(out, prev_hard ? "<br>" : " ");
        inline_span(out, fn, x);
        prev_hard = hard;
    }
    b_puts(out, "</p>\n");
}

static u32 ln_indent(Ln l) {
    u32 i = 0;
    while (i < l.n && l.p[i] == ' ') i++;
    return i;
}

static void blocks_win(B *out, Fn *fn, Ln *ls, u32 lo, u32 hi, u32 depth) {
    u32 i = lo;
    while (i < hi) {
        Ln l = ls[i];
        if (ln_blank(l)) { i++; continue; }
        /* 脚注定義は収集のみ（ブロックを出さない） */
        IfStr fid, ftx;
        if (ln_fndef(l, &fid, &ftx)) { fn_add_def(fn, fid, ftx); i++; continue; }
        int hh = ln_heading(l);
        if (hh) {
            char tag[16];
            snprintf(tag, sizeof tag, "<h%d>", hh);
            b_puts(out, tag);
            u32 k = (u32)hh;
            while (k < l.n && (l.p[k] == ' ' || l.p[k] == '\t')) k++;
            IfStr t = if_str(l.p + k, l.n - k);
            i32 e = (i32)t.n - 1;
            while (e >= 0 && (t.p[e] == ' ' || t.p[e] == '\t')) e--;
            i32 he = e;
            while (he >= 0 && t.p[he] == '#') he--;
            if (he < e && he >= 0 && (t.p[he] == ' ' || t.p[he] == '\t')) t.n = (u32)he;
            else t.n = (u32)(e + 1);
            inline_span(out, fn, t);
            snprintf(tag, sizeof tag, "</h%d>\n", hh);
            b_puts(out, tag);
            i++;
            continue;
        }
        if (ln_is_hr(l)) { b_puts(out, "<hr>\n"); i++; continue; }
        char fsym;
        if (ln_fence(l, &fsym)) {
            u32 k = 0;
            while (k < l.n && l.p[k] == fsym) k++;
            while (k < l.n && (l.p[k] == ' ' || l.p[k] == '\t')) k++;
            IfStr lang = if_str(l.p + k, l.n - k);
            b_puts(out, "<pre><code");
            if (lang.n) {
                b_puts(out, " class=\"lang-");
                esc_attr(out, lang);
                b_putc(out, '"');
            }
            b_putc(out, '>');
            i++;
            while (i < hi) {
                Ln cl = ls[i];
                char s2;
                if (ln_fence(cl, &s2) && s2 == fsym) { i++; break; } /* 閉鎖（未閉鎖は EOF 閉鎖） */
                esc_text(out, if_str(cl.p, cl.n));
                b_putc(out, '\n');
                i++;
            }
            b_puts(out, "</code></pre>\n");
            continue;
        }
        u32 q = ln_quote(l);
        if (q) {
            /* 連続（'>' で始まる限り。空行も '>' 継続なら取り込む）をデンデンして再帰 */
            B qb; b_init(&qb, out->a);
            if (depth < 8) {
                while (i < hi) {
                    u32 w = ln_quote(ls[i]);
                    if (!w) break;
                    Ln x = { ls[i].p + w, ls[i].n - w };
                    b_putn(&qb, x.p, x.n);
                    b_putc(&qb, '\n');
                    i++;
                }
                b_puts(out, "<blockquote>\n");
                blocks_str(out, fn, if_str(qb.p ? qb.p : "", (u32)qb.n), depth + 1);
                b_puts(out, "</blockquote>\n");
            } else {
                /* 深度飽和: 記号を剥がした段落に落とす（敵対防御・台帳） */
                u32 j = i;
                while (j < hi && ln_quote(ls[j])) j++;
                B flat; b_init(&flat, out->a);
                for (u32 k2 = i; k2 < j; k2++) {
                    Ln x = { ls[k2].p + ln_quote(ls[k2]), ls[k2].n - ln_quote(ls[k2]) };
                    b_putn(&flat, x.p, x.n);
                    b_putc(&flat, '\n');
                }
                b_puts(out, "<blockquote>\n<p>");
                inline_span(out, fn, if_str(flat.p ? flat.p : "", (u32)flat.n));
                b_puts(out, "</p>\n</blockquote>\n");
                i = j;
            }
            continue;
        }
        LiMark mk;
        if (ln_list_item(l, &mk)) {
            bool ordered = mk.ordered;
            u32 base = mk.indent;
            b_puts(out, ordered ? "<ol>\n" : "<ul>\n");
            while (i < hi) {
                LiMark m2;
                if (!ln_list_item(ls[i], &m2) || m2.ordered != ordered || m2.indent != base) break;
                b_puts(out, "<li>");
                inline_span(out, fn, if_str(ls[i].p + m2.mwidth, ls[i].n - m2.mwidth));
                i++;
                /* 入れ子: 連続する「ベース深より深い行」(空行止まり) を窓再帰 */
                u32 j = i;
                while (j < hi && !ln_blank(ls[j]) && ln_indent(ls[j]) > base) j++;
                if (j > i) {
                    blocks_win(out, fn, ls, i, j, depth);
                    i = j;
                }
                b_puts(out, "</li>\n");
            }
            b_puts(out, ordered ? "</ol>\n" : "</ul>\n");
            continue;
        }
        /* GFM 表: 現行に '|' + 次行が区切り行 */
        bool has_pipe = false;
        for (u32 k = 0; k < l.n; k++) if (l.p[k] == '|') { has_pipe = true; break; }
        if (has_pipe && i + 1 < hi && ln_is_table_delim(ls[i + 1])) {
            IfStr heads[32];
            u32 nh = split_cells(l, heads, 32);
            b_puts(out, "<table>\n<thead><tr>");
            for (u32 k2 = 0; k2 < nh; k2++) {
                b_puts(out, "<th>");
                inline_span(out, fn, heads[k2]);
                b_puts(out, "</th>");
            }
            b_puts(out, "</tr></thead>\n<tbody>\n");
            i += 2;
            while (i < hi && !ln_blank(ls[i])) {
                bool pipe2 = false;
                for (u32 k = 0; k < ls[i].n; k++) if (ls[i].p[k] == '|') { pipe2 = true; break; }
                if (!pipe2) break;
                IfStr cells[32];
                u32 nc = split_cells(ls[i], cells, 32);
                b_puts(out, "<tr>");
                for (u32 k2 = 0; k2 < nc; k2++) {
                    b_puts(out, "<td>");
                    inline_span(out, fn, cells[k2]);
                    b_puts(out, "</td>");
                }
                b_puts(out, "</tr>\n");
                i++;
            }
            b_puts(out, "</tbody>\n</table>\n");
            continue;
        }
        /* 段落: ブロック開始条件に触れない行の連続を集める */
        u32 j = i;
        while (j < hi) {
            Ln x = ls[j];
            if (ln_blank(x)) break;
            if (j > i) { /* 先行行は段落継続の対象外（CommonMark の interrupt 規則近似） */
                IfStr i2d, i2t;
                if (ln_heading(x) || ln_is_hr(x) || ln_quote(x) || ln_fndef(x, &i2d, &i2t)) break;
                char s3; LiMark m3;
                if (ln_fence(x, &s3)) break;
                if (ln_list_item(x, &m3)) break;
                if (ln_is_table_delim(x)) break;
                for (u32 k = 0; k < x.n; k++) if (x.p[k] == '|') { /* 表開始判定は次行必要なので保守的に継続 */ (void)0; break; }
            }
            j++;
        }
        emit_para_lines(out, fn, ls, i, j);
        i = j;
    }
}

static void blocks_str(B *out, Fn *fn, IfStr s, u32 depth) {
    /* 行配列へ割り切る（入力を切片化、コピーなし） */
    Ln *ls = NULL; u32 n = 0, cap = 0;
    u32 st = 0;
    for (u32 p = 0; p <= s.n; p++) {
        if (p == s.n || s.p[p] == '\n') {
            if (p == s.n && st == s.n && s.n) break; /* 終端 LF の幻空行は行と数えない */
            if (n >= cap) {
                u64 c2 = cap;
                ls = (Ln *)if_arena_grow(out->a, ls, &c2, n + 1, sizeof(Ln));
                cap = (u32)c2;
            }
            ls[n].p = s.p + st;
            ls[n].n = p - st;
            n++;
            st = p + 1;
        }
    }
    blocks_win(out, fn, ls, 0, n, depth);
}

void if_md_to_html(IfArena *a, IfStr in, IfStr *out_html) {
    /* 入力正規化: CR/CRLF → LF（仕様の preprocess に相当） */
    B norm; b_init(&norm, a);
    for (u32 i = 0; i < in.n; i++) {
        char cc = in.p[i];
        if (cc == '\r') {
            if (i + 1 < in.n && in.p[i + 1] == '\n') i++;
            b_putc(&norm, '\n');
        } else b_putc(&norm, cc);
    }
    Fn fn;
    memset(&fn, 0, sizeof fn);
    fn.a = a;
    B out; b_init(&out, a);
    blocks_str(&out, &fn, if_str(norm.p ? norm.p : "", (u32)norm.n), 0);
    /* 脚注セクション（参照されたものだけ、参照順） */
    if (fn.n_refs) {
        b_puts(&out, "<section class=\"footnotes\">\n<hr>\n<ol>\n");
        for (u32 i = 0; i < fn.n_refs; i++) {
            u32 di = fn_find_def(&fn, fn.refs[i]);
            b_puts(&out, "<li id=\"fn-");
            esc_attr(&out, fn.refs[i]);
            b_puts(&out, "\">");
            IfStr txt = di != UINT32_MAX ? fn.defs[di].text : if_str("", 0);
            inline_span(&out, &fn, txt);
            b_puts(&out, " <a href=\"#fr-");
            esc_attr(&out, fn.refs[i]);
            b_puts(&out, "\">↩</a>");
            b_puts(&out, "</li>\n");
        }
        b_puts(&out, "</ol>\n</section>\n");
    }
    out_html->p = out.p ? out.p : "";
    out_html->n = (u32)out.n;
}
