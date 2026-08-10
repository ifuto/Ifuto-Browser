/* Ifuto — ゼロコピー文字列スライス。
 * 原則: 入力バッファを指すスライスをそのまま流通させ、文字列ごとの malloc を撲滅する。
 * len は u32（ページ上限 64MB なので十分）。NUL 終端は期待しない。 */
#ifndef IFUTO_STRUTIL_H
#define IFUTO_STRUTIL_H

#include "common.h"
#include <string.h>

typedef struct __attribute__((packed)) {
    const char *p;   /* 8B（packed で 8B アライン保証なし — x86-64/ARMv7+ は unaligned ネイティブ） */
    u32 n;           /* 4B */
} IfStr; /* 12B（通常 align なら 16B。IfNode 80→72B / IfAttr 32→24B の省メモリ。2026-08-10 実測で採用判断） */

#define IF_S(lit) ((IfStr){ (lit), (u32)(sizeof(lit) - 1) })

static inline IfStr if_str(const char *p, u32 n) { IfStr s; s.p = p; s.n = n; return s; }
static inline bool if_str_empty(IfStr s) { return s.n == 0; }

static inline bool if_str_eq(IfStr a, IfStr b) {
    return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0);
}

static inline u8 if_ascii_lower(u8 c) {
    return (u8)((c >= 'A' && c <= 'Z') ? (c + 32) : c);
}

/* ASCII 限定の大文字小文字無視比較（タグ名・属性名・HTTP ヘッダ用） */
static inline bool if_str_eq_ci(IfStr a, IfStr b) {
    if (a.n != b.n) return false;
    for (u32 i = 0; i < a.n; i++)
        if (if_ascii_lower((u8)a.p[i]) != if_ascii_lower((u8)b.p[i])) return false;
    return true;
}

static inline bool if_str_is_ws_only(IfStr s) {
    for (u32 i = 0; i < s.n; i++) {
        char c = s.p[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') return false;
    }
    return true;
}

/* 部分文字列検索（スライス版 strstr。NUL 終端は一切要求しない）。
 * needle は C 文字列（内部ページ検証・属性値検査などリテラル用途が主）。
 * memcmp 走査のみで allocator を触らない = メモリ法則に忠実。 */
static inline bool if_str_contains(IfStr hay, const char *needle) {
    size_t nn = strlen(needle);
    if (nn == 0) return true;
    if (nn > hay.n) return false;
    for (u32 i = 0; i + nn <= hay.n; i++)
        if (memcmp(hay.p + i, needle, nn) == 0) return true;
    return false;
}

static inline IfStr if_str_trim(IfStr s) {
    u32 a = 0, b = s.n;
    while (a < b && (s.p[a] == ' ' || s.p[a] == '\t' || s.p[a] == '\n' || s.p[a] == '\r' || s.p[a] == '\f')) a++;
    while (b > a && (s.p[b-1] == ' ' || s.p[b-1] == '\t' || s.p[b-1] == '\n' || s.p[b-1] == '\r' || s.p[b-1] == '\f')) b--;
    return if_str(s.p + a, b - a);
}

#endif
