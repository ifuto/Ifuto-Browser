/* Ifuto — fuzz ドライバ: リモート入力面（src/net.c の 4 パーサ）。
 * 入力ファイルを URL / base+loc（最初の '\v' で 2 分割）/ HTTP ヘッダ /
 * chunked ボディの 4 形態でそのまま喰わせる。crash・UB・sanitizer 違反・
 * 不変条件違反があれば abort → 検出。静かに 0 で抜けるのが正。
 * 不変条件（実装契約の機械監査）:
 *  - parse_url 成功 ⇒ host 非空・port ∈ [1,65535]・path が '/' 始まり
 *  - resolve 成功 ⇒ 出力は cap 未満の C 文字列で "http://" 始まり
 *  - head_parse 成功 ⇒ body_off <= n、status は 3 桁
 *  - dechunk 成功 ⇒ 出力長 <= 入力長 + 16（hex 桁と CRLF が消えるだけで
 *    ボディが符号化入力より「純増え」することはありえない） */
#include "common.h"
#include "arena.h"
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    u8 buf[65536 + 1];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;

    IfArena a;
    if_arena_init(&a, 1 << 16);

    /* (1) URL（全文。埋め込み NUL は C 文字列としての打ち切りとして自然に働く） */
    {
        IfHttpUrl u;
        if (if_http_parse_url((const char *)buf, &u)) {
            if (u.host[0] == 0) { fputs("HOST\n", stderr); abort(); }
            if (u.port < 1) { fputs("PORT\n", stderr); abort(); }
            if (u.path[0] != '/') { fputs("PATH\n", stderr); abort(); }
            if (strlen(u.host) >= 256 || strlen(u.path) >= 768) { fputs("LEN\n", stderr); abort(); }
        }
    }

    /* (2) base + loc（'\v' で 2 分割。cap 小さめ 2 種で切詰め境界を打つ） */
    {
        u8 *v = (u8 *)memchr(buf, '\v', n);
        const char *base = (const char *)buf;
        const char *loc = v ? (const char *)v + 1 : "";
        if (v) *v = 0;
        char out[576];
        for (size_t cap = 1; cap <= sizeof out; cap = cap * 7 + 3) {
            memset(out, 0xAB, cap);
            if (if_http_resolve_url(base, loc, out, cap)) {
                if (strlen(out) >= cap) { fputs("RESOLVE CAP\n", stderr); abort(); }
                if (strncmp(out, "http://", 7) != 0) { fputs("RESOLVE SCHEME\n", stderr); abort(); }
            }
            if (cap > sizeof out / 4) break;
        }
    }

    /* (3) HTTP ヘッダ */
    {
        IfHttpHead h;
        if (if_http_head_parse(buf, (u64)n, &h)) {
            if (h.body_off > (u64)n) { fputs("BODY_OFF\n", stderr); abort(); }
            if (h.status < 100 || h.status > 999) { fputs("STATUS\n", stderr); abort(); }
        }
    }

    /* (4) chunked ボディ */
    {
        IfStr body;
        if (if_http_dechunk(&a, buf, (u64)n, &body)) {
            if (body.n > (u64)n + 16) { fputs("DECHUNK GROW\n", stderr); abort(); }
            if (body.n && !body.p) { fputs("DECHUNK NULL\n", stderr); abort(); }
        }
    }

    if_arena_destroy(&a);
    return 0;
}
