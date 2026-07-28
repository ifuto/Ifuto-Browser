/* Ifuto Browser — common base types.
 * 方針: 隠れたアロケーション・隠れた終了を作らない。OOM は即 abort（ページ単位 arena のため
 * 部分回復の価値が低く、errno を伝播させる複雑性より fail-fast が防御として優位）。 */
#ifndef IFUTO_COMMON_H
#define IFUTO_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  i32;
typedef int64_t  i64;

/* 攻撃的入力へのハードリミット。これを超える構造は「壊れた文書ではなく攻撃」とみなす。 */
#define IF_MAX_INPUT_BYTES   (64u * 1024u * 1024u)  /* 1 ページ 64MB 上限 */
#define IF_MAX_ARENA_ALLOC   (256u * 1024u * 1024u) /* 単一 alloc の上限   */
#define IF_MAX_DOM_NODES     (4u * 1000u * 1000u)
#define IF_MAX_STACK_DEPTH   4096u                    /* ツリー構築の開いている要素スタック */

_Noreturn void if_fatal(const char *msg);

#endif
