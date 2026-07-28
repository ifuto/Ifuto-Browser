/* Ifuto — HTML ツリービルダ（簡易 insertion modes）
 *
 * 仕様からの意図的な偏差（すべて文書化・v0.1 では受理）:
 *   - quirks モードなし（常に no-quirks 相当）
 *   - foster parenting なし（table 内迷子テキストは現在地に置く）
 *   - <table> 内部構造の強制（tbody 暗黙挿入など）は行わない（レイアウト側で block として扱う）
 *   - adoption agency（<b><i></b></i> の修復）は近似: 単純な「積み遡り pop」
 *   - frameset は無視
 *
 * 敵対防御: 深さ上限・ノード総数上限・単調進行の保証は tokenizer 側が請け負う。
 */
#include "html_int.h"
#include "utf8.h"
#include <string.h>

typedef enum {
    M_INITIAL, M_BEFORE_HTML, M_BEFORE_HEAD, M_IN_HEAD, M_AFTER_HEAD, M_IN_BODY, M_AFTER_BODY
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
    bool stopped;
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
        n->tag_name = if_str(lc, tok->tag_raw.n);
    } else {
        const char *s = if_tag_name(tok->tag);
        n->tag_name = if_str(s, (u32)strlen(s));
    }
    n->attrs = tok->attrs;
    n->n_attrs = tok->n_attrs;
    return n;
}

static void insert_element(IfTB *b, const IfTok *tok, bool do_push) {
    IfNode *n = make_element(b, tok);
    append_child(top(b), n);
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
        return true;
    default: return false;
    }
}

static bool has_open(IfTB *b, u16 tag) {
    for (u32 i = b->depth; i > 0; i--)
        if (b->stack[i - 1]->tag == tag) return true;
    return false;
}

static void close_p_if_open(IfTB *b) {
    if (has_open(b, IF_TAG_P))
        while (b->depth && top(b)->tag != IF_TAG_P) pop(b);
    pop_if(b, IF_TAG_P);
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
    /* 直前が TEXT ノードなら連結（ノード数とメモリの節約。コピーは発生するが稀） */
    IfNode *p = top(b);
    IfNode *last = p->last_child;
    if (last && last->kind == IF_NODE_TEXT) {
        /* 隣接していればコピーなし拡張は不可能（別領域）なので、連結コピー */
        u64 nn = (u64)last->text.n + text.n;
        char *buf = (char *)if_arena_alloc(b->arena, nn);
        memcpy(buf, last->text.p, last->text.n);
        memcpy(buf + last->text.n, text.p, text.n);
        last->text = if_str(buf, (u32)nn);
        return;
    }
    IfNode *n = new_node(b, IF_NODE_TEXT);
    n->text = text;
    append_child(p, n);
}

/* モードごとのトークン処理 */
static void step(IfTB *b, IfTok tok);

static void step_initial(IfTB *b, IfTok tok) {
    switch (tok.kind) {
    case TOK_DOCTYPE:
        b->dom->quirks = false; /* doctype 検査は行わない。v0.1 は常に no-quirks */
        return;
    case TOK_COMMENT: return;
    case TOK_TEXT:
        if (if_str_is_ws_only(tok.text)) return;
        /* fallthrough */
    default:
        b->mode = M_BEFORE_HTML;
        step(b, tok);
        return;
    }
}

static void ensure_html(IfTB *b) {
    if (!b->html) {
        IfTok t = { TOK_START, {0}, IF_TAG_HTML, {0}, NULL, 0, false };
        b->html = make_element(b, &t);
        append_child(b->dom->root, b->html);
        push(b, b->html);
    }
}

static void ensure_head(IfTB *b) {
    ensure_html(b);
    if (!b->head) {
        IfTok t = { TOK_START, {0}, IF_TAG_HEAD, {0}, NULL, 0, false };
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
        IfTok t = { TOK_START, {0}, IF_TAG_BODY, {0}, NULL, 0, false };
        b->body = make_element(b, &t);
        append_child(b->html, b->body);
        if (b->depth < IF_MAX_STACK_DEPTH) push(b, b->body);
    }
}

static void step_before_html(IfTB *b, IfTok tok) {
    if (tok.kind == TOK_COMMENT) { IfNode *r = b->dom->root; (void)r; return; }
    if (tok.kind == TOK_TEXT && if_str_is_ws_only(tok.text)) return;
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
    if (tok.kind == TOK_TEXT && if_str_is_ws_only(tok.text)) return;
    if (tok.kind == TOK_COMMENT) return;
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

static void step_in_head(IfTB *b, IfTok tok) {
    if (tok.kind == TOK_COMMENT) return;
    if (tok.kind == TOK_TEXT) {
        if (if_str_is_ws_only(tok.text)) { append_text(b, tok.text); return; }
        b->mode = M_AFTER_HEAD;
        pop_if(b, IF_TAG_HEAD);
        step(b, tok);
        return;
    }
    if (tok.kind == TOK_START) {
        switch (tok.tag) {
        case IF_TAG_TITLE: case IF_TAG_STYLE: case IF_TAG_SCRIPT: case IF_TAG_TEXTAREA:
            insert_element(b, &tok, true);
            if_tok_set_raw(&b->tok, tok.tag);
            return;
        case IF_TAG_META: case IF_TAG_LINK: case IF_TAG_PARAM: case IF_TAG_SOURCE: case IF_TAG_TRACK:
        case IF_TAG_WBR: case IF_TAG_INPUT: case IF_TAG_IMG:
            insert_element(b, &tok, false);
            return;
        case IF_TAG_HEAD:
            b->dom->n_errors++; return; /* 無視 */
        case IF_TAG_NOSCRIPT:
            insert_element(b, &tok, true);
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
        if (tok.tag == IF_TAG_TITLE || tok.tag == IF_TAG_STYLE || tok.tag == IF_TAG_SCRIPT ||
            tok.tag == IF_TAG_TEXTAREA || tok.tag == IF_TAG_NOSCRIPT) {
            pop_if(b, tok.tag);
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
    if (tok.kind == TOK_COMMENT) return; /* コメントノードは生成しない（v0.1 方針: 描画に不要） */
    /* 仕様: after-head の空白は「現在ノード（=html）に挿入」。body を勝手に作らない
     * （これを作ると直後の明示 <body> で二重 body になる——実際に発生した欠陥） */
    if (tok.kind == TOK_TEXT && if_str_is_ws_only(tok.text)) { append_text(b, tok.text); return; }
    if (tok.kind == TOK_START && tok.tag == IF_TAG_BODY) {
        ensure_html(b);
        while (b->depth && top(b)->tag == IF_TAG_HEAD) pop(b);
        if (b->body) {
            /* 2 個目の <body>: 新規作成禁止。不足している属性だけマージする（WHATWG 準拠） */
            b->dom->n_errors++;
            for (u32 i = 0; i < tok.n_attrs; i++) {
                IfStr nm = tok.attrs[i].name;
                bool exists = false;
                for (u32 k = 0; k < b->body->n_attrs; k++)
                    if (if_str_eq_ci(b->body->attrs[k].name, nm)) { exists = true; break; }
                if (exists) continue;
                u64 cap = 0;
                IfAttr *na = (IfAttr *)if_arena_grow(b->arena, NULL, &cap, b->body->n_attrs + 1, sizeof(IfAttr));
                memcpy(na, b->body->attrs, (u64)b->body->n_attrs * sizeof(IfAttr));
                na[b->body->n_attrs] = tok.attrs[i];
                b->body->attrs = na;
                b->body->n_attrs++;
            }
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
    ensure_body(b);
    b->mode = M_IN_BODY;
    step(b, tok);
}

static void step_in_body(IfTB *b, IfTok tok) {
    switch (tok.kind) {
    case TOK_TEXT:
        append_text(b, tok.text);
        return;
    case TOK_COMMENT:
        return;
    case TOK_DOCTYPE:
        b->dom->n_errors++;
        return;
    case TOK_EOF:
        b->stopped = true;
        return;
    case TOK_END:
        break;
    case TOK_START:
        break;
    }

    if (tok.kind == TOK_START) {
        u16 t = tok.tag;
        if (t == IF_TAG_HTML || t == IF_TAG_BODY || t == IF_TAG_HEAD) { b->dom->n_errors++; return; }
        if (closes_p(t)) close_p_if_open(b);
        if (t == IF_TAG_LI) implied_close(b, IF_TAG_LI, 0, IF_TAG_UL, IF_TAG_OL);
        if (t == IF_TAG_DT || t == IF_TAG_DD) implied_close(b, IF_TAG_DT, IF_TAG_DD, IF_TAG_DL, 0);
        if (t >= IF_TAG_H1 && t <= IF_TAG_H6) close_heading_if_open(b);

        if (if_tag_is_void(t)) { insert_element(b, &tok, false); return; }
        if (if_tag_is_rawtext(t)) {
            insert_element(b, &tok, true);
            if_tok_set_raw(&b->tok, t);
            return;
        }
        insert_element(b, &tok, !tok.self_closing);
        return;
    }

    /* TOK_END */
    u16 t = tok.tag;
    if (t == IF_TAG_BODY || t == IF_TAG_HTML) { b->mode = M_AFTER_BODY; return; }
    if (t == IF_TAG_BR) {
        /* 仕様の quirk: </br> は <br> として扱う */
        IfTok br = { TOK_START, {0}, IF_TAG_BR, {0}, NULL, 0, false };
        step_in_body(b, br);
        return;
    }
    if (t == IF_TAG_P && !has_open(b, IF_TAG_P)) {
        /* 開いていない </p> → 空の p を挿入して閉じる（仕様準拠の小qirk） */
        IfTok p = { TOK_START, {0}, IF_TAG_P, {0}, NULL, 0, false };
        insert_element(b, &p, false);
        return;
    }
    /* 一般終了タグ: スタックを遡って一致を探し、見つかればそこまで pop。見つからなければ無視 */
    for (u32 i = b->depth; i > 0; i--) {
        u16 st = b->stack[i - 1]->tag;
        if (st == t) {
            while (b->depth > i - 1) pop(b);
            return;
        }
        if (st == IF_TAG_HTML) break;
    }
    b->dom->n_errors++;
}

static void step(IfTB *b, IfTok tok) {
    /* rawtext 要素が開いている間、その TEXT は常に現ノードに追記する（モードに依らない）。
     * これを挿入モード側の規則に紛れ込ませると <title> の中身が body に逃げる。 */
    if (tok.kind == TOK_TEXT && b->depth && if_tag_is_rawtext(top(b)->tag)) {
        append_text(b, tok.text);
        return;
    }
    switch (b->mode) {
    case M_INITIAL:     step_initial(b, tok); break;
    case M_BEFORE_HTML: step_before_html(b, tok); break;
    case M_BEFORE_HEAD: step_before_head(b, tok); break;
    case M_IN_HEAD:     step_in_head(b, tok); break;
    case M_AFTER_HEAD:  step_after_head(b, tok); break;
    case M_IN_BODY:     step_in_body(b, tok); break;
    case M_AFTER_BODY:
        if (tok.kind == TOK_EOF || tok.kind == TOK_TEXT || tok.kind == TOK_COMMENT) return;
        b->dom->n_errors++;
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
    if_tok_init(&b.tok, arena, input);

    while (!b.stopped) {
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
