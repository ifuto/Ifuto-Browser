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
    M_INITIAL, M_BEFORE_HTML, M_BEFORE_HEAD, M_IN_HEAD, M_AFTER_HEAD, M_IN_BODY, M_AFTER_BODY,
    M_AFTER_AFTER_BODY
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
    bool seen_doctype;
    u8 skip_lf; /* pre/listing/xmp 直後の LF 1 個を無視 */
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

/* rawtext または RCDATA の container 要素か（内容は element の子テキスト1本） */
static bool rawish(u16 t) { return if_tag_is_rawtext(t) || if_tag_is_rcdata(t); }

static void insert_comment(IfTB *b, IfNode *parent, const IfTok *tok) {
    IfNode *n = new_node(b, IF_NODE_COMMENT);
    n->text = tok->text;
    if (tok->is_pi) n->tag_name = tok->pi_target; /* PI は target を tag_name に保持（comment では未使用の欄） */
    append_child(parent, n);
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

static void step_initial(IfTB *b, IfTok tok) {
    switch (tok.kind) {
    case TOK_DOCTYPE:
        b->dom->quirks = false; /* quirks 判定は行わない（no-quirks 固定方針）。 */
        if (!b->seen_doctype) {
            b->seen_doctype = true;
            IfNode *d = new_node(b, IF_NODE_DOCTYPE);
            d->dtype = (IfDoctype *)if_arena_calloc(b->arena, sizeof(IfDoctype));
            d->dtype->name = tok.text;      d->dtype->has_name = tok.dt_has_name;
            d->dtype->pub = tok.dt_pub;     d->dtype->has_pub = tok.dt_has_pub;
            d->dtype->sys = tok.dt_sys;     d->dtype->has_sys = tok.dt_has_sys;
            append_child(b->dom->root, d);
        } else {
            b->dom->n_errors++; /* 複数 doctype は無視 */
        }
        return;
    case TOK_COMMENT: insert_comment(b, b->dom->root, &tok); return;
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

static void step_in_head(IfTB *b, IfTok tok) {
    if (tok.kind == TOK_COMMENT) { insert_comment(b, top(b), &tok); return; }
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
    /* 仕様: after-head の空白は「現在ノード（=html）に挿入」。body を勝手に作らない
     * （これを作ると直後の明示 <body> で二重 body になる——実際に発生した欠陥） */
    if (tok.kind == TOK_TEXT && if_str_is_ws_only(tok.text)) { append_text(b, tok.text); return; }
    if (tok.kind == TOK_START && tok.tag == IF_TAG_BODY) {
        ensure_html(b);
        while (b->depth && top(b)->tag == IF_TAG_HEAD) pop(b);
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
    if (tok.kind == TOK_END && tok.tag != IF_TAG_BODY && tok.tag != IF_TAG_HTML &&
        tok.tag != IF_TAG_BR) {
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
        append_text(b, tok.text);
        return;
    case TOK_COMMENT:
        insert_comment(b, top(b), &tok);
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
        if (in_foreign(b, &tok)) { foreign_step(b, &tok); return; }
        if (t == IF_TAG_SVG || t == IF_TAG_MATH) { /* foreign ルート入域（属性も調整） */
            IfNode *n = make_element(b, &tok);
            n->ns = (t == IF_TAG_SVG) ? IF_NS_SVG : IF_NS_MATHML;
            foreign_adjust(b, n);
            append_child(top(b), n);
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
        if (t == IF_TAG_BODY || t == IF_TAG_HEAD) { b->dom->n_errors++; return; }
        if (closes_p(t)) close_p_if_open(b);
        if (t == IF_TAG_LI) implied_close(b, IF_TAG_LI, 0, IF_TAG_UL, IF_TAG_OL);
        if (t == IF_TAG_DT || t == IF_TAG_DD) implied_close(b, IF_TAG_DT, IF_TAG_DD, IF_TAG_DL, 0);
        /* ruby 系: rp/rt/rtc は implied end tags（近似: p のみ閉じる） */
        if (t == IF_TAG_RP || t == IF_TAG_RT || t == IF_TAG_RTC) close_p_if_open(b);
        /* pre/listing/xmp: 直後の LF 1 個を無視 */
        if (t == IF_TAG_PRE || t == IF_TAG_LISTING || t == IF_TAG_XMP) b->skip_lf = 1;
        if (t == IF_TAG_PLAINTEXT) b->tok.plaintext = 1;
        if (t >= IF_TAG_H1 && t <= IF_TAG_H6) close_heading_if_open(b);

        if (if_tag_is_void(t)) { insert_element(b, &tok, false); return; }
        if (rawish(t)) {
            insert_element(b, &tok, true);
            if_tok_set_raw(&b->tok, t);
            return;
        }
        insert_element(b, &tok, !tok.self_closing);
        return;
    }

    /* TOK_END */
    if (in_foreign(b, &tok)) { foreign_step(b, &tok); return; }
    u16 t = tok.tag;
    if (t == IF_TAG_BODY) { b->mode = M_AFTER_BODY; return; }
    if (t == IF_TAG_HTML) { b->mode = M_AFTER_AFTER_BODY; return; }
    if (t == IF_TAG_BR) {
        /* 仕様の quirk: </br> は <br> として扱う */
        IfTok br = { .kind = TOK_START, .tag = IF_TAG_BR };
        step_in_body(b, br);
        return;
    }
    if (t == IF_TAG_P && !has_open(b, IF_TAG_P)) {
        /* 開いていない </p> → 空の p を挿入して閉じる（仕様準拠の小qirk） */
        IfTok p = { .kind = TOK_START, .tag = IF_TAG_P };
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
    switch (b->mode) {
    case M_INITIAL:     step_initial(b, tok); break;
    case M_BEFORE_HTML: step_before_html(b, tok); break;
    case M_BEFORE_HEAD: step_before_head(b, tok); break;
    case M_IN_HEAD:     step_in_head(b, tok); break;
    case M_AFTER_HEAD:  step_after_head(b, tok); break;
    case M_IN_BODY:     step_in_body(b, tok); break;
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

/* 属性名の調整（foreign 要素にのみ適用。該当しなければ元のまま） */
static IfStr adjust_attr_name(IfTB *b, IfStr name) {
    if (if_str_eq_ci(name, IF_S("definitionurl")))
        return IF_S("definitionURL");
    for (u32 i = 0; i < sizeof IF_SVG_ATTR_ADJUST_LC / sizeof IF_SVG_ATTR_ADJUST_LC[0]; i++)
        if (if_str_eq_ci(name, if_str(IF_SVG_ATTR_ADJUST_LC[i], (u32)strlen(IF_SVG_ATTR_ADJUST_LC[i]))))
            return if_str(IF_SVG_ATTR_ADJUST_CANON[i], (u32)strlen(IF_SVG_ATTR_ADJUST_CANON[i]));
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
            if (if_str_eq_ci(n->tag_name, lc)) {
                n->tag_name = if_str(IF_SVG_TAG_ADJUST[i].canon,
                                     (u32)strlen(IF_SVG_TAG_ADJUST[i].canon));
                break;
            }
        }
    }
    if (n->n_attrs) {
        IfAttr *adj = (IfAttr *)if_arena_alloc(b->arena, (u64)n->n_attrs * sizeof(IfAttr));
        for (u32 i = 0; i < n->n_attrs; i++) {
            adj[i] = n->attrs[i];
            adj[i].name = adjust_attr_name(b, n->attrs[i].name);
        }
        n->attrs = adj;
    }
}

/* foreign 要素の挿入: ns 伝播 + case 調整。self_closing を尊重する。 */
static void foreign_insert(IfTB *b, const IfTok *tok) {
    IfNode *n = make_element(b, tok);
    n->ns = top(b)->ns;
    foreign_adjust(b, n);
    append_child(top(b), n);
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
    /* TOK_END: 現在ノード名（小文字比較）が一致すれば pop、
     * さもなくばスタックを遡って一致する要素まで pop（無ければ無視） */
    IfStr name = tok_end_name(tok);
    if (b->depth && if_str_eq_ci(top(b)->tag_name, name)) { pop(b); return; }
    for (u32 i = b->depth; i > 0; i--) {
        IfNode *e = b->stack[i - 1];
        if (e->ns == IF_NS_HTML) break;
        if (if_str_eq_ci(e->tag_name, name)) {
            b->dom->n_errors++;
            while (b->depth > i - 1) pop(b);
            return;
        }
    }
    b->dom->n_errors++; /* 対応する要素がない: 無視 */
}
