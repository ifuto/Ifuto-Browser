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
#define IF_MAX_INPUT_BYTES   (512u * 1024u * 1024u) /* 1 ページ 512MB 上限 */
#define IF_MAX_ARENA_ALLOC   (1u * 1024u * 1024u * 1024u) /* 単一 arena block 上限 1GB */
/* 上限の根拠（2026-07-31 実測台帳。恨みではなく計測で決める）:
 * 旧値 64MB/256MB は正当な巨大文書を敵性誤認して fail していた（.md 16MB でも発火）。
 * 新値の意図: 正当な大型ページは読めるが、暴走は依然として「綺麗な fail」で止まる
 * （このコンテナ RAM 4GB に対し render 実測係数 ~48KB/MB なら 512MB 印面は
 *   物理的に収まらないため、壁は OOM ではなく budget として先に効く＝安全側維持）。
 * 本丸の 3.3GB 級は、全量保持のアーキではなく v0.3 台帳の入力 compaction /
 * ストリーミング化で対処する（BENCH.md「巨大 IDM 計測」節を参照） */
#define IF_MAX_DOM_NODES     (4u * 1000u * 1000u)
#define IF_MAX_STACK_DEPTH   4096u                    /* ツリー構築の開いている要素スタック */

_Noreturn void if_fatal(const char *msg);

#endif
