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
use crate::parser::{BinOp, Expr, ForInit, Stmt, UnaryOp};

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
    };

    // パス 1: トップレベル関数宣言を収集・登録（box 化ローカルを解析してから）
    let mut funcs: Vec<(String, u32)> = Vec::new();
    for stmt in program {
        if let Stmt::FuncDecl { name, params, body } = stmt {
            let boxed = compute_boxed(params, body);
            let fidx = c.compile_function(name, params, body, &boxed)?;
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
    rt.funcs.push(FuncObj { code, name: None, n_params: 0, n_locals });
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
}

impl Compiler<'_> {
    /// 関数をコンパイルして関数表に登録し、index を返す。
    /// `boxed` は「ネスト関数に捕捉される自ローカル」の集合（共有セルとして env に box 化）。
    fn compile_function(
        &mut self,
        name: &str,
        params: &[String],
        body: &[Stmt],
        boxed: &HashMap<String, u32>,
    ) -> Result<u32, CompileError> {
        let mut locals = HashMap::new();
        for p in params {
            locals.insert(p.clone(), locals.len() as u32);
        }
        collect_vars(body, &mut locals);
        // box 化されたローカルは locals に残したまま、参照時に captures を優先する
        // （スロット番号の穴を避ける。box 化ローカルは未使用の locals スロットが残るが無害）

        let saved_locals = std::mem::take(&mut self.locals);
        let saved_captures = std::mem::take(&mut self.captures);
        let saved_order = std::mem::take(&mut self.capture_order);
        let saved_breaks = std::mem::take(&mut self.break_patches);
        let saved_continues = std::mem::take(&mut self.continue_patches);
        self.locals = locals;
        self.captures = boxed.clone();
        self.capture_order = Vec::new();
        self.break_patches = Vec::new();
        self.continue_patches = Vec::new();

        let mut code = Vec::new();
        // 関数入口で自前 env（box 化ローカル）を生成（C の frame_hidden 相当）
        if !boxed.is_empty() {
            code.push(Op::MakeEnv(boxed.len() as u32));
        }
        for stmt in body {
            self.gen_stmt(stmt, &mut code)?;
        }
        // 暗黙の return undefined
        code.push(Op::Undef);
        code.push(Op::Ret);

        let n_locals = self.locals.len();
        let n_params = params.len();
        let name_id = self.rt.intern(name).ok_or_else(|| CompileError("intern failed".into()))?;
        let fidx = self.rt.funcs.len() as u32;
        self.rt.funcs.push(FuncObj {
            code,
            name: Some(name_id),
            n_params,
            n_locals,
        });

        self.locals = saved_locals;
        self.captures = saved_captures;
        self.capture_order = saved_order;
        self.break_patches = saved_breaks;
        self.continue_patches = saved_continues;
        Ok(fidx)
    }

    /// ネスト関数をコンパイルして関数表に登録し、fidx を返す。
    /// 自由変数は「外側の env（captures）に解決」されたら CeLoad/CeStore、なければグローバル。
    fn compile_nested(
        &mut self,
        name: &str,
        params: &[String],
        body: &[Stmt],
        enclosing_env: &HashMap<String, u32>,
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
        self.locals = locals;
        self.captures = captures;
        self.capture_order = Vec::new();
        self.break_patches = Vec::new();
        self.continue_patches = Vec::new();

        let mut code = Vec::new();
        for stmt in body {
            self.gen_stmt(stmt, &mut code)?;
        }
        code.push(Op::Undef);
        code.push(Op::Ret);

        let n_locals = self.locals.len();
        let n_params = params.len();
        let name_id = self.rt.intern(name).ok_or_else(|| CompileError("intern failed".into()))?;
        let fidx = self.rt.funcs.len() as u32;
        self.rt.funcs.push(FuncObj { code, name: Some(name_id), n_params, n_locals });

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
            Expr::Num(NumLit::BigInt(_)) => {
                return Err(CompileError("BigInt literals are not yet supported".into()))
            }
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
                });
            }
            Expr::Assign { name, rhs } => {
                self.gen_expr(rhs, code)?;
                code.push(Op::Dup); // 代入式の値（rhs）を残す
                self.gen_store(name, code)?;
            }
            Expr::Call { callee, args } => {
                self.gen_expr(callee, code)?;
                for a in args {
                    self.gen_expr(a, code)?;
                }
                if args.len() > u8::MAX as usize {
                    return Err(CompileError("too many arguments".into()));
                }
                code.push(Op::Call(args.len() as u8));
            }
            Expr::Arr(items) => {
                for item in items {
                    self.gen_expr(item, code)?;
                }
                if items.len() > u32::MAX as usize {
                    return Err(CompileError("array too large".into()));
                }
                code.push(Op::ArrNew(items.len() as u32));
            }
            Expr::ObjLit(entries) => {
                code.push(Op::ObjNew);
                for (key, val) in entries {
                    code.push(Op::Dup);
                    self.gen_expr(val, code)?;
                    let key_id = self
                        .rt
                        .intern(key)
                        .ok_or_else(|| CompileError("intern failed".into()))?;
                    code.push(Op::PStore(key_id));
                    code.push(Op::Pop); // PStore が返す val を捨て、obj を残す
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
            Expr::CompoundAssign { name, op, rhs } => {
                // x op= y  →  x = x op y（値は新値）
                self.gen_load(name, code)?;
                self.gen_expr(rhs, code)?;
                code.push(match op {
                    BinOp::Add => Op::Add,
                    BinOp::Sub => Op::Sub,
                    BinOp::Mul => Op::Mul,
                    BinOp::Div => Op::Div,
                    BinOp::Mod => Op::Mod,
                    _ => return Err(CompileError("invalid compound assignment operator".into())),
                });
                code.push(Op::Dup);
                self.gen_store(name, code)?;
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
        }
        Ok(())
    }

    /// 文をコード生成。
    fn gen_stmt(&mut self, stmt: &Stmt, code: &mut Vec<Op>) -> Result<(), CompileError> {
        match stmt {
            Stmt::Empty => {}
            Stmt::Expr(e) => {
                self.gen_expr(e, code)?;
                code.push(Op::PopV); // 式文の値を last_val に保存（C の POPV）
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
            Stmt::Break => {
                let patches = self
                    .break_patches
                    .last_mut()
                    .ok_or_else(|| CompileError("break outside loop".into()))?;
                let idx = code.len();
                code.push(Op::Jmp(0)); // プレースホルダ
                patches.push(idx);
            }
            Stmt::Continue => {
                let patches = self
                    .continue_patches
                    .last_mut()
                    .ok_or_else(|| CompileError("continue outside loop".into()))?;
                let idx = code.len();
                code.push(Op::Jmp(0)); // プレースホルダ
                patches.push(idx);
            }
            Stmt::FuncDecl { name, params, body } => {
                // ネスト関数宣言: 現在フレームの env を共有するクロージャを生成して束縛
                let enclosing_env = self.captures.clone();
                let fidx = self.compile_nested(name, params, body, &enclosing_env)?;
                code.push(Op::MakeClosure(fidx));
                self.gen_store(name, code)?;
            }
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
}

/// 関数本体（ブロック）内の `var`/`let`/`const` 宣言名を収集してローカルスロットに割り当てる。
/// ネスト関数の中身は走査しない（ネスト関数は未対応）。
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
            _ => {}
        }
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
            Stmt::Block(inner) => collect_refs(inner, out),
            // ネスト関数宣言の中身は、その関数自身の自由変数として別途解析されるため
            // ここでは名前（束縛先）だけを参照扱いしない（locals で解決される）。
            Stmt::FuncDecl { .. } | Stmt::Empty | Stmt::Break | Stmt::Continue => {}
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
        Expr::IncDec { name, .. } => {
            out.insert(name.clone());
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
            for (_, v) in entries {
                collect_expr_refs(v, out);
            }
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
        Expr::Num(_) | Expr::Str(_) | Expr::Bool(_) | Expr::Null | Expr::Undef => {}
    }
}

/// 制御フローを跨いで文列から直接のネスト関数宣言を収集する（ネスト関数の中身は降りない）。
fn collect_nested_funcs(
    stmts: &[Stmt],
    out: &mut Vec<(String, Vec<String>, Vec<Stmt>)>,
) {
    for stmt in stmts {
        match stmt {
            Stmt::FuncDecl { name, params, body } => {
                out.push((name.clone(), params.clone(), body.clone()));
            }
            Stmt::Block(inner) => collect_nested_funcs(inner, out),
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
        // 文字列連結の結果が "ab" であることを内容で確認（intern id は非決定的なので値比較しない）
        let program = crate::parser::Parser::new("\"a\" + \"b\";").parse_program().unwrap();
        let mut rt = Runtime::new();
        let fidx = compile(&mut rt, &program).unwrap();
        let v = rt.run(fidx, &[]).unwrap();
        match rt.heap.get(v.get_obj()) {
            Some(crate::obj::Obj::Str(s)) => assert_eq!(&**s, "ab"),
            other => panic!("expected string, got {other:?}"),
        }
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
}
