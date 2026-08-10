/* HTTP/1.1 取得の純粋関数検査（ソケット不要: URL 分解・解決・ヘッダ解析・
 * chunked 復号。実ソケット経路は gui_smoke が loopback サーバで黒盒検査する） */
#include "tests.h"
#include "../src/net.h"
#include <netinet/in.h> /* struct sockaddr_in（if_addr_is_private のユニット検査） */
#include <string.h>
#include <stdint.h>

static void test_http_parse(void) {
    IfHttpUrl u;
    CHECK(if_http_parse_url("http://example.com/", &u));
    CHECK(strcmp(u.host, "example.com") == 0 && u.port == 80 && !u.has_port);
    CHECK(strcmp(u.path, "/") == 0);
    CHECK(if_http_parse_url("http://example.com", &u));
    CHECK(strcmp(u.path, "/") == 0); /* path 無しは "/" 正規化 */
    CHECK(if_http_parse_url("http://127.0.0.1:8080/a/b?x=1#frag", &u));
    CHECK(strcmp(u.host, "127.0.0.1") == 0 && u.port == 8080 && u.has_port);
    CHECK(strcmp(u.path, "/a/b?x=1") == 0); /* fragment 除去、query 保持 */
    CHECK(if_http_parse_url("http://example.com?q=1", &u));
    CHECK(strcmp(u.path, "/?q=1") == 0); /* "?query" は "/?" 正規化 */
    /* v0.3: https 受理（tls フラグ + 既定 port 443） */
    CHECK(if_http_parse_url("https://example.com/", &u));
    CHECK(u.tls && u.port == 443 && !u.has_port);
    CHECK(strcmp(u.host, "example.com") == 0 && strcmp(u.path, "/") == 0);
    CHECK(if_http_parse_url("https://example.com:8443/a", &u));
    CHECK(u.tls && u.port == 8443 && u.has_port);
    /* 拒否群（曖昧解釈しない） */
    CHECK(!if_http_parse_url("ftp://example.com/", &u));
    CHECK(!if_http_parse_url("http://user@example.com/", &u)); /* userinfo */
    CHECK(!if_http_parse_url("http://[::1]:8080/", &u));       /* IPv6 bracket */
    CHECK(!if_http_parse_url("http://example.com:0/", &u));
    CHECK(!if_http_parse_url("http://example.com:65536/", &u));
    CHECK(!if_http_parse_url("http://example.com:12a4/", &u));
    CHECK(!if_http_parse_url("http://", &u));
    CHECK(!if_http_parse_url("http:///path", &u));             /* 空 host */
    CHECK(!if_http_parse_url("", &u));
    /* host 長溢れ */
    {
        char long_url[400];
        strcpy(long_url, "http://");
        memset(long_url + 7, 'a', 300);
        strcpy(long_url + 307, "/");
        CHECK(!if_http_parse_url(long_url, &u));
    }
    /* path 長溢れ（767 まで受理、768 以上で拒否） */
    {
        char pu[900];
        strcpy(pu, "http://h");
        memset(pu + 8, 'p', 760);
        strcpy(pu + 768, "");
        pu[8] = '/';
        CHECK(if_http_parse_url(pu, &u)); /* 1 + 759 = 760 文字: 受理 */
        memset(pu + 8, 'p', 780);
        pu[8] = '/';
        pu[8 + 780] = 0;
        CHECK(!if_http_parse_url(pu, &u));
    }
}

static void test_http_resolve(void) {
    char out[1024];
    CHECK(if_http_resolve_url("http://h/dir/page", "http://x/y", out, sizeof out));
    CHECK(strcmp(out, "http://x/y") == 0);
    CHECK(if_http_resolve_url("http://h/dir/page", "//x/y", out, sizeof out));
    CHECK(strcmp(out, "http://x/y") == 0);
    CHECK(if_http_resolve_url("http://h:8080/dir/page", "/abs", out, sizeof out));
    CHECK(strcmp(out, "http://h:8080/abs") == 0);
    CHECK(if_http_resolve_url("http://h/dir/page", "rel", out, sizeof out));
    CHECK(strcmp(out, "http://h/dir/rel") == 0);
    CHECK(if_http_resolve_url("http://h", "rel2", out, sizeof out));
    CHECK(strcmp(out, "http://h/rel2") == 0); /* path 無し base は末尾連結 */
    CHECK(if_http_resolve_url("http://h/", "rel3", out, sizeof out));
    CHECK(strcmp(out, "http://h/rel3") == 0);
    CHECK(if_http_resolve_url("http://h/dir/page?q=1#f", "next", out, sizeof out));
    CHECK(strcmp(out, "http://h/dir/next") == 0); /* base query/frag は落ちる */
    CHECK(if_http_resolve_url("http://h/dir/page", "?q=2", out, sizeof out));
    CHECK(strcmp(out, "http://h/dir/page?q=2") == 0); /* クエリ置換 */
    CHECK(if_http_resolve_url("http://h/dir/", "next2", out, sizeof out));
    CHECK(strcmp(out, "http://h/dir/next2") == 0);
    /* 前後空白の宽容 */
    CHECK(if_http_resolve_url("http://h/d", "  /sp  ", out, sizeof out));
    CHECK(strcmp(out, "http://h/sp") == 0);
    /* http に落ちないもの・空は拒否 */
    CHECK(!if_http_resolve_url("http://h/d", "https://x/", out, sizeof out));
    CHECK(!if_http_resolve_url("http://h/d", "javascript:void(0)", out, sizeof out));
    CHECK(!if_http_resolve_url("http://h/d", "", out, sizeof out));
    /* 出力溢れは拒否（切り詰め URL を返さない） */
    {
        char sm[20];
        CHECK(!if_http_resolve_url("http://h/dir/page", "http://x/this-is-a-long-url",
                                   sm, sizeof sm));
    }
}

static void test_http_head(void) {
    IfHttpHead h;
    const char *r1 = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    CHECK(if_http_head_parse((const u8 *)r1, strlen(r1), &h));
    CHECK(h.status == 200 && h.content_length == 5 && h.chunked == 0);
    CHECK(h.body_off == strlen(r1) - 5);
    /* 大文字小文字・余分空白の宽容 */
    const char *r2 = "HTTP/1.0 404 Not Found\r\ncONTENT-lENGTH:   3 \r\n\r\nabc";
    CHECK(if_http_head_parse((const u8 *)r2, strlen(r2), &h));
    CHECK(h.status == 404 && h.content_length == 3);
    /* chunked 検出（列形式も宽容） */
    const char *r3 = "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, Chunked\r\n\r\n0\r\n\r\n";
    CHECK(if_http_head_parse((const u8 *)r3, strlen(r3), &h));
    CHECK(h.chunked == 1 && h.content_length == UINT64_MAX);
    /* Location 抽出 */
    const char *r4 = "HTTP/1.1 301 Moved\r\nLocation: /new\r\nContent-Length: 0\r\n\r\n";
    CHECK(if_http_head_parse((const u8 *)r4, strlen(r4), &h));
    CHECK(h.status == 301 && h.location.n == 4);
    CHECK(h.location.p && memcmp(h.location.p, "/new", 4) == 0);
    /* "\n\n" 宽容 */
    const char *r5 = "HTTP/1.1 204 X\nServer: s\n\n";
    CHECK(if_http_head_parse((const u8 *)r5, strlen(r5), &h));
    CHECK(h.status == 204 && h.body_off == strlen(r5));
    /* CL 無し → close まで */
    const char *r6 = "HTTP/1.1 200 OK\r\nServer: s\r\n\r\nbody-through-close";
    CHECK(if_http_head_parse((const u8 *)r6, strlen(r6), &h));
    CHECK(h.content_length == UINT64_MAX && !h.chunked);
    /* 偽物ヘッダ名の prefix 誤爆をしない（Content-Length-X は CL でない） */
    const char *r7 = "HTTP/1.1 200 OK\r\nContent-Length-X: 9\r\n\r\nb";
    CHECK(if_http_head_parse((const u8 *)r7, strlen(r7), &h));
    CHECK(h.content_length == UINT64_MAX);
    /* 受理しないもの */
    const char *b1 = "garbage\r\n\r\n";
    CHECK(!if_http_head_parse((const u8 *)b1, strlen(b1), &h));
    const char *b2 = "HTTP/1.1 200 OK\r\nServer: s\r\n"; /* ヘッダ未完 */
    CHECK(!if_http_head_parse((const u8 *)b2, strlen(b2), &h));
    const char *b3 = "HTTP/1.1 ABC X\r\n\r\n";                 /* 非数ステータス */
    CHECK(!if_http_head_parse((const u8 *)b3, strlen(b3), &h));
    /* CL 複数は先勝ち */
    const char *r8 = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 9\r\n\r\nab";
    CHECK(if_http_head_parse((const u8 *)r8, strlen(r8), &h));
    CHECK(h.content_length == 2);
}

static void test_http_dechunk(void) {
    IfArena a;
    if_arena_init(&a, 1 << 12);
    IfStr o;
    const char *c1 = "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    CHECK(if_http_dechunk(&a, (const u8 *)c1, strlen(c1), &o));
    CHECK(o.n == 9 && memcmp(o.p, "Wikipedia", 9) == 0);
    /* chunk 拡張・トレーラ */
    const char *c2 = "4;x=y\r\nWiki\r\n0\r\nX-Trailer: v\r\n\r\n";
    CHECK(if_http_dechunk(&a, (const u8 *)c2, strlen(c2), &o));
    CHECK(o.n == 4 && memcmp(o.p, "Wiki", 4) == 0);
    /* LF-only 宽容 */
    const char *c3 = "4\nWiki\n5\npedia\n0\n\n";
    CHECK(if_http_dechunk(&a, (const u8 *)c3, strlen(c3), &o));
    CHECK(o.n == 9);
    /* 大文字 hex */
    const char *c4 = "A\r\n0123456789\r\n0\r\n";
    CHECK(if_http_dechunk(&a, (const u8 *)c4, strlen(c4), &o));
    CHECK(o.n == 10 && o.p[9] == '9');
    /* 空 chunked ボディ（0 チャンクのみ、p は非 NULL を保証） */
    const char *c5 = "0\r\n\r\n";
    CHECK(if_http_dechunk(&a, (const u8 *)c5, strlen(c5), &o));
    CHECK(o.n == 0 && o.p != NULL);
    /* 拒否群 */
    const char *x1 = "5\r\nWik\r\n";              /* 宣言サイズを食い違い */
    CHECK(!if_http_dechunk(&a, (const u8 *)x1, strlen(x1), &o));
    const char *x2 = "Z\r\nxx\r\n";               /* 非 hex */
    CHECK(!if_http_dechunk(&a, (const u8 *)x2, strlen(x2), &o));
    const char *x3 = "4\r\nWiki";                /* 切断 */
    CHECK(!if_http_dechunk(&a, (const u8 *)x3, strlen(x3), &o));
    const char *x4 = "1\r\nA";                   /* chunk 末改行が無い */
    CHECK(!if_http_dechunk(&a, (const u8 *)x4, strlen(x4), &o));
    if_arena_destroy(&a);
}

static void test_private_addr(void);

void test_http(void) {
    test_http_parse();
    test_http_resolve();
    test_http_head();
    test_http_dechunk();
    test_private_addr();
}

/* v0.5: リダイレクト経由 private 接続のブロック判定（DNS rebinding / SSRF 対策） */
static void test_private_addr(void) {
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    #define PRIV(ip, want) do { \
        sin.sin_addr.s_addr = htonl(ip); \
        CHECK(if_addr_is_private((const struct sockaddr *)&sin) == want); \
    } while (0)
    PRIV(0x7F000001u, true);   /* 127.0.0.1 loopback */
    PRIV(0x7F0000FFu, true);   /* 127.0.0.255 */
    PRIV(0x0A000001u, true);   /* 10.0.0.1 */
    PRIV(0x0AFFFFFFu, true);   /* 10.255.255.255 */
    PRIV(0xAC100001u, true);   /* 172.16.0.1 */
    PRIV(0xAC1F0001u, true);   /* 172.31.0.1 */
    PRIV(0xAC1F0001u, true);
    PRIV(0xAC100001u, true);
    PRIV(0xAC1F0001u, true);
    PRIV(0xC0A80001u, true);   /* 192.168.0.1 */
    PRIV(0xC0A8FFFEu, true);   /* 192.168.255.254 */
    PRIV(0xA9FE0001u, true);   /* 169.254.0.1 link-local */
    PRIV(0x64400001u, true);   /* 100.64.0.1 CGNAT */
    PRIV(0x647FFFFEu, true);   /* 100.127.255.254 */
    PRIV(0x00000000u, true);   /* 0.0.0.0 */
    PRIV(0x08080808u, false);  /* 8.8.8.8 public */
    PRIV(0x0A0A0A0Au, true);   /* 10.10.10.10 は 10/8 なので private */
    PRIV(0xC0000201u, false);  /* 192.0.2.1 documentation (public 扱い) */
    PRIV(0xAC100000u, true);   /* 172.16.0.0 */
    PRIV(0xAC0F0001u, false);  /* 172.15.0.1 public */
    PRIV(0xAC200001u, false);  /* 172.32.0.1 public */
    #undef PRIV
}


