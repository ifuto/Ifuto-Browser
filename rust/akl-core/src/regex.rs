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
        let mut caps = vec![None; self.ncap + 1];
        for branch in &self.branches {
            let mut bcap = vec![None; self.ncap + 1];
            if match_node(branch, &chars, start, ignore, &mut bcap) {
                // 全体マッチの終端を取得
                let end = find_end(branch, &chars, start, ignore, &mut bcap.clone());
                bcap[0] = Some((start, end));
                caps = bcap;
                break;
            }
        }
        if caps[0].is_some() {
            let result: Vec<String> = caps
                .iter()
                .map(|c| match c {
                    Some((s, e)) => chars[*s..*e].iter().collect(),
                    None => String::new(),
                })
                .collect();
            Some(result)
        } else {
            None
        }
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
        match self.peek() {
            Some('*') => {
                self.pos += 1;
                Ok(RegexNode::Repeat(Box::new(node), 0, None))
            }
            Some('+') => {
                self.pos += 1;
                Ok(RegexNode::Repeat(Box::new(node), 1, None))
            }
            Some('?') => {
                self.pos += 1;
                Ok(RegexNode::Repeat(Box::new(node), 0, Some(1)))
            }
            _ => Ok(node),
        }
    }

    fn parse_atom(&mut self) -> Result<RegexNode, String> {
        let c = self.peek().ok_or("unexpected end of pattern")?;
        match c {
            '(' => {
                self.pos += 1;
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
                Ok(RegexNode::Literal(e))
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
            self.pos += 1;
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

/// 連結のマッチ（バックトラッキング対応）。
fn match_concat(
    nodes: &[RegexNode],
    chars: &[char],
    pos: usize,
    ignore: bool,
    caps: &mut [Option<(usize, usize)>],
) -> bool {
    if nodes.is_empty() {
        return true;
    }
    // バックトラッキング: 各 Repeat の消費量を段階的に試す
    // 簡易実装: 先頭から順に貪欲マッチし、失敗したら直前の Repeat の消費を 1 減らす
    let mut positions = vec![pos; nodes.len() + 1];
    positions[0] = pos;
    let mut counts: Vec<u32> = vec![0; nodes.len()];
    let mut i = 0usize;
    while i < nodes.len() {
        let node = &nodes[i];
        match node {
            RegexNode::Repeat(child, min, _max) => {
                // 消費量を counts[i] に設定してマッチ
                let mut p = positions[i];
                let mut c = 0u32;
                while c < counts[i] {
                    if match_node(child, chars, p, ignore, caps) {
                        p = advance_past(child, chars, p, ignore, caps);
                        c += 1;
                    } else {
                        return false;
                    }
                }
                // さらに貪欲に伸ばせるか
                let mut extend = p;
                let mut ec = c;
                while match_node(child, chars, extend, ignore, caps) {
                    extend = advance_past(child, chars, extend, ignore, caps);
                    ec += 1;
                }
                if ec < *min {
                    return false;
                }
                positions[i + 1] = extend;
                counts[i] = ec;
                i += 1;
            }
            _ => {
                if match_node(node, chars, positions[i], ignore, caps) {
                    positions[i + 1] = advance_past(node, chars, positions[i], ignore, caps);
                    i += 1;
                } else {
                    // バックトラック: 直前の Repeat を探す
                    let mut j = i;
                    loop {
                        if j == 0 {
                            return false;
                        }
                        j -= 1;
                        if let RegexNode::Repeat(child, min, _) = &nodes[j] {
                            if counts[j] > *min {
                                counts[j] -= 1;
                                // 再計算
                                let mut p = positions[j];
                                let mut c = 0u32;
                                while c < counts[j] {
                                    if match_node(child, chars, p, ignore, caps) {
                                        p = advance_past(child, chars, p, ignore, caps);
                                        c += 1;
                                    } else {
                                        return false;
                                    }
                                }
                                positions[j + 1] = p;
                                i = j + 1;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    true
}

/// ノードのマッチ（位置 start から）。
fn match_node(
    node: &RegexNode,
    chars: &[char],
    pos: usize,
    ignore: bool,
    caps: &mut [Option<(usize, usize)>],
) -> bool {
    match node {
        RegexNode::Literal(c) => {
            pos < chars.len() && char_eq(*c, chars[pos], ignore)
        }
        RegexNode::Any => pos < chars.len(),
        RegexNode::Class { negated, chars: cl, ranges } => {
            if pos >= chars.len() {
                return false;
            }
            let tc = chars[pos];
            let in_class = cl.iter().any(|c| char_eq(*c, tc, ignore))
                || ranges.iter().any(|(a, b)| char_in_range(*a, *b, tc, ignore));
            in_class != *negated
        }
        RegexNode::Start => pos == 0,
        RegexNode::End => pos == chars.len(),
        RegexNode::Concat(nodes) => {
            // バックトラッキング付き連結
            match_concat(nodes, chars, pos, ignore, caps)
        }
        RegexNode::Alt(alts) => alts.iter().any(|a| match_node(a, chars, pos, ignore, caps)),
        RegexNode::Repeat(child, min, max) => {
            // 貪欲マッチ（できるだけ消費。バックトラッキングは match_concat が担う）
            let mut p = pos;
            let mut count = 0u32;
            loop {
                if let Some(m) = max {
                    if count >= *m {
                        break;
                    }
                }
                if match_node(child, chars, p, ignore, caps) {
                    p = advance_past(child, chars, p, ignore, caps);
                    count += 1;
                } else {
                    break;
                }
            }
            count >= *min
        }
        RegexNode::Group(idx, child) => {
            if match_node(child, chars, pos, ignore, caps) {
                let end = advance_past(child, chars, pos, ignore, caps);
                caps[*idx] = Some((pos, end));
                true
            } else {
                false
            }
        }
    }
}

/// ノードがマッチしたときの消費文字数を求める（advance 用）。
fn advance_past(
    node: &RegexNode,
    chars: &[char],
    pos: usize,
    ignore: bool,
    caps: &mut [Option<(usize, usize)>],
) -> usize {
    match node {
        RegexNode::Literal(_) | RegexNode::Any | RegexNode::Class { .. } | RegexNode::Start | RegexNode::End => {
            if matches!(node, RegexNode::Start | RegexNode::End) {
                pos
            } else {
                pos + 1
            }
        }
        RegexNode::Concat(nodes) => {
            let mut p = pos;
            for n in nodes {
                if !match_node(n, chars, p, ignore, caps) {
                    break;
                }
                p = advance_past(n, chars, p, ignore, caps);
            }
            p
        }
        RegexNode::Alt(alts) => {
            for a in alts {
                if match_node(a, chars, pos, ignore, caps) {
                    return advance_past(a, chars, pos, ignore, caps);
                }
            }
            pos
        }
        RegexNode::Repeat(child, min, max) => {
            let mut p = pos;
            let mut count = 0u32;
            loop {
                if let Some(m) = max {
                    if count >= *m {
                        break;
                    }
                }
                if match_node(child, chars, p, ignore, caps) {
                    p = advance_past(child, chars, p, ignore, caps);
                    count += 1;
                } else {
                    break;
                }
            }
            if count >= *min {
                p
            } else {
                pos
            }
        }
        RegexNode::Group(idx, child) => {
            if match_node(child, chars, pos, ignore, caps) {
                let end = advance_past(child, chars, pos, ignore, caps);
                caps[*idx] = Some((pos, end));
                end
            } else {
                pos
            }
        }
    }
}

/// 全体マッチの終端を求める。
fn find_end(
    node: &RegexNode,
    chars: &[char],
    pos: usize,
    ignore: bool,
    caps: &mut [Option<(usize, usize)>],
) -> usize {
    advance_past(node, chars, pos, ignore, caps)
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
