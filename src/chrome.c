/* Ifuto — TUI クローム モデル（実装） */
#include "chrome.h"
#include "css.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- fs 実装（本番） ---- */
bool if_fs_exists_real(const char *path, void *ctx) {
    (void)ctx;
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

IfStr if_fs_read_real(IfArena *a, const char *path, void *ctx) {
    (void)ctx;
    FILE *f = fopen(path, "rb");
    if (!f) return if_str("", 0);
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return if_str("", 0); }
    long n = ftell(f);
    if (n < 0 || (u64)n > IF_MAX_INPUT_BYTES) { fclose(f); return if_str("", 0); }
    rewind(f);
    char *buf = (char *)if_arena_alloc(a, (u64)n ? (u64)n : 1);
    u64 r = fread(buf, 1, (u64)n, f);
    fclose(f);
    if (r != (u64)n) return if_str("", 0);
    return if_str(buf, (u32)n);
}

/* ---- 内部 ---- */

static void set_toast(IfChrome *c, const char *msg) {
    u32 n = (u32)strlen(msg);
    if (n >= sizeof c->toast) n = sizeof c->toast - 1;
    memcpy(c->toast, msg, n);
    c->toast[n] = 0;
    c->toast_len = (u8)n;
}

static char *dup_cap(const char *s, u32 cap) {
    u32 n = (u32)strlen(s);
    if (n >= cap) n = cap - 1;
    char *p = (char *)malloc((size_t)n + 1);
    if (!p) if_fatal("oom: tab metadata");
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static void tab_free(IfTab *t) {
    if (!t) return;
    if (t->doc) { if_arena_destroy(t->doc); free(t->doc); }
    if (t->view) { if_arena_destroy(t->view); free(t->view); }
    free(t->url);
    free(t->title);
    free(t);
}

static void tab_build_view(IfTab *t, i32 width) {
    if (t->view) { if_arena_destroy(t->view); free(t->view); }
    t->view = (IfArena *)malloc(sizeof(IfArena));
    if (!t->view) if_fatal("oom: view arena");
    if_arena_init(t->view, 1 << 18);
    t->lay = if_layout_build(t->view, t->dom, width);
    t->grid = if_render_grid(t->view, t->lay);
    t->dirty = false;
}

/* url/title コピー後に文書を構築する共通ローダ */
static bool tab_load(IfChrome *c, IfTab *t, const char *path, i32 width) {
    IfArena *doc = (IfArena *)malloc(sizeof(IfArena));
    if (!doc) if_fatal("oom: doc arena");
    if_arena_init(doc, 1 << 18);
    IfStr input = c->fs.read_file(doc, path, c->fs.ctx);
    /* read_file 失敗の判定: 空ファイルは合法、失敗は ctx 別の手段が必要 →
     * ファイルサイズ 0 との区別は stat のみ存在で担保する */
    if (!input.p || (input.n == 0 && !c->fs.exists(path, c->fs.ctx))) {
        if_arena_destroy(doc);
        free(doc);
        return false;
    }
    t->dom = if_parse_html(doc, input);
    if_style_apply(doc, t->dom);
    if (t->doc) if_arena_destroy(t->doc);
    free(t->doc);
    t->doc = doc;
    tab_build_view(t, width);
    /* タイトル: <title> があればそれ（chrome メタにコピー——文書 arena を跨がない） */
    free(t->title);
    if (t->dom->title.n) {
        u32 n = t->dom->title.n > (u32)IF_TITLE_CAP - 1 ? (u32)IF_TITLE_CAP - 1 : t->dom->title.n;
        char *tp = (char *)malloc((size_t)n + 1);
        if (!tp) if_fatal("oom: title");
        memcpy(tp, t->dom->title.p, n);
        tp[n] = 0;
        t->title = tp;
    } else {
        /* フォールバック: ファイル名末尾 */
        const char *base = strrchr(path, '/');
        t->title = dup_cap(base ? base + 1 : path, IF_TITLE_CAP);
    }
    return true;
}

/* ---- モデル API ---- */

void if_chrome_init(IfChrome *c, IfFsOps fs) {
    memset(c, 0, sizeof *c);
    c->fs = fs;
    if_arena_init(&c->engine_scratch, 1 << 16);
    c->active = -1;
    c->next_id = 1;
    c->mode = CM_NORMAL;
    c->quit_armed_at = -1;
}

void if_chrome_destroy(IfChrome *c) {
    for (i32 i = 0; i < c->n_tabs; i++) tab_free(c->tabs[i]);
    c->n_tabs = 0;
    if_arena_destroy(&c->engine_scratch);
}

IfTab *if_chrome_cur(IfChrome *c) {
    if (c->active < 0 || c->active >= c->n_tabs) return NULL;
    return c->tabs[c->active];
}

IfTab *if_chrome_new_blank(IfChrome *c) {
    if (c->n_tabs >= IF_TABS_MAX) { set_toast(c, "tab limit (64)"); return NULL; }
    IfTab *t = (IfTab *)calloc(1, sizeof(IfTab));
    if (!t) if_fatal("oom: tab");
    t->id = c->next_id++;
    t->url = dup_cap("", IF_URL_CAP);
    t->title = dup_cap("New Tab", IF_TITLE_CAP);
    t->link_idx = -1;
    c->tabs[c->n_tabs++] = t;
    c->active = c->n_tabs - 1;
    c->mode = CM_OMNIBOX; /* INV-1: 空白新タブ → アドレス入力へ直行 */
    c->omni_len = 0;
    c->omni[0] = 0;
    return t;
}

bool if_chrome_open(IfChrome *c, const char *path, i32 width) {
    IfTab *t = if_chrome_cur(c);
    bool reuse_blank = t && !t->doc && t->url[0] == 0; /* 空白タブなら再利用（二重 blank 回避） */
    if (!reuse_blank) t = if_chrome_new_blank(c);
    if (!t) return false;
    if (!tab_load(c, t, path, width)) {
        set_toast(c, "cannot open file");
        if (!reuse_blank) if_chrome_close(c);
        return false;
    }
    free(t->url);
    t->url = dup_cap(path, IF_URL_CAP);
    c->mode = CM_NORMAL;
    t->scroll = 0;
    t->link_idx = -1;
    (void)width;
    return true;
}

bool if_chrome_close(IfChrome *c) {
    IfTab *t = if_chrome_cur(c);
    if (!t) return false;
    tab_free(t);
    for (i32 i = c->active; i < c->n_tabs - 1; i++) c->tabs[i] = c->tabs[i + 1];
    c->n_tabs--;
    if (c->n_tabs == 0) {
        c->active = -1;
        if_chrome_new_blank(c);
        if (c->tabs[0]) c->mode = CM_NORMAL; /* 最後の1枚を閉じたら空白タブが残る（quit は q） */
        return true;
    }
    if (c->active >= c->n_tabs) c->active = c->n_tabs - 1;
    return true;
}

void if_chrome_switch(IfChrome *c, i32 idx) {
    if (idx < 0 || idx >= c->n_tabs) return;
    c->active = idx;
    c->quit_armed_at = -1;
    c->toast_len = 0;
    c->toast[0] = 0;
}

bool if_chrome_reload(IfChrome *c, i32 width) {
    IfTab *t = if_chrome_cur(c);
    if (!t || !t->url[0]) { set_toast(c, "nothing to reload"); return false; }
    i32 keep_scroll = t->scroll;
    if (!tab_load(c, t, t->url, width)) { set_toast(c, "reload failed"); return false; }
    t->scroll = keep_scroll;
    return true;
}

void if_chrome_relayout(IfChrome *c, i32 width) {
    for (i32 i = 0; i < c->n_tabs; i++) c->tabs[i]->dirty = true;
    IfTab *t = if_chrome_cur(c);
    if (t && t->doc) {
        tab_build_view(t, width);
        i32 maxs = t->grid && t->grid->h > 0 ? t->grid->h : 0;
        if (t->scroll > maxs) t->scroll = maxs;
    }
}

i32 if_chrome_scroll(IfChrome *c, i32 delta, i32 vh) {
    IfTab *t = if_chrome_cur(c);
    if (!t || !t->grid) return 0;
    i32 maxs = t->grid->h > vh ? t->grid->h - vh : 0;
    t->scroll += delta;
    if (t->scroll < 0) t->scroll = 0;
    if (t->scroll > maxs) t->scroll = maxs;
    return t->scroll;
}

void if_chrome_scroll_to(IfChrome *c, i32 pos, i32 vh) {
    IfTab *t = if_chrome_cur(c);
    if (!t || !t->grid) return;
    i32 maxs = t->grid->h > vh ? t->grid->h - vh : 0;
    t->scroll = pos < 0 ? 0 : (pos > maxs ? maxs : pos);
}

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

i32 if_chrome_resolve(IfChrome *c, const char *input, const char *cwd,
                      char *out, u32 cap) {
    (void)c;
    input = skip_ws(input);
    if (!*input) return 2;
    /* ネットワークは v0.3: ここで明示的に拒否（黙って解釈しない = INV-3 の実装側） */
    const char *p = strstr(input, "://");
    if (p && p - input >= 2 && p - input <= 8) return 1;
    /* 絶対 / cwd 相対 / 現タブ相対の順（現タブ相対は tui 側で cwd 合成済みを渡す） */
    char cand[4096];
    if (input[0] == '/') {
        u32 n = (u32)strlen(input);
        if (n >= cap) return 2;
        memcpy(out, input, n + 1);
        return c->fs.exists(out, c->fs.ctx) ? 0 : 2;
    }
    snprintf(cand, sizeof cand, "%s/%s", cwd, input);
    if (c->fs.exists(cand, c->fs.ctx)) {
        u32 n = (u32)strlen(cand);
        if (n >= cap) return 2;
        memcpy(out, cand, n + 1);
        return 0;
    }
    return 2;
}

bool if_chrome_quit(IfChrome *c, i64 now) {
    if (c->n_tabs <= 1) return true;
    if (c->quit_armed_at >= 0 && now - c->quit_armed_at <= 3) return true;
    c->quit_armed_at = now;
    set_toast(c, "press q again to quit");
    return false;
}

i32 if_chrome_link_move(IfChrome *c, i32 delta) {
    IfTab *t = if_chrome_cur(c);
    if (!t || !t->lay || t->lay->n_links == 0) { t && (t->link_idx = -1); return -1; }
    i32 n = (i32)t->lay->n_links;
    if (t->link_idx < 0) t->link_idx = (delta >= 0) ? 0 : n - 1;
    else t->link_idx = (t->link_idx + delta) % n;
    if (t->link_idx < 0) t->link_idx += n;
    return t->link_idx;
}

u64 if_chrome_cur_doc_bytes(IfChrome *c) {
    IfTab *t = if_chrome_cur(c);
    if (!t) return 0;
    u64 n = 0;
    if (t->doc) n += if_arena_reserved(t->doc);
    if (t->view) n += if_arena_reserved(t->view);
    return n;
}
