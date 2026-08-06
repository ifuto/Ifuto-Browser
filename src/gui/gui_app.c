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
    /* 差分描画: 画面セル行ごとの内容ハッシュ（0=未描画/露出。同一値 0 は予約で
     * 現れないよう hash 側で 0→1 に補正する）。イベントごとの全面再送を止め、
     * 変化行だけを paint+PutImage する */
    u32 *rowhash;
    u32 rowhash_cap;
    /* 差分スクロール測定（ifuto://memory 表示用: 省略できた PutImage 行数の累積） */
    u64 diff_skip_rows;
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
        if (cell->cp == 0) continue; /* 全角継続セル（先頭セルの glyph16 が 2 セルぶん塗る） */
        u32 fg = ansi_to_rgb(cell->fg, GUI_PAGE_FG);
        u32 bg = ansi_to_rgb(cell->bg, GUI_PAGE_BG);
        /* グリフ選択はラスタ層（fb_glyph_cp）の責務: ASCII 外形付け替え・
         * 全角互換形・font16（かな/カナ/記号）・明示豆腐を一点化する */
        fb_glyph_cp(&p->strip, col * GUI_CELL_W, gy_local_px_top, cell->cp, fg, bg,
                    (cell->flags & IF_F_BOLD) != 0, (cell->flags & IF_F_ULINE) != 0);
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

/* ---- 差分描画（ユーザー則: 再描画ではなく差分描画） ---- */

static u32 fnv1a32(const void *p, u64 n, u32 h) {
    const u8 *b = (const u8 *)p;
    for (u64 i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/* 画面セル行 row の「その行を描くのに必要な情報全部」のハッシュ。
 * 変更行検出はこれの front/back 比較（ハッシュ衝突率 2^-32/行/イベントを許容。
 * 衝突したら「その行だけ 1 イベント古いまま」という劣化であり破壊的でない） */
static u32 gui_row_hash(Gui *g, i32 row) {
    u32 h = 2166136261u;
    if (row == 0) { /* tabstrip: タブ列+active+セル幅 */
        for (i32 i = 0; i < g->c.n_tabs; i++) {
            IfTab *tt = g->c.tabs[i];
            const char *ti = tt->title ? tt->title : "";
            h = fnv1a32(ti, strlen(ti) > 12 ? 12 : strlen(ti), h);
            h ^= (u32)i * 2654435761u;
        }
        h ^= (u32)g->c.active * 2246822519u ^ (u32)g->w_cells;
    } else if (row == 1) { /* omnibox */
        h = fnv1a32(g->omni, g->omni_len, h) ^ (u32)g->omni_focus * 668265263u;
    } else if (row == g->h_cells - 1) { /* status */
        h = fnv1a32(g->status, strlen(g->status), h);
    } else { /* 文書 viewport 行 */
        IfTab *t = if_chrome_cur(&g->c);
        i32 vh = g->h_cells - ROWS_TOP - ROWS_BOT;
        i32 vr = row - ROWS_TOP;
        const IfGrid *vg = gui_view_grid(g);
        if (!t || !vg || vr >= vh) return 0x9E3779B9u;
        i32 gy = t->scroll + vr;
        if (gy < vg->y_off || gy - vg->y_off >= vg->h || gy >= t->doc_h)
            return 0x811C9DC5u;
        u32 w = (u32)(vg->w < g->w_cells ? vg->w : g->w_cells);
        h = fnv1a32(&vg->cells[(i64)(gy - vg->y_off) * vg->w], (u64)w * sizeof(IfCell), h);
    }
    return h ? h : 1; /* 0 は「未描画」予約 */
}

static void strip_flush_span(IfX *x, u32 win, Painter *p, u32 w_px,
                             u32 row_lo_px_off, u32 n_rows_px) {
    /* ストリップ内 [row_lo_px_off, +n_rows_px) を上限分割しつつ送る */
    for (u32 y = 0; y < n_rows_px;) {
        u32 chunk = n_rows_px - y;
        u32 max_rows = x11_max_request_payload(x) ?
                       (u32)x11_max_request_payload(x) / (w_px * 4) - 8 : 32;
        if (max_rows == 0) max_rows = 1;
        if (chunk > max_rows) chunk = max_rows; /* max_rows は px 行単位 */
        x11_put_image(x, win, 0, (i32)(p->y0_px + row_lo_px_off + y), w_px, chunk,
                      p->strip.px + (u64)(row_lo_px_off + y) * w_px);
        y += chunk;
    }
}

static void gui_repaint_x(IfX *x, u32 win, Painter *p, Gui *g, u32 w_px, u32 h_px) {
    if (!g->rowhash || g->rowhash_cap < (u32)g->h_cells) {
        free(g->rowhash);
        g->rowhash = (u32 *)calloc((u32)(g->h_cells > 0 ? g->h_cells : 1), 4);
        if (!g->rowhash) if_fatal("oom: rowhash");
        g->rowhash_cap = (u32)g->h_cells;
    }
    for (u32 y0 = 0; y0 < h_px; y0 += p->strip.h_px) {
        p->y0_px = y0;
        p->rows = h_px - y0 < p->strip.h_px ? h_px - y0 : p->strip.h_px;
        i32 r0 = (i32)(y0 / GUI_CELL_H);
        i32 r1 = (i32)((y0 + p->rows + GUI_CELL_H - 1) / GUI_CELL_H) - 1;
        /* 差分判定（paint より先に全行の hash を確定させる） */
        bool changed[GUI_STRIP_CELLS];
        bool any = false;
        for (i32 r = r0; r <= r1; r++) {
            u32 h = gui_row_hash(g, r);
            changed[r - r0] = (g->rowhash[r] != h);
            any |= changed[r - r0];
            g->rowhash[r] = h;
        }
        if (!any) { g->diff_skip_rows += (u32)(r1 - r0 + 1); continue; }
        /* 変化行だけ paint し、連続ランごとに PutImage */
        for (i32 r = r0; r <= r1; r++)
            if (changed[r - r0]) paint_screen_row(g, p, r);
        i32 r = r0;
        while (r <= r1) {
            if (!changed[r - r0]) { r++; continue; }
            i32 ra = r;
            while (r <= r1 && changed[r - r0]) r++;
            i32 rb = r; /* [ra, rb) 全変化 */
            strip_flush_span(x, win, p, w_px,
                             (u32)(ra * GUI_CELL_H) - y0, (u32)(rb - ra) * GUI_CELL_H);
        }
    }
}

/* 差分スクロール: CopyArea でオーバーラップ部をサーバ側シフトし、
 * 露出した行だけ hash を 0（未描画）に刻んで次の repaint に描かせる。 */
static void gui_scroll_copy(IfX *x, u32 win, Gui *g, u32 w_px, i32 d, i32 vh) {
    if (!g->rowhash) return;
    u32 doc_top_px = ROWS_TOP * GUI_CELL_H;
    u32 vh_px = (u32)vh * GUI_CELL_H;
    u32 mag_cells = (u32)(d > 0 ? d : -d);
    if (mag_cells >= (u32)vh) { /* 全面露出 */
        memset(g->rowhash + ROWS_TOP, 0, (u32)vh * sizeof(u32));
        return;
    }
    u32 mag = mag_cells * GUI_CELL_H;
    if (d > 0)
        x11_copy_area(x, win, 0, (i32)(doc_top_px + mag), 0, (i32)doc_top_px,
                      w_px, vh_px - mag);
    else
        x11_copy_area(x, win, 0, (i32)doc_top_px, 0, (i32)(doc_top_px + mag),
                      w_px, vh_px - mag);
    u32 *H = g->rowhash;
    if (d > 0) {
        memmove(H + ROWS_TOP, H + ROWS_TOP + d,
                ((u32)vh - (u32)d) * sizeof(u32));
        /* 下辺に露出 [vh-d, vh) */
        memset(H + ROWS_TOP + (u32)vh - (u32)d, 0, (u32)d * sizeof(u32));
    } else {
        memmove(H + ROWS_TOP - d, H + ROWS_TOP,
                ((u32)vh + (u32)d) * sizeof(u32));
        /* 上辺に露出 [0, -d) */
        memset(H + ROWS_TOP, 0, (u32)(-d) * sizeof(u32));
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
            /* Expose/Configure はサーバ側の表示内容が信用できない → 全面差分無効化 */
            if (g.rowhash) memset(g.rowhash, 0, g.rowhash_cap * sizeof(u32));
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
            IfTab *tpre = if_chrome_cur(&g.c);
            i32 spre = tpre ? tpre->scroll : 0;
            if (ks == XK_DOWN || ks == 'j') { if_chrome_scroll(&g.c, 1, vh); dirty = true; }
            else if (ks == XK_UP || ks == 'k') { if_chrome_scroll(&g.c, -1, vh); dirty = true; }
            else if (ks == XK_NEXT) { if_chrome_scroll(&g.c, vh, vh); dirty = true; }
            else if (ks == XK_PRIOR) { if_chrome_scroll(&g.c, -vh, vh); dirty = true; }
            else if (ks == '/') { g.omni_focus = true; dirty = true; }
            else if (ks == 'q') { running = false; }
            if (dirty) {
                /* 差分スクロール: 純粋なスクロール変化なら CopyArea で
                 * 画面をずらし露出行だけ描く（全面再 paint/再送を回避） */
                IfTab *tpost = if_chrome_cur(&g.c);
                i32 d = (tpre && tpre == tpost) ? tpost->scroll - spre : 0;
                if (d != 0)
                    gui_scroll_copy(x, win, &g, w_px, d, vh);
                gui_repaint_x(x, win, &p, &g, w_px, h_px);
            }
            break;
        }
        default: break;
        }
    }
    x11_destroy(x, win);
    /* IfChrome の tabs arean は leak 相当だが OS 回収に委ねる（GUI v0.2）。台帳 */
    free(p.strip.px);
    free(g.wcells);
    free(g.rowhash);
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
    if_chrome_destroy(&g.c); /* LSan 対象の正当解体（タブ「1 タブ 1 arena」ごと） */
    free(g.wcells);
    free(g.rowhash);
    return ok ? 0 : 1;
}
