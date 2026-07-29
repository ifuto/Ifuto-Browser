/* Ifuto — TUI キー入力デコーダ（純粋・tty 非依存。単体テスト可能）
 *
 * バイト列を「アクション」に単射する小さな状態機械。
 * エスケープシーケンス（CSI/SS3）は内部バッファに保留する。未知列は読み捨て。
 */
#ifndef IFUTO_UI_INPUT_H
#define IFUTO_UI_INPUT_H

#include "common.h"

typedef enum {
    UA_NONE = 0,
    UA_SCROLL_UP, UA_SCROLL_DOWN, UA_PAGE_UP, UA_PAGE_DOWN, UA_TOP, UA_BOTTOM,
    UA_LINK_NEXT, UA_LINK_PREV, UA_OPEN_LINK,
    UA_OMNIBOX, UA_NEW_TAB, UA_CLOSE_TAB, UA_NEXT_TAB, UA_PREV_TAB,
    UA_TAB_1, /* … UA_TAB_9 = UA_TAB_1 + 8 */
    UA_RELOAD, UA_HELP, UA_QUIT, UA_ESC,
    UA_CHAR,      /* omnibox 編集用: a1 にバイト値（UTF-8 はバイト列で届く） */
    UA_BACKSPACE
} IfUiAction;

typedef struct {
    IfUiAction act;
    u8 a1; /* UA_CHAR: バイト値 / UA_TAB_n: 番号用途なし（act+8 範囲） */
} IfUiEvent;

/* 1 バイト喰わせる。イベント完成で *ev を埋めて true。シーケンス途中は false。 */
typedef struct {
    u8 buf[8];   /* ESC 系列の保留バイト */
    u8 n;
    u8 state;    /* 0=通常 1=ESC 後 2=CSI 中 */
    u8 literal;  /* 1=オムニボックス編集中: 印字可能キーはすべて UA_CHAR */
} IfUiDecoder;

void if_ui_dec_init(IfUiDecoder *d);
bool if_ui_dec_feed(IfUiDecoder *d, u8 byte, IfUiEvent *ev);

#endif
