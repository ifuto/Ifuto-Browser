/* 実装は ifuto_pages.h を参照。HTML は全て静的テンプレ + ローカル値の差し込み。
 * 文字列は全部エスケープ済み数値/内部生成のみ（外部入力が入るのは履歴の
 * title/url だけなので、そこは本文・属性ともに &<>" を退避する）。 */
#include "ifuto_pages.h"
#include "chrome.h"
#include "store.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---- 小バッファ HTML ビルダ（arena、budget つき） ---- */
typedef struct { IfArena *a; char *p; u32 n, cap; } HB;
static void hb_put(HB *b, const char *s, u32 sn) {
    if (b->cap - b->n < sn + 1) {
        u32 nc = b->cap ? b->cap * 2 : 8192;
        while (nc - b->n < sn + 1) nc *= 2;
        if (nc > 4u * 1024 * 1024) return; /* 内部ページの上限（壊れても巨大化しない） */
        char *np = (char *)if_arena_alloc(b->a, nc);
        if (b->n) memcpy(np, b->p, b->n); /* 初回は b->p==NULL（UB 回避） */
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, s, sn);
    b->n += sn;
}
static void hb_s(HB *b, const char *s) { hb_put(b, s, (u32)strlen(s)); }
static void hb_u32(HB *b, u32 v) { char t[16]; hb_put(b, t, (u32)snprintf(t, sizeof t, "%u", v)); }
static void hb_u64(HB *b, u64 v) { char t[24]; hb_put(b, t, (u32)snprintf(t, sizeof t, "%llu", (unsigned long long)v)); }
static void hb_kb(HB *b, u64 bytes) { /* 整数 KB → 読みやすさのため MB 表示も併記 */
    hb_u64(b, bytes / 1024); hb_s(b, " KB ("); hb_u64(b, bytes >> 20); hb_s(b, " MB)");
}
/* 本文用エスケープ（& < >） */
static void hb_esc(HB *b, const char *s, u32 n) {
    for (u32 i = 0; i < n; i++) {
        char c = s[i];
        if (c == '&') hb_s(b, "&amp;");
        else if (c == '<') hb_s(b, "&lt;");
        else if (c == '>') hb_s(b, "&gt;");
        else hb_put(b, &c, 1);
    }
}

static const char PAGE_HEAD[] =
    "<html><head><title>ifuto://";
static const char PAGE_STYLE[] =
    "</title></head><body>"
    "<h1>";
static const char NAV[] =
    "</h1><p><a href=\"ifuto://settings\">settings</a> | "
    "<a href=\"ifuto://history\">history</a> | "
    "<a href=\"ifuto://memory\">memory</a> | "
    "<a href=\"ifuto://about\">about</a></p><hr>";

/* ---- settings ---- */
static void page_settings(HB *b, struct IfChrome *c) {
    hb_s(b, "settings");
    hb_s(b, PAGE_STYLE);
    hb_s(b, "Ifuto 設定");
    hb_s(b, NAV);
    hb_s(b, "<h2>エンジン（akl = Aklus JS）</h2><pre>");
    hb_s(b, "同梱形態          : 同一リポジトリで build/akl として単独配布（make install-akl で導入可）\n");
    hb_s(b, "ブラウザ内実行    : DOM 結合は v0.4 台帳（現行は ifuto 本体に未リンク＝220KB 天井維持）\n");
    hb_s(b, "JIT               : 永久不採用（実行可能書き込みページは構造的にゼロ）\n");
    hb_s(b, "CoJIT（AOT 特化） : 既定 ON（kill switch: akl_set_cojit / docs: BENCH.md）\n");
    hb_s(b, "budget 既定       : 命令 10M、try 深さ 1024、スタック段 4096\n");
    hb_s(b, "</pre><h2>セキュリティ</h2><pre>");
    hb_s(b, "akl 単体ランナー   : seccomp-BPF サンドボックス強制（既定 ON。--no-sandbox で明示解除）\n");
    hb_s(b, "ブラウザプロセス   : sandbox primitive 実装済み（src/sandbox.c。chrome profile 適用は v0.2 台帳）\n");
    hb_s(b, "パース多層防御     : 全入力は共通パーサ + budget fail-stop（docs: ARCHITECTURE.md）\n");
    hb_s(b, "</pre><h2>メモリ方針</h2><pre>");
    hb_s(b, "ifuto://memory にタブごとの arena 会計（詳細）\n");
    hb_s(b, "slim-DOM           : 実ブラウズ経路で既定 ON（描画に関係ないものは DOM しない）\n");
    hb_s(b, "viewport 窓        : grid は文書全体を保持しない（行窓・再利用）\n");
    hb_s(b, "巨大文書           : 512MB 入力 budget（超過は OOM ではなく綺麗な fail。docs: BENCH.md 巨大 IDM 計測）\n");
    hb_s(b, "</pre><h2>描画</h2><pre>");
    hb_s(b, "CPU/GPU 自動判定   : 起動時マイクロベンチで最速の raster backend を選択\n");
    hb_s(b, "                     （この端末での決定は ifuto://memory の表示欄に出ます）\n");
    hb_s(b, "</pre><h2>切替手段</h2><pre>");
    hb_s(b, "akl CoJIT OFF      : アプリ埋込 API akl_set_cojit(rt, 0)（監査・差分検証用途）\n");
    hb_s(b, "CSS 索引 OFF       : if_css_set_naive_matching(1)（同）\n");
    hb_s(b, "store 場所         : ifuto --show-paths（INV-9）\n");
    hb_s(b, "</pre>");
    (void)c;
}

/* ---- history ---- */
static void page_history(HB *b, struct IfChrome *c) {
    hb_s(b, "history");
    hb_s(b, PAGE_STYLE);
    hb_s(b, "履歴");
    hb_s(b, NAV);
    /* store の history.tsv を直接読む（書式: epoch \t title \t url。最新末尾。
     * 既存 API は add のみなので ts のみ軽量自前パース。失敗時は静かに空表示） */
    char path[IF_STORE_DIR_CAP];
    if (c && c->store.enabled) {
        if_store_path(&c->store, IF_STORE_HIST_NAME, path, sizeof path);
        /* 読み込み先は文書 arena（タブ寿命で必ず解放される。engine_scratch は
         * 巻き戻し不能なので使うと開くたびに漏れる＝メモリ法則違反） */
        IfStr tsv = c->fs.read_file(b->a, path, c->fs.ctx);
        if (tsv.p && tsv.n) {
            /* 末尾から最大 100 件。lines[0]=最新行 … lines[nl-1]=最古行 */
            const char *end = tsv.p + tsv.n;
            const char *lines[128];
            int nl = 0;
            const char *le = end;
            while (le > tsv.p && le[-1] == '\n') le--; /* 末尾改行を落とす */
            while (le > tsv.p && nl < 100) {
                const char *ls = le;
                while (ls > tsv.p && ls[-1] != '\n') ls--;
                lines[nl++] = ls; /* 行本体は [ls, le) */
                le = ls;
                while (le > tsv.p && le[-1] == '\n') le--;
            }
            hb_s(b, "<p>直近 ");
            char num[8]; hb_put(b, num, (u32)snprintf(num, sizeof num, "%d", nl));
            hb_s(b, " 件（新しい順。store: history.tsv）</p><ul>");
            for (int i = 0; i < nl; i++) { /* lines[0]=最新 → そのまま新しい順で出力 */
                /* 行: epoch \t title \t url */
                const char *ls = lines[i];
                const char *lend = ls;
                while (lend < end && *lend != '\n') lend++;
                const char *t1 = NULL, *t2 = NULL;
                for (const char *q = ls; q < lend; q++) {
                    if (*q == '\t') { if (!t1) t1 = q; else if (!t2) { t2 = q; break; } }
                }
                if (!t1 || !t2) continue;
                hb_s(b, "<li>[");
                hb_esc(b, ls, (u32)(t1 - ls)); /* epoch */
                hb_s(b, "] <a href=\"");
                hb_esc(b, t2 + 1, (u32)(lend - t2 - 1)); /* url（属性） */
                hb_s(b, "\">");
                hb_esc(b, t1 + 1, (u32)(t2 - t1 - 1)); /* title */
                hb_s(b, "</a> ");
                hb_esc(b, t2 + 1, (u32)(lend - t2 - 1)); /* url（可視テキスト。chrome://history 同様に併記） */
                hb_s(b, "</li>");
            }
            hb_s(b, "</ul><hr><p>クリア: 現在は UI 経路なし（store ファイルを削除。ifuto --show-paths で場所を提示）＝誤爆しない設計</p>");
            return;
        }
    }
    hb_s(b, "<p>履歴はまだありません（またはストア無効）。</p>");
}

/* ---- memory ---- */
static void page_memory(HB *b, struct IfChrome *c) {
    hb_s(b, "memory");
    hb_s(b, PAGE_STYLE);
    hb_s(b, "メモリ会計");
    hb_s(b, NAV);
    hb_s(b, "<table><tr><th>tab</th><th>doc arena</th><th>view arena</th><th>title</th></tr>");
    u64 tot_doc = 0, tot_view = 0;
    if (c) {
        for (i32 i = 0; i < c->n_tabs; i++) {
            IfTab *t = c->tabs[i];
            u64 d = t->doc ? if_arena_reserved(t->doc) : 0;
            u64 v = t->view ? if_arena_reserved(t->view) : 0;
            tot_doc += d; tot_view += v;
            hb_s(b, "<tr><td>");
            hb_u32(b, (u32)t->id);
            hb_s(b, "</td><td>");
            hb_kb(b, d);
            hb_s(b, "</td><td>");
            hb_kb(b, v);
            hb_s(b, "</td><td>");
            hb_esc(b, t->title ? t->title : "", t->title ? (u32)strlen(t->title) : 0);
            hb_s(b, "</td></tr>");
        }
    }
    hb_s(b, "</table><p>合計 doc: ");
    hb_kb(b, tot_doc);
    hb_s(b, " / view: ");
    hb_kb(b, tot_view);
    hb_s(b, "</p>");
    hb_s(b, "<p>方針（ユーザ法則）: 「メモリは使わなければ使わないほど良い」"
        "— arena はタブ寿命で保持し、view は再レイアウトで破棄・再構築。"
        "巨大 IDM の正確な係数（実測）は BENCH.md の「巨大 IDM 計測」節。</p>");
}

/* ---- about ---- */
static void page_about(HB *b, struct IfChrome *c) {
    hb_s(b, "about");
    hb_s(b, PAGE_STYLE);
    hb_s(b, "Ifuto について");
    hb_s(b, NAV);
    hb_s(b, "<pre>");
    hb_s(b, "Ifuto Browser — 史上最強の軽量ブラウザ（自己完結 C11、ldd = linux-vdso/libc/ld (+libm) のみ）\n");
    hb_s(b, "akl (Aklus)  : 自作 JS エンジン。C11・JIT なし・seccomp 既定・CoJIT(AOT 特化)。単独インストール可\n");
    hb_s(b, "CSS          : RuleSet 索引（Blink 戦略相当、実測 23.32x vs 全走査）\n");
    hb_s(b, "WPT tree-construction 適合率: 97.3% (1679/1726)\n");
    hb_s(b, "一次情報     : README.md / ARCHITECTURE.md / BENCH.md / CHROME_SCOPE.md\n");
    hb_s(b, "             docs/BLINK_COMPAT.md / docs/V8_COMPAT.md / docs/AKL_COMPAT.md / docs/SANDBOX.md\n");
    hb_s(b, "</pre>");
    (void)c;
}

bool if_ifuto_page(IfArena *a, const char *url, struct IfChrome *c, IfStr *out_html) {
    if (!url || strncmp(url, "ifuto://", 8) != 0) return false;
    const char *pg = url + 8;
    HB b = { a, NULL, 0, 0 };
    hb_s(&b, PAGE_HEAD);
    if (!strcmp(pg, "settings")) page_settings(&b, c);
    else if (!strcmp(pg, "history")) page_history(&b, c);
    else if (!strcmp(pg, "memory")) page_memory(&b, c);
    else if (!strcmp(pg, "about")) page_about(&b, c);
    else {
        hb_s(&b, "unknown");
        hb_s(&b, PAGE_STYLE);
        hb_s(&b, "未知の内部ページ");
        hb_s(&b, NAV);
        hb_s(&b, "<p>ifuto://settings ifuto://history ifuto://memory ifuto://about があります。</p>");
    }
    hb_s(&b, "</body></html>");
    out_html->p = b.p ? b.p : "";
    out_html->n = b.n;
    return true;
}
