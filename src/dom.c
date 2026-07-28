/* Ifuto — DOM 補助（タグ表・属性アクセス・テキスト連結・ダンプ） */
#include "dom.h"
#include <stdio.h>

/* dom.h の IfTag 列挙と 1:1 対応。順序ズレは test_html の round-trip テストが検出する。 */
#define F_VOID 1u
#define F_RAW  2u
static const struct { const char *s; u8 n; u8 flags; } IF_TAGS[IF_TAG_N_TAGS] = {
    {"", 0, 0},                          /* UNKNOWN */
    {"html",4,0}, {"head",4,0}, {"body",4,0}, {"title",5,F_RAW}, {"meta",4,F_VOID},
    {"link",4,F_VOID}, {"style",5,F_RAW}, {"script",6,F_RAW},
    {"div",3,0}, {"span",4,0}, {"p",1,0}, {"a",1,0},
    {"b",1,0}, {"i",1,0}, {"u",1,0}, {"s",1,0},
    {"em",2,0}, {"strong",6,0}, {"code",4,0}, {"pre",3,0}, {"blockquote",10,0},
    {"h1",2,0}, {"h2",2,0}, {"h3",2,0}, {"h4",2,0}, {"h5",2,0}, {"h6",2,0},
    {"ul",2,0}, {"ol",2,0}, {"li",2,0}, {"dl",2,0}, {"dt",2,0}, {"dd",2,0},
    {"table",5,0}, {"thead",5,0}, {"tbody",5,0}, {"tr",2,0}, {"td",2,0}, {"th",2,0},
    {"caption",7,0},
    {"img",3,F_VOID}, {"br",2,F_VOID}, {"hr",2,F_VOID},
    {"form",4,0}, {"input",5,F_VOID}, {"button",6,0}, {"select",6,0}, {"option",6,0},
    {"label",5,0}, {"textarea",8,F_RAW},
    {"header",6,0}, {"footer",6,0}, {"nav",3,0}, {"main",4,0},
    {"section",7,0}, {"article",7,0}, {"aside",5,0},
    {"figure",6,0}, {"figcaption",10,0}, {"address",7,0},
    {"small",5,0}, {"big",3,0}, {"sub",3,0}, {"sup",3,0}, {"mark",4,0},
    {"time",4,0}, {"q",1,0}, {"cite",4,0}, {"abbr",4,0}, {"dfn",3,0},
    {"kbd",3,0}, {"samp",4,0}, {"var",3,0},
    {"font",4,0}, {"center",6,0}, {"strike",6,0}, {"tt",2,0}, {"wbr",3,F_VOID},
    {"noscript",8,0}, {"iframe",6,0}, {"object",6,0}, {"param",5,F_VOID},
    {"source",6,F_VOID}, {"track",5,F_VOID},
    {"video",5,0}, {"audio",5,0}, {"canvas",6,0},
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
        s += (c->kind == IF_NODE_TEXT) ? c->text.n : text_len_dfs(c);
    return s;
}

static void text_write_dfs(const IfNode *n, u8 *buf, u64 *w) {
    for (const IfNode *c = n->first_child; c; c = c->next_sibling) {
        if (c->kind == IF_NODE_TEXT) {
            memcpy(buf + *w, c->text.p, c->text.n);
            *w += c->text.n;
        } else {
            text_write_dfs(c, buf, w);
        }
    }
}

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
        IfStr t = n->text;
        if (t.n > 48) t.n = 48;
        fprintf(out, "#text \"");
        for (u32 i = 0; i < t.n; i++) {
            char c = t.p[i];
            if (c == '\n') fputs("\\n", out);
            else if (c == '"') fputs("\\\"", out);
            else fputc(c, out);
        }
        if (n->text.n > 48) fputs("…", out);
        fputs("\"\n", out);
        return;
    }
    fprintf(out, "<%s", n->tag_name.p ? n->tag_name.p : "?");
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
