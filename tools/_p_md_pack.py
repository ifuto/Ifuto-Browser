import io
s = io.open('src/md.c', encoding='utf-8').read()

def rep(old, new, cnt=1):
    global s
    assert old in s, "ANCHOR MISSING: " + old[:70]
    s = s.replace(old, new, cnt)

# ---- Mo: node slab fields ----
rep('''    char *c_buf;          /* 複製バッファ（heap。flush ごとに使い回し） */
    u32 c_n, c_cap;
    u8 mode;              /* 0=empty 1=borrow 2=copy */
} Mo;''','''    char *c_buf;          /* 複製バッファ（heap。flush ごとに使い回し） */
    u32 c_n, c_cap;
    u8 mode;              /* 0=empty 1=borrow 2=copy */
    /* node slab: 生成は直列 bump（ノード単位の arena 呼出を slab refill に畳む） */
    IfNode *nslab, *nslab_end;
} Mo;''')

# ---- mnew: slab + inline ----
rep('''static IfNode *mnew(Mo *m, IfNodeKind kind) {
    u64 _t0; if (mpf()) _t0 = if_rdtsc_md(); else _t0 = 0;
    IfDom *d = m->dom;
    if (d->n_nodes >= IF_MAX_DOM_NODES) { mo_taint(m); return NULL; }
    IfNode *n = (IfNode *)if_arena_alloc(m->a, sizeof(IfNode));
    n->kind = kind;
    n->tag = 0; n->ns = IF_NS_HTML; n->flags = 0;
    n->attrs = NULL; n->n_attrs = 0;
    n->style = NULL;
    n->parent = n->first_child = n->last_child = n->next_sibling = NULL;
    n->content = NULL;
    d->n_nodes++;
    if (_t0) MP_MNEW += if_rdtsc_md() - _t0;
    return n;
}''','''static inline IfNode *mnew(Mo *m, IfNodeKind kind) {
    u64 _t0; if (mpf()) _t0 = if_rdtsc_md(); else _t0 = 0;
    IfDom *d = m->dom;
    IfNode *n;
    if (__builtin_expect(m->nslab != m->nslab_end, 1)) {
        n = m->nslab++;
    } else {
        if (__builtin_expect(d->n_nodes >= IF_MAX_DOM_NODES, 0)) { mo_taint(m); return NULL; }
        n = (IfNode *)if_arena_bump(m->a, 128 * sizeof(IfNode));
        m->nslab = n + 1;
        m->nslab_end = n + 128;
    }
    if (__builtin_expect(d->n_nodes >= IF_MAX_DOM_NODES, 0)) { mo_taint(m); return NULL; }
    d->n_nodes++;
    n->kind = kind;
    n->tag = 0; n->ns = IF_NS_HTML; n->flags = 0;
    n->attrs = NULL; n->n_attrs = 0;
    n->style = NULL;
    n->parent = n->first_child = n->last_child = n->next_sibling = NULL;
    n->content = NULL;
    if (_t0) MP_MNEW += if_rdtsc_md() - _t0;
    return n;
}''')

# ---- mo_close: tag_name ポインタ一致の fast path（各サイトは同一 static 名を対で使う） ----
rep('''    run_flush(m);
    if (m->sp <= 0 || m->stk[m->sp - 1]->u.tag_name.n != nl ||
        memcmp(m->stk[m->sp - 1]->u.tag_name.p, name, nl) != 0) {
        mo_taint(m); /* 到達不能のはず（emitter は常に対応させる） */
        return;
    }''','''    if (m->mode) run_flush(m); /* flush は mode≠0 のときだけ（呼出自体を畳む） */
    IfNode *top = m->sp > 0 ? m->stk[m->sp - 1] : NULL;
    if (!top || top->u.tag_name.n != nl ||
        (top->u.tag_name.p != name && memcmp(top->u.tag_name.p, name, nl) != 0)) {
        mo_taint(m); /* 到達不能のはず（emitter は常に対応させる） */
        return;
    }''')

# ---- mo_elem_store: flush ゲート ----
rep('''static void mo_elem_store(Mo *m, bool push) {
    run_flush(m);''','''static void mo_elem_store(Mo *m, bool push) {
    if (m->mode) run_flush(m); /* flush は mode≠0 のときだけ */
    if (__builtin_expect(m->tainted, 0)) return;''')

io.open('src/md.c','w',encoding='utf-8').write(s)
print("patched ok")
