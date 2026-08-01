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


/* ---- (style, avail_w) 幾何キャッシュ ---- */
typedef struct {
    const IfStyle *st;
    i32 w;
    i32 ml, mr, mt, mb, pl, pr, pt, pb;
    i32 bl, brd, bt, bbo;
    i32 content_w;
    i32 height_spec; /* <0 = auto */
    u8 ok;
} IfGeomEnt;
#define IF_GEOM_SIZE 1024u
typedef struct { IfGeomEnt tab[IF_GEOM_SIZE]; } IfGeomCache;

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
    IfGeomCache *geom;
    IfLayout *lay;       /* lines ログ記録先 */
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

static u32 deco_add(IfLC *lc, u8 kind, i32 x, i32 y, i32 w, i32 h, u32 argb,
                    const IfStyle *st, const char *text, u8 tlen) {
    IfLayout *lay = lc->lay;
    lay->deco = (IfDeco *)if_arena_grow(lc->arena, lay->deco, &lay->cap_deco,
                                        lay->n_deco + 1, sizeof(IfDeco));
    IfDeco *d = &lay->deco[lay->n_deco];
    d->kind = kind; d->tlen = tlen;
    if (tlen && text) memcpy(d->text, text, tlen);
    d->x = x; d->y = y; d->w = w; d->h = h;
    d->argb = argb; d->st = st;
    return lay->n_deco++;
}

static IfBox *new_box(IfLC *lc, u8 kind, IfNode *node, const IfStyle *st) {
    /* calloc ではなく alloc + 全メンバ明示初期化（64B memset の依存チェインを避ける） */
    IfBox *b = (IfBox *)if_arena_alloc(lc->arena, sizeof(IfBox));
    b->first_child = NULL;
    b->next_sibling = NULL;
    b->node = node;
    b->st = st;
    b->segs = NULL;
    b->x = 0; b->y = 0; b->w = 0; b->h = 0;
    b->n_segs = 0;
    b->kind = kind;
    b->text_align = 0;
    b->_pad[0] = 0; b->_pad[1] = 0;
    return b;
}


static const IfGeomEnt *geom_get(IfLC *lc, IfGeomCache *gc, const IfStyle *st, i32 avail_w) {
    u64 h = ((uintptr_t)st >> 4) * 2654435761u ^ (u64)(u32)avail_w * 40503u;
    IfGeomEnt *e = &gc->tab[h & (IF_GEOM_SIZE - 1)];
    if (e->ok && e->st == st && e->w == avail_w) return e;
    float fs = st->font_size;
    e->st = st;
    e->w = avail_w;
    e->bl  = st->border_w[3] > 0.0f ? 1 : 0;
    e->brd = st->border_w[1] > 0.0f ? 1 : 0;
    e->bt  = st->border_w[0] > 0.0f ? 1 : 0;
    e->bbo = st->border_w[2] > 0.0f ? 1 : 0;
    e->ml = len_h(st->margin[3], fs, lc->root_fs, avail_w);
    e->mr = len_h(st->margin[1], fs, lc->root_fs, avail_w);
    e->mt = len_v(st->margin[0], fs, lc->root_fs, avail_w);
    e->mb = len_v(st->margin[2], fs, lc->root_fs, avail_w);
    e->pl = len_h(st->padding[3], fs, lc->root_fs, avail_w);
    e->pr = len_h(st->padding[1], fs, lc->root_fs, avail_w);
    e->pt = len_v(st->padding[0], fs, lc->root_fs, avail_w);
    e->pb = len_v(st->padding[2], fs, lc->root_fs, avail_w);
    if (st->width.unit != IF_U_AUTO) {
        e->content_w = len_h(st->width, fs, lc->root_fs, avail_w);
        if (e->content_w < 0) e->content_w = 0;
        i32 total = e->ml + e->bl + e->pl + e->content_w + e->pr + e->brd + e->mr;
        if (total < avail_w && st->margin[3].unit == IF_U_AUTO && st->margin[1].unit == IF_U_AUTO)
            e->ml = e->mr = (avail_w - total) / 2; /* margin:auto センタリング */
    } else {
        e->content_w = avail_w - e->ml - e->mr - e->bl - e->brd - e->pl - e->pr;
        if (e->content_w < 0) e->content_w = 0;
    }
    e->height_spec = -1;
    if (st->height.unit != IF_U_AUTO)
        e->height_spec = len_v(st->height, fs, lc->root_fs, avail_w);
    e->ok = 1;
    return e;
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
    u8 direct_all;         /* 現行の全グリフが IF_LF_DIRECT_BYTES 条件を満たす */
} IfWrap;

/* デコード済み 1 グリフの byte-direct 妥当性を畳み込む。
 * 条件: gw>0（gw==0 はセルを生成しないためバイト列とセル列が乖離する）かつ
 * U+FFFD 置換発生時は元バイトが正に EF BF BD（enc∘dec 恒等の唯一の許容例）。 */
static inline void wrap_note_direct(IfWrap *w, const u8 *base, u32 from, u32 to, u32 cp, int gw) {
    if (gw == 0) { w->direct_all = 0; return; }
    if (cp == IF_CP_REPLACEMENT &&
        !(to - from == 3 && base[from] == 0xEF && base[from + 1] == 0xBF && base[from + 2] == 0xBD))
        w->direct_all = 0;
}

/* 直前 seg と style が同じでソース上連続なら拡張する合体 push（seg 爆発の構造消去。
 * レンダリングされるセル列は変わらない（同じバイト・同じ x・同じ style）） */
static void wrap_push_seg(IfWrap *w, IfStr text, i32 x, i32 width, const IfStyle *st);
static void wrap_push_merge(IfWrap *w, const char *p, u32 n, i32 x, i32 width, const IfStyle *st) {
    if (n == 0) return;
    if (w->n_segs) {
        IfSeg *last = &w->segs[w->n_segs - 1];
        if (last->st == st && last->text.p + last->text.n == p) {
            last->text.n += n;
            last->w += width;
            return;
        }
    }
    wrap_push_seg(w, if_str(p, n), x, width, st);
}

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
    line->_pad[0] = w->direct_all ? IF_LF_DIRECT_BYTES : 0;
    w->direct_all = 1; /* 次行は既定で有効（無効化は wrap_note_direct で畳む） */
    box_add_child(w->lc, w->parent, line);
    /* 行スイープ直接発行用のログ（生成順 = y 単調非減少） */
    {
        IfLC *lc2 = w->lc;
        if (lc2->lay) {
            lc2->lay->lines = (IfBox **)if_arena_grow(lc2->arena, lc2->lay->lines,
                &lc2->lay->cap_lines, lc2->lay->n_lines + 1, sizeof(IfBox *));
            lc2->lay->lines[lc2->lay->n_lines++] = line;
        }
    }
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
                /* ' '(0x20) のみセル列とバイト列が一致（\t は前進 8、他は gw==0 で非発行セル） */
                if (b0 != ' ') w->direct_all = 0;
                wrap_push_seg(w, if_str((const char *)s + i, 1), w->content_x + cx, adv, st);
                cx += adv; i++; *any = true; continue;
            }
            u32 wsend = i + 1;
            while (wsend < n && (s[wsend] == ' ' || s[wsend] == '\t' || s[wsend] == '\n' ||
                                 s[wsend] == '\r' || s[wsend] == '\f')) wsend++;
            u8 last_ws = s[wsend - 1];
            i = wsend;
            if (cx > 0 && cx < w->content_w) {
                if (last_ws == ' ') {
                    /* 実ソースの 0x20 に乗せる（static 文字列と同じ cp のまま seg 合体可） */
                    wrap_push_merge(w, (const char *)s + wsend - 1, 1, w->content_x + cx, 1, st);
                } else {
                    wrap_push_seg(w, IF_S(" "), w->content_x + cx, 1, st);
                }
                cx += 1;
            }
            continue;
        }

        /* アトム切り出し: ASCII 可視ランは一括、全角は 1 グリフ、それ以外は従来規則。 */
        u32 atom_start = i;
        i32 atom_w = 0;
        if (b0 >= 0x21 && b0 <= 0x7E) {
            u32 j = i + 1;
            while (j < n) { u8 cc = s[j]; if (cc < 0x21 || cc > 0x7E) break; j++; }
            atom_w = (i32)(j - i);
            i = j;
        } else {
            u32 save = i;
            u32 cp = if_utf8_decode(s, n, &save);
            int gw = if_glyph_width(cp);
            wrap_note_direct(w, s, i, save, cp, gw);
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
                    wrap_note_direct(w, s, i, s2, cp2, gw2);
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
            /* 行尾の折り畳み空白を除く: 全seg空白なら pop、合体 seg の場合は末尾 1B を削る。
             * （アトムは '\x20' を含まないので「末尾が 0x20」⇔ 直近 push が折り畳み空白） */
            if (w->n_segs > 0) {
                IfSeg *last = &w->segs[w->n_segs - 1];
                if (last->text.n && last->text.p[last->text.n - 1] == ' ') {
                    if (last->text.n == 1) w->n_segs--;
                    else { last->text.n--; last->w--; }
                }
            }
            cx = w->n_segs > 0 ? w->segs[w->n_segs - 1].x + w->segs[w->n_segs - 1].w - w->content_x : 0;
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
                wrap_note_direct(w, s, gs, g, cp3, gw3);
                i32 gwidth = gw3 == 2 ? 2 : 1;
                if (cx > 0 && cx + gwidth > w->content_w) {
                    wrap_end_line(w, *max_lh);
                    cx = 0;
                    *max_lh = lh;
                }
                wrap_push_merge(w, (const char *)s + gs, g - gs, w->content_x + cx, gwidth, st);
                cx += gwidth;
            }
            continue;
        }

        wrap_push_merge(w, (const char *)s + atom_start, atom_end - atom_start,
                        w->content_x + cx, atom_w, st);
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

    IfWrap w = { lc, content_x, content_w, *y_io, parent, lc->segs_scratch, 0, 0, base_st, 1 };
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
    u32 li_ord = 0; /* ol 番号: この親の LIST_ITEM li を出現順に数える（draw_marker 同値） */
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
        const IfGeomEnt *cg = geom_get(lc, lc->geom, cst, content_w);
        i32 mt = cg->mt;
        i32 mb = cg->mb;
        y += (prev_mb > mt ? prev_mb : mt); /* 兄弟縦マージン相殺: max */
        if (lc->lay && c->tag == IF_TAG_LI && cst->display == IF_D_LIST_ITEM) {
            /* draw_marker と同値の計算を layout 時点で行い MARKER op を追記する。
             * （配置は li box 左端の手前/上端。x,y はこの時点で確定済み） */
            li_ord++;
            u16 list = IF_TAG_UL;
            for (IfNode *p = c->parent; p; p = p->parent) {
                if (p->kind == IF_NODE_ELEMENT && (p->tag == IF_TAG_UL || p->tag == IF_TAG_OL)) { list = p->tag; break; }
                if (p->kind == IF_NODE_ELEMENT && p->tag == IF_TAG_LI) break;
            }
            /* li_ord = 「この親で自分より前の LIST_ITEM li 数+1」（draw_marker の
             * 兄弟走査と同値だが O(1)。c->parent == node なので同じ集合を数えている） */
            u32 idx = li_ord;
            i32 bx = content_x + cg->ml;
            if (list == IF_TAG_UL) {
                i32 mx = bx - 2;
                if (mx < 0) mx = bx;
                deco_add(lc, IF_DECO_MARKER, mx, y, 2, 1, 0, cst, "\xE2\x80\xA2 ", 4);
            } else {
                char nb[12];
                int m = snprintf(nb, sizeof nb, "%u.", idx);
                if (m > 0) {
                    i32 mx = bx - (m + 1);
                    if (mx < 0) mx = 0;
                    deco_add(lc, IF_DECO_MARKER, mx, y, (i32)m, 1, 0, cst, nb, (u8)m);
                }
            }
        }
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
    const IfGeomEnt *g = geom_get(lc, lc->geom, st, avail_w);
    i32 bl = g->bl, brd = g->brd, bt = g->bt, bbo = g->bbo;
    i32 pad_l = g->pl, pad_r = g->pr, pad_t = g->pt, pad_b = g->pb;
    i32 content_w = g->content_w;

    i32 x = ax + g->ml;
    i32 content_x = x + bl + pad_l;
    i32 y = ay; /* margin-top は呼び出し側（兄弟相殺）で処理済み */
    i32 content_y = y + bt + pad_t;

    /* 行スイープ用装飾 op（DFS=paint 順: 親の装飾は子より先に追記される） */
    u32 deco_bg = UINT32_MAX, deco_bd = UINT32_MAX;
    if (lc->lay && (st->bg & 0xFF) >= 128)
        deco_bg = deco_add(lc, IF_DECO_BG, x, y, bl + pad_l + content_w + pad_r + brd,
                           0 /* h 後埋め */, st->bg, NULL, NULL, 0);

    if (node->tag == IF_TAG_HR) {
        box->x = x; box->y = y;
        box->w = bl + pad_l + content_w + pad_r + brd;
        box->h = bt + 1 + bbo;
        if (lc->lay) {
            /* paint_shell の HR: bt を除いた行へ罫線（NULL style のフルペン既定） */
            deco_add(lc, IF_DECO_HLINE, x, y + (bt ? 1 : 0), box->w, 1, 0, NULL, NULL, 0);
            if (deco_bg != UINT32_MAX) lc->lay->deco[deco_bg].h = box->h;
        }
        return box;
    }

    if (lc->lay && (bl | brd | bt | bbo)) {
        u8 sides = (u8)((bt ? 1 : 0) | (brd ? 2 : 0) | (bbo ? 4 : 0) | (bl ? 8 : 0));
        deco_bd = deco_add(lc, IF_DECO_BORDER, x, y, bl + pad_l + content_w + pad_r + brd,
                           0, st->border_color, NULL, NULL, sides);
    }

    frame_push(lc, box); /* box の子追加は frame_top==box で O(1) tail を引く */
    i32 content_h = layout_children(lc, box, node, content_x, content_y, content_w);
    frame_pop(lc, box);
    if (g->height_spec >= 0 && g->height_spec > content_h)
        content_h = g->height_spec; /* 指定高はクリップせず拡張のみ（v0.1 近似） */

    box->x = x;
    box->y = y;
    box->w = bl + pad_l + content_w + pad_r + brd;
    box->h = bt + pad_t + content_h + pad_b + bbo;
    if (lc->lay) {
        if (deco_bg != UINT32_MAX) lc->lay->deco[deco_bg].h = box->h;
        if (deco_bd != UINT32_MAX) lc->lay->deco[deco_bd].h = box->h;
    }
    return box;
}

IfLayout *if_layout_build(IfArena *arena, IfDom *dom, i32 width_cells) {
    if (width_cells < 4) width_cells = 4;
    IfLayout *lay = (IfLayout *)if_arena_calloc(arena, sizeof(IfLayout));
    IfLC lc = { .arena = arena, .root_fs = 16.0f };
    lc.geom = (IfGeomCache *)if_arena_calloc(arena, sizeof(IfGeomCache));
    lc.lay = lay;
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
