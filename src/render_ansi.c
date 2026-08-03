/* Ifuto — セルグリッドレンダラ実装（ソフトウェアラスタ） */
#include "render.h"
#include "utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h> /* 2-way 並列 sweep（glibc>=2.34 で ldd 不変） */

/* ---- レンダ rdtsc ゾーン計測（IF_RENDER_PROF=1。構造読み専用: rdtsc ペア税 ~35-70cy/回を含む） ---- */
#if defined(__x86_64__) || defined(__i386__)
static inline u64 rz_rdtsc(void) { u32 lo, hi; __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi)); return ((u64)hi << 32) | lo; }
#else
static inline u64 rz_rdtsc(void) { return 0; }
#endif
static int rz_on = -1;
static inline bool rz(void) {
    if (rz_on < 0) { const char *e = getenv("IF_RENDER_PROF"); rz_on = (e && e[0] == '1') ? 1 : 0; }
    return rz_on > 0;
}
static unsigned long long RZ_GAP, RZ_DIRECT, RZ_FAST, RZ_SLOW, RZ_BLANK, RZ_FLUSH;
static unsigned long long RN_GAP, RN_DIRECT, RN_FAST, RN_SLOW, RN_BLANK;
__attribute__((destructor)) static void rz_dump(void) {
    if (rz_on > 0)
        fprintf(stderr, "RENDERPROF gap=%llu(%llu) direct=%llu(%llu) fast=%llu(%llu) slow=%llu(%llu) blank=%llu(%llu) flush=%llu (cycles)\n",
                RZ_GAP, RN_GAP, RZ_DIRECT, RN_DIRECT, RZ_FAST, RN_FAST, RZ_SLOW, RN_SLOW, RZ_BLANK, RN_BLANK, RZ_FLUSH);
}

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

/* ================= 行スイープ直接発行（巨大文書のグリッドレス経路） =================
 * 窓グリッド経路は「全セル既定充填 + 描画 + 発行」で O(w×rows) のセルメモリを必ず踏む。
 * 本経路は paint op（行 seg / 装飾 deco）を「paint 順」のまま行カーソルで合流し、
 * 再利用の行バッファ（L1 定常）にその行の op だけを構成して発行する。
 * op 合流の paint 順序（証明）:
 *   - lines[] は wrap_end_line 生成順 = DFS 順 = y 単調非減少
 *   - deco[] は layout_element entry(bg→border) + 子 DFS の追記順 = y 開始が単調非減少
 *   - 同一行の op 列は「deco（追記順）→ lines（生成順）」で paint 順と一致する:
 *       * 兄弟 box の行は共有しない（純粋垂直フローで y が縮退しない）
 *       * 祖先の deco は子孫の line より先に追記される（entry 追記規則）
 *       * marker は li 降下前、hline は HR box 点で追記される
 * 発行バイト列は if_render_emit(_rows) の完全一致がオラクル（tests/test_layout.c）。
 */

typedef struct {
    u32 cp;     /* 0 = 全角継続セル */
    u8 fg, bg, flags;
} IfRCell;

/* 行バッファにテキストを paint 順で上書き（draw_text/put_cp と同値: フルペン上書き） */
static void row_paint_text(IfRCell *row, i32 w, i32 x, IfStr t, const IfStyle *st) {
    u8 fg = st ? if_rgba_to_ansi(st->color) : (u8)IF_CELL_DEFAULT;
    u8 bg = st ? if_rgba_to_ansi(st->bg) : (u8)IF_CELL_DEFAULT;
    u8 fl = 0;
    if (st) {
        if (st->bold) fl |= IF_F_BOLD;
        if (st->italic) fl |= IF_F_ITALIC;
        if (st->underline) fl |= IF_F_ULINE;
        if (st->strike) fl |= IF_F_STRIKE;
    }
    const u8 *s = (const u8 *)t.p;
    u32 i = 0;
    i32 cx = x;
    while (i < t.n) {
        u32 cp = if_utf8_decode(s, t.n, &i);
        int gw = if_glyph_width(cp);
        if (gw == 0) continue;
        if (cx >= 0 && cx < w) { row[cx].cp = cp; row[cx].fg = fg; row[cx].bg = bg; row[cx].flags = fl; }
        if (gw == 2 && cx + 1 >= 0 && cx + 1 < w) {
            row[cx + 1].cp = 0; row[cx + 1].fg = fg; row[cx + 1].bg = bg; row[cx + 1].flags = fl;
        }
        cx += gw;
    }
}

static void row_paint_cprun(IfRCell *row, i32 w, i32 x0, i32 x1, u32 cp, u8 fgA, bool keep_pen) {
    if (x0 < 0) x0 = 0;
    if (x1 > w) x1 = w;
    for (i32 x = x0; x < x1; x++) {
        row[x].cp = cp;
        row[x].fg = fgA;
        if (!keep_pen) { row[x].bg = (u8)IF_CELL_DEFAULT; row[x].flags = 0; }
    }
}

static void row_paint_dec(IfRCell *row, i32 w, const IfDeco *d, i32 r) {
    switch (d->kind) {
    case IF_DECO_BG: {
        u8 idx = if_rgba_to_ansi(d->argb);
        if (idx == IF_CELL_DEFAULT && (d->argb & 0xFF) < 128) break; /* fill_bg と同値の早期return */
        i32 x0 = d->x < 0 ? 0 : d->x, x1 = d->x + d->w > w ? w : d->x + d->w;
        for (i32 x = x0; x < x1; x++) row[x].bg = idx;
        break;
    }
    case IF_DECO_BORDER: {
        u8 fg = if_rgba_to_ansi(d->argb);
        u8 sides = d->tlen;
        i32 y0 = d->y, y1 = d->y + d->h - 1;
        if ((sides & 1) && r == y0) row_paint_cprun(row, w, d->x, d->x + d->w, 0x2500, fg, true);
        if ((sides & 4) && r == y1) row_paint_cprun(row, w, d->x, d->x + d->w, 0x2500, fg, true);
        if (sides & 8) row_paint_cprun(row, w, d->x, d->x + 1, 0x2502, fg, true);
        if (sides & 2) row_paint_cprun(row, w, d->x + d->w - 1, d->x + d->w, 0x2502, fg, true);
        if ((sides & 1) && (sides & 8) && r == y0 && d->x >= 0 && d->x < w) row[d->x].cp = 0x250C;
        if ((sides & 1) && (sides & 2) && r == y0 && d->x + d->w - 1 >= 0 && d->x + d->w - 1 < w) row[d->x + d->w - 1].cp = 0x2510;
        if ((sides & 4) && (sides & 8) && r == y1 && d->x >= 0 && d->x < w) row[d->x].cp = 0x2514;
        if ((sides & 4) && (sides & 2) && r == y1 && d->x + d->w - 1 >= 0 && d->x + d->w - 1 < w) row[d->x + d->w - 1].cp = 0x2518;
        break;
    }
    case IF_DECO_HLINE:
        row_paint_cprun(row, w, d->x, d->x + d->w, 0x2500, (u8)IF_CELL_DEFAULT, false);
        break;
    case IF_DECO_MARKER:
        row_paint_text(row, w, d->x, if_str(d->text, d->tlen), d->st);
        break;
    default: break;
    }
}

/* 行スイープ発行。発行規約は if_render_emit_rows と一致（trim・行末リセット） */
/* ---- バッチ書き出し（行ごと fwrite の消去。850k 行規模だと fwrite コール自体が律速） ---- */
typedef struct { u8 *p; u64 n, cap; FILE *out; u8 *mem; u64 mem_n, mem_cap; } IfBB;
static void bb_init(IfBB *b, FILE *out) {
    b->cap = 1u << 20;
    b->p = (u8 *)malloc(b->cap);
    if (!b->p) if_fatal("render: oom batch buffer");
    b->n = 0; b->out = out;
    b->mem = NULL; b->mem_n = 0; b->mem_cap = 0;
}
static void bb_flush(IfBB *b);
static void bb_ensure(IfBB *b, u64 k) {
    if (b->cap - b->n >= k) return;
    if (b->n) bb_flush(b); /* FILE / メモリシンク共通（旧: 直接 fwrite） */
    if (b->cap < k) {
        while (b->cap < k) b->cap *= 2;
        u8 *np = (u8 *)realloc(b->p, b->cap);
        if (!np) if_fatal("render: oom batch grow");
        b->p = np;
    }
}
static inline void bb_put(IfBB *b, const void *p, u64 k) {
    bb_ensure(b, k);
    memcpy(b->p + b->n, p, (size_t)k);
    b->n += k;
}
static inline void bb_ch(IfBB *b, u8 c) {
    bb_ensure(b, 1);
    b->p[b->n++] = c;
}
static void bb_flush(IfBB *b) {
    if (b->n) {
        u64 _t; if (rz()) _t = rz_rdtsc(); else _t = 0;
        if (b->out) {
            fwrite(b->p, 1, b->n, b->out);
        } else {
            /* メモリシンク（並列 sweep の後半スレッド用。join 後に主スレッドが一括 fwrite） */
            if (b->mem_cap - b->mem_n < b->n) {
                while (b->mem_cap - b->mem_n < b->n) b->mem_cap = b->mem_cap ? b->mem_cap * 2 : (1u << 20);
                u8 *nm = (u8 *)realloc(b->mem, b->mem_cap);
                if (!nm) if_fatal("render: oom batch memsink");
                b->mem = nm;
            }
            memcpy(b->mem + b->mem_n, b->p, b->n);
            b->mem_n += b->n;
        }
        if (_t) RZ_FLUSH += rz_rdtsc() - _t;
        b->n = 0;
    }
}

/* no-ansi byte-direct 行の 1 ラン（seg バイト or 装飾の ─ 連打） */
typedef struct { i32 x, w; const u8 *p; u32 n; u8 kind; } IfRRun;
enum { IF_RR_BYTES = 0, IF_RR_HLINE = 1 };

/* 行 r を byte-direct で発行試行。成功なら *li_io を 1 進めて true。
 * 失敗（seg 不整合・重複・未対応 deco）は何も出さず false（呼出側がセル経路へ）。
 * 正当性: LINE の IF_LF_DIRECT_BYTES は wrap 時に「全グリフ gw>0 かつ enc∘dec 恒等」を
 * 検査済み → seg バイトの連結 ≡ セル列の再エンコード列。trim は末尾 0x20 の除去で同値。 */
/* cp_free（active が BG のみ）行の seg 直行: row_emit_fast が runs 構築+挿入ソートで
 * 行う処理の nr=n_segs・deco 無し特殊形。segs は x 単調非減少（挿入ソートが no-op に
 * なる前提）であれば走査一回で発行できる。生成するバイト列は row_emit_fast の
 * gap/clip/trim 規則を「クリップ不発の圏内」で機械的に展開したものと同値:
 *  - 全 seg が x>=0 ∧ x+w<=mx（右端跨ぎ clip 判定が恒真で失敗しない）
 *  - x 非減少かつ非重複（runs[a].x < pos の堕落判定が不発）
 *  - clip 分岐（x>mx, pos>=mx, pos+w>mx）は上記条件で全て不発
 * 同値根拠は 1 行=1 セル列への写像が IF_LF_DIRECT_BYTES で byte 恒等であること
 * （旧単一 seg 直行と同じ空手形）。条件違反時は false で row_emit_fast へ委譲。 */
static bool row_emit_direct(IfBB *bb, const IfBox *b, i32 mx) {
    if (!(b->_pad[0] & IF_LF_DIRECT_BYTES)) return false;
    u64 mark = bb->n;
    i32 pos = 0;
    for (u32 u = 0; u < b->n_segs; u++) {
        i32 x = b->segs[u].x, w = b->segs[u].w;
        if (__builtin_expect(x < pos || x + w > mx, 0)) { bb->n = mark; return false; }
        if (x > pos) {
            bb_ensure(bb, (u64)(x - pos) + 8);
            memset(bb->p + bb->n, ' ', (size_t)(x - pos));
            bb->n += (u64)(x - pos);
        }
        bb_put(bb, b->segs[u].text.p, b->segs[u].text.n);
        pos = x + w;
    }
    while (bb->n > mark && bb->p[bb->n - 1] == ' ') bb->n--;
    bb_ch(bb, '\n');
    return true;
}

static bool row_emit_fast(IfBB *bb, const IfLayout *lay, u32 *li_io,
                          const IfDeco **active, u32 n_active, i32 mx, i32 r) {
    u32 li = *li_io;
    const IfBox *b = lay->lines[li];
    if (!(b->_pad[0] & IF_LF_DIRECT_BYTES)) return false;
    if (li + 1 < lay->n_lines && lay->lines[li + 1]->y == r) return false; /* 同行複数行は堕落 */

    /* ラン集合: segs（x 単調）+ marker/hline。BORDER は未対応、BG は no-ansi で無影響 */
    IfRRun runs[64];
    u32 nr = 0;
    for (u32 u = 0; u < b->n_segs; u++) {
        if (nr == 64) return false;
        runs[nr].x = b->segs[u].x;
        runs[nr].w = b->segs[u].w;
        runs[nr].p = (const u8 *)b->segs[u].text.p;
        runs[nr].n = b->segs[u].text.n;
        runs[nr].kind = IF_RR_BYTES;
        nr++;
    }
    for (u32 k = 0; k < n_active; k++) {
        const IfDeco *d = active[k];
        if (d->kind == IF_DECO_BG) continue;
        if (nr == 64) return false;
        if (d->kind == IF_DECO_MARKER) {
            runs[nr].x = d->x; runs[nr].w = d->w;
            runs[nr].p = (const u8 *)d->text; runs[nr].n = d->tlen;
            runs[nr].kind = IF_RR_BYTES; nr++;
        } else if (d->kind == IF_DECO_HLINE) {
            runs[nr].x = d->x; runs[nr].w = d->w;
            runs[nr].p = NULL; runs[nr].n = 0;
            runs[nr].kind = IF_RR_HLINE; nr++;
        } else {
            return false; /* BORDER 等はセル経路へ */
        }
    }
    /* x 昇順へ挿入ソート（segs は既に単調、deco は小数） */
    for (u32 a = 1; a < nr; a++) {
        IfRRun t = runs[a];
        u32 c = a;
        while (c > 0 && runs[c - 1].x > t.x) { runs[c] = runs[c - 1]; c--; }
        runs[c] = t;
    }

    u64 mark = bb->n;
    i32 pos = 0;
    for (u32 a = 0; a < nr; a++) {
        if (runs[a].x < pos) { bb->n = mark; return false; } /* 重複はセル経路へ */
        if (runs[a].x > pos) {
            bb_ensure(bb, (u64)(runs[a].x > mx ? mx : runs[a].x) - (u64)pos + 8);
            i32 gap = runs[a].x > mx ? mx - pos : runs[a].x - pos;
            if (gap < 0) { bb->n = mark; return false; }
            memset(bb->p + bb->n, ' ', (size_t)gap);
            bb->n += (u64)gap;
            pos = runs[a].x;
        }
        if (pos >= mx) break; /* viewport 右端でクリップ（セル版と同じ cx<w 判定の同値） */
        if (runs[a].kind == IF_RR_HLINE) {
            i32 w = runs[a].w;
            if (pos + w > mx) w = mx - pos;
            bb_ensure(bb, (u64)w * 3 + 8);
            for (i32 k = 0; k < w; k++) { memcpy(bb->p + bb->n, "\xE2\x94\x80", 3); bb->n += 3; }
            pos += runs[a].w;
        } else {
            /* クリップ: 右端を跨ぐ seg はセル単位で切り詰めなければならないため堕落 */
            if (pos + runs[a].w > mx) { bb->n = mark; return false; }
            bb_put(bb, runs[a].p, runs[a].n);
            pos += runs[a].w;
        }
    }
    /* 末尾空白 trim（byte 0x20 ＝ セル ' ' の同値変換） */
    while (bb->n > mark && bb->p[bb->n - 1] == ' ') bb->n--;
    bb_ch(bb, '\n');
    *li_io = li + 1;
    return true;
}

/* 行スイープ発行。発行規約は if_render_emit_rows と一致（trim・行末リセット） */
/* [r0,r1) の行範囲を発行する本体。li0 = 最初の y>=r0 の line 添字（下界探索で確定）。
 * 分割点での状態一意性（並列化の同値証明）:
 *  - di/active は r0 時点で deco 前置走査により構築できる: active = {d: d.y<=r0<d.y+dh}
 *    を deco[] 添字順（=追記順）で集めたもので、増分掃引が r0 到達時に持つ集合・順序と一致。
 *  - pen（ansi）は既定のままでは分割できないため、並列は no-ansi 限定（呼出側で保証）。
 *  - ギャップ充填の nl_y は r1 でクランプ（直列時 r1==my で不変。分割時は後半が自分の
 *    区間を同じ規則で発行するため、'\n' 総数・content 行は区間和で厳密一致）。 */
static void sweep_range(const IfLayout *lay, i32 mx, i32 r0, i32 r1, int ansi, u32 li0, IfBB *bb_ptr) {
    IfRCell *row = (IfRCell *)malloc((size_t)mx * sizeof(IfRCell));
    if (!row) if_fatal("render: oom row cells");
    u64 outcap = (u64)mx * 4 + 128;
    u8 *buf = (u8 *)malloc(outcap);
    if (!buf) if_fatal("render: oom row buffer");
    IfBB bb = *bb_ptr; /* 呼出側のバッファ状態を値で引き継ぎ、終端で書き戻す */
    IfPen cur = { (u8)IF_CELL_DEFAULT, (u8)IF_CELL_DEFAULT, 0 };

    /* deco の active 集合（追記順を保持したまま期限切れを惰性除去） */
    const IfDeco **active = (const IfDeco **)malloc(sizeof(IfDeco *) * 64);
    u32 n_active = 0, cap_active = 64;
    u32 di = 0, li = li0;

    /* r0 時点の di/active を前置走査で構築（r0==0 では空集合・di=0 で従来と同一） */
    while (di < lay->n_deco && lay->deco[di].y <= r0) {
        const IfDeco *d = &lay->deco[di++];
        i32 dh = d->h > 0 ? d->h : 1;
        if (r0 < d->y + dh) {
            if (n_active == cap_active) {
                cap_active *= 2;
                active = (const IfDeco **)realloc(active, sizeof(IfDeco *) * cap_active);
                if (!active) if_fatal("render: oom deco active");
            }
            active[n_active++] = d;
        }
    }

    for (i32 r = r0; r < r1; r++) {
        if (!ansi) {
            /* ギャップ一括充填: 次の LINE 到達までの区間 [r, nl_y) に
             * (a) active の非 BG deco、(b) 新たに開始する非 BG deco が無ければ、
             * 各行の発行は '\n' のみ（BG は no-ansi で不可視、cp_free/BG-only 規約と同値）。
             * di/active はここでは消費せず次行の正規経路に委ねるため状態は同期する。 */
            i32 nl_y = (li < lay->n_lines) ? lay->lines[li]->y : r1;
            if (nl_y > r1) nl_y = r1; /* 分割区間では後半が自分の区間を発行（直列時 r1==my で不変） */
            if (nl_y > r) {
                bool clean = true;
                for (u32 k = 0; k < n_active; k++)
                    if (active[k]->kind != IF_DECO_BG) { clean = false; break; }
                if (clean)
                    for (u32 d2 = di; d2 < lay->n_deco && lay->deco[d2].y < nl_y; d2++)
                        if (lay->deco[d2].kind != IF_DECO_BG) { clean = false; break; }
                if (clean) {
                    u64 _t; if (rz()) _t = rz_rdtsc(); else _t = 0;
                    u64 gap = (u64)(nl_y - r);
                    bb_ensure(&bb, gap);
                    memset(bb.p + bb.n, '\n', (size_t)gap);
                    bb.n += gap;
                    r = nl_y - 1;
                    if (_t) { RZ_GAP += rz_rdtsc() - _t; RN_GAP++; }
                    continue;
                }
            }
        }
        /* deco の開始（y==r）は追記順に active へ */
        while (di < lay->n_deco && lay->deco[di].y <= r) {
            const IfDeco *d = &lay->deco[di++];
            i32 dh = d->h > 0 ? d->h : 1;
            if (r < d->y + dh) {
                if (n_active == cap_active) {
                    cap_active *= 2;
                    active = (const IfDeco **)realloc(active, sizeof(IfDeco *) * cap_active);
                    if (!active) if_fatal("render: oom deco active");
                }
                active[n_active++] = d;
            }
        }
        /* 期限切れ除去（順序保持の compact） */
        for (u32 k = 0; k < n_active; ) {
            const IfDeco *d = active[k];
            i32 dh = d->h > 0 ? d->h : 1;
            if (!(d->y <= r && r < d->y + dh)) {
                memmove(active + k, active + k + 1, (n_active - 1 - k) * sizeof(IfDeco *));
                n_active--;
                continue;
            }
            k++;
        }
        bool has_line = (li < lay->n_lines && lay->lines[li]->y == r);
        if (!has_line && n_active == 0) {
            u64 _t; if (rz()) _t = rz_rdtsc(); else _t = 0;
            /* ブランク行（発行規約どおり） */
            if (ansi) {
                bb_ensure(&bb, (u64)mx + 8);
                memset(bb.p + bb.n, ' ', (size_t)mx); bb.n += (u64)mx;
                memcpy(bb.p + bb.n, "\x1b[0m", 4); bb.n += 4;
                cur = (IfPen){ (u8)IF_CELL_DEFAULT, (u8)IF_CELL_DEFAULT, 0 };
            }
            bb_ch(&bb, '\n');
            if (_t) { RZ_BLANK += rz_rdtsc() - _t; RN_BLANK++; }
            continue;
        }
        /* no-ansi かつ byte-direct 条件を満たす行はセルモデルを経由しない */
        if (!ansi) {
            bool cp_free = true, fastable = true;
            for (u32 k = 0; k < n_active; k++) {
                u8 dk = active[k]->kind;
                if (dk != IF_DECO_BG) cp_free = false;
                if (dk != IF_DECO_BG && dk != IF_DECO_MARKER && dk != IF_DECO_HLINE)
                    fastable = false;
            }
            if (!has_line && cp_free) {
                /* BG のみの行は no-ansi では見えない（fill_bg は cp を触らない） */
                bb_ch(&bb, '\n');
                continue;
            }
            if (has_line && fastable) {
                /* seg 直行（cp_free・単一行・クリップ不発の圏内）: row_emit_fast の
                 * runs 構築/ソートを経ない一回走査版。発行バイト列は同値（関数頭参照） */
                const IfBox *b0 = lay->lines[li];
                if (cp_free &&
                    !(li + 1 < lay->n_lines && lay->lines[li + 1]->y == r)) {
                    u64 _t; if (rz()) _t = rz_rdtsc(); else _t = 0;
                    if (row_emit_direct(&bb, b0, mx)) {
                        li++;
                        if (_t) { RZ_DIRECT += rz_rdtsc() - _t; RN_DIRECT++; }
                        continue;
                    }
                    if (_t) RZ_SLOW += rz_rdtsc() - _t;
                }
                u64 _t; if (rz()) _t = rz_rdtsc(); else _t = 0;
                if (row_emit_fast(&bb, lay, &li, active, n_active, mx, r)) {
                    if (_t) { RZ_FAST += rz_rdtsc() - _t; RN_FAST++; }
                    continue;
                }
                if (_t) RZ_SLOW += rz_rdtsc() - _t; /* row_emit_fast 失敗分は slow 側に寄せる */
            }
        }
        /* 行構成: [0,maxx] だけ既定充填（ansi は全幅必要） */
        u64 _ts; if (rz()) _ts = rz_rdtsc(); else _ts = 0;
        RN_SLOW++;
        i32 maxx = mx;
        if (!ansi) {
            maxx = 0;
            if (has_line) {
                const IfBox *b = lay->lines[li];
                for (u32 u = 0; u < b->n_segs; u++)
                    if (b->segs[u].x + b->segs[u].w > maxx) maxx = b->segs[u].x + b->segs[u].w;
                for (u32 u = li + 1; u < lay->n_lines && lay->lines[u]->y == r; u++) {
                    const IfBox *b2 = lay->lines[u];
                    for (u32 v = 0; v < b2->n_segs; v++)
                        if (b2->segs[v].x + b2->segs[v].w > maxx) maxx = b2->segs[v].x + b2->segs[v].w;
                }
            }
            for (u32 k = 0; k < n_active; k++) {
                const IfDeco *d = active[k];
                if (d->kind == IF_DECO_BG) continue; /* bg は no-ansi 出力に影響しない */
                if (d->x + d->w > maxx) maxx = d->x + d->w;
            }
            if (maxx > mx) maxx = mx;
        }
        for (i32 x = 0; x < maxx; x++) {
            row[x].cp = ' ';
            row[x].fg = (u8)IF_CELL_DEFAULT;
            row[x].bg = (u8)IF_CELL_DEFAULT;
            row[x].flags = 0;
        }
        /* paint: deco（追記順）→ lines */
        for (u32 k = 0; k < n_active; k++)
            row_paint_dec(row, maxx, active[k], r);
        while (li < lay->n_lines && lay->lines[li]->y == r) {
            const IfBox *b = lay->lines[li];
            for (u32 u = 0; u < b->n_segs; u++)
                row_paint_text(row, maxx, b->segs[u].x, b->segs[u].text, b->segs[u].st);
            li++;
        }
        /* 発行（emit_rows 同値） */
        u64 n = 0;
        i32 last = maxx - 1;
        if (!ansi) {
            while (last >= 0 && row[last].cp == ' ') last--;
        }
        for (i32 x = 0; x <= last; x++) {
            if (row[x].cp == 0) continue;
            if (ansi) {
                IfPen p = { row[x].fg, row[x].bg, row[x].flags };
                if (p.fg != cur.fg || p.bg != cur.bg || p.flags != cur.flags) {
                    if (outcap - n < 64) { bb_put(&bb, buf, n); n = 0; }
                    memcpy(buf + n, "\x1b[0m", 4); n += 4;
                    if (p.flags & IF_F_BOLD) { memcpy(buf + n, "\x1b[1m", 4); n += 4; }
                    if (p.flags & IF_F_ITALIC) { memcpy(buf + n, "\x1b[3m", 4); n += 4; }
                    if (p.flags & IF_F_ULINE) { memcpy(buf + n, "\x1b[4m", 4); n += 4; }
                    if (p.flags & IF_F_STRIKE) { memcpy(buf + n, "\x1b[9m", 4); n += 4; }
                    if (p.fg != IF_CELL_DEFAULT) n += (u64)snprintf((char *)buf + n, 64, "\x1b[38;5;%um", p.fg);
                    if (p.bg != IF_CELL_DEFAULT) n += (u64)snprintf((char *)buf + n, 64, "\x1b[48;5;%um", p.bg);
                    cur = p;
                }
            }
            u8 enc[4];
            u64 en = if_utf8_encode(row[x].cp, enc);
            if (outcap - n < en + 8) { bb_put(&bb, buf, n); n = 0; }
            memcpy(buf + n, enc, en); n += en;
        }
        if (ansi) { memcpy(buf + n, "\x1b[0m", 4); n += 4; cur = (IfPen){ (u8)IF_CELL_DEFAULT, (u8)IF_CELL_DEFAULT, 0 }; }
        buf[n++] = '\n';
        bb_put(&bb, buf, n);
        if (_ts) RZ_SLOW += rz_rdtsc() - _ts;
    }
    bb_flush(&bb);
    *bb_ptr = bb; /* バッファ状態（mem シンク含む）を呼出側へ書き戻す */
    free(bb.p);
    free(active);
    free(buf);
    free(row);
}

/* ---- 2-way 並列 sweep（no-ansi 限定） ----
 * 後半区間をワーカ（メモリシンク）へ、前半を主スレッド（FILE 直書き）で同時進行し、
 * join 後に B バッファを fwrite。バイト列は行 0..my の連結で直列 sweep と厳密一致
 * （区間境界の状態一意性は sweep_range 頭書参照。kill switch: IF_RENDER_PAR=0） */
typedef struct {
    const IfLayout *lay;
    i32 mx, r0, r1;
    u32 li0;
    IfBB *bb;
} IfSweepArg;

static void *sweep_worker(void *vp) {
    const IfSweepArg *a = (const IfSweepArg *)vp;
    sweep_range(a->lay, a->mx, a->r0, a->r1, 0, a->li0, a->bb);
    return NULL;
}

void if_render_emit_rows_sweep(FILE *out, const IfLayout *lay, int ansi) {
    /* 幅は lay->width（旧 sweep 規約）。行数は lay->height と同値:
     * 全 box/line は root の [y, y+h) に包含され lay->height = root.y+root.h であるため
     * grid_max_walk の my は常に lay->height に一致する（包含の空手形。出力 sha256 が機械監査） */
    i32 mx = lay->width;
    if (mx < 1) mx = 1;
    i32 my = lay->height;
    if (my < 1) my = 1;

    int par = 1;
    { const char *e = getenv("IF_RENDER_PAR"); if (e && e[0] == '0') par = 0; }
    if (ansi || !par || my < 1024 || lay->n_lines < 256 || !lay->lines) {
        IfBB bb; bb_init(&bb, out);
        sweep_range(lay, mx, 0, my, ansi, 0, &bb);
        return;
    }
    /* 内容行数で二等分（split 行の同一 y は全て前半に含める: lower_bound 規則） */
    u32 mid = lay->n_lines / 2;
    i32 r_split = lay->lines[mid]->y;
    if (r_split <= 0 || r_split >= my) {
        IfBB bb; bb_init(&bb, out);
        sweep_range(lay, mx, 0, my, ansi, 0, &bb);
        return;
    }
    u32 li_b = mid;
    while (li_b > 0 && lay->lines[li_b - 1]->y >= r_split) li_b--;

    IfBB bbB; bb_init(&bbB, NULL); /* メモリシンク */
    IfSweepArg argB = { lay, mx, r_split, my, li_b, &bbB };
    pthread_t th;
    int spawned = (pthread_create(&th, NULL, sweep_worker, &argB) == 0);
    {
        IfBB bbA; bb_init(&bbA, out);
        sweep_range(lay, mx, 0, r_split, 0, 0, &bbA);
    }
    if (spawned) pthread_join(th, NULL);
    else sweep_worker(&argB); /* pthread 生成失敗時は同分割を直列で（同値） */
    if (bbB.mem_n) fwrite(bbB.mem, 1, bbB.mem_n, out);
    free(bbB.mem);
    /* 注意: bbB.p は sweep_range 内部で free 済み（書き戻された p は dangling） */
}
