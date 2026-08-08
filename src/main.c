/* Ifuto Browser — CLI フロントエンド v0.1
 * 使い方: ifuto [--width N] [--no-ansi] [--no-style] [--dump-dom|--dump-layout|--dump-tokens]
 *               [--stats] FILE | http://URL | -
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime */
#include "common.h"
#include "arena.h"
#include "dom.h"
#include "md.h"
#include "script.h" /* v0.3: <script> akl 実行（正本 docs/SCRIPTING.md） */
#include "css.h"
#include "layout.h"
#include "render.h"
#include "gui/gui.h"
#include "chrome.h"
#include "ext.h"
#include "net.h"
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include "html_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static IfStr read_all(IfArena *a, const char *path) {
    /* http:// はネットワーク取得（v0.3）。以後の pipeline はファイル入力と同一 */
    if (strncmp(path, "http://", 7) == 0) {
        IfStr body;
        u32 status = 0;
        const char *err = NULL;
        if (!if_http_get(a, path, &body, &status, &err)) {
            fprintf(stderr, "ifuto: cannot fetch %s: %s\n", path, err);
            exit(1);
        }
        (void)status; /* 404 等でもボディを処理（404 ページ描画は正常動作） */
        return body;
    }
    /* まず mmap を試す: 入力のユーザランド複製を避け、読み切り clean page は
     * カーネルがメモリ逼迫時に退避できる（巨大文書の RSS 削減の構造的一歩。
     * ゼロコピー設計の tokenizer/DOM 参照がそのまま mmap 上に乗る）。
     * stdin や特殊ファイルでは fread 経路にフォールバック（同一インターフェース）。 */
    if (strcmp(path, "-") != 0) {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            struct stat st;
            if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
                if ((unsigned long long)st.st_size > IF_MAX_INPUT_BYTES) {
                    fprintf(stderr, "ifuto: input too large\n"); close(fd); exit(1);
                }
                void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
                close(fd);
                if (m != MAP_FAILED) return if_str((const char *)m, (u32)st.st_size);
            } else {
                close(fd);
            }
        }
        /* fopen が NULL なら下の fread 経路がエラー報告する */
    }
    FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
    if (!f) { fprintf(stderr, "ifuto: cannot open %s\n", path); exit(1); }
    u64 cap = 1 << 16, n = 0;
    u8 *buf = (u8 *)if_arena_alloc(a, cap);
    for (;;) {
        if (n > IF_MAX_INPUT_BYTES) { fprintf(stderr, "ifuto: input too large\n"); exit(1); }
        size_t got = fread(buf + n, 1, cap - n > 65536 ? 65536 : cap - n, f);
        n += got;
        if (got == 0) break;
        if (n >= cap) {
            u64 ncap = cap * 2;
            u8 *nb = (u8 *)if_arena_alloc(a, ncap);
            memcpy(nb, buf, cap);
            buf = nb; cap = ncap;
        }
    }
    if (ferror(f)) { fprintf(stderr, "ifuto: read error on %s\n", path); exit(1); }
    if (f != stdin) fclose(f);
    return if_str((const char *)buf, (u32)n);
}

static void usage(FILE *f) {
    fputs("ifuto v0.1 — the strongest lightweight browser (core slice)\n"
          "usage: ifuto [options] FILE | http://URL | -\n"
          "  --width N        viewport cell width (default 100)\n"
          "  --no-ansi        plain text output (no SGR colors)\n"
          "  --no-style       skip stylesheet application\n"
          "  --dump-dom       print DOM tree\n"
          "  --dump-layout    print box tree\n"
          "  --dump-tokens    print HTML tokens\n"
          "  --dump-wptdom    print DOM in html5lib tree-construction format\n"
          "  --fragment CTX   parse HTML fragment with context CTX (\"body\" / \"svg path\"...)\n"
          "  --dump-styles    print computed styles per element (devtools)\n"
          "  --gui            interactive GUI (single supported UI; TUI は廃止)\n"
          "  --shot OUT.ppm   headless full-page raster to PPM (GUI 検証経路)\n"
          "  --md             force Markdown parsing (auto for .md/.markdown files)\n"
          "  --slim-dom       drop display-irrelevant subtrees (script/template) from DOM\n"
          "  --links          print collected links\n"
          "  --stats          print timing/memory stats to stderr\n"
          "  --ext DIR        load extensions from DIR at chrome init (GUI/--shot。E1)\n"
          "  --show-paths     list persisted-data paths (INV-9; no side effects)\n", f);
}

/* INV-9: 永続データの発見可能なパス一覧。--show-paths は副作用ゼロ（mkdir しない） */
static int show_paths(void) {
    IfFsOps fs = { if_fs_exists_real, if_fs_read_real, NULL,
                   if_fs_write_real, if_fs_append_real, if_fs_mkpath_real };
    IfStore s;
    if (!if_store_init(&s, &fs, /*create=*/false)) {
        fputs("ifuto: no data dir (IFUTO_HOME / XDG_DATA_HOME / HOME are unset)\n", stdout);
        return 0;
    }
    const char *names[] = { IF_STORE_SESS_NAME, IF_STORE_HIST_NAME, IF_STORE_BMRK_NAME };
    printf("data dir: %s\n", s.dir);
    char p[IF_STORE_DIR_CAP + 64];
    for (u32 i = 0; i < 3; i++) {
        if_store_path(&s, names[i], p, sizeof p);
        struct stat st;
        if (stat(p, &st) == 0)
            printf("  %-14s %s (%lld bytes)\n", names[i], p, (long long)st.st_size);
        else
            printf("  %-14s %s (absent)\n", names[i], p);
    }
    return 0;
}

int main(int argc, char **argv) {
    i32 width = 100;
    int ansi = 1, do_style = 1, links = 0, stats = 0, force_md = 0;
    enum { M_RENDER, M_DOM, M_LAYOUT, M_TOKENS, M_WPTDOM, M_STYLES, M_GUI } mode = M_RENDER;
    const char *path = NULL, *shot = NULL, *frag_ctx = NULL;
    bool legacy_ui = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) width = atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-ansi") == 0) ansi = 0;
        else if (strcmp(argv[i], "--no-style") == 0) do_style = 0;
        else if (strcmp(argv[i], "--dump-dom") == 0) mode = M_DOM;
        else if (strcmp(argv[i], "--dump-layout") == 0) mode = M_LAYOUT;
        else if (strcmp(argv[i], "--dump-tokens") == 0) mode = M_TOKENS;
        else if (strcmp(argv[i], "--dump-wptdom") == 0) mode = M_WPTDOM;
        else if (strcmp(argv[i], "--fragment") == 0 && i + 1 < argc) frag_ctx = argv[++i];
        else if (strcmp(argv[i], "--dump-styles") == 0) mode = M_STYLES;
        else if (strcmp(argv[i], "--gui") == 0) mode = M_GUI;
        else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (strcmp(argv[i], "--ui") == 0) { mode = M_GUI; legacy_ui = true; }
        else if (strcmp(argv[i], "--links") == 0) links = 1;
        else if (strcmp(argv[i], "--stats") == 0) stats = 1;
        else if (strcmp(argv[i], "--md") == 0) force_md = 1;
        else if (strcmp(argv[i], "--slim-dom") == 0) if_dom_slim = true;
        else if (strcmp(argv[i], "--ext") == 0 && i + 1 < argc) if_ext_set_dir(argv[++i]);
        else if (strcmp(argv[i], "--show-paths") == 0) return show_paths();
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { usage(stdout); return 0; }
        else if (argv[i][0] == '-' && argv[i][1] != 0) { usage(stderr); return 2; }
        else path = argv[i];
    }
    if (!path && mode != M_GUI && !shot) { usage(stderr); return 2; }
    if (legacy_ui)
        fputs("ifuto: --ui(TUI) は完全廃止。GUI（--gui）へ移行しました\n", stderr);
    if (shot) return if_gui_shot(path, shot);
    if (mode == M_GUI) return if_gui_run(path);
    if (width < 4 || width > 100000) { fprintf(stderr, "ifuto: bad --width\n"); return 2; }

    double t0 = now_ms();
    IfArena a;
    if_arena_init(&a, 1 << 23) /* ブロック数半減で THP 同期コンパクション stall 削減（paired +2.6ms。reserved +~7MB と引き換え、BENCH 台帳） */; /* CLI: 4MB ブロック→THP 直取り（マイナーフォールト税の構造除去。GUI/テストは従来のまま） */
    IfStr input = read_all(&a, path);

    double t1 = now_ms();
    /* v0.2: Markdown（+GFM 表/脚注。表示テキストは MD 以上の情報密度を持つ方針）
     * は HTML に前段変換してから単一の WHATWG パーサへ（多層防御） */
    IfDom *dom = NULL;
    if (frag_ctx) {
        /* fragment（innerHTML 相当）解析は観測モード専用（WHATWG 13.4） */
        if (mode != M_WPTDOM && mode != M_DOM) {
            fprintf(stderr, "ifuto: --fragment は --dump-wptdom / --dump-dom 専用\n");
            return 2;
        }
        dom = if_parse_html_fragment(&a, input, frag_ctx);
    } else if (force_md || if_path_is_md(path)) {
        /* v0.3: md は DOM 直構築を先に試す（HTML 往復を消す高速経路）。
         * dump-tokens / wptdom は「HTML 段」の観測点なので従来どおり 2 段で。
         * taint 観測時は従来経路へフォールバック（正しさは本パーサに集約） */
        if (mode == M_TOKENS || mode == M_WPTDOM || getenv("IFUTO_MD_SLOW") ||
            !if_md_parse_fast_f(&a, input, &dom, (mode == M_RENDER) ? IF_MD_F_SLIM_ATTRS : 0)) {
            IfStr md_html;
            if_md_to_html(&a, input, &md_html);
            input = md_html;
            dom = if_parse_html(&a, input);
        }
    } else {
        dom = if_parse_html(&a, input);
    }
    double t2 = now_ms();

    if (mode == M_WPTDOM) {
        if (frag_ctx) if_dom_serialize_wpt_frag(dom, stdout);
        else if_dom_serialize_wpt(dom, stdout);
        if_arena_destroy(&a);
        return 0;
    }
    if (mode == M_TOKENS) {
        IfArena ta;
        if_arena_init(&ta, 1 << 16);
        IfHtmlTok t;
        if_tok_init(&t, &ta, input);
        for (;;) {
            IfTok tok = if_tok_next(&t);
            if (tok.kind == TOK_EOF) break;
            const char *k = tok.kind == TOK_TEXT ? "TEXT" : tok.kind == TOK_START ? "START"
                          : tok.kind == TOK_END ? "END" : tok.kind == TOK_COMMENT ? "COMMENT" : "DOCTYPE";
            printf("%-8s %-12.*s", k, (int)tok.tag_raw.n, tok.tag_raw.p ? tok.tag_raw.p : "");
            if (tok.kind == TOK_TEXT) printf(" \"%.*s\"", (int)(tok.text.n > 48 ? 48 : tok.text.n), tok.text.p);
            for (u32 i = 0; i < tok.n_attrs; i++)
                printf(" %.*s=\"%.*s\"", (int)tok.attrs[i].name.n, tok.attrs[i].name.p,
                       (int)(tok.attrs[i].value.n > 32 ? 32 : tok.attrs[i].value.n), tok.attrs[i].value.p);
            printf("\n");
        }
        if_arena_destroy(&ta);
        if_arena_destroy(&a);
        return 0;
    }

    if (mode == M_DOM) {
        if_dom_dump(dom, stdout);
        if_arena_destroy(&a);
        return 0;
    }

    double arena_after_parse = (double)if_arena_reserved(&a);

    /* v0.3: <script> akl 実行（凍結正本: docs/SCRIPTING.md）。
     * dump 系モード（wptdom/tokens/dom）は上流で return 済み = 文字列観測系オラクル不変。
     * 本家順序に従い style 適用前（DOM 変更が style/layout/render へ反映される）。
     * 計測は既存 5 段を汚さない独立枠（script 実行時のみ stats に追加行）。
     * script RT は if_script_run 内で必ず破棄される → DOM arena 解体より先死ぬ
     * （HANDLE ptr 規約の構造保証。akl/akl.h の AklHandleVTab 注記参照）。 */
    double tscr0 = now_ms();
    IfScriptReport srep = if_script_run(&a, dom, stderr);
    double script_ms = now_ms() - tscr0;

    /* lazy style: md fast-DOM × CLI 行スイープでは style 全面走査を消し、
     * layout の DFS 訪問時に必要箇所だけ解決する（解決値は if_style_apply と同値。
     * 詳細は css.h の IfStyleLazy 注釈。--no-style / dump / GUI は従来経路） */
    bool style_lazy = false;
    if (do_style) {
        style_lazy = (mode == M_RENDER) && if_md_style_lazy_ok(dom);
        if (!style_lazy) if_style_apply(&a, dom);
    }
    double t3 = now_ms();
    double arena_after_style = (double)if_arena_reserved(&a);
    /* style 段の正直な帰属: script 実行（style 適用前の本家順序）を差引く。
     * script 非含有文書は has_script 早期リターンで script_ms≈0 = 既存計測不変 */
    double style_ms = (t3 - t2) - script_ms;

    /* devtools 観測点: style 適用直後の computed style を全要素ダンプして終了
     * （layout 以降へは進まない。style_lazy は M_RENDER 専用なので本経路は常に
     * eager 適用済み。--no-style 併用時はカスケード未実行の正直な姿が出る） */
    if (mode == M_STYLES) {
        if_style_dump(dom, stdout);
        if_arena_destroy(&a);
        return 0;
    }

    /* CLI 行スイープは box 木を参照しない → 線形モード（BLOCK 箱再利用）。dump-layout は従来経路 */
    IfLayout *lay = (mode == M_LAYOUT) ? if_layout_build(&a, dom, width)
                                       : if_layout_build_linear(&a, dom, width, (u8)style_lazy);
    if (mode == M_LAYOUT) {
        if_layout_dump(lay, stdout);
        if_arena_destroy(&a);
        return 0;
    }
    double t4 = now_ms();
    double arena_after_layout = (double)if_arena_reserved(&a);

    /* v0.3: 行スイープ直接発行（グリッド全面充填を消す。op が触るセルだけを再利用
     * 行バッファに構成する。発行バイト列は従来と完全一致 = tests/test_layout.c の
     * 差分オラクルが機械固定） */
    double tg0 = now_ms();
    if_render_emit_rows_sweep(stdout, lay, ansi);
    double acc_emit = now_ms() - tg0, acc_grid = 0.0;
    double t5 = now_ms();
    i32 mx = 0, my = 0;
    if_render_extent(lay, &mx, &my);

    if (links && lay->n_links) {
        printf("\nリンク:\n");
        for (u32 i = 0; i < lay->n_links; i++)
            printf("[%u] %.*s\n", lay->links[i].n, (int)lay->links[i].href.n, lay->links[i].href.p);
    }

    if (stats) {
        /* ピーク RSS は自己報告（/proc/self/status の VmHWM を終了直前に読む。
         * 外部ポーリングは短命プロセスで取り逃がす。ru_maxrss は環境によって壞れている） */
        u64 vwhwm_kb = 0;
        FILE *st = fopen("/proc/self/status", "r");
        if (st) {
            char line[256];
            while (fgets(line, sizeof line, st))
                if (sscanf(line, "VmHWM: %llu kB", (unsigned long long *)&vwhwm_kb) == 1) break;
            fclose(st);
        }
        if (srep.n_run)
            fprintf(stderr, "ifuto stats: scripts=%u errors=%u skipped=%u script_ms=%.2f\n",
                    srep.n_run, srep.n_errors, srep.n_skipped, script_ms);
        fprintf(stderr,
            "ifuto stats: read=%.2fms parse=%.2fms style=%.2fms layout=%.2fms render=%.2fms total=%.2fms\n"
            "  nodes=%u parse_errors=%u grid=%dx%d links=%u peak_rss_kb=%llu\n"
            "  render_split: grid=%.2fms emit=%.2fms\n"
            "  arena_kb: parse=%.1f style=%.1f layout=%.1f render=%.1f\n",
            t1 - t0, t2 - t1, style_ms, t4 - t3, t5 - t4, t5 - t0,
            dom->n_nodes, dom->n_errors, mx, my, lay->n_links,
            (unsigned long long)vwhwm_kb,
            acc_grid, acc_emit,
            arena_after_parse / 1024.0, arena_after_style / 1024.0,
            arena_after_layout / 1024.0, (double)if_arena_reserved(&a) / 1024.0);
    }
    if_arena_destroy(&a);
    return 0;
}
