/* V8x — Ifuto 自作 JS エンジン（C11, JIT なし）公開 API。
 *
 * 設計不変条件（CHROME_SCOPE §0.1 / ARCHITECTURE v0.4 採択事由）:
 *   - JIT を持たない: 実行可能書き込みページは構造的にゼロ（W^X は OS 全域で成立）。
 *   - 値は NaN-boxed 8B（V8xVal）。double 全値域を保持しつつ、タグ空間は
 *     0xFFFF 上位帯。演算結果の double は canonical NaN に正規化するため、
 *     v0.0 の API 面からはタグ空間に衝突する double を生成する経路が存在しない
 *     （bit レベルの API（TypedArray 等）を将来足す時にこの不変条件の再監査が必須）。
 *   - ヒープ参照はポインタではなく rt 所有のオブジェクト配列への u32 index。
 *     dangling 生ポインタを API 面に出さない（UAF の構造的排除 + メモリ極限方針）。
 *   - 全ての反復・再帰に budget/上限（無限ループ・スタック枯渇で宿主を殺さない）。
 *
 * v0.0 公開面は eval/値検査のみ。内部表現はすべて非公開。
 */
#ifndef V8X_H
#define V8X_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t V8xVal;
typedef struct V8xRT V8xRT;

V8xRT *v8x_new(void);
void   v8x_free(V8xRT *rt);

/* src をプログラムとして実行し、最後に評価された式文の値を *out へ。
 * 成功なら true（*out が有効）。構文エラー・実行時例外・budget 超過なら false
 * （理由は v8x_error の文字列）。out は NULL 可。 */
bool v8x_eval(V8xRT *rt, const char *src, V8xVal *out);

/* 1 回の eval で許す命令数の上限（既定 10,000,000）。0 にすると呼び出し側責任で
 * 「全命令即枯渇」になるので 1 以上を渡すこと。ブラウザ統合時はタブ経路ごとに設定する。 */
void v8x_set_insn_budget(V8xRT *rt, uint64_t budget);

/* CoJIT（静的検証駆動の AOT 特化: runtime codegen は行わない）の ON/OFF。
 * 既定 ON。特化器は失敗しても汎用命令のまま残るため、off は主に監査・差分検証用 */
void v8x_set_cojit(V8xRT *rt, int enabled);
/* CoJIT が適用した特化の累積数（観測・検証用） */
uint32_t v8x_cojit_count(V8xRT *rt);

/* 直近のエラー文言（rt 所有、次の v8x_eval で上書き。無ければ ""） */
const char *v8x_error(V8xRT *rt);

/* 値検査 API（型が合えば true を返し out を満たす） */
bool v8x_as_num(V8xVal v, double *out);
bool v8x_as_bool(V8xVal v, bool *out);
bool v8x_is_null(V8xVal v);
bool v8x_is_undefined(V8xVal v);
bool v8x_is_string(V8xRT *rt, V8xVal v); /* rt 非依存にできない（obj 表を見るため） */
/* string のとき内部ポインタを返す（rt 所有。len はバイト長。非文字列なら NULL） */
const char *v8x_as_str(V8xRT *rt, V8xVal v, uint32_t *len);

/* ホスト側からの値生成（V8 API ファサード層・将来の DOM バインディングが使う）。
 * 数値/bool/null/undefined は純粋関数。文字列は engine GC ヒープに確保する:
 * 呼出し時点で VM 停止中（gc_live==false）なら GC は発火せず hard budget のみ適用。
 * **生成直後にどこからも参照されない文字列は次回 GC で回収され得る**（ファサード側は
 * 値を即座に使うか Utf8Value へ写すこと。V8 における Persistent handle 相当は v0 未提供）。
 * 失敗時は err を設定して undefined を返す。 */
V8xVal v8x_mknum(double d);
V8xVal v8x_mkbool(bool b);
V8xVal v8x_mknull(void);
V8xVal v8x_mkundefined(void);
V8xVal v8x_mkstring(V8xRT *rt, const char *s, uint32_t len);


/* 組込側責任の上限引上げ（0 は据置）。既定値はブラウザ製品値（台帳の睩殺防止）。
 * クロスエンジン比較ベンチのように「打ち切りなしで収束する同一ソース」を
 * 走らせる用途でのみ既定から上げること。depth は固定（構造的保護）。 */
void v8x_tune(V8xRT *rt, uint64_t insn, uint32_t heap_mb, uint32_t max_objs);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
