/* Ifuto GUI — X11 コアプロトコル・ミニクライアント（raw socket, libc のみ）
 *
 * Xlib 非依存（ldd 法の維持: libc/libm のみ）。UNIX domain socket /tmp/.X11-unix/Xn
 * に直接コアプロトコルを話す。Xauthority の MIT-MAGIC-COOKIE-1 を読む。
 * 対応: CreateWindow/MapWindow/InternAtom/SetWMProtocols/GetKeyboardMapping/
 *       PutImage(ZPixmap, depth 24/16)/DestroyWindow + イベント (Expose/KeyPress/
 *       ButtonPress/ConfigureNotify/ClientMessage)。
 * 転送は「1 要求 = 1 writev 相当の逐次 write」で同期化（要求の詰めすぎはしない。
 * 返信の必要な要求（InternAtom/GetKeyboardMapping）は即時 read する設計）。 */
#ifndef IFUTO_GUI_X11T_H
#define IFUTO_GUI_X11T_H

#include "../common.h"

typedef struct IfX IfX;

typedef struct {
    u8  kind;     /* IF_XEV_* */
    u8  code;     /* key: keysym-low（0x20-0x7E は ASCII 通し） */
    u32 keysym;   /* KeyPress の keysym（BackSpace 等の特殊値含む） */
    u32 state;    /* modifier mask（Shift=1, Control=4） */
    i32 x, y;     /* ButtonPress/MotionNotify 座標 / ConfigureNotify は (w,h) */
    u32 aux;      /* ClientMessage の atom 等 */
} IfXev;

enum {
    IF_XEV_NONE = 0, IF_XEV_EXPOSE, IF_XEV_KEY, IF_XEV_BUTTON,
    IF_XEV_CONFIGURE, IF_XEV_CLIENTMSG, IF_XEV_MOTION
};

/* キーシム（X 規格の固定値のうち使用するもの） */
enum {
    XK_BACKSPACE = 0xFF08, XK_TAB = 0xFF09, XK_RETURN = 0xFF0D, XK_ESCAPE = 0xFF1B,
    XK_DELETE = 0xFFFF, XK_LEFT = 0xFF51, XK_UP = 0xFF52, XK_RIGHT = 0xFF53,
    XK_DOWN = 0xFF54, XK_PRIOR = 0xFF55, XK_NEXT = 0xFF56,
    XK_ISO_LEFTTAB = 0xFE20 /* Shift+Tab は多くのキーマップでこのシムを返す */
};

/* 接続。$DISPLAY（":0" 形）から接続先を決める。失敗時 NULL + stderr に理由 */
IfX *x11_open(void);
void x11_close(IfX *x);

/* 窓: 生成（白背景）→ Map。イベントマスクは Expose/KeyPress/ButtonPress/Structure */
u32 x11_window(IfX *x, u32 w, u32 h, const char *title);
void x11_map(IfX *x, u32 win);
void x11_destroy(IfX *x, u32 win);

/* ピクセル転送（データは 0xRRGGBB の 32bit 配列, X の深度変換はここで行う。
 * 1 要求の最大長は setup 値でクリップ（w*h が大きいと false） */
bool x11_put_image(IfX *x, u32 win, i32 dst_x, i32 dst_y, u32 w, u32 h, const u32 *rgb);

/* CopyArea（同一 drawable の矩形コピー。差分スクロール: 重なり領域の
 * サーバ側シフトに使う。PutImage 共用の既定 GC で function=copy） */
bool x11_copy_area(IfX *x, u32 win, i32 src_x, i32 src_y, i32 dst_x, i32 dst_y,
                   u32 w, u32 h);

/* 1 イベント受信（ブロッキング）。他イベントは吸収して構わない（Expose の統合は呼出側） */
bool x11_next_event(IfX *x, IfXev *ev);

u32 x11_atom_wm_delete(IfX *x); /* WM_DELETE_WINDOW（初回で intern+protocols 設定済の前提で保持） */
u32 x11_max_request_payload(const IfX *x);

#endif
