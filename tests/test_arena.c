#include "tests.h"
#include "../src/arena.h"
#include <string.h>

void test_arena(void) {
    IfArena a;
    if_arena_init(&a, 128);

    /* 基本 alloc/書き込み */
    u8 *p = (u8 *)if_arena_alloc(&a, 32);
    memset(p, 0xAB, 32);
    CHECK(p != NULL);
    CHECK(((uintptr_t)p & 15u) == 0); /* 16B アライン保証 */

    /* 連続 alloc はアラインを維持 */
    for (int i = 0; i < 100; i++) {
        void *q = if_arena_alloc(&a, (u64)(i % 7) + 1);
        CHECK(((uintptr_t)q & 15u) == 0);
    }

    /* ブロック境界を越える大きな alloc */
    u8 *big = (u8 *)if_arena_alloc(&a, 5000);
    CHECK(big != NULL);
    memset(big, 1, 5000);
    CHECK(if_arena_reserved(&a) >= 5000 + 128);

    /* calloc はゼロ */
    u8 *z = (u8 *)if_arena_calloc(&a, 64);
    for (int i = 0; i < 64; i++) CHECK(z[i] == 0);

    /* grow: 内容保持 + 倍増 */
    u64 cap = 0;
    int *arr = NULL;
    for (int i = 0; i < 1000; i++) {
        arr = (int *)if_arena_grow(&a, arr, &cap, (u64)i + 1, sizeof(int));
        arr[i] = i * 3;
    }
    CHECK(cap >= 1000);
    for (int i = 0; i < 1000; i++) CHECK(arr[i] == i * 3);

    if_arena_destroy(&a);
    CHECK(a.head == NULL);
    CHECK(if_arena_reserved(&a) == 0);
}
