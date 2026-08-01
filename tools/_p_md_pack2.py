import io, re
s = io.open('src/md.c', encoding='utf-8').read()

def rep(old, new, cnt=1):
    global s
    assert old in s, "ANCHOR MISSING: " + old[:70]
    s = s.replace(old, new, cnt)

# ---- fused open+push (no-attr 要素) ----
rep('''static void mo_open_end(Mo *m) { /* 開始タグ閉じ＋要素をスタックへ */''','''/* 無属性要素の open+push 融合（pend 経由の多段コールを 1 つに畳む。
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

static void mo_open_end(Mo *m) { /* 開始タグ閉じ＋要素をスタックへ */''')

# ---- サイト書き換え（no-attr ペアのみ） ----
pairs = [
('''            mo_open(out, IF_TAG_SUP, "sup", 3);
            mo_open_end(out);''','''            mo_open_push(out, IF_TAG_SUP, "sup", 3);'''),
('''    mo_open(out, IF_TAG_P, "p", 1);
    mo_open_end(out);''','''    mo_open_push(out, IF_TAG_P, "p", 1);'''),
('''            mo_open(out, (u16)(IF_TAG_H1 + (hh - 1)), nm, 2);
            mo_open_end(out);''','''            mo_open_push(out, (u16)(IF_TAG_H1 + (hh - 1)), nm, 2);'''),
('''            mo_open(out, IF_TAG_HR, "hr", 2);
            mo_open_end_void(out);''','''            mo_open_void(out, IF_TAG_HR, "hr", 2);'''),
('''            mo_open(out, IF_TAG_PRE, "pre", 3);
            mo_open_end(out);''','''            mo_open_push(out, IF_TAG_PRE, "pre", 3);'''),
('''                mo_open(out, IF_TAG_BLOCKQUOTE, "blockquote", 10);
                mo_open_end(out);''','''                mo_open_push(out, IF_TAG_BLOCKQUOTE, "blockquote", 10);'''),
('''                mo_open(out, IF_TAG_P, "p", 1);
                mo_open_end(out);''','''                mo_open_push(out, IF_TAG_P, "p", 1);'''),
('''            mo_open(out, ordered ? IF_TAG_OL : IF_TAG_UL, ordered ? "ol" : "ul", 2);
            mo_open_end(out);''','''            mo_open_push(out, ordered ? IF_TAG_OL : IF_TAG_UL, ordered ? "ol" : "ul", 2);'''),
('''                mo_open(out, IF_TAG_LI, "li", 2);
                mo_open_end(out);''','''                mo_open_push(out, IF_TAG_LI, "li", 2);'''),
('''            mo_open(out, IF_TAG_TABLE, "table", 5);
            mo_open_end(out);''','''            mo_open_push(out, IF_TAG_TABLE, "table", 5);'''),
('''            mo_open(out, IF_TAG_THEAD, "thead", 5);
            mo_open_end(out);''','''            mo_open_push(out, IF_TAG_THEAD, "thead", 5);'''),
('''            mo_open(out, IF_TAG_TR, "tr", 2);
            mo_open_end(out);''','''            mo_open_push(out, IF_TAG_TR, "tr", 2);'''),
('''                mo_open(out, IF_TAG_TH, "th", 2);
                mo_open_end(out);''','''                mo_open_push(out, IF_TAG_TH, "th", 2);'''),
('''            mo_open(out, IF_TAG_TBODY, "tbody", 5);
            mo_open_end(out);''','''            mo_open_push(out, IF_TAG_TBODY, "tbody", 5);'''),
('''                mo_open(out, IF_TAG_TR, "tr", 2);
                mo_open_end(out);''','''                mo_open_push(out, IF_TAG_TR, "tr", 2);'''),
('''                    mo_open(out, IF_TAG_TD, "td", 2);
                    mo_open_end(out);''','''                    mo_open_push(out, IF_TAG_TD, "td", 2);'''),
('''        mo_open(out, IF_TAG_HR, "hr", 2);
        mo_open_end_void(out);''','''        mo_open_void(out, IF_TAG_HR, "hr", 2);'''),
('''        mo_open(out, IF_TAG_OL, "ol", 2);
        mo_open_end(out);''','''        mo_open_push(out, IF_TAG_OL, "ol", 2);'''),
('''                mo_open(out, IF_TAG_BR, "br", 2);
                mo_open_end_void(out);''','''                mo_open_void(out, IF_TAG_BR, "br", 2);'''),
]
n_applied = 0
for old, new in pairs:
    if old in s:
        s = s.replace(old, new)
        n_applied += 1
    else:
        print("SKIP (not found):", old.replace("\n"," ")[:60])

io.open('src/md.c','w',encoding='utf-8').write(s)
print("applied:", n_applied, "of", len(pairs))
