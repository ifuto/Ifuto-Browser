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
    /// ローカル変数数（パラメータ含む）。
    pub n_locals: usize,
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
    /// グローバル変数へ pop した値を保存（C の `GSTORE`）。
    GStore(ObjId),
    /// 無条件ジャンプ。
    Jmp(u32),
    /// pop して偽ならジャンプ（C の `JMPF`）。
    JmpF(u32),
    /// pop して真ならジャンプ（C の `JMPT`）。
    JmpT(u32),
    /// 関数呼び出し（argc 個の引数 + callee を pop）。
    Call(u8),
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
    /// 要素読み出し（idx, obj を pop、値を push。C の `AGET`）。
    AGet,
    /// 要素書き込み（val, idx, obj を pop、val を push。C の `ASET`）。
    ASet,
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
}

impl Runtime {
    /// 空のランタイムを作る。
    pub fn new() -> Self {
        Self::default()
    }

    /// 文字列を intern してヒープ上の文字列 ObjId を返す（失敗時 None）。
    pub fn intern(&mut self, s: &str) -> Option<ObjId> {
        self.interner.intern(&mut self.heap, s)
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

    /// 関数表 index の関数を実行する（C の `vm_exec` 相当）。
    ///
    /// `args` はエントリ関数の引数。戻り値は関数の返り値（`Ret`）または
    /// 停止時（`Halt`）のスタック先頭。
    pub fn run(&mut self, func_idx: u32, args: &[AklVal]) -> Result<AklVal, VmError> {
        let mut stack: Vec<AklVal> = Vec::new();
        let mut frames: Vec<Frame> = Vec::new();
        let mut pc = 0usize;

        // エントリフレーム
        {
            let f = self.funcs.get(func_idx as usize).ok_or(VmError::NotCallable)?;
            let n = f.n_locals.max(f.n_params);
            let mut locals = vec![AklVal::UNDEF; n];
            for (i, a) in args.iter().enumerate().take(f.n_params) {
                locals[i] = *a;
            }
            frames.push(Frame { func: func_idx, ret_pc: 0, locals, this: AklVal::UNDEF });
        }

        loop {
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
                    return Ok(v);
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
                Op::Call(argc) => {
                    let argc = argc as usize;
                    if stack.len() < argc + 1 {
                        return Err(VmError::StackUnderflow);
                    }
                    let callee = stack[stack.len() - argc - 1];
                    let args: Vec<AklVal> = stack[stack.len() - argc..].to_vec();
                    stack.truncate(stack.len() - argc - 1);
                    self.do_call(&mut frames, &mut pc, callee, &args)?;
                    continue;
                }
                Op::Ret => {
                    let v = stack.pop().ok_or(VmError::StackUnderflow)?;
                    // 戻るフレーム自身の ret_pc（呼び出し元の再開位置）を使う。
                    // 呼び出し元フレームの ret_pc（= その呼び出し元への再開位置）では
                    // 誤って直上の呼び出し元へ戻ってしまう（実測で特定）。
                    let frame = frames.pop().ok_or(VmError::StackUnderflow)?;
                    if frames.is_empty() {
                        return Ok(v);
                    }
                    stack.push(v);
                    pc = frame.ret_pc;
                    continue;
                }
                Op::MakeF(fidx) => {
                    let env = None; // クロージャ捕捉はパーサ・codegen フェーズで導入
                    let id = self
                        .heap
                        .alloc(Obj::Func { fidx, env })
                        .map_err(|_| VmError::Oom)?;
                    stack.push(AklVal::mk_obj(id));
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
                    let v = self.heap.prop_get(obj.get_obj(), name).unwrap_or(AklVal::UNDEF);
                    stack.push(v);
                }
                Op::PStore(name) => {
                    let val = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let obj = stack.pop().ok_or(VmError::StackUnderflow)?;
                    if !obj.is_obj() {
                        return Err(VmError::NotObject);
                    }
                    self.heap
                        .prop_set(obj.get_obj(), name, val)
                        .map_err(|_| VmError::NotObject)?;
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
                Op::AGet => {
                    let idx = stack.pop().ok_or(VmError::StackUnderflow)?;
                    let obj = stack.pop().ok_or(VmError::StackUnderflow)?;
                    if !obj.is_obj() {
                        return Err(VmError::NotObject);
                    }
                    let i = self.to_number(idx) as i64;
                    let v = if i >= 0 {
                        self.heap.arr_get(obj.get_obj(), i as usize).unwrap_or(AklVal::UNDEF)
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
                    let i = self.to_number(idx) as i64;
                    if i >= 0 {
                        self.arr_set(obj.get_obj(), i as usize, val)?;
                    }
                    stack.push(val);
                }
                Op::Halt => {
                    return Ok(self.last_val);
                }
            }
            pc += 1;
        }
    }

    /// 関数呼び出し（C の `OP_CALL` ハンドラ相当）。フレームを積み pc を callee 先頭へ。
    fn do_call(
        &mut self,
        frames: &mut Vec<Frame>,
        pc: &mut usize,
        callee: AklVal,
        args: &[AklVal],
    ) -> Result<(), VmError> {
        if !callee.is_obj() {
            return Err(VmError::NotCallable);
        }
        let id = callee.get_obj();
        let fidx = match self.heap.get(id) {
            Some(Obj::Func { fidx, .. }) => *fidx,
            _ => return Err(VmError::NotCallable),
        };
        let (n_params, n_locals) = {
            let f = self.funcs.get(fidx as usize).ok_or(VmError::NotCallable)?;
            (f.n_params, f.n_locals)
        };
        let n = n_locals.max(n_params);
        let mut locals = vec![AklVal::UNDEF; n];
        for (i, a) in args.iter().enumerate().take(n_params) {
            locals[i] = *a;
        }
        let ret_pc = *pc + 1;
        frames.push(Frame { func: fidx, ret_pc, locals, this: AklVal::UNDEF });
        *pc = 0;
        Ok(())
    }

    /// 加算（int fast path + double + 文字列連結）。
    fn add(&mut self, a: AklVal, b: AklVal) -> Result<AklVal, VmError> {
        if self.is_string(a) || self.is_string(b) {
            let sa = self.stringify(a)?;
            let sb = self.stringify(b)?;
            let (ca, cb) = {
                let x = self.heap.get(sa).and_then(str_of).unwrap_or("");
                let y = self.heap.get(sb).and_then(str_of).unwrap_or("");
                (x.to_owned(), y.to_owned())
            };
            let combined = format!("{ca}{cb}");
            let id = self.intern(&combined).ok_or(VmError::Oom)?;
            return Ok(AklVal::mk_obj(id));
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
    fn arith_bin(&self, a: AklVal, b: AklVal, kind: Arith) -> Result<AklVal, VmError> {
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
                Some(Obj::Func { .. }) => "function",
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
                _ => f64::NAN,
            }
        } else {
            f64::NAN
        }
    }

    /// 値 → intern 済み文字列 ObjId（C の `akl_to_string` 相当の簡易版）。
    fn stringify(&mut self, v: AklVal) -> Result<ObjId, VmError> {
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
                Some(Obj::Arr(_)) => "[object Array]".into(),
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

    /// 値が文字列か。
    fn is_string(&self, v: AklVal) -> bool {
        v.is_obj() && matches!(self.heap.get(v.get_obj()), Some(Obj::Str(_)))
    }

    /// 文字列値を `&str` で返す（文字列でなければ空）。
    fn str_slice(&self, v: AklVal) -> &str {
        if v.is_obj() {
            if let Some(Obj::Str(s)) = self.heap.get(v.get_obj()) {
                return s;
            }
        }
        ""
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
}

/// 二項算術の種類（`arith_bin` 用）。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Arith {
    /// 減算。
    Sub,
    /// 乗算。
    Mul,
}

/// `Obj::Str` から `&str` を取り出すヘルパー。
fn str_of(obj: &Obj) -> Option<&str> {
    match obj {
        Obj::Str(s) => Some(s),
        _ => None,
    }
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
        rt.funcs.push(FuncObj { code, name: None, n_params: 1, n_locals: 2 });
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
        rt.funcs.push(FuncObj { code, name: None, n_params: 0, n_locals: 2 });
        assert_eq!(rt.run(fidx, &[]).unwrap(), AklVal::mk_int(45));
    }

    #[test]
    fn string_concat() {
        let mut rt = Runtime::new();
        let a = rt.intern("hello ").unwrap();
        let b = rt.intern("world").unwrap();
        let code = vec![Op::ConstStr(a), Op::ConstStr(b), Op::Add, Op::Ret];
        let fidx = rt.funcs.len() as u32;
        rt.funcs.push(FuncObj { code, name: None, n_params: 0, n_locals: 0 });
        let r = rt.run(fidx, &[]).unwrap();
        assert_eq!(r.is_obj(), true);
        match rt.heap.get(r.get_obj()) {
            Some(Obj::Str(s)) => assert_eq!(&**s, "hello world"),
            other => panic!("結果は文字列であるべき: {other:?}"),
        }
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
        rt.funcs.push(FuncObj { code, name: None, n_params: 0, n_locals: 0 });
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
        rt.funcs.push(FuncObj { code, name: None, n_params: 0, n_locals: 0 });
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
        rt.funcs.push(FuncObj { code, name: None, n_params: 0, n_locals: 0 });
        assert_eq!(rt.run(fidx, &[]).unwrap(), AklVal::TRUE);
    }

    #[test]
    fn call_not_callable_errors() {
        let mut rt = Runtime::new();
        let code = vec![Op::ConstI(5), Op::Call(0), Op::Ret];
        let fidx = rt.funcs.len() as u32;
        rt.funcs.push(FuncObj { code, name: None, n_params: 0, n_locals: 0 });
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
        rt.funcs.push(FuncObj { code, name: None, n_params: 0, n_locals: 0 });
        assert_eq!(rt.run(fidx, &[]).unwrap(), AklVal::mk_int(7));
        assert_eq!(rt.global_get(g), Some(AklVal::mk_int(7)));
    }
}
