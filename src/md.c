/* Ifuto — Markdown 処理層 v0.3（C11, no deps）
 *
 * 2 つの backend を同じ emitter から駆動する（sink 二層化）:
 *   1. HTML 文字列 backend（従来 API if_md_to_html。決定的出力でテストが文字列一致）
 *   2. DOM 直構築 backend（if_md_parse。HTML 往復を消す高速経路）
 *
 * DOM 直構築の正当性（速さの代償に正しさを払わない規約）:
 *   この emitter が生成しうる tag stream は閉集合・開始/終了が常に対応・void は
 *   hr/img/br のみ・属性値と本文は常に escape される。したがって WHATWG 本パーサは
 *   この流れに対して単純スタック push/pop と同一の木を生成する（implied end / AAA
 *   / foster / adoption は構造上到達しない）。到達しうる経路だけは taint 検出して
 *   文書全体を本パーサ経路へフォールバックする（稀・敵対入力のみ）:
 *     T1. escape リテラルが '<' / '&'（文字列出力が文法外になり本パーサの意味が変わる）
 *     T2. <a> を open 中に更に <a> を open（adoption agency が発火し木が変わる）
 *     T3. ネスト上限超過 / close 不一致（内部不変条件の破壊）
 *     T4. 入力 NUL（トークナイザの NUL 処理が分岐する。呼び出し側で先読み検出）
 *     T5. ノード数上限（IF_MAX_DOM_NODES。本パーサの打ち切り規約に委ねる）
 *   差分オラクル: テストが string backend の厳密バイト列を知り、--dump-dom 比較と
 *   差分 fuzz（tests/md_diff）が fast DOM ≡ string+本パーサ を機械固定する。
 *
 * 速度: 行分割は memchr、inline 通常ランは SIMD(SSE2) 一括スキャン、text node は
 * 入力のゼロコピースライス（persistent 範囲外だけ arena 複製）。dispatch は密 switch
 * でコンパイラの jump table に委ねる（計算 goto 同等の threading を -O2 が生成済み。
 * 計測: 特殊文字は SIMD 側が先に拾うので dispatch は特殊1文字あたり1回）。
 *
 * メモリ則: 既存どおり入力は 1 回の線形走査、行はポインタ切片。脚注とバッファ
 * 規約は backend ごとに寿命管理（string: malloc/realloc で消費後解放、
 * dom: 借用参照の寿命のため arena 恒久化）。破損入力で無限ループしない
 * （各行処理は必ず前行消費、引用 depth ≤ 8、リスト入れ子は 16 段飽和は維持）。
 */
#include "md.h"
#include "dom.h"
#include "strutil.h"
#include "utf8.h" /* if_utf8_band_w2（TEXT 内容分類 CJK3W2 判定の唯一の定義） */
#include <stdio.h>
#include <string.h>
#include <stdlib.h> /* realloc/free（string backend のステージング解放規約） */
#include <pthread.h> /* 2-way 並列 fast parse（glibc>=2.34 で libc 内。ldd 不変） */

#if defined(__SSE2__) || (defined(__x86_64__) && !defined(IFUTO_NO_SIMD))
#define IF_MD_SIMD 1
#include <emmintrin.h>
#endif

/* ================= 出力 sink ================= */

typedef struct { IfArena *a; char *p; u64 n, cap; } B; /* string staging */

static void b_init(B *b, IfArena *a) { b->a = a; b->p = NULL; b->n = 0; b->cap = 0; }
static void b_putn(B *b, const char *s, u64 n) {
    if (b->n + n + 1 > b->cap) {
        u64 nc = b->cap ? b->cap : 256;
        while (nc < b->n + n + 1) nc *= 2;
        char *np = (char *)realloc(b->p, nc ? nc : 1);
        if (!np) if_fatal("md: staging oom");
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, s, (size_t)n);
    b->n += n; b->p[b->n] = 0;
}
static void b_drop(B *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }
static IfStr b_finish(B *b) {
    IfArena *a = b->a;
    char *np = (char *)if_arena_alloc(a, b->n + 1);
    if (b->n) memcpy(np, b->p, b->n);
    np[b->n] = 0;
    IfStr r = if_str(np, (u32)b->n);
    b_drop(b);
    return r;
}
static void b_puts(B *b, const char *s) { b_putn(b, s, (u64)strlen(s)); }
static void b_putc(B *b, char c) { b_putn(b, &c, 1); }

/* ---- sink 本体 ----
 * dom モードの text run: 借用モード（persistent 範囲内の連続切片）と複製モードを
 * 自動切替する。範囲は最大 16 本（入力 or norm + 引用/フラット化の arena コピー）。 */
typedef struct {
    IfArena *a;
    bool is_dom;
    bool tainted;
    B str;
    /* dom backend */
    IfDom *dom;
    IfNode *cur;
    IfNode *stk[128];
    int sp;
    const char *rng_p[16];
    u32 rng_n[16];
    int n_rng;
    /* text run accumulator */
    const char *b_p;      /* 借用アンカー */
    u32 b_n;
    char *c_buf;          /* 複製バッファ（heap。flush ごとに使い回し） */
    u32 c_n, c_cap;
    u8 mode;              /* 0=empty 1=borrow 2=copy */
    u32 n_nodes;          /* ノード数のローカル計数（並列時はスレッド別。join で合算） */
    u8 slim_attrs;        /* IF_MD_F_SLIM_ATTRS: 保持属性を A[href]/IMG[alt] に限定 */
    /* node slab: 生成は直列 bump（ノード単位の arena 呼出を slab refill に畳む） */
    IfNode *nslab, *nslab_end;
} Mo;

static void mo_taint(Mo *m) { m->tainted = true; }

/* ---- 開発用 rdtsc ゾーン計測（IF_MD_PROF=1 のときのみ活性） ---- */
#if defined(__x86_64__) || defined(__i386__)
static inline u64 if_rdtsc_md(void) { u32 lo, hi; __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi)); return ((u64)hi << 32) | lo; }
#else
static inline u64 if_rdtsc_md(void) { return 0; }
#endif
static int if_mp_on = -2;
static u64 MP_NORM, MP_LINES, MP_BLOCKS, MP_INLINE, MP_MNEW, MP_FLUSH;
__attribute__((destructor)) static void mpf_dump(void) {
    if (if_mp_on > 0)
        fprintf(stderr, "MDPROF norm=%llu lines=%llu blocks=%llu inline=%llu mnew=%llu flush=%llu (cycles)\n",
                (unsigned long long)MP_NORM, (unsigned long long)MP_LINES, (unsigned long long)MP_BLOCKS,
                (unsigned long long)MP_INLINE, (unsigned long long)MP_MNEW, (unsigned long long)MP_FLUSH);
}
static inline bool mpf(void) {
    if (if_mp_on == -2) { const char *e = getenv("IF_MD_PROF"); if_mp_on = (e && e[0] == '1') ? 1 : 0; }
    return if_mp_on > 0;
}

static void mo_range(Mo *m, const char *p, u32 n) {
    if (!m->dom || m->n_rng >= 16 || !p) return;
    m->rng_p[m->n_rng] = p; m->rng_n[m->n_rng] = n; m->n_rng++;
}

static bool mo_persistent(const Mo *m, const char *p) {
    for (int i = 0; i < m->n_rng; i++) {
        const char *s = m->rng_p[i];
        if (p >= s && p <= s + m->rng_n[i]) return true; /* p==end は n=0 借用用 */
    }
    return false;
}

static void run_reset(Mo *m) { m->b_p = NULL; m->b_n = 0; m->c_n = 0; m->mode = 0; }

static void run_to_copy(Mo *m) {
    if (m->mode == 2) return;
    u32 need = m->b_n;
    if (need + 1 > m->c_cap) {
        u64 nc = m->c_cap ? m->c_cap : 64;
        while (nc < need + 1) nc *= 2;
        char *np = (char *)realloc(m->c_buf, nc);
        if (!np) if_fatal("md: run oom");
        m->c_buf = np; m->c_cap = (u32)nc;
    }
    if (m->b_n) memcpy(m->c_buf, m->b_p, m->b_n);
    m->c_n = m->b_n;
    m->mode = 2;
}

/* raw slice を text run に追加（dom モード。借用量判定つき） */
static inline void run_add(Mo *m, const char *p, u32 n) {
    if (!n) return;
    if (m->mode == 0) {
        if (mo_persistent(m, p)) { m->b_p = p; m->b_n = n; m->mode = 1; return; }
        run_to_copy(m);
    } else if (m->mode == 1) {
        if (p == m->b_p + m->b_n) { m->b_n += n; return; }
        run_to_copy(m);
    }
    if (m->c_n + n + 1 > m->c_cap) {
        u64 nc = m->c_cap ? m->c_cap : 64;
        while (nc < m->c_n + n + 1) nc *= 2;
        char *np = (char *)realloc(m->c_buf, nc);
        if (!np) if_fatal("md: run oom");
        m->c_buf = np; m->c_cap = (u32)nc;
    }
    memcpy(m->c_buf + m->c_n, p, n);
    m->c_n += n;
}

static inline void run_add_ch(Mo *m, char c) {
    if (m->mode != 2) run_to_copy(m);
    if (m->c_n + 2 > m->c_cap) {
        u64 nc = m->c_cap ? m->c_cap : 64;
        while (nc < m->c_n + 2) nc *= 2;
        char *np = (char *)realloc(m->c_buf, nc);
        if (!np) if_fatal("md: run oom");
        m->c_buf = np; m->c_cap = (u32)nc;
    }
    m->c_buf[m->c_n++] = c;
}

static inline IfNode *mnew(Mo *m, IfNodeKind kind) {
    u64 _t0; if (mpf()) _t0 = if_rdtsc_md(); else _t0 = 0;
    IfNode *n;
    /* ノード数は Mo ローカルに計数（2-way 並列で dom 共有カウンタの競合を構造排除）。
     * 並列時の T5 同値性: 合計 na+nb >= cap ⟺ 逐語走査が cap に到達、のため
     * 「ローカル >= cap → taint」は保守側であり合計判定で厳密に一致させる（join 参照）。 */
    if (__builtin_expect(m->nslab != m->nslab_end, 1)) {
        n = m->nslab++;
    } else {
        /* cap 到達時でも bump 自体は無害（直後の一括判定で taint・返却 NULL。
         * 到達時に無駄になるのは taint で中断される走査の 1 ブロック分のみ） */
        n = (IfNode *)if_arena_bump(m->a, 128 * sizeof(IfNode));
        m->nslab = n + 1;
        m->nslab_end = n + 128;
    }
    if (__builtin_expect(m->n_nodes >= IF_MAX_DOM_NODES, 0)) { mo_taint(m); return NULL; }
    m->n_nodes++;
    n->kind = kind;
    n->tag = 0; n->ns = IF_NS_HTML; n->flags = 0;
    n->attrs = NULL; n->n_attrs = 0;
    n->style = NULL;
    n->parent = n->first_child = n->last_child = n->next_sibling = NULL;
    n->content = NULL;
    if (_t0) MP_MNEW += if_rdtsc_md() - _t0;
    return n;
}

static void mattach(IfNode *parent, IfNode *ch) {
    ch->parent = parent;
    if (parent->last_child) parent->last_child->next_sibling = ch;
    else parent->first_child = ch;
    parent->last_child = ch;
}

/* 現在の run を TEXT ノードとして確定（open/close/void の前に必ず呼ぶ） */
/* TEXT 内容分類（layout fitdom の再トークン化を消すための parse 確定メタデータ）。
 * 分類述語は layout 側の走査条件と同一定義（結果の同値性は述語一致で保証）:
 *  - ASCII_VIS 判定 = fitdom の ASCII 可視ラン走査が全バイト通過する条件そのもの
 *  - CJK3W2 判定 = ok3 ランの全グリフが if_utf8_band_w2（⊂ decode 成功 ∧ 幅 2） */
static u8 mo_txtcls(IfStr t) {
    const u8 *s = (const u8 *)t.p;
    u32 n = t.n;
    u32 i = 0;
#if defined(IF_MD_SIMD)
    for (; i + 16 <= n; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)(s + i));
        __m128i m = _mm_and_si128(_mm_cmpgt_epi8(v, _mm_set1_epi8(0x20)),
                                  _mm_cmpgt_epi8(_mm_set1_epi8(0x7F), v));
        if ((unsigned)_mm_movemask_epi8(m) != 0xFFFFu) goto not_ascii_vis;
    }
#endif
    for (; i < n; i++) if (s[i] < 0x21 || s[i] > 0x7E) goto not_ascii_vis;
    return IF_NF_TXTCLS_ASCII_VIS;
not_ascii_vis:
    if (n % 3 == 0) {
        u32 j = 0;
        for (; j + 2 < n; j += 3)
            if (!if_utf8_band_w2(s[j], s[j + 1], s[j + 2])) break;
        if (j == n) return IF_NF_TXTCLS_CJK3W2;
    }
    return 0;
}

static inline void run_flush(Mo *m) {
    u64 _t0; if (mpf()) _t0 = if_rdtsc_md(); else _t0 = 0;
    if (m->mode != 0) {
        IfStr t;
        if (m->mode == 1) t = if_str(m->b_p, m->b_n);
        else {
            char *np = (char *)if_arena_bump(m->a, m->c_n ? m->c_n : 1);
            if (m->c_n) memcpy(np, m->c_buf, m->c_n);
            t = if_str(np, m->c_n);
        }
        if (t.n) {
            IfNode *n = mnew(m, IF_NODE_TEXT);
            if (n) { n->u.text = t; n->flags |= mo_txtcls(t); mattach(m->cur, n); }
        }
        run_reset(m);
    }
    if (_t0) MP_FLUSH += if_rdtsc_md() - _t0;
}

/* ---- emitter 命令（string/dom 両 backend 共通の中間表現） ---- */

typedef struct {
    u16 tag;
    IfStr name;       /* 静的文字列 */
    IfStr an[4];      /* 属性名（静的文字列） */
    IfStr av[4];      /* 属性値（raw） */
    u32 nattr;
} MoPend;

/* emission はスレッド内直列（再帰内で open 完結してから深く潜る）。
 * 2-way 並列のため __thread 局所化（各スレッドの emission は独立に完結する） */
static __thread MoPend g_pend;

static void mo_open(Mo *m, u16 tag, const char *name, u32 nl) {
    /* string backend: バイト列は厳密に従来どおり "<name" + 属性 + ">" */
    if (m->is_dom && tag == IF_TAG_A) {
        /* T2: 開いている <a> の内側で <a> を開くと adoption agency が発火して
         * 本パーサと木が変わる → 文書ごと従来経路へフォールバック */
        for (int k = m->sp - 1; k >= 0; k--)
            if (m->stk[k]->tag == IF_TAG_A) { mo_taint(m); break; }
    }
    g_pend.tag = tag;
    g_pend.name = if_str(name, nl);
    g_pend.nattr = 0;
    if (!m->is_dom) { b_putn(&m->str, "<", 1); b_putn(&m->str, name, nl); }
}

static void mo_attr(Mo *m, const char *an, u32 anl, IfStr v) {
    if (m->is_dom) {
        if (g_pend.nattr < 4) {
            g_pend.an[g_pend.nattr] = if_str(an, anl);
            g_pend.av[g_pend.nattr] = v;
            g_pend.nattr++;
        }
        return;
    }
    /* name="v"(escape) */
    b_putc(&m->str, ' ');
    b_putn(&m->str, an, anl);
    b_putn(&m->str, "=\"", 2);
    for (u32 i = 0; i < v.n; i++) {
        char c = v.p[i];
        if (c == '&') b_puts(&m->str, "&amp;");
        else if (c == '<') b_puts(&m->str, "&lt;");
        else if (c == '"') b_puts(&m->str, "&quot;");
        else b_putc(&m->str, c);
    }
    b_putc(&m->str, '"');
}

static void mo_elem_store(Mo *m, bool push) {
    if (m->mode) run_flush(m); /* flush は mode≠0 のときだけ */
    if (__builtin_expect(m->tainted, 0)) return;
    IfNode *n = mnew(m, IF_NODE_ELEMENT);
    if (!n) return;
    n->tag = g_pend.tag;
    n->u.tag_name = g_pend.name;
    if (g_pend.nattr) {
        /* slim: レンダリング経路で読まれる属性のみ保持（A[href], IMG[alt]）。
         * 落とした属性は DOM 上の ~30B/箇所の確保＋書込みを消す。読み手は
         * layout.c の collect_link/IMG alt と dom dump のみ（dump は非 slim 経路）。
         * 保持規則は if_md_parse_fast_f の契約と一致（name は emitter 由来の小文字） */
        u32 kn = 0;
        u8 ki[4];
        for (u32 i = 0; i < g_pend.nattr; i++) {
            bool keep = !m->slim_attrs; /* 非 slim は全保持 */
            if (m->slim_attrs) {
                IfStr an = g_pend.an[i];
                keep = (g_pend.tag == IF_TAG_A && an.n == 4 &&
                        memcmp(an.p, "href", 4) == 0) ||
                       (g_pend.tag == IF_TAG_IMG && an.n == 3 &&
                        memcmp(an.p, "alt", 3) == 0);
            }
            if (keep) ki[kn++] = (u8)i;
        }
        if (kn) {
            IfAttr *at = (IfAttr *)if_arena_alloc(m->a, kn * sizeof(IfAttr));
            for (u32 k = 0; k < kn; k++) {
                u32 i = ki[k];
                IfStr v = g_pend.av[i];
                /* 値が persistent 範囲外なら arena に複製（脚注 id 等の生成文字列） */
                if (v.n && !mo_persistent(m, v.p)) {
                    char *np = (char *)if_arena_alloc(m->a, v.n);
                    memcpy(np, v.p, v.n);
                    v = if_str(np, v.n);
                }
                at[k].name = g_pend.an[i];
                at[k].value = v;
            }
            n->attrs = at;
            n->n_attrs = kn;
        }
    }
    if (g_pend.tag == IF_TAG_STYLE) m->dom->has_style = 1;
    mattach(m->cur, n);
    if (push) {
        if (m->sp >= 128) { mo_taint(m); return; }
        m->stk[m->sp++] = n;
        m->cur = n;
    }
}

/* 無属性要素の open+push 融合（pend 経由の多段コールを 1 つに畳む。
 * DOM の生出力は mo_open+mo_open_end と同値（pend.nattr==0 のときの mo_elem_store と同一遷移）。
 * string backend も "<name>" の同じ 3 書きでバイト一致。has_style 監視対象外タグのみに使う
 * （md emitter は style タグを生成しない） */
static inline void mo_open_push(Mo *m, u16 tag, const char *nm, u32 nl) {
    if (__builtin_expect(!m->is_dom, 0)) {
        b_putn(&m->str, "<", 1); b_putn(&m->str, nm, nl); b_putc(&m->str, '>');
        return;
    }
    if (m->mode) run_flush(m);
    IfNode *n = mnew(m, IF_NODE_ELEMENT);
    if (__builtin_expect(!n || m->tainted, 0)) return;
    n->tag = tag;
    n->u.tag_name = if_str(nm, nl);
    mattach(m->cur, n);
    if (__builtin_expect(m->sp >= 128, 0)) { mo_taint(m); return; }
    m->stk[m->sp++] = n;
    m->cur = n;
}

/* void 要素（hr/br）の融合 */
static inline void mo_open_void(Mo *m, u16 tag, const char *nm, u32 nl) {
    if (__builtin_expect(!m->is_dom, 0)) {
        b_putn(&m->str, "<", 1); b_putn(&m->str, nm, nl); b_putc(&m->str, '>');
        return;
    }
    if (m->mode) run_flush(m);
    IfNode *n = mnew(m, IF_NODE_ELEMENT);
    if (__builtin_expect(!n || m->tainted, 0)) return;
    n->tag = tag;
    n->u.tag_name = if_str(nm, nl);
    mattach(m->cur, n);
}

static void mo_open_end(Mo *m) { /* 開始タグ閉じ＋要素をスタックへ */
    if (!m->is_dom) { b_putc(&m->str, '>'); return; }
    mo_elem_store(m, true);
}

static void mo_open_end_void(Mo *m) { /* void 要素（hr, img, br） */
    if (!m->is_dom) { b_putc(&m->str, '>'); return; }
    mo_elem_store(m, false);
}

static void mo_close(Mo *m, const char *name, u32 nl) {
    if (!m->is_dom) {
        b_putn(&m->str, "</", 2);
        b_putn(&m->str, name, nl);
        b_putc(&m->str, '>');
        return;
    }
    if (m->mode) run_flush(m); /* flush は mode≠0 のときだけ（呼出自体を畳む） */
    IfNode *top = m->sp > 0 ? m->stk[m->sp - 1] : NULL;
    if (!top || top->u.tag_name.n != nl ||
        (top->u.tag_name.p != name && memcmp(top->u.tag_name.p, name, nl) != 0)) {
        mo_taint(m); /* 到達不能のはず（emitter は常に対応させる） */
        return;
    }
    m->sp--;
    m->cur = m->sp > 0 ? m->stk[m->sp - 1] : m->dom->root;
}

/* 「純ブロック容器」: 子が全てブロック要素で、直接の ws-only テキストは描画に
 * 一切寄与しないコンテナ（INV: 画面描画に関係ないものは DOM しない）。
 * 同値性: これらの直下の ws-only TEXT は layout で any=false/seg 無しの空 IFC に
 * しかならず (line 非発行)、style/semantics にも到達しない → ノード自体を作らない。
 * 除外: li/td/th/p/hN/pre/code は inline 本文の混在 or 保持必須（li 直下の "\n" は
 * 行末空白として幅を変えうるため残す）。IDM では全 TEXT の ~45%（全ノードの ~27%）が
 * この死ノードだった。 */
static inline bool mo_ws_sink(const Mo *m) {
    switch (m->cur->tag) {
    case IF_TAG_BODY: case IF_TAG_BLOCKQUOTE: case IF_TAG_TABLE: case IF_TAG_THEAD:
    case IF_TAG_TBODY: case IF_TAG_TR: case IF_TAG_UL: case IF_TAG_OL:
    case IF_TAG_SECTION:
        return true;
    default:
        return false;
    }
}

/* raw 本文（string backend は 3 文字 escape。dom は raw スライス） */
static void mo_text(Mo *m, IfStr s) {
    if (m->is_dom) {
        if (s.n && mo_ws_sink(m)) { /* ws-only なら死ノード候補 → 読み捨て */
            u32 i = 0;
            while (i < s.n && (s.p[i] == ' ' || s.p[i] == '\n' || s.p[i] == '\t' ||
                               s.p[i] == '\r' || s.p[i] == '\f')) i++;
            if (i == s.n) return;
        }
        run_add(m, s.p ? s.p : "", s.n); return;
    }
    for (u32 i = 0; i < s.n; i++) {
        char c = s.p[i];
        if (c == '&') b_puts(&m->str, "&amp;");
        else if (c == '<') b_puts(&m->str, "&lt;");
        else if (c == '>') b_puts(&m->str, "&gt;");
        else b_putc(&m->str, c);
    }
}

/* escape リテラル等の「生1文字」（string backend も生で出す＝従来の b_putc） */
static void mo_raw_ch(Mo *m, char c) {
    if (m->is_dom && (c == '<' || c == '&')) mo_taint(m); /* T1: 文字列側が文法外になる */
    if (m->is_dom) { run_add_ch(m, c); return; }
    b_putc(&m->str, c);
}

/* ブロック間テキスト（改行等。string はそのまま、dom は TEXT ノード） */
static void mo_text_ch(Mo *m, char c) {
    if (m->is_dom) {
        if (mo_ws_sink(m) && (c == '\n' || c == ' ' || c == '\t' || c == '\r' || c == '\f'))
            return; /* 純ブロック容器直下の ws-only 断片は DOM 化しない（描画不寄与。INV） */
        run_add_ch(m, c); return;
    }
    b_putc(&m->str, c);
}

/* ================= inline 展開（SIMD + 密 dispatch） ================= */

typedef struct Fn Fn; /* 脚注（後段で定義） */

static void inline_span(Mo *out, Fn *fn, IfStr s);

/* 次の「特殊文字」位置を返す（無ければ s.n 以降の消費で呼び出し側が終了を知る）。
 * 特殊集合: \ ` * _ ~ ! [ < & >  */
#ifdef IF_MD_SIMD
#include <immintrin.h>
/* AVX2 版: 32B/iter。target attribute + ランタイム dispatch（ビルドフラグ不変・
 * __builtin_cpu_supports は 1 回だけ評価）。10 種の特殊文字を 10 本の cmpeq で一括。 */
__attribute__((target("avx2"))) static u32 scan_special_avx2(IfStr s, u32 from) {
    const char *p = s.p;
    u32 n = s.n;
    const __m256i v_bs = _mm256_set1_epi8('\\'), v_bt = _mm256_set1_epi8('`'),
                  v_st = _mm256_set1_epi8('*'), v_un = _mm256_set1_epi8('_'),
                  v_ti = _mm256_set1_epi8('~'), v_ex = _mm256_set1_epi8('!'),
                  v_ob = _mm256_set1_epi8('['), v_lt = _mm256_set1_epi8('<'),
                  v_am = _mm256_set1_epi8('&'), v_gt = _mm256_set1_epi8('>');
    u32 i = from;
    for (; i + 32 <= n; i += 32) {
        __m256i b = _mm256_loadu_si256((const __m256i *)(p + i));
        __m256i e = _mm256_cmpeq_epi8(b, v_bs);
        e = _mm256_or_si256(e, _mm256_cmpeq_epi8(b, v_bt));
        e = _mm256_or_si256(e, _mm256_cmpeq_epi8(b, v_st));
        e = _mm256_or_si256(e, _mm256_cmpeq_epi8(b, v_un));
        e = _mm256_or_si256(e, _mm256_cmpeq_epi8(b, v_ti));
        e = _mm256_or_si256(e, _mm256_cmpeq_epi8(b, v_ex));
        e = _mm256_or_si256(e, _mm256_cmpeq_epi8(b, v_ob));
        e = _mm256_or_si256(e, _mm256_cmpeq_epi8(b, v_lt));
        e = _mm256_or_si256(e, _mm256_cmpeq_epi8(b, v_am));
        e = _mm256_or_si256(e, _mm256_cmpeq_epi8(b, v_gt));
        unsigned mask = (unsigned)_mm256_movemask_epi8(e);
        if (mask) return i + (u32)__builtin_ctz(mask);
    }
    for (; i < n; i++) {
        char c = p[i];
        if (c == '\\' || c == '`' || c == '*' || c == '_' || c == '~' ||
            c == '!' || c == '[' || c == '<' || c == '&' || c == '>')
            return i;
    }
    return n;
}
#endif

static u32 scan_special(IfStr s, u32 from) {
#ifdef IF_MD_SIMD
    static int have_avx2 = -1;
    if (__builtin_expect(have_avx2 < 0, 0))
        have_avx2 = __builtin_cpu_supports("avx2") ? 1 : 0;
    if (have_avx2) return scan_special_avx2(s, from);
    const char *p = s.p;
    u32 n = s.n;
    static const char bs = '\\', bt = '`', st = '*', un = '_', ti = '~', ex = '!', ob = '[', lt = '<', am = '&', gt = '>';
    const __m128i v_bs = _mm_set1_epi8(bs), v_bt = _mm_set1_epi8(bt), v_st = _mm_set1_epi8(st),
                  v_un = _mm_set1_epi8(un), v_ti = _mm_set1_epi8(ti), v_ex = _mm_set1_epi8(ex),
                  v_ob = _mm_set1_epi8(ob), v_lt = _mm_set1_epi8(lt), v_am = _mm_set1_epi8(am),
                  v_gt = _mm_set1_epi8(gt);
    u32 i = from;
    for (; i + 16 <= n; i += 16) {
        __m128i b = _mm_loadu_si128((const __m128i *)(p + i));
        __m128i e = _mm_cmpeq_epi8(b, v_bs);
        e = _mm_or_si128(e, _mm_cmpeq_epi8(b, v_bt));
        e = _mm_or_si128(e, _mm_cmpeq_epi8(b, v_st));
        e = _mm_or_si128(e, _mm_cmpeq_epi8(b, v_un));
        e = _mm_or_si128(e, _mm_cmpeq_epi8(b, v_ti));
        e = _mm_or_si128(e, _mm_cmpeq_epi8(b, v_ex));
        e = _mm_or_si128(e, _mm_cmpeq_epi8(b, v_ob));
        e = _mm_or_si128(e, _mm_cmpeq_epi8(b, v_lt));
        e = _mm_or_si128(e, _mm_cmpeq_epi8(b, v_am));
        e = _mm_or_si128(e, _mm_cmpeq_epi8(b, v_gt));
        unsigned mask = (unsigned)_mm_movemask_epi8(e);
        if (mask) return i + (u32)__builtin_ctz(mask);
    }
    for (; i < n; i++) {
        char c = p[i];
        if (c == '\\' || c == '`' || c == '*' || c == '_' || c == '~' ||
            c == '!' || c == '[' || c == '<' || c == '&' || c == '>')
            return i;
    }
    return n;
#else
    for (u32 i = from; i < s.n; i++) {
        char c = s.p[i];
        if (c == '\\' || c == '`' || c == '*' || c == '_' || c == '~' ||
            c == '!' || c == '[' || c == '<' || c == '&' || c == '>')
            return i;
    }
    return s.n;
#endif
}

/* 閉じ区切りを探す（delim は "**", "*", "__", "_", "~~" 等）。戻り値: 終了位置 or 負 */
static i32 find_close(IfStr s, u32 from, const char *delim, u32 dn) {
    for (u32 i = from; i + dn <= s.n; i++) {
        if (s.p[i] == '\\') { i++; continue; }
        if (memcmp(s.p + i, delim, dn) == 0) return (i32)i;
    }
    return -1;
}

static void emit_fmt(Mo *out, Fn *fn, u16 tag, const char *name, u32 nl, IfStr inner) {
    mo_open_push(out, tag, name, nl);
    inline_span(out, fn, inner);
    mo_close(out, name, nl);
}

/* "[text](dest)" / "![alt](dest)" / "[^id]" / "<http://>" の判定を 1 箇所で */
static bool try_link(Mo *out, Fn *fn, IfStr s, u32 *adv);

/* ================= 脚注 ================= */
typedef struct { IfStr id; IfStr text; } FnDef;
struct Fn {
    IfArena *a;
    bool is_dom;                  /* dom モードは id/text を arena 恒久化 */
    FnDef *defs; u32 n_defs, cap_defs;
    IfStr *refs; u32 n_refs, cap_refs;
};

static IfStr fn_own(Fn *f, IfStr s) {
    if (f->is_dom) {
        char *p = (char *)if_arena_alloc(f->a, s.n + 1);
        if (s.n) memcpy(p, s.p, s.n);
        p[s.n] = 0;
        return if_str(p, s.n);
    }
    char *p = (char *)malloc(s.n + 1);
    if (!p) if_fatal("md: fn oom");
    if (s.n) memcpy(p, s.p, s.n);
    p[s.n] = 0;
    return if_str(p, s.n);
}

static u32 fn_find_def(Fn *f, IfStr id) {
    for (u32 i = 0; i < f->n_defs; i++) if (if_str_eq(f->defs[i].id, id)) return i;
    return UINT32_MAX;
}
static u32 fn_ref_number(Fn *f, IfStr id) {
    for (u32 i = 0; i < f->n_refs; i++) if (if_str_eq(f->refs[i], id)) return i + 1;
    if (f->n_refs >= f->cap_refs) {
        u64 cap = f->cap_refs;
        f->refs = (IfStr *)if_arena_grow(f->a, f->refs, &cap, f->n_refs + 1, sizeof(IfStr));
        f->cap_refs = (u32)cap;
    }
    f->refs[f->n_refs++] = fn_own(f, id);
    return f->n_refs;
}
static void fn_add_def(Fn *f, IfStr id, IfStr text) {
    if (fn_find_def(f, id) != UINT32_MAX) return;
    if (f->n_defs >= f->cap_defs) {
        u64 cap = f->cap_defs;
        f->defs = (FnDef *)if_arena_grow(f->a, f->defs, &cap, f->n_defs + 1, sizeof(FnDef));
        f->cap_defs = (u32)cap;
    }
    f->defs[f->n_defs].id = fn_own(f, id);
    f->defs[f->n_defs].text = fn_own(f, text);
    f->n_defs++;
}
static void fn_free(Fn *f) {
    if (!f->is_dom) {
        for (u32 i = 0; i < f->n_refs; i++) free((void *)f->refs[i].p);
        for (u32 i = 0; i < f->n_defs; i++) {
            free((void *)f->defs[i].id.p);
            free((void *)f->defs[i].text.p);
        }
    }
    f->n_refs = f->n_defs = 0;
}

/* prefix + id (+ suffix) を正確な長さで malloc scratch に作る（脚注は稀なので
 * heap でよい。値は open_end までに消費/複製される。切り詰めは絶対禁止） */
static IfStr scratch_cat(char **slot, const char *prefix, u32 pn, IfStr id,
                         const char *suffix, u32 sn) {
    u32 need = pn + id.n + sn + 1;
    char *p = (char *)realloc(*slot, need);
    if (!p) if_fatal("md: scratch oom");
    *slot = p;
    memcpy(p, prefix, pn);
    memcpy(p + pn, id.p, id.n);
    if (sn) memcpy(p + pn + id.n, suffix, sn);
    p[pn + id.n + sn] = 0;
    return if_str(p, pn + id.n + sn);
}
/* スレッド局所（2-way 並列）。ワーカ終了時の 2 バッファのみ回収されない
 * 既知の小リーク（既存のプロセス寿命解放規約と整合。CLI は 1 発実行） */
static __thread char *g_scr1, *g_scr2;

static bool try_link(Mo *out, Fn *fn, IfStr s, u32 *adv) {
    u32 i = 1;
    if (i < s.n && s.p[i] == '^') { /* footnote ref */
        u32 is = ++i;
        while (i < s.n && s.p[i] != ']') i++;
        if (i >= s.n) return false;
        IfStr id = if_str(s.p + is, i - is);
        if (fn && id.n) {
            u32 seen = 0;
            for (u32 r = 0; r < fn->n_refs; r++) if (if_str_eq(fn->refs[r], id)) seen++;
            u32 num = fn_ref_number(fn, id);
            char nb[24];
            snprintf(nb, sizeof nb, "%u", num);
            /* 従来は seen>=1 でも "-2" を出す（refs は unique 化されるので seen+1 は常に 2。
             * 旧挙動そのものが正（テスト固定）なので厳密に再現する） */
            IfStr idv = scratch_cat(&g_scr1, "fr-", 3, id, seen ? "-2" : "", seen ? 2 : 0);
            IfStr hrv = scratch_cat(&g_scr2, "#fn-", 4, id, "", 0);
            mo_open_push(out, IF_TAG_SUP, "sup", 3);
            mo_open(out, IF_TAG_A, "a", 1);
            mo_attr(out, "href", 4, hrv);
            mo_attr(out, "id", 2, idv);
            mo_open_end(out);
            if (out->is_dom) { IfStr t = if_str(nb, (u32)strlen(nb)); mo_text(out, t); }
            else b_puts(&out->str, nb);
            mo_close(out, "a", 1);
            mo_close(out, "sup", 3);
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
    mo_open(out, IF_TAG_A, "a", 1);
    mo_attr(out, "href", 4, dest);
    mo_open_end(out);
    inline_span(out, fn, text);
    mo_close(out, "a", 1);
    *adv = i + 1;
    return true;
}

static __thread int inl_depth = 0; /* inline_span 再帰深（スレッド局所） */
static void inline_span(Mo *out, Fn *fn, IfStr s) {
    u64 _t0 = 0; bool _top = false;
    if (mpf() && inl_depth++ == 0) { _t0 = if_rdtsc_md(); _top = true; }
    else if (!mpf()) inl_depth = inl_depth;
    u32 i = 0;
    while (i < s.n) {
        /* 通常ランを一括消費（次の特殊文字まで。string backend でも従来の
         * per-char b_putc の連結と同じバイト列になる） */
        u32 sp0 = scan_special(s, i);
        if (sp0 > i) { mo_text(out, if_str(s.p + i, sp0 - i)); i = sp0; }
        if (i >= s.n) break;
        char c = s.p[i];
        switch (c) {
        case '\\':
            if (i + 1 < s.n) {
                char n2 = s.p[i + 1];
                if (strchr("\\`*_{}[]()#+-.!~|<>", n2)) { mo_raw_ch(out, n2); i += 2; continue; }
            }
            mo_text(out, if_str(s.p + i, 1)); i++; continue;
        case '`': { /* インラインコード */
            u32 run = 1;
            while (i + run < s.n && s.p[i + run] == '`') run++;
            char delim[4] = "``";
            i32 close = -1;
            for (u32 j = i + run; j + run <= s.n; j++) {
                if (memcmp(s.p + j, delim, run <= 2 ? run : 2) == 0 && run <= 2) { close = (i32)j; break; }
                if (s.p[j] == '`' && run > 2) { close = (i32)j; break; }
            }
            if (close < 0) { mo_text(out, if_str(s.p + i, 1)); i++; continue; }
            mo_open(out, IF_TAG_CODE, "code", 4);
            mo_open_end(out);
            mo_text(out, if_str(s.p + i + run, (u32)close - (i + run)));
            mo_close(out, "code", 4);
            i = (u32)close + run;
            continue;
        }
        case '*': case '_': {
            if (i + 1 < s.n && s.p[i + 1] == c) {
                i32 close = find_close(s, i + 2, c == '*' ? "**" : "__", 2);
                if (close > (i32)i + 2) {
                    emit_fmt(out, fn, IF_TAG_STRONG, "strong", 6,
                             if_str(s.p + i + 2, (u32)close - i - 2));
                    i = (u32)close + 2;
                    continue;
                }
            }
            i32 close = find_close(s, i + 1, c == '*' ? "*" : "_", 1);
            if (close > (i32)i + 1) {
                emit_fmt(out, fn, IF_TAG_EM, "em", 2, if_str(s.p + i + 1, (u32)close - i - 1));
                i = (u32)close + 1;
                continue;
            }
            mo_text(out, if_str(s.p + i, 1)); i++; continue;
        }
        case '~':
            if (i + 1 < s.n && s.p[i + 1] == '~') {
                i32 close = find_close(s, i + 2, "~~", 2);
                if (close > (i32)i + 2) {
                    emit_fmt(out, fn, IF_TAG_UNKNOWN, "del", 3,
                             if_str(s.p + i + 2, (u32)close - i - 2));
                    i = (u32)close + 2;
                    continue;
                }
            }
            mo_text(out, if_str(s.p + i, 1)); i++; continue;
        case '!':
            if (i + 1 < s.n && s.p[i + 1] == '[') {
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
                        mo_open(out, IF_TAG_IMG, "img", 3);
                        mo_attr(out, "src", 3, if_str(rest.p + ds, ke - ds));
                        mo_attr(out, "alt", 3, if_str(rest.p + 1, k - 1));
                        mo_open_end_void(out);
                        adv0 = ke + 1;
                    }
                }
                if (adv0) { i = i + 1 + adv0; continue; }
            }
            mo_text(out, if_str(s.p + i, 1)); i++; continue;
        case '[': {
            u32 adv = 0;
            if (try_link(out, fn, if_str(s.p + i, s.n - i), &adv)) { i += adv; continue; }
            mo_text(out, if_str(s.p + i, 1)); i++; continue;
        }
        case '<': { /* 自動リンク <http://…> */
            u32 j = i + 1;
            while (j < s.n && s.p[j] != '>') j++;
            if (j < s.n) {
                IfStr url = if_str(s.p + i + 1, j - i - 1);
                if ((url.n > 7 && memcmp(url.p, "http://", 7) == 0) ||
                    (url.n > 8 && memcmp(url.p, "https://", 8) == 0)) {
                    mo_open(out, IF_TAG_A, "a", 1);
                    mo_attr(out, "href", 4, url);
                    mo_open_end(out);
                    mo_text(out, url);
                    mo_close(out, "a", 1);
                    i = j + 1;
                    continue;
                }
            }
            mo_text(out, if_str(s.p + i, 1)); i++; continue;
        }
        case '&': case '>':
        default:
            mo_text(out, if_str(s.p + i, 1)); i++; continue;
        }
    }
    if (_top) MP_INLINE += if_rdtsc_md() - _t0;
    if (mpf()) inl_depth--;
}


/* ================= ブロック層 ================= */

/* 行走査 */
typedef struct { const char *p; u32 n; } Ln;

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

static int ln_heading(Ln l) {
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

static u32 ln_quote(Ln l) {
    u32 i = 0;
    while (i < l.n && l.p[i] == ' ') i++;
    if (i >= l.n || l.p[i] != '>') return 0;
    i++;
    if (i < l.n && l.p[i] == ' ') i++;
    return i;
}

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
    if (n && cells[n - 1].n == 0) n--;
    return n;
}

static bool ln_is_table_delim(Ln l) {
    IfStr cells[32];
    u32 n = split_cells(l, cells, 32);
    if (!n) return false;
    for (u32 i = 0; i < n; i++) {
        IfStr c = cells[i];
        u32 j = 0;
        if (j < c.n && c.p[j] == ':') j++;
        u32 ds = j;
        while (j < c.n && c.p[j] == '-') j++;
        if (j - ds < 3) return false;
        if (j < c.n && c.p[j] == ':') j++;
        if (j != c.n) return false;
    }
    return true;
}

static void blocks_win(Mo *out, Fn *fn, Ln *ls, u32 lo, u32 hi, u32 depth);
static void blocks_str(Mo *out, Fn *fn, IfStr s, u32 depth);

static u32 ln_indent(Ln l) {
    u32 i = 0;
    while (i < l.n && l.p[i] == ' ') i++;
    return i;
}

/* 段落: 連結は「スペース」、前行の末尾 2 空白（ハードブレーク）なら <br> で接続 */
static void emit_para_lines(Mo *out, Fn *fn, Ln *ls, u32 lo, u32 hi) {
    mo_open_push(out, IF_TAG_P, "p", 1);
    bool prev_hard = false;
    for (u32 i = lo; i < hi; i++) {
        IfStr x = if_str(ls[i].p, ls[i].n);
        u32 trail = 0;
        while (trail < x.n && x.p[x.n - 1 - trail] == ' ') trail++;
        bool hard = trail >= 2;
        if (hard) x.n -= trail;
        if (i > lo) {
            if (prev_hard) {
                mo_open_void(out, IF_TAG_BR, "br", 2);
            } else {
                mo_text_ch(out, ' ');
            }
        }
        inline_span(out, fn, x);
        prev_hard = hard;
    }
    mo_close(out, "p", 1);
    mo_text_ch(out, '\n');
}

static __thread int bw_depth = 0; /* prof 括弧の深さ（スレッド局所） */
static void blocks_win(Mo *out, Fn *fn, Ln *ls, u32 lo, u32 hi, u32 depth) {
    u64 _t0 = 0; bool _top = false;
    if (mpf() && bw_depth++ == 0) { _t0 = if_rdtsc_md(); _top = true; }
    u32 i = lo;
    while (i < hi) {
        Ln l = ls[i];
        if (ln_blank(l)) { i++; continue; }
        /* 行分類ゲート: 先頭の非空白文字 _cs で発火不可能なチェックを構造的にスキップ。
         * 同値性: 各条件は「そのチェックが true を返しうる必要条件」のみ
         * （fndef/heading は col0 固定、hr/fence は ' '/\t インデント許容、
         * quote/list は ' ' インデントのみ許容。'\t' のときは保守的に hr/fence を実行）。
         * 副作用を持つのは fndef 成功時（fn_add_def）のみで、ゲート外で true には
         * ならないため状態遷移も同値。チェック順序は従来と同一。 */
        u32 _sp = 0;
        while (_sp < l.n && l.p[_sp] == ' ') _sp++;
        const u8 _cs = _sp < l.n ? (u8)l.p[_sp] : 0;
        if (_sp == 0 && _cs == '[') {
            IfStr fid, ftx;
            if (ln_fndef(l, &fid, &ftx)) { fn_add_def(fn, fid, ftx); i++; continue; }
        }
        if (_sp == 0 && _cs == '#') {
            int hh = ln_heading(l);
            if (hh) {
            static const char HNM[6][3] = { "h1", "h2", "h3", "h4", "h5", "h6" };
            const char *nm = HNM[hh - 1]; /* 静的文字列（tag_name の寿命規約） */
            mo_open_push(out, (u16)(IF_TAG_H1 + (hh - 1)), nm, 2);
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
            mo_close(out, nm, 2);
            mo_text_ch(out, '\n');
            i++;
            continue;
        }
        } /* end gate: heading */
        if (_cs == '-' || _cs == '*' || _cs == '_' || _cs == '\t') {
        if (ln_is_hr(l)) {
            mo_open_void(out, IF_TAG_HR, "hr", 2);
            mo_text_ch(out, '\n');
            i++;
            continue;
        }
        }
        char fsym;
        if ((_cs == '`' || _cs == '~' || _cs == '\t') && ln_fence(l, &fsym)) {
            u32 k = 0;
            while (k < l.n && l.p[k] == fsym) k++;
            while (k < l.n && (l.p[k] == ' ' || l.p[k] == '\t')) k++;
            IfStr lang = if_str(l.p + k, l.n - k);
            mo_open_push(out, IF_TAG_PRE, "pre", 3);
            mo_open(out, IF_TAG_CODE, "code", 4);
            if (lang.n) {
                IfStr cv = scratch_cat(&g_scr1, "lang-", 5, lang, "", 0);
                mo_attr(out, "class", 5, cv);
            }
            mo_open_end(out);
            i++;
            while (i < hi) {
                Ln cl = ls[i];
                char s2;
                if (ln_fence(cl, &s2) && s2 == fsym) { i++; break; }
                mo_text(out, if_str(cl.p, cl.n));
                mo_text_ch(out, '\n');
                i++;
            }
            mo_close(out, "code", 4);
            mo_close(out, "pre", 3);
            mo_text_ch(out, '\n');
            continue;
        }
        u32 q = _cs == '>' ? ln_quote(l) : 0;
        if (q) {
            if (depth < 8) {
                /* コピー不要路: 引用符 w を行ごとに除いた Ln 副窓を作って blocks_win を
                 * 直接歩く。blocks_str(join) と同値の根拠:
                 *   - join 内容 = 各行(x_k)＋'\n' → split で得られる行は {x_k} そのもの
                 *   - 各行の切り出し p+=w/n-=w は元コードと完全同一
                 *   - 行終端の '\n' は ls 側に含まれない（blocks_str 側は幻空行を捨てる）
                 *   - borrow 先は原本（mo_range 登録済み）か CR 正規化コピー（同）を指す
                 * よって emit されるイベント列・text バイト列は現行と完全一致。 */
                u32 j = i;
                while (j < hi && ln_quote(ls[j])) j++;
                u32 cnt = j - i;
                Ln stk[256];
                Ln *wq = stk;
                if (cnt > 256) wq = (Ln *)if_arena_alloc(out->a, (u64)cnt * sizeof(Ln));
                for (u32 k = i; k < j; k++) {
                    u32 w = ln_quote(ls[k]);
                    wq[k - i].p = ls[k].p + w;
                    wq[k - i].n = ls[k].n - w;
                }
                mo_open_push(out, IF_TAG_BLOCKQUOTE, "blockquote", 10);
                mo_text_ch(out, '\n');
                blocks_win(out, fn, wq, 0, cnt, depth + 1);
                mo_close(out, "blockquote", 10);
                mo_text_ch(out, '\n');
                i = j;
            } else {
                u32 j = i;
                while (j < hi && ln_quote(ls[j])) j++;
                B flat; b_init(&flat, out->a);
                for (u32 k2 = i; k2 < j; k2++) {
                    u32 w = ln_quote(ls[k2]);
                    Ln x = { ls[k2].p + w, ls[k2].n - w };
                    b_putn(&flat, x.p, x.n);
                    b_putc(&flat, '\n');
                }
                mo_open_push(out, IF_TAG_BLOCKQUOTE, "blockquote", 10);
                mo_text_ch(out, '\n');
                mo_open_push(out, IF_TAG_P, "p", 1);
                if (out->is_dom) {
                    IfStr fin = b_finish(&flat);
                    mo_range(out, fin.p, fin.n);
                    inline_span(out, fn, fin);
                } else {
                    inline_span(out, fn, if_str(flat.p ? flat.p : "", (u32)flat.n));
                    b_drop(&flat);
                }
                mo_close(out, "p", 1);
                mo_text_ch(out, '\n');
                mo_close(out, "blockquote", 10);
                mo_text_ch(out, '\n');
                i = j;
            }
            continue;
        }
        LiMark mk;
        if ((_cs == '-' || _cs == '*' || _cs == '+' || (_cs >= '0' && _cs <= '9')) &&
            ln_list_item(l, &mk)) {
            bool ordered = mk.ordered;
            u32 base = mk.indent;
            mo_open_push(out, ordered ? IF_TAG_OL : IF_TAG_UL, ordered ? "ol" : "ul", 2);
            mo_text_ch(out, '\n');
            while (i < hi) {
                LiMark m2;
                if (!ln_list_item(ls[i], &m2) || m2.ordered != ordered || m2.indent != base) break;
                mo_open_push(out, IF_TAG_LI, "li", 2);
                inline_span(out, fn, if_str(ls[i].p + m2.mwidth, ls[i].n - m2.mwidth));
                i++;
                u32 j = i;
                while (j < hi && !ln_blank(ls[j]) && ln_indent(ls[j]) > base) j++;
                if (j > i) {
                    blocks_win(out, fn, ls, i, j, depth);
                    i = j;
                }
                mo_close(out, "li", 2);
                mo_text_ch(out, '\n');
            }
            mo_close(out, ordered ? "ol" : "ul", 2);
            mo_text_ch(out, '\n');
            continue;
        }
        /* GFM 表: 判定は連言なので順序を入れ替えて安い方（次行の delim 形）を先に。
         * delim 行の先頭非空白は必ず '|','-',':'（split_cells が ws 後の '|' を許し、
         * 各 cell は -/: のみ）→ それ以外では ln_is_table_delim を呼ばない。 */
        bool is_table = false;
        if (i + 1 < hi) {
            Ln dl = ls[i + 1];
            u32 dsp = 0;
            while (dsp < dl.n && (dl.p[dsp] == ' ' || dl.p[dsp] == '\t')) dsp++;
            if (dsp < dl.n && (dl.p[dsp] == '|' || dl.p[dsp] == '-' || dl.p[dsp] == ':') &&
                ln_is_table_delim(dl)) {
                bool has_pipe = false;
                for (u32 k = 0; k < l.n; k++) if (l.p[k] == '|') { has_pipe = true; break; }
                if (has_pipe) is_table = true;
            }
        }
        if (is_table) {
            IfStr heads[32];
            u32 nh = split_cells(l, heads, 32);
            if (nh > 32) nh = 32; /* 旧実装の読み出し範囲は cells 配列まで（32 列天井） */
            mo_open_push(out, IF_TAG_TABLE, "table", 5);
            mo_text_ch(out, '\n');
            mo_open_push(out, IF_TAG_THEAD, "thead", 5);
            mo_open_push(out, IF_TAG_TR, "tr", 2);
            for (u32 k2 = 0; k2 < nh; k2++) {
                mo_open_push(out, IF_TAG_TH, "th", 2);
                inline_span(out, fn, heads[k2]);
                mo_close(out, "th", 2);
            }
            mo_close(out, "tr", 2);
            mo_close(out, "thead", 5);
            mo_text_ch(out, '\n');
            mo_open_push(out, IF_TAG_TBODY, "tbody", 5);
            mo_text_ch(out, '\n');
            i += 2;
            while (i < hi && !ln_blank(ls[i])) {
                bool pipe2 = false;
                for (u32 k = 0; k < ls[i].n; k++) if (ls[i].p[k] == '|') { pipe2 = true; break; }
                if (!pipe2) break;
                IfStr cells[32];
                u32 nc = split_cells(ls[i], cells, 32);
                if (nc > 32) nc = 32;
                mo_open_push(out, IF_TAG_TR, "tr", 2);
                for (u32 k2 = 0; k2 < nc; k2++) {
                    mo_open_push(out, IF_TAG_TD, "td", 2);
                    inline_span(out, fn, cells[k2]);
                    mo_close(out, "td", 2);
                }
                mo_close(out, "tr", 2);
                mo_text_ch(out, '\n');
                i++;
            }
            mo_close(out, "tbody", 5);
            mo_text_ch(out, '\n');
            mo_close(out, "table", 5);
            mo_text_ch(out, '\n');
            continue;
        }
        /* 段落 */
        u32 j = i;
        while (j < hi) {
            Ln x = ls[j];
            if (ln_blank(x)) break;
            if (j > i) {
                /* 主カスケードと同じ行分類ゲート（必要条件でのみ実行。順序・結果は同値） */
                u32 xsp = 0;
                while (xsp < x.n && x.p[xsp] == ' ') xsp++;
                const u8 xcs = xsp < x.n ? (u8)x.p[xsp] : 0;
                IfStr i2d, i2t;
                if (((xsp == 0 && xcs == '#') && ln_heading(x)) ||
                    ((xcs == '-' || xcs == '*' || xcs == '_' || xcs == '\t') && ln_is_hr(x)) ||
                    (xcs == '>' && ln_quote(x)) ||
                    ((xsp == 0 && xcs == '[') && ln_fndef(x, &i2d, &i2t))) break;
                char s3; LiMark m3;
                if ((xcs == '`' || xcs == '~' || xcs == '\t') && ln_fence(x, &s3)) break;
                if ((xcs == '-' || xcs == '*' || xcs == '+' || (xcs >= '0' && xcs <= '9')) &&
                    ln_list_item(x, &m3)) break;
                if ((xcs == '|' || xcs == '-' || xcs == ':' || xcs == '\t') &&
                    ln_is_table_delim(x)) break;
                /* （かつて '|' 走査があったが副作用のない死コードと判明 → 除去）
                 * 段落継続の '|' 行は表中継ぎではなく普通の段落行として扱う規約は不変。 */
            }
            j++;
        }
        emit_para_lines(out, fn, ls, i, j);
        i = j;
    }
    if (_top) MP_BLOCKS += if_rdtsc_md() - _t0;
    if (mpf()) bw_depth--;
}

static void blocks_str(Mo *out, Fn *fn, IfStr s, u32 depth) {
    u64 _t0 = 0; if (mpf()) _t0 = if_rdtsc_md();
    /* 行配列へ割り切る（入力を切片化、コピーなし）: '\n' 探索は memchr */
    Ln *ls = NULL; u32 n = 0, cap = 0;
    u32 st = 0;
    for (u32 p = 0; p <= s.n;) {
        const char *nl = NULL;
        if (p < s.n) nl = (const char *)memchr(s.p + p, '\n', s.n - p);
        u32 e = nl ? (u32)(nl - s.p) : s.n;
        /* 終端 LF の幻空行は行と数えない */
        if (e == s.n && st == s.n && s.n) break;
        if (n >= cap) {
            u32 c2 = cap ? cap * 2 : 64;
            Ln *nl2 = (Ln *)realloc(ls, (u64)c2 * sizeof(Ln));
            if (!nl2) if_fatal("md: lines oom");
            ls = nl2; cap = c2;
        }
        ls[n].p = s.p + st;
        ls[n].n = e - st;
        n++;
        if (!nl) break;
        st = e + 1;
        p = e + 1;
    }
    if (_t0) MP_LINES += if_rdtsc_md() - _t0;
    blocks_win(out, fn, ls, 0, n, depth);
    free(ls);
}

/* ================= 入口 ================= */

static void run_blocks(Mo *out, Fn *fn, IfStr in) {
    u64 _t0 = 0; if (mpf()) _t0 = if_rdtsc_md();
    /* 正規化: CR/CRLF → LF。'\r' が無ければ入力をそのまま使う（ゼロコピー） */
    const char *cr = (const char *)memchr(in.p, '\r', in.n);
    IfStr s = in;
    if (cr) {
        if (out->is_dom) {
            char *np = (char *)if_arena_alloc(out->a, in.n + 1);
            u64 w = 0;
            for (u32 i = 0; i < in.n; i++) {
                char cc = in.p[i];
                if (cc == '\r') {
                    if (i + 1 < in.n && in.p[i + 1] == '\n') i++;
                    np[w++] = '\n';
                } else np[w++] = cc;
            }
            np[w] = 0;
            mo_range(out, np, (u32)w);
            s = if_str(np, (u32)w);
        } else {
            B norm; b_init(&norm, out->a);
            for (u32 i = 0; i < in.n; i++) {
                char cc = in.p[i];
                if (cc == '\r') {
                    if (i + 1 < in.n && in.p[i + 1] == '\n') i++;
                    b_putc(&norm, '\n');
                } else b_putc(&norm, cc);
            }
            blocks_str(out, fn, if_str(norm.p ? norm.p : "", (u32)norm.n), 0);
            b_drop(&norm);
            goto footnotes;
        }
    }
    if (_t0) { MP_NORM += if_rdtsc_md() - _t0; }
    mo_range(out, s.p, s.n);
    blocks_str(out, fn, s, 0);
footnotes:;
    /* 脚注セクション（参照されたものだけ、参照順） */
    if (fn->n_refs) {
        mo_open(out, IF_TAG_SECTION, "section", 7);
        mo_attr(out, "class", 5, IF_S("footnotes"));
        mo_open_end(out);
        mo_text_ch(out, '\n');
        mo_open_void(out, IF_TAG_HR, "hr", 2);
        mo_text_ch(out, '\n');
        mo_open_push(out, IF_TAG_OL, "ol", 2);
        mo_text_ch(out, '\n');
        for (u32 i = 0; i < fn->n_refs; i++) {
            u32 di = fn_find_def(fn, fn->refs[i]);
            IfStr idv = scratch_cat(&g_scr1, "fn-", 3, fn->refs[i], "", 0);
            IfStr hrv = scratch_cat(&g_scr2, "#fr-", 4, fn->refs[i], "", 0);
            mo_open(out, IF_TAG_LI, "li", 2);
            mo_attr(out, "id", 2, idv);
            mo_open_end(out);
            IfStr txt = di != UINT32_MAX ? fn->defs[di].text : if_str("", 0);
            inline_span(out, fn, txt);
            mo_text_ch(out, ' ');
            mo_open(out, IF_TAG_A, "a", 1);
            mo_attr(out, "href", 4, hrv);
            mo_open_end(out);
            mo_text(out, IF_S("\xE2\x86\xA9")); /* ↩ */
            mo_close(out, "a", 1);
            mo_close(out, "li", 2);
            mo_text_ch(out, '\n');
        }
        mo_close(out, "ol", 2);
        mo_text_ch(out, '\n');
        mo_close(out, "section", 7);
        mo_text_ch(out, '\n');
    }
}

void if_md_to_html(IfArena *a, IfStr in, IfStr *out_html) {
    Mo m;
    memset(&m, 0, sizeof m);
    m.a = a;
    m.is_dom = false;
    b_init(&m.str, a);
    Fn fn;
    memset(&fn, 0, sizeof fn);
    fn.a = a;
    fn.is_dom = false;
    run_blocks(&m, &fn, in);
    fn_free(&fn);
    IfStr fin = b_finish(&m.str);
    free(m.c_buf);
    out_html->p = fin.p;
    out_html->n = fin.n;
}

/* 高速経路: Markdown → DOM 直構築。
 * taint（T1..T5）を観測したら false を返して中間物を捨てる（呼び出し側が
 * 従来の 2 段経路で処理する。正しさは常に本パーサ側に集約）。 */
static bool if_md_parse_fast_serial_f(IfArena *a, IfStr in, IfDom **out_dom, u8 flags) {
    /* T4: input NUL はトークナイザと意味が分かれるので fallback */
    if (in.n && memchr(in.p, 0, in.n)) return false;
    Mo m;
    memset(&m, 0, sizeof m);
    m.a = a;
    m.is_dom = true;
    m.slim_attrs = (u8)(flags & IF_MD_F_SLIM_ATTRS);
    IfDom *dom = (IfDom *)if_arena_calloc(a, sizeof(IfDom));
    dom->arena = a;
    dom->n_nodes = 1;
    IfNode *root = (IfNode *)if_arena_calloc(a, sizeof(IfNode));
    root->kind = IF_NODE_DOCUMENT;
    dom->root = root;
    /* doctype なし: quirks=true, n_errors=1（本パーサと同じ初期条件） */
    dom->quirks = true;
    dom->n_errors = 1;
    m.dom = dom;
    IfNode *html = (IfNode *)if_arena_calloc(a, sizeof(IfNode));
    html->kind = IF_NODE_ELEMENT; html->tag = IF_TAG_HTML; html->ns = IF_NS_HTML;
    html->u.tag_name = IF_S("html");
    dom->n_nodes++;
    mattach(root, html);
    m.cur = root;
    IfNode *head = (IfNode *)if_arena_calloc(a, sizeof(IfNode));
    head->kind = IF_NODE_ELEMENT; head->tag = IF_TAG_HEAD; head->ns = IF_NS_HTML;
    head->u.tag_name = IF_S("head");
    dom->n_nodes++;
    mattach(html, head);
    IfNode *body = (IfNode *)if_arena_calloc(a, sizeof(IfNode));
    body->kind = IF_NODE_ELEMENT; body->tag = IF_TAG_BODY; body->ns = IF_NS_HTML;
    body->u.tag_name = IF_S("body");
    dom->n_nodes++;
    mattach(html, body);
    m.stk[0] = html;
    m.stk[1] = body;
    m.sp = 2;
    m.cur = body;
    m.n_nodes = dom->n_nodes;

    Fn fn;
    memset(&fn, 0, sizeof fn);
    fn.a = a;
    fn.is_dom = true;

    run_blocks(&m, &fn, in);
    run_flush(&m);
    fn_free(&fn);
    free(m.c_buf);
    if (m.tainted) return false;
    dom->n_nodes = m.n_nodes;
    /* 純ブロック容器直下の ws-only TEXT を意図的に剥がした DOM（INV: 描画不寄与物は
     * DOM しない）。layout はこのビットを見て、当該容器内の兄弟マージン相殺を
     * 「ws TEXT が間にあった旧 DOM と逐語同じ」結果（相殺無効＝mt のみ）に補正する。 */
    dom->md_ws_stripped = 1;
    *out_dom = dom;
    return true;
}

/* ---- 2-way 並列 fast parse ----
 * 分割スキャン: fence 状態機械を真に追跡し、「直前が空行 ∧ fence 外 ∧ 先頭非空行」
 * の最初の行頭（入力中央以降）を分割点として返す。見つからなければ NULL。
 * 同値証明の骨格:
 *  - md のブロック構造（段落/リスト/表/引用/脚注定義）は全て真の空行で硬く壁に
 *    区切られる（空行を跨いで継続する構造は実装上存在しない。引用は '>' 非空行のみ
 *    継続、リスト children は非空 ∧ 深インデントのみ継続、表は空行で終了）。
 *  - 唯一空行を跨ぐのは fence（```/~~~）のみ。depth-0 の fence 状態は「先頭非空白が
 *    `/~ で 3 連以上」の行のトグルで追跡できる（ln_fence と同じ述語を再適用）。
 *    引用/リスト内の fence は '>'/マーカー/深インデントを持ち、この述語では
 *    開かないか、偶発的に開いても「余分に開く方向」にしか働かない（保守側。
 *    真の空行を nested fence が跨ぐことはない → 空行直上の分割点では常に
 *    シミュレーションと実パーサの fence 状態が一致する）。
 *  - 脚注は "[^" が入力に無いことを別査定し、参照番号の文書順共有状態を排除。
 *    （fence 内テキスト中の "[^" も保守的に検出して単走査へ逃げる）
 * 検証は oracle sha256（2mb/16mb）＋全テストが機械固定する。 */
static const char *md_par_scan(const char *p, u32 n) {
    /* 脚注ゲート＋CR ゲート＋fence 状態機械を 1 本の SIMD 前パスで済ませる。
     * （memchr-per-line/per-'[' の呼出コストを構造消去。見つからなければ NULL） */
    bool in_fence = false;
    char fsym = 0;
    bool prev_blank = true; /* 先頭はブロック境界として開始 */
    u32 off = 0;            /* 現在行の先頭 */
    u32 i = 0;
#ifdef IF_MD_SIMD
    const __m128i v_nl = _mm_set1_epi8('\n'), v_cr = _mm_set1_epi8('\r'),
                  v_ca = _mm_set1_epi8('^');
    for (; i + 16 <= n; i += 16) {
        __m128i b = _mm_loadu_si128((const __m128i *)(p + i));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(b, v_cr))) return NULL; /* CR → 単走査（正規化が必要） */
        unsigned carets = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(b, v_ca));
        while (carets) { /* "[^" 検出は希少文字 '^' 駆動（隣接確認のみ） */
            u32 k = (u32)__builtin_ctz(carets);
            carets &= carets - 1;
            if (i + k > 0 && p[i + k - 1] == '[') return NULL;
        }
        unsigned nls = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(b, v_nl));
        while (nls) {
            u32 k = (u32)__builtin_ctz(nls);
            nls &= nls - 1;
            u32 e = i + k; /* 行 [off, e) を確定 */
            Ln l = { p + off, e - off };
            u32 w = 0;
            while (w < l.n && (l.p[w] == ' ' || l.p[w] == '\t')) w++;
            bool blank = (w == l.n); /* ln_blank は ' '/'\t' のみ空白視 → 同値 */
            if (in_fence) {
                if (!blank) {
                    char s2;
                    if (ln_fence(l, &s2) && s2 == fsym) in_fence = false;
                }
            } else {
                if (!blank) {
                    char s2;
                    if (ln_fence(l, &s2)) { in_fence = true; fsym = s2; }
                    else if (prev_blank && off >= n / 2)
                        return p + off; /* 最初の安全なブロック境界（中央以降） */
                }
            }
            prev_blank = blank;
            off = e + 1;
        }
    }
#endif
    for (; i < n; i++) { /* 尾（SIMD 未対応環境は全行こちら） */
        char c = p[i];
        if (c == '\r') return NULL;
        if (c == '^' && i > 0 && p[i - 1] == '[') return NULL;
        if (c != '\n') continue;
        u32 e = i;
        Ln l = { p + off, e - off };
        u32 w = 0;
        while (w < l.n && (l.p[w] == ' ' || l.p[w] == '\t')) w++;
        bool blank = (w == l.n);
        if (in_fence) {
            if (!blank) {
                char s2;
                if (ln_fence(l, &s2) && s2 == fsym) in_fence = false;
            }
        } else {
            if (!blank) {
                char s2;
                if (ln_fence(l, &s2)) { in_fence = true; fsym = s2; }
                else if (prev_blank && off >= n / 2)
                    return p + off;
            }
        }
        prev_blank = blank;
        off = e + 1;
    }
    return NULL;
}

typedef struct {
    IfArena *a;
    IfDom *dom;
    IfNode *html, *stub;
    const char *full_p;
    u32 full_n;
    IfStr slice;
    bool tainted;
    u32 n_nodes;
    u8 slim_attrs;
} MdSliceJob;

static void *md_slice_run(void *arg) {
    MdSliceJob *j = (MdSliceJob *)arg;
    Mo mb;
    memset(&mb, 0, sizeof mb);
    mb.a = j->a;
    mb.is_dom = true;
    mb.slim_attrs = j->slim_attrs;
    mb.dom = j->dom;
    mb.stk[0] = j->html;
    mb.stk[1] = j->stub;
    mb.sp = 2;
    mb.cur = j->stub;
    mo_range(&mb, j->full_p, j->full_n);
    Fn fnb;
    memset(&fnb, 0, sizeof fnb);
    fnb.a = j->a;
    fnb.is_dom = true;
    blocks_str(&mb, &fnb, j->slice, 0);
    run_flush(&mb);
    fn_free(&fnb);
    free(mb.c_buf);
    j->tainted = mb.tainted;
    j->n_nodes = mb.n_nodes;
    free(g_scr1); free(g_scr2); /* TLS スクラッチはワーカ寿命で解放（LSan 清浄） */
    g_scr1 = g_scr2 = NULL;
    return NULL;
}

/* DOM 直構築の入口: 安全分割点が見つかれば 2-way 並列、なければ従来の単走査。
 * 並列でも生成される DOM/フラグは単走査と逐語同値（分割点の証明は md_par_scan、
 * 接合規約は下記。検証: oracle sha256＋全テスト＋ASan が機械固定）。 */
bool if_md_parse_fast_f(IfArena *a, IfStr in, IfDom **out_dom, u8 flags) {
    const u8 slim = (u8)(flags & IF_MD_F_SLIM_ATTRS);
    if (in.n >= (1u << 20)) {
        const char *ep = getenv("IF_MD_PAR");
        bool par_on = !(ep && ep[0] == '0'); /* 殺しスイッチ（調査用。既定は並列） */
        if (par_on && !(in.n && memchr(in.p, 0, in.n)) && !memchr(in.p, '\r', in.n)) {
            const char *splitp = md_par_scan(in.p, in.n);
            if (splitp && splitp > in.p) {
                /* scaffold（serial と同一形状をここでも構築） */
                Mo m;
                memset(&m, 0, sizeof m);
                m.a = a;
                m.is_dom = true;
                m.slim_attrs = slim;
                IfDom *dom = (IfDom *)if_arena_calloc(a, sizeof(IfDom));
                dom->arena = a;
                dom->n_nodes = 1;
                IfNode *root = (IfNode *)if_arena_calloc(a, sizeof(IfNode));
                root->kind = IF_NODE_DOCUMENT;
                dom->root = root;
                dom->quirks = true;
                dom->n_errors = 1;
                m.dom = dom;
                IfNode *html = (IfNode *)if_arena_calloc(a, sizeof(IfNode));
                html->kind = IF_NODE_ELEMENT; html->tag = IF_TAG_HTML; html->ns = IF_NS_HTML;
                html->u.tag_name = IF_S("html");
                dom->n_nodes++;
                mattach(root, html);
                IfNode *head = (IfNode *)if_arena_calloc(a, sizeof(IfNode));
                head->kind = IF_NODE_ELEMENT; head->tag = IF_TAG_HEAD; head->ns = IF_NS_HTML;
                head->u.tag_name = IF_S("head");
                dom->n_nodes++;
                mattach(html, head);
                IfNode *body = (IfNode *)if_arena_calloc(a, sizeof(IfNode));
                body->kind = IF_NODE_ELEMENT; body->tag = IF_TAG_BODY; body->ns = IF_NS_HTML;
                body->u.tag_name = IF_S("body");
                dom->n_nodes++;
                mattach(html, body);
                m.stk[0] = html;
                m.stk[1] = body;
                m.sp = 2;
                m.cur = body;
                m.n_nodes = dom->n_nodes;

                Fn fn;
                memset(&fn, 0, sizeof fn);
                fn.a = a;
                fn.is_dom = true;

                /* B 側は専用 arena + stub body（mattach が実 body に触れないように） */
                IfArena ab;
                if_arena_init(&ab, 1u << 22);
                IfNode *stub = (IfNode *)if_arena_calloc(&ab, sizeof(IfNode));
                stub->kind = IF_NODE_ELEMENT; stub->tag = IF_TAG_BODY; stub->ns = IF_NS_HTML;
                stub->u.tag_name = IF_S("body");
                MdSliceJob j;
                j.a = &ab; j.dom = dom; j.html = html; j.stub = stub; j.slim_attrs = slim;
                j.full_p = in.p; j.full_n = in.n;
                j.slice = if_str(splitp, (u32)(in.p + in.n - splitp));
                j.tainted = false; j.n_nodes = 0;

                pthread_t th;
                int rc = pthread_create(&th, NULL, md_slice_run, &j);
                mo_range(&m, in.p, in.n);
                blocks_str(&m, &fn, if_str(in.p, (u32)(splitp - in.p)), 0);
                run_flush(&m);
                if (rc == 0) pthread_join(th, NULL);
                else md_slice_run(&j); /* 生成失敗時は同じ分割を直列実行（結果は同値） */
                fn_free(&fn);
                free(m.c_buf);
                u32 total = m.n_nodes + j.n_nodes;
                bool tainted = m.tainted || j.tainted || total >= IF_MAX_DOM_NODES;
                if_arena_absorb(a, &ab); /* B 側の全確保を主 arena の寿命に畳む */
                if_arena_destroy(&ab);
                if (tainted) return false; /* 2 段経路が同じ結論へ至る（T5 含む） */
                /* body 子列の接合: A 末尾 → B stub 先頭。B 直下の parent を実 body へ */
                if (stub->first_child) {
                    if (!body->first_child) body->first_child = stub->first_child;
                    else body->last_child->next_sibling = stub->first_child;
                    body->last_child = stub->last_child;
                    for (IfNode *c = stub->first_child; c; c = c->next_sibling)
                        c->parent = body;
                }
                dom->n_nodes = total;
                dom->md_ws_stripped = 1;
                *out_dom = dom;
                return true;
            }
        }
    }
    return if_md_parse_fast_serial_f(a, in, out_dom, flags);
}

bool if_md_parse_fast(IfArena *a, IfStr in, IfDom **out_dom) {
    return if_md_parse_fast_f(a, in, out_dom, 0);
}

