//! バイトコード VM（フェーズ 3b）。
//!
//! C 実装 `src/akl/akl.c` の `vm_exec`（スタックマシン VM）を、実 JS 値
//! （[`crate::AklVal`]）と単一ヒープ（[`crate::obj::ObjTable`]）の上に移植する。
//!
//! C 実装からの移植対応:
//! | C (akl.c) | Rust |
//! |---|---|
//! | `vm_exec`（computed-goto 巨大ループ） | [`Runtime::run`]（match ループ） |
//! | 値スタック `rt->stk` + `AKL_PUSH`/`AKL_POP` マクロ | `Vec<AklVal>` の push/pop |
//! | フレーム（`base` オフセット + `frames[]`） | [`Frame`]（locals を明示保持） |
//! | `AKL_POP` の下限検査（fail-stop） | `Vec::pop().ok_or(VmError::StackUnderflow)` |
//! | `AKL_NEXT`（pc 送り） | `pc` の明示更新 |
//!
//! # C 実装で実際に起きたバグと、Rust で構造的に消える理由
//!
//! 1. **フレーム設定後の GC ルート深さ同期漏れ**（v0.7 実測）: C は呼出前に `gc_sp`
//!    を同期し忘れると、実行中 GC が新フレームの cap env/ローカルを回収して
//!    `"cap env chain broken"` になった。Rust では locals が [`Frame`] に
//!    所有され、値スタックと別に GC ルートへ明示的に渡すため、同期漏れが起きない。
//!
//! 2. **スタック下限検査の漏れ**: C の `AKL_POP` はマクロで、呼び出し箇所によって
//!    検査が抜け得た。Rust の `Vec::pop` は `Option` を返すため、`?` で網羅的に
//!    検査される（検査しないコードはコンパイルできない）。
//!
//! 3. **命令即値の範囲検査漏れ**: C の `LLOAD slot` は `base + slot < sp` を各所で
//!    手動検査。Rust では `locals.get(slot)` が範囲外を `None` で返す（＝構造的安全）。
//!
//! # 既知の近似（今後のフェーズ）
//!
//! - **クロージャ**: [`crate::obj::Obj::Func`] は `env` を保持するが、VM の
//!   `LLoad`/`LStore` は自フレームの locals のみを参照する。自由変数の env 経由解決
//!   （C の `CELOAD`/`CESTORE` 相当）はパーサ・codegen フェーズで導入する。
//! - **ルーズ等値 `==`**: 数値・文字列・null/undefined・真偽値の単純化版。オブジェクトの
//!   ToPrimitive 強制は未対応（`===` は完全実装）。
//! - **文字列連結以外の ToString**: 数値/真偽値/undefined/null は正確。オブジェクトは
//!   `[object Object]` / `[object Array]` の近似。

#![forbid(unsafe_code)]
#![warn(missing_docs)]

use crate::obj::{Obj, ObjId, ObjTable};
use crate::string::Interner;
use crate::{int_add, AklVal, IntAdd};

/// 比較演算子（C の `CJMPF_L` 等の cmp 表: 0=Lt 1=Le 2=Gt 3=Ge）。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Cmp {
    /// `<`
    Lt,
    /// `<=`
    Le,
    /// `>`
    Gt,
    /// `>=`
    Ge,
}

impl Cmp {
    /// 比較を適用する（i64 全域で panic しない）。
    pub const fn apply(self, a: i64, b: i64) -> bool {
        match self {
            Cmp::Lt => a < b,
            Cmp::Le => a <= b,
            Cmp::Gt => a > b,
            Cmp::Ge => a >= b,
        }
    }
}

/// バイトコード関数（C の `AklFuncEnt` + FUNC obj 相当）。
///
/// コード本体（`Vec<Op>`）はヒープに置かず関数表（[`Runtime::funcs`]）に保持する。
/// ヒープ側の [`Obj::Func`] は `fidx`（関数表 index）だけを持つ（C の `code_off` と同型）。
#[derive(Clone, Debug, PartialEq)]
pub struct FuncObj {
    /// 関数本体のバイトコード列。
    pub code: Vec<Op>,
    /// 関数名（intern 済み文字列 ObjId。無名は None）。
    pub name: Option<ObjId>,
    /// パラメータ数。
    pub n_params: usize,
    /// rest パラメータのローカルスロット（無ければ None）。余剰引数はここに配列で束縛。
    pub rest_slot: Option<u32>,
    /// ローカル変数数（パラメータ含む）。
    pub n_locals: usize,
    /// ジェネレータ関数（`function*`）か。呼び出しは `Obj::Gen` を生成する。
    pub is_gen: bool,
}

/// VM 命令（C の `OP_*` のコア部分。融合命令・CoJIT 特化は未移植）。
///
/// 即値は全て `Copy`（i32 / f64 / ObjId / u32 / u8）で、命令境界は型で保証される
/// （C の可変長即値 + `akl_op_imm_len` 表の drift 問題が構造的に消える）。
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Op {
    /// i32 定数を push（C の `CONST_I`）。
    ConstI(i32),
    /// f64 定数を push（C の `CONST_D`）。
    ConstD(f64),
    /// intern 済み文字列定数を push（C の `CONST_STR`。ObjId はヒープ文字列）。
    ConstStr(ObjId),
    /// `true` を push。
    True,
    /// `false` を push。
    False,
    /// `null` を push。
    Null,
    /// `undefined` を push。
    Undef,
    /// 加算（int fast path + double + 文字列連結。C の `ADD`）。
    Add,
    /// 減算。
    Sub,
    /// 乗算。
    Mul,
    /// 除算（常に double。JS の `7/2 == 3.5`）。
    Div,
    /// 剰余（int fast path + f64）。
    Mod,
    /// `<`。
    Lt,
    /// `<=`。
    Le,
    /// `>`。
    Gt,
    /// `>=`。
    Ge,
    /// ルーズ等値 `==`。
    Eq,
    /// ルーズ不等 `!=`。
    Ne,
    /// 厳密等値 `===`。
    Seq,
    /// 厳密不等 `!==`。
    Sne,
    /// 論理積 `&&`（両辺を真偽値化して返す近似。JS の値返しは未対応）。
    And,
    /// 論理和 `||`（同上）。
    Or,
    /// 論理否定 `!`。
    Not,
    /// ビット NOT `~`（ToInt32）。
    BNot,
    /// ビット AND `&`。
    BAnd,
    /// ビット OR `|`。
    BOr,
    /// ビット XOR `^`。
    BXor,
    /// 左シフト `<<`。
    BShl,
    /// 算術右シフト `>>`。
    BShr,
    /// 論理右シフト `>>>`。
    BUShr,
    /// 単項マイナス `-`。
    Neg,
    /// 単項プラス `+`（数値化）。
    Pos,
    /// `typeof`。
    Typeof,
    /// スタックから 1 個捨てる。
    Pop,
    /// スタックから 1 個 pop して `last_val` に保存（C の `POPV`。式文の値）。
    PopV,
    /// スタック先頭を複製。
    Dup,
    /// ローカル slot を push（C の `LLOAD`）。
    LLoad(u32),
    /// ローカル slot へ pop した値を保存（C の `LSTORE`）。
    LStore(u32),
    /// グローバル変数（name 指定）を push（C の `GLOAD`）。
    GLoad(ObjId),
    /// グローバル変数を push。未宣言なら undefined（`typeof undeclared` 用。C の
    /// `GLOAD` は未宣言で ReferenceError だが、JS の `typeof` は投げない）。
    GLoadSafe(ObjId),
    /// グローバル変数へ pop した値を保存（C の `GSTORE`）。
    GStore(ObjId),
    /// 無条件ジャンプ。
    Jmp(u32),
    /// pop して偽ならジャンプ（C の `JMPF`）。
    JmpF(u32),
    /// pop して真ならジャンプ（C の `JMPT`）。
    JmpT(u32),
    /// pop して null/undefined でなければジャンプ（`??=` の短絡用）。
    JmpNotNullish(u32),
    /// 関数呼び出し（argc 個の引数 + callee を pop）。
    Call(u8),
    /// 配列を spread してスタックへ展開し、要素数をグローバル `name` のカウンタへ加算
    /// （`f(...a, b)` の引数展開。C の `ARRSPREADC` 相当）。
    ArrSpreadC(ObjId),
    /// 動的 argc の呼び出し。argc はグローバル `name` のカウンタ（C の `CALLN` 相当）。
    CallDyn(ObjId),
    /// メソッド呼び出し（`[receiver, callee, ...args]`。callee を this=receiver で呼ぶ）。
    MCall(u8),
    /// 名前指定のメソッド呼び出し（`[receiver, ...args]`。name を this=receiver で解決して呼ぶ）。
    /// C の `OP_MCALL`（argc + name）相当。ハンドル（DOM 等）は vtable の `call` へ直接
    /// ディスパッチする（プロパティ取得とメソッド呼び出しを構文的に分離するため）。
    MCallName {
        /// メソッド名（intern 済み文字列 ObjId）。
        name: ObjId,
        /// 引数の個数。
        argc: u8,
    },
    /// 名前指定・動的 argc のメソッド呼び出し（spread 引数用）。argc はグローバル
    /// `argc_name` のカウンタ。
    MCallDyn {
        /// メソッド名（intern 済み文字列 ObjId）。
        name: ObjId,
        /// argc カウンタのグローバル名 ObjId。
        argc_name: ObjId,
    },
    /// コンストラクタ呼び出し（`new`。argc 個の引数 + callee を pop。this=新オブジェクト）。
    New(u8),
    /// 正規表現オブジェクトを生成して push（pattern 文字列 ObjId を pop、flags は即値）。
    NewRegex(u32),
    /// 関数の prototype を設定（proto, fn を pop、fn を push。fn_protos テーブル登録）。
    SetFnProto,
    /// 親クラス継承（pop proto, pop parent_ctor → proto の [[Prototype]] = parent.prototype。
    /// class extends のメソッド継承チェーン構築）。
    LinkSuper,
    /// 戻る（TOS を返り値として pop）。
    Ret,
    /// 関数オブジェクトを生成して push（C の `MAKEF`。fidx = 関数表 index）。
    MakeF(u32),
    /// 現在フレームの `this` を push。
    This,
    /// 空のプレーンオブジェクトを push（C の `OBJNEW`）。
    ObjNew,
    /// プロパティ読み出し（name 指定。obj を pop、値を push。C の `PLOAD`）。
    PLoad(ObjId),
    /// プロパティ書き込み（name 指定。val, obj を pop、val を push。C の `PSTORE`）。
    PStore(ObjId),
    /// 配列リテラル（count 個 pop して配列を push。C の `ANEW`）。
    ArrNew(u32),
    /// 配列へ要素を追加（val, arr を pop、arr を push。C の `ARRPUSH` 相当）。
    ArrPush,
    /// 配列へ別配列の全要素を追加（src, arr を pop、arr を push。C の `ARRPUSHALL` 相当）。
    ArrPushAll,
    /// オブジェクト spread（pop src → TOS の OBJ に全プロパティをコピー。C の `OBJSPREAD`）。
    ObjSpread,
    /// オブジェクト rest（pop src → 全プロパティをコピーした新 OBJ を push。C の `OBJREST`）。
    ObjRest,
    /// 配列の残り [start..n) を新配列に（start, arr を pop、新配列を push。C の `ARRREST` 相当）。
    ArrRest,
    /// 要素読み出し（idx, obj を pop、値を push。C の `AGET`）。
    AGet,
    /// 要素書き込み（val, idx, obj を pop、val を push。C の `ASET`）。
    ASet,
    /// `instanceof`（obj, f を pop、真偽値を push）。
    Instanceof,
    /// `in`（obj, key を pop、真偽値を push）。
    In,
    /// `delete obj[key]`（idx, obj を pop、真偽値を push）。
    Delete,
    /// `throw`（TOS の例外値を投げる。C の `OP_THROW`）。
    Throw,
    /// try ブロック入口（catch_pc を現在フレームに記録。C の `OP_TRY_PUSH` 相当）。
    TryPush(u32),
    /// try ブロック終了（catch ハンドラを解除。C の `OP_TRY_LEAVE` 相当）。
    TryPop,
    /// 環境オブジェクトを生成（n 個 pop して Env を push。C の env 生成）。
    MakeEnv(u32),
    /// クロージャを生成（env を pop、Func{fidx, env} を push。C の `MAKEF` env 版）。
    MakeClosure(u32),
    /// 捕捉変数の読み出し（`frame.env` から `depth` 段親を辿り `vals[idx]` を push）。
    /// C の `CELOAD` 相当。`depth` は env チェーンの段数（0 = 自 env）。
    CeLoad(u32, u32),
    /// 捕捉変数への書き込み（pop → `frame.env` の `depth` 段親の `vals[idx]`）。
    /// C の `CESTORE` 相当。
    CeStore(u32, u32),
    /// BigInt 定数を push（i64 保持近似。`Obj::BigInt` を生成）。
    BigInt(i64),
    /// `yield expr`（ジェネレータ。TOS の値を yield し、再開位置を保存する）。
    Yield,
    /// `await expr`（TOS が解決済み Promise なら値を unwrap して push）。
    Await,
    /// TOS の値を解決済み Promise（`Obj::Promise { state: 1, .. }`）で包む
    /// （async 関数の戻り値ラップ用）。
    PromiseWrap,
    /// 停止（C の `HALT`）。
    Halt,
}

/// VM 実行エラー。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum VmError {
    /// 値スタックが不足。
    StackUnderflow,
    /// ローカル slot が範囲外。
    LocalOob,
    /// グローバル変数が見つからない（未宣言参照）。
    GlobalNotFound,
    /// 呼び出し対象が関数でない。
    NotCallable,
    /// ジャンプ先がコード範囲外。
    JumpOob,
    /// ヒープ上限等で値生成に失敗。
    Oom,
    /// プロパティ/要素アクセスの対象がオブジェクトでない。
    NotObject,
    /// 未捕捉の例外（`throw` が catch されずに伝搬した）。
    Thrown(AklVal),
    /// 命令バジェット枯渇（無限ループ等の打ち切り。C の `instruction budget exhausted`）。
    BudgetExhausted,
}

/// VM 実行ループの終了理由（[`Runtime::run_loop`] の戻り値）。
/// 通常の関数実行は [`RunEnd::Value`] のみ。ジェネレータの `yield` は
/// [`RunEnd::Yield`]（再開位置は `yield` 命令内で保存済み）。
enum RunEnd {
    /// 関数が return / 暗黙終端した（TOS が返り値）。
    Value(AklVal),
    /// ジェネレータが yield した（値は yield した値）。
    Yield(AklVal),
}

/// ネイティブ関数（C の `AklNativeFn` 相当）。
///
/// `this` はメソッド呼び出しのレシーバ。`args` は引数列。戻り値はエラー付き
/// （失敗は実行を停止させる）。
pub type NativeFn = fn(&mut Runtime, AklVal, &[AklVal]) -> Result<AklVal, VmError>;

/// ホスト（FFI 層）提供のネイティブ関数。
///
/// `data` は FFI 層が C ネイティブを識別する不透明 `u64`（例: 登録表 index）。
/// 本クレートは `data` を解釈せず、そのままコールバックへ渡すだけ（unsafe は
/// FFI 層に隔離され、`akl-core` は `#![forbid(unsafe_code)]` を維持する）。
pub type ForeignNativeFn = fn(&mut Runtime, AklVal, &[AklVal], u64) -> Result<AklVal, VmError>;

/// ホストハンドル（DOM 要素等の不透明参照）の vtable。
///
/// コールバックは全て安全 Rust の `fn` ポインタ。`data`（vtable を識別する不透明
/// u64）と `ptr`（ホスト側オブジェクトの不透明アドレス）の参照解除・生ポインタ
/// 生成などの `unsafe` は FFI 層（`rust/akl-ffi`）が担い、本クレート
/// （`#![forbid(unsafe_code)]`）には unsafe を持ち込まない。C の `AklHandleVTab` 相当。
#[derive(Clone, Copy, Debug)]
pub struct HandleVTab {
    /// `[object TAG]` 用のタグ名（診断・文字列化）。
    pub tag: &'static str,
    /// プロパティ取得。`None` = 未知プロパティ（undefined / メソッド扱い）。
    pub get: fn(&mut Runtime, data: u64, ptr: u64, name: &str) -> Option<AklVal>,
    /// プロパティ設定。`false` = 拒否（TypeError）。
    pub set: fn(&mut Runtime, data: u64, ptr: u64, name: &str, v: AklVal) -> bool,
    /// メソッド呼び出し。`None` = 未定義メソッド（TypeError: not a function）。
    pub call: fn(&mut Runtime, data: u64, ptr: u64, name: &str, args: &[AklVal]) -> Option<AklVal>,
}

impl PartialEq for HandleVTab {
    /// 関数ポインタの等値比較（`fn_addr_eq`。コード生成ユニット間でアドレスが
    /// 異なり得るため derive では誤判定し得る）。
    fn eq(&self, other: &Self) -> bool {
        self.tag == other.tag
            && std::ptr::fn_addr_eq(self.get, other.get)
            && std::ptr::fn_addr_eq(self.set, other.set)
            && std::ptr::fn_addr_eq(self.call, other.call)
    }
}

/// 呼び出しフレーム。locals を明示的に保持する（C の `base` オフセット方式より安全）。
#[derive(Clone, Debug)]
struct Frame {
    /// 関数表 index。
    func: u32,
    /// 呼び出し元の再開位置（Call 命令の次）。
    ret_pc: usize,
    /// ローカル変数（パラメータ含む）。
    locals: Vec<AklVal>,
    /// このフレームの `this`。
    this: AklVal,
    /// クロージャ捕捉環境（無ければ None）。
    env: Option<ObjId>,
    /// 現在の try ブロックの catch 位置（無ければ None）。
    catch_pc: Option<usize>,
    /// new 呼び出しか（true なら戻り値が非オブジェクトの場合 this を返す）。
    is_new: bool,
}

/// ランタイム（C の `AklRT` 相当）。ヒープ・文字列インターン・関数表・グローバルを束ねる。
#[derive(Debug, Default)]
pub struct Runtime {
    /// ヒープ（文字列・配列・オブジェクト・関数・環境の単一 id 空間）。
    pub heap: ObjTable,
    /// 文字列インターン（内容 → ヒープ文字列 ObjId）。
    pub interner: Interner,
    /// 関数表（C の `rt->funcs`。ヒープの `Obj::Func` はこの index を参照）。
    pub funcs: Vec<FuncObj>,
    /// グローバル変数（name = intern 済み文字列 ObjId, value）。
    pub globals: Vec<(ObjId, AklVal)>,
    /// 最後の式文の値（C の `rt->last_val`。`Halt` 時に返す）。
    pub last_val: AklVal,
    /// ネイティブ関数表（C の native fn ポインタ + udata と同型。`Obj::Native(i)` が参照）。
    pub native_fns: Vec<NativeFn>,
    /// ホスト（FFI 層）提供のネイティブ関数表（`Obj::ForeignNative` が参照）。
    pub foreign_fns: Vec<ForeignNativeFn>,
    /// ホスト側コンテキストへの不透明ポインタ（アドレスを u64 で保持。FFI 層が
    /// `AklRT` ラッパーのアドレスを設定し、コールバック内で復元して使う）。
    pub host_ctx: u64,
    /// `console.log` の出力先バッファ（テストで検証可能に。None なら無視）。
    pub console_out: Vec<String>,
    /// 直近のエラー文言（C の `rt->err` 相当。`akl_error` 用）。
    pub err: String,
    /// 1 回の `run` で許す命令数（C の `insn_budget_def` 相当。既定 10M。
    /// 無限ループ等を打ち切る。FFI の `akl_set_insn_budget` が書き換える）。
    pub insn_budget: u64,
    /// 文字列メソッド表（name → native）。`PLoad` が文字列リテラルで解決する。
    /// C の `str_meth_vals` 相当。
    pub str_methods: Vec<(ObjId, AklVal)>,
    /// 配列メソッド表（name → native）。`PLoad` が配列で解決する。C の `arr_meth_vals` 相当。
    pub arr_methods: Vec<(ObjId, AklVal)>,
    /// Map メソッド表（name → native）。
    pub map_methods: Vec<(ObjId, AklVal)>,
    /// Set メソッド表（name → native）。
    pub set_methods: Vec<(ObjId, AklVal)>,
    /// Date メソッド表（name → native）。`PLoad` が `Obj::Date` で解決する。
    pub date_methods: Vec<(ObjId, AklVal)>,
    /// 正規表現メソッド表（name → native）。`PLoad` が `Obj::RegExp` で解決する。
    pub regex_methods: Vec<(ObjId, AklVal)>,
    /// ジェネレータメソッド表（name → native）。`PLoad` が `Obj::Gen` で解決する。
    pub gen_methods: Vec<(ObjId, AklVal)>,
    /// Promise メソッド表（name → native）。`PLoad` が `Obj::Promise` で解決する。
    pub promise_methods: Vec<(ObjId, AklVal)>,
    /// 関数メソッド表（name → native）。`PLoad` が `Obj::Func`/`Native` で解決する
    /// （`Function.prototype.call` / `apply`）。
    pub func_methods: Vec<(ObjId, AklVal)>,
    /// `length` プロパティ名の ObjId（install_builtins で設定。文字列/配列の length 用）。
    pub length_id: ObjId,
    /// `size` プロパティ名の ObjId（Map/Set の size 用）。
    pub size_id: ObjId,
    /// `constructor` プロパティ名の ObjId（コンストラクタ解決フォールバック用）。
    pub ctor_name: ObjId,
    /// `\x00proto` プロパティ名の ObjId（プロトタイプチェーン用。C の `proto_name` 相当）。
    pub proto_name: ObjId,
    /// 関数 → prototype オブジェクト（C の `fn_protos` 相当。new/instanceof 用）。
    pub fn_protos: Vec<(ObjId, ObjId)>,
}

impl Runtime {
    /// 空のランタイムを作る。
    pub fn new() -> Self {
        let mut rt = Self::default();
        // `length` プロパティ名を設定（文字列/配列の length 解決用。install_builtins 前でも有効）。
        rt.length_id = rt.intern("length").unwrap_or(0);
        // `size` プロパティ名（Map/Set の size 解決用）。
        rt.size_id = rt.intern("size").unwrap_or(0);
        // `\x00proto` プロパティ名（プロトタイプチェーン）。
        rt.proto_name = rt.intern("\x00proto").unwrap_or(0);
        // `constructor` プロパティ名（コンストラクタ解決フォールバック用）。
        rt.ctor_name = rt.intern("constructor").unwrap_or(0);
        // 命令バジェット既定（C の insn_budget_def = 10M と同値）。
        rt.insn_budget = 10_000_000;
        // JS の非予約グローバル（`undefined` は識別子として参照可能 = shadowable）。
        // これにより `function (undefined) {}` 等の慣用句がパース可能になる。
        let undef_id = rt.intern("undefined").unwrap_or(0);
        rt.global_set(undef_id, AklVal::UNDEF);
        let nan_id = rt.intern("NaN").unwrap_or(0);
        rt.global_set(nan_id, AklVal::from_f64(f64::NAN));
        let inf_id = rt.intern("Infinity").unwrap_or(0);
        rt.global_set(inf_id, AklVal::from_f64(f64::INFINITY));
        rt
    }

    /// 文字列を intern してヒープ上の文字列 ObjId を返す（失敗時 None）。
    pub fn intern(&mut self, s: &str) -> Option<ObjId> {
        self.interner.intern(&mut self.heap, s)
    }

    /// エラー文言を設定（C の `akl_errf` 相当）。
    pub fn set_err(&mut self, msg: impl Into<String>) {
        self.err = msg.into();
    }

    /// グローバル変数を name で読む（未宣言は None）。
    pub fn global_get(&self, name: ObjId) -> Option<AklVal> {
        self.globals.iter().find(|(n, _)| *n == name).map(|(_, v)| *v)
    }

    /// グローバル変数を name で設定（新規は追加、既存は上書き）。
    pub fn global_set(&mut self, name: ObjId, value: AklVal) {
        if let Some(slot) = self.globals.iter_mut().find(|(n, _)| *n == name) {
            slot.1 = value;
        } else {
            self.globals.push((name, value));
        }
    }

    /// ネイティブ関数を登録して `Obj::Native` の AklVal を返す。
    pub fn register_native(&mut self, f: NativeFn) -> Result<AklVal, VmError> {
        let idx = self.native_fns.len() as u32;
        self.native_fns.push(f);
        let id = self.heap.alloc(Obj::Native(idx)).map_err(|_| VmError::Oom)?;
        Ok(AklVal::mk_obj(id))
    }

    /// ネイティブ関数をグローバルに登録する（C の `akl_native_register` 相当）。
    pub fn register_global_native(&mut self, name: &str, f: NativeFn) -> Result<(), VmError> {
        let v = self.register_native(f)?;
        let name_id = self.intern(name).ok_or(VmError::Oom)?;
        self.global_set(name_id, v);
        Ok(())
    }

    /// ホスト（FFI 層）提供のネイティブ関数を登録して表 index を返す。
    /// `data` は `Obj::ForeignNative` の不透明フィールドとして保持される。
    pub fn register_foreign_native(&mut self, f: ForeignNativeFn) -> Result<u32, VmError> {
        let idx = self.foreign_fns.len() as u32;
        self.foreign_fns.push(f);
        Ok(idx)
    }

    /// プロパティをプロトタイプチェーン込みで解決（C の `obj_proto_find` 相当）。
    /// own → proto チェーンの順。深さ 64 で有界。
    pub fn prop_get_chain(&self, id: ObjId, name: ObjId) -> Option<AklVal> {
        let mut cur = id;
        for _ in 0..64 {
            let obj = self.heap.get(cur)?;
            match obj {
                Obj::Obj(props) => {
                    if let Some((_, v)) = props.iter().find(|(n, _)| *n == name) {
                        return Some(*v);
                    }
                    // proto チェーンを辿る
                    let proto = props
                        .iter()
                        .find(|(n, _)| *n == self.proto_name)
                        .and_then(|(_, v)| if v.is_obj() { Some(v.get_obj()) } else { None });
                    cur = proto?;
                }
                _ => return None,
            }
        }
        None
    }

    /// プロパティ解決（ハンドル以外。`PLoad` / `MCallName` の共通部）。
    /// 文字列・配列の `length` と各プロトタイプメソッド、Map/Set/Date メソッド、
    /// プレーンオブジェクトの proto チェーンをこの順で解決する。無ければ UNDEF。
    fn prop_load(&self, id: ObjId, name: ObjId) -> AklVal {
        if let Some(Obj::Str(s)) = self.heap.get(id) {
            if name == self.length_id {
                return AklVal::mk_int(s.chars().count() as i32);
            }
            return self
                .str_methods
                .iter()
                .find(|(n, _)| *n == name)
                .map(|(_, v)| *v)
                .unwrap_or(AklVal::UNDEF);
        }
        if let Some(Obj::Arr(items)) = self.heap.get(id) {
            if name == self.length_id {
                return AklVal::mk_int(items.len() as i32);
            }
            return self
                .arr_methods
                .iter()
                .find(|(n, _)| *n == name)
                .map(|(_, v)| *v)
                .unwrap_or(AklVal::UNDEF);
        }
        if let Some(Obj::Map(kv)) = self.heap.get(id) {
            if name == self.size_id {
                return AklVal::mk_int(kv.len() as i32);
            }
            return self
                .map_methods
                .iter()
                .find(|(n, _)| *n == name)
                .map(|(_, v)| *v)
                .unwrap_or(AklVal::UNDEF);
        }
        if let Some(Obj::Set(items)) = self.heap.get(id) {
            if name == self.size_id {
                return AklVal::mk_int(items.len() as i32);
            }
            return self
                .set_methods
                .iter()
                .find(|(n, _)| *n == name)
                .map(|(_, v)| *v)
                .unwrap_or(AklVal::UNDEF);
        }
        if let Some(Obj::Date { .. }) = self.heap.get(id) {
            return self
                .date_methods
                .iter()
                .find(|(n, _)| *n == name)
                .map(|(_, v)| *v)
                .unwrap_or(AklVal::UNDEF);
        }
        if let Some(Obj::RegExp { .. }) = self.heap.get(id) {
            return self
                .regex_methods
                .iter()
                .find(|(n, _)| *n == name)
                .map(|(_, v)| *v)
                .unwrap_or(AklVal::UNDEF);
        }
        if let Some(Obj::Gen { .. }) = self.heap.get(id) {
            return self
                .gen_methods
                .iter()
                .find(|(n, _)| *n == name)
                .map(|(_, v)| *v)
                .unwrap_or(AklVal::UNDEF);
        }
        if let Some(Obj::Promise { .. }) = self.heap.get(id) {
            return self
                .promise_methods
                .iter()
                .find(|(n, _)| *n == name)
                .map(|(_, v)| *v)
                .unwrap_or(AklVal::UNDEF);
        }
        if matches!(
            self.heap.get(id),
            Some(Obj::Func { .. })
                | Some(Obj::Native(_))
                | Some(Obj::ForeignNative { .. })
                | Some(Obj::BoundMethod { .. })
        ) {
            return self
                .func_methods
                .iter()
                .find(|(n, _)| *n == name)
                .map(|(_, v)| *v)
                .unwrap_or(AklVal::UNDEF);
        }
        self.prop_get_chain(id, name).unwrap_or(AklVal::UNDEF)
    }

    /// プレーンオブジェクトのプロパティ取得（getter アクセサ対応）。
    /// 通常プロパティ → getter（`get:\x01name`）の順。getter は this=obj で呼ぶ。
    fn obj_prop_load(&mut self, id: ObjId, name: ObjId) -> Result<AklVal, VmError> {
        if let Some(v) = self.prop_get_chain(id, name) {
            return Ok(v);
        }
        let name_str = match self.heap.get(name) {
            Some(Obj::Str(s)) => s.to_string(),
            _ => return Ok(AklVal::UNDEF),
        };
        let special = format!("get:\x01{name_str}");
        let sid = self.intern(&special).ok_or(VmError::Oom)?;
        if let Some(g) = self.prop_get_chain(id, sid) {
            if g.is_obj() {
                return self.call_value(g, AklVal::mk_obj(id), &[]);
            }
        }
        Ok(AklVal::UNDEF)
    }

    /// プレーンオブジェクトのプロパティ設定（setter アクセサ対応）。
    /// setter（`set:\x01name`）があれば this=obj で呼ぶ。なければ通常の prop_set。
    fn obj_prop_store(
        &mut self,
        id: ObjId,
        name: ObjId,
        val: AklVal,
    ) -> Result<AklVal, VmError> {
        let name_str = match self.heap.get(name) {
            Some(Obj::Str(s)) => s.to_string(),
            _ => String::new(),
        };
        if !name_str.is_empty() {
            let special = format!("set:\x01{name_str}");
            let sid = self.intern(&special).ok_or(VmError::Oom)?;
            if let Some(s) = self.prop_get_chain(id, sid) {
                if s.is_obj() {
                    self.call_value(s, AklVal::mk_obj(id), &[val])?;
                    return Ok(val);
                }
            }
        }
        self.heap
            .prop_set(id, name, val)
            .map_err(|_| VmError::NotObject)?;
        Ok(val)
    }

    /// オブジェクトに [[Prototype]] を設定（C の `akl_obj_set_proto` 相当）。
    pub fn obj_set_proto(&mut self, id: ObjId, proto: ObjId) -> Result<(), VmError> {
        self.heap
            .prop_set(id, self.proto_name, AklVal::mk_obj(proto))
            .map_err(|_| VmError::NotObject)
    }

    /// 関数の prototype オブジェクトを取得（無ければ生成。C の `akl_fn_proto_get` 相当）。
    pub fn func_proto(&mut self, f: AklVal) -> Option<ObjId> {
        if !f.is_obj() {
            return None;
        }
        let fid = f.get_obj();
        // 既存の "prototype" プロパティ（Obj の場合）
        let pname = self.intern("prototype")?;
        if let Some(Obj::Obj(props)) = self.heap.get(fid) {
            if let Some((_, v)) = props.iter().find(|(n, _)| *n == pname) {
                if v.is_obj() {
                    return Some(v.get_obj());
                }
            }
        }
        // fn_protos テーブルから探す
        if let Some((_, p)) = self.fn_protos.iter().find(|(f, _)| *f == fid) {
            return Some(*p);
        }
        // 生成
        let proto = self.heap.alloc(Obj::Obj(Vec::new())).ok()?;
        self.fn_protos.push((fid, proto));
        Some(proto)
    }

    /// コンストラクタ解決: callee が `Obj::Obj` で `constructor` プロパティ
    /// （callable）を持つ場合、その値を返す（C の CALL ハンドラの「OBJ の
    /// constructor を呼ぶ」フォールバック相当。Date 等の OBJ コンストラクタ用）。
    /// それ以外は callee をそのまま返す。
    fn ctor_of(&self, callee: AklVal) -> AklVal {
        if !callee.is_obj() {
            return callee;
        }
        if let Some(Obj::Obj(props)) = self.heap.get(callee.get_obj()) {
            if let Some((_, v)) = props.iter().find(|(n, _)| *n == self.ctor_name) {
                if v.is_obj()
                    && matches!(
                        self.heap.get(v.get_obj()),
                        Some(Obj::Func { .. }) | Some(Obj::Native(_))
                    )
                {
                    return *v;
                }
            }
        }
        callee
    }

    /// 任意の callable 値（Func / Native / ForeignNative / BoundMethod）を呼ぶ。
    /// C の `akl_call` / `akl_call_this` 相当（ホストからの再入呼び出し。
    /// `Runtime::run` は再入可能なので、バイトコード関数もここで完全に実行できる）。
    pub fn call_value(
        &mut self,
        f: AklVal,
        _this: AklVal,
        args: &[AklVal],
    ) -> Result<AklVal, VmError> {
        let callee = self.ctor_of(f);
        if !callee.is_obj() {
            return Err(VmError::NotCallable);
        }
        let id = callee.get_obj();
        if let Some(Obj::BoundMethod { handle, name }) = self.heap.get(id) {
            let (handle, name) = (*handle, *name);
            let (vtab, data, ptr) = match self.heap.get(handle) {
                Some(Obj::Handle { vtab, data, ptr }) => (*vtab, *data, *ptr),
                _ => return Err(VmError::NotCallable),
            };
            let name_str = match self.heap.get(name) {
                Some(Obj::Str(s)) => s.to_string(),
                _ => String::new(),
            };
            return match (vtab.call)(self, data, ptr, &name_str, args) {
                Some(v) => Ok(v),
                None => {
                    let msg = self.intern("TypeError: not a function").unwrap_or(0);
                    Err(VmError::Thrown(AklVal::mk_obj(msg)))
                }
            };
        }
        if let Some(Obj::ForeignNative { idx, data }) = self.heap.get(id) {
            let (idx, data) = (*idx, *data);
            let f = self.foreign_fns.get(idx as usize).ok_or(VmError::NotCallable)?;
            let f = *f;
            return f(self, _this, args, data);
        }
        if let Some(Obj::Native(nidx)) = self.heap.get(id) {
            let nidx = *nidx;
            let f = self.native_fns.get(nidx as usize).ok_or(VmError::NotCallable)?;
            let f = *f;
            return f(self, _this, args);
        }
        if let Some(Obj::Func { fidx, env }) = self.heap.get(id) {
            let (fidx, env) = (*fidx, *env);
            // ジェネレータ関数は実行せずに Obj::Gen を返す（`call_value` は FFI 再入。
            // 通常の JS 呼び出しは do_call が処理）。
            if self.funcs.get(fidx as usize).is_some_and(|f| f.is_gen) {
                let n = self.funcs[fidx as usize].n_locals.max(self.funcs[fidx as usize].n_params);
                let mut locals = vec![AklVal::UNDEF; n];
                for (i, a) in args.iter().enumerate().take(self.funcs[fidx as usize].n_params) {
                    locals[i] = *a;
                }
                let gen_id = self
                    .heap
                    .alloc(Obj::Gen { fidx, pc: 0, locals, env, this: _this, done: false })
                    .map_err(|_| VmError::Oom)?;
                return Ok(AklVal::mk_obj(gen_id));
            }
            return self.run_with_this(fidx, args, _this);
        }
        Err(VmError::NotCallable)
    }

    /// 関数表 index の関数を実行する（C の `vm_exec` 相当）。
    /// 停止時（`Halt`）のスタック先頭。エントリの `this` は undefined。
    pub fn run(&mut self, func_idx: u32, args: &[AklVal]) -> Result<AklVal, VmError> {
        self.run_with_this(func_idx, args, AklVal::UNDEF)
    }

    /// エントリの `this` を指定して実行する（getter/setter や `call_value` の再入用）。
    pub fn run_with_this(
        &mut self,
        func_idx: u32,
        args: &[AklVal],
        this: AklVal,
    ) -> Result<AklVal, VmError> {
        let stack: Vec<AklVal> = Vec::new();
        let mut frames: Vec<Frame> = Vec::new();
        let pc = 0usize;
        // エントリフレーム
        {
            let f = self.funcs.get(func_idx as usize).ok_or(VmError::NotCallable)?;
            let n = f.n_locals.max(f.n_params);
            let mut locals = vec![AklVal::UNDEF; n];
            for (i, a) in args.iter().enumerate().take(f.n_params) {
                locals[i] = *a;
            }
            frames.push(Frame { func: func_idx, ret_pc: 0, locals, this, env: None, catch_pc: None, is_new: false });
        }
        match self.run_loop(stack, frames, pc, None)? {
            RunEnd::Value(v) => Ok(v),
            // 非ジェネレータコンテキストの Yield は起きない（Yield 命令は
            // ジェネレータ関数のコードにのみ現れる）。
            RunEnd::Yield(_) => Err(VmError::StackUnderflow),
        }
    }

    /// VM 実行ループ本体（C の `vm_exec` 相当）。`stack` / `frames` / `pc` を初期
    /// 状態として実行し、[`RunEnd`] を返す。`gen` が `Some(id)` なら `yield` 命令で
    /// そのジェネレータの再開位置を保存して [`RunEnd::Yield`] を返す。
    fn run_loop(
        &mut self,
        mut stack: Vec<AklVal>,
        mut frames: Vec<Frame>,
        mut pc: usize,
        gen: Option<ObjId>,
    ) -> Result<RunEnd, VmError> {
        // 命令バジェット（無限ループ等の打ち切り。C の vm_exec の budget 相当）。
        // 再入（call_value → run）はそれぞれ新規に初期化される（C の akl_call と同じ）。
        let mut remaining = self.insn_budget;

        loop {
            if remaining == 0 {
                return Err(VmError::BudgetExhausted);
            }
            remaining -= 1;
            // 命令フェッチ（Op は Copy。self.funcs の借用はこのブロックで終了）
            let op = {
                let frame = frames.last().ok_or(VmError::StackUnderflow)?;
                let func = self
                    .funcs
                    .get(frame.func as usize)
                    .ok_or(VmError::NotCallable)?;
                if pc >= func.code.len() {
                    // 暗黙の終端（Halt 無し）。スタック先頭を返す。
                    let v = stack.last().copied().unwrap_or(AklVal::UNDEF);
                    return Ok(RunEnd::Value(v));
                }
                func.code[pc]
            };

            match op {
                Op::ConstI(v) => stack.push(AklVal::mk_int(v)),
                Op::ConstD(d) => stack.push(AklVal::from_f64(d)),
                Op::ConstStr(id) => stack.push(AklVal::mk_obj(id)),
                Op::True => stack.push(AklVal::TRUE),
                Op::False => stack.push(AklVal::FALSE),
                Op::Null => stack.push(AklVal::NULL),
                Op::Undef => stack.push(AklVal::UNDEF),
                Op::Add => {
                    let b = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let a = stack.pop().ok_or(VmError::StackUnderflow)?;
                    stack.push(self.add(a, b)?);
                }
                Op::Sub => {
                    let b = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let a = stack.pop().ok_or(VmError::StackUnderflow)?;
                    stack.push(self.arith_bin(a, b, Arith::Sub)?);
                }
                Op::Mul => {
                    let b = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let a = stack.pop().ok_or(VmError::StackUnderflow)?;
                    stack.push(self.arith_bin(a, b, Arith::Mul)?);
                }
                Op::Div => {
                    let b = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let a = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let (da, db) = (self.to_number(a), self.to_number(b));
                    stack.push(AklVal::from_f64(da / db));
                }
                Op::Mod => {
                    let b = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let a = stack.pop().ok_or(VmError::StackUnderflow)?;
                    stack.push(self.mod_(a, b));
                }
                Op::Lt | Op::Le | Op::Gt | Op::Ge => {
                    let b = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let a = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let cmp = match op {
                        Op::Lt => Cmp::Lt,
                        Op::Le => Cmp::Le,
                        Op::Gt => Cmp::Gt,
                        _ => Cmp::Ge,
                    };
                    stack.push(self.compare(a, b, cmp));
                }
                Op::Eq | Op::Ne | Op::Seq | Op::Sne => {
                    let b = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let a = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let r = match op {
                        Op::Eq => self.eq(a, b),
                        Op::Ne => !self.eq(a, b),
                        Op::Seq => self.strict_eq(a, b),
                        _ => !self.strict_eq(a, b),
                    };
                    stack.push(AklVal::from_bool(r));
                }
                Op::Not => {
                    let v = stack.pop().ok_or(VmError::StackUnderflow)?;
                    stack.push(AklVal::from_bool(!self.truthy(v)));
                }
                Op::BNot => {
                    let v = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let i = self.to_number(v) as i32;
                    stack.push(AklVal::mk_int(!i));
                }
                Op::BAnd | Op::BOr | Op::BXor | Op::BShl | Op::BShr | Op::BUShr => {
                    let b = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let a = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let (ia, ib) = (self.to_number(a) as i32, self.to_number(b) as i32);
                    let r = match op {
                        Op::BAnd => ia & ib,
                        Op::BOr => ia | ib,
                        Op::BXor => ia ^ ib,
                        Op::BShl => ia << (ib & 31),
                        Op::BShr => ia >> (ib & 31),
                        _ => ((ia as u32) >> (ib & 31)) as i32,
                    };
                    stack.push(AklVal::mk_int(r));
                }
                Op::And => {
                    let b = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let a = stack.pop().ok_or(VmError::StackUnderflow)?;
                    stack.push(AklVal::from_bool(self.truthy(a) && self.truthy(b)));
                }
                Op::Or => {
                    let b = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let a = stack.pop().ok_or(VmError::StackUnderflow)?;
                    stack.push(AklVal::from_bool(self.truthy(a) || self.truthy(b)));
                }
                Op::Neg => {
                    let v = stack.pop().ok_or(VmError::StackUnderflow)?;
                    stack.push(AklVal::from_f64(-self.to_number(v)));
                }
                Op::Pos => {
                    let v = stack.pop().ok_or(VmError::StackUnderflow)?;
                    stack.push(AklVal::from_f64(self.to_number(v)));
                }
                Op::Typeof => {
                    let v = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let name = self.type_name(v);
                    let id = self.intern(name).ok_or(VmError::Oom)?;
                    stack.push(AklVal::mk_obj(id));
                }
                Op::Pop => {
                    stack.pop().ok_or(VmError::StackUnderflow)?;
                }
                Op::PopV => {
                    let v = stack.pop().ok_or(VmError::StackUnderflow)?;
                    self.last_val = v;
                }
                Op::Dup => {
                    let top = *stack.last().ok_or(VmError::StackUnderflow)?;
                    stack.push(top);
                }
                Op::LLoad(slot) => {
                    let frame = frames.last().ok_or(VmError::StackUnderflow)?;
                    let v = *frame
                        .locals
                        .get(slot as usize)
                        .ok_or(VmError::LocalOob)?;
                    stack.push(v);
                }
                Op::LStore(slot) => {
                    let v = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let frame = frames.last_mut().ok_or(VmError::StackUnderflow)?;
                    let dst = frame.locals.get_mut(slot as usize).ok_or(VmError::LocalOob)?;
                    *dst = v;
                }
                Op::GLoad(name) => {
                    let v = self.global_get(name).ok_or(VmError::GlobalNotFound)?;
                    stack.push(v);
                }
                Op::GLoadSafe(name) => {
                    let v = self.global_get(name).unwrap_or(AklVal::UNDEF);
                    stack.push(v);
                }
                Op::GStore(name) => {
                    let v = stack.pop().ok_or(VmError::StackUnderflow)?;
                    self.global_set(name, v);
                }
                Op::Jmp(t) => {
                    let t = t as usize;
                    if t > self.funcs[frames.last().unwrap().func as usize].code.len() {
                        return Err(VmError::JumpOob);
                    }
                    pc = t;
                    continue;
                }
                Op::JmpF(t) => {
                    let v = stack.pop().ok_or(VmError::StackUnderflow)?;
                    if !self.truthy(v) {
                        pc = t as usize;
                        continue;
                    }
                }
                Op::JmpT(t) => {
                    let v = stack.pop().ok_or(VmError::StackUnderflow)?;
                    if self.truthy(v) {
                        pc = t as usize;
                        continue;
                    }
                }
                Op::JmpNotNullish(t) => {
                    let v = stack.pop().ok_or(VmError::StackUnderflow)?;
                    if !v.is_null() && !v.is_undef() {
                        pc = t as usize;
                        continue;
                    }
                }
                Op::Call(argc) => {
                    let argc = argc as usize;
                    if stack.len() < argc + 1 {
                        return Err(VmError::StackUnderflow);
                    }
                    let callee = stack[stack.len() - argc - 1];
                    let args: Vec<AklVal> = stack[stack.len() - argc..].to_vec();
                    stack.truncate(stack.len() - argc - 1);
                    match self.do_call(&mut frames, &mut pc, callee, AklVal::UNDEF, &args) {
                        Ok(Some(r)) => {
                            stack.push(r);
                            pc += 1;
                            continue;
                        }
                        Ok(None) => continue,
                        Err(VmError::Thrown(v)) => {
                            self.unwind(&mut frames, &mut stack, &mut pc, v)?;
                            continue;
                        }
                        Err(e) => return Err(e),
                    }
                }
                Op::MCall(argc) => {
                    let argc = argc as usize;
                    if stack.len() < argc + 2 {
                        return Err(VmError::StackUnderflow);
                    }
                    let callee = stack[stack.len() - argc - 1];
                    let receiver = stack[stack.len() - argc - 2];
                    let args: Vec<AklVal> = stack[stack.len() - argc..].to_vec();
                    stack.truncate(stack.len() - argc - 2);
                    match self.do_call(&mut frames, &mut pc, callee, receiver, &args) {
                        Ok(Some(r)) => {
                            stack.push(r);
                            pc += 1;
                            continue;
                        }
                        Ok(None) => continue,
                        Err(VmError::Thrown(v)) => {
                            self.unwind(&mut frames, &mut stack, &mut pc, v)?;
                            continue;
                        }
                        Err(e) => return Err(e),
                    }
                }
                Op::MCallName { name, argc } => {
                    let argc = argc as usize;
                    if stack.len() < argc + 1 {
                        return Err(VmError::StackUnderflow);
                    }
                    let receiver = stack[stack.len() - argc - 1];
                    let args: Vec<AklVal> = stack[stack.len() - argc..].to_vec();
                    stack.truncate(stack.len() - argc - 1);
                    // ホストハンドルのメソッド: vtable の call へ直接ディスパッチ
                    let handle = if receiver.is_obj() {
                        match self.heap.get(receiver.get_obj()) {
                            Some(Obj::Handle { vtab, data, ptr }) => Some((*vtab, *data, *ptr)),
                            _ => None,
                        }
                    } else {
                        None
                    };
                    if let Some((vtab, data, ptr)) = handle {
                        let name_str = match self.heap.get(name) {
                            Some(Obj::Str(s)) => s.to_string(),
                            _ => String::new(),
                        };
                        match (vtab.call)(self, data, ptr, &name_str, &args) {
                            Some(v) => {
                                stack.push(v);
                                pc += 1;
                                continue;
                            }
                            None => {
                                let msg = self.intern("TypeError: not a function").unwrap_or(0);
                                self.unwind(&mut frames, &mut stack, &mut pc, AklVal::mk_obj(msg))?;
                                continue;
                            }
                        }
                    }
                    // 通常オブジェクト: プロパティを解決して this=receiver で呼ぶ
                    let method = if receiver.is_obj() {
                        self.prop_load(receiver.get_obj(), name)
                    } else {
                        AklVal::UNDEF
                    };
                    match self.do_call(&mut frames, &mut pc, method, receiver, &args) {
                        Ok(Some(r)) => {
                            stack.push(r);
                            pc += 1;
                            continue;
                        }
                        Ok(None) => continue,
                        Err(VmError::Thrown(v)) => {
                            self.unwind(&mut frames, &mut stack, &mut pc, v)?;
                            continue;
                        }
                        Err(e) => return Err(e),
                    }
                }
                Op::MCallDyn { name, argc_name } => {
                    let argc_v = self.global_get(argc_name).unwrap_or(AklVal::mk_int(0));
                    let argc = if argc_v.is_int() && argc_v.get_int() >= 0 {
                        argc_v.get_int() as usize
                    } else {
                        0
                    };
                    if argc > 4096 || stack.len() < argc + 1 {
                        return Err(VmError::StackUnderflow);
                    }
                    let receiver = stack[stack.len() - argc - 1];
                    let args: Vec<AklVal> = stack[stack.len() - argc..].to_vec();
                    stack.truncate(stack.len() - argc - 1);
                    // ハンドルのメソッドは vtable call へ
                    let handle = if receiver.is_obj() {
                        match self.heap.get(receiver.get_obj()) {
                            Some(Obj::Handle { vtab, data, ptr }) => Some((*vtab, *data, *ptr)),
                            _ => None,
                        }
                    } else {
                        None
                    };
                    if let Some((vtab, data, ptr)) = handle {
                        let name_str = match self.heap.get(name) {
                            Some(Obj::Str(s)) => s.to_string(),
                            _ => String::new(),
                        };
                        match (vtab.call)(self, data, ptr, &name_str, &args) {
                            Some(v) => {
                                stack.push(v);
                                pc += 1;
                                continue;
                            }
                            None => {
                                let msg = self.intern("TypeError: not a function").unwrap_or(0);
                                self.unwind(&mut frames, &mut stack, &mut pc, AklVal::mk_obj(msg))?;
                                continue;
                            }
                        }
                    }
                    let method = if receiver.is_obj() {
                        self.prop_load(receiver.get_obj(), name)
                    } else {
                        AklVal::UNDEF
                    };
                    match self.do_call(&mut frames, &mut pc, method, receiver, &args) {
                        Ok(Some(r)) => {
                            stack.push(r);
                            pc += 1;
                            continue;
                        }
                        Ok(None) => continue,
                        Err(VmError::Thrown(v)) => {
                            self.unwind(&mut frames, &mut stack, &mut pc, v)?;
                            continue;
                        }
                        Err(e) => return Err(e),
                    }
                }
                Op::New(argc) => {
                    let argc = argc as usize;
                    if stack.len() < argc + 1 {
                        return Err(VmError::StackUnderflow);
                    }
                    let f = stack[stack.len() - argc - 1];
                    let args: Vec<AklVal> = stack[stack.len() - argc..].to_vec();
                    stack.truncate(stack.len() - argc - 1);
                    // OBJ コンストラクタのフォールバック（Date 等）
                    let f = self.ctor_of(f);
                    // 新オブジェクト
                    let obj_id = self
                        .heap
                        .alloc(Obj::Obj(Vec::new()))
                        .map_err(|_| VmError::Oom)?;
                    // [[Prototype]] = f.prototype
                    if let Some(fp) = self.func_proto(f) {
                        self.obj_set_proto(obj_id, fp)?;
                    }
                    let this = AklVal::mk_obj(obj_id);
                    match self.do_call(&mut frames, &mut pc, f, this, &args) {
                        Ok(Some(r)) => {
                            // ネイティブ関数の new: 戻り値がオブジェクトならそれ、非オブジェクトなら this
                            let result = if r.is_obj() { r } else { this };
                            stack.push(result);
                            pc += 1;
                            continue;
                        }
                        Ok(None) => {
                            // バイトコード関数の new: is_new を立てる（Ret で this を返す）
                            if let Some(frame) = frames.last_mut() {
                                frame.is_new = true;
                            }
                            continue;
                        }
                        Err(VmError::Thrown(v)) => {
                            self.unwind(&mut frames, &mut stack, &mut pc, v)?;
                            continue;
                        }
                        Err(e) => return Err(e),
                    }
                }
                Op::SetFnProto => {
                    let proto = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let f = stack.pop().ok_or(VmError::StackUnderflow)?;
                    if !proto.is_obj() || !f.is_obj() {
                        return Err(VmError::NotObject);
                    }
                    let fid = f.get_obj();
                    // 既存の登録を置き換え
                    if let Some(slot) = self.fn_protos.iter_mut().find(|(x, _)| *x == fid) {
                        slot.1 = proto.get_obj();
                    } else {
                        self.fn_protos.push((fid, proto.get_obj()));
                    }
                    stack.push(f);
                }
                Op::LinkSuper => {
                    // pop parent_ctor → TOS の proto に [[Prototype]] = parent.prototype を設定
                    // （proto は pop せず SetFnProto 用に残す）
                    let parent = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let proto = *stack.last().ok_or(VmError::StackUnderflow)?;
                    if proto.is_obj() && parent.is_obj() {
                        if let Some(pp) = self.func_proto(parent) {
                            self.obj_set_proto(proto.get_obj(), pp)?;
                        }
                    }
                }
                Op::NewRegex(flags_id) => {
                    let pat_id = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let pattern = match self.heap.get(pat_id.get_obj()) {
                        Some(Obj::Str(s)) => s.clone(),
                        _ => Box::from(""),
                    };
                    let flags = match self.heap.get(flags_id) {
                        Some(Obj::Str(s)) => s.clone(),
                        _ => Box::from(""),
                    };
                    let id = self
                        .heap
                        .alloc(Obj::RegExp { pattern, flags })
                        .map_err(|_| VmError::Oom)?;
                    stack.push(AklVal::mk_obj(id));
                }
                Op::Ret => {
                    let v = stack.pop().ok_or(VmError::StackUnderflow)?;
                    // 戻るフレーム自身の ret_pc（呼び出し元の再開位置）を使う。
                    // 呼び出し元フレームの ret_pc（= その呼び出し元への再開位置）では
                    // 誤って直上の呼び出し元へ戻ってしまう（実測で特定）。
                    let frame = frames.pop().ok_or(VmError::StackUnderflow)?;
                    // new 呼び出し: 戻り値が非オブジェクトなら this（新オブジェクト）を返す
                    let v = if frame.is_new && !v.is_obj() { frame.this } else { v };
                    if frames.is_empty() {
                        return Ok(RunEnd::Value(v));
                    }
                    stack.push(v);
                    pc = frame.ret_pc;
                    continue;
                }
                Op::MakeF(fidx) => {
                    let id = self
                        .heap
                        .alloc(Obj::Func { fidx, env: None })
                        .map_err(|_| VmError::Oom)?;
                    stack.push(AklVal::mk_obj(id));
                }
                Op::Throw => {
                    let v = stack.pop().ok_or(VmError::StackUnderflow)?;
                    self.unwind(&mut frames, &mut stack, &mut pc, v)?;
                    continue;
                }
                Op::TryPush(catch_pc) => {
                    let frame = frames.last_mut().ok_or(VmError::StackUnderflow)?;
                    frame.catch_pc = Some(catch_pc as usize);
                }
                Op::TryPop => {
                    let frame = frames.last_mut().ok_or(VmError::StackUnderflow)?;
                    frame.catch_pc = None;
                }
                Op::MakeEnv(n) => {
                    // 現在フレームの自前 env（box 化されたローカル n 個を undefined で生成）。
                    // parent は「この関数を呼んだ時点の frame.env」（＝外側関数の env）で、
                    // 多段クロージャの env チェーンを構成する。
                    let parent = frames.last().and_then(|f| f.env);
                    let id = self
                        .heap
                        .alloc(Obj::Env { vals: vec![AklVal::UNDEF; n as usize], parent })
                        .map_err(|_| VmError::Oom)?;
                    let frame = frames.last_mut().ok_or(VmError::StackUnderflow)?;
                    frame.env = Some(id);
                }
                Op::MakeClosure(fidx) => {
                    // 現在フレームの env を共有するクロージャを生成（C の MAKEF env 版）。
                    // 捕捉変数は共有セルなので、値コピーではなく env 参照を共有する。
                    let env = frames.last().and_then(|f| f.env);
                    let id = self
                        .heap
                        .alloc(Obj::Func { fidx, env })
                        .map_err(|_| VmError::Oom)?;
                    stack.push(AklVal::mk_obj(id));
                }
                Op::CeLoad(depth, idx) => {
                    let frame = frames.last().ok_or(VmError::StackUnderflow)?;
                    let mut env_id = frame.env.ok_or(VmError::LocalOob)?;
                    // env チェーンを depth 段辿る
                    for _ in 0..depth {
                        env_id = match self.heap.get(env_id) {
                            Some(Obj::Env { parent: Some(p), .. }) => *p,
                            _ => return Err(VmError::LocalOob),
                        };
                    }
                    let v = match self.heap.get(env_id) {
                        Some(Obj::Env { vals, .. }) => {
                            *vals.get(idx as usize).ok_or(VmError::LocalOob)?
                        }
                        _ => return Err(VmError::LocalOob),
                    };
                    stack.push(v);
                }
                Op::CeStore(depth, idx) => {
                    let v = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let frame = frames.last().ok_or(VmError::StackUnderflow)?;
                    let mut env_id = frame.env.ok_or(VmError::LocalOob)?;
                    for _ in 0..depth {
                        env_id = match self.heap.get(env_id) {
                            Some(Obj::Env { parent: Some(p), .. }) => *p,
                            _ => return Err(VmError::LocalOob),
                        };
                    }
                    match self.heap.get_mut(env_id) {
                        Some(Obj::Env { vals, .. }) => {
                            let dst = vals.get_mut(idx as usize).ok_or(VmError::LocalOob)?;
                            *dst = v;
                        }
                        _ => return Err(VmError::LocalOob),
                    }
                }
                Op::This => {
                    let this = frames.last().ok_or(VmError::StackUnderflow)?.this;
                    stack.push(this);
                }
                Op::ObjNew => {
                    let id = self
                        .heap
                        .alloc(Obj::Obj(Vec::new()))
                        .map_err(|_| VmError::Oom)?;
                    stack.push(AklVal::mk_obj(id));
                }
                Op::PLoad(name) => {
                    let obj = stack.pop().ok_or(VmError::StackUnderflow)?;
                    if !obj.is_obj() {
                        return Err(VmError::NotObject);
                    }
                    let id = obj.get_obj();
                    // ホストハンドル（DOM 要素等）のプロパティ解決。未知プロパティは
                    // undefined（メソッドは MCallName が vtable の call へ直接ディスパッチ）。
                    if let Some(Obj::Handle { vtab, data, ptr }) = self.heap.get(id) {
                        let (vtab, data, ptr) = (*vtab, *data, *ptr);
                        let name_str = match self.heap.get(name) {
                            Some(Obj::Str(s)) => s.to_string(),
                            _ => String::new(),
                        };
                        let v = (vtab.get)(self, data, ptr, &name_str).unwrap_or(AklVal::UNDEF);
                        stack.push(v);
                    } else if matches!(self.heap.get(id), Some(Obj::Obj(_))) {
                        // プレーンオブジェクトは getter アクセサ対応
                        let v = self.obj_prop_load(id, name)?;
                        stack.push(v);
                    } else {
                        stack.push(self.prop_load(id, name));
                    }
                }
                Op::PStore(name) => {
                    let val = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let obj = stack.pop().ok_or(VmError::StackUnderflow)?;
                    if !obj.is_obj() {
                        return Err(VmError::NotObject);
                    }
                    let id = obj.get_obj();
                    // 配列の length セッター（`arr.length = n` で伸縮）
                    if name == self.length_id {
                        let new_len = self.to_number(val) as i64;
                        if let Some(Obj::Arr(items)) = self.heap.get_mut(id) {
                            if new_len >= 0 {
                                let nl = new_len as usize;
                                if nl < items.len() {
                                    items.truncate(nl);
                                } else {
                                    items.resize(nl, AklVal::UNDEF);
                                }
                            }
                            stack.push(val);
                            pc += 1;
                            continue;
                        }
                    }
                    // ホストハンドルのプロパティ設定
                    if let Some(Obj::Handle { vtab, data, ptr }) = self.heap.get(id) {
                        let (vtab, data, ptr) = (*vtab, *data, *ptr);
                        let name_str = match self.heap.get(name) {
                            Some(Obj::Str(s)) => s.to_string(),
                            _ => String::new(),
                        };
                        if !(vtab.set)(self, data, ptr, &name_str, val) {
                            return Err(VmError::NotObject);
                        }
                        stack.push(val);
                        pc += 1;
                        continue;
                    }
                    // プレーンオブジェクトは setter アクセサ対応
                    if matches!(self.heap.get(id), Some(Obj::Obj(_))) {
                        self.obj_prop_store(id, name, val)?;
                    } else {
                        self.heap
                            .prop_set(id, name, val)
                            .map_err(|_| VmError::NotObject)?;
                    }
                    stack.push(val);
                }
                Op::ArrNew(count) => {
                    let count = count as usize;
                    if stack.len() < count {
                        return Err(VmError::StackUnderflow);
                    }
                    let items = stack[stack.len() - count..].to_vec();
                    stack.truncate(stack.len() - count);
                    let id = self.heap.alloc(Obj::Arr(items)).map_err(|_| VmError::Oom)?;
                    stack.push(AklVal::mk_obj(id));
                }
                Op::ArrPush => {
                    let val = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let arr = stack.pop().ok_or(VmError::StackUnderflow)?;
                    if !arr.is_obj() {
                        return Err(VmError::NotObject);
                    }
                    let id = arr.get_obj();
                    match self.heap.get_mut(id) {
                        Some(Obj::Arr(items)) => items.push(val),
                        _ => return Err(VmError::NotObject),
                    }
                    stack.push(arr);
                }
                Op::ArrPushAll => {
                    let src = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let arr = stack.pop().ok_or(VmError::StackUnderflow)?;
                    if !arr.is_obj() || !src.is_obj() {
                        return Err(VmError::NotObject);
                    }
                    let src_items = match self.heap.get(src.get_obj()) {
                        Some(Obj::Arr(items)) => items.clone(),
                        _ => return Err(VmError::NotObject),
                    };
                    let id = arr.get_obj();
                    match self.heap.get_mut(id) {
                        Some(Obj::Arr(items)) => items.extend_from_slice(&src_items),
                        _ => return Err(VmError::NotObject),
                    }
                    stack.push(arr);
                }
                Op::ArrSpreadC(name) => {
                    // pop 配列 → 要素を順に push + グローバルカウンタ name へ要素数を加算
                    let arr = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let items = if arr.is_obj() {
                        match self.heap.get(arr.get_obj()) {
                            Some(Obj::Arr(v)) => v.clone(),
                            _ => Vec::new(),
                        }
                    } else {
                        Vec::new()
                    };
                    let n = items.len();
                    let cur = self.global_get(name).unwrap_or(AklVal::mk_int(0));
                    let newc = if cur.is_int() { cur.get_int() + n as i32 } else { n as i32 };
                    self.global_set(name, AklVal::mk_int(newc));
                    stack.extend(items);
                }
                Op::CallDyn(name) => {
                    let argc_v = self
                        .global_get(name)
                        .unwrap_or(AklVal::mk_int(0));
                    let argc = if argc_v.is_int() && argc_v.get_int() >= 0 {
                        argc_v.get_int() as usize
                    } else {
                        0
                    };
                    if argc > 4096 || stack.len() < argc + 1 {
                        return Err(VmError::StackUnderflow);
                    }
                    let callee = stack[stack.len() - argc - 1];
                    let args: Vec<AklVal> = stack[stack.len() - argc..].to_vec();
                    stack.truncate(stack.len() - argc - 1);
                    match self.do_call(&mut frames, &mut pc, callee, AklVal::UNDEF, &args) {
                        Ok(Some(r)) => {
                            stack.push(r);
                            pc += 1;
                            continue;
                        }
                        Ok(None) => continue,
                        Err(VmError::Thrown(v)) => {
                            self.unwind(&mut frames, &mut stack, &mut pc, v)?;
                            continue;
                        }
                        Err(e) => return Err(e),
                    }
                }
                Op::ObjSpread => {
                    // pop src → TOS の OBJ に全 props をコピー（\x00proto は除く）
                    let src = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let dst = *stack.last().ok_or(VmError::StackUnderflow)?;
                    if src.is_obj() && dst.is_obj() {
                        let src_props = match self.heap.get(src.get_obj()) {
                            Some(Obj::Obj(props)) => props.clone(),
                            _ => Vec::new(),
                        };
                        if let Some(Obj::Obj(_)) = self.heap.get(dst.get_obj()) {
                            for (n, v) in src_props {
                                if n != self.proto_name {
                                    let _ = self.heap.prop_set(dst.get_obj(), n, v);
                                }
                            }
                        }
                    }
                }
                Op::ObjRest => {
                    let src = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let src_props = if src.is_obj() {
                        match self.heap.get(src.get_obj()) {
                            Some(Obj::Obj(props)) => props.clone(),
                            _ => Vec::new(),
                        }
                    } else {
                        Vec::new()
                    };
                    let id = self
                        .heap
                        .alloc(Obj::Obj(Vec::new()))
                        .map_err(|_| VmError::Oom)?;
                    for (n, v) in src_props {
                        if n != self.proto_name {
                            let _ = self.heap.prop_set(id, n, v);
                        }
                    }
                    stack.push(AklVal::mk_obj(id));
                }
                Op::ArrRest => {
                    let start = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let arr = stack.pop().ok_or(VmError::StackUnderflow)?;
                    if !arr.is_obj() {
                        return Err(VmError::NotObject);
                    }
                    let items = match self.heap.get(arr.get_obj()) {
                        Some(Obj::Arr(v)) => v.clone(),
                        _ => return Err(VmError::NotObject),
                    };
                    let s = self.to_number(start) as i64;
                    let s = if s < 0 { 0 } else { s as usize };
                    let rest: Vec<AklVal> = if s < items.len() {
                        items[s..].to_vec()
                    } else {
                        Vec::new()
                    };
                    let id = self.heap.alloc(Obj::Arr(rest)).map_err(|_| VmError::Oom)?;
                    stack.push(AklVal::mk_obj(id));
                }
                Op::AGet => {
                    let idx = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let obj = stack.pop().ok_or(VmError::StackUnderflow)?;
                    if !obj.is_obj() {
                        return Err(VmError::NotObject);
                    }
                    let id = obj.get_obj();
                    // ホストハンドルのブラケットアクセス（キー文字列で get）
                    if let Some(Obj::Handle { vtab, data, ptr }) = self.heap.get(id) {
                        let (vtab, data, ptr) = (*vtab, *data, *ptr);
                        let key_str = match self.stringify(idx) {
                            Ok(k) => match self.heap.get(k) {
                                Some(Obj::Str(s)) => s.to_string(),
                                _ => String::new(),
                            },
                            Err(_) => String::new(),
                        };
                        let v = (vtab.get)(self, data, ptr, &key_str).unwrap_or(AklVal::UNDEF);
                        stack.push(v);
                        pc += 1;
                        continue;
                    }
                    // プレーンオブジェクトの計算済みプロパティ取得 `obj[key]`
                    if let Some(Obj::Obj(_)) = self.heap.get(id) {
                        let key = self.stringify(idx).ok();
                        let v = key
                            .and_then(|k| self.prop_get_chain(id, k))
                            .unwrap_or(AklVal::UNDEF);
                        stack.push(v);
                        pc += 1;
                        continue;
                    }
                    let i = self.to_number(idx) as i64;
                    let v = if i >= 0 {
                        self.heap.arr_get(id, i as usize).unwrap_or(AklVal::UNDEF)
                    } else {
                        AklVal::UNDEF
                    };
                    stack.push(v);
                }
                Op::ASet => {
                    let val = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let idx = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let obj = stack.pop().ok_or(VmError::StackUnderflow)?;
                    if !obj.is_obj() {
                        return Err(VmError::NotObject);
                    }
                    let id = obj.get_obj();
                    if let Some(Obj::Arr(_)) = self.heap.get(id) {
                        let i = self.to_number(idx) as i64;
                        if i >= 0 {
                            self.arr_set(id, i as usize, val)?;
                        }
                    } else if let Some(Obj::Obj(_)) = self.heap.get(id) {
                        // 計算済みプロパティ設定 `obj[key] = v`
                        let key = self.stringify(idx)?;
                        self.heap
                            .prop_set(id, key, val)
                            .map_err(|_| VmError::NotObject)?;
                    }
                    stack.push(val);
                }
                Op::Instanceof => {
                    // obj instanceof f（プロトタイプチェーンを辿る）
                    let f = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let obj = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let mut r = false;
                    if f.is_obj() && obj.is_obj() {
                        // f の prototype を取得（f が Obj なら "prototype" プロパティ、
                        // f が Func/Native なら fn_protos テーブル）
                        let f_proto = self.func_proto(f);
                        if let Some(fp) = f_proto {
                            // obj の [[Prototype]] チェーンを辿って fp と比較
                            let mut cur = obj.get_obj();
                            for _ in 0..64 {
                                if cur == fp {
                                    r = true;
                                    break;
                                }
                                // cur の [[Prototype]] を取得
                                match self.heap.get(cur) {
                                    Some(Obj::Obj(props)) => {
                                        let parent = props
                                            .iter()
                                            .find(|(n, _)| *n == self.proto_name)
                                            .and_then(|(_, v)| if v.is_obj() { Some(v.get_obj()) } else { None });
                                        match parent {
                                            Some(p) => cur = p,
                                            None => break,
                                        }
                                    }
                                    _ => break,
                                }
                            }
                        }
                    }
                    stack.push(AklVal::from_bool(r));
                }
                Op::In => {
                    // key in obj
                    let obj = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let key = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let key_id = self.stringify(key).ok();
                    let r = match (obj.is_obj(), key_id) {
                        (true, Some(kid)) => match self.heap.get(obj.get_obj()) {
                            Some(Obj::Obj(props)) => props.iter().any(|(n, _)| *n == kid),
                            _ => false,
                        },
                        _ => false,
                    };
                    stack.push(AklVal::from_bool(r));
                }
                Op::Delete => {
                    // delete obj[key]
                    let idx = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let obj = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let mut deleted = false;
                    if obj.is_obj() {
                        let id = obj.get_obj();
                        if let Some(Obj::Arr(_)) = self.heap.get(id) {
                            let i = self.to_number(idx) as i64;
                            if i >= 0 {
                                deleted = self.arr_delete(id, i as usize);
                            }
                        } else if let Some(Obj::Obj(_)) = self.heap.get(id) {
                            let key_id = self.stringify(idx).ok();
                            if let Some(kid) = key_id {
                                deleted = self.prop_delete(id, kid);
                            }
                        }
                    }
                    stack.push(AklVal::from_bool(deleted));
                }
                Op::Halt => {
                    return Ok(RunEnd::Value(self.last_val));
                }
                Op::BigInt(v) => {
                    let id = self
                        .heap
                        .alloc(Obj::BigInt(v))
                        .map_err(|_| VmError::Oom)?;
                    stack.push(AklVal::mk_obj(id));
                }
                Op::Yield => {
                    let v = stack.pop().ok_or(VmError::StackUnderflow)?;
                    if let Some(gen_id) = gen {
                        // 再開位置（次の命令）と現在フレームのローカルを保存する。
                        let frame = frames.last().ok_or(VmError::StackUnderflow)?;
                        let locals = frame.locals.clone();
                        if let Some(Obj::Gen { pc: gpc, locals: glocals, .. }) =
                            self.heap.get_mut(gen_id)
                        {
                            *gpc = pc + 1;
                            *glocals = locals;
                        }
                        return Ok(RunEnd::Yield(v));
                    }
                    return Err(VmError::StackUnderflow);
                }
                Op::Await => {
                    let v = stack.pop().ok_or(VmError::StackUnderflow)?;
                    // 解決済み Promise なら値を unwrap。それ以外はそのまま。
                    let out = if v.is_obj() {
                        match self.heap.get(v.get_obj()) {
                            Some(Obj::Promise { state: 1, value }) => *value,
                            _ => v,
                        }
                    } else {
                        v
                    };
                    stack.push(out);
                }
                Op::PromiseWrap => {
                    let v = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let id = self
                        .heap
                        .alloc(Obj::Promise { state: 1, value: v })
                        .map_err(|_| VmError::Oom)?;
                    stack.push(AklVal::mk_obj(id));
                }
            }
            pc += 1;
        }
    }

    /// 例外を巻き戻す（frames を遡って catch_pc を探す。C の `akl_vm_unwind` 相当）。
    /// catch が見つかれば例外値をスタックに積んで pc を catch 位置へ。見つからなければ
    /// `Thrown` を返す（未捕捉例外）。
    fn unwind(
        &mut self,
        frames: &mut Vec<Frame>,
        stack: &mut Vec<AklVal>,
        pc: &mut usize,
        v: AklVal,
    ) -> Result<(), VmError> {
        while let Some(frame) = frames.last() {
            if let Some(catch_pc) = frame.catch_pc {
                stack.push(v);
                *pc = catch_pc;
                return Ok(());
            }
            if frames.len() == 1 {
                // 最上位フレーム（main）で catch なし
                return Err(VmError::Thrown(v));
            }
            frames.pop();
        }
        Err(VmError::Thrown(v))
    }

    /// 関数呼び出し（C の `OP_CALL` / `OP_MCALL` ハンドラ相当）。フレームを積み pc を
    /// callee 先頭へ。`this_v` は呼び出し時の `this`（メソッド呼び出しならレシーバ）。
    ///
    /// 戻り値: ネイティブ関数なら `Some(結果)`（フレームを積まず即座に返す）。
    /// バイトコード関数なら `None`（フレームを積んで pc を callee 先頭へ）。
    fn do_call(
        &mut self,
        frames: &mut Vec<Frame>,
        pc: &mut usize,
        callee: AklVal,
        this_v: AklVal,
        args: &[AklVal],
    ) -> Result<Option<AklVal>, VmError> {
        // OBJ コンストラクタのフォールバック（Date 等）
        let callee = self.ctor_of(callee);
        if !callee.is_obj() {
            return Err(VmError::NotCallable);
        }
        let id = callee.get_obj();
        // ハンドル束縛メソッド: vtable の call へディスパッチ
        if let Some(Obj::BoundMethod { handle, name }) = self.heap.get(id) {
            let (handle, name) = (*handle, *name);
            let (vtab, data, ptr) = match self.heap.get(handle) {
                Some(Obj::Handle { vtab, data, ptr }) => (*vtab, *data, *ptr),
                _ => return Err(VmError::NotCallable),
            };
            let name_str = match self.heap.get(name) {
                Some(Obj::Str(s)) => s.to_string(),
                _ => String::new(),
            };
            let r = match (vtab.call)(self, data, ptr, &name_str, args) {
                Some(v) => v,
                None => {
                    let msg = self.intern("TypeError: not a function").unwrap_or(0);
                    return Err(VmError::Thrown(AklVal::mk_obj(msg)));
                }
            };
            return Ok(Some(r));
        }
        // ホスト（FFI 層）提供のネイティブ関数
        if let Some(Obj::ForeignNative { idx, data }) = self.heap.get(id) {
            let (idx, data) = (*idx, *data);
            let f = self.foreign_fns.get(idx as usize).ok_or(VmError::NotCallable)?;
            let f = *f;
            let r = f(self, this_v, args, data)?;
            return Ok(Some(r));
        }
        // ネイティブ関数なら直接呼んで結果を返す
        if let Some(Obj::Native(nidx)) = self.heap.get(id) {
            let nidx = *nidx;
            let f = self.native_fns.get(nidx as usize).ok_or(VmError::NotCallable)?;
            let f = *f;
            let r = f(self, this_v, args)?;
            return Ok(Some(r));
        }
        let (fidx, env) = match self.heap.get(id) {
            Some(Obj::Func { fidx, env }) => (*fidx, *env),
            _ => return Err(VmError::NotCallable),
        };
        let (n_params, n_locals, rest_slot, is_gen) = {
            let f = self.funcs.get(fidx as usize).ok_or(VmError::NotCallable)?;
            (f.n_params, f.n_locals, f.rest_slot, f.is_gen)
        };
        let n = n_locals.max(n_params);
        let mut locals = vec![AklVal::UNDEF; n];
        for (i, a) in args.iter().enumerate().take(n_params) {
            locals[i] = *a;
        }
        // rest パラメータ: 余剰引数を配列で rest_slot に束縛
        if let Some(rs) = rest_slot {
            let rest_items: Vec<AklVal> = if args.len() > n_params {
                args[n_params..].to_vec()
            } else {
                Vec::new()
            };
            let arr_id = self.heap.alloc(Obj::Arr(rest_items)).map_err(|_| VmError::Oom)?;
            if (rs as usize) < locals.len() {
                locals[rs as usize] = AklVal::mk_obj(arr_id);
            }
        }
        // ジェネレータ関数: 実行せずに実行状態（Obj::Gen）を生成して返す。
        // 実際の実行は `next()`（gen_resume）で行う。
        if is_gen {
            let gen_id = self
                .heap
                .alloc(Obj::Gen { fidx, pc: 0, locals, env, this: this_v, done: false })
                .map_err(|_| VmError::Oom)?;
            return Ok(Some(AklVal::mk_obj(gen_id)));
        }
        let ret_pc = *pc + 1;
        frames.push(Frame { func: fidx, ret_pc, locals, this: this_v, env, catch_pc: None, is_new: false });
        *pc = 0;
        Ok(None)
    }

    /// ジェネレータを 1 段進める（`gen.next()`）。戻り値は `{ value, done }` の
    /// プレーンオブジェクト。C の `AKL_OK_GEN` の `gen_next` 相当。
    pub fn gen_resume(&mut self, gen_id: ObjId) -> Result<AklVal, VmError> {
        let (fidx, pc, locals, env, this, done) = match self.heap.get(gen_id) {
            Some(Obj::Gen { fidx, pc, locals, env, this, done }) => {
                (*fidx, *pc, locals.clone(), *env, *this, *done)
            }
            _ => return Err(VmError::NotCallable),
        };
        if done {
            return self.gen_result(AklVal::UNDEF, true);
        }
        let frames = vec![Frame {
            func: fidx,
            ret_pc: 0,
            locals,
            this,
            env,
            catch_pc: None,
            is_new: false,
        }];
        let stack = Vec::new();
        match self.run_loop(stack, frames, pc, Some(gen_id))? {
            RunEnd::Value(v) => {
                if let Some(Obj::Gen { done, .. }) = self.heap.get_mut(gen_id) {
                    *done = true;
                }
                self.gen_result(v, true)
            }
            RunEnd::Yield(v) => self.gen_result(v, false),
        }
    }

    /// `{ value, done }` のプレーンオブジェクトを生成する（`gen.next()` の戻り値）。
    fn gen_result(&mut self, value: AklVal, done: bool) -> Result<AklVal, VmError> {
        let value_id = self.intern("value").ok_or(VmError::Oom)?;
        let done_id = self.intern("done").ok_or(VmError::Oom)?;
        let id = self
            .heap
            .alloc(Obj::Obj(vec![
                (value_id, value),
                (done_id, AklVal::from_bool(done)),
            ]))
            .map_err(|_| VmError::Oom)?;
        Ok(AklVal::mk_obj(id))
    }

    /// 加算（int fast path + double + 文字列連結）。
    fn add(&mut self, a: AklVal, b: AklVal) -> Result<AklVal, VmError> {
        // BigInt + BigInt（i64 近似。オーバーフローは wrapping）
        if let (Some(x), Some(y)) = (self.bigint_of(a), self.bigint_of(b)) {
            let id = self
                .heap
                .alloc(Obj::BigInt(x.wrapping_add(y)))
                .map_err(|_| VmError::Oom)?;
            return Ok(AklVal::mk_obj(id));
        }
        if self.is_string(a) || self.is_string(b) {
            // ROPE 連結（遅延表現。両辺を文字列化して ROPE ノードを生成）
            let sa = self.stringify(a)?;
            let sb = self.stringify(b)?;
            let rope = self
                .heap
                .alloc(Obj::Rope { left: sa, right: sb })
                .map_err(|_| VmError::Oom)?;
            return Ok(AklVal::mk_obj(rope));
        }
        if a.is_int() && b.is_int() {
            return Ok(match int_add(a.get_int(), b.get_int()) {
                IntAdd::I32(v) => AklVal::mk_int(v),
                IntAdd::I64(v) => AklVal::from_f64(v as f64),
            });
        }
        Ok(AklVal::from_f64(self.to_number(a) + self.to_number(b)))
    }

    /// 二項算術（`-` `*`。int fast path + double）。
    fn arith_bin(&mut self, a: AklVal, b: AklVal, kind: Arith) -> Result<AklVal, VmError> {
        // BigInt op BigInt（i64 近似。オーバーフローは wrapping）
        if let (Some(x), Some(y)) = (self.bigint_of(a), self.bigint_of(b)) {
            let r = match kind {
                Arith::Sub => x.wrapping_sub(y),
                Arith::Mul => x.wrapping_mul(y),
            };
            let id = self.heap.alloc(Obj::BigInt(r)).map_err(|_| VmError::Oom)?;
            return Ok(AklVal::mk_obj(id));
        }
        if a.is_int() && b.is_int() {
            let (x, y) = (a.get_int() as i64, b.get_int() as i64);
            let r = match kind {
                Arith::Sub => x - y,
                Arith::Mul => x * y,
            };
            if r >= i32::MIN as i64 && r <= i32::MAX as i64 {
                return Ok(AklVal::mk_int(r as i32));
            }
            return Ok(AklVal::from_f64(r as f64));
        }
        let (da, db) = (self.to_number(a), self.to_number(b));
        Ok(AklVal::from_f64(match kind {
            Arith::Sub => da - db,
            Arith::Mul => da * db,
        }))
    }

    /// 剰余 `%`（int fast path + f64。0 除算は NaN）。
    fn mod_(&self, a: AklVal, b: AklVal) -> AklVal {
        if a.is_int() && b.is_int() && b.get_int() != 0 {
            let (x, y) = (a.get_int(), b.get_int());
            // INT32_MIN % -1 は C の UB を避けるため NaN（JS は 0 だが近似として回避）
            if x == i32::MIN && y == -1 {
                return AklVal::from_f64(f64::NAN);
            }
            return AklVal::mk_int(x % y);
        }
        AklVal::from_f64(self.to_number(a) % self.to_number(b))
    }

    /// 比較（数値化して比較。C の LT/LE/GT/GE 相当。NaN を含む比較は常に false）。
    fn compare(&self, a: AklVal, b: AklVal, cmp: Cmp) -> AklVal {
        let (da, db) = (self.to_number(a), self.to_number(b));
        if da.is_nan() || db.is_nan() {
            return AklVal::FALSE;
        }
        let r = match cmp {
            Cmp::Lt => da < db,
            Cmp::Le => da <= db,
            Cmp::Gt => da > db,
            Cmp::Ge => da >= db,
        };
        AklVal::from_bool(r)
    }

    /// ルーズ等値 `==`（単純化版。数値・文字列・null/undefined・真偽値を扱う）。
    fn eq(&mut self, a: AklVal, b: AklVal) -> bool {
        // null == undefined
        if (a.is_null() && b.is_undef()) || (a.is_undef() && b.is_null()) {
            return true;
        }
        // BigInt == BigInt（i64 比較）/ BigInt == 数値（数値化して比較）
        if let (Some(x), Some(y)) = (self.bigint_of(a), self.bigint_of(b)) {
            return x == y;
        }
        if let Some(x) = self.bigint_of(a) {
            return self.is_number(b) && x as f64 == self.to_number(b);
        }
        if let Some(y) = self.bigint_of(b) {
            return self.is_number(a) && self.to_number(a) == y as f64;
        }
        // 数値（int/double）同士・数値 vs 真偽値・数値 vs 数値文字列
        if self.is_number(a) && self.is_number(b) {
            return self.to_number(a) == self.to_number(b);
        }
        if self.is_number(a) && self.is_bool(b) {
            return self.to_number(a) == self.bool_to_num(b);
        }
        if self.is_bool(a) && self.is_number(b) {
            return self.bool_to_num(a) == self.to_number(b);
        }
        // 文字列同士
        if self.is_string(a) && self.is_string(b) {
            let (sa, sb) = (self.str_slice(a), self.str_slice(b));
            return sa == sb;
        }
        // 文字列 vs 数値（文字列を数値化）
        if self.is_string(a) && self.is_number(b) {
            return self.str_to_num(a) == self.to_number(b);
        }
        if self.is_number(a) && self.is_string(b) {
            return self.to_number(a) == self.str_to_num(b);
        }
        // 真偽値同士
        if self.is_bool(a) && self.is_bool(b) {
            return a == b;
        }
        // それ以外（オブジェクト参照・その他）は厳密等値に一致
        self.strict_eq(a, b)
    }

    /// 厳密等値 `===`（数値は int/double を統一して比較。NaN は自身と非等値）。
    fn strict_eq(&self, a: AklVal, b: AklVal) -> bool {
        if let (Some(x), Some(y)) = (self.bigint_of(a), self.bigint_of(b)) {
            return x == y;
        }
        if a.is_int() && b.is_int() {
            return a.get_int() == b.get_int();
        }
        if self.is_number(a) && self.is_number(b) {
            let (da, db) = (self.to_number(a), self.to_number(b));
            return !da.is_nan() && !db.is_nan() && da.to_bits() == db.to_bits();
        }
        if a.is_obj() && b.is_obj() {
            return a.get_obj() == b.get_obj();
        }
        // tagged 同士（undef/null/bool/tdz）はビット一致
        a.bits() == b.bits()
    }

    /// truthy 判定（JS の真偽値化）。
    fn truthy(&self, v: AklVal) -> bool {
        if v.is_undef() || v.is_null() || v == AklVal::FALSE {
            return false;
        }
        if v == AklVal::TRUE {
            return true;
        }
        if v.is_int() {
            return v.get_int() != 0;
        }
        if let Some(d) = v.as_f64() {
            return d != 0.0 && !d.is_nan();
        }
        if v.is_obj() {
            return match self.heap.get(v.get_obj()) {
                Some(Obj::Str(s)) => !s.is_empty(),
                Some(Obj::BigInt(x)) => *x != 0,
                _ => true,
            };
        }
        true
    }

    /// 値の `typeof` 文字列。
    fn type_name(&self, v: AklVal) -> &'static str {
        if v.is_int() || !v.is_tagged() {
            "number"
        } else if v.is_undef() {
            "undefined"
        } else if v.is_null() {
            "object"
        } else if v == AklVal::TRUE || v == AklVal::FALSE {
            "boolean"
        } else if v.is_obj() {
            match self.heap.get(v.get_obj()) {
                Some(Obj::Str(_)) => "string",
                Some(Obj::BigInt(_)) => "bigint",
                Some(Obj::Func { .. })
                | Some(Obj::Native(_))
                | Some(Obj::ForeignNative { .. })
                | Some(Obj::BoundMethod { .. }) => "function",
                _ => "object",
            }
        } else {
            "object"
        }
    }

    /// 値 → 数値（NaN-box の ToNumber。文字列は数値パース、失敗は NaN）。
    fn to_number(&self, v: AklVal) -> f64 {
        if v.is_int() {
            v.get_int() as f64
        } else if let Some(d) = v.as_f64() {
            d
        } else if v.is_undef() {
            f64::NAN
        } else if v.is_null() {
            0.0
        } else if v == AklVal::TRUE {
            1.0
        } else if v == AklVal::FALSE {
            0.0
        } else if v.is_obj() {
            match self.heap.get(v.get_obj()) {
                Some(Obj::Str(s)) => s.parse::<f64>().unwrap_or(f64::NAN),
                // Date は valueOf（エポック ms）に数値化
                Some(Obj::Date { ms }) => *ms,
                // BigInt は数値化（`==` 比較・単項 `+` 等）
                Some(Obj::BigInt(x)) => *x as f64,
                _ => f64::NAN,
            }
        } else {
            f64::NAN
        }
    }

    /// 値 → intern 済み文字列 ObjId（C の `akl_to_string` 相当の簡易版）。
    /// 値 → intern 済み文字列 ObjId（C の `akl_to_string` 相当。FFI 層の
    /// `akl_tostring` が使用する）。
    pub fn stringify(&mut self, v: AklVal) -> Result<ObjId, VmError> {
        let s: String = if v.is_int() {
            v.get_int().to_string()
        } else if let Some(d) = v.as_f64() {
            fmt_num(d)
        } else if v.is_undef() {
            "undefined".into()
        } else if v.is_null() {
            "null".into()
        } else if v == AklVal::TRUE {
            "true".into()
        } else if v == AklVal::FALSE {
            "false".into()
        } else if v.is_obj() {
            match self.heap.get(v.get_obj()) {
                Some(Obj::Str(_)) => return Ok(v.get_obj()),
                Some(Obj::Rope { .. }) => {
                    let flat = self.flatten_str(v);
                    return self.intern(&flat).ok_or(VmError::Oom);
                }
                Some(Obj::Arr(_)) => "[object Array]".into(),
                Some(Obj::Date { ms }) => date_to_string(*ms),
                Some(Obj::BigInt(x)) => x.to_string(),
                Some(Obj::Handle { vtab, .. }) => format!("[object {}]", vtab.tag),
                Some(Obj::BoundMethod { .. }) => "[object Function]".into(),
                _ => "[object Object]".into(),
            }
        } else {
            "undefined".into()
        };
        self.intern(&s).ok_or(VmError::Oom)
    }

    /// 値が数値（int または double）か。
    fn is_number(&self, v: AklVal) -> bool {
        v.is_int() || !v.is_tagged()
    }

    /// 値が BigInt（`Obj::BigInt`）ならその i64 を返す。
    fn bigint_of(&self, v: AklVal) -> Option<i64> {
        if !v.is_obj() {
            return None;
        }
        match self.heap.get(v.get_obj()) {
            Some(Obj::BigInt(x)) => Some(*x),
            _ => None,
        }
    }

    /// 値が真偽値か。
    fn is_bool(&self, v: AklVal) -> bool {
        v == AklVal::TRUE || v == AklVal::FALSE
    }

    /// 真偽値 → 数値（true=1, false=0）。
    fn bool_to_num(&self, v: AklVal) -> f64 {
        if v == AklVal::TRUE {
            1.0
        } else {
            0.0
        }
    }

    /// 値が文字列（Str または Rope）か。
    fn is_string(&self, v: AklVal) -> bool {
        v.is_obj()
            && matches!(
                self.heap.get(v.get_obj()),
                Some(Obj::Str(_)) | Some(Obj::Rope { .. })
            )
    }

    /// 文字列値を平坦化して `String` で返す（文字列でなければ空）。
    pub fn flatten_str(&self, v: AklVal) -> String {
        if !v.is_obj() {
            return String::new();
        }
        let mut result = String::new();
        self.flatten_str_rec(v.get_obj(), &mut result);
        result
    }

    /// ROPE を平坦化し、同一 ObjId の `Str` に置き換える（FFI の `akl_as_str` が安定
    /// ポインタを返すための準備。Rust 側は実行中に自動 GC しないため、置換後の
    /// `Box<str>` のヒープアドレスは `akl_free` まで不変）。
    pub fn flatten_rope_in_place(&mut self, id: ObjId) {
        if !matches!(self.heap.get(id), Some(Obj::Rope { .. })) {
            return;
        }
        let flat = self.flatten_str(AklVal::mk_obj(id));
        if let Some(slot) = self.heap.get_mut(id) {
            *slot = Obj::Str(flat.into_boxed_str());
        }
    }

    /// ROPE を再帰的に平坦化（深さ上限で防御）。
    fn flatten_str_rec(&self, id: ObjId, out: &mut String) {
        if out.len() > 1024 * 1024 * 1024 {
            return; // 1GB 防御
        }
        match self.heap.get(id) {
            Some(Obj::Str(s)) => out.push_str(s),
            Some(Obj::Rope { left, right }) => {
                self.flatten_str_rec(*left, out);
                self.flatten_str_rec(*right, out);
            }
            _ => {}
        }
    }

    /// 文字列値を `String` で返す（文字列でなければ空）。
    fn str_slice(&self, v: AklVal) -> String {
        self.flatten_str(v)
    }

    /// 文字列値を数値化（パース失敗は NaN）。
    fn str_to_num(&self, v: AklVal) -> f64 {
        self.str_slice(v).parse::<f64>().unwrap_or(f64::NAN)
    }

    /// 配列へ要素を設定（範囲拡張を伴う。C の `OP_ASET` 相当）。
    fn arr_set(&mut self, id: ObjId, index: usize, value: AklVal) -> Result<(), VmError> {
        match self.heap.get_mut(id) {
            Some(Obj::Arr(items)) => {
                if index >= items.len() {
                    items.resize(index + 1, AklVal::UNDEF);
                }
                items[index] = value;
                Ok(())
            }
            _ => Err(VmError::NotObject),
        }
    }

    /// 配列要素を削除（穴化。C の `OP_IDEL` 相当）。
    fn arr_delete(&mut self, id: ObjId, index: usize) -> bool {
        match self.heap.get_mut(id) {
            Some(Obj::Arr(items)) if index < items.len() => {
                items[index] = AklVal::UNDEF;
                true
            }
            _ => false,
        }
    }

    /// プレーンオブジェクトのプロパティを削除。
    fn prop_delete(&mut self, id: ObjId, name: ObjId) -> bool {
        match self.heap.get_mut(id) {
            Some(Obj::Obj(props)) => {
                if let Some(pos) = props.iter().position(|(n, _)| *n == name) {
                    props.remove(pos);
                    true
                } else {
                    false
                }
            }
            _ => false,
        }
    }
}

/// 二項算術の種類（`arith_bin` 用）。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Arith {
    /// 減算。
    Sub,
    /// 乗算。
    Mul,
}

/// f64 の JS 風文字列化（整数は小数点なし、NaN/Infinity は JS 表記）。公開版。
pub fn fmt_num_pub(d: f64) -> String {
    fmt_num(d)
}

/// f64 の JS 風文字列化（整数は小数点なし、NaN/Infinity は JS 表記）。
fn fmt_num(d: f64) -> String {
    if d.is_nan() {
        "NaN".into()
    } else if d.is_infinite() {
        if d > 0.0 {
            "Infinity".into()
        } else {
            "-Infinity".into()
        }
    } else if d == d.trunc() && d.abs() < 1e15 {
        format!("{}", d as i64)
    } else {
        format!("{d}")
    }
}

/// Date の UTC 分解フィールド。
#[derive(Clone, Copy, Debug)]
pub struct DateFields {
    /// 年（グレゴリオ暦、正数/負数）。
    pub year: i32,
    /// 月（1-12。JS の 0-based ではない）。
    pub month: u32,
    /// 日（1-31）。
    pub day: u32,
    /// 曜日（0=日曜 .. 6=土曜）。
    pub weekday: u32,
    /// 時（0-23）。
    pub hour: u32,
    /// 分（0-59）。
    pub minute: u32,
    /// 秒（0-59）。
    pub second: u32,
    /// ミリ秒（0-999）。
    pub millisecond: u32,
}

/// プロレプティック・グレゴリオ暦: 1970-01-01 からの日数 → (年, 月, 日)
/// （Howard Hinnant の `civil_from_days` アルゴリズム。seccomp 下の gmtime_r 代替）。
pub fn civil_from_days(z: i64) -> (i32, u32, u32) {
    let z = z + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365;
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    (if m <= 2 { y as i32 + 1 } else { y as i32 }, m as u32, d)
}

/// (年, 月, 日) → 1970-01-01 からの日数（グレゴリオ暦。月は 1-based）。
pub fn days_from_civil(y: i32, m: u32, d: u32) -> i64 {
    let y = y - i32::from(m <= 2);
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = (y - era * 400) as u64;
    let mp = if m > 2 { (m - 3) as u64 } else { (m + 9) as u64 };
    let doy = (153 * mp + 2) / 5 + d as u64 - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era as i64 * 146_097 + doe as i64 - 719_468
}

/// エポック ms → UTC フィールド（C の `akl_date_utc_fields` 相当）。
pub fn date_utc_fields(ms: f64) -> DateFields {
    let total_ms = ms as i64;
    let days = if total_ms >= 0 {
        total_ms / 86_400_000
    } else {
        (total_ms - 86_399_999) / 86_400_000
    };
    let mut rem = total_ms - days * 86_400_000;
    let days = if rem < 0 {
        rem += 86_400_000;
        days - 1
    } else {
        days
    };
    let (year, month, day) = civil_from_days(days);
    // 1970-01-01 は木曜(4)。(days % 7 + 4) を正規化して曜日 0..6。
    let weekday = ((days % 7) + 4).rem_euclid(7) as u32;
    DateFields {
        year,
        month,
        day,
        weekday,
        hour: (rem / 3_600_000) as u32,
        minute: ((rem / 60_000) % 60) as u32,
        second: ((rem / 1_000) % 60) as u32,
        millisecond: (rem % 1_000) as u32,
    }
}

/// エポック ms → ISO 8601 文字列（`YYYY-MM-DDTHH:MM:SS.sssZ`）。
pub fn date_to_iso_string(ms: f64) -> String {
    let f = date_utc_fields(ms);
    format!(
        "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
        f.year, f.month, f.day, f.hour, f.minute, f.second, f.millisecond
    )
}

/// エポック ms → `Date.prototype.toString` 相当（UTC 固定近似）。
pub fn date_to_string(ms: f64) -> String {
    const W: [&str; 7] = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
    const M: [&str; 12] = [
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    ];
    let f = date_utc_fields(ms);
    format!(
        "{} {} {:02} {:04} {:02}:{:02}:{:02} GMT+0000",
        W[f.weekday as usize],
        M[(f.month as usize).saturating_sub(1)],
        f.day,
        f.year,
        f.hour,
        f.minute,
        f.second
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    /// fib をグローバルに束縛して実行するテスト用ランタイムを組む。
    /// `fib(n) = n < 2 ? n : fib(n-1) + fib(n-2)` を手書きバイトコードで表現。
    fn fib_runtime() -> (Runtime, u32) {
        let mut rt = Runtime::new();
        // fib 本体: locals[0]=n
        //   0: LLoad(0); ConstI(2); Lt; JmpF(8)   // n < 2 なら続行、偽なら 8 へ
        //   4: LLoad(0); Ret
        //   8: GLoad(fib); LLoad(0); ConstI(1); Sub; Call(1)
        //   13: GLoad(fib); LLoad(0); ConstI(2); Sub; Call(1)
        //   18: Add; Ret
        let code = vec![
            Op::LLoad(0),
            Op::ConstI(2),
            Op::Lt,
            Op::JmpF(8),
            Op::LLoad(0),
            Op::Ret,
            // pc = 6 は JmpF が飛ばす先が 8 なので、ここは未達。番地を合わせる。
            Op::Undef,
            Op::Undef,
            Op::GLoad(ObjId::MAX), // 仮。後で fib_name に差し替え
            Op::LLoad(0),
            Op::ConstI(1),
            Op::Sub,
            Op::Call(1),
            Op::GLoad(ObjId::MAX), // 仮
            Op::LLoad(0),
            Op::ConstI(2),
            Op::Sub,
            Op::Call(1),
            Op::Add,
            Op::Ret,
        ];
        let fidx = rt.funcs.len() as u32;
        rt.funcs.push(FuncObj { code, name: None, n_params: 1, rest_slot: None, n_locals: 2, is_gen: false });
        let fib_name = rt.intern("fib").unwrap();
        // 仮置きの GLoad(ObjId::MAX) を fib_name に差し替える
        for op in &mut rt.funcs[fidx as usize].code {
            if matches!(op, Op::GLoad(ObjId::MAX)) {
                *op = Op::GLoad(fib_name);
            }
        }
        let fib_obj = rt.heap.alloc(Obj::Func { fidx, env: None }).unwrap();
        rt.global_set(fib_name, AklVal::mk_obj(fib_obj));
        (rt, fidx)
    }

    #[test]
    fn fib_recursion() {
        let (mut rt, fidx) = fib_runtime();
        assert_eq!(rt.run(fidx, &[AklVal::mk_int(0)]).unwrap(), AklVal::mk_int(0));
        assert_eq!(rt.run(fidx, &[AklVal::mk_int(1)]).unwrap(), AklVal::mk_int(1));
        assert_eq!(rt.run(fidx, &[AklVal::mk_int(10)]).unwrap(), AklVal::mk_int(55));
    }

    #[test]
    fn loop_sum() {
        let mut rt = Runtime::new();
        // var s = 0; for (i = 0; i < 10; i++) s += i;
        // locals: 0 = s, 1 = i
        let code = vec![
            Op::ConstI(0),
            Op::LStore(0), // s = 0
            Op::ConstI(0),
            Op::LStore(1), // i = 0
            // loop head (pc=4): i < 10 ? 
            Op::LLoad(1),
            Op::ConstI(10),
            Op::Lt,
            Op::JmpF(17), // false -> exit
            // s += i
            Op::LLoad(0),
            Op::LLoad(1),
            Op::Add,
            Op::LStore(0),
            // i++
            Op::LLoad(1),
            Op::ConstI(1),
            Op::Add,
            Op::LStore(1),
            Op::Jmp(4),
            // exit (pc=18): return s
            Op::LLoad(0),
            Op::Ret,
        ];
        let fidx = rt.funcs.len() as u32;
        rt.funcs.push(FuncObj { code, name: None, n_params: 0, rest_slot: None, n_locals: 2, is_gen: false });
        assert_eq!(rt.run(fidx, &[]).unwrap(), AklVal::mk_int(45));
    }

    #[test]
    fn string_concat() {
        let mut rt = Runtime::new();
        let a = rt.intern("hello ").unwrap();
        let b = rt.intern("world").unwrap();
        let code = vec![Op::ConstStr(a), Op::ConstStr(b), Op::Add, Op::Ret];
        let fidx = rt.funcs.len() as u32;
        rt.funcs.push(FuncObj { code, name: None, n_params: 0, rest_slot: None, n_locals: 0, is_gen: false });
        let r = rt.run(fidx, &[]).unwrap();
        // ROPE 連結: 平坦化して内容を検証
        let flattened = rt.flatten_str(r);
        assert_eq!(flattened, "hello world");
    }

    #[test]
    fn object_prop_set_get() {
        let mut rt = Runtime::new();
        let key = rt.intern("x").unwrap();
        let code = vec![
            Op::ObjNew,       // [obj]
            Op::Dup,          // [obj, obj]
            Op::ConstI(42),   // [obj, obj, 42]
            Op::PStore(key),  // obj.x = 42 → [obj, 42]
            Op::Pop,          // [obj]
            Op::PLoad(key),   // [obj.x]
            Op::Ret,
        ];
        let fidx = rt.funcs.len() as u32;
        rt.funcs.push(FuncObj { code, name: None, n_params: 0, rest_slot: None, n_locals: 0, is_gen: false });
        assert_eq!(rt.run(fidx, &[]).unwrap(), AklVal::mk_int(42));
    }

    #[test]
    fn array_literal_and_get() {
        let mut rt = Runtime::new();
        let code = vec![
            Op::ConstI(1),
            Op::ConstI(2),
            Op::ConstI(3),
            Op::ArrNew(3), // [1,2,3]
            Op::Dup,
            Op::ConstI(1),
            Op::AGet, // [1,2,3][1] = 2
            Op::Ret,
        ];
        let fidx = rt.funcs.len() as u32;
        rt.funcs.push(FuncObj { code, name: None, n_params: 0, rest_slot: None, n_locals: 0, is_gen: false });
        assert_eq!(rt.run(fidx, &[]).unwrap(), AklVal::mk_int(2));
    }

    #[test]
    fn typeof_and_strict_eq() {
        let mut rt = Runtime::new();
        let code = vec![
            Op::ConstI(1),
            Op::Typeof, // "number"
            Op::ConstI(1),
            Op::ConstD(1.0),
            Op::Seq, // 1 === 1.0 → true
            Op::Ret,
        ];
        let fidx = rt.funcs.len() as u32;
        rt.funcs.push(FuncObj { code, name: None, n_params: 0, rest_slot: None, n_locals: 0, is_gen: false });
        assert_eq!(rt.run(fidx, &[]).unwrap(), AklVal::TRUE);
    }

    #[test]
    fn call_not_callable_errors() {
        let mut rt = Runtime::new();
        let code = vec![Op::ConstI(5), Op::Call(0), Op::Ret];
        let fidx = rt.funcs.len() as u32;
        rt.funcs.push(FuncObj { code, name: None, n_params: 0, rest_slot: None, n_locals: 0, is_gen: false });
        assert_eq!(rt.run(fidx, &[]), Err(VmError::NotCallable));
    }

    #[test]
    fn global_store_load() {
        let mut rt = Runtime::new();
        let g = rt.intern("g").unwrap();
        let code = vec![
            Op::ConstI(7),
            Op::GStore(g),
            Op::GLoad(g),
            Op::Ret,
        ];
        let fidx = rt.funcs.len() as u32;
        rt.funcs.push(FuncObj { code, name: None, n_params: 0, rest_slot: None, n_locals: 0, is_gen: false });
        assert_eq!(rt.run(fidx, &[]).unwrap(), AklVal::mk_int(7));
        assert_eq!(rt.global_get(g), Some(AklVal::mk_int(7)));
    }
}
