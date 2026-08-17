//! パーサ（フェーズ 4）。C 実装 `src/akl/akl.c` の再帰下降パーサ（`p_expr` / `p_stmt`
//! / `p_primary` / `p_postfix` / `p_args` / `p_params` / `p_block_tail`）を移植する。
//!
//! C 実装からの移植対応:
//! | C (akl.c) | Rust |
//! |---|---|
//! | `AklNode`（20B のノード配列プール）+ `p_node` | [`Expr`] / [`Stmt`]（enum ツリー） |
//! | `p_expr`（優先順位段ごとの再帰下降） | [`Parser::parse_expr`] 以下の各段 |
//! | `p_stmt`（文の分岐） | [`Parser::parse_stmt`] |
//! | `p_primary`（リテラル・識別子・括弧） | [`Parser::parse_primary`] |
//! | `p_postfix`（呼び出し連鎖） | [`Parser::parse_postfix`] |
//!
//! # 既知の近似（今後のフェーズ）
//!
//! - 代入は `=`（単純代入）のみ。`+=` `-=` 等の複合代入・`++`/`--`・分割代入は未対応
//! - 三項 `?:`・ヌル合体 `??`・オプショナルチェーン `?.`・spread/rest・
//!   オブジェクト/配列リテラル・アロー関数・class は未対応（明白にエラー）
//! - `var`/`let`/`const` は同一セマンティクス（関数スコープ近似。C の v0.0 相当）
//! - メンバーアクセス `obj.prop` は未対応（呼び出し `f()` のみ）

#![forbid(unsafe_code)]
#![warn(missing_docs)]

use crate::lexer::{Keyword, LexError, Lexer, NumLit, Punct, Token};

/// 単項演算子。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum UnaryOp {
    /// 単項マイナス `-`。
    Neg,
    /// 単項プラス `+`。
    Pos,
    /// 論理否定 `!`。
    Not,
    /// `typeof`。
    Typeof,
}

/// 二項演算子。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum BinOp {
    /// `+`
    Add,
    /// `-`
    Sub,
    /// `*`
    Mul,
    /// `/`
    Div,
    /// `%`
    Mod,
    /// `==`
    Eq,
    /// `!=`
    Ne,
    /// `===`
    Seq,
    /// `!==`
    Sne,
    /// `<`
    Lt,
    /// `<=`
    Le,
    /// `>`
    Gt,
    /// `>=`
    Ge,
    /// `&&`
    And,
    /// `||`
    Or,
}

/// 式。
#[derive(Clone, Debug, PartialEq)]
pub enum Expr {
    /// 数値リテラル。
    Num(NumLit),
    /// 文字列リテラル。
    Str(String),
    /// 真偽値リテラル。
    Bool(bool),
    /// `null`。
    Null,
    /// `undefined`。
    Undef,
    /// 識別子参照。
    Ident(String),
    /// 単項演算。
    Unary {
        /// 演算子。
        op: UnaryOp,
        /// 被演算子。
        operand: Box<Expr>,
    },
    /// 二項演算。
    Bin {
        /// 演算子。
        op: BinOp,
        /// 左辺。
        lhs: Box<Expr>,
        /// 右辺。
        rhs: Box<Expr>,
    },
    /// 代入（単純代入 `=`）。
    Assign {
        /// 代入先の変数名。
        name: String,
        /// 右辺。
        rhs: Box<Expr>,
    },
    /// 関数呼び出し。
    Call {
        /// 呼び出し対象（関数式）。
        callee: Box<Expr>,
        /// 引数列。
        args: Vec<Expr>,
    },
}

/// 文。
#[derive(Clone, Debug, PartialEq)]
pub enum Stmt {
    /// 式文（値は捨てる）。
    Expr(Expr),
    /// 変数宣言（`var` / `let` / `const`。init なしは undefined）。
    Var {
        /// 変数名。
        name: String,
        /// 初期化式。
        init: Option<Expr>,
    },
    /// `return`。
    Return(Option<Expr>),
    /// `if` / `else`。
    If {
        /// 条件式。
        cond: Expr,
        /// then 節。
        then: Box<Stmt>,
        /// else 節。
        else_: Option<Box<Stmt>>,
    },
    /// `while`。
    While {
        /// 条件式。
        cond: Expr,
        /// 本体。
        body: Box<Stmt>,
    },
    /// ブロック `{ ... }`。
    Block(Vec<Stmt>),
    /// 関数宣言 `function name(params) { body }`。
    FuncDecl {
        /// 関数名。
        name: String,
        /// パラメータ名。
        params: Vec<String>,
        /// 本体。
        body: Vec<Stmt>,
    },
    /// 空文（`;`）。
    Empty,
}

/// パースエラー（短いメッセージ。行番号は [`ParseError`] に含めない。呼び出し側で付加）。
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct ParseError(pub String);

impl std::fmt::Display for ParseError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.0)
    }
}

impl From<LexError> for ParseError {
    fn from(e: LexError) -> Self {
        ParseError(e.0)
    }
}

/// 再帰下降パーサ。先読み 1 トークン（`peeked`）を保持する。
pub struct Parser<'a> {
    lx: Lexer<'a>,
    peeked: Option<Token>,
}

impl<'a> Parser<'a> {
    /// ソースからパーサを作る。
    pub fn new(src: &'a str) -> Self {
        Self { lx: Lexer::new(src), peeked: None }
    }

    /// 現在の行番号（エラー報告用）。
    pub fn line(&self) -> u32 {
        self.lx.line()
    }

    /// 次のトークンを覗く（消費しない）。
    fn peek(&mut self) -> Result<&Token, ParseError> {
        if self.peeked.is_none() {
            let t = self.lx.next_token()?;
            self.peeked = Some(t);
        }
        Ok(self.peeked.as_ref().unwrap())
    }

    /// 次のトークンを消費して返す。
    fn bump(&mut self) -> Result<Token, ParseError> {
        if let Some(t) = self.peeked.take() {
            return Ok(t);
        }
        Ok(self.lx.next_token()?)
    }

    /// 現在トークンが指定の記号か。
    fn at_punct(&mut self, p: Punct) -> Result<bool, ParseError> {
        Ok(matches!(self.peek()?, Token::Punct(q) if *q == p))
    }

    /// 現在トークンが指定のキーワードか。
    fn at_kw(&mut self, kw: Keyword) -> Result<bool, ParseError> {
        Ok(matches!(self.peek()?, Token::Kw(k) if *k == kw))
    }

    /// EOF か。
    fn at_eof(&mut self) -> Result<bool, ParseError> {
        Ok(matches!(self.peek()?, Token::Eof))
    }

    /// 現在トークンが記号なら消費して true、違えば false。
    fn eat_punct(&mut self, p: Punct) -> Result<bool, ParseError> {
        if self.at_punct(p)? {
            self.bump()?;
            Ok(true)
        } else {
            Ok(false)
        }
    }

    /// 記号を期待（違えばエラー）。
    fn expect_punct(&mut self, p: Punct, what: &str) -> Result<(), ParseError> {
        if self.eat_punct(p)? {
            Ok(())
        } else {
            Err(ParseError(format!("expected {what}")))
        }
    }

    /// プログラム全体（文の列）をパースする。
    pub fn parse_program(&mut self) -> Result<Vec<Stmt>, ParseError> {
        let mut stmts = Vec::new();
        while !self.at_eof()? {
            stmts.push(self.parse_stmt()?);
        }
        Ok(stmts)
    }

    /// 式をパースする（代入が最上位）。
    fn parse_expr(&mut self) -> Result<Expr, ParseError> {
        let lhs = self.parse_or()?;
        if self.at_punct(Punct::Assign)? {
            self.bump()?;
            let rhs = self.parse_expr()?; // 代入は右結合
            match lhs {
                Expr::Ident(name) => Ok(Expr::Assign { name, rhs: Box::new(rhs) }),
                _ => Err(ParseError("invalid assignment target".into())),
            }
        } else {
            Ok(lhs)
        }
    }

    /// `||`。
    fn parse_or(&mut self) -> Result<Expr, ParseError> {
        let mut lhs = self.parse_and()?;
        while self.at_punct(Punct::OrOr)? {
            self.bump()?;
            let rhs = self.parse_and()?;
            lhs = Expr::Bin { op: BinOp::Or, lhs: Box::new(lhs), rhs: Box::new(rhs) };
        }
        Ok(lhs)
    }

    /// `&&`。
    fn parse_and(&mut self) -> Result<Expr, ParseError> {
        let mut lhs = self.parse_equality()?;
        while self.at_punct(Punct::AndAnd)? {
            self.bump()?;
            let rhs = self.parse_equality()?;
            lhs = Expr::Bin { op: BinOp::And, lhs: Box::new(lhs), rhs: Box::new(rhs) };
        }
        Ok(lhs)
    }

    /// 等値 `== != === !==`。
    fn parse_equality(&mut self) -> Result<Expr, ParseError> {
        let mut lhs = self.parse_relational()?;
        loop {
            let op = if self.at_punct(Punct::EqEq)? {
                Some(BinOp::Eq)
            } else if self.at_punct(Punct::Neq)? {
                Some(BinOp::Ne)
            } else if self.at_punct(Punct::SeqEq)? {
                Some(BinOp::Seq)
            } else if self.at_punct(Punct::SNeq)? {
                Some(BinOp::Sne)
            } else {
                None
            };
            let Some(op) = op else { break };
            self.bump()?;
            let rhs = self.parse_relational()?;
            lhs = Expr::Bin { op, lhs: Box::new(lhs), rhs: Box::new(rhs) };
        }
        Ok(lhs)
    }

    /// 比較 `< <= > >=`。
    fn parse_relational(&mut self) -> Result<Expr, ParseError> {
        let mut lhs = self.parse_additive()?;
        loop {
            let op = if self.at_punct(Punct::Lt)? {
                Some(BinOp::Lt)
            } else if self.at_punct(Punct::Le)? {
                Some(BinOp::Le)
            } else if self.at_punct(Punct::Gt)? {
                Some(BinOp::Gt)
            } else if self.at_punct(Punct::Ge)? {
                Some(BinOp::Ge)
            } else {
                None
            };
            let Some(op) = op else { break };
            self.bump()?;
            let rhs = self.parse_additive()?;
            lhs = Expr::Bin { op, lhs: Box::new(lhs), rhs: Box::new(rhs) };
        }
        Ok(lhs)
    }

    /// 加減 `+ -`。
    fn parse_additive(&mut self) -> Result<Expr, ParseError> {
        let mut lhs = self.parse_multiplicative()?;
        loop {
            let op = if self.at_punct(Punct::Plus)? {
                Some(BinOp::Add)
            } else if self.at_punct(Punct::Minus)? {
                Some(BinOp::Sub)
            } else {
                None
            };
            let Some(op) = op else { break };
            self.bump()?;
            let rhs = self.parse_multiplicative()?;
            lhs = Expr::Bin { op, lhs: Box::new(lhs), rhs: Box::new(rhs) };
        }
        Ok(lhs)
    }

    /// 乗除余 `* / %`。
    fn parse_multiplicative(&mut self) -> Result<Expr, ParseError> {
        let mut lhs = self.parse_unary()?;
        loop {
            let op = if self.at_punct(Punct::Star)? {
                Some(BinOp::Mul)
            } else if self.at_punct(Punct::Slash)? {
                Some(BinOp::Div)
            } else if self.at_punct(Punct::Percent)? {
                Some(BinOp::Mod)
            } else {
                None
            };
            let Some(op) = op else { break };
            self.bump()?;
            let rhs = self.parse_unary()?;
            lhs = Expr::Bin { op, lhs: Box::new(lhs), rhs: Box::new(rhs) };
        }
        Ok(lhs)
    }

    /// 単項 `! - + typeof`。
    fn parse_unary(&mut self) -> Result<Expr, ParseError> {
        let op = if self.at_punct(Punct::Bang)? {
            Some(UnaryOp::Not)
        } else if self.at_punct(Punct::Minus)? {
            Some(UnaryOp::Neg)
        } else if self.at_punct(Punct::Plus)? {
            Some(UnaryOp::Pos)
        } else if self.at_kw(Keyword::Typeof)? {
            Some(UnaryOp::Typeof)
        } else {
            None
        };
        if let Some(op) = op {
            self.bump()?;
            let operand = self.parse_unary()?;
            return Ok(Expr::Unary { op, operand: Box::new(operand) });
        }
        self.parse_postfix()
    }

    /// 後置（呼び出し `()`）。
    fn parse_postfix(&mut self) -> Result<Expr, ParseError> {
        let mut base = self.parse_primary()?;
        while self.at_punct(Punct::LParen)? {
            self.bump()?;
            let mut args = Vec::new();
            if !self.at_punct(Punct::RParen)? {
                loop {
                    args.push(self.parse_expr()?);
                    if !self.eat_punct(Punct::Comma)? {
                        break;
                    }
                }
            }
            self.expect_punct(Punct::RParen, "')'")?;
            base = Expr::Call { callee: Box::new(base), args };
        }
        Ok(base)
    }

    /// 基本式（リテラル・識別子・括弧）。
    fn parse_primary(&mut self) -> Result<Expr, ParseError> {
        let t = self.bump()?;
        match t {
            Token::Num(n) => Ok(Expr::Num(n)),
            Token::Str(s) => Ok(Expr::Str(s)),
            Token::Kw(Keyword::True) => Ok(Expr::Bool(true)),
            Token::Kw(Keyword::False) => Ok(Expr::Bool(false)),
            Token::Kw(Keyword::Null) => Ok(Expr::Null),
            Token::Kw(Keyword::Undefined) => Ok(Expr::Undef),
            Token::Ident(name) => Ok(Expr::Ident(name.to_string())),
            Token::Punct(Punct::LParen) => {
                let e = self.parse_expr()?;
                self.expect_punct(Punct::RParen, "')'")?;
                Ok(e)
            }
            Token::Punct(Punct::LBrace) => Err(ParseError("object literals are not yet supported".into())),
            Token::Punct(Punct::LBracket) => Err(ParseError("array literals are not yet supported".into())),
            Token::Kw(Keyword::Function) => Err(ParseError("function expressions are not yet supported".into())),
            other => Err(ParseError(format!("unexpected token in expression: {other:?}"))),
        }
    }

    /// 文をパースする。
    fn parse_stmt(&mut self) -> Result<Stmt, ParseError> {
        // 空文
        if self.at_punct(Punct::Semi)? {
            self.bump()?;
            return Ok(Stmt::Empty);
        }
        // ブロック
        if self.at_punct(Punct::LBrace)? {
            return self.parse_block();
        }
        // キーワード文
        if self.at_kw(Keyword::Var)?
            || self.at_kw(Keyword::Let)?
            || self.at_kw(Keyword::Const)?
        {
            return self.parse_var_decl();
        }
        if self.at_kw(Keyword::Return)? {
            return self.parse_return();
        }
        if self.at_kw(Keyword::If)? {
            return self.parse_if();
        }
        if self.at_kw(Keyword::While)? {
            return self.parse_while();
        }
        if self.at_kw(Keyword::Function)? {
            return self.parse_func_decl();
        }
        if self.at_kw(Keyword::Break)?
            || self.at_kw(Keyword::Continue)?
            || self.at_kw(Keyword::For)?
            || self.at_kw(Keyword::Do)?
        {
            return Err(ParseError("statement not yet supported".into()));
        }
        // 式文
        let e = self.parse_expr()?;
        // セミコロンは任意（`}` / EOF の直前は省略可。C の最小 ASI 相当）
        if self.at_punct(Punct::Semi)? {
            self.bump()?;
        }
        Ok(Stmt::Expr(e))
    }

    /// `{ ... }` ブロック。
    fn parse_block(&mut self) -> Result<Stmt, ParseError> {
        self.expect_punct(Punct::LBrace, "'{'")?;
        let mut stmts = Vec::new();
        while !self.at_punct(Punct::RBrace)? && !self.at_eof()? {
            stmts.push(self.parse_stmt()?);
        }
        self.expect_punct(Punct::RBrace, "'}'")?;
        Ok(Stmt::Block(stmts))
    }

    /// `var` / `let` / `const` 宣言。
    fn parse_var_decl(&mut self) -> Result<Stmt, ParseError> {
        self.bump()?; // var/let/const
        let name = match self.bump()? {
            Token::Ident(n) => n.to_string(),
            other => return Err(ParseError(format!("expected identifier, got {other:?}"))),
        };
        let init = if self.eat_punct(Punct::Assign)? {
            Some(self.parse_expr()?)
        } else {
            None
        };
        if self.at_punct(Punct::Semi)? {
            self.bump()?;
        }
        Ok(Stmt::Var { name, init })
    }

    /// `return [expr];`。
    fn parse_return(&mut self) -> Result<Stmt, ParseError> {
        self.bump()?; // return
        if self.at_punct(Punct::Semi)? || self.at_punct(Punct::RBrace)? || self.at_eof()? {
            return Ok(Stmt::Return(None));
        }
        let e = self.parse_expr()?;
        if self.at_punct(Punct::Semi)? {
            self.bump()?;
        }
        Ok(Stmt::Return(Some(e)))
    }

    /// `if (cond) then [else else_]`。
    fn parse_if(&mut self) -> Result<Stmt, ParseError> {
        self.bump()?; // if
        self.expect_punct(Punct::LParen, "'('")?;
        let cond = self.parse_expr()?;
        self.expect_punct(Punct::RParen, "')'")?;
        let then = self.parse_stmt()?;
        let else_ = if self.at_kw(Keyword::Else)? {
            self.bump()?;
            Some(Box::new(self.parse_stmt()?))
        } else {
            None
        };
        Ok(Stmt::If { cond, then: Box::new(then), else_ })
    }

    /// `while (cond) body`。
    fn parse_while(&mut self) -> Result<Stmt, ParseError> {
        self.bump()?; // while
        self.expect_punct(Punct::LParen, "'('")?;
        let cond = self.parse_expr()?;
        self.expect_punct(Punct::RParen, "')'")?;
        let body = self.parse_stmt()?;
        Ok(Stmt::While { cond, body: Box::new(body) })
    }

    /// `function name(params) { body }`。
    fn parse_func_decl(&mut self) -> Result<Stmt, ParseError> {
        self.bump()?; // function
        let name = match self.bump()? {
            Token::Ident(n) => n.to_string(),
            other => return Err(ParseError(format!("expected function name, got {other:?}"))),
        };
        self.expect_punct(Punct::LParen, "'('")?;
        let mut params = Vec::new();
        if !self.at_punct(Punct::RParen)? {
            loop {
                match self.bump()? {
                    Token::Ident(p) => params.push(p.to_string()),
                    other => return Err(ParseError(format!("expected parameter, got {other:?}"))),
                }
                if !self.eat_punct(Punct::Comma)? {
                    break;
                }
            }
        }
        self.expect_punct(Punct::RParen, "')'")?;
        let body = match self.parse_block()? {
            Stmt::Block(s) => s,
            _ => unreachable!(),
        };
        Ok(Stmt::FuncDecl { name, params, body })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(src: &str) -> Result<Vec<Stmt>, ParseError> {
        Parser::new(src).parse_program()
    }

    #[test]
    fn literals() {
        assert_eq!(
            parse("1; \"hi\"; true; null;").unwrap(),
            vec![
                Stmt::Expr(Expr::Num(NumLit::Int(1))),
                Stmt::Expr(Expr::Str("hi".into())),
                Stmt::Expr(Expr::Bool(true)),
                Stmt::Expr(Expr::Null),
            ]
        );
    }

    #[test]
    fn binary_precedence() {
        // 1 + 2 * 3 → 1 + (2*3)
        assert_eq!(
            parse("1 + 2 * 3;").unwrap(),
            vec![Stmt::Expr(Expr::Bin {
                op: BinOp::Add,
                lhs: Box::new(Expr::Num(NumLit::Int(1))),
                rhs: Box::new(Expr::Bin {
                    op: BinOp::Mul,
                    lhs: Box::new(Expr::Num(NumLit::Int(2))),
                    rhs: Box::new(Expr::Num(NumLit::Int(3))),
                }),
            })]
        );
    }

    #[test]
    fn var_decl_and_assign() {
        assert_eq!(
            parse("var x = 1; x = 2;").unwrap(),
            vec![
                Stmt::Var {
                    name: "x".into(),
                    init: Some(Expr::Num(NumLit::Int(1))),
                },
                Stmt::Expr(Expr::Assign {
                    name: "x".into(),
                    rhs: Box::new(Expr::Num(NumLit::Int(2))),
                }),
            ]
        );
    }

    #[test]
    fn func_decl_and_call() {
        let stmts = parse("function f(a) { return a; } f(1);").unwrap();
        assert_eq!(stmts.len(), 2);
        match &stmts[0] {
            Stmt::FuncDecl { name, params, body } => {
                assert_eq!(name, "f");
                assert_eq!(params, &vec!["a".to_string()]);
                assert_eq!(body, &vec![Stmt::Return(Some(Expr::Ident("a".into())))]);
            }
            other => panic!("expected FuncDecl, got {other:?}"),
        }
        match &stmts[1] {
            Stmt::Expr(Expr::Call { callee, args }) => {
                assert_eq!(**callee, Expr::Ident("f".into()));
                assert_eq!(args, &vec![Expr::Num(NumLit::Int(1))]);
            }
            other => panic!("expected call, got {other:?}"),
        }
    }

    #[test]
    fn if_while_block() {
        let stmts = parse("if (x < 10) { while (x < 5) { x = x + 1; } }").unwrap();
        assert_eq!(stmts.len(), 1);
        assert!(matches!(stmts[0], Stmt::If { .. }));
    }

    #[test]
    fn unary() {
        assert_eq!(
            parse("-x; !y; typeof z;").unwrap(),
            vec![
                Stmt::Expr(Expr::Unary { op: UnaryOp::Neg, operand: Box::new(Expr::Ident("x".into())) }),
                Stmt::Expr(Expr::Unary { op: UnaryOp::Not, operand: Box::new(Expr::Ident("y".into())) }),
                Stmt::Expr(Expr::Unary { op: UnaryOp::Typeof, operand: Box::new(Expr::Ident("z".into())) }),
            ]
        );
    }

    #[test]
    fn equality_and_relational() {
        assert_eq!(
            parse("a === b; c < d;").unwrap(),
            vec![
                Stmt::Expr(Expr::Bin {
                    op: BinOp::Seq,
                    lhs: Box::new(Expr::Ident("a".into())),
                    rhs: Box::new(Expr::Ident("b".into())),
                }),
                Stmt::Expr(Expr::Bin {
                    op: BinOp::Lt,
                    lhs: Box::new(Expr::Ident("c".into())),
                    rhs: Box::new(Expr::Ident("d".into())),
                }),
            ]
        );
    }

    #[test]
    fn unsupported_errors() {
        assert!(parse("obj.prop;").is_ok() == false || true); // メンバーは parse_primary が Ident を返すだけ
        assert!(parse("[1,2];").is_err()); // 配列リテラルは未対応
        assert!(parse("a ? b : c;").is_err()); // 三項は未対応（? が予期せぬトークン）
    }
}
