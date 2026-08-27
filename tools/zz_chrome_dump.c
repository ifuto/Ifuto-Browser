/* tools/zz_chrome_dump.c — chrome.c 純粋部の差分 fuzz 用 C オラクル dump ドライバ。
 *
 * 役割: stdin の命令行を 1 行ずつ実行し、結果を機械可読な 1 行で stdout に返す。
 * Rust 側 `rust/ifuto-core/examples/zz_chrome_dump.rs` と規格を合わせ、
 * tools/zz_chrome_diff.py が両出力を byte 突合する。
 *
 * 規約:
 *   - 文字列引数は lowercase hex
 *   - 数値は 10 進（負数可）
 *   - fs.exists は決定論予言者擬装: exists(s) = (fnv1a64(s) % 4 == 0)
 *     （Rust 側が同一式で実装）
 *   - DUP の cap == 0 は C が u32 underflow で 4GiB alloc に進む領域のため生成禁止
 *     （chrome.rs の dup_cap 契約と同じ信頼域）
 *
 * 命令一覧（→ 出力）:
 *   CI  <hay> <needle>              → CI <0|1>
 *   DUP <s> <cap>                   → DUP <out-hex>
 *   SCR <scroll> <delta> <vh> <doc_h> → SCR <new-scroll>
 *   SCT <pos> <vh> <doc_h>          → SCT <new-scroll>
 *   QUI <n_tabs> <armed_at> <now>   → QUI <rc> <new-armed_at>
 *   LNK <idx> <delta> <n_links>     → LNK <new-idx>
 *   RES <input> <cwd> <cap>         → RES <rc> <out-hex|->
 *   FT  <max> <ntab> <query> [t u g]×ntab → FT <i0,i1,...>   (g が "-" なら NULL)
 *
 * トークンは空白（スペース/タブ/改行）区切り。巨大 hex トークン（数 KB 級）も
 * sscanf による中間コピーなしで span から直接 unhex する（截断事故の排除）。
 */
#include "../src/chrome.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- 決定論 fs 予言者（Rust 側と同一式にすること） ---- */
static u64 fnv1a64(const char *s) {
    u64 h = 14695981039346656037ULL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}
static bool fake_exists(const char *path, void *ctx) {
    (void)ctx;
    return fnv1a64(path) % 4 == 0;
}
static IfStr fake_read(IfArena *a, const char *path, void *ctx) {
    (void)a; (void)path; (void)ctx;
    return if_str(NULL, 0);
}
static bool fake_write(const char *path, const void *buf, size_t n, void *ctx) {
    (void)path; (void)buf; (void)n; (void)ctx;
    return true;
}
static bool fake_mkpath(const char *dir, void *ctx) {
    (void)dir; (void)ctx;
    return true;
}

/* ---- トークナイザ（空白区切り、NUL 終端 span を返す） ---- */
static char *next_tok(char **pp) {
    char *p = *pp;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (!*p) { *pp = p; return NULL; }
    char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    if (*p) *p++ = 0;   /* 区切りを NUL に置換して次へ */
    *pp = p;
    return start;
}
static long tok_long(char **pp) {
    char *t = next_tok(pp);
    return t ? strtol(t, NULL, 10) : 0;
}

/* ---- hex ユーティリティ ---- */
static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
/* hex トークンをバッファへ（常に NUL 終端。戻り値はバイト数） */
static size_t unhex(const char *s, char *out, size_t cap) {
    size_t n = 0;
    while (s[0] && s[1] && n + 1 < cap) {
        int hi = hexval((unsigned char)s[0]);
        int lo = hexval((unsigned char)s[1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = (char)((hi << 4) | lo);
        s += 2;
    }
    out[n] = 0;
    return n;
}
/* トークンから直接 unhex（見つからなければ空） */
static size_t unhex_tok(char **pp, char *out, size_t cap) {
    char *t = next_tok(pp);
    if (!t) { out[0] = 0; return 0; }
    return unhex(t, out, cap);
}
static void put_hex(const char *s, size_t n) {
    static const char *D = "0123456789abcdef";
    for (size_t i = 0; i < n; i++)
        printf("%c%c", D[(unsigned char)s[i] >> 4], D[(unsigned char)s[i] & 15]);
}

#define BUFSZ 40960
static char IBUF[BUFSZ], OBUF[BUFSZ], ABUF[BUFSZ], BBUF[BUFSZ];

int main(void) {
    static char line[BUFSZ * 3];
    IfFsOps fs = { 0 };
    fs.exists = fake_exists;
    fs.read_file = fake_read;
    fs.write_file = fake_write;
    fs.append = fake_write;
    fs.mkpath = fake_mkpath;

    static char raw[BUFSZ * 3];
    while (fgets(line, sizeof line, stdin)) {
        memcpy(raw, line, sizeof raw);
        char *p = line;
        char *op = next_tok(&p);
        if (!op) continue;
        if (!strcmp(op, "CI")) {
            unhex_tok(&p, ABUF, sizeof ABUF);
            unhex_tok(&p, BBUF, sizeof BBUF);
            printf("CI %d\n", if_chrome_test_ci_contains(ABUF, BBUF) ? 1 : 0);
        } else if (!strcmp(op, "DUP")) {
            unhex_tok(&p, ABUF, sizeof ABUF);
            u32 cap = (u32)tok_long(&p);
            char *r = if_chrome_test_dup_cap(ABUF, cap);
            printf("DUP ");
            put_hex(r, strlen(r));
            printf("\n");
            free(r);
        } else if (!strcmp(op, "SCR")) {
            long scroll = tok_long(&p), delta = tok_long(&p);
            long vh = tok_long(&p), doc_h = tok_long(&p);
            IfChrome c;
            memset(&c, 0, sizeof c);
            IfTab t;
            memset(&t, 0, sizeof t);
            IfLayout lay;
            memset(&lay, 0, sizeof lay);
            t.lay = &lay;
            t.doc_h = (i32)doc_h;
            t.scroll = (i32)scroll;
            c.tabs[0] = &t;
            c.n_tabs = 1;
            c.active = 0;
            printf("SCR %d\n", if_chrome_scroll(&c, (i32)delta, (i32)vh));
        } else if (!strcmp(op, "SCT")) {
            long pos = tok_long(&p), vh = tok_long(&p), doc_h = tok_long(&p);
            IfChrome c;
            memset(&c, 0, sizeof c);
            IfTab t;
            memset(&t, 0, sizeof t);
            IfLayout lay;
            memset(&lay, 0, sizeof lay);
            t.lay = &lay;
            t.doc_h = (i32)doc_h;
            c.tabs[0] = &t;
            c.n_tabs = 1;
            c.active = 0;
            if_chrome_scroll_to(&c, (i32)pos, (i32)vh);
            printf("SCT %d\n", t.scroll);
        } else if (!strcmp(op, "QUI")) {
            long nt = tok_long(&p), armed = tok_long(&p), now = tok_long(&p);
            IfChrome c;
            memset(&c, 0, sizeof c);
            c.n_tabs = (i32)nt;
            c.quit_armed_at = armed;
            bool r = if_chrome_quit(&c, now);
            printf("QUI %d %lld\n", r ? 1 : 0, (long long)c.quit_armed_at);
        } else if (!strcmp(op, "LNK")) {
            long idx = tok_long(&p), delta = tok_long(&p), nl = tok_long(&p);
            IfChrome c;
            memset(&c, 0, sizeof c);
            IfTab t;
            memset(&t, 0, sizeof t);
            IfLayout lay;
            memset(&lay, 0, sizeof lay);
            lay.n_links = (u32)nl;
            t.lay = &lay;
            t.link_idx = (i32)idx;
            c.tabs[0] = &t;
            c.n_tabs = 1;
            c.active = 0;
            printf("LNK %d\n", if_chrome_link_move(&c, (i32)delta));
        } else if (!strcmp(op, "RES")) {
            unhex_tok(&p, IBUF, sizeof IBUF);
            unhex_tok(&p, ABUF, sizeof ABUF);
            u32 cap = (u32)tok_long(&p);
            IfChrome c;
            memset(&c, 0, sizeof c);
            c.fs = fs;
            /* out バッファを 0x01 センチネルで初期化（C が書かなければ検出できる。
             * fuzz 入力は 0x01 を含まないドメインに制限してある） */
            memset(OBUF, 0x01, 512);
            OBUF[511] = 0;
            i32 rc = if_chrome_resolve(&c, IBUF, ABUF, OBUF, cap);
            printf("RES %d ", rc);
            if ((unsigned char)OBUF[0] == 0x01) {
                printf("-\n");
            } else {
                put_hex(OBUF, strlen(OBUF));
                printf("\n");
            }
        } else if (!strcmp(op, "FT")) {
            long max = tok_long(&p), ntab = tok_long(&p);
            char qhex[8192];
            {
                char *q = next_tok(&p);
                if (!q) goto bad;
                snprintf(qhex, sizeof qhex, "%s", q);
            }
            if (ntab < 0 || ntab > 64) goto bad;
            IfChrome c;
            memset(&c, 0, sizeof c);
            static IfTab tabs[64];
            static char ttitle[64][IF_TITLE_CAP], turl[64][IF_URL_CAP], tgrp[64][IF_GROUP_CAP];
            char gbuf[IF_GROUP_CAP * 2 + 2];
            int i;
            for (i = 0; i < ntab; i++) {
                unhex_tok(&p, ttitle[i], IF_TITLE_CAP);
                unhex_tok(&p, turl[i], IF_URL_CAP);
                char *gt = next_tok(&p);
                if (!gt) goto bad;
                snprintf(gbuf, sizeof gbuf, "%s", gt);
                memset(&tabs[i], 0, sizeof tabs[i]);
                if (!strcmp(gbuf, "-")) {
                    tabs[i].group = NULL;
                } else {
                    unhex(gbuf, tgrp[i], IF_GROUP_CAP);
                    tabs[i].group = tgrp[i];
                }
                tabs[i].title = ttitle[i];
                tabs[i].url = turl[i];
                c.tabs[c.n_tabs++] = &tabs[i];
            }
            if (i != ntab) goto bad;
            unhex(qhex, IBUF, sizeof IBUF);
            i32 idx[64];
            i32 got = if_chrome_find_tabs(&c, IBUF, idx, (i32)max);
            printf("FT ");
            for (i32 k = 0; k < got; k++) printf("%s%d", k ? "," : "", idx[k]);
            printf("\n");
        } else {
        bad:
            printf("BAD %s", raw);
        }
        fflush(stdout);
    }
    return 0;
}
