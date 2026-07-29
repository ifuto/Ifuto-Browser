/* Ifuto — TUI 糊層（termios・ペイント・イベントループ）。
 * モデルは chrome.c、キー解釈は ui_input.c にあり、ここは薄く保つ。
 * INV-5: タイマー駆動再描画なし。read ブロック待ち、イベント時に1回だけ全再描画。
 */
#define _POSIX_C_SOURCE 200809L
#include "tui.h"
#include "ui_input.h"
#include "chrome.h"
#include "utf8.h"
#include <termios.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define CHROME_ROWS_TOP 2 /* tabstrip + omnibox */
#define CHROME_ROWS_BOTTOM 1 /* status */

static volatile sig_atomic_t g_winch = 0;
static void on_winch(int sig) { (void)sig; g_winch = 1; }

typedef struct {
    struct termios saved;
    bool raw;
    i32 cols, rows;
    char cwd[4096];
    IfUiDecoder dec;
    char *buf;      /* 1 フレーム分の出力を組み立てて一括 write（ちらつき防止） */
    u64 buflen, bufcap;
} IfTui;

/* ---- 出力バッファ ---- */
static void out_cat(IfTui *t, const char *s, u64 n) {
    if (t->buflen + n > t->bufcap) {
        u64 cap = t->bufcap ? t->bufcap : 1 << 16;
        while (t->buflen + n > cap) cap *= 2;
        t->buf = (char *)realloc(t->buf, cap);
        if (!t->buf) if_fatal("oom: tui paint buffer");
        t->bufcap = cap;
    }
    memcpy(t->buf + t->buflen, s, n);
    t->buflen += n;
}
static void out_s(IfTui *t, const char *s) { out_cat(t, s, strlen(s)); }
static void out_n(IfTui *t, char c, i32 n) { while (n-- > 0) out_cat(t, &c, 1); }
static void out_num(IfTui *t, i64 v) { char tmp[24]; int n = snprintf(tmp, sizeof tmp, "%lld", (long long)v); out_cat(t, tmp, (u64)n); }

/* ---- 端末 ---- */
static void tty_enter(IfTui *t) {
    if (tcgetattr(STDIN_FILENO, &t->saved) != 0) if_fatal("tiu: not a tty");
    struct termios raw = t->saved;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    t->raw = true;
    static const char init[] = "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H";
    (void)!write(STDOUT_FILENO, init, sizeof init - 1);
}

static void tty_leave(IfTui *t) {
    static const char fin[] = "\x1b[0m\x1b[?25h\x1b[?1049l";
    (void)!write(STDOUT_FILENO, fin, sizeof fin - 1);
    if (t->raw) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &t->saved); t->raw = false; }
}

static void tty_size(IfTui *t) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
        t->cols = ws.ws_col;
        t->rows = ws.ws_row;
    } else {
        t->cols = 80;
        t->rows = 24;
    }
}

/* ---- SGR 行ペイント ---- */
static void sgr_run(IfTui *t, u8 fg, u8 bg, u8 fl) {
    char seq[48];
    int n = snprintf(seq, sizeof seq, "\x1b[0%s%s%sm%s%s%s%s",
                     (fl & IF_F_BOLD) ? ";1" : "",
                     (fl & IF_F_ITALIC) ? ";3" : "",
                     (fl & IF_F_ULINE) ? ";4" : "",
                     fg == IF_CELL_DEFAULT ? "" : "", "", "", "");
    out_cat(t, seq, (u64)n);
    if (fl & IF_F_STRIKE) out_s(t, "\x1b[9m");
    if (fg != IF_CELL_DEFAULT) { char s2[16]; int m = snprintf(s2, sizeof s2, "\x1b[38;5;%um", fg); out_cat(t, s2, (u64)m); }
    if (bg != IF_CELL_DEFAULT) { char s2[16]; int m = snprintf(s2, sizeof s2, "\x1b[48;5;%um", bg); out_cat(t, s2, (u64)m); }
}

/* grid の 1 行を幅 w で SGR run を繋げて塗る。末尾は既定色+消去で残りを埋める */
static void paint_grid_row(IfTui *t, const IfGrid *g, i32 gy, i32 w) {
    if (!g || gy < 0 || gy >= g->h) return;
    const IfCell *row = g->cells + (u64)gy * (u64)g->w;
    u8 cfg = 250, cbg = 250, cfl = 250; /* あり得ない初期値 */
    i32 x = 0;
    for (i32 gx = 0; gx < g->w && x < w;) {
        const IfCell *c = &row[gx];
        if (c->fg != cfg || c->bg != cbg || c->flags != cfl) {
            sgr_run(t, c->fg, c->bg, c->flags);
            cfg = c->fg; cbg = c->bg; cfl = c->flags;
        }
        if (c->cp == 0) { gx++; continue; } /* 全角継続セルは出さない */
        if (c->cp == ' ') { out_cat(t, " ", 1); gx++; x++; continue; }
        u8 tmp[4];
        u32 n = if_utf8_encode(c->cp, tmp);
        out_cat(t, (const char *)tmp, n);
        i32 cw = if_glyph_width(c->cp);
        gx += cw > 1 ? cw : 1;
        x += cw > 1 ? cw : 1;
    }
}

/* テキストをセル幅制限で出す（UTF-8 安全に打ち切り） */
static i32 paint_text_cells(IfTui *t, const char *s, i32 maxw) {
    i32 w = 0;
    u32 i = 0, total = (u32)strlen(s);
    while (s[i] && w < maxw) {
        u32 pos = 0;
        u32 cp = if_utf8_decode((const u8 *)s + i, total - i, &pos);
        u32 adv = pos;
        if (!adv) adv = 1;
        if (cp == 0xfffd && adv <= 1) { i += adv; continue; } /* 不正バイトは捨てる */
        i32 cw = if_glyph_width(cp);
        if (cw <= 0) { i += adv; continue; }
        if (w + cw > maxw) break;
        out_cat(t, s + i, adv);
        w += cw;
        i += adv;
    }
    return w;
}

/* ---- 画面 ---- */
static void goto_row(IfTui *t, i32 r) { char s[16]; int n = snprintf(s, sizeof s, "\x1b[%d;1H", r + 1); out_cat(t, s, (u64)n); }
static void clear_eol(IfTui *t) { out_s(t, "\x1b[0m\x1b[K"); }

static u64 rss_hwm_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    u64 kb = 0;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, "VmHWM:", 6) == 0) { kb = strtoull(line + 6, NULL, 10); break; }
    fclose(f);
    return kb;
}

static void paint(IfTui *t, IfChrome *c, i32 vh) {
    t->buflen = 0;
    goto_row(t, 0);
    /* tabstrip */
    IfTab *cur = if_chrome_cur(c);
    i32 used = 0;
    for (i32 i = 0; i < c->n_tabs && used < t->cols; i++) {
        bool act = (i == c->active);
        out_s(t, act ? "\x1b[7m" : "\x1b[0m");
        /* letter-tile 代替: タイトル頭 1 文字（favicon REJ の TUI 表現、§8 裁定） */
        char tpfx[8];
        int pn = snprintf(tpfx, sizeof tpfx, " %c%d ", act ? 'I' : ' ', c->tabs[i]->id);
        out_cat(t, tpfx, (u64)pn);
        i32 room = t->cols - used - 22;
        i32 w = room > 0 ? paint_text_cells(t, c->tabs[i]->title ? c->tabs[i]->title : "?", room) : 0;
        used += 5 + w + 2;
        if (used < t->cols) out_n(t, ' ', 2);
    }
    out_s(t, "\x1b[0m");
    clear_eol(t);

    /* omnibox */
    goto_row(t, 1);
    out_s(t, "\x1b[0m");
    out_s(t, "\x1b[1m> \x1b[0m");
    if (c->mode == CM_OMNIBOX) {
        out_s(t, "\x1b[4m");
        paint_text_cells(t, c->omni, t->cols - 8);
        out_s(t, "\x1b[0m\x1b[7m \x1b[0m"); /* カーソル */
    } else if (cur && cur->url[0]) {
        paint_text_cells(t, cur->url, t->cols - 8);
    } else {
        out_s(t, "\x1b[2mnew tab — o: open file\x1b[0m");
    }
    clear_eol(t);

    /* content */
    for (i32 vy = 0; vy < vh; vy++) {
        goto_row(t, CHROME_ROWS_TOP + vy);
        if (cur && cur->grid && cur->scroll + vy < cur->grid->h)
            paint_grid_row(t, cur->grid, cur->scroll + vy, t->cols);
        clear_eol(t);
    }

    /* status */
    goto_row(t, t->rows - 1);
    out_s(t, "\x1b[7m");
    if (c->toast_len) {
        out_cat(t, " ", 1);
        paint_text_cells(t, c->toast, t->cols - 20);
    } else if (cur && cur->grid) {
        i32 maxs = cur->grid->h > vh ? cur->grid->h - vh : 0;
        i32 pct = maxs ? (i32)((i64)cur->scroll * 100 / maxs) : 100;
        out_cat(t, " ", 1);
        out_num(t, pct);
        out_s(t, "% ");
        if (cur->link_idx >= 0 && cur->lay && (u32)cur->link_idx < cur->lay->n_links) {
            out_num(t, cur->link_idx + 1);
            out_cat(t, "/", 1);
            out_num(t, cur->lay->n_links);
            out_cat(t, " ", 1);
            IfStr href = cur->lay->links[cur->link_idx].href;
            char htmp[512];
            u32 hn = href.n < sizeof htmp - 1 ? href.n : (u32)sizeof htmp - 1;
            memcpy(htmp, href.p, hn);
            htmp[hn] = 0;
            paint_text_cells(t, htmp, t->cols / 2);
        }
    } else {
        out_s(t, " Ifuto Browser v0 — o: open · ?: keys");
    }
    /* 右側: タブ文書メモリ（C1 正確計装）＋ RSS */
    out_s(t, "\x1b[0m\x1b[7m");
    u64 db = if_chrome_cur_doc_bytes(c);
    out_s(t, " mem ");
    out_num(t, (i64)(db / 1024));
    out_s(t, "KB rss ");
    out_num(t, (i64)(rss_hwm_kb() / 1024));
    out_s(t, "MB tabs ");
    out_num(t, c->n_tabs);
    out_cat(t, " ", 1);
    clear_eol(t);
    (void)!write(STDOUT_FILENO, t->buf, t->buflen);
}

/* href を現タブのディレクトリ基準で解決して open する */
static void open_href(IfChrome *c, IfTab *cur, i32 vw) {
    if (!cur->lay || cur->link_idx < 0 || (u32)cur->link_idx >= cur->lay->n_links) return;
    IfStr href = cur->lay->links[cur->link_idx].href;
    if (!href.n) return;
    char h[4096];
    u32 hn = href.n < sizeof h - 1 ? href.n : (u32)sizeof h - 1;
    memcpy(h, href.p, hn);
    h[hn] = 0;
    if (h[0] == '#') return;
    if (strstr(h, "://") && strstr(h, "://") - h <= 8) {
        snprintf(c->toast, sizeof c->toast, "network: v0.3 milestone (%s)", h);
        c->toast_len = (u8)strlen(c->toast);
        return;
    }
    char dir[4096] = ".";
    if (cur->url[0]) {
        snprintf(dir, sizeof dir, "%s", cur->url);
        char *sl = strrchr(dir, '/');
        if (sl) *sl = 0; else snprintf(dir, sizeof dir, ".");
    }
    char joined[4096];
    const char *cand = h;
    if (h[0] != '/') { snprintf(joined, sizeof joined, "%s/%s", dir, h); cand = joined; }
    if (!if_chrome_open(c, cand, vw)) {
        snprintf(c->toast, sizeof c->toast, "not found: %.80s", cand);
        c->toast_len = (u8)strlen(c->toast);
    }
}

static void apply_normal(IfChrome *c, IfUiEvent e, i32 vw, i32 vh, bool *running) {
    switch (e.act) {
    case UA_SCROLL_UP: if_chrome_scroll(c, -1, vh); break;
    case UA_SCROLL_DOWN: if_chrome_scroll(c, 1, vh); break;
    case UA_PAGE_UP: if_chrome_scroll(c, -(vh > 1 ? vh - 1 : 1), vh); break;
    case UA_PAGE_DOWN: if_chrome_scroll(c, vh > 1 ? vh - 1 : 1, vh); break;
    case UA_TOP: if_chrome_scroll_to(c, 0, vh); break;
    case UA_BOTTOM: if_chrome_scroll_to(c, 1 << 30, vh); break;
    case UA_LINK_NEXT: if_chrome_link_move(c, 1); break;
    case UA_LINK_PREV: if_chrome_link_move(c, -1); break;
    case UA_OPEN_LINK: {
        IfTab *cur = if_chrome_cur(c);
        if (cur && cur->link_idx >= 0) open_href(c, cur, vw);
        break;
    }
    case UA_OMNIBOX:
        c->mode = CM_OMNIBOX;
        c->omni_len = 0;
        c->omni[0] = 0;
        break;
    case UA_NEW_TAB: if_chrome_new_blank(c); break;
    case UA_CLOSE_TAB: if_chrome_close(c); break;
    case UA_NEXT_TAB: if (c->n_tabs) if_chrome_switch(c, (c->active + 1) % c->n_tabs); break;
    case UA_PREV_TAB: if (c->n_tabs) if_chrome_switch(c, (c->active + c->n_tabs - 1) % c->n_tabs); break;
    case UA_RELOAD: if_chrome_reload(c, vw); break;
    case UA_HELP: c->mode = CM_HELP; break;
    case UA_QUIT: if (if_chrome_quit(c, (i64)time(NULL))) *running = false; break;
    case UA_ESC: {
        IfTab *cur = if_chrome_cur(c);
        if (cur) cur->link_idx = -1;
        c->toast_len = 0;
        c->toast[0] = 0;
        c->quit_armed_at = -1;
        break;
    }
    default:
        if (e.act >= UA_TAB_1 && e.act <= UA_TAB_1 + 8) {
            i32 idx = e.act - UA_TAB_1;
            if (idx < c->n_tabs) if_chrome_switch(c, idx);
        }
        break;
    }
}

static void apply_omnibox(IfTui *t, IfChrome *c, IfUiEvent e, i32 vw) {
    (void)t;
    if (e.act == UA_ESC) { c->mode = CM_NORMAL; return; }
    if (e.act == UA_OPEN_LINK) { /* Enter */
        char resolved[4096];
        i32 rc = if_chrome_resolve(c, c->omni, t->cwd, resolved, sizeof resolved);
        if (rc == 0) { if_chrome_open(c, resolved, vw); return; }
        const char *msg = rc == 1 ? "network: v0.3 milestone" : "not found";
        snprintf(c->toast, sizeof c->toast, "%s", msg);
        c->toast_len = (u8)strlen(c->toast);
        return;
    }
    if (e.act == UA_BACKSPACE) {
        /* UTF-8 の継続バイト（10xxxxxx）を辿って 1 グリフ消す */
        while (c->omni_len > 0 && (c->omni[c->omni_len - 1] & 0xc0) == 0x80)
            c->omni[--c->omni_len] = 0;
        if (c->omni_len > 0) c->omni[--c->omni_len] = 0;
        return;
    }
    if (e.act == UA_CHAR && c->omni_len < IF_OMNI_CAP - 1) {
        c->omni[c->omni_len++] = (char)e.a1;
        c->omni[c->omni_len] = 0;
        return;
    }
}

static const char *HELP_LINES[] = {
    "Ifuto Browser — keys",
    "",
    "  j/k, ↑/↓  scroll    d/u, PgDn/PgUp  page    g/G, Home/End  top/bottom",
    "  Tab/S-Tab next/prev link              Enter  open focused link",
    "  o          omnibox (path open; network is v0.3)     r  reload",
    "  t  new tab (blank)  w  close tab      ]/[    next/prev tab   1..9 jump",
    "  ?  this help        q  quit (x2 if tabs>1)          Esc cancel/back",
    "",
    "press any key",
};

static void paint_help(IfTui *t, i32 vh) {
    goto_row(t, CHROME_ROWS_TOP);
    out_s(t, "\x1b[0m");
    for (i32 i = 0; i < vh && (u32)i < sizeof HELP_LINES / sizeof HELP_LINES[0]; i++) {
        goto_row(t, CHROME_ROWS_TOP + i);
        paint_text_cells(t, HELP_LINES[i], t->cols - 2);
        clear_eol(t);
    }
}

int if_tui_run(const char *initial_path) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fputs("ifuto --ui: interactive terminal required\n", stderr);
        return 2;
    }
    IfTui t;
    memset(&t, 0, sizeof t);
    if (!getcwd(t.cwd, sizeof t.cwd)) snprintf(t.cwd, sizeof t.cwd, ".");
    if_ui_dec_init(&t.dec);

    IfFsOps fs = { if_fs_exists_real, if_fs_read_real, NULL };
    IfChrome c;
    if_chrome_init(&c, fs);

    struct sigaction sa = {0};
    sa.sa_handler = on_winch; /* SA_RESTART なし: read を EINTR で起こす */
    sigaction(SIGWINCH, &sa, NULL);

    tty_enter(&t);
    tty_size(&t);
    i32 vw = t.cols, vh = t.rows - CHROME_ROWS_TOP - CHROME_ROWS_BOTTOM;
    if (initial_path) if_chrome_open(&c, initial_path, vw);
    if (!if_chrome_cur(&c)) if_chrome_new_blank(&c);

    bool running = true;
    while (running) {
        if (g_winch) {
            g_winch = 0;
            tty_size(&t);
            vw = t.cols;
            vh = t.rows - CHROME_ROWS_TOP - CHROME_ROWS_BOTTOM;
            if_chrome_relayout(&c, vw);
        }
        IfTab *cur = if_chrome_cur(&c);
        if (cur && cur->dirty && cur->doc) if_chrome_relayout(&c, vw);
        paint(&t, &c, vh);
        if (c.mode == CM_HELP) paint_help(&t, vh);
        (void)!write(STDOUT_FILENO, t.buf, 0); /* flush 済みだが念のため */

        u8 byte;
        ssize_t r = read(STDIN_FILENO, &byte, 1);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        IfUiEvent e;
        t.dec.literal = (c.mode == CM_OMNIBOX) ? 1 : 0;
        if (!if_ui_dec_feed(&t.dec, byte, &e)) {
            /* ESC 待ちのまま次バイトが 25ms 来なければ単独 ESC と確定する
             * （再描画タイマーではない read 待機なので INV-5 非違反） */
            if (t.dec.state == 1) {
                struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
                if (poll(&pfd, 1, 25) == 0) {
                    t.dec.state = 0;
                    e.act = UA_ESC;
                    e.a1 = 0;
                    goto have_event;
                }
            }
            continue;
        }
        have_event:
        if (e.act == UA_NONE) continue;
        if (c.mode == CM_HELP) { c.mode = CM_NORMAL; continue; }
        if (c.mode == CM_OMNIBOX) apply_omnibox(&t, &c, e, vw);
        else apply_normal(&c, e, vw, vh, &running);
    }

    tty_leave(&t);
    if_chrome_destroy(&c);
    free(t.buf);
    return 0;
}
