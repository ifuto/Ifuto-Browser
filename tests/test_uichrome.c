/* Ifuto — クロームモデルの単体テスト（タブ/ストア/ifuto:// ページ）。
 * TUI 入力デコーダのテストは TUI 廃止（2026-08-01、GUI 一本移行）で除去済み。 */
#define _POSIX_C_SOURCE 200809L /* setenv は c11 では隠れる */
#include "tests.h"
#include "../src/chrome.h"
#include "../src/ifuto_pages.h"
#include <string.h>
#include <stdlib.h>

/* --- モデル（in-memory fake fs: 読み書き完全実装） --- */

typedef struct {
    char path[1024];
    char data[65536];
    u32 n;
    bool used;
} FakeEnt;

#define FPOOL_MAX 64
static FakeEnt POOL[FPOOL_MAX];

static FakeEnt *pool_find(const char *path) {
    for (i32 i = 0; i < FPOOL_MAX; i++)
        if (POOL[i].used && strcmp(POOL[i].path, path) == 0) return &POOL[i];
    return NULL;
}

static FakeEnt *pool_put(const char *path) {
    FakeEnt *e = pool_find(path);
    if (e) return e;
    for (i32 i = 0; i < FPOOL_MAX; i++) {
        if (!POOL[i].used) {
            e = &POOL[i];
            snprintf(e->path, sizeof e->path, "%s", path);
            e->n = 0;
            e->data[0] = 0;
            e->used = true;
            return e;
        }
    }
    return NULL;
}

static bool fake_exists(const char *path, void *ctx) {
    (void)ctx;
    return pool_find(path) != NULL;
}

static IfStr fake_read(IfArena *a, const char *path, void *ctx) {
    (void)ctx;
    FakeEnt *e = pool_find(path);
    if (!e) return if_str("", 0);
    char *buf = (char *)if_arena_alloc(a, e->n ? e->n : 1);
    memcpy(buf, e->data, e->n);
    return if_str(buf, e->n);
}

static bool fake_write(const char *path, const void *buf, size_t n, void *ctx) {
    (void)ctx;
    FakeEnt *e = pool_put(path);
    if (!e || n > sizeof e->data) return false;
    memcpy(e->data, buf, n);
    e->n = (u32)n;
    e->data[n] = 0;
    return true;
}

static bool fake_append(const char *path, const void *buf, size_t n, void *ctx) {
    (void)ctx;
    FakeEnt *e = pool_put(path);
    if (!e || e->n + n > sizeof e->data) return false;
    memcpy(e->data + e->n, buf, n);
    e->n += (u32)n;
    e->data[e->n] = 0;
    return true;
}

static bool fake_mkpath(const char *dir, void *ctx) {
    (void)dir; (void)ctx;
    return true;
}

static void pool_reset(void) {
    memset(POOL, 0, sizeof POOL);
    pool_put("/tmp/x/a.html");
    FakeEnt *a = pool_find("/tmp/x/a.html");
    const char *sa = "<title>Alpha</title><p>hello <a href=\"b.html\">link</a></p>"
                     "<p>line2</p><p>line3</p>";
    pool_put("/tmp/x/b.html");
    FakeEnt *b = pool_find("/tmp/x/b.html");
    const char *sb = "<title>Beta</title><p>second</p>";
    fake_write("/tmp/x/a.html", sa, strlen(sa), NULL);
    fake_write("/tmp/x/b.html", sb, strlen(sb), NULL);
    (void)a; (void)b;
}

static IfFsOps fake_fs(void) {
    IfFsOps fs = { fake_exists, fake_read, NULL,
                   fake_write, fake_append, fake_mkpath };
    return fs;
}

static void test_chrome_model(void) {
    pool_reset();
    setenv("IFUTO_HOME", "/fk", 1); /* fake 上のストア prefix */
    IfChrome c;
    if_chrome_init(&c, fake_fs());
    CHECK(c.store.enabled); /* 拡張 fake なのでストア有効 */

    /* 空白タブから開くと再利用される（二重 blank 不発） */
    CHECK(if_chrome_new_blank(&c) != NULL);
    CHECK(c.n_tabs == 1 && c.mode == CM_OMNIBOX);
    CHECK(if_chrome_open(&c, "/tmp/x/a.html", 100));
    CHECK(c.n_tabs == 1);
    IfTab *t = if_chrome_cur(&c);
    CHECK(t && t->doc && t->lay && t->doc_h > 0); /* grid は持たない（viewport 窓は paint 時） */
    CHECK(strcmp(t->title, "Alpha") == 0);
    CHECK(if_chrome_cur_doc_bytes(&c) > 0);

    /* スクロールはクランプされる */
    i32 maxs = t->doc_h;
    CHECK(if_chrome_scroll(&c, 10000, 4) <= maxs);
    CHECK(if_chrome_scroll(&c, -100000, 4) == 0);

    /* リンク巡回 */
    CHECK(t->link_idx == -1);
    CHECK(if_chrome_link_move(&c, 1) == 0);
    CHECK(if_chrome_link_move(&c, 1) == 0);  /* 1 件しか無いので循環 */
    CHECK(if_chrome_link_move(&c, -1) == 0);

    /* 2 枚目を開いて切替（幅は遅延ロードで使われる） */
    CHECK(if_chrome_open(&c, "/tmp/x/b.html", 100));
    CHECK(c.n_tabs == 2 && c.active == 1);
    if_chrome_switch(&c, 0, 100);
    CHECK(if_chrome_cur(&c)->id == c.tabs[0]->id);

    /* quit: タブ複数では 2 連打のみ確定 */
    CHECK(!if_chrome_quit(&c, 1000));
    CHECK(c.toast_len > 0);
    CHECK(if_chrome_quit(&c, 1003));   /* 3 秒以内 */
    CHECK(c.quit_armed_at >= 0);
    if_chrome_quit(&c, 1010);          /* 期限切れ → 再武装して false */
    CHECK(!if_chrome_quit(&c, 1020));

    /* 閉じると arena も解放（計装ゼロに） */
    if_chrome_switch(&c, 1, 100);
    CHECK(if_chrome_close(&c, 100));
    CHECK(c.n_tabs == 1);

    /* resolve */
    char out[4096];
    CHECK(if_chrome_resolve(&c, "/tmp/x/a.html", "/nonexist", out, sizeof out) == 0);
    CHECK(if_chrome_resolve(&c, "https://example.com", "/tmp", out, sizeof out) == 1);
    CHECK(if_chrome_resolve(&c, "nope.html", "/tmp", out, sizeof out) == 2);

    /* 最後の 1 枚を閉じると空白タブが残る */
    CHECK(if_chrome_close(&c, 100));
    CHECK(c.n_tabs == 1 && if_chrome_cur(&c));
    CHECK(!if_chrome_cur(&c)->doc);

    if_chrome_destroy(&c);
}

/* slice-2: グループ / 検索 / セッション往復 / ブックマーク / 履歴 */
static void test_chrome_store(void) {
    pool_reset();
    setenv("IFUTO_HOME", "/fk", 1);

    IfChrome c;
    if_chrome_init(&c, fake_fs());
    c.now = 1750000000;

    /* 2 タブを開き、片方にグループ＆スクロールを与える */
    CHECK(if_chrome_open(&c, "/tmp/x/a.html", 100));
    CHECK(if_chrome_open(&c, "/tmp/x/b.html", 100));
    if_chrome_set_group(&c, "work");
    CHECK(strcmp(if_chrome_cur(&c)->group, "work") == 0);
    if_chrome_scroll(&c, 2, 1); /* vh=1: 短い文書でも maxs>=1、非 0 位置が得られる */
    i32 saved_scroll = if_chrome_cur(&c)->scroll;
    CHECK(saved_scroll > 0);
    i32 saved_active = c.active;

    /* 検索（大小無視・group も対象） */
    i32 hits[16];
    CHECK(if_chrome_find_tabs(&c, "alpha", hits, 16) == 1 && hits[0] == 0);
    CHECK(if_chrome_find_tabs(&c, "ALPHA", hits, 16) == 1);
    CHECK(if_chrome_find_tabs(&c, "work", hits, 16) == 1 && hits[0] == 1);
    CHECK(if_chrome_find_tabs(&c, "b.html", hits, 16) == 1);
    CHECK(if_chrome_find_tabs(&c, "zzz-none", hits, 16) == 0);
    CHECK(if_chrome_find_tabs(&c, "", hits, 16) == 0);

    /* ブックマーク: toggle → list → toggle（除去）→ toggle（再追加の冪等性） */
    if_chrome_bookmark_cur(&c);
    {
        IfArena a;
        if_arena_init(&a, 1 << 16);
        IfStr titles[8], urls[8];
        i32 nb = if_store_bookmarks_list(&c.store, &a, titles, urls, 8);
        CHECK(nb == 1);
        CHECK(urls[0].n == strlen("/tmp/x/b.html")
              && memcmp(urls[0].p, "/tmp/x/b.html", urls[0].n) == 0);
        if_arena_destroy(&a);
    }
    if_chrome_bookmark_cur(&c); /* 除去 */
    {
        IfArena a;
        if_arena_init(&a, 1 << 16);
        IfStr titles[8], urls[8];
        i32 nb = if_store_bookmarks_list(&c.store, &a, titles, urls, 8);
        CHECK(nb == 0);
        if_arena_destroy(&a);
    }

    /* 履歴: open は 2 行を追記した（時刻注入、c.now による） */
    FakeEnt *hist = pool_find("/fk/" IF_STORE_HIST_NAME);
    CHECK(hist != NULL);
    CHECK(strstr(hist->data, "/tmp/x/a.html") != NULL);
    CHECK(strstr(hist->data, "/tmp/x/b.html") != NULL);
    CHECK(strstr(hist->data, "1750000000") != NULL);

    /* セッション往復: destroy → 別インスタンスで restore。
     * メタ (url/title/group/scroll/active) が維持され、非 active は遅延 doc=NULL。
     * scroll は構造変化では自動保存されない（書込増幅禁止）ので、TUI の quit 時と
     * 同じ「最終保存」をここでも明示的に踏む */
    CHECK(if_store_session_save(&c.store, &c));
    if_chrome_destroy(&c);
    IfChrome c2;
    if_chrome_init(&c2, fake_fs());
    i32 nr = if_chrome_restore(&c2, 100);
    CHECK(nr == 2 && c2.n_tabs == 2);
    CHECK(c2.active == saved_active);
    CHECK(strcmp(c2.tabs[1]->group, "work") == 0);
    CHECK(c2.tabs[1]->scroll == saved_scroll);
    CHECK(strcmp(c2.tabs[0]->url, "/tmp/x/a.html") == 0);
    CHECK(strcmp(c2.tabs[1]->title, "Beta") == 0);
    CHECK(c2.tabs[1]->doc != NULL);                 /* active は即ロード */
    CHECK(c2.tabs[0]->doc == NULL);                 /* 非 active は遅延 */
    /* next_id が回復済みで衝突しない */
    CHECK(c2.next_id > c2.tabs[0]->id && c2.next_id > c2.tabs[1]->id);
    /* 遅延ロード: 切替で doc が構築される */
    if_chrome_switch(&c2, 0, 100);
    CHECK(c2.tabs[0]->doc != NULL && c2.tabs[0]->lay != NULL);
    /* 新規タブが既存 id と衝突しない */
    IfTab *nb = if_chrome_new_blank(&c2);
    CHECK(nb != NULL && nb->id != c2.tabs[0]->id && nb->id != c2.tabs[1]->id);
    if_chrome_destroy(&c2);

    /* ストアを消せば restore は 0 */
    FakeEnt *sess = pool_find("/fk/" IF_STORE_SESS_NAME);
    CHECK(sess != NULL);
    sess->used = false;
    IfChrome c3;
    if_chrome_init(&c3, fake_fs());
    CHECK(if_chrome_restore(&c3, 100) == 0);
    if_chrome_destroy(&c3);
}

/* ---- ifuto:// 内部ページ（普通のブラウザの settings/history 相当） ---- */
void test_ifuto_pages(void) {
    pool_reset();
    setenv("IFUTO_HOME", "/fk", 1);
    IfChrome c;
    if_chrome_init(&c, fake_fs());
    CHECK(c.store.enabled);

    /* settings / about / memory: 内部ページが通常 DOM 経路で読める */
    CHECK(if_chrome_open(&c, "ifuto://settings", 100));
    IfTab *t = if_chrome_cur(&c);
    CHECK(t && t->doc && strcmp(t->url, "ifuto://settings") == 0);
    CHECK(strcmp(t->title, "ifuto://settings") == 0);
    IfArena ta; if_arena_init(&ta, 1 << 16);
    IfStr body = if_dom_text_content(&ta, t->dom->root);
    CHECK(body.p != NULL);
    CHECK(if_str_contains(body, "akl") || if_str_contains(body, "Aklus"));
    CHECK(if_str_contains(body, "CoJIT"));

    /* 履歴は開いた順にストアへ。history ページに直前の URL が現れる */
    CHECK(if_chrome_open(&c, "/tmp/x/a.html", 100));
    CHECK(if_chrome_open(&c, "ifuto://history", 100));
    t = if_chrome_cur(&c);
    CHECK(t && t->doc && strcmp(t->title, "ifuto://history") == 0);
    if_arena_destroy(&ta); if_arena_init(&ta, 1 << 16);
    body = if_dom_text_content(&ta, t->dom->root);
    CHECK(if_str_contains(body, "a.html"));
    CHECK(if_str_contains(body, "Alpha"));

    /* memory ページはタブ会計を含む（Alpha が開かれている） */
    CHECK(if_chrome_open(&c, "ifuto://memory", 100));
    t = if_chrome_cur(&c);
    CHECK(t && t->doc && strcmp(t->title, "ifuto://memory") == 0);
    if_arena_destroy(&ta); if_arena_init(&ta, 1 << 16);
    body = if_dom_text_content(&ta, t->dom->root);
    CHECK(if_str_contains(body, "Alpha"));
    CHECK(if_str_contains(body, "KB"));

    /* about / 未知ページ（未知ページも落ちない・案内を出す） */
    CHECK(if_chrome_open(&c, "ifuto://about", 100));
    t = if_chrome_cur(&c);
    CHECK(t && t->doc && strcmp(t->title, "ifuto://about") == 0);
    CHECK(if_chrome_open(&c, "ifuto://nope", 100));
    t = if_chrome_cur(&c);
    CHECK(t && t->doc); /* 内部ページでなければ通常ファイル経路へ（ここでは開けない） */
    CHECK(strcmp(t->url, "ifuto://nope") == 0);

    if_arena_destroy(&ta);
    if_chrome_destroy(&c);
}

void test_uichrome(void) {
    test_chrome_model();
    test_chrome_store();
}
