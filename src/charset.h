/* Ifuto Browser — 文字コード層（v0.3: 「普通のブラウザ」S/A 完遂プログラム A1）
 * 日本語 Web の実効文字コード（Shift_JIS 系 / EUC-JP）→ UTF-8 変換。
 * 設計凍結（docs/CHARSET.md が正本）:
 *  - 100% 自製 C11。変換表は tools/gen_charset.py が python codec から生成
 *    （再生成一致オラクルで凍結。手編集禁止）
 *  - 対応ラベル: shift_jis 系（windows-31j/cp932 含む）・euc-jp 系・utf-8 系。
 *    未知ラベルは UTF-8 へ安全側フォールバック（キネマティクスに曖昧解釈を持ち込まない）
 *  - 波ダッシュ 6 件は cp932 採用（実在ページの実効挙動。Unicode 厳密系との乖離は明記済）
 *  - malformed は常に U+FFFD で 1 文字前进（WHATWG 準拠の restore 規則。無限ループ不成立）
 *  - UTF-8 既定（HTTP charset > BOM > meta prescan(4096B) > UTF-8）。MD は UTF-8 凍結で対象外
 */
#ifndef IF_CHARSET_H
#define IF_CHARSET_H

#include "common.h"
#include "strutil.h"
#include "arena.h"

typedef enum { IF_ENC_UTF8 = 0, IF_ENC_SJIS, IF_ENC_EUCJP } IfEnc;

/* charset ラベル（前後空白許容・ci）→ IfEnc。未知/非対応は IF_ENC_UTF8。 */
IfEnc if_charset_label(const char *p, u32 n);

/* HTTP Content-Type 値（"text/html; charset=x" 丸ごと。無指定は p=NULL）から
 * charset ラベルを抽出して IfEnc へ。無効・未知は IF_ENC_UTF8（＝ヘッダ不成立扱い）。 */
IfEnc if_charset_from_http(IfStr ctype_header);

/* 判定: HTTP > BOM(UTF-8) > meta prescan(先頭 4096B) > UTF-8。
 * out_utf8_bom: UTF-8 BOM を検出したか（呼び出し側が先頭 3B を剥がす責務を持つ）。 */
IfEnc if_charset_sniff(IfStr ctype_header, IfStr bytes, bool *out_utf8_bom);

/* bytes(enc) → UTF-8（arena 所有。呼び出し側の入力寿命と同じ）。
 * malformed は U+FFFD。enc == IF_ENC_UTF8 は呼ばないこと（恒等は呼び出し側で選ぶ）。 */
IfStr if_charset_decode(IfArena *a, IfStr bytes, IfEnc enc);

#endif
