/* akl_regex.h — Ifuto 軽量正規表現エンジン公開 API
 *
 * 設計方針:
 *  - パターンを 1 回スキャンして命令列（RexIns）にコンパイルし、
 *    バックトラッキング VM でマッチする（再帰下降マッチャ）。
 *  - 入出力は UTF-8 バイト列。文字はコードポイント単位で扱う。
 *  - 「対応しない構文はコンパイル時にエラー」を原則とする（明白な失敗）。
 *    現状の非対応: 先読み (?=..)/(?!..)、名前付きキャプチャ、バックリファレンス、
 *    文字クラス内の非 ASCII 文字・範囲、\u{...}。
 *  - 実行ステップ数と再帰深さに上限を設け、猫踏みパターン (a+)+b 等の
 *    指数爆発を有界化する。
 */
#ifndef IFUTO_AKL_REGEX_H
#define IFUTO_AKL_REGEX_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* フラグ（リテラル/コンストラクタで解釈されるビット） */
#define AKL_RX_F_IGNORE 0x01u  /* i */
#define AKL_RX_F_GLOBAL 0x02u  /* g */
#define AKL_RX_F_MULTI  0x04u  /* m */
#define AKL_RX_F_DOTALL 0x08u  /* s */
#define AKL_RX_F_STICKY 0x10u  /* y */
#define AKL_RX_F_UNICODE 0x20u /* u（受け付ける。ただし \u{...} は非対応） */

typedef struct AklRex AklRex;

/* パターンをコンパイル。失敗時 NULL を返し err_buf（err_cap>0 時）に原因を書く。
 * flags は AKL_RX_F_* の OR。 */
AklRex *akl_rex_compile(const uint8_t *pat, uint32_t pat_len, uint32_t flags,
                        char *err_buf, uint32_t err_cap);
void akl_rex_free(AklRex *rx);

/* キャプチャグループ数（$1..$n の n） */
uint32_t akl_rex_ncap(const AklRex *rx);
/* パターン文字列（source プロパティ用。コンパイル時のコピーを返す） */
const uint8_t *akl_rex_pat(const AklRex *rx, uint32_t *len);
uint32_t akl_rex_flags(const AklRex *rx);

/* マッチ試行: s[0..s_len) の start 位置から。
 *  - sticky でなければ start..s_len を順に走査して最初のマッチを返す。
 *  - 成功時 true、cap_beg[k]/cap_end[k]（k = 0..ncap）にグループ範囲を書く。
 *    cap_beg[0]=全体の開始、cap_end[0]=全体の終了。参加しなかったグループは
 *    UINT32_MAX。ncap は akl_rex_ncap(rx) 以上を渡すこと。
 *  - ステップ上限超過時は false を返し *lim に true をセットする。 */
bool akl_rex_match(const AklRex *rx, const uint8_t *s, uint32_t s_len, uint32_t start,
                   uint32_t *cap_beg, uint32_t *cap_end, uint32_t ncap, bool *lim);

#ifdef __cplusplus
}
#endif
#endif
