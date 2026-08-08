/* Ifuto — TUI クローム モデル（実装。不変条件は chrome.h 参照）
 *
 * slice-2 での変更は「加算のみ」: slice-1 のセマンティクス（scroll クランプ/resolve/
 * reload 失敗時タブ保持/switch で toast 消去）は一切変えない（git diff で照合済）。 */
#include "chrome.h"
#include "css.h"
#include "md.h"
#include "net.h"
#include "script.h" /* v0.3: <script> akl 実行（style 適用前。正本 docs/SCRIPTING.md） */
#include "ifuto_pages.h"
#include "ext.h" /* 拡張 E1（chrome_init 走査結線） */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

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

/* C2 書き込み側: tmp ファイルへ全量 write+fsync → rename → 親 dir fsync。
 * クラッシュしても旧版か新版のどちらか完全な方だけが残る（#19 の根拠） */
bool if_fs_write_real(const char *path, const void *buf, size_t n, void *ctx) {
    (void)ctx;
    char tmp[4400];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return false;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    const u8 *p = (const u8 *)buf;
    size_t left = n;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp);
            return false;
        }
        p += w;
        left -= (size_t)w;
    }
    if (fsync(fd) != 0) { close(fd); unlink(tmp); return false; }
    if (close(fd) != 0) { unlink(tmp); return false; }
    if (rename(tmp, path) != 0) { unlink(tmp); return false; }
    /* rename 自体の永続化（dir エントリの fsync） */
    char dircopy[4400];
    snprintf(dircopy, sizeof dircopy, "%s", path);
    char *slash = strrchr(dircopy, '/');
    if (slash) {
        *slash = 0;
        int dfd = open(dircopy, O_RDONLY); /* O_DIRECTORY は _GNU_SOURCE 依存なので使わない */
        if (dfd >= 0) {
            if (fsync(dfd) != 0) { /* dir fsync の失敗は非致命 */ }
            close(dfd);
        }
    }
    return true;
}

/* 追記専用（history）。O_APPEND + 書込長一致 + fsync */
bool if_fs_append_real(const char *path, const void *buf, size_t n, void *ctx) {
    (void)ctx;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return false;
    const u8 *p = (const u8 *)buf;
    size_t left = n;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        p += w;
        left -= (size_t)w;
    }
    if (fsync(fd) != 0) { close(fd); return false; }
    return close(fd) == 0;
}

bool if_fs_mkpath_real(const char *dir, void *ctx) {
    (void)ctx;
    char d[4400];
    size_t n = strlen(dir);
    if (n == 0 || n >= sizeof d) return false;
    memcpy(d, dir, n + 1);
    for (char *p = d + 1; p < d + n; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(d, 0700) != 0 && errno != EEXIST) { *p = '/'; return false; }
            *p = '/';
        }
    }
    return mkdir(d, 0700) == 0 || errno == EEXIST;
}

/* ---- 内部 ---- */

static void set_toast(IfChrome *c, const char *msg) {
    u32 n = (u32)strlen(msg);
    if (n >= sizeof c->toast) n = sizeof c->toast - 1;
    memcpy(c->toast, msg, n);
    c->toast_len = (u8)n;
    c->toast[n] = 0;
}

void if_chrome_toast(IfChrome *c, const char *msg) { set_toast(c, msg); } /* 拡張 E1 用の公開窓口（chrome.h 参照） */

static char *dup_cap(const char *s, u32 cap) {
    u32 n = (u32)strlen(s);
    if (n >= cap) n = cap - 1;
    char *p = (char *)malloc((size_t)n + 1);
    if (!p) if_fatal("oom: tab metadata");
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* 構造変化の後にだけ呼ぶ自動セーブ（scroll 移動では呼ばない=書込増幅を禁止。
 * C2 原子的なのでクラッシュしても壊れない） */
static void autosave(IfChrome *c) {
    if (c->store.enabled) if_store_session_save(&c->store, c);
}

static void tab_free(IfTab *t) {
    if (!t) return;
    if (t->doc) { if_arena_destroy(t->doc); free(t->doc); }
    if (t->view) { if_arena_destroy(t->view); free(t->view); }
    free(t->group);
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
    t->doc_h = t->lay ? t->lay->height : 0; /* grid は paint 時に viewport 分だけ構築 */
    t->dirty = false;
}

/* url/title コピー後に文書を構築する共通ローダ。失敗時は t を一切触らない。 */
static bool tab_load(IfChrome *c, IfTab *t, const char *path, i32 width) {
    IfArena *doc = (IfArena *)malloc(sizeof(IfArena));
    if (!doc) if_fatal("oom: doc arena");
    if_arena_init(doc, 1 << 18);
    IfStr input;
    bool is_http = strncmp(path, "http://", 7) == 0;
    /* ifuto:// 内部ページ（普通のブラウザの settings/history 相当）はローカル情報を
     * HTML として生成して通常 DOM 経路に乗せる（多層防御 = 共通パーサに統一） */
    if (!if_ifuto_page(doc, path, c, &input)) {
        if (is_http) {
            /* v0.3: http:// を取得。404 等でも応答ボディを描画する
             * （普通のブラウザが 404 ページを表示するのと同じ）。
             * ネットワーク失敗のみロード失敗（input.p = NULL） */
            const char *err = NULL;
            u32 status = 0;
            if (!if_http_get(doc, path, &input, &status, &err))
                input = if_str(NULL, 0);
        } else {
            input = c->fs.read_file(doc, path, c->fs.ctx);
        }
    }
    /* read_file 失敗の判定: 空ファイルは合法、失敗は ctx 別の手段が必要 →
     * ファイルサイズ 0 との区別は stat のみ存在で担保する（内部ページは stat 評価を飛ばす） */
    else if (!input.p) {
        if_arena_destroy(doc);
        free(doc);
        return false;
    }
    if (!input.p) { /* fetch/read の NULL 失敗をここで一元判定 */
        if_arena_destroy(doc);
        free(doc);
        return false;
    }
    if (input.n == 0 && !is_http && strncmp(path, "ifuto://", 8) != 0 && !c->fs.exists(path, c->fs.ctx)) {
        if_arena_destroy(doc);
        free(doc);
        return false;
    }
    /* v0.2: .md は HTML に前段変換（CLI と同一ゲート。多層防御は共通パーサ側）。
     * v0.3: DOM 直構築の高速経路を先に試す（CLI main.c と同一構造。汚染時は
     * 2 段経路が同じ結論へ至る = 多層防御不変。GUI は flags=0（full attrs 保持）:
     * リンク収集・将来の #id アンカー参照が属性を読む。CLI の SLIM_ATTRS は
     * 線形レンダ専用の剃りで GUI には適用しない） */
    if (if_path_is_md(path)) {
        if (getenv("IFUTO_MD_SLOW") ||
            !if_md_parse_fast_f(doc, input, &t->dom, 0)) {
            IfStr md_html;
            if_md_to_html(doc, input, &md_html);
            input = md_html;
            if_dom_slim = true;
            t->dom = if_parse_html(doc, input);
        }
    } else {
        if_dom_slim = true; /* 実ブラウズ法則: 画面描画に関係ないものは DOM しない */
        t->dom = if_parse_html(doc, input);
    }
    /* v0.3: <script> akl 実行（本家順序: style 適用前。DOM 変更が style/layout に
     * 反映される。script RT は if_script_run 内で必ず破棄 → doc arena より先に
     * 死ぬ = HANDLE ptr 規約の構造保証。失敗は script 単位で隔離・描画継続） */
    if_script_run(doc, t->dom, stderr);
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

/* silent 版 new_blank（restore の構築で使う: mode 遷移/autosave を発火しない） */
static IfTab *tab_new_silent(IfChrome *c) {
    if (c->n_tabs >= IF_TABS_MAX) return NULL;
    IfTab *t = (IfTab *)calloc(1, sizeof(IfTab));
    if (!t) if_fatal("oom: tab");
    t->id = c->next_id++;
    t->url = dup_cap("", IF_URL_CAP);
    t->title = dup_cap("New Tab", IF_TITLE_CAP);
    t->link_idx = -1;
    c->tabs[c->n_tabs++] = t;
    return t;
}

/* doc=NULL で url を持つタブを表示するときの on-demand ロード。
 * 失敗してもタブは残す=消えないことが正しい（復元タブの墓場問題） */
static void lazy_load(IfChrome *c, IfTab *t, i32 width) {
    if (!t || t->doc || t->url[0] == 0) return;
    if (!tab_load(c, t, t->url, width))
        set_toast(c, "cannot open (kept in tab list)");
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
    c->now = 0;
    if_store_init(&c->store, &c->fs, /*create=*/true); /* 失敗は enabled=false で no-op */
    /* 拡張 E1（docs/EXTENSIONS.md）: --ext 指定があれば優先（開けなければエラー行）、
     * 無ければ既定 <store>/ext（不在は黙殺）。IFUTO_NO_EXT=1 は全面停止の救済
     * スイッチ。失敗は拡張単位で打切られ本体初期化は中断しない */
    {
        const char *noext = getenv("IFUTO_NO_EXT");
        if (!(noext && noext[0] == '1')) {
            const char *xd = if_ext_dir();
            if (xd) if_ext_scan_and_run(c, xd, stderr, true);
            else if (c->store.enabled) {
                char dbuf[IF_STORE_DIR_CAP + 8];
                snprintf(dbuf, sizeof dbuf, "%s/ext", c->store.dir);
                if_ext_scan_and_run(c, dbuf, stderr, false);
            }
        }
    }
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
    IfTab *t = tab_new_silent(c);
    if (!t) { set_toast(c, "tab limit (64)"); return NULL; }
    c->active = c->n_tabs - 1;
    c->mode = CM_OMNIBOX; /* INV-1: 空白新タブ → アドレス入力へ直行 */
    c->omni_len = 0;
    c->omni[0] = 0;
    autosave(c);
    return t;
}

bool if_chrome_open(IfChrome *c, const char *path, i32 width) {
    IfTab *t = if_chrome_cur(c);
    bool reuse_blank = (t && t->url[0] == 0 && t->doc == NULL);
    if (!reuse_blank) {
        t = tab_new_silent(c);
        if (!t) { set_toast(c, "tab limit (64)"); return false; }
        c->active = c->n_tabs - 1;
    }
    if (!tab_load(c, t, path, width)) {
        if (!reuse_blank) {
            /* 失敗した新タブは捨てて前に戻る（失敗痕を残さない） */
            if_chrome_close(c, width);
        }
        set_toast(c, "open failed");
        return false;
    }
    free(t->url);
    t->url = dup_cap(path, IF_URL_CAP);
    if_chrome_scroll_to(c, 0, 1);
    t->link_idx = -1;
    c->mode = CM_NORMAL;
    c->omni_len = 0;
    c->omni[0] = 0;
    /* 監査ログ（local-first。オープンが最も軽いユーザ意思表示） */
    if (c->store.enabled)
        if_store_history_add(&c->store, c->now, t->title, t->url);
    autosave(c);
    return true;
}

bool if_chrome_close(IfChrome *c, i32 width) {
    IfTab *t = if_chrome_cur(c);
    if (!t) return false;
    i32 idx = c->active;
    tab_free(t);
    for (i32 i = idx; i < c->n_tabs - 1; i++) c->tabs[i] = c->tabs[i + 1];
    c->n_tabs--;
    if (c->n_tabs == 0) {
        c->active = -1;
        if_chrome_new_blank(c);
        if (c->tabs[0]) c->mode = CM_NORMAL; /* 最後の1枚を閉じたら空白タブが残る（quit は q） */
        autosave(c);
        return true;
    }
    if (c->active >= c->n_tabs) c->active = c->n_tabs - 1;
    lazy_load(c, if_chrome_cur(c), width);
    autosave(c);
    return true;
}

void if_chrome_switch(IfChrome *c, i32 idx, i32 width) {
    if (idx < 0 || idx >= c->n_tabs) return;
    c->active = idx;
    c->quit_armed_at = -1;
    c->toast_len = 0;
    c->toast[0] = 0;
    lazy_load(c, if_chrome_cur(c), width);
    autosave(c);
}

bool if_chrome_reload(IfChrome *c, i32 width) {
    IfTab *t = if_chrome_cur(c);
    if (!t || t->url[0] == 0) { set_toast(c, "nothing to reload"); return false; }
    i32 keep_scroll = t->scroll;
    if (!tab_load(c, t, t->url, width)) { set_toast(c, "reload failed"); return false; }
    t->scroll = keep_scroll;
    autosave(c);
    return true;
}

void if_chrome_relayout(IfChrome *c, i32 width) {
    for (i32 i = 0; i < c->n_tabs; i++) c->tabs[i]->dirty = true;
    IfTab *t = if_chrome_cur(c);
    if (t && t->doc) {
        tab_build_view(t, width);
        i32 maxs = t->doc_h > 0 ? t->doc_h : 0;
        if (t->scroll > maxs) t->scroll = maxs;
    }
    /* autosave なし: リサイズは構造変化ではなく、SIGWINCH 連打での書込増幅を避ける */
}

/* ---- スクロール（slice-1 のクランプ規則そのまま: max = h - vh） ---- */
i32 if_chrome_scroll(IfChrome *c, i32 delta, i32 vh) {
    IfTab *t = if_chrome_cur(c);
    if (!t || !t->lay) return 0;
    i32 maxs = t->doc_h > vh ? t->doc_h - vh : 0;
    t->scroll += delta;
    if (t->scroll < 0) t->scroll = 0;
    if (t->scroll > maxs) t->scroll = maxs;
    return t->scroll;
}

void if_chrome_scroll_to(IfChrome *c, i32 pos, i32 vh) {
    IfTab *t = if_chrome_cur(c);
    if (!t || !t->lay) return;
    i32 maxs = t->doc_h > vh ? t->doc_h - vh : 0;
    t->scroll = pos < 0 ? 0 : (pos > maxs ? maxs : pos);
}

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* 規則は slice-1 のまま（変更禁止で固定済みの公開セマンティクス） */
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

/* ---- slice-2: グループ / 検索 / ブックマーク / 復元 ---- */

void if_chrome_set_group(IfChrome *c, const char *name) {
    IfTab *t = if_chrome_cur(c);
    if (!t) return;
    free(t->group);
    t->group = NULL;
    if (name && name[0]) t->group = dup_cap(name, IF_GROUP_CAP);
    autosave(c);
    set_toast(c, name && name[0] ? "grouped" : "group cleared");
}

/* 大小無視 ASCII の部分一致（title/url/group のどれかに含まれるタブを拾う） */
static bool ci_contains(const char *hay, const char *needle) {
    u32 n = (u32)strlen(needle);
    u32 h = (u32)strlen(hay);
    if (n == 0 || n > h) return false;
    for (u32 i = 0; i + n <= h; i++) {
        u32 j = 0;
        while (j < n) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            j++;
        }
        if (j == n) return true;
    }
    return false;
}

i32 if_chrome_find_tabs(const IfChrome *c, const char *query, i32 *idx_out, i32 max) {
    if (max <= 0 || !query || !query[0]) return 0;
    i32 n = 0;
    for (i32 i = 0; i < c->n_tabs && n < max; i++) {
        const IfTab *t = c->tabs[i];
        if (!t) continue;
        if (ci_contains(t->title, query) || ci_contains(t->url, query)
            || (t->group && ci_contains(t->group, query)))
            idx_out[n++] = i;
    }
    return n;
}

void if_chrome_bookmark_cur(IfChrome *c) {
    IfTab *t = if_chrome_cur(c);
    if (!t || t->url[0] == 0) { set_toast(c, "nothing to bookmark"); return; }
    bool added = false;
    if (!if_store_bookmark_toggle(&c->store, t->title, t->url, &added)) {
        set_toast(c, "bookmark failed (store unavailable)");
        return;
    }
    set_toast(c, added ? "bookmarked" : "bookmark removed");
}

i32 if_chrome_restore(IfChrome *c, i32 width) {
    IfArena a;
    if_arena_init(&a, 1 << 18);
    IfSessionTab *tabs = NULL;
    i32 active_id = -1;
    i32 n = if_store_session_parse(&c->store, &a, &tabs, &active_id);
    if (n <= 0) {
        if_arena_destroy(&a);
        return 0;
    }
    i32 max_id = 0;
    for (i32 i = 0; i < n && c->n_tabs < IF_TABS_MAX; i++) {
        IfTab *t = tab_new_silent(c);
        if (!t) break;
        t->id = tabs[i].id > 0 ? tabs[i].id : (i + 1);
        if (t->id > max_id) max_id = t->id;
        free(t->url);
        t->url = dup_cap(tabs[i].url ? tabs[i].url : "", IF_URL_CAP);
        free(t->title);
        t->title = dup_cap(tabs[i].title && tabs[i].title[0] ? tabs[i].title : "New Tab",
                           IF_TITLE_CAP);
        if (tabs[i].group && tabs[i].group[0])
            t->group = dup_cap(tabs[i].group, IF_GROUP_CAP);
        t->scroll = tabs[i].scroll; /* grid はまだ無い: lazy_load 後に大域ロード済の vh でクランプ */
    }
    c->next_id = max_id + 1;
    i32 want = -1;
    for (i32 i = 0; i < c->n_tabs; i++)
        if (c->tabs[i]->id == active_id) { want = i; break; }
    c->active = (want >= 0) ? want : 0;
    if_arena_destroy(&a);
    /* active のみ即ロード。他は切替時に lazy_load。scroll は t->scroll に保持済み */
    lazy_load(c, if_chrome_cur(c), width);
    return c->n_tabs;
}
