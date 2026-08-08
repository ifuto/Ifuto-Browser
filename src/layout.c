/* Ifuto — レイアウト実装。
 * アルゴリズム: トップダウン DFS。幅は包含ブロックから確定、高さは子を敷いてから確定。
 *   block: ネストした要素を縦に積む（兄弟マージンは max 相殺）。
 *   inline: IFC を flatten → アトム化（単語ラン / 全角 1 グリフ）→ 貪欲折り返し。
 * 計算量: O(ノード数 + グリフ数)。兄弟走査はカーソル前進で O(N)（先頭からの辿り直し禁止）。
 */
#include "layout.h"
#include "utf8.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h> /* 2-way 並列 layout（md fast-DOM のみ。glibc>=2.34 で ldd 不変） */
#if defined(__SSE2__) || defined(__x86_64__)
#include <emmintrin.h> /* ASCII 可視ラン一括分類（SSE2 は x86_64 基底。dispatch 不要） */
#include <immintrin.h> /* AVX2 版可視ラン走査（target attr + runtime dispatch、md.c 同型） */

/* ASCII 可視ランの終端（最初の非可視バイト位置）。可視 ⇔ 0x21<=b<=0x7E。
 * 判定: r = b-0x21（epu8 wrap）は in-range で 0x00-0x5D、out-range で 0x5E-0xFF。
 * t = r^0x80 とおくと out-range ⟺ t >s8 0xDD(=0x5D^0x80)。境界 4 点は機械検証済:
 * b=0x20→r=0xFF→t=0x7F>−35 stop / b=0x21→t=−128 no-stop / b=0x7E→t=0xDD no-stop /
 * b=0x7F→t=0xDE>−35 stop。停止位置はスカラ規則（最初に範囲外のバイト）と厳密一致。 */
__attribute__((target("avx2"))) static u32 lw_ascii_run_end_avx2(const u8 *s, u32 n, u32 i) {
    const __m256i off = _mm256_set1_epi8(0x21);
    const __m256i flip = _mm256_set1_epi8((char)0x80);
    const __m256i lim = _mm256_set1_epi8((char)0xDD);
    for (; i + 32 <= n; i += 32) {
        __m256i b = _mm256_loadu_si256((const __m256i *)(s + i));
        __m256i t = _mm256_xor_si256(_mm256_sub_epi8(b, off), flip);
        unsigned m = (unsigned)_mm256_movemask_epi8(_mm256_cmpgt_epi8(t, lim));
        if (m) return i + (u32)__builtin_ctz(m);
    }
    return i; /* 末尾 32B 未満は呼び出し側のスカラが続きから処理 */
}

static u32 lw_ascii_run_end(const u8 *s, u32 n, u32 i) {
    static int have_avx2 = -1;
    if (__builtin_expect(have_avx2 < 0, 0))
        have_avx2 = __builtin_cpu_supports("avx2") ? 1 : 0;
    if (have_avx2) return lw_ascii_run_end_avx2(s, n, i);
    return i;
}
#define IF_SSE2 1
#endif


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
typedef struct {
    IfGeomEnt tab[IF_GEOM_SIZE];
    /* 1 エントリ直前メモ: 同一 (st,w) の連続呼出（兄弟で style が同じ形状が支配的）を
     * ハッシュ計算+tab 参照なしで返す。不変条件: last_e は tab 内の有効エントリを指す。 */
    const IfGeomEnt *last_e;
    const IfStyle *last_st;
    i32 last_w;
} IfGeomCache;

typedef struct IfPiece {
    IfStr text;
    const IfStyle *st;
    u8 br;   /* 1 = 強制改行 */
} IfPiece;

/* flatten 経路の <a> 表示矩形収集: piece 区間 [p0,p1) を DFS preorder（=piece 順、
 * p0 単調）に記録し、wrap 連鎖で確定 seg 添字（open/close = 大域 seg 位置）へ写像
 * → 行ログと交差解決する。木構築モード（no_boxlink=0）のみ記録。線形 CLI は
 * 収集自体が発生しない（zero cost）。close=UINT32_MAX は未閉鎖（p1 未到達） */
typedef struct { u32 link, p0, p1, open, close; } IfLinkPrec;

/* ---- 開発用 rdtsc ゾーン計測（IF_LAYOUT_PROF=1 のときのみ。既定経路は分岐 1 個） ---- */
#if defined(__x86_64__) || defined(__i386__)
static inline u64 if_rdtsc(void) { u32 lo, hi; __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi)); return ((u64)hi << 32) | lo; }
#else
static inline u64 if_rdtsc(void) { return 0; }
#endif
static int if_lp_on = -2; /* -2 = 未初期化 */
static u64 LPF_TOTAL, LPF_IFC, LPF_FLAT, LPF_WRAP, LPF_ENDL, LPF_GEOM, LPF_CHILDREN;
static u64 LPF_FITOK, LPF_FITNG, LPF_ELEM;
__attribute__((destructor)) static void lpf_dump(void) {
    if (if_lp_on > 0)
        fprintf(stderr, "LAYOUTPROF total=%llu ifc=%llu flat=%llu wrap=%llu endl=%llu geom=%llu children-sites=%llu fit_ok=%llu fit_ng=%llu elem=%llu (cycles)\n",
                (unsigned long long)LPF_TOTAL, (unsigned long long)LPF_IFC, (unsigned long long)LPF_FLAT,
                (unsigned long long)LPF_WRAP, (unsigned long long)LPF_ENDL, (unsigned long long)LPF_GEOM,
                (unsigned long long)LPF_CHILDREN, (unsigned long long)LPF_FITOK, (unsigned long long)LPF_FITNG,
                (unsigned long long)LPF_ELEM);
}
static inline bool lpf(void) {
    if (if_lp_on == -2) { const char *e = getenv("IF_LAYOUT_PROF"); if_lp_on = (e && e[0] == '1') ? 1 : 0; }
    return if_lp_on > 0;
}

typedef struct {
    IfArena *arena;
    float root_fs;
    IfLink *links;
    u32 n_links;
    u64 links_cap;
    /* IFC/折り返しスクラッチ: pieces 配列は「run 内で消費が完結」するので再利用する。
     * （segs は 2026-08-01 に直接 arena 確定へ移行: LINE ごとの alloc+memcpy 確定を消去。
     *   巻き戻し可能な pop のみを許し、スクラッチ二重書きは構造排除） */
    IfPiece *pieces_scratch;
    u64 pieces_scratch_cap;
    IfLinkPrec *prec_scratch;  /* link span 収集のスクラッチ（pieces_scratch と同規約。
                                * ifc ごとに n 0 リセットで再利用。木構築モードのみ消費） */
    u64 prec_scratch_cap;
    /* box 構築時 tail（IfBox から last_child フィールドを追放した代替。子追加は
     * 親 box 構築中に集中かつ再帰スタック規律なので、frame に持って O(1) を保つ。
     * frame 深さ超過/不一致時は兄弟走査の正しいフォールバックに落ちる（性能のみ） */
    IfBox *frame_box[512];
    IfBox *frame_tail[512];
    int frame_n;
    IfGeomCache *geom;
    IfLayout *lay;       /* lines ログ記録先 */
    /* 線形モード（CLI 行スイープ専用）: BLOCK 箱は親接続・木 dump の対象外で、
     * 戻り値 h のみが意味を持つ → 深さ分だけ生存するスクラッチから再利用する
     * （arena への ~49MB の box stream と add_child/frame の接続作業を構造消去）。
     * 不変条件: no_boxlink の LC では box 木を誰も辿らない（出力は lines[]/deco[] のみ）。 */
    IfBox *box_pool;
    u8 no_boxlink;
    u8 md_ws_stripped;   /* dom->md_ws_stripped のコピー（下記 mo_ws_sink 対応補正用） */
    IfNode *stop;        /* 並列シャードの spine 打ち止め（NULL=従来どおり兄弟末尾まで） */
    /* lazy style（線形 CLI 路専用。NULL=従来の eager 経路）。詳細は css.h の注釈。
     * lazy 時は ELEMENT の st を n->style から読まず if_style_lazy_get で解決する
     * （値は if_style_apply 全面走査と同値。解決は DFS 親→子の順序で行われる） */
    IfStyleLazy *lazy;
    float lazy_rfs;      /* html 由来の rem 基準（build_impl が確定して全 ctx で共有） */
} IfLC;

/* st アクセスの一点化。戻り値規約:
 *   lazy  : ELEMENT → 解決値（非 NULL）。非 ELEMENT → pst（従来の継承と同じ意味）
 *   eager : n->style（呼出側が ?: 既定で処理。従来どおり NULL があり得る） */
static inline const IfStyle *lc_st_of(IfLC *lc, IfNode *n, const IfStyle *pst) {
    if (__builtin_expect(lc->lazy != NULL, 0))
        return (n->kind == IF_NODE_ELEMENT) ? if_style_lazy_get(lc->lazy, n, pst, lc->lazy_rfs) : pst;
    return n->style;
}

/* md.c の mo_ws_sink と同じタグ集合（ md fast-DOM はこれら直下の ws-only TEXT を
 * 剥がす）。旧 DOM では当該 ws TEXT が ifc 経由で prev_mb を 0 にしていたので、
 * 剥がし後も同じ容器の兄弟では相殺を無効化して逐語同値を保つ（layout_children 参照）。 */
static inline bool ws_sink_parent(u16 tag) {
    switch (tag) {
    case IF_TAG_BODY: case IF_TAG_BLOCKQUOTE: case IF_TAG_TABLE: case IF_TAG_THEAD:
    case IF_TAG_TBODY: case IF_TAG_TR: case IF_TAG_UL: case IF_TAG_OL:
    case IF_TAG_SECTION:
        return true;
    default:
        return false;
    }
}

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
    if (lc->no_boxlink) { (void)parent; (void)child; return; } /* 線形: 接続なし */
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

/* 線形モードの BLOCK 箱回収: 呼出側が child->h を読み終えた直後にのみ呼ぶ。
* （スクラッチ返却。arena 確定の LINE 箱や root は絶対に返さない） */
static inline void box_recycle(IfLC *lc, IfBox *b) {
    if (lc->no_boxlink && b->kind == IF_BOX_BLOCK) {
        b->next_sibling = lc->box_pool;
        lc->box_pool = b;
    }
}

/* IfBox 生成直後（子の構築を始める前）に frame を積み、構築完了で降ろす */
static void frame_push(IfLC *lc, IfBox *box) {
    if (lc->no_boxlink) return; /* 線形: 子接続をしないので frame も不要 */
    if (lc->frame_n < 512) { lc->frame_box[lc->frame_n] = box; lc->frame_tail[lc->frame_n] = NULL; }
    lc->frame_n++;
}
static void frame_pop(IfLC *lc, IfBox *box) {
    if (lc->no_boxlink) return;
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
    IfBox *b;
    if (kind == IF_BOX_BLOCK && lc->no_boxlink && lc->box_pool) {
        b = lc->box_pool;          /* 線形: 深さ生存スクラッチから再利用 */
        lc->box_pool = b->next_sibling;
    } else {
        b = (IfBox *)if_arena_alloc(lc->arena, sizeof(IfBox));
    }
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
    u64 _t0; if (lpf()) _t0 = if_rdtsc(); else _t0 = 0;
    if (__builtin_expect(st == gc->last_st && avail_w == gc->last_w, 1)) {
        if (_t0) LPF_GEOM += if_rdtsc() - _t0;
        return gc->last_e;
    }
    u64 h = ((uintptr_t)st >> 4) * 2654435761u ^ (u64)(u32)avail_w * 40503u;
    IfGeomEnt *e = &gc->tab[h & (IF_GEOM_SIZE - 1)];
    if (e->ok && e->st == st && e->w == avail_w) {
        gc->last_e = e; gc->last_st = st; gc->last_w = avail_w;
        if (_t0) LPF_GEOM += if_rdtsc() - _t0;
        return e;
    }
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
    gc->last_e = e; gc->last_st = st; gc->last_w = avail_w;
    if (_t0) LPF_GEOM += if_rdtsc() - _t0;
    return e;
}

/* ---------- インラインフラット化 ---------- */

typedef struct {
    IfPiece *pieces;   /* == lc->pieces_scratch（共有スクラッチ） */
    u32 n_pieces;
    IfLC *lc;
    IfLinkPrec *prec;  /* == lc->prec_scratch（共有スクラッチ。NULL なら収集なし） */
    u32 n_prec;
} IfFlat;

/* <a> の piece 区間を DFS preorder に追記（p0 単調が wrap 連鎖のカーソル前進を可能にする）。
 * 破壊的上限 4096/ifc: 超過分は収集せず（巨大ナビバー等の異常系で wrap 連鎖の走査を
 * 上限内に抑える。GUI 実用で到達しない） */
static void flat_push_prec(IfFlat *f, u32 link, u32 p0, u32 p1) {
    if (f->n_prec >= 4096) return;
    f->prec = (IfLinkPrec *)if_arena_grow(f->lc->arena, f->prec, &f->lc->prec_scratch_cap,
                                          f->n_prec + 1, sizeof(IfLinkPrec));
    f->prec[f->n_prec].link = link;
    f->prec[f->n_prec].p0 = p0;
    f->prec[f->n_prec].p1 = p1;
    f->prec[f->n_prec].close = UINT32_MAX;
    f->n_prec++;
}

static void flat_push_grow(IfFlat *f, IfStr text, const IfStyle *st, u8 br);
static inline void flat_push(IfFlat *f, IfStr text, const IfStyle *st, u8 br) {
    if (__builtin_expect(f->n_pieces < f->lc->pieces_scratch_cap, 1)) {
        f->pieces[f->n_pieces].text = text;
        f->pieces[f->n_pieces].st = st;
        f->pieces[f->n_pieces].br = br;
        f->n_pieces++;
        return;
    }
    flat_push_grow(f, text, st, br);
}
static void flat_push_grow(IfFlat *f, IfStr text, const IfStyle *st, u8 br) {
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
    lc->links[lc->n_links].spans = NULL;
    lc->links[lc->n_links].n_spans = 0;
    lc->n_links++;
}

static void flatten_into(IfFlat *f, IfNode *n, const IfStyle *st) {
    if (n->kind == IF_NODE_TEXT) { flat_push(f, n->u.text, st, 0); return; }
    if (n->kind != IF_NODE_ELEMENT) return;
    const IfStyle *est;
    if (__builtin_expect(f->lc->lazy != NULL, 0)) {
        est = lc_st_of(f->lc, n, st);           /* lazy: 解決値（非 NULL） */
        if (est->display == IF_D_NONE) return;
    } else {
        est = n->style ? n->style : st;
        if (n->style && n->style->display == IF_D_NONE) return;
    }
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
    case IF_TAG_A: {
        /* <a> の piece 区間は「入口 — 子 flatten 完了位置」で厳密に切る
         * （collect 不発（href 無し）なら記録自体しない: index 不整合を防ぐ） */
        u32 ln0 = f->lc->n_links;
        u32 p0 = f->n_pieces;
        collect_link(f->lc, n);
        bool rec = !f->lc->no_boxlink && f->lc->n_links != ln0;
        for (IfNode *c = n->first_child; c; c = c->next_sibling)
            flatten_into(f, c, est);
        if (rec) flat_push_prec(f, ln0, p0, f->n_pieces);
        return;
    }
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
    IfSeg *seg_base;       /* 現行の seg 先頭（arena 直接確定。LINE ごとのコピー不在） */
    u32 n_segs;
    i32 line_w;
    const IfStyle *align_st;
    u8 direct_all;         /* 現行の全グリフが IF_LF_DIRECT_BYTES 条件を満たす */
    u32 seg_hi;            /* この Wrap が wrap_end_line で確定した seg の累計
                            * （ifc 局所の大域 seg 添字の基底。現行位置 = seg_hi + n_segs） */
    /* merge-hit 判定のレジスタ常駐化（seg 配列最終要素の二重ロード消去）。
     * 整合規約: 真値は常に seg_base[n_segs-1] から導出可能。push 確定点で更新、
     * pop/巻き戻しで pm_st=NULL に無効化（n_segs==0 ゲートでも陳腐化は読まれない） */
    const IfStyle *pm_st;
    const char *pm_end;
} IfWrap;

/* direct のみ版（gw は呼出側が処理済み） */
static inline void wrap_note_direct2(IfWrap *w, const u8 *base, u32 from, u32 to, u32 cp) {
    if (cp == IF_CP_REPLACEMENT &&
        !(to - from == 3 && base[from] == 0xEF && base[from + 1] == 0xBF && base[from + 2] == 0xBD))
        w->direct_all = 0;
}

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
static inline void wrap_push_merge(IfWrap *w, const char *p, u32 n, i32 x, i32 width, const IfStyle *st) {
    if (n == 0) return;
    if (__builtin_expect(w->n_segs != 0 && w->pm_st == st && w->pm_end == p, 1)) {
        IfSeg *last = &w->seg_base[w->n_segs - 1];
        last->text.n += n;
        last->w += width;
        w->pm_end = p + n;
        return;
    }
    wrap_push_seg(w, if_str(p, n), x, width, st);
}

/* seg 系列のブロック跨ぎ検知時の移設: (n_segs+1) 厳密の連続領域へ写し、
 * bump 位置が seg_base+n_segs に自然接続するようにする（cap==n_segs+1 の不変条件）。
 * 発生頻度: 256KB arena ブロック境界ごとに 1 行分まで（30MB で ~120 回規模）。 */
static void wrap_repack_segs(IfWrap *w, IfSeg *spill) {
    IfSeg *nb = (IfSeg *)if_arena_alloc(w->lc->arena, (u64)(w->n_segs + 1) * sizeof(IfSeg));
    if (w->n_segs) memcpy(nb, w->seg_base, (u64)w->n_segs * sizeof(IfSeg));
    nb[w->n_segs] = *spill;
    w->seg_base = nb;
}

static inline void wrap_push_seg(IfWrap *w, IfStr text, i32 x, i32 width, const IfStyle *st) {
    if (text.n == 0) return;
    IfSeg *s = (IfSeg *)if_arena_bump(w->lc->arena, sizeof(IfSeg));
    if (w->n_segs == 0) {
        w->seg_base = s;
    } else if (__builtin_expect(s != w->seg_base + w->n_segs, 0)) {
        /* arena ブロック境界で不連続: repack して spill を自分で書き込む */
        wrap_repack_segs(w, s);
        w->seg_base[w->n_segs].text = text;
        w->seg_base[w->n_segs].x = x;
        w->seg_base[w->n_segs].w = width;
        w->seg_base[w->n_segs].st = st;
        w->n_segs++;
        w->pm_st = st; w->pm_end = text.p + text.n;
        return;
    }
    s->text = text; s->x = x; s->w = width; s->st = st;
    w->n_segs++;
    w->pm_st = st; w->pm_end = text.p + text.n;
}

/* 行尾の折り畳み空白 pop（巻き戻し可能なのは seg 系列が最新端のときだけ） */
static inline void wrap_pop_last_seg(IfWrap *w) {
    IfSeg *last = &w->seg_base[w->n_segs - 1];
    if_arena_rewind_last(w->lc->arena, last, sizeof(IfSeg));
    w->n_segs--;
    w->pm_st = NULL; /* 陳腐化防止（次 push は必ず wrap_push_seg 経由で再設定） */
}

static void wrap_end_line(IfWrap *w, float max_lh) {
    u64 _e0; if (lpf()) _e0 = if_rdtsc(); else _e0 = 0;
    /* px2row(x)==1 ⇔ 8<=x<24（floor(x/16+.5) の区間同値。頻出 lh=19.2 を 1 分岐で抜ける） */
    i32 rows = (max_lh >= 8.0f && max_lh < 24.0f) ? 1 : px2row(max_lh);
    if (rows < 1) rows = 1;
    u8 align = w->align_st ? w->align_st->text_align : IF_TA_LEFT;
    i32 shift = 0;
    if (align == IF_TA_CENTER && w->line_w < w->content_w) shift = (w->content_w - w->line_w) / 2;
    else if (align == IF_TA_RIGHT && w->line_w < w->content_w) shift = w->content_w - w->line_w;

    /* seg は arena 直接確定済み（無コピー）。align シフトだけ確定時に適用 */
    if (w->n_segs && shift)
        for (u32 i = 0; i < w->n_segs; i++) w->seg_base[i].x += shift;
    u16 lflags = w->direct_all ? IF_LF_DIRECT_BYTES : 0;
    w->direct_all = 1; /* 次行は既定で有効（無効化は wrap_note_direct で畳む） */
    /* 木構築モードのみ IfBox を実体化（線形モードの LINE box は誰も読まない） */
    if (!w->lc->no_boxlink) {
        IfBox *line = new_box(w->lc, IF_BOX_LINE, NULL, w->align_st);
        line->x = w->content_x;
        line->y = w->y;
        line->w = w->line_w;
        line->h = rows;
        line->text_align = align;
        if (w->n_segs) {
            line->segs = w->seg_base;
            line->n_segs = w->n_segs;
        }
        line->_pad[0] = (u8)lflags;
        box_add_child(w->lc, w->parent, line);
    }
    /* 行スイープ直接発行用のログ（生成順 = y 単調非減少）。sweep は y/segs/
     * n_segs/flags 以外を読まない（全 src 監査済）→ 24B の値レコードで確定 */
    {
        IfLC *lc2 = w->lc;
        IfLayout *lay = lc2->lay;
        if (lay) {
            IfRLine rl;
            rl.segs = w->n_segs ? w->seg_base : NULL;
            rl.n_segs = w->n_segs;
            rl.y = w->y;
            rl.flags = lflags;
            rl._pad = 0;
            IfLChunk *ck = lay->lines_tail;
            if (__builtin_expect(!ck || ck->n == IF_LCHUNK_N, 0)) {
                ck = (IfLChunk *)if_arena_alloc(lc2->arena, sizeof(IfLChunk));
                ck->next = NULL;
                ck->n = 0;
                if (lay->lines_tail) lay->lines_tail->next = ck;
                else lay->lines_head = ck;
                lay->lines_tail = ck;
            }
            ck->v[ck->n++] = rl;
            lay->n_lines++;
        }
    }
    w->y += rows;
    w->seg_hi += w->n_segs; /* ifc 局所 seg 累計（flatten 経路の link span 解決基底） */
    w->n_segs = 0;
    if (_e0) LPF_ENDL += if_rdtsc() - _e0;
}

/* 高頻度ワイドレンジ先出し（互いに排他的なレンジ一致で if_glyph_width と同値） */
static inline int lw_glyph_width(u32 cp) {
    if (cp >= 0x3041 && cp <= 0x33FF) return 2; /* ひらがな・カタカナ・CJK 記号 */
    if (cp >= 0x4E00 && cp <= 0x9FFF) return 2; /* CJK 統合漢字 */
    return if_glyph_width(cp);
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
                if (b0 != ' ') {
                    w->direct_all = 0;
                    wrap_push_seg(w, if_str((const char *)s + i, 1), w->content_x + cx, adv, st);
                } else {
                    /* 連続バイト・同 style → 先行 seg へ合体（pre 内のスペース嵐を消す） */
                    wrap_push_merge(w, (const char *)s + i, 1, w->content_x + cx, 1, st);
                }
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
            /* AVX2 で 32B ずつ走査（停止位置はスカラ規則と厳密一致）、残りはスカラ */
            j = lw_ascii_run_end(s, n, j);
            while (j < n) { u8 cc = s[j]; if (cc < 0x21 || cc > 0x7E) break; j++; }
            atom_w = (i32)(j - i);
            i = j;
        } else {
            u32 save = i;
            u32 cp = if_utf8_decode(s, n, &save);
            int gw = lw_glyph_width(cp);
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
                    int gw2 = lw_glyph_width(cp2);
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
                IfSeg *last = &w->seg_base[w->n_segs - 1];
                if (last->text.n && last->text.p[last->text.n - 1] == ' ') {
                    if (last->text.n == 1) wrap_pop_last_seg(w);
                    else { last->text.n--; last->w--; }
                }
            }
            cx = w->n_segs > 0 ? w->seg_base[w->n_segs - 1].x + w->seg_base[w->n_segs - 1].w - w->content_x : 0;
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
                int gw3 = lw_glyph_width(cp3);
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

/* 3バイト UTF-8 の inline デコード（有効な CJK のみ命中。不正形は従来デコーダへ。
 * E0/ED の overlong/サロゲート規則は if_utf8_decode と同値の除外） */
static inline bool lw_decode3(const u8 *s, u32 n, u32 *io, u32 *cp_out) {
    u32 i = *io;
    u8 b0 = s[i];
    if (__builtin_expect(b0 >= 0xE0 && b0 <= 0xEF && i + 2 < n, 1)) {
        u8 b1 = s[i + 1], b2 = s[i + 2];
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 &&
            !((b0 == 0xE0 && b1 < 0xA0) || (b0 == 0xED && b1 > 0x9F))) {
            *cp_out = ((u32)(b0 & 0x0F) << 12) | ((u32)(b1 & 0x3F) << 6) | (u32)(b2 & 0x3F);
            *io = i + 3;
            return true;
        }
    }
    return false;
}

/* DOM 直接走査版 try_fit: flatten(pieces 配列化) + piece 消費の 2 段を融合し、
 * 成功時は pieces 配列を一切作らない。
 * 同値性の構成:
 *  - 走査順・style 解決（est 連鎖）・br/img-alt/link 収集は flatten_into の機械的鏡像
 *    （img の alt 文字列は同じ snprintf/上限規則で arena に同バイト確保、
 *      link は collect の DFS 順一致、失敗時は n_links を退避値へ復帰）。
 *  - テキスト処理（空白折畳・幅会計・ラン融合・direct/any/lh 畳込み）は
 *    旧 ifc_try_fit のピース処理と逐語同一（TEXT ノード == piece）。
 *  - 失敗時は seg 系列を LIFO rewind、line_w/direct/any/lh/links を全復帰し
 *    従来経路（flatten + wrap 連鎖）へ無痕でフォールバック。 */
typedef struct { u32 link; u32 seg0, seg1; } IfLinkArec;

typedef struct {
    IfWrap *w;
    IfLC *lc;
    const IfStyle *base_st;
    i32 content_w;
    i32 cx;
    bool pre0;
    const IfStyle *lh_st; /* (st→lh) 1-entry メモ: 連続 TEXT の st はほぼ不変 */
    float lh_val;
    /* <a> ヒットテスト幾何: ifc 内の (link_idx, 開始 seg) を記録し、成功確定後に
     * 表示矩形へ解決する（木構築モード限定。失敗では n_links 復帰とともに破棄） */
    IfLinkArec *arec;
    u32 arec_n;
    u64 arec_cap; /* 要素数（if_arena_grow 契約: cap/need は要素、esz=sizeof）*/
 } IfFitDom;

/* TEXT piece 相当の処理（旧 ifc_try_fit の内側ループと逐語同一）。
 * clsf: TEXT ノードの IF_NF_TXTCLS_*（parse 確定の内容分類。0=未知は常に安全側） */
static bool fitdom_text(IfFitDom *fd, IfStr t, const IfStyle *st, u8 clsf,
                        float *max_lh_io, bool *any_io) {
    IfWrap *w = fd->w;
    bool pre_p = (st && st->white_space == IF_WS_PRE) || fd->pre0;
    if (pre_p) return false;
    /* wrap_text 冒頭の lh 畳込みと同じもの（ピース到着で必ず畳む）。st は run 内で
     * ほぼ不変なので (st→lh) を 1-entry メモ化（純粋キャッシュ、結果は同値） */
    if (st != fd->lh_st) {
        float fs = st ? st->font_size : 16.0f;
        fd->lh_val = st && st->line_height > 0.0f ? st->line_height : fs * 1.2f;
        fd->lh_st = st;
    }
    if (fd->lh_val > *max_lh_io) *max_lh_io = fd->lh_val;

    const u8 *s = (const u8 *)t.p;
    u32 n = t.n;
    i32 cx = fd->cx;

    /* parse 確定の内容分類による全走査省略（分類は下部スキャンと同じ述語: dom.h 参照） */
    if (__builtin_expect(clsf != 0, 1)) {
        i32 adv = -1;
        if (clsf == IF_NF_TXTCLS_ASCII_VIS) adv = (i32)n;          /* 全バイト 0x21-0x7E */
        else if (clsf == IF_NF_TXTCLS_CJK3W2) adv = (i32)(n / 3) * 2; /* 全 3B が幅 2 帯 */
        if (adv >= 0) {
            i32 nx = cx + adv;
            /* 失敗境界: 直後の判定 `cx > content_w` と同じ（単一アトムのため一境界のみ）。
             * 下位経路は push→失敗→全 pop なのに対しこちらは push しないが、
             * 失敗時は駆動側が全 seg を LIFO 復帰するため正味状態は厳密一致 */
            if (__builtin_expect(nx > fd->content_w, 0)) { fd->cx = nx; return false; }
            wrap_push_merge(w, (const char *)s, n, w->content_x + cx, adv, st);
            fd->cx = nx;
            *any_io = true;
            return true;
        }
    }

    u32 i = 0;
    /* 単一 ' ' run の空白セル遅延（pend）: 現行は [空白 1B push] → [トークン push] の
     * 2 回で、merge-hit 時も miss 時（空白とトークンは必ずソース連続・同一 style）も
     * 最終 seg 系列が一意に定まる。遅延して 1 回の融合 push（[空白+トークン]）に畳んでも
     * 最終系列は厳密に同一 — hit: 両方連続 merge で [..空白+トークン] ≡、
     * miss: 空白新 seg＋トークン連続 merge → [空白+トークン] ≡。push 回数は半減する。
     * 単一 ' ' のみ対象（複数 ws/tab は即時経路で現行と逐語同一）。pend 生存中は
     * cx がちょうど +1 進んでおり、flush 位置は常に cx-1 / ソース位置は次トークン直前。 */
    u8 pend = 0;
    while (i < n) {
        u8 b0 = s[i];
        if (b0 == ' ' || b0 == '\t' || b0 == '\n' || b0 == '\r' || b0 == '\f') {
            /* ASCII 可視以外の空白 run（折畳は wrap 版と逐語同一） */
            u32 j = i + 1;
            while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\n' ||
                             s[j] == '\r' || s[j] == '\f')) j++;
            if (pend) { /* 連 ws run 前に確定（現在位置の 1B 手前 = pend バイト） */
                wrap_push_merge(w, (const char *)s + i - 1, 1, w->content_x + cx - 1, 1, st);
                pend = 0;
            }
            if (cx > 0 && cx < w->content_w) {
                if (j - i == 1 && b0 == ' ') pend = 1; /* 単一 ' ' → 遅延 */
                else if (s[j - 1] == ' ') wrap_push_merge(w, (const char *)s + j - 1, 1,
                                                   w->content_x + cx, 1, st);
                else wrap_push_seg(w, IF_S(" "), w->content_x + cx, 1, st);
                cx += 1;
            }
            i = j;
            continue;
        }
        if (__builtin_expect(b0 >= 0x21 && b0 <= 0x7E, 1)) {
            /* ASCII 可視ラン（wrap のアトム規則と同じく 0x21-0x7E の連続）。
             * AVX2/SSE2-替代の 32B 一括走査で wrap 側と同じ助っ人に統一。
             * 停止位置はスカラ規則と厳密一致（lw_ascii_run_end 参照） */
            u32 j = i + 1;
            j = lw_ascii_run_end(s, n, j);
            while (j < n) { u8 cc = s[j]; if (cc < 0x21 || cc > 0x7E) break; j++; }
            if (pend) {
                wrap_push_merge(w, (const char *)s + i - 1, (j - i) + 1,
                                w->content_x + cx - 1, (i32)(j - i) + 1, st);
                pend = 0;
            } else {
                wrap_push_merge(w, (const char *)s + i, j - i, w->content_x + cx,
                                (i32)(j - i), st);
            }
            cx += (i32)(j - i);
            *any_io = true;
            i = j;
        } else {
            u32 pos = i, cp;
            bool ok3 = lw_decode3(s, n, &pos, &cp);
            if (ok3) {
                /* 3バイトグリフランの融合（旧 ifc_try_fit と同じ規則）。
                 * ok3（妥当 3 バイト列）の圏内では wrap_note_direct2 は恒等的に no-op:
                 * cp==U+FFFD となる妥当 3 バイト列は EF BF BD のみであり、それは
                 * direct2 が許容する唯一の置換例そのもの（畳込み省略は同値）。 */
                int gw = lw_glyph_width(cp);
                if (__builtin_expect(gw == 0, 0)) return false;
                u32 rend = pos;
                i32 rw = gw;
                while (rend < n) {
                    /* width=2 確定帯を lead/継続バイトだけで先取り（utf8.h 参照。
                     * 帯は decode3 成功 ∧ gw==2 の部分集合 → 同値。外は従来経路） */
                    if (rend + 2 < n && if_utf8_band_w2(s[rend], s[rend + 1], s[rend + 2])) {
                        rw += 2; rend += 3; continue;
                    }
                    u32 p2 = rend, cp2;
                    if (!lw_decode3(s, n, &p2, &cp2)) break;
                    int gw2 = lw_glyph_width(cp2);
                    if (__builtin_expect(gw2 == 0, 0)) return false;
                    rw += gw2;
                    rend = p2;
                }
                if (pend) {
                    wrap_push_merge(w, (const char *)s + i - 1, (rend - i) + 1,
                                    w->content_x + cx - 1, rw + 1, st);
                    pend = 0;
                } else {
                    wrap_push_merge(w, (const char *)s + i, rend - i, w->content_x + cx, rw, st);
                }
                cx += rw;
                *any_io = true;
                i = rend;
            } else {
                u32 save = i;
                cp = if_utf8_decode(s, n, &save);
                pos = save;
                int gw = lw_glyph_width(cp);
                if (__builtin_expect(gw == 0, 0)) return false;
                wrap_note_direct2(w, s, i, pos, cp);
                if (pend) {
                    wrap_push_merge(w, (const char *)s + i - 1, (pos - i) + 1,
                                    w->content_x + cx - 1, gw + 1, st);
                    pend = 0;
                } else {
                    wrap_push_merge(w, (const char *)s + i, pos - i, w->content_x + cx, gw, st);
                }
                cx += gw;
                *any_io = true;
                i = pos;
            }
        }
        if (__builtin_expect(cx > fd->content_w, 0)) { fd->cx = cx; return false; }
    }
    if (pend) /* 行末空白セル（現行と同じ順番・位置で最後に確定。バイトは s[n-1]） */
        wrap_push_merge(w, (const char *)s + n - 1, 1, w->content_x + cx - 1, 1, st);
    fd->cx = cx;
    return true;
}

/* flatten_into の機械的鏡像。失敗（br/pre/gw==0/溢れ）で false。 */
static bool fitdom_walk(IfFitDom *fd, IfNode *n, const IfStyle *st,
                        float *max_lh_io, bool *any_io) {
    if (n->kind == IF_NODE_TEXT)
        return fitdom_text(fd, n->u.text, st, n->flags, max_lh_io, any_io);
    if (n->kind != IF_NODE_ELEMENT) return true;
    const IfStyle *est;
    if (__builtin_expect(fd->lc->lazy != NULL, 0)) {
        est = lc_st_of(fd->lc, n, st);
        if (est->display == IF_D_NONE) return true; /* flow から除去 */
    } else {
        est = n->style ? n->style : st;
        if (n->style && n->style->display == IF_D_NONE) return true; /* flow から除去 */
    }
    switch (n->tag) {
    case IF_TAG_BR:
        return false; /* br は wrap_end_line が要る従来経路へ */
    case IF_TAG_IMG: {
        IfStr alt = if_dom_attr(n, "alt");
        char buf[1024];
        int m = snprintf(buf, sizeof buf, "[img: %.*s]", (int)(alt.n > 900 ? 900 : alt.n), alt.p ? alt.p : "");
        if (m < 0) m = 0;
        char *s = (char *)if_arena_alloc(fd->lc->arena, (u64)m);
        memcpy(s, buf, (u64)m);
        return fitdom_text(fd, if_str(s, (u32)m), est, 0, max_lh_io, any_io);
    }
    case IF_TAG_A: {
        /* <a> の表示 seg 範囲は「入口位置 — 子 walk 完了位置」で厳密に切る
         * （collect 不発（href 無し）なら記録自体しない: index 不整合を防ぐ） */
        u32 ln0 = fd->lc->n_links;
        u32 s0 = fd->w->n_segs;
        collect_link(fd->lc, n);
        bool rec = !fd->lc->no_boxlink && fd->lc->n_links != ln0;
        for (IfNode *c = n->first_child; c; c = c->next_sibling)
            if (!fitdom_walk(fd, c, est, max_lh_io, any_io)) return false;
        if (rec) {
            if (fd->arec_n + 1 > fd->arec_cap)
                fd->arec = (IfLinkArec *)if_arena_grow(fd->lc->arena, fd->arec, &fd->arec_cap,
                                                       fd->arec_n + 1, sizeof(IfLinkArec));
            fd->arec[fd->arec_n].link = ln0;
            fd->arec[fd->arec_n].seg0 = s0;
            fd->arec[fd->arec_n].seg1 = fd->w->n_segs;
            fd->arec_n++;
        }
        return true;
    }
    default:
        break;
    }
    for (IfNode *c = n->first_child; c; c = c->next_sibling)
        if (!fitdom_walk(fd, c, est, max_lh_io, any_io)) return false;
    return true;
}

/* links[link_idx] に表示矩形を 1 個追記する一点化（fused 単行経路・flatten 複数行
 * 経路の共通終端）。arena は回収不能なので成長は「新割当+コピー」（GUI 規模・
 * 木構築モードのみ。線形 CLI では呼出自体が発生しない） */
static void link_span_add(IfLC *lc, u32 link_idx, i32 x0, i32 y0, i32 x1, i32 y1) {
    /* lc->links が生配列（lay->links は build 末に束ねるため此処では NULL あり得） */
    IfLink *L = &lc->links[link_idx];
    IfLSpan *ns = (IfLSpan *)if_arena_alloc(lc->arena, (u64)(L->n_spans + 1) * sizeof(IfLSpan));
    if (L->n_spans) memcpy(ns, L->spans, (u64)L->n_spans * sizeof(IfLSpan));
    ns[L->n_spans].x0 = x0; ns[L->n_spans].y0 = y0;
    ns[L->n_spans].x1 = x1; ns[L->n_spans].y1 = y1;
    L->spans = ns;
    L->n_spans++;
}

/* <a> の表示矩形を links[] に確定する。call 条件: fused-fit 成功の単行 ifc。
 * seg は fused 経路では ifc 内で単調蓄積する（wrap_end_line は ifc 末の 1 回）
 * ため [seg0, 次 arec.seg0 or 終端) がその <a> の表示範囲。単行ゆえ矩形は高々 1 個。
 * 近似の明示: 界の seg が同 st 合体した場合、矩形は原子 1 個分はみ出しうる
 * （クリック領域がごく僅かに広い。UA のリンク配色で通常不発） */
static void ifc_link_spans(IfLC *lc, const IfWrap *w, const IfLinkArec *arec, u32 nrec,
                           u32 nsegs, i32 y0, i32 rows) {
    if (rows < 1) rows = 1;
    for (u32 r = 0; r < nrec; r++) {
        u32 s0 = arec[r].seg0, s1 = arec[r].seg1;
        if (s1 > nsegs) s1 = nsegs; /* 捕捉契約外の参照を防ぐ防御クランプ */
        if (s1 <= s0) continue;
        i32 x0 = w->seg_base[s0].x, x1 = x0 + w->seg_base[s0].w;
        for (u32 i = s0 + 1; i < s1; i++) {
            if (w->seg_base[i].x < x0) x0 = w->seg_base[i].x;
            i32 e = w->seg_base[i].x + w->seg_base[i].w;
            if (e > x1) x1 = e;
        }
        link_span_add(lc, arec[r].link, x0, y0, x1, y0 + rows);
    }
}

/* flatten 経路の <a> 表示矩形を「piece 区間→確定 seg 添字→行ログ交差」で解決する。
 * call 条件: 従来経路（flatten + wrap 連鎖）の ifc 完了直後（w.seg_hi が ifc 全 seg
 * を畳み込み済み、行ログ [ln_start, lay->n_lines) がこの ifc の行）。
 * 解決規則: 大域 seg 添字 G（ifc 局所）で、open[k]=piece p0 到達時の G、
 * close[k]=piece p1 到達時の G。行 L（seg 大域区間 [G0,G1)）との交差が非空なら
 * 交差 seg の x 和集合を矩形 [G0 行の y, 次行の y) に追記。seg 合体による境界の
 * 原子 1 個はみ出し近似は単行経路と同じ（ifc_link_spans 参照）。 */
static void flat_link_spans(IfLC *lc, const IfWrap *w, const IfLinkPrec *prec, u32 n_prec,
                            u32 ln_start) {
    IfLayout *lay = lc->lay;
    if (!lay || ln_start >= lay->n_lines) return;
    /* 行ログの ifc 起点へ: チャンク累積で O（チャンク数）（GUI 規模で償却） */
    const IfLChunk *ck = lay->lines_head;
    u32 base = 0;
    while (ck && ln_start >= base + ck->n) { base += ck->n; ck = ck->next; }
    if (!ck) return;
    u32 off = ln_start - base;
    u32 G0 = 0;
    for (u32 li = ln_start; li < lay->n_lines && ck; li++) {
        const IfRLine *rl = &ck->v[off];
        u32 G1 = G0 + rl->n_segs;
        /* 次行の y（無ければ ifc 終端 = w->y。行高 rows>1 を含む厳密な矩形下端） */
        const IfRLine *nl = NULL;
        if (li + 1 < lay->n_lines) {
            nl = (off + 1 < ck->n) ? &ck->v[off + 1]
                 : ck->next ? &ck->next->v[0] : NULL;
        }
        i32 y1 = nl ? nl->y : w->y;
        for (u32 k = 0; k < n_prec; k++) {
            u32 s0 = prec[k].open > G0 ? prec[k].open : G0;
            u32 s1 = prec[k].close < G1 ? prec[k].close : G1;
            if (s1 <= s0) continue;
            u32 i0 = s0 - G0, i1 = s1 - G0; /* 行内 seg 添字 */
            i32 x0 = rl->segs[i0].x, x1 = rl->segs[i0].x + rl->segs[i0].w;
            for (u32 i = i0 + 1; i < i1; i++) {
                if (rl->segs[i].x < x0) x0 = rl->segs[i].x;
                i32 e = rl->segs[i].x + rl->segs[i].w;
                if (e > x1) x1 = e;
            }
            link_span_add(lc, prec[k].link, x0, rl->y, x1, y1);
        }
        G0 = G1;
        if (++off == ck->n) { ck = ck->next; off = 0; }
    }
}

/* インライン run の先頭ノードから連続するインラインレベルを IFC に流し込み、
 * run の次のノード（ブロック or NULL）を返す。 */
static IfNode *layout_ifc(IfLC *lc, IfBox *parent, IfNode *cur, const IfStyle *base_st,
                          i32 content_x, i32 *y_io, i32 content_w) {
    u64 _i0; if (lpf()) _i0 = if_rdtsc(); else _i0 = 0;
    IfWrap w = { lc, content_x, content_w, *y_io, parent, NULL, 0, 0, base_st, 1, 0, NULL, NULL };
    float max_lh = base_st && base_st->line_height > 0.0f ? base_st->line_height
                 : base_st ? base_st->font_size * 1.2f : 16.0f * 1.2f;
    bool pre = base_st && base_st->white_space == IF_WS_PRE;

    /* 融合経路: flatten を通さず DOM を直接 wrap へ流す。
     * 成功時は pieces 配列を一切作らない。失敗時は seg 系列を LIFO rewind、
     * line_w/direct/max_lh/links を全復帰して従来経路へ無痕で降りる。 */
    {
        float lh0 = max_lh;
        u32 n_links0 = lc->n_links;
        IfFitDom fd = { &w, lc, base_st, content_w, 0, pre, NULL, 0.0f, NULL, 0, 0 };
        bool any_text = false;
        IfNode *c = cur;
        for (; c; c = c->next_sibling) {
            if (c->kind == IF_NODE_ELEMENT) {
                /* lazy: st をここで解決（値は eager 走査と同値）。eager: 従来の読み方。
                 * style 未適用（--no-style）は従来通りゲート自体が不発。 */
                const IfStyle *cst = lc_st_of(lc, c, base_st);
                if (cst && cst->display == IF_D_NONE) continue;   /* flow から除去 */
                if (cst && cst->display != IF_D_INLINE) break;     /* ブロック子: run 終了 */
            }
            if (!fitdom_walk(&fd, c, base_st, &max_lh, &any_text)) goto fit_fail;
        }
        if (_i0) LPF_FITOK++;
        w.line_w = fd.cx;
        i32 y0 = w.y;
        u32 nsegs0 = w.n_segs; /* wrap_end_line は n_segs を 0 戻しするため先に捕捉 */
        if (w.n_segs > 0 || any_text)
            wrap_end_line(&w, max_lh);
        if (fd.arec_n) ifc_link_spans(lc, &w, fd.arec, fd.arec_n, nsegs0, y0, w.y - y0);
        *y_io = w.y;
        if (_i0) LPF_IFC += if_rdtsc() - _i0;
        return c;
    fit_fail:
        if (_i0) LPF_FITNG++;
        while (w.n_segs > 0) wrap_pop_last_seg(&w);
        w.line_w = 0;
        w.direct_all = 1;
        max_lh = lh0;
        lc->n_links = n_links0;
    }

    /* 従来経路: flatten + piece wrap 連鎖 */
    IfNode *c;
    {
        /* link span 収集は木構築モードのみ（線形 CLI は no_boxlink ゲートで全分岐不発。
         * prec は pieces_scratch と同規約（NULL 始まり・arena grow・ifc 毎 n 0 再利用） */
        IfFlat f = { lc->pieces_scratch, 0, lc, lc->prec_scratch, 0 };
        u64 _f0; if (_i0) _f0 = if_rdtsc(); else _f0 = 0;
        for (c = cur; c; c = c->next_sibling) {
            if (c->kind == IF_NODE_ELEMENT) {
                const IfStyle *cst = lc_st_of(lc, c, base_st);
                if (cst && cst->display == IF_D_NONE) continue;   /* flow から除去 */
                if (cst && cst->display != IF_D_INLINE) break;     /* ブロック子: run 終了 */
            }
            flatten_into(&f, c, base_st);
        }
        if (_f0) { LPF_FLAT += if_rdtsc() - _f0; }

        /* piece 区間 → 確定 seg 添字の写像（prec エントリ自身が台帳。
         * open/close = piece p / p1 到達時の大域 seg 位置 = w.seg_hi + w.n_segs。
         * f.n_prec>0 のときだけ分岐が生きる（線形 CLI・<a> 無し ifc は完全に不発） */
        u32 pc0 = 0;
        u32 ln_start = (f.n_prec && lc->lay) ? lc->lay->n_lines : 0;

        bool any_text = false;
        for (u32 p = 0; p < f.n_pieces; p++) {
            if (__builtin_expect(f.n_prec != 0, 0)) {
                while (pc0 < f.n_prec && f.prec[pc0].p0 == p) {
                    f.prec[pc0].open = w.seg_hi + w.n_segs;
                    pc0++;
                }
                for (u32 k = 0; k < pc0; k++)
                    if (f.prec[k].close == UINT32_MAX && f.prec[k].p1 == p)
                        f.prec[k].close = w.seg_hi + w.n_segs;
            }
            if (f.pieces[p].br) {
                wrap_end_line(&w, max_lh);
                max_lh = base_st && base_st->line_height > 0.0f ? base_st->line_height
                       : base_st ? base_st->font_size * 1.2f : 19.2f;
                continue;
            }
            u64 _w0; if (_i0) _w0 = if_rdtsc(); else _w0 = 0;
            wrap_text(&w, f.pieces[p].text, f.pieces[p].st ? f.pieces[p].st : base_st,
                      (f.pieces[p].st && f.pieces[p].st->white_space == IF_WS_PRE) || pre,
                      &max_lh, &any_text);
            if (_i0) LPF_WRAP += if_rdtsc() - _w0;
        }
        if (w.n_segs > 0 || any_text)
            wrap_end_line(&w, max_lh);
        if (__builtin_expect(pc0 != 0, 0)) {
            /* p1==n_pieces で打ち止められた open は ifc 全末尾で閉じる */
            for (u32 k = 0; k < pc0; k++)
                if (f.prec[k].close == UINT32_MAX) f.prec[k].close = w.seg_hi + w.n_segs;
            flat_link_spans(lc, &w, f.prec, pc0, ln_start);
        }
        lc->pieces_scratch = f.pieces; /* スクラッチを後続 IFC に引き継ぐ（n は毎回 0 リセット） */
        lc->prec_scratch = f.prec;
    }
    *y_io = w.y;
    if (_i0) LPF_IFC += if_rdtsc() - _i0;
    return c;
}

static IfBox *layout_element(IfLC *lc, IfNode *node, const IfStyle *st,
                             i32 ax, i32 ay, i32 avail_w);

/* node の子を走査して配置し、content 高を返す（カーソル前進で O(N)） */
static i32 layout_children(IfLC *lc, IfBox *box, IfNode *node, const IfStyle *base_st,
                           i32 content_x, i32 content_y, i32 content_w) {
    i32 y = content_y;
    i32 prev_mb = 0;
    u32 li_ord = 0; /* ol 番号: この親の LIST_ITEM li を出現順に数える（draw_marker 同値） */
    /* ws 相殺補正は親タグのみの関数 → ループ不変（旧: 子毎に switch 評価） */
    const bool sinkp = lc->md_ws_stripped && ws_sink_parent(node->tag);
    IfNode *c = node->first_child;
    while (c && c != lc->stop) {
        /* sibling 鎖は arena 上で MB 級ストライドに散る → 次ノードを処理中に先読み。
         * c->next_sibling の読み自体は c が常駐のため単一 load（追加ポインタ追従なし） */
        __builtin_prefetch(c->next_sibling, 0, 3);
        /* lazy: ELEMENT の st をここで解決（値は eager 全面走査と同値）。
         * eager: 従来どおり c->style（NULL = style 未適用 = 判定不発） */
        const IfStyle *cst = (c->kind == IF_NODE_ELEMENT) ? lc_st_of(lc, c, base_st) : NULL;
        if (cst && cst->display == IF_D_NONE) {
            c = c->next_sibling;
            continue;
        }
        bool blockish = false;
        if (cst)
            blockish = cst->display == IF_D_BLOCK || cst->display == IF_D_LIST_ITEM;
        if (!blockish) {
            c = layout_ifc(lc, box, c, base_st, content_x, &y, content_w);
            prev_mb = 0;
            continue;
        }
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
        IfBox *child = layout_element(lc, c, cst, content_x, y, content_w);
        box_add_child(lc, box, child);
        i32 child_h = child->h;
        box_recycle(lc, child); /* 線形モード以外では no-op */
        y += child_h;
                prev_mb = mb;
        if (sinkp)
            prev_mb = 0; /* 旧 DOM では sink 容器直下の ws TEXT が ifc 経由で必ず
                          * prev_mb を 0 にしていた → 剥がし後の同値補正（md.c 参照） */
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

static IfBox *layout_element(IfLC *lc, IfNode *node, const IfStyle *st,
                             i32 ax, i32 ay, i32 avail_w) {
    u64 _s0, _acc; if (lpf()) { _s0 = if_rdtsc(); _acc = 0; } else { _s0 = 0; _acc = 0; }
    /* st は呼出側で解決済み（ELEMENT の lazy 解決は layout_children ゲート 1 回だけ。
     * 防御: NULL は style 未適用とみなし従来 FALLBACK） */
    if (__builtin_expect(!st, 0)) st = &IF_STYLE_FALLBACK;
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
        if (_s0) LPF_ELEM += _acc + (if_rdtsc() - _s0);
        return box;
    }

    if (lc->lay && (bl | brd | bt | bbo)) {
        u8 sides = (u8)((bt ? 1 : 0) | (brd ? 2 : 0) | (bbo ? 4 : 0) | (bl ? 8 : 0));
        deco_bd = deco_add(lc, IF_DECO_BORDER, x, y, bl + pad_l + content_w + pad_r + brd,
                           0, st->border_color, NULL, NULL, sides);
    }

    frame_push(lc, box); /* box の子追加は frame_top==box で O(1) tail を引く */
    if (_s0) _acc += if_rdtsc() - _s0;
    i32 content_h = layout_children(lc, box, node, st, content_x, content_y, content_w);
    if (_s0) _s0 = if_rdtsc();
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
    if (_s0) LPF_ELEM += _acc + (if_rdtsc() - _s0);
    return box;
}

/* ---- 2-way 並列 layout（md fast-DOM 限定） ----
 * body 直下の子を 2 分割し、各スレッドが独立に y=content_y から敷き、join で B 側の
 * 全 y を H_A シフトして連結する。
 * 同値証明の骨格:
 *  - body 直下の兄弟マージン相殺は md_ws_stripped DOM では ws_sink 補正により常に
 *    prev_mb=0（layout_children 参照）→ 分割点で必要な境界補正は 0、H_A 加算で厳密一致。
 *  - 各子に必要な包含コンテキストは body 由来の不変量（content_x/w・body style）のみ。
 *  - IfBox に親ポインタは存在しない（接合は first_child/next_sibling の継ぎ目のみで完結）。
 *  - lines/deco ログは生成順=y 単調非減少 → H_A で区間を分離すれば連結で統合できる。
 *  - links は A 側の終番から B 側を再採番（collect 順=文書順で同値）。
 * IfNode は style 適用後で読み取り専用、LC・geom・lay・links は全て shard ローカル。 */
typedef struct IfLayShard {
    IfArena *arena;
    IfNode vbody;          /* 実 body の浅い複製（first_child だけ差し替え） */
    IfNode *stop;
    const IfStyle *body_st;
    i32 content_x, content_y, content_w;
    i32 bx, by, box_w;     /* body 幾とう（layout_element の計算をドライバが再現） */
    u8 bsides;
    i32 width_cells;
    u8 md_ws_stripped;
    u8 no_boxlink;
    u8 lazy_style;       /* lazy style 有効（ctx は arena 局所に begin する） */
    float lazy_rfs;      /* html 由来 rfs（shard 間で同値） */
    /* out */
    IfLayout *lay;
    i32 content_h;
    IfBox *vroot;
    u32 deco_bg, deco_bd;
} IfLayShard;

static void layout_shard_run_body(IfLayShard *s) {
    IfArena *arena = s->arena;
    IfLayout *lay = (IfLayout *)if_arena_calloc(arena, sizeof(IfLayout));
    lay->arena = arena;
    lay->width = s->width_cells;
    IfLC lc = { .arena = arena, .root_fs = 16.0f };
    IfStyleLazy lz;      /* 使用時のみ begin（shard 専用 ctx。arena はこの shard 専有） */
    if (s->lazy_style) {
        if_style_lazy_init(&lz, arena);
        lc.lazy = &lz;
        lc.lazy_rfs = s->lazy_rfs;
    }
    lc.geom = (IfGeomCache *)if_arena_calloc(arena, sizeof(IfGeomCache));
    lc.lay = lay;
    lc.md_ws_stripped = s->md_ws_stripped;
    lc.no_boxlink = s->no_boxlink;
    lc.stop = s->stop;
    IfBox *vroot = new_box(&lc, IF_BOX_BLOCK, NULL, s->body_st);
    lay->root = vroot;
    /* body 自身の装飾は paint 順で最初（shard A のみ担当。h は join 後に総量で後埋め） */
    s->deco_bg = UINT32_MAX;
    s->deco_bd = UINT32_MAX;
    if (s->bsides & 0x80) {
        if (lc.lay) s->deco_bg = deco_add(&lc, IF_DECO_BG, s->bx, s->by, s->box_w, 0,
                                          s->body_st->bg, NULL, NULL, 0);
    }
    if (s->bsides & 0x0F) {
        if (lc.lay) s->deco_bd = deco_add(&lc, IF_DECO_BORDER, s->bx, s->by, s->box_w, 0,
                                          s->body_st->border_color, NULL, NULL,
                                          (u8)(s->bsides & 0x0F));
    }
    frame_push(&lc, vroot); /* box_add_child の O(1) tail 経路（layout_element と同じ約款） */
    i32 h = layout_children(&lc, vroot, &s->vbody, s->body_st,
                            s->content_x, s->content_y, s->content_w);
    frame_pop(&lc, vroot);
    s->lay = lay;
    s->content_h = h;
    s->vroot = vroot;
    lay->links = lc.links;
    lay->n_links = lc.n_links;
}

static void *layout_shard_thread(void *arg) { layout_shard_run_body(arg); return NULL; }

static void shift_tree(IfBox *b, i32 dy) {
    for (IfBox *c = b->first_child; c; c = c->next_sibling) {
        c->y += dy;
        shift_tree(c, dy);
    }
}

static IfLayout *build_impl(IfArena *arena, IfDom *dom, i32 width_cells, u8 linear,
                            u8 lazy_style) {
    if (width_cells < 4) width_cells = 4;
    IfLayout *lay = (IfLayout *)if_arena_calloc(arena, sizeof(IfLayout));
    IfStyleLazy lz0;         /* lazy の親走査 ctx（並列時は各 shard が別 ctx を持つ） */
    IfLC lc = { .arena = arena, .root_fs = 16.0f };
    if (lazy_style) { if_style_lazy_init(&lz0, arena); lc.lazy = &lz0; lc.lazy_rfs = 16.0f; }
    lc.geom = (IfGeomCache *)if_arena_calloc(arena, sizeof(IfGeomCache));
    lc.lay = lay;
    lc.no_boxlink = linear;
    lc.md_ws_stripped = dom->md_ws_stripped;
    lay->arena = arena;
    lay->width = width_cells;
    lay->root = new_box(&lc, IF_BOX_BLOCK, NULL, NULL);

    IfNode *html = NULL;
    IfNode *body = NULL;
    for (IfNode *c = dom->root->first_child; c && !body; c = c->next_sibling)
        if (c->kind == IF_NODE_ELEMENT && c->tag == IF_TAG_HTML)
            for (IfNode *g = c->first_child; g; g = g->next_sibling)
                if (g->kind == IF_NODE_ELEMENT && g->tag == IF_TAG_BODY) { html = c; body = g; break; }
    if (!body) return lay;

    /* lazy: html→body の 2 要素だけ先に解決し、以降は DFS 訪問時に各所で解決する。
     * eager: 従来どおり style 適用済みの node->style を読む。ietf な両経路で bst の
     * 値は一致する（compute_walk の迷子: html の parent=NULL/rfs=16、body の
     * parent=html_st/rfs=html font_size） */
    const IfStyle *html_st = NULL;
    const IfStyle *bst;
    if (lazy_style) {
        html_st = if_style_lazy_get(&lz0, html, NULL, 16.0f);
        lc.lazy_rfs = html_st->font_size;   /* compute_walk: HTML 直下で rfs 確定 */
        bst = if_style_lazy_get(&lz0, body, html_st, lc.lazy_rfs);
    } else {
        bst = body->style ? body->style : &IF_STYLE_FALLBACK;
    }
    float body_fs = bst->font_size;
    i32 body_mt = len_v(bst->margin[0], body_fs, 16.0f, width_cells);

    /* 並列可否: md fast-DOM（md_ws_stripped）かつ body の直下子が十分に多いこと。
     * md 2-slice パースは分割境界の子ポインタ（md_body_mid = byte 半分境界）を既知
     * として渡す → 大文書では直下子の全計数・中点ウォーク（どちらも DRAM ミス鎖の
     * 逐次ポインタチェイス: 16MB IDM で ~131k 遷移 ~31ms 実測）を構造消去する。
     * ヒント非所持（serial md / HTML / 小文書）は従来の計数経路。ゲートのみの差で
     * 2 経路の生出力は oracle/テストが固定する同値関係にある。 */
    u32 nch = 0;
    IfNode *mid_hint = NULL;
    const char *ep = dom->md_ws_stripped ? getenv("IF_LAYOUT_PAR") : NULL;
    if (dom->md_ws_stripped && !(ep && ep[0] == '0')) {
        mid_hint = dom->md_body_mid;
        if (mid_hint) nch = (dom->n_nodes >= 4096) ? 64 : 0;
        else for (IfNode *c = body->first_child; c; c = c->next_sibling) nch++;
    }
    if (nch >= 64) {
        /* body 自身の幾何を layout_element と同じ手順で先に確定する（bst は上で解決済） */
        const IfGeomEnt *bg = geom_get(&lc, lc.geom, bst, width_cells);
        i32 bl = bg->bl, brd = bg->brd, bt = bg->bt, bbo = bg->bbo;
        i32 pad_l = bg->pl, pad_r = bg->pr, pad_t = bg->pt, pad_b = bg->pb;
        i32 content_w = bg->content_w;
        i32 bx = 0 + bg->ml;
        i32 by = body_mt;
        i32 content_x = bx + bl + pad_l;
        i32 content_y = by + bt + pad_t;
        i32 box_w = bl + pad_l + content_w + pad_r + brd;

        /* ヒントがあれば中点ウォーク自体を消去（A 範囲は [first_child, hint)） */
        IfNode *mid = mid_hint;
        if (!mid) {
            mid = body->first_child;
            for (u32 k = 0; k < nch / 2; k++) mid = mid->next_sibling;
        }

        IfArena ab;
        if_arena_init(&ab, 1u << 23);
        IfLayShard sa, sb;
        memset(&sa, 0, sizeof sa);
        memset(&sb, 0, sizeof sb);
        u8 bsides = (u8)(((bst->bg & 0xFF) >= 128 ? 0x80 : 0) |
                         ((bt ? 1 : 0) | (brd ? 2 : 0) | (bbo ? 4 : 0) | (bl ? 8 : 0)));
        sa.arena = arena; sa.vbody = *body; sa.stop = mid;
        sa.body_st = bst; sa.content_x = content_x; sa.content_y = content_y;
        sa.content_w = content_w; sa.bx = bx; sa.by = by; sa.box_w = box_w;
        sa.bsides = bsides;  /* 実 body の装飾は A が担当 */
        sa.width_cells = width_cells; sa.md_ws_stripped = 1;
        sa.no_boxlink = linear;
        sa.lazy_style = lazy_style; sa.lazy_rfs = lc.lazy_rfs;
        sb.arena = &ab; sb.vbody = *body; sb.vbody.first_child = mid; sb.stop = NULL;
        sb.body_st = bst; sb.content_x = content_x; sb.content_y = content_y;
        sb.content_w = content_w; sb.bx = bx; sb.by = by; sb.box_w = box_w;
        sb.bsides = 0;       /* B は body 装飾を出さない（A のみ） */
        sb.width_cells = width_cells; sb.md_ws_stripped = 1;
        sb.no_boxlink = linear;
        sb.lazy_style = lazy_style; sb.lazy_rfs = lc.lazy_rfs;

        pthread_t th;
        int rc = pthread_create(&th, NULL, layout_shard_thread, &sb);
        layout_shard_run_body(&sa);
        if (rc == 0) pthread_join(th, NULL);
        else layout_shard_run_body(&sb); /* 生成失敗時は同じ分割を直列実行（結果は同値） */

        i32 hA = sa.content_h, hB = sb.content_h;
        i32 content_h = hA + hB;
        if (bg->height_spec >= 0 && bg->height_spec > content_h)
            content_h = bg->height_spec; /* 指定高はクリップせず拡張のみ（layout_element 同値） */

        /* B の全 y を H_A シフト（lines は DFS で確実に捕捉、deco は配列で）。
         * 線形モードは box 木未接続のため lines[] 配列を直接シフト（同じ対象・同じ量） */
        if (linear) {
            for (IfLChunk *ck = sb.lay->lines_head; ck; ck = ck->next)
                for (u32 i = 0; i < ck->n; i++) ck->v[i].y += hA;
        } else {
            shift_tree(sb.vroot, hA);
        }
        for (u32 i = 0; i < sb.lay->n_deco; i++) sb.lay->deco[i].y += hA;

        IfLayout *lay2 = sa.lay;
        /* 実 body box（layout_element と同一公式） */
        IfBox *root = new_box(&lc, IF_BOX_BLOCK, body, bst);
        root->x = bx;
        root->y = by;
        root->w = box_w;
        root->h = bt + pad_t + content_h + pad_b + bbo;
        /* 子列の接合: A 末尾 → B 先頭（vroot は破棄） */
        root->first_child = sa.vroot->first_child;
        IfBox *last = root->first_child;
        if (last) { while (last->next_sibling) last = last->next_sibling; }
        if (last) last->next_sibling = sb.vroot->first_child;
        else root->first_child = sb.vroot->first_child;
        lay2->root = root;
        lay2->height = root->y + root->h;
        /* body 装飾 h の後埋め（layout_element と同値の最終 h） */
        if (sa.deco_bg != UINT32_MAX) lay2->deco[sa.deco_bg].h = root->h;
        if (sa.deco_bd != UINT32_MAX) lay2->deco[sa.deco_bd].h = root->h;
        /* lines/deco 連結（単調区間が交差しないため memcpy 連結で統合） */
        /* lines 連結はチャンク列のポインタ接続（A 末尾→B 先頭。コピー・アドレス
         * 再計算ともに不要: チャンクが値所有のため参照がそのまま生きる） */
        if (sb.lay->lines_head) {
            if (sa.lay->lines_tail) sa.lay->lines_tail->next = sb.lay->lines_head;
            else sa.lay->lines_head = sb.lay->lines_head;
            sa.lay->lines_tail = sb.lay->lines_tail;
        }
        lay2->lines_head = sa.lay->lines_head;
        lay2->lines_tail = sa.lay->lines_tail;
        lay2->n_lines = sa.lay->n_lines + sb.lay->n_lines;
        u32 nd = sa.lay->n_deco + sb.lay->n_deco;
        IfDeco *darr = (IfDeco *)if_arena_alloc(arena, (u64)(nd ? nd : 1) * sizeof(IfDeco));
        /* n==0 では src が未確保 NULL になり得る → memcpy(NULL,0) は厳密 UB（nonnull 制約）。
         * 0 件コピー自体を呼ばない構造にして UBSan runtime note を消す（byte-exact 不変） */
        if (sa.lay->n_deco) memcpy(darr, sa.lay->deco, (u64)sa.lay->n_deco * sizeof(IfDeco));
        if (sb.lay->n_deco) memcpy(darr + sa.lay->n_deco, sb.lay->deco, (u64)sb.lay->n_deco * sizeof(IfDeco));
        lay2->deco = darr;
        lay2->n_deco = nd;
        lay2->cap_deco = nd;
        /* links: B を A の終番から再採番して連結（文書順同値）。
         * B の span 矩形は shard 局所 y のままでは lines/deco/box と整合しない
         * （B の全 y を hA シフトするのと同じ対象・同じ量）→ ここで一体補正する。
         * 線形 CLI では span 未収集（n_spans==0）のため内側ループは常に空 */
        u32 nlk = sa.lay->n_links + sb.lay->n_links;
        IfLink *lk = (IfLink *)if_arena_alloc(arena, (u64)(nlk ? nlk : 1) * sizeof(IfLink));
        if (sa.lay->n_links) memcpy(lk, sa.lay->links, (u64)sa.lay->n_links * sizeof(IfLink)); /* deco 同型の UB ガード */
        for (u32 i = 0; i < sb.lay->n_links; i++) {
            lk[sa.lay->n_links + i] = sb.lay->links[i];
            lk[sa.lay->n_links + i].n = sa.lay->n_links + i + 1;
            IfLink *L = &lk[sa.lay->n_links + i];
            for (u32 k = 0; k < L->n_spans; k++) { L->spans[k].y0 += hA; L->spans[k].y1 += hA; }
        }
        lay2->links = lk;
        lay2->n_links = nlk;
        lay2->arena = arena;
        if_arena_absorb(arena, &ab);
        if_arena_destroy(&ab);
        return lay2;
    }

    IfBox *root = layout_element(&lc, body, bst, 0, body_mt, width_cells);
    lay->root = root;
    lay->height = root->y + root->h;
    lay->links = lc.links;
    lay->n_links = lc.n_links;
    return lay;
}

IfLayout *if_layout_build(IfArena *arena, IfDom *dom, i32 width_cells) {
    return build_impl(arena, dom, width_cells, 0, 0);
}

/* 線形モード入口（CLI 行スイープ専用）: 幾何・lines/deco/links の全出力が従来経路と
 * 同値であることを oracle/idm・golden・tests がロックする。kill switch: IF_LAYOUT_LIN=0 */
IfLayout *if_layout_build_linear(IfArena *arena, IfDom *dom, i32 width_cells, u8 lazy_style) {
    const char *e = getenv("IF_LAYOUT_LIN");
    u8 linear = !(e && e[0] == '0');
    /* lazy は線形モードのときだけ有効（木構築は node->style 読みの従来路を維持） */
    return build_impl(arena, dom, width_cells, linear, linear ? lazy_style : 0);
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
