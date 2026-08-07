/* Ifuto Browser — HTTP/1.1 取得（v0.3: 「普通のブラウザ」最終ブロック）。
 * 設計制約:
 *  - 100% 自製 C11（ソケットは OS 直叩き。外部ライブラリ禁止は不変）
 *  - http:// のみ。https/userinfo/IPv6-bracket は受理しない（曖昧解釈しない）
 *  - Connection: close で EOF まで読む単純形（コネクション枯渇・reuse を持たない）
 *  - 全ブッファは arena 所有。上限 32MB（暴走サーバからローカルを守る）
 *  - err は static 文字列（分類のみ。ヒープを汚さない） */
#ifndef IF_NET_H
#define IF_NET_H

#include "common.h"
#include "strutil.h"
#include "arena.h"

#define IF_HTTP_MAX_BYTES (32u * 1024u * 1024u) /* 応答全体の上限 */
#define IF_HTTP_MAX_REDIRECTS 5

typedef struct {
    char host[256];
    u16  port;      /* 省略時 80 */
    bool has_port;  /* URL に明示 :port があったか（Host ヘッダ生成に使う） */
    char path[768]; /* 常に '/' 始まり（?query 含む、#fragment は除去済み） */
} IfHttpUrl;

/* http://host[:port]/path[?query] を分解。#fragment は除去。
 * 受理しないもの: http 以外の scheme、userinfo('@')、IPv6 bracket、
 * 空 host、port 0/非数/桁溢れ、host/path の長さ溢れ。 */
bool if_http_parse_url(const char *url, IfHttpUrl *out);

/* base（現在の絶対 URL）に対して Location 値 loc を解決して out(cap) へ。
 * absolute / "//host" scheme-relative / "/abs" / 相対 / "?q" を受理。
 * http:// に落ちない解決結果は受理しない（false）。 */
bool if_http_resolve_url(const char *base, const char *loc, char *out, size_t cap);

/* 応答ヘッダの解析結果。buf 内参照のみ持つ（所有権は移らない）。 */
typedef struct {
    u32  status;
    u64  body_off;       /* buf 内の body 開始オフセット */
    u64  content_length; /* UINT64_MAX = 未指定 */
    bool chunked;        /* Transfer-Encoding: chunked */
    IfStr location;      /* Location 値（無ければ p=NULL） */
} IfHttpHead;

/* "\r\n\r\n"（宽容 "\n\n"）までをヘッダとして状態行と
 * Content-Length / Transfer-Encoding / Location を抜く。
 * ヘッダが未完、または状態行が HTTP/1.x 形でなければ false。 */
bool if_http_head_parse(const u8 *buf, u64 n, IfHttpHead *out);

/* chunked ボディのデコード（純粋関数）。p,n は chunked 符号化バイト列全体。
 * 成功すれば out が復号ボディ（arena 所有）、末尾 trailer(section)は消費して捨てる。 */
bool if_http_dechunk(IfArena *a, const u8 *p, u64 n, IfStr *out);

/* http GET。リダイレクト（301/302/303/307/308 + Location）は IF_HTTP_MAX_REDIRECTS
 * 回まで内部で追跡する。404 等でも応答が届けば成功扱いで status に乗せる
 * （普通のブラウザが 404 ページを描画するのと同じ）。
 * 失敗時のみ err に分類文字列（"bad url"/"dns"/"connect"/"send"/"recv"/
 * "too large"/"bad response"/"truncated"/"redirect loop"）。 */
bool if_http_get(IfArena *a, const char *url, IfStr *out_body, u32 *out_status,
                 const char **err);

#endif
