/* Ifuto — 拡張サブシステム E1（docs/EXTENSIONS.md が唯一の正）。
 *
 * E1 機構の要点:
 *  - 1 拡張 = 1 ディレクトリ = manifest.txt（ext_manifest.h 文法）+ entry .js
 *  - 評価は拡張ごと独立 AklRT・akl 製品既定 budget（insn 10M 等、akl_tune しない）
 *  - 効果は「戻り値効果」スキーマ: 呼べるホスト関数は存在せず（akl に IO
 *    プリミティブが構造不在）、entry の最終式文の値が宣言ケイパビリティの
 *    効果に流れる（status → 起動トースト / log → stderr 行）
 *  - 失敗は当該拡張単位で打切り・本体継続（fail-stop 粒度 = 拡張）
 *  - ロード結果はサイレント失敗禁止で 1 拡張 1 行必ず report へ */
#ifndef IFUTO_EXT_H
#define IFUTO_EXT_H

#include "common.h"
#include <stdio.h>

struct IfChrome;

/* --ext DIR の process 全局設定（main.c が起動引数から 1 度だけ呼ぶ。
 * NULL/未設定なら chrome_init は既定 <store>/ext を黙って試す） */
void if_ext_set_dir(const char *dir);
const char *if_ext_dir(void);

/* dir 直下の各サブディレクトリを走査し E1 規則で評価・適用する。
 * explicit_req=true のとき dir が開けなければ 1 行エラーを出す
 * （既定自動ロード経路の不在は黙殺するための区別）。
 * 戻り値: ロード成功数。失敗拡張は report に理由行を出して継続。 */
i32 if_ext_scan_and_run(struct IfChrome *c, const char *dir, FILE *report, bool explicit_req);

#endif
