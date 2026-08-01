/* Ifuto — パイプライン精密プロファイラ（開発用。製品バイナリには非加入）
 * main.c と同じ段を踏み、各段の時間と構造カウントを報告する。 */
#define _POSIX_C_SOURCE 199309L
#include "../src/common.h"
#include "../src/arena.h"
#include "../src/dom.h"
#include "../src/md.h"
#include "../src/css.h"
#include "../src/layout.h"
#include "../src/render.h"
#include "../src/html_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static IfStr read_all(IfArena *a, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open: %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = (char *)if_arena_alloc(a, (u64)n + 1);
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read\n"); exit(2); }
    fclose(f);
    p[n] = 0;
    return if_str(p, (u32)n);
}

int main(int argc, char **argv) {
    const char *path = argv[1];
    i32 width = 100;
    IfArena a;
    if_arena_init(&a, 1 << 18);

    double t0 = now_ms();
    IfStr input = read_all(&a, path);
    double t1 = now_ms();

    IfStr html = input;
    int is_md = if_path_is_md(path);
    if (is_md) if_md_to_html(&a, input, &html);
    double t2 = now_ms();

    /* tokenizer 単体（tree 内 tok との差分で tree 本体を推定） */
    {
        IfArena ta; if_arena_init(&ta, 1 << 16);
        IfHtmlTok t; if_tok_init(&t, &ta, html);
        u64 ntok = 0;
        for (;;) { IfTok tok = if_tok_next(&t); ntok++; if (tok.kind == TOK_EOF) break; }
        fprintf(stderr, "  tok_only: n_tokens=%llu\n", (unsigned long long)ntok);
        if_arena_destroy(&ta);
    }
    double t3 = now_ms();

    IfDom *dom = if_parse_html(&a, html);
    double t4 = now_ms();

    /* DOM 構成カウント */
    u64 n_elem = 0, n_text = 0, n_other = 0, text_bytes = 0;
    {
        IfNode *n = dom->root;
        while (n) {
            if (n->kind == IF_NODE_ELEMENT) n_elem++;
            else if (n->kind == IF_NODE_TEXT) { n_text++; text_bytes += n->u.text.n; }
            else n_other++;
            if (n->first_child) { n = n->first_child; continue; }
            while (n && !n->next_sibling) n = n->parent;
            if (n) n = n->next_sibling;
        }
    }
    fprintf(stderr, "  dom: elem=%llu text=%llu other=%llu text_bytes=%llu (%.1f%% of html %uB)\n",
            (unsigned long long)n_elem, (unsigned long long)n_text, (unsigned long long)n_other,
            (unsigned long long)text_bytes, 100.0 * (double)text_bytes / (double)html.n, html.n);

    if_style_apply(&a, dom);
    double t5 = now_ms();
    fprintf(stderr, "  style unique=%u\n", if_css_intern_last);

    IfLayout *lay = if_layout_build(&a, dom, width);
    double t6 = now_ms();

    /* box 構成カウント */
    u64 n_block = 0, n_line = 0, n_segs = 0, seg_bytes = 0;
    {
        u64 cap = 1 << 16, sp = 0;
        IfBox **stk = (IfBox **)malloc(cap * sizeof(IfBox *));
        stk[sp++] = lay->root;
        /* 深さ優先（子処理のたbornにスタックが最大深さ+末尾兄弟分だけ伸びる） */
        while (sp) {
            IfBox *b = stk[--sp];
            while (b) {
                if (b->kind == IF_BOX_LINE) { n_line++; n_segs += b->n_segs; for (u32 i = 0; i < b->n_segs; i++) seg_bytes += b->segs[i].text.n; }
                else n_block++;
                IfBox *ns = b->next_sibling;
                if (ns) { if (sp == cap) { cap *= 2; stk = (IfBox **)realloc(stk, cap * sizeof(IfBox *)); } stk[sp++] = ns; }
                b = b->first_child;
            }
        }
        free(stk);
    }
    fprintf(stderr, "  layout: block=%llu line=%llu segs=%llu (%.2f/line) seg_bytes=%llu height=%d\n",
            (unsigned long long)n_block, (unsigned long long)n_line, (unsigned long long)n_segs,
            n_line ? (double)n_segs / (double)n_line : 0.0, (unsigned long long)seg_bytes, lay->height);

    /* render: grid と emit を分離（emit は /dev/null へ） */
    FILE *devnull = fopen("/dev/null", "w");
    i32 mx = 0, my = 0;
    if_render_extent(lay, &mx, &my);
    double t7 = now_ms();
    IfGrid win;
    win.cells = (IfCell *)malloc((size_t)mx * 4096 * sizeof(IfCell));
    win.y_off = 0;
    IfPaintCursor cur = { 0 };
    double acc_grid = 0;
    for (i32 r0 = 0; r0 < my; r0 += 4096) {
        i32 r1 = r0 + 4096 < my ? r0 + 4096 : my;
        double g0 = now_ms();
        if_render_grid_rows_into_cur(lay, r0, r1, &win, &cur);
        acc_grid += now_ms() - g0;
    }
    double t8 = now_ms();
    win.y_off = 0; win.h = my; /* 全行を 1 窓相当として再帰発行はできないので emit は窓再計測 */
    /* emit だけ再測: 窓グリッドを再塗りつつ発行（grid 再計算ぶんは t7 と同一なので引く） */
    double acc_emit = 0;
    {
        IfPaintCursor cur2 = { 0 };
        for (i32 r0 = 0; r0 < my; r0 += 4096) {
            i32 r1 = r0 + 4096 < my ? r0 + 4096 : my;
            if_render_grid_rows_into_cur(lay, r0, r1, &win, &cur2);
            double e0 = now_ms();
            if_render_emit_rows(devnull, &win, 0);
            acc_emit += now_ms() - e0;
        }
    }
    double t9 = now_ms();
    fflush(devnull);
    fclose(devnull);

    fprintf(stderr,
        "STAGES read=%.1f md2html=%.1f tok=%.1f parse=%.1f style=%.1f layout=%.1f grid=%.1f emit=%.1f total=%.1f\n",
        t1 - t0, t2 - t1, t3 - t2, t4 - t3, t5 - t4, t6 - t5, acc_grid, acc_emit, t9 - t0);
    if_arena_destroy(&a);
    return 0;
}
