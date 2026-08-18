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
    /// 配列リテラル `[e, ...]`。
    Arr(Vec<Expr>),
    /// オブジェクトリテラル `{ k: v, ... }`。
    ObjLit(Vec<(String, Expr)>),
    /// メンバーアクセス `obj.prop`。
    Member {
        /// 対象。
        obj: Box<Expr>,
        /// プロパティ名。
        name: String,
    },
    /// インデックスアクセス `obj[index]`。
    Index {
        /// 対象。
        obj: Box<Expr>,
        /// インデックス式。
        index: Box<Expr>,
    },
    /// 配列/オブジェクトへの要素代入 `obj[index] = rhs`。
    IndexAssign {
        /// 対象。
        obj: Box<Expr>,
        /// インデックス式。
        index: Box<Expr>,
        /// 右辺。
        rhs: Box<Expr>,
    },
    /// メンバー代入 `obj.prop = rhs`。
    MemberAssign {
        /// 対象。
        obj: Box<Expr>,
        /// プロパティ名。
        name: String,
        /// 右辺。
        rhs: Box<Expr>,
    },
    /// `this` キーワード。
    This,
    /// 関数式 `function [name] (params) { body }`（無名は name=None）。
    FuncExpr {
        /// 関数名（無名は None）。
        name: Option<String>,
        /// パラメータ名。
        params: Vec<String>,
        /// 本体。
        body: Vec<Stmt>,
    },
    /// アロー関数 `(params) => expr` または `(params) => { body }`。
    Arrow {
        /// パラメータ名。
        params: Vec<String>,
        /// 本体（式の場合は暗黙 return、ブロックは文列）。
        body: Box<Expr>,
    },
    /// 三項演算子 `cond ? then : else_`。
    Ternary {
        /// 条件式。
        cond: Box<Expr>,
        /// 真のときの値。
        then: Box<Expr>,
        /// 偽のときの値。
        else_: Box<Expr>,
    },
    /// 複合代入 `x += y`（`op` は二項演算子。変数のみサポート）。
    CompoundAssign {
        /// 代入先の変数名。
        name: String,
        /// 演算子。
        op: BinOp,
        /// 右辺。
        rhs: Box<Expr>,
    },
    /// 前置/後置インクリメント・デクリメント（変数のみサポート）。
    IncDec {
        /// 対象の変数名。
        name: String,
        /// true = `++`、false = `--`。
        inc: bool,
        /// true = 前置（`++x`）、false = 後置（`x++`）。
        prefix: bool,
    },
}

/// `for` 文の初期化節。
#[derive(Clone, Debug, PartialEq)]
pub enum ForInit {
    /// `var x = init`（変数宣言）。
    Var {
        /// 変数名。
        name: String,
        /// 初期化式。
        init: Option<Expr>,
    },
    /// 式（代入など）。
    Expr(Expr),
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
    /// `for (init; cond; step) body`。
    For {
        /// 初期化節。
        init: Option<ForInit>,
        /// 条件式（省略時は常に真）。
        cond: Option<Expr>,
        /// 更新節。
        step: Option<Expr>,
        /// 本体。
        body: Box<Stmt>,
    },
    /// `do { body } while (cond);`。
    DoWhile {
        /// 本体。
        body: Box<Stmt>,
        /// 条件式。
        cond: Expr,
    },
    /// `break;`。
    Break,
    /// `continue;`。
    Continue,
    /// `switch (disc) { case x: ...; default: ... }`。
    Switch {
        /// 判別式。
        disc: Expr,
        /// (case 値（None = default）, 本体) の列。
        cases: Vec<(Option<Expr>, Vec<Stmt>)>,
    },
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

    /// パーサ状態を退避する（アロー関数の先読みロールバック用）。
    fn save_state(&self) -> (Lexer<'a>, Option<Token>) {
        (self.lx.clone(), self.peeked.clone())
    }

    /// パーサ状態を復元する。
    fn restore_state(&mut self, state: (Lexer<'a>, Option<Token>)) {
        self.lx = state.0;
        self.peeked = state.1;
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
        let lhs = self.parse_conditional()?;
        // 単純代入 =
        if self.at_punct(Punct::Assign)? {
            self.bump()?;
            let rhs = self.parse_expr()?; // 代入は右結合
            return match lhs {
                Expr::Ident(name) => Ok(Expr::Assign { name, rhs: Box::new(rhs) }),
                Expr::Index { obj, index } => {
                    Ok(Expr::IndexAssign { obj, index, rhs: Box::new(rhs) })
                }
                Expr::Member { obj, name } => {
                    Ok(Expr::MemberAssign { obj, name, rhs: Box::new(rhs) })
                }
                _ => Err(ParseError("invalid assignment target".into())),
            };
        }
        // 複合代入 += -= *= /= %=
        let op = if self.at_punct(Punct::AddAss)? {
            Some(BinOp::Add)
        } else if self.at_punct(Punct::SubAss)? {
            Some(BinOp::Sub)
        } else if self.at_punct(Punct::MulAss)? {
            Some(BinOp::Mul)
        } else if self.at_punct(Punct::DivAss)? {
            Some(BinOp::Div)
        } else if self.at_punct(Punct::ModAss)? {
            Some(BinOp::Mod)
        } else {
            None
        };
        if let Some(op) = op {
            self.bump()?;
            let rhs = self.parse_expr()?;
            return match lhs {
                Expr::Ident(name) => Ok(Expr::CompoundAssign { name, op, rhs: Box::new(rhs) }),
                _ => Err(ParseError("invalid assignment target".into())),
            };
        }
        Ok(lhs)
    }

    /// 三項 `?:`（`||` より低い優先順位。then/else は代入レベル）。
    fn parse_conditional(&mut self) -> Result<Expr, ParseError> {
        let cond = self.parse_or()?;
        if self.at_punct(Punct::Question)? {
            self.bump()?;
            let then = self.parse_expr()?;
            self.expect_punct(Punct::Colon, "':'")?;
            let else_ = self.parse_expr()?;
            Ok(Expr::Ternary {
                cond: Box::new(cond),
                then: Box::new(then),
                else_: Box::new(else_),
            })
        } else {
            Ok(cond)
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

    /// 単項 `! - + typeof` + 前置 `++`/`--`。
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
        // 前置 ++ / --
        let incdec = if self.at_punct(Punct::Inc)? {
            Some(true)
        } else if self.at_punct(Punct::Dec)? {
            Some(false)
        } else {
            None
        };
        if let Some(inc) = incdec {
            self.bump()?;
            let operand = self.parse_unary()?;
            return match operand {
                Expr::Ident(name) => Ok(Expr::IncDec { name, inc, prefix: true }),
                _ => Err(ParseError("invalid increment/decrement target".into())),
            };
        }
        self.parse_postfix()
    }

    /// 後置（呼び出し `()`・メンバー `.prop`・インデックス `[i]`）。
    fn parse_postfix(&mut self) -> Result<Expr, ParseError> {
        let mut base = self.parse_primary()?;
        loop {
            if self.at_punct(Punct::LParen)? {
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
            } else if self.at_punct(Punct::Dot)? {
                self.bump()?;
                let name = match self.bump()? {
                    Token::Ident(n) => n.to_string(),
                    Token::Kw(kw) => format!("{kw:?}").to_lowercase(),
                    other => {
                        return Err(ParseError(format!("expected property name after '.', got {other:?}")))
                    }
                };
                base = Expr::Member { obj: Box::new(base), name };
            } else if self.at_punct(Punct::LBracket)? {
                self.bump()?;
                let index = self.parse_expr()?;
                self.expect_punct(Punct::RBracket, "']'")?;
                base = Expr::Index { obj: Box::new(base), index: Box::new(index) };
            } else if self.at_punct(Punct::Inc)? || self.at_punct(Punct::Dec)? {
                // 後置 ++ / --
                let inc = self.at_punct(Punct::Inc)?;
                self.bump()?;
                return match base {
                    Expr::Ident(name) => Ok(Expr::IncDec { name, inc, prefix: false }),
                    _ => Err(ParseError("invalid increment/decrement target".into())),
                };
            } else {
                break;
            }
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
            Token::Kw(Keyword::This) => Ok(Expr::This),
            Token::Ident(name) => {
                // アロー関数の単一引数形: `x => body`
                if self.at_punct(Punct::Arrow)? {
                    self.bump()?;
                    let body = self.parse_arrow_body()?;
                    return Ok(Expr::Arrow { params: vec![name.to_string()], body: Box::new(body) });
                }
                Ok(Expr::Ident(name.to_string()))
            }
            Token::Kw(Keyword::Function) => self.parse_func_expr(),
            Token::Punct(Punct::LParen) => {
                // アロー関数 `(params) => body` か、グループ式 `(expr)` かの先読み
                let saved = self.save_state();
                if let Some(params) = self.try_parse_params()? {
                    if self.at_punct(Punct::Arrow)? {
                        self.bump()?;
                        let body = self.parse_arrow_body()?;
                        return Ok(Expr::Arrow { params, body: Box::new(body) });
                    }
                }
                // ロールバックしてグループ式
                self.restore_state(saved);
                let e = self.parse_expr()?;
                self.expect_punct(Punct::RParen, "')'")?;
                Ok(e)
            }
            Token::Punct(Punct::LBrace) => {
                // オブジェクトリテラル { k: v, ... }
                let mut entries = Vec::new();
                if !self.at_punct(Punct::RBrace)? {
                    loop {
                        let key = match self.bump()? {
                            Token::Ident(n) => n.to_string(),
                            Token::Str(s) => s,
                            Token::Kw(kw) => format!("{kw:?}").to_lowercase(),
                            other => {
                                return Err(ParseError(format!("expected property key, got {other:?}")))
                            }
                        };
                        self.expect_punct(Punct::Colon, "':'")?;
                        let val = self.parse_expr()?;
                        entries.push((key, val));
                        if !self.eat_punct(Punct::Comma)? {
                            break;
                        }
                    }
                }
                self.expect_punct(Punct::RBrace, "'}'")?;
                Ok(Expr::ObjLit(entries))
            }
            Token::Punct(Punct::LBracket) => {
                // 配列リテラル [ e, ... ]
                let mut items = Vec::new();
                if !self.at_punct(Punct::RBracket)? {
                    loop {
                        items.push(self.parse_expr()?);
                        if !self.eat_punct(Punct::Comma)? {
                            break;
                        }
                    }
                }
                self.expect_punct(Punct::RBracket, "']'")?;
                Ok(Expr::Arr(items))
            }
            other => Err(ParseError(format!("unexpected token in expression: {other:?}"))),
        }
    }

    /// `function [name] (params) { body }`（関数式。`function` は消費済み）。
    fn parse_func_expr(&mut self) -> Result<Expr, ParseError> {
        // 省略可能な名前
        let name = match self.peek()? {
            Token::Ident(n) => {
                let n = n.to_string();
                self.bump()?;
                Some(n)
            }
            _ => None,
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
        Ok(Expr::FuncExpr { name, params, body })
    }

    /// `=> body` の本体（`=>` は消費済み）。式なら暗黙 return、ブロックは文列。
    fn parse_arrow_body(&mut self) -> Result<Expr, ParseError> {
        if self.at_punct(Punct::LBrace)? {
            // ブロック本体 → Return 文に変換して 1 つの Expr::Block 風に包む
            // （Expr::Arrow の body は Box<Expr> なので、Block を表す専用式は持たず、
            // ここでは式を返す。ブロック本体は codegen 側で特別扱いしないため、
            // 式のみサポートし、ブロックは未対応とする）
            return Err(ParseError("arrow function with block body is not yet supported".into()));
        }
        self.parse_expr()
    }

    /// `(params)` を試す（アロー関数用）。成功なら params、失敗（グループ式）なら None。
    /// 呼び出し時点で `(` は既に消費済み。
    fn try_parse_params(&mut self) -> Result<Option<Vec<String>>, ParseError> {
        // 空の `()` はアロー（params 空）にもグループ（空は不正）にもなり得るが、
        // `() => ...` のみ有効なので空でも params として扱う。
        if self.at_punct(Punct::RParen)? {
            self.bump()?;
            return Ok(Some(Vec::new()));
        }
        let mut params = Vec::new();
        loop {
            match self.peek()? {
                Token::Ident(_) => {
                    let p = match self.bump()? {
                        Token::Ident(n) => n.to_string(),
                        _ => unreachable!(),
                    };
                    params.push(p);
                }
                _ => return Ok(None), // グループ式（識別子でない）
            }
            if self.eat_punct(Punct::Comma)? {
                continue;
            }
            if self.at_punct(Punct::RParen)? {
                self.bump()?;
                return Ok(Some(params));
            }
            return Ok(None);
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
        if self.at_kw(Keyword::For)? {
            return self.parse_for();
        }
        if self.at_kw(Keyword::Do)? {
            return self.parse_do_while();
        }
        if self.at_kw(Keyword::Break)? {
            self.bump()?;
            if self.at_punct(Punct::Semi)? {
                self.bump()?;
            }
            return Ok(Stmt::Break);
        }
        if self.at_kw(Keyword::Continue)? {
            self.bump()?;
            if self.at_punct(Punct::Semi)? {
                self.bump()?;
            }
            return Ok(Stmt::Continue);
        }
        if self.at_kw(Keyword::Function)? {
            return self.parse_func_decl();
        }
        if self.at_kw(Keyword::Switch)? {
            return self.parse_switch();
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

    /// `for (init; cond; step) body`。
    fn parse_for(&mut self) -> Result<Stmt, ParseError> {
        self.bump()?; // for
        self.expect_punct(Punct::LParen, "'('")?;
        // init
        let init = if self.at_punct(Punct::Semi)? {
            self.bump()?;
            None
        } else if self.at_kw(Keyword::Var)?
            || self.at_kw(Keyword::Let)?
            || self.at_kw(Keyword::Const)?
        {
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
            self.expect_punct(Punct::Semi, "';'")?;
            Some(ForInit::Var { name, init })
        } else {
            let e = self.parse_expr()?;
            self.expect_punct(Punct::Semi, "';'")?;
            Some(ForInit::Expr(e))
        };
        // cond
        let cond = if self.at_punct(Punct::Semi)? {
            self.bump()?;
            None
        } else {
            let c = self.parse_expr()?;
            self.expect_punct(Punct::Semi, "';'")?;
            Some(c)
        };
        // step
        let step = if self.at_punct(Punct::RParen)? {
            None
        } else {
            let s = self.parse_expr()?;
            Some(s)
        };
        self.expect_punct(Punct::RParen, "')'")?;
        let body = self.parse_stmt()?;
        Ok(Stmt::For { init, cond, step, body: Box::new(body) })
    }

    /// `do { body } while (cond);`。
    fn parse_do_while(&mut self) -> Result<Stmt, ParseError> {
        self.bump()?; // do
        let body = self.parse_stmt()?;
        if !self.at_kw(Keyword::While)? {
            return Err(ParseError("expected 'while' after do body".into()));
        }
        self.bump()?; // while
        self.expect_punct(Punct::LParen, "'('")?;
        let cond = self.parse_expr()?;
        self.expect_punct(Punct::RParen, "')'")?;
        if self.at_punct(Punct::Semi)? {
            self.bump()?;
        }
        Ok(Stmt::DoWhile { body: Box::new(body), cond })
    }

    /// `switch (disc) { case x: ...; default: ... }`。
    fn parse_switch(&mut self) -> Result<Stmt, ParseError> {
        self.bump()?; // switch
        self.expect_punct(Punct::LParen, "'('")?;
        let disc = self.parse_expr()?;
        self.expect_punct(Punct::RParen, "')'")?;
        self.expect_punct(Punct::LBrace, "'{'")?;
        let mut cases = Vec::new();
        while !self.at_punct(Punct::RBrace)? && !self.at_eof()? {
            // case 値 or default
            let case_val = if self.at_kw(Keyword::Case)? {
                self.bump()?;
                Some(self.parse_expr()?)
            } else if self.at_kw(Keyword::Default)? {
                self.bump()?;
                None
            } else {
                return Err(ParseError("expected 'case' or 'default'".into()));
            };
            self.expect_punct(Punct::Colon, "':'")?;
            // 本体の文列（次の case/default か `}` まで）
            let mut body = Vec::new();
            while !self.at_kw(Keyword::Case)?
                && !self.at_kw(Keyword::Default)?
                && !self.at_punct(Punct::RBrace)?
                && !self.at_eof()?
            {
                body.push(self.parse_stmt()?);
            }
            cases.push((case_val, body));
        }
        self.expect_punct(Punct::RBrace, "'}'")?;
        Ok(Stmt::Switch { disc, cases })
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
        assert!(parse("class C {}").is_err()); // class は未対応
    }

    #[test]
    fn this_and_arrow() {
        assert_eq!(parse("this;").unwrap(), vec![Stmt::Expr(Expr::This)]);
        // アロー関数（単一引数）
        let stmts = parse("var f = x => x * 2;").unwrap();
        assert!(matches!(stmts[0], Stmt::Var { name: _, init: Some(Expr::Arrow { .. }) }));
        // アロー関数（複数引数）
        let stmts = parse("var g = (a, b) => a + b;").unwrap();
        assert!(matches!(stmts[0], Stmt::Var { name: _, init: Some(Expr::Arrow { .. }) }));
    }

    #[test]
    fn func_expr_and_switch() {
        let stmts = parse("var f = function(x) { return x; };").unwrap();
        assert!(matches!(stmts[0], Stmt::Var { name: _, init: Some(Expr::FuncExpr { .. }) }));
        let stmts = parse("switch (x) { case 1: break; default: break; }").unwrap();
        assert!(matches!(stmts[0], Stmt::Switch { .. }));
    }

    #[test]
    fn ternary() {
        assert_eq!(
            parse("a ? b : c;").unwrap(),
            vec![Stmt::Expr(Expr::Ternary {
                cond: Box::new(Expr::Ident("a".into())),
                then: Box::new(Expr::Ident("b".into())),
                else_: Box::new(Expr::Ident("c".into())),
            })]
        );
    }

    #[test]
    fn compound_assign_and_incdec() {
        assert_eq!(
            parse("x += 1;").unwrap(),
            vec![Stmt::Expr(Expr::CompoundAssign {
                name: "x".into(),
                op: BinOp::Add,
                rhs: Box::new(Expr::Num(NumLit::Int(1))),
            })]
        );
        assert_eq!(
            parse("++x;").unwrap(),
            vec![Stmt::Expr(Expr::IncDec { name: "x".into(), inc: true, prefix: true })]
        );
        assert_eq!(
            parse("y--;").unwrap(),
            vec![Stmt::Expr(Expr::IncDec { name: "y".into(), inc: false, prefix: false })]
        );
    }

    #[test]
    fn for_do_while_break_continue() {
        let stmts = parse("for (var i = 0; i < 10; i = i + 1) { break; }").unwrap();
        assert_eq!(stmts.len(), 1);
        assert!(matches!(stmts[0], Stmt::For { .. }));
        let stmts = parse("do { continue; } while (x < 5);").unwrap();
        assert!(matches!(stmts[0], Stmt::DoWhile { .. }));
    }

    #[test]
    fn array_literal() {
        assert_eq!(
            parse("[1, 2, 3];").unwrap(),
            vec![Stmt::Expr(Expr::Arr(vec![
                Expr::Num(NumLit::Int(1)),
                Expr::Num(NumLit::Int(2)),
                Expr::Num(NumLit::Int(3)),
            ]))]
        );
    }

    #[test]
    fn object_literal() {
        // 文頭の `{` はブロックなので、式文脈にするため括弧で包む（JS と同一の曖昧性解決）
        assert_eq!(
            parse("({a: 1, b: \"x\"});").unwrap(),
            vec![Stmt::Expr(Expr::ObjLit(vec![
                ("a".into(), Expr::Num(NumLit::Int(1))),
                ("b".into(), Expr::Str("x".into())),
            ]))]
        );
    }

    #[test]
    fn member_and_index_access() {
        assert_eq!(
            parse("o.a;").unwrap(),
            vec![Stmt::Expr(Expr::Member {
                obj: Box::new(Expr::Ident("o".into())),
                name: "a".into(),
            })]
        );
        assert_eq!(
            parse("a[0];").unwrap(),
            vec![Stmt::Expr(Expr::Index {
                obj: Box::new(Expr::Ident("a".into())),
                index: Box::new(Expr::Num(NumLit::Int(0))),
            })]
        );
    }

    #[test]
    fn index_and_member_assign() {
        assert_eq!(
            parse("a[0] = 5;").unwrap(),
            vec![Stmt::Expr(Expr::IndexAssign {
                obj: Box::new(Expr::Ident("a".into())),
                index: Box::new(Expr::Num(NumLit::Int(0))),
                rhs: Box::new(Expr::Num(NumLit::Int(5))),
            })]
        );
        assert_eq!(
            parse("o.x = 1;").unwrap(),
            vec![Stmt::Expr(Expr::MemberAssign {
                obj: Box::new(Expr::Ident("o".into())),
                name: "x".into(),
                rhs: Box::new(Expr::Num(NumLit::Int(1))),
            })]
        );
    }
}
