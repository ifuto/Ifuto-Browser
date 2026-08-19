//! コード生成（フェーズ 4）。AST（[`crate::parser`]）→ バイトコード（[`crate::bytecode::Op`]）。
//!
//! C 実装からの移植対応:
//! | C (akl.c) | Rust |
//! |---|---|
//! | `cg_stmt` / `cg_expr`（one-pass codegen） | [`Compiler::gen_stmt`] / [`Compiler::gen_expr`] |
//! | `Cg` の locals 表（名前 → slot） | [`Compiler::locals`]（`HashMap<String, u32>`） |
//! | `cg_op`（バイトコード追記） | `code.push(Op::...)` |
//! | `cg_hoist_funcs`（関数宣言の先頭バインド） | [`compile`] のパス 1 + hoist |
//!
//! # スコープ解決（C の関数スコープ近似）
//!
//! - 各関数について、パラメータと `var`/`let`/`const` 宣言名をローカルスロットに割り当てる
//! - それ以外の識別子はグローバル変数として扱う（`GLoad` / `GStore`。名前は intern 済み
//!   ObjId）
//! - 関数宣言はトップレベルのみ。先に全関数をコンパイルして関数表 index を確定し、
//!   main の先頭で `MakeF` + `GStore` によりグローバルへ hoist 束縛する（C の
//!   `cg_hoist_funcs` 相当）
//!
//! # 既知の近似（今後のフェーズ）
//!
//! - `&&` / `||` は両辺を真偽値化した論理演算（JS の「オペランドの値を返す」短絡は未対応。
//!   条件コンテキストでは正しい）
//! - ネスト関数・クロージャ・アロー関数・class・オブジェクト/配列リテラルは未対応
//! - 複合代入（`+=` 等）・`++`/`--`・分割代入・三項・`for`/`do-while`/`switch` は未対応

#![forbid(unsafe_code)]
#![warn(missing_docs)]

use std::collections::HashMap;

use crate::bytecode::{FuncObj, Op, Runtime};
use crate::lexer::NumLit;
use crate::parser::{BinOp, Expr, ForInit, LogicalOp, ObjEntry, Stmt, UnaryOp};

/// コード生成エラー。
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct CompileError(pub String);

impl std::fmt::Display for CompileError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.0)
    }
}

/// プログラムをコンパイルし、エントリ（main）関数の関数表 index を返す。
///
/// パス 1 で全トップレベル関数宣言をコンパイル・登録し、パス 2 で main を生成する
/// （main の先頭で各関数をグローバルへ hoist 束縛）。
pub fn compile(rt: &mut Runtime, program: &[Stmt]) -> Result<u32, CompileError> {
    let mut c = Compiler {
        rt,
        locals: HashMap::new(),
        captures: HashMap::new(),
        capture_order: Vec::new(),
        break_patches: Vec::new(),
        continue_patches: Vec::new(),
        cur_is_async: false,
        labels: HashMap::new(),
    };

    // パス 1: トップレベル関数宣言を収集・登録（box 化ローカルを解析してから）
    let mut funcs: Vec<(String, u32)> = Vec::new();
    for stmt in program {
        if let Stmt::FuncDecl { name, params, rest, body, is_gen, is_async } = stmt {
            let boxed = compute_boxed(params, body);
            let fidx = c.compile_function(name, params, rest.as_deref(), body, &boxed, *is_gen, *is_async)?;
            funcs.push((name.clone(), fidx));
        }
    }

    // パス 2: main はローカルを持たない（トップレベル var は JS 同様グローバル）。
    // これにより `var g = 1; function f(){ return g; }` の g が正しくグローバル解決される。
    c.locals = HashMap::new();
    c.captures = HashMap::new();
    c.capture_order = Vec::new();

    let mut code = Vec::new();
    // 関数宣言の hoist（MakeF + GStore）
    for (name, fidx) in &funcs {
        let name_id = c.rt.intern(name).ok_or_else(|| CompileError("intern failed".into()))?;
        code.push(Op::MakeF(*fidx));
        code.push(Op::GStore(name_id));
    }
    // 本体
    for stmt in program {
        if matches!(stmt, Stmt::FuncDecl { .. }) {
            continue; // 先頭で hoist 済み
        }
        c.gen_stmt(stmt, &mut code)?;
    }
    code.push(Op::Undef);
    code.push(Op::Halt);

    let n_locals = c.locals.len();
    let fidx = rt.funcs.len() as u32;
    rt.funcs.push(FuncObj { code, name: None, n_params: 0, rest_slot: None, n_locals, is_gen: false });
    Ok(fidx)
}

/// コード生成器。`rt` への排他参照（intern 用）と、現在の関数のローカルスロット表を持つ。
struct Compiler<'a> {
    rt: &'a mut Runtime,
    locals: HashMap<String, u32>,
    /// 現在の関数が捕捉する変数（name → env index）。トップレベル/main は空。
    captures: HashMap<String, u32>,
    /// 捕捉変数の出現順（env 構築用）。
    capture_order: Vec<String>,
    /// `break` のジャンプ先パッチ位置（ループごとのスタック）。
    break_patches: Vec<Vec<usize>>,
    /// `continue` のジャンプ先パッチ位置（ループごとのスタック）。
    continue_patches: Vec<Vec<usize>>,
    /// 現在コンパイル中の関数が async か（`return` を Promise で包む）。
    cur_is_async: bool,
    /// ラベル名 → break/continue patch リストの深さ（`break label` / `continue label` 用）。
    labels: HashMap<String, usize>,
}

impl Compiler<'_> {
    /// 関数をコンパイルして関数表に登録し、index を返す。
    /// `boxed` は「ネスト関数に捕捉される自ローカル」の集合（共有セルとして env に box 化）。
    #[allow(clippy::too_many_arguments)]
    fn compile_function(
        &mut self,
        name: &str,
        params: &[String],
        rest: Option<&str>,
        body: &[Stmt],
        boxed: &HashMap<String, u32>,
        is_gen: bool,
        is_async: bool,
    ) -> Result<u32, CompileError> {
        let mut locals = HashMap::new();
        for p in params {
            locals.insert(p.clone(), locals.len() as u32);
        }
        // rest パラメータはローカルスロットに束縛（余剰引数が配列で入る）
        let rest_slot = if let Some(r) = rest {
            let slot = locals.len() as u32;
            locals.insert(r.to_string(), slot);
            Some(slot)
        } else {
            None
        };
        collect_vars(body, &mut locals);
        // box 化されたローカルは locals に残したまま、参照時に captures を優先する
        // （スロット番号の穴を避ける。box 化ローカルは未使用の locals スロットが残るが無害）

        let saved_locals = std::mem::take(&mut self.locals);
        let saved_captures = std::mem::take(&mut self.captures);
        let saved_order = std::mem::take(&mut self.capture_order);
        let saved_breaks = std::mem::take(&mut self.break_patches);
        let saved_continues = std::mem::take(&mut self.continue_patches);
        let saved_async = self.cur_is_async;
        self.locals = locals;
        self.captures = boxed.clone();
        self.capture_order = Vec::new();
        self.break_patches = Vec::new();
        self.continue_patches = Vec::new();
        self.cur_is_async = is_async;

        let mut code = Vec::new();
        // 関数入口で自前 env（box 化ローカル）を生成（C の frame_hidden 相当）
        if !boxed.is_empty() {
            code.push(Op::MakeEnv(boxed.len() as u32));
        }
        for stmt in body {
            self.gen_stmt(stmt, &mut code)?;
        }
        // 暗黙の return undefined（async は Promise で包む）
        code.push(Op::Undef);
        if is_async {
            code.push(Op::PromiseWrap);
        }
        code.push(Op::Ret);

        let n_locals = self.locals.len();
        let n_params = params.len();
        let name_id = self.rt.intern(name).ok_or_else(|| CompileError("intern failed".into()))?;
        let fidx = self.rt.funcs.len() as u32;
        self.rt.funcs.push(FuncObj {
            code,
            name: Some(name_id),
            n_params,
            rest_slot,
            n_locals,
            is_gen,
        });

        self.locals = saved_locals;
        self.captures = saved_captures;
        self.capture_order = saved_order;
        self.break_patches = saved_breaks;
        self.continue_patches = saved_continues;
        self.cur_is_async = saved_async;
        Ok(fidx)
    }

    /// ネスト関数をコンパイルして関数表に登録し、fidx を返す。
    /// 自由変数は「外側の env（captures）に解決」されたら CeLoad/CeStore、なければグローバル。
    #[allow(clippy::too_many_arguments)]
    fn compile_nested(
        &mut self,
        name: &str,
        params: &[String],
        rest: Option<&str>,
        body: &[Stmt],
        enclosing_env: &HashMap<String, u32>,
        is_gen: bool,
        is_async: bool,
    ) -> Result<u32, CompileError> {
        // 深いネスト（3 段以上）は未対応（env チェーンが必要。明白に失敗させる）
        if body.iter().any(|s| matches!(s, Stmt::FuncDecl { .. })) {
            return Err(CompileError("deeply nested closures are not yet supported".into()));
        }

        // ネスト関数のローカル（パラメータ + var 宣言 + ネスト関数名）
        let mut locals = HashMap::new();
        for p in params {
            locals.insert(p.clone(), locals.len() as u32);
        }
        let rest_slot = if let Some(r) = rest {
            let slot = locals.len() as u32;
            locals.insert(r.to_string(), slot);
            Some(slot)
        } else {
            None
        };
        collect_vars(body, &mut locals);

        // 自由変数（参照されるがローカルでない名前）
        let mut refs = std::collections::HashSet::new();
        collect_refs(body, &mut refs);
        let mut captures: HashMap<String, u32> = HashMap::new();
        for r in refs {
            if locals.contains_key(&r) {
                continue;
            }
            if let Some(idx) = enclosing_env.get(&r) {
                captures.insert(r, *idx);
            }
            // それ以外はグローバル（GLoad/GStore）
        }

        let saved_locals = std::mem::take(&mut self.locals);
        let saved_captures = std::mem::take(&mut self.captures);
        let saved_order = std::mem::take(&mut self.capture_order);
        let saved_breaks = std::mem::take(&mut self.break_patches);
        let saved_continues = std::mem::take(&mut self.continue_patches);
        let saved_async = self.cur_is_async;
        self.locals = locals;
        self.captures = captures;
        self.capture_order = Vec::new();
        self.break_patches = Vec::new();
        self.continue_patches = Vec::new();
        self.cur_is_async = is_async;

        let mut code = Vec::new();
        for stmt in body {
            self.gen_stmt(stmt, &mut code)?;
        }
        code.push(Op::Undef);
        if is_async {
            code.push(Op::PromiseWrap);
        }
        code.push(Op::Ret);

        let n_locals = self.locals.len();
        let n_params = params.len();
        let name_id = self.rt.intern(name).ok_or_else(|| CompileError("intern failed".into()))?;
        let fidx = self.rt.funcs.len() as u32;
        self.rt.funcs.push(FuncObj { code, name: Some(name_id), n_params, rest_slot, n_locals, is_gen });

        self.locals = saved_locals;
        self.captures = saved_captures;
        self.capture_order = saved_order;
        self.break_patches = saved_breaks;
        self.continue_patches = saved_continues;
        self.cur_is_async = saved_async;

        Ok(fidx)
    }

    /// 匿名関数（関数式・アロー関数）をコンパイルして関数表に登録し、fidx を返す。
    /// 自由変数は `enclosing_env`（外側の捕捉 env）に解決されたら CeLoad/CeStore、
    /// なければグローバル。`boxed` は自ローカルのうちネスト関数に捕捉されるもの。
    fn compile_function_anon(
        &mut self,
        params: &[String],
        rest: Option<&str>,
        body: &[Stmt],
        boxed: &HashMap<String, u32>,
        enclosing_env: &HashMap<String, u32>,
    ) -> Result<u32, CompileError> {
        let mut locals = HashMap::new();
        for p in params {
            locals.insert(p.clone(), locals.len() as u32);
        }
        let rest_slot = if let Some(r) = rest {
            let slot = locals.len() as u32;
            locals.insert(r.to_string(), slot);
            Some(slot)
        } else {
            None
        };
        collect_vars(body, &mut locals);

        // 自由変数（参照されるが自ローカルでない名前）を捕捉解決
        let mut refs = std::collections::HashSet::new();
        collect_refs(body, &mut refs);
        let mut captures: HashMap<String, u32> = boxed.clone();
        for r in refs {
            if locals.contains_key(&r) || captures.contains_key(&r) {
                continue;
            }
            if let Some(idx) = enclosing_env.get(&r) {
                captures.insert(r, *idx);
            }
        }

        let saved_locals = std::mem::take(&mut self.locals);
        let saved_captures = std::mem::take(&mut self.captures);
        let saved_order = std::mem::take(&mut self.capture_order);
        let saved_breaks = std::mem::take(&mut self.break_patches);
        let saved_continues = std::mem::take(&mut self.continue_patches);
        self.locals = locals;
        self.captures = captures;
        self.capture_order = Vec::new();
        self.break_patches = Vec::new();
        self.continue_patches = Vec::new();

        let mut code = Vec::new();
        if !boxed.is_empty() {
            code.push(Op::MakeEnv(boxed.len() as u32));
        }
        for stmt in body {
            self.gen_stmt(stmt, &mut code)?;
        }
        code.push(Op::Undef);
        code.push(Op::Ret);

        let n_locals = self.locals.len();
        let n_params = params.len();
        let fidx = self.rt.funcs.len() as u32;
        self.rt.funcs.push(FuncObj { code, name: None, n_params, rest_slot, n_locals, is_gen: false });

        self.locals = saved_locals;
        self.captures = saved_captures;
        self.capture_order = saved_order;
        self.break_patches = saved_breaks;
        self.continue_patches = saved_continues;

        Ok(fidx)
    }

    /// 式をコード生成（結果をスタックに残す）。
    fn gen_expr(&mut self, expr: &Expr, code: &mut Vec<Op>) -> Result<(), CompileError> {
        match expr {
            Expr::Num(NumLit::Int(v)) => code.push(Op::ConstI(*v)),
            Expr::Num(NumLit::Float(d)) => code.push(Op::ConstD(*d)),
            Expr::Num(NumLit::BigInt(v)) => code.push(Op::BigInt(*v)),
            Expr::Str(s) => {
                let id = self.rt.intern(s).ok_or_else(|| CompileError("intern failed".into()))?;
                code.push(Op::ConstStr(id));
            }
            Expr::Bool(true) => code.push(Op::True),
            Expr::Bool(false) => code.push(Op::False),
            Expr::Null => code.push(Op::Null),
            Expr::Undef => code.push(Op::Undef),
            Expr::Ident(name) => self.gen_load(name, code)?,
            Expr::Unary { op, operand } => {
                self.gen_expr(operand, code)?;
                code.push(match op {
                    UnaryOp::Neg => Op::Neg,
                    UnaryOp::Pos => Op::Pos,
                    UnaryOp::Not => Op::Not,
                    UnaryOp::Typeof => Op::Typeof,
                    UnaryOp::BNot => Op::BNot,
                });
            }
            Expr::Bin { op, lhs, rhs } => {
                self.gen_expr(lhs, code)?;
                self.gen_expr(rhs, code)?;
                code.push(match op {
                    BinOp::Add => Op::Add,
                    BinOp::Sub => Op::Sub,
                    BinOp::Mul => Op::Mul,
                    BinOp::Div => Op::Div,
                    BinOp::Mod => Op::Mod,
                    BinOp::Eq => Op::Eq,
                    BinOp::Ne => Op::Ne,
                    BinOp::Seq => Op::Seq,
                    BinOp::Sne => Op::Sne,
                    BinOp::Lt => Op::Lt,
                    BinOp::Le => Op::Le,
                    BinOp::Gt => Op::Gt,
                    BinOp::Ge => Op::Ge,
                    BinOp::And => Op::And,
                    BinOp::Or => Op::Or,
                    BinOp::BAnd => Op::BAnd,
                    BinOp::BOr => Op::BOr,
                    BinOp::BXor => Op::BXor,
                    BinOp::BShl => Op::BShl,
                    BinOp::BShr => Op::BShr,
                    BinOp::BUShr => Op::BUShr,
                });
            }
            Expr::Assign { name, rhs } => {
                self.gen_expr(rhs, code)?;
                code.push(Op::Dup); // 代入式の値（rhs）を残す
                self.gen_store(name, code)?;
            }
            Expr::Call { callee, args } => {
                // spread 引数があるか（`f(...a, b)`）
                let has_spread = args.iter().any(|a| matches!(a, Expr::Spread(_)));
                let argc_name = self
                    .rt
                    .intern("\x01spread_argc")
                    .ok_or_else(|| CompileError("intern failed".into()))?;
                // メソッド呼び出し `obj.method(...)` は this=obj で呼ぶ（MCallName）。
                // ハンドル（DOM 等）は vtable の call へ直接ディスパッチし、プロパティ
                // 取得（PLoad）と構文的に分離する（C の OP_MCALL と同型）。
                if let Expr::Member { obj, name } = &**callee {
                    self.gen_expr(obj, code)?; // receiver
                    let name_id = self
                        .rt
                        .intern(name)
                        .ok_or_else(|| CompileError("intern failed".into()))?;
                    if has_spread {
                        // 固定引数数をカウンタに設定 → spread 引数を展開 → MCallDyn
                        let fixed = args.iter().filter(|a| !matches!(a, Expr::Spread(_))).count();
                        code.push(Op::ConstI(fixed as i32));
                        code.push(Op::GStore(argc_name));
                        for a in args {
                            match a {
                                Expr::Spread(e) => {
                                    self.gen_expr(e, code)?;
                                    code.push(Op::ArrSpreadC(argc_name));
                                }
                                _ => self.gen_expr(a, code)?,
                            }
                        }
                        code.push(Op::MCallDyn { name: name_id, argc_name });
                    } else {
                        for a in args {
                            self.gen_expr(a, code)?;
                        }
                        if args.len() > u8::MAX as usize {
                            return Err(CompileError("too many arguments".into()));
                        }
                        code.push(Op::MCallName { name: name_id, argc: args.len() as u8 });
                    }
                } else {
                    self.gen_expr(callee, code)?;
                    if has_spread {
                        let fixed = args.iter().filter(|a| !matches!(a, Expr::Spread(_))).count();
                        code.push(Op::ConstI(fixed as i32));
                        code.push(Op::GStore(argc_name));
                        for a in args {
                            match a {
                                Expr::Spread(e) => {
                                    self.gen_expr(e, code)?;
                                    code.push(Op::ArrSpreadC(argc_name));
                                }
                                _ => self.gen_expr(a, code)?,
                            }
                        }
                        code.push(Op::CallDyn(argc_name));
                    } else {
                        for a in args {
                            self.gen_expr(a, code)?;
                        }
                        if args.len() > u8::MAX as usize {
                            return Err(CompileError("too many arguments".into()));
                        }
                        code.push(Op::Call(args.len() as u8));
                    }
                }
            }
            Expr::Arr(items) => {
                // spread がある場合は ArrPush/ArrPushAll でビルドアップ
                if items.iter().any(|i| matches!(i, Expr::Spread(_))) {
                    code.push(Op::ArrNew(0));
                    for item in items {
                        match item {
                            Expr::Spread(e) => {
                                self.gen_expr(e, code)?;
                                code.push(Op::ArrPushAll);
                            }
                            Expr::Hole => {
                                code.push(Op::Undef);
                                code.push(Op::ArrPush);
                            }
                            _ => {
                                self.gen_expr(item, code)?;
                                code.push(Op::ArrPush);
                            }
                        }
                    }
                } else {
                    for item in items {
                        match item {
                            Expr::Hole => code.push(Op::Undef),
                            _ => self.gen_expr(item, code)?,
                        }
                    }
                    if items.len() > u32::MAX as usize {
                        return Err(CompileError("array too large".into()));
                    }
                    code.push(Op::ArrNew(items.len() as u32));
                }
            }
            Expr::ObjLit(entries) => {
                code.push(Op::ObjNew);
                for entry in entries {
                    match entry {
                        ObjEntry::KeyValue(key, val) => {
                            code.push(Op::Dup);
                            self.gen_expr(val, code)?;
                            let key_id = self
                                .rt
                                .intern(key)
                                .ok_or_else(|| CompileError("intern failed".into()))?;
                            code.push(Op::PStore(key_id));
                            code.push(Op::Pop); // PStore が返す val を捨て、obj を残す
                        }
                        ObjEntry::Spread(e) => {
                            self.gen_expr(e, code)?; // [obj, src]
                            code.push(Op::ObjSpread); // src の全 props を obj へコピー
                        }
                        ObjEntry::Getter(name, body) => {
                            let enclosing = self.captures.clone();
                            let empty = HashMap::new();
                            let fidx =
                                self.compile_function_anon(&[], None, body, &empty, &enclosing)?;
                            code.push(Op::Dup);
                            code.push(Op::MakeF(fidx));
                            let sid = self
                                .rt
                                .intern(&format!("get:\x01{name}"))
                                .ok_or_else(|| CompileError("intern failed".into()))?;
                            code.push(Op::PStore(sid));
                            code.push(Op::Pop);
                        }
                        ObjEntry::Setter(name, param, body) => {
                            let enclosing = self.captures.clone();
                            let empty = HashMap::new();
                            let params: Vec<String> =
                                if param.is_empty() { vec![] } else { vec![param.clone()] };
                            let fidx = self.compile_function_anon(
                                &params,
                                None,
                                body,
                                &empty,
                                &enclosing,
                            )?;
                            code.push(Op::Dup);
                            code.push(Op::MakeF(fidx));
                            let sid = self
                                .rt
                                .intern(&format!("set:\x01{name}"))
                                .ok_or_else(|| CompileError("intern failed".into()))?;
                            code.push(Op::PStore(sid));
                            code.push(Op::Pop);
                        }
                    }
                }
            }
            Expr::Member { obj, name } => {
                self.gen_expr(obj, code)?;
                let name_id = self
                    .rt
                    .intern(name)
                    .ok_or_else(|| CompileError("intern failed".into()))?;
                code.push(Op::PLoad(name_id));
            }
            Expr::Index { obj, index } => {
                self.gen_expr(obj, code)?;
                self.gen_expr(index, code)?;
                code.push(Op::AGet);
            }
            Expr::IndexAssign { obj, index, rhs } => {
                self.gen_expr(obj, code)?;
                self.gen_expr(index, code)?;
                self.gen_expr(rhs, code)?;
                code.push(Op::ASet);
            }
            Expr::MemberAssign { obj, name, rhs } => {
                self.gen_expr(obj, code)?;
                self.gen_expr(rhs, code)?;
                let name_id = self
                    .rt
                    .intern(name)
                    .ok_or_else(|| CompileError("intern failed".into()))?;
                code.push(Op::PStore(name_id));
            }
            Expr::Ternary { cond, then, else_ } => {
                self.gen_expr(cond, code)?;
                let jmpf_idx = code.len();
                code.push(Op::JmpF(0)); // 偽 → else
                self.gen_expr(then, code)?;
                let jmp_idx = code.len();
                code.push(Op::Jmp(0)); // → end
                let else_pos = code.len();
                code[jmpf_idx] = Op::JmpF(else_pos as u32);
                self.gen_expr(else_, code)?;
                let end_pos = code.len();
                code[jmp_idx] = Op::Jmp(end_pos as u32);
            }
            Expr::Instanceof { lhs, rhs } => {
                self.gen_expr(lhs, code)?;
                self.gen_expr(rhs, code)?;
                code.push(Op::Instanceof);
            }
            Expr::In { key, obj } => {
                self.gen_expr(key, code)?;
                self.gen_expr(obj, code)?;
                code.push(Op::In);
            }
            Expr::Delete { target } => {
                // delete obj.key → 専用命令は delete obj[key] のみなので、
                // メンバーはインデックス形に変換して対応（キーを文字列として渡す）
                match &**target {
                    Expr::Member { obj, name } => {
                        self.gen_expr(obj, code)?;
                        let key_id = self.rt.intern(name).ok_or_else(|| CompileError("intern failed".into()))?;
                        code.push(Op::ConstStr(key_id));
                        code.push(Op::Delete);
                    }
                    Expr::Index { obj, index } => {
                        self.gen_expr(obj, code)?;
                        self.gen_expr(index, code)?;
                        code.push(Op::Delete);
                    }
                    _ => return Err(CompileError("invalid delete target".into())),
                }
            }
            Expr::New { callee, args } => {
                self.gen_expr(callee, code)?;
                for a in args {
                    self.gen_expr(a, code)?;
                }
                if args.len() > u8::MAX as usize {
                    return Err(CompileError("too many arguments".into()));
                }
                code.push(Op::New(args.len() as u8));
            }
            Expr::Regex { pattern, flags } => {
                // 正規表現オブジェクトを生成: パターン文字列 + flags 文字列を intern して
                // NewRegex 命令で Obj::RegExp を生成
                let pat_id = self
                    .rt
                    .intern(pattern)
                    .ok_or_else(|| CompileError("intern failed".into()))?;
                let flags_id = self
                    .rt
                    .intern(flags)
                    .ok_or_else(|| CompileError("intern failed".into()))?;
                code.push(Op::ConstStr(pat_id));
                code.push(Op::NewRegex(flags_id));
            }
            Expr::Spread(_) | Expr::Rest(_) | Expr::Hole => {
                return Err(CompileError("spread/rest/hole outside valid context".into()))
            }
            Expr::CompoundAssign { name, op, rhs } => {
                // x op= y  →  x = x op y（値は新値）
                self.gen_load(name, code)?;
                self.gen_expr(rhs, code)?;
                code.push(self.binop_op(op)?);
                code.push(Op::Dup);
                self.gen_store(name, code)?;
            }
            Expr::MemberCompoundAssign { obj, name, op, rhs } => {
                // obj.name op= rhs → obj.name = obj.name op rhs（obj は 1 回評価）
                let tobj = self.rt.intern("\x01mca_obj").ok_or_else(|| CompileError("intern failed".into()))?;
                let tval = self.rt.intern("\x01mca_val").ok_or_else(|| CompileError("intern failed".into()))?;
                let name_id = self.rt.intern(name).ok_or_else(|| CompileError("intern failed".into()))?;
                self.gen_expr(obj, code)?;
                code.push(Op::GStore(tobj));
                code.push(Op::GLoad(tobj));
                code.push(Op::PLoad(name_id));
                self.gen_expr(rhs, code)?;
                code.push(self.binop_op(op)?);
                code.push(Op::GStore(tval));
                code.push(Op::GLoad(tobj));
                code.push(Op::GLoad(tval));
                code.push(Op::PStore(name_id));
            }
            Expr::IndexCompoundAssign { obj, index, op, rhs } => {
                // obj[idx] op= rhs → obj[idx] = obj[idx] op rhs（obj/index は 1 回評価）
                let tobj = self.rt.intern("\x01ica_obj").ok_or_else(|| CompileError("intern failed".into()))?;
                let tidx = self.rt.intern("\x01ica_idx").ok_or_else(|| CompileError("intern failed".into()))?;
                let tval = self.rt.intern("\x01ica_val").ok_or_else(|| CompileError("intern failed".into()))?;
                self.gen_expr(obj, code)?;
                code.push(Op::GStore(tobj));
                self.gen_expr(index, code)?;
                code.push(Op::GStore(tidx));
                code.push(Op::GLoad(tobj));
                code.push(Op::GLoad(tidx));
                code.push(Op::AGet);
                self.gen_expr(rhs, code)?;
                code.push(self.binop_op(op)?);
                code.push(Op::GStore(tval));
                code.push(Op::GLoad(tobj));
                code.push(Op::GLoad(tidx));
                code.push(Op::GLoad(tval));
                code.push(Op::ASet);
            }
            Expr::IncDec { name, inc, prefix } => {
                if *prefix {
                    // ++x / --x: 値は新値
                    self.gen_load(name, code)?;
                    code.push(Op::ConstI(1));
                    code.push(if *inc { Op::Add } else { Op::Sub });
                    code.push(Op::Dup);
                    self.gen_store(name, code)?;
                } else {
                    // x++ / x--: 値は旧値
                    self.gen_load(name, code)?;
                    code.push(Op::Dup); // 旧値を残す
                    code.push(Op::ConstI(1));
                    code.push(if *inc { Op::Add } else { Op::Sub });
                    self.gen_store(name, code)?;
                }
            }
            Expr::MemberIncDec { obj, name, inc, prefix } => {
                self.gen_member_incdec(obj, name, *inc, *prefix, code)?;
            }
            Expr::IndexIncDec { obj, index, inc, prefix } => {
                self.gen_index_incdec(obj, index, *inc, *prefix, code)?;
            }
            Expr::LogicalAssign { target, op, rhs } => {
                // x ||= y → falsy なら x = y（値は新 x）。&&= は truthy、??= は nullish。
                let jmp = |op: &LogicalOp| match op {
                    LogicalOp::Or => Op::JmpT(0),
                    LogicalOp::And => Op::JmpF(0),
                    LogicalOp::Nullish => Op::JmpNotNullish(0),
                };
                match &**target {
                    Expr::Ident(name) => {
                        self.gen_load(name, code)?; // [x]
                        code.push(Op::Dup); // [x, x]
                        let jmp_idx = code.len();
                        code.push(jmp(op));
                        code.push(Op::Pop); // 条件不成立: x を捨てる
                        self.gen_expr(rhs, code)?; // [y]
                        code.push(Op::Dup); // [y, y]
                        self.gen_store(name, code)?; // [y]
                        let skip = code.len();
                        code[jmp_idx] = match op {
                            LogicalOp::Or => Op::JmpT(skip as u32),
                            LogicalOp::And => Op::JmpF(skip as u32),
                            LogicalOp::Nullish => Op::JmpNotNullish(skip as u32),
                        };
                    }
                    Expr::Member { obj, name } => {
                        // obj.name op= rhs。obj は一時グローバルに退避して評価を 1 回に。
                        let tmp = self
                            .rt
                            .intern("\x01la_obj")
                            .ok_or_else(|| CompileError("intern failed".into()))?;
                        let tmpv = self
                            .rt
                            .intern("\x01la_val")
                            .ok_or_else(|| CompileError("intern failed".into()))?;
                        let name_id = self
                            .rt
                            .intern(name)
                            .ok_or_else(|| CompileError("intern failed".into()))?;
                        self.gen_expr(obj, code)?; // [obj]
                        code.push(Op::GStore(tmp)); // []
                        code.push(Op::GLoad(tmp)); // [obj]
                        code.push(Op::PLoad(name_id)); // [prop]
                        code.push(Op::Dup); // [prop, prop]
                        let jmp_idx = code.len();
                        code.push(jmp(op)); // pop 1。条件成立なら skip（[prop] が値）
                        code.push(Op::Pop); // 条件不成立: prop を捨てる
                        self.gen_expr(rhs, code)?; // [rhs]
                        code.push(Op::GStore(tmpv)); // []
                        code.push(Op::GLoad(tmp)); // [obj]
                        code.push(Op::GLoad(tmpv)); // [obj, rhs]
                        code.push(Op::PStore(name_id)); // [rhs]
                        let skip = code.len();
                        code[jmp_idx] = match op {
                            LogicalOp::Or => Op::JmpT(skip as u32),
                            LogicalOp::And => Op::JmpF(skip as u32),
                            LogicalOp::Nullish => Op::JmpNotNullish(skip as u32),
                        };
                    }
                    _ => return Err(CompileError("invalid logical assignment target".into())),
                }
            }
            Expr::DestructureAssign { pattern, rhs } => {
                // 右辺の値を式の値として残す（JS の分割代入式の値は右辺）。
                self.gen_expr(rhs, code)?; // [rhs]
                code.push(Op::Dup); // [rhs, rhs]
                self.gen_destructure(pattern, code)?; // [rhs]（束縛は 2 個目を消費）
            }
            Expr::SuperCall { parent, args } => {
                // super(args) → this で親コンストラクタを呼ぶ（MCall は this=receiver）
                code.push(Op::This); // [this]
                let parent_id = self
                    .rt
                    .intern(parent)
                    .ok_or_else(|| CompileError("intern failed".into()))?;
                code.push(Op::GLoad(parent_id)); // [this, parent]
                for a in args {
                    self.gen_expr(a, code)?;
                }
                if args.len() > u8::MAX as usize {
                    return Err(CompileError("too many arguments".into()));
                }
                code.push(Op::MCall(args.len() as u8));
            }
            Expr::Yield(operand) => {
                match operand {
                    Some(e) => self.gen_expr(e, code)?,
                    None => code.push(Op::Undef),
                }
                code.push(Op::Yield);
            }
            Expr::Await(operand) => {
                self.gen_expr(operand, code)?;
                code.push(Op::Await);
            }
            Expr::Seq(items) => {
                // 左から順に評価し、最後の値だけ残す（コンマ演算子の値）。
                for (i, e) in items.iter().enumerate() {
                    self.gen_expr(e, code)?;
                    if i + 1 < items.len() {
                        code.push(Op::Pop);
                    }
                }
            }
            Expr::This => {
                code.push(Op::This);
            }
            Expr::FuncExpr { name, params, rest, body } => {
                // 関数式: 関数をコンパイルして MakeF/MakeClosure で生成
                let enclosing_env = self.captures.clone();
                let boxed = compute_boxed(params, body);
                let fidx = self.compile_function_anon(params, rest.as_deref(), body, &boxed, &enclosing_env)?;
                // 名前付き関数式は自分自身を束縛（簡易: グローバルに置く近似は避け、
                // ローカルスコープの自己参照は未対応のため名前は無視）
                let _ = name;
                if boxed.is_empty() {
                    code.push(Op::MakeF(fidx));
                } else {
                    code.push(Op::MakeClosure(fidx));
                }
            }
            Expr::Arrow { params, rest, body } => {
                // アロー関数: 式本体を暗黙 return に包んでコンパイル
                let stmts = vec![Stmt::Return(Some((**body).clone()))];
                let enclosing_env = self.captures.clone();
                let boxed = compute_boxed(params, &stmts);
                let fidx = self.compile_function_anon(params, rest.as_deref(), &stmts, &boxed, &enclosing_env)?;
                if boxed.is_empty() {
                    code.push(Op::MakeF(fidx));
                } else {
                    code.push(Op::MakeClosure(fidx));
                }
            }
        }
        Ok(())
    }

    /// 文をコード生成。
    fn gen_stmt(&mut self, stmt: &Stmt, code: &mut Vec<Op>) -> Result<(), CompileError> {
        match stmt {
            Stmt::Empty => {}
            Stmt::Expr(e) => {
                self.gen_expr(e, code)?;
                // `yield expr;` は Yield 命令が即座に戻る（再開は Yield の次から）。
                // PopV を出すと再開時に空スタックを pop してしまうため省略する。
                if !matches!(e, Expr::Yield(_)) {
                    code.push(Op::PopV); // 式文の値を last_val に保存（C の POPV）
                }
            }
            Stmt::Var { name, init } => {
                match init {
                    Some(e) => self.gen_expr(e, code)?,
                    None => code.push(Op::Undef),
                }
                self.gen_store(name, code)?;
            }
            Stmt::Return(e) => {
                match e {
                    Some(e) => self.gen_expr(e, code)?,
                    None => code.push(Op::Undef),
                }
                // async 関数は戻り値を解決済み Promise で包む
                if self.cur_is_async {
                    code.push(Op::PromiseWrap);
                }
                code.push(Op::Ret);
            }
            Stmt::If { cond, then, else_ } => {
                self.gen_expr(cond, code)?;
                let jmpf_idx = code.len();
                code.push(Op::JmpF(0)); // 仮。falsy なら else へ
                self.gen_stmt(then, code)?;
                let jmp_idx = code.len();
                code.push(Op::Jmp(0)); // 仮。end へ
                let else_pos = code.len();
                code[jmpf_idx] = Op::JmpF(else_pos as u32);
                if let Some(else_) = else_ {
                    self.gen_stmt(else_, code)?;
                }
                let end_pos = code.len();
                code[jmp_idx] = Op::Jmp(end_pos as u32);
            }
            Stmt::Block(stmts) => {
                for s in stmts {
                    self.gen_stmt(s, code)?;
                }
            }
            Stmt::While { cond, body } => {
                let loop_start = code.len();
                self.gen_expr(cond, code)?;
                let jmpf_idx = code.len();
                code.push(Op::JmpF(0)); // 偽 → end
                self.break_patches.push(Vec::new());
                self.continue_patches.push(Vec::new());
                let continue_target = loop_start;
                self.gen_stmt(body, code)?;
                code.push(Op::Jmp(loop_start as u32));
                let end_pos = code.len();
                code[jmpf_idx] = Op::JmpF(end_pos as u32);
                // パッチ適用
                let breaks = self.break_patches.pop().unwrap();
                let continues = self.continue_patches.pop().unwrap();
                for idx in breaks {
                    code[idx] = Op::Jmp(end_pos as u32);
                }
                for idx in continues {
                    code[idx] = Op::Jmp(continue_target as u32);
                }
            }
            Stmt::For { init, cond, step, body } => {
                // init（ループ前）
                match init {
                    Some(ForInit::Var { name, init }) => {
                        match init {
                            Some(e) => self.gen_expr(e, code)?,
                            None => code.push(Op::Undef),
                        }
                        self.gen_store(name, code)?;
                    }
                    Some(ForInit::Expr(e)) => {
                        self.gen_expr(e, code)?;
                        code.push(Op::Pop);
                    }
                    None => {}
                }
                let loop_start = code.len();
                // cond（省略時は常に真）
                if let Some(c) = cond {
                    self.gen_expr(c, code)?;
                } else {
                    code.push(Op::True);
                }
                let jmpf_idx = code.len();
                code.push(Op::JmpF(0)); // 偽 → end
                self.break_patches.push(Vec::new());
                self.continue_patches.push(Vec::new());
                self.gen_stmt(body, code)?;
                // continue のジャンプ先 = step 実行
                let step_target = code.len();
                if let Some(s) = step {
                    self.gen_expr(s, code)?;
                    code.push(Op::Pop);
                }
                code.push(Op::Jmp(loop_start as u32));
                let end_pos = code.len();
                code[jmpf_idx] = Op::JmpF(end_pos as u32);
                let breaks = self.break_patches.pop().unwrap();
                let continues = self.continue_patches.pop().unwrap();
                for idx in breaks {
                    code[idx] = Op::Jmp(end_pos as u32);
                }
                for idx in continues {
                    code[idx] = Op::Jmp(step_target as u32);
                }
            }
            Stmt::ForIn { name, obj, body, is_of } => {
                // for-of: 配列をイテレート / for-in: オブジェクトのキーをイテレート。
                // 対象を一時グローバルに退避し、インデックス i で回す。
                self.gen_expr(obj, code)?;
                let tmp_obj = self
                    .rt
                    .intern("\x01forin_obj")
                    .ok_or_else(|| CompileError("intern failed".into()))?;
                code.push(Op::GStore(tmp_obj));

                // for-in の場合は Object.keys(obj) でキー配列を取得
                if !*is_of {
                    let keys_obj = self
                        .rt
                        .intern("\x01forin_keys")
                        .ok_or_else(|| CompileError("intern failed".into()))?;
                    let obj_id = self.rt.intern("Object").ok_or_else(|| CompileError("intern failed".into()))?;
                    code.push(Op::GLoad(obj_id)); // Object
                    code.push(Op::Dup);
                    let keys_name = self.rt.intern("keys").ok_or_else(|| CompileError("intern failed".into()))?;
                    code.push(Op::PLoad(keys_name)); // Object.keys
                    code.push(Op::GLoad(tmp_obj)); // obj
                    code.push(Op::Call(1)); // Object.keys(obj)
                    code.push(Op::GStore(keys_obj)); // keys = ...
                    // 対象を keys 配列に差し替え
                    let tmp_actual = self
                        .rt
                        .intern("\x01forin_actual")
                        .ok_or_else(|| CompileError("intern failed".into()))?;
                    code.push(Op::GLoad(keys_obj));
                    code.push(Op::GStore(tmp_actual));
                } else {
                    let tmp_actual = self
                        .rt
                        .intern("\x01forin_actual")
                        .ok_or_else(|| CompileError("intern failed".into()))?;
                    code.push(Op::GLoad(tmp_obj));
                    code.push(Op::GStore(tmp_actual));
                }

                let tmp_actual = self
                    .rt
                    .intern("\x01forin_actual")
                    .ok_or_else(|| CompileError("intern failed".into()))?;
                let i_name = "\x01forin_i";
                let i_id = self.rt.intern(i_name).ok_or_else(|| CompileError("intern failed".into()))?;
                code.push(Op::ConstI(0));
                code.push(Op::GStore(i_id));
                let loop_start = code.len();
                // i < actual.length
                code.push(Op::GLoad(i_id));
                code.push(Op::GLoad(tmp_actual));
                code.push(Op::PLoad(self.rt.length_id));
                code.push(Op::Lt);
                let jmpf_idx = code.len();
                code.push(Op::JmpF(0));
                // name = actual[i]
                code.push(Op::GLoad(tmp_actual));
                code.push(Op::GLoad(i_id));
                code.push(Op::AGet);
                self.gen_store(name, code)?;
                self.break_patches.push(Vec::new());
                self.continue_patches.push(Vec::new());
                let continue_target = code.len();
                self.gen_stmt(body, code)?;
                // i = i + 1
                code.push(Op::GLoad(i_id));
                code.push(Op::ConstI(1));
                code.push(Op::Add);
                code.push(Op::GStore(i_id));
                code.push(Op::Jmp(loop_start as u32));
                let end_pos = code.len();
                code[jmpf_idx] = Op::JmpF(end_pos as u32);
                let breaks = self.break_patches.pop().unwrap();
                let continues = self.continue_patches.pop().unwrap();
                for idx in breaks {
                    code[idx] = Op::Jmp(end_pos as u32);
                }
                for idx in continues {
                    code[idx] = Op::Jmp(continue_target as u32);
                }
            }
            Stmt::DoWhile { body, cond } => {
                let loop_start = code.len();
                self.break_patches.push(Vec::new());
                self.continue_patches.push(Vec::new());
                self.gen_stmt(body, code)?;
                // continue のジャンプ先 = 条件評価
                let cond_target = code.len();
                self.gen_expr(cond, code)?;
                let jmpf_idx = code.len();
                code.push(Op::JmpF(0)); // 偽 → end
                code.push(Op::Jmp(loop_start as u32));
                let end_pos = code.len();
                code[jmpf_idx] = Op::JmpF(end_pos as u32);
                let breaks = self.break_patches.pop().unwrap();
                let continues = self.continue_patches.pop().unwrap();
                for idx in breaks {
                    code[idx] = Op::Jmp(end_pos as u32);
                }
                for idx in continues {
                    code[idx] = Op::Jmp(cond_target as u32);
                }
            }
            Stmt::Break(label) => {
                let idx = code.len();
                code.push(Op::Jmp(0)); // プレースホルダ
                match label {
                    Some(l) => {
                        let depth = *self
                            .labels
                            .get(l)
                            .ok_or_else(|| CompileError(format!("undefined label: {l}")))?;
                        let patches = self
                            .break_patches
                            .get_mut(depth)
                            .ok_or_else(|| CompileError("break outside loop".into()))?;
                        patches.push(idx);
                    }
                    None => {
                        let patches = self
                            .break_patches
                            .last_mut()
                            .ok_or_else(|| CompileError("break outside loop".into()))?;
                        patches.push(idx);
                    }
                }
            }
            Stmt::Continue(label) => {
                let idx = code.len();
                code.push(Op::Jmp(0)); // プレースホルダ
                match label {
                    Some(l) => {
                        let depth = *self
                            .labels
                            .get(l)
                            .ok_or_else(|| CompileError(format!("undefined label: {l}")))?;
                        let patches = self
                            .continue_patches
                            .get_mut(depth)
                            .ok_or_else(|| CompileError("continue outside loop".into()))?;
                        patches.push(idx);
                    }
                    None => {
                        let patches = self
                            .continue_patches
                            .last_mut()
                            .ok_or_else(|| CompileError("continue outside loop".into()))?;
                        patches.push(idx);
                    }
                }
            }
            Stmt::Labeled { label, body } => {
                // ラベル付きループ: ループの break/continue patch リストの深さを記録。
                // ループは gen_stmt(body) 内で break_patches.push するため、その push 先の
                // index（= 現在の深さ）をラベルに紐付ける。
                let depth = self.break_patches.len();
                self.labels.insert(label.clone(), depth);
                self.gen_stmt(body, code)?;
                self.labels.remove(label);
            }
            Stmt::FuncDecl { name, params, rest, body, is_gen, is_async } => {
                // ネスト関数宣言: 現在フレームの env を共有するクロージャを生成して束縛
                let enclosing_env = self.captures.clone();
                let fidx = self.compile_nested(name, params, rest.as_deref(), body, &enclosing_env, *is_gen, *is_async)?;
                code.push(Op::MakeClosure(fidx));
                self.gen_store(name, code)?;
            }
            Stmt::Throw(e) => {
                self.gen_expr(e, code)?;
                code.push(Op::Throw);
            }
            Stmt::Destructure { pattern, init } => {
                // 右辺を評価してスタックに積み、パターンに従って各要素を束縛
                self.gen_expr(init, code)?;
                self.gen_destructure(pattern, code)?;
            }
            Stmt::ClassDecl { name, parent, constructor, methods, fields } => {
                // メソッドを先にコンパイル
                let enclosing_env = self.captures.clone();
                let empty_boxed = HashMap::new();
                let mut method_fidxs = Vec::new();
                for (mname, mparams, mrest, mbody) in methods {
                    let fidx = self.compile_function_anon(
                        mparams,
                        mrest.as_deref(),
                        mbody,
                        &empty_boxed,
                        &enclosing_env,
                    )?;
                    method_fidxs.push((mname.clone(), fidx));
                }
                // フィールド初期化を constructor 本体の先頭に前置（`this.name = init`）
                let mut ctor_body = Vec::new();
                for (fname, finit) in fields {
                    ctor_body.push(Stmt::Expr(Expr::MemberAssign {
                        obj: Box::new(Expr::This),
                        name: fname.clone(),
                        rhs: Box::new(finit.clone()),
                    }));
                }
                ctor_body.extend(constructor.2.iter().cloned());
                // constructor をコンパイル
                let ctor_fidx = self.compile_function_anon(
                    &constructor.0,
                    constructor.1.as_deref(),
                    &ctor_body,
                    &empty_boxed,
                    &enclosing_env,
                )?;
                // 実行時コード生成:
                //   MakeF(ctor); Dup; ObjNew  → [ctor, ctor, proto]
                //   各メソッド: Dup; MakeF(m); PStore(name); Pop  → [ctor, ctor, proto]
                //   PStore(prototype)  → ctor.prototype = proto → [ctor, proto]
                //   Pop → [ctor]
                //   gen_store(name) → []
                code.push(Op::MakeF(ctor_fidx));
                code.push(Op::Dup);
                code.push(Op::ObjNew);
                for (mname, mfidx) in &method_fidxs {
                    code.push(Op::Dup); // [ctor, ctor, proto, proto]
                    code.push(Op::MakeF(*mfidx)); // [ctor, ctor, proto, proto, method]
                    let mname_id = self
                        .rt
                        .intern(mname)
                        .ok_or_else(|| CompileError("intern failed".into()))?;
                    code.push(Op::PStore(mname_id)); // proto.mname = method → [ctor, ctor, proto, method]
                    code.push(Op::Pop); // [ctor, ctor, proto]
                }
                // extends: proto の [[Prototype]] を親クラスの prototype に繋ぐ
                if let Some(p) = parent {
                    let p_id = self
                        .rt
                        .intern(p)
                        .ok_or_else(|| CompileError("intern failed".into()))?;
                    code.push(Op::GLoad(p_id)); // [ctor, ctor, proto, parent]
                    code.push(Op::LinkSuper); // [ctor, ctor]
                }
                code.push(Op::SetFnProto); // ctor.prototype = proto（fn_protos 登録）→ [ctor]
                self.gen_store(name, code)?; // []
            }
            Stmt::Try { try_body, catch_param, catch_body } => {
                // TryPush(catch_pc) を発行 → try 本体 → TryPop → Jmp(end)
                // catch_pc: 例外値がスタックに積まれている → catch 変数に束縛 → catch 本体
                // end:
                let try_push_idx = code.len();
                code.push(Op::TryPush(0)); // プレースホルダ
                for s in try_body {
                    self.gen_stmt(s, code)?;
                }
                code.push(Op::TryPop);
                let jmp_idx = code.len();
                code.push(Op::Jmp(0)); // → end
                let catch_pc = code.len();
                code[try_push_idx] = Op::TryPush(catch_pc as u32);
                // catch 変数へ束縛（例外値がスタック先頭）
                if let Some(param) = catch_param {
                    self.gen_store(param, code)?;
                } else {
                    code.push(Op::Pop);
                }
                for s in catch_body {
                    self.gen_stmt(s, code)?;
                }
                let end_pos = code.len();
                code[jmp_idx] = Op::Jmp(end_pos as u32);
            }
            Stmt::Import { name, spec: _ } => {
                // 簡易近似: import は no-op（名前束縛はグローバルに委ねる）。
                // モジュール解決は未対応。name があれば undefined で初期化する。
                if let Some(n) = name {
                    // グローバルに undefined を束縛（未解決参照を防ぐ）
                    let id = self.rt.intern(n).ok_or_else(|| CompileError("intern failed".into()))?;
                    if self.rt.global_get(id).is_none() {
                        code.push(Op::Undef);
                        code.push(Op::GStore(id));
                    }
                }
            }
            Stmt::Export { name, value } => {
                // 簡易近似: export は値式をグローバルに束縛する（モジュール namespace は未対応）
                self.gen_expr(value, code)?;
                let id = self.rt.intern(name).ok_or_else(|| CompileError("intern failed".into()))?;
                code.push(Op::GStore(id));
            }
            Stmt::Switch { disc, cases } => {
                // switch を if/else-if チェーンにコンパイル（=== 比較、break 前提で
                // フォールスルー無し）。disc はグローバル一時に退避して各 case で比較。
                self.gen_expr(disc, code)?;
                let tmp_id = self
                    .rt
                    .intern("\x01switch_disc")
                    .ok_or_else(|| CompileError("intern failed".into()))?;
                code.push(Op::GStore(tmp_id));

                // パス 1: 値 case の比較ヘッダ（GLoad; val; Seq; JmpF）を生成。
                // 値 case の「case 全体における index」を記録して default を正しく扱う。
                let mut jmpf_patches: Vec<usize> = Vec::new();
                let mut value_case_idx: Vec<usize> = Vec::new();
                for (idx, (case_val, _)) in cases.iter().enumerate() {
                    if let Some(val_expr) = case_val {
                        code.push(Op::GLoad(tmp_id));
                        self.gen_expr(val_expr, code)?;
                        code.push(Op::Seq);
                        jmpf_patches.push(code.len());
                        code.push(Op::JmpF(0)); // プレースホルダ
                        value_case_idx.push(idx);
                    }
                }

                // パス 2: 各 case 本体（開始位置を記録）+ break 相当の Jmp(end)。
                // switch 内の `break` もこの end へ飛ばすため break_patches を積む。
                let mut case_starts = vec![0usize; cases.len()];
                let mut end_patches: Vec<usize> = Vec::new();
                self.break_patches.push(Vec::new());
                for (idx, (_, body)) in cases.iter().enumerate() {
                    case_starts[idx] = code.len();
                    for s in body {
                        self.gen_stmt(s, code)?;
                    }
                    end_patches.push(code.len());
                    code.push(Op::Jmp(0)); // フォールスルー防止（break 相当）
                }
                let end_pos = code.len();
                for p in &end_patches {
                    code[*p] = Op::Jmp(end_pos as u32);
                }
                let breaks = self.break_patches.pop().unwrap();
                for b in &breaks {
                    code[*b] = Op::Jmp(end_pos as u32);
                }
                // JmpF のターゲット = 「次の case」の開始（無ければ end）
                for (j, patch) in jmpf_patches.iter().enumerate() {
                    let case_idx = value_case_idx[j];
                    let target = case_starts.get(case_idx + 1).copied().unwrap_or(end_pos);
                    code[*patch] = Op::JmpF(target as u32);
                }
            }
        }
        Ok(())
    }

    /// 分割代入パターンをコード生成（スタックトップの値を pattern に束縛して消費）。
    fn gen_destructure(
        &mut self,
        pattern: &crate::parser::Pattern,
        code: &mut Vec<Op>,
    ) -> Result<(), CompileError> {
        match pattern {
            crate::parser::Pattern::Ident(name) => {
                self.gen_store(name, code)?;
            }
            crate::parser::Pattern::Arr(items) => {
                // スタックトップは配列。各要素を取り出して束縛。
                for (i, item) in items.iter().enumerate() {
                    match item {
                        crate::parser::Pattern::Rest(name) => {
                            // 残り [i..n) を rest 配列に束縛
                            code.push(Op::Dup); // [arr, arr]
                            code.push(Op::ConstI(i as i32)); // [arr, arr, i]
                            code.push(Op::ArrRest); // [arr, rest]
                            self.gen_store(name, code)?; // [arr]
                            break;
                        }
                        crate::parser::Pattern::Hole => {
                            // 空き要素: 束縛しない（インデックスは進む）
                        }
                        _ => {
                            code.push(Op::Dup); // [arr, arr]
                            code.push(Op::ConstI(i as i32)); // [arr, arr, i]
                            code.push(Op::AGet); // [arr, elem]
                            self.gen_destructure(item, code)?; // [arr]
                        }
                    }
                }
                code.push(Op::Pop); // arr を消費
            }
            crate::parser::Pattern::Obj(items) => {
                // スタックトップはオブジェクト。各キーを取り出して束縛。rest は残り。
                let mut rest_name: Option<String> = None;
                let mut explicit_keys: Vec<String> = Vec::new();
                for (key, item) in items {
                    match item {
                        crate::parser::Pattern::ObjRest(name) => {
                            rest_name = Some(name.clone());
                        }
                        _ => {
                            explicit_keys.push(key.clone());
                            code.push(Op::Dup); // [obj, obj]
                            let key_id = self
                                .rt
                                .intern(key)
                                .ok_or_else(|| CompileError("intern failed".into()))?;
                            code.push(Op::PLoad(key_id)); // [obj, val]
                            self.gen_destructure(item, code)?; // [obj]
                        }
                    }
                }
                if let Some(rname) = rest_name {
                    // 残りのプロパティをコピーした新 OBJ を作り、明示キーを削除
                    code.push(Op::Dup); // [obj, obj]
                    code.push(Op::ObjRest); // [obj, copy]
                    for key in &explicit_keys {
                        code.push(Op::Dup); // [obj, copy, copy]
                        let key_id = self
                            .rt
                            .intern(key)
                            .ok_or_else(|| CompileError("intern failed".into()))?;
                        code.push(Op::ConstStr(key_id)); // [obj, copy, copy, key]
                        code.push(Op::Delete); // [obj, copy, bool]
                        code.push(Op::Pop); // [obj, copy]
                    }
                    self.gen_store(&rname, code)?; // [obj]
                }
                code.push(Op::Pop); // obj を消費
            }
            crate::parser::Pattern::Rest(_) | crate::parser::Pattern::ObjRest(_) => {
                return Err(CompileError("rest outside pattern".into()))
            }
            crate::parser::Pattern::Hole => {}
        }
        Ok(())
    }

    /// 変数を読み出す（捕捉 env → ローカル → グローバルの順で解決）。
    /// 捕捉をローカルより優先するのは、box 化ローカルが locals に残るため。
    fn gen_load(&mut self, name: &str, code: &mut Vec<Op>) -> Result<(), CompileError> {
        if let Some(idx) = self.captures.get(name) {
            code.push(Op::CeLoad(*idx));
        } else if let Some(slot) = self.locals.get(name) {
            code.push(Op::LLoad(*slot));
        } else {
            let id = self.rt.intern(name).ok_or_else(|| CompileError("intern failed".into()))?;
            code.push(Op::GLoad(id));
        }
        Ok(())
    }

    /// 変数へ書き込む（捕捉 env → ローカル → グローバルの順で解決）。
    fn gen_store(&mut self, name: &str, code: &mut Vec<Op>) -> Result<(), CompileError> {
        if let Some(idx) = self.captures.get(name) {
            code.push(Op::CeStore(*idx));
        } else if let Some(slot) = self.locals.get(name) {
            code.push(Op::LStore(*slot));
        } else {
            let id = self.rt.intern(name).ok_or_else(|| CompileError("intern failed".into()))?;
            code.push(Op::GStore(id));
        }
        Ok(())
    }

    /// 複合代入の演算子 `BinOp` → 対応する `Op`（対応外は Err）。
    fn binop_op(&self, op: &BinOp) -> Result<Op, CompileError> {
        Ok(match op {
            BinOp::Add => Op::Add,
            BinOp::Sub => Op::Sub,
            BinOp::Mul => Op::Mul,
            BinOp::Div => Op::Div,
            BinOp::Mod => Op::Mod,
            BinOp::BShl => Op::BShl,
            BinOp::BShr => Op::BShr,
            BinOp::BUShr => Op::BUShr,
            BinOp::BAnd => Op::BAnd,
            BinOp::BOr => Op::BOr,
            BinOp::BXor => Op::BXor,
            _ => return Err(CompileError("invalid compound assignment operator".into())),
        })
    }

    /// `obj.name++` / `--obj.name` のコード生成（`obj` は 1 回評価）。
    fn gen_member_incdec(
        &mut self,
        obj: &Expr,
        name: &str,
        inc: bool,
        prefix: bool,
        code: &mut Vec<Op>,
    ) -> Result<(), CompileError> {
        let tobj = self.rt.intern("\x01mio_obj").ok_or_else(|| CompileError("intern failed".into()))?;
        let tval = self.rt.intern("\x01mio_val").ok_or_else(|| CompileError("intern failed".into()))?;
        let told = self.rt.intern("\x01mio_old").ok_or_else(|| CompileError("intern failed".into()))?;
        let name_id = self.rt.intern(name).ok_or_else(|| CompileError("intern failed".into()))?;
        let op = if inc { Op::Add } else { Op::Sub };
        self.gen_expr(obj, code)?;
        code.push(Op::GStore(tobj));
        if prefix {
            code.push(Op::GLoad(tobj));
            code.push(Op::PLoad(name_id));
            code.push(Op::ConstI(1));
            code.push(op);
            code.push(Op::GStore(tval));
            code.push(Op::GLoad(tobj));
            code.push(Op::GLoad(tval));
            code.push(Op::PStore(name_id));
        } else {
            code.push(Op::GLoad(tobj));
            code.push(Op::PLoad(name_id));
            code.push(Op::Dup);
            code.push(Op::GStore(told));
            code.push(Op::ConstI(1));
            code.push(op);
            code.push(Op::GStore(tval));
            code.push(Op::GLoad(tobj));
            code.push(Op::GLoad(tval));
            code.push(Op::PStore(name_id));
            code.push(Op::Pop);
            code.push(Op::GLoad(told));
        }
        Ok(())
    }

    /// `obj[i]++` / `--obj[i]` のコード生成（`obj`/`index` は 1 回評価）。
    #[allow(clippy::too_many_arguments)]
    fn gen_index_incdec(
        &mut self,
        obj: &Expr,
        index: &Expr,
        inc: bool,
        prefix: bool,
        code: &mut Vec<Op>,
    ) -> Result<(), CompileError> {
        let tobj = self.rt.intern("\x01iio_obj").ok_or_else(|| CompileError("intern failed".into()))?;
        let tidx = self.rt.intern("\x01iio_idx").ok_or_else(|| CompileError("intern failed".into()))?;
        let tval = self.rt.intern("\x01iio_val").ok_or_else(|| CompileError("intern failed".into()))?;
        let told = self.rt.intern("\x01iio_old").ok_or_else(|| CompileError("intern failed".into()))?;
        let op = if inc { Op::Add } else { Op::Sub };
        self.gen_expr(obj, code)?;
        code.push(Op::GStore(tobj));
        self.gen_expr(index, code)?;
        code.push(Op::GStore(tidx));
        if prefix {
            code.push(Op::GLoad(tobj));
            code.push(Op::GLoad(tidx));
            code.push(Op::AGet);
            code.push(Op::ConstI(1));
            code.push(op);
            code.push(Op::GStore(tval));
            code.push(Op::GLoad(tobj));
            code.push(Op::GLoad(tidx));
            code.push(Op::GLoad(tval));
            code.push(Op::ASet);
        } else {
            code.push(Op::GLoad(tobj));
            code.push(Op::GLoad(tidx));
            code.push(Op::AGet);
            code.push(Op::Dup);
            code.push(Op::GStore(told));
            code.push(Op::ConstI(1));
            code.push(op);
            code.push(Op::GStore(tval));
            code.push(Op::GLoad(tobj));
            code.push(Op::GLoad(tidx));
            code.push(Op::GLoad(tval));
            code.push(Op::ASet);
            code.push(Op::Pop);
            code.push(Op::GLoad(told));
        }
        Ok(())
    }
}

/// 関数本体（ブロック）内の `var`/`let`/`const` 宣言名を収集してローカルスロットに割り当てる。
/// ネスト関数の中身は走査しない（ネスト関数は未対応）。
#[allow(clippy::collapsible_match)]
fn collect_vars(stmts: &[Stmt], locals: &mut HashMap<String, u32>) {
    for stmt in stmts {
        match stmt {
            Stmt::Var { name, .. } => {
                if !locals.contains_key(name) {
                    locals.insert(name.clone(), locals.len() as u32);
                }
            }
            Stmt::FuncDecl { name, .. } => {
                // ネスト関数宣言の名前も外側関数のローカル（クロージャ束縛先）
                if !locals.contains_key(name) {
                    locals.insert(name.clone(), locals.len() as u32);
                }
            }
            Stmt::Block(inner) => collect_vars(inner, locals),
            Stmt::Labeled { body, .. } => collect_vars(std::slice::from_ref(body), locals),
            Stmt::If { then, else_, .. } => {
                collect_vars(std::slice::from_ref(then), locals);
                if let Some(e) = else_ {
                    collect_vars(std::slice::from_ref(e), locals);
                }
            }
            Stmt::While { body, .. } => collect_vars(std::slice::from_ref(body), locals),
            Stmt::For { init, body, .. } => {
                if let Some(ForInit::Var { name, .. }) = init {
                    if !locals.contains_key(name) {
                        locals.insert(name.clone(), locals.len() as u32);
                    }
                }
                collect_vars(std::slice::from_ref(body), locals);
            }
            Stmt::DoWhile { body, .. } => collect_vars(std::slice::from_ref(body), locals),
            Stmt::ForIn { name, body, .. } => {
                if !locals.contains_key(name) {
                    locals.insert(name.clone(), locals.len() as u32);
                }
                collect_vars(std::slice::from_ref(body), locals);
            }
            Stmt::Switch { cases, .. } => {
                for (_, body) in cases {
                    collect_vars(body, locals);
                }
            }
            Stmt::Try { try_body, catch_param, catch_body } => {
                collect_vars(try_body, locals);
                if let Some(p) = catch_param {
                    if !locals.contains_key(p) {
                        locals.insert(p.clone(), locals.len() as u32);
                    }
                }
                collect_vars(catch_body, locals);
            }
            Stmt::Throw(_) => {}
            Stmt::Destructure { pattern, .. } => {
                collect_pattern_vars(pattern, locals);
            }
            Stmt::ClassDecl { name, .. } => {
                if !locals.contains_key(name) {
                    locals.insert(name.clone(), locals.len() as u32);
                }
            }
            Stmt::Export { name, .. } => {
                if !locals.contains_key(name) {
                    locals.insert(name.clone(), locals.len() as u32);
                }
            }
            Stmt::Import { name, .. } => {
                if let Some(n) = name {
                    if !locals.contains_key(n) {
                        locals.insert(n.clone(), locals.len() as u32);
                    }
                }
            }
            _ => {}
        }
    }
}

/// 分割代入パターンから参照される識別子名を収集（自由変数解析用）。
fn collect_pattern_refs(pattern: &crate::parser::Pattern, out: &mut std::collections::HashSet<String>) {
    match pattern {
        crate::parser::Pattern::Ident(name)
        | crate::parser::Pattern::Rest(name)
        | crate::parser::Pattern::ObjRest(name) => {
            out.insert(name.clone());
        }
        crate::parser::Pattern::Arr(items) => {
            for item in items {
                collect_pattern_refs(item, out);
            }
        }
        crate::parser::Pattern::Obj(items) => {
            for (_, item) in items {
                collect_pattern_refs(item, out);
            }
        }
        crate::parser::Pattern::Hole => {}
    }
}

/// 分割代入パターンから変数名を収集。
fn collect_pattern_vars(pattern: &crate::parser::Pattern, locals: &mut HashMap<String, u32>) {
    match pattern {
        crate::parser::Pattern::Ident(name)
        | crate::parser::Pattern::Rest(name)
        | crate::parser::Pattern::ObjRest(name) => {
            if !locals.contains_key(name) {
                locals.insert(name.clone(), locals.len() as u32);
            }
        }
        crate::parser::Pattern::Arr(items) => {
            for item in items {
                collect_pattern_vars(item, locals);
            }
        }
        crate::parser::Pattern::Obj(items) => {
            for (_, item) in items {
                collect_pattern_vars(item, locals);
            }
        }
        crate::parser::Pattern::Hole => {}
    }
}

/// 文列から参照される識別子名を収集する（クロージャの自由変数解析用）。
fn collect_refs(stmts: &[Stmt], out: &mut std::collections::HashSet<String>) {
    for stmt in stmts {
        match stmt {
            Stmt::Expr(e) => collect_expr_refs(e, out),
            Stmt::Var { init, .. } => {
                if let Some(e) = init {
                    collect_expr_refs(e, out);
                }
            }
            Stmt::Return(e) => {
                if let Some(e) = e {
                    collect_expr_refs(e, out);
                }
            }
            Stmt::If { cond, then, else_ } => {
                collect_expr_refs(cond, out);
                collect_refs(std::slice::from_ref(then), out);
                if let Some(e) = else_ {
                    collect_refs(std::slice::from_ref(e), out);
                }
            }
            Stmt::While { cond, body } => {
                collect_expr_refs(cond, out);
                collect_refs(std::slice::from_ref(body), out);
            }
            Stmt::For { init, cond, step, body } => {
                if let Some(ForInit::Expr(e)) = init {
                    collect_expr_refs(e, out);
                }
                if let Some(ForInit::Var { init: Some(e), .. }) = init {
                    collect_expr_refs(e, out);
                }
                if let Some(c) = cond {
                    collect_expr_refs(c, out);
                }
                if let Some(s) = step {
                    collect_expr_refs(s, out);
                }
                collect_refs(std::slice::from_ref(body), out);
            }
            Stmt::DoWhile { body, cond } => {
                collect_refs(std::slice::from_ref(body), out);
                collect_expr_refs(cond, out);
            }
            Stmt::ForIn { obj, body, .. } => {
                collect_expr_refs(obj, out);
                collect_refs(std::slice::from_ref(body), out);
            }
            Stmt::Switch { disc, cases } => {
                collect_expr_refs(disc, out);
                for (case_val, body) in cases {
                    if let Some(v) = case_val {
                        collect_expr_refs(v, out);
                    }
                    collect_refs(body, out);
                }
            }
            Stmt::Throw(e) => collect_expr_refs(e, out),
            Stmt::Try { try_body, catch_body, .. } => {
                collect_refs(try_body, out);
                collect_refs(catch_body, out);
            }
            Stmt::Destructure { init, .. } => {
                collect_expr_refs(init, out);
            }
            Stmt::ClassDecl { .. } => {
                // メソッド内の自由変数は compile_function_anon が別途解析するため
                // ここでは何もしない（クラス名は locals で解決される）。
            }
            Stmt::Export { value, .. } => {
                collect_expr_refs(value, out);
            }
            Stmt::Import { .. } => {}
            Stmt::Block(inner) => collect_refs(inner, out),
            Stmt::Labeled { body, .. } => collect_refs(std::slice::from_ref(body), out),
            // ネスト関数宣言の中身は、その関数自身の自由変数として別途解析されるため
            // ここでは名前（束縛先）だけを参照扱いしない（locals で解決される）。
            Stmt::FuncDecl { .. } | Stmt::Empty | Stmt::Break(_) | Stmt::Continue(_) => {}
        }
    }
}

/// 式から参照される識別子名を収集する。
fn collect_expr_refs(expr: &Expr, out: &mut std::collections::HashSet<String>) {
    match expr {
        Expr::Ident(name) => {
            out.insert(name.clone());
        }
        Expr::Assign { name, rhs } => {
            out.insert(name.clone());
            collect_expr_refs(rhs, out);
        }
        Expr::CompoundAssign { name, rhs, .. } => {
            out.insert(name.clone());
            collect_expr_refs(rhs, out);
        }
        Expr::MemberCompoundAssign { obj, rhs, .. } => {
            collect_expr_refs(obj, out);
            collect_expr_refs(rhs, out);
        }
        Expr::IndexCompoundAssign { obj, index, rhs, .. } => {
            collect_expr_refs(obj, out);
            collect_expr_refs(index, out);
            collect_expr_refs(rhs, out);
        }
        Expr::IncDec { name, .. } => {
            out.insert(name.clone());
        }
        Expr::MemberIncDec { obj, .. } => collect_expr_refs(obj, out),
        Expr::IndexIncDec { obj, index, .. } => {
            collect_expr_refs(obj, out);
            collect_expr_refs(index, out);
        }
        Expr::Unary { operand, .. } => collect_expr_refs(operand, out),
        Expr::Bin { lhs, rhs, .. } => {
            collect_expr_refs(lhs, out);
            collect_expr_refs(rhs, out);
        }
        Expr::Call { callee, args } => {
            collect_expr_refs(callee, out);
            for a in args {
                collect_expr_refs(a, out);
            }
        }
        Expr::Arr(items) => {
            for i in items {
                collect_expr_refs(i, out);
            }
        }
        Expr::ObjLit(entries) => {
            for e in entries {
                match e {
                    crate::parser::ObjEntry::KeyValue(_, v) => collect_expr_refs(v, out),
                    crate::parser::ObjEntry::Spread(v) => collect_expr_refs(v, out),
                    crate::parser::ObjEntry::Getter(_, body) => collect_refs(body, out),
                    crate::parser::ObjEntry::Setter(_, _, body) => collect_refs(body, out),
                }
            }
        }
        Expr::LogicalAssign { target, rhs, .. } => {
            collect_expr_refs(target, out);
            collect_expr_refs(rhs, out);
        }
        Expr::DestructureAssign { pattern, rhs } => {
            collect_expr_refs(rhs, out);
            collect_pattern_refs(pattern, out);
        }
        Expr::Member { obj, .. } => collect_expr_refs(obj, out),
        Expr::Index { obj, index } => {
            collect_expr_refs(obj, out);
            collect_expr_refs(index, out);
        }
        Expr::IndexAssign { obj, index, rhs } => {
            collect_expr_refs(obj, out);
            collect_expr_refs(index, out);
            collect_expr_refs(rhs, out);
        }
        Expr::MemberAssign { obj, rhs, .. } => {
            collect_expr_refs(obj, out);
            collect_expr_refs(rhs, out);
        }
        Expr::Ternary { cond, then, else_ } => {
            collect_expr_refs(cond, out);
            collect_expr_refs(then, out);
            collect_expr_refs(else_, out);
        }
        Expr::FuncExpr { body, .. } => {
            collect_refs(body, out);
        }
        Expr::Arrow { body, .. } => {
            collect_expr_refs(body, out);
        }
        Expr::Instanceof { lhs, rhs } => {
            collect_expr_refs(lhs, out);
            collect_expr_refs(rhs, out);
        }
        Expr::In { key, obj } => {
            collect_expr_refs(key, out);
            collect_expr_refs(obj, out);
        }
        Expr::Delete { target } => {
            collect_expr_refs(target, out);
        }
        Expr::Spread(e) => collect_expr_refs(e, out),
        Expr::SuperCall { args, .. } => {
            // parent はグローバル名（GLoad で解決）。引数のみ収集。
            for a in args {
                collect_expr_refs(a, out);
            }
        }
        Expr::Yield(operand) => {
            if let Some(e) = operand {
                collect_expr_refs(e, out);
            }
        }
        Expr::Await(operand) => collect_expr_refs(operand, out),
        Expr::Seq(items) => {
            for e in items {
                collect_expr_refs(e, out);
            }
        }
        Expr::New { callee, args } => {
            collect_expr_refs(callee, out);
            for a in args {
                collect_expr_refs(a, out);
            }
        }
        Expr::Rest(_) | Expr::Hole => {}
        Expr::Regex { .. } => {}
        Expr::This
        | Expr::Num(_)
        | Expr::Str(_)
        | Expr::Bool(_)
        | Expr::Null
        | Expr::Undef => {}
    }
}

/// 制御フローを跨いで文列から直接のネスト関数宣言を収集する（ネスト関数の中身は降りない）。
fn collect_nested_funcs(
    stmts: &[Stmt],
    out: &mut Vec<(String, Vec<String>, Vec<Stmt>)>,
) {
    for stmt in stmts {
        match stmt {
            Stmt::FuncDecl { name, params, body, .. } => {
                out.push((name.clone(), params.clone(), body.clone()));
            }
            Stmt::Block(inner) => collect_nested_funcs(inner, out),
            Stmt::Labeled { body, .. } => collect_nested_funcs(std::slice::from_ref(body), out),
            Stmt::If { then, else_, .. } => {
                collect_nested_funcs(std::slice::from_ref(then), out);
                if let Some(e) = else_ {
                    collect_nested_funcs(std::slice::from_ref(e), out);
                }
            }
            Stmt::While { body, .. } => collect_nested_funcs(std::slice::from_ref(body), out),
            Stmt::For { body, .. } => collect_nested_funcs(std::slice::from_ref(body), out),
            Stmt::DoWhile { body, .. } => collect_nested_funcs(std::slice::from_ref(body), out),
            _ => {}
        }
    }
}

/// 関数の自由変数（参照されるが自ローカルでない名前）を列挙する。
fn free_vars(params: &[String], body: &[Stmt]) -> Vec<String> {
    let mut locals = std::collections::HashSet::new();
    for p in params {
        locals.insert(p.clone());
    }
    let mut local_map = HashMap::new();
    for l in &locals {
        local_map.insert(l.clone(), 0u32);
    }
    collect_vars(body, &mut local_map);
    let mut refs = std::collections::HashSet::new();
    collect_refs(body, &mut refs);
    let mut free = Vec::new();
    let mut seen = std::collections::HashSet::new();
    for r in refs {
        if local_map.contains_key(&r) {
            continue;
        }
        if !seen.contains(&r) {
            seen.insert(r.clone());
            free.push(r);
        }
    }
    free
}

/// ネスト関数に捕捉される「自ローカル」の集合を算出（共有セルとして env に box 化）。
/// 戻り値は name → env index（出現順）。
fn compute_boxed(params: &[String], body: &[Stmt]) -> HashMap<String, u32> {
    let mut locals = HashMap::new();
    for p in params {
        locals.insert(p.clone(), locals.len() as u32);
    }
    collect_vars(body, &mut locals);

    let mut nested = Vec::new();
    collect_nested_funcs(body, &mut nested);

    let mut boxed = HashMap::new();
    for (_name, nparams, nbody) in &nested {
        let free = free_vars(nparams, nbody);
        for f in free {
            if locals.contains_key(&f) && !boxed.contains_key(&f) {
                boxed.insert(f.clone(), boxed.len() as u32);
            }
        }
    }
    boxed
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bytecode::Runtime;

    fn run_src(src: &str) -> Result<crate::AklVal, CompileError> {
        let program = crate::parser::Parser::new(src)
            .parse_program()
            .map_err(|e| CompileError(e.0))?;
        let mut rt = Runtime::new();
        let fidx = compile(&mut rt, &program)?;
        rt.run(fidx, &[]).map_err(|e| CompileError(format!("{e:?}")))
    }

    #[test]
    fn arithmetic() {
        assert_eq!(run_src("1 + 2 * 3;").unwrap(), crate::AklVal::mk_int(7));
        assert_eq!(run_src("(1 + 2) * 3;").unwrap(), crate::AklVal::mk_int(9));
        assert_eq!(run_src("7 / 2;").unwrap(), crate::AklVal::from_f64(3.5));
        assert_eq!(run_src("7 % 2;").unwrap(), crate::AklVal::mk_int(1));
    }

    #[test]
    fn variables() {
        assert_eq!(run_src("var x = 10; x + 5;").unwrap(), crate::AklVal::mk_int(15));
        assert_eq!(run_src("var x = 1; x = 2; x;").unwrap(), crate::AklVal::mk_int(2));
    }

    #[test]
    fn fib_via_source() {
        let src = "
            function fib(n) {
                if (n < 2) { return n; }
                return fib(n - 1) + fib(n - 2);
            }
            fib(10);
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(55));
    }

    #[test]
    fn while_loop_sum() {
        let src = "
            var s = 0;
            var i = 0;
            while (i < 10) { s = s + i; i = i + 1; }
            s;
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(45));
    }

    #[test]
    fn string_concat() {
        // 文字列連結の結果が "ab" であることを内容で確認（ROPE 連結。intern id は非決定的）
        let program = crate::parser::Parser::new("\"a\" + \"b\";").parse_program().unwrap();
        let mut rt = Runtime::new();
        let fidx = compile(&mut rt, &program).unwrap();
        let v = rt.run(fidx, &[]).unwrap();
        let flattened = rt.flatten_str(v);
        assert_eq!(flattened, "ab");
    }

    #[test]
    fn comparisons_and_logic() {
        assert_eq!(run_src("1 < 2;").unwrap(), crate::AklVal::TRUE);
        assert_eq!(run_src("2 <= 1;").unwrap(), crate::AklVal::FALSE);
        assert_eq!(run_src("1 === 1.0;").unwrap(), crate::AklVal::TRUE);
        assert_eq!(run_src("true && false;").unwrap(), crate::AklVal::FALSE);
        assert_eq!(run_src("true || false;").unwrap(), crate::AklVal::TRUE);
    }

    #[test]
    fn typeof_unary() {
        let program = crate::parser::Parser::new("typeof 5;").parse_program().unwrap();
        let mut rt = Runtime::new();
        let fidx = compile(&mut rt, &program).unwrap();
        let v = rt.run(fidx, &[]).unwrap();
        match rt.heap.get(v.get_obj()) {
            Some(crate::obj::Obj::Str(s)) => assert_eq!(&**s, "number"),
            other => panic!("expected string, got {other:?}"),
        }
    }

    #[test]
    fn if_else() {
        assert_eq!(run_src("if (1 < 2) { 10; } else { 20; }").unwrap(), crate::AklVal::mk_int(10));
        assert_eq!(run_src("if (1 > 2) { 10; } else { 20; }").unwrap(), crate::AklVal::mk_int(20));
    }

    #[test]
    fn nested_call() {
        let src = "
            function add(a, b) { return a + b; }
            function double(x) { return add(x, x); }
            double(21);
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(42));
    }

    #[test]
    fn array_literal_and_index() {
        assert_eq!(run_src("[10, 20, 30][1];").unwrap(), crate::AklVal::mk_int(20));
        assert_eq!(
            run_src("var a = [1, 2, 3]; a[2];").unwrap(),
            crate::AklVal::mk_int(3)
        );
        // 要素代入
        assert_eq!(
            run_src("var a = [1, 2, 3]; a[0] = 9; a[0];").unwrap(),
            crate::AklVal::mk_int(9)
        );
    }

    #[test]
    fn object_literal_and_member() {
        assert_eq!(run_src("({x: 42}).x;").unwrap(), crate::AklVal::mk_int(42));
        assert_eq!(
            run_src("var o = {a: 1, b: 2}; o.b;").unwrap(),
            crate::AklVal::mk_int(2)
        );
        // メンバー代入
        assert_eq!(
            run_src("var o = {a: 1}; o.a = 7; o.a;").unwrap(),
            crate::AklVal::mk_int(7)
        );
    }

    #[test]
    fn object_method_call() {
        // 関数を値としてオブジェクトに持たせ、メンバー経由で呼ぶ
        let src = "
            function add(a, b) { return a + b; }
            var obj = { op: add };
            obj.op(3, 4);
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(7));
    }

    #[test]
    fn for_loop() {
        let src = "
            var s = 0;
            for (var i = 0; i < 10; i = i + 1) { s = s + i; }
            s;
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(45));
    }

    #[test]
    fn for_loop_with_break() {
        let src = "
            var s = 0;
            for (var i = 0; i < 10; i = i + 1) { if (i === 5) { break; } s = s + i; }
            s;
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(10)); // 0+1+2+3+4
    }

    #[test]
    fn for_loop_with_continue() {
        let src = "
            var s = 0;
            for (var i = 0; i < 5; i = i + 1) { if (i === 2) { continue; } s = s + i; }
            s;
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(8)); // 0+1+3+4
    }

    #[test]
    fn do_while_loop() {
        let src = "
            var i = 0;
            var s = 0;
            do { s = s + i; i = i + 1; } while (i < 5);
            s;
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(10)); // 0+1+2+3+4
    }

    #[test]
    fn ternary_expr() {
        assert_eq!(run_src("1 < 2 ? 10 : 20;").unwrap(), crate::AklVal::mk_int(10));
        assert_eq!(run_src("1 > 2 ? 10 : 20;").unwrap(), crate::AklVal::mk_int(20));
    }

    #[test]
    fn compound_assign() {
        assert_eq!(
            run_src("var x = 10; x += 5; x;").unwrap(),
            crate::AklVal::mk_int(15)
        );
        assert_eq!(
            run_src("var x = 10; x -= 3; x;").unwrap(),
            crate::AklVal::mk_int(7)
        );
        assert_eq!(
            run_src("var x = 2; x *= 6; x;").unwrap(),
            crate::AklVal::mk_int(12)
        );
        assert_eq!(
            run_src("var x = 10; x /= 4; x;").unwrap(),
            crate::AklVal::from_f64(2.5)
        );
        assert_eq!(
            run_src("var x = 10; x %= 3; x;").unwrap(),
            crate::AklVal::mk_int(1)
        );
    }

    #[test]
    fn inc_dec() {
        assert_eq!(run_src("var x = 5; ++x;").unwrap(), crate::AklVal::mk_int(6));
        assert_eq!(run_src("var x = 5; x++;").unwrap(), crate::AklVal::mk_int(5));
        assert_eq!(run_src("var x = 5; x++; x;").unwrap(), crate::AklVal::mk_int(6));
        assert_eq!(run_src("var x = 5; --x;").unwrap(), crate::AklVal::mk_int(4));
        assert_eq!(run_src("var x = 5; x--; x;").unwrap(), crate::AklVal::mk_int(4));
    }

    #[test]
    fn break_continue_outside_loop_errors() {
        // break / continue はループ外でエラー（コンパイル時）
        let src = "break;";
        let program = crate::parser::Parser::new(src).parse_program().unwrap();
        let mut rt = Runtime::new();
        assert!(compile(&mut rt, &program).is_err());
    }

    #[test]
    fn closure_captures_enclosing_local() {
        let src = "
            function outer() {
                var x = 10;
                function inner() { return x; }
                return inner();
            }
            outer();
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(10));
    }

    #[test]
    fn closure_counter() {
        let src = "
            function makeCounter() {
                var count = 0;
                function inc() { count = count + 1; return count; }
                return inc;
            }
            var c = makeCounter();
            c();
            c();
            c();
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(3));
    }

    #[test]
    fn closure_mutates_captured() {
        // 捕捉変数への書き込みが外側にも反映される（共有セマンティクス）
        let src = "
            function makeCounter() {
                var count = 0;
                function bump() { count = count + 1; }
                function read() { return count; }
                bump();
                bump();
                return read();
            }
            makeCounter();
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(2));
    }

    #[test]
    fn closure_recursive_inner() {
        // ネスト関数が自分自身を再帰呼び出し（自由変数として捕捉）
        let src = "
            function outer() {
                function fib(n) {
                    if (n < 2) { return n; }
                    return fib(n - 1) + fib(n - 2);
                }
                return fib(10);
            }
            outer();
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(55));
    }

    #[test]
    fn closure_uses_global_not_captured() {
        // ネスト関数の自由変数がグローバルなら GLoad のまま（捕捉しない）
        let src = "
            var g = 42;
            function outer() {
                function inner() { return g; }
                return inner();
            }
            outer();
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(42));
    }

    #[test]
    fn arrow_function() {
        assert_eq!(run_src("var f = x => x * 2; f(21);").unwrap(), crate::AklVal::mk_int(42));
        assert_eq!(run_src("var g = (a, b) => a + b; g(1, 2);").unwrap(), crate::AklVal::mk_int(3));
        // 即時実行
        assert_eq!(run_src("(x => x + 1)(41);").unwrap(), crate::AklVal::mk_int(42));
    }

    #[test]
    fn function_expression() {
        assert_eq!(
            run_src("var f = function(x) { return x * 2; }; f(21);").unwrap(),
            crate::AklVal::mk_int(42)
        );
        assert_eq!(
            run_src("(function(x) { return x + 1; })(41);").unwrap(),
            crate::AklVal::mk_int(42)
        );
    }

    #[test]
    fn switch_statement() {
        assert_eq!(
            run_src("var x = 2; switch (x) { case 1: 10; break; case 2: 20; break; default: 30; } 20;").unwrap(),
            crate::AklVal::mk_int(20)
        );
        // switch の結果は break 前の式文の値ではなく、後続の文の値を見る
        let src = "
            function f(x) {
                switch (x) {
                    case 1: return 10;
                    case 2: return 20;
                    default: return 30;
                }
            }
            f(2);
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(20));
        assert_eq!(
            run_src("function f(x) { switch (x) { case 1: return 10; default: return 99; } } f(5);").unwrap(),
            crate::AklVal::mk_int(99)
        );
    }

    #[test]
    fn this_in_method() {
        // メソッド内の this がレシーバを指す
        let src = "
            var obj = { value: 42 };
            obj.get = function() { return this.value; };
            obj.get();
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(42));
    }

    #[test]
    fn bitwise_ops() {
        assert_eq!(run_src("5 & 3;").unwrap(), crate::AklVal::mk_int(1));
        assert_eq!(run_src("5 | 3;").unwrap(), crate::AklVal::mk_int(7));
        assert_eq!(run_src("5 ^ 3;").unwrap(), crate::AklVal::mk_int(6));
        assert_eq!(run_src("~5;").unwrap(), crate::AklVal::mk_int(-6));
        assert_eq!(run_src("1 << 3;").unwrap(), crate::AklVal::mk_int(8));
        assert_eq!(run_src("8 >> 2;").unwrap(), crate::AklVal::mk_int(2));
        assert_eq!(run_src("8 >>> 2;").unwrap(), crate::AklVal::mk_int(2));
    }

    #[test]
    fn in_and_delete() {
        assert_eq!(run_src("var o = {a: 1}; \"a\" in o;").unwrap(), crate::AklVal::TRUE);
        assert_eq!(run_src("var o = {a: 1}; \"b\" in o;").unwrap(), crate::AklVal::FALSE);
        assert_eq!(
            run_src("var o = {a: 1}; delete o.a; \"a\" in o;").unwrap(),
            crate::AklVal::FALSE
        );
    }

    #[test]
    fn destructuring_array() {
        assert_eq!(
            run_src("var [a, b] = [1, 2]; a + b;").unwrap(),
            crate::AklVal::mk_int(3)
        );
        assert_eq!(
            run_src("var [a, , b] = [1, 2, 3]; a + b;").unwrap(),
            crate::AklVal::mk_int(4)
        );
        // rest
        assert_eq!(
            run_src("var [a, ...rest] = [1, 2, 3]; rest.length;").unwrap(),
            crate::AklVal::mk_int(2)
        );
    }

    #[test]
    fn destructuring_object() {
        assert_eq!(
            run_src("var {a, b} = {a: 1, b: 2}; a + b;").unwrap(),
            crate::AklVal::mk_int(3)
        );
        assert_eq!(
            run_src("var {a: x} = {a: 10}; x;").unwrap(),
            crate::AklVal::mk_int(10)
        );
    }

    #[test]
    fn spread_array() {
        assert_eq!(
            run_src("var a = [2, 3]; [1, ...a, 4].length;").unwrap(),
            crate::AklVal::mk_int(4)
        );
        assert_eq!(
            run_src("var a = [2, 3]; [1, ...a, 4][2];").unwrap(),
            crate::AklVal::mk_int(3)
        );
    }

    #[test]
    fn throw_and_catch() {
        let src = "
            function f() {
                throw 42;
            }
            try {
                f();
            } catch (e) {
                e;
            }
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(42));
    }

    #[test]
    fn try_catch_no_throw() {
        let src = "
            try {
                1 + 1;
            } catch (e) {
                99;
            }
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(2));
    }

    #[test]
    fn class_basic() {
        let src = "
            class Point {
                constructor(x, y) { this.x = x; this.y = y; }
                sum() { return this.x + this.y; }
            }
            var p = new Point(3, 4);
            p.sum();
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(7));
    }

    #[test]
    fn class_field_access() {
        let src = "
            class C {
                constructor(v) { this.value = v; }
                get() { return this.value; }
            }
            var c = new C(42);
            c.value;
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(42));
    }

    #[test]
    fn instanceof_class() {
        let src = "
            class A {}
            var a = new A();
            a instanceof A;
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::TRUE);
    }

    #[test]
    fn rest_parameter() {
        let src = "
            function sum(...nums) {
                var total = 0;
                for (var i = 0; i < nums.length; i = i + 1) {
                    total = total + nums[i];
                }
                return total;
            }
            sum(1, 2, 3, 4);
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(10));
    }

    #[test]
    fn rest_parameter_with_named() {
        let src = "
            function f(first, ...rest) {
                return first + rest.length;
            }
            f(10, 1, 2, 3);
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(13));
    }

    #[test]
    fn for_of_loop() {
        let src = "
            var sum = 0;
            for (var x of [1, 2, 3, 4]) {
                sum = sum + x;
            }
            sum;
        ";
        assert_eq!(run_src(src).unwrap(), crate::AklVal::mk_int(10));
    }

    #[test]
    fn for_in_loop() {
        // for-in は Object.keys を使うため builtins が必要
        let src = "
            var keys = [];
            for (var k in {a: 1, b: 2}) {
                keys.push(k);
            }
            keys.length;
        ";
        let program = crate::parser::Parser::new(src).parse_program().unwrap();
        let mut rt = Runtime::new();
        crate::builtins::install_builtins(&mut rt).unwrap();
        let fidx = compile(&mut rt, &program).unwrap();
        assert_eq!(rt.run(fidx, &[]).unwrap(), crate::AklVal::mk_int(2));
    }
}
