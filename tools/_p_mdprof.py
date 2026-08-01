import io
s = io.open('src/md.c', encoding='utf-8').read()

def rep(old, new, cnt=1):
    global s
    assert old in s, "ANCHOR MISSING: " + old[:60]
    s = s.replace(old, new, cnt)

rep('''static void mo_taint(Mo *m) { m->tainted = true; }''',
'''static void mo_taint(Mo *m) { m->tainted = true; }

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
        fprintf(stderr, "MDPROF norm=%llu lines=%llu blocks=%llu inline=%llu mnew=%llu flush=%llu (cycles)\\n",
                (unsigned long long)MP_NORM, (unsigned long long)MP_LINES, (unsigned long long)MP_BLOCKS,
                (unsigned long long)MP_INLINE, (unsigned long long)MP_MNEW, (unsigned long long)MP_FLUSH);
}
static inline bool mpf(void) {
    if (if_mp_on == -2) { const char *e = getenv("IF_MD_PROF"); if_mp_on = (e && e[0] == '1') ? 1 : 0; }
    return if_mp_on > 0;
}''')

rep('''static IfNode *mnew(Mo *m, IfNodeKind kind) {
    IfDom *d = m->dom;''','''static IfNode *mnew(Mo *m, IfNodeKind kind) {
    u64 _t0; if (mpf()) _t0 = if_rdtsc_md(); else _t0 = 0;
    IfDom *d = m->dom;''')
rep('''    d->n_nodes++;
    return n;
}''','''    d->n_nodes++;
    if (_t0) MP_MNEW += if_rdtsc_md() - _t0;
    return n;
}''')

rep('''static void run_flush(Mo *m) {
    if (m->mode != 0) {''','''static void run_flush(Mo *m) {
    u64 _t0; if (mpf()) _t0 = if_rdtsc_md(); else _t0 = 0;
    if (m->mode != 0) {''')
rep('''        run_reset(m);
    }
}''','''        run_reset(m);
    }
    if (_t0) MP_FLUSH += if_rdtsc_md() - _t0;
}''')

rep('''static void inline_span(Mo *out, Fn *fn, IfStr s) {
    u32 i = 0;
    while (i < s.n) {''','''static int inl_depth = 0;
static void inline_span(Mo *out, Fn *fn, IfStr s) {
    u64 _t0 = 0; bool _top = false;
    if (mpf() && inl_depth++ == 0) { _t0 = if_rdtsc_md(); _top = true; }
    else if (!mpf()) inl_depth = inl_depth;
    u32 i = 0;
    while (i < s.n) {''')

# inline_span end: function ends right before "/* ---- ブロック判定 ----"
idx = s.index('/* ================= ブロック層 ================= */')
seg = s[:idx].rstrip()
assert seg.endswith('}')
lastclose = seg.rfind('\n}')  # closing brace of inline_span (col 0)
assert lastclose > 0
s = s[:lastclose] + '''
    if (_top) MP_INLINE += if_rdtsc_md() - _t0;
    if (mpf()) inl_depth--;
}
''' + s[lastclose+2:]

rep('''static void blocks_str(Mo *out, Fn *fn, IfStr s, u32 depth) {''','''static void blocks_str(Mo *out, Fn *fn, IfStr s, u32 depth) {
    u64 _t0 = 0; if (mpf()) _t0 = if_rdtsc_md();''')
rep('''    blocks_win(out, fn, ls, 0, n, depth);
    free(ls);
}''','''    if (_t0) MP_LINES += if_rdtsc_md() - _t0;
    blocks_win(out, fn, ls, 0, n, depth);
    free(ls);
}''')

rep('''static void blocks_win(Mo *out, Fn *fn, Ln *ls, u32 lo, u32 hi, u32 depth) {
    u32 i = lo;''','''static int bw_depth = 0;
static void blocks_win(Mo *out, Fn *fn, Ln *ls, u32 lo, u32 hi, u32 depth) {
    u64 _t0 = 0; bool _top = false;
    if (mpf() && bw_depth++ == 0) { _t0 = if_rdtsc_md(); _top = true; }
    u32 i = lo;''')
rep('''        emit_para_lines(out, fn, ls, i, j);
        i = j;
    }
}''','''        emit_para_lines(out, fn, ls, i, j);
        i = j;
    }
    if (_top) MP_BLOCKS += if_rdtsc_md() - _t0;
    if (mpf()) bw_depth--;
}''')

rep('''static void run_blocks(Mo *out, Fn *fn, IfStr in) {
    /* 正規化''','''static void run_blocks(Mo *out, Fn *fn, IfStr in) {
    u64 _t0 = 0; if (mpf()) _t0 = if_rdtsc_md();
    /* 正規化''')
rep("    mo_range(out, s.p, s.n);\n    blocks_str(out, fn, s, 0);\nfootnotes:;",
"    if (_t0) { MP_NORM += if_rdtsc_md() - _t0; }\n    mo_range(out, s.p, s.n);\n    blocks_str(out, fn, s, 0);\nfootnotes:;")

io.open('src/md.c','w',encoding='utf-8').write(s)
print("patched ok")
