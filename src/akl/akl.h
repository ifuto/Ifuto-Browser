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

/* v0.3 再入呼び出し（高階関数のコールバック等）: fn（FUNC/NATIVE）を argc/argv で呼ぶ。
 * FUNC は VM を再入実行（outer スタックは GC ルートとして退避）。コールバック内の
 * 例外・budget 枯渇は false で rt->err に倒れる。再入深さ上限 AKL_MAX_REENTRY。 */
bool akl_call(AklRT *rt, AklVal fn, int argc, const AklVal *argv, AklVal *out);

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

/* ---- ネイティブ登録層 + オブジェクト（v0.3 前倒し: ブラウザ DOM 結合の前提基盤） ----
 * ホスト C 関数を akl 関数値としてスクリプトに露出する層。設計不変条件:
 *  - native 呼出は固定コスト（AKL_NATIVE_COST 命令相当）を insn budget から課金。
 *    native 実時間は budget が裁けないため、重い処理は宿主側で別途制限する責任。
 *  - native が akl_mkstring 等で作った一時値は nursery（上限 AKL_NURY_CAP 件）で
 *    GC から保護される。native 1 呼出で作る unpinned 値はその範囲に収める規約。
 *  - 失敗の規約は「明白に失敗」: akl_native_throw で例外相当に落とす。
 *    黙って undefined を返すでも落とすでもない第三の状態を作らない。
 *  - 登録系 API（register/global_set/mkobject/mknative/prop_set）は VM 停止中
 *    （akl_eval の外）でのみ有効。native コールバック内からの登録は拒否（err 設定）。
 * self: 通常呼出 `f()` は undefined、`o.f()` メソッド呼出はレシーバ o。
 * （バイトコード関数に this セマンティクスは未導入 — 言語に this は無い。native のみ
 *  self を受ける。BROWSERS 側 document.getElementById 等の前提） */
typedef AklVal (*AklNativeFn)(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata);

/* name をグローバル const として native 関数値に束縛。失敗時 false（err 設定）。 */
bool akl_native_register(AklRT *rt, const char *name, AklNativeFn fn, void *udata);
/* name をグローバル const として値 v に束縛（既出名は上書き。document/page 等の
 * ホストオブジェクト供給口）。失敗時 false。 */
bool akl_global_set(AklRT *rt, const char *name, AklVal v);
/* ホスト側オブジェクト/ネイティブ関数値の生成。生成直後は未ルートなので即座に
 * global_set/prop_set で束縛すること（akl_mkstring と同一のライフサイクル規約）。 */
AklVal akl_mkobject(AklRT *rt);
AklVal akl_mknative(AklRT *rt, AklNativeFn fn, void *udata);
/* obj（akl_mkobject 由来）のプロパティ操作。prop 数は 1 オブジェクト 64 まで。 */
bool akl_prop_set(AklRT *rt, AklVal obj, const char *name, AklVal v);
AklVal akl_prop_get(AklRT *rt, AklVal obj, const char *name); /* 無ければ undefined */
bool akl_is_object(AklRT *rt, AklVal v);
/* native コールバック内から例外相当を起こす: err を msg に設定し、VM に
 * 「この eval は失敗」を通知する。呼んだ側は直ちに AKL_VAL_UNDEF 相当を返すこと。 */
void akl_native_throw(AklRT *rt, const char *msg);

/* JS ToString（ホスト側プリミティブ。console.log / 将来の DOM バインドが使う）。
 * 全型で文字列値を返す（失敗は budget 枯渇のみ: undefined + err 設定、
 * VM 実行中なら native_err 連動で eval も失敗に倒れる）。
 * 規約: 戻り値はルートに積まない（nursery 浪費で任意可変引数処理を殺さないため）。
 * VM 実行中の呼出側は「直後に akl_as_str でコピーするか即座に束縛する」こと
 * （コピー前に他の確保を挟むと GC で失われ得る — akl_mkstring の pin 規約とは意図的に別）。 */
AklVal akl_tostring(AklRT *rt, AklVal v);

/* ---- ホストハンドル（DOM バインド向け不透明参照。AKL_OK_HANDLE） ----
 * C 側オブジェクトを vtab 経由で akl に露出する値型。ptr は GC 非管理:
 * ライフサイクル規約は「ptr はハンドル値が死ぬ RT より長く生きること」を呼出側が組織する
 * （ブラウザでは script RT は DOM arena 解体より先に破棄＝構造保証。akl は ptr を
 * 参照・解放・比較しない。所有は完全にホスト側）。 */
typedef struct AklHandleVTab {
    const char *tag; /* tostring/診断用（"[object TAG]"。NULL なら "Handle"） */
    /* unknown prop は false を返す → akl 側は undefined を返す（set は TypeError） */
    bool (*get)(AklRT *rt, void *ptr, const char *name, uint32_t len, AklVal *out);
    bool (*set)(AklRT *rt, void *ptr, const char *name, uint32_t len, AklVal v);
    /* メソッドディスパッチ。未定義名は false → "TypeError: not a function"。NULL 可 */
    bool (*call)(AklRT *rt, void *ptr, const char *name, uint32_t len,
                 int argc, const AklVal *argv, AklVal *out);
} AklHandleVTab;
AklVal akl_mkhandle(AklRT *rt, const AklHandleVTab *vt, void *ptr); /* pin 規約は mkobject 同様 */
bool akl_is_handle(AklRT *rt, AklVal v);

/* v0.3: ホスト側から配列を生成（items は n 個の値。n==0 は空配列）。
 * 生成直後は未ルートなので即座に束縛/返すこと（mkstring と同じライフサイクル規約）。 */
AklVal akl_mkarray(AklRT *rt, const AklVal *items, uint32_t n);
/* 配列の要素数（非配列は 0） */
uint32_t akl_arr_len(AklRT *rt, AklVal arr);


/* 組込側責任の上限引上げ（0 は据置）。既定値はブラウザ製品値（台帳の睩殺防止）。
 * クロスエンジン比較ベンチのように「打ち切りなしで収束する同一ソース」を
 * 走らせる用途でのみ既定から上げること。depth は固定（構造的保護）。 */
void akl_tune(AklRT *rt, uint64_t insn, uint32_t heap_mb, uint32_t max_objs);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
