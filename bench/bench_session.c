/* v-chrome 天井検証: 50 タブのセッション復元（遅延ロード込み、≤ 100ms 採択天井）。
 * 実 fs 上に 50 個の小さな HTML と session.txt を作って if_chrome_restore を計る。
 * 計測には active の即ロード（parse+style+layout+grid）も含む（厳しい側の定義）。
 */
#define _POSIX_C_SOURCE 200809L
#include "../src/chrome.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(void) {
    const char *dir = "/tmp/ifuto-sessbench";
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s/docs", dir, dir);
    if (system(cmd) != 0) return 1;

    /* 文書 50 件（小） */
    for (int i = 0; i < 50; i++) {
        char p[256];
        snprintf(p, sizeof p, "%s/docs/d%02d.html", dir, i);
        FILE *f = fopen(p, "wb");
        if (!f) return 1;
        fprintf(f, "<title>Doc %d</title><h1>Heading %d</h1>"
                   "<p>paragraph one with <a href=\"d%02d.html\">a link</a></p>"
                   "<p>日本語の混ざる段落もどき。abc def ghi jkl mno pqr.</p>",
                i, i, (i + 1) % 50);
        fclose(f);
    }

    setenv("IFUTO_HOME", dir, 1);
    IfFsOps fs = { if_fs_exists_real, if_fs_read_real, NULL,
                   if_fs_write_real, if_fs_append_real, if_fs_mkpath_real };

    /* 1) session.txt を本物のモデル経由で作る */
    IfChrome c;
    if_chrome_init(&c, fs);
    c.now = 1750000000;
    char p[256];
    for (int i = 0; i < 50; i++) {
        snprintf(p, sizeof p, "%s/docs/d%02d.html", dir, i);
        if (!if_chrome_open(&c, p, 100)) { fprintf(stderr, "open %d failed\n", i); return 1; }
    }
    if (c.n_tabs != 50) { fprintf(stderr, "tabs=%d expected 50\n", (int)c.n_tabs); return 1; }
    if_chrome_switch(&c, 25, 100); /* 中間のタブを active に */
    i32 active_id = if_chrome_cur(&c)->id;
    if_chrome_destroy(&c);

    /* 2) 復元時間（parse+メタ再構築+active の即ロード） */
    double t0 = now_ms();
    IfChrome c2;
    if_chrome_init(&c2, fs);
    i32 n = if_chrome_restore(&c2, 100);
    double dt = now_ms() - t0;

    printf("restored_tabs: %d (<=64)\n", n);
    printf("restore_ms: %.2f  (ceiling: 100 ms)\n", dt);
    printf("active_id_roundtrip: %s\n",
           if_chrome_cur(&c2) && if_chrome_cur(&c2)->id == active_id ? "ok" : "FAIL");
    printf("non_active_lazy: %s\n",
           (c2.tabs[0] && c2.tabs[0]->doc == NULL) ? "ok (doc=NULL)" : "FAIL");

    if_chrome_destroy(&c2);
    if (n != 50) return 1;
    if (dt > 100.0) { fprintf(stderr, "FAIL: exceeds 100ms ceiling\n"); return 2; }
    return 0;
}
