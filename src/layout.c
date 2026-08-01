/* Ifuto — レイアウト実装。
 * アルゴリズム: トップダウン DFS。幅は包含ブロックから確定、高さは子を敷いてから確定。
 *   block: ネストした要素を縦に積む（兄弟マージンは max 相殺）。
 *   inline: IFC を flatten → アトム化（単語ラン / 全角 1 グリフ）→ 貪欲折り返し。
 * 計算量: O(ノード数 + グリフ数)。兄弟走査はカーソル前進で O(N)（先頭からの辿り直し禁止）。
 */
#include "layout.h"
#include "utf8.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct IfPiece {
    IfStr text;
    const IfStyle *st;
    u8 br;   /* 1 = 強制改行 */
} IfPiece;

typedef struct {
    IfArena *arena;
    float root_fs;
    IfLink *links;
    u32 n_links;
    u64 links_cap;
    /* IFC/折り返しスクラッチ: pieces/segs 配列は「行・run 内で消費が完結」するので再利用する。
     * （arena は free しない。使い回せる一時配列を毎回確定させると大文書で数十 MB 無駄になる——実測済み） */
    IfPiece *pieces_scratch;
    u64 pieces_scratch_cap;
    IfSeg *segs_scratch;
    u64 segs_scratch_cap;
    /* box 構築時 tail（IfBox から last_child フィールドを追放した代替。子追加は
     * 親 box 構築中に集中かつ再帰スタック規律なので、frame に持って O(1) を保つ。
     * frame 深さ超過/不一致時は兄弟走査の正しいフォールバックに落ちる（性能のみ） */
    IfBox *frame_box[512];
    IfBox *frame_tail[512];
    int frame_n;
} IfLC;

static i32 px2col(float px) { return (i32)floorf(px / IF_CHAR_W_PX + 0.5f); }
static i32 px2row(float px) { return (i32)floorf(px / IF_ROW_H_PX + 0.5f); }

static i32 len_h(IfLen l, float self_fs, float root_fs, i32 basis) {
    if (l.unit == IF_U_PCT) return (i32)((float)basis * l.v / 100.0f);
    if (l.unit == IF_U_AUTO) return 0;
    return px2col(if_css_resolve_len(l, self_fs, root_fs));
}

static i32 len_v(IfLen l, float self_fs, float root_fs, i32 basis_w) {
    /* 縦方向の % も包含ブロックの「幅」基準（CSS 仕様） */
    if (l.unit == IF_U_PCT) return px2row((float)basis_w * IF_CHAR_W_PX * l.v / 100.0f);
    if (l.unit == IF_U_AUTO) return 0;
    return px2row(if_css_resolve_len(l, self_fs, root_fs));
}

static void box_add_child(IfLC *lc, IfBox *parent, IfBox *child) {
    child->next_sibling = NULL;
    IfBox *tail = NULL;
    bool use_frame = lc && lc->frame_n > 0 && lc->frame_box[lc->frame_n - 1] == parent;
    if (use_frame) tail = lc->frame_tail[lc->frame_n - 1];
    else { /* フォールバック（設計上到達しないはずの防御経路。正しさ優先） */
        for (tail = parent->first_child; tail && tail->next_sibling; tail = tail->next_sibling) {}
    }
    if (tail) tail->next_sibling = child;
    else parent->first_child = child;
    if (use_frame) lc->frame_tail[lc->frame_n - 1] = child;
}

/* IfBox 生成直後（子の構築を始める前）に frame を積み、構築完了で降ろす */
static void frame_push(IfLC *lc, IfBox *box) {
    if (lc->frame_n < 512) { lc->frame_box[lc->frame_n] = box; lc->frame_tail[lc->frame_n] = NULL; }
    lc->frame_n++;
}
static void frame_pop(IfLC *lc, IfBox *box) {
    lc->frame_n--;
    (void)box; /* 対称性のみ。depth 超過分は push で積まれないので pop も対称 */
    if (lc->frame_n < 0) lc->frame_n = 0;
}

static IfBox *new_box(IfLC *lc, u8 kind, IfNode *node, const IfStyle *st) {
    IfBox *b = (IfBox *)if_arena_calloc(lc->arena, sizeof(IfBox));
    b->kind = kind;
    b->node = node;
    b->st = st;
    return b;
}

/* ---------- インラインフラット化 ---------- */

typedef struct {
    IfPiece *pieces;   /* == lc->pieces_scratch（共有スクラッチ） */
    u32 n_pieces;
    IfLC *lc;
} IfFlat;

static void flat_push(IfFlat *f, IfStr text, const IfStyle *st, u8 br) {
    f->pieces = (IfPiece *)if_arena_grow(f->lc->arena, f->pieces, &f->lc->pieces_scratch_cap,
                                         f->n_pieces + 1, sizeof(IfPiece));
    /* if_arena_grow のコピー時に f->pieces も cap も lc 側に一元反映される */
    f->pieces[f->n_pieces].text = text;
    f->pieces[f->n_pieces].st = st;
    f->pieces[f->n_pieces].br = br;
    f->n_pieces++;
}

static void collect_link(IfLC *lc, IfNode *a) {
    IfStr href = if_dom_attr(a, "href");
    if (!href.p || href.n == 0) return;
    lc->links = (IfLink *)if_arena_grow(lc->arena, lc->links, &lc->links_cap, lc->n_links + 1, sizeof(IfLink));
    lc->links[lc->n_links].n = lc->n_links + 1;
    lc->links[lc->n_links].href = href;
    lc->n_links++;
}

static void flatten_into(IfFlat *f, IfNode *n, const IfStyle *st) {
    if (n->kind == IF_NODE_TEXT) { flat_push(f, n->u.text, st, 0); return; }
    if (n->kind != IF_NODE_ELEMENT) return;
    const IfStyle *est = n->style ? n->style : st;
    if (n->style && n->style->display == IF_D_NONE) return;
    switch (n->tag) {
    case IF_TAG_BR: flat_push(f, if_str(NULL, 0), est, 1); return;
    case IF_TAG_IMG: {
        IfStr alt = if_dom_attr(n, "alt");
        char buf[1024];
        int m = snprintf(buf, sizeof buf, "[img: %.*s]", (int)(alt.n > 900 ? 900 : alt.n), alt.p ? alt.p : "");
        if (m < 0) m = 0;
        char *s = (char *)if_arena_alloc(f->lc->arena, (u64)m);
        memcpy(s, buf, (u64)m);
        flat_push(f, if_str(s, (u32)m), est, 0);
        return;
    }
    case IF_TAG_A: collect_link(f->lc, n); break;
    default: break;
    }
    for (IfNode *c = n->first_child; c; c = c->next_sibling)
        flatten_into(f, c, est);
}

/* ---------- 折り返し ---------- */

typedef struct {
    IfLC *lc;
    i32 content_x, content_w;
    i32 y;
    IfBox *parent;
    IfSeg *segs;           /* == lc->segs_scratch（共有スクラッチ、n_segs は現在行の分） */
    u32 n_segs;
    i32 line_w;
    const IfStyle *align_st;
} IfWrap;

static void wrap_push_seg(IfWrap *w, IfStr text, i32 x, i32 width, const IfStyle *st) {
    if (text.n == 0) return;
    w->segs = (IfSeg *)if_arena_grow(w->lc->arena, w->segs, &w->lc->segs_scratch_cap,
                                     w->n_segs + 1, sizeof(IfSeg));
    IfSeg *s = &w->segs[w->n_segs++];
    s->text = text; s->x = x; s->w = width; s->st = st;
}

static void wrap_end_line(IfWrap *w, float max_lh) {
    i32 rows = px2row(max_lh);
    if (rows < 1) rows = 1;
    u8 align = w->align_st ? w->align_st->text_align : IF_TA_LEFT;
    i32 shift = 0;
    if (align == IF_TA_CENTER && w->line_w < w->content_w) shift = (w->content_w - w->line_w) / 2;
    else if (align == IF_TA_RIGHT && w->line_w < w->content_w) shift = w->content_w - w->line_w;

    IfBox *line = new_box(w->lc, IF_BOX_LINE, NULL, w->align_st);
    line->x = w->content_x;
    line->y = w->y;
    line->w = w->line_w;
    line->h = rows;
    line->text_align = align;
    /* スクラッチから行ボックスに厳密サイズでコピー確定する。
     * （指数成長のコピー浪費を行ごとに払うと大文書で数十 MB になる——実測済み） */
    if (w->n_segs) {
        IfSeg *dst = (IfSeg *)if_arena_alloc(w->lc->arena, (u64)w->n_segs * sizeof(IfSeg));
        memcpy(dst, w->segs, (u64)w->n_segs * sizeof(IfSeg));
        line->segs = dst;
        line->n_segs = w->n_segs;
        for (u32 i = 0; i < line->n_segs; i++) dst[i].x += shift;
    }
    box_add_child(w->lc, w->parent, line);
    w->y += rows;
    w->n_segs = 0;
    w->line_w = 0;
    w->lc->segs_scratch = w->segs; /* スクラッチ保持を lc に同期 */
}

/* テキスト 1 ピースを流し込む。*any は実グリフが存在したか（空白のみの run で空行を防ぐ）。 */
static void wrap_text(IfWrap *w, IfStr text, const IfStyle *st, bool pre,
                      float *max_lh, bool *any) {
    float fs = st ? st->font_size : 16.0f;
    float lh = st && st->line_height > 0.0f ? st->line_height : fs * 1.2f;
    if (lh > *max_lh) *max_lh = lh;

    const u8 *s = (const u8 *)text.p;
    u32 n = text.n;
    u32 i = 0;
    i32 cx = w->line_w;

    while (i < n) {
        u8 b0 = s[i];
        /* 空白処理 */
        if (b0 == ' ' || b0 == '\t' || b0 == '\n' || b0 == '\r' || b0 == '\f') {
            if (pre && b0 == '\n') { wrap_end_line(w, *max_lh); cx = 0; i++; *max_lh = lh; continue; }
            if (pre) {
                i32 adv = (b0 == '\t') ? 8 : 1;
                wrap_push_seg(w, if_str((const char *)s + i, 1), w->content_x + cx, adv, st);
                cx += adv; i++; *any = true; continue;
            }
            while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\f')) i++;
            if (cx > 0 && cx < w->content_w) {
                wrap_push_seg(w, IF_S(" "), w->content_x + cx, 1, st);
                cx += 1;
            }
            continue;
        }

        /* アトム切り出し: 全角は 1 グリフ、それ以外は空白・全角までのラン。
         * 不変条件: このブロックは必ず i を 1 グリフ以上前進させる。 */
        u32 atom_start = i;
        i32 atom_w = 0;
        {
            u32 save = i;
            u32 cp = if_utf8_decode(s, n, &save);
            int gw = if_glyph_width(cp);
            i = save; /* 1 グリフ消費（保証） */
            if (gw == 2) {
                atom_w = 2;
            } else {
                atom_w = (gw == 0) ? 0 : 1;
                while (i < n) {
                    u32 s2 = i;
                    u8 c = s[i];
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') break;
                    u32 cp2 = if_utf8_decode(s, n, &s2);
                    int gw2 = if_glyph_width(cp2);
                    if (gw2 == 2) break;
                    i = s2;
                    if (gw2 != 0) atom_w += 1;
                }
                if (atom_w == 0) atom_w = 1; /* 結合/制御のみでも前進を保証 */
            }
        }
        u32 atom_end = i;
        *any = true;

        /* 折り返し判定（pre 以外） */
        if (!pre && cx > 0 && cx + atom_w > w->content_w) {
            if (w->n_segs > 0 && if_str_eq(w->segs[w->n_segs - 1].text, IF_S(" "))) {
                w->n_segs--;
                cx = w->n_segs > 0 ? w->segs[w->n_segs - 1].x + w->segs[w->n_segs - 1].w - w->content_x : 0;
            }
            wrap_end_line(w, *max_lh);
            cx = 0;
            *max_lh = lh;
        }

        /* アトム自体が行幅超過 → グリフ単位ハード分割 */
        if (!pre && atom_w > w->content_w) {
            u32 g = atom_start;
            while (g < atom_end) {
                u32 gs = g;
                u32 cp3 = if_utf8_decode(s, atom_end, &g); /* g は必ず前進 */
                int gw3 = if_glyph_width(cp3);
                i32 gwidth = gw3 == 2 ? 2 : 1;
                if (cx > 0 && cx + gwidth > w->content_w) {
                    wrap_end_line(w, *max_lh);
                    cx = 0;
                    *max_lh = lh;
                }
                wrap_push_seg(w, if_str((const char *)s + gs, g - gs), w->content_x + cx, gwidth, st);
                cx += gwidth;
            }
            continue;
        }

        wrap_push_seg(w, if_str((const char *)s + atom_start, atom_end - atom_start), w->content_x + cx, atom_w, st);
        cx += atom_w;
    }
    w->line_w = cx;
}

/* インライン run の先頭ノードから連続するインラインレベルを IFC に流し込み、
 * run の次のノード（ブロック or NULL）を返す。 */
static IfNode *layout_ifc(IfLC *lc, IfBox *parent, IfNode *cur, const IfStyle *base_st,
                          i32 content_x, i32 *y_io, i32 content_w) {
    IfFlat f = { lc->pieces_scratch, 0, lc }; /* スクラッチ再利用。run 内で消費が完結 */
    IfNode *c = cur;
    for (; c; c = c->next_sibling) {
        if (c->kind == IF_NODE_ELEMENT && c->style) {
            if (c->style->display == IF_D_NONE) continue;      /* flow から除去 */
            if (c->style->display != IF_D_INLINE) break;        /* ブロック子: run 終了 */
        }
        flatten_into(&f, c, base_st);
    }

    IfWrap w = { lc, content_x, content_w, *y_io, parent, lc->segs_scratch, 0, 0, base_st };
    float max_lh = base_st && base_st->line_height > 0.0f ? base_st->line_height
                 : base_st ? base_st->font_size * 1.2f : 16.0f * 1.2f;
    bool any_text = false;
    bool pre = base_st && base_st->white_space == IF_WS_PRE;
    for (u32 p = 0; p < f.n_pieces; p++) {
        if (f.pieces[p].br) {
            wrap_end_line(&w, max_lh);
            max_lh = base_st && base_st->line_height > 0.0f ? base_st->line_height
                   : base_st ? base_st->font_size * 1.2f : 19.2f;
            continue;
        }
        wrap_text(&w, f.pieces[p].text, f.pieces[p].st ? f.pieces[p].st : base_st,
                  (f.pieces[p].st && f.pieces[p].st->white_space == IF_WS_PRE) || pre,
                  &max_lh, &any_text);
    }
    if (w.n_segs > 0 || any_text)
        wrap_end_line(&w, max_lh);
    *y_io = w.y;
    lc->pieces_scratch = f.pieces; /* スクラッチを後続 IFC に引き継ぐ（n は毎回 0 にリセット） */
    return c;
}

static IfBox *layout_element(IfLC *lc, IfNode *node, i32 ax, i32 ay, i32 avail_w);

/* node の子を走査して配置し、content 高を返す（カーソル前進で O(N)） */
static i32 layout_children(IfLC *lc, IfBox *box, IfNode *node,
                           i32 content_x, i32 content_y, i32 content_w) {
    i32 y = content_y;
    i32 prev_mb = 0;
    const IfStyle *base_st = node->style;
    IfNode *c = node->first_child;
    while (c) {
        if (c->kind == IF_NODE_ELEMENT && c->style && c->style->display == IF_D_NONE) {
            c = c->next_sibling;
            continue;
        }
        bool blockish = false;
        if (c->kind == IF_NODE_ELEMENT && c->style)
            blockish = c->style->display == IF_D_BLOCK || c->style->display == IF_D_LIST_ITEM;
        if (!blockish) {
            c = layout_ifc(lc, box, c, base_st, content_x, &y, content_w);
            prev_mb = 0;
            continue;
        }
        const IfStyle *cst = c->style;
        i32 mt = len_v(cst->margin[0], cst->font_size, lc->root_fs, content_w);
        i32 mb = len_v(cst->margin[2], cst->font_size, lc->root_fs, content_w);
        y += (prev_mb > mt ? prev_mb : mt); /* 兄弟縦マージン相殺: max */
        IfBox *child = layout_element(lc, c, content_x, y, content_w);
        box_add_child(lc, box, child);
        y += child->h;
        prev_mb = mb;
        c = c->next_sibling;
    }
    return y - content_y;
}

/* スタイル未適用（--no-style 等）でも落ちないための既定値。
 * レイアウトは style NULL を受理する（防御: 上位の都合を下位に押し付けない）。
 * 位置指定初期化は構造体変更で壊れるので指示初期化子のみ使う。 */
static const IfStyle IF_STYLE_FALLBACK = {
    .color = 0x000000FFu,
    .font_size = 16.0f,
    .width = { 0.0f, IF_U_AUTO },
    .height = { 0.0f, IF_U_AUTO },
    .display = IF_D_BLOCK,
    .text_align = IF_TA_LEFT,
    .white_space = IF_WS_NORMAL,
};

static IfBox *layout_element(IfLC *lc, IfNode *node, i32 ax, i32 ay, i32 avail_w) {
    const IfStyle *st = node->style ? node->style : &IF_STYLE_FALLBACK;
    IfBox *box = new_box(lc, IF_BOX_BLOCK, node, st);
    float fs = st->font_size;

    i32 bl  = st->border_w[3] > 0.0f ? 1 : 0;
    i32 brd = st->border_w[1] > 0.0f ? 1 : 0;
    i32 bt  = st->border_w[0] > 0.0f ? 1 : 0;
    i32 bbo = st->border_w[2] > 0.0f ? 1 : 0;
    i32 ml = len_h(st->margin[3], fs, lc->root_fs, avail_w);
    i32 mr = len_h(st->margin[1], fs, lc->root_fs, avail_w);
    i32 pad_l = len_h(st->padding[3], fs, lc->root_fs, avail_w);
    i32 pad_r = len_h(st->padding[1], fs, lc->root_fs, avail_w);
    i32 pad_t = len_v(st->padding[0], fs, lc->root_fs, avail_w);
    i32 pad_b = len_v(st->padding[2], fs, lc->root_fs, avail_w);

    i32 content_w;
    if (st->width.unit != IF_U_AUTO) {
        content_w = len_h(st->width, fs, lc->root_fs, avail_w);
        if (content_w < 0) content_w = 0;
        i32 total = ml + bl + pad_l + content_w + pad_r + brd + mr;
        if (total < avail_w && st->margin[3].unit == IF_U_AUTO && st->margin[1].unit == IF_U_AUTO)
            ml = mr = (avail_w - total) / 2; /* margin:auto センタリング */
    } else {
        content_w = avail_w - ml - mr - bl - brd - pad_l - pad_r;
        if (content_w < 0) content_w = 0;
    }

    i32 x = ax + ml;
    i32 content_x = x + bl + pad_l;
    i32 y = ay; /* margin-top は呼び出し側（兄弟相殺）で処理済み */
    i32 content_y = y + bt + pad_t;

    if (node->tag == IF_TAG_HR) {
        box->x = x; box->y = y;
        box->w = bl + pad_l + content_w + pad_r + brd;
        box->h = bt + 1 + bbo;
        return box;
    }

    frame_push(lc, box); /* box の子追加は frame_top==box で O(1) tail を引く */
    i32 content_h = layout_children(lc, box, node, content_x, content_y, content_w);
    frame_pop(lc, box);
    if (st->height.unit != IF_U_AUTO) {
        i32 spec = len_v(st->height, fs, lc->root_fs, avail_w);
        if (spec > content_h) content_h = spec; /* 指定高はクリップせず拡張のみ（v0.1 近似） */
    }

    box->x = x;
    box->y = y;
    box->w = bl + pad_l + content_w + pad_r + brd;
    box->h = bt + pad_t + content_h + pad_b + bbo;
    return box;
}

IfLayout *if_layout_build(IfArena *arena, IfDom *dom, i32 width_cells) {
    if (width_cells < 4) width_cells = 4;
    IfLC lc = { .arena = arena, .root_fs = 16.0f };
    IfLayout *lay = (IfLayout *)if_arena_calloc(arena, sizeof(IfLayout));
    lay->arena = arena;
    lay->width = width_cells;
    lay->root = new_box(&lc, IF_BOX_BLOCK, NULL, NULL);

    IfNode *body = NULL;
    for (IfNode *c = dom->root->first_child; c && !body; c = c->next_sibling)
        if (c->kind == IF_NODE_ELEMENT && c->tag == IF_TAG_HTML)
            for (IfNode *g = c->first_child; g; g = g->next_sibling)
                if (g->kind == IF_NODE_ELEMENT && g->tag == IF_TAG_BODY) { body = g; break; }
    if (!body) return lay;

    float body_fs = body->style ? body->style->font_size : 16.0f;
    i32 body_mt = body->style ? len_v(body->style->margin[0], body_fs, 16.0f, width_cells) : 0;
    IfBox *root = layout_element(&lc, body, 0, body_mt, width_cells);
    lay->root = root;
    lay->height = root->y + root->h;
    lay->links = lc.links;
    lay->n_links = lc.n_links;
    return lay;
}

/* ---------- デバッグダンプ ---------- */

static void dump_box(const IfBox *b, FILE *out, int depth) {
    for (int i = 0; i < depth; i++) fputs("  ", out);
    if (b->kind == IF_BOX_LINE) {
        fprintf(out, "LINE x=%d y=%d w=%d h=%d segs=%u \"", b->x, b->y, b->w, b->h, b->n_segs);
        for (u32 i = 0; i < b->n_segs; i++) {
            IfStr t = b->segs[i].text;
            for (u32 k = 0; k < t.n && k < 60; k++) {
                char c = t.p[k];
                if (c == '\n') fputs("\\n", out);
                else fputc(c, out);
            }
        }
        fputs("\"\n", out);
        return;
    }
    fprintf(out, "BLOCK x=%d y=%d w=%d h=%d", b->x, b->y, b->w, b->h);
    if (b->node) fprintf(out, " <%s>", b->node->u.tag_name.p ? b->node->u.tag_name.p : "?");
    fputc('\n', out);
    for (const IfBox *c = b->first_child; c; c = c->next_sibling) dump_box(c, out, depth + 1);
}

void if_layout_dump(const IfLayout *lay, void *out_FILE) {
    FILE *out = (FILE *)out_FILE;
    if (!lay || !lay->root) { fputs("(empty layout)\n", out); return; }
    dump_box(lay->root, out, 0);
}
