/* Akl — Ifuto 自作 JS エンジン（C11, JIT なし）公開 API。
 *
 * 設計不変条件（CHROME_SCOPE §0.1 / ARCHITECTURE v0.4 採択事由）:
 *   - JIT を持たない: 実行可能書き込みページは構造的にゼロ（W^X は OS 全域で成立）。
 *   - 値は NaN-boxed 8B（AklVal）。double 全値域を保持しつつ、タグ空間は
 *     0xFFFF 上位帯。演算結果の double は canonical NaN に正規化するため、
 *     v0.0 の API 面からはタグ空間に衝突する double を生成する経路が存在しない
 *     （bit レベルの API（TypedArray 等）を将来足す時にこの不変条件の再監査が必須）。
 *   - ヒープ参照はポインタではなく rt 所有のオブジェクト配列への u32 index。
 *     dangling 生ポインタを API 面に出さない（UAF の構造的排除 + メモリ極限方針）。
 *   - 全ての反復・再帰に budget/上限（無限ループ・スタック枯渇で宿主を殺さない）。
 *
 * v0.0 公開面は eval/値検査のみ。内部表現はすべて非公開。
 */
#ifndef AKL_H
#define AKL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t AklVal;
typedef struct AklRT AklRT;

AklRT *akl_new(void);
void   akl_free(AklRT *rt);

/* src をプログラムとして実行し、最後に評価された式文の値を *out へ。
 * 成功なら true（*out が有効）。構文エラー・実行時例外・budget 超過なら false
 * （理由は akl_error の文字列）。out は NULL 可。 */
bool akl_eval(AklRT *rt, const char *src, AklVal *out);

/* 1 回の eval で許す命令数の上限（既定 10,000,000）。0 にすると呼び出し側責任で
 * 「全命令即枯渇」になるので 1 以上を渡すこと。ブラウザ統合時はタブ経路ごとに設定する。 */
void akl_set_insn_budget(AklRT *rt, uint64_t budget);

/* CoJIT（静的検証駆動の AOT 特化: runtime codegen は行わない）の ON/OFF。
 * 既定 ON。特化器は失敗しても汎用命令のまま残るため、off は主に監査・差分検証用 */
void akl_set_cojit(AklRT *rt, int enabled);
/* CoJIT が適用した特化の累積数（観測・検証用） */
uint32_t akl_cojit_count(AklRT *rt);

/* 直近のエラー文言（rt 所有、次の akl_eval で上書き。無ければ ""） */
const char *akl_error(AklRT *rt);

/* 値検査 API（型が合えば true を返し out を満たす） */
bool akl_as_num(AklVal v, double *out);
bool akl_as_bool(AklVal v, bool *out);
bool akl_is_null(AklVal v);
bool akl_is_undefined(AklVal v);
bool akl_is_string(AklRT *rt, AklVal v); /* rt 非依存にできない（obj 表を見るため） */
/* string のとき内部ポインタを返す（rt 所有。len はバイト長。非文字列なら NULL） */
const char *akl_as_str(AklRT *rt, AklVal v, uint32_t *len);

/* ホスト側からの値生成（V8 API ファサード層・将来の DOM バインディングが使う）。
 * 数値/bool/null/undefined は純粋関数。文字列は engine GC ヒープに確保する:
 * 呼出し時点で VM 停止中（gc_live==false）なら GC は発火せず hard budget のみ適用。
 * **生成直後にどこからも参照されない文字列は次回 GC で回収され得る**（ファサード側は
 * 値を即座に使うか Utf8Value へ写すこと。V8 における Persistent handle 相当は v0 未提供）。
 * 失敗時は err を設定して undefined を返す。 */
AklVal akl_mknum(double d);
AklVal akl_mkbool(bool b);
AklVal akl_mknull(void);
AklVal akl_mkundefined(void);
AklVal akl_mkstring(AklRT *rt, const char *s, uint32_t len);


/* 組込側責任の上限引上げ（0 は据置）。既定値はブラウザ製品値（台帳の睩殺防止）。
 * クロスエンジン比較ベンチのように「打ち切りなしで収束する同一ソース」を
 * 走らせる用途でのみ既定から上げること。depth は固定（構造的保護）。 */
void akl_tune(AklRT *rt, uint64_t insn, uint32_t heap_mb, uint32_t max_objs);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
