//! レキサ（フェーズ 4）。C 実装 `src/akl/akl.c` の `lex_next` / `lex_skip_ws` /
//! `lex_string` / `lex_template` / `lex_digits` を Rust に移植する。
//!
//! C 実装からの移植対応:
//! | C (akl.c) | Rust |
//! |---|---|
//! | `Lex`（pos/line/kind/pk/num/str_p…）+ `lex_next` | [`Lexer`] + [`Lexer::next_token`] |
//! | `TK_EOF/TK_IDENT/TK_NUM/TK_STR/TK_PUNCT/TK_KW/TK_TPL` | [`Token`] enum |
//! | `KW_*`（41 キーワード）+ `AKL_KWS` | [`Keyword`] + [`Keyword::lookup`] |
//! | `P_*`（57 記号）+ `AKL_PUNCTS`（最長一致） | [`Punct`] + 最長一致テーブル |
//! | `lex_string`（エスケープ復号） | [`Lexer::lex_string`] |
//! | `lex_template`（テンプレート断片） | [`Lexer::lex_template`] |
//! | 数値（10/16/2/8 進・float・指数・BigInt `n`・`_` 区切り） | [`Lexer::lex_number`] |
//!
//! # セキュリティ・正しさ設計
//!
//! - 全ての `pos` 前進は `cur()` / `peek()` / `advance()` 経由（範囲外は番兵 0 を返す
//!   ため、C の `lx->s[lx->pos]` の範囲外読みが構造的に起きない）
//! - 数値リテラルの桁上限（16 進 16 桁・2 進 64 桁・8 進 22 桁）を C と同一に守る
//! - BigInt は 64 bit 符号付き範囲を超えたら明白に失敗（黙って wrap しない）
//! - 文字列の UTF-8 マルチバイトは原列を複製（C の `lex_emit_raw`。各バイトを code
//!   point 化すると CJK が二重エンコード化する実バグの再発防止）

#![forbid(unsafe_code)]
#![warn(missing_docs)]

/// キーワード（C の `KW_*`。順序は C の `AKL_KWS` と一致）。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Keyword {
    /// `var`
    Var,
    /// `let`
    Let,
    /// `const`
    Const,
    /// `function`
    Function,
    /// `return`
    Return,
    /// `if`
    If,
    /// `else`
    Else,
    /// `while`
    While,
    /// `for`
    For,
    /// `break`
    Break,
    /// `continue`
    Continue,
    /// `true`
    True,
    /// `false`
    False,
    /// `null`
    Null,
    /// `typeof`
    Typeof,
    /// `throw`
    Throw,
    /// `try`
    Try,
    /// `catch`
    Catch,
    /// `finally`
    Finally,
    /// `do`
    Do,
    /// `switch`
    Switch,
    /// `case`
    Case,
    /// `default`
    Default,
    /// `this`
    This,
    /// `void`
    Void,
    /// `delete`
    Delete,
    /// `in`
    In,
    /// `new`
    New,
    /// `of`
    Of,
    /// `instanceof`
    Instanceof,
    /// `class`
    Class,
    /// `extends`
    Extends,
    /// `static`
    Static,
    /// `super`
    Super,
    /// `debugger`
    Debugger,
    /// `async`
    Async,
    /// `await`
    Await,
    /// `yield`
    Yield,
    /// `import`
    Import,
    /// `export`
    Export,
    /// `from`（import 文用）
    From,
}

impl Keyword {
    /// キーワード表（文字列 → Keyword）。順序は C の `AKL_KWS` と一致。
    const ALL: &'static [(&'static str, Keyword)] = &[
        ("var", Keyword::Var),
        ("let", Keyword::Let),
        ("const", Keyword::Const),
        ("function", Keyword::Function),
        ("return", Keyword::Return),
        ("if", Keyword::If),
        ("else", Keyword::Else),
        ("while", Keyword::While),
        ("for", Keyword::For),
        ("break", Keyword::Break),
        ("continue", Keyword::Continue),
        ("true", Keyword::True),
        ("false", Keyword::False),
        ("null", Keyword::Null),
        ("typeof", Keyword::Typeof),
        ("throw", Keyword::Throw),
        ("try", Keyword::Try),
        ("catch", Keyword::Catch),
        ("finally", Keyword::Finally),
        ("do", Keyword::Do),
        ("switch", Keyword::Switch),
        ("case", Keyword::Case),
        ("default", Keyword::Default),
        ("this", Keyword::This),
        ("void", Keyword::Void),
        ("delete", Keyword::Delete),
        ("in", Keyword::In),
        ("new", Keyword::New),
        ("of", Keyword::Of),
        ("instanceof", Keyword::Instanceof),
        ("class", Keyword::Class),
        ("extends", Keyword::Extends),
        ("static", Keyword::Static),
        ("super", Keyword::Super),
        ("debugger", Keyword::Debugger),
        ("async", Keyword::Async),
        ("await", Keyword::Await),
        ("yield", Keyword::Yield),
        ("import", Keyword::Import),
        ("export", Keyword::Export),
        ("from", Keyword::From),
    ];

    /// 文字列がキーワードなら対応する [`Keyword`] を返す。
    pub fn lookup(s: &str) -> Option<Keyword> {
        Self::ALL.iter().find(|(k, _)| *k == s).map(|(_, kw)| *kw)
    }
}

/// 記号（C の `P_*`。順序は C の `AKL_PUNCTS` と一致）。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Punct {
    /// `(`
    LParen,
    /// `)`
    RParen,
    /// `{`
    LBrace,
    /// `}`
    RBrace,
    /// `;`
    Semi,
    /// `,`
    Comma,
    /// `=`
    Assign,
    /// `+`
    Plus,
    /// `-`
    Minus,
    /// `*`
    Star,
    /// `/`
    Slash,
    /// `%`
    Percent,
    /// `!`
    Bang,
    /// `<`
    Lt,
    /// `<=`
    Le,
    /// `>`
    Gt,
    /// `>=`
    Ge,
    /// `==`
    EqEq,
    /// `!=`
    Neq,
    /// `===`
    SeqEq,
    /// `!==`
    SNeq,
    /// `&&`
    AndAnd,
    /// `||`
    OrOr,
    /// `.`
    Dot,
    /// `:`
    Colon,
    /// `[`
    LBracket,
    /// `]`
    RBracket,
    /// `?`
    Question,
    /// `++`
    Inc,
    /// `--`
    Dec,
    /// `+=`
    AddAss,
    /// `-=`
    SubAss,
    /// `*=`
    MulAss,
    /// `/=`
    DivAss,
    /// `%=`
    ModAss,
    /// `<<`
    Shl,
    /// `>>`
    Shr,
    /// `>>>`
    UShr,
    /// `&`
    Band,
    /// `|`
    Bor,
    /// `^`
    Bxor,
    /// `~`
    Bnot,
    /// `<<=`
    ShlAss,
    /// `>>=`
    ShrAss,
    /// `>>>=`
    UShrAss,
    /// `&=`
    AndAss,
    /// `|=`
    OrAss,
    /// `^=`
    XorAss,
    /// `**`
    Pow,
    /// `**=`
    PowAss,
    /// `?.`
    QuestionDot,
    /// `??`
    Nullish,
    /// `...`
    Ellipsis,
    /// `` ` ``
    Backtick,
    /// `&&=`
    AndAndAss,
    /// `||=`
    OrOrAss,
    /// `??=`
    NullishAss,
    /// `=>`
    Arrow,
}

impl Punct {
    /// 記号表（文字列 → Punct）。順序は C の `AKL_PUNCTS` と一致（多文字優先）。
    const ALL: &'static [(&'static str, Punct)] = &[
        ("(", Punct::LParen),
        (")", Punct::RParen),
        ("{", Punct::LBrace),
        ("}", Punct::RBrace),
        (";", Punct::Semi),
        (",", Punct::Comma),
        ("=", Punct::Assign),
        ("+", Punct::Plus),
        ("-", Punct::Minus),
        ("*", Punct::Star),
        ("/", Punct::Slash),
        ("%", Punct::Percent),
        ("!", Punct::Bang),
        ("<", Punct::Lt),
        ("<=", Punct::Le),
        (">", Punct::Gt),
        (">=", Punct::Ge),
        ("==", Punct::EqEq),
        ("!=", Punct::Neq),
        ("===", Punct::SeqEq),
        ("!==", Punct::SNeq),
        ("&&", Punct::AndAnd),
        ("||", Punct::OrOr),
        (".", Punct::Dot),
        (":", Punct::Colon),
        ("[", Punct::LBracket),
        ("]", Punct::RBracket),
        ("?", Punct::Question),
        ("++", Punct::Inc),
        ("--", Punct::Dec),
        ("+=", Punct::AddAss),
        ("-=", Punct::SubAss),
        ("*=", Punct::MulAss),
        ("/=", Punct::DivAss),
        ("%=", Punct::ModAss),
        ("<<", Punct::Shl),
        (">>", Punct::Shr),
        (">>>", Punct::UShr),
        ("&", Punct::Band),
        ("|", Punct::Bor),
        ("^", Punct::Bxor),
        ("~", Punct::Bnot),
        ("<<=", Punct::ShlAss),
        (">>=", Punct::ShrAss),
        (">>>=", Punct::UShrAss),
        ("&=", Punct::AndAss),
        ("|=", Punct::OrAss),
        ("^=", Punct::XorAss),
        ("**", Punct::Pow),
        ("**=", Punct::PowAss),
        ("?.", Punct::QuestionDot),
        ("??", Punct::Nullish),
        ("...", Punct::Ellipsis),
        ("`", Punct::Backtick),
        ("&&=", Punct::AndAndAss),
        ("||=", Punct::OrOrAss),
        ("??=", Punct::NullishAss),
        ("=>", Punct::Arrow),
    ];
}

/// 数値リテラル。
#[derive(Clone, Copy, PartialEq, Debug)]
pub enum NumLit {
    /// i32 に収まる整数リテラル（C の `num_is_int`）。
    Int(i32),
    /// 浮動小数点数、または i32 に収まらない整数リテラル（C の double 値）。
    Float(f64),
    /// BigInt リテラル（`n` 接尾辞。64 bit 符号付き）。
    BigInt(i64),
}

/// トークン。
#[derive(Clone, PartialEq, Debug)]
pub enum Token {
    /// 入力終端。
    Eof,
    /// 識別子（`$` `_` 英数字。キーワードは除く）。
    Ident(Box<str>),
    /// 数値リテラル。
    Num(NumLit),
    /// 文字列リテラル（エスケープ復号済み）。
    Str(String),
    /// 記号。
    Punct(Punct),
    /// キーワード。
    Kw(Keyword),
    /// テンプレートリテラルの文字列断片。
    Tpl {
        /// 断片の文字列（エスケープ復号済み）。
        text: String,
        /// 直後に `${`（式）が続くか。
        mid: bool,
    },
}

/// レキサエラー（短いメッセージ。行番号は [`Lexer::line`] が持つ）。
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct LexError(pub String);

impl std::fmt::Display for LexError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.0)
    }
}

/// レキサ。ソースは `&str` を借用し、`pos`（バイト位置）と `line`（1 始まり）を進める。
#[derive(Clone)]
pub struct Lexer<'a> {
    src: &'a str,
    bytes: &'a [u8],
    pos: usize,
    line: u32,
}

impl<'a> Lexer<'a> {
    /// ソースからレキサを作る。
    pub fn new(src: &'a str) -> Self {
        Self {
            src,
            bytes: src.as_bytes(),
            pos: 0,
            line: 1,
        }
    }

    /// 現在の行番号（1 始まり。エラー報告用）。
    pub fn line(&self) -> u32 {
        self.line
    }

    /// 現在位置のバイト（EOF なら 0）。
    fn cur(&self) -> u8 {
        self.bytes.get(self.pos).copied().unwrap_or(0)
    }

    /// 現在位置のバイト（正規表現リテラル用。pub 版）。
    pub fn cur_char(&self) -> u8 {
        self.cur()
    }

    /// 1 バイト前進（正規表現リテラル用。pub 版。改行は数えない）。
    pub fn advance_raw(&mut self) {
        self.pos += 1;
    }

    /// 現在位置から `off` 進んだバイト（範囲外・EOF なら 0）。
    fn peek(&self, off: usize) -> u8 {
        self.bytes.get(self.pos + off).copied().unwrap_or(0)
    }

    /// EOF か。
    fn eof(&self) -> bool {
        self.pos >= self.bytes.len()
    }

    /// 1 バイト前進。
    fn advance(&mut self) {
        self.pos += 1;
    }

    /// 1 文字（コードポイント）前進。ASCII は 1 バイト、マルチバイトは UTF-8 列長分。
    fn advance_char(&mut self) {
        let b = self.cur();
        let len = utf8_len(b);
        self.pos += len;
    }

    /// 現在位置から始まる識別子（`$` `_` 英数字）を読んで返す。
    fn lex_ident(&mut self) -> Box<str> {
        let start = self.pos;
        while !self.eof() {
            let b = self.cur();
            if b == b'_' || b == b'$' || b.is_ascii_alphanumeric() {
                self.advance();
            } else {
                break;
            }
        }
        self.src[start..self.pos].into()
    }

    /// 空白とコメントを読み飛ばす（C の `lex_skip_ws`）。
    fn skip_ws(&mut self) {
        loop {
            let c = self.cur();
            match c {
                b' ' | b'\t' | b'\r' | 0x0c | 0x0b => {
                    self.advance();
                }
                b'\n' => {
                    self.advance();
                    self.line += 1;
                }
                b'/' if self.peek(1) == b'/' => {
                    // 行コメント
                    while !self.eof() && self.cur() != b'\n' {
                        self.advance();
                    }
                }
                b'/' if self.peek(1) == b'*' => {
                    // ブロックコメント
                    self.advance();
                    self.advance();
                    while self.pos + 1 < self.bytes.len()
                        && !(self.cur() == b'*' && self.peek(1) == b'/')
                    {
                        if self.cur() == b'\n' {
                            self.line += 1;
                        }
                        self.advance();
                    }
                    self.advance(); // '*'
                    self.advance(); // '/'
                }
                _ => return,
            }
        }
    }

    /// 次のトークンを読む。エラーは [`LexError`]。
    pub fn next_token(&mut self) -> Result<Token, LexError> {
        self.skip_ws();
        if self.eof() {
            return Ok(Token::Eof);
        }
        let c = self.cur();
        // 文字列リテラル
        if c == b'"' || c == b'\'' {
            return self.lex_string();
        }
        // テンプレートリテラル
        if c == b'`' {
            return self.lex_template();
        }
        // 数値リテラル（`123` / `.5`）
        if c.is_ascii_digit() || (c == b'.' && self.peek(1).is_ascii_digit()) {
            return self.lex_number();
        }
        // 識別子・キーワード
        if c == b'_' || c == b'$' || c.is_ascii_alphabetic() {
            let ident = self.lex_ident();
            if let Some(kw) = Keyword::lookup(&ident) {
                return Ok(Token::Kw(kw));
            }
            return Ok(Token::Ident(ident));
        }
        // 記号（最長一致。C の「多文字 → 1 文字」の順序を維持）
        let mut best: Option<(usize, Punct)> = None;
        for (text, p) in Punct::ALL {
            if self.bytes[self.pos..].starts_with(text.as_bytes())
                && best.is_none_or(|(l, _)| text.len() > l)
            {
                best = Some((text.len(), *p));
            }
        }
        if let Some((len, p)) = best {
            self.pos += len;
            return Ok(Token::Punct(p));
        }
        Err(LexError(format!("unexpected character {:?}", c as char)))
    }

    /// 文字列リテラル（`'` / `"`。エスケープ復号。C の `lex_string`）。
    fn lex_string(&mut self) -> Result<Token, LexError> {
        let quote = self.cur();
        self.advance();
        let mut out = String::new();
        loop {
            if self.eof() || self.cur() == b'\n' {
                return Err(LexError("unterminated string literal".into()));
            }
            let c = self.cur();
            if c == quote {
                self.advance();
                return Ok(Token::Str(out));
            }
            if c != b'\\' {
                // UTF-8 マルチバイトは原列をそのまま複製（C の lex_emit_raw）。
                let ch = self.src[self.pos..].chars().next().unwrap();
                out.push(ch);
                self.advance_char();
                continue;
            }
            // エスケープ
            self.advance(); // '\\'
            if self.eof() {
                return Err(LexError("unterminated escape".into()));
            }
            let e = self.cur();
            self.advance();
            match e {
                b'n' => out.push('\n'),
                b't' => out.push('\t'),
                b'r' => out.push('\r'),
                b'b' => out.push('\u{0008}'),
                b'f' => out.push('\u{000c}'),
                b'v' => out.push('\u{000b}'),
                b'0' => out.push('\0'),
                b'\\' => out.push('\\'),
                b'\'' => out.push('\''),
                b'"' => out.push('"'),
                b'/' => out.push('/'),
                b'x' => {
                    let h1 =
                        hex_digit(self.cur()).ok_or_else(|| LexError("bad \\x escape".into()))?;
                    let h2 =
                        hex_digit(self.peek(1)).ok_or_else(|| LexError("bad \\x escape".into()))?;
                    self.advance();
                    self.advance();
                    out.push((h1 * 16 + h2) as char);
                }
                b'u' => {
                    let mut cp = 0u32;
                    for _ in 0..4 {
                        let h = hex_digit(self.cur())
                            .ok_or_else(|| LexError("bad \\u escape".into()))?;
                        cp = cp * 16 + h as u32;
                        self.advance();
                    }
                    let ch = char::from_u32(cp).ok_or_else(|| LexError("bad \\u escape".into()))?;
                    out.push(ch);
                }
                b'\n' => {
                    self.line += 1; // 行継続
                }
                other => out.push(other as char), // 未知エスケープは文字そのまま
            }
        }
    }

    /// テンプレートリテラル断片（C の `lex_template`）。backtick または `${` まで。
    fn lex_template(&mut self) -> Result<Token, LexError> {
        self.advance(); // backtick
        let mut text = String::new();
        let mut mid = false;
        loop {
            if self.eof() {
                return Err(LexError("unterminated template literal".into()));
            }
            let c = self.cur();
            if c == b'`' {
                self.advance();
                break;
            }
            if c == b'$' && self.peek(1) == b'{' {
                self.advance();
                self.advance();
                mid = true;
                break;
            }
            if c == b'\\' {
                self.advance();
                if self.eof() {
                    return Err(LexError("unterminated template escape".into()));
                }
                let e = self.cur();
                self.advance();
                if e == b'\n' {
                    self.line += 1;
                }
                text.push(e as char);
                continue;
            }
            let ch = self.src[self.pos..].chars().next().unwrap();
            text.push(ch);
            self.advance_char();
        }
        Ok(Token::Tpl { text, mid })
    }

    /// 数値リテラル（C の `lex_next` の数値分岐）。
    fn lex_number(&mut self) -> Result<Token, LexError> {
        // 0x / 0b / 0o 接頭辞
        if self.cur() == b'0' {
            match self.peek(1) {
                b'x' | b'X' => {
                    self.advance();
                    self.advance();
                    return self.lex_radix(16, 16);
                }
                b'b' | b'B' => {
                    self.advance();
                    self.advance();
                    return self.lex_radix(2, 64);
                }
                b'o' | b'O' => {
                    self.advance();
                    self.advance();
                    return self.lex_radix(8, 22);
                }
                _ => {}
            }
        }
        self.lex_decimal()
    }

    /// 2/8/16 進リテラル。`base` は基数、`max_guard` は桁上限。
    fn lex_radix(&mut self, base: u64, max_guard: u32) -> Result<Token, LexError> {
        let mut val: u64 = 0;
        let mut guard = 0u32;
        let mut any = false;
        loop {
            let c = self.cur();
            if let Some(d) = radix_digit(c, base) {
                val = val * base + d as u64;
                self.advance();
                any = true;
                guard += 1;
                if guard > max_guard {
                    return Err(LexError("numeric literal too long".into()));
                }
                if guard == max_guard && val > i64::MAX as u64 {
                    return Err(LexError("numeric literal out of range".into()));
                }
                continue;
            }
            if c == b'_' {
                // 区切りは前後が数字であること
                if !any || radix_digit(self.peek(1), base).is_none() {
                    return Err(LexError("bad numeric separator".into()));
                }
                self.advance();
                continue;
            }
            break;
        }
        if !any {
            return Err(LexError("expected digit".into()));
        }
        // BigInt 接尾辞
        if self.cur() == b'n' {
            self.advance();
            return Ok(Token::Num(NumLit::BigInt(val as i64)));
        }
        Ok(int_or_float(val))
    }

    /// 10 進リテラル（整数・小数・指数・BigInt）。
    fn lex_decimal(&mut self) -> Result<Token, LexError> {
        let mut val: f64 = 0.0;
        let mut is_int = true;
        let mut int64: u64 = 0;
        let mut toobig = false;
        let mut any = false;

        // 整数部
        loop {
            let c = self.cur();
            if let Some(d) = digit_value(c) {
                val = val * 10.0 + d as f64;
                if !toobig {
                    let dig = d as u64;
                    if int64 > (i64::MAX as u64 - dig) / 10 {
                        toobig = true;
                    } else {
                        int64 = int64 * 10 + dig;
                    }
                }
                self.advance();
                any = true;
                continue;
            }
            if c == b'_' {
                if !any || !self.peek(1).is_ascii_digit() {
                    return Err(LexError("bad numeric separator".into()));
                }
                self.advance();
                continue;
            }
            break;
        }
        if !any {
            return Err(LexError("expected digit".into()));
        }

        // 小数部
        if self.cur() == b'.' {
            is_int = false;
            self.advance();
            let mut frac = 0.0;
            let mut scale = 1.0;
            while let Some(d) = digit_value(self.cur()) {
                frac = frac * 10.0 + d as f64;
                scale *= 10.0;
                self.advance();
            }
            val += frac / scale;
        }

        // 指数
        if self.cur() == b'e' || self.cur() == b'E' {
            is_int = false;
            self.advance();
            let mut sign = 1.0;
            if self.cur() == b'+' || self.cur() == b'-' {
                if self.cur() == b'-' {
                    sign = -1.0;
                }
                self.advance();
            }
            if !self.cur().is_ascii_digit() {
                return Err(LexError("bad exponent".into()));
            }
            let mut ex = 0i32;
            while let Some(d) = digit_value(self.cur()) {
                if ex < 10000 {
                    ex = ex * 10 + d as i32;
                }
                self.advance();
            }
            val *= 10f64.powi(sign as i32 * ex);
        }

        // BigInt 接尾辞
        if is_int && self.cur() == b'n' {
            self.advance();
            if toobig {
                return Err(LexError("bigint literal out of range".into()));
            }
            return Ok(Token::Num(NumLit::BigInt(int64 as i64)));
        }

        if is_int && val.abs() <= i32::MAX as f64 {
            Ok(Token::Num(NumLit::Int(val as i32)))
        } else {
            Ok(Token::Num(NumLit::Float(val)))
        }
    }
}

/// UTF-8 の先頭バイトから列長を求める（ASCII=1）。
fn utf8_len(b: u8) -> usize {
    match b {
        0x00..=0x7f => 1,
        0xc0..=0xdf => 2,
        0xe0..=0xef => 3,
        0xf0..=0xf7 => 4,
        _ => 1,
    }
}

/// ASCII 16 進数字 → 値（それ以外は None）。
fn hex_digit(b: u8) -> Option<u8> {
    match b {
        b'0'..=b'9' => Some(b - b'0'),
        b'a'..=b'f' => Some(b - b'a' + 10),
        b'A'..=b'F' => Some(b - b'A' + 10),
        _ => None,
    }
}

/// ASCII 10 進数字 → 値（それ以外は None）。
fn digit_value(b: u8) -> Option<u8> {
    if b.is_ascii_digit() {
        Some(b - b'0')
    } else {
        None
    }
}

/// 基数 `base` における数字 → 値（`0-9` `a-f` `A-F` のうち `base` 未満。それ以外は None）。
fn radix_digit(b: u8, base: u64) -> Option<u8> {
    let d = hex_digit(b)?;
    if (d as u64) < base {
        Some(d)
    } else {
        None
    }
}

/// u64 の整数リテラル値を、i32 に収まれば Int、そうでなければ Float にする。
fn int_or_float(v: u64) -> Token {
    if v <= i32::MAX as u64 {
        Token::Num(NumLit::Int(v as i32))
    } else {
        Token::Num(NumLit::Float(v as f64))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn lex_all(src: &str) -> Vec<Token> {
        let mut lx = Lexer::new(src);
        let mut out = Vec::new();
        loop {
            let t = lx.next_token().expect("lex error");
            let done = matches!(t, Token::Eof);
            out.push(t);
            if done {
                break;
            }
        }
        out
    }

    #[test]
    fn identifiers_and_keywords() {
        let toks = lex_all("var x let _y $z return");
        assert_eq!(
            toks,
            vec![
                Token::Kw(Keyword::Var),
                Token::Ident("x".into()),
                Token::Kw(Keyword::Let),
                Token::Ident("_y".into()),
                Token::Ident("$z".into()),
                Token::Kw(Keyword::Return),
                Token::Eof,
            ]
        );
    }

    #[test]
    fn all_keywords_roundtrip() {
        for (s, kw) in Keyword::ALL {
            let toks = lex_all(s);
            assert_eq!(toks, vec![Token::Kw(*kw), Token::Eof], "keyword {s}");
        }
    }

    #[test]
    fn numbers_decimal() {
        assert_eq!(lex_all("0"), vec![Token::Num(NumLit::Int(0)), Token::Eof]);
        assert_eq!(
            lex_all("123"),
            vec![Token::Num(NumLit::Int(123)), Token::Eof]
        );
        assert_eq!(
            lex_all("-1"),
            vec![
                Token::Punct(Punct::Minus),
                Token::Num(NumLit::Int(1)),
                Token::Eof,
            ]
        );
        assert_eq!(
            lex_all("3.5"),
            vec![Token::Num(NumLit::Float(3.5)), Token::Eof]
        );
        assert_eq!(
            lex_all("1e3"),
            vec![Token::Num(NumLit::Float(1000.0)), Token::Eof]
        );
        assert_eq!(
            lex_all("1.5e-2"),
            vec![Token::Num(NumLit::Float(0.015)), Token::Eof]
        );
        // i32 超は Float
        assert_eq!(
            lex_all("2147483648"),
            vec![Token::Num(NumLit::Float(2147483648.0)), Token::Eof]
        );
    }

    #[test]
    fn numbers_radix() {
        assert_eq!(
            lex_all("0xff"),
            vec![Token::Num(NumLit::Int(255)), Token::Eof]
        );
        assert_eq!(
            lex_all("0b101"),
            vec![Token::Num(NumLit::Int(5)), Token::Eof]
        );
        assert_eq!(
            lex_all("0o77"),
            vec![Token::Num(NumLit::Int(63)), Token::Eof]
        );
    }

    #[test]
    fn numbers_bigint_and_separators() {
        assert_eq!(
            lex_all("10n"),
            vec![Token::Num(NumLit::BigInt(10)), Token::Eof]
        );
        assert_eq!(
            lex_all("0xFFn"),
            vec![Token::Num(NumLit::BigInt(255)), Token::Eof]
        );
        assert_eq!(
            lex_all("1_000"),
            vec![Token::Num(NumLit::Int(1000)), Token::Eof]
        );
        assert_eq!(
            lex_all("0xAB_CD"),
            vec![Token::Num(NumLit::Int(0xABCD)), Token::Eof]
        );
    }

    #[test]
    fn strings_and_escapes() {
        assert_eq!(
            lex_all("\"hello\""),
            vec![Token::Str("hello".into()), Token::Eof]
        );
        assert_eq!(
            lex_all("\"a\\nb\""),
            vec![Token::Str("a\nb".into()), Token::Eof]
        );
        assert_eq!(
            lex_all("\"\\x41\\u0042\""),
            vec![Token::Str("AB".into()), Token::Eof]
        );
        assert_eq!(lex_all("'q'"), vec![Token::Str("q".into()), Token::Eof]);
        // CJK は二重エンコードされない（原列複製）
        assert_eq!(
            lex_all("\"日本語\""),
            vec![Token::Str("日本語".into()), Token::Eof]
        );
    }

    #[test]
    fn template_literals() {
        assert_eq!(
            lex_all("`abc`"),
            vec![
                Token::Tpl {
                    text: "abc".into(),
                    mid: false
                },
                Token::Eof
            ]
        );
        // `${` で mid=true（閉じ backtick はパーサが lex_template を再呼び出しして
        // 消費するため、レキサ単体では `${` までが 1 トークン）
        let toks = lex_all("`a${");
        assert_eq!(
            toks,
            vec![
                Token::Tpl {
                    text: "a".into(),
                    mid: true
                },
                Token::Eof
            ]
        );
    }

    #[test]
    fn punctuators_longest_match() {
        assert_eq!(lex_all("==="), vec![Token::Punct(Punct::SeqEq), Token::Eof]);
        assert_eq!(lex_all(">>>"), vec![Token::Punct(Punct::UShr), Token::Eof]);
        assert_eq!(
            lex_all(">>="),
            vec![Token::Punct(Punct::ShrAss), Token::Eof]
        );
        assert_eq!(lex_all("=>"), vec![Token::Punct(Punct::Arrow), Token::Eof]);
        assert_eq!(
            lex_all("??="),
            vec![Token::Punct(Punct::NullishAss), Token::Eof]
        );
    }

    #[test]
    fn comments_skipped() {
        assert_eq!(
            lex_all("// comment\n1"),
            vec![Token::Num(NumLit::Int(1)), Token::Eof]
        );
        assert_eq!(
            lex_all("/* a\nb */ 2"),
            vec![Token::Num(NumLit::Int(2)), Token::Eof]
        );
    }

    #[test]
    fn line_tracking() {
        let mut lx = Lexer::new("a\nb\nc");
        assert_eq!(lx.line(), 1);
        lx.next_token().unwrap();
        assert_eq!(lx.line(), 1);
        lx.next_token().unwrap();
        assert_eq!(lx.line(), 2);
        lx.next_token().unwrap();
        assert_eq!(lx.line(), 3);
    }

    #[test]
    fn unterminated_string_errors() {
        let mut lx = Lexer::new("\"abc");
        assert!(lx.next_token().is_err());
    }

    #[test]
    fn bad_numeric_separator_errors() {
        let mut lx = Lexer::new("1__0");
        assert!(lx.next_token().is_err());
    }
}
