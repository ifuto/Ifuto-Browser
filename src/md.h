/* Ifuto — Markdown 変換層（v0.2, C11 self-made）
 * Markdown 拡張（GFM 表・脚注つき）→ HTML 変換器。出力は必ず既存の
 * WHATWG 準拠 HTML パーサ（if_parse_html）に食わせるので、MD へ信頼できる
 * ツリー規則は「変換器自身の決定的構成」だけに限定できる（多層防御: MD 変換器には
 * ツリー修復責任を持たせない）。
 *
 * 対応: ATX 見出し, 段落, 強調(strong/em), 打ち消し, インライン/フェンスコード,
 *      リンク/画像, 引用, ul/ol（インデント入れ子）, hr, GFM パイプ表,
 *      脚注 [^id] / [^id]:（参照順 numbering）, 自動リンク <http://…>, バックスラッシュ escape
 * 安全: 生 HTML 埋め込みは通さない（全テキストは必ず escape される。spec 偏差・台帳） */
#ifndef IFUTO_MD_H
#define IFUTO_MD_H

#include "arena.h"
#include "common.h"
#include "strutil.h"

/* 拡張子判定（.md / .markdown、case-insensitive） */
bool if_path_is_md(const char *path);

/* in を HTML に変換して out_html に書き出す（arena アロケート、UTF-8 透過） */
void if_md_to_html(IfArena *a, IfStr in, IfStr *out_html);

#endif
