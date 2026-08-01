/* Ifuto Browser — CLI フロントエンド v0.1
 * 使い方: ifuto [--width N] [--no-ansi] [--no-style] [--dump-dom|--dump-layout|--dump-tokens]
 *               [--stats] FILE | -
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime */
#include "common.h"
#include "arena.h"
#include "dom.h"
#include "md.h"
#include "css.h"
#include "layout.h"
#include "render.h"
#include "gui/gui.h"
#include "chrome.h"
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
          "usage: ifuto [options] FILE | -\n"
          "  --width N        viewport cell width (default 100)\n"
          "  --no-ansi        plain text output (no SGR colors)\n"
          "  --no-style       skip stylesheet application\n"
          "  --dump-dom       print DOM tree\n"
          "  --dump-layout    print box tree\n"
          "  --dump-tokens    print HTML tokens\n"
          "  --dump-wptdom    print DOM in html5lib tree-construction format\n"
          "  --gui            interactive GUI (single supported UI; TUI は廃止)\n"
          "  --shot OUT.ppm   headless full-page raster to PPM (GUI 検証経路)\n"
          "  --md             force Markdown parsing (auto for .md/.markdown files)\n"
          "  --slim-dom       drop display-irrelevant subtrees (script/template) from DOM\n"
          "  --links          print collected links\n"
          "  --stats          print timing/memory stats to stderr\n"
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
    enum { M_RENDER, M_DOM, M_LAYOUT, M_TOKENS, M_WPTDOM, M_GUI } mode = M_RENDER;
    const char *path = NULL, *shot = NULL;
    bool legacy_ui = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) width = atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-ansi") == 0) ansi = 0;
        else if (strcmp(argv[i], "--no-style") == 0) do_style = 0;
        else if (strcmp(argv[i], "--dump-dom") == 0) mode = M_DOM;
        else if (strcmp(argv[i], "--dump-layout") == 0) mode = M_LAYOUT;
        else if (strcmp(argv[i], "--dump-tokens") == 0) mode = M_TOKENS;
        else if (strcmp(argv[i], "--dump-wptdom") == 0) mode = M_WPTDOM;
        else if (strcmp(argv[i], "--gui") == 0) mode = M_GUI;
        else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (strcmp(argv[i], "--ui") == 0) { mode = M_GUI; legacy_ui = true; }
        else if (strcmp(argv[i], "--links") == 0) links = 1;
        else if (strcmp(argv[i], "--stats") == 0) stats = 1;
        else if (strcmp(argv[i], "--md") == 0) force_md = 1;
        else if (strcmp(argv[i], "--slim-dom") == 0) if_dom_slim = true;
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
    if_arena_init(&a, 1 << 18);
    IfStr input = read_all(&a, path);

    double t1 = now_ms();
    /* v0.2: Markdown（+GFM 表/脚注。表示テキストは MD 以上の情報密度を持つ方針）
     * は HTML に前段変換してから単一の WHATWG パーサへ（多層防御） */
    IfDom *dom = NULL;
    if (force_md || if_path_is_md(path)) {
        /* v0.3: md は DOM 直構築を先に試す（HTML 往復を消す高速経路）。
         * dump-tokens / wptdom は「HTML 段」の観測点なので従来どおり 2 段で。
         * taint 観測時は従来経路へフォールバック（正しさは本パーサに集約） */
        if (mode == M_TOKENS || mode == M_WPTDOM || getenv("IFUTO_MD_SLOW") ||
            !if_md_parse_fast(&a, input, &dom)) {
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
        if_dom_serialize_wpt(dom, stdout);
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
    if (do_style) if_style_apply(&a, dom);
    double t3 = now_ms();
    double arena_after_style = (double)if_arena_reserved(&a);

    IfLayout *lay = if_layout_build(&a, dom, width);
    if (mode == M_LAYOUT) {
        if_layout_dump(lay, stdout);
        if_arena_destroy(&a);
        return 0;
    }
    double t4 = now_ms();
    double arena_after_layout = (double)if_arena_reserved(&a);

    /* 巨大文書はチャンク窓グリッドの再利用で定数メモリ発行する（法則: 画面描画に
     * 関係ないものは保持しない。グリッド・出力バッファ双方の全量保持をやめる）。
     * 窓は 4096 行単位で cells を malloc 一回だけ再利用。発行バイト列は従来と完全一致
     * （差分は tests の tui/gui smoke の sha256 が機械監査） */
    i32 mx = 0, my = 0;
    if_render_extent(lay, &mx, &my);
    IfGrid win;
    win.cells = (IfCell *)malloc((size_t)mx * 4096 * sizeof(IfCell));
    if (!win.cells) if_fatal("render: oom window grid");
    win.y_off = 0;
    /* 厳密増加窓なので走査カーソルを巡航（root 直下の子リスト first-child 再走査を
     * 消す。render O(窓×子) → O(box+描画cell)。後退しない規約のみで使用） */
    IfPaintCursor cur = { 0 };
    double acc_grid = 0, acc_emit = 0;
    for (i32 r0 = 0; r0 < my; r0 += 4096) {
        i32 r1 = r0 + 4096 < my ? r0 + 4096 : my;
        double tg0 = now_ms();
        if_render_grid_rows_into_cur(lay, r0, r1, &win, &cur);
        double tg1 = now_ms();
        if_render_emit_rows(stdout, &win, ansi);
        acc_grid += tg1 - tg0; acc_emit += now_ms() - tg1;
    }
    free(win.cells);
    double t5 = now_ms();

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
        fprintf(stderr,
            "ifuto stats: read=%.2fms parse=%.2fms style=%.2fms layout=%.2fms render=%.2fms total=%.2fms\n"
            "  nodes=%u parse_errors=%u grid=%dx%d links=%u peak_rss_kb=%llu\n"
            "  render_split: grid=%.2fms emit=%.2fms\n"
            "  arena_kb: parse=%.1f style=%.1f layout=%.1f render=%.1f\n",
            t1 - t0, t2 - t1, t3 - t2, t4 - t3, t5 - t4, t5 - t0,
            dom->n_nodes, dom->n_errors, mx, my, lay->n_links,
            (unsigned long long)vwhwm_kb,
            acc_grid, acc_emit,
            arena_after_parse / 1024.0, arena_after_style / 1024.0,
            arena_after_layout / 1024.0, (double)if_arena_reserved(&a) / 1024.0);
    }
    if_arena_destroy(&a);
    return 0;
}
