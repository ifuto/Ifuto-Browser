/* Ifuto — セルグリッドレンダラ実装（ソフトウェアラスタ） */
#include "render.h"
#include "utf8.h"
#include <stdio.h>
#include <string.h>

u8 if_rgba_to_ansi(u32 rgba) {
    u32 a = rgba & 0xFF;
    if (a < 128) return (u8)IF_CELL_DEFAULT;
    u32 r = rgba >> 24, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF;
    /* グレー特別経路: 立方体の端に寄らず灰色ランプへ */
    if (r == g && g == b) {
        if (r < 8) return 16;
        if (r > 238) return 15;
        return (u8)(232 + ((r - 8) * 24) / 240);
    }
    u32 rq = (r * 5 + 127) / 255, gq = (g * 5 + 127) / 255, bq = (b * 5 + 127) / 255;
    return (u8)(16 + 36 * rq + 6 * gq + bq);
}

typedef struct { i32 x, y; } IfMax;

static void grid_max_walk(const IfBox *b, i32 *mx, i32 *my) {
    if (b->kind == IF_BOX_LINE) {
        for (u32 i = 0; i < b->n_segs; i++) {
            if (b->segs[i].x + b->segs[i].w > *mx) *mx = b->segs[i].x + b->segs[i].w;
        }
        if (b->y + b->h > *my) *my = b->y + b->h;
        return;
    }
    if (b->x + b->w > *mx) *mx = b->x + b->w;
    if (b->y + b->h > *my) *my = b->y + b->h;
    for (const IfBox *c = b->first_child; c; c = c->next_sibling) grid_max_walk(c, mx, my);
}

static IfCell *grid_at(IfGrid *g, i32 x, i32 y) {
    if (x < 0 || y < 0 || x >= g->w || y >= g->h) return NULL;
    return &g->cells[(i64)y * g->w + x];
}

static void put_cp(IfGrid *g, i32 x, i32 y, u32 cp, const IfStyle *st, u32 bg_override) {
    IfCell *c = grid_at(g, x, y);
    if (!c) return;
    c->cp = cp;
    c->fg = st ? if_rgba_to_ansi(st->color) : (u8)IF_CELL_DEFAULT;
    u32 bg = bg_override ? bg_override : (st ? st->bg : 0);
    c->bg = if_rgba_to_ansi(bg);
    c->flags = 0;
    if (st) {
        if (st->bold) c->flags |= IF_F_BOLD;
        if (st->italic) c->flags |= IF_F_ITALIC;
        if (st->underline) c->flags |= IF_F_ULINE;
        if (st->strike) c->flags |= IF_F_STRIKE;
    }
}

/* 背景のみ上書き（テキストは保持） */
static void fill_bg(IfGrid *g, i32 x0, i32 y0, i32 w, i32 h, u32 bg) {
    u8 idx = if_rgba_to_ansi(bg);
    if (idx == IF_CELL_DEFAULT && (bg & 0xFF) < 128) return;
    for (i32 y = y0; y < y0 + h; y++)
        for (i32 x = x0; x < x0 + w; x++) {
            IfCell *c = grid_at(g, x, y);
            if (c) c->bg = idx;
        }
}

static void draw_text(IfGrid *g, i32 x, i32 y, IfStr text, const IfStyle *st) {
    const u8 *s = (const u8 *)text.p;
    u32 i = 0;
    i32 cx = x;
    while (i < text.n) {
        u32 cp = if_utf8_decode(s, text.n, &i);
        int gw = if_glyph_width(cp);
        if (gw == 0) continue;
        put_cp(g, cx, y, cp, st, 0);
        if (gw == 2) {
            IfCell *c = grid_at(g, cx + 1, y);
            if (c) { c->cp = 0; c->fg = st ? if_rgba_to_ansi(st->color) : (u8)IF_CELL_DEFAULT;
                     c->bg = if_rgba_to_ansi(st ? st->bg : 0); c->flags = 0; }
        }
        cx += gw;
    }
}

static void draw_hline(IfGrid *g, i32 x0, i32 x1, i32 y, const IfStyle *st) {
    for (i32 x = x0; x < x1; x++) put_cp(g, x, y, 0x2500, st, 0); /* ─ */
}

/* li マーカー: 直近の祖先 ul/ol と、li 兄弟内の順序から決める。
 * 配置はボックス左端+1（ul: "• "、ol: 右揃えの "N."）。padding-left の帯に収まる近似。 */
static void draw_marker(IfGrid *g, const IfBox *b) {
    IfNode *li = b->node;
    if (!li) return;
    u16 list = IF_TAG_UL;
    for (IfNode *p = li->parent; p; p = p->parent) {
        if (p->kind == IF_NODE_ELEMENT && (p->tag == IF_TAG_UL || p->tag == IF_TAG_OL)) { list = p->tag; break; }
        if (p->kind == IF_NODE_ELEMENT && p->tag == IF_TAG_LI) break; /* ネストした li の外に出ない */
    }
    /* マーカーは li のコンテンツ左外側（親リストの padding 帯）に置く。
     * li 自身のテキスト行開始位置 = b->x なので、その手前なら上書きされない。 */
    if (list == IF_TAG_UL) {
        i32 mx = b->x - 2;
        if (mx < 0) mx = b->x;
        draw_text(g, mx, b->y, if_str("\xE2\x80\xA2 ", 4), b->st); /* "• " */
        return;
    }
    u32 idx = 1;
    for (IfNode *s = li->parent ? li->parent->first_child : NULL; s && s != li; s = s->next_sibling)
        if (s->kind == IF_NODE_ELEMENT && s->tag == IF_TAG_LI && s->style && s->style->display == IF_D_LIST_ITEM)
            idx++;
    char buf[16];
    int m = snprintf(buf, sizeof buf, "%u.", idx);
    if (m < 0) return;
    i32 mx = b->x - (m + 1); /* "N. " が b->x-1 で終わる右揃え */
    if (mx < 0) mx = 0;
    draw_text(g, mx, b->y, if_str(buf, (u32)m), b->st);
}

static void paint_box(IfGrid *g, const IfBox *b) {
    if (b->kind == IF_BOX_LINE) {
        for (u32 i = 0; i < b->n_segs; i++) {
            const IfSeg *s = &b->segs[i];
            draw_text(g, s->x, b->y, s->text, s->st);
        }
        return;
    }
    const IfStyle *st = b->st;
    if (st && (st->bg & 0xFF) >= 128) fill_bg(g, b->x, b->y, b->w, b->h, st->bg);

    if (b->node && b->node->tag == IF_TAG_HR) {
        /* bt を除いた行へ罫線 */
        i32 off = (st && st->border_w[0] > 0.0f) ? 1 : 0;
        draw_hline(g, b->x, b->x + b->w, b->y + off, NULL);
        return;
    }

    /* 罫線（solid のみ。Unicode 罫線素片で描く） */
    if (st) {
        u32 bc = st->border_color;
        u8 fg = if_rgba_to_ansi(bc);
        bool any = st->border_w[0] > 0 || st->border_w[1] > 0 || st->border_w[2] > 0 || st->border_w[3] > 0;
        if (any) {
            for (i32 x = b->x; x < b->x + b->w; x++) {
                if (st->border_w[0] > 0) { IfCell *c = grid_at(g, x, b->y); if (c) { c->cp = 0x2500; c->fg = fg; } }
                if (st->border_w[2] > 0) { IfCell *c = grid_at(g, x, b->y + b->h - 1); if (c) { c->cp = 0x2500; c->fg = fg; } }
            }
            for (i32 y = b->y; y < b->y + b->h; y++) {
                if (st->border_w[3] > 0) { IfCell *c = grid_at(g, b->x, y); if (c) { c->cp = 0x2502; c->fg = fg; } }
                if (st->border_w[1] > 0) { IfCell *c = grid_at(g, b->x + b->w - 1, y); if (c) { c->cp = 0x2502; c->fg = fg; } }
            }
            IfCell *c;
            if (st->border_w[0] > 0 && st->border_w[3] > 0) { c = grid_at(g, b->x, b->y); if (c) c->cp = 0x250C; }
            if (st->border_w[0] > 0 && st->border_w[1] > 0) { c = grid_at(g, b->x + b->w - 1, b->y); if (c) c->cp = 0x2510; }
            if (st->border_w[2] > 0 && st->border_w[3] > 0) { c = grid_at(g, b->x, b->y + b->h - 1); if (c) c->cp = 0x2514; }
            if (st->border_w[2] > 0 && st->border_w[1] > 0) { c = grid_at(g, b->x + b->w - 1, b->y + b->h - 1); if (c) c->cp = 0x2518; }
        }
        if (b->node && b->st->display == IF_D_LIST_ITEM) draw_marker(g, b);
    }

    for (const IfBox *c = b->first_child; c; c = c->next_sibling) paint_box(g, c);
}

IfGrid *if_render_grid(IfArena *arena, const IfLayout *lay) {
    i32 mx = lay->width, my = lay->height;
    if (lay->root) grid_max_walk(lay->root, &mx, &my);
    if (my < 1) my = 1;
    if (mx < 1) mx = 1;
    /* pre のはみ出し等で広がるのは許す。上限は暴力的な文書への防御 */
    if ((i64)mx * my > 64LL * 1024 * 1024) if_fatal("grid too large");
    IfGrid *g = (IfGrid *)if_arena_calloc(arena, sizeof(IfGrid));
    g->w = mx; g->h = my;
    g->cells = (IfCell *)if_arena_alloc(arena, (u64)mx * (u64)my * sizeof(IfCell));
    for (i64 i = 0; i < (i64)mx * my; i++) {
        g->cells[i].cp = ' ';
        g->cells[i].fg = (u8)IF_CELL_DEFAULT;
        g->cells[i].bg = (u8)IF_CELL_DEFAULT;
        g->cells[i].flags = 0;
    }
    if (lay->root) paint_box(g, lay->root);
    return g;
}

/* ---------- 発行 ---------- */

typedef struct { u8 *buf; u64 len, cap; IfArena *a; } IfOut;

static void out_bytes(IfOut *o, const void *p, u64 n) {
    o->buf = (u8 *)if_arena_grow(o->a, o->buf, &o->cap, o->len + n, 1);
    memcpy(o->buf + o->len, p, n);
    o->len += n;
}
static void out_str(IfOut *o, const char *s) { out_bytes(o, s, strlen(s)); }
static void out_u32(IfOut *o, u32 v) {
    char tmp[12];
    int n = snprintf(tmp, sizeof tmp, "%u", v);
    out_bytes(o, tmp, (u64)n);
}

typedef struct { u8 fg, bg, flags; } IfPen;

static void pen_apply(IfOut *o, const IfPen *pen) {
    out_str(o, "\x1b[0m");
    if (pen->flags & IF_F_BOLD) out_str(o, "\x1b[1m");
    if (pen->flags & IF_F_ITALIC) out_str(o, "\x1b[3m");
    if (pen->flags & IF_F_ULINE) out_str(o, "\x1b[4m");
    if (pen->flags & IF_F_STRIKE) out_str(o, "\x1b[9m");
    if (pen->fg != IF_CELL_DEFAULT) { out_str(o, "\x1b[38;5;"); out_u32(o, pen->fg); out_str(o, "m"); }
    if (pen->bg != IF_CELL_DEFAULT) { out_str(o, "\x1b[48;5;"); out_u32(o, pen->bg); out_str(o, "m"); }
}

IfStr if_render_emit(IfArena *arena, const IfGrid *grid, int ansi) {
    IfOut o = { NULL, 0, 0, arena };
    IfPen cur = { (u8)IF_CELL_DEFAULT, (u8)IF_CELL_DEFAULT, 0 };
    bool pen_dirty = false;

    for (i32 y = 0; y < grid->h; y++) {
        i32 last = grid->w - 1;
        /* プレーン出力では行末空白を落とす */
        if (!ansi) {
            while (last >= 0 && grid->cells[(i64)y * grid->w + last].cp == ' ') last--;
        }
        for (i32 x = 0; x <= last; x++) {
            const IfCell *c = &grid->cells[(i64)y * grid->w + x];
            if (c->cp == 0) continue; /* 全角 2 セル目 */
            if (ansi) {
                IfPen p = { c->fg, c->bg, c->flags };
                if (p.fg != cur.fg || p.bg != cur.bg || p.flags != cur.flags) {
                    pen_apply(&o, &p);
                    cur = p;
                    pen_dirty = true;
                }
            }
            u8 enc[4];
            u32 cp = c->cp ? c->cp : ' ';
            u64 n = if_utf8_encode(cp, enc);
            out_bytes(&o, enc, n);
        }
        if (ansi && pen_dirty) { out_str(&o, "\x1b[0m"); cur = (IfPen){ (u8)IF_CELL_DEFAULT, (u8)IF_CELL_DEFAULT, 0 }; }
        out_str(&o, "\n");
    }
    return if_str((const char *)o.buf, (u32)o.len);
}
