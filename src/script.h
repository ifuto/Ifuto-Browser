/* Ifuto — <script> akl 実行配線（v0.3。凍結正本: docs/SCRIPTING.md）。
 *
 * 凍結仕様の要点（変更は docs/SCRIPTING.md の改訂と同コミットでのみ）:
 *  - 対象: HTML 名前空間の <script> のみ（svg script・type!=javascript・src 属性付きは
 *    明白にスキップして数える。外部スクリプト取得は v1 非対象）。
 *  - 評価: 文書順・同一 AklRT（グローバル共有はブラウザ本家と同じ意味論）。
 *    eval 失敗（構文/例外/budget）は当該 script のみ打切り・後続と描画は継続し、
 *    1 行 `[script] FAILED: <理由>`（改行畳み・128 文字）を log へ。
 *  - kill switch: IF_SCRIPT=0 で完全 no-op。上限: 1 頁 128 script・script 単体 4MB。
 *  - budget は akl 製品既定（10M insn・ヒープ 16MB 等）のまま（akl_tune しない）。
 */
#ifndef IFUTO_SCRIPT_H
#define IFUTO_SCRIPT_H

#include "dom.h"
#include <stdio.h>

typedef struct {
    u32 n_run;     /* 実際に akl_eval した script 数 */
    u32 n_errors;  /* eval/前段で明白に失敗した数（後続は継続） */
    u32 n_skipped; /* src/type/空テキスト等で明白にスキップした数 */
} IfScriptReport;

/* dom_arena: DOM 所有 arena（document.title / textContent の確保先）。
 * log: NULL なら stderr。 */
IfScriptReport if_script_run(IfArena *dom_arena, IfDom *dom, FILE *log);

#endif
