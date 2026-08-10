#define _POSIX_C_SOURCE 200809L /* getaddrinfo, poll */
#include "net.h"
#include "tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <netdb.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>

static const char *E_URL = "bad url";
static const char *E_DNS = "dns";
static const char *E_CONN = "connect";
static const char *E_SEND = "send";
static const char *E_RECV = "recv";
static const char *E_BIG = "too large";
static const char *E_RESP = "bad response";
static const char *E_TRUNC = "truncated";
static const char *E_LOOP = "redirect loop";
static const char *E_PRIV = "private redirect blocked"; /* DNS rebinding / SSRF 対策（v0.5） */
static const char *E_DOWNGRADE = "https downgrade blocked"; /* https→http 降格（v0.5） */
static const char *E_TLS = "tls";
static const char *E_CERT = "cert";
static const char *E_CA = "ca";

/* ---- URL 分解 ---- */

bool if_http_parse_url(const char *url, IfHttpUrl *out) {
    if (!url) return false;
    bool tls;
    if (strncmp(url, "http://", 7) == 0) {
        tls = false;
    } else if (strncmp(url, "https://", 8) == 0) {
        tls = true;
    } else {
        return false;
    }
    const char *p = url + (tls ? 8 : 7);
    memset(out, 0, sizeof *out);
    out->tls = tls;
    out->port = tls ? 443 : 80;
    /* fragment はここで切る（以後一切見ない） */
    const char *frag = strchr(p, '#');
    size_t rem = frag ? (size_t)(frag - p) : strlen(p);
    if (rem == 0) return false;
    /* host 部: '/', '?' の手前まで */
    size_t hl = 0;
    while (hl < rem && p[hl] != '/' && p[hl] != '?') hl++;
    /* host[:port] 分離（':' は host 部内の最初のもののみ意味を持つ。
     * ':' がある = IPv6 bracket も userinfo 由来も全部 port 検査で落ちる） */
    size_t colon = hl;
    for (size_t i = 0; i < hl; i++)
        if (p[i] == ':') { colon = i; break; }
    size_t hostlen = colon;
    if (hostlen == 0 || hostlen >= sizeof out->host) return false;
    for (size_t i = 0; i < hostlen; i++) {
        u8 c = (u8)p[i];
        /* 構造だけ見る（厳密な DNS 名検査は resolver に委譲）。
         * '@'(userinfo) と空白類と ':' は受理しない */
        if (c <= 0x20 || c == '/' || c == '?' || c == '#' || c == '@' || c == ':')
            return false;
    }
    memcpy(out->host, p, hostlen);
    out->host[hostlen] = 0;
    if (colon < hl) {
        size_t d0 = colon + 1, nd = hl - d0;
        if (nd == 0 || nd > 5) return false;
        u32 v = 0;
        for (size_t i = d0; i < hl; i++) {
            if (p[i] < '0' || p[i] > '9') return false;
            v = v * 10 + (u32)(p[i] - '0');
        }
        if (v == 0 || v > 65535) return false;
        out->port = (u16)v;
        out->has_port = true;
    }
    /* path: '/' から。'?' で始まる場合は "/?" に正規化。無指定は "/" */
    const char *q = p + hl;
    size_t qlen = rem - hl;
    if (qlen == 0) {
        out->path[0] = '/';
        out->path[1] = 0;
        return true;
    }
    size_t off = 0;
    if (q[0] == '?') {
        if (1 + qlen >= sizeof out->path) return false;
        out->path[0] = '/';
        off = 1;
    } else if (q[0] != '/') {
        return false;
    }
    if (off + qlen >= sizeof out->path) return false;
    memcpy(out->path + off, q, qlen);
    out->path[off + qlen] = 0;
    return true;
}

/* ---- URL 解決（redirect 用の最小 RFC3986 5.2/5.3） ---- */

/* base の scheme+authority 長（"http://h:8080" まで） */
static size_t base_origin_len(const char *base) {
    size_t i = 7, n = strlen(base);
    while (i < n && base[i] != '/' && base[i] != '?' && base[i] != '#') i++;
    return i;
}

bool if_http_resolve_url(const char *base, const char *loc, char *out, size_t cap) {
    if (!base || !loc || !out || cap < 16) return false;
    while (*loc == ' ' || *loc == '\t') loc++; /* 前後空白の宽容 */
    size_t ln = strlen(loc);
    while (ln && (loc[ln - 1] == ' ' || loc[ln - 1] == '\t' ||
                  loc[ln - 1] == '\r' || loc[ln - 1] == '\n')) ln--;
    if (ln == 0) return false;
    bool tls = strncmp(base, "https://", 8) == 0;
    const char *sch = tls ? "https://" : "http://";
    if (strncmp(base, sch, tls ? 8 : 7) != 0) return false;
    int w;
    if (ln >= (tls ? 8 : 7) && strncmp(loc, sch, tls ? 8 : 7) == 0) {
        w = snprintf(out, cap, "%.*s", (int)ln, loc);
    } else if (ln >= (tls ? 8 : 7) && strncmp(loc, tls ? "http://" : "https://", tls ? 7 : 8) == 0) {
        /* scheme 変更（http<->https）は現行は追わない（正直な拒否） */
        return false;
    } else if (ln >= 2 && loc[0] == '/' && loc[1] == '/') {
        w = snprintf(out, cap, "%s%.*s", sch, (int)(ln - 2), loc + 2);
    } else if (loc[0] == '/') {
        size_t o = base_origin_len(base);
        w = snprintf(out, cap, "%.*s%.*s", (int)o, base, (int)ln, loc);
    } else if (loc[0] == '?') {
        /* クエリ置換: base の path までを保持（path 無しなら "/" 補完） */
        size_t o = base_origin_len(base), i = o, n = strlen(base);
        while (i < n && base[i] != '?' && base[i] != '#') i++;
        if (i == o)
            w = snprintf(out, cap, "%.*s/%.*s", (int)o, base, (int)ln, loc);
        else
            w = snprintf(out, cap, "%.*s%.*s", (int)i, base, (int)ln, loc);
    } else {
        /* 先頭セグメント内の ':' = scheme 付き absolute-URI（RFC3986 §4.2:
         * relative-ref の先頭セグメントは ':' を含めない）。http:// は上の
         * 第一分岐で受理済みなので、ここに来た scheme 付きは全て拒否 */
        for (size_t k = 0; k < ln && loc[k] != '/' && loc[k] != '?'; k++)
            if (loc[k] == ':') return false;
        /* 相対: base の query/fragment を落とし、path 最終セグメントを置換 */
        size_t o = base_origin_len(base), i = o, n = strlen(base);
        while (i < n && base[i] != '?' && base[i] != '#') i++;
        if (i == o) { /* path 無し */
            w = snprintf(out, cap, "%.*s/%.*s", (int)o, base, (int)ln, loc);
        } else {
            size_t sl = i;
            while (sl > o && base[sl - 1] != '/') sl--;
            w = snprintf(out, cap, "%.*s%.*s", (int)sl, base, (int)ln, loc);
        }
    }
    if (w < 0 || (size_t)w >= cap) return false;
    if (tls) return strncmp(out, "https://", 8) == 0 && out[8] != 0;
    return strncmp(out, "http://", 7) == 0 && out[7] != 0;
}

/* ---- 応答ヘッダ解析（純粋関数） ---- */

static const u8 *find_lf(const u8 *p, const u8 *end) {
    while (p < end && *p != '\n') p++;
    return p;
}

static bool ci_eq(const u8 *p, const u8 *end, const char *name, size_t nl) {
    if ((size_t)(end - p) != nl) return false;
    for (size_t i = 0; i < nl; i++) {
        u8 c = p[i];
        if (c >= 'A' && c <= 'Z') c = (u8)(c + 32);
        char d = name[i];
        if (d >= 'A' && d <= 'Z') d = (char)(d + 32);
        if (c != (u8)d) return false;
    }
    return true;
}

static bool ci_mem(const u8 *p, const u8 *end, const char *pat) {
    size_t nl = strlen(pat);
    while ((size_t)(end - p) >= nl) {
        if (ci_eq(p, p + nl, pat, nl)) return true;
        p++;
    }
    return false;
}

bool if_http_head_parse(const u8 *buf, u64 n, IfHttpHead *out) {
    if (!buf || n < 14) return false;
    memset(out, 0, sizeof *out);
    out->content_length = UINT64_MAX;
    /* 状態行: "HTTP/1." digit SP 3digit */
    if (memcmp(buf, "HTTP/1.", 7) != 0) return false;
    u64 i = 7;
    if (buf[i] < '0' || buf[i] > '9') return false;
    i++;
    while (i < n && buf[i] != ' ' && buf[i] != '\r' && buf[i] != '\n') i++;
    if (i >= n || buf[i] != ' ') return false;
    i++;
    if (i + 3 > n) return false;
    u32 st = 0;
    for (int k = 0; k < 3; k++) {
        u8 c = buf[i + (u64)k];
        if (c < '0' || c > '9') return false;
        st = st * 10 + (u32)(c - '0');
    }
    out->status = st;
    /* 状態行の残りを捨てる */
    const u8 *end = buf + n;
    const u8 *cur = find_lf(buf + i, end);
    if (cur == end) return false; /* 状態行すら未完 */
    cur++;
    while (cur < end) {
        const u8 *lf = find_lf(cur, end);
        const u8 *le = lf; /* 行末（\n 位置 or end） */
        /* 空行 = ヘッダ終端 */
        if (le == cur || (le == cur + 1 && *cur == '\r')) {
            out->body_off = lf < end ? (u64)(lf + 1 - buf) : n;
            return true;
        }
        if (le == end) return false; /* 空行無しに尽きた = 未完 */
        const u8 *colon = cur;
        while (colon < le && *colon != ':') colon++;
        if (colon < le && colon > cur) {
            const u8 *v = colon + 1;
            while (v < le && (*v == ' ' || *v == '\t')) v++;
            const u8 *ve = le;
            while (ve > v && (ve[-1] == '\r' || ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            size_t nl = (size_t)(colon - cur);
            if (nl == 14 && ci_eq(cur, colon, "content-length", 14)) {
                if (out->content_length == UINT64_MAX) { /* 先勝ち */
                    u64 cl = 0;
                    const u8 *d = v;
                    if (d == ve) return false;
                    while (d < ve) {
                        if (*d < '0' || *d > '9') return false;
                        if (cl > (UINT64_MAX - 9) / 10) return false;
                        cl = cl * 10 + (u64)(*d - '0');
                        d++;
                    }
                    out->content_length = cl;
                }
            } else if (nl == 17 && ci_eq(cur, colon, "transfer-encoding", 17)) {
                if (ci_mem(v, ve, "chunked")) out->chunked = true;
            } else if (nl == 8 && ci_eq(cur, colon, "location", 8)) {
                out->location = if_str((const char *)v, (u32)(ve - v));
            } else if (nl == 12 && ci_eq(cur, colon, "content-type", 12)) {
                out->content_type = if_str((const char *)v, (u32)(ve - v));
            }
        }
        cur = lf + 1;
    }
    return false; /* 空行に到達せず = ヘッダ未完 */
}

/* ---- chunked 復号 ---- */

bool if_http_dechunk(IfArena *a, const u8 *p, u64 n, IfStr *out) {
    u64 cap = 0, len = 0, i = 0;
    u8 *buf = NULL;
    for (;;) {
        /* サイズ行: hex[;ext] まで */
        u64 size = 0;
        u32 ndig = 0;
        while (i < n && ((p[i] >= '0' && p[i] <= '9') ||
                         (p[i] >= 'a' && p[i] <= 'f') ||
                         (p[i] >= 'A' && p[i] <= 'F'))) {
            if (++ndig > 8) goto bad;
            u8 c = p[i];
            u32 v = c <= '9' ? (u32)(c - '0')
                  : c <= 'F' ? (u32)(c - 'A' + 10) : (u32)(c - 'a' + 10);
            size = size * 16 + v;
            i++;
        }
        if (ndig == 0) goto bad;
        /* chunk-ext を捨てて行末へ */
        while (i < n && p[i] != '\n') i++;
        if (i >= n) goto bad;
        i++; /* \n */
        if (size == 0) {
            /* trailer section を捨てる: 空行まで（EOF 直後 0 チャンクも宽容受理） */
            for (;;) {
                if (i >= n) break;
                u64 ls = i;
                while (i < n && p[i] != '\n') i++;
                bool empty = (i == ls) || (i == ls + 1 && p[ls] == '\r');
                if (i < n) i++;
                if (empty) break;
            }
            if (!buf) { /* 空ボディでも p を非 NULL に（読み失敗判定との区別） */
                u64 z = 0;
                buf = (u8 *)if_arena_grow(a, NULL, &z, 1, 1);
            }
            *out = if_str((const char *)buf, (u32)len);
            return true;
        }
        if (size > IF_HTTP_MAX_BYTES || len + size > IF_HTTP_MAX_BYTES) goto bad;
        if (i + size > n) goto bad;
        buf = (u8 *)if_arena_grow(a, buf, &cap, len + size, 1);
        memcpy(buf + len, p + i, size);
        len += size;
        i += size;
        if (i >= n) goto bad;
        if (p[i] == '\r') i++;
        if (i >= n || p[i] != '\n') goto bad;
        i++;
    }
bad:
    return false;
}

/* ---- ソケット ---- */

/* リダイレクト経由の接続では private/loopback/link-local を拒否（DNS rebinding / SSRF 対策。
 * トップレベル（ユーザ直接入力）は allow_private=true — ローカル開発サーバ等を壊さない。
 * 公開: tests/test_http.c がユニット検査する。 */
bool if_addr_is_private(const struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        uint32_t a = ntohl(sin->sin_addr.s_addr);
        if ((a >> 24) == 127) return true;          /* 127.0.0.0/8 */
        if ((a >> 24) == 10) return true;           /* 10.0.0.0/8 */
        if ((a >> 24) == 169 && (a >> 16) == 0xA9FE) return true; /* 169.254.0.0/16 */
        if ((a >> 24) == 172) { /* 172.16.0.0/12: 第 2 オクテット 16..31 */
            u32 o2 = (a >> 16) & 0xFF;
            if (o2 >= 16 && o2 <= 31) return true;
        }
        if ((a >> 24) == 192 && (a >> 16) == 0xC0A8) return true; /* 192.168.0.0/16 */
        if ((a >> 24) == 100) { /* 100.64.0.0/10 CGNAT: 第 2 オクテット 64..127 */
            u32 o2 = (a >> 16) & 0xFF;
            if (o2 >= 64 && o2 <= 127) return true;
        }
        if (a == 0) return true;                    /* 0.0.0.0/8 */
        return false;
    }
    return false; /* AF_INET のみ使用中。他は許可しない判定不要 */
}

/* 0 以上 = fd、-1 = connect 失敗、-2 = DNS 失敗。
 * out_private（NULL 可）: 接続成功したアドレスが private か（チェーン追跡用） */
static int connect_one(const char *host, u16 port, bool allow_private, bool *out_private) {
    char ps[8];
    snprintf(ps, sizeof ps, "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; /* v0.3: IPv4 のみ（URL 側も bracket 拒否） */
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, ps, &hints, &res) != 0) return -2;
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (!allow_private && if_addr_is_private(ai->ai_addr)) continue; /* private は試さない */
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl < 0) fl = 0;
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            rc = poll(&pfd, 1, 8000); /* connect 8s 上限 */
            if (rc > 0) {
                int so = 0;
                socklen_t sl = sizeof so;
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl) == 0 && so == 0)
                    rc = 0;
                else
                    rc = -1;
            } else {
                rc = -1; /* timeout or error */
            }
        }
        if (rc == 0) {
            if (out_private) *out_private = if_addr_is_private(ai->ai_addr);
            /* ブロッキングに戻し、送受信は SO_*TIMEO で縛る（10s/回） */
            fcntl(fd, F_SETFL, fl);
            struct timeval tv;
            tv.tv_sec = 10;
            tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static bool send_all(int fd, const u8 *p, size_t n) {
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return true;
}

/* 1 回分の GET（redirect は追わない）。成功で body（デコード済）を返す */
static bool fetch_once(IfArena *a, const IfHttpUrl *u, IfStr *out_body,
                       u32 *out_status, IfStr *out_loc, IfStr *out_ctype,
                       const char **err, bool allow_private, bool *out_conn_private) {
    /* connect_one が allow_private=false で private アドレスをスキップするため、
     * 解決は 1 回（判定と接続の分離による DNS rebinding の窓を広げない） */
    bool conn_priv = false;
    int fd = connect_one(u->host, u->port, allow_private, &conn_priv);
    if (out_conn_private) *out_conn_private = conn_priv;
    if (fd == -2) { *err = E_DNS; return false; }
    if (fd < 0) { *err = allow_private ? E_CONN : E_PRIV; return false; }
    /* https: TLS 1.2 ハンドシェイク（CA 検証 + サーバ名照合は BearSSL が実施） */
    IfTls *tls = NULL;
    if (u->tls) {
        tls = if_tls_client(fd, u->host, err);
        if (!tls) {
            close(fd);
            /* BearSSL の err 分類（tls/cert/ca）を net の分類へ写像 */
            if (*err == E_TLS || *err == E_CERT || *err == E_CA) { /* そのまま */ }
            return false;
        }
    }
    /* Host: 明示 :port のときだけ付ける（既定 80/443 は載せない普通の形） */
    char req[1280];
    int rl;
    if (u->has_port)
        rl = snprintf(req, sizeof req,
                      "GET %s HTTP/1.1\r\nHost: %s:%u\r\n"
                      "User-Agent: Ifuto/0.3\r\nAccept: */*\r\n"
                      "Connection: close\r\n\r\n",
                      u->path, u->host, (unsigned)u->port);
    else
        rl = snprintf(req, sizeof req,
                      "GET %s HTTP/1.1\r\nHost: %s\r\n"
                      "User-Agent: Ifuto/0.3\r\nAccept: */*\r\n"
                      "Connection: close\r\n\r\n",
                      u->path, u->host);
    if (rl < 0 || (size_t)rl >= sizeof req) {
        if (tls) if_tls_close(tls);
        close(fd);
        *err = E_URL;
        return false;
    }
    if (tls) {
        if (!if_tls_send_all(tls, (const u8 *)req, (size_t)rl, err)) {
            if_tls_close(tls);
            close(fd);
            return false;
        }
    } else if (!send_all(fd, (const u8 *)req, (size_t)rl)) {
        close(fd);
        *err = E_SEND;
        return false;
    }
    /* EOF まで読む（Connection: close を宣言済み）。Content-Length が確定したら
     * その分だけ読んで早期完了（TLS では close_notify 待ちの無駄を省く） */
    u64 cap = 0, n = 0;
    u8 *buf = NULL;
    u64 body_need = UINT64_MAX; /* 未確定 */
    for (;;) {
        if (n >= IF_HTTP_MAX_BYTES) {
            if (tls) if_tls_close(tls);
            close(fd);
            *err = E_BIG;
            return false;
        }
        buf = (u8 *)if_arena_grow(a, buf, &cap, n + 16384, 1);
        ssize_t r;
        if (tls) {
            r = if_tls_recv(tls, buf + n, cap - n, err);
            if (r < 0) {
                if_tls_close(tls);
                close(fd);
                return false;
            }
            if (r == 0) break; /* close_notify / FIN = EOF */
        } else {
            r = recv(fd, buf + n, (size_t)(cap - n), 0);
            if (r < 0) {
                if (errno == EINTR) continue;
                close(fd);
                *err = E_RECV;
                return false;
            }
            if (r == 0) break;
        }
        n += (u64)r;
        /* ヘッダが揃ったら Content-Length を確認し、必要量を確定 */
        if (body_need == UINT64_MAX) {
            IfHttpHead hh;
            if (if_http_head_parse(buf, n, &hh)) {
                if (hh.content_length != UINT64_MAX)
                    body_need = hh.body_off + hh.content_length;
            }
        }
        if (body_need != UINT64_MAX && n >= body_need) break;
    }
    if (tls) if_tls_close(tls);
    close(fd);
    IfHttpHead h;
    if (!if_http_head_parse(buf, n, &h)) { *err = E_RESP; return false; }
    *out_status = h.status;
    *out_loc = h.location;
    if (out_ctype) *out_ctype = h.content_type;
    u64 bn = n - h.body_off;
    const u8 *bp = buf + h.body_off;
    if (h.chunked) {
        if (!if_http_dechunk(a, bp, bn, out_body)) { *err = E_RESP; return false; }
        return true;
    }
    if (h.content_length != UINT64_MAX) {
        if (h.content_length > IF_HTTP_MAX_BYTES) { *err = E_BIG; return false; }
        if (bn < h.content_length) { *err = E_TRUNC; return false; }
        *out_body = if_str((const char *)bp, (u32)h.content_length);
        return true;
    }
    *out_body = if_str((const char *)bp, (u32)bn); /* close までがボディ */
    return true;
}

bool if_http_get(IfArena *a, const char *url, IfStr *out_body, u32 *out_status,
                 const char **err) {
    return if_http_get_ex(a, url, out_body, out_status, NULL, err);
}

bool if_http_get_ex(IfArena *a, const char *url, IfStr *out_body, u32 *out_status,
                    IfStr *out_content_type, const char **err) {
    char cur[1024];
    if (strlen(url) >= sizeof cur) { *err = E_URL; return false; }
    snprintf(cur, sizeof cur, "%s", url);
    bool chain_tls = strncmp(cur, "https://", 8) == 0; /* チェーンが https で始まったか（降格防止） */
    bool chain_private = false; /* 最初の接続が private ならローカルチェーン（リダイレクトも private 許可）。
                                 * public 開始チェーンは private へのリダイレクトを拒否（DNS rebinding/SSRF） */
    for (u32 depth = 0;; depth++) {
        IfHttpUrl u;
        if (!if_http_parse_url(cur, &u)) { *err = E_URL; return false; }
        IfStr loc = if_str(NULL, 0);
        /* トップレベルは private 許可（ユーザ直接入力）。以後は chain_private に従う */
        bool allow_priv = depth == 0 || chain_private;
        bool conn_priv = false;
        if (!fetch_once(a, &u, out_body, out_status, &loc, out_content_type, err, allow_priv, &conn_priv)) return false;
        if (depth == 0 && conn_priv) chain_private = true;
        bool redir = (*out_status == 301 || *out_status == 302 ||
                      *out_status == 303 || *out_status == 307 ||
                      *out_status == 308) && loc.p && loc.n;
        if (!redir) return true;
        if (depth >= IF_HTTP_MAX_REDIRECTS) { *err = E_LOOP; return false; }
        /* loc は arena 内参照 → スタックへ写してから解決 */
        char locbuf[1024], nxt[1024];
        if (loc.n >= sizeof locbuf) return true; /* 異常に長い Location は追わない */
        memcpy(locbuf, loc.p, loc.n);
        locbuf[loc.n] = 0;
        if (!if_http_resolve_url(cur, locbuf, nxt, sizeof nxt))
            return true; /* 解決不能（https 等）は最後の応答を返す */
        /* https で始まったチェーンが http へ降格するのを拒否（mixed 降格の構造的防止） */
        if (chain_tls && strncmp(nxt, "http://", 7) == 0 && strncmp(nxt, "https://", 8) != 0) {
            *err = E_DOWNGRADE;
            return false;
        }
        snprintf(cur, sizeof cur, "%s", nxt);
    }
}
