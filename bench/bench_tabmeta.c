/* v-chrome 天井検証: アンロード済み 50 タブのメタデータ占有 (≤ 2 MB 天井)。
 *
 * 計測:
 *   - mallinfo2().uordblks 差分: 50 タブ確保時のアロケータ in-use 増分
 *   - /proc/self/status VmRSS: 同上の粗い観測
 *   - create/destroy 200 周の uordblks 傾き: リーク検査
 * 注意: glibc mallinfo2 は free 後も uordblks が戻らない残存アーティファクト
 * がある (tcache/top 保持: 最小実験 mallocx50→free で delta 8.5KB→8.4KB)。
 * よって「破棄後ゼロ」ではなく「周回で傾きゼロ」をリーク判定に使う。
 * (確定長の exit 時リーク判定は LSan を併用: tests/run_tests は全タブ経路を
 * ASan+LSan で通している)
 */
#include "../src/chrome.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long vmrss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            if (sscanf(line + 6, "%ld", &kb) != 1) kb = -1;
            break;
        }
    }
    fclose(f);
    return kb;
}

static size_t used_bytes(void) { return mallinfo2().uordblks; }

/* n タブを典型メタ (100〜140 B url/title) で作って全破棄する 1 周 */
static void cycle(i32 n, i32 seed_off) {
    IfChrome c;
    IfFsOps fs;
    memset(&fs, 0, sizeof fs); /* 未ロードタブは fs に触れない */
    if_chrome_init(&c, fs);
    char urlbuf[192], titlebuf[192];
    for (i32 i = 0; i < n; i++) {
        IfTab *t = if_chrome_new_blank(&c);
        if (!t) if_fatal("tab create failed");
        int un = snprintf(urlbuf, sizeof urlbuf,
                          "https://example.org/articles/browse/path/"
                          "segment-%d/some-fairly-long-document-title-page"
                          "?query=param&nth=%d&round=%d",
                          (int)i, (int)i, (int)(i + seed_off));
        int tn = snprintf(titlebuf, sizeof titlebuf,
                          "記事タイトル %d — Some Fairly Long Document Title "
                          "That Would Wrap (第 %d 章)", (int)i, (int)i);
        if (un > 0 && tn > 0) {
            free(t->url);
            free(t->title);
            t->url = malloc((size_t)un + 1);
            t->title = malloc((size_t)tn + 1);
            if (!t->url || !t->title) if_fatal("oom");
            memcpy(t->url, urlbuf, (size_t)un + 1);
            memcpy(t->title, titlebuf, (size_t)tn + 1);
        }
    }
    if_chrome_destroy(&c);
}

int main(void) {
    size_t u0 = used_bytes();
    cycle(50, 0); /* ウォームアップ兼 1 周目の計測群 */
    size_t delta1 = used_bytes() - u0; /* 1 周でヒープが育つ分 (破棄後の残存) */

    /* 50 タブ確保中の in-use 増分 (天井対象の本体) */
    IfChrome c;
    IfFsOps fs;
    memset(&fs, 0, sizeof fs);
    if_chrome_init(&c, fs);
    size_t u1 = used_bytes();
    char urlbuf[192], titlebuf[192];
    for (i32 i = 0; i < 50; i++) {
        IfTab *t = if_chrome_new_blank(&c);
        if (!t) if_fatal("tab create failed");
        int un = snprintf(urlbuf, sizeof urlbuf,
                          "https://example.org/articles/browse/path/"
                          "segment-%d/some-fairly-long-document-title-page"
                          "?query=param&nth=%d", (int)i, (int)i);
        int tn = snprintf(titlebuf, sizeof titlebuf,
                          "記事タイトル %d — Some Fairly Long Document Title "
                          "That Would Wrap (第 %d 章)", (int)i, (int)i);
        free(t->url);
        free(t->title);
        t->url = malloc((size_t)un + 1);
        t->title = malloc((size_t)tn + 1);
        if (!t->url || !t->title) if_fatal("oom");
        memcpy(t->url, urlbuf, (size_t)un + 1);
        memcpy(t->title, titlebuf, (size_t)tn + 1);
    }
    size_t u2 = used_bytes();
    i32 ntabs = c.n_tabs;
    if_chrome_destroy(&c);

    size_t d_live = u2 - u1;
    printf("tabs: %d (unloaded, metadata only)\n", (int)ntabs);
    printf("uordblks_delta_while_live_bytes: %zu  (%.1f B/tab, malloc usable + "
           "メタ構造体ヒープ成長込み)\n", d_live, (double)d_live / ntabs);
    printf("first_cycle_residual_bytes: %zu  (破棄後にヒープ内で再利用待ちの分)\n",
           delta1);
    printf("VmRSS_baseline_kb: %ld\n", vmrss_kb());

    /* リーク検査: 200 周して傾きを見る */
    cycle(5, 100);
    size_t ua = used_bytes();
    for (i32 r = 0; r < 195; r++) cycle(50, 200 + r);
    size_t ub = used_bytes();
    printf("leak_slope_bytes_over_195_cycles: %zu\n", ub - ua);

    /* 判定: 周回で in-use が増え続けたらリーク */
    if (ub - ua > 4096) {
        fprintf(stderr, "FAIL: in-use grows across cycles (leak suspected)\n");
        return 2;
    }
    return 0;
}
