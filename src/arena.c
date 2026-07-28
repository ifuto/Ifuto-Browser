#include "arena.h"
#include <stdlib.h>
#include <string.h>

#define IF_ARENA_ALIGN 16u

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
    IfArenaBlock *b = (IfArenaBlock *)malloc(sizeof(IfArenaBlock) + cap);
    if (!b) if_fatal("arena: out of memory");
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
        free(b);
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
