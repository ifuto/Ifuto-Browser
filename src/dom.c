/* Ifuto — DOM 補助（タグ表・属性アクセス・テキスト連結・ダンプ） */
#include "dom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool if_dom_slim = false; /* 既定 full DOM（適合ハーネス）。実ブラウズ経路が true にする */

/* dom.h の IfTag 列挙と 1:1 対応。順序ズレは test_html の round-trip テストが検出する。 */
#define F_VOID 1u
#define F_RAW  2u
#define F_RCDATA 4u
static const struct { const char *s; u8 n; u8 flags; } IF_TAGS[IF_TAG_N_TAGS] = {
    {"", 0, 0},                          /* UNKNOWN */
    {"html",4,0}, {"head",4,0}, {"body",4,0}, {"title",5,F_RCDATA}, {"meta",4,F_VOID},
    {"link",4,F_VOID}, {"style",5,F_RAW}, {"script",6,F_RAW},
    {"div",3,0}, {"span",4,0}, {"p",1,0}, {"a",1,0},
    {"b",1,0}, {"i",1,0}, {"u",1,0}, {"s",1,0},
    {"em",2,0}, {"strong",6,0}, {"code",4,0}, {"pre",3,0}, {"blockquote",10,0},
    {"h1",2,0}, {"h2",2,0}, {"h3",2,0}, {"h4",2,0}, {"h5",2,0}, {"h6",2,0},
    {"ul",2,0}, {"ol",2,0}, {"li",2,0}, {"dl",2,0}, {"dt",2,0}, {"dd",2,0},
    {"table",5,0}, {"thead",5,0}, {"tbody",5,0}, {"tfoot",5,0}, {"tr",2,0}, {"td",2,0}, {"th",2,0},
    {"caption",7,0},
    {"img",3,F_VOID}, {"br",2,F_VOID}, {"hr",2,F_VOID},
    {"form",4,0}, {"input",5,F_VOID}, {"button",6,0}, {"select",6,0}, {"option",6,0},
    {"label",5,0}, {"textarea",8,F_RCDATA},
    {"header",6,0}, {"footer",6,0}, {"nav",3,0}, {"main",4,0},
    {"section",7,0}, {"article",7,0}, {"aside",5,0},
    {"figure",6,0}, {"figcaption",10,0}, {"address",7,0},
    {"small",5,0}, {"big",3,0}, {"sub",3,0}, {"sup",3,0}, {"mark",4,0},
    {"time",4,0}, {"q",1,0}, {"cite",4,0}, {"abbr",4,0}, {"dfn",3,0},
    {"kbd",3,0}, {"samp",4,0}, {"var",3,0},
    {"font",4,0}, {"center",6,0}, {"strike",6,0}, {"tt",2,0}, {"wbr",3,F_VOID},
    {"noscript",8,0}, {"iframe",6,F_RAW}, {"object",6,0}, {"param",5,F_VOID},
    {"source",6,F_VOID}, {"track",5,F_VOID},
    {"video",5,0}, {"audio",5,0}, {"canvas",6,0},
    {"svg",3,0}, {"math",4,0}, {"mi",2,0}, {"mo",2,0}, {"mn",2,0}, {"ms",2,0},
    {"mtext",5,0}, {"annotation-xml",14,0}, {"foreignobject",13,0}, {"desc",4,0},
    {"mglyph",6,0}, {"malignmark",10,0},
    {"listing",7,0},
    {"plaintext",9,0},
    {"xmp",3,F_RAW},
    {"noembed",7,F_RAW},
    {"noframes",8,F_RAW},
    {"ruby",4,0},
    {"rp",2,0},
    {"rt",2,0},
    {"rtc",3,0},
    {"rb",2,0},
    {"frameset",8,0},
    {"frame",5,F_VOID},
    {"optgroup",8,0},
    {"legend",6,0},
    {"fieldset",8,0},
    {"base",4,F_VOID},
    {"col",3,F_VOID},
    {"colgroup",8,0},
    {"area",4,F_VOID},
    {"map",3,0},
    {"embed",5,F_VOID},
    {"dir",3,0},
    {"menu",4,0},
    {"applet",6,0},
    {"marquee",7,0},
    {"basefont",8,F_VOID},
    {"keygen",6,F_VOID},
    {"template",8,0},
    {"nobr",4,0},
    {"details",7,0},
    {"dialog",6,0},
    {"hgroup",6,0},
    {"search",6,0},
    {"summary",7,0},
    {"bgsound",7,F_VOID}, /* in-head の void。WPT tests19 */
    {"image",5,0},        /* in-body で img へ改名される quirk 入力 */
};

const char *if_tag_name(u16 tag) {
    if (tag == IF_TAG_UNKNOWN || tag >= IF_TAG_N_TAGS) return NULL;
    return IF_TAGS[tag].s;
}

bool if_tag_is_void(u16 tag) {
    return tag < IF_TAG_N_TAGS && tag != IF_TAG_UNKNOWN && (IF_TAGS[tag].flags & F_VOID);
}

bool if_tag_is_rawtext(u16 tag) {
    return tag < IF_TAG_N_TAGS && (IF_TAGS[tag].flags & F_RAW);
}

bool if_tag_is_rcdata(u16 tag) {
    return tag < IF_TAG_N_TAGS && (IF_TAGS[tag].flags & F_RCDATA);
}

/* タグ表整合性: s の長さ == 宣言 n（手書き長さミスを恒久検査する） */
bool if_dom_tag_table_sane(void) {
    for (u16 i = 0; i < IF_TAG_N_TAGS; i++)
        if (strlen(IF_TAGS[i].s) != IF_TAGS[i].n) return false;
    return true;
}

u16 if_tag_id(IfStr name) {
    for (u16 i = 1; i < IF_TAG_N_TAGS; i++) {
        if (name.n == IF_TAGS[i].n && if_str_eq_ci(name, if_str(IF_TAGS[i].s, IF_TAGS[i].n)))
            return i;
    }
    return IF_TAG_UNKNOWN;
}

IfStr if_dom_attr(const IfNode *n, const char *name_ci) {
    if (!n) return if_str(NULL, 0);
    IfStr want = if_str(name_ci, (u32)strlen(name_ci));
    for (u32 i = 0; i < n->n_attrs; i++)
        if (if_str_eq_ci(n->attrs[i].name, want)) return n->attrs[i].value;
    return if_str(NULL, 0);
}

bool if_dom_has_class(const IfNode *n, IfStr cls) {
    IfStr v = if_dom_attr(n, "class");
    u32 i = 0;
    while (i < v.n) {
        while (i < v.n && (v.p[i] == ' ' || v.p[i] == '\t' || v.p[i] == '\n' || v.p[i] == '\r' || v.p[i] == '\f')) i++;
        u32 start = i;
        while (i < v.n && !(v.p[i] == ' ' || v.p[i] == '\t' || v.p[i] == '\n' || v.p[i] == '\r' || v.p[i] == '\f')) i++;
        /* class は case-sensitive（HTML 仕様） */
        if (i - start == cls.n && memcmp(v.p + start, cls.p, cls.n) == 0) return true;
    }
    return false;
}

static u64 text_len_dfs(const IfNode *n) {
    u64 s = 0;
    for (const IfNode *c = n->first_child; c; c = c->next_sibling)
        s += (c->kind == IF_NODE_TEXT) ? c->u.text.n : text_len_dfs(c);
    return s;
}

static void text_write_dfs(const IfNode *n, u8 *buf, u64 *w) {
    for (const IfNode *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == IF_NODE_TEXT) {
            memcpy(buf + *w, c->u.text.p, c->u.text.n);
            *w += c->u.text.n;
        } else {
            text_write_dfs(c, buf, w);
        }
    }
}

/* ---- script 実行専用の最小 DOM 変更面（dom.h 契約） ---- */

static IfNode *dom_find_rec(IfNode *n, u16 tag, IfStr id, bool by_id) {
    for (IfNode *c = n; c; c = c->next_sibling) {
        if (c->kind == IF_NODE_ELEMENT) {
            if (by_id) {
                IfStr v = if_dom_attr(c, "id");
                if (v.p && v.n == id.n && (id.n == 0 || memcmp(v.p, id.p, id.n) == 0)) return c;
            } else if (c->tag == tag) return c;
        }
        IfNode *r = c->first_child ? dom_find_rec(c->first_child, tag, id, by_id) : NULL;
        if (r) return r;
    }
    return NULL;
}
IfNode *if_dom_find_tag_dfs(const IfDom *d, u16 tag) {
    return (d && d->root) ? dom_find_rec(d->root, tag, if_str(NULL, 0), false) : NULL;
}
IfNode *if_dom_find_by_id(const IfDom *d, IfStr id) {
    return (d && d->root) ? dom_find_rec(d->root, 0, id, true) : NULL;
}

static IfNode *dom_new_node(IfArena *a, IfNodeKind kind) {
    IfNode *n = (IfNode *)if_arena_alloc(a, sizeof(IfNode));
    memset(n, 0, sizeof *n);
    n->kind = kind;
    return n;
}

/* text は akl ヒープ等の一時寿命を想定 → dom arena へ必ずコピーして保持する。 */
void if_dom_set_text(IfArena *a, IfNode *n, IfStr t) {
    if (!a || !n || n->kind != IF_NODE_ELEMENT) return;
    n->first_child = NULL;
    n->last_child = NULL;
    if (!t.n || !t.p) return;
    IfNode *tn = dom_new_node(a, IF_NODE_TEXT);
    u8 *cp = (u8 *)if_arena_alloc(a, t.n);
    memcpy(cp, t.p, t.n);
    tn->u.text = if_str((const char *)cp, t.n);
    tn->parent = n;
    n->first_child = tn;
    n->last_child = tn;
}

IfNode *if_dom_title_set(IfArena *a, IfDom *d, IfStr t) {
    if (!a || !d) return NULL;
    IfNode *ttl = if_dom_find_tag_dfs(d, IF_TAG_TITLE);
    if (!ttl) {
        IfNode *head = if_dom_find_tag_dfs(d, IF_TAG_HEAD);
        if (!head) return NULL; /* html/tree 構造上 head 必須（保証はパーサ） */
        ttl = dom_new_node(a, IF_NODE_ELEMENT);
        ttl->tag = IF_TAG_TITLE;
        ttl->ns = IF_NS_HTML;
        ttl->u.tag_name = if_str(if_tag_name(IF_TAG_TITLE), (u32)strlen(if_tag_name(IF_TAG_TITLE))); /* 静的表参照（寿命無限） */
        ttl->parent = head;
        ttl->next_sibling = head->first_child;
        head->first_child = ttl;
        if (!head->last_child) head->last_child = ttl;
    }
    if_dom_set_text(a, ttl, t);
    /* d->title の保持規約はパーサ由来（trim 済み arena ビュー）と揃える */
    IfNode *fresh = if_dom_find_tag_dfs(d, IF_TAG_TITLE);
    d->title = fresh ? if_str_trim(if_dom_text_content(a, fresh)) : if_str(NULL, 0);
    return ttl;
}

/* v0.3 本丸フラグ（dom.h 契約）。GUI ロード中のみ true（chrome.c が設定/復元） */
bool if_dom_copy_strings = false;

/* 深さはツリービルダが IF_MAX_STACK_DEPTH に制限するため再帰は安全（スタック枯渇不能） */
IfStr if_dom_text_content(IfArena *a, const IfNode *n) {
    if (!n) return if_str(NULL, 0);
    u64 count = text_len_dfs(n);
    if (count == 0) return if_str(NULL, 0);
    if (count > 16u * 1024u * 1024u) if_fatal("text_content: absurd size");
    u8 *buf = (u8 *)if_arena_alloc(a, count);
    u64 w = 0;
    text_write_dfs(n, buf, &w);
    return if_str((const char *)buf, (u32)w);
}

static void dump_node(const IfNode *n, FILE *out, int depth) {
    for (int i = 0; i < depth; i++) fputs("  ", out);
    if (n->kind == IF_NODE_TEXT) {
        IfStr t = n->u.text;
        if (t.n > 48) t.n = 48;
        fprintf(out, "#text \"");
        for (u32 i = 0; i < t.n; i++) {
            char c = t.p[i];
            if (c == '\n') fputs("\\n", out);
            else if (c == '"') fputs("\\\"", out);
            else fputc(c, out);
        }
        if (n->u.text.n > 48) fputs("…", out);
        fputs("\"\n", out);
        return;
    }
    fprintf(out, "<%s", n->u.tag_name.p ? n->u.tag_name.p : "?");
    for (u32 i = 0; i < n->n_attrs; i++) {
        fprintf(out, " %.*s=\"%.*s\"",
                (int)n->attrs[i].name.n, n->attrs[i].name.p,
                (int)(n->attrs[i].value.n > 64 ? 64 : n->attrs[i].value.n), n->attrs[i].value.p);
    }
    fputs(">\n", out);
    for (const IfNode *c = n->first_child; c; c = c->next_sibling) dump_node(c, out, depth + 1);
}

void if_dom_dump(const IfDom *dom, void *out_FILE) {
    FILE *out = (FILE *)out_FILE;
    if (!dom || !dom->root) { fputs("(empty dom)\n", out); return; }
    fputs("#document\n", out);
    for (const IfNode *c = dom->root->first_child; c; c = c->next_sibling) dump_node(c, out, 0);
    fprintf(out, "; nodes=%u errors=%u title=\"%.*s\"\n",
            dom->n_nodes, dom->n_errors, dom->title.n ? (int)dom->title.n : 0,
            dom->title.p ? dom->title.p : "");
}

/* ---- template content rare-data 側車（IfNode 88B→80B 分離の受け皿） ----
 * 開放番地法・線形 probe・cap 2 冪・負荷率 0.7 で倍長再ハッシュ。template は
 * 文書あたり 0〜数個が典型で、md fast-DOM では struct 上出現しない（cap=0 のまま
 * = ゼロコスト）。key は arena 若番ポインタ: 8B アライン下位を捨てて乗算ハッシュ。 */

static u32 tpl_hash(const IfNode *p) {
    return (u32)(((uintptr_t)p >> 4) * 2654435761u);
}

IfNode *if_dom_tpl_content(const IfDom *d, const IfNode *tpl) {
    if (!d || !d->tpl_map_cap) return NULL;
    u32 m = d->tpl_map_cap - 1, i = tpl_hash(tpl) & m;
    for (;;) {
        IfNode *k = d->tpl_map[i].tpl;
        if (!k) return NULL;
        if (k == tpl) return d->tpl_map[i].content;
        i = (i + 1) & m;
    }
}

void if_dom_tpl_set_content(IfDom *d, IfNode *tpl, IfNode *content) {
    if ((d->tpl_map_n + 1) * 10 >= d->tpl_map_cap * 7) {
        u32 ncap = d->tpl_map_cap ? d->tpl_map_cap * 2 : 8;
        IfTplMapEnt *nt = (IfTplMapEnt *)if_arena_calloc(d->arena, (u64)ncap * sizeof(IfTplMapEnt));
        for (u32 i = 0; i < d->tpl_map_cap; i++) {
            IfTplMapEnt e = d->tpl_map[i];
            if (!e.tpl) continue;
            u32 m = ncap - 1, j = tpl_hash(e.tpl) & m;
            while (nt[j].tpl) j = (j + 1) & m;
            nt[j] = e;
        }
        d->tpl_map = nt; d->tpl_map_cap = ncap;
    }
    u32 m = d->tpl_map_cap - 1, i = tpl_hash(tpl) & m;
    for (;;) {
        IfNode *k = d->tpl_map[i].tpl;
        if (!k) { d->tpl_map[i].tpl = tpl; d->tpl_map[i].content = content; d->tpl_map_n++; return; }
        if (k == tpl) { d->tpl_map[i].content = content; return; } /* 再 set は上書き */
        i = (i + 1) & m;
    }
}

/* ---- html5lib tree-construction 形式シリアライザ（採点用） ---- */

static void ser_indent(FILE *o, int depth) {
    for (int i = 0; i < depth; i++) fputs("  ", o);
}

static void ser_str(FILE *o, IfStr s) { if (s.n) fwrite(s.p, 1, s.n, o); }

static int attr_name_cmp(const void *a, const void *b) {
    const IfAttr *x = *(const IfAttr *const *)a, *y = *(const IfAttr *const *)b;
    u32 n = x->name.n < y->name.n ? x->name.n : y->name.n;
    int c = memcmp(x->name.p, y->name.p, n);
    if (c) return c;
    return (x->name.n > y->name.n) - (x->name.n < y->name.n);
}

/* template は子を「content」擬似ノード配下として出力する（html5lib 形式）。 */
static void ser_children(const IfDom *dom, const IfNode *n, FILE *o, int depth);

static void ser_node(const IfDom *dom, const IfNode *n, FILE *o, int depth) {
    switch (n->kind) {
    case IF_NODE_TEXT:
        fputc('|', o); fputc(' ', o); ser_indent(o, depth); fputc('"', o);
        ser_str(o, n->u.text); fputs("\"\n", o);
        return;
    case IF_NODE_COMMENT:
        fputc('|', o); fputc(' ', o); ser_indent(o, depth);
        if (n->n_attrs) { /* PI: <?target data?>（target は attrs[0].name。insert_comment 規約） */
            fputs("<?", o); ser_str(o, n->attrs[0].name); fputc(' ', o);
            ser_str(o, n->u.text); fputs("?>\n", o);
        } else {
            fputs("<!-- ", o); ser_str(o, n->u.text); fputs(" -->\n", o);
        }
        return;
    case IF_NODE_DOCTYPE: {
        const IfDoctype *d = n->u.dtype;
        fputc('|', o); fputc(' ', o); ser_indent(o, depth);
        if (!d || !d->has_name) { fputs("<!DOCTYPE >\n", o); return; }
        fputs("<!DOCTYPE ", o); ser_str(o, d->name);
        /* WPT serializer: ids は空でないときのみ両方引用（欠落は空文字） */
        if (d->pub.n == 0 && d->sys.n == 0) { fputs(">\n", o); return; }
        fputs(" \"", o); ser_str(o, d->pub);
        fputs("\" \"", o); ser_str(o, d->sys);
        fputs("\">\n", o);
        return;
    }
    case IF_NODE_ELEMENT: {
        fputc('|', o); fputc(' ', o); ser_indent(o, depth);
        fputc('<', o);
        if (n->ns == IF_NS_SVG) fputs("svg ", o);
        else if (n->ns == IF_NS_MATHML) fputs("math ", o);
        ser_str(o, n->u.tag_name); fputs(">\n", o);
        if (n->n_attrs) {
            /* html5lib 形式は属性を名前の辞書順で出す */
            const IfAttr **sorted = (const IfAttr **)malloc((size_t)n->n_attrs * sizeof *sorted);
            if (!sorted) if_fatal("oom in serializer");
            for (u32 i = 0; i < n->n_attrs; i++) sorted[i] = &n->attrs[i];
            qsort(sorted, n->n_attrs, sizeof *sorted, attr_name_cmp);
            for (u32 i = 0; i < n->n_attrs; i++) {
                fputc('|', o); fputc(' ', o); ser_indent(o, depth + 1);
                /* 名前空間属性は調整時点で "prefix local" 表記に変換済み。
                 * 調整対象外の接頭辞付き属性（xml:baaah 等）はリテラル保持が
                 * vendored dataset の期待値なので、ここで勝手に分割しない。 */
                IfStr an = sorted[i]->name;
                ser_str(o, an); fputs("=\"", o);
                ser_str(o, sorted[i]->value); fputs("\"\n", o);
            }
            free((void *)sorted);
        }
        /* "content" 擬似ノードの出力は template 意味論を持つノード（HTML ns の
         * template）のみ。foreign 内の <svg template>/<math template> は plain
         * 要素なので content を出さない（template.dat#98/#99） */
        if (n->ns == IF_NS_HTML && n->u.tag_name.n == 8 &&
            memcmp(n->u.tag_name.p, "template", 8) == 0) {
            fputc('|', o); fputc(' ', o); ser_indent(o, depth + 1);
            fputs("content\n", o);
            /* 子は content フラグメント（IfDom 側の rare-data 写像）配下（要素子は常空） */
            IfNode *tc = if_dom_tpl_content(dom, n);
            ser_children(dom, tc ? tc : n, o, depth + 2);
        } else {
            ser_children(dom, n, o, depth + 1);
        }
        return;
    }
    case IF_NODE_DOCUMENT:
        ser_children(dom, n, o, depth);
        return;
    }
}

static void ser_children(const IfDom *dom, const IfNode *n, FILE *o, int depth) {
    for (const IfNode *c = n->first_child; c; c = c->next_sibling) ser_node(dom, c, o, depth);
}

void if_dom_serialize_wpt(const IfDom *dom, void *out_FILE) {
    FILE *out = (FILE *)out_FILE;
    if (!dom || !dom->root) return;
    ser_children(dom, dom->root, out, 0);
}

void if_dom_serialize_wpt_frag(const IfDom *dom, void *out_FILE) {
    FILE *out = (FILE *)out_FILE;
    if (!dom || !dom->root) return;
    /* fragment の結果 = 仮想 html root（Document 直下の最初の要素）の子群 */
    for (const IfNode *c = dom->root->first_child; c; c = c->next_sibling)
        if (c->kind == IF_NODE_ELEMENT) { ser_children(dom, c, out, 0); return; }
}
