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
use crate::parser::{BinOp, Expr, Stmt, UnaryOp};

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
    let mut c = Compiler { rt, locals: HashMap::new() };

    // パス 1: トップレベル関数宣言を収集・登録
    let mut funcs: Vec<(String, u32)> = Vec::new();
    for stmt in program {
        if let Stmt::FuncDecl { name, params, body } = stmt {
            let fidx = c.compile_function(name, params, body)?;
            funcs.push((name.clone(), fidx));
        }
    }

    // パス 2: main のローカルスロット（トップレベル var 宣言）を割り当て
    let mut locals = HashMap::new();
    for stmt in program {
        if let Stmt::Var { name, .. } = stmt {
            if !locals.contains_key(name) {
                locals.insert(name.clone(), locals.len() as u32);
            }
        }
    }
    c.locals = locals;

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
}

impl Compiler<'_> {
    /// 関数をコンパイルして関数表に登録し、index を返す。
    fn compile_function(
        &mut self,
        name: &str,
        params: &[String],
        body: &[Stmt],
    ) -> Result<u32, CompileError> {
        let mut locals = HashMap::new();
        for p in params {
            locals.insert(p.clone(), locals.len() as u32);
        }
        collect_vars(body, &mut locals);

        let saved = std::mem::take(&mut self.locals);
        self.locals = locals;

        let mut code = Vec::new();
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

        self.locals = saved;
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
            Stmt::While { cond, body } => {
                let loop_start = code.len();
                self.gen_expr(cond, code)?;
                let jmpf_idx = code.len();
                code.push(Op::JmpF(0)); // 仮。falsy なら end へ
                self.gen_stmt(body, code)?;
                code.push(Op::Jmp(loop_start as u32));
                let end_pos = code.len();
                code[jmpf_idx] = Op::JmpF(end_pos as u32);
            }
            Stmt::Block(stmts) => {
                for s in stmts {
                    self.gen_stmt(s, code)?;
                }
            }
            Stmt::FuncDecl { .. } => {
                return Err(CompileError("nested function declarations are not yet supported".into()))
            }
        }
        Ok(())
    }

    /// 変数を読み出す（ローカルなら LLoad、グローバルなら GLoad）。
    fn gen_load(&mut self, name: &str, code: &mut Vec<Op>) -> Result<(), CompileError> {
        if let Some(slot) = self.locals.get(name) {
            code.push(Op::LLoad(*slot));
        } else {
            let id = self.rt.intern(name).ok_or_else(|| CompileError("intern failed".into()))?;
            code.push(Op::GLoad(id));
        }
        Ok(())
    }

    /// 変数へ書き込む（ローカルなら LStore、グローバルなら GStore）。
    fn gen_store(&mut self, name: &str, code: &mut Vec<Op>) -> Result<(), CompileError> {
        if let Some(slot) = self.locals.get(name) {
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
            Stmt::Block(inner) => collect_vars(inner, locals),
            Stmt::If { then, else_, .. } => {
                collect_vars(std::slice::from_ref(then), locals);
                if let Some(e) = else_ {
                    collect_vars(std::slice::from_ref(e), locals);
                }
            }
            Stmt::While { body, .. } => collect_vars(std::slice::from_ref(body), locals),
            _ => {}
        }
    }
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
}
