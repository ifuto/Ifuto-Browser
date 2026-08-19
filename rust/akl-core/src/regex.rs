//! 簡易正規表現エンジン（フェーズ 2/3 の RegExp 相当）。
//!
//! C 実装 `src/akl/akl_regex.c`（1,329 行）のサブセットを、依存ゼロで移植する。
//! 現状は「リテラル一致 + `.` + `*` + `+` + `?` + 文字クラス `[...]` + `^` `$` +
//! 代替 `|` + グループ `(...)`」の正規表現言語を扱う。
//!
//! # 既知の制限
//!
//! - 先読み/後読み、後方参照、名前付きグループ、Unicode プロパティは未対応
//! - 量指定子 `{m,n}` は未対応
//! - 大文字小文字を無視する `i` フラグのみ対応（`g`/`m`/`s`/`u`/`y` は受理するが挙動は近似）

#![forbid(unsafe_code)]
#![warn(missing_docs)]

/// コンパイル済み正規表現。
#[derive(Clone, Debug, PartialEq)]
pub struct Regex {
    /// パターン文字列。
    #[allow(dead_code)]
    pattern: String,
    /// フラグ（bit0=i）。
    flags: u32,
    /// トップレベルの代替枝（パース済み AST）。
    branches: Vec<RegexNode>,
    /// 捕捉グループ数。
    ncap: usize,
}

/// 正規表現 AST ノード。
#[derive(Clone, Debug, PartialEq)]
enum RegexNode {
    /// リテラル文字。
    Literal(char),
    /// 任意の 1 文字（`.`）。
    Any,
    /// 文字クラス `[...]`（否定フラグ + 文字集合）。
    Class { negated: bool, chars: Vec<char>, ranges: Vec<(char, char)> },
    /// 連結。
    Concat(Vec<RegexNode>),
    /// 代替 `|`。
    Alt(Vec<RegexNode>),
    /// 繰り返し（子, min, max=None=無限）。
    Repeat(Box<RegexNode>, u32, Option<u32>),
    /// グループ（index=捕捉番号）。
    Group(usize, Box<RegexNode>),
    /// 先読み `(?=...)` / `(?!...)`（ゼロ幅。positive=true なら肯定）。
    LookAhead { positive: bool, inner: Box<RegexNode> },
    /// 行頭 `^`。
    Start,
    /// 行末 `$`。
    End,
}

/// フラグ定数。
const FLAG_IGNORE_CASE: u32 = 1;

impl Regex {
    /// 正規表現をコンパイルする。失敗時はエラーメッセージ。
    pub fn compile(pattern: &str, flags: u32) -> Result<Self, String> {
        let mut parser = Parser { chars: pattern.chars().collect(), pos: 0, flags, group_count: 0 };
        let mut branches = Vec::new();
        branches.push(parser.parse_alt()?);
        while parser.pos < parser.chars.len() {
            if parser.chars[parser.pos] == '|' {
                parser.pos += 1;
                branches.push(parser.parse_alt()?);
            } else {
                break;
            }
        }
        let ncap = parser.group_count;
        Ok(Regex { pattern: pattern.to_string(), flags, branches, ncap })
    }

    /// マッチを試みる。成功時 Some(捕捉グループ)、失敗時 None。
    /// 捕捉グループ 0 は全体マッチ。
    pub fn find(&self, text: &str) -> Option<Vec<String>> {
        let ignore = self.flags & FLAG_IGNORE_CASE != 0;
        for start in 0..=text.chars().count() {
            if let Some(m) = self.try_match_at(text, start, ignore) {
                return Some(m);
            }
        }
        None
    }

    /// 位置 `start` からマッチを試みる。
    fn try_match_at(&self, text: &str, start: usize, ignore: bool) -> Option<Vec<String>> {
        let chars: Vec<char> = text.chars().collect();
        for branch in &self.branches {
            let mut bcap = vec![None; self.ncap + 1];
            let mut end = None;
            let ok = match_at(branch, &chars, start, ignore, &mut bcap, &mut |e, _caps| {
                end = Some(e);
                true
            });
            if ok {
                if let Some(e) = end {
                    bcap[0] = Some((start, e));
                    let result: Vec<String> = bcap
                        .iter()
                        .map(|c| match c {
                            Some((s, e2)) => chars[*s..*e2].iter().collect(),
                            None => String::new(),
                        })
                        .collect();
                    return Some(result);
                }
            }
        }
        None
    }

    /// 全マッチを返す（`g` フラグ用）。各要素はマッチ全体文字列。
    pub fn find_all(&self, text: &str) -> Vec<String> {
        let mut result = Vec::new();
        let mut rest = text;
        let mut guard = 0;
        loop {
            if guard > 10000 {
                break;
            }
            guard += 1;
            match self.find(rest) {
                Some(caps) => {
                    let full = &caps[0];
                    if full.is_empty() {
                        break;
                    }
                    let start = rest.find(full).unwrap_or(0);
                    result.push(full.clone());
                    rest = &rest[start + full.len()..];
                }
                None => break,
            }
        }
        result
    }
}

/// パーサ。
struct Parser {
    chars: Vec<char>,
    pos: usize,
    #[allow(dead_code)]
    flags: u32,
    group_count: usize,
}

impl Parser {
    fn peek(&self) -> Option<char> {
        self.chars.get(self.pos).copied()
    }

    fn parse_alt(&mut self) -> Result<RegexNode, String> {
        let mut alts = vec![self.parse_concat()?];
        while self.peek() == Some('|') {
            self.pos += 1;
            alts.push(self.parse_concat()?);
        }
        if alts.len() == 1 {
            Ok(alts.remove(0))
        } else {
            Ok(RegexNode::Alt(alts))
        }
    }

    fn parse_concat(&mut self) -> Result<RegexNode, String> {
        let mut nodes = Vec::new();
        while let Some(c) = self.peek() {
            if c == '|' || c == ')' {
                break;
            }
            let node = self.parse_atom()?;
            // 量指定子
            let node = self.parse_quantifier(node)?;
            nodes.push(node);
        }
        if nodes.is_empty() {
            Ok(RegexNode::Literal('\0'))
        } else if nodes.len() == 1 {
            Ok(nodes.remove(0))
        } else {
            Ok(RegexNode::Concat(nodes))
        }
    }

    fn parse_quantifier(&mut self, node: RegexNode) -> Result<RegexNode, String> {
        let result = match self.peek() {
            Some('*') => {
                self.pos += 1;
                Some(RegexNode::Repeat(Box::new(node.clone()), 0, None))
            }
            Some('+') => {
                self.pos += 1;
                Some(RegexNode::Repeat(Box::new(node.clone()), 1, None))
            }
            Some('?') => {
                self.pos += 1;
                Some(RegexNode::Repeat(Box::new(node.clone()), 0, Some(1)))
            }
            // `{m}` / `{m,}` / `{m,n}`
            Some('{') => {
                // 先読み: `{` の直後に数字が無ければリテラルとして扱う（`{` を消費しない）
                let save = self.pos;
                self.pos += 1;
                if !self.peek().is_some_and(|c| c.is_ascii_digit()) {
                    self.pos = save;
                    return Ok(node);
                }
                let m = self.parse_decimal()?;
                let (min, max) = match self.peek() {
                    Some('}') => {
                        self.pos += 1;
                        (m, Some(m))
                    }
                    Some(',') => {
                        self.pos += 1;
                        if self.peek() == Some('}') {
                            self.pos += 1;
                            (m, None)
                        } else {
                            let n = self.parse_decimal()?;
                            self.pos += 1; // '}'
                            (m, Some(n))
                        }
                    }
                    _ => return Err("bad quantifier".to_string()),
                };
                Some(RegexNode::Repeat(Box::new(node.clone()), min, max))
            }
            _ => None,
        };
        // 遅延修飾子 `?`（`*?` `+?` `??` `{m,n}?`）は貪欲として近似（`?` を消費）。
        if self.peek() == Some('?') {
            self.pos += 1;
        }
        Ok(result.unwrap_or(node))
    }

    /// 10 進整数を読む（数字が無ければ Err）。
    fn parse_decimal(&mut self) -> Result<u32, String> {
        let mut v = 0u32;
        let mut any = false;
        while let Some(c) = self.peek() {
            if let Some(d) = c.to_digit(10) {
                v = v.saturating_mul(10).saturating_add(d);
                self.pos += 1;
                any = true;
            } else {
                break;
            }
        }
        if any {
            Ok(v)
        } else {
            Err("expected digit".to_string())
        }
    }

    /// `\uXXXX`（`\u` は消費済み）を読んで文字を返す。`\u{XXXX}` にも対応。
    fn parse_unicode_escape(&mut self) -> Result<char, String> {
        if self.peek() == Some('{') {
            self.pos += 1;
            let mut v = 0u32;
            let mut any = false;
            while let Some(c) = self.peek() {
                if let Some(d) = c.to_digit(16) {
                    v = v.saturating_mul(16).saturating_add(d);
                    self.pos += 1;
                    any = true;
                } else {
                    break;
                }
            }
            if self.peek() == Some('}') {
                self.pos += 1;
            }
            if any {
                return Ok(char::from_u32(v).unwrap_or('?'));
            }
            return Err("bad \\u escape".to_string());
        }
        let mut v = 0u32;
        for _ in 0..4 {
            let c = self.peek().ok_or("bad \\u escape")?;
            let d = c.to_digit(16).ok_or("bad \\u escape")?;
            v = v * 16 + d;
            self.pos += 1;
        }
        Ok(char::from_u32(v).unwrap_or('?'))
    }

    /// グループの閉じ `)` を期待（無ければ Err）。
    fn expect_close_group(&mut self) -> Result<(), String> {
        if self.peek() != Some(')') {
            return Err("unclosed group".to_string());
        }
        self.pos += 1;
        Ok(())
    }

    fn parse_atom(&mut self) -> Result<RegexNode, String> {
        let c = self.peek().ok_or("unexpected end of pattern")?;
        match c {
            '(' => {
                self.pos += 1;
                // 非捕捉グループ `(?:...)` / 先読み `(?=...)` / `(?!...)`
                if self.peek() == Some('?') {
                    self.pos += 1;
                    match self.peek() {
                        Some(':') => {
                            self.pos += 1;
                            let inner = self.parse_alt()?;
                            self.expect_close_group()?;
                            return Ok(inner);
                        }
                        Some('=') => {
                            self.pos += 1;
                            let inner = self.parse_alt()?;
                            self.expect_close_group()?;
                            return Ok(RegexNode::LookAhead { positive: true, inner: Box::new(inner) });
                        }
                        Some('!') => {
                            self.pos += 1;
                            let inner = self.parse_alt()?;
                            self.expect_close_group()?;
                            return Ok(RegexNode::LookAhead { positive: false, inner: Box::new(inner) });
                        }
                        _ => return Err("unsupported group construct".to_string()),
                    }
                }
                self.group_count += 1;
                let idx = self.group_count;
                let inner = self.parse_alt()?;
                if self.peek() != Some(')') {
                    return Err("unclosed group".to_string());
                }
                self.pos += 1;
                Ok(RegexNode::Group(idx, Box::new(inner)))
            }
            '[' => self.parse_class(),
            '.' => {
                self.pos += 1;
                Ok(RegexNode::Any)
            }
            '^' => {
                self.pos += 1;
                Ok(RegexNode::Start)
            }
            '$' => {
                self.pos += 1;
                Ok(RegexNode::End)
            }
            '\\' => {
                self.pos += 1;
                let e = self.peek().ok_or("trailing backslash")?;
                self.pos += 1;
                // 文字クラスエスケープ（\d \w \s と否定形）。範囲/文字集合へ展開する。
                match e {
                    'd' => Ok(RegexNode::Class { negated: false, chars: vec![], ranges: vec![('0', '9')] }),
                    'D' => Ok(RegexNode::Class { negated: true, chars: vec![], ranges: vec![('0', '9')] }),
                    'w' => Ok(RegexNode::Class {
                        negated: false,
                        chars: vec!['_'],
                        ranges: vec![('0', '9'), ('A', 'Z'), ('a', 'z')],
                    }),
                    'W' => Ok(RegexNode::Class {
                        negated: true,
                        chars: vec!['_'],
                        ranges: vec![('0', '9'), ('A', 'Z'), ('a', 'z')],
                    }),
                    's' => Ok(RegexNode::Class {
                        negated: false,
                        chars: vec![' ', '\t', '\n', '\r', '\u{0b}', '\u{0c}'],
                        ranges: vec![],
                    }),
                    'S' => Ok(RegexNode::Class {
                        negated: true,
                        chars: vec![' ', '\t', '\n', '\r', '\u{0b}', '\u{0c}'],
                        ranges: vec![],
                    }),
                    // `\uXXXX` / `\u{XXXX}` Unicode エスケープ → リテラル文字
                    'u' => Ok(RegexNode::Literal(self.parse_unicode_escape()?)),
                    // `\xXX` 16 進エスケープ → リテラル文字
                    'x' => {
                        let hi = self.peek().ok_or("trailing \\x")?;
                        self.pos += 1;
                        let lo = self.peek().ok_or("trailing \\x")?;
                        self.pos += 1;
                        let h = hi.to_digit(16).ok_or("bad \\x escape")?;
                        let l = lo.to_digit(16).ok_or("bad \\x escape")?;
                        Ok(RegexNode::Literal(char::from_u32(h * 16 + l).unwrap_or('?')))
                    }
                    'n' => Ok(RegexNode::Literal('\n')),
                    't' => Ok(RegexNode::Literal('\t')),
                    'r' => Ok(RegexNode::Literal('\r')),
                    'f' => Ok(RegexNode::Literal('\u{0c}')),
                    'v' => Ok(RegexNode::Literal('\u{0b}')),
                    '0' => Ok(RegexNode::Literal('\0')),
                    _ => Ok(RegexNode::Literal(e)),
                }
            }
            '*' | '+' | '?' => Err(format!("quantifier without target: {c}")),
            _ => {
                self.pos += 1;
                Ok(RegexNode::Literal(c))
            }
        }
    }

    fn parse_class(&mut self) -> Result<RegexNode, String> {
        self.pos += 1; // '['
        let mut negated = false;
        if self.peek() == Some('^') {
            negated = true;
            self.pos += 1;
        }
        let mut chars = Vec::new();
        let mut ranges = Vec::new();
        while let Some(c) = self.peek() {
            if c == ']' {
                self.pos += 1;
                return Ok(RegexNode::Class { negated, chars, ranges });
            }
            // エスケープ（`\uXXXX` / `\xXX` / `\\` / `\-` 等）
            let c = if c == '\\' {
                self.pos += 1;
                let e = self.peek().ok_or("trailing backslash in class")?;
                match e {
                    'u' => {
                        self.pos += 1;
                        self.parse_unicode_escape()?
                    }
                    'x' => {
                        self.pos += 1;
                        let hi = self.peek().ok_or("trailing \\x")?;
                        self.pos += 1;
                        let lo = self.peek().ok_or("trailing \\x")?;
                        self.pos += 1;
                        let h = hi.to_digit(16).ok_or("bad \\x")?;
                        let l = lo.to_digit(16).ok_or("bad \\x")?;
                        char::from_u32(h * 16 + l).unwrap_or('?')
                    }
                    'n' => '\n',
                    't' => '\t',
                    'r' => '\r',
                    'f' => '\u{0c}',
                    'd' => {
                        // `\d` は範囲 [0-9] に展開
                        self.pos += 1;
                        ranges.push(('0', '9'));
                        continue;
                    }
                    's' => {
                        self.pos += 1;
                        chars.extend([' ', '\t', '\n', '\r', '\u{0b}', '\u{0c}']);
                        continue;
                    }
                    'w' => {
                        self.pos += 1;
                        chars.push('_');
                        ranges.extend([('0', '9'), ('A', 'Z'), ('a', 'z')]);
                        continue;
                    }
                    _ => {
                        self.pos += 1;
                        e
                    }
                }
            } else {
                self.pos += 1;
                c
            };
            // 範囲 a-z
            if self.peek() == Some('-') {
                let next = self.chars.get(self.pos + 1).copied();
                if let Some(next) = next {
                    if next != ']' {
                        self.pos += 1; // '-'
                        let end = self.peek().unwrap();
                        self.pos += 1;
                        ranges.push((c, end));
                        continue;
                    }
                }
            }
            chars.push(c);
        }
        Err("unclosed character class".to_string())
    }
}

/// 捕捉グループ列の型。
type Caps = Vec<Option<(usize, usize)>>;

/// 継続渡し（CPS）の正規表現マッチャ。`node` が `pos` からマッチし、消費後の位置で
/// 継続 `k` を呼ぶ。`k` が false を返すと別の消費量を試す（バックトラッキング）。
/// `caps` は継続に明示的に渡すことで、`Group` 内の Repeat 等のネストでも正しく
/// バックトラックできる。
fn match_at(
    node: &RegexNode,
    chars: &[char],
    pos: usize,
    ignore: bool,
    caps: &mut Caps,
    k: &mut dyn FnMut(usize, &mut Caps) -> bool,
) -> bool {
    match node {
        RegexNode::Literal(c) => {
            if pos < chars.len() && char_eq(*c, chars[pos], ignore) {
                k(pos + 1, caps)
            } else {
                false
            }
        }
        RegexNode::Any => {
            if pos < chars.len() {
                k(pos + 1, caps)
            } else {
                false
            }
        }
        RegexNode::Class { negated, chars: cl, ranges } => {
            if pos >= chars.len() {
                return false;
            }
            let tc = chars[pos];
            let in_class = cl.iter().any(|c| char_eq(*c, tc, ignore))
                || ranges.iter().any(|(a, b)| char_in_range(*a, *b, tc, ignore));
            if in_class != *negated {
                k(pos + 1, caps)
            } else {
                false
            }
        }
        RegexNode::Start => {
            if pos == 0 {
                k(pos, caps)
            } else {
                false
            }
        }
        RegexNode::End => {
            if pos == chars.len() {
                k(pos, caps)
            } else {
                false
            }
        }
        RegexNode::Concat(nodes) => match_concat(nodes, chars, pos, ignore, caps, k),
        RegexNode::Alt(alts) => alts
            .iter()
            .any(|a| match_at(a, chars, pos, ignore, caps, k)),
        RegexNode::Repeat(child, min, max) => {
            let maxc = match max {
                Some(m) => *m as usize,
                None => chars.len().saturating_sub(pos) + 1,
            };
            let minc = *min as usize;
            // 貪欲: 最大消費から最小へ順に試す
            let mut c = maxc;
            loop {
                if c >= minc && match_repeat(child, c, chars, pos, ignore, caps, k) {
                    return true;
                }
                if c <= minc {
                    break;
                }
                c -= 1;
            }
            false
        }
        RegexNode::Group(idx, child) => {
            let saved = caps[*idx];
            let gpos = pos;
            let gidx = *idx;
            let ok = match_at(child, chars, pos, ignore, caps, &mut |end, caps2| {
                caps2[gidx] = Some((gpos, end));
                k(end, caps2)
            });
            if !ok {
                caps[gidx] = saved;
            }
            ok
        }
        RegexNode::LookAhead { positive, inner } => {
            // ゼロ幅の先読み: inner が pos でマッチするかを確認して継続する（消費しない）。
            let mut matched = false;
            let mut tmp = caps.clone();
            let _ = match_at(inner, chars, pos, ignore, &mut tmp, &mut |_end, _c| {
                matched = true;
                true
            });
            if matched == *positive {
                k(pos, caps)
            } else {
                false
            }
        }
    }
}

/// 連結（`Concat`）のマッチ。先頭ノードをマッチし、残りを継続で再帰する。
fn match_concat(
    nodes: &[RegexNode],
    chars: &[char],
    pos: usize,
    ignore: bool,
    caps: &mut Caps,
    k: &mut dyn FnMut(usize, &mut Caps) -> bool,
) -> bool {
    if nodes.is_empty() {
        return k(pos, caps);
    }
    match_at(&nodes[0], chars, pos, ignore, caps, &mut |end, caps2| {
        match_concat(&nodes[1..], chars, end, ignore, caps2, k)
    })
}

/// `child` をちょうど `count` 回マッチさせて継続へ渡す（Repeat のバックトラッキング用）。
fn match_repeat(
    child: &RegexNode,
    count: usize,
    chars: &[char],
    pos: usize,
    ignore: bool,
    caps: &mut Caps,
    k: &mut dyn FnMut(usize, &mut Caps) -> bool,
) -> bool {
    if count == 0 {
        return k(pos, caps);
    }
    match_at(child, chars, pos, ignore, caps, &mut |end, caps2| {
        match_repeat(child, count - 1, chars, end, ignore, caps2, k)
    })
}

/// 文字の等値判定（i フラグ対応）。
fn char_eq(a: char, b: char, ignore: bool) -> bool {
    if ignore {
        a.eq_ignore_ascii_case(&b)
    } else {
        a == b
    }
}

/// 範囲判定（i フラグ対応）。
fn char_in_range(a: char, b: char, c: char, ignore: bool) -> bool {
    let (lo, hi) = if a <= b { (a, b) } else { (b, a) };
    if ignore {
        let cl = c.to_ascii_lowercase();
        cl >= lo.to_ascii_lowercase() && cl <= hi.to_ascii_lowercase()
    } else {
        c >= lo && c <= hi
    }
}

/// 文字列の全置換（`String.replace` の RegExp 版簡易実装）。
/// パターンの最初のマッチを置換する（g フラグは未対応）。
pub fn replace_first(text: &str, rx: &Regex, replacement: &str) -> String {
    if let Some(caps) = rx.find(text) {
        let full = &caps[0];
        let start = text.find(full).unwrap_or(0);
        let end = start + full.len();
        let mut result = String::new();
        result.push_str(&text[..start]);
        // $1 などの展開
        let repl = expand_replacement(replacement, &caps);
        result.push_str(&repl);
        result.push_str(&text[end..]);
        result
    } else {
        text.to_string()
    }
}

/// 文字列の全置換（`String.replace` の RegExp 版、`g` フラグ用）。
/// 各マッチで `$1` 等の捕捉グループを展開する。
pub fn replace_all(text: &str, rx: &Regex, replacement: &str) -> String {
    let mut result = String::new();
    let mut rest = text;
    let mut guard = 0;
    loop {
        if guard > 10000 {
            break;
        }
        guard += 1;
        match rx.find(rest) {
            Some(caps) => {
                let full = &caps[0];
                if full.is_empty() {
                    result.push_str(rest);
                    break;
                }
                let start = rest.find(full).unwrap_or(0);
                result.push_str(&rest[..start]);
                result.push_str(&expand_replacement(replacement, &caps));
                rest = &rest[start + full.len()..];
            }
            None => {
                result.push_str(rest);
                break;
            }
        }
    }
    result
}

/// 置換文字列の `$1` 展開。
fn expand_replacement(replacement: &str, caps: &[String]) -> String {
    let mut out = String::new();
    let mut chars = replacement.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '$' {
            if let Some(&d) = chars.peek() {
                if d.is_ascii_digit() {
                    chars.next();
                    let idx = d.to_digit(10).unwrap() as usize;
                    if idx < caps.len() {
                        out.push_str(&caps[idx]);
                    }
                    continue;
                }
            }
            out.push('$');
        } else {
            out.push(c);
        }
    }
    out
}

/// 文字列を正規表現で分割（`String.split` の RegExp 版）。
pub fn split(text: &str, rx: &Regex) -> Vec<String> {
    let mut result = Vec::new();
    let mut rest = text;
    let mut guard = 0;
    loop {
        if guard > 10000 {
            break;
        }
        guard += 1;
        match rx.find(rest) {
            Some(caps) => {
                let full = &caps[0];
                if full.is_empty() {
                    break;
                }
                let start = rest.find(full).unwrap_or(0);
                result.push(rest[..start].to_string());
                rest = &rest[start + full.len()..];
            }
            None => {
                result.push(rest.to_string());
                break;
            }
        }
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn literal_match() {
        let rx = Regex::compile("abc", 0).unwrap();
        assert!(rx.find("xxabcyy").is_some());
        assert!(rx.find("xyz").is_none());
    }

    #[test]
    fn dot_star() {
        let rx = Regex::compile("a.c", 0).unwrap();
        assert!(rx.find("abc").is_some());
        let rx = Regex::compile("a.*c", 0).unwrap();
        assert!(rx.find("aXXXc").is_some());
    }

    #[test]
    fn char_class() {
        let rx = Regex::compile("[0-9]+", 0).unwrap();
        let caps = rx.find("ab123cd").unwrap();
        assert_eq!(caps[0], "123");
    }

    #[test]
    fn alternation_and_group() {
        let rx = Regex::compile("(ab|cd)+", 0).unwrap();
        assert!(rx.find("abcd").is_some());
    }

    #[test]
    fn anchors() {
        let rx = Regex::compile("^abc$", 0).unwrap();
        assert!(rx.find("abc").is_some());
        assert!(rx.find("xabc").is_none());
    }

    #[test]
    fn ignore_case() {
        let rx = Regex::compile("abc", FLAG_IGNORE_CASE).unwrap();
        assert!(rx.find("ABC").is_some());
    }
}
