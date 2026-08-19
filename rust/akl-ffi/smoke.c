/* akl-ffi の C リンク検証（スモークテスト）。
 *
 * ビルド & 実行:
 *   cd rust && cargo build --release -p akl-ffi
 *   cc ../rust/akl-ffi/smoke.c -I.. -o /tmp/smoke \
 *      ../rust/target/release/libakl_ffi.a -lpthread -ldl -lm && /tmp/smoke
 *
 * 本物の akl.h（src/akl/akl.h）を include し、Rust の staticlib にリンクして
 * eval / native 登録 / ハンドル / エラー報告が ABI 互換で動くことを検証する。
 */
#include "src/akl/akl.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static AklVal c_double(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)rt; (void)self; (void)udata;
    double d = 0;
    if (argc > 0 && akl_as_num(argv[0], &d)) return akl_mknum(d * 2);
    return akl_mkundefined();
}
static bool h_get(AklRT *rt, void *ptr, const char *name, uint32_t len, AklVal *out) {
    (void)rt; (void)ptr;
    if (len == 5 && memcmp(name, "title", 5) == 0) { *out = akl_mknum(999); return true; }
    return false;
}
static const AklHandleVTab h_vt = { "Test", h_get, NULL, NULL };

int main(void) {
    AklRT *rt = akl_new();
    assert(rt);
    AklVal v; double d; bool b;

    assert(akl_eval(rt, "1 + 2;", &v));
    assert(akl_as_num(v, &d) && d == 3.0);

    assert(akl_native_register(rt, "double", c_double, NULL));
    assert(akl_eval(rt, "double(21) === 42;", &v));
    assert(akl_as_bool(v, &b) && b);

    assert(akl_global_set(rt, "document", akl_mkhandle(rt, &h_vt, NULL)));
    assert(akl_eval(rt, "document.title;", &v));
    assert(akl_as_num(v, &d) && d == 999.0);

    assert(!akl_eval(rt, "bad {", &v));
    assert(strlen(akl_error(rt)) > 0);

    akl_free(rt);
    printf("C smoke test OK\n");
    return 0;
}
