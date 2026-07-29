/* Ifuto — TUI キー入力デコーダ（実装） */
#include "ui_input.h"

void if_ui_dec_init(IfUiDecoder *d) { d->n = 0; d->state = 0; d->literal = 0; }

static IfUiEvent ev(IfUiAction a, u8 a1) { IfUiEvent e; e.act = a; e.a1 = a1; return e; }

/* CSI 最終バイトを解釈 */
static IfUiEvent csi_final(const u8 *b, u8 n, u8 fin) {
    /* ESC [ A/B/C/D, ESC [ H/F, ESC [ 5~/6~, ESC [ Z */
    if (n == 0) {
        switch (fin) {
        case 'A': return ev(UA_SCROLL_UP, 0);
        case 'B': return ev(UA_SCROLL_DOWN, 0);
        case 'C': return ev(UA_NONE, 0);
        case 'D': return ev(UA_NONE, 0);
        case 'H': return ev(UA_TOP, 0);
        case 'F': return ev(UA_BOTTOM, 0);
        case 'Z': return ev(UA_LINK_PREV, 0); /* Shift-Tab */
        default: return ev(UA_NONE, 0);
        }
    }
    if (fin == '~') {
        if (n == 1 && b[0] == '5') return ev(UA_PAGE_UP, 0);
        if (n == 1 && b[0] == '6') return ev(UA_PAGE_DOWN, 0);
        if (n == 1 && b[0] == '1') return ev(UA_TOP, 0);
        if (n == 1 && b[0] == '4') return ev(UA_BOTTOM, 0);
    }
    return ev(UA_NONE, 0);
}

bool if_ui_dec_feed(IfUiDecoder *d, u8 c, IfUiEvent *out) {
    if (d->state == 1) { /* ESC 直後 */
        if (c == '[') { d->state = 2; d->n = 0; return false; }
        if (c == 'O') { d->state = 3; return false; } /* SS3 */
        /* 単独 ESC と判断（ALT+key は未サポート） */
        d->state = 0;
        *out = ev(UA_ESC, 0);
        return true;
    }
    if (d->state == 3) { /* SS3: ESC O H/F 等 */
        d->state = 0;
        if (c == 'H') { *out = ev(UA_TOP, 0); return true; }
        if (c == 'F') { *out = ev(UA_BOTTOM, 0); return true; }
        *out = ev(UA_NONE, 0);
        return true;
    }
    if (d->state == 2) { /* CSI 中 */
        if (c >= 0x40 && c <= 0x7e) { /* 最終バイト */
            *out = csi_final(d->buf, d->n, c);
            d->state = 0;
            return true;
        }
        if (d->n < sizeof d->buf) d->buf[d->n++] = c;
        else d->state = 0; /* 長すぎ: 破棄 */
        return false;
    }

    if (c == 0x1b) { d->state = 1; return false; }

    switch (c) {
    case '\t': *out = ev(UA_LINK_NEXT, 0); return true;
    case '\r': case '\n': *out = ev(UA_OPEN_LINK, 0); return true;
    case 0x7f: case 0x08: *out = ev(UA_BACKSPACE, 0); return true;
    }

    if (d->literal && (c >= 0x20 || c >= 0x80)) {
        *out = ev(UA_CHAR, c);
        return true;
    }

    if (c >= '1' && c <= '9') { *out = ev((IfUiAction)(UA_TAB_1 + (c - '1')), 0); return true; }

    switch (c) {
    case 'j': *out = ev(UA_SCROLL_DOWN, 0); return true;
    case 'k': *out = ev(UA_SCROLL_UP, 0); return true;
    case 'd': *out = ev(UA_PAGE_DOWN, 0); return true;
    case 'u': *out = ev(UA_PAGE_UP, 0); return true;
    case 'g': *out = ev(UA_TOP, 0); return true;
    case 'G': *out = ev(UA_BOTTOM, 0); return true;
    case 'o': *out = ev(UA_OMNIBOX, 0); return true;
    case 't': *out = ev(UA_NEW_TAB, 0); return true;
    case 'w': *out = ev(UA_CLOSE_TAB, 0); return true;
    case ']': *out = ev(UA_NEXT_TAB, 0); return true;
    case '[': *out = ev(UA_PREV_TAB, 0); return true;
    case 'r': *out = ev(UA_RELOAD, 0); return true;
    case 'b': *out = ev(UA_BOOKMARK_TOGGLE, 0); return true;
    case 'B': *out = ev(UA_BOOKMARKS, 0); return true;
    case '?': *out = ev(UA_HELP, 0); return true;
    case 'q': *out = ev(UA_QUIT, 0); return true;
    }

    if (c >= 0x20 || c >= 0x80) { /* 印字可能バイト（UTF-8 継続含む） */
        *out = ev(UA_CHAR, c);
        return true;
    }
    *out = ev(UA_NONE, 0);
    return true;
}
