/* Ifuto — セルグリッドレンダラ実装（ソフトウェアラスタ） */
#include "render.h"
#include "utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long CNT_PAINT, CNT_SHELL, CNT_SEGS, CNT_FILLBG, CNT_MARKER;
static unsigned long long CNT_FILLBG_CELLS, CNT_MARKER_SIB, CNT_DEC;
__attribute__((destructor)) static void paintcnt_dump(void) {
    if (getenv("PAINTCNT"))
        fprintf(stderr, "PAINTCNT paint=%llu shell=%llu segs=%llu fillbg=%llu fillbg_cells=%llu marker=%llu marker_sib=%llu dec_break=%llu\n",
                CNT_PAINT, CNT_SHELL, CNT_SEGS, CNT_FILLBG, CNT_FILLBG_CELLS, CNT_MARKER, CNT_MARKER_SIB, CNT_DEC);
}

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
    if (x < 0 || y < g->y_off || x >= g->w || y >= g->y_off + g->h) return NULL;
    return &g->cells[(i64)(y - g->y_off) * g->w + x];
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
    /* ループの前に窓へ切り詰める（2026-08-01 同定: 全文書を跨ぐ背景箱が
     * 窓ごとに「全行 × grid_at NULL 拒否」で 88 億セル走査していた実測起因）。
     * 切り詰め後は全セル窓内が保証される。grid_at の拒否は安全側として残す
     * （ペイント順序==バイト列は tui/gui smoke の sha256 が機械監査） */
    i32 yy0 = y0 < g->y_off ? g->y_off : y0;
    i32 yy1 = y0 + h > g->y_off + g->h ? g->y_off + g->h : y0 + h;
    i32 xx0 = x0 < 0 ? 0 : x0;
    i32 xx1 = x0 + w > g->w ? g->w : x0 + w;
    for (i32 y = yy0; y < yy1; y++)
        for (i32 x = xx0; x < xx1; x++) {
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
    for (IfNode *s = li->parent ? li->parent->first_child : NULL; s && s != li; s = s->next_sibling) {
        CNT_MARKER_SIB++;
        if (s->kind == IF_NODE_ELEMENT && s->tag == IF_TAG_LI && s->style && s->style->display == IF_D_LIST_ITEM)
            idx++;
    }
    char buf[16];
    int m = snprintf(buf, sizeof buf, "%u.", idx);
    if (m < 0) return;
    i32 mx = b->x - (m + 1); /* "N. " が b->x-1 で終わる右揃え */
    if (mx < 0) mx = 0;
    draw_text(g, mx, b->y, if_str(buf, (u32)m), b->st);
}

static unsigned long long CNT_PAINT, CNT_SHELL, CNT_SEGS, CNT_FILLBG, CNT_MARKER, CNT_DEC;
static void paint_box(IfGrid *g, const IfBox *b); /* 前方宣言: paint_children から相互参照 */

/* b 自身の装飾（背景/HR/罫線/li マーカー）を描く。子供に進むなら true。
 * HR だけは自身の描画で完結するため false を返す。 */
static bool paint_shell(IfGrid *g, const IfBox *b) {
    const IfStyle *st = b->st;
    CNT_SHELL++;
    if (st && (st->bg & 0xFF) >= 128) { CNT_FILLBG++; fill_bg(g, b->x, b->y, b->w, b->h, st->bg); }

    if (b->node && b->node->tag == IF_TAG_HR) {
        /* bt を除いた行へ罫線 */
        i32 off = (st && st->border_w[0] > 0.0f) ? 1 : 0;
        draw_hline(g, b->x, b->x + b->w, b->y + off, NULL);
        return false;
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
        if (b->node && b->st->display == IF_D_LIST_ITEM) { CNT_MARKER++; draw_marker(g, b); }
    }
    return true;
}

/* 子供イテレーション（start_from: NULL なら b->first_child から）。
 * record: NULL でなければ「窓に最初に交差した子」を *record に書く（窓カーソル用） */
static void paint_children(IfGrid *g, const IfBox *b, const IfBox *start_from, const IfBox **record) {
    i32 gy0 = g->y_off, gy1 = g->y_off + g->h;
    for (const IfBox *c = start_from ? start_from : b->first_child; c; c = c->next_sibling) {
        /* この layout は兄弟の y が単調非減少（垂直フローのみ。負マージン/浮動なし）。
         * 窓の下端を超えた時点で以降の兄弟は全て窓外なので走査自体を打ち切る
         * （打ち切らないと「窓より後の全兄弟」の空走査が全窓に重複して O(n^2) になる）。
         * バイト列同一性は tui/gui smoke の sha256 が機械監査する。 */
        if (c->y >= gy1) { CNT_DEC++; break; }
        /* 窓との交差判定（record 用）: この子かその子孫が窓内に描きうる最初の子 */
        if (record) {
            bool inter = (c->kind == IF_BOX_LINE)
                ? (c->y >= gy0 && c->y < gy1)
                : (c->h == 0 || (i64)c->y + c->h > gy0);
            if (inter) { *record = c; record = NULL; }
        }
        paint_box(g, c);
    }
}

static void paint_box(IfGrid *g, const IfBox *b) {
    /* 窓グリッドの部分木剪定（巨大文書 O(窓数×box総数) → O(box+描画cell)）。
     * grid の y 窓 [y0,y1) と交差しない line ボックス/高さ確定ボックスは子孫ごと捨てる。
     * 安全根拠: この layout では子の描画 y 範囲は親の [y, y+h) に包含される
     * （親はフロー内で子を含んで伸びる）。h==0 の容器だけは保守側で潜る（稀）。
     * 発行バイト列は tests の tui/gui smoke sha256 が if_render_emit との完全一致で機械監査。 */
    i32 gy0 = g->y_off, gy1 = g->y_off + g->h;
    if (b->kind == IF_BOX_LINE) {
        if (b->y < gy0 || b->y >= gy1) return;
        CNT_PAINT++; CNT_SEGS += b->n_segs;
        for (u32 i = 0; i < b->n_segs; i++) {
            const IfSeg *s = &b->segs[i];
            draw_text(g, s->x, b->y, s->text, s->st);
        }
        return;
    }
    if (b->h > 0 && (b->y >= gy1 || b->y + b->h <= gy0)) return;
    CNT_PAINT++;
    if (paint_shell(g, b))
        paint_children(g, b, NULL, NULL);
}

IfGrid *if_render_grid(IfArena *arena, const IfLayout *lay) {
    i32 mx = lay->width, my = lay->height;
    if (lay->root) grid_max_walk(lay->root, &mx, &my);
    if (my < 1) my = 1;
    if (mx < 1) mx = 1;
    /* pre のはみ出し等で広がるのは許す。上限は暴力的な文書への防御 */
    if ((i64)mx * my > 64LL * 1024 * 1024) if_fatal("grid too large");
    IfGrid *g = (IfGrid *)if_arena_calloc(arena, sizeof(IfGrid));
    g->w = mx; g->h = my; g->y_off = 0;
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

void if_render_grid_rows_into_cur(const IfLayout *lay, i32 row0, i32 row1, IfGrid *out, IfPaintCursor *cur) {
    out->w = lay->width;
    out->h = row1 > row0 ? row1 - row0 : 0;
    out->y_off = row0;
    if (out->h <= 0) return;
    /* 既定セル充填＝フルビルダと同一規約（space + DEFAULT 色） */
    for (i64 i = 0; i < (i64)out->w * out->h; i++) {
        out->cells[i].cp = ' ';
        out->cells[i].fg = (u8)IF_CELL_DEFAULT;
        out->cells[i].bg = (u8)IF_CELL_DEFAULT;
        out->cells[i].flags = 0;
    }
    if (!lay->root) return;
    const IfBox *r = lay->root;
    /* 窓の上下端と box の y 範囲の一致は「隣接」であって交差ではない（半開区間）。
     * 端一致自体は正しい。root は全高を跨ぐので原則交差するが、壳の塗り（背景/罫線）
     * は「box が交差する窓」でのみ必要であり、交差しない窓では一切描かないのが
     * full-grid 経路との完全一致の条件（tests/test_layout の差分オラクルが機械固定） */
    if (!cur) { paint_box(out, r); return; }
    /* カーソル規約: 後退検知（窓が resume 時代より前に戻った）は自動で無効化。 */
    const IfBox *start = NULL;
    if (cur->resume) {
        const IfBox *rc = (const IfBox *)cur->resume;
        if (rc->y <= row0) start = rc; /* 単調前進のみ抱拥 */
    }
    cur->resume = NULL;
    if (r->kind == IF_BOX_LINE) { paint_box(out, r); return; } /* 防御: root が line は通常ない */
    /* root は常に交差する（全高）ため shell は必ず塗る。下部で gy 範囲外なら
     * paint_shell 側の fill_bg/draw が窓クリップして no-op になるので正しい */
    if (paint_shell(out, r)) {
        const IfBox *first = NULL;
        paint_children(out, r, start, &first);
        cur->resume = first;
    }
}

void if_render_grid_rows_into(const IfLayout *lay, i32 row0, i32 row1, IfGrid *out) {
    if_render_grid_rows_into_cur(lay, row0, row1, out, NULL);
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

void if_render_extent(const IfLayout *lay, i32 *mx, i32 *my) {
    i32 x = lay->width, y = lay->height;
    if (lay->root) grid_max_walk(lay->root, &x, &y);
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    *mx = x; *my = y;
}

void if_render_emit_rows(FILE *out, const IfGrid *grid, int ansi) {
    IfPen cur = { (u8)IF_CELL_DEFAULT, (u8)IF_CELL_DEFAULT, 0 };
    /* 1 行ぶんの作業バッファを一度だけ確保（出力全体は保持しない = 定数メモリ） */
    u64 rowcap = (u64)grid->w * 4 + 128;
    u8 *row = (u8 *)malloc(rowcap ? rowcap : 1);
    if (!row) if_fatal("render: oom row buffer");
    for (i32 y = 0; y < grid->h; y++) {
        u64 n = 0;
        i32 last = grid->w - 1;
        if (!ansi) {
            while (last >= 0 && grid->cells[(i64)y * grid->w + last].cp == ' ') last--;
        }
        for (i32 x = 0; x <= last; x++) {
            const IfCell *c = &grid->cells[(i64)y * grid->w + x];
            if (c->cp == 0) continue; /* 全角 2 セル目 */
            if (ansi) {
                IfPen p = { c->fg, c->bg, c->flags };
                if (p.fg != cur.fg || p.bg != cur.bg || p.flags != cur.flags) {
                    /* インライン展開: pen_apply 相当を直接バッファに（行バッファ徹底のため） */
                    const char *esc = "\x1b[0m";
                    u64 en = 4; if (rowcap - n < 64) { fwrite(row, 1, n, out); n = 0; }
                    memcpy(row + n, esc, en); n += en;
                    if (p.flags & IF_F_BOLD) { memcpy(row + n, "\x1b[1m", 4); n += 4; }
                    if (p.flags & IF_F_ITALIC) { memcpy(row + n, "\x1b[3m", 4); n += 4; }
                    if (p.flags & IF_F_ULINE) { memcpy(row + n, "\x1b[4m", 4); n += 4; }
                    if (p.flags & IF_F_STRIKE) { memcpy(row + n, "\x1b[9m", 4); n += 4; }
                    if (p.fg != IF_CELL_DEFAULT) n += (u64)snprintf((char *)row + n, 64, "\x1b[38;5;%um", p.fg);
                    if (p.bg != IF_CELL_DEFAULT) n += (u64)snprintf((char *)row + n, 64, "\x1b[48;5;%um", p.bg);
                    cur = p;
                }
            }
            u8 enc[4];
            u32 cp = c->cp ? c->cp : ' ';
            u64 en = if_utf8_encode(cp, enc);
            if (rowcap - n < en + 8) { fwrite(row, 1, n, out); n = 0; }
            memcpy(row + n, enc, en); n += en;
        }
        /* 行末リセット無条件（if_render_emit と同一契約。窓発行 ≡ 全体発行を構造保証） */
        if (ansi) { memcpy(row + n, "\x1b[0m", 4); n += 4; cur = (IfPen){ (u8)IF_CELL_DEFAULT, (u8)IF_CELL_DEFAULT, 0 }; }
        row[n++] = '\n';
        fwrite(row, 1, n, out);
    }
    free(row);
}

IfStr if_render_emit(IfArena *arena, const IfGrid *grid, int ansi) {
    IfOut o = { NULL, 0, 0, arena };
    IfPen cur = { (u8)IF_CELL_DEFAULT, (u8)IF_CELL_DEFAULT, 0 };

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
                }
            }
            u8 enc[4];
            u32 cp = c->cp ? c->cp : ' ';
            u64 n = if_utf8_encode(cp, enc);
            out_bytes(&o, enc, n);
        }
        /* 行末リセットは無条件（2026-08-01 契約統一）: 旧規約 pen_dirty=true で不変では
         * 「窓の先頭行」がストリーム履歴に依存して full 経路と乖離し得た（差分オラクルが
         * ws=7 の HR 行で同定）。無条件化で行は完全に自己完結し、窓発行 ≡ 全体発行が
         * 構造的に保証される。バイト代償は +4B/行（実測で出力比 <0.5%）。 */
        if (ansi) { out_str(&o, "\x1b[0m"); cur = (IfPen){ (u8)IF_CELL_DEFAULT, (u8)IF_CELL_DEFAULT, 0 }; }
        out_str(&o, "\n");
    }
    return if_str((const char *)o.buf, (u32)o.len);
}
