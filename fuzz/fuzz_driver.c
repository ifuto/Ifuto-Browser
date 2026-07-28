/* Ifuto — fuzz ドライバ。全パイプライン（parse → style → layout → render）を
 * 幅を変えて 2 回回す。crash/UB/sanitizer 違反があれば abort → 検出。静かに 0 で抜けるのが正。 */
#include "common.h"
#include "arena.h"
#include "dom.h"
#include "css.h"
#include "layout.h"
#include "render.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    IfArena a;
    if_arena_init(&a, 1 << 18);
    u64 cap = 1 << 16, n = 0;
    u8 *buf = (u8 *)if_arena_alloc(&a, cap);
    size_t got;
    while ((got = fread(buf + n, 1, 1 << 16, f)) > 0) {
        n += got;
        if (n > IF_MAX_INPUT_BYTES) break;
        buf = (u8 *)if_arena_grow(&a, buf, &cap, n + (1 << 16), 1);
    }
    fclose(f);
    IfStr in = if_str((const char *)buf, (u32)n);

    IfDom *dom = if_parse_html(&a, in);
    if_style_apply(&a, dom);
    /* 3 種の幅で: 折り返し境界条件を総当たり気味に打つ */
    for (i32 w = 1; w <= 3; w++) {
        IfLayout *lay = if_layout_build(&a, dom, w * 13 + 1);
        IfGrid *grid = if_render_grid(&a, lay);
        IfStr out = if_render_emit(&a, grid, 0);
        (void)out;
    }
    if_arena_destroy(&a);
    return 0;
}
