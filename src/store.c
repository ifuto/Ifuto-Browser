/* Ifuto — 永続ストア層（実装。規約は store.h ヘッダ参照） */
#include "store.h"
#include "chrome.h" /* IfChrome/IfTab のフィールド参照（save のみ） */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>





/* ---- dir 解決 ---- */

bool if_store_init(IfStore *s, const IfFsOps *fs, bool create) {
    memset(s, 0, sizeof *s);
    s->fs = fs;
    if (!fs->write_file || !fs->append || !fs->mkpath || !fs->exists || !fs->read_file)
        return false; /* 書き込み不能な fs = ストア無効（静かに no-op） */
    /* IFUTO_NO_STORE: 痕跡を残さない明示 kill switch（QA の --shot ラスタ oracle が
     * セッション履歴で揺れないよう、保存も復元も丸ごと止める） */
    if (getenv("IFUTO_NO_STORE")) return false;

    const char *base = getenv("IFUTO_HOME");
    char d[IF_STORE_DIR_CAP];
    if (base && base[0]) {
        if (strlen(base) >= sizeof d) return false;
        snprintf(d, sizeof d, "%s", base);
    } else if ((base = getenv("XDG_DATA_HOME")) != NULL && base[0]) {
        if (snprintf(d, sizeof d, "%s/ifuto", base) >= (int)sizeof d) return false;
    } else if ((base = getenv("HOME")) != NULL && base[0]) {
        if (snprintf(d, sizeof d, "%s/.local/share/ifuto", base) >= (int)sizeof d)
            return false;
    } else {
        return false; /* cwd への不意書き込み禁止: 場所不明ならストア無効 */
    }
    memcpy(s->dir, d, strlen(d) + 1);
    if (create && !fs->mkpath(s->dir, fs->ctx)) return false;
    s->enabled = true;
    return true;
}

void if_store_path(const IfStore *s, const char *name, char *out, u32 cap) {
    if (!s->enabled) { if (cap) out[0] = 0; return; }
    snprintf(out, cap, "%s/%s", s->dir, name);
}

/* ---- 行ビルダ（arena 内 grow バッファ） ---- */

typedef struct { IfArena *a; char *p; u32 n, cap; } GenBuf;

static void gb_init(GenBuf *g, IfArena *a, u32 cap0) {
    g->a = a;
    g->cap = cap0;
    g->n = 0;
    g->p = (char *)if_arena_alloc(a, g->cap);
    g->p[0] = 0;
}

static void gb_raw(GenBuf *g, const char *s, u32 n) {
    if (g->n + n + 1 > g->cap) {
        u32 ncap = g->cap * 2;
        while (ncap < g->n + n + 1) ncap *= 2;
        char *np = (char *)if_arena_alloc(g->a, ncap);
        memcpy(np, g->p, g->n);
        g->p = np; /* 旧確保は arena リーク内処理（短命で上限あり） */
        g->cap = ncap;
    }
    memcpy(g->p + g->n, s, n);
    g->n += n;
    g->p[g->n] = 0;
}

static void gb_lit(GenBuf *g, const char *s) { gb_raw(g, s, (u32)strlen(s)); }

/* 保存側の無害化: \t \n \r は空白へ（行形式の構造文字を殺す） */
static void gb_safe(GenBuf *g, const char *s, u32 maxlen) {
    for (u32 i = 0; s[i] && i < maxlen; i++) {
        char ch = s[i];
        gb_raw(g, (ch == '\t' || ch == '\n' || ch == '\r') ? " " : &ch, 1);
    }
}

static void gb_i32(GenBuf *g, i32 v) {
    char t[16];
    snprintf(t, sizeof t, "%d", (int)v);
    gb_lit(g, t);
}

/* ---- session ---- */

bool if_store_session_save(const IfStore *s, const IfChrome *c) {
    if (!s->enabled) return false;
    IfArena a;
    if_arena_init(&a, 1 << 15);
    GenBuf g;
    gb_init(&g, &a, 1 << 12);
    gb_lit(&g, "ifuto-session 1\n");

    /* 空白タブは保存しない（次回の復元で blank は再生しない方がゴミ残りしない） */
    i32 first_managed = -1;
    for (i32 i = 0; i < c->n_tabs; i++) {
        IfTab *t = c->tabs[i];
        if (!t || t->url[0] == 0) continue;
        if (first_managed < 0) first_managed = t->id;
        gb_lit(&g, "url ");
        gb_i32(&g, t->id);
        gb_lit(&g, " ");
        gb_safe(&g, t->url, IF_URL_CAP);
        gb_lit(&g, "\n");
        if (t->title && t->title[0]) {
            gb_lit(&g, "title ");
            gb_i32(&g, t->id);
            gb_lit(&g, " ");
            gb_safe(&g, t->title, IF_TITLE_CAP);
            gb_lit(&g, "\n");
        }
        if (t->group && t->group[0]) {
            gb_lit(&g, "group ");
            gb_i32(&g, t->id);
            gb_lit(&g, " ");
            gb_safe(&g, t->group, IF_GROUP_CAP);
            gb_lit(&g, "\n");
        }
        if (t->scroll > 0) {
            gb_lit(&g, "scroll ");
            gb_i32(&g, t->id);
            gb_lit(&g, " ");
            gb_i32(&g, t->scroll);
            gb_lit(&g, "\n");
        }
    }
    /* active が保存対象ならそれを、でなければ先頭（Managed なしなら active 行自体無し） */
    IfTab *cur = NULL;
    if (c->active >= 0 && c->active < c->n_tabs) cur = c->tabs[c->active];
    if (cur && cur->url[0] != 0) {
        gb_lit(&g, "active ");
        gb_i32(&g, cur->id);
        gb_lit(&g, "\n");
    } else if (first_managed >= 0) {
        gb_lit(&g, "active ");
        gb_i32(&g, first_managed);
        gb_lit(&g, "\n");
    }
    gb_lit(&g, "end\n");

    char path[IF_STORE_DIR_CAP + 64];
    if_store_path(s, IF_STORE_SESS_NAME, path, sizeof path);
    bool ok = s->fs->write_file(path, g.p, g.n, s->fs->ctx);
    if_arena_destroy(&a);
    return ok;
}

/* "i32 の後ろの rest-of-line" を取る厳密ラッパ。戻り値は line 内 id、REST に内容。
 * 失敗時 -1。行長・i32 の厳密範囲チェック込み（攻撃的入力を黙殺する） */
static i32 pre_id(const char *line, const char *kind, const char **rest) {
    u32 k = (u32)strlen(kind);
    if (strncmp(line, kind, k) != 0 || line[k] != ' ') return -1;
    const char *p = line + k + 1;
    long id = 0;
    bool got = false;
    while (*p >= '0' && *p <= '9') {
        id = id * 10 + (*p - '0');
        if (id > 1000000) return -1; /* id 過大: 異常行 */
        got = true;
        p++;
    }
    if (!got) return -1;
    if (rest) {
        if (*p != ' ') return -1;
        *rest = p + 1;
    }
    return (i32)id;
}

static IfSessionTab *by_id(IfSessionTab *t, i32 n, i32 id) {
    for (i32 i = 0; i < n; i++)
        if (t[i].id == id) return &t[i];
    return NULL;
}

i32 if_store_session_parse(const IfStore *s, IfArena *a,
                           IfSessionTab **tabs_out, i32 *active_id) {
    *tabs_out = NULL;
    *active_id = -1;
    if (!s->enabled) return 0;
    char path[IF_STORE_DIR_CAP + 64];
    if_store_path(s, IF_STORE_SESS_NAME, path, sizeof path);
    IfStr txt = s->fs->read_file(a, path, s->fs->ctx);
    if (!txt.p || txt.n == 0) return 0;

    IfSessionTab *tabs = (IfSessionTab *)if_arena_alloc(a, sizeof(IfSessionTab) * IF_TABS_MAX);
    i32 n = 0;

    const char *p = txt.p;
    const char *end = txt.p + txt.n;
    /* 1 行目: マジック */
    const char *nl = memchr(p, '\n', (u64)(end - p));
    if (!nl) return 0;
    if ((u64)(nl - p) != 15 || memcmp(p, "ifuto-session 1", 15) != 0) return 0;
    p = nl + 1;

    while (p < end) {
        nl = memchr(p, '\n', (u64)(end - p));
        u32 nlen = nl ? (u32)(nl - p) : (u32)(end - p);
        /* 行を arena に終端コピー（行文字列は以後の参照をここから得る） */
        char *line = (char *)if_arena_alloc(a, (u64)nlen + 1);
        memcpy(line, p, nlen);
        line[nlen] = 0;
        p = nl ? nl + 1 : end;
        if (nlen == 0) continue;
        if (strcmp(line, "end") == 0)
            break; /* 構造的停止点。残りは seek 不能エリアとして捨てる */

        const char *rest = NULL;
        i32 id;
        if ((id = pre_id(line, "url", &rest)) >= 0) {
            if (n < IF_TABS_MAX) {
                IfSessionTab *t = &tabs[n++];
                t->id = id;
                t->scroll = 0;
                t->title = NULL;
                t->group = NULL;
                t->url = (char *)rest; /* arena 内バッファ（行コピー）を直接指す */
            }
        } else if ((id = pre_id(line, "title", &rest)) >= 0) {
            IfSessionTab *t = by_id(tabs, n, id);
            if (t) t->title = (char *)rest;
        } else if ((id = pre_id(line, "group", &rest)) >= 0) {
            IfSessionTab *t = by_id(tabs, n, id);
            if (t) t->group = (char *)rest;
        } else if ((id = pre_id(line, "scroll", &rest)) >= 0) {
            IfSessionTab *t = by_id(tabs, n, id);
            if (t) {
                long v = strtol(rest, NULL, 10);
                if (v < 0) v = 0;
                if (v > (1L << 24)) v = 1L << 24;
                t->scroll = (i32)v;
            }
        } else if (strncmp(line, "active ", 7) == 0) {
            long v = strtol(line + 7, NULL, 10);
            if (v > 0 && v <= 1000000) *active_id = (i32)v;
        }
        /* 未知行は黙殺（前向き互換で未来版行を spurious に読み殺す） */
    }
    if (n == 0) return 0;
    *tabs_out = tabs;
    return n;
}

/* ---- history ---- */

bool if_store_history_add(IfStore *s, i64 now, const char *title, const char *url) {
    if (!s->enabled || !url || url[0] == 0) return false;
    char path[IF_STORE_DIR_CAP + 64];
    if_store_path(s, IF_STORE_HIST_NAME, path, sizeof path);

    /* 32 回に 1 度、容量点検→超過なら後半を残して縮退（tmp→rename） */
    bool ok = true;
    if (++s->hist_appends % 32 == 0) {
        IfArena a;
        if_arena_init(&a, 1 << 20);
        IfStr txt = s->fs->read_file(&a, path, s->fs->ctx);
        if (txt.p && txt.n > IF_HISTORY_MAX_BYTES) {
            u64 keep = txt.n - IF_HISTORY_MAX_BYTES / 2;
            const char *cut = (const char *)memchr(txt.p + keep, '\n', txt.n - keep);
            if (cut) {
                const char *tail = cut + 1;
                u64 tn = (u64)(txt.p + txt.n - tail);
                ok = s->fs->write_file(path, tail, tn, s->fs->ctx);
            }
        }
        if_arena_destroy(&a);
    }

    GenBuf g;
    IfArena ta;
    if_arena_init(&ta, 1 << 12);
    gb_init(&g, &ta, 1 << 10);
    char ts[32];
    snprintf(ts, sizeof ts, "%lld", (long long)now);
    gb_lit(&g, ts);
    gb_lit(&g, "\t");
    gb_safe(&g, title ? title : "", IF_TITLE_CAP);
    gb_lit(&g, "\t");
    gb_safe(&g, url, IF_URL_CAP);
    gb_lit(&g, "\n");
    bool ok2 = s->fs->append(path, g.p, g.n, s->fs->ctx);
    if_arena_destroy(&ta);
    return ok && ok2;
}

/* ---- bookmarks ---- */

/* 行 "<title>\t<url>\n" の URL 部だけ比較（title 側に \t は保存時に殺してある） */
static bool line_url_eq(const char *line, const char *url, u32 linelen) {
    const char *tab = memchr(line, '\t', linelen);
    if (!tab) return false;
    const char *u = tab + 1;
    u32 un = linelen - (u32)(u - line);
    if (u[un - 1] == '\n') un--;
    return strlen(url) == un && memcmp(u, url, un) == 0;
}

static bool bmrk_write_filtered(const IfStore *s, const char *txt, u32 n,
                                const char *remove_url, const char *add_title,
                                const char *add_url) {
    char path[IF_STORE_DIR_CAP + 64];
    if_store_path(s, IF_STORE_BMRK_NAME, path, sizeof path);
    IfArena a;
    if_arena_init(&a, 1 << 16);
    GenBuf g;
    gb_init(&g, &a, n + 256);
    const char *p = txt;
    const char *end = txt + n;
    while (p < end) {
        const char *nl = memchr(p, '\n', (u64)(end - p));
        u32 nlen = nl ? (u32)(nl - p) + 1 : (u32)(end - p);
        if (!remove_url || !line_url_eq(p, remove_url, nlen))
            gb_raw(&g, p, nlen);
        p += nlen;
    }
    if (add_url) {
        gb_safe(&g, add_title ? add_title : "", IF_TITLE_CAP);
        gb_lit(&g, "\t");
        gb_safe(&g, add_url, IF_URL_CAP);
        gb_lit(&g, "\n");
    }
    bool ok = s->fs->write_file(path, g.p, g.n, s->fs->ctx);
    if_arena_destroy(&a);
    return ok;
}

bool if_store_bookmark_toggle(IfStore *s, const char *title, const char *url, bool *added) {
    if (!s->enabled || !url || url[0] == 0) return false;
    char path[IF_STORE_DIR_CAP + 64];
    if_store_path(s, IF_STORE_BMRK_NAME, path, sizeof path);
    IfArena a;
    if_arena_init(&a, 1 << 16);
    IfStr txt = s->fs->read_file(&a, path, s->fs->ctx);
    bool present = false;
    if (txt.p) {
        const char *p = txt.p;
        const char *end = txt.p + txt.n;
        while (p < end) {
            const char *nl = memchr(p, '\n', (u64)(end - p));
            u32 nlen = nl ? (u32)(nl - p) + 1 : (u32)(end - p);
            if (line_url_eq(p, url, nlen)) { present = true; break; }
            p += nlen;
        }
    }
    bool ok;
    if (present) {
        ok = bmrk_write_filtered(s, txt.p ? txt.p : "", txt.p ? txt.n : 0,
                                 url, NULL, NULL);
        *added = false;
    } else {
        ok = bmrk_write_filtered(s, txt.p ? txt.p : "", txt.p ? txt.n : 0,
                                 NULL, title, url);
        *added = true;
    }
    if_arena_destroy(&a);
    return ok;
}

i32 if_store_bookmarks_list(const IfStore *s, IfArena *a,
                            IfStr *titles, IfStr *urls, i32 max) {
    if (!s->enabled || max <= 0) return 0;
    char path[IF_STORE_DIR_CAP + 64];
    if_store_path(s, IF_STORE_BMRK_NAME, path, sizeof path);
    IfStr txt = s->fs->read_file(a, path, s->fs->ctx);
    if (!txt.p || txt.n == 0) return 0;
    i32 n = 0;
    const char *p = txt.p;
    const char *end = txt.p + txt.n;
    while (p < end && n < max) {
        const char *nl = memchr(p, '\n', (u64)(end - p));
        u32 nlen = nl ? (u32)(nl - p) : (u32)(end - p);
        const char *tab = memchr(p, '\t', nlen);
        if (tab) {
            titles[n] = if_str((char *)p, (u32)(tab - p));
            urls[n] = if_str((char *)tab + 1, (u32)(nlen - (tab - p) - 1));
            n++;
        }
        p = nl ? nl + 1 : end;
    }
    return n;
}
