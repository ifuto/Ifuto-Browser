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
    /// ビット NOT `~`。
    BNot,
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
    /// `&`（ビット AND）
    BAnd,
    /// `|`（ビット OR）
    BOr,
    /// `^`（ビット XOR）
    BXor,
    /// `<<`（左シフト）
    BShl,
    /// `>>`（算術右シフト）
    BShr,
    /// `>>>`（論理右シフト）
    BUShr,
}

/// 論理代入演算子（`||=` / `&&=` / `??=`）。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum LogicalOp {
    /// `||=`（falsy なら代入）。
    Or,
    /// `&&=`（truthy なら代入）。
    And,
    /// `??=`（nullish なら代入）。
    Nullish,
}

/// オブジェクトリテラルの 1 エントリ。
#[derive(Clone, Debug, PartialEq)]
pub enum ObjEntry {
    /// 通常の `key: value`。
    KeyValue(String, Expr),
    /// getter `get name() { body }`。
    Getter(String, Vec<Stmt>),
    /// setter `set name(v) { body }`（パラメータ名 + 本体）。
    Setter(String, String, Vec<Stmt>),
    /// オブジェクト spread `...expr`。
    Spread(Expr),
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
    /// オブジェクトリテラル `{ k: v, get/set, ...spread }`。
    ObjLit(Vec<ObjEntry>),
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
        /// rest パラメータ（末尾の `...name`）。
        rest: Option<String>,
        /// 本体。
        body: Vec<Stmt>,
    },
    /// アロー関数 `(params) => expr` または `(params) => { body }`。
    Arrow {
        /// パラメータ名。
        params: Vec<String>,
        /// rest パラメータ。
        rest: Option<String>,
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
    /// `instanceof` 演算子 `lhs instanceof rhs`。
    Instanceof {
        /// 左辺。
        lhs: Box<Expr>,
        /// 右辺。
        rhs: Box<Expr>,
    },
    /// `in` 演算子 `key in obj`。
    In {
        /// キー。
        key: Box<Expr>,
        /// オブジェクト。
        obj: Box<Expr>,
    },
    /// `delete` 演算子 `delete target`。
    Delete {
        /// 削除対象（メンバー/インデックス）。
        target: Box<Expr>,
    },
    /// `new Callee(args)` コンストラクタ呼び出し。
    New {
        /// コンストラクタ式。
        callee: Box<Expr>,
        /// 引数列。
        args: Vec<Expr>,
    },
    /// spread 要素 `...expr`（配列リテラル・関数呼び出し引数）。
    Spread(Box<Expr>),
    /// rest 要素（関数パラメータ・分割代入）。
    Rest(String),
    /// 配列リテラルの空き要素 `[,,]`（elision = undefined）。
    Hole,
    /// 正規表現リテラル `/pat/flags`。
    Regex {
        /// パターン文字列。
        pattern: String,
        /// フラグ文字列（"gim" 等）。
        flags: String,
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
    /// 複合代入 `obj.prop op= rhs`（メンバー対象。`obj` は 1 回だけ評価）。
    MemberCompoundAssign {
        /// 対象オブジェクト。
        obj: Box<Expr>,
        /// プロパティ名。
        name: String,
        /// 演算子。
        op: BinOp,
        /// 右辺。
        rhs: Box<Expr>,
    },
    /// 複合代入 `obj[index] op= rhs`（インデックス対象。`obj`/`index` は 1 回評価）。
    IndexCompoundAssign {
        /// 対象オブジェクト。
        obj: Box<Expr>,
        /// インデックス式。
        index: Box<Expr>,
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
    /// メンバーの前置/後置インクリメント・デクリメント `obj.name++` / `--obj.name`。
    MemberIncDec {
        /// 対象オブジェクト。
        obj: Box<Expr>,
        /// プロパティ名。
        name: String,
        /// true = `++`、false = `--`。
        inc: bool,
        /// true = 前置、false = 後置。
        prefix: bool,
    },
    /// インデックスの前置/後置インクリメント・デクリメント `obj[i]++` / `--obj[i]`。
    IndexIncDec {
        /// 対象オブジェクト。
        obj: Box<Expr>,
        /// インデックス式。
        index: Box<Expr>,
        /// true = `++`、false = `--`。
        inc: bool,
        /// true = 前置、false = 後置。
        prefix: bool,
    },
    /// 論理代入 `x ||= y` / `x &&= y` / `x ??= y`。target は識別子 or メンバー。
    LogicalAssign {
        /// 代入先（`Ident` / `Member`）。
        target: Box<Expr>,
        /// 演算子。
        op: LogicalOp,
        /// 右辺。
        rhs: Box<Expr>,
    },
    /// 分割代入（式文 `[a, b] = expr` / `({a} = expr)`）。`var` 宣言を伴わない形。
    DestructureAssign {
        /// 束縛パターン。
        pattern: Pattern,
        /// 右辺。
        rhs: Box<Expr>,
    },
    /// `super(args)`（派生クラスのコンストラクタで親コンストラクタを this で呼ぶ）。
    SuperCall {
        /// 親クラス名（グローバル名）。
        parent: String,
        /// 引数列。
        args: Vec<Expr>,
    },
    /// `yield [expr]`（ジェネレータ。operand は無ければ None = undefined）。
    Yield(Option<Box<Expr>>),
    /// `await expr`（async 関数。解決済み Promise を unwrap する）。
    Await(Box<Expr>),
    /// シーケンス式 `(a, b, c)`（コンマ演算子。左から評価して最後の値を返す）。
    Seq(Vec<Expr>),
}

/// 分割代入パターン。
#[derive(Clone, Debug, PartialEq)]
pub enum Pattern {
    /// 識別子（変数名）。
    Ident(String),
    /// 配列パターン `[a, b, ...rest]`。
    Arr(Vec<Pattern>),
    /// オブジェクトパターン `{a, b: x, ...rest}`。
    Obj(Vec<(String, Pattern)>),
    /// rest 要素（配列パターンの末尾 `...rest`）。
    Rest(String),
    /// オブジェクト rest（`{a, ...rest}` の `rest`。残りのプロパティを束縛）。
    ObjRest(String),
    /// 空き要素（elision。束縛しない）。
    Hole,
}

/// パラメータリスト（通常パラメータ + rest パラメータ）。
pub type ParamList = (Vec<String>, Option<String>);

/// クラスメソッド（name, params, rest, body）。
pub type ClassMethod = (String, Vec<String>, Option<String>, Vec<Stmt>);

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
        /// rest パラメータ。
        rest: Option<String>,
        /// 本体。
        body: Vec<Stmt>,
        /// ジェネレータ関数（`function*`）か。
        is_gen: bool,
        /// async 関数（`async function`）か。
        is_async: bool,
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
    /// `for (var x in obj) body` / `for (var x of iterable) body`。
    ForIn {
        /// ループ変数名。
        name: String,
        /// 対象式。
        obj: Expr,
        /// 本体。
        body: Box<Stmt>,
        /// true = `of`（値で回す）、false = `in`（キーで回す）。
        is_of: bool,
    },
    /// `break [label];`（label は無ければ None）。
    Break(Option<String>),
    /// `continue [label];`（label は無ければ None）。
    Continue(Option<String>),
    /// ラベル文 `label: statement`（`break label` / `continue label` の対象）。
    Labeled {
        /// ラベル名。
        label: String,
        /// 本体（通常はループ）。
        body: Box<Stmt>,
    },
    /// `switch (disc) { case x: ...; default: ... }`。
    Switch {
        /// 判別式。
        disc: Expr,
        /// (case 値（None = default）, 本体) の列。
        cases: Vec<(Option<Expr>, Vec<Stmt>)>,
    },
    /// `throw expr;`。
    Throw(Expr),
    /// `try { ... } catch (e) { ... }`。
    Try {
        /// try 本体。
        try_body: Vec<Stmt>,
        /// catch パラメータ名（catch なしの finally のみは未対応）。
        catch_param: Option<String>,
        /// catch 本体。
        catch_body: Vec<Stmt>,
    },
    /// 分割代入宣言 `var [a, b] = expr;` / `var {a} = expr;`。
    Destructure {
        /// パターン。
        pattern: Pattern,
        /// 右辺。
        init: Expr,
    },
    /// class 宣言 `class C extends P { constructor() {...} method() {...} }`。
    ClassDecl {
        /// クラス名。
        name: String,
        /// 親クラス名（`extends P`。無ければ None）。
        parent: Option<String>,
        /// コンストラクタ（params, rest, body）。
        constructor: (Vec<String>, Option<String>, Vec<Stmt>),
        /// メソッド列（name, params, rest, body）。
        methods: Vec<ClassMethod>,
        /// フィールド列（name, 初期化式）。
        fields: Vec<(String, Expr)>,
    },
    /// `export` 宣言（簡易近似: 値式をエクスポートする）。
    Export {
        /// エクスポート名。
        name: String,
        /// 値式。
        value: Expr,
    },
    /// `import name from "spec"`（簡易近似: 副作用のみ・名前束縛は no-op）。
    Import {
        /// 束縛名（無ければ副作用のみ）。
        name: Option<String>,
        /// モジュール指定子。
        spec: String,
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
    /// 現在パース中の class の親クラス名（`super(...)` 解決用）。
    super_class: Option<String>,
}

impl<'a> Parser<'a> {
    /// ソースからパーサを作る。
    pub fn new(src: &'a str) -> Self {
        Self {
            lx: Lexer::new(src),
            peeked: None,
            super_class: None,
        }
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
                Expr::Ident(name) => Ok(Expr::Assign {
                    name,
                    rhs: Box::new(rhs),
                }),
                Expr::Index { obj, index } => Ok(Expr::IndexAssign {
                    obj,
                    index,
                    rhs: Box::new(rhs),
                }),
                Expr::Member { obj, name } => Ok(Expr::MemberAssign {
                    obj,
                    name,
                    rhs: Box::new(rhs),
                }),
                // 分割代入（式文 `[a, b] = expr` / `({a} = expr)`）
                Expr::Arr(_) | Expr::ObjLit(_) => {
                    let pattern = expr_to_pattern(lhs)
                        .ok_or_else(|| ParseError("invalid destructuring pattern".into()))?;
                    Ok(Expr::DestructureAssign {
                        pattern,
                        rhs: Box::new(rhs),
                    })
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
        } else if self.at_punct(Punct::ShlAss)? {
            Some(BinOp::BShl)
        } else if self.at_punct(Punct::ShrAss)? {
            Some(BinOp::BShr)
        } else if self.at_punct(Punct::UShrAss)? {
            Some(BinOp::BUShr)
        } else if self.at_punct(Punct::AndAss)? {
            Some(BinOp::BAnd)
        } else if self.at_punct(Punct::OrAss)? {
            Some(BinOp::BOr)
        } else if self.at_punct(Punct::XorAss)? {
            Some(BinOp::BXor)
        } else {
            None
        };
        if let Some(op) = op {
            self.bump()?;
            let rhs = self.parse_expr()?;
            return match lhs {
                Expr::Ident(name) => Ok(Expr::CompoundAssign {
                    name,
                    op,
                    rhs: Box::new(rhs),
                }),
                Expr::Member { obj, name } => Ok(Expr::MemberCompoundAssign {
                    obj,
                    name,
                    op,
                    rhs: Box::new(rhs),
                }),
                Expr::Index { obj, index } => Ok(Expr::IndexCompoundAssign {
                    obj,
                    index,
                    op,
                    rhs: Box::new(rhs),
                }),
                _ => Err(ParseError("invalid assignment target".into())),
            };
        }
        // 論理代入 ||= &&= ??=
        let logop = if self.at_punct(Punct::OrOrAss)? {
            Some(LogicalOp::Or)
        } else if self.at_punct(Punct::AndAndAss)? {
            Some(LogicalOp::And)
        } else if self.at_punct(Punct::NullishAss)? {
            Some(LogicalOp::Nullish)
        } else {
            None
        };
        if let Some(op) = logop {
            self.bump()?;
            let rhs = self.parse_expr()?;
            return match lhs {
                Expr::Ident(_) | Expr::Member { .. } => Ok(Expr::LogicalAssign {
                    target: Box::new(lhs),
                    op,
                    rhs: Box::new(rhs),
                }),
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
            lhs = Expr::Bin {
                op: BinOp::Or,
                lhs: Box::new(lhs),
                rhs: Box::new(rhs),
            };
        }
        Ok(lhs)
    }

    /// `&&`。
    fn parse_and(&mut self) -> Result<Expr, ParseError> {
        let mut lhs = self.parse_bitor()?;
        while self.at_punct(Punct::AndAnd)? {
            self.bump()?;
            let rhs = self.parse_bitor()?;
            lhs = Expr::Bin {
                op: BinOp::And,
                lhs: Box::new(lhs),
                rhs: Box::new(rhs),
            };
        }
        Ok(lhs)
    }

    /// `|`（ビット OR）。
    fn parse_bitor(&mut self) -> Result<Expr, ParseError> {
        let mut lhs = self.parse_bitxor()?;
        while self.at_punct(Punct::Bor)? {
            self.bump()?;
            let rhs = self.parse_bitxor()?;
            lhs = Expr::Bin {
                op: BinOp::BOr,
                lhs: Box::new(lhs),
                rhs: Box::new(rhs),
            };
        }
        Ok(lhs)
    }

    /// `^`（ビット XOR）。
    fn parse_bitxor(&mut self) -> Result<Expr, ParseError> {
        let mut lhs = self.parse_bitand()?;
        while self.at_punct(Punct::Bxor)? {
            self.bump()?;
            let rhs = self.parse_bitand()?;
            lhs = Expr::Bin {
                op: BinOp::BXor,
                lhs: Box::new(lhs),
                rhs: Box::new(rhs),
            };
        }
        Ok(lhs)
    }

    /// `&`（ビット AND）。
    fn parse_bitand(&mut self) -> Result<Expr, ParseError> {
        let mut lhs = self.parse_equality()?;
        while self.at_punct(Punct::Band)? {
            self.bump()?;
            let rhs = self.parse_equality()?;
            lhs = Expr::Bin {
                op: BinOp::BAnd,
                lhs: Box::new(lhs),
                rhs: Box::new(rhs),
            };
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
            lhs = Expr::Bin {
                op,
                lhs: Box::new(lhs),
                rhs: Box::new(rhs),
            };
        }
        Ok(lhs)
    }

    /// 比較 `< <= > >=` + `instanceof` + `in`。
    fn parse_relational(&mut self) -> Result<Expr, ParseError> {
        let mut lhs = self.parse_shift()?;
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
            let Some(op) = op else {
                // instanceof / in（比較と同レベル）
                if self.at_kw(Keyword::Instanceof)? {
                    self.bump()?;
                    let rhs = self.parse_shift()?;
                    lhs = Expr::Instanceof {
                        lhs: Box::new(lhs),
                        rhs: Box::new(rhs),
                    };
                    continue;
                }
                if self.at_kw(Keyword::In)? {
                    self.bump()?;
                    let rhs = self.parse_shift()?;
                    lhs = Expr::In {
                        key: Box::new(lhs),
                        obj: Box::new(rhs),
                    };
                    continue;
                }
                break;
            };
            self.bump()?;
            let rhs = self.parse_shift()?;
            lhs = Expr::Bin {
                op,
                lhs: Box::new(lhs),
                rhs: Box::new(rhs),
            };
        }
        Ok(lhs)
    }

    /// シフト `<< >> >>>`。
    fn parse_shift(&mut self) -> Result<Expr, ParseError> {
        let mut lhs = self.parse_additive()?;
        loop {
            let op = if self.at_punct(Punct::Shl)? {
                Some(BinOp::BShl)
            } else if self.at_punct(Punct::Shr)? {
                Some(BinOp::BShr)
            } else if self.at_punct(Punct::UShr)? {
                Some(BinOp::BUShr)
            } else {
                None
            };
            let Some(op) = op else { break };
            self.bump()?;
            let rhs = self.parse_additive()?;
            lhs = Expr::Bin {
                op,
                lhs: Box::new(lhs),
                rhs: Box::new(rhs),
            };
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
            lhs = Expr::Bin {
                op,
                lhs: Box::new(lhs),
                rhs: Box::new(rhs),
            };
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
            lhs = Expr::Bin {
                op,
                lhs: Box::new(lhs),
                rhs: Box::new(rhs),
            };
        }
        Ok(lhs)
    }

    /// 単項 `! - + typeof` + 前置 `++`/`--` + `await`。
    fn parse_unary(&mut self) -> Result<Expr, ParseError> {
        // await（単項と同じ優先順位。右結合）
        if self.at_kw(Keyword::Await)? {
            self.bump()?;
            let operand = self.parse_unary()?;
            return Ok(Expr::Await(Box::new(operand)));
        }
        let op = if self.at_punct(Punct::Bang)? {
            Some(UnaryOp::Not)
        } else if self.at_punct(Punct::Minus)? {
            Some(UnaryOp::Neg)
        } else if self.at_punct(Punct::Plus)? {
            Some(UnaryOp::Pos)
        } else if self.at_punct(Punct::Bnot)? {
            Some(UnaryOp::BNot)
        } else if self.at_kw(Keyword::Typeof)? {
            Some(UnaryOp::Typeof)
        } else {
            None
        };
        if let Some(op) = op {
            self.bump()?;
            let operand = self.parse_unary()?;
            return Ok(Expr::Unary {
                op,
                operand: Box::new(operand),
            });
        }
        // delete
        if self.at_kw(Keyword::Delete)? {
            self.bump()?;
            let target = self.parse_unary()?;
            return Ok(Expr::Delete {
                target: Box::new(target),
            });
        }
        // new 演算子
        if self.at_kw(Keyword::New)? {
            self.bump()?;
            // callee: メンバーアクセスまで（呼び出しは含まない）
            let callee = self.parse_new_callee()?;
            let mut args = Vec::new();
            if self.at_punct(Punct::LParen)? {
                self.bump()?;
                if !self.at_punct(Punct::RParen)? {
                    loop {
                        if self.eat_punct(Punct::Ellipsis)? {
                            let e = self.parse_expr()?;
                            args.push(Expr::Spread(Box::new(e)));
                        } else {
                            args.push(self.parse_expr()?);
                        }
                        if !self.eat_punct(Punct::Comma)? {
                            break;
                        }
                    }
                }
                self.expect_punct(Punct::RParen, "')'")?;
            }
            let base = Expr::New {
                callee: Box::new(callee),
                args,
            };
            // `new Foo(...).method(...)` / `new Foo(...)[i]` の後置連鎖
            return self.parse_postfix_suffix(base);
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
                Expr::Ident(name) => Ok(Expr::IncDec {
                    name,
                    inc,
                    prefix: true,
                }),
                Expr::Member { obj, name } => Ok(Expr::MemberIncDec {
                    obj,
                    name,
                    inc,
                    prefix: true,
                }),
                Expr::Index { obj, index } => Ok(Expr::IndexIncDec {
                    obj,
                    index,
                    inc,
                    prefix: true,
                }),
                _ => Err(ParseError("invalid increment/decrement target".into())),
            };
        }
        self.parse_postfix()
    }

    /// 後置（呼び出し `()`・メンバー `.prop`・インデックス `[i]`）。
    fn parse_postfix(&mut self) -> Result<Expr, ParseError> {
        let base = self.parse_primary()?;
        self.parse_postfix_suffix(base)
    }

    /// 後置演算の連鎖（`base` に対する `.prop` / `[i]` / 呼び出し / `++`/`--`）。
    /// `new` 式の結果にも適用できるよう `parse_postfix` から分離。
    fn parse_postfix_suffix(&mut self, mut base: Expr) -> Result<Expr, ParseError> {
        loop {
            if self.at_punct(Punct::LParen)? {
                self.bump()?;
                let mut args = Vec::new();
                if !self.at_punct(Punct::RParen)? {
                    loop {
                        if self.eat_punct(Punct::Ellipsis)? {
                            let e = self.parse_expr()?;
                            args.push(Expr::Spread(Box::new(e)));
                        } else {
                            args.push(self.parse_expr()?);
                        }
                        if !self.eat_punct(Punct::Comma)? {
                            break;
                        }
                    }
                }
                self.expect_punct(Punct::RParen, "')'")?;
                base = Expr::Call {
                    callee: Box::new(base),
                    args,
                };
            } else if self.at_punct(Punct::Dot)? {
                self.bump()?;
                let name = match self.bump()? {
                    Token::Ident(n) => n.to_string(),
                    Token::Kw(kw) => format!("{kw:?}").to_lowercase(),
                    other => {
                        return Err(ParseError(format!(
                            "expected property name after '.', got {other:?}"
                        )))
                    }
                };
                base = Expr::Member {
                    obj: Box::new(base),
                    name,
                };
            } else if self.at_punct(Punct::LBracket)? {
                self.bump()?;
                let index = self.parse_expr()?;
                self.expect_punct(Punct::RBracket, "']'")?;
                base = Expr::Index {
                    obj: Box::new(base),
                    index: Box::new(index),
                };
            } else if self.at_punct(Punct::Inc)? || self.at_punct(Punct::Dec)? {
                // 後置 ++ / --
                let inc = self.at_punct(Punct::Inc)?;
                self.bump()?;
                return match base {
                    Expr::Ident(name) => Ok(Expr::IncDec {
                        name,
                        inc,
                        prefix: false,
                    }),
                    Expr::Member { obj, name } => Ok(Expr::MemberIncDec {
                        obj,
                        name,
                        inc,
                        prefix: false,
                    }),
                    Expr::Index { obj, index } => Ok(Expr::IndexIncDec {
                        obj,
                        index,
                        inc,
                        prefix: false,
                    }),
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
            Token::Kw(Keyword::This) => Ok(Expr::This),
            Token::Kw(Keyword::Super) => {
                // super(args): 親コンストラクタ呼び出し（this で呼ぶ）
                let parent = self
                    .super_class
                    .clone()
                    .ok_or_else(|| ParseError("super outside class".into()))?;
                self.expect_punct(Punct::LParen, "'('")?;
                let mut args = Vec::new();
                if !self.at_punct(Punct::RParen)? {
                    loop {
                        if self.eat_punct(Punct::Ellipsis)? {
                            args.push(Expr::Spread(Box::new(self.parse_expr()?)));
                        } else {
                            args.push(self.parse_expr()?);
                        }
                        if !self.eat_punct(Punct::Comma)? {
                            break;
                        }
                    }
                }
                self.expect_punct(Punct::RParen, "')'")?;
                Ok(Expr::SuperCall { parent, args })
            }
            Token::Kw(Keyword::Yield) => {
                // yield [expr]（operand 省略時は undefined）
                let operand = if self.at_punct(Punct::Semi)?
                    || self.at_punct(Punct::RBrace)?
                    || self.at_eof()?
                {
                    None
                } else {
                    Some(Box::new(self.parse_expr()?))
                };
                Ok(Expr::Yield(operand))
            }
            Token::Ident(name) => {
                // アロー関数の単一引数形: `x => body`
                if self.at_punct(Punct::Arrow)? {
                    self.bump()?;
                    let body = self.parse_arrow_body()?;
                    return Ok(Expr::Arrow {
                        params: vec![name.to_string()],
                        rest: None,
                        body: Box::new(body),
                    });
                }
                Ok(Expr::Ident(name.to_string()))
            }
            Token::Kw(Keyword::Function) => self.parse_func_expr(),
            Token::Punct(Punct::LParen) => {
                // アロー関数 `(params) => body` か、グループ式 `(expr)` かの先読み
                let saved = self.save_state();
                if let Some((params, rest)) = self.try_parse_params()? {
                    if self.at_punct(Punct::Arrow)? {
                        self.bump()?;
                        let body = self.parse_arrow_body()?;
                        return Ok(Expr::Arrow {
                            params,
                            rest,
                            body: Box::new(body),
                        });
                    }
                }
                // ロールバックしてグループ式
                self.restore_state(saved);
                let e = self.parse_expr()?;
                // シーケンス式 `(a, b, c)`（コンマ演算子。括弧内のみ。関数呼び出しの
                // 引数区切りと衝突しないよう、グループ式の内側に限定する）。
                if self.at_punct(Punct::Comma)? {
                    let mut items = vec![e];
                    while self.eat_punct(Punct::Comma)? {
                        items.push(self.parse_expr()?);
                    }
                    self.expect_punct(Punct::RParen, "')'")?;
                    return Ok(Expr::Seq(items));
                }
                self.expect_punct(Punct::RParen, "')'")?;
                Ok(e)
            }
            Token::Punct(Punct::LBrace) => {
                // オブジェクトリテラル { k: v, get/set name(), ...spread }
                let mut entries = Vec::new();
                if !self.at_punct(Punct::RBrace)? {
                    loop {
                        // spread
                        if self.eat_punct(Punct::Ellipsis)? {
                            let e = self.parse_expr()?;
                            entries.push(ObjEntry::Spread(e));
                        } else {
                            // getter/setter 先読み（get/set は識別子。名前 + '(' ならアクセサ）
                            let saved = self.save_state();
                            let mut accessor: Option<(bool, String, String, Vec<Stmt>)> = None;
                            if let Token::Ident(k) = self.peek()?.clone() {
                                let k = k.to_string();
                                if k == "get" || k == "set" {
                                    let is_get = k == "get";
                                    self.bump()?; // get/set
                                    if let Token::Ident(name) = self.peek()?.clone() {
                                        self.bump()?; // name
                                        if self.at_punct(Punct::LParen)? {
                                            self.bump()?; // (
                                            let (params, _) = self.parse_params_list()?;
                                            self.expect_punct(Punct::RParen, "')'")?;
                                            let body = match self.parse_block()? {
                                                Stmt::Block(s) => s,
                                                _ => unreachable!(),
                                            };
                                            let param = if is_get {
                                                String::new()
                                            } else {
                                                params.first().cloned().unwrap_or_default()
                                            };
                                            accessor =
                                                Some((is_get, name.to_string(), param, body));
                                        }
                                    }
                                }
                            }
                            match accessor {
                                Some((true, name, _p, body)) => {
                                    entries.push(ObjEntry::Getter(name, body))
                                }
                                Some((false, name, p, body)) => {
                                    entries.push(ObjEntry::Setter(name, p, body))
                                }
                                None => {
                                    self.restore_state(saved);
                                    let key = match self.bump()? {
                                        Token::Ident(n) => n.to_string(),
                                        Token::Str(s) => s,
                                        Token::Kw(kw) => format!("{kw:?}").to_lowercase(),
                                        other => {
                                            return Err(ParseError(format!(
                                                "expected property key, got {other:?}"
                                            )))
                                        }
                                    };
                                    self.expect_punct(Punct::Colon, "':'")?;
                                    let val = self.parse_expr()?;
                                    entries.push(ObjEntry::KeyValue(key, val));
                                }
                            }
                        }
                        if !self.eat_punct(Punct::Comma)? {
                            break;
                        }
                        if self.at_punct(Punct::RBrace)? {
                            break; // trailing comma
                        }
                    }
                }
                self.expect_punct(Punct::RBrace, "'}'")?;
                Ok(Expr::ObjLit(entries))
            }
            Token::Punct(Punct::LBracket) => {
                // 配列リテラル [ e, ... ]（spread・elision 対応）
                let mut items = Vec::new();
                if !self.at_punct(Punct::RBracket)? {
                    loop {
                        if self.at_punct(Punct::Comma)? {
                            // エルジョン: 空き要素
                            items.push(Expr::Hole);
                            self.bump()?;
                            if self.at_punct(Punct::RBracket)? {
                                break;
                            }
                            continue;
                        }
                        if self.eat_punct(Punct::Ellipsis)? {
                            let e = self.parse_expr()?;
                            items.push(Expr::Spread(Box::new(e)));
                        } else {
                            items.push(self.parse_expr()?);
                        }
                        if !self.eat_punct(Punct::Comma)? {
                            break;
                        }
                        if self.at_punct(Punct::RBracket)? {
                            break; // trailing comma
                        }
                    }
                }
                self.expect_punct(Punct::RBracket, "']'")?;
                Ok(Expr::Arr(items))
            }
            Token::Punct(Punct::Slash) => {
                // 正規表現リテラル /pat/flags（式の開始位置の '/' のみ）
                self.parse_regex_literal()
            }
            other => Err(ParseError(format!(
                "unexpected token in expression: {other:?}"
            ))),
        }
    }

    /// 正規表現リテラル `/pat/flags` をパース（`/` は消費済み）。
    fn parse_regex_literal(&mut self) -> Result<Expr, ParseError> {
        // パターン: エスケープを考慮して '/' までスキャン
        let mut pattern = String::new();
        let mut in_class = false;
        loop {
            let c = self.lx.cur_char();
            if c == 0 {
                return Err(ParseError("unterminated regexp literal".into()));
            }
            if c == b'\\' {
                // エスケープ: 次文字もそのまま含める
                self.lx.advance_raw();
                pattern.push(c as char);
                let e = self.lx.cur_char();
                if e == 0 {
                    return Err(ParseError("unterminated regexp literal".into()));
                }
                pattern.push(e as char);
                self.lx.advance_raw();
                continue;
            }
            if c == b'[' {
                in_class = true;
            } else if c == b']' {
                in_class = false;
            } else if c == b'/' && !in_class {
                self.lx.advance_raw();
                break;
            }
            pattern.push(c as char);
            self.lx.advance_raw();
        }
        // フラグ
        let mut flags = String::new();
        loop {
            let c = self.lx.cur_char();
            if c != 0 && c.is_ascii_alphabetic() {
                flags.push(c as char);
                self.lx.advance_raw();
            } else {
                break;
            }
        }
        Ok(Expr::Regex { pattern, flags })
    }

    /// パラメータ列をパース（`(...)` の内側）。rest パラメータ対応。
    /// 戻り値は (params, rest)。
    fn parse_params_list(&mut self) -> Result<ParamList, ParseError> {
        let mut params = Vec::new();
        let mut rest = None;
        if !self.at_punct(Punct::RParen)? {
            loop {
                if self.eat_punct(Punct::Ellipsis)? {
                    match self.bump()? {
                        Token::Ident(n) => {
                            rest = Some(n.to_string());
                        }
                        other => {
                            return Err(ParseError(format!("expected rest name, got {other:?}")))
                        }
                    }
                    break; // rest は末尾
                }
                match self.bump()? {
                    Token::Ident(p) => params.push(p.to_string()),
                    other => return Err(ParseError(format!("expected parameter, got {other:?}"))),
                }
                if !self.eat_punct(Punct::Comma)? {
                    break;
                }
            }
        }
        Ok((params, rest))
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
        let (params, rest) = self.parse_params_list()?;
        self.expect_punct(Punct::RParen, "')'")?;
        let body = match self.parse_block()? {
            Stmt::Block(s) => s,
            _ => unreachable!(),
        };
        Ok(Expr::FuncExpr {
            name,
            params,
            rest,
            body,
        })
    }

    /// `new` の callee をパース（メンバーアクセスの連鎖。呼び出しは含まない）。
    fn parse_new_callee(&mut self) -> Result<Expr, ParseError> {
        let mut base = self.parse_primary()?;
        // メンバーアクセスのみ畳む（`.` `[ ]`）。呼び出しは含めない
        loop {
            if self.at_punct(Punct::Dot)? {
                self.bump()?;
                let name = match self.bump()? {
                    Token::Ident(n) => n.to_string(),
                    other => {
                        return Err(ParseError(format!("expected property name, got {other:?}")))
                    }
                };
                base = Expr::Member {
                    obj: Box::new(base),
                    name,
                };
            } else if self.at_punct(Punct::LBracket)? {
                self.bump()?;
                let index = self.parse_expr()?;
                self.expect_punct(Punct::RBracket, "']'")?;
                base = Expr::Index {
                    obj: Box::new(base),
                    index: Box::new(index),
                };
            } else {
                break;
            }
        }
        Ok(base)
    }

    /// `=> body` の本体（`=>` は消費済み）。式なら暗黙 return、ブロックは文列。
    fn parse_arrow_body(&mut self) -> Result<Expr, ParseError> {
        if self.at_punct(Punct::LBrace)? {
            // ブロック本体 → Return 文に変換して 1 つの Expr::Block 風に包む
            // （Expr::Arrow の body は Box<Expr> なので、Block を表す専用式は持たず、
            // ここでは式を返す。ブロック本体は codegen 側で特別扱いしないため、
            // 式のみサポートし、ブロックは未対応とする）
            return Err(ParseError(
                "arrow function with block body is not yet supported".into(),
            ));
        }
        self.parse_expr()
    }

    /// `(params)` を試す（アロー関数用）。成功なら (params, rest)、失敗（グループ式）なら None。
    /// 呼び出し時点で `(` は既に消費済み。
    fn try_parse_params(&mut self) -> Result<Option<ParamList>, ParseError> {
        // 空の `()` はアロー（params 空）にもグループ（空は不正）にもなり得るが、
        // `() => ...` のみ有効なので空でも params として扱う。
        if self.at_punct(Punct::RParen)? {
            self.bump()?;
            return Ok(Some((Vec::new(), None)));
        }
        let mut params = Vec::new();
        let mut rest = None;
        loop {
            match self.peek()? {
                Token::Ident(_) => {
                    let p = match self.bump()? {
                        Token::Ident(n) => n.to_string(),
                        _ => unreachable!(),
                    };
                    params.push(p);
                }
                Token::Punct(Punct::Ellipsis) => {
                    self.bump()?;
                    match self.bump()? {
                        Token::Ident(n) => rest = Some(n.to_string()),
                        _ => return Ok(None),
                    }
                }
                _ => return Ok(None), // グループ式（識別子でない）
            }
            if self.eat_punct(Punct::Comma)? {
                continue;
            }
            if self.at_punct(Punct::RParen)? {
                self.bump()?;
                return Ok(Some((params, rest)));
            }
            return Ok(None);
        }
    }

    /// 文をパースする。
    fn parse_stmt(&mut self) -> Result<Stmt, ParseError> {
        // ラベル文 `label: statement`（識別子 + ':'。キーワードは識別子ではないため
        // 衝突しない。`break label` / `continue label` の対象）。
        if let Token::Ident(_) = self.peek()? {
            let saved = self.save_state();
            let label = match self.bump()? {
                Token::Ident(n) => n.to_string(),
                _ => unreachable!(),
            };
            if self.eat_punct(Punct::Colon)? {
                let body = self.parse_stmt()?;
                return Ok(Stmt::Labeled {
                    label,
                    body: Box::new(body),
                });
            }
            self.restore_state(saved);
        }
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
        if self.at_kw(Keyword::Var)? || self.at_kw(Keyword::Let)? || self.at_kw(Keyword::Const)? {
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
            // ラベル付き break（改行を挟む場合はラベルとみなさない簡易近似）
            let label = if let Token::Ident(n) = self.peek()? {
                let n = n.to_string();
                self.bump()?;
                Some(n)
            } else {
                None
            };
            if self.at_punct(Punct::Semi)? {
                self.bump()?;
            }
            return Ok(Stmt::Break(label));
        }
        if self.at_kw(Keyword::Continue)? {
            self.bump()?;
            let label = if let Token::Ident(n) = self.peek()? {
                let n = n.to_string();
                self.bump()?;
                Some(n)
            } else {
                None
            };
            if self.at_punct(Punct::Semi)? {
                self.bump()?;
            }
            return Ok(Stmt::Continue(label));
        }
        if self.at_kw(Keyword::Async)? {
            self.bump()?; // async
            if self.at_kw(Keyword::Function)? {
                self.bump()?; // function
                return self.parse_func_decl_rest(false, true);
            }
            return Err(ParseError(
                "async only supports function declarations".into(),
            ));
        }
        if self.at_kw(Keyword::Function)? {
            self.bump()?; // function
            let is_gen = self.eat_punct(Punct::Star)?;
            return self.parse_func_decl_rest(is_gen, false);
        }
        if self.at_kw(Keyword::Switch)? {
            return self.parse_switch();
        }
        if self.at_kw(Keyword::Throw)? {
            return self.parse_throw();
        }
        if self.at_kw(Keyword::Try)? {
            return self.parse_try();
        }
        if self.at_kw(Keyword::Class)? {
            return self.parse_class();
        }
        if self.at_kw(Keyword::Import)? {
            return self.parse_import();
        }
        if self.at_kw(Keyword::Export)? {
            return self.parse_export();
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

    /// `var` / `let` / `const` 宣言（分割代入含む）。
    fn parse_var_decl(&mut self) -> Result<Stmt, ParseError> {
        self.bump()?; // var/let/const
                      // 分割代入: var [a, b] = expr / var {a} = expr
        if self.at_punct(Punct::LBracket)? || self.at_punct(Punct::LBrace)? {
            let pattern = self.parse_pattern()?;
            if !self.eat_punct(Punct::Assign)? {
                return Err(ParseError("expected '=' in destructuring".into()));
            }
            let init = self.parse_expr()?;
            if self.at_punct(Punct::Semi)? {
                self.bump()?;
            }
            return Ok(Stmt::Destructure { pattern, init });
        }
        // 複数の宣言子 `var a = 1, b = 2, c;`（ES5 慣用。lodash 等が依存）。
        let mut decls: Vec<Stmt> = Vec::new();
        loop {
            let name = match self.bump()? {
                Token::Ident(n) => n.to_string(),
                other => return Err(ParseError(format!("expected identifier, got {other:?}"))),
            };
            let init = if self.eat_punct(Punct::Assign)? {
                Some(self.parse_expr()?)
            } else {
                None
            };
            decls.push(Stmt::Var { name, init });
            if !self.eat_punct(Punct::Comma)? {
                break;
            }
        }
        if self.at_punct(Punct::Semi)? {
            self.bump()?;
        }
        if decls.len() == 1 {
            Ok(decls.pop().unwrap())
        } else {
            Ok(Stmt::Block(decls))
        }
    }

    /// 分割代入パターン（配列 `[a, b, ...r]` / オブジェクト `{a, b: x}`）。
    fn parse_pattern(&mut self) -> Result<Pattern, ParseError> {
        if self.at_punct(Punct::LBracket)? {
            self.bump()?;
            let mut items = Vec::new();
            if !self.at_punct(Punct::RBracket)? {
                loop {
                    if self.eat_punct(Punct::Ellipsis)? {
                        let name = match self.bump()? {
                            Token::Ident(n) => n.to_string(),
                            other => {
                                return Err(ParseError(format!(
                                    "expected rest name, got {other:?}"
                                )))
                            }
                        };
                        items.push(Pattern::Rest(name));
                        break; // rest は末尾
                    }
                    // 空き要素（elision）: コンマが連続
                    if self.at_punct(Punct::Comma)? {
                        items.push(Pattern::Hole);
                        self.bump()?;
                        if self.at_punct(Punct::RBracket)? {
                            break;
                        }
                        continue;
                    }
                    // ネスト配列/オブジェクトパターン
                    if self.at_punct(Punct::LBracket)? || self.at_punct(Punct::LBrace)? {
                        items.push(self.parse_pattern()?);
                    } else {
                        let name = match self.bump()? {
                            Token::Ident(n) => n.to_string(),
                            other => {
                                return Err(ParseError(format!(
                                    "expected pattern name, got {other:?}"
                                )))
                            }
                        };
                        items.push(Pattern::Ident(name));
                    }
                    if !self.eat_punct(Punct::Comma)? {
                        break;
                    }
                }
            }
            self.expect_punct(Punct::RBracket, "']'")?;
            Ok(Pattern::Arr(items))
        } else if self.at_punct(Punct::LBrace)? {
            self.bump()?;
            let mut items = Vec::new();
            if !self.at_punct(Punct::RBrace)? {
                loop {
                    // オブジェクト rest `...rest`
                    if self.eat_punct(Punct::Ellipsis)? {
                        let name = match self.bump()? {
                            Token::Ident(n) => n.to_string(),
                            other => {
                                return Err(ParseError(format!(
                                    "expected rest name, got {other:?}"
                                )))
                            }
                        };
                        items.push((String::new(), Pattern::ObjRest(name)));
                        break;
                    }
                    let key = match self.bump()? {
                        Token::Ident(n) => n.to_string(),
                        Token::Str(s) => s,
                        other => {
                            return Err(ParseError(format!("expected pattern key, got {other:?}")))
                        }
                    };
                    // {a: x} の形（x はパターン）
                    let val = if self.eat_punct(Punct::Colon)? {
                        if self.at_punct(Punct::LBracket)? || self.at_punct(Punct::LBrace)? {
                            self.parse_pattern()?
                        } else {
                            match self.bump()? {
                                Token::Ident(n) => Pattern::Ident(n.to_string()),
                                other => {
                                    return Err(ParseError(format!(
                                        "expected pattern, got {other:?}"
                                    )))
                                }
                            }
                        }
                    } else {
                        // {a} の shorthand
                        Pattern::Ident(key.clone())
                    };
                    items.push((key, val));
                    if !self.eat_punct(Punct::Comma)? {
                        break;
                    }
                }
            }
            self.expect_punct(Punct::RBrace, "'}'")?;
            Ok(Pattern::Obj(items))
        } else {
            Err(ParseError("expected destructuring pattern".into()))
        }
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
        Ok(Stmt::If {
            cond,
            then: Box::new(then),
            else_,
        })
    }

    /// `for (init; cond; step) body` / `for (var x in obj) body` / `for (var x of it) body`。
    fn parse_for(&mut self) -> Result<Stmt, ParseError> {
        self.bump()?; // for
        self.expect_punct(Punct::LParen, "'('")?;
        // for-in / for-of: `for (var x in obj)` / `for (var x of it)`
        if self.at_kw(Keyword::Var)? || self.at_kw(Keyword::Let)? || self.at_kw(Keyword::Const)? {
            self.bump()?; // var/let/const
            let name = match self.bump()? {
                Token::Ident(n) => n.to_string(),
                other => return Err(ParseError(format!("expected identifier, got {other:?}"))),
            };
            // in / of 判定
            if self.at_kw(Keyword::In)? {
                self.bump()?;
                let obj = self.parse_expr()?;
                self.expect_punct(Punct::RParen, "')'")?;
                let body = self.parse_stmt()?;
                return Ok(Stmt::ForIn {
                    name,
                    obj,
                    body: Box::new(body),
                    is_of: false,
                });
            }
            if self.at_kw(Keyword::Of)? {
                self.bump()?;
                let obj = self.parse_expr()?;
                self.expect_punct(Punct::RParen, "')'")?;
                let body = self.parse_stmt()?;
                return Ok(Stmt::ForIn {
                    name,
                    obj,
                    body: Box::new(body),
                    is_of: true,
                });
            }
            // 通常の for の var init
            let init = if self.eat_punct(Punct::Assign)? {
                Some(self.parse_expr()?)
            } else {
                None
            };
            self.expect_punct(Punct::Semi, "';'")?;
            let cond = if self.at_punct(Punct::Semi)? {
                self.bump()?;
                None
            } else {
                let c = self.parse_expr()?;
                self.expect_punct(Punct::Semi, "';'")?;
                Some(c)
            };
            let step = if self.at_punct(Punct::RParen)? {
                None
            } else {
                let s = self.parse_expr()?;
                Some(s)
            };
            self.expect_punct(Punct::RParen, "')'")?;
            let body = self.parse_stmt()?;
            return Ok(Stmt::For {
                init: Some(ForInit::Var { name, init }),
                cond,
                step,
                body: Box::new(body),
            });
        }
        // init
        let init = if self.at_punct(Punct::Semi)? {
            self.bump()?;
            None
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
        Ok(Stmt::For {
            init,
            cond,
            step,
            body: Box::new(body),
        })
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
        Ok(Stmt::DoWhile {
            body: Box::new(body),
            cond,
        })
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

    /// `throw expr;`。
    fn parse_throw(&mut self) -> Result<Stmt, ParseError> {
        self.bump()?; // throw
        let e = self.parse_expr()?;
        if self.at_punct(Punct::Semi)? {
            self.bump()?;
        }
        Ok(Stmt::Throw(e))
    }

    /// `try { ... } catch (e) { ... }`。
    fn parse_try(&mut self) -> Result<Stmt, ParseError> {
        self.bump()?; // try
        let try_body = match self.parse_block()? {
            Stmt::Block(s) => s,
            _ => unreachable!(),
        };
        if !self.at_kw(Keyword::Catch)? {
            return Err(ParseError("expected 'catch'".into()));
        }
        self.bump()?; // catch
        self.expect_punct(Punct::LParen, "'('")?;
        let catch_param = match self.bump()? {
            Token::Ident(n) => Some(n.to_string()),
            other => {
                return Err(ParseError(format!(
                    "expected catch parameter, got {other:?}"
                )))
            }
        };
        self.expect_punct(Punct::RParen, "')'")?;
        let catch_body = match self.parse_block()? {
            Stmt::Block(s) => s,
            _ => unreachable!(),
        };
        Ok(Stmt::Try {
            try_body,
            catch_param,
            catch_body,
        })
    }

    /// `while (cond) body`。
    fn parse_while(&mut self) -> Result<Stmt, ParseError> {
        self.bump()?; // while
        self.expect_punct(Punct::LParen, "'('")?;
        let cond = self.parse_expr()?;
        self.expect_punct(Punct::RParen, "')'")?;
        let body = self.parse_stmt()?;
        Ok(Stmt::While {
            cond,
            body: Box::new(body),
        })
    }

    /// `class C extends P { constructor() {...} method() {...} }`。
    fn parse_class(&mut self) -> Result<Stmt, ParseError> {
        self.bump()?; // class
        let name = match self.bump()? {
            Token::Ident(n) => n.to_string(),
            other => return Err(ParseError(format!("expected class name, got {other:?}"))),
        };
        // `extends Parent`
        let parent = if self.at_kw(Keyword::Extends)? {
            self.bump()?;
            match self.bump()? {
                Token::Ident(n) => Some(n.to_string()),
                other => return Err(ParseError(format!("expected parent class, got {other:?}"))),
            }
        } else {
            None
        };
        // メソッド/コンストラクタ本体のパース中は super を親名に解決する
        let saved_super = self.super_class.clone();
        self.super_class = parent.clone();
        let result = self.parse_class_body(name, parent);
        self.super_class = saved_super;
        result
    }

    /// class 本体 `{ ... }` をパースする（`super_class` は呼び出し側が設定済み）。
    fn parse_class_body(
        &mut self,
        name: String,
        parent: Option<String>,
    ) -> Result<Stmt, ParseError> {
        self.expect_punct(Punct::LBrace, "'{'")?;
        let mut constructor = (Vec::new(), None, Vec::new());
        let mut methods = Vec::new();
        let mut fields = Vec::new();
        while !self.at_punct(Punct::RBrace)? && !self.at_eof()? {
            // `static` は簡易近似（無視してインスタンスメンバーとして扱う）
            if self.at_kw(Keyword::Static)? {
                self.bump()?;
            }
            // メンバー名
            let mname = match self.bump()? {
                Token::Ident(n) => n.to_string(),
                Token::Kw(kw) => format!("{kw:?}").to_lowercase(),
                other => return Err(ParseError(format!("expected member name, got {other:?}"))),
            };
            // フィールド（`name = expr;`）
            if self.eat_punct(Punct::Assign)? {
                let init = self.parse_expr()?;
                if self.at_punct(Punct::Semi)? {
                    self.bump()?;
                }
                fields.push((mname, init));
                continue;
            }
            // パラメータ
            self.expect_punct(Punct::LParen, "'('")?;
            let (params, rest) = self.parse_params_list()?;
            self.expect_punct(Punct::RParen, "')'")?;
            // 本体
            let body = match self.parse_block()? {
                Stmt::Block(s) => s,
                _ => unreachable!(),
            };
            if mname == "constructor" {
                constructor = (params, rest, body);
            } else {
                methods.push((mname, params, rest, body));
            }
        }
        self.expect_punct(Punct::RBrace, "'}'")?;
        Ok(Stmt::ClassDecl {
            name,
            parent,
            constructor,
            methods,
            fields,
        })
    }

    /// `import name from "spec"`（簡易近似）。
    fn parse_import(&mut self) -> Result<Stmt, ParseError> {
        self.bump()?; // import
                      // 名前束縛（`import x from` / `import { x } from` / `import * as x from` / `import "spec"`）
        let name = match self.peek()? {
            Token::Ident(n) => {
                let n = n.to_string();
                self.bump()?;
                // `from` を期待
                if self.at_kw(Keyword::From)? {
                    self.bump()?;
                }
                Some(n)
            }
            Token::Str(_) => None, // 副作用のみ import
            Token::Punct(Punct::LBrace) => {
                // { a, b } from "spec"
                self.bump()?;
                let first = match self.peek()? {
                    Token::Ident(n) => n.to_string(),
                    _ => return Err(ParseError("expected import name".into())),
                };
                self.bump()?;
                while self.eat_punct(Punct::Comma)? {
                    // 追加の名前は無視（簡易）
                    match self.peek()? {
                        Token::Ident(_) => {
                            self.bump()?;
                        }
                        _ => break,
                    }
                }
                self.expect_punct(Punct::RBrace, "'}'")?;
                if self.at_kw(Keyword::From)? {
                    self.bump()?;
                }
                Some(first)
            }
            _ => return Err(ParseError("expected import specifier".into())),
        };
        // モジュール指定子（文字列）
        let spec = match self.bump()? {
            Token::Str(s) => s,
            other => {
                return Err(ParseError(format!(
                    "expected module specifier, got {other:?}"
                )))
            }
        };
        if self.at_punct(Punct::Semi)? {
            self.bump()?;
        }
        Ok(Stmt::Import { name, spec })
    }

    /// `export name`（簡易近似: 式をエクスポート）。
    fn parse_export(&mut self) -> Result<Stmt, ParseError> {
        self.bump()?; // export
                      // `export function name` / `export const name` / `export default expr`
        let name = match self.peek()? {
            Token::Ident(n) => n.to_string(),
            _ => "default".to_string(),
        };
        self.bump()?;
        // 値式（= があれば代入、なければ識別子参照）
        let value = if self.eat_punct(Punct::Assign)? {
            self.parse_expr()?
        } else {
            Expr::Ident(name.clone())
        };
        if self.at_punct(Punct::Semi)? {
            self.bump()?;
        }
        Ok(Stmt::Export { name, value })
    }

    /// `name(params) { body }`（`function` / `function*` / `async function` の
    /// `function` キーワードは呼び出し側が消費済み。`is_gen` / `is_async` は呼び出し側が判定）。
    fn parse_func_decl_rest(&mut self, is_gen: bool, is_async: bool) -> Result<Stmt, ParseError> {
        let name = match self.bump()? {
            Token::Ident(n) => n.to_string(),
            other => return Err(ParseError(format!("expected function name, got {other:?}"))),
        };
        self.expect_punct(Punct::LParen, "'('")?;
        let (params, rest) = self.parse_params_list()?;
        self.expect_punct(Punct::RParen, "')'")?;
        let body = match self.parse_block()? {
            Stmt::Block(s) => s,
            _ => unreachable!(),
        };
        Ok(Stmt::FuncDecl {
            name,
            params,
            rest,
            body,
            is_gen,
            is_async,
        })
    }
}

/// 配列/オブジェクトリテラル式を分割代入パターンへ変換する（式文の分割代入用）。
/// 対応外（spread 等）は None。
fn expr_to_pattern(expr: Expr) -> Option<Pattern> {
    match expr {
        Expr::Arr(items) => {
            let mut pats = Vec::new();
            for it in items {
                match it {
                    Expr::Ident(name) => pats.push(Pattern::Ident(name)),
                    Expr::Arr(_) | Expr::ObjLit(_) => pats.push(expr_to_pattern(it)?),
                    Expr::Hole => pats.push(Pattern::Hole),
                    _ => return None,
                }
            }
            Some(Pattern::Arr(pats))
        }
        Expr::ObjLit(entries) => {
            let mut pats = Vec::new();
            for e in entries {
                match e {
                    ObjEntry::KeyValue(key, val) => {
                        let p = match val {
                            Expr::Ident(name) => Pattern::Ident(name),
                            Expr::Arr(_) | Expr::ObjLit(_) => expr_to_pattern(val)?,
                            _ => return None,
                        };
                        pats.push((key, p));
                    }
                    _ => return None,
                }
            }
            Some(Pattern::Obj(pats))
        }
        _ => None,
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
            Stmt::FuncDecl {
                name,
                params,
                rest,
                body,
                ..
            } => {
                assert_eq!(name, "f");
                assert_eq!(params, &vec!["a".to_string()]);
                assert_eq!(rest, &None);
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
                Stmt::Expr(Expr::Unary {
                    op: UnaryOp::Neg,
                    operand: Box::new(Expr::Ident("x".into()))
                }),
                Stmt::Expr(Expr::Unary {
                    op: UnaryOp::Not,
                    operand: Box::new(Expr::Ident("y".into()))
                }),
                Stmt::Expr(Expr::Unary {
                    op: UnaryOp::Typeof,
                    operand: Box::new(Expr::Ident("z".into()))
                }),
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
        // 現在未対応の構文（テンプレートリテラル等）
        assert!(parse("`hello ${name}`;").is_err()); // テンプレートリテラルは未対応
    }

    #[test]
    fn generator_and_async_decl() {
        // function* 宣言（yield 付き）
        let stmts = parse("function* g() { yield 1; yield 2; }").unwrap();
        match &stmts[0] {
            Stmt::FuncDecl {
                is_gen,
                is_async,
                body,
                ..
            } => {
                assert!(*is_gen);
                assert!(!*is_async);
                assert_eq!(body.len(), 2);
            }
            other => panic!("expected FuncDecl, got {other:?}"),
        }
        // async function 宣言（await 付き）
        let stmts = parse("async function f() { return await 7; }").unwrap();
        match &stmts[0] {
            Stmt::FuncDecl {
                is_gen, is_async, ..
            } => {
                assert!(!*is_gen);
                assert!(*is_async);
            }
            other => panic!("expected FuncDecl, got {other:?}"),
        }
    }

    #[test]
    fn import_export() {
        let stmts = parse("import x from \"mod\";").unwrap();
        assert!(matches!(stmts[0], Stmt::Import { name: Some(_), .. }));
        let stmts = parse("export const y = 1;").unwrap();
        assert!(matches!(stmts[0], Stmt::Export { .. }));
    }

    #[test]
    fn this_and_arrow() {
        assert_eq!(parse("this;").unwrap(), vec![Stmt::Expr(Expr::This)]);
        // アロー関数（単一引数）
        let stmts = parse("var f = x => x * 2;").unwrap();
        assert!(matches!(
            stmts[0],
            Stmt::Var {
                name: _,
                init: Some(Expr::Arrow { .. })
            }
        ));
        // アロー関数（複数引数）
        let stmts = parse("var g = (a, b) => a + b;").unwrap();
        assert!(matches!(
            stmts[0],
            Stmt::Var {
                name: _,
                init: Some(Expr::Arrow { .. })
            }
        ));
    }

    #[test]
    fn func_expr_and_switch() {
        let stmts = parse("var f = function(x) { return x; };").unwrap();
        assert!(matches!(
            stmts[0],
            Stmt::Var {
                name: _,
                init: Some(Expr::FuncExpr { .. })
            }
        ));
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
            vec![Stmt::Expr(Expr::IncDec {
                name: "x".into(),
                inc: true,
                prefix: true
            })]
        );
        assert_eq!(
            parse("y--;").unwrap(),
            vec![Stmt::Expr(Expr::IncDec {
                name: "y".into(),
                inc: false,
                prefix: false
            })]
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
                ObjEntry::KeyValue("a".into(), Expr::Num(NumLit::Int(1))),
                ObjEntry::KeyValue("b".into(), Expr::Str("x".into())),
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
    fn regex_literal() {
        let stmts = parse("/abc/g;").unwrap();
        assert_eq!(
            stmts,
            vec![Stmt::Expr(Expr::Regex {
                pattern: "abc".into(),
                flags: "g".into(),
            })]
        );
        // エスケープと文字クラス
        let stmts = parse("/a\\/b[0-9]+/i;").unwrap();
        assert!(matches!(stmts[0], Stmt::Expr(Expr::Regex { .. })));
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
