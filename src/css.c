/* Ifuto — CSS サブセット実装。
 * パーサは「壊れた部分を捨てて次に進む」規律で書く。止まらない・正確に消費する・曖昧を残さない。
 */
#include "css.h"
#include <string.h>
#include <stdlib.h>

/* ================= レキサ的補助 ================= */

typedef struct { const char *p; u32 n; u32 i; } IfCur;

static bool c_ws(u8 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static u8 c_peek(IfCur *c) { return c->i < c->n ? (u8)c->p[c->i] : 0; }
static u8 c_peek2(IfCur *c) { return c->i + 1 < c->n ? (u8)c->p[c->i + 1] : 0; }
static void c_skip_ws(IfCur *c)  { while (c->i < c->n && c_ws((u8)c->p[c->i])) c->i++; }

/* ブロックコメントも空白として扱う */
static void c_skip_ws_comments(IfCur *c) {
    for (;;) {
        c_skip_ws(c);
        if (c_peek(c) == '/' && c_peek2(c) == '*') {
            c->i += 2;
            while (c->i + 1 < c->n && !(c->p[c->i] == '*' && c->p[c->i + 1] == '/')) c->i++;
            if (c->i + 1 < c->n) c->i += 2; else c->i = c->n; /* 閉じなし → 末尾まで */
            continue;
        }
        return;
    }
}

static bool c_ident_start(u8 c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-' || c >= 0x80; }
static bool c_ident_char(u8 c) { return c_ident_start(c) || (c >= '0' && c <= '9'); }

static IfStr c_ident(IfCur *c) {
    u32 s = c->i;
    if (c->i < c->n && (c_ident_char((u8)c->p[c->i]))) {
        c->i++;
        while (c->i < c->n && c_ident_char((u8)c->p[c->i])) c->i++;
    }
    return if_str(c->p + s, c->i - s);
}

/* 文字列トークン（'...' / "..."）。閉じなしは末尾まで。 */
static IfStr c_string(IfCur *c) {
    u8 q = c_peek(c);
    c->i++;
    u32 s = c->i;
    while (c->i < c->n && (u8)c->p[c->i] != q) {
        if (c->p[c->i] == '\\' && c->i + 1 < c->n) c->i++; /* エスケープ1文字読み飛ばし（解釈はしない） */
        c->i++;
    }
    IfStr out = if_str(c->p + s, c->i - s);
    if (c->i < c->n) c->i++;
    return out;
}

/* ================= 色 ================= */

static int hexv(u8 c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const struct { const char *name; u8 n; u32 rgb; } IF_COLORS[] = {
    {"black",5,0x000000},{"silver",6,0xC0C0C0},{"gray",4,0x808080},{"grey",4,0x808080},
    {"white",5,0xFFFFFF},{"maroon",6,0x800000},{"red",3,0xFF0000},{"purple",6,0x800080},
    {"fuchsia",7,0xFF00FF},{"magenta",7,0xFF00FF},{"green",5,0x008000},{"lime",4,0x00FF00},
    {"olive",5,0x808000},{"yellow",6,0xFFFF00},{"navy",4,0x000080},{"blue",4,0x0000FF},
    {"teal",4,0x008080},{"aqua",4,0x00FFFF},{"cyan",4,0x00FFFF},{"orange",6,0xFFA500},
    {"aliceblue",9,0xF0F8FF},{"brown",5,0xA52A2A},{"coral",5,0xFF7F50},{"crimson",7,0xDC143C},
    {"darkblue",8,0x00008B},{"darkgray",8,0xA9A9A9},{"darkgreen",9,0x006400},{"darkred",7,0x8B0000},
    {"gold",4,0xFFD700},{"goldenrod",9,0xDAA520},{"hotpink",7,0xFF69B4},{"indigo",6,0x4B0082},
    {"ivory",5,0xFFFFF0},{"khaki",5,0xF0E68C},{"lavender",8,0xE6E6FA},{"lightgray",9,0xD3D3D3},
    {"lightgreen",10,0x90EE90},{"lightyellow",11,0xFFFFE0},{"limegreen",9,0x32CD32},
    {"magenta",7,0xFF00FF},{"midnightblue",12,0x191970},{"orchid",6,0xDA70D6},{"pink",4,0xFFC0CB},
    {"plum",4,0xDDA0DD},{"rebeccapurple",13,0x663399},{"salmon",6,0xFA8072},{"skyblue",7,0x87CEEB},
    {"slategray",9,0x708090},{"snow",4,0xFFFAFA},{"tan",3,0xD2B48C},{"tomato",6,0xFF6347},
    {"turquoise",9,0x40E0D0},{"violet",6,0xEE82EE},{"wheat",5,0xF5DEB3},{"whitesmoke",10,0xF5F5F5},
};

static u32 rgba8(u32 r, u32 g, u32 b, u32 a) {
    return (r << 24) | (g << 16) | (b << 8) | a;
}

bool if_css_color(IfStr s, u32 *out) {
    s = if_str_trim(s);
    if (s.n == 0) return false;

    if (s.p[0] == '#') {
        if (s.n == 4 || s.n == 5 || s.n == 7 || s.n == 9) {
            u32 d[8];
            for (u32 i = 1; i < s.n; i++) {
                int v = hexv((u8)s.p[i]);
                if (v < 0) return false;
                d[i - 1] = (u32)v;
            }
            if (s.n == 4) { *out = rgba8(d[0]*17, d[1]*17, d[2]*17, 255); return true; }
            if (s.n == 5) { *out = rgba8(d[0]*17, d[1]*17, d[2]*17, d[3]*17); return true; }
            if (s.n == 7) { *out = rgba8(d[0]*16+d[1], d[2]*16+d[3], d[4]*16+d[5], 255); return true; }
            *out = rgba8(d[0]*16+d[1], d[2]*16+d[3], d[4]*16+d[5], d[6]*16+d[7]);
            return true;
        }
        return false;
    }

    /* rgb(...) / rgba(...) : カンマ区切り 3-4 数値（%は未対応で棄却） */
    if (s.n >= 5 && (s.p[0] == 'r' || s.p[0] == 'R')) {
        bool alpha = (s.n >= 5 && if_ascii_lower((u8)s.p[3]) == 'a');
        u32 off = alpha ? 5 : 4;
        if (s.n <= off || s.p[s.n - 1] != ')') return false;
        IfCur c = { s.p + off, s.n - off - 1, 0 };
        u32 ch[4] = {0, 0, 0, 255};
        u32 got = 0;
        for (u32 k = 0; k < 4; k++) {
            while (c.i < c.n && (c_ws((u8)c.p[c.i]) || c.p[c.i] == ',')) c.i++;
            if (c.i >= c.n) break;
            /* 整数のみ（実数・%は捨てる: 近似方針） */
            u32 start = c.i;
            if (c.p[c.i] == '-') return false;
            while (c.i < c.n && c.p[c.i] >= '0' && c.p[c.i] <= '9') c.i++;
            if (c.i == start) return false;
            u64 v = 0;
            for (u32 j = start; j < c.i; j++) v = v * 10 + (u64)(c.p[j] - '0');
            if (v > 255) return false;
            if (c.i < c.n && c.p[c.i] == '%') return false;
            ch[got++] = (u32)v;
            if (got == 3 && !alpha) break;
        }
        if (got < 3) return false;
        *out = rgba8(ch[0], ch[1], ch[2], alpha ? ch[3] : 255);
        return true;
    }

    if (if_str_eq_ci(s, IF_S("transparent"))) { *out = 0; return true; }
    for (u64 i = 0; i < sizeof(IF_COLORS) / sizeof(IF_COLORS[0]); i++) {
        if (s.n == IF_COLORS[i].n && if_str_eq_ci(s, if_str(IF_COLORS[i].name, IF_COLORS[i].n))) {
            *out = rgba8(IF_COLORS[i].rgb >> 16, (IF_COLORS[i].rgb >> 8) & 0xff, IF_COLORS[i].rgb & 0xff, 255);
            return true;
        }
    }
    return false;
}

/* ================= 値レクサ（shorthand 展開・宣言パース共用） ================= */

enum { VI_NUM, VI_DIM, VI_PCT, VI_IDENT, VI_COLOR, VI_STR, VI_AUTO };
#define IF_MAX_VALUE_ITEMS 8u

typedef struct { u8 kind; float num; IfStr unit; IfStr text; u32 color; } IfValItem;

static bool parse_number(IfCur *c, float *out) {
    u32 s = c->i;
    if (c->i < c->n && (c->p[c->i] == '-' || c->p[c->i] == '+')) c->i++;
    bool digits = false;
    while (c->i < c->n && c->p[c->i] >= '0' && c->p[c->i] <= '9') { c->i++; digits = true; }
    if (c->i < c->n && c->p[c->i] == '.' ) {
        c->i++;
        while (c->i < c->n && c->p[c->i] >= '0' && c->p[c->i] <= '9') { c->i++; digits = true; }
    }
    if (!digits) { c->i = s; return false; }
    /* C ロケール非依存の手動変換（locale による挙動差を排除） */
    float v = 0, frac = 0.1f;
    u32 i = s;
    bool neg = false;
    if (c->p[i] == '-') { neg = true; i++; } else if (c->p[i] == '+') i++;
    while (i < c->n && c->p[i] >= '0' && c->p[i] <= '9') { v = v * 10.0f + (float)(c->p[i] - '0'); i++; }
    if (i < c->n && c->p[i] == '.') {
        i++;
        while (i < c->n && c->p[i] >= '0' && c->p[i] <= '9') { v += frac * (float)(c->p[i] - '0'); frac *= 0.1f; i++; }
    }
    *out = neg ? -v : v;
    return true;
}

/* 値をトークン列に分解。戻り値は個数（0 = 空/不正）。 */
static u32 lex_value(IfArena *a, IfStr raw, IfValItem **out) {
    (void)a;
    IfValItem *items = (IfValItem *)if_arena_alloc(a, IF_MAX_VALUE_ITEMS * sizeof(IfValItem));
    u32 n = 0;
    IfCur c = { raw.p, raw.n, 0 };
    while (c.i < c.n) {
        if (n >= IF_MAX_VALUE_ITEMS) return 0;
        if (c_ws(c_peek(&c))) { c.i++; continue; }
        u8 ch = c_peek(&c);
        if (ch == '/' && c_peek2(&c) == '*') { c_skip_ws_comments(&c); continue; }
        if (ch == '"' || ch == '\'') {
            items[n].kind = VI_STR;
            items[n].text = c_string(&c);
            n++;
            continue;
        }
        float num;
        if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '+') {
            IfCur save = c;
            if (!parse_number(&c, &num)) { c = save; return 0; }
            if (c_peek(&c) == '%') {
                c.i++;
                items[n].kind = VI_PCT; items[n].num = num;
            } else if (c_ident_start(c_peek(&c))) {
                IfStr unit = c_ident(&c);
                items[n].kind = VI_DIM; items[n].num = num; items[n].unit = unit;
            } else {
                items[n].kind = VI_NUM; items[n].num = num;
            }
            n++;
            continue;
        }
        if (ch == '#') {
            u32 s = c.i;
            while (c.i < c.n && (hexv((u8)c.p[c.i]) >= 0 || c.p[c.i] == '#')) c.i++;
            u32 col;
            if (!if_css_color(if_str(raw.p + s, c.i - s), &col)) return 0;
            items[n].kind = VI_COLOR; items[n].color = col;
            n++;
            continue;
        }
        if (c_ident_start(ch)) {
            IfStr id = c_ident(&c);
            if (c_peek(&c) == '(') {
                /* 関数（rgb 等）: 対応する閉じ括弧までを一括で取り、色としてのみ解釈 */
                u32 s = c.i - id.n;
                u32 depth = 0;
                while (c.i < c.n) {
                    if (c.p[c.i] == '(') depth++;
                    else if (c.p[c.i] == ')') { depth--; if (!depth) { c.i++; break; } }
                    c.i++;
                }
                u32 col;
                if (if_css_color(if_str(raw.p + s, c.i - s), &col)) {
                    items[n].kind = VI_COLOR; items[n].color = col;
                    n++;
                    continue;
                }
                return 0; /* 未知関数を含む値は棄却 */
            }
            u32 col;
            if (if_css_color(id, &col)) {
                items[n].kind = VI_COLOR; items[n].color = col;
                n++;
                continue;
            }
            if (if_str_eq_ci(id, IF_S("auto"))) {
                items[n].kind = VI_AUTO;
                n++;
                continue;
            }
            items[n].kind = VI_IDENT; items[n].text = id;
            n++;
            continue;
        }
        if (ch == ',' || ch == '/') { c.i++; continue; } /* 区切りは読み飛ばす（v0.1 近似） */
        return 0; /* 未知文字 → 値全体を棄却 */
    }
    *out = items;
    return n;
}

/* ================= プロパティ表 ================= */

static const struct { const char *name; u8 prop; } IF_PROPS[] = {
    {"display", IF_P_DISPLAY}, {"color", IF_P_COLOR}, {"background-color", IF_P_BACKGROUND_COLOR},
    {"font-size", IF_P_FONT_SIZE}, {"font-weight", IF_P_FONT_WEIGHT},
    {"font-style", IF_P_FONT_STYLE}, {"text-decoration", IF_P_TEXT_DECORATION},
    {"margin-top", IF_P_MARGIN_TOP}, {"margin-right", IF_P_MARGIN_RIGHT},
    {"margin-bottom", IF_P_MARGIN_BOTTOM}, {"margin-left", IF_P_MARGIN_LEFT},
    {"padding-top", IF_P_PADDING_TOP}, {"padding-right", IF_P_PADDING_RIGHT},
    {"padding-bottom", IF_P_PADDING_BOTTOM}, {"padding-left", IF_P_PADDING_LEFT},
    {"border-top-width", IF_P_BORDER_TOP_WIDTH}, {"border-right-width", IF_P_BORDER_RIGHT_WIDTH},
    {"border-bottom-width", IF_P_BORDER_BOTTOM_WIDTH}, {"border-left-width", IF_P_BORDER_LEFT_WIDTH},
    {"border-color", IF_P_BORDER_COLOR},
    {"width", IF_P_WIDTH}, {"height", IF_P_HEIGHT},
    {"text-align", IF_P_TEXT_ALIGN}, {"line-height", IF_P_LINE_HEIGHT},
    {"white-space", IF_P_WHITE_SPACE},
};

static int prop_id(IfStr name) {
    for (u32 i = 0; i < sizeof(IF_PROPS) / sizeof(IF_PROPS[0]); i++)
        if (if_str_eq_ci(name, if_str(IF_PROPS[i].name, (u32)strlen(IF_PROPS[i].name)))) return IF_PROPS[i].prop;
    return -1;
}

/* ================= 宣言パース ================= */

typedef struct {
    IfArena *a;
    IfDecl *decls; u32 n_decls; u64 cap;
    u32 dropped;
} IfDeclSink;

static void decl_push(IfDeclSink *s, u16 prop, u8 important, u8 vkind, float num, u8 unit, u32 color, IfStr text) {
    s->decls = (IfDecl *)if_arena_grow(s->a, s->decls, &s->cap, s->n_decls + 1, sizeof(IfDecl));
    IfDecl *d = &s->decls[s->n_decls++];
    d->prop = prop; d->important = important; d->vkind = vkind;
    d->num = num; d->unit = unit; d->color = color; d->text = text;
}

static bool item_to_len(const IfValItem *it, IfLen *out) {
    switch (it->kind) {
    case VI_AUTO: *out = IF_LEN_AUTO; return true;
    case VI_NUM:
        if (it->num == 0.0f) { out->v = 0; out->unit = IF_U_PX; return true; }
        return false; /* 単位なし非ゼロは不正 */
    case VI_PCT: out->v = it->num; out->unit = IF_U_PCT; return true;
    case VI_DIM:
        if (if_str_eq_ci(it->unit, IF_S("px"))) { out->v = it->num; out->unit = IF_U_PX; return true; }
        if (if_str_eq_ci(it->unit, IF_S("em"))) { out->v = it->num; out->unit = IF_U_EM; return true; }
        if (if_str_eq_ci(it->unit, IF_S("rem"))) { out->v = it->num; out->unit = IF_U_REM; return true; }
        if (if_str_eq_ci(it->unit, IF_S("pt"))) { out->v = it->num; out->unit = IF_U_PT; return true; }
        return false;
    default: return false;
    }
}

static void decl_push_len(IfDeclSink *s, u16 prop, u8 important, IfLen l) {
    decl_push(s, prop, important, IF_V_LEN, l.v, l.unit, 0, if_str(NULL, 0));
}

/* 1 宣言（name: value 済み）を sink に展開して押し込む。 */
static void decl_one(IfDeclSink *s, IfStr name, IfStr value, u8 important) {
    /* shorthand 判定 */
    if (if_str_eq_ci(name, IF_S("margin")) || if_str_eq_ci(name, IF_S("padding"))) {
        IfValItem *items; u32 n = lex_value(s->a, value, &items);
        if (n == 0 || n > 4) { s->dropped++; return; }
        IfLen l[4];
        for (u32 i = 0; i < n; i++)
            if (!item_to_len(&items[i], &l[i])) { s->dropped++; return; }
        l[1] = (n > 1) ? l[1] : l[0];
        l[2] = (n > 2) ? l[2] : l[0];
        l[3] = (n > 3) ? l[3] : l[1];
        u16 base = if_str_eq_ci(name, IF_S("margin")) ? IF_P_MARGIN_TOP : IF_P_PADDING_TOP;
        for (u32 i = 0; i < 4; i++) decl_push_len(s, (u16)(base + i), important, l[i]);
        return;
    }
    if (if_str_eq_ci(name, IF_S("border"))) {
        IfValItem *items; u32 n = lex_value(s->a, value, &items);
        if (n == 0) { s->dropped++; return; }
        bool have_w = false, have_c = false, none = false;
        IfLen w = { 1.0f, IF_U_PX };
        u32 col = 0;
        for (u32 i = 0; i < n; i++) {
            IfLen l;
            if (item_to_len(&items[i], &l)) { w = l; have_w = true; continue; }
            if (items[i].kind == VI_COLOR) { col = items[i].color; have_c = true; continue; }
            if (items[i].kind == VI_IDENT) {
                if (if_str_eq_ci(items[i].text, IF_S("none")) || if_str_eq_ci(items[i].text, IF_S("hidden"))) { none = true; continue; }
                if (if_str_eq_ci(items[i].text, IF_S("solid")) || if_str_eq_ci(items[i].text, IF_S("dotted")) ||
                    if_str_eq_ci(items[i].text, IF_S("dashed")) || if_str_eq_ci(items[i].text, IF_S("double"))) continue; /* solid に丸める */
            }
            s->dropped++; return;
        }
        if (none) { w.v = 0; w.unit = IF_U_PX; }
        if (!have_w && !none) { s->dropped++; return; }
        for (u32 i = 0; i < 4; i++) decl_push_len(s, (u16)(IF_P_BORDER_TOP_WIDTH + i), important, w);
        if (have_c) decl_push(s, IF_P_BORDER_COLOR, important, IF_V_COLOR, 0, 0, col, if_str(NULL, 0));
        return;
    }
    if (if_str_eq_ci(name, IF_S("background"))) {
        IfValItem *items; u32 n = lex_value(s->a, value, &items);
        for (u32 i = 0; i < n; i++)
            if (items[i].kind == VI_COLOR) {
                decl_push(s, IF_P_BACKGROUND_COLOR, important, IF_V_COLOR, 0, 0, items[i].color, if_str(NULL, 0));
                return;
            }
        s->dropped++;
        return;
    }
    if (if_str_eq_ci(name, IF_S("border-width"))) {
        IfValItem *items; u32 n = lex_value(s->a, value, &items);
        if (n < 1 || n > 4) { s->dropped++; return; }
        IfLen l[4];
        for (u32 i = 0; i < n; i++)
            if (!item_to_len(&items[i], &l[i])) { s->dropped++; return; }
        l[1] = (n > 1) ? l[1] : l[0];
        l[2] = (n > 2) ? l[2] : l[0];
        l[3] = (n > 3) ? l[3] : l[1];
        for (u32 i = 0; i < 4; i++) decl_push_len(s, (u16)(IF_P_BORDER_TOP_WIDTH + i), important, l[i]);
        return;
    }
    if (if_str_eq_ci(name, IF_S("font")) || if_str_eq_ci(name, IF_S("list-style")) ||
        if_str_eq_ci(name, IF_S("text-decoration-line")) || if_str_eq_ci(name, IF_S("flex")) ||
        if_str_eq_ci(name, IF_S("grid")) || if_str_eq_ci(name, IF_S("animation")) ||
        if_str_eq_ci(name, IF_S("transition"))) {
        s->dropped++; return; /* 未対応 shorthand/別名は丸ごと棄却 */
    }

    int p = prop_id(name);
    if (p < 0) { s->dropped++; return; }

    /* color 系 */
    if (p == IF_P_COLOR || p == IF_P_BACKGROUND_COLOR || p == IF_P_BORDER_COLOR) {
        if (if_str_eq_ci(if_str_trim(value), IF_S("currentcolor"))) { s->dropped++; return; }
        u32 col;
        if (!if_css_color(value, &col)) { s->dropped++; return; }
        decl_push(s, (u16)p, important, IF_V_COLOR, 0, 0, col, if_str(NULL, 0));
        return;
    }

    /* length 系 */
    if (p == IF_P_MARGIN_TOP || p == IF_P_MARGIN_RIGHT || p == IF_P_MARGIN_BOTTOM || p == IF_P_MARGIN_LEFT ||
        p == IF_P_PADDING_TOP || p == IF_P_PADDING_RIGHT || p == IF_P_PADDING_BOTTOM || p == IF_P_PADDING_LEFT ||
        p == IF_P_WIDTH || p == IF_P_HEIGHT ||
        p == IF_P_BORDER_TOP_WIDTH || p == IF_P_BORDER_RIGHT_WIDTH || p == IF_P_BORDER_BOTTOM_WIDTH || p == IF_P_BORDER_LEFT_WIDTH) {
        IfValItem *items; u32 n = lex_value(s->a, value, &items);
        /* border-*-width のキーワード */
        if (n == 1 && items[0].kind == VI_IDENT && p >= IF_P_BORDER_TOP_WIDTH && p <= IF_P_BORDER_LEFT_WIDTH) {
            float w = if_str_eq_ci(items[0].text, IF_S("thin")) ? 1.0f
                    : if_str_eq_ci(items[0].text, IF_S("medium")) ? 3.0f
                    : if_str_eq_ci(items[0].text, IF_S("thick")) ? 5.0f : -1.0f;
            if (w < 0) { s->dropped++; return; }
            decl_push_len(s, (u16)p, important, (IfLen){ w, IF_U_PX });
            return;
        }
        if (n != 1) { s->dropped++; return; }
        IfLen l;
        if (!item_to_len(&items[0], &l)) { s->dropped++; return; }
        if ((p == IF_P_PADDING_TOP || p == IF_P_PADDING_RIGHT || p == IF_P_PADDING_BOTTOM || p == IF_P_PADDING_LEFT) &&
            l.unit == IF_U_AUTO) { s->dropped++; return; } /* padding に auto はない */
        decl_push_len(s, (u16)p, important, l);
        return;
    }

    /* font-size / line-height は「長さ or キーワード/無単位数値」: 長さとして読めれば LEN、
     * 読めなければ RAW に残して適用側がキーワード/倍率として解釈する。 */
    if (p == IF_P_FONT_SIZE || p == IF_P_LINE_HEIGHT) {
        IfValItem *items; u32 n = lex_value(s->a, value, &items);
        if (n == 1) {
            IfLen l;
            if (item_to_len(&items[0], &l)) {
                decl_push_len(s, (u16)p, important, l);
                return;
            }
            if (p == IF_P_LINE_HEIGHT && items[0].kind == VI_NUM && items[0].num >= 0.0f) {
                /* 無単位 line-height は倍率として RAW に残す（px 丸め事故を防ぐ） */
            } else if (p == IF_P_LINE_HEIGHT && items[0].kind == VI_NUM && items[0].num < 0.0f) {
                s->dropped++;
                return;
            }
        }
        IfStr v = if_str_trim(value);
        decl_push(s, (u16)p, important, IF_V_RAW, 0, 0, 0, v);
        return;
    }

    /* 残りは ident / num / raw をそのまま格納し、適用側で解釈 */
    IfStr v = if_str_trim(value);
    decl_push(s, (u16)p, important, IF_V_RAW, 0, 0, 0, v);
}

/* 宣言ブロック（'{' の中身 or inline style）をパース */
static void parse_decl_block(IfDeclSink *s, IfStr text) {
    IfCur c = { text.p, text.n, 0 };
    while (c.i < c.n) {
        u32 stmt_start = c.i;
        /* ';' までを 1 宣言として切り出す（括弧と文字列を考慮） */
        u32 depth = 0;
        u8 in_str = 0;
        while (c.i < c.n) {
            u8 ch = (u8)c.p[c.i];
            if (in_str) {
                if (ch == in_str) in_str = 0;
                else if (ch == '\\') c.i++;
            } else if (ch == '"' || ch == '\'') in_str = ch;
            else if (ch == '(') depth++;
            else if (ch == ')' && depth) depth--;
            else if (ch == ';' && !depth) break;
            c.i++;
        }
        IfStr stmt = if_str(text.p + stmt_start, c.i - stmt_start);
        if (c.i < c.n) c.i++; /* ';' */

        /* name: value に分解 */
        u32 colon = 0;
        while (colon < stmt.n && stmt.p[colon] != ':') colon++;
        if (colon >= stmt.n) { if (if_str_empty(if_str_trim(stmt))) continue; s->dropped++; continue; }
        IfStr name = if_str_trim(if_str(stmt.p, colon));
        IfStr value = if_str_trim(if_str(stmt.p + colon + 1, stmt.n - colon - 1));
        if (name.n == 0 || !c_ident_start((u8)name.p[0])) { s->dropped++; continue; }

        /* !important の検出（末尾） */
        u8 important = 0;
        IfStr v = value;
        /* 末尾から走査: "... ! important" 形 */
        {
            u32 k = v.n;
            while (k > 0 && c_ws((u8)v.p[k - 1])) k--;
            if (k >= 9) {
                IfStr tail = if_str(v.p + k - 9, 9);
                /* "important" の 9 文字。直前に '!' があるか見る */
                u32 h = k - 9;
                while (h > 0 && c_ws((u8)v.p[h - 1])) h--;
                if (if_str_eq_ci(tail, IF_S("important")) && h > 0 && v.p[h - 1] == '!') {
                    /* '!' の前が値の終わり or 空白で区切られていること */
                    important = 1;
                    v = if_str_trim(if_str(v.p, h - 1));
                }
            }
        }
        decl_one(s, name, v, important);
    }
}

u32 if_css_parse_decls(IfArena *a, IfStr text, IfDecl **out) {
    IfDeclSink s = { a, NULL, 0, 0, 0 };
    parse_decl_block(&s, text);
    *out = s.decls;
    return s.n_decls;
}

/* ================= セレクタパース ================= */

static bool parse_compound(IfCur *c, IfArena *a, IfCompound *out) {
    memset(out, 0, sizeof *out);
    bool any = false;
    IfStr *classes = NULL; u32 nc = 0; u64 ccap = 0;
    IfStr *ids = NULL; u32 ni = 0; u64 icap = 0;

    for (;;) {
        u8 ch = c_peek(c);
        if (ch == '*') {
            if (any) return false;
            any = true; /* universal: has_tag=false のまま */
            c->i++;
            continue;
        }
        if (ch == '.') {
            c->i++;
            IfStr id = c_ident(c);
            if (id.n == 0) return false;
            classes = (IfStr *)if_arena_grow(a, classes, &ccap, nc + 1, sizeof(IfStr));
            classes[nc++] = id;
            any = true;
            continue;
        }
        if (ch == '#') {
            c->i++;
            IfStr id = c_ident(c);
            if (id.n == 0) return false;
            ids = (IfStr *)if_arena_grow(a, ids, &icap, ni + 1, sizeof(IfStr));
            ids[ni++] = id;
            any = true;
            continue;
        }
        if (c_ident_start(ch) || ch == '\\') {
            if (ch == '\\') return false; /* エスケープは丸ごと棄却（防御） */
            if (out->has_tag) return false; /* タイプは先頭に一度だけ */
            IfStr t = c_ident(c);
            out->has_tag = true;
            out->tag = if_tag_id(t);
            out->tag_name = out->tag == IF_TAG_UNKNOWN ? t : if_str(NULL, 0);
            any = true;
            continue;
        }
        break;
    }
    if (!any) return false;
    out->classes = classes; out->n_classes = nc;
    out->ids = ids; out->n_ids = ni;
    return true;
}

/* カンマ区切りのセレクタ群をパース。返り値は arena の IfSelector 配列。 */
static u32 parse_selector_list(IfArena *a, IfStr raw, IfSelector **out) {
    /* まずカンマで分割（括弧・文字列考慮） */
    IfSelector *sels = NULL; u32 ns = 0; u64 cap = 0;
    IfCur c = { raw.p, raw.n, 0 };
    while (c.i < c.n) {
        u32 s = c.i;
        u32 depth = 0; u8 in_str = 0;
        while (c.i < c.n) {
            u8 ch = (u8)c.p[c.i];
            if (in_str) { if (ch == in_str) in_str = 0; }
            else if (ch == '"' || ch == '\'') in_str = ch;
            else if (ch == '(' || ch == '[') depth++;
            else if (ch == ')' || ch == ']') { if (depth) depth--; }
            else if (ch == ',' && !depth) break;
            c.i++;
        }
        IfStr one = if_str_trim(if_str(raw.p + s, c.i - s));
        if (c.i < c.n && c.p[c.i] == ',') c.i++;
        if (one.n == 0) continue;

        /* 1 セレクタをパース: comps[0] combs[0] comps[1] combs[1] ... comps[n-1]
         * 単一パスで comps と combs を同時構築（再パースは二度読みでズレの温床。禁止）。 */
        IfSelector sel;
        memset(&sel, 0, sizeof sel);
        IfCompound *comps = NULL; u32 nc = 0; u64 ccap = 0;
        u8 *combs = NULL; u64 bcap = 0;
        bool valid = true;
        IfCur p = { one.p, one.n, 0 };
        u8 pending_comb = IF_CX_DESCENDANT;
        bool need_compound = true;
        while (p.i < p.n) {
            if (c_ws(c_peek(&p)) || (c_peek(&p) == '/' && c_peek2(&p) == '*')) {
                c_skip_ws_comments(&p);
                if (p.i < p.n && c_peek(&p) != '>' && nc > 0 && !need_compound) {
                    pending_comb = IF_CX_DESCENDANT;
                    need_compound = true;
                }
                continue;
            }
            if (c_peek(&p) == '>') {
                if (need_compound || nc == 0) { valid = false; break; }
                p.i++;
                pending_comb = IF_CX_CHILD;
                need_compound = true;
                continue;
            }
            if (c_peek(&p) == '+' || c_peek(&p) == '~' || c_peek(&p) == ':' || c_peek(&p) == '[' || c_peek(&p) == ',') {
                valid = false; break; /* 未対応構文 → セレクタごと棄却 */
            }
            IfCompound comp;
            if (!parse_compound(&p, a, &comp)) { valid = false; break; }
            if (nc > 0) {
                if (!need_compound) { valid = false; break; } /* 結合子なし連接は壊れている */
                combs = (u8 *)if_arena_grow(a, combs, &bcap, nc, 1);
                combs[nc - 1] = pending_comb;
            }
            comps = (IfCompound *)if_arena_grow(a, comps, &ccap, nc + 1, sizeof(IfCompound));
            comps[nc] = comp;
            nc++;
            need_compound = false;
        }
        if (valid && need_compound && nc > 0) valid = false; /* 末尾結合子は壊れている */
        if (valid && nc > 0) {
            sel.comps = comps;
            sel.combs = combs;
            sel.n_comps = nc;
            /* specificity */
            u32 ids = 0, cls = 0, typ = 0;
            for (u32 i = 0; i < nc; i++) {
                ids += comps[i].n_ids;
                cls += comps[i].n_classes;
                if (comps[i].has_tag) typ++;
            }
            sel.spec = (ids << 16) | (cls << 8) | typ;
            sels = (IfSelector *)if_arena_grow(a, sels, &cap, ns + 1, sizeof(IfSelector));
            sels[ns++] = sel;
        }
    }
    *out = sels;
    return ns;
}

/* ================= スタイルシートパース ================= */

/* ================= RuleSet 風インデックス =================
 * 戦略は Blink RuleSet と同一: 右端 compound の最強特徴（id > class > tag > universal）
 * をキーに (rule, sel) を単一バケツへ。要素マッチ時は自身の特徴のバケツ + universal のみ
 * 全マッチする。正しさの骨格: selector が要素 n にマッチするなら右端 compound が成立し、
 * n はそのキーのバケツを必ず引く（バケツ外エントリの右端は必ず不成立）⇒ 候補集合は
 * 全走査のマッチ集合と一致。カスケードの勝者決定は (important, origin, spec, order) の
 * 厳密全順序で反復順序に依存しないため、結論も一致。差分 audit: if_css_set_naive_matching。 */

static u32 rs_hash(IfStr s) {
    u32 h = 2166136261u;
    for (u32 i = 0; i < s.n; i++) { h ^= (u8)s.p[i]; h *= 16777619u; }
    return h;
}
static u8 rs_lower(u8 c) { return (c >= 'A' && c <= 'Z') ? (u8)(c - 'A' + 'a') : c; }

/* 右端 compound のキー。戻り 0=id 1=class 2=tag 3=universal（tag 未知名は CI 照合に
 * 合わせ ASCII-lowercase 複製に正規化する。既知タグは canonical 静的名を借用） */
static int rs_entry_key(IfArena *a, const IfSelector *sel, IfStr *key_out) {
    const IfCompound *cp = &sel->comps[sel->n_comps - 1];
    *key_out = if_str(NULL, 0); /* universal は無キー（cmp の規則を破らせない） */
    if (cp->n_ids) { *key_out = cp->ids[0]; return 0; }
    if (cp->n_classes) { *key_out = cp->classes[0]; return 1; }
    if (cp->has_tag) {
        if (cp->tag != IF_TAG_UNKNOWN) {
            const char *nm = if_tag_name(cp->tag);
            *key_out = if_str(nm, nm ? (u32)strlen(nm) : 0);
        } else {
            u8 *buf = (u8 *)if_arena_alloc(a, cp->tag_name.n ? cp->tag_name.n : 1);
            for (u32 i = 0; i < cp->tag_name.n; i++) buf[i] = rs_lower((u8)cp->tag_name.p[i]);
            *key_out = if_str((const char *)buf, cp->tag_name.n);
        }
        return 2;
    }
    return 3;
}

typedef struct { u32 hash; IfStr key; u32 rule, sel; u16 type, pad; } RsItem;

static int rs_item_cmp(const void *x, const void *y) {
    const RsItem *a = (const RsItem *)x, *b = (const RsItem *)y;
    if (a->type != b->type) return a->type < b->type ? -1 : 1;
    if (a->hash != b->hash) return a->hash < b->hash ? -1 : 1;
    u32 n = a->key.n < b->key.n ? a->key.n : b->key.n;
    int c = n ? memcmp(a->key.p, b->key.p, n) : 0;
    if (c) return c < 0 ? -1 : 1;
    if (a->key.n != b->key.n) return a->key.n < b->key.n ? -1 : 1;
    if (a->rule != b->rule) return a->rule < b->rule ? -1 : 1;
    if (a->sel != b->sel) return a->sel < b->sel ? -1 : 1;
    return 0;
}

static bool rs_same_key(const RsItem *x, const RsItem *y) {
    return x->type == y->type && x->hash == y->hash && x->key.n == y->key.n &&
           (!x->key.n || memcmp(x->key.p, y->key.p, x->key.n) == 0);
}

static void css_build_ruleset(IfArena *a, IfStyleSheet *sh) {
    IfRuleSet *rs = &sh->rs;
    u32 total = 0;
    for (u32 r = 0; r < sh->n_rules; r++) total += sh->rules[r].n_sels;
    rs->pool = NULL; rs->n_pool = 0;
    rs->id_b = NULL; rs->n_id = 0; rs->cl_b = NULL; rs->n_cl = 0; rs->tg_b = NULL; rs->n_tg = 0;
    rs->univ_start = 0; rs->univ_len = 0;
    if (!total) return;
    RsItem *items = (RsItem *)malloc((u64)total * sizeof(RsItem));
    IfSelEntry *pool = (IfSelEntry *)if_arena_alloc(a, (u64)total * sizeof(IfSelEntry));
    if (!items) return; /* OOM: n_pool=0 のまま = 呼出し側が naive 全走査へフォールバック（安全側） */
    u32 ni = 0;
    for (u32 r = 0; r < sh->n_rules; r++)
        for (u32 s = 0; s < sh->rules[r].n_sels; s++) {
            IfStr key; int t = rs_entry_key(a, &sh->rules[r].sels[s], &key);
            items[ni].type = (u16)t; items[ni].pad = 0;
            items[ni].key = key;
            items[ni].hash = (t == 3) ? 0 : rs_hash(key);
            items[ni].rule = r; items[ni].sel = s;
            ni++;
        }
    qsort(items, total, sizeof(RsItem), rs_item_cmp);

    /* distinct key 数を片っ端から数え、バケツ配列をぴったり確保（slack ゼロ = 軽さの法則） */
    u32 distinct[3] = { 0, 0, 0 };
    for (u32 i = 0; i < total && items[i].type < 3; i++)
        if (i == 0 || !rs_same_key(&items[i], &items[i - 1])) distinct[items[i].type]++;
    rs->id_b = distinct[0] ? (IfSelBucket *)if_arena_alloc(a, (u64)distinct[0] * sizeof(IfSelBucket)) : NULL;
    rs->cl_b = distinct[1] ? (IfSelBucket *)if_arena_alloc(a, (u64)distinct[1] * sizeof(IfSelBucket)) : NULL;
    rs->tg_b = distinct[2] ? (IfSelBucket *)if_arena_alloc(a, (u64)distinct[2] * sizeof(IfSelBucket)) : NULL;
    IfSelBucket *btab[3] = { rs->id_b, rs->cl_b, rs->tg_b };
    u32 *bnum[3] = { &rs->n_id, &rs->n_cl, &rs->n_tg };

    u32 pos = 0; /* pool カーソル（id → class → tag → universal の順で連続配置） */
    u32 i = 0;
    while (i < total && items[i].type < 3) {
        u32 t = items[i].type;
        IfSelBucket *b = &btab[t][(*bnum[t])++];
        b->hash = items[i].hash; b->key = items[i].key; b->start = pos; b->len = 0;
        do {
            pool[pos].rule = items[i].rule; pool[pos].sel = items[i].sel; pos++;
            b->len++; i++;
        } while (i < total && rs_same_key(&items[i], &items[i - 1]));
    }
    rs->univ_start = pos;
    while (i < total) { pool[pos].rule = items[i].rule; pool[pos].sel = items[i].sel; pos++; i++; }
    rs->univ_len = pos - rs->univ_start;
    free(items);
    rs->pool = pool; rs->n_pool = total;
}

/* バケツは hash 昇順。同 hash は memcmp 順の連続区画（build のソート規約） */
static const IfSelBucket *rs_find(const IfSelBucket *b, u32 n, u32 h, IfStr key) {
    u32 lo = 0, hi = n;
    while (lo < hi) { u32 m = lo + ((hi - lo) >> 1); if (b[m].hash < h) lo = m + 1; else hi = m; }
    for (; lo < n && b[lo].hash == h; lo++)
        if (b[lo].key.n == key.n && (!key.n || memcmp(b[lo].key.p, key.p, key.n) == 0)) return &b[lo];
    return NULL;
}

IfStyleSheet *if_css_parse(IfArena *a, IfStr css, u32 order_base) {
    IfStyleSheet *sh = (IfStyleSheet *)if_arena_calloc(a, sizeof(IfStyleSheet));
    IfRule *rules = NULL; u32 nr = 0; u64 cap = 0;
    u32 order = order_base;
    IfCur c = { css.p, css.n, 0 };

    while (c.i < c.n) {
        c_skip_ws_comments(&c);
        if (c.i >= c.n) break;

        /* @-rule は丸ごと棄却 */
        if (c.p[c.i] == '@') {
            while (c.i < c.n && c.p[c.i] != '{' && c.p[c.i] != ';') c.i++;
            if (c.i < c.n && c.p[c.i] == '{') {
                u32 depth = 1; c.i++;
                while (c.i < c.n && depth) {
                    if (c.p[c.i] == '{') depth++;
                    else if (c.p[c.i] == '}') depth--;
                    c.i++;
                }
            } else if (c.i < c.n) c.i++; /* ';' */
            sh->n_dropped_rules++;
            continue;
        }

        /* プリリュード（セレクタ群）: '{' まで */
        u32 s = c.i;
        u32 depth = 0; u8 in_str = 0;
        while (c.i < c.n) {
            u8 ch = (u8)c.p[c.i];
            if (in_str) { if (ch == in_str) in_str = 0; }
            else if (ch == '"' || ch == '\'') in_str = ch;
            else if (ch == '(' || ch == '[') depth++;
            else if (ch == ')' || ch == ']') { if (depth) depth--; }
            else if (ch == '{' && !depth) break;
            else if (ch == '}' && !depth) break; /* 孤立 '}' */
            c.i++;
        }
        IfStr prelude = if_str_trim(if_str(css.p + s, c.i - s));
        if (c.i >= c.n || c.p[c.i] != '{') {
            /* 孤立 '}' 等。不変条件: 全経路でカーソルを必ず 1 以上前進させる（無限ループ禁止） */
            if (c.i < c.n) c.i++;
            sh->n_dropped_rules++;
            continue;
        }
        c.i++; /* '{' */

        /* ブロック中身: 対応 '}' まで（ネスト考慮） */
        u32 bs = c.i;
        depth = 0; in_str = 0;
        while (c.i < c.n) {
            u8 ch = (u8)c.p[c.i];
            if (in_str) { if (ch == in_str) in_str = 0; else if (ch == '\\') c.i++; }
            else if (ch == '"' || ch == '\'') in_str = ch;
            else if (ch == '(' || ch == '[' || ch == '{') depth++;
            else if (ch == ')' || ch == ']') { if (depth) depth--; }
            else if (ch == '}') { if (!depth) break; depth--; }
            c.i++;
        }
        IfStr body = if_str(css.p + bs, c.i - bs);
        if (c.i < c.n) c.i++; /* '}' */

        IfSelector *sels = NULL;
        u32 nsels = parse_selector_list(a, prelude, &sels);
        IfDeclSink sink = { a, NULL, 0, 0, 0 };
        parse_decl_block(&sink, body);
        sh->n_dropped_decls += sink.dropped;

        if (nsels == 0 || sink.n_decls == 0) {
            sh->n_dropped_rules += (nsels == 0) ? 1u : 0u;
            continue;
        }
        rules = (IfRule *)if_arena_grow(a, rules, &cap, nr + 1, sizeof(IfRule));
        rules[nr].sels = sels;
        rules[nr].n_sels = nsels;
        rules[nr].decls = sink.decls;
        rules[nr].n_decls = sink.n_decls;
        /* order は decl 単位で一意に単調（旧: rule 単位 ++ は order=rule_base+decl_idx が
         * ルール間で衝突し、同 spec 同重要度のタイで「後勝ち」が成立しないことがあった。
         * 索引化の差分監査がこの不成立を実測で炙り出したため、CSS 仕様の後勝ちへ修正） */
        rules[nr].order = order;
        order += sink.n_decls;
        nr++;
    }
    sh->rules = rules;
    sh->n_rules = nr;
    sh->order_end = order;
    css_build_ruleset(a, sh);
    return sh;
}

/* ================= マッチャ ================= */

static bool match_compound(const IfNode *n, const IfCompound *cp) {
    if (n->kind != IF_NODE_ELEMENT) return false;
    if (cp->has_tag) {
        if (cp->tag != IF_TAG_UNKNOWN) {
            if (n->tag != cp->tag) return false;
        } else {
            if (n->tag != IF_TAG_UNKNOWN || !if_str_eq_ci(n->u.tag_name, cp->tag_name)) return false;
        }
    }
    for (u32 i = 0; i < cp->n_classes; i++)
        if (!if_dom_has_class(n, cp->classes[i])) return false;
    for (u32 i = 0; i < cp->n_ids; i++) {
        IfStr id = if_dom_attr(n, "id");
        if (!(id.n == cp->ids[i].n && memcmp(id.p, cp->ids[i].p, id.n) == 0)) return false;
    }
    return true;
}

static IfNode *elem_parent(const IfNode *n) {
    for (IfNode *p = n->parent; p; p = p->parent)
        if (p->kind == IF_NODE_ELEMENT) return p;
    return NULL;
}

static bool match_at(const IfNode *n, const IfSelector *sel, u32 i) {
    if (!match_compound(n, &sel->comps[i])) return false;
    if (i == 0) return true;
    u8 comb = sel->combs[i - 1];
    if (comb == IF_CX_CHILD) {
        IfNode *p = elem_parent(n);
        return p && match_at(p, sel, i - 1);
    }
    /* descendant: 祖先を遡って試す（バックトラッキング） */
    for (IfNode *p = elem_parent(n); p; p = elem_parent(p))
        if (match_at(p, sel, i - 1)) return true;
    return false;
}

bool if_css_match_selector(const IfNode *n, const IfSelector *sel) {
    if (!sel->n_comps) return false;
    return match_at(n, sel, sel->n_comps - 1);
}

/* ================= カスケード ================= */

static const char IF_UA_SHEET[] =
    "html,body{display:block}"
    "body{margin:8px;font-size:16px;color:#000;background-color:#fff;line-height:1.2}"
    "head,title,meta,link,style,script,input,select,textarea,button,object,iframe,"
    "param,source,track,video,audio,canvas,option{display:none}"
    "div,p,pre,blockquote,address,center,figure,figcaption,header,footer,nav,main,"
    "section,article,aside,form,dl,noscript{display:block}"
    "h1{display:block;font-size:2em;font-weight:bold;margin-top:0.67em;margin-bottom:0.67em}"
    "h2{display:block;font-size:1.5em;font-weight:bold;margin-top:0.83em;margin-bottom:0.83em}"
    "h3{display:block;font-size:1.17em;font-weight:bold;margin-top:1em;margin-bottom:1em}"
    "h4,h5,h6{display:block;font-weight:bold;margin-top:1.33em;margin-bottom:1.33em}"
    "p{margin-top:1em;margin-bottom:1em}"
    "ul,ol{display:block;margin-top:1em;margin-bottom:1em;padding-left:40px}"
    "li{display:list-item}"
    "dd{display:block;margin-left:40px}"
    "dt{display:block}"
    "pre{white-space:pre;margin-top:1em;margin-bottom:1em}"
    "blockquote{display:block;margin-left:40px;margin-right:40px;margin-top:1em;margin-bottom:1em}"
    "b,strong{font-weight:bold}"
    "i,em,cite,dfn,var{font-style:italic}"
    "s,strike{text-decoration:line-through}"
    "u{text-decoration:underline}"
    "a{color:#0000ee;text-decoration:underline}"
    "mark{background-color:#ffff00;color:#000}"
    "hr{display:block;margin-top:0.5em;margin-bottom:0.5em}"
    "small,sub,sup{font-size:0.83em}"
    "big{font-size:1.17em}"
    "table,thead,tbody,tr{display:block}"
    "td,th{display:block}"
    "th{font-weight:bold}"
    "caption{display:block;text-align:center}";

typedef struct {
    bool set;
    u32 spec, order;
    u8 important, origin;
    const IfDecl *decl;
} IfWinner;

static bool winner_beats(const IfWinner *w, const IfWinner *cur) {
    if (!cur->set) return true;
    if (w->important != cur->important) return w->important > cur->important;
    if (w->origin != cur->origin) return w->origin > cur->origin;
    if (w->spec != cur->spec) return w->spec > cur->spec;
    return w->order > cur->order; /* 後勝ち */
}

static void collect_from_sheet_naive(const IfNode *n, const IfStyleSheet *sh, u8 origin, IfWinner win[]) {
    for (u32 r = 0; r < sh->n_rules; r++) {
        const IfRule *rule = &sh->rules[r];
        u32 best_spec = 0; bool matched = false;
        for (u32 s = 0; s < rule->n_sels; s++) {
            if (if_css_match_selector(n, &rule->sels[s])) {
                matched = true;
                if (rule->sels[s].spec > best_spec) best_spec = rule->sels[s].spec;
            }
        }
        if (!matched) continue;
        for (u32 d = 0; d < rule->n_decls; d++) {
            const IfDecl *decl = &rule->decls[d];
            IfWinner w = { true, best_spec, rule->order + d, decl->important, origin, decl };
            if (winner_beats(&w, &win[decl->prop])) win[decl->prop] = w;
        }
    }
}

/* 1 エントリの全マッチ + 勝者収集（naive と同一の winner 規則、spec はその selector 自身） */
static void collect_apply(const IfNode *n, const IfStyleSheet *sh, const IfSelEntry *e,
                          u8 origin, IfWinner win[]) {
    const IfRule *rule = &sh->rules[e->rule];
    const IfSelector *sel = &rule->sels[e->sel];
    if (!if_css_match_selector(n, sel)) return;
    for (u32 d = 0; d < rule->n_decls; d++) {
        const IfDecl *decl = &rule->decls[d];
        IfWinner w = { true, sel->spec, rule->order + d, decl->important, origin, decl };
        if (winner_beats(&w, &win[decl->prop])) win[decl->prop] = w;
    }
}

static void collect_slice(const IfNode *n, const IfStyleSheet *sh,
                          const IfSelBucket *b, u8 origin, IfWinner win[]) {
    if (!b) return;
    for (u32 i = 0; i < b->len; i++)
        collect_apply(n, sh, &sh->rs.pool[b->start + i], origin, win);
}

static int g_css_naive = 0;
void if_css_set_naive_matching(int enabled) { g_css_naive = enabled != 0; }

static bool c_class_ws(u8 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static void collect_from_sheet(IfArena *a, const IfNode *n, const IfStyleSheet *sh, u8 origin, IfWinner win[]) {
    if (g_css_naive || !sh->rs.pool) { collect_from_sheet_naive(n, sh, origin, win); return; }
    if (n->kind != IF_NODE_ELEMENT) return; /* 非要素は右端 compound が必ず不成立（matcher と同値） */
    const IfRuleSet *rs = &sh->rs;

    /* id（照合は memcmp 完全一致 = matcher の cp->ids と同規則） */
    IfStr id = if_dom_attr(n, "id");
    if (id.n) collect_slice(n, sh, rs_find(rs->id_b, rs->n_id, rs_hash(id), id), origin, win);

    /* class（トークン化規則は dom.c if_dom_has_class と同一の空白集合 = matcher 同値） */
    IfStr cv = if_dom_attr(n, "class");
    u32 ci = 0;
    while (ci < cv.n) {
        while (ci < cv.n && c_class_ws((u8)cv.p[ci])) ci++;
        u32 start = ci;
        while (ci < cv.n && !c_class_ws((u8)cv.p[ci])) ci++;
        if (ci == start) break;
        IfStr tok = if_str(cv.p + start, ci - start);
        collect_slice(n, sh, rs_find(rs->cl_b, rs->n_cl, rs_hash(tok), tok), origin, win);
    }

    /* tag（既知 = canonical 静的名、未知 = ASCII-lowercase 複製。索引側と同じ正規化） */
    IfStr tkey = if_str(NULL, 0);
    if (n->tag != IF_TAG_UNKNOWN) {
        const char *nm = if_tag_name(n->tag);
        if (nm) tkey = if_str(nm, (u32)strlen(nm));
    } else if (n->u.tag_name.n) {
        u8 *buf = (u8 *)if_arena_alloc(a, n->u.tag_name.n);
        for (u32 i = 0; i < n->u.tag_name.n; i++) buf[i] = rs_lower((u8)n->u.tag_name.p[i]);
        tkey = if_str((const char *)buf, n->u.tag_name.n);
    }
    if (tkey.n) collect_slice(n, sh, rs_find(rs->tg_b, rs->n_tg, rs_hash(tkey), tkey), origin, win);

    /* universal（特徴なし右端）は常時スキャン */
    for (u32 i = 0; i < rs->univ_len; i++)
        collect_apply(n, sh, &rs->pool[rs->univ_start + i], origin, win);
}

float if_css_resolve_len(IfLen l, float self_fs, float root_fs) {
    switch (l.unit) {
    case IF_U_PX: return l.v;
    case IF_U_PT: return l.v * (96.0f / 72.0f);
    case IF_U_EM: return l.v * self_fs;
    case IF_U_REM: return l.v * root_fs;
    default: return 0.0f; /* PCT/AUTO はレイアウト側で分母を掛けて解決 */
    }
}

enum { IF_ORIGIN_UA, IF_ORIGIN_AUTHOR, IF_ORIGIN_INLINE };

/* 継承するプロパティは compute_node 冒頭で親から直接コピーする（表駆動より単純な方を取る） */

static float kw_font_size(IfStr v, float parent) {
    if (if_str_eq_ci(v, IF_S("xx-small"))) return 9.0f;
    if (if_str_eq_ci(v, IF_S("x-small"))) return 10.0f;
    if (if_str_eq_ci(v, IF_S("small"))) return 13.0f;
    if (if_str_eq_ci(v, IF_S("medium"))) return 16.0f;
    if (if_str_eq_ci(v, IF_S("large"))) return 18.0f;
    if (if_str_eq_ci(v, IF_S("x-large"))) return 24.0f;
    if (if_str_eq_ci(v, IF_S("xx-large"))) return 32.0f;
    if (if_str_eq_ci(v, IF_S("smaller"))) return parent / 1.2f;
    if (if_str_eq_ci(v, IF_S("larger"))) return parent * 1.2f;
    return -1.0f;
}

/* ---- computed style interning（2026-08-01 メモリ本丸） ----
 * IfStyle(124B) を要素ごとに確保すると巨大文書で style arena が ~97MB/16MiB 入力に
 * 膨らむ（実測）。算出後の IfStyle は全フィールド値のみの不変構造かつ calloc 由来で
 * パディングまで決定的 → memcmp 等値で intern できる（損失ゼロの dedup）。
 * 規則: node->style は読み取り専用（カスカード後に誰も書かないことは全ファイル監査済）。
 * 同一内容のスタイルは 1 実体を全要素が指す。巨大文書で数百 unique まで絞られる。
 * 型本体は css.h（IfStyleLazy 経路でも同じ規則で intern するため公開）。 */

u32 if_css_intern_last = 0; /* 観測用: 直近の style apply での unique 数 */

static u64 st_hash(const IfStyle *s) {
    const u8 *p = (const u8 *)s;
    u64 h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof(IfStyle); i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* 同一内容の実体を返す。無ければスタック tmp を arena に定着させて登録する。 */
static const IfStyle *st_intern(IfStyleIntern *in, const IfStyle *tmp) {
    if (in->n * 4 >= in->cap * 3) { /* 負荷率 0.75 で再ハッシュ（×2） */
        u32 nc = in->cap ? in->cap * 2 : 1024;
        IfStyle **nt = (IfStyle **)if_arena_calloc(in->a, (u64)nc * sizeof(IfStyle *));
        for (u32 i = 0; i < in->cap; i++) {
            IfStyle *e = in->tab[i];
            if (!e) continue;
            u32 j = (u32)st_hash(e) & (nc - 1);
            while (nt[j]) j = (j + 1) & (nc - 1);
            nt[j] = e;
        }
        in->tab = nt; in->cap = nc;
    }
    u32 j = (u32)st_hash(tmp) & (in->cap - 1);
    while (in->tab[j]) {
        if (memcmp(in->tab[j], tmp, sizeof(IfStyle)) == 0) return in->tab[j];
        j = (j + 1) & (in->cap - 1);
    }
    IfStyle *e = (IfStyle *)if_arena_alloc(in->a, sizeof(IfStyle));
    memcpy(e, tmp, sizeof(IfStyle));
    in->tab[j] = e;
    in->n++;
    return e;
}

/* ---- 決定メモ化（UA-only ページの style 全走査をほぼ消す） ----
 * computed style の決定変数は (右端照合に響く n の特徴, parent_st, root_fs, sheets)。
 * UA シートはタグセレクタのみ（class/id/属性/擬似を含まない）なので、author シートが
 * 無いページでは (tag or 未知タグ名, parent_st, inline style attr の有無) に縮約できる。
 * 直接マップキャッシュ: 衝突は verify で必ず検出して再計上（損失ゼロの exact memo）。
 * inline style attr を持つ要素は memo を通さない（値が結果に効く）。 */
/* IfStCacheEnt / IF_STCACHE_* / IfStyleIntern は css.h へ移設（IfStyleLazy と共有） */
typedef struct {
    IfStCacheEnt *tab;  /* arena 確保（ページ寿命） */
} IfStCache;

static bool node_has_inline_style(const IfNode *n) {
    for (u32 i = 0; i < n->n_attrs; i++)
        if (if_str_eq_ci(n->attrs[i].name, IF_S("style"))) return true;
    return false;
}

static void compute_node(IfArena *a, IfNode *n, const IfStyle *parent_st, float root_fs,
                         const IfStyleSheet **sheets, u32 n_sheets, IfStyleIntern *in) {
    /* スタックの tmp に算出してから intern する（重複時は arena を一切消費しない）。
     * memcmp 等価のために tmp は必ず全バイト確定的に初期化する（memset 0 + 全フィールド代入） */
    IfStyle tmp;
    memset(&tmp, 0, sizeof tmp);
    IfStyle *st = &tmp;
    n->style = NULL; /* 誤参照防止（最後に intern 結果を代入する） */

    /* 1) 初期値 + 継承 */
    st->display = IF_D_INLINE;         /* 未知要素の既定 */
    st->color = parent_st ? parent_st->color : rgba8(0, 0, 0, 255);
    st->bg = 0;
    st->font_size = parent_st ? parent_st->font_size : 16.0f;
    st->line_height = 0.0f;            /* auto */
    st->bold = parent_st ? parent_st->bold : false;
    st->italic = parent_st ? parent_st->italic : false;
    st->underline = parent_st ? parent_st->underline : false;
    st->strike = parent_st ? parent_st->strike : false;
    st->text_align = parent_st ? parent_st->text_align : IF_TA_LEFT;
    st->white_space = parent_st ? parent_st->white_space : IF_WS_NORMAL;
    st->width = IF_LEN_AUTO; st->height = IF_LEN_AUTO;
    st->border_color = 0;
    for (int i = 0; i < 4; i++) {
        st->margin[i] = (IfLen){ 0, IF_U_PX };
        st->padding[i] = (IfLen){ 0, IF_U_PX };
        st->border_w[i] = 0.0f;
    }
    if (parent_st && parent_st->line_height > 0.0f)
        st->line_height = parent_st->line_height; /* 継承（近似） */

    /* 2) 勝者収集 */
    IfWinner win[IF_P_N];
    memset(win, 0, sizeof win);
    for (u32 s = 0; s < n_sheets; s++)
        collect_from_sheet(a, n, sheets[s], s == 0 ? IF_ORIGIN_UA : IF_ORIGIN_AUTHOR, win);

    /* 3) inline style */
    IfStr style_attr = if_dom_attr(n, "style");
    if (style_attr.p && style_attr.n) {
        IfDecl *decls;
        u32 nd = if_css_parse_decls(a, style_attr, &decls);
        for (u32 d = 0; d < nd; d++) {
            IfWinner w = { true, 0xFFFFFF, 0xFFFFFF, decls[d].important, IF_ORIGIN_INLINE, &decls[d] };
            if (winner_beats(&w, &win[decls[d].prop])) win[decls[d].prop] = w;
        }
    }

    /* 4) 適用（font-size を先に解決してから他を解決するため並び順を制御） */
    static const u8 APPLY_ORDER[IF_P_N] = {
        IF_P_FONT_SIZE, IF_P_LINE_HEIGHT, IF_P_FONT_WEIGHT, IF_P_FONT_STYLE,
        IF_P_WHITE_SPACE, IF_P_TEXT_ALIGN, IF_P_COLOR, IF_P_BACKGROUND_COLOR,
        IF_P_TEXT_DECORATION, IF_P_DISPLAY,
        IF_P_MARGIN_TOP, IF_P_MARGIN_RIGHT, IF_P_MARGIN_BOTTOM, IF_P_MARGIN_LEFT,
        IF_P_PADDING_TOP, IF_P_PADDING_RIGHT, IF_P_PADDING_BOTTOM, IF_P_PADDING_LEFT,
        IF_P_BORDER_TOP_WIDTH, IF_P_BORDER_RIGHT_WIDTH, IF_P_BORDER_BOTTOM_WIDTH, IF_P_BORDER_LEFT_WIDTH,
        IF_P_BORDER_COLOR, IF_P_WIDTH, IF_P_HEIGHT,
    };
    _Static_assert(sizeof(APPLY_ORDER) == IF_P_N, "APPLY_ORDER must cover all props");
    for (u32 oi = 0; oi < IF_P_N; oi++) {
        u16 p = APPLY_ORDER[oi];
        if (!win[p].set) continue;
        const IfDecl *d = win[p].decl;
        switch (p) {
        case IF_P_FONT_SIZE: {
            float v = -1.0f;
            if (d->vkind == IF_V_LEN) {
                IfLen l = { d->num, d->unit };
                if (l.unit == IF_U_PCT) v = (parent_st ? parent_st->font_size : 16.0f) * l.v / 100.0f;
                else v = if_css_resolve_len(l, parent_st ? parent_st->font_size : 16.0f, root_fs); /* em は親基準 */
            } else if (d->vkind == IF_V_RAW) {
                v = kw_font_size(d->text, parent_st ? parent_st->font_size : 16.0f);
            }
            if (v > 0.0f && v < 10000.0f) st->font_size = v;
            break;
        }
        case IF_P_LINE_HEIGHT: {
            if (d->vkind == IF_V_LEN) {
                IfLen l = { d->num, d->unit };
                float v = (l.unit == IF_U_PCT) ? st->font_size * l.v / 100.0f : if_css_resolve_len(l, st->font_size, root_fs);
                if (v > 0.0f && v < 10000.0f) st->line_height = v;
            } else if (d->vkind == IF_V_RAW) {
                if (if_str_eq_ci(d->text, IF_S("normal"))) st->line_height = 0.0f;
                else {
                    /* 無単位数値: font-size の倍率 */
                    float mult = -1.0f; bool ok = true;
                    float acc = 0.0f; u32 k = 0; bool digits = false;
                    for (; k < d->text.n; k++) {
                        char ch = d->text.p[k];
                        if (ch >= '0' && ch <= '9') { acc = acc * 10.0f + (float)(ch - '0'); digits = true; }
                        else if (ch == '.' && digits) { /* 小数は粗く読む */ k++; float frac = 0.1f; for (; k < d->text.n && d->text.p[k] >= '0' && d->text.p[k] <= '9'; k++) { acc += frac * (float)(d->text.p[k] - '0'); frac *= 0.1f; } k--; }
                        else { ok = false; break; }
                    }
                    if (ok && digits) mult = acc;
                    if (mult >= 0.0f) { st->line_height = mult * st->font_size; }
                }
            }
            break;
        }
        case IF_P_FONT_WEIGHT: {
            IfStr v = d->vkind == IF_V_RAW ? if_str_trim(d->text) : if_str(NULL, 0);
            if (d->vkind == IF_V_RAW) {
                if (if_str_eq_ci(v, IF_S("bold")) || if_str_eq_ci(v, IF_S("bolder"))) st->bold = true;
                else if (if_str_eq_ci(v, IF_S("normal")) || if_str_eq_ci(v, IF_S("lighter"))) st->bold = false;
                else {
                    long num = 0; bool ok = true;
                    for (u32 k = 0; k < v.n; k++) {
                        if (v.p[k] < '0' || v.p[k] > '9') { ok = false; break; }
                        num = num * 10 + (v.p[k] - '0');
                    }
                    if (ok && v.n) st->bold = num >= 600;
                }
            }
            break;
        }
        case IF_P_FONT_STYLE:
            if (d->vkind == IF_V_RAW) st->italic = if_str_eq_ci(if_str_trim(d->text), IF_S("italic")) || if_str_eq_ci(if_str_trim(d->text), IF_S("oblique"));
            break;
        case IF_P_WHITE_SPACE:
            if (d->vkind == IF_V_RAW) {
                IfStr v = if_str_trim(d->text);
                if (if_str_eq_ci(v, IF_S("pre")) || if_str_eq_ci(v, IF_S("pre-wrap"))) st->white_space = IF_WS_PRE;
                else if (if_str_eq_ci(v, IF_S("normal")) || if_str_eq_ci(v, IF_S("pre-line"))) st->white_space = IF_WS_NORMAL;
            }
            break;
        case IF_P_TEXT_ALIGN:
            if (d->vkind == IF_V_RAW) {
                IfStr v = if_str_trim(d->text);
                if (if_str_eq_ci(v, IF_S("center"))) st->text_align = IF_TA_CENTER;
                else if (if_str_eq_ci(v, IF_S("right")) || if_str_eq_ci(v, IF_S("end"))) st->text_align = IF_TA_RIGHT;
                else if (if_str_eq_ci(v, IF_S("left")) || if_str_eq_ci(v, IF_S("start"))) st->text_align = IF_TA_LEFT;
                /* justify は未対応: 現状値維持 */
            }
            break;
        case IF_P_COLOR:
            if (d->vkind == IF_V_COLOR) st->color = d->color;
            break;
        case IF_P_BACKGROUND_COLOR:
            if (d->vkind == IF_V_COLOR) st->bg = d->color;
            break;
        case IF_P_TEXT_DECORATION: {
            if (d->vkind == IF_V_RAW) {
                IfStr v = d->text;
                u32 i = 0;
                st->underline = false; st->strike = false;
                while (i < v.n) {
                    while (i < v.n && c_ws((u8)v.p[i])) i++;
                    u32 s = i;
                    while (i < v.n && !c_ws((u8)v.p[i])) i++;
                    IfStr w = if_str(v.p + s, i - s);
                    if (if_str_eq_ci(w, IF_S("underline"))) st->underline = true;
                    else if (if_str_eq_ci(w, IF_S("line-through"))) st->strike = true;
                    else if (if_str_eq_ci(w, IF_S("none"))) { st->underline = false; st->strike = false; }
                    /* overline/blink は丸ごと無視 */
                }
            }
            break;
        }
        case IF_P_DISPLAY:
            if (d->vkind == IF_V_RAW) {
                IfStr v = if_str_trim(d->text);
                if (if_str_eq_ci(v, IF_S("none"))) st->display = IF_D_NONE;
                else if (if_str_eq_ci(v, IF_S("inline"))) st->display = IF_D_INLINE;
                else if (if_str_eq_ci(v, IF_S("block"))) st->display = IF_D_BLOCK;
                else if (if_str_eq_ci(v, IF_S("list-item"))) st->display = IF_D_LIST_ITEM;
                else if (if_str_eq_ci(v, IF_S("inline-block"))) st->display = IF_D_INLINE;  /* 近似 */
                else if (if_str_eq_ci(v, IF_S("table")) || if_str_eq_ci(v, IF_S("table-row")) ||
                         if_str_eq_ci(v, IF_S("table-cell")) || if_str_eq_ci(v, IF_S("table-row-group")) ||
                         if_str_eq_ci(v, IF_S("table-header-group"))) st->display = IF_D_BLOCK; /* 近似 */
                /* flex/grid 等は v0.1 非対応 → 変更しない */
            }
            break;
        case IF_P_MARGIN_TOP: case IF_P_MARGIN_RIGHT: case IF_P_MARGIN_BOTTOM: case IF_P_MARGIN_LEFT: {
            int idx = p - IF_P_MARGIN_TOP;
            if (d->vkind == IF_V_LEN) st->margin[idx] = (IfLen){ d->num, d->unit };
            break;
        }
        case IF_P_PADDING_TOP: case IF_P_PADDING_RIGHT: case IF_P_PADDING_BOTTOM: case IF_P_PADDING_LEFT: {
            int idx = p - IF_P_PADDING_TOP;
            if (d->vkind == IF_V_LEN) st->padding[idx] = (IfLen){ d->num, d->unit };
            break;
        }
        case IF_P_BORDER_TOP_WIDTH: case IF_P_BORDER_RIGHT_WIDTH:
        case IF_P_BORDER_BOTTOM_WIDTH: case IF_P_BORDER_LEFT_WIDTH: {
            int idx = p - IF_P_BORDER_TOP_WIDTH;
            if (d->vkind == IF_V_LEN) {
                float v = if_css_resolve_len((IfLen){ d->num, d->unit }, st->font_size, root_fs);
                if (v >= 0.0f && v < 10000.0f) st->border_w[idx] = v;
            }
            break;
        }
        case IF_P_BORDER_COLOR:
            if (d->vkind == IF_V_COLOR) st->border_color = d->color;
            break;
        case IF_P_WIDTH:
            if (d->vkind == IF_V_LEN) st->width = (IfLen){ d->num, d->unit };
            break;
        case IF_P_HEIGHT:
            if (d->vkind == IF_V_LEN) st->height = (IfLen){ d->num, d->unit };
            break;
        default: break;
        }
    }
    /* 算出確定。不変かつ memcmp 等価が効くので intern して実体指す（dedup、損失ゼロ） */
    n->style = st_intern(in, &tmp);
}

/* 1 要素の決定解決（memo hit or compute_node）。compute_walk と IfStyleLazy の
 * 双方がこの同一手続きを使う（規則の一点化＝同値性の構造保証）。
 * キー・ハッシュ・memo ゲートは compute_walk の旧インラインブロックと厳密一致。 */
static const IfStyle *st_resolve_memo(IfArena *a, IfStCacheEnt *tab, IfNode *n,
                                      const IfStyle *parent_st, float rfs,
                                      const IfStyleSheet **sheets, u32 n_sheets,
                                      IfStyleIntern *in) {
    uintptr_t pk = (uintptr_t)parent_st;
    uintptr_t k2 = (n->tag != IF_TAG_UNKNOWN)
        ? (((uintptr_t)n->tag << 1) | 1u)
        : (uintptr_t)n->u.tag_name.p;
    if (tab && !node_has_inline_style(n)) {
        u64 h = (pk >> 4) * 2654435761u + k2 * 40503u;
        IfStCacheEnt *e = &tab[h & (IF_STCACHE_SIZE - 1)];
        if (e->st && e->parent == pk && e->key2 == k2) { n->style = e->st; return e->st; }
        compute_node(a, n, parent_st, rfs, sheets, n_sheets, in);
        e->parent = pk; e->key2 = k2; e->st = n->style;
        return n->style;
    }
    compute_node(a, n, parent_st, rfs, sheets, n_sheets, in);
    return n->style;
}

static void compute_walk(IfArena *a, IfNode *n, const IfStyle *parent_st, float root_fs,
                         const IfStyleSheet **sheets, u32 n_sheets, IfStyleIntern *in,
                         IfStCache *cache) {
    /* 反復 DFS（深い文書ツリーでも C スタックを消費しない） */
    typedef struct { IfNode *n; const IfStyle *pst; float rfs; IfNode *next_sib; } Fr;
    if (n->kind != IF_NODE_ELEMENT) return;
    Fr stack[64];
    int sp = 0;
    const IfStyle *cur_parent = parent_st;
    float cur_rfs = root_fs;
    IfNode *cur = n;
    for (;;) {
        if (cur->kind == IF_NODE_ELEMENT) {
            const IfStyle *st = st_resolve_memo(a, cache ? cache->tab : NULL, cur,
                                                cur_parent, cur_rfs, sheets, n_sheets, in);
            float child_rfs = cur_rfs;
            if (cur->tag == IF_TAG_HTML) child_rfs = st->font_size;
            /* 子を潜る: frame を積む */
            IfNode *fc = cur->first_child;
            while (fc && fc->kind != IF_NODE_ELEMENT) fc = fc->next_sibling;
            if (fc) {
                if (sp < 64) {
                    stack[sp].n = cur->next_sibling;
                    stack[sp].pst = cur_parent;
                    stack[sp].rfs = cur_rfs;
                    sp++;
                    cur = fc; cur_parent = st; cur_rfs = child_rfs;
                    continue;
                } else {
                    /* 深さ超過: 再帰フォールバック（性能のみ、正しさ不変） */
                    for (IfNode *c = fc; c; c = c->next_sibling)
                        if (c->kind == IF_NODE_ELEMENT)
                            compute_walk(a, c, st, child_rfs, sheets, n_sheets, in, cache);
                }
            }
        }
        /* 兄弟へ、無ければ frame を降りる（frame の兄弟 NULL は更に降りる） */
        for (;;) {
            if (cur->next_sibling) { cur = cur->next_sibling; break; }
            if (sp == 0) return;
            sp--;
            cur_parent = stack[sp].pst;
            cur_rfs = stack[sp].rfs;
            if (stack[sp].n) { cur = stack[sp].n; break; }
            if (sp == 0) return;
        }
    }
}

static void collect_author_sheets(IfArena *a, IfNode *n, const IfStyleSheet ***arr, u32 *count, u64 *cap, u32 *order) {
    if (n->kind == IF_NODE_ELEMENT && n->tag == IF_TAG_STYLE) {
        IfStr css = if_dom_text_content(a, n);
        if (css.n) {
            IfStyleSheet *sh = if_css_parse(a, css, *order);
            *order = sh->order_end + 1; /* シート間の order 衝突回避（消費分を正確に積む。旧 64/rule 見積もりは decl 多数で破綻し得た） */
            *arr = (const IfStyleSheet **)if_arena_grow(a, (void *)*arr, cap, *count + 1, sizeof(IfStyleSheet *));
            (*arr)[(*count)++] = sh;
        }
        return; /* style の中身は更に潜らない */
    }
    for (IfNode *c = n->first_child; c; c = c->next_sibling)
        collect_author_sheets(a, c, arr, count, cap, order);
}

void if_style_apply(IfArena *a, IfDom *dom) {
    if (!dom || !dom->root) return;
    IfStyleSheet *ua = if_css_parse(a, if_str(IF_UA_SHEET, (u32)sizeof(IF_UA_SHEET) - 1), 0);

    const IfStyleSheet **sheets = NULL;
    u32 n_sheets = 0; u64 cap = 0; u32 order = 100000;
    if (dom->has_style)
        collect_author_sheets(a, dom->root, &sheets, &n_sheets, &cap, &order);

    const IfStyleSheet **all = (const IfStyleSheet **)if_arena_alloc(a, (n_sheets + 1) * sizeof(IfStyleSheet *));
    all[0] = ua;
    for (u32 i = 0; i < n_sheets; i++) all[i + 1] = sheets[i];

    IfNode *html = if_node_first_elem_child(dom->root);
    if (html) {
        IfStyleIntern in = { NULL, 0, 0, a };
        /* author シートが無いときだけ決定メモ化を有効化（UA シートはタグセレクタのみ） */
        IfStCache cache = { NULL };
        IfStCache *cp = NULL;
        if (n_sheets == 0) {
            cache.tab = (IfStCacheEnt *)if_arena_calloc(a, IF_STCACHE_SIZE * sizeof(IfStCacheEnt));
            cp = &cache;
        }
        compute_walk(a, html, NULL, 16.0f, all, n_sheets + 1, &in, cp);
        if_css_intern_last = in.n;
    }
}

/* ---- lazy computed style（css.h の設計注釈参照） ----
 * if_style_apply との同値性:
 *   - シート列は [UA] のみ（author 無しは if_md_style_lazy_ok が保証）
 *   - 解決は st_resolve_memo 一点（compute_walk と同一手続き・同一キー・同一ハッシュ）
 *   - 解決順は layout の DFS（pre-order で compute_walk と一致）なので intern の
 *     挿入系列も一致し、ポインタ帰属まで同じ（ただし並列 shard は arena 別・値のみ保証） */
void if_style_lazy_init(IfStyleLazy *lz, IfArena *a) {
    lz->arena = a;
    lz->sheet = if_css_parse(a, if_str(IF_UA_SHEET, (u32)sizeof(IF_UA_SHEET) - 1), 0);
    lz->in.tab = NULL; lz->in.cap = 0; lz->in.n = 0; lz->in.a = a;
    lz->ctab = (IfStCacheEnt *)if_arena_calloc(a, IF_STCACHE_SIZE * sizeof(IfStCacheEnt));
    lz->rfs = 16.0f;
}

const IfStyle *if_style_lazy_get(IfStyleLazy *lz, IfNode *n, const IfStyle *parent_st, float rfs) {
    const IfStyleSheet *ss[1] = { lz->sheet };
    return st_resolve_memo(lz->arena, lz->ctab, n, parent_st, rfs, ss, 1, &lz->in);
}

bool if_md_style_lazy_ok(const IfDom *dom) {
    if (!dom || !dom->md_ws_stripped || dom->has_style) return false;
    const char *e = getenv("IF_STYLE_LAZY"); /* kill switch */
    return !(e && e[0] == '0');
}
