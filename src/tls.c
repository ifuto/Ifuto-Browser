/* Ifuto — TLS クライアント（v0.3: https:// 対応。ロードマップ既定の BearSSL 静的リンク）。
 *
 * 設計:
 *  - 自作 TLS は禁止（ARCHITECTURE.md §6 明記）→ BearSSL（vendor/bearssl, MIT）を静的リンク。
 *    製品法則「ldd = vdso/libm/libc/ld」は静的リンクで維持される。
 *  - プロトコル: TLS 1.2 のみ（BearSSL の上限。TLS 1.0/1.1 は明示拒否）。
 *  - 証明書検証: システム CA バンドル（/etc/ssl/certs/ca-certificates.crt 等）を
 *    トラストアンカーとして、チェーン検証 + サーバ名（SAN/CN）照合を BearSSL が行う。
 *  - CA バンドルはプロセスで 1 回だけロード（毎接続パースしない）。
 *  - ソケット IO は呼出側（net.c）の fd に対してブロッキング send/recv。
 *    SO_SNDTIMEO/SO_RCVTIMEO（10s）は net.c が設定済みの前提。
 *  - エラー分類: "tls"（ハンドシェイク失敗）/ "cert"（証明書検証失敗）/
 *    "ca"（CA ロード失敗）。net.c の err 分類に合わせる。
 */
#define _POSIX_C_SOURCE 200809L
#include "tls.h"
#include "bearssl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>

#define IF_CA_PATHS \
    "/etc/ssl/certs/ca-certificates.crt", \
    "/etc/ssl/cert.pem", \
    "/etc/pki/tls/certs/ca-bundle.crt", \
    "/etc/ssl/ca-bundle.pem"

/* ---- プロセス静的: トラストアンカー（1 回ロード） ---- */
static br_x509_trust_anchor *g_ta;
static size_t g_ta_num;
static u8 *g_der;             /* 各 CA の DER を連結保持（TA の dn/pkey が参照） */
static u64 g_der_len, g_der_cap;
static bool g_ca_attempted;

/* base64 デコード（PEM 標準表。空白類は無視。padding 対応）。
 * in_len は「終端を含まない」バイト列。成功で *out_len にバイト数。 */
static bool b64_decode(const u8 *in, u64 in_len, u8 *out, u64 *out_len) {
    static const int8_t T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    u64 acc = 0;
    int nbits = 0;
    u64 w = 0;
    bool seen_nonpad = false;
    for (u64 i = 0; i < in_len; i++) {
        u8 c = in[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        if (c == '=') { seen_nonpad = true; continue; }
        if (c == '\0') break;
        if (seen_nonpad) return false; /* padding 後の非 padding */
        int8_t v = c < 128 ? T[c] : -1;
        if (v < 0) return false;
        acc = (acc << 6) | (u64)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out[w++] = (u8)(acc >> nbits);
            acc &= (1u << nbits) - 1;
        }
    }
    *out_len = w;
    return true;
}

/* DER を g_der に永続コピーし、オフセットを返す（TA のポインタ参照用） */
static u64 der_keep(const u8 *der, u64 len) {
    if (g_der_len + len > g_der_cap) {
        u64 nc = g_der_cap ? g_der_cap * 2 : 65536;
        while (nc < g_der_len + len) nc *= 2;
        u8 *nb = (u8 *)realloc(g_der, (size_t)nc);
        if (!nb) return UINT64_MAX;
        g_der = nb;
        g_der_cap = nc;
    }
    u64 at = g_der_len;
    memcpy(g_der + at, der, (size_t)len);
    g_der_len += len;
    return at;
}

/* DN 受信コールバック（x509_decoder がチャンク単位で届ける） */
typedef struct {
    u8 buf[4096];
    u32 len;
    bool overflow;
} DnTmp;
static void dn_cb(void *ctx, const void *data, size_t len) {
    DnTmp *d = (DnTmp *)ctx;
    if (d->len + (u32)len > sizeof d->buf) { d->overflow = true; return; }
    memcpy(d->buf + d->len, data, len);
    d->len += (u32)len;
}

/* 1 枚の DER 証明書をトラストアンカーへ追加。
 * DN は append_dn コールバック、公開鍵は get_pkey（エンジン内バッファ参照）を
 * それぞれ g_der にコピーして永続化する（TA はポインタ参照のみ）。 */
static bool ta_add(const u8 *der, u64 der_len) {
    br_x509_decoder_context dc;
    DnTmp dn;
    memset(&dn, 0, sizeof dn);
    br_x509_decoder_init(&dc, dn_cb, &dn);
    br_x509_decoder_push(&dc, der, (size_t)der_len);
    if (br_x509_decoder_last_error(&dc) != 0) return false; /* 0 = 成功（BR_ERR_X509_OK=32 は x509_minimal 系） */
    if (dn.overflow || dn.len == 0) return false;
    br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (!pk) return false;
    u64 dn_off = der_keep(dn.buf, dn.len);
    if (dn_off == UINT64_MAX) return false;
    /* 鍵データを 1 ブロックに連結して永続化 */
    u64 nlen = 0, elen = 0, qlen = 0;
    if (pk->key_type == BR_KEYTYPE_RSA) {
        nlen = pk->key.rsa.nlen;
        elen = pk->key.rsa.elen;
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        qlen = pk->key.ec.qlen;
    }
    u64 key_off = UINT64_MAX;
    u64 total = nlen + elen + qlen;
    if (total) {
        u8 *tmp = (u8 *)malloc((size_t)total);
        if (!tmp) return false;
        u64 w = 0;
        if (nlen) { memcpy(tmp + w, pk->key.rsa.n, (size_t)nlen); w += nlen; }
        if (elen) { memcpy(tmp + w, pk->key.rsa.e, (size_t)elen); w += elen; }
        if (qlen) { memcpy(tmp + w, pk->key.ec.q, (size_t)qlen); w += qlen; }
        key_off = der_keep(tmp, w);
        free(tmp);
        if (key_off == UINT64_MAX) return false;
    }
    br_x509_trust_anchor *nta = (br_x509_trust_anchor *)realloc(
        g_ta, (g_ta_num + 1) * sizeof(br_x509_trust_anchor));
    if (!nta) return false;
    g_ta = nta;
    br_x509_trust_anchor *ta = &g_ta[g_ta_num++];
    memset(ta, 0, sizeof *ta);
    ta->dn.data = g_der + dn_off;
    ta->dn.len = dn.len;
    ta->flags = BR_X509_TA_CA;
    ta->pkey.key_type = pk->key_type;
    if (pk->key_type == BR_KEYTYPE_RSA) {
        ta->pkey.key.rsa.n = key_off != UINT64_MAX ? g_der + key_off : NULL;
        ta->pkey.key.rsa.nlen = (size_t)nlen;
        ta->pkey.key.rsa.e = key_off != UINT64_MAX ? g_der + key_off + nlen : NULL;
        ta->pkey.key.rsa.elen = (size_t)elen;
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        ta->pkey.key.ec.curve = pk->key.ec.curve;
        ta->pkey.key.ec.q = key_off != UINT64_MAX ? g_der + key_off : NULL;
        ta->pkey.key.ec.qlen = (size_t)qlen;
    }
    return true;
}

/* PEM バンドルから証明書を抽出して TA へ。1 枚でも成功すれば true */
static bool ca_load_pem(const u8 *pem, u64 n) {
    const u8 *p = pem;
    const u8 *end = pem + n;
    bool any = false;
    for (;;) {
        /* BEGIN を探す */
        const u8 *b = p;
        for (; b + 27 <= end; b++) {
            if (memcmp(b, "-----BEGIN CERTIFICATE-----", 27) == 0) break;
        }
        if (b + 27 > end) break;
        const u8 *b64 = b + 27;
        /* END を探す */
        const u8 *e = b64;
        for (; e + 25 <= end; e++) {
            if (memcmp(e, "-----END CERTIFICATE-----", 25) == 0) break;
        }
        if (e + 25 > end) break;
        u64 der_len = 0;
        u8 *der = (u8 *)malloc((size_t)(e - b64) + 1);
        if (!der) break;
        if (b64_decode(b64, (u64)(e - b64), der, &der_len) && der_len > 0) {
            if (ta_add(der, der_len)) any = true;
        }
        free(der);
        p = e + 25;
    }
    return any;
}

/* CA バンドルの探索とロード（1 回だけ）。false = どのパスも読めなかった */
static bool ca_load(void) {
    const char *env = getenv("IFUTO_CA_BUNDLE");
    const char *paths[] = {
        env,
        IF_CA_PATHS
    };
    u32 np = env ? 1 : (u32)(sizeof paths / sizeof paths[0]);
    for (u32 i = 0; i < np; i++) {
        const char *path = paths[i];
        if (!path || !path[0]) continue;
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        u64 cap = 1 << 16, n = 0;
        u8 *buf = (u8 *)malloc((size_t)cap);
        if (!buf) { fclose(f); continue; }
        for (;;) {
            if (n + 65536 > cap) {
                u64 nc = cap * 2;
                u8 *nb = (u8 *)realloc(buf, (size_t)nc);
                if (!nb) { free(buf); fclose(f); return false; }
                buf = nb;
                cap = nc;
            }
            size_t r = fread(buf + n, 1, 65536, f);
            n += r;
            if (r == 0) break;
        }
        fclose(f);
        bool ok = ca_load_pem(buf, n);
        free(buf);
        if (ok) return true;
    }
    return false;
}

/* ---- 接続インスタンス ---- */

struct IfTls {
    int fd;
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    u8 iobuf[BR_SSL_BUFSIZE_BIDI];
};

/* エンジン状態に応じて IO を進め、target 状態になるまでループ。
 * 0 = 到達 / 1 = 正常クローズ（EOF） / -1 = エラー（*err 設定） */
static int tls_run_until(br_ssl_engine_context *eng, int target, int fd, const char **err) {
    for (;;) {
        int state = br_ssl_engine_current_state(eng);
        if (state & BR_SSL_CLOSED) {
            int e = br_ssl_engine_last_error(eng);
            if (e == BR_ERR_OK) return 1; /* close_notify 正常 */
            /* X509 系エラー（33..63: 検証失敗・期限切れ・名前不一致等）は "cert" に分類 */
            *err = (e >= 33 && e <= 63) ? "cert" : "tls";
            return -1;
        }
        if (state & target) return 0;
        if (state & BR_SSL_SENDREC) {
            size_t len;
            unsigned char *buf = br_ssl_engine_sendrec_buf(eng, &len);
            if (len == 0) return -1;
            size_t off = 0;
            while (off < len) {
                ssize_t w = send(fd, buf + off, len - off, 0);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    *err = "send";
                    return -1;
                }
                off += (size_t)w;
            }
            br_ssl_engine_sendrec_ack(eng, len);
        } else if (state & BR_SSL_RECVREC) {
            size_t len;
            unsigned char *buf = br_ssl_engine_recvrec_buf(eng, &len);
            if (len == 0) continue;
            ssize_t r = recv(fd, buf, len, 0);
            if (r == 0) {
                /* TCP FIN: レコード境界での close_notify なし切断は HTTP/1.1
                 * Connection: close では一般的（Content-Length でボディ確定済みなら
                 * セキュリティ影響なし）。エンジンに EOF を伝達して正常終了扱い。 */
                    br_ssl_engine_recvrec_ack(eng, 0);
                return 1;
            }
            if (r < 0) {
                if (errno == EINTR) continue;
                *err = "recv";
                return -1;
            }
            br_ssl_engine_recvrec_ack(eng, (size_t)r);
        } else {
            /* 待機すべき状態が無い（理論上到達しない） */
            *err = "tls";
            return -1;
        }
    }
}

IfTls *if_tls_client(int fd, const char *host, const char **err) {
    if (!g_ca_attempted) {
        g_ca_attempted = true;
        if (!ca_load()) { *err = "ca"; return NULL; }
    }
    if (g_ta_num == 0) { *err = "ca"; return NULL; }
    IfTls *t = (IfTls *)calloc(1, sizeof *t);
    if (!t) { *err = "tls"; return NULL; }
    t->fd = fd;
    br_ssl_client_init_full(&t->sc, &t->xc, g_ta, g_ta_num);
    /* TLS 1.2 のみ（1.0/1.1 は明示拒否。BearSSL の既定は 10-12） */
    br_ssl_engine_set_versions(&t->sc.eng, BR_TLS12, BR_TLS12);
    /* bidi=1（全二重）: bidi=0 だと送信フェーズ中は recvrec_buf が NULL になり、
     * サーバ応答を待てない（実測で特定）。BR_SSL_BUFSIZE_BIDI は全二重の最適値。 */
    br_ssl_engine_set_buffer(&t->sc.eng, t->iobuf, sizeof t->iobuf, 1);
    br_ssl_client_reset(&t->sc, host, 0);
    /* ハンドシェイク完了 = アプリデータ送信可（SENDAPP が立つ） */
    int r = tls_run_until(&t->sc.eng, BR_SSL_SENDAPP, fd, err);
    if (r != 0) {
        if (r == 1) *err = "tls"; /* ハンドシェイク中のクローズ */
        free(t);
        return NULL;
    }
    /* 検証失敗はエンジンが CLOSED にしているはずだが、二重確認 */
    if (br_ssl_engine_last_error(&t->sc.eng) != BR_ERR_OK) {
        *err = "cert";
        free(t);
        return NULL;
    }
    return t;
}

bool if_tls_send_all(IfTls *t, const u8 *p, u64 n, const char **err) {
    br_ssl_engine_context *eng = &t->sc.eng;
    while (n > 0) {
        size_t len;
        unsigned char *buf = br_ssl_engine_sendapp_buf(eng, &len);
        if (len == 0) {
            int r = tls_run_until(eng, BR_SSL_SENDAPP, t->fd, err);
            if (r != 0) return false;
            continue;
        }
        size_t c = n < (u64)len ? (size_t)n : len;
        memcpy((void *)buf, p, c);
        br_ssl_engine_sendapp_ack(eng, c);
        p += c;
        n -= c;
    }
    /* フラッシュ: アプリデータのレコード化を強制（sendapp_ack だけでは SENDREC が
     * 立たない — BearSSL は flush を呼ぶまでレコードを組み立てない。実測で特定:
     * リクエストが送信されず受信ループがタイムアウトした）してから、
     * SENDREC が立たなくなるまで送信処理を回す。 */
    br_ssl_engine_flush(eng, 1);
    for (;;) {
        int st = br_ssl_engine_current_state(eng);
        if (st & BR_SSL_CLOSED) {
            int e = br_ssl_engine_last_error(eng);
            if (e != BR_ERR_OK) { *err = "tls"; return false; }
            return true; /* close_notify 受信 = 相手はもう閉じる。送信は完了扱い */
        }
        if (st & BR_SSL_SENDREC) {
            size_t len;
            unsigned char *buf = br_ssl_engine_sendrec_buf(eng, &len);
            if (len == 0) return true;
            size_t off = 0;
            while (off < len) {
                ssize_t w = send(t->fd, buf + off, len - off, 0);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    *err = "send";
                    return false;
                }
                off += (size_t)w;
            }
            br_ssl_engine_sendrec_ack(eng, len);
            continue;
        }
        break; /* 送信すべきデータなし = フラッシュ完了 */
    }
    return true;
}

ssize_t if_tls_recv(IfTls *t, u8 *p, u64 cap, const char **err) {
    br_ssl_engine_context *eng = &t->sc.eng;
    for (;;) {
        int r = tls_run_until(eng, BR_SSL_RECVAPP, t->fd, err);
        if (r == 1) return 0; /* close_notify = EOF */
        if (r != 0) return -1;
        size_t len;
        unsigned char *buf = br_ssl_engine_recvapp_buf(eng, &len);
        if (len == 0) continue; /* レコード境界（再ループ） */
        size_t c = cap < (u64)len ? (size_t)cap : len;
        memcpy(p, buf, c);
        br_ssl_engine_recvapp_ack(eng, c);
        return (ssize_t)c;
    }
}

void if_tls_close(IfTls *t) {
    if (!t) return;
    br_ssl_engine_close(&t->sc.eng);
    /* close_notify の送信はベストエフォート（タイムアウトで諦める） */
    const char *err = NULL;
    (void)tls_run_until(&t->sc.eng, BR_SSL_CLOSED, t->fd, &err);
    free(t);
}
