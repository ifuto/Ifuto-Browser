/* Akl fuzz ドライバ: 入力を JS ソースとして eval する。crash/UB/sanitizer 違反が
 * あれば abort で検出。budget は fuzz 用に小さく固定（入力に無限ループが混ざっても
 * ドライバ全体が高速に回るように）。静かに 0 で抜けるのが正。 */
#include "akl/akl.h"
#include <stdio.h>
#include <stdlib.h>

#define FUZZ_MAX_BYTES (256u << 10)

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    unsigned char *buf = (unsigned char *)malloc(FUZZ_MAX_BYTES + 1);
    if (!buf) { fclose(f); return 1; }
    size_t n = fread(buf, 1, FUZZ_MAX_BYTES, f);
    fclose(f);
    buf[n] = 0; /* akl_eval は C 文字列入力。NUL 以降は切れるだけ（クラッシュ検査が目的） */

    AklRT *rt = akl_new();
    if (!rt) { free(buf); return 1; }
    akl_set_insn_budget(rt, 200000);
    (void)akl_eval(rt, (const char *)buf, NULL);
    akl_free(rt);
    free(buf);
    return 0;
}
