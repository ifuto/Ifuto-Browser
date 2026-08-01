#include "arena.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* 厳格 -std=c11 では glibc が隠す定数（ABI 固定値） */
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif
#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

#define IF_ARENA_ALIGN 16u
/* 大ブロックは malloc を通さず mmap 直取り + MADV_HUGEPAGE。
 * 巨大文書の初回タッチは「新規 4KB ページのマイナーフォールト 9 万回≒60-180ms」（実測
 * 2026-08-01）で、THP 化は fault を 1/512 に減らす唯一の構造策（prefault でも fault は消えない）。
 * pad は free/munmap の区別に使う（旧来 0 初期化のままの小ブロックは malloc 経由）。 */
#define IF_ABLK_MMAP 0xAB12CD34EF567890ull
#define IF_ABLK_MMAP_MIN (2u << 20)

void if_arena_init(IfArena *a, u64 default_cap) {
    a->head = NULL;
    a->default_cap = default_cap ? default_cap : (64u * 1024u);
    a->total_reserved = 0;
}

static IfArenaBlock *if_arena_new_block(IfArena *a, u64 need) {
    u64 cap = a->default_cap;
    if (need > cap) cap = need;
    /* オーバーフロー検査: header + align + cap */
    if (cap > IF_MAX_ARENA_ALLOC) if_fatal("arena: single block too large (hostile input?)");
    IfArenaBlock *b;
    u64 total = sizeof(IfArenaBlock) + cap;
    if (cap >= IF_ABLK_MMAP_MIN) {
        /* 2MB アライン矯正つき mmap: 非アラインだと窓の両端 ~511 ページが 4KB fault 化する
         * （2026-08-01 実測: 4MB block × 89 で ~45k fault）。揃えれば全ページ THP。 */
        u64 ask = total + (2u << 20);
        void *m = mmap(NULL, ask, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m != MAP_FAILED) {
            uintptr_t base = (uintptr_t)m;
            uintptr_t aligned = (base + ((2u << 20) - 1)) & ~(uintptr_t)((2u << 20) - 1);
            if (aligned > base) munmap((void *)base, aligned - base);
            if (aligned + total < base + ask) munmap((void *)(aligned + total), base + ask - (aligned + total));
            (void)madvise((void *)aligned, total, MADV_HUGEPAGE);
            b = (IfArenaBlock *)aligned;
            b->pad = IF_ABLK_MMAP;
        } else {
            b = (IfArenaBlock *)malloc(total);
            if (!b) if_fatal("arena: out of memory");
            b->pad = 0;
        }
    } else {
        b = (IfArenaBlock *)malloc(total);
        if (!b) if_fatal("arena: out of memory");
        b->pad = 0;
    }
    b->next = a->head;
    b->cap = cap;
    b->used = 0;
    a->head = b;
    a->total_reserved += cap;
    return b;
}

void *if_arena_alloc(IfArena *a, u64 size) {
    if (size == 0) size = 1;
    if (size > IF_MAX_ARENA_ALLOC) if_fatal("arena: alloc too large (hostile input?)");
    IfArenaBlock *b = a->head;
    u64 off;
    if (b) {
        /* (used + align-1) & ~(align-1) — used < cap ≤ 256MB なので加算オーバーフローは不可 */
        off = (b->used + (IF_ARENA_ALIGN - 1)) & ~(u64)(IF_ARENA_ALIGN - 1);
        if (off > b->cap || size > b->cap - off) b = NULL; /* 収まらない */
    }
    if (!b) {
        b = if_arena_new_block(a, size + IF_ARENA_ALIGN);
        off = 0;
    }
    b->used = off + size;
    return (u8 *)b + sizeof(IfArenaBlock) + off;
}

void *if_arena_calloc(IfArena *a, u64 size) {
    void *p = if_arena_alloc(a, size);
    memset(p, 0, size ? size : 1);
    return p;
}

void if_arena_destroy(IfArena *a) {
    IfArenaBlock *b = a->head;
    while (b) {
        IfArenaBlock *next = b->next;
        if (b->pad == IF_ABLK_MMAP) munmap(b, sizeof(IfArenaBlock) + b->cap);
        else free(b);
        b = next;
    }
    a->head = NULL;
    a->total_reserved = 0;
}

u64 if_arena_reserved(const IfArena *a) {
    return a->total_reserved;
}

void *if_arena_grow(IfArena *a, void *ptr, u64 *cap, u64 need, u64 esz) {
    if (need <= *cap) return ptr;
    /* esz * newcap のオーバーフロー検査 */
    u64 newcap = *cap ? *cap : 8;
    while (newcap < need) {
        newcap *= 2;
        if (esz && newcap > IF_MAX_ARENA_ALLOC / esz) if_fatal("arena: grow overflow");
    }
    void *np = if_arena_alloc(a, newcap * esz);
    if (ptr && *cap) memcpy(np, ptr, (*cap) * esz);
    *cap = newcap;
    return np;
}
