/* Ifuto — 拡張 manifest パーサ（E1、docs/EXTENSIONS.md §2 が唯一の正）
 *
 * manifest.txt: 行ベース "key: value"（JSON パーサ不在 = 新 fuzz 面を背負わない。
 * manifest.json と名乗らない = 互換の嘘をつかない）。本モジュールは純粋関数のみ
 * （FS/グローバル不触 = fuzz 対象として最小リンクで単体監査できる分離）。
 *
 * 文法（E1 凍結）:
 *   行        : 空行 / '#' 始まりコメントはスキップ。それ以外は key ':' value
 *   トリム    : 前後の ' ' '\t'。行末の単一 '\r' は除去（CRLF 救済。session パーサと
 *               異なる規則 = doc 記載必須）
 *   key       : name | version | entry | permissions（完全一致・重複は失敗）
 *   name      : [A-Za-z0-9_.-]{1,63}（表示安全のため制御文字を構造排除）
 *   version   : 同 charset {1,23}
 *   entry     : basename のみ（'/' '\\' ・先頭 '.' ・空を拒否）・charset 同・≤120
 *   permissions: ',' 区切り・各 token は trim 後 {"status","log"} のいずれか。
 *               E1 は単一効果規則: 2 つ以上の宣言は失敗（複合効果の意味論を凍結していない）
 *   必須      : name / version / entry（permissions は省略可 = 効果なし評価のみ）
 *   サイズ    : src ≤ 64KB（IF_EXT_MANIFEST_CAP）
 *
 * 失敗は err へ 1 行理由（\n を含まない）。成功時 out の全フィールドが有効。 */
#ifndef IFUTO_EXT_MANIFEST_H
#define IFUTO_EXT_MANIFEST_H

#include "common.h"
#include "strutil.h"

#define IF_EXT_MANIFEST_CAP 65536u
#define IF_EXT_NAME_CAP 64
#define IF_EXT_VER_CAP 24
#define IF_EXT_ENTRY_CAP 128
#define IF_EXT_ERR_CAP 96

/* perm 値（E1 単一効果） */
enum { IF_EXT_PERM_NONE = 0, IF_EXT_PERM_STATUS = 1, IF_EXT_PERM_LOG = 2 };

typedef struct {
    char name[IF_EXT_NAME_CAP];
    char version[IF_EXT_VER_CAP];
    char entry[IF_EXT_ENTRY_CAP];
    u8 perm; /* IF_EXT_PERM_* */
} IfExtManifest;

bool if_ext_manifest_parse(IfStr src, IfExtManifest *out, char *err, u32 errcap);
const char *if_ext_perm_name(u8 perm); /* 表示用: "none"/"status"/"log" */

#endif
