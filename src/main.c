/* Ifuto Browser — CLI フロントエンド v0.1
 * 使い方: ifuto [--width N] [--no-ansi] [--no-style] [--dump-dom|--dump-layout|--dump-tokens]
 *               [--stats] FILE | -
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime */
#include "common.h"
#include "arena.h"
#include "dom.h"
#include "css.h"
#include "layout.h"
#include "render.h"
#include "tui.h"
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
          "  --ui             interactive TUI (tabs, omnibox; needs a tty)\n"
          "  --links          print collected links\n"
          "  --stats          print timing/memory stats to stderr\n", f);
}

int main(int argc, char **argv) {
    i32 width = 100;
    int ansi = 1, do_style = 1, links = 0, stats = 0;
    enum { M_RENDER, M_DOM, M_LAYOUT, M_TOKENS, M_WPTDOM, M_UI } mode = M_RENDER;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) width = atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-ansi") == 0) ansi = 0;
        else if (strcmp(argv[i], "--no-style") == 0) do_style = 0;
        else if (strcmp(argv[i], "--dump-dom") == 0) mode = M_DOM;
        else if (strcmp(argv[i], "--dump-layout") == 0) mode = M_LAYOUT;
        else if (strcmp(argv[i], "--dump-tokens") == 0) mode = M_TOKENS;
        else if (strcmp(argv[i], "--dump-wptdom") == 0) mode = M_WPTDOM;
        else if (strcmp(argv[i], "--ui") == 0) mode = M_UI;
        else if (strcmp(argv[i], "--links") == 0) links = 1;
        else if (strcmp(argv[i], "--stats") == 0) stats = 1;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { usage(stdout); return 0; }
        else if (argv[i][0] == '-' && argv[i][1] != 0) { usage(stderr); return 2; }
        else path = argv[i];
    }
    if (!path && mode != M_UI) { usage(stderr); return 2; }
    if (mode == M_UI) return if_tui_run(path);
    if (width < 4 || width > 100000) { fprintf(stderr, "ifuto: bad --width\n"); return 2; }

    double t0 = now_ms();
    IfArena a;
    if_arena_init(&a, 1 << 18);
    IfStr input = read_all(&a, path);

    double t1 = now_ms();
    IfDom *dom = if_parse_html(&a, input);
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

    IfGrid *grid = if_render_grid(&a, lay);
    IfStr out = if_render_emit(&a, grid, ansi);
    double t5 = now_ms();

    fwrite(out.p, 1, out.n, stdout);

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
            "  arena_kb: parse=%.1f style=%.1f layout=%.1f render=%.1f\n",
            t1 - t0, t2 - t1, t3 - t2, t4 - t3, t5 - t4, t5 - t0,
            dom->n_nodes, dom->n_errors, grid->w, grid->h, lay->n_links,
            (unsigned long long)vwhwm_kb,
            arena_after_parse / 1024.0, arena_after_style / 1024.0,
            arena_after_layout / 1024.0, (double)if_arena_reserved(&a) / 1024.0);
    }
    if_arena_destroy(&a);
    return 0;
}
