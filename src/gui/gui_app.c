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
#include <time.h>

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
    /* ホバー中リンク（マウスポインタ直下）。hover_tab はタブ切替跨ぎの陳腐化
     * 防止（hover_idx は現タブの links 添字としてのみ有効。NULL で無ホバー）。
     * status_saved はホバーで statusbar を href 表示に上書きする前の退避 */
    i32 hover_idx;
    IfTab *hover_tab;
    char status_saved[96];
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
/* フォーカス中リンク（キーボード Tab/Enter・クリックで確定）の表示矩形に (gy,col)
 * が含まれるか。n_spans=0 のリンク（複数行 wrap 経路等・台帳残課題）は可視化なし */
static const IfLink *gui_focus_link(const IfTab *t) {
    if (!t || !t->lay || t->link_idx < 0 || t->link_idx >= (i32)t->lay->n_links) return NULL;
    const IfLink *fl = &t->lay->links[t->link_idx];
    return fl->n_spans ? fl : NULL;
}
static bool gui_focus_cell(const IfLink *fl, i32 gy, i32 col) {
    for (u32 s = 0; s < fl->n_spans; s++)
        if (gy >= fl->spans[s].y0 && gy < fl->spans[s].y1 &&
            col >= fl->spans[s].x0 && col < fl->spans[s].x1) return true;
    return false;
}

/* ホバー中リンク（矩形未収集のリンクは可視化なし — focus と同じ規則） */
static const IfLink *gui_hover_link(Gui *g) {
    const IfTab *t = if_chrome_cur(&g->c);
    if (!t || !t->lay || g->hover_tab != t || g->hover_idx < 0 ||
        g->hover_idx >= (i32)t->lay->n_links) return NULL;
    const IfLink *L = &t->lay->links[g->hover_idx];
    return L->n_spans ? L : NULL;
}

/* (col,gy) 文書座標のリンク添字ヒットテスト（クリック/ホバー共用の一点化） */
static i32 gui_link_hit(const IfTab *t, i32 col, i32 gy) {
    if (!t || !t->lay) return -1;
    for (u32 i = 0; i < t->lay->n_links; i++) {
        const IfLink *L = &t->lay->links[i];
        for (u32 s = 0; s < L->n_spans; s++)
            if (col >= L->spans[s].x0 && col < L->spans[s].x1 &&
                gy >= L->spans[s].y0 && gy < L->spans[s].y1) return (i32)i;
    }
    return -1;
}

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
    const IfLink *fl = gui_focus_link(t);
    const IfLink *hl = gui_hover_link(g);
    for (i32 col = 0; col < vg->w && col < g->w_cells; col++) {
        IfCell *cell = &vg->cells[(i64)(gy - vg->y_off) * vg->w + col];
        if (cell->cp == 0) continue; /* 全角継続セル（先頭セルの glyph16 が 2 セルぶん塗る） */
        u32 fg = ansi_to_rgb(cell->fg, GUI_PAGE_FG);
        u32 bg = ansi_to_rgb(cell->bg, GUI_PAGE_BG);
        if (fl && gui_focus_cell(fl, gy, col)) { bg = 0xbfe3ff; fg = 0x10243f; }
        else if (hl && gui_focus_cell(hl, gy, col)) { bg = 0xd8e7ff; } /* ホバーは focus より薄い強調 */
        /* グリフ選択はラスタ層（fb_glyph_cp）の責務: ASCII 外形付け替え・
         * 全角互換形・font16（かな/カナ/記号）・明示豆腐を一点化する */
        fb_glyph_cp(&p->strip, col * GUI_CELL_W, gy_local_px_top, cell->cp, fg, bg,
                    (cell->flags & IF_F_BOLD) != 0, (cell->flags & IF_F_ULINE) != 0,
                    (cell->flags & IF_F_ITALIC) != 0);
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
static void gui_load(Gui *g, const char *path, i32 width_cells);

static void statusf(Gui *g, const char *s) {
    snprintf(g->status, sizeof g->status, "%s", s);
}

/* chrome モデルの toast を statusbar へ表面化（共通の 1 行通知流路） */
static void gui_toast(Gui *g) {
    if (g->c.toast_len) statusf(g, g->c.toast);
}

/* omnibox を現タブ url に同期（タブ切替・復元でも omnibox は常に真実を示す） */
static void gui_sync_omni(Gui *g) {
    IfTab *t = if_chrome_cur(&g->c);
    const char *u = (t && t->url) ? t->url : "";
    snprintf(g->omni, sizeof g->omni, "%s", u);
    g->omni_len = (u32)strlen(g->omni);
}

/* href を開く。http:// は取得して表示（v0.3）。他 scheme はステータス表示で止める。
 * 相対参照は現タブ url の dirname 基準で join（URL 正規化の v0.1 形） */
static void gui_open_href(Gui *g, IfStr href, const IfTab *t) {
    char buf[960], msg[1024];
    if (!href.p || href.n == 0 || href.n >= sizeof buf) return;
    memcpy(buf, href.p, href.n);
    buf[href.n] = 0;
    if (buf[0] == '#') { statusf(g, "anchor は未対応"); return; }
    if (strncmp(buf, "http://", 7) == 0) { gui_load(g, buf, g->w_cells); return; }
    if (buf[0] == '/' && buf[1] == '/') { /* scheme-relative → http 補完 */
        char full[1024];
        snprintf(full, sizeof full, "http:%s", buf);
        gui_load(g, full, g->w_cells);
        return;
    }
    if (strstr(buf, "://")) { snprintf(msg, sizeof msg, "未対応 scheme: %.80s", buf); statusf(g, msg); return; }
    if (buf[0] == '/') { gui_load(g, buf, g->w_cells); return; }
    /* 相対 join: 現 url の最後の '/' までを基底に */
    const char *base = t && t->url ? t->url : "";
    bool http_base = strncmp(base, "http://", 7) == 0;
    const char *sl = strrchr(base, '/');
    char joined[1024];
    if (sl && (!http_base || sl >= base + 7)) /* http:// 内側の '/' は基底境界にしない */
        snprintf(joined, sizeof joined, "%.*s/%s", (int)(sl - base), base, buf);
    else if (http_base) /* authority のみで path 無し → 末尾に付ける */
        snprintf(joined, sizeof joined, "%s/%s", base, buf);
    else
        snprintf(joined, sizeof joined, "%s", buf);
    gui_load(g, joined, g->w_cells);
}

static void gui_load(Gui *g, const char *path, i32 width_cells) {
    g->hover_idx = -1; g->hover_tab = NULL; /* 遷移でホバー対象は失効 */
    if (if_chrome_open(&g->c, path, width_cells)) {
        gui_sync_omni(g);
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
        /* フォーカスリンクの交差行: フォーカス移動時に旧位置・新位置の双方の行が
         * 変化扱いとなるよう salt を doc 座標 gy の関数として混ぜる（スクロール不変）*/
        const IfLink *fl = gui_focus_link(t);
        if (fl)
            for (u32 s = 0; s < fl->n_spans; s++)
                if (gy >= fl->spans[s].y0 && gy < fl->spans[s].y1) {
                    h ^= 0x85EBCA6Bu + (u32)t->link_idx * 0x9E3779B1u;
                    break;
                }
        /* ホバーリンクの交差行: ホバー移動時に旧位置・新位置の双方の行が
         * 変化扱いとなるよう salt を混ぜる（focus と同じ不変条件）*/
        const IfLink *hl = gui_hover_link(g);
        if (hl)
            for (u32 s = 0; s < hl->n_spans; s++)
                if (gy >= hl->spans[s].y0 && gy < hl->spans[s].y1) {
                    h ^= 0x27D4EB2Fu + (u32)g->hover_idx * 0x9E3779B1u;
                    break;
                }
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
    g.c.now = (i64)time(NULL); /* 履歴の timestamp（open 時に store へ書かれる） */
    u32 w_px = GUI_DEF_W_PX, h_px = GUI_DEF_H_PX;
    g.w_cells = (i32)(w_px / GUI_CELL_W);
    g.h_cells = (i32)(h_px / GUI_CELL_H);
    if (if_chrome_restore(&g.c, g.w_cells) > 0) {
        /* 前回セッション（session.txt: 永続化）。active のみ即ロード、
         * 他タブは切替時 lazy_load（chrome モデルの規約どおり）。
         * initial 指定時も先に復元する: しない場合、autosave が旧セッションを
         * 未表示のまま上書きして失う（データ喪失の防止が優先。CLI 単発起動でも同規則） */
        if (!initial) statusf(&g, "前回のセッションを復元しました");
        gui_sync_omni(&g);
    } else if (!initial) {
        if_chrome_new_blank(&g.c);
        g.omni_focus = true;
    }
    gui_toast(&g); /* lazy_load 失敗等の toast があれば表面化 */
    if (initial) {
        g.omni_focus = true;
        gui_load(&g, initial, g.w_cells);
    }
    Painter p;
    fb_init(&p.strip, w_px);

    bool running = true;
    u32 wm_del = x11_atom_wm_delete(x);
    while (running) {
        IfXev ev;
        if (!x11_next_event(x, &ev)) { fputs("ifuto-gui: X connection lost\n", stderr); break; }
        g.c.now = (i64)time(NULL); /* ユーザ駆動 Hz 級: イベント毎に時刻を追従 */
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
            /* 左クリック: オムニボックス行はフォーカス。文書部はリンクのヒットテスト
             * （span 未収集のリンク — 複数行 wrap 等 — は v0.3 台帳の残課題） */
            g.omni_focus = ev.y < (i32)(2 * GUI_CELL_H);
            if (!g.omni_focus && ev.code == 1) {
                IfTab *t = if_chrome_cur(&g.c);
                i32 col = ev.x / GUI_CELL_W;
                i32 gy = ev.y / GUI_CELL_H - ROWS_TOP + (t ? t->scroll : 0);
                i32 hit = gui_link_hit(t, col, gy);
                if (hit >= 0) {
                    t->link_idx = hit; /* クリック = フォーカス（Chrome 流） */
                    gui_open_href(&g, t->lay->links[hit].href, t);
                }
            }
            gui_repaint_x(x, win, &p, &g, w_px, h_px);
            break;
        }
        case IF_XEV_MOTION: {
            /* ホバー: ポインタ直下のリンクを statusbar に href 表示 + 矩形を
             * 薄く強調（普通のブラウザの hover 挙動）。再描画は対象変更時のみ
             * （motion イベント律は塗り分けではなく状態遷移に畳む） */
            IfTab *t = if_chrome_cur(&g.c);
            i32 vh2 = g.h_cells - ROWS_TOP - ROWS_BOT;
            i32 vr = ev.y / GUI_CELL_H - ROWS_TOP;
            i32 hit = -1;
            if (t && vr >= 0 && vr < vh2)
                hit = gui_link_hit(t, ev.x / GUI_CELL_W, t->scroll + vr);
            bool was = g.hover_tab != NULL; /* ホバー有効状態は hover_tab で判定 */
            if (hit != g.hover_idx || (hit >= 0 && g.hover_tab != t)) {
                if (!was && hit >= 0)
                    memcpy(g.status_saved, g.status, sizeof g.status_saved);
                g.hover_idx = hit;
                g.hover_tab = hit >= 0 ? t : NULL;
                if (hit >= 0) {
                    IfStr hr = t->lay->links[hit].href;
                    char hm[96];
                    u32 hn = hr.p && hr.n < sizeof hm ? hr.n : 0;
                    if (hn) memcpy(hm, hr.p, hn);
                    hm[hn] = 0;
                    statusf(&g, hm);
                } else if (was) {
                    statusf(&g, g.status_saved);
                }
                gui_repaint_x(x, win, &p, &g, w_px, h_px);
            }
            break;
        }
        case IF_XEV_KEY: {
            bool ctrl = (ev.state & 4) != 0;
            u32 ks = ev.keysym;
            if (ctrl) {
                if (ks == 'q' || ks == 'Q') {
                    if (if_chrome_quit(&g.c, g.c.now)) running = false;
                    else { gui_toast(&g); gui_repaint_x(x, win, &p, &g, w_px, h_px); }
                    break;
                }
                if (ks == 'r' || ks == 'R') { /* Ctrl+R リロード */
                    g.win_tab = NULL; /* wingrid キャッシュ陳腐化（lay 再構築のため） */
                    bool okr = if_chrome_reload(&g.c, g.w_cells);
                    gui_toast(&g);
                    if (okr) statusf(&g, "reloaded");
                    g.omni_focus = false;
                    gui_repaint_x(x, win, &p, &g, w_px, h_px);
                    break;
                }
                if (ks == 'd' || ks == 'D') { /* ブックマーク トグル（store 連動） */
                    if_chrome_bookmark_cur(&g.c);
                    gui_toast(&g);
                    gui_repaint_x(x, win, &p, &g, w_px, h_px);
                    break;
                }
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
                /* keycode_to_keysym は ctrl では index を変えないため Tab は
                 * 常に XK_TAB (0xFF09) で届く（旧来の '\x09' 比較は不発のデッド条件だった） */
                if (ks == XK_TAB || ks == 'i' /* Ctrl+Tab / Ctrl+I でタブ切替 */) {
                    if (g.c.n_tabs > 1) {
                        if_chrome_switch(&g.c, (g.c.active + 1) % g.c.n_tabs, g.w_cells);
                        gui_sync_omni(&g); /* 切替後も omnibox は現タブの真実を示す */
                        gui_toast(&g);     /* lazy_load 失敗等を表面化 */
                    }
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
                    if (g.omni_len) {
                        /* omnibox 解決: 絶対 / cwd 相対の実在検査とスキーム明示拒否
                         * （resolve 失敗時に生パスを黙って開かない = INV-3） */
                        char out[4096];
                        i32 rc = if_chrome_resolve(&g.c, g.omni, ".", out, sizeof out);
                        if (rc == 0) gui_load(&g, out, g.w_cells);
                        else if (rc == 1) statusf(&g, "http(s) は未取得（v0.3 台帳）");
                        else statusf(&g, "見つかりません");
                    }
                    g.omni_focus = false;
                } else if (ev.code >= 0x20 && ev.code <= 0x7E) {
                    omni_insert(&g, ev.code);
                }
                gui_repaint_x(x, win, &p, &g, w_px, h_px);
                break;
            }
            i32 vh = g.h_cells - ROWS_TOP - ROWS_BOT;
            bool dirty = false;
            bool focus_change = false;
            IfTab *tpre = if_chrome_cur(&g.c);
            i32 spre = tpre ? tpre->scroll : 0;
            if (ks == XK_DOWN || ks == 'j') { if_chrome_scroll(&g.c, 1, vh); dirty = true; }
            else if (ks == XK_UP || ks == 'k') { if_chrome_scroll(&g.c, -1, vh); dirty = true; }
            else if (ks == XK_NEXT) { if_chrome_scroll(&g.c, vh, vh); dirty = true; }
            else if (ks == XK_PRIOR) { if_chrome_scroll(&g.c, -vh, vh); dirty = true; }
            else if ((ks == XK_TAB || ks == XK_ISO_LEFTTAB) && !ctrl) {
                /* リンクフォーカス巡回（Shift=逆方向）。n_spans=0 のリンクも巡回対象
                 * （href は全リンクにある。可視矩形のない経路は台帳残課題どおり） */
                IfTab *t = if_chrome_cur(&g.c);
                i32 delta = (ks == XK_ISO_LEFTTAB || (ev.state & 1)) ? -1 : +1;
                i32 li = if_chrome_link_move(&g.c, delta);
                if (li < 0) statusf(&g, "リンクなし");
                if (t && t->lay && li >= 0) {
                    const IfLink *L = &t->lay->links[li];
                    char m[1088];
                    snprintf(m, sizeof m, "link %d/%u: %.*s", li + 1, t->lay->n_links,
                             (int)(L->href.n > 1000 ? 1000 : L->href.n), L->href.p ? L->href.p : "");
                    statusf(&g, m);
                    /* 最初の span が viewport 外ならそこへ跳ぶ */
                    if (L->n_spans) {
                        i32 ty = L->spans[0].y0;
                        if (ty < t->scroll || ty >= t->scroll + vh)
                            if_chrome_scroll(&g.c, ty - 2 - t->scroll, vh);
                    }
                }
                dirty = true; focus_change = true;
            }
            else if (ks == XK_RETURN) {
                IfTab *t = if_chrome_cur(&g.c);
                if (t && t->lay && t->link_idx >= 0 && t->link_idx < (i32)t->lay->n_links) {
                    gui_open_href(&g, t->lay->links[t->link_idx].href, t);
                    focus_change = true;
                }
                dirty = true;
            }
            else if (ks == 'r') { /* リロード（F5 非対応端末もあるため素のキーも割当） */
                g.win_tab = NULL; /* wingrid キャッシュ陳腐化（lay 再構築のため） */
                bool okr = if_chrome_reload(&g.c, g.w_cells);
                gui_toast(&g);
                if (okr) statusf(&g, "reloaded");
                dirty = true;
            }
            else if (ks == '/') { g.omni_focus = true; dirty = true; }
            else if (ks == 'q') {
                if (if_chrome_quit(&g.c, g.c.now)) running = false;
                else { gui_toast(&g); dirty = true; }
            }
            if (dirty) {
                /* 差分スクロール: 純粋なスクロール変化なら CopyArea で
                 * 画面をずらし露出行だけ描く（全面再 paint/再送を回避）。
                 * フォーカス変化・文書再読込が絡む場合は行 hash の陳腐化を避ける
                 * ため CopyArea を使わず内容比較で最小再描する（安全性優先） */
                IfTab *tpost = if_chrome_cur(&g.c);
                i32 d = (tpre && tpre == tpost && !focus_change) ? tpost->scroll - spre : 0;
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
    /* 起動規則は対話 GUI（gui_run_x）と完全に同一: 常に restore-first、引数ページは
     * 追加タブ（autosave のクラッバー=データ喪失を全起動経路で防ぐ）。
     * 内容 oracle シナリオは IFUTO_NO_STORE=1 でストア自体を止めて揺れを外す
     * （gui_smoke の環境分離として固定）。無引数+復元あり = 復元 oracle */
    if (if_chrome_restore(&g.c, g.w_cells) > 0 && !input_path) gui_sync_omni(&g);
    if (input_path) gui_load(&g, input_path, g.w_cells);
    /* 検査フック: IF_SHOT_FOCUS=N でフォーカスリンク確定後の描画を再現する
     * （対話キー操作と同一の paint/hash 経路。範囲外・解析失敗は無フォーカスとして
     * 未フォーカス shot とバイト一致することが oracle（gui_smoke で固定）） */
    {
        const char *fo = getenv("IF_SHOT_FOCUS");
        if (fo && *fo) {
            char *end = NULL;
            long li = strtol(fo, &end, 10);
            IfTab *t = if_chrome_cur(&g.c);
            if (t && t->lay && end != fo && li >= 0 && li < (long)t->lay->n_links)
                t->link_idx = (i32)li;
        }
    }
    /* 検査フック: IF_SHOT_HOVER=N でホバー中の描画を再現する（矩形強調 +
     * statusbar href 表示。対話 MotionNotify 処理と同一 paint/hash 経路。
     * 範囲外・解析失敗は無ホバーとして未ホバー shot とバイト一致が oracle） */
    {
        const char *ho = getenv("IF_SHOT_HOVER");
        if (ho && *ho) {
            char *end = NULL;
            long li = strtol(ho, &end, 10);
            IfTab *t = if_chrome_cur(&g.c);
            if (t && t->lay && end != ho && li >= 0 && li < (long)t->lay->n_links) {
                g.hover_idx = (i32)li;
                g.hover_tab = t;
                IfStr hr = t->lay->links[li].href;
                u32 hn = hr.p && hr.n < sizeof g.status ? hr.n : 0;
                if (hn) memcpy(g.status, hr.p, hn);
                g.status[hn] = 0;
            }
        }
    }
    bool ok = shot_ppm(&g, out_ppm);
    if_chrome_destroy(&g.c); /* LSan 対象の正当解体（タブ「1 タブ 1 arena」ごと） */
    free(g.wcells);
    free(g.rowhash);
    return ok ? 0 : 1;
}
