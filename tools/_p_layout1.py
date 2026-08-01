import io
s = io.open('src/layout.c', encoding='utf-8').read()

def rep(old, new, cnt=1):
    global s
    assert old in s, "ANCHOR MISSING: " + old[:70]
    s = s.replace(old, new, cnt)

# ---- IfLC: segs_scratch は直接確定に置き換わるので跡地化（利用は全消去） ----
rep('''    /* IFC/折り返しスクラッチ: pieces/segs 配列は「行・run 内で消費が完結」するので再利用する。
     * （arena は free しない。使い回せる一時配列を毎回確定させると大文書で数十 MB 無駄になる——実測済み） */
    IfPiece *pieces_scratch;
    u64 pieces_scratch_cap;
    IfSeg *segs_scratch;
    u64 segs_scratch_cap;''','''    /* IFC/折り返しスクラッチ: pieces 配列は「run 内で消費が完結」するので再利用する。
     * （segs は 2026-08-01 に直接 arena 確定へ移行: LINE ごとの alloc+memcpy 確定を消去。
     *   巻き戻し可能な pop のみを許し、スクラッチ二重書きは構造排除） */
    IfPiece *pieces_scratch;
    u64 pieces_scratch_cap;''')

# ---- IfWrap: seg_base 直接確定 ----
rep('''typedef struct {
    IfLC *lc;
    i32 content_x, content_w;
    i32 y;
    IfBox *parent;
    IfSeg *segs;           /* == lc->segs_scratch（共有スクラッチ、n_segs は現在行の分） */
    u32 n_segs;
    i32 line_w;
    const IfStyle *align_st;
    u8 direct_all;         /* 現行の全グリフが IF_LF_DIRECT_BYTES 条件を満たす */
} IfWrap;''','''typedef struct {
    IfLC *lc;
    i32 content_x, content_w;
    i32 y;
    IfBox *parent;
    IfSeg *seg_base;       /* 現行の seg 先頭（arena 直接確定。LINE ごとのコピー不在） */
    u32 n_segs;
    i32 line_w;
    const IfStyle *align_st;
    u8 direct_all;         /* 現行の全グリフが IF_LF_DIRECT_BYTES 条件を満たす */
} IfWrap;''')

rep('''/* 直前 seg と style が同じでソース上連続なら拡張する合体 push（seg 爆発の構造消去。
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
}''','''/* 直前 seg と style が同じでソース上連続なら拡張する合体 push（seg 爆発の構造消去。
 * レンダリングされるセル列は変わらない（同じバイト・同じ x・同じ style）） */
static void wrap_push_seg(IfWrap *w, IfStr text, i32 x, i32 width, const IfStyle *st);
static inline void wrap_push_merge(IfWrap *w, const char *p, u32 n, i32 x, i32 width, const IfStyle *st) {
    if (n == 0) return;
    if (w->n_segs) {
        IfSeg *last = &w->seg_base[w->n_segs - 1];
        if (last->st == st && last->text.p + last->text.n == p) {
            last->text.n += n;
            last->w += width;
            return;
        }
    }
    wrap_push_seg(w, if_str(p, n), x, width, st);
}

static inline void wrap_push_seg(IfWrap *w, IfStr text, i32 x, i32 width, const IfStyle *st) {
    if (text.n == 0) return;
    /* 直接確定（arena bump）。行末確定時の alloc+memcpy を消す。
     * wrap フェーズ中の領域確保は seg 系列だけなので、後続の pop は rewind_last で畳める。 */
    IfSeg *s = (IfSeg *)if_arena_bump(w->lc->arena, sizeof(IfSeg));
    if (w->n_segs == 0) w->seg_base = s;
    s->text = text; s->x = x; s->w = width; s->st = st;
    w->n_segs++;
}

/* 行尾の折り畳み空白 pop（巻き戻し可能なのは seg 系列が最新端のときだけ） */
static inline void wrap_pop_last_seg(IfWrap *w) {
    IfSeg *last = &w->seg_base[w->n_segs - 1];
    if_arena_rewind_last(w->lc->arena, last, sizeof(IfSeg));
    w->n_segs--;
}''')

# ---- wrap_end_line: seg コピー撤廃 + px2row fast path + lines[] inline append ----
rep('''static void wrap_end_line(IfWrap *w, float max_lh) {
    u64 _e0; if (lpf()) _e0 = if_rdtsc(); else _e0 = 0;
    i32 rows = px2row(max_lh);
    if (rows < 1) rows = 1;''','''static void wrap_end_line(IfWrap *w, float max_lh) {
    u64 _e0; if (lpf()) _e0 = if_rdtsc(); else _e0 = 0;
    /* px2row(x)==1 ⇔ 8<=x<24（floor(x/16+.5) の区間同値。頻出 lh=19.2 を 1 分岐で抜ける） */
    i32 rows = (max_lh >= 8.0f && max_lh < 24.0f) ? 1 : px2row(max_lh);
    if (rows < 1) rows = 1;''')

rep('''    IfBox *line = new_box(w->lc, IF_BOX_LINE, NULL, w->align_st);
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
    }''','''    IfBox *line = new_box(w->lc, IF_BOX_LINE, NULL, w->align_st);
    line->x = w->content_x;
    line->y = w->y;
    line->w = w->line_w;
    line->h = rows;
    line->text_align = align;
    /* seg は arena 直接確定済み（無コピー）。align シフトだけ確定時に適用 */
    if (w->n_segs) {
        line->segs = w->seg_base;
        line->n_segs = w->n_segs;
        if (shift)
            for (u32 i = 0; i < line->n_segs; i++) line->segs[i].x += shift;
    }''')

rep('''    /* 行スイープ直接発行用のログ（生成順 = y 単調非減少） */
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
    if (_e0) LPF_ENDL += if_rdtsc() - _e0;
}''','''    /* 行スイープ直接発行用のログ（生成順 = y 単調非減少） */
    {
        IfLC *lc2 = w->lc;
        IfLayout *lay = lc2->lay;
        if (lay) {
            if (__builtin_expect(lay->n_lines < lay->cap_lines, 1)) {
                lay->lines[lay->n_lines++] = line;
            } else {
                lay->lines = (IfBox **)if_arena_grow(lc2->arena, lay->lines,
                    &lay->cap_lines, lay->n_lines + 1, sizeof(IfBox *));
                lay->lines[lay->n_lines++] = line;
            }
        }
    }
    w->y += rows;
    w->n_segs = 0;
    if (_e0) LPF_ENDL += if_rdtsc() - _e0;
}''')

# ---- 行尾空白 pop の呼出を巻き戻し版に ----
rep('''            if (w->n_segs > 0) {
                IfSeg *last = &w->segs[w->n_segs - 1];
                if (last->text.n && last->text.p[last->text.n - 1] == ' ') {
                    if (last->text.n == 1) w->n_segs--;
                    else { last->text.n--; last->w--; }
                }
            }
            cx = w->n_segs > 0 ? w->segs[w->n_segs - 1].x + w->segs[w->n_segs - 1].w - w->content_x : 0;''','''            if (w->n_segs > 0) {
                IfSeg *last = &w->seg_base[w->n_segs - 1];
                if (last->text.n && last->text.p[last->text.n - 1] == ' ') {
                    if (last->text.n == 1) wrap_pop_last_seg(w);
                    else { last->text.n--; last->w--; }
                }
            }
            cx = w->n_segs > 0 ? w->seg_base[w->n_segs - 1].x + w->seg_base[w->n_segs - 1].w - w->content_x : 0;''')

# ---- IfWrap 初期化と seg scratch 参照の消去 ----
rep('''    IfWrap w = { lc, content_x, content_w, *y_io, parent, lc->segs_scratch, 0, 0, base_st, 1 };''','''    IfWrap w = { lc, content_x, content_w, *y_io, parent, NULL, 0, 0, base_st, 1 };''')

# ---- pre 空白の合体（' ' のみ: セル列同値。\t/制御は幅規則が違うので合体しない） ----
rep('''            if (pre) {
                i32 adv = (b0 == '\\t') ? 8 : 1;
                /* ' '(0x20) のみセル列とバイト列が一致（\\t は前進 8、他は gw==0 で非発行セル） */
                if (b0 != ' ') w->direct_all = 0;
                wrap_push_seg(w, if_str((const char *)s + i, 1), w->content_x + cx, adv, st);
                cx += adv; i++; *any = true; continue;
            }''','''            if (pre) {
                i32 adv = (b0 == '\\t') ? 8 : 1;
                /* ' '(0x20) のみセル列とバイト列が一致（\\t は前進 8、他は gw==0 で非発行セル） */
                if (b0 != ' ') {
                    w->direct_all = 0;
                    wrap_push_seg(w, if_str((const char *)s + i, 1), w->content_x + cx, adv, st);
                } else {
                    /* 連続バイト・同 style → 先行 seg へ合体（pre 内のスペース嵐を消す） */
                    wrap_push_merge(w, (const char *)s + i, 1, w->content_x + cx, 1, st);
                }
                cx += adv; i++; *any = true; continue;
            }''')

# ---- flat_push inline ----
rep('''static void flat_push(IfFlat *f, IfStr text, const IfStyle *st, u8 br) {
    f->pieces = (IfPiece *)if_arena_grow(f->lc->arena, f->pieces, &f->lc->pieces_scratch_cap,
                                         f->n_pieces + 1, sizeof(IfPiece));
    /* if_arena_grow のコピー時に f->pieces も cap も lc 側に一元反映される */
    f->pieces[f->n_pieces].text = text;
    f->pieces[f->n_pieces].st = st;
    f->pieces[f->n_pieces].br = br;
    f->n_pieces++;
}''','''static void flat_push_grow(IfFlat *f, IfStr text, const IfStyle *st, u8 br);
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
}''')

# ---- 高頻度ワイドレンジの先出し幅判定（同値: 排他的レンジ一致） ----
rep('''/* テキスト 1 ピースを流し込む。*any は実グリフが存在したか（空白のみの run で空行を防ぐ）。 */''','''/* 高頻度ワイドレンジ先出し（互いに排他的なレンジ一致で if_glyph_width と同値） */
static inline int lw_glyph_width(u32 cp) {
    if (cp >= 0x3041 && cp <= 0x33FF) return 2; /* ひらがな・カタカナ・CJK 記号 */
    if (cp >= 0x4E00 && cp <= 0x9FFF) return 2; /* CJK 統合漢字 */
    return if_glyph_width(cp);
}

/* テキスト 1 ピースを流し込む。*any は実グリフが存在したか（空白のみの run で空行を防ぐ）。 */''')

s = s.replace('int gw = if_glyph_width(cp);\n            wrap_note_direct', 'int gw = lw_glyph_width(cp);\n            wrap_note_direct')
s = s.replace('int gw2 = if_glyph_width(cp2);\n                    wrap_note_direct', 'int gw2 = lw_glyph_width(cp2);\n                    wrap_note_direct')
s = s.replace('int gw3 = if_glyph_width(cp3);\n                wrap_note_direct', 'int gw3 = lw_glyph_width(cp3);\n                wrap_note_direct')

io.open('src/layout.c','w',encoding='utf-8').write(s)
print("patched ok")
