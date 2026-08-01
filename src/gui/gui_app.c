/* Ifuto GUI — v0.2 GUI フロントエンド（X11, C11, ldd は libc/libm のみ）
 *
 * 構成: tabstrip(1 セル行) / omnibox(1) / viewport(H-3) / status(1)。
 * 文書描画は TUI と同一パイプライン（dom → layout → IfGrid）を
 * 自前 5x7 フォントでブリットするだけに限定（表示ロジックの二重実装なし）。
 * 全面バックバッファは持たず、GUI_STRIP_CELLS 行のストリップで流す（メモリ則）。
 *
 * 検証: --shot OUT.ppm FILE は X なしで同じラスタパイプラインを走らせ
 * フルページ PPM を吐く（ヘッドレス QA 用）。 */
#include "../common.h"
#include "../arena.h"
#include "../store.h"
#include "../chrome.h"
#include "../render.h"
#include "fb.h"
#include "x11t.h"
#include "gui.h"
#include "../raster.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUI_DEF_W_PX 1000
#define GUI_DEF_H_PX 720
#define ROWS_TOP 2    /* tabstrip + omnibox */
#define ROWS_BOT 1    /* status */

/* ANSI → RGB（xterm パレット。render_grid が 256 色化済の逆変換） */
static u32 ansi_to_rgb(u8 idx, u32 fallback) {
    static const u16 BASIC[16] = {
        0x000,0x800,0x080,0x880,0x008,0x808,0x088,0xccc,
        0x888,0xf00,0x0f0,0xff0,0x00f,0xf0f,0x0ff,0xfff
    };
    if (idx == IF_CELL_DEFAULT) return fallback;
    if (idx < 16) {
        u16 v = BASIC[idx];
        return ((u32)((v >> 8) & 0xF) * 17 << 16) | ((u32)((v >> 4) & 0xF) * 17 << 8) |
               ((u32)(v & 0xF) * 17);
    }
    if (idx < 232) {
        u8 c = (u8)(idx - 16);
        u8 r = (u8)(c / 36), g = (u8)((c / 6) % 6), b = (u8)(c % 6);
        u8 cv[6] = { 0, 95, 135, 175, 215, 255 };
        return ((u32)cv[r] << 16) | ((u32)cv[g] << 8) | cv[b];
    }
    u8 lv = (u8)(8 + (idx - 232) * 10);
    return ((u32)lv << 16) | ((u32)lv << 8) | lv;
}

/* 文書既定（ブラウザの暗黙既定）: fg ほぼ黒 / bg 白。UI クロームは黒系で別 */
#define GUI_PAGE_FG 0x111111
#define GUI_PAGE_BG 0xffffff

typedef struct {
    IfChrome c;
    char omni[IF_OMNI_CAP];
    u32 omni_len;
    bool omni_focus;
    i32 w_cells, h_cells;
    char status[96];
    bool need_repaint;
    /* viewport 窓グリッドのキャッシュ（[scroll, scroll+vh) のみ materialize。
     * 文書長に比例しない: 「表示しない分は持たない」。再構築は scroll/tab/寸法の変化時のみ） */
    IfCell *wcells;
    u64 wcap;
    IfGrid wingrid;
    IfTab *win_tab;
    i32 win_scroll, win_w, win_h;
} Gui;

/* 現タブの viewport 窓グリッドを（必要時のみ再構築して）返す。文書なしなら NULL。 */
static const IfGrid *gui_view_grid(Gui *g) {
    IfTab *t = if_chrome_cur(&g->c);
    i32 vh = g->h_cells - ROWS_TOP - ROWS_BOT;
    if (!t || !t->lay || vh <= 0) return NULL;
    if (g->wcells && g->win_tab == t && g->win_scroll == t->scroll &&
        g->win_w == g->w_cells && g->win_h == vh)
        return &g->wingrid;
    u64 need = (u64)(g->w_cells > 0 ? g->w_cells : 1) * (u64)vh;
    if (need > g->wcap) {
        free(g->wcells);
        g->wcells = (IfCell *)malloc(need * sizeof(IfCell));
        if (!g->wcells) if_fatal("oom: gui viewport grid");
        g->wcap = need;
    }
    g->wingrid.cells = g->wcells;
    if_render_grid_rows_into(t->lay, t->scroll, t->scroll + vh, &g->wingrid);
    g->win_tab = t; g->win_scroll = t->scroll; g->win_w = g->w_cells; g->win_h = vh;
    return &g->wingrid;
}

/* IfGrid の可視範囲をストリップに描き、consumer へ流す形の共通コア。
 * content_y_px は viewport の原点。戻り値は描いた行数（ストリップ未満で残は未送信、
 * 呼び出し側は flush で残りも流す） */
typedef struct {
    u32 y0_px;        /* このストリップがカバーする画面 y */
    u32 rows;         /* 有効 px 行数 */
    IfFbStrip strip;
} Painter;

static void paint_cell(Painter *p, i32 col_px, i32 row_cell, u8 ch, u32 fg, u32 bg,
                       u8 flags) {
    i32 y_local_px = row_cell * GUI_CELL_H - (i32)p->y0_px;
    fb_glyph(&p->strip, col_px, y_local_px, ch, fg, bg,
             (flags & IF_F_BOLD) != 0, (flags & IF_F_ULINE) != 0);
}

/* 行 row_cell（セル行。0 起点で画面全体）を描く。gui 部（tabstrip/omni/status)は
 * ここで、文書部は grid から。 */
static void paint_screen_row(Gui *g, Painter *p, i32 row_cell) {
    IfTab *t = if_chrome_cur(&g->c);
    if (row_cell == 0) { /* tabstrip */
        fb_rect(&p->strip, 0, row_cell * GUI_CELL_H - (i32)p->y0_px,
                (i32)p->strip.w_px, GUI_CELL_H, 0x2a2f45);
        i32 x = 0;
        for (i32 i = 0; i < g->c.n_tabs; i++) {
            IfTab *tt = g->c.tabs[i];
            bool act = i == g->c.active;
            char label[32];
            snprintf(label, sizeof label, " %s ", tt->title ? tt->title : "?");
            u32 n = (u32)strlen(label);
            if (n > 12) { label[12] = 0; n = 12; }
            fb_text(&p->strip, x * GUI_CELL_W, row_cell * GUI_CELL_H - (i32)p->y0_px,
                    (const u8 *)label, n, act ? 0xffffff : 0x8899bb,
                    act ? 0x3d5af1 : 0x23283c, act, false);
            x += (i32)n + 1;
            if (x > g->w_cells - 4) break;
        }
        char plus[4] = " +";
        fb_text(&p->strip, (g->w_cells - 3) * GUI_CELL_W,
                row_cell * GUI_CELL_H - (i32)p->y0_px, (const u8 *)plus, 2,
                0x8899bb, 0x2a2f45, false, false);
        return;
    }
    if (row_cell == 1) { /* omnibox */
        fb_rect(&p->strip, 0, row_cell * GUI_CELL_H - (i32)p->y0_px,
                (i32)p->strip.w_px, GUI_CELL_H, g->omni_focus ? 0x1b1f30 : 0x14171f);
        fb_rect(&p->strip, 0, row_cell * GUI_CELL_H - (i32)p->y0_px + GUI_CELL_H - 2,
                (i32)p->strip.w_px, 2, g->omni_focus ? 0x3d5af1 : 0x333a55);
        if (g->omni_len)
            fb_text(&p->strip, GUI_CELL_W, row_cell * GUI_CELL_H - (i32)p->y0_px,
                    (const u8 *)g->omni,
                    g->omni_len > (u32)(g->w_cells - 2) ? (u32)(g->w_cells - 2) : g->omni_len,
                    0xffffff, g->omni_focus ? 0x1b1f30 : 0x14171f, false, false);
        if (g->omni_focus) { /* caret */
            i32 cx = 1 + (i32)g->omni_len;
            if (cx > g->w_cells - 1) cx = g->w_cells - 1;
            fb_rect(&p->strip, cx * GUI_CELL_W, row_cell * GUI_CELL_H - (i32)p->y0_px + 3,
                    2, GUI_CELL_H - 6, 0xffffff);
        }
        return;
    }
    if (row_cell == g->h_cells - 1) { /* status */
        fb_rect(&p->strip, 0, row_cell * GUI_CELL_H - (i32)p->y0_px,
                (i32)p->strip.w_px, GUI_CELL_H, 0x2a2f45);
        if (g->status[0])
            fb_text(&p->strip, GUI_CELL_W, row_cell * GUI_CELL_H - (i32)p->y0_px,
                    (const u8 *)g->status, (u32)strlen(g->status), 0xdde6ff, 0x2a2f45,
                    false, false);
        return;
    }
    /* viewport: 窓グリッドからブリット（文書外・窓外はブラウザ既定の白） */
    i32 doc_top_row = ROWS_TOP;
    i32 vh = g->h_cells - ROWS_TOP - ROWS_BOT;
    const IfGrid *vg = (t && row_cell - doc_top_row < vh) ? gui_view_grid(g) : NULL;
    i32 gy = (t ? t->scroll : 0) + (row_cell - doc_top_row);
    if (!t || !vg || row_cell - doc_top_row >= vh || gy < vg->y_off ||
        gy - vg->y_off >= vg->h || gy >= t->doc_h) {
        fb_rect(&p->strip, 0, row_cell * GUI_CELL_H - (i32)p->y0_px,
                (i32)p->strip.w_px, GUI_CELL_H, GUI_PAGE_BG);
        return;
    }
    i32 gy_local_px_top = row_cell * GUI_CELL_H - (i32)p->y0_px;
    fb_rect(&p->strip, 0, gy_local_px_top, (i32)p->strip.w_px, GUI_CELL_H, GUI_PAGE_BG);
    for (i32 col = 0; col < vg->w && col < g->w_cells; col++) {
        IfCell *cell = &vg->cells[(i64)(gy - vg->y_off) * vg->w + col];
        if (cell->cp == 0) continue; /* 全角継続セル */
        u32 fg = ansi_to_rgb(cell->fg, GUI_PAGE_FG);
        u32 bg = ansi_to_rgb(cell->bg, GUI_PAGE_BG);
        /* ASCII 外形付け替え: box-drawing 系は「構図の等価物」に落とす（豆腐回避） */
        u8 ch = cell->cp <= 0x7E && cell->cp >= 0x20 ? (u8)cell->cp
              : (cell->cp >= 0x2500 && cell->cp <= 0x257F) ? '-'
              : (cell->cp == 0x2014 || cell->cp == 0x2013) ? '-' /* em/en dash */
              : (cell->cp == 0x2022 || cell->cp == 0x25CF) ? '*' /* 箇条書き記号 */
              : '?';
        if (cell->cp == ' ') {
            /* 空白でも bg が既定でなければ塗る */
            if (bg != GUI_PAGE_BG)
                fb_rect(&p->strip, col * GUI_CELL_W, gy_local_px_top,
                        GUI_CELL_W, GUI_CELL_H, bg);
            continue;
        }
        bool link_focus = t->link_idx >= 0;
        paint_cell(p, col * GUI_CELL_W, row_cell, ch, fg, bg, cell->flags);
        (void)link_focus;
    }
}

/* PPM 全画面ダンプ（X 不使用の検証経路）。ヘッダ + strips 逐次 */
static bool shot_ppm(Gui *g, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    u32 w_px = (u32)g->w_cells * GUI_CELL_W;
    u32 h_px = (u32)g->h_cells * GUI_CELL_H;
    fprintf(f, "P6\n%u %u\n255\n", w_px, h_px);
    Painter p;
    fb_init(&p.strip, w_px);
    for (u32 y0 = 0; y0 < h_px; y0 += p.strip.h_px) {
        p.y0_px = y0;
        p.rows = h_px - y0 < p.strip.h_px ? h_px - y0 : p.strip.h_px;
        i32 r0 = (i32)(y0 / GUI_CELL_H);
        i32 r1 = (i32)((y0 + p.rows + GUI_CELL_H - 1) / GUI_CELL_H) - 1;
        for (i32 r = r0; r <= r1; r++) paint_screen_row(g, &p, r);
        for (u32 yy = 0; yy < p.rows; yy++)
            for (u32 xx = 0; xx < w_px; xx++) {
                u32 c = p.strip.px[(u64)yy * w_px + xx];
                u8 rgb[3] = { (u8)(c >> 16), (u8)(c >> 8), (u8)c };
                fwrite(rgb, 1, 3, f);
            }
    }
    fclose(f);
    free(p.strip.px);
    return true;
}

/* ---- X11 メインループ ---- */
static void statusf(Gui *g, const char *s) {
    snprintf(g->status, sizeof g->status, "%s", s);
}

static void gui_load(Gui *g, const char *path, i32 width_cells) {
    if (if_chrome_open(&g->c, path, width_cells)) {
        snprintf(g->omni, sizeof g->omni, "%s", path);
        g->omni_len = (u32)strlen(g->omni);
        IfTab *t = if_chrome_cur(&g->c);
        statusf(g, t && t->title ? t->title : "loaded");
    } else {
        statusf(g, "failed to open");
    }
    g->need_repaint = true;
}

static void strip_flush_x(IfX *x, u32 win, Painter *p, u32 w_px) {
    for (u32 y = 0; y < p->rows;) {
        /* PutImage ペイロード上限に合わせてさらに細分け（max_request 考慮） */
        u32 chunk = p->rows - y;
        u32 max_rows = x11_max_request_payload(x) ?
                       (u32)x11_max_request_payload(x) / (w_px * 4) - 8 : 32;
        if (max_rows == 0) max_rows = 1;
        if (chunk > max_rows) chunk = max_rows;
        x11_put_image(x, win, 0, (i32)(p->y0_px + y), w_px, chunk,
                      p->strip.px + (u64)y * w_px);
        y += chunk;
    }
}

static void gui_repaint_x(IfX *x, u32 win, Painter *p, Gui *g, u32 w_px, u32 h_px) {
    for (u32 y0 = 0; y0 < h_px; y0 += p->strip.h_px) {
        p->y0_px = y0;
        p->rows = h_px - y0 < p->strip.h_px ? h_px - y0 : p->strip.h_px;
        i32 r0 = (i32)(y0 / GUI_CELL_H);
        i32 r1 = (i32)((y0 + p->rows + GUI_CELL_H - 1) / GUI_CELL_H) - 1;
        for (i32 r = r0; r <= r1; r++) paint_screen_row(g, p, r);
        strip_flush_x(x, win, p, w_px);
    }
}

static void omni_insert(Gui *g, u8 ch) {
    if (g->omni_len + 1 >= IF_OMNI_CAP) return;
    g->omni[g->omni_len++] = (char)ch;
    g->omni[g->omni_len] = 0;
}

static int gui_run_x(const char *initial) {
    /* 起動時 microbench: raster fill kernel をこの端末実測で決定（冪等。
     * 決定の根拠数値は ifuto://memory に表示される） */
    if_raster_autodetect();
    IfX *x = x11_open();
    if (!x) { fputs("ifuto-gui: X11 connect failed (headless? use --shot)\n", stderr); return 2; }
    u32 win = x11_window(x, GUI_DEF_W_PX, GUI_DEF_H_PX, "Ifuto Browser");
    if (!win) { fputs("ifuto-gui: CreateWindow failed\n", stderr); x11_close(x); return 2; }
    x11_map(x, win);

    Gui g;
    memset(&g, 0, sizeof g);
    IfFsOps fs = { if_fs_exists_real, if_fs_read_real, NULL,
                   if_fs_write_real, if_fs_append_real, if_fs_mkpath_real };
    if_chrome_init(&g.c, fs);
    g.c.now = 0; /* GUI v0.2 は永続セッションを拾わない（store 読み込み無し） */
    u32 w_px = GUI_DEF_W_PX, h_px = GUI_DEF_H_PX;
    g.w_cells = (i32)(w_px / GUI_CELL_W);
    g.h_cells = (i32)(h_px / GUI_CELL_H);
    g.omni_focus = true;
    if (initial) gui_load(&g, initial, g.w_cells);
    Painter p;
    fb_init(&p.strip, w_px);

    bool running = true;
    u32 wm_del = x11_atom_wm_delete(x);
    while (running) {
        IfXev ev;
        if (!x11_next_event(x, &ev)) { fputs("ifuto-gui: X connection lost\n", stderr); break; }
        switch (ev.kind) {
        case IF_XEV_EXPOSE:
        case IF_XEV_CONFIGURE:
            if (ev.kind == IF_XEV_CONFIGURE && ev.x > 0 && ev.y > 0 &&
                ((u32)ev.x != w_px || (u32)ev.y != h_px)) {
                w_px = (u32)ev.x; h_px = (u32)ev.y;
                g.w_cells = (i32)(w_px / GUI_CELL_W);
                g.h_cells = (i32)(h_px / GUI_CELL_H);
                free(p.strip.px);
                fb_init(&p.strip, w_px);
                if_chrome_relayout(&g.c, g.w_cells);
            }
            gui_repaint_x(x, win, &p, &g, w_px, h_px);
            break;
        case IF_XEV_CLIENTMSG:
            if (ev.aux == wm_del) running = false;
            break;
        case IF_XEV_BUTTON: {
            /* ビューポートのクリック: omnibox 行ならフォーカス、以下は将来リンク */
            g.omni_focus = ev.y < (i32)(2 * GUI_CELL_H);
            if (g.omni_focus) gui_repaint_x(x, win, &p, &g, w_px, h_px);
            break;
        }
        case IF_XEV_KEY: {
            bool ctrl = (ev.state & 4) != 0;
            u32 ks = ev.keysym;
            if (ctrl) {
                if (ks == 'q' || ks == 'Q') { running = false; break; }
                if (ks == 'l' || ks == 'L') { g.omni_focus = true; gui_repaint_x(x, win, &p, &g, w_px, h_px); break; }
                if (ks == 't' || ks == 'T') {
                    IfTab *t = if_chrome_new_blank(&g.c);
                    (void)t; /* 空白タブを current に（open 時に置き換わる） */
                    g.omni_len = 0; g.omni[0] = 0; g.omni_focus = true;
                    gui_repaint_x(x, win, &p, &g, w_px, h_px);
                    break;
                }
                if (ks == 'w' || ks == 'W') {
                    if_chrome_close(&g.c, g.w_cells); /* 最後の 1 枚は空白化（API 規約） */
                    gui_repaint_x(x, win, &p, &g, w_px, h_px);
                    break;
                }
                if (ks == '\x09' || ks == 'i' /* Ctrl+Tab 近似: X の Tab に Ctrl が載る */) {
                    if (g.c.n_tabs > 1) if_chrome_switch(&g.c, (g.c.active + 1) % g.c.n_tabs, g.w_cells);
                    gui_repaint_x(x, win, &p, &g, w_px, h_px);
                    break;
                }
            }
            if (g.omni_focus) {
                if (ks == XK_ESCAPE) { g.omni_focus = false; }
                else if (ks == XK_BACKSPACE) {
                    if (g.omni_len) g.omni[--g.omni_len] = 0;
                } else if (ks == XK_RETURN) {
                    g.omni[g.omni_len] = 0;
                    if (g.omni_len) gui_load(&g, g.omni, g.w_cells);
                    g.omni_focus = false;
                } else if (ev.code >= 0x20 && ev.code <= 0x7E) {
                    omni_insert(&g, ev.code);
                }
                gui_repaint_x(x, win, &p, &g, w_px, h_px);
                break;
            }
            i32 vh = g.h_cells - ROWS_TOP - ROWS_BOT;
            bool dirty = false;
            if (ks == XK_DOWN || ks == 'j') { if_chrome_scroll(&g.c, 1, vh); dirty = true; }
            else if (ks == XK_UP || ks == 'k') { if_chrome_scroll(&g.c, -1, vh); dirty = true; }
            else if (ks == XK_NEXT) { if_chrome_scroll(&g.c, vh, vh); dirty = true; }
            else if (ks == XK_PRIOR) { if_chrome_scroll(&g.c, -vh, vh); dirty = true; }
            else if (ks == '/') { g.omni_focus = true; dirty = true; }
            else if (ks == 'q') { running = false; }
            if (dirty) gui_repaint_x(x, win, &p, &g, w_px, h_px);
            break;
        }
        default: break;
        }
    }
    x11_destroy(x, win);
    /* IfChrome の tabs arean は leak 相当だが OS 回収に委ねる（GUI v0.2）。台帳 */
    free(p.strip.px);
    free(g.wcells);
    x11_close(x);
    return 0;
}

int if_gui_run(const char *initial_path) {
    return gui_run_x(initial_path);
}

int if_gui_shot(const char *input_path, const char *out_ppm) {
    /* ヘッドレス検証: X なしで同じパイプライン（grid まで共有）を走らせる */
    if_raster_autodetect(); /* ラスタ kernel 決定（対話 GUI と同一規則） */
    Gui g;
    memset(&g, 0, sizeof g);
    IfFsOps fs = { if_fs_exists_real, if_fs_read_real, NULL,
                   if_fs_write_real, if_fs_append_real, if_fs_mkpath_real };
    if_chrome_init(&g.c, fs);
    g.w_cells = 125; /* 1000px 相当 */
    g.h_cells = 45;  /* 720px 相当 */
    g.omni_focus = true;
    if (input_path) gui_load(&g, input_path, g.w_cells);
    bool ok = shot_ppm(&g, out_ppm);
    free(g.wcells);
    return ok ? 0 : 1;
}
