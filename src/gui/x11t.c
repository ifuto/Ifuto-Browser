/* Ifuto GUI — X11 コアプロトコル・ミニクライアント実装。
 * 全リクエストはリトルエンディアン ('l') で送る。x86_64ホスト前提だが
 * LE シリアライズは逐バイト組み立てでホスト依存を排除している。 */
#include "x11t.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

/* 単一クライアント設計の GC 保持（CreateWindow 時に 1 本だけ作る） */
static u32 g_x11_gc;

struct IfX {
    int fd;
    u32 rid_base, rid_mask, rid_seq;
    u32 root, root_depth, root_visual, white, black;
    u32 max_req_len; /* setup の maximum-request-length（4byte 単位） */
    u8  bpp;         /* 24 または 16 */
    u8  kmin, kmax;
    u8  kper;        /* keysyms per keycode */
    u32 *kmap; u32 n_kmap; /* (kmax-kmin+1)*kper */
    u32 atom_wm_protocols, atom_wm_delete;
    u16 seq;
    char err[128];
};

/* ---- LE 直列化 ---- */
static void p16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void p32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u16 g16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 g32(const u8 *p) { return (u32)(p[0] | (p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24)); }

static bool wr_all(IfX *x, const u8 *buf, u64 n) {
    u64 off = 0;
    while (off < n) {
        ssize_t w = write(x->fd, buf + off, (size_t)(n - off));
        if (w < 0) { if (errno == EINTR) continue; return false; }
        off += (u64)w;
    }
    return true;
}
static bool rd_all(IfX *x, u8 *buf, u64 n) {
    u64 off = 0;
    while (off < n) {
        ssize_t r = read(x->fd, buf + off, (size_t)(n - off));
        if (r < 0) { if (errno == EINTR) continue; return false; }
        if (r == 0) return false;
        off += (u64)r;
    }
    return true;
}

/* エラー応答（type 0）かどうかを返信先頭で捌く。要求ごとの逐次 read 設計 */
static bool rd_reply32(IfX *x, u8 *hdr, const char *what) {
    if (!rd_all(x, hdr, 32)) { snprintf(x->err, sizeof x->err, "x11: %s: read failed", what); return false; }
    if (hdr[0] == 0) {
        snprintf(x->err, sizeof x->err, "x11: %s: X error code=%u major=%u minor=%u",
                 what, hdr[1], hdr[10], hdr[11]);
        return false;
    }
    return true;
}

/* ---- Xauthority ---- */
typedef struct { u8 data[16]; bool ok; } XCookie;

/* ~/.Xauthority（または $XAUTHORITY）から display 番号に一致する
 * MIT-MAGIC-COOKIE-1 を拾う。レコード: family(2) alen(2) addr nlen(2) num dlen(2) name vlen(2) data */
static XCookie read_xauth(const char *dispnum) {
    XCookie c; c.ok = false;
    const char *path = getenv("XAUTHORITY");
    char fallback[1024];
    if (!path) {
        const char *home = getenv("HOME");
        if (!home) return c;
        snprintf(fallback, sizeof fallback, "%s/.Xauthority", home);
        path = fallback;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return c;
    u8 buf[4096];
    while (1) {
        u8 hdr[4];
        if (fread(hdr, 1, 2, f) != 2) break; /* family(2) だけ流用: 先に family を読む */
        fseek(f, -2, SEEK_CUR);
        /* family */
        if (fread(hdr, 1, 2, f) != 2) break;
        (void)g16; (void)hdr;
        /* addr */
        if (fread(hdr, 1, 2, f) != 2) break;
        u16 alen = (u16)((hdr[0] << 8) | hdr[1]); /* Xauthority は BIG ENDIAN! */
        if (fseek(f, alen, SEEK_CUR)) break;
        /* conn number */
        if (fread(hdr, 1, 2, f) != 2) break;
        u16 nlen = (u16)((hdr[0] << 8) | hdr[1]);
        if ((int)fread(buf, 1, nlen > sizeof buf ? sizeof buf : nlen, f) != (int)nlen) break;
        bool num_match = (nlen == strlen(dispnum)) && !memcmp(buf, dispnum, nlen);
        /* name */
        if (fread(hdr, 1, 2, f) != 2) break;
        u16 nmlen = (u16)((hdr[0] << 8) | hdr[1]);
        if ((int)fread(buf, 1, nmlen > sizeof buf ? sizeof buf : nmlen, f) != (int)nmlen) break;
        bool is_cookie = (nmlen == 18) && !memcmp(buf, "MIT-MAGIC-COOKIE-1", 18);
        /* data */
        if (fread(hdr, 1, 2, f) != 2) break;
        u16 dlen = (u16)((hdr[0] << 8) | hdr[1]);
        if (dlen > 4096) break;
        if ((int)fread(buf, 1, dlen, f) != (int)dlen) break;
        if (is_cookie && num_match && dlen == 16) {
            memcpy(c.data, buf, 16);
            c.ok = true;
            break;
        }
    }
    fclose(f);
    return c;
}

IfX *x11_open(void) {
    const char *disp = getenv("DISPLAY");
    if (!disp || !*disp) { fputs("x11: $DISPLAY is not set\n", stderr); return NULL; }
    /* ":N[.S]" or "unix:N" or "hostname:N"（N のみ採る。ローカル socket 前提） */
    const char *colon = strrchr(disp, ':');
    if (!colon) { fputs("x11: bad $DISPLAY\n", stderr); return NULL; }
    char num[16];
    u32 i = 0;
    for (const char *p = colon + 1; *p && *p != '.' && i < sizeof num - 1; p++) num[i++] = *p;
    num[i] = 0;

    char sockpath[128];
    snprintf(sockpath, sizeof sockpath, "/tmp/.X11-unix/X%s", num);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("x11: socket"); return NULL; }
    struct sockaddr_un un;
    memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof un.sun_path, "%s", sockpath);
    if (connect(fd, (struct sockaddr *)&un, sizeof un) != 0) {
        fprintf(stderr, "x11: cannot connect to %s (%s)\n", sockpath, strerror(errno));
        close(fd);
        return NULL;
    }
    IfX *x = (IfX *)calloc(1, sizeof *x);
    if (!x) { close(fd); return NULL; }
    x->fd = fd;

    /* setup request */
    XCookie ck = read_xauth(num);
    u8 req[12 + 20 + 16]; /* hdr + "MIT-MAGIC-COOKIE-1"(pad 20) + 16B */
    memset(req, 0, sizeof req);
    req[0] = 'l';
    p16(req + 2, 11);
    p16(req + 4, 0);
    u16 an = 0, ad = 0;
    if (ck.ok) { an = 18; ad = 16; }
    p16(req + 6, an);
    p16(req + 8, ad);
    u32 rlen = 12;
    if (an) {
        memcpy(req + rlen, "MIT-MAGIC-COOKIE-1", 18);
        rlen += (u32)((18 + 3) & ~3);
        memcpy(req + rlen, ck.data, 16);
        rlen += 16;
    }
    if (!wr_all(x, req, rlen)) { fputs("x11: setup write failed\n", stderr); goto fail; }
    u8 hdr[8];
    if (!rd_all(x, hdr, 8)) { fputs("x11: setup read failed\n", stderr); goto fail; }
    if (hdr[0] != 1) {
        u16 addlen = g16(hdr + 6) * 4;
        char *reason = (char *)malloc((size_t)addlen + 1);
        reason[0] = 0;
        if (addlen && rd_all(x, (u8 *)reason + 0, addlen)) reason[addlen] = 0;
        fprintf(stderr, "x11: connection refused (need cookie?): %.*s\n",
                (int)(addlen < 121 ? addlen : 121), reason);
        free(reason);
        goto fail;
    }
    u32 body = (u32)g16(hdr + 6) * 4;
    if (body < 32) { fputs("x11: short setup body\n", stderr); goto fail; }
    u8 *bb = (u8 *)malloc(body);
    if (!bb) goto fail;
    if (!rd_all(x, bb, body)) { free(bb); fputs("x11: setup body read failed\n", stderr); goto fail; }
    x->rid_base = g32(bb + 4);
    x->rid_mask = g32(bb + 8);
    x->max_req_len = g16(bb + 16) ? g16(bb + 16) : 65535;
    u8 nscreens = bb[20], nformats = bb[21];
    x->kmin = bb[29]; x->kmax = bb[30];
    u32 vend = g16(bb + 24);
    u32 off = 32 + ((vend + 3) & ~3u);
    /* pixmap formats: depth 24 or 16 を探す */
    x->bpp = 0;
    for (u32 fi = 0; fi < nformats; fi++) {
        u8 d = bb[off + fi * 8], b2 = bb[off + fi * 8 + 1];
        if ((d == 24 && b2 == 32) || (d == 16 && b2 == 16)) { if (!x->bpp) x->bpp = d; }
    }
    off += (u32)nformats * 8;
    if (off + 40 > body) { free(bb); fputs("x11: no screen in setup\n", stderr); goto fail; }
    x->root = g32(bb + off);
    x->white = g32(bb + off + 8);
    x->black = g32(bb + off + 12);
    x->root_visual = g32(bb + off + 32);
    x->root_depth = bb[off + 38];
    (void)nscreens;
    free(bb);
    if (!x->bpp) { fprintf(stderr, "x11: unsupported visual depths (need 16/24)\n"); goto fail; }
    if (x->root_depth != x->bpp) {
        fprintf(stderr, "x11: root depth %u unsupported bpp config (want %u)\n",
                x->root_depth, x->bpp);
        goto fail;
    }
    return x;
fail:
    close(x->fd); free(x);
    return NULL;
}

void x11_close(IfX *x) {
    if (!x) return;
    close(x->fd);
    free(x->kmap);
    free(x);
}

static u32 x11_gen_id(IfX *x) { return x->rid_base | (x->rid_seq++ & x->rid_mask); }

/* 要求に応答がない種類は投げる（エラーは非同期的に来るが v0.2 は致命度の高い
 * CreateWindow/InternAtom/GetKeyboardMapping だけ同期確認する） */

u32 x11_window(IfX *x, u32 w, u32 h, const char *title) {
    u32 win = x11_gen_id(x), gc = x11_gen_id(x);
    u8 req[32 + 8 + 32 + 256];
    memset(req, 0, sizeof req);
    req[0] = 1; /* CreateWindow */
    p16(req + 2, 8 + 2);
    p32(req + 4, win);
    p32(req + 8, x->root);
    p16(req + 12, 0); p16(req + 14, 0);
    p16(req + 16, (u16)w); p16(req + 18, (u16)h);
    p16(req + 20, 0);
    p16(req + 22, 1); /* InputOutput */
    p32(req + 24, 0); /* CopyFromParent visual */
    p32(req + 28, 0x1 | 0x800); /* CWBackPixel | CWEventMask */
    p32(req + 32, x->white);
    p32(req + 36, 0x1 /*KeyPress*/ | 0x4 /*ButtonPress*/ | 0x8000 /*Exposure*/ |
                   0x20000 /*StructureNotify*/);
    if (!wr_all(x, req, 40)) goto bad;
    /* ChangeProperty: WM_NAME (STRING=31) */
    {
        u32 tn = (u32)strlen(title);
        u8 *r2 = req;
        memset(r2, 0, 32 + 256);
        r2[0] = 18; /* ChangeProperty, mode Replace */
        p16(r2 + 2, (u16)(6 + (tn + 3) / 4));
        p32(r2 + 4, win);
        p32(r2 + 8, 39); /* WM_NAME */
        p32(r2 + 12, 31); /* STRING */
        r2[16] = 8;
        p32(r2 + 20, tn);
        memcpy(r2 + 24, title, tn);
        if (!wr_all(x, r2, 24 + ((tn + 3) & ~3u))) goto bad;
    }
    /* InternAtom: WM_PROTOCOLS / WM_DELETE_WINDOW */
    const char *names[2] = { "WM_PROTOCOLS", "WM_DELETE_WINDOW" };
    u32 atoms[2] = { 0, 0 };
    for (int k = 0; k < 2; k++) {
        u32 nn = (u32)strlen(names[k]);
        u8 *r3 = req;
        memset(r3, 0, 32 + 256);
        r3[0] = 16; /* InternAtom */
        p16(r3 + 2, (u16)(2 + (nn + 3) / 4));
        p16(r3 + 4, (u16)nn);
        memcpy(r3 + 8, names[k], nn);
        if (!wr_all(x, r3, 8 + ((nn + 3) & ~3u))) goto bad;
        u8 hdr[32];
        if (!rd_reply32(x, hdr, "InternAtom")) { fputs(x->err, stderr); fputc('\n', stderr); goto bad; }
        atoms[k] = g32(hdr + 8);
        u16 extra = g16(hdr + 6) * 4;
        if (extra) { /* 追加データ（普通 0） */
            u8 junk[256];
            while (extra) {
                u16 chunk = extra > sizeof junk ? sizeof junk : extra;
                if (!rd_all(x, junk, chunk)) goto bad;
                extra = (u16)(extra - chunk);
            }
        }
    }
    x->atom_wm_protocols = atoms[0];
    x->atom_wm_delete = atoms[1];
    /* SetWMProtocols */
    {
        u8 *r4 = req;
        memset(r4, 0, 64);
        r4[0] = 18;
        p16(r4 + 2, 7);
        p32(r4 + 4, win);
        p32(r4 + 8, x->atom_wm_protocols);
        p32(r4 + 12, 4); /* ATOM */
        r4[16] = 32;
        p32(r4 + 20, 1);
        p32(r4 + 24, x->atom_wm_delete);
        if (!wr_all(x, r4, 28)) goto bad;
    }
    /* CreateGC（イベント/画像転送用。値は既定） */
    {
        u8 *r5 = req;
        memset(r5, 0, 32);
        r5[0] = 55;
        p16(r5 + 2, 4);
        p32(r5 + 4, gc);
        p32(r5 + 8, win);
        p32(r5 + 12, 0);
        if (!wr_all(x, r5, 16)) goto bad;
    }
    /* GetKeyboardMapping（keysym 解決に必須） */
    {
        u8 *r6 = req;
        memset(r6, 0, 32);
        r6[0] = 101;
        p16(r6 + 2, 2);
        r6[4] = x->kmin;
        r6[5] = (u8)(x->kmax - x->kmin + 1);
        if (!wr_all(x, r6, 8)) goto bad;
        u8 hdr[32];
        if (!rd_reply32(x, hdr, "GetKeyboardMapping")) { fputs(x->err, stderr); fputc('\n', stderr); goto bad; }
        u32 total = g16(hdr + 6) * 4;
        u8 kper = hdr[1];
        u8 *data = (u8 *)malloc(total ? total : 1);
        if (!rd_all(x, data, total)) { free(data); goto bad; }
        x->kper = kper;
        x->n_kmap = total / 4;
        x->kmap = (u32 *)malloc(x->n_kmap ? x->n_kmap * 4 : 4);
        for (u32 k2 = 0; k2 < x->n_kmap; k2++) x->kmap[k2] = g32(data + k2 * 4);
        free(data);
    }
    g_x11_gc = gc; /* PutImage が共用する GC（DestroyWindow でサーバ側に残置可の設計） */
    return win;
bad:
    return 0;
}

void x11_map(IfX *x, u32 win) {
    u8 r[8];
    memset(r, 0, 8);
    r[0] = 8; /* MapWindow */
    p16(r + 2, 2);
    p32(r + 4, win);
    wr_all(x, r, 8);
    /* 初回 Expose を促す窓 configure 等は不要（サーバが Expose を投げる） */
}

void x11_destroy(IfX *x, u32 win) {
    u8 r[8];
    memset(r, 0, 8);
    r[0] = 4;
    p16(r + 2, 2);
    p32(r + 4, win);
    wr_all(x, r, 8);
}

u32 x11_atom_wm_delete(IfX *x) { return x->atom_wm_delete; }
u32 x11_max_request_payload(const IfX *x) { return x->max_req_len * 4; }

/* ピクセル 0xRRGGBB 配列 → X の depth 24 (BGRX) / 16 (RGB565) に並べ替えて PutImage */
bool x11_put_image(IfX *x, u32 win, i32 dst_x, i32 dst_y, u32 w, u32 h, const u32 *rgb) {
    u32 bytes_per_px = x->bpp == 24 ? 4 : 2;
    u64 payload = (u64)w * h * bytes_per_px;
    u64 pad = (4 - (payload & 3)) & 3;
    u64 total_body = payload + pad;
    u64 max_body = (u64)x->max_req_len * 4 - 32;
    if (total_body > max_body) return false; /* 呼出側でストリップ分割 */
    u64 reqn = 24 + total_body;
    u8 *req = (u8 *)malloc(reqn);
    if (!req) return false;
    memset(req, 0, reqn);
    req[0] = 72; /* PutImage */
    req[1] = 2;  /* ZPixmap */
    p16(req + 2, (u16)(6 + total_body / 4));
    p32(req + 4, win);
    p32(req + 8, g_x11_gc);
    p16(req + 12, (u16)w); p16(req + 14, (u16)h);
    p16(req + 16, (u16)dst_x); p16(req + 18, (u16)dst_y);
    req[20] = 0;    /* left_pad */
    req[21] = x->bpp;
    u8 *dst = req + 24;
    if (x->bpp == 24) {
        for (u64 i = 0; i < (u64)w * h; i++) {
            u32 c = rgb[i];
            dst[i * 4 + 0] = (u8)c;         /* B */
            dst[i * 4 + 1] = (u8)(c >> 8);  /* G */
            dst[i * 4 + 2] = (u8)(c >> 16); /* R */
            dst[i * 4 + 3] = 0;
        }
    } else {
        for (u64 i = 0; i < (u64)w * h; i++) {
            u32 c = rgb[i];
            u16 v = (u16)(((c >> 19) & 0x1F) << 11) | (u16)(((c >> 10) & 0x3F) << 5) |
                    (u16)((c >> 3) & 0x1F);
            p16(dst + i * 2, v);
        }
    }
    bool ok = wr_all(x, req, reqn);
    free(req);
    return ok;
}

bool x11_copy_area(IfX *x, u32 win, i32 src_x, i32 src_y, i32 dst_x, i32 dst_y,
                   u32 w, u32 h) {
    u8 req[32];
    memset(req, 0, sizeof req);
    req[0] = 62; /* CopyArea */
    p16(req + 2, 7); /* 28 bytes */
    p32(req + 4, win); p32(req + 8, win);
    p32(req + 12, g_x11_gc);
    p16(req + 16, (u16)src_x); p16(req + 18, (u16)src_y);
    p16(req + 20, (u16)dst_x); p16(req + 22, (u16)dst_y);
    p16(req + 24, (u16)w); p16(req + 26, (u16)h);
    return wr_all(x, req, 28);
}

static u32 keycode_to_keysym(IfX *x, u8 code, u32 state) {
    if (code < x->kmin || code > x->kmax || !x->kmap || !x->kper) return 0;
    u32 base = (u32)(code - x->kmin) * x->kper;
    if (base >= x->n_kmap) return 0;
    /* shift 位置: X 規則では index1 が shifted。簡約: shift 時は index1 があれば採る */
    u32 idx = base;
    if ((state & 1) && x->kper >= 2 && x->kmap[base + 1]) idx = base + 1;
    if (idx >= x->n_kmap) return 0;
    return x->kmap[idx];
}

bool x11_next_event(IfX *x, IfXev *ev) {
    memset(ev, 0, sizeof *ev);
    ev->kind = IF_XEV_NONE;
    u8 buf[32];
    for (;;) {
        if (!rd_all(x, buf, 32)) return false;
        u8 type = buf[0] & 0x7F;
        if (type == 0) { /* エラー非同期受信 */
            fprintf(stderr, "x11: async error code=%u major=%u minor=%u\n",
                    buf[1], buf[10], buf[11]);
            continue;
        }
        switch (type) {
        case 2: { /* KeyPress */
            u8 code = buf[1];
            u32 state = g32(buf + 28) & 0xFF;
            ev->kind = IF_XEV_KEY;
            ev->state = state;
            ev->keysym = keycode_to_keysym(x, code, state);
            if (ev->keysym >= 0x20 && ev->keysym <= 0x7E) ev->code = (u8)ev->keysym;
            return true;
        }
        case 4: /* ButtonPress */
            ev->kind = IF_XEV_BUTTON;
            ev->code = buf[1];
            ev->x = (short)g16(buf + 24);
            ev->y = (short)g16(buf + 26);
            return true;
        case 12: /* Expose */
            ev->kind = IF_XEV_EXPOSE;
            ev->x = (short)g16(buf + 8);
            ev->y = (short)g16(buf + 10);
            return true;
        case 22: /* ConfigureNotify */
            ev->kind = IF_XEV_CONFIGURE;
            ev->x = (short)g16(buf + 16);
            ev->y = (short)g16(buf + 18);
            return true;
        case 33: /* ClientMessage */
            if (g32(buf + 8) == x->atom_wm_protocols) {
                ev->kind = IF_XEV_CLIENTMSG;
                ev->aux = g32(buf + 12);
                return true;
            }
            break;
        default:
            break; /* 他用マスク外のはずだが来たら吸収 */
        }
    }
}
