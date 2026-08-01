/* Ifuto — HTML ツリービルダ（WHATWG insertion modes 実装）
 *
 * 実装済み: quirks モード判定（DOCTYPE 完全表。limited-quirks は no-quirks 同効）、
 * foster parenting、table 挿入モード群、active formatting elements + adoption agency、
 * frameset モード群、foreign content。
 *
 * 現存する偏差（ARCHITECTURE.md 台帳に集約）:
 *   - \"in template\" 挿入モードは未採用（vendored dataset 世代との乖離、台帳注）
 *   - \"in select\" 系は現行仕様の in-body 統合規則に追従済み
 *
 * 敵対防御: 深さ上限・ノード総数上限・単調進行の保証は tokenizer 側が請け負う。
 */
#include "html_int.h"
#include "utf8.h"
#include <string.h>

typedef enum {
    M_INITIAL, M_BEFORE_HTML, M_BEFORE_HEAD, M_IN_HEAD, M_AFTER_HEAD, M_IN_BODY, M_AFTER_BODY,
    M_AFTER_AFTER_BODY,
    M_IN_TABLE, M_IN_CAPTION, M_IN_COLUMN_GROUP, M_IN_TABLE_BODY, M_IN_ROW, M_IN_CELL,
    M_IN_FRAMESET, M_AFTER_FRAMESET, M_AFTER_AFTER_FRAMESET
} IfMode;

typedef struct {
    IfArena *arena;
    IfDom *dom;
    IfHtmlTok tok;
    IfMode mode;
    IfNode **stack;
    u32 depth;
    u64 cap;
    IfNode *html, *head, *body;
    IfNode *form;          /* form element pointer（<form> ネスト制御用。WPT の挙動根拠） */
    bool stopped;
    bool seen_doctype;
    u8 skip_lf; /* pre/listing/xmp 直後の LF 1 個を無視 */
    /* foreign end-tag が HTML 要素に到達した際の「現モード HTML 規則で再処理」を
     * 1 トークン分だけ foreign 再判定から守る旗（<math></html> の相互再帰=SEGV 防止） */
    bool no_foreign;
    /* foster parenting: table 系モードの "anything else" 転送時のみ立つ旗。
     * place() は旗が立ち、かつ現在ノードが table/tbody/tfoot/thead/tr のときだけ
     * 「table の兄」の位置を選ぶ（WHATWG 12.2.6.1 相当） */
    bool foster;
    /* in-table text の保留バッファ（非空白判定を遅延させるための仕様形） */
    char *pend;
    u32 pend_n, pend_cap;
    bool pend_nonws;
    /* frameset-ok flag（WHATWG 13.2.4.5）。「ok」のときのみ in-body の
     * <frameset> が body を置き換える。テキスト(非空白)/各種要素で not-ok へ */
    bool frameset_ok;
    /* stack of template insertion modes（template がネストするごとに積む平行スタック） */
    u8 tpl_modes[64];
    u32 n_tpl;
    /* list of active formatting elements（AFE）。marker=true はスコープ境界
     * （td/th/caption/template/object/applet/marquee で挿入） */
    struct IfAfE { IfNode *n; bool marker; } *afe;
    u32 n_afe, afe_cap;
} IfTB;

static IfNode *new_node(IfTB *b, IfNodeKind kind) {
    IfNode *n = (IfNode *)if_arena_calloc(b->arena, sizeof(IfNode));
    n->kind = kind;
    b->dom->n_nodes++;
    return n;
}

static void append_child(IfNode *parent, IfNode *child) {
    child->parent = parent;
    if (!parent->first_child) parent->first_child = child;
    if (parent->last_child) parent->last_child->next_sibling = child;
    parent->last_child = child;
}

/* ---- slim-DOM（法則「画面描画に関係ないものは DOM しない」、dom.h 参照） ----
 * 剃るのは script / template の *子孫と本文のみ*。style は cascade が読むので残す。
 * root 要素自体は marker として接続する（構造の可観測性・パース規則の不変性のため）。
 * 子孫の生成ノードは stack 規則のため生成自体は行うが木に接続しない
 * （stack/pop/scope 系は全て stack 走査なので挙動不変）。
 * 判定は stack 走査（foster/insertion-mode で parent 鎖がずれても current で正しい）。 */
static bool under_slim(IfTB *b) {
    if (!if_dom_slim) return false;
    for (u32 i = b->depth; i > 0; i--)
        if (b->stack[i - 1]->flags & IF_NF_SLIM) return true;
    return false;
}
static bool slim_root_tag(u16 t) { return t == IF_TAG_SCRIPT || t == IF_TAG_TEMPLATE; }

/* ファイル前半のユーティリティで参照される下方定義の前方宣言群 */
static IfNode *top(IfTB *b);
static void pop(IfTB *b);
static void insert_comment(IfTB *b, IfNode *parent, const IfTok *tok);
static void insert_comment_placed(IfTB *b, const IfTok *tok);
static void step_in_body(IfTB *b, IfTok tok);
static void step_in_head(IfTB *b, IfTok tok);
static void step(IfTB *b, IfTok tok);
static void tpl_end(IfTB *b);
static void tpl_start(IfTB *b, const IfTok *tok);
static void gen_implied(IfTB *b, u16 except);
static void pop_until(IfTB *b, u16 tag);
static bool is_formatting(u16 t);
typedef enum { SC_DEFAULT = 0, SC_BUTTON, SC_LIST_ITEM, SC_TABLE } IfScopeKind;
static bool has_in_scope2(IfTB *b, u16 tag, IfScopeKind k);
static bool scope_barrier(const IfNode *n, IfScopeKind k);
static void afe_reconstruct(IfTB *b);
static void afe_push(IfTB *b, IfNode *n);
static void afe_insert_marker(IfTB *b);
static void afe_clear_to_marker(IfTB *b);
static bool has_in_default_scope_tag(IfTB *b, u16 tag);
static void adoption(IfTB *b, u16 tag);
static void any_other_end_tag(IfTB *b, u16 tag);
static bool has_open(IfTB *b, u16 tag);

/* before の直前に挿入（foster parenting の「table の兄」経路で必要） */
static void insert_child_before(IfNode *parent, IfNode *child, IfNode *before) {
    if (!before) { append_child(parent, child); return; }
    child->parent = parent;
    child->next_sibling = before;
    if (parent->first_child == before) {
        parent->first_child = child;
    } else {
        IfNode *p = parent->first_child;
        while (p && p->next_sibling != before) p = p->next_sibling;
        if (p) p->next_sibling = child;
    }
    /* before が last_child のまま残る（insert は last を変えない） */
    if (!parent->first_child) parent->first_child = child;
}

/* 適切な挿入位置: 通常は現在ノード末尾（現在ノードが template なら content
 * フラグメントの末尾）。foster 時は (table の親, table の前) か template の content。 */
typedef struct { IfNode *parent; IfNode *before; } IfPlace;
static IfPlace place(IfTB *b) {
    IfNode *target = top(b);
    IfPlace r = { target, NULL };
    if (!b->foster) {
        if (target->tag == IF_TAG_TEMPLATE && target->content) r.parent = target->content;
        return r;
    }
    switch (target->tag) {
    case IF_TAG_TABLE: case IF_TAG_TBODY: case IF_TAG_TFOOT:
    case IF_TAG_THEAD: case IF_TAG_TR:
        break;
    default:
        if (target->tag == IF_TAG_TEMPLATE && target->content) r.parent = target->content;
        return r;
    }
    i32 iti = -1, ita = -1;
    for (i32 i = (i32)b->depth - 1; i >= 0; i--) {
        if (iti < 0 && b->stack[i]->tag == IF_TAG_TEMPLATE) iti = i;
        if (ita < 0 && b->stack[i]->tag == IF_TAG_TABLE) ita = i;
    }
    if (iti >= 0 && (ita < 0 || iti > ita)) {
        IfNode *t = b->stack[iti];
        r.parent = t->content ? t->content : t; /* foster 時は最新 template の content */
        return r;
    }
    if (ita < 0) { /* table 不在: fragment 相当。body があれば body、無ければ html */
        r.parent = b->body ? b->body : (b->html ? b->html : top(b));
        return r;
    }
    IfNode *tab = b->stack[ita];
    if (tab->parent) { r.parent = tab->parent; r.before = tab; return r; }
    r.parent = ita > 0 ? b->stack[ita - 1] : b->dom->root;
    return r;
}

static void append_placed(IfTB *b, IfNode *n) {
    IfPlace pl = place(b);
    if (pl.before) insert_child_before(pl.parent, n, pl.before);
    else append_child(pl.parent, n);
}

static void push(IfTB *b, IfNode *n) {
    if (b->depth >= b->cap) {
        b->stack = (IfNode **)if_arena_grow(b->arena, b->stack, &b->cap, b->depth + 1, sizeof(IfNode *));
    }
    b->stack[b->depth++] = n;
}

static IfNode *top(IfTB *b) { return b->depth ? b->stack[b->depth - 1] : b->dom->root; }

static void pop(IfTB *b) { if (b->depth) b->depth--; }

/* 要素ノード生成。未知タグ名は lowercase 正規化して arena に保持（canonical 名の不変条件）。 */
static IfNode *make_element(IfTB *b, const IfTok *tok) {
    IfNode *n = new_node(b, IF_NODE_ELEMENT);
    n->tag = tok->tag;
    if (tok->tag == IF_TAG_UNKNOWN) {
        char *lc = (char *)if_arena_alloc(b->arena, (u64)tok->tag_raw.n + 1);
        for (u32 i = 0; i < tok->tag_raw.n; i++) lc[i] = (char)if_ascii_lower((u8)tok->tag_raw.p[i]);
        lc[tok->tag_raw.n] = 0;
        n->u.tag_name = if_str(lc, tok->tag_raw.n);
    } else {
        const char *s = if_tag_name(tok->tag);
        n->u.tag_name = if_str(s, (u32)strlen(s));
    }
    n->attrs = tok->attrs;
    n->n_attrs = tok->n_attrs;
    return n;
}

static void insert_element(IfTB *b, const IfTok *tok, bool do_push) {
    IfNode *n = make_element(b, tok);
    if (if_dom_slim) {
        bool u = under_slim(b);
        if (u || slim_root_tag(tok->tag)) n->flags |= IF_NF_SLIM;
        /* 剃り領域の子孫は木に接続しない（node は stack 規則のため残る）。 */
        if (!u) append_placed(b, n);
    } else {
        append_placed(b, n); /* foster 時は「table の兄」に置く（push 先は stack で不変） */
    }
    if (do_push && b->depth < IF_MAX_STACK_DEPTH) push(b, n);
    else if (do_push) b->dom->n_errors++;
}

/* 現在ノードが指定タグなら pop */
static void pop_if(IfTB *b, u16 tag) {
    if (b->depth && top(b)->tag == tag) pop(b);
}

/* 「p を閉じる」べきブロック開始タグ群 */
static bool closes_p(u16 t) {
    switch (t) {
    case IF_TAG_P: case IF_TAG_DIV: case IF_TAG_H1: case IF_TAG_H2: case IF_TAG_H3:
    case IF_TAG_H4: case IF_TAG_H5: case IF_TAG_H6: case IF_TAG_UL: case IF_TAG_OL:
    case IF_TAG_DL: case IF_TAG_PRE: case IF_TAG_BLOCKQUOTE: case IF_TAG_TABLE:
    case IF_TAG_FORM: case IF_TAG_FIGURE: case IF_TAG_FIGCAPTION: case IF_TAG_ADDRESS:
    case IF_TAG_ARTICLE: case IF_TAG_ASIDE: case IF_TAG_FOOTER: case IF_TAG_HEADER:
    case IF_TAG_HR: case IF_TAG_MAIN: case IF_TAG_NAV: case IF_TAG_SECTION: case IF_TAG_CENTER:
    case IF_TAG_DETAILS: case IF_TAG_DIALOG: case IF_TAG_HGROUP: case IF_TAG_SEARCH:
    case IF_TAG_SUMMARY:
    case IF_TAG_LISTING: case IF_TAG_PLAINTEXT: case IF_TAG_XMP: case IF_TAG_FIELDSET:
    case IF_TAG_DIR: case IF_TAG_MENU:
        return true;
    default: return false;
    }
}

static bool has_open(IfTB *b, u16 tag) {
    for (u32 i = b->depth; i > 0; i--)
        if (b->stack[i - 1]->tag == tag) return true;
    return false;
}

/* 「p を閉じる」: button スコープで p が見えるときのみ（html/template/marquee/object/applet
 * で遮断。無ければ何もしない = 仕様の if-in-button-scope 条件）。閉じるときは
 * implied end tags を生成してから p まで畳む（WHATWG "close a p element"）。 */
static void close_p_if_open(IfTB *b) {
    if (!has_in_scope2(b, IF_TAG_P, SC_BUTTON)) return;
    gen_implied(b, IF_TAG_P); /* except=p（自身は畳まない） */
    if (top(b)->tag != IF_TAG_P) b->dom->n_errors++;
    pop_until(b, IF_TAG_P);
}

/* li/dt/dd の暗黙終了: 同名タグを積み遡り、バリアに当たらなければそこまで pop */
static void implied_close(IfTB *b, u16 a, u16 b2, u16 barrier1, u16 barrier2) {
    for (u32 i = b->depth; i > 0; i--) {
        u16 t = b->stack[i - 1]->tag;
        if (t == a || t == b2) {
            while (b->depth > i - 1) pop(b);
            return;
        }
        if (t == barrier1 || t == barrier2) return;
    }
}

static void close_heading_if_open(IfTB *b) {
    if (!b->depth) return;
    u16 t = top(b)->tag;
    if (t >= IF_TAG_H1 && t <= IF_TAG_H6) pop(b);
}

static void append_text(IfTB *b, IfStr text) {
    if (text.n == 0) return;
    if (under_slim(b)) return; /* 表示しない subtree の本文は arena 確保すらしない */
    IfPlace pl = place(b);
    /* before がある（foster の兄挿入）時は「直前の兄が TEXT ならそこに連結」。
     * 無ければ親の末尾 TEXT に連結（従来の規則）。 */
    IfNode *merge_to = NULL;
    if (pl.before) {
        IfNode *s = pl.parent->first_child;
        while (s && s->next_sibling != pl.before) s = s->next_sibling;
        if (s && s->kind == IF_NODE_TEXT) merge_to = s;
    } else {
        IfNode *last = pl.parent->last_child;
        if (last && last->kind == IF_NODE_TEXT) merge_to = last;
    }
    if (merge_to) {
        u64 nn = (u64)merge_to->u.text.n + text.n;
        char *buf = (char *)if_arena_alloc(b->arena, nn);
        memcpy(buf, merge_to->u.text.p, merge_to->u.text.n);
        memcpy(buf + merge_to->u.text.n, text.p, text.n);
        merge_to->u.text = if_str(buf, (u32)nn);
        return;
    }
    IfNode *n = new_node(b, IF_NODE_TEXT);
    n->u.text = text;
    if (pl.before) insert_child_before(pl.parent, n, pl.before);
    else append_child(pl.parent, n);
}

/* in-table text の保留バッファ */
static void pend_add(IfTB *b, IfStr text) {
    if (!text.n) return;
    if (under_slim(b)) return; /* 保留 buffer にすら溜めない（append_text でも落ちる） */
    if (b->pend_n + text.n > b->pend_cap) {
        u32 ncap = b->pend_cap ? b->pend_cap : 64;
        while (ncap < b->pend_n + text.n) ncap *= 2;
        char *np = (char *)if_arena_alloc(b->arena, ncap);
        if (b->pend_n) memcpy(np, b->pend, b->pend_n);
        b->pend = np;
        b->pend_cap = ncap;
    }
    memcpy(b->pend + b->pend_n, text.p, text.n);
    b->pend_n += text.n;
    if (!if_str_is_ws_only(text)) b->pend_nonws = true;
}

static void pend_reset(IfTB *b) { b->pend_n = 0; b->pend_nonws = false; }

/* 保留テキストを実 DOM に流す（全空白 → 現在ノードへ。非空白混じり → foster で body 規則） */
static void pend_flush(IfTB *b);

/* ---- table 系モード用のユーティリティ（WHATWG 12.2.6.4.7 系の実装中核） ---- */

static bool tag_in(u16 t, const u16 *a, u32 n) {
    for (u32 i = 0; i < n; i++) if (a[i] == t) return true;
    return false;
}

/* scope 判定: tag が stack 上で stops に遮られずに見えるか（下向き走査） */

/* ---- scope 系の正式形（WHATWG "has an element in scope" 13.2.4.2 系） ----
 * バリア集合は namespace 修飾つき: 既定スコープの遮断要素は
 *   HTML: applet, caption, html, table, td, th, marquee, object, template
 *   MathML: mi, mo, mn, ms, mtext, annotation-xml
 *   SVG: foreignObject, desc, title
 * で、これを button / list-item / table scope が拡張・置換する。
 * マッチ対象は常に「HTML 名前空間の tag」要素（foreign 同名要素を誤検しない）。
 * IfScopeKind 自体はファイル先頭で宣言済み。 */
static bool scope_barrier(const IfNode *n, IfScopeKind k) {
    if (k == SC_TABLE)
        return n->ns == IF_NS_HTML && (n->tag == IF_TAG_HTML || n->tag == IF_TAG_TABLE ||
                                       n->tag == IF_TAG_TEMPLATE);
    if (n->ns == IF_NS_HTML) {
        if (k == SC_BUTTON && n->tag == IF_TAG_BUTTON) return true;
        if (k == SC_LIST_ITEM && (n->tag == IF_TAG_OL || n->tag == IF_TAG_UL)) return true;
        switch (n->tag) {
        case IF_TAG_APPLET: case IF_TAG_CAPTION: case IF_TAG_HTML: case IF_TAG_TABLE:
        case IF_TAG_TD: case IF_TAG_TH: case IF_TAG_MARQUEE: case IF_TAG_OBJECT:
        case IF_TAG_TEMPLATE:
            return true;
        default: return false;
        }
    }
    if (n->ns == IF_NS_MATHML)
        return n->tag == IF_TAG_MI || n->tag == IF_TAG_MO || n->tag == IF_TAG_MN ||
               n->tag == IF_TAG_MS || n->tag == IF_TAG_MTEXT || n->tag == IF_TAG_ANNOTATION_XML;
    if (n->ns == IF_NS_SVG)
        return n->tag == IF_TAG_FOREIGNOBJECT || n->tag == IF_TAG_DESC ||
               n->tag == IF_TAG_TITLE;
    return false;
}

static bool has_in_scope2(IfTB *b, u16 tag, IfScopeKind k) {
    for (u32 i = b->depth; i > 0; i--) {
        IfNode *x = b->stack[i - 1];
        if (x->ns == IF_NS_HTML && x->tag == tag) return true;
        if (scope_barrier(x, k)) return false;
    }
    return false;
}

static bool has_in_table_scope(IfTB *b, u16 tag) {
    return has_in_scope2(b, tag, SC_TABLE);
}

static void clear_back(IfTB *b, const u16 *ctx, u32 n) {
    while (b->depth && !tag_in(top(b)->tag, ctx, n)) pop(b);
}

static const u16 C_TABLE[] = { IF_TAG_TABLE, IF_TAG_TEMPLATE, IF_TAG_HTML };
static const u16 C_TBODY[] = { IF_TAG_TBODY, IF_TAG_TFOOT, IF_TAG_THEAD, IF_TAG_TEMPLATE, IF_TAG_HTML };
static const u16 C_TR[] = { IF_TAG_TR, IF_TAG_TEMPLATE, IF_TAG_HTML };
#define C_TABLE_N ((u32)(sizeof C_TABLE / sizeof C_TABLE[0]))
#define C_TBODY_N ((u32)(sizeof C_TBODY / sizeof C_TBODY[0]))
#define C_TR_N    ((u32)(sizeof C_TR / sizeof C_TR[0]))

static void pop_until(IfTB *b, u16 tag) {
    while (b->depth) {
        u16 t = top(b)->tag;
        pop(b);
        if (t == tag) break;
    }
}

/* 「暗示終了タグを生成する」: dd/dt/li/optgroup/option/p/rp/rt が top の間 pop（except は除外） */
static void gen_implied(IfTB *b, u16 except) {
    while (b->depth) {
        u16 t = top(b)->tag;
        if (t == except) return;
        switch (t) {
        case IF_TAG_DD: case IF_TAG_DT: case IF_TAG_LI: case IF_TAG_OPTGROUP:
        case IF_TAG_OPTION: case IF_TAG_P: case IF_TAG_RP: case IF_TAG_RT:
            pop(b);
            continue;
        default:
            return;
        }
    }
}

/* thorough 版: 上記に caption/colgroup/tbody/td/tfoot/th/thead/tr を加える
 * （template 終了時の「generate implied end tags thoroughly」） */
static void gen_implied_thorough(IfTB *b) {
    while (b->depth) {
        switch (top(b)->tag) {
        case IF_TAG_CAPTION: case IF_TAG_COLGROUP: case IF_TAG_DD: case IF_TAG_DT:
        case IF_TAG_LI: case IF_TAG_OPTGROUP: case IF_TAG_OPTION: case IF_TAG_P:
        case IF_TAG_RP: case IF_TAG_RT: case IF_TAG_TBODY: case IF_TAG_TD:
        case IF_TAG_TFOOT: case IF_TAG_TH: case IF_TAG_THEAD: case IF_TAG_TR:
            pop(b);
            continue;
        default:
            return;
        }
    }
}

/* reset_mode/pop_until はこのブロックの下方にあるため前方宣言 */
static void reset_mode(IfTB *b);
static void pop_until(IfTB *b, u16 tag);
static bool has_open(IfTB *b, u16 tag);

/* template の開始: 要素+content フラグメントを作り、template 挿入モードを推して遷移 */
static void tpl_start(IfTB *b, const IfTok *tok) {
    afe_insert_marker(b);
    insert_element(b, tok, true);
    IfNode *t = top(b);
    if (!t->content) t->content = new_node(b, IF_NODE_DOCUMENT);
    /* ・現行 WHATWG の "in template" モード（別実装で試作）は vendored template.dat と
     *   系統的に不整合（the data は 2010 年代中盤の旧「appropriate template insertion mode」
     *   世代と読める）。一次 spec 一致より台帳(pin)一致を優先し、in-body で近似: `tpl_set` 試作は台帳注１。
     * ・将来 WPT カレントの fresh データに差し替わった時点で "in template" へ再査定する。
     */
    IfMode m = M_IN_BODY;
    if (b->n_tpl < sizeof b->tpl_modes) b->tpl_modes[b->n_tpl++] = (u8)m;
    b->mode = m;
}

static void tpl_end(IfTB *b) {
    if (!has_open(b, IF_TAG_TEMPLATE)) { b->dom->n_errors++; return; }
    gen_implied_thorough(b);
    if (top(b)->tag != IF_TAG_TEMPLATE) b->dom->n_errors++;
    pop_until(b, IF_TAG_TEMPLATE);
    afe_clear_to_marker(b);
    if (b->n_tpl) b->n_tpl--;
    reset_mode(b);
}

/* reset the insertion mode appropriately（WHATWG アルゴリズムの非 fragment 版） */
static void reset_mode(IfTB *b) {
    for (i32 i = (i32)b->depth - 1; i >= 0; i--) {
        u16 t = b->stack[i]->tag;
        if (i == 0) { /* 基底 html: head 既出なら after-head、未出なら before-head */
            b->mode = b->head ? M_AFTER_HEAD : M_BEFORE_HEAD;
            return;
        }
        switch (t) {
        case IF_TAG_TD: case IF_TAG_TH: b->mode = M_IN_CELL; return;
        case IF_TAG_TR: b->mode = M_IN_ROW; return;
        case IF_TAG_TBODY: case IF_TAG_THEAD: case IF_TAG_TFOOT:
            b->mode = M_IN_TABLE_BODY; return;
        case IF_TAG_CAPTION: b->mode = M_IN_CAPTION; return;
        case IF_TAG_COLGROUP: b->mode = M_IN_COLUMN_GROUP; return;
        case IF_TAG_TABLE: b->mode = M_IN_TABLE; return;
        case IF_TAG_TEMPLATE:
            /* template の挿入モードスタック先頭に戻す（空なら in-body） */
            b->mode = b->n_tpl ? (IfMode)b->tpl_modes[b->n_tpl - 1] : M_IN_BODY;
            return;
        /* 現行仕様: reset は select 分岐を持たない（customizable-select 統合で
         * "in select" / "in select in table" 挿入モードは廃止済み） */
        case IF_TAG_HEAD: b->mode = M_IN_HEAD; return;
        case IF_TAG_BODY: b->mode = M_IN_BODY; return;
        default: continue; /* それ以外の要素はさらに下を見る */
        }
    }
    b->mode = b->head ? M_AFTER_HEAD : M_BEFORE_HEAD;
}

/* table セルを強制閉鎖して M_IN_ROW に戻す（"close the cell"） */
static void close_cell(IfTB *b) {
    if (has_in_table_scope(b, IF_TAG_TD)) {
        gen_implied(b, IF_TAG_TD);
        if (top(b)->tag != IF_TAG_TD) b->dom->n_errors++;
        pop_until(b, IF_TAG_TD);
    } else if (has_in_table_scope(b, IF_TAG_TH)) {
        gen_implied(b, IF_TAG_TH);
        if (top(b)->tag != IF_TAG_TH) b->dom->n_errors++;
        pop_until(b, IF_TAG_TH);
    }
    afe_clear_to_marker(b);
    b->mode = M_IN_ROW;
}

/* ================= AFE (active formatting elements) + AAA =================
 * WHATWG 12.2.6.4 "active formatting elements" / 12.2.6.5 "adoption agency"。
 * 書式要素 (a,b,big,code,em,font,i,nobr,s,small,strike,strong,tt,u) の
 * 誤ネストを「クローンで再構成」し、終了タグでは adoption agency で修復する。
 */

static bool is_formatting(u16 t) {
    switch (t) {
    case IF_TAG_A: case IF_TAG_B: case IF_TAG_BIG: case IF_TAG_CODE: case IF_TAG_EM:
    case IF_TAG_FONT: case IF_TAG_I: case IF_TAG_NOBR: case IF_TAG_S: case IF_TAG_SMALL:
    case IF_TAG_STRIKE: case IF_TAG_STRONG: case IF_TAG_TT: case IF_TAG_U:
        return true;
    default:
        return false;
    }
}

/* "special element" カテゴリ（AAA の furthest block 判定・any-other-end の barrier） */
static bool is_special(const IfNode *n) {
    if (n->kind != IF_NODE_ELEMENT) return false;
    if (n->ns == IF_NS_MATHML) {
        switch (n->tag) {
        case IF_TAG_MI: case IF_TAG_MO: case IF_TAG_MN: case IF_TAG_MS:
        case IF_TAG_MTEXT: case IF_TAG_ANNOTATION_XML:
            return true;
        default: return false;
        }
    }
    if (n->ns == IF_NS_SVG) {
        return n->tag == IF_TAG_FOREIGNOBJECT || n->tag == IF_TAG_DESC || n->tag == IF_TAG_TITLE;
    }
    switch (n->tag) {
    case IF_TAG_ADDRESS: case IF_TAG_APPLET: case IF_TAG_AREA: case IF_TAG_ARTICLE:
    case IF_TAG_ASIDE: case IF_TAG_BASE: case IF_TAG_BASEFONT: case IF_TAG_BLOCKQUOTE:
    case IF_TAG_BODY: case IF_TAG_BR: case IF_TAG_BUTTON: case IF_TAG_CAPTION:
    case IF_TAG_CENTER: case IF_TAG_COL: case IF_TAG_COLGROUP: case IF_TAG_DD:
    case IF_TAG_DIR: case IF_TAG_DIV: case IF_TAG_DL: case IF_TAG_DT: case IF_TAG_EMBED:
    case IF_TAG_FIELDSET: case IF_TAG_FIGCAPTION: case IF_TAG_FIGURE: case IF_TAG_FOOTER:
    case IF_TAG_FORM: case IF_TAG_FRAME: case IF_TAG_FRAMESET:
    case IF_TAG_H1: case IF_TAG_H2: case IF_TAG_H3: case IF_TAG_H4: case IF_TAG_H5: case IF_TAG_H6:
    case IF_TAG_HEAD: case IF_TAG_HEADER: case IF_TAG_HR: case IF_TAG_HTML:
    case IF_TAG_IFRAME: case IF_TAG_IMG: case IF_TAG_INPUT: case IF_TAG_KEYGEN:
    case IF_TAG_LI: case IF_TAG_LINK: case IF_TAG_LISTING: case IF_TAG_MAIN:
    case IF_TAG_MARQUEE: case IF_TAG_MENU: case IF_TAG_META: case IF_TAG_NAV:
    case IF_TAG_NOEMBED: case IF_TAG_NOFRAMES: case IF_TAG_NOSCRIPT: case IF_TAG_OBJECT:
    case IF_TAG_OL: case IF_TAG_P: case IF_TAG_PARAM: case IF_TAG_PLAINTEXT:
    case IF_TAG_PRE: case IF_TAG_SCRIPT: case IF_TAG_SECTION: case IF_TAG_SELECT:
    case IF_TAG_SOURCE: case IF_TAG_STYLE: case IF_TAG_TABLE: case IF_TAG_TBODY:
    case IF_TAG_TD: case IF_TAG_TEMPLATE: case IF_TAG_TEXTAREA: case IF_TAG_TFOOT:
    case IF_TAG_DETAILS: case IF_TAG_DIALOG: case IF_TAG_HGROUP: case IF_TAG_SEARCH:
    case IF_TAG_SUMMARY:
    case IF_TAG_TH: case IF_TAG_THEAD: case IF_TAG_TITLE: case IF_TAG_TR:
    case IF_TAG_TRACK: case IF_TAG_UL: case IF_TAG_WBR: case IF_TAG_XMP:
        return true;
    default:
        return false;
    }
}

static i32 stack_find_node(const IfTB *b, const IfNode *n) {
    for (i32 i = (i32)b->depth - 1; i >= 0; i--)
        if (b->stack[i] == n) return i;
    return -1;
}

/* 既定スコープ（namespace 修飾つき正式バリア集合: scope_barrier 参照） */
static bool node_in_default_scope(const IfTB *b, const IfNode *n) {
    for (u32 i = b->depth; i > 0; i--) {
        IfNode *x = b->stack[i - 1];
        if (x == n) return true;
        if (scope_barrier(x, SC_DEFAULT)) return false;
    }
    return false;
}

static bool has_in_default_scope_tag(IfTB *b, u16 tag) {
    return has_in_scope2(b, tag, SC_DEFAULT);
}

static bool attrs_equal(const IfNode *a, const IfNode *b2) {
    if (a->n_attrs != b2->n_attrs) return false;
    for (u32 i = 0; i < a->n_attrs; i++) {
        bool hit = false;
        for (u32 j = 0; j < b2->n_attrs; j++) {
            if (if_str_eq_ci(a->attrs[i].name, b2->attrs[j].name)
                && if_str_eq_ci(a->attrs[i].value, b2->attrs[j].value)) { hit = true; break; }
        }
        if (!hit) return false;
    }
    return true;
}

/* AFE 配列の容量確保（IfTB::afe_cap を単一の真実として grow に渡す） */
static void afe_ensure(IfTB *b) {
    if (b->n_afe < b->afe_cap) return;
    u64 cap = b->afe_cap;
    b->afe = (struct IfAfE *)if_arena_grow(b->arena, b->afe, &cap, b->n_afe + 1, sizeof b->afe[0]);
    b->afe_cap = (u32)cap;
}

/* AFE リスト末尾へ push。Noah's Ark: 同一 tag/ns/attrs の未閉鎖が 3 件目で最古を除去 */
static void afe_push(IfTB *b, IfNode *n) {
    u32 eq = 0;
    i32 oldest = -1;
    for (i32 i = (i32)b->n_afe - 1; i >= 0; i--) {
        if (b->afe[i].marker) break;
        IfNode *x = b->afe[i].n;
        if (x->tag == n->tag && x->ns == n->ns && attrs_equal(x, n)) {
            eq++;
            if (oldest < 0) oldest = i;
            else oldest = i; /* 下へ行くほど古い */
        }
    }
    if (eq >= 3) { /* 3 件規則: 最古（最小 index）を除去 */
        memmove(&b->afe[oldest], &b->afe[oldest + 1], (size_t)(b->n_afe - (u32)oldest - 1) * sizeof b->afe[0]);
        b->n_afe--;
    }
    afe_ensure(b);
    b->afe[b->n_afe].n = n;
    b->afe[b->n_afe].marker = false;
    b->n_afe++;
}

static void afe_insert_marker(IfTB *b) {
    IfNode *sentinel = b->dom->root; /* marker は「ノードではない」ので root を番兵に */
    afe_ensure(b);
    b->afe[b->n_afe].n = sentinel;
    b->afe[b->n_afe].marker = true;
    b->n_afe++;
}

static void afe_clear_to_marker(IfTB *b) {
    while (b->n_afe && !b->afe[b->n_afe - 1].marker) b->n_afe--;
    if (b->n_afe) b->n_afe--; /* marker 自体も除去 */
}

static i32 afe_find_tag(const IfTB *b, u16 tag) {
    for (i32 i = (i32)b->n_afe - 1; i >= 0; i--) {
        if (b->afe[i].marker) return -1;
        if (b->afe[i].n->tag == tag) return i;
    }
    return -1;
}

static bool afe_has_node(const IfTB *b, const IfNode *n) {
    for (i32 i = (i32)b->n_afe - 1; i >= 0; i--) {
        if (b->afe[i].marker) return false;
        if (b->afe[i].n == n) return true;
    }
    return false;
}

static void afe_remove_at(IfTB *b, u32 i) {
    if (i >= b->n_afe) return;
    memmove(&b->afe[i], &b->afe[i + 1], (size_t)(b->n_afe - i - 1) * sizeof b->afe[0]);
    b->n_afe--;
}

/* 要素の浅コピー（clone 用）: tag/attrs を arena に複製。子・style は引き継がない */
static IfNode *clone_element(IfTB *b, const IfNode *src) {
    IfNode *c = new_node(b, IF_NODE_ELEMENT);
    c->tag = src->tag;
    c->ns = src->ns;
    c->u.tag_name = src->u.tag_name;
    if (src->n_attrs) {
        c->attrs = (IfAttr *)if_arena_alloc(b->arena, (u64)src->n_attrs * sizeof(IfAttr));
        memcpy(c->attrs, src->attrs, (u64)src->n_attrs * sizeof(IfAttr));
        c->n_attrs = src->n_attrs;
    }
    return c;
}

/* DOM 上の親子リストから外す（AAA でノードを移動するときの detach） */
static void detach(IfNode *n) {
    IfNode *p = n->parent;
    if (!p) return;
    if (p->first_child == n) {
        p->first_child = n->next_sibling;
        if (p->last_child == n) p->last_child = NULL;
    } else {
        IfNode *x = p->first_child;
        while (x && x->next_sibling != n) x = x->next_sibling;
        if (x) x->next_sibling = n->next_sibling;
        if (p->last_child == n) p->last_child = x;
    }
    n->parent = NULL;
    n->next_sibling = NULL;
}

static void move_children(IfNode *src, IfNode *dst) {
    IfNode *c = src->first_child;
    while (c) {
        IfNode *nx = c->next_sibling;
        c->parent = NULL;
        c->next_sibling = NULL;
        append_child(dst, c);
        c = nx;
    }
    src->first_child = src->last_child = NULL;
}

/* 適切な挿入位置を「任意 target に対して」計算（AAA の再 anchor で使用） */
static IfPlace place_for(IfTB *b, IfNode *target) {
    IfPlace r = { target, NULL };
    if (target->tag == IF_TAG_TEMPLATE && target->content) r.parent = target->content;
    switch (target->tag) {
    case IF_TAG_TABLE: case IF_TAG_TBODY: case IF_TAG_TFOOT:
    case IF_TAG_THEAD: case IF_TAG_TR: {
        i32 iti = -1, ita = -1;
        for (i32 i = (i32)b->depth - 1; i >= 0; i--) {
            if (iti < 0 && b->stack[i]->tag == IF_TAG_TEMPLATE) iti = i;
            if (ita < 0 && b->stack[i]->tag == IF_TAG_TABLE) ita = i;
        }
        if (iti >= 0 && (ita < 0 || iti > ita)) {
            IfNode *t = b->stack[iti];
            r.parent = t->content ? t->content : t;
            r.before = NULL;
            return r;
        }
        if (ita < 0) {
            r.parent = b->body ? b->body : (b->html ? b->html : target);
            return r;
        }
        IfNode *tab = b->stack[ita];
        if (tab->parent) { r.parent = tab->parent; r.before = tab; return r; }
        r.parent = ita > 0 ? b->stack[ita - 1] : b->dom->root;
        return r;
    }
    default:
        return r;
    }
}

static void insert_at_place(IfPlace pl, IfNode *n) {
    if (pl.before) insert_child_before(pl.parent, n, pl.before);
    else append_child(pl.parent, n);
}

/* AFE の再構成: 未クローズ書式をクローンし直して現在位置に開き直す */
static void afe_reconstruct(IfTB *b) {
    if (!b->n_afe) return;
    u32 i = b->n_afe - 1;
    if (b->afe[i].marker || stack_find_node(b, b->afe[i].n) >= 0) return;
    u32 start = 0;
    for (i32 k = (i32)i - 1; k >= 0; k--) {
        if (b->afe[k].marker || stack_find_node(b, b->afe[k].n) >= 0) { start = (u32)k + 1; break; }
    }
    for (u32 k = start; k < b->n_afe; k++) {
        IfNode *c = clone_element(b, b->afe[k].n);
        append_placed(b, c);
        push(b, c);
        b->afe[k].n = c;
    }
}

/* any other end tag（AAA の step (a) と in-body fallback の共通形） */
static void any_other_end_tag(IfTB *b, u16 tag) {
    for (i32 i = (i32)b->depth - 1; i >= 0; i--) {
        IfNode *node = b->stack[i];
        if (node->tag == tag && node->ns == IF_NS_HTML) {
            gen_implied(b, tag);
            if (top(b)->tag != tag) b->dom->n_errors++;
            while (b->depth > (u32)i) pop(b);
            return;
        }
        if (is_special(node)) { b->dom->n_errors++; return; } /* barrier: 無視 */
    }
    b->dom->n_errors++;
}

/* in-body 明示終了: scope で見えていれば implied end tags 生成 → そこまで畳む。
 * （address/div/ol/pre 等のブロック群・li・dd/dt の共通形） */
static void end_in_scope(IfTB *b, u16 tag, IfScopeKind k, bool except_self) {
    if (!has_in_scope2(b, tag, k)) { b->dom->n_errors++; return; }
    gen_implied(b, except_self ? tag : 0);
    if (top(b)->tag != tag) b->dom->n_errors++;
    pop_until(b, tag);
}

static void end_hgroup(IfTB *b) {
    static const u16 H[] = { IF_TAG_H1, IF_TAG_H2, IF_TAG_H3, IF_TAG_H4, IF_TAG_H5, IF_TAG_H6 };
    bool any = false;
    for (u32 k = 0; k < 6; k++) if (has_open(b, H[k])) { any = true; break; }
    if (!any) { b->dom->n_errors++; return; }
    gen_implied(b, 0);
    u16 tt = top(b)->tag;
    if (tt < IF_TAG_H1 || tt > IF_TAG_H6) b->dom->n_errors++;
    for (u32 k = 0; k < 6 && b->depth; k++) {
        /* h1..h6 のどれかが出るまで畳む */
        if (top(b)->tag >= IF_TAG_H1 && top(b)->tag <= IF_TAG_H6) { pop(b); break; }
        pop(b);
    }
}

/* adoption agency algorithm（outer ≤ 8, inner ≤ 3 の仕様打ち切り込み） */
/* AFE: spec の「bookmark 位置へ挿入」を正確に（fe の位置または inner loop で移動後） */
static void afe_insert_at(IfTB *b, u32 pos, IfNode *n) {
    afe_ensure(b);
    if (pos > b->n_afe) pos = b->n_afe;
    memmove(&b->afe[pos + 1], &b->afe[pos], (size_t)(b->n_afe - pos) * sizeof b->afe[0]);
    b->afe[pos].n = n;
    b->afe[pos].marker = false;
    b->n_afe++;
}

static i32 afe_find_node(const IfTB *b, const IfNode *n) {
    for (i32 i = (i32)b->n_afe - 1; i >= 0; i--) {
        if (b->afe[i].marker) break;
        if (b->afe[i].n == n) return i;
    }
    return -1;
}

/* stack[idx] を除去（idx < depth のみ、範囲外は到達不能の設計だが防御で無視） */
static void stack_remove_at(IfTB *b, u32 idx) {
    if (idx >= b->depth) return;
    memmove(&b->stack[idx], &b->stack[idx + 1],
            (size_t)(b->depth - idx - 1) * sizeof b->stack[0]);
    b->depth--;
}

/* stack[idx] の直後へ挿入 */
static void stack_insert_after(IfTB *b, u32 idx, IfNode *n) {
    push(b, n); /* 容量確保は push に委譲し末尾に仮置き */
    memmove(&b->stack[idx + 2], &b->stack[idx + 1],
            (size_t)(b->depth - idx - 2) * sizeof b->stack[0]);
    b->stack[idx + 1] = n;
}

/* Adoption Agency Algorithm（WHATWG HTML "adoption agency algorithm" 厳密版。
 * subject = 終了タグ名。8 外回・3 内回の打ち切り・bookmark の移動まで spec の番号どおり。
 * ポインタではなく位置を index/afe-index で保持し、splice 後も破綻しないようにする。 */
static void adoption(IfTB *b, u16 tag) {
    /* step 1: current node が subject と同名の HTML 要素で AFE に無い → pop して終了 */
    if (b->depth && top(b)->tag == tag && !afe_has_node(b, top(b))) { pop(b); return; }
    for (u32 outer = 0; outer < 8; outer++) {
        /* step 4: AFE から subject の最後の要素（marker 手前まで） */
        i32 fi = afe_find_tag(b, tag);
        if (fi < 0) { any_other_end_tag(b, tag); return; }
        IfNode *fe = b->afe[fi].n;
        i32 fs = stack_find_node(b, fe);
        if (fs < 0) { b->dom->n_errors++; afe_remove_at(b, (u32)fi); return; }
        if (!node_in_default_scope(b, fe)) { b->dom->n_errors++; return; }
        if (fe != top(b)) b->dom->n_errors++;
        /* step 8: furthest block = fe より上（index が大きい側）で最初に現れる
         * special 要素。仕様の "topmost node lower in the stack than the
         * formatting element"（stack は current node が bottommost）を素直に読むと
         * この向きで、「最後の」ではなく「fe に最も近い」が正解。逆方向を取ると
         * 外周ループによる ladder 化（div1→div2 順に包む）が起きず、
         * adoption01 の <a>1<div>2<div>3</a>… 系が全面不一致になる実測あり
         * （html5lib 計装で div→a の reparent が 2 段で起きることを確認済み） */
        i32 fb = -1;
        for (u32 i = (u32)fs + 1; i < b->depth; i++)
            if (is_special(b->stack[i])) { fb = (i32)i; break; }
        /* step 9: 無ければ fe まで pop して AFE からも除去して終了 */
        if (fb < 0) {
            while (b->depth > (u32)fs) pop(b);
            afe_remove_at(b, (u32)fi);
            return;
        }
        IfNode *furthest = b->stack[fb];
        /* step 10: common ancestor = fe の一つ上 */
        IfNode *ancestor = (fs > 0) ? b->stack[fs - 1] : b->dom->root;
        /* step 11: bookmark = AFE 内 fe の直後（挿入位置として） */
        u32 bookmark = (u32)fi + 1;
        /* step 12 inner loop: node/lastNode = furthest から開始 */
        IfNode *node = furthest, *lastNode = furthest;
        i32 ni = fb; /* stack[ni] == node の index */
        for (u32 inner = 0;;) {
            inner++; /* 12.1 */
            /* 12.2: node の一つ上へ（fe 方向へ進む） */
            if (ni <= 0) break;
            ni--;
            node = b->stack[ni];
            if (node == fe) break; /* 12.3: fe に着いたら inner 終了 */
            /* 12.4: inner>3 で AFE の node は除去 */
            if (inner > 3) {
                i32 rm = afe_find_node(b, node);
                if (rm >= 0) afe_remove_at(b, (u32)rm);
            }
            /* 12.5: AFE に無い node は stack から除去して次へ（ni 補正は不要:
             * 除去で上方要素が詰まり、次の ni-- が正しく一つ上を指す） */
            i32 a2 = afe_find_node(b, node);
            if (a2 < 0) { stack_remove_at(b, (u32)ni); continue; }
            /* 12.6: node の clone で AFE/stack の当該箇所を置換 */
            IfNode *clone = clone_element(b, node);
            b->afe[a2].n = clone;
            b->stack[ni] = clone;
            node = clone;
            /* 12.7: lastNode が furthest のまま（初回）なら bookmark を node の直後へ */
            if (lastNode == furthest) bookmark = (u32)a2 + 1;
            /* 12.8/12.9: lastNode を node の子に移し、lastNode = node */
            detach(lastNode);
            append_child(node, lastNode);
            lastNode = node;
        }
        /* step 13: lastNode を common ancestor 基点の「適切な挿入位置」へ（foster 含む） */
        detach(lastNode);
        insert_at_place(place_for(b, ancestor), lastNode);
        /* step 14: fe の clone を作り、furthest の全子を移して furthest に装着 */
        IfNode *fclone = clone_element(b, fe);
        move_children(furthest, fclone);
        append_child(furthest, fclone);
        /* step 15: fe を AFE から除き fclone を bookmark 位置へ */
        if (afe_find_node(b, fe) >= 0) afe_remove_at(b, (u32)afe_find_node(b, fe));
        if (bookmark > b->n_afe) bookmark = b->n_afe;
        afe_insert_at(b, bookmark, fclone);
        /* step 16: fe を stack から除き、fclone を furthest の直後へ */
        i32 fs2 = stack_find_node(b, fe);
        if (fs2 >= 0) stack_remove_at(b, (u32)fs2);
        i32 fb2 = stack_find_node(b, furthest);
        if (fb2 < 0) push(b, fclone); /* 構造上到達不能 */
        else stack_insert_after(b, (u32)fb2, fclone);
    }
}

static IfTok synth_start(u16 tag) {
    IfTok t;
    memset(&t, 0, sizeof t);
    t.kind = TOK_START;
    t.tag = tag;
    return t;
}

static bool attr_is_ci(const IfTok *tok, const char *name, const char *val) {
    for (u32 i = 0; i < tok->n_attrs; i++)
        if (if_str_eq_ci(tok->attrs[i].name, if_str(name, (u32)strlen(name)))
            && if_str_eq_ci(tok->attrs[i].value, if_str(val, (u32)strlen(val))))
            return true;
    return false;
}

static void step_in_body(IfTB *b, IfTok tok);
static void step_in_head(IfTB *b, IfTok tok);
static void step(IfTB *b, IfTok tok);
static void step_in_table(IfTB *b, IfTok tok);
static void step_in_caption(IfTB *b, IfTok tok);
static void step_in_colgroup(IfTB *b, IfTok tok);
static void step_in_table_body(IfTB *b, IfTok tok);
static void step_in_row(IfTB *b, IfTok tok);
static void step_in_cell(IfTB *b, IfTok tok);

static void step_in_table(IfTB *b, IfTok tok) {
    pend_flush(b);
    switch (tok.kind) {
    case TOK_TEXT: pend_add(b, tok.text); return;
    case TOK_COMMENT: insert_comment_placed(b, &tok); return; /* template top でも content へ */
    case TOK_DOCTYPE: b->dom->n_errors++; return;
    case TOK_EOF: step_in_body(b, tok); return; /* "anything else": body 規則で停止 */
    default: break;
    }

    if (tok.kind == TOK_START) {
        switch (tok.tag) {
        case IF_TAG_CAPTION:
            clear_back(b, C_TABLE, C_TABLE_N);
            afe_insert_marker(b);
            insert_element(b, &tok, true);
            b->mode = M_IN_CAPTION;
            return;
        case IF_TAG_COLGROUP:
            clear_back(b, C_TABLE, C_TABLE_N);
            insert_element(b, &tok, true);
            b->mode = M_IN_COLUMN_GROUP;
            return;
        case IF_TAG_COL: {
            clear_back(b, C_TABLE, C_TABLE_N);
            IfTok cg = synth_start(IF_TAG_COLGROUP);
            insert_element(b, &cg, true);
            b->mode = M_IN_COLUMN_GROUP;
            step_in_colgroup(b, tok);
            return;
        }
        case IF_TAG_TBODY: case IF_TAG_THEAD: case IF_TAG_TFOOT:
            clear_back(b, C_TABLE, C_TABLE_N);
            insert_element(b, &tok, true);
            b->mode = M_IN_TABLE_BODY;
            return;
        case IF_TAG_TR: {
            clear_back(b, C_TABLE, C_TABLE_N);
            IfTok tb = synth_start(IF_TAG_TBODY);
            insert_element(b, &tb, true);
            b->mode = M_IN_TABLE_BODY;
            step_in_table_body(b, tok);
            return;
        }
        case IF_TAG_TD: case IF_TAG_TH: {
            b->dom->n_errors++;
            clear_back(b, C_TABLE, C_TABLE_N);
            IfTok tb = synth_start(IF_TAG_TBODY);
            insert_element(b, &tb, true);
            b->mode = M_IN_TABLE_BODY;
            IfTok tr = synth_start(IF_TAG_TR);
            step_in_table_body(b, tr);
            step_in_row(b, tok);
            return;
        }
        case IF_TAG_TABLE:
            b->dom->n_errors++;
            if (!has_in_table_scope(b, IF_TAG_TABLE)) return;
            pop_until(b, IF_TAG_TABLE);
            reset_mode(b);
            step(b, tok); /* 閉じた外側の後ろで再処理 = HTML5 の「ネスト table は兄弟」規則 */
            return;
        case IF_TAG_STYLE: case IF_TAG_SCRIPT: case IF_TAG_TEMPLATE:
            step_in_head(b, tok); /* in-table の style/script/template は head 規則 */
            return;
        case IF_TAG_INPUT:
            if (attr_is_ci(&tok, "type", "hidden")) {
                insert_element(b, &tok, false);
                return;
            }
            goto foster_body; /* hidden 以外の input は foster 経路 */
        case IF_TAG_FORM:
            if (b->form) return; /* form pointer 2 重設定の禁止（template 判定線は B3） */
            insert_element(b, &tok, true);
            b->form = top(b);
            pop(b); /* spec: in-table の form は push 後に即 pop して pointer のみ残す */
            return;
        default:
            goto foster_body;
        }
    }

    /* TOK_END */
    if (tok.tag == IF_TAG_TABLE) {
        if (!has_in_table_scope(b, IF_TAG_TABLE)) { b->dom->n_errors++; return; }
        pop_until(b, IF_TAG_TABLE);
        reset_mode(b);
        return;
    }
    if (tok.tag == IF_TAG_TEMPLATE) { step_in_head(b, tok); return; }
    switch (tok.tag) {
    case IF_TAG_BODY: case IF_TAG_CAPTION: case IF_TAG_COL: case IF_TAG_COLGROUP:
    case IF_TAG_HTML: case IF_TAG_TBODY: case IF_TAG_TD: case IF_TAG_TFOOT:
    case IF_TAG_TH: case IF_TAG_THEAD: case IF_TAG_TR:
        b->dom->n_errors++;
        return; /* in-table で無視される終了タグ群（仕様表再現） */
    default:
        goto foster_body;
    }

foster_body:
    b->dom->n_errors++;
    b->foster = true;
    step_in_body(b, tok);
    b->foster = false;
}

static void step_in_caption(IfTB *b, IfTok tok) {
    if (tok.kind == TOK_END && tok.tag == IF_TAG_CAPTION) {
        if (!has_in_table_scope(b, IF_TAG_CAPTION)) { b->dom->n_errors++; return; }
        pop_until(b, IF_TAG_CAPTION); /* AFE マーカー消去は B2 */
        b->mode = M_IN_TABLE;
        return;
    }
    if (tok.kind == TOK_START) {
        switch (tok.tag) {
        case IF_TAG_CAPTION: case IF_TAG_COL: case IF_TAG_COLGROUP:
        case IF_TAG_TBODY: case IF_TAG_TD: case IF_TAG_TFOOT: case IF_TAG_TH:
        case IF_TAG_THEAD: case IF_TAG_TR: case IF_TAG_TABLE:
            /* caption を畳んで in-table で再処理（現行仕様の in-caption 規則） */
            if (!has_in_table_scope(b, IF_TAG_CAPTION)) { b->dom->n_errors++; return; }
            pop_until(b, IF_TAG_CAPTION);
            b->mode = M_IN_TABLE;
            step(b, tok);
            return;
        default:
            break;
        }
    }
    if (tok.kind == TOK_END && tok.tag == IF_TAG_TABLE) {
        if (!has_in_table_scope(b, IF_TAG_CAPTION)) { b->dom->n_errors++; return; }
        pop_until(b, IF_TAG_CAPTION);
        b->mode = M_IN_TABLE;
        step(b, tok);
        return;
    }
    if (tok.kind == TOK_END) {
        switch (tok.tag) {
        case IF_TAG_BODY: case IF_TAG_COL: case IF_TAG_COLGROUP: case IF_TAG_HTML:
        case IF_TAG_TBODY: case IF_TAG_TD: case IF_TAG_TFOOT: case IF_TAG_TH:
        case IF_TAG_THEAD: case IF_TAG_TR:
            b->dom->n_errors++;
            return;
        default:
            break;
        }
    }
    step_in_body(b, tok);
}

static void step_in_colgroup(IfTB *b, IfTok tok) {
    if (tok.kind == TOK_TEXT && if_str_is_ws_only(tok.text)) { append_text(b, tok.text); return; }
    if (tok.kind == TOK_COMMENT) { insert_comment_placed(b, &tok); return; }
    if (tok.kind == TOK_DOCTYPE) { b->dom->n_errors++; return; }
    if (tok.kind == TOK_START) {
        if (tok.tag == IF_TAG_HTML) { step_in_body(b, tok); return; }
        if (tok.tag == IF_TAG_COL) { insert_element(b, &tok, false); return; }
        if (tok.tag == IF_TAG_TEMPLATE) { step_in_head(b, tok); return; }
        goto anything;
    }
    if (tok.kind == TOK_END) {
        if (tok.tag == IF_TAG_COLGROUP) {
            if (top(b)->tag == IF_TAG_COLGROUP) { pop(b); b->mode = M_IN_TABLE; }
            else b->dom->n_errors++;
            return;
        }
        if (tok.tag == IF_TAG_COL) { b->dom->n_errors++; return; }
        if (tok.tag == IF_TAG_TEMPLATE) { step_in_head(b, tok); return; }
        goto anything;
    }
anything:
    /* "colgroup 抜け" は現在ノードが colgroup のときだけ。それ以外は無視（hole を防ぐ） */
    if (top(b)->tag == IF_TAG_COLGROUP) {
        pop(b);
        b->mode = M_IN_TABLE;
        step(b, tok);
    } else {
        b->dom->n_errors++;
    }
}

static bool any_tbf_in_scope(IfTB *b) {
    return has_in_table_scope(b, IF_TAG_TBODY) || has_in_table_scope(b, IF_TAG_THEAD)
           || has_in_table_scope(b, IF_TAG_TFOOT);
}

/* tbody 系で「現在のセクションを閉じて M_IN_TABLE で再処理する」共通形 */
static void leave_tbody_reprocess(IfTB *b, IfTok tok) {
    clear_back(b, C_TBODY, C_TBODY_N);
    pop(b);
    b->mode = M_IN_TABLE;
    step(b, tok);
}

static void step_in_table_body(IfTB *b, IfTok tok) {
    if (tok.kind == TOK_START) {
        switch (tok.tag) {
        case IF_TAG_TR:
            clear_back(b, C_TBODY, C_TBODY_N);
            insert_element(b, &tok, true);
            b->mode = M_IN_ROW;
            return;
        case IF_TAG_TD: case IF_TAG_TH: {
            b->dom->n_errors++;
            IfTok tr = synth_start(IF_TAG_TR);
            step_in_table_body(b, tr); /* 合成 tr を入れてから本トークンを行規則で */
            step_in_row(b, tok);
            return;
        }
        case IF_TAG_CAPTION: case IF_TAG_COL: case IF_TAG_COLGROUP:
        case IF_TAG_TBODY: case IF_TAG_THEAD: case IF_TAG_TFOOT:
            if (!any_tbf_in_scope(b)) { b->dom->n_errors++; return; }
            leave_tbody_reprocess(b, tok);
            return;
        default:
            break;
        }
    } else if (tok.kind == TOK_END) {
        switch (tok.tag) {
        case IF_TAG_TBODY: case IF_TAG_THEAD: case IF_TAG_TFOOT:
            if (!has_in_table_scope(b, tok.tag)) { b->dom->n_errors++; return; }
            clear_back(b, C_TBODY, C_TBODY_N);
            pop(b);
            b->mode = M_IN_TABLE;
            return;
        case IF_TAG_TABLE:
            if (!any_tbf_in_scope(b)) { b->dom->n_errors++; return; }
            clear_back(b, C_TBODY, C_TBODY_N);
            pop(b);
            b->mode = M_IN_TABLE;
            step(b, tok);
            return;
        case IF_TAG_BODY: case IF_TAG_CAPTION: case IF_TAG_COL: case IF_TAG_COLGROUP:
        case IF_TAG_HTML: case IF_TAG_TR: case IF_TAG_TD: case IF_TAG_TH:
            b->dom->n_errors++;
            return;
        default:
            break;
        }
    }
    /* anything else: in-table 規則へ（= foster + in-body / 保留テキスト経路） */
    step_in_table(b, tok);
}

/* tr を正當に終わらせて M_IN_TABLE_BODY に戻し、tok を再処理 */
static void end_tr_reprocess(IfTB *b, IfTok tok) {
    clear_back(b, C_TR, C_TR_N);
    pop(b); /* tr */
    b->mode = M_IN_TABLE_BODY;
    step(b, tok);
}

static void step_in_row(IfTB *b, IfTok tok) {
    if (tok.kind == TOK_START) {
        switch (tok.tag) {
        case IF_TAG_TD: case IF_TAG_TH:
            clear_back(b, C_TR, C_TR_N);
            afe_insert_marker(b);
            insert_element(b, &tok, true);
            b->mode = M_IN_CELL;
            return;
        case IF_TAG_CAPTION: case IF_TAG_COL: case IF_TAG_COLGROUP:
        case IF_TAG_TBODY: case IF_TAG_THEAD: case IF_TAG_TFOOT:
            if (!has_in_table_scope(b, IF_TAG_TR)) { b->dom->n_errors++; return; }
            end_tr_reprocess(b, tok);
            return;
        case IF_TAG_TABLE:
            b->dom->n_errors++; /* spec: in-row の table 開始は error 付き再処理 */
            if (!has_in_table_scope(b, IF_TAG_TR)) return;
            end_tr_reprocess(b, tok);
            return;
        default:
            break;
        }
    } else if (tok.kind == TOK_END) {
        switch (tok.tag) {
        case IF_TAG_TR:
            if (!has_in_table_scope(b, IF_TAG_TR)) { b->dom->n_errors++; return; }
            clear_back(b, C_TR, C_TR_N);
            pop(b);
            b->mode = M_IN_TABLE_BODY;
            return;
        case IF_TAG_TABLE:
            if (!has_in_table_scope(b, IF_TAG_TR)) { b->dom->n_errors++; return; }
            end_tr_reprocess(b, tok);
            return;
        case IF_TAG_TBODY: case IF_TAG_THEAD: case IF_TAG_TFOOT:
            if (!has_in_table_scope(b, tok.tag)) { b->dom->n_errors++; return; }
            end_tr_reprocess(b, tok);
            return;
        case IF_TAG_BODY: case IF_TAG_CAPTION: case IF_TAG_COL: case IF_TAG_COLGROUP:
        case IF_TAG_HTML: case IF_TAG_TD: case IF_TAG_TH:
            b->dom->n_errors++;
            return;
        default:
            break;
        }
    }
    /* anything else: in-table 規則へ（= foster + in-body / 保留テキスト経路） */
    step_in_table(b, tok);
}

static void step_in_cell(IfTB *b, IfTok tok) {
    if (tok.kind == TOK_END && (tok.tag == IF_TAG_TD || tok.tag == IF_TAG_TH)) {
        if (!has_in_table_scope(b, tok.tag)) { b->dom->n_errors++; return; }
        gen_implied(b, tok.tag);
        if (top(b)->tag != tok.tag) b->dom->n_errors++;
        pop_until(b, tok.tag);
        afe_clear_to_marker(b);
        b->mode = M_IN_ROW;
        return;
    }
    if (tok.kind == TOK_START) {
        switch (tok.tag) {
        case IF_TAG_CAPTION: case IF_TAG_COL: case IF_TAG_COLGROUP:
        case IF_TAG_TBODY: case IF_TAG_TD: case IF_TAG_TH:
        case IF_TAG_THEAD: case IF_TAG_TFOOT: case IF_TAG_TR:
            b->dom->n_errors++;
            close_cell(b);
            step(b, tok);
            return;
        default:
            break;
        }
    }
    if (tok.kind == TOK_END) {
        switch (tok.tag) {
        case IF_TAG_TABLE: case IF_TAG_TBODY: case IF_TAG_TFOOT: case IF_TAG_THEAD:
        case IF_TAG_TR:
            if (!has_in_table_scope(b, tok.tag)) { b->dom->n_errors++; return; }
            close_cell(b);
            step(b, tok);
            return;
        case IF_TAG_BODY: case IF_TAG_CAPTION: case IF_TAG_COL: case IF_TAG_COLGROUP:
        case IF_TAG_HTML:
            b->dom->n_errors++;
            return;
        default:
            break;
        }
    }
    step_in_body(b, tok);
}

static void pend_flush(IfTB *b) {
    if (!b->pend_n) return;
    /* pend バッファは再利用されるため、そのまま DOM ノードに保持させると次回の
     * pend_add で内容が破壊される（<table><a>1...</a>3 型で text が後続文字に
     * 化ける実害があった）。pend 経由のテキストのみ arena に定着させる。 */
    char *keep = (char *)if_arena_alloc(b->arena, b->pend_n);
    memcpy(keep, b->pend, b->pend_n);
    IfStr t = if_str(keep, b->pend_n);
    b->pend_n = 0;
    if (!b->pend_nonws) {
        /* 全空白: 現行 spec では pending-text に含まれるが、フラッシュ時「挿入位置に
         * 直接追記」が正解（foster させない）。vendored データセット (#tests7 12:
         * <table><tbody><script> が「 」を script の兄として tbody 内に置く) と
         * html5lib の InTable spaceCharacters 規則で両側実証済み。append_text は
         * place() 経由なので foster フラグが立っていない限り current node 直下。 */
        bool f = b->foster;
        b->foster = false;
        append_text(b, t);
        b->foster = f;
        return;
    }
    b->pend_nonws = false;
    /* 非空白混じり: spec の flush 規則は「current node が table 系(table/tbody/tfoot/
     * thead/tr)なら foster、それ以外(script 内テキスト等)は foster なしで body 規則」。 */
    bool tableish = false;
    if (b->depth) {
        switch (top(b)->tag) {
        case IF_TAG_TABLE: case IF_TAG_TBODY: case IF_TAG_TFOOT:
        case IF_TAG_THEAD: case IF_TAG_TR:
            tableish = true; break;
        default: break;
        }
    }
    bool f = b->foster;
    b->foster = f || tableish;
    step_in_body(b, (IfTok){ .kind = TOK_TEXT, .text = t });
    b->foster = f;
}

/* ---- frameset 挿入モード（WHATWG 12.2.6.4.19-21 の核。tests18/19/plain-text-unsafe 由来） ---- */
/* frameset 系モード（in/after/after-after）の character 規則共通部:
 * テキスト中の ws 文字(TAB/LF/FF/CR/SP)のみ挿入。非 ws は parse error で捨てる
 * （トークン丸ごと捨てではない — "<frameset> te st" → "  " が残る spec 挙動） */
static void frameset_text(IfTB *b, IfStr s) {
    u32 nws = 0;
    for (u32 i = 0; i < s.n; i++) {
        char c = s.p[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') nws++;
    }
    if (nws == s.n) { append_text(b, s); return; }
    char *buf = (char *)if_arena_alloc(b->arena, nws);
    u32 w = 0;
    for (u32 i = 0; i < s.n; i++) {
        char c = s.p[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') buf[w++] = c;
    }
    append_text(b, if_str(buf, w));
    b->dom->n_errors++;
}

static void step_in_frameset(IfTB *b, IfTok tok) {
    if (tok.kind == TOK_TEXT) { frameset_text(b, tok.text); return; }
    if (tok.kind == TOK_COMMENT) { insert_comment_placed(b, &tok); return; }
    if (tok.kind == TOK_DOCTYPE) { b->dom->n_errors++; return; }
    if (tok.kind == TOK_START && tok.tag == IF_TAG_HTML) { step_in_body(b, tok); return; }
    if (tok.kind == TOK_START && tok.tag == IF_TAG_FRAMESET) { insert_element(b, &tok, true); return; }
    if (tok.kind == TOK_START && tok.tag == IF_TAG_FRAME) { insert_element(b, &tok, false); return; }
    if (tok.kind == TOK_START && tok.tag == IF_TAG_NOFRAMES) { step_in_head(b, tok); return; }
    if (tok.kind == TOK_END && tok.tag == IF_TAG_FRAMESET) {
        if (b->depth && top(b)->tag == IF_TAG_FRAMESET) {
            pop(b);
            /* fragment 環境非対応: frameset を抜けたら常に after-frameset */
            b->mode = M_AFTER_FRAMESET;
            return;
        }
        if (b->depth <= 1) { b->stopped = true; return; } /* html のみ: 仕様の stop 経路 */
        b->dom->n_errors++;
        return;
    }
    /* それ以外: parse error で無視（body は作らない! frameset_doc の不変条件） */
    b->dom->n_errors++;
}

static void step_after_frameset(IfTB *b, IfTok tok) {
    if (tok.kind == TOK_TEXT) { frameset_text(b, tok.text); return; }
    if (tok.kind == TOK_COMMENT) { insert_comment_placed(b, &tok); return; }
    if (tok.kind == TOK_DOCTYPE) { b->dom->n_errors++; return; }
    if (tok.kind == TOK_START && tok.tag == IF_TAG_HTML) { step_in_body(b, tok); return; }
    if (tok.kind == TOK_END && tok.tag == IF_TAG_HTML) { b->mode = M_AFTER_AFTER_FRAMESET; return; }
    if (tok.kind == TOK_START && tok.tag == IF_TAG_NOFRAMES) { step_in_head(b, tok); return; }
    if (tok.kind == TOK_EOF) { b->stopped = true; return; }
    b->dom->n_errors++;
}

static void step_after_after_frameset(IfTB *b, IfTok tok) {
    if (tok.kind == TOK_TEXT) { frameset_text(b, tok.text); return; }
    if (tok.kind == TOK_COMMENT) { insert_comment(b, b->dom->root, &tok); return; }
    if (tok.kind == TOK_DOCTYPE) { b->dom->n_errors++; return; }
    if (tok.kind == TOK_START && tok.tag == IF_TAG_HTML) { step_in_body(b, tok); return; }
    if (tok.kind == TOK_START && tok.tag == IF_TAG_NOFRAMES) { step_in_head(b, tok); return; }
    if (tok.kind == TOK_EOF) { b->stopped = true; return; }
    b->dom->n_errors++;
}

/* rawtext または RCDATA の container 要素か（内容は element の子テキスト1本） */
static bool rawish(u16 t) { return if_tag_is_rawtext(t) || if_tag_is_rcdata(t); }

/* PI の target を attrs[0].name に積む（IfNode union 化で COMMENT の tag_name 欄は
 * text と共有になったため。COMMENT は attrs を通常使わない規約。serializer は
 * dom.c IF_NODE_COMMENT 分岐で n_attrs>0 を PI 印として読む） */
static void pi_target_save(IfTB *b, IfNode *n, IfStr target) {
    if (!target.n) return; /* 空 target は旧挙動どおり通常 comment として serialize */
    IfAttr *at = (IfAttr *)if_arena_alloc(b->arena, sizeof(IfAttr));
    if (!at) return; /* OOM 時は target なし PI（通常 comment として見える）に劣化 */
    at->name = target;
    at->value = if_str(NULL, 0);
    n->attrs = at;
    n->n_attrs = 1;
}

static void insert_comment(IfTB *b, IfNode *parent, const IfTok *tok) {
    if (under_slim(b)) return;
    IfNode *n = new_node(b, IF_NODE_COMMENT);
    n->u.text = tok->text;
    if (tok->is_pi) pi_target_save(b, n, tok->pi_target);
    append_child(parent, n);
}

/* in-body 経路のコメント: foster フラグが立っていれば「table の兄」に置く */
static void insert_comment_placed(IfTB *b, const IfTok *tok) {
    if (under_slim(b)) return;
    IfNode *n = new_node(b, IF_NODE_COMMENT);
    n->u.text = tok->text;
    if (tok->is_pi) pi_target_save(b, n, tok->pi_target);
    append_placed(b, n);
}

/* 2 個目の <body>/<html>: 不足している属性だけマージする（WHATWG 準拠） */
static void merge_attrs(IfTB *b, IfNode *dst, const IfTok *tok) {
    for (u32 i = 0; i < tok->n_attrs; i++) {
        bool exists = false;
        for (u32 k = 0; k < dst->n_attrs; k++)
            if (if_str_eq_ci(dst->attrs[k].name, tok->attrs[i].name)) { exists = true; break; }
        if (exists) continue;
        u64 cap = 0;
        IfAttr *na = (IfAttr *)if_arena_grow(b->arena, NULL, &cap, dst->n_attrs + 1, sizeof(IfAttr));
        memcpy(na, dst->attrs, (u64)dst->n_attrs * sizeof(IfAttr));
        na[dst->n_attrs] = tok->attrs[i];
        dst->attrs = na;
        dst->n_attrs++;
    }
}

/* モードごとのトークン処理 */
static void step(IfTB *b, IfTok tok);
/* foreign content 系（実体はファイル末尾） */
static bool in_foreign(const IfTB *b, const IfTok *tok);
static bool in_foreign_text(const IfTB *b);
static void foreign_step(IfTB *b, const IfTok *tok);
static void foreign_adjust(IfTB *b, IfNode *n);
static bool is_html_ip(const IfNode *n);
static bool is_math_ip(const IfNode *n);
static IfNode *make_element(IfTB *b, const IfTok *tok);

/* 先頭の HTML 空白（TAB LF FF CR SPACE）ランを剥がして返す。入力スライスは残りに進む */
static IfStr peel_leading_ws(IfStr *s) {
    u32 n = 0;
    while (n < s->n && (s->p[n] == ' ' || s->p[n] == '\t' || s->p[n] == '\n' ||
                        s->p[n] == '\f' || s->p[n] == '\r'))
        n++;
    IfStr ws = if_str(s->p, n);
    s->p += n;
    s->n -= n;
    return ws;
}


/* ---- quirks モード判定（WHATWG 13.2.5.1 DOCTYPE 規則の完全表） ---- */
static bool if_quirks_pub_prefix(const char *pub) {
    static const char *P[] = {
        "+//silmaril//dtd html pro v0r11 19970101//",
        "-//advasoft ltd//dtd html 3.0 aswedit + extensions//",
        "-//as//dtd html 3.0 aswedit + extensions//",
        "-//ietf//dtd html 2.0 level 1//", "-//ietf//dtd html 2.0 level 2//",
        "-//ietf//dtd html 2.0 strict level 1//", "-//ietf//dtd html 2.0 strict level 2//",
        "-//ietf//dtd html 2.0 strict//", "-//ietf//dtd html 2.0//", "-//ietf//dtd html 2.1e//",
        "-//ietf//dtd html 3.0//", "-//ietf//dtd html 3.2 final//", "-//ietf//dtd html 3.2//",
        "-//ietf//dtd html 3//", "-//ietf//dtd html level 0//", "-//ietf//dtd html level 1//",
        "-//ietf//dtd html level 2//", "-//ietf//dtd html level 3//",
        "-//ietf//dtd html strict level 0//", "-//ietf//dtd html strict level 1//",
        "-//ietf//dtd html strict level 2//", "-//ietf//dtd html strict level 3//",
        "-//ietf//dtd html strict//", "-//ietf//dtd html//",
        "-//metrius//dtd metrius presentational//",
        "-//microsoft//dtd internet explorer 2.0 html strict//",
        "-//microsoft//dtd internet explorer 2.0 html//",
        "-//microsoft//dtd internet explorer 2.0 tables//",
        "-//microsoft//dtd internet explorer 3.0 html strict//",
        "-//microsoft//dtd internet explorer 3.0 html//",
        "-//microsoft//dtd internet explorer 3.0 tables//",
        "-//netscape comm. corp.//dtd html//", "-//netscape comm. corp.//dtd strict html//",
        "-//o'reilly and associates//dtd html 2.0//",
        "-//o'reilly and associates//dtd html extended 1.0//",
        "-//o'reilly and associates//dtd html extended relaxed 1.0//",
        "-//softquad software//dtd hotmetal pro 6.0::19990601::extensions to html 4.0//",
        "-//softquad//dtd hotmetal pro 4.0::19971010::extensions to html 4.0//",
        "-//spyglass//dtd html 2.0 extended//", "-//sq//dtd html 2.0 hotmetal + extensions//",
        "-//sun microsystems corp.//dtd hotjava html//",
        "-//sun microsystems corp.//dtd hotjava strict html//",
        "-//w3c//dtd html 3 1995-03-24//", "-//w3c//dtd html 3.2 draft//",
        "-//w3c//dtd html 3.2 final//", "-//w3c//dtd html 3.2//", "-//w3c//dtd html 3.2s draft//",
        "-//w3c//dtd html 4.0 frameset//", "-//w3c//dtd html 4.0 transitional//",
        "-//w3c//dtd html experimental 19960712//", "-//w3c//dtd html experimental 970421//",
        "-//w3c//dtd w3 html//", "-//w3o//dtd w3 html 3.0//",
        "-//webtechs//dtd mozilla html 2.0//", "-//webtechs//dtd mozilla html//"
    };
    for (u32 i = 0; i < sizeof P / sizeof *P; i++) {
        const char *p = P[i];
        u32 k = 0;
        while (p[k] && (char)(pub[k] | 0x20) == p[k]) k++; /* pub は ascii-insensitive 前方一致 */
        if (p[k] == 0) return true;
    }
    return false;
}

static bool if_quirks_pub_exact(const char *pub) {
    static const char *E[] = {
        "-//w3o//dtd w3 html strict 3.0//en//",
        "-/w3c/dtd html 4.0 transitional/en",
        "html"
    };
    char buf[96];
    u32 n = 0;
    for (const char *s = pub; *s && n < sizeof buf - 1; s++) buf[n++] = (char)(*s | 0x20);
    buf[n] = 0;
    for (u32 i = 0; i < sizeof E / sizeof *E; i++)
        if (strcmp(buf, E[i]) == 0) return true;
    return false;
}

static bool if_doctype_is_quirks(const IfTok *tok) {
    /* name 無し / name != "html" → quirks */
    if (!tok->dt_has_name) return true;
    if (!if_str_eq_ci(tok->text, IF_S("html"))) return true;
    if (tok->dt_has_pub) {
        char *pub = (char *)tok->dt_pub.p;
        /* arena 内文字列は NUL 終端保証がない → 長さ限定コピー */
        static char pb[512];
        u32 n = tok->dt_pub.n < sizeof pb - 1 ? tok->dt_pub.n : (u32)sizeof pb - 1;
        memcpy(pb, pub, n); pb[n] = 0;
        pub = pb;
        if (if_quirks_pub_prefix(pub) || if_quirks_pub_exact(pub)) return true;
        /* system id 無しで 4.01 frameset/transitional → quirks */
        if (!tok->dt_has_sys) {
            if (strncmp((const char *)pub, "-//W3C//DTD HTML 4.01 Frameset//", 33) == 0 ||
                strncmp((const char *)pub, "-//w3c//dtd html 4.01 frameset//", 33) == 0 ||
                strncmp((const char *)pub, "-//W3C//DTD HTML 4.01 Transitional//", 37) == 0 ||
                strncmp((const char *)pub, "-//w3c//dtd html 4.01 transitional//", 37) == 0)
                return true;
        }
    }
    if (tok->dt_has_sys) {
        IfStr s = tok->dt_sys;
        IfStr ibm = IF_S("http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd");
        if (if_str_eq_ci(s, ibm)) return true;
    }
    if (tok->dt_has_pub && !tok->dt_has_sys) {
        /* public あり system 無しは quirks（spec 条項） */
        return true;
    }
    return false;
}

static void step_initial(IfTB *b, IfTok tok) {
    switch (tok.kind) {
    case TOK_DOCTYPE:
        b->dom->quirks = if_doctype_is_quirks(&tok); /* 完全表による spec 準拠判定 */
        b->mode = M_BEFORE_HTML; /* spec: initial の DOCTYPE 受理後は before html へ。
                                  * これが無いと次トークンが M_INITIAL の anything-else に
                                  * 落ちて quirks が強制上書きされる（quirks01#0 回帰の根因） */
        if (!b->seen_doctype) {
            b->seen_doctype = true;
            IfNode *d = new_node(b, IF_NODE_DOCTYPE);
            d->u.dtype = (IfDoctype *)if_arena_calloc(b->arena, sizeof(IfDoctype));
            d->u.dtype->name = tok.text;      d->u.dtype->has_name = tok.dt_has_name;
            d->u.dtype->pub = tok.dt_pub;     d->u.dtype->has_pub = tok.dt_has_pub;
            d->u.dtype->sys = tok.dt_sys;     d->u.dtype->has_sys = tok.dt_has_sys;
            append_child(b->dom->root, d);
        } else {
            b->dom->n_errors++; /* 複数 doctype は無視 */
        }
        return;
    case TOK_COMMENT: insert_comment(b, b->dom->root, &tok); return;
    case TOK_TEXT: {
        IfStr rest = tok.text;
        peel_leading_ws(&rest); /* initial の先頭空白は捨てる（ws char → ignore） */
        if (!rest.n) return;
        tok.text = rest;
        /* doctype 無し: spec の anything-else は quirks を立てて before-html へ */
        b->dom->quirks = true;
        b->dom->n_errors++;
        b->mode = M_BEFORE_HTML;
        step(b, tok);
        return;
    }
    default:
        /* doctype 無し: spec の anything-else は quirks を立てて before-html へ */
        b->dom->quirks = true;
        b->dom->n_errors++;
        b->mode = M_BEFORE_HTML;
        step(b, tok);
        return;
    }
}

static void ensure_html(IfTB *b) {
    if (!b->html) {
        IfTok t = { .kind = TOK_START, .tag = IF_TAG_HTML };
        b->html = make_element(b, &t);
        append_child(b->dom->root, b->html);
        push(b, b->html);
    }
}

static void ensure_head(IfTB *b) {
    ensure_html(b);
    if (!b->head) {
        IfTok t = { .kind = TOK_START, .tag = IF_TAG_HEAD };
        b->head = make_element(b, &t);
        append_child(top(b), b->head);
        /* head はスタックに積まず、内容は head を一時 top にして処理する設計を避け、
         * 単純化のため push する。IN_HEAD 抜け時に pop する。 */
        if (b->depth < IF_MAX_STACK_DEPTH) push(b, b->head);
    }
}

static void ensure_body(IfTB *b) {
    ensure_html(b);
    if (!b->head) ensure_head(b);
    /* head が開いたままなら閉じる */
    while (b->depth && top(b)->tag == IF_TAG_HEAD) pop(b);
    if (!b->body) {
        IfTok t = { .kind = TOK_START, .tag = IF_TAG_BODY };
        b->body = make_element(b, &t);
        append_child(b->html, b->body);
        if (b->depth < IF_MAX_STACK_DEPTH) push(b, b->body);
    }
}

static void step_before_html(IfTB *b, IfTok tok) {
    if (tok.kind == TOK_COMMENT) { insert_comment(b, b->dom->root, &tok); return; }
    if (tok.kind == TOK_TEXT) {
        IfStr rest = tok.text;
        peel_leading_ws(&rest); /* before-html も先頭空白のみ捨てる */
        if (!rest.n) return;
        tok.text = rest;
        b->mode = M_BEFORE_HEAD;
        step(b, tok);
        return;
    }
    if (tok.kind == TOK_START && tok.tag == IF_TAG_HTML) {
        if (!b->html) {
            b->html = make_element(b, &tok);
            append_child(b->dom->root, b->html);
            push(b, b->html);
        }
        b->mode = M_BEFORE_HEAD;
        return;
    }
    b->mode = M_BEFORE_HEAD;
    step(b, tok);
}

static void step_before_head(IfTB *b, IfTok tok) {
    if (tok.kind == TOK_TEXT) {
        IfStr rest = tok.text;
        peel_leading_ws(&rest); /* before-head: 先頭空白だけ捨てて残りは head 側へ */
        if (!rest.n) return;
        tok.text = rest;
        b->mode = M_IN_HEAD;
        ensure_head(b);
        step(b, tok);
        return;
    }
    if (tok.kind == TOK_COMMENT) { insert_comment(b, b->html ? b->html : b->dom->root, &tok); return; }
    if (tok.kind == TOK_END && tok.tag != IF_TAG_HEAD && tok.tag != IF_TAG_HTML &&
        tok.tag != IF_TAG_BODY && tok.tag != IF_TAG_BR) {
        b->dom->n_errors++; /* before-head: 無関係な終了タグは無視（WHATWG） */
        return;
    }
    if (tok.kind == TOK_START && tok.tag == IF_TAG_HEAD) {
        ensure_html(b);
        b->head = make_element(b, &tok);
        append_child(top(b), b->head);
        if (b->depth < IF_MAX_STACK_DEPTH) push(b, b->head);
        b->mode = M_IN_HEAD;
        return;
    }
    b->mode = M_IN_HEAD;
    ensure_head(b);
    step(b, tok);
}

/* スタック中の指定ノードを（トップとは限らない位置から）除去。
 * after-head の from-head 規則で head pointer を一時 push → in-head 処理 → 除去、に使う。
 * 除去は index シフトで行い、子孫要素の DOM 位置には触れない。 */
static void remove_from_stack(IfTB *b, IfNode *n) {
    for (u32 i = b->depth; i > 0; i--) {
        if (b->stack[i - 1] == n) {
            memmove(&b->stack[i - 1], &b->stack[i], (size_t)(b->depth - i) * sizeof b->stack[0]);
            b->depth--;
            return;
        }
    }
}

static void step_in_head(IfTB *b, IfTok tok) {
    if (tok.kind == TOK_COMMENT) { insert_comment(b, top(b), &tok); return; }
    if (tok.kind == TOK_TEXT) {
        IfStr rest = tok.text;
        IfStr ws = peel_leading_ws(&rest); /* head 内の先頭空白は head に挿入（13.2.6.4.5） */
        if (ws.n) append_text(b, ws);
        if (!rest.n) return;
        tok.text = rest;
        b->mode = M_AFTER_HEAD;
        pop_if(b, IF_TAG_HEAD);
        step(b, tok);
        return;
    }
    if (tok.kind == TOK_START) {
        switch (tok.tag) {
        case IF_TAG_TITLE: case IF_TAG_STYLE: case IF_TAG_SCRIPT:
            insert_element(b, &tok, true);
            if_tok_set_raw(&b->tok, tok.tag);
            return;
        /* in-head の void 集合は仕様どおり base/basefont/bgsound/link/meta のみ。
         * img/input/wbr/param/source/track/textarea は in-head 項目ではなく
         * body 側へ流れる（tests19 の want 木で実測検証済み） */
        case IF_TAG_BASE: case IF_TAG_BASEFONT: case IF_TAG_BGSOUND:
        case IF_TAG_META: case IF_TAG_LINK:
            insert_element(b, &tok, false);
            return;
        case IF_TAG_HEAD:
            b->dom->n_errors++; return; /* 無視 */
        case IF_TAG_TEMPLATE:
            tpl_start(b, &tok);
            return;
        case IF_TAG_NOSCRIPT:
            insert_element(b, &tok, true);
            return;
        case IF_TAG_NOFRAMES: /* head 内 noframes は body 規則（raw 処理込み） */
            insert_element(b, &tok, true);
            if_tok_set_raw(&b->tok, tok.tag);
            return;
        default:
            b->mode = M_AFTER_HEAD;
            pop_if(b, IF_TAG_HEAD);
            step(b, tok);
            return;
        }
    }
    if (tok.kind == TOK_END) {
        if (tok.tag == IF_TAG_HEAD) { pop_if(b, IF_TAG_HEAD); b->mode = M_AFTER_HEAD; return; }
        if (tok.tag == IF_TAG_TEMPLATE) { tpl_end(b); return; }
        if (tok.tag == IF_TAG_TITLE || tok.tag == IF_TAG_STYLE || tok.tag == IF_TAG_SCRIPT ||
            tok.tag == IF_TAG_TEXTAREA || tok.tag == IF_TAG_NOSCRIPT) {
            pop_if(b, tok.tag);
            return;
        }
        if (tok.tag != IF_TAG_BR && tok.tag != IF_TAG_HTML && tok.tag != IF_TAG_BODY) {
            b->dom->n_errors++; /* in-head: 無関係な終了タグは無視（WHATWG） */
            return;
        }
        b->mode = M_AFTER_HEAD;
        pop_if(b, IF_TAG_HEAD);
        step(b, tok);
        return;
    }
    if (tok.kind == TOK_EOF) {
        b->mode = M_AFTER_HEAD;
        pop_if(b, IF_TAG_HEAD);
        step(b, tok);
    }
}

static void step_after_head(IfTB *b, IfTok tok) {
    if (tok.kind == TOK_COMMENT) { insert_comment(b, top(b), &tok); return; }
    /* 仕様 13.2.6.4.6: after-head の空白は「現在ノード（=html）に挿入」。
     * 混在テキストは先頭空白ランだけ挿入し、残りを anything-else 経路へ */
    if (tok.kind == TOK_TEXT) {
        IfStr rest = tok.text;
        IfStr ws = peel_leading_ws(&rest);
        if (ws.n) append_text(b, ws);
        if (!rest.n) return;
        tok.text = rest;
        ensure_body(b);
        b->frameset_ok = true; /* 暗黙 body: spec/html5lib は ok に戻す */
        b->mode = M_IN_BODY;
        step(b, tok);
        return;
    }
    if (tok.kind == TOK_START && tok.tag == IF_TAG_BODY) {
        ensure_html(b);
        while (b->depth && top(b)->tag == IF_TAG_HEAD) pop(b);
        b->frameset_ok = false; /* 明示 body: not-ok（13.2.6.4.6） */
        if (b->body) {
            b->dom->n_errors++;
            merge_attrs(b, b->body, &tok);
            b->mode = M_IN_BODY;
            return;
        }
        b->body = make_element(b, &tok);
        append_child(b->html, b->body);
        if (b->depth < IF_MAX_STACK_DEPTH) push(b, b->body);
        b->mode = M_IN_BODY;
        return;
    }
    if (tok.kind == TOK_START && tok.tag == IF_TAG_HEAD) { b->dom->n_errors++; return; }
    if (tok.kind == TOK_START && tok.tag == IF_TAG_FRAMESET) {
        ensure_html(b);
        while (b->depth && top(b)->tag == IF_TAG_HEAD) pop(b);
        insert_element(b, &tok, true);
        b->mode = M_IN_FRAMESET;
        return;
    }
    /* from-head 集合: head pointer を一時 push → in-head 規則で処理 → スタックから除去 */
    if (tok.kind == TOK_START &&
        (tok.tag == IF_TAG_BASE || tok.tag == IF_TAG_BASEFONT || tok.tag == IF_TAG_BGSOUND ||
         tok.tag == IF_TAG_LINK || tok.tag == IF_TAG_META || tok.tag == IF_TAG_NOFRAMES ||
         tok.tag == IF_TAG_SCRIPT || tok.tag == IF_TAG_STYLE || tok.tag == IF_TAG_TEMPLATE ||
         tok.tag == IF_TAG_TITLE)) {
        b->dom->n_errors++;
        if (b->head) {
            push(b, b->head);
            step_in_head(b, tok);
            remove_from_stack(b, b->head);
        } else {
            step_in_head(b, tok);
        }
        return;
    }
    if (tok.kind == TOK_END && tok.tag == IF_TAG_TEMPLATE) {
        if (b->head) {
            push(b, b->head);
            step_in_head(b, tok);
            remove_from_stack(b, b->head);
        } else step_in_head(b, tok);
        return;
    }
    if (tok.kind == TOK_END && tok.tag == IF_TAG_HTML) {
        /* after-head で </html>: ensure_body して in-body へ（仕様の経路） */
        ensure_body(b);
        b->mode = M_IN_BODY;
        step(b, tok);
        return;
    }
    if (tok.kind == TOK_END && tok.tag != IF_TAG_BODY && tok.tag != IF_TAG_BR) {
        b->dom->n_errors++; /* after-head: 無関係な終了タグは無視（WHATWG） */
        return;
    }
    ensure_body(b);
    b->mode = M_IN_BODY;
    step(b, tok);
}

static void step_in_body(IfTB *b, IfTok tok) {
    switch (tok.kind) {
    case TOK_TEXT:
        if (in_foreign_text(b)) {
            /* foreign 直下の文字（13.2.6.5）: AFE 再構築なし。非空白混じりは not-ok */
            append_text(b, tok.text);
            if (!if_str_is_ws_only(tok.text)) b->frameset_ok = false;
            return;
        }
        afe_reconstruct(b); /* 書式の誤ネストをテキスト挿入前に修復（<b>x<p>y → y は b の中） */
        append_text(b, tok.text);
        if (!if_str_is_ws_only(tok.text)) b->frameset_ok = false; /* 非空白は not-ok */
        return;
    case TOK_COMMENT:
        insert_comment_placed(b, &tok);
        return;
    case TOK_DOCTYPE:
        b->dom->n_errors++;
        return;
    case TOK_EOF:
        /* spec: stack に template が残っていれば 1 枚ずつ畳んで EOF を再処理
         * （各反復で template が必ず減るので有限停止する） */
        if (has_open(b, IF_TAG_TEMPLATE)) {
            b->dom->n_errors++;
            pop_until(b, IF_TAG_TEMPLATE);
            if (b->n_tpl) b->n_tpl--;
            reset_mode(b);
            step(b, tok);
            return;
        }
        b->stopped = true;
        return;
    case TOK_END:
        break;
    case TOK_START:
        break;
    }

    if (tok.kind == TOK_START) {
        u16 t = tok.tag;
        if (t == IF_TAG_IMAGE) { /* quirk（仕様）: <image> は parse error のうえ img として扱う */
            b->dom->n_errors++;
            tok.tag = t = IF_TAG_IMG;
        }
        if (in_foreign(b, &tok) && !b->no_foreign) { foreign_step(b, &tok); return; }
        if (t == IF_TAG_SVG || t == IF_TAG_MATH) { /* foreign ルート入域（属性も調整） */
            IfNode *n = make_element(b, &tok);
            n->ns = (t == IF_TAG_SVG) ? IF_NS_SVG : IF_NS_MATHML;
            foreign_adjust(b, n);
            append_placed(b, n); /* foster 時は「table の兄」へ（<table><svg> 等） */
            if (!tok.self_closing) {
                if (b->depth < IF_MAX_STACK_DEPTH) push(b, n);
                else b->dom->n_errors++;
            }
            return;
        }
        if (t == IF_TAG_HTML) {
            b->dom->n_errors++;
            if (b->html) merge_attrs(b, b->html, &tok);
            return;
        }
        if (t == IF_TAG_BODY || t == IF_TAG_HEAD) {
            /* 仕様 in-body body: parse error。2番目が body で template 無しなら
             * 属性マージ + frameset-ok = not-ok。head は無視のみ */
            b->dom->n_errors++;
            if (t == IF_TAG_BODY && b->depth >= 2 && b->stack[1]->tag == IF_TAG_BODY &&
                !has_open(b, IF_TAG_TEMPLATE)) {
                b->frameset_ok = false;
                merge_attrs(b, b->stack[1], &tok);
            }
            return;
        }
        /* in-body frameset（13.2.6.4.7）: frameset-ok が立つときだけ body を置換 */
        if (t == IF_TAG_FRAMESET) {
            b->dom->n_errors++;
            if (b->depth < 2 || b->stack[1]->tag != IF_TAG_BODY) return;
            if (!b->frameset_ok) return;
            detach(b->stack[1]);         /* body を DOM から除去（子孫ごと切り離し） */
            b->body = NULL;
            while (b->depth > 1) pop(b); /* html 以外を畳む */
            insert_element(b, &tok, true);
            b->mode = M_IN_FRAMESET;
            return;
        }
        if (t == IF_TAG_TABLE) {
            /* quirks モードでは p を畳まない（spec: "If the Document is NOT set to
             * quirks mode, and ... has a p element in button scope" の条件） */
            if (!b->dom->quirks) close_p_if_open(b);
            insert_element(b, &tok, true);
            pend_reset(b);
            b->mode = M_IN_TABLE;
            b->frameset_ok = false;
            return;
        }
        /* form: pointer が既にあれば重複を無視（ネスト form は DOM に出さない WHATWG） */
        if (t == IF_TAG_FORM) {
            if (b->form) { b->dom->n_errors++; return; }
            close_p_if_open(b);
            insert_element(b, &tok, true);
            b->form = top(b);
            return;
        }
        /* template: content 分離 + template 挿入モードへ（in-body/他モード共通の規則） */
        if (t == IF_TAG_TEMPLATE) { tpl_start(b, &tok); return; }
        /* select（現行仕様の in-body 規則: 挿入モード切替なし。
         * 既定スコープに select があれば parse error + 畳んでトークン自体は無視） */
        if (t == IF_TAG_SELECT) {
            if (has_in_default_scope_tag(b, IF_TAG_SELECT)) {
                b->dom->n_errors++;
                pop_until(b, IF_TAG_SELECT);
                return;
            }
            afe_reconstruct(b);
            insert_element(b, &tok, true);
            b->frameset_ok = false;
            return;
        }
        /* option: select-scope 内なら implied end tags（except optgroup）で畳む。
         * 外なら current==option の畳み込みのみ（現行仕様の二肢構造） */
        if (t == IF_TAG_OPTION) {
            if (has_in_default_scope_tag(b, IF_TAG_SELECT)) {
                gen_implied(b, IF_TAG_OPTGROUP);
                if (has_in_default_scope_tag(b, IF_TAG_OPTION)) b->dom->n_errors++;
            } else if (top(b)->tag == IF_TAG_OPTION) {
                pop(b);
            }
            afe_reconstruct(b);
            insert_element(b, &tok, true);
            return;
        }
        /* optgroup: select-scope 内なら implied end tags で option/optgroup を畳む */
        if (t == IF_TAG_OPTGROUP) {
            if (has_in_default_scope_tag(b, IF_TAG_SELECT)) {
                gen_implied(b, 0);
                if (has_in_default_scope_tag(b, IF_TAG_OPTION) ||
                    has_in_default_scope_tag(b, IF_TAG_OPTGROUP)) b->dom->n_errors++;
            } else if (top(b)->tag == IF_TAG_OPTION) {
                pop(b);
            }
            afe_reconstruct(b);
            insert_element(b, &tok, true);
            return;
        }
        /* 書式要素: reconstruct → 挿入 → AFE へ。nobr は二重 nobr の仕様 quirk 込み */
        if (is_formatting(t)) {
            if (t == IF_TAG_A) {
                /* 同名 AFE が残っているなら parse error: AAA を走らせ、
                 * AAA が畳み損ねた場合でも旧 a を AFE/stack から確実に除去する
                 * （<a><p>X<a>Y… で旧 a が p の外に出て空で残る spec 挙動） */
                i32 fi = afe_find_tag(b, IF_TAG_A);
                if (fi >= 0) {
                    IfNode *old = b->afe[fi].n;
                    b->dom->n_errors++;
                    adoption(b, IF_TAG_A);
                    if (afe_find_node(b, old) >= 0) afe_remove_at(b, (u32)afe_find_node(b, old));
                    for (u32 i = 0; i < b->depth; i++)
                        if (b->stack[i] == old) { stack_remove_at(b, i); break; }
                }
            }
            afe_reconstruct(b);
            if (t == IF_TAG_NOBR && has_in_default_scope_tag(b, IF_TAG_NOBR)) {
                b->dom->n_errors++;
                adoption(b, IF_TAG_NOBR);
                afe_reconstruct(b);
            }
            insert_element(b, &tok, true);
            afe_push(b, top(b));
            return;
        }
        /* in-body の from-head void 集合: in-head 規則（reconstruct なし）で void insert */
        if (t == IF_TAG_BASE || t == IF_TAG_BASEFONT || t == IF_TAG_BGSOUND ||
            t == IF_TAG_LINK || t == IF_TAG_META) {
            insert_element(b, &tok, false);
            return;
        }
        /* button（13.2.6.4.7）: scope 内の button を畳む → reconstruct → 挿入 → not-ok */
        if (t == IF_TAG_BUTTON) {
            if (has_in_scope2(b, IF_TAG_BUTTON, SC_DEFAULT)) {
                b->dom->n_errors++;
                gen_implied(b, 0);
                pop_until(b, IF_TAG_BUTTON);
            }
            afe_reconstruct(b);
            insert_element(b, &tok, true);
            b->frameset_ok = false;
            return;
        }
        /* plaintext（13.2.6.4.7）: p を閉じる → 挿入 → PLAINTEXT（reconstruct/not-ok 無し） */
        if (t == IF_TAG_PLAINTEXT) {
            close_p_if_open(b);
            insert_element(b, &tok, true);
            b->tok.plaintext = 1;
            return;
        }
        /* table 内部要素の開始タグ（caption/col/colgroup/frame/tbody/td/tfoot/th/thead/tr）:
         * in-body 規則では parse error で無視。ただし template が stack にある文脈では
         * WPT dataset（template.dat）が content 内保持を期待するので通常挿入に落とす
         * （現行仕様は in-template で in-table-body/in-row へ re-dispatch するが、
         * dataset は template 内 <td>/<tr> を content 内要素として保持する挙動で収録） */
        if (!has_open(b, IF_TAG_TEMPLATE)) {
            switch (t) {
            case IF_TAG_CAPTION: case IF_TAG_COL: case IF_TAG_COLGROUP: case IF_TAG_FRAME:
            case IF_TAG_TBODY: case IF_TAG_TD: case IF_TAG_TFOOT: case IF_TAG_TH:
            case IF_TAG_THEAD: case IF_TAG_TR:
                b->dom->n_errors++;
                return;
            default:
                break;
            }
        }
        if (closes_p(t)) close_p_if_open(b);
        /* スコープ障壁となる要素（object/applet/marquee）: reconstruct → 挿入 → marker */
        if ((t == IF_TAG_OBJECT || t == IF_TAG_APPLET || t == IF_TAG_MARQUEE) && !tok.self_closing) {
            afe_reconstruct(b);
            insert_element(b, &tok, true);
            afe_insert_marker(b);
            b->frameset_ok = false;
            return;
        }
        if (t == IF_TAG_LI) {
            b->frameset_ok = false;
            implied_close(b, IF_TAG_LI, 0, IF_TAG_UL, IF_TAG_OL);
            close_p_if_open(b);
        }
        if (t == IF_TAG_DT || t == IF_TAG_DD) {
            b->frameset_ok = false;
            implied_close(b, IF_TAG_DT, IF_TAG_DD, IF_TAG_DL, 0);
            close_p_if_open(b);
        }
        /* ruby 系: rb/rp/rt/rtc は ruby 文脈の implied end tags。
         * rtc は閉じない（rt は rtc の子として残る仕様）、rb/rp/rt は互いに閉じ合う。
         * ruby がスコープに無い時は通常要素として挿入（parse error なし近似は台帳） */
        if (t == IF_TAG_RB || t == IF_TAG_RP || t == IF_TAG_RT || t == IF_TAG_RTC) {
            if (has_in_default_scope_tag(b, IF_TAG_RUBY)) {
                /* rb/rtc は rtc も含めて全畳み、rp/rt は rtc を残す（tests19 の
                 * <rtc>..</rt><rb> パターンで実測検証した規則） */
                bool keep_rtc = (t == IF_TAG_RP || t == IF_TAG_RT);
                while (b->depth) {
                    u16 ct = top(b)->tag;
                    if (keep_rtc && ct == IF_TAG_RTC) break;
                    switch (ct) {
                    case IF_TAG_DD: case IF_TAG_DT: case IF_TAG_LI: case IF_TAG_OPTGROUP:
                    case IF_TAG_OPTION: case IF_TAG_P: case IF_TAG_RP: case IF_TAG_RT:
                    case IF_TAG_RB: case IF_TAG_RTC:
                        pop(b);
                        continue;
                    default:
                        goto ruby_ctx_done;
                    }
                }
            }
        ruby_ctx_done:
            /* spec の rb/rtc/rp/rt 規則は reconstruct を要求しない（純粋な挿入） */
            insert_element(b, &tok, !tok.self_closing);
            return;
        }
        /* pre/listing/xmp/textarea: 直後の LF 1 個を無視（textarea も仕様どおり） */
        if (t == IF_TAG_PRE || t == IF_TAG_LISTING || t == IF_TAG_XMP || t == IF_TAG_TEXTAREA)
            b->skip_lf = 1;
        /* not-ok 化（startTag 規則群、html5lib 1.1 との差分検証済み） */
        if (t == IF_TAG_PRE || t == IF_TAG_LISTING || t == IF_TAG_XMP || t == IF_TAG_TEXTAREA ||
            t == IF_TAG_IFRAME)
            b->frameset_ok = false;
        if (t == IF_TAG_XMP) afe_reconstruct(b); /* rawtext 系で xmp のみ spec が要求 */
        if (t >= IF_TAG_H1 && t <= IF_TAG_H6) close_heading_if_open(b);

        /* ブロック家系 + li/dd/dt + heading + ruby コンテキスト要素の共通挿入規則:
         * AFE reconstruct を「行わない」HTML 要素挿入が仕様（ここへ到達するまでに
         * close_p/implied_close/skip_lf/frameset-ok 等の個別規則は適用済み）。
         * 従来は末尾の anything-else 経路へ落ちて reconstruct されてしまい、
         * <p><font>..<p> 系で両方フォントが新 p を包む誤 DOM になっていた。 */
        switch (t) {
        case IF_TAG_P: case IF_TAG_DIV: case IF_TAG_UL: case IF_TAG_OL: case IF_TAG_DL:
        case IF_TAG_PRE: case IF_TAG_LISTING: case IF_TAG_BLOCKQUOTE: case IF_TAG_ADDRESS:
        case IF_TAG_ARTICLE: case IF_TAG_ASIDE: case IF_TAG_FOOTER: case IF_TAG_HEADER:
        case IF_TAG_MAIN: case IF_TAG_NAV: case IF_TAG_SECTION: case IF_TAG_FIELDSET:
        case IF_TAG_FIGURE: case IF_TAG_FIGCAPTION: case IF_TAG_CENTER:
        case IF_TAG_DETAILS: case IF_TAG_DIALOG: case IF_TAG_DIR: case IF_TAG_MENU:
        case IF_TAG_HGROUP: case IF_TAG_SEARCH: case IF_TAG_SUMMARY:
        case IF_TAG_LI: case IF_TAG_DD: case IF_TAG_DT:
        case IF_TAG_H1: case IF_TAG_H2: case IF_TAG_H3: case IF_TAG_H4:
        case IF_TAG_H5: case IF_TAG_H6:
        case IF_TAG_RB: case IF_TAG_RP: case IF_TAG_RT: case IF_TAG_RTC:
            insert_element(b, &tok, true);
            return;
        default:
            break;
        }
        if (if_tag_is_void(t)) {
            /* input: select-scope 規則 + type=hidden 以外で not-ok、reconstruct あり */
            if (t == IF_TAG_INPUT) {
                if (has_in_default_scope_tag(b, IF_TAG_SELECT)) {
                    b->dom->n_errors++;
                    pop_until(b, IF_TAG_SELECT);
                }
                afe_reconstruct(b);
                IfStr ty = if_str(NULL, 0);
                for (u32 i = 0; i < tok.n_attrs; i++)
                    if (if_str_eq_ci(tok.attrs[i].name, IF_S("type"))) { ty = tok.attrs[i].value; break; }
                if (!if_str_eq_ci(ty, IF_S("hidden"))) b->frameset_ok = false;
            } else {
                /* area/br/embed/img/keygen/wbr は not-ok + reconstruct（spec の共通形） */
                if (t == IF_TAG_AREA || t == IF_TAG_BR || t == IF_TAG_EMBED ||
                    t == IF_TAG_IMG || t == IF_TAG_KEYGEN || t == IF_TAG_WBR) {
                    b->frameset_ok = false;
                    afe_reconstruct(b);
                }
                if (t == IF_TAG_HR) {
                    /* select-scope 規則（現行仕様）: option/optgroup を implied ends で畳み、
                     * 畳み切れなければ parse error。hr 自体は select 内に void 挿入される */
                    if (has_in_default_scope_tag(b, IF_TAG_SELECT)) {
                        gen_implied(b, 0);
                        if (has_in_default_scope_tag(b, IF_TAG_OPTION) ||
                            has_in_default_scope_tag(b, IF_TAG_OPTGROUP)) b->dom->n_errors++;
                    }
                    b->frameset_ok = false;
                }
            }
            insert_element(b, &tok, false);
            return;
        }
        if (rawish(t)) {
            insert_element(b, &tok, true);
            if_tok_set_raw(&b->tok, t);
            return;
        }
        /* "anything else" start: spec は reconstruct → insert */
        if (!tok.self_closing) afe_reconstruct(b);
        insert_element(b, &tok, !tok.self_closing);
        return;
    }

    /* TOK_END */
    if (in_foreign(b, &tok) && !b->no_foreign) { foreign_step(b, &tok); return; }
    u16 t = tok.tag;
    if (t == IF_TAG_BODY) { b->mode = M_AFTER_BODY; return; }
    if (t == IF_TAG_HTML) { b->mode = M_AFTER_AFTER_BODY; return; }
    if (t == IF_TAG_BR) {
        /* 仕様の quirk: </br> は <br> として扱う */
        IfTok br = { .kind = TOK_START, .tag = IF_TAG_BR };
        step_in_body(b, br);
        return;
    }
    if (t == IF_TAG_FORM && b->form) {
        /* form pointer 規則（template 無し時）: pointer 解除 → implied ends →
         * stack から node を「除去」（pop_until ではない — 中間要素は残る。
         * <form><div></form><div> で 2 個目の div が 1 個目の中に入る根拠） */
        IfNode *node = b->form;
        b->form = NULL;
        bool in_scope = false;
        for (u32 i = b->depth; i > 0; i--) {
            IfNode *s = b->stack[i - 1];
            if (s == node) { in_scope = true; break; }
            if (scope_barrier(s, SC_DEFAULT)) break;
        }
        if (!in_scope) { b->dom->n_errors++; return; }
        gen_implied(b, 0);
        if (top(b) != node) b->dom->n_errors++;
        for (u32 i = 0; i < b->depth; i++)
            if (b->stack[i] == node) { stack_remove_at(b, i); break; }
        return;
    }
    if (t == IF_TAG_TEMPLATE) { tpl_end(b); return; }
    /* 書式要素の終了: adoption agency（outer ≤8, inner ≤3 の打ち切り込み） */
    if (is_formatting(t)) { adoption(b, t); return; }
    if (t == IF_TAG_P) {
        /* button スコープに p が無ければ空 p を挿入してから畳む（仕様の quirk 完結形） */
        if (!has_in_scope2(b, IF_TAG_P, SC_BUTTON)) {
            b->dom->n_errors++;
            IfTok p = { .kind = TOK_START, .tag = IF_TAG_P };
            insert_element(b, &p, true);
        }
        close_p_if_open(b);
        return;
    }
    /* li: list-item スコープ（既定 + ol/ul で遮断）・dd/dt: 既定スコープで except-self implied */
    if (t == IF_TAG_LI) {
        end_in_scope(b, t, SC_LIST_ITEM, true);
        return;
    }
    if (t == IF_TAG_DD || t == IF_TAG_DT) {
        end_in_scope(b, t, SC_DEFAULT, true);
        return;
    }
    /* h1..h6: implied 生成 → どれかの heading まで畳む */
    if (t >= IF_TAG_H1 && t <= IF_TAG_H6) { end_hgroup(b); return; }
    /* applet/marquee/object: implied 生成 → 畳む → AFE marker 消去 */
    if (t == IF_TAG_APPLET || t == IF_TAG_MARQUEE || t == IF_TAG_OBJECT) {
        if (!has_open(b, t)) { b->dom->n_errors++; return; }
        gen_implied(b, 0);
        if (top(b)->tag != t) b->dom->n_errors++;
        pop_until(b, t);
        afe_clear_to_marker(b);
        return;
    }
    /* ブロック群（address/div/ol/pre/... + select）: 既定スコープ + implied 生成で畳む。
     * select は現行仕様でこの一覧 end-tag 規則に統合された（in-select モード廃止） */
    switch (t) {
    case IF_TAG_ADDRESS: case IF_TAG_ARTICLE: case IF_TAG_ASIDE: case IF_TAG_BLOCKQUOTE:
    case IF_TAG_BUTTON: case IF_TAG_CENTER: case IF_TAG_DIR: case IF_TAG_DIV:
    case IF_TAG_DL: case IF_TAG_FIELDSET: case IF_TAG_FIGCAPTION: case IF_TAG_FIGURE:
    case IF_TAG_FOOTER: case IF_TAG_HEADER: case IF_TAG_LISTING: case IF_TAG_MAIN:
    case IF_TAG_MENU: case IF_TAG_NAV: case IF_TAG_OL: case IF_TAG_PRE:
    case IF_TAG_SECTION: case IF_TAG_UL:
    case IF_TAG_DETAILS: case IF_TAG_DIALOG: case IF_TAG_HGROUP: case IF_TAG_SEARCH:
    case IF_TAG_SUMMARY:
    case IF_TAG_SELECT:
        end_in_scope(b, t, SC_DEFAULT, false);
        return;
    default:
        break;
    }
    /* option/optgroup の終了タグは専用規則を持たない — "any other end tag" の
     * 下降探索で処理される（両者は special カテゴリ外なので降りて来られる） */
    /* その他の終了タグ: spec の「any other end tag」規則（implied 生成 + special 障壁） */
    any_other_end_tag(b, t);
}

static void step(IfTB *b, IfTok tok) {
    /* pre/listing/xmp の直後 1 トークンだけ先頭 LF を無視（WHATWG）。次トークンで必ず消費 */
    if (b->skip_lf) {
        b->skip_lf = 0;
        if (tok.kind == TOK_TEXT && tok.text.n && tok.text.p[0] == '\n') {
            tok.text.p++; tok.text.n--;
            if (!tok.text.n) return;
        }
    }
    /* rawtext 要素が開いている間、その TEXT は常に現ノードに追記する（モードに依らない）。
     * これを挿入モード側の規則に紛れ込ませると <title> の中身が body に逃げる。 */
    if (tok.kind == TOK_TEXT && b->depth && rawish(top(b)->tag)) {
        append_text(b, tok.text);
        return;
    }
    /* rawtext/RCDATA 要素の END は mode 問わず「top 一致で pop」のみ（text mode 規則）。 */
    if (tok.kind == TOK_END && b->depth && rawish(top(b)->tag) && top(b)->tag == tok.tag &&
        top(b)->ns == IF_NS_HTML) {
        pop(b);
        return;
    }
    /* table 系モード横断の in-table-text 規則: 非テキストトークン到着時は、いかなる
     * スタック変更より先に pend をフラッシュする（html5lib の InTableText と同相）。
     * これを怠ると </table> で tbody/... を pop した後に " " が table の子へ落ちる
     * （tests7#12: <table><TBODY><script> <tr>x </script> </table>）。 */
    switch (b->mode) {
    case M_IN_TABLE: case M_IN_CAPTION: case M_IN_COLUMN_GROUP:
    case M_IN_TABLE_BODY: case M_IN_ROW: case M_IN_CELL:
        if (tok.kind != TOK_TEXT) pend_flush(b);
        break;
    default: break;
    }
    switch (b->mode) {
    case M_INITIAL:     step_initial(b, tok); break;
    case M_BEFORE_HTML: step_before_html(b, tok); break;
    case M_BEFORE_HEAD: step_before_head(b, tok); break;
    case M_IN_HEAD:     step_in_head(b, tok); break;
    case M_AFTER_HEAD:  step_after_head(b, tok); break;
    case M_IN_BODY:     step_in_body(b, tok); break;
    case M_IN_TABLE:    step_in_table(b, tok); break;
    case M_IN_CAPTION:  step_in_caption(b, tok); break;
    case M_IN_COLUMN_GROUP: step_in_colgroup(b, tok); break;
    case M_IN_TABLE_BODY:  step_in_table_body(b, tok); break;
    case M_IN_ROW:      step_in_row(b, tok); break;
    case M_IN_CELL:     step_in_cell(b, tok); break;
    case M_IN_FRAMESET: step_in_frameset(b, tok); break;
    case M_AFTER_FRAMESET: step_after_frameset(b, tok); break;
    case M_AFTER_AFTER_FRAMESET: step_after_after_frameset(b, tok); break;
    case M_AFTER_BODY:
        /* 仕様: コメントは html 要素の最後の子、空白は in-body 規則、EOF で終了。
         * それ以外は parse error で in-body に戻って再処理。 */
        if (tok.kind == TOK_COMMENT) {
            insert_comment(b, b->html ? b->html : b->dom->root, &tok);
            return;
        }
        if (tok.kind == TOK_TEXT && if_str_is_ws_only(tok.text)) { step_in_body(b, tok); return; }
        if (tok.kind == TOK_EOF) { b->stopped = true; return; }
        b->dom->n_errors++;
        b->mode = M_IN_BODY;
        step(b, tok);
        return;
    case M_AFTER_AFTER_BODY:
        /* 仕様: コメントは Document、空白は in-body 規則、EOF で終了。 */
        if (tok.kind == TOK_COMMENT) { insert_comment(b, b->dom->root, &tok); return; }
        if (tok.kind == TOK_TEXT && if_str_is_ws_only(tok.text)) { step_in_body(b, tok); return; }
        if (tok.kind == TOK_EOF) { b->stopped = true; return; }
        b->dom->n_errors++;
        b->mode = M_IN_BODY;
        step(b, tok);
        return;
    }
}

IfDom *if_parse_html(IfArena *arena, IfStr input) {
    if (input.n > IF_MAX_INPUT_BYTES) if_fatal("input exceeds per-page byte limit");
    IfDom *dom = (IfDom *)if_arena_calloc(arena, sizeof(IfDom));
    dom->arena = arena;
    IfNode *root = (IfNode *)if_arena_calloc(arena, sizeof(IfNode));
    root->kind = IF_NODE_DOCUMENT;
    dom->root = root;
    dom->n_nodes = 1;

    IfTB b;
    memset(&b, 0, sizeof b);
    b.arena = arena;
    b.dom = dom;
    b.mode = M_INITIAL;
    b.frameset_ok = true; /* WHATWG: 初期値 "ok" */
    if_tok_init(&b.tok, arena, input);

    while (!b.stopped) {
        b.tok.cdata_foreign = in_foreign_text(&b) ? 1 : 0;
        IfTok tok = if_tok_next(&b.tok);
        step(&b, tok);
        if (tok.kind == TOK_EOF) break;
        if (dom->n_nodes > IF_MAX_DOM_NODES) { dom->n_errors++; break; } /* 攻撃文書: 打ち切り（crash ではない） */
    }
    dom->n_errors += b.tok.errors;

    /* title の回収 */
    if (b.head) {
        for (IfNode *c = b.head->first_child; c; c = c->next_sibling) {
            if (c->kind == IF_NODE_ELEMENT && c->tag == IF_TAG_TITLE) {
                dom->title = if_str_trim(if_dom_text_content(arena, c));
                break;
            }
        }
    }
    return dom;
}

/* ================= foreign content（MathML/SVG 名前空間） =================
 * 目的: WHATWG "The rules for parsing tokens in foreign content" の実装可能な中核。
 *   - svg/math ルートで名前空間入域、integration points で HTML に復帰
 *   - breakout 開始タグで HTML namespace まで pop して HTML として再処理
 *   - タグ名/属性名の case 調整（下の表）
 * 未実装（次の台帳項目）: template の content 分離、foster parenting、frameset。
 */

/* SVG タグ名の case 調整（lowercase → canonical） */
static const struct { const char *lc, *canon; } IF_SVG_TAG_ADJUST[] = {
    {"altglyph","altGlyph"}, {"altglyphdef","altGlyphDef"}, {"altglyphitem","altGlyphItem"},
    {"animatecolor","animateColor"}, {"animatemotion","animateMotion"},
    {"animatetransform","animateTransform"}, {"clippath","clipPath"},
    {"feblend","feBlend"}, {"fecolormatrix","feColorMatrix"},
    {"fecomponenttransfer","feComponentTransfer"}, {"fecomposite","feComposite"},
    {"feconvolvematrix","feConvolveMatrix"}, {"fediffuselighting","feDiffuseLighting"},
    {"fedisplacementmap","feDisplacementMap"}, {"fedistantlight","feDistantLight"},
    {"fedropshadow","feDropShadow"}, {"feflood","feFlood"}, {"fefunca","feFuncA"},
    {"fefuncb","feFuncB"}, {"fefuncg","feFuncG"}, {"fefuncr","feFuncR"},
    {"fegaussianblur","feGaussianBlur"}, {"feimage","feImage"}, {"femerge","feMerge"},
    {"femergenode","feMergeNode"}, {"femorphology","feMorphology"}, {"feoffset","feOffset"},
    {"fepointlight","fePointLight"}, {"fespecularlighting","feSpecularLighting"},
    {"fespotlight","feSpotLight"}, {"fetile","feTile"}, {"feturbulence","feTurbulence"},
    {"foreignobject","foreignObject"}, {"glyphref","glyphRef"},
    {"lineargradient","linearGradient"}, {"radialgradient","radialGradient"},
    {"textpath","textPath"},
};

/* SVG/MathML 属性の case 調整 + 名前空間付き属性名の正規形 */
static const char *IF_SVG_ATTR_ADJUST_LC[] = {
    "attributename","attributetype","basefrequency","baseprofile","calcmode",
    "clippathunits","diffuseconstant","edgemode","filterunits","glyphref",
    "gradienttransform","gradientunits","kernelmatrix","kernelunitlength",
    "keypoints","keysplines","keytimes","lengthadjust","limitingconeangle",
    "markerheight","markerunits","markerwidth","maskcontentunits","maskunits",
    "numoctaves","pathlength","patterncontentunits","patterntransform",
    "patternunits","pointsatx","pointsaty","pointsatz","preservealpha",
    "preserveaspectratio","primitiveunits","refx","refy","repeatcount","repeatdur",
    "requiredextensions","requiredfeatures","specularconstant","specularexponent",
    "spreadmethod","startoffset","stddeviation","stitchtiles","surfacescale",
    "systemlanguage","tablevalues","targetx","targety","textlength","viewbox",
    "viewtarget","xchannelselector","ychannelselector","zoomandpan",
};
static const char *IF_SVG_ATTR_ADJUST_CANON[] = {
    "attributeName","attributeType","baseFrequency","baseProfile","calcMode",
    "clipPathUnits","diffuseConstant","edgeMode","filterUnits","glyphRef",
    "gradientTransform","gradientUnits","kernelMatrix","kernelUnitLength",
    "keyPoints","keySplines","keyTimes","lengthAdjust","limitingConeAngle",
    "markerHeight","markerUnits","markerWidth","maskContentUnits","maskUnits",
    "numOctaves","pathLength","patternContentUnits","patternTransform",
    "patternUnits","pointsAtX","pointsAtY","pointsAtZ","preserveAlpha",
    "preserveAspectRatio","primitiveUnits","refX","refY","repeatCount","repeatDur",
    "requiredExtensions","requiredFeatures","specularConstant","specularExponent",
    "spreadMethod","startOffset","stdDeviation","stitchTiles","surfaceScale",
    "systemLanguage","tableValues","targetX","targetY","textLength","viewBox",
    "viewTarget","xChannelSelector","yChannelSelector","zoomAndPan",
};

/* foreign 属性の接頭辞分離（"adjust foreign attributes"）。wptdom 形式は
 * "prefix local" — DOM 上もその表記で保持する（意味論的名前空間分割は後段へ送る）。
 * 台帳: 現行仕様は xml:base も調整対象だが、vendored dataset (wpt@0acb81f)
 * は webkit02#22 で xml:base をリテラル保持のまま収録している（旧仕様世代の
 * 期待値）。dataset 第一主義で xml:base は調整しない。 */
static const struct { const char *from, *to; } IF_FOREIGN_ATTR_ADJUST[] = {
    {"xlink:actuate","xlink actuate"}, {"xlink:arcrole","xlink arcrole"},
    {"xlink:href","xlink href"}, {"xlink:role","xlink role"},
    {"xlink:show","xlink show"}, {"xlink:title","xlink title"},
    {"xlink:type","xlink type"},
    {"xml:lang","xml lang"}, {"xml:space","xml space"},
    {"xmlns:xlink","xmlns xlink"},
};

/* 属性名の調整（foreign 要素にのみ適用。該当しなければ元のまま）。
 * definitionurl→definitionURL は MathML 属性調整（svg 上ではリテラル残し:
 * webkit02#22 の期待値が確証）、camelCase 表調整は SVG 要素にのみ。 */
static IfStr adjust_attr_name(IfTB *b, IfStr name, u8 ns) {
    if (ns == IF_NS_MATHML && if_str_eq_ci(name, IF_S("definitionurl")))
        return IF_S("definitionURL");
    if (ns == IF_NS_SVG)
        for (u32 i = 0; i < sizeof IF_SVG_ATTR_ADJUST_LC / sizeof IF_SVG_ATTR_ADJUST_LC[0]; i++)
            if (if_str_eq_ci(name, if_str(IF_SVG_ATTR_ADJUST_LC[i], (u32)strlen(IF_SVG_ATTR_ADJUST_LC[i]))))
                return if_str(IF_SVG_ATTR_ADJUST_CANON[i], (u32)strlen(IF_SVG_ATTR_ADJUST_CANON[i]));
    for (u32 i = 0; i < sizeof IF_FOREIGN_ATTR_ADJUST / sizeof IF_FOREIGN_ATTR_ADJUST[0]; i++) {
        IfStr from = if_str(IF_FOREIGN_ATTR_ADJUST[i].from,
                            (u32)strlen(IF_FOREIGN_ATTR_ADJUST[i].from));
        if (if_str_eq(name, from)) /* 接頭辞系は case-sensitive マッチ（spec表再現） */
            return if_str(IF_FOREIGN_ATTR_ADJUST[i].to,
                          (u32)strlen(IF_FOREIGN_ATTR_ADJUST[i].to));
    }
    (void)b;
    return name;
}

/* integration points */
static bool is_html_ip(const IfNode *n) { /* HTML integration points (svg 内) */
    return n->ns == IF_NS_SVG && (n->tag == IF_TAG_FOREIGNOBJECT || n->tag == IF_TAG_DESC ||
                                  n->tag == IF_TAG_TITLE);
}
static bool is_math_ip(const IfNode *n) { /* MathML text integration points */
    return n->ns == IF_NS_MATHML && (n->tag == IF_TAG_MI || n->tag == IF_TAG_MO ||
                                     n->tag == IF_TAG_MN || n->tag == IF_TAG_MS ||
                                     n->tag == IF_TAG_MTEXT);
}

/* adjusted current node が foreign content か（start/end トークンについて）。
 * テキスト/CDATA 用に別建ての in_foreign_text も下に用意する。 */
static bool in_foreign(const IfTB *b, const IfTok *tok) {
    if (!b->depth) return false;
    IfNode *node = b->stack[b->depth - 1];
    if (node->ns == IF_NS_HTML) return false;
    if (tok->kind == TOK_START) {
        if (is_math_ip(node) && tok->tag != IF_TAG_MGLYPH && tok->tag != IF_TAG_MALIGNMARK)
            return false;
        if (node->ns == IF_NS_MATHML && node->tag == IF_TAG_ANNOTATION_XML) {
            /* annotation-xml 内の <svg> start は HTML content 扱い（spec の例外:
             * "if token is a start tag whose tag name is 'svg'"） */
            if (tok->tag == IF_TAG_SVG) return false;
            IfStr enc = if_dom_attr(node, "encoding");
            if (if_str_eq_ci(enc, IF_S("text/html")) ||
                if_str_eq_ci(enc, IF_S("application/xhtml+xml"))) return false;
        }
        if (is_html_ip(node)) return false;
        return true;
    }
    if (tok->kind == TOK_END) return true;
    return false;
}

static bool in_foreign_text(const IfTB *b) {
    if (!b->depth) return false;
    IfNode *node = b->stack[b->depth - 1];
    if (node->ns == IF_NS_HTML) return false;
    if (is_math_ip(node) || is_html_ip(node)) return false;
    return true;
}

/* breakout 開始タグ（foreign 側でこれが来たら HTML namespace まで pop して再処理） */
static bool is_breakout_start(const IfTok *tok) {
    switch (tok->tag) {
    case IF_TAG_B: case IF_TAG_BIG: case IF_TAG_BLOCKQUOTE: case IF_TAG_BODY:
    case IF_TAG_BR: case IF_TAG_CENTER: case IF_TAG_CODE: case IF_TAG_DD:
    case IF_TAG_DIV: case IF_TAG_DL: case IF_TAG_DT: case IF_TAG_EM:
    case IF_TAG_H1: case IF_TAG_H2: case IF_TAG_H3: case IF_TAG_H4:
    case IF_TAG_H5: case IF_TAG_H6: case IF_TAG_HEAD: case IF_TAG_HR:
    case IF_TAG_I: case IF_TAG_IMG: case IF_TAG_LI: case IF_TAG_OL:
    case IF_TAG_P: case IF_TAG_PRE: case IF_TAG_S: case IF_TAG_SMALL:
    case IF_TAG_SPAN: case IF_TAG_STRONG: case IF_TAG_STRIKE: case IF_TAG_SUB:
    case IF_TAG_SUP: case IF_TAG_TABLE: case IF_TAG_TT: case IF_TAG_U:
    case IF_TAG_UL: case IF_TAG_VAR:
        return true;
    case IF_TAG_FONT:
        /* color/face/size のいずれかの属性があるときのみ breakout */
        for (u32 i = 0; i < tok->n_attrs; i++) {
            IfStr a = tok->attrs[i].name;
            if (if_str_eq_ci(a, IF_S("color")) || if_str_eq_ci(a, IF_S("face")) ||
                if_str_eq_ci(a, IF_S("size"))) return true;
        }
        return false;
    default:
        return false;
    }
}

/* foreign 要素の tag/attr 名の case 調整（適用対象のみ書き換える） */
static void foreign_adjust(IfTB *b, IfNode *n) {
    if (n->ns == IF_NS_SVG) {
        for (u32 i = 0; i < sizeof IF_SVG_TAG_ADJUST / sizeof IF_SVG_TAG_ADJUST[0]; i++) {
            IfStr lc = if_str(IF_SVG_TAG_ADJUST[i].lc, (u32)strlen(IF_SVG_TAG_ADJUST[i].lc));
            if (if_str_eq_ci(n->u.tag_name, lc)) {
                n->u.tag_name = if_str(IF_SVG_TAG_ADJUST[i].canon,
                                     (u32)strlen(IF_SVG_TAG_ADJUST[i].canon));
                break;
            }
        }
    }
    if (n->n_attrs) {
        IfAttr *adj = (IfAttr *)if_arena_alloc(b->arena, (u64)n->n_attrs * sizeof(IfAttr));
        for (u32 i = 0; i < n->n_attrs; i++) {
            adj[i] = n->attrs[i];
            adj[i].name = adjust_attr_name(b, n->attrs[i].name, n->ns);
        }
        n->attrs = adj;
    }
}

/* foreign 要素の挿入: ns 伝播 + case 調整。self_closing を尊重する。 */
static void foreign_insert(IfTB *b, const IfTok *tok) {
    IfNode *n = make_element(b, tok);
    n->ns = top(b)->ns;
    foreign_adjust(b, n);
    append_placed(b, n); /* template top なら content へ、foster なら「table の兄」へ */
    if (!tok->self_closing) {
        if (b->depth < IF_MAX_STACK_DEPTH) push(b, n);
        else b->dom->n_errors++;
    }
}

static IfStr tok_end_name(const IfTok *tok) {
    if (tok->tag != IF_TAG_UNKNOWN) {
        const char *s = if_tag_name(tok->tag);
        if (s) return if_str(s, (u32)strlen(s));
    }
    return tok->tag_raw;
}

static void foreign_step(IfTB *b, const IfTok *tok) {
    if (tok->kind == TOK_START) {
        if (is_breakout_start(tok)) {
            b->dom->n_errors++;
            /* HTML namespace（または integration point）まで pop して HTML として再処理 */
            while (b->depth) {
                IfNode *t2 = top(b);
                if (t2->ns == IF_NS_HTML || is_html_ip(t2) || is_math_ip(t2)) break;
                pop(b);
            }
            step_in_body(b, *tok);
            return;
        }
        foreign_insert(b, tok);
        return;
    }
    /* TOK_END の br/p は特例: HTML 名前空間まで pop して in-body 規則へ
     * （<p><math></p>a で math/p を畳み "a" が p の外に出る spec 挙動） */
    if (tok->tag == IF_TAG_BR || tok->tag == IF_TAG_P) {
        b->dom->n_errors++;
        if (b->depth <= 1) return; /* fragment case: 無視 */
        while (b->depth && top(b)->ns != IF_NS_HTML) pop(b);
        step_in_body(b, *tok);
        return;
    }
    /* TOK_END（spec 13.2.6.5 の厳密形）: top が一致すれば pop。
     * 遡り中に foreign 要素の lowercase 一致があればそこまで pop。
     * HTML 名前空間要素に到達したら無視ではなく「現行挿入モードの HTML 規則」で
     * 再処理（<div><svg></div>a で div が畳まれ "a" が外に出る挙動の根拠） */
    IfStr name = tok_end_name(tok);
    if (b->depth && if_str_eq_ci(top(b)->u.tag_name, name)) { pop(b); return; }
    b->dom->n_errors++; /* step 2: 先端のタグ名不一致 */
    for (u32 i = b->depth; i > 0; i--) {
        IfNode *e = b->stack[i - 1];
        if (e->ns == IF_NS_HTML) {
            b->no_foreign = true;        /* このトークンは foreign 再判定を飛ばす */
            step(b, *tok);               /* html5lib の phase.processEndTag 直接呼出し相当 */
            b->no_foreign = false;
            return;
        }
        if (if_str_eq_ci(e->u.tag_name, name)) {
            while (b->depth > i - 1) pop(b);
            return;
        }
    }
    /* 対応する要素がない: 無視 */
}
