/* Ifuto — TUI 入力デコーダとクロームモデルの単体テスト */
#include "tests.h"
#include "../src/ui_input.h"
#include "../src/chrome.h"
#include <string.h>

/* --- デコーダ --- */

static IfUiAction feed_str(IfUiDecoder *d, const char *s, IfUiEvent *last) {
    IfUiAction a = UA_NONE;
    IfUiEvent e;
    for (const char *p = s; *p; p++)
        if (if_ui_dec_feed(d, (u8)*p, &e)) { a = e.act; if (last) *last = e; }
    return a;
}

static void test_uinput(void) {
    IfUiDecoder d;
    if_ui_dec_init(&d);

    CHECK(feed_str(&d, "j", NULL) == UA_SCROLL_DOWN);
    CHECK(feed_str(&d, "k", NULL) == UA_SCROLL_UP);
    CHECK(feed_str(&d, "G", NULL) == UA_BOTTOM);
    CHECK(feed_str(&d, "\t", NULL) == UA_LINK_NEXT);
    CHECK(feed_str(&d, "\x1b[Z", NULL) == UA_LINK_PREV);
    CHECK(feed_str(&d, "\x1b[A", NULL) == UA_SCROLL_UP);
    CHECK(feed_str(&d, "\x1b[B", NULL) == UA_SCROLL_DOWN);
    CHECK(feed_str(&d, "\x1b[5~", NULL) == UA_PAGE_UP);
    CHECK(feed_str(&d, "\x1b[6~", NULL) == UA_PAGE_DOWN);
    CHECK(feed_str(&d, "\x1b[H", NULL) == UA_TOP);
    CHECK(feed_str(&d, "\x1bOH", NULL) == UA_TOP);
    IfUiEvent e;
    CHECK(feed_str(&d, "3", &e) == UA_TAB_1 + 2);
    CHECK(feed_str(&d, "\x1b", NULL) == UA_NONE);           /* 単独 ESC は prefix 待ち */
    CHECK(d.state == 1);
    CHECK(feed_str(&d, "x", NULL) == UA_ESC);                /* 後続が非系列 → ESC 確定 */
    if_ui_dec_init(&d);
    IfUiEvent ce;
    CHECK(feed_str(&d, "\xc3", &ce) == UA_CHAR && ce.a1 == 0xc3); /* UTF-8 head バイト */
    if_ui_dec_init(&d);
    CHECK(feed_str(&d, "\x7f", NULL) == UA_BACKSPACE);
    CHECK(feed_str(&d, "\r", NULL) == UA_OPEN_LINK);

    /* literal（オムニボックス）モード: アクションキーは UA_CHAR になる */
    if_ui_dec_init(&d);
    d.literal = 1;
    IfUiEvent le;
    CHECK(feed_str(&d, "j", &le) == UA_CHAR && le.a1 == 'j');
    CHECK(feed_str(&d, "q", &le) == UA_CHAR && le.a1 == 'q');
    CHECK(feed_str(&d, "3", &le) == UA_CHAR && le.a1 == '3');
    CHECK(feed_str(&d, "\x1b[A", NULL) == UA_SCROLL_UP); /* 矢印は引き続きアクション */
    CHECK(feed_str(&d, "\r", NULL) == UA_OPEN_LINK);
    CHECK(feed_str(&d, "\x7f", NULL) == UA_BACKSPACE);
}

/* --- モデル（フェイク fs） --- */

typedef struct { const char *path; const char *content; } FakeFile;

static bool fake_exists(const char *path, void *ctx) {
    const FakeFile *fs = (const FakeFile *)ctx;
    for (; fs->path; fs++)
        if (strcmp(fs->path, path) == 0) return true;
    return false;
}

static IfStr fake_read(IfArena *a, const char *path, void *ctx) {
    const FakeFile *fs = (const FakeFile *)ctx;
    for (; fs->path; fs++) {
        if (strcmp(fs->path, path) == 0) {
            u64 n = strlen(fs->content);
            char *buf = (char *)if_arena_alloc(a, n ? n : 1);
            memcpy(buf, fs->content, n);
            return if_str(buf, (u32)n);
        }
    }
    return if_str("", 0);
}

static const FakeFile FAKE[] = {
    { "/tmp/x/a.html", "<title>Alpha</title><p>hello <a href=\"b.html\">link</a></p>"
      "<p>line2</p><p>line3</p>" },
    { "/tmp/x/b.html", "<title>Beta</title><p>second</p>" },
    { NULL, NULL },
};

static IfFsOps fake_fs(void) {
    IfFsOps fs = { fake_exists, fake_read, (void *)FAKE };
    return fs;
}

static void test_chrome_model(void) {
    IfChrome c;
    if_chrome_init(&c, fake_fs());

    /* 空白タブから開くと再利用される（二重 blank 不発） */
    CHECK(if_chrome_new_blank(&c) != NULL);
    CHECK(c.n_tabs == 1 && c.mode == CM_OMNIBOX);
    CHECK(if_chrome_open(&c, "/tmp/x/a.html", 100));
    CHECK(c.n_tabs == 1);
    IfTab *t = if_chrome_cur(&c);
    CHECK(t && t->doc && t->lay && t->grid);
    CHECK(strcmp(t->title, "Alpha") == 0);
    CHECK(if_chrome_cur_doc_bytes(&c) > 0);

    /* スクロールはクランプされる */
    i32 maxs = t->grid->h;
    CHECK(if_chrome_scroll(&c, 10000, 4) <= maxs);
    CHECK(if_chrome_scroll(&c, -100000, 4) == 0);

    /* リンク巡回 */
    CHECK(t->link_idx == -1);
    CHECK(if_chrome_link_move(&c, 1) == 0);
    CHECK(if_chrome_link_move(&c, 1) == 0);  /* 1 件しか無いので循環 */
    CHECK(if_chrome_link_move(&c, -1) == 0);

    /* 2 枚目を開いて切替 */
    CHECK(if_chrome_open(&c, "/tmp/x/b.html", 100));
    CHECK(c.n_tabs == 2 && c.active == 1);
    if_chrome_switch(&c, 0);
    CHECK(if_chrome_cur(&c)->id == c.tabs[0]->id);

    /* quit: タブ複数では 2 連打のみ確定 */
    CHECK(!if_chrome_quit(&c, 1000));
    CHECK(c.toast_len > 0);
    CHECK(if_chrome_quit(&c, 1003));   /* 3 秒以内 */
    CHECK(c.quit_armed_at >= 0);
    if_chrome_quit(&c, 1010);          /* 期限切れ → 再武装して false */
    CHECK(!if_chrome_quit(&c, 1020));

    /* 閉じると arena も解放（計装ゼロに） */
    if_chrome_switch(&c, 1);
    CHECK(if_chrome_close(&c));
    CHECK(c.n_tabs == 1);

    /* resolve */
    char out[4096];
    CHECK(if_chrome_resolve(&c, "/tmp/x/a.html", "/nonexist", out, sizeof out) == 0);
    CHECK(if_chrome_resolve(&c, "https://example.com", "/tmp", out, sizeof out) == 1);
    CHECK(if_chrome_resolve(&c, "nope.html", "/tmp", out, sizeof out) == 2);

    /* 最後の 1 枚を閉じると空白タブが残る */
    CHECK(if_chrome_close(&c));
    CHECK(c.n_tabs == 1 && if_chrome_cur(&c));
    CHECK(!if_chrome_cur(&c)->doc);

    if_chrome_destroy(&c);
}

void test_uichrome(void) {
    test_uinput();
    test_chrome_model();
}
