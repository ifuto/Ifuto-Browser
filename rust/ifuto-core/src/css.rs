//! CSS サブセット（パース + カスケード。C の `src/css.c` 相当）。
//!
//! | C (css.c / css.h) | Rust |
//! |---|---|
//! | `if_css_color` | [`css_color`] |
//! | `if_css_parse_decls` | [`parse_decls`] |
//! | `if_css_parse` | [`parse_stylesheet`] |
//! | `if_css_match_selector` | [`match_selector`] |
//! | `if_css_resolve_len` | [`resolve_len`] |
//! | `if_style_apply` | [`apply_styles`] |
//! | `if_style_dump` | [`dump_styles`] |
//!
//! # 実装済み
//!
//! 色（`#hex` / `rgb()` / `rgba()` / 色名 / `transparent`）、値レクサ（NUM/DIM/PCT/
//! IDENT/COLOR/STR/AUTO）、宣言パーサ（shorthand 展開: margin/padding/border/
//! background/border-width）、セレクタパーサ（type/class/id/universal + 子孫・子結合子）、
//! スタイルシートパーサ（@規則は丸ごと棄却）、マッチャ（子孫バックトラッキング）、
//! カスケード（important, origin, specificity, order の辞書順）、computed style dump。
//!
//! # C との違い（所有権による構造的な改善）
//!
//! C は `IfDecl.text` / `IfCompound.tag_name` 等を arena スライス（借用）で保持する。
//! Rust では所有 `Vec<u8>` に置き換え、手動の arena 確保・境界検査を排除する。
//! 計算済みスタイルは `Node` に埋め込まず、`NodeId` と並行の `Vec<Option<Style>>`
//! で返す（`dom` から `css` への依存を断ち、モジュール間の循環参照を防ぐ）。
//!
//! # 未移植（性能最適化・観測不変）
//!
//! 以下の最適化は出力に影響しない（`--dump-styles` のバイト列は全経路で同一）ため、
//! 本移植では素朴な全走査マッチに一本化し、将来の最適化として保留する:
//! - RuleSet 風セレクタインデックス（`css_build_ruleset` / `rs_find` / `collect_from_sheet`）
//! - computed style interning（`st_intern` による dedup）
//! - 決定メモ化（`IfStCache` / `st_resolve_memo`）
//! - lazy computed style（`if_style_lazy_*` / `if_md_style_lazy_ok`。md fast-DOM 専用）
//! - Blink ファサード（`css_blink.h`。C-only header、生成バイト 0）

use crate::dom::{Dom, NodeId, NodeKind};
use crate::strutil::str_eq_ci;
use crate::tags::{self, Tag};

// ================= 色 =================

fn rgba8(r: u32, g: u32, b: u32, a: u32) -> u32 {
    (r << 24) | (g << 16) | (b << 8) | a
}

fn hexv(c: u8) -> i32 {
    match c {
        b'0'..=b'9' => (c - b'0') as i32,
        b'a'..=b'f' => (c - b'a' + 10) as i32,
        b'A'..=b'F' => (c - b'A' + 10) as i32,
        _ => -1,
    }
}

/// 色名表（C の `IF_COLORS`。重複は同一 RGB のため無害）。
const COLORS: &[(&str, u32)] = &[
    ("black", 0x000000),
    ("silver", 0xC0C0C0),
    ("gray", 0x808080),
    ("grey", 0x808080),
    ("white", 0xFFFFFF),
    ("maroon", 0x800000),
    ("red", 0xFF0000),
    ("purple", 0x800080),
    ("fuchsia", 0xFF00FF),
    ("magenta", 0xFF00FF),
    ("green", 0x008000),
    ("lime", 0x00FF00),
    ("olive", 0x808000),
    ("yellow", 0xFFFF00),
    ("navy", 0x000080),
    ("blue", 0x0000FF),
    ("teal", 0x008080),
    ("aqua", 0x00FFFF),
    ("cyan", 0x00FFFF),
    ("orange", 0xFFA500),
    ("aliceblue", 0xF0F8FF),
    ("brown", 0xA52A2A),
    ("coral", 0xFF7F50),
    ("crimson", 0xDC143C),
    ("darkblue", 0x00008B),
    ("darkgray", 0xA9A9A9),
    ("darkgreen", 0x006400),
    ("darkred", 0x8B0000),
    ("gold", 0xFFD700),
    ("goldenrod", 0xDAA520),
    ("hotpink", 0xFF69B4),
    ("indigo", 0x4B0082),
    ("ivory", 0xFFFFF0),
    ("khaki", 0xF0E68C),
    ("lavender", 0xE6E6FA),
    ("lightgray", 0xD3D3D3),
    ("lightgreen", 0x90EE90),
    ("lightyellow", 0xFFFFE0),
    ("limegreen", 0x32CD32),
    ("magenta", 0xFF00FF),
    ("midnightblue", 0x191970),
    ("orchid", 0xDA70D6),
    ("pink", 0xFFC0CB),
    ("plum", 0xDDA0DD),
    ("rebeccapurple", 0x663399),
    ("salmon", 0xFA8072),
    ("skyblue", 0x87CEEB),
    ("slategray", 0x708090),
    ("snow", 0xFFFAFA),
    ("tan", 0xD2B48C),
    ("tomato", 0xFF6347),
    ("turquoise", 0x40E0D0),
    ("violet", 0xEE82EE),
    ("wheat", 0xF5DEB3),
    ("whitesmoke", 0xF5F5F5),
];

/// 色文字列を RGBA8（0xRRGGBBAA）へ。C の `if_css_color` 相当。失敗は `None`。
pub fn css_color(s: &[u8]) -> Option<u32> {
    let s = trim(s);
    if s.is_empty() {
        return None;
    }
    if s[0] == b'#' {
        if s.len() == 4 || s.len() == 5 || s.len() == 7 || s.len() == 9 {
            let mut d = [0u32; 8];
            for (i, &b) in s[1..].iter().enumerate() {
                let v = hexv(b);
                if v < 0 {
                    return None;
                }
                d[i] = v as u32;
            }
            if s.len() == 4 {
                return Some(rgba8(d[0] * 17, d[1] * 17, d[2] * 17, 255));
            }
            if s.len() == 5 {
                return Some(rgba8(d[0] * 17, d[1] * 17, d[2] * 17, d[3] * 17));
            }
            if s.len() == 7 {
                return Some(rgba8(
                    d[0] * 16 + d[1],
                    d[2] * 16 + d[3],
                    d[4] * 16 + d[5],
                    255,
                ));
            }
            return Some(rgba8(
                d[0] * 16 + d[1],
                d[2] * 16 + d[3],
                d[4] * 16 + d[5],
                d[6] * 16 + d[7],
            ));
        }
        return None;
    }

    // rgb(...) / rgba(...)
    if s.len() >= 5 && (s[0] == b'r' || s[0] == b'R') {
        let alpha = s.len() >= 5 && s[3].eq_ignore_ascii_case(&b'a');
        let off = if alpha { 5 } else { 4 };
        if s.len() <= off || s[s.len() - 1] != b')' {
            return None;
        }
        let mut c = Cur {
            p: &s[off..s.len() - 1],
            i: 0,
        };
        let mut ch = [0u32, 0, 0, 255];
        let mut got = 0usize;
        for _ in 0..4 {
            while c.i < c.p.len() && (Cur::ws(c.peek()) || c.p[c.i] == b',') {
                c.i += 1;
            }
            if c.i >= c.p.len() {
                break;
            }
            let start = c.i;
            if c.p[c.i] == b'-' {
                return None;
            }
            while c.i < c.p.len() && c.p[c.i].is_ascii_digit() {
                c.i += 1;
            }
            if c.i == start {
                return None;
            }
            let mut v: u64 = 0;
            for j in start..c.i {
                v = v * 10 + (c.p[j] - b'0') as u64;
            }
            if v > 255 {
                return None;
            }
            if c.i < c.p.len() && c.p[c.i] == b'%' {
                return None;
            }
            ch[got] = v as u32;
            got += 1;
            if got == 3 && !alpha {
                break;
            }
        }
        if got < 3 {
            return None;
        }
        return Some(rgba8(ch[0], ch[1], ch[2], if alpha { ch[3] } else { 255 }));
    }

    if str_eq_ci(s, b"transparent") {
        return Some(0);
    }
    for (name, rgb) in COLORS {
        if s.len() == name.len() && str_eq_ci(s, name.as_bytes()) {
            return Some(rgba8(
                (rgb >> 16) & 0xff,
                (rgb >> 8) & 0xff,
                rgb & 0xff,
                255,
            ));
        }
    }
    None
}

// ================= レキサ的補助 =================

/// カーソル（C の `IfCur` 相当。`&[u8]` + pos）。
struct Cur<'a> {
    p: &'a [u8],
    i: usize,
}

impl<'a> Cur<'a> {
    fn ws(c: u8) -> bool {
        c == b' ' || c == b'\t' || c == b'\n' || c == b'\r' || c == b'\x0c'
    }

    fn peek(&self) -> u8 {
        self.p.get(self.i).copied().unwrap_or(0)
    }

    fn peek2(&self) -> u8 {
        self.p.get(self.i + 1).copied().unwrap_or(0)
    }

    fn skip_ws(&mut self) {
        while self.i < self.p.len() && Self::ws(self.p[self.i]) {
            self.i += 1;
        }
    }

    fn skip_ws_comments(&mut self) {
        loop {
            self.skip_ws();
            if self.peek() == b'/' && self.peek2() == b'*' {
                self.i += 2;
                while self.i + 1 < self.p.len()
                    && !(self.p[self.i] == b'*' && self.p[self.i + 1] == b'/')
                {
                    self.i += 1;
                }
                if self.i + 1 < self.p.len() {
                    self.i += 2;
                } else {
                    self.i = self.p.len();
                }
                continue;
            }
            return;
        }
    }

    fn ident(&mut self) -> &'a [u8] {
        let s = self.i;
        if self.i < self.p.len() && ident_char(self.p[self.i]) {
            self.i += 1;
            while self.i < self.p.len() && ident_char(self.p[self.i]) {
                self.i += 1;
            }
        }
        &self.p[s..self.i]
    }

    /// 文字列トークン（'...' / "..."）。閉じなしは末尾まで。
    fn string(&mut self) -> &'a [u8] {
        let q = self.peek();
        self.i += 1;
        let s = self.i;
        while self.i < self.p.len() && self.p[self.i] != q {
            if self.p[self.i] == b'\\' && self.i + 1 < self.p.len() {
                self.i += 1;
            }
            self.i += 1;
        }
        let out = &self.p[s..self.i];
        if self.i < self.p.len() {
            self.i += 1;
        }
        out
    }
}

fn ident_start(c: u8) -> bool {
    c.is_ascii_alphabetic() || c == b'_' || c == b'-' || c >= 0x80
}

fn ident_char(c: u8) -> bool {
    ident_start(c) || c.is_ascii_digit()
}

fn trim(s: &[u8]) -> &[u8] {
    let mut a = 0;
    let mut b = s.len();
    while a < b && Cur::ws(s[a]) {
        a += 1;
    }
    while b > a && Cur::ws(s[b - 1]) {
        b -= 1;
    }
    &s[a..b]
}

// ================= 値レクサ =================

#[derive(Clone, Copy, PartialEq, Eq)]
enum ValKind {
    Num,
    Dim,
    Pct,
    Ident,
    Color,
    Str,
    Auto,
}

struct ValItem {
    kind: ValKind,
    num: f32,
    unit: Vec<u8>,
    text: Vec<u8>,
    color: u32,
}

const MAX_VALUE_ITEMS: usize = 8;

fn parse_number(c: &mut Cur) -> Option<f32> {
    let s = c.i;
    if c.peek() == b'-' || c.peek() == b'+' {
        c.i += 1;
    }
    let mut digits = false;
    while c.peek().is_ascii_digit() {
        c.i += 1;
        digits = true;
    }
    if c.peek() == b'.' {
        c.i += 1;
        while c.peek().is_ascii_digit() {
            c.i += 1;
            digits = true;
        }
    }
    if !digits {
        c.i = s;
        return None;
    }
    // C ロケール非依存の手動変換（f32 で累積）
    let mut v: f32 = 0.0;
    let mut frac: f32 = 0.1;
    let mut i = s;
    let mut neg = false;
    if c.p[i] == b'-' {
        neg = true;
        i += 1;
    } else if c.p[i] == b'+' {
        i += 1;
    }
    while i < c.p.len() && c.p[i].is_ascii_digit() {
        v = v * 10.0 + (c.p[i] - b'0') as f32;
        i += 1;
    }
    if i < c.p.len() && c.p[i] == b'.' {
        i += 1;
        while i < c.p.len() && c.p[i].is_ascii_digit() {
            v += frac * (c.p[i] - b'0') as f32;
            frac *= 0.1;
            i += 1;
        }
    }
    Some(if neg { -v } else { v })
}

/// 値をトークン列に分解。`None` = 空/不正（値全体を棄却）。
fn lex_value(raw: &[u8]) -> Option<Vec<ValItem>> {
    let mut items: Vec<ValItem> = Vec::new();
    let mut c = Cur { p: raw, i: 0 };
    let mk = |kind: ValKind, num: f32, unit: Vec<u8>, text: Vec<u8>, color: u32| ValItem {
        kind,
        num,
        unit,
        text,
        color,
    };
    while c.i < c.p.len() {
        if items.len() >= MAX_VALUE_ITEMS {
            return None;
        }
        if Cur::ws(c.peek()) {
            c.i += 1;
            continue;
        }
        let ch = c.peek();
        if ch == b'/' && c.peek2() == b'*' {
            c.skip_ws_comments();
            continue;
        }
        if ch == b'"' || ch == b'\'' {
            items.push(mk(ValKind::Str, 0.0, Vec::new(), c.string().to_vec(), 0));
            continue;
        }
        if ch.is_ascii_digit() || ch == b'.' || ch == b'-' || ch == b'+' {
            let num = parse_number(&mut c)?;
            if c.peek() == b'%' {
                c.i += 1;
                items.push(mk(ValKind::Pct, num, Vec::new(), Vec::new(), 0));
            } else if ident_start(c.peek()) {
                let unit = c.ident().to_vec();
                items.push(mk(ValKind::Dim, num, unit, Vec::new(), 0));
            } else {
                items.push(mk(ValKind::Num, num, Vec::new(), Vec::new(), 0));
            }
            continue;
        }
        if ch == b'#' {
            let s = c.i;
            while c.i < c.p.len() && (hexv(c.p[c.i]) >= 0 || c.p[c.i] == b'#') {
                c.i += 1;
            }
            let col = css_color(&raw[s..c.i])?;
            items.push(mk(ValKind::Color, 0.0, Vec::new(), Vec::new(), col));
            continue;
        }
        if ident_start(ch) {
            let id = c.ident();
            if c.peek() == b'(' {
                let s = c.i - id.len();
                let mut depth = 0usize;
                while c.i < c.p.len() {
                    if c.p[c.i] == b'(' {
                        depth += 1;
                    } else if c.p[c.i] == b')' {
                        depth -= 1;
                        if depth == 0 {
                            c.i += 1;
                            break;
                        }
                    }
                    c.i += 1;
                }
                let col = css_color(&raw[s..c.i])?;
                items.push(mk(ValKind::Color, 0.0, Vec::new(), Vec::new(), col));
                continue;
            }
            if let Some(col) = css_color(id) {
                items.push(mk(ValKind::Color, 0.0, Vec::new(), Vec::new(), col));
                continue;
            }
            if str_eq_ci(id, b"auto") {
                items.push(mk(ValKind::Auto, 0.0, Vec::new(), Vec::new(), 0));
                continue;
            }
            items.push(mk(ValKind::Ident, 0.0, Vec::new(), id.to_vec(), 0));
            continue;
        }
        if ch == b',' || ch == b'/' {
            c.i += 1;
            continue;
        }
        return None;
    }
    Some(items)
}

// ================= 長さ =================

/// 長さ単位 px（C の `IF_U_PX`）。
pub const U_PX: u8 = 0;
/// 長さ単位 em。
pub const U_EM: u8 = 1;
/// 長さ単位 rem。
pub const U_REM: u8 = 2;
/// 長さ単位 pt。
pub const U_PT: u8 = 3;
/// 長さ単位 %。
pub const U_PCT: u8 = 4;
/// 長さ単位 auto。
pub const U_AUTO: u8 = 5;

/// 長さ（C の `IfLen` 相当）。
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Len {
    /// 数値。
    pub v: f32,
    /// 単位（`U_*`）。
    pub unit: u8,
}

const LEN_AUTO: Len = Len {
    v: 0.0,
    unit: U_AUTO,
};

// ================= プロパティ表 =================

/// プロパティ ID: display（C の `IF_P_DISPLAY`）。
pub const P_DISPLAY: u16 = 0;
/// プロパティ ID: color。
pub const P_COLOR: u16 = 1;
/// プロパティ ID: background-color。
pub const P_BACKGROUND_COLOR: u16 = 2;
/// プロパティ ID: font-size。
pub const P_FONT_SIZE: u16 = 3;
/// プロパティ ID: font-weight。
pub const P_FONT_WEIGHT: u16 = 4;
/// プロパティ ID: font-style。
pub const P_FONT_STYLE: u16 = 5;
/// プロパティ ID: text-decoration。
pub const P_TEXT_DECORATION: u16 = 6;
/// プロパティ ID: margin-top。
pub const P_MARGIN_TOP: u16 = 7;
/// プロパティ ID: margin-right。
pub const P_MARGIN_RIGHT: u16 = 8;
/// プロパティ ID: margin-bottom。
pub const P_MARGIN_BOTTOM: u16 = 9;
/// プロパティ ID: margin-left。
pub const P_MARGIN_LEFT: u16 = 10;
/// プロパティ ID: padding-top。
pub const P_PADDING_TOP: u16 = 11;
/// プロパティ ID: padding-right。
pub const P_PADDING_RIGHT: u16 = 12;
/// プロパティ ID: padding-bottom。
pub const P_PADDING_BOTTOM: u16 = 13;
/// プロパティ ID: padding-left。
pub const P_PADDING_LEFT: u16 = 14;
/// プロパティ ID: border-top-width。
pub const P_BORDER_TOP_WIDTH: u16 = 15;
/// プロパティ ID: border-right-width。
pub const P_BORDER_RIGHT_WIDTH: u16 = 16;
/// プロパティ ID: border-bottom-width。
pub const P_BORDER_BOTTOM_WIDTH: u16 = 17;
/// プロパティ ID: border-left-width。
pub const P_BORDER_LEFT_WIDTH: u16 = 18;
/// プロパティ ID: border-color。
pub const P_BORDER_COLOR: u16 = 19;
/// プロパティ ID: width。
pub const P_WIDTH: u16 = 20;
/// プロパティ ID: height。
pub const P_HEIGHT: u16 = 21;
/// プロパティ ID: text-align。
pub const P_TEXT_ALIGN: u16 = 22;
/// プロパティ ID: line-height。
pub const P_LINE_HEIGHT: u16 = 23;
/// プロパティ ID: white-space。
pub const P_WHITE_SPACE: u16 = 24;
/// プロパティ総数（C の `IF_P_N`）。
pub const P_N: usize = 25;

const PROPS: &[(&str, u16)] = &[
    ("display", P_DISPLAY),
    ("color", P_COLOR),
    ("background-color", P_BACKGROUND_COLOR),
    ("font-size", P_FONT_SIZE),
    ("font-weight", P_FONT_WEIGHT),
    ("font-style", P_FONT_STYLE),
    ("text-decoration", P_TEXT_DECORATION),
    ("margin-top", P_MARGIN_TOP),
    ("margin-right", P_MARGIN_RIGHT),
    ("margin-bottom", P_MARGIN_BOTTOM),
    ("margin-left", P_MARGIN_LEFT),
    ("padding-top", P_PADDING_TOP),
    ("padding-right", P_PADDING_RIGHT),
    ("padding-bottom", P_PADDING_BOTTOM),
    ("padding-left", P_PADDING_LEFT),
    ("border-top-width", P_BORDER_TOP_WIDTH),
    ("border-right-width", P_BORDER_RIGHT_WIDTH),
    ("border-bottom-width", P_BORDER_BOTTOM_WIDTH),
    ("border-left-width", P_BORDER_LEFT_WIDTH),
    ("border-color", P_BORDER_COLOR),
    ("width", P_WIDTH),
    ("height", P_HEIGHT),
    ("text-align", P_TEXT_ALIGN),
    ("line-height", P_LINE_HEIGHT),
    ("white-space", P_WHITE_SPACE),
];

fn prop_id(name: &[u8]) -> Option<u16> {
    PROPS
        .iter()
        .find(|(n, _)| str_eq_ci(name, n.as_bytes()))
        .map(|(_, p)| *p)
}

// ================= 宣言 =================

/// 値種別: 長さ（C の `IF_V_LEN`）。
pub const V_LEN: u8 = 0;
/// 値種別: 識別子。
pub const V_IDENT: u8 = 1;
/// 値種別: 色。
pub const V_COLOR: u8 = 2;
/// 値種別: 生テキスト。
pub const V_RAW: u8 = 3;

/// 宣言（C の `IfDecl` 相当）。
#[derive(Clone, Debug, PartialEq)]
pub struct Decl {
    /// プロパティ ID（`P_*`）。
    pub prop: u16,
    /// `!important`。
    pub important: bool,
    /// 値種別（`V_*`）。
    pub vkind: u8,
    /// 単位（`U_*`。`V_LEN` のみ）。
    pub unit: u8,
    /// 数値（`V_LEN`）。
    pub num: f32,
    /// 色（`V_COLOR`）。
    pub color: u32,
    /// テキスト（`V_IDENT` / `V_RAW`）。
    pub text: Vec<u8>,
}

struct DeclSink {
    decls: Vec<Decl>,
    dropped: u32,
}

impl DeclSink {
    #[allow(clippy::too_many_arguments)]
    fn push(
        &mut self,
        prop: u16,
        important: bool,
        vkind: u8,
        num: f32,
        unit: u8,
        color: u32,
        text: Vec<u8>,
    ) {
        self.decls.push(Decl {
            prop,
            important,
            vkind,
            unit,
            num,
            color,
            text,
        });
    }

    fn push_len(&mut self, prop: u16, important: bool, l: Len) {
        self.push(prop, important, V_LEN, l.v, l.unit, 0, Vec::new());
    }
}

fn item_to_len(it: &ValItem) -> Option<Len> {
    match it.kind {
        ValKind::Auto => Some(LEN_AUTO),
        ValKind::Num => {
            if it.num == 0.0 {
                Some(Len { v: 0.0, unit: U_PX })
            } else {
                None
            }
        }
        ValKind::Pct => Some(Len {
            v: it.num,
            unit: U_PCT,
        }),
        ValKind::Dim => {
            if str_eq_ci(&it.unit, b"px") {
                Some(Len {
                    v: it.num,
                    unit: U_PX,
                })
            } else if str_eq_ci(&it.unit, b"em") {
                Some(Len {
                    v: it.num,
                    unit: U_EM,
                })
            } else if str_eq_ci(&it.unit, b"rem") {
                Some(Len {
                    v: it.num,
                    unit: U_REM,
                })
            } else if str_eq_ci(&it.unit, b"pt") {
                Some(Len {
                    v: it.num,
                    unit: U_PT,
                })
            } else {
                None
            }
        }
        _ => None,
    }
}

fn expand_4(n: usize, l: &mut [Len; 4]) {
    l[1] = if n > 1 { l[1] } else { l[0] };
    l[2] = if n > 2 { l[2] } else { l[0] };
    l[3] = if n > 3 { l[3] } else { l[1] };
}

/// 1 宣言（name: value 済み）を sink に展開して押し込む。
fn decl_one(s: &mut DeclSink, name: &[u8], value: &[u8], important: bool) {
    if str_eq_ci(name, b"margin") || str_eq_ci(name, b"padding") {
        match lex_value(value) {
            Some(items) if !items.is_empty() && items.len() <= 4 => {
                let mut l = [LEN_AUTO; 4];
                for (i, it) in items.iter().enumerate() {
                    match item_to_len(it) {
                        Some(x) => l[i] = x,
                        None => {
                            s.dropped += 1;
                            return;
                        }
                    }
                }
                let n = items.len();
                expand_4(n, &mut l);
                let base = if str_eq_ci(name, b"margin") {
                    P_MARGIN_TOP
                } else {
                    P_PADDING_TOP
                };
                for (i, &x) in l.iter().enumerate() {
                    s.push_len(base + i as u16, important, x);
                }
            }
            _ => s.dropped += 1,
        }
        return;
    }
    if str_eq_ci(name, b"border") {
        match lex_value(value) {
            Some(items) if !items.is_empty() => {
                let mut have_w = false;
                let mut have_c = false;
                let mut none = false;
                let mut w = Len { v: 1.0, unit: U_PX };
                let mut col = 0u32;
                for it in &items {
                    if let Some(l) = item_to_len(it) {
                        w = l;
                        have_w = true;
                        continue;
                    }
                    if it.kind == ValKind::Color {
                        col = it.color;
                        have_c = true;
                        continue;
                    }
                    if it.kind == ValKind::Ident {
                        if str_eq_ci(&it.text, b"none") || str_eq_ci(&it.text, b"hidden") {
                            none = true;
                            continue;
                        }
                        if str_eq_ci(&it.text, b"solid")
                            || str_eq_ci(&it.text, b"dotted")
                            || str_eq_ci(&it.text, b"dashed")
                            || str_eq_ci(&it.text, b"double")
                        {
                            continue;
                        }
                    }
                    s.dropped += 1;
                    return;
                }
                if none {
                    w.v = 0.0;
                    w.unit = U_PX;
                }
                if !have_w && !none {
                    s.dropped += 1;
                    return;
                }
                for i in 0..4 {
                    s.push_len(P_BORDER_TOP_WIDTH + i, important, w);
                }
                if have_c {
                    s.push(P_BORDER_COLOR, important, V_COLOR, 0.0, 0, col, Vec::new());
                }
            }
            _ => s.dropped += 1,
        }
        return;
    }
    if str_eq_ci(name, b"background") {
        if let Some(items) = lex_value(value) {
            for it in &items {
                if it.kind == ValKind::Color {
                    s.push(
                        P_BACKGROUND_COLOR,
                        important,
                        V_COLOR,
                        0.0,
                        0,
                        it.color,
                        Vec::new(),
                    );
                    return;
                }
            }
        }
        s.dropped += 1;
        return;
    }
    if str_eq_ci(name, b"border-width") {
        match lex_value(value) {
            Some(items) if !items.is_empty() && items.len() <= 4 => {
                let mut l = [LEN_AUTO; 4];
                for (i, it) in items.iter().enumerate() {
                    match item_to_len(it) {
                        Some(x) => l[i] = x,
                        None => {
                            s.dropped += 1;
                            return;
                        }
                    }
                }
                let n = items.len();
                expand_4(n, &mut l);
                for (i, &x) in l.iter().enumerate() {
                    s.push_len(P_BORDER_TOP_WIDTH + i as u16, important, x);
                }
            }
            _ => s.dropped += 1,
        }
        return;
    }
    if str_eq_ci(name, b"font")
        || str_eq_ci(name, b"list-style")
        || str_eq_ci(name, b"text-decoration-line")
        || str_eq_ci(name, b"flex")
        || str_eq_ci(name, b"grid")
        || str_eq_ci(name, b"animation")
        || str_eq_ci(name, b"transition")
    {
        s.dropped += 1;
        return;
    }

    let p = match prop_id(name) {
        Some(p) => p,
        None => {
            s.dropped += 1;
            return;
        }
    };

    // color 系
    if p == P_COLOR || p == P_BACKGROUND_COLOR || p == P_BORDER_COLOR {
        if str_eq_ci(trim(value), b"currentcolor") {
            s.dropped += 1;
            return;
        }
        match css_color(value) {
            Some(col) => s.push(p, important, V_COLOR, 0.0, 0, col, Vec::new()),
            None => s.dropped += 1,
        }
        return;
    }

    // length 系
    if matches!(
        p,
        P_MARGIN_TOP
            | P_MARGIN_RIGHT
            | P_MARGIN_BOTTOM
            | P_MARGIN_LEFT
            | P_PADDING_TOP
            | P_PADDING_RIGHT
            | P_PADDING_BOTTOM
            | P_PADDING_LEFT
            | P_WIDTH
            | P_HEIGHT
            | P_BORDER_TOP_WIDTH
            | P_BORDER_RIGHT_WIDTH
            | P_BORDER_BOTTOM_WIDTH
            | P_BORDER_LEFT_WIDTH
    ) {
        match lex_value(value) {
            Some(items)
                if items.len() == 1
                    && items[0].kind == ValKind::Ident
                    && (P_BORDER_TOP_WIDTH..=P_BORDER_LEFT_WIDTH).contains(&p) =>
            {
                let w = if str_eq_ci(&items[0].text, b"thin") {
                    1.0
                } else if str_eq_ci(&items[0].text, b"medium") {
                    3.0
                } else if str_eq_ci(&items[0].text, b"thick") {
                    5.0
                } else {
                    -1.0
                };
                if w < 0.0 {
                    s.dropped += 1;
                    return;
                }
                s.push_len(p, important, Len { v: w, unit: U_PX });
                return;
            }
            Some(items) if items.len() == 1 => {
                let l = match item_to_len(&items[0]) {
                    Some(l) => l,
                    None => {
                        s.dropped += 1;
                        return;
                    }
                };
                if matches!(
                    p,
                    P_PADDING_TOP | P_PADDING_RIGHT | P_PADDING_BOTTOM | P_PADDING_LEFT
                ) && l.unit == U_AUTO
                {
                    s.dropped += 1;
                    return;
                }
                s.push_len(p, important, l);
                return;
            }
            _ => s.dropped += 1,
        }
        return;
    }

    // font-size / line-height
    if p == P_FONT_SIZE || p == P_LINE_HEIGHT {
        if let Some(items) = lex_value(value) {
            if items.len() == 1 {
                if let Some(l) = item_to_len(&items[0]) {
                    s.push_len(p, important, l);
                    return;
                }
                if p == P_LINE_HEIGHT && items[0].kind == ValKind::Num && items[0].num < 0.0 {
                    s.dropped += 1;
                    return;
                }
            }
        }
        let v = trim(value).to_vec();
        s.push(p, important, V_RAW, 0.0, 0, 0, v);
        return;
    }

    // 残りは raw
    let v = trim(value).to_vec();
    s.push(p, important, V_RAW, 0.0, 0, 0, v);
}

/// 宣言ブロック（'{' の中身 or inline style）をパース。C の `parse_decl_block` 相当。
fn parse_decl_block(s: &mut DeclSink, text: &[u8]) {
    let mut c = Cur { p: text, i: 0 };
    while c.i < c.p.len() {
        let stmt_start = c.i;
        // ';' までを 1 宣言として切り出す（括弧と文字列を考慮）
        let mut depth = 0usize;
        let mut in_str = 0u8;
        while c.i < c.p.len() {
            let ch = c.p[c.i];
            if in_str != 0 {
                if ch == in_str {
                    in_str = 0;
                } else if ch == b'\\' {
                    c.i += 1;
                }
            } else if ch == b'"' || ch == b'\'' {
                in_str = ch;
            } else if ch == b'(' {
                depth += 1;
            } else if ch == b')' && depth != 0 {
                depth -= 1;
            } else if ch == b';' && depth == 0 {
                break;
            }
            c.i += 1;
        }
        let stmt = &text[stmt_start..c.i];
        if c.i < c.p.len() {
            c.i += 1; // ';'
        }

        // name: value に分解
        let colon = stmt.iter().position(|&b| b == b':');
        let colon = match colon {
            Some(x) => x,
            None => {
                if trim(stmt).is_empty() {
                    continue;
                }
                s.dropped += 1;
                continue;
            }
        };
        let name = trim(&stmt[..colon]);
        let value = trim(&stmt[colon + 1..]);
        if name.is_empty() || !ident_start(name[0]) {
            s.dropped += 1;
            continue;
        }

        // !important の検出（末尾）
        let mut important = false;
        let mut v = value;
        {
            let mut k = v.len();
            while k > 0 && Cur::ws(v[k - 1]) {
                k -= 1;
            }
            if k >= 9 {
                let tail = &v[k - 9..k];
                let mut h = k - 9;
                while h > 0 && Cur::ws(v[h - 1]) {
                    h -= 1;
                }
                if str_eq_ci(tail, b"important") && h > 0 && v[h - 1] == b'!' {
                    important = true;
                    v = trim(&v[..h - 1]);
                }
            }
        }
        decl_one(s, name, v, important);
    }
}

/// 宣言列（inline style 属性用）をパース。C の `if_css_parse_decls` 相当。
pub fn parse_decls(text: &[u8]) -> Vec<Decl> {
    let mut s = DeclSink {
        decls: Vec::new(),
        dropped: 0,
    };
    parse_decl_block(&mut s, text);
    s.decls
}

// ================= セレクタ =================

/// 結合子: 子孫（C の `IF_CX_DESCENDANT`）。
pub const CX_DESCENDANT: u8 = 0;
/// 結合子: 子。
pub const CX_CHILD: u8 = 1;

/// 複合セレクタ（C の `IfCompound` 相当）。
#[derive(Clone, Debug, Default)]
pub struct Compound {
    /// タグあり（type セレクタ）。
    pub has_tag: bool,
    /// 既知タグ ID（未知は `TAG_UNKNOWN`）。
    pub tag: Tag,
    /// 未知タグ名（CI 照合。既知タグは空）。
    pub tag_name: Vec<u8>,
    /// クラス列。
    pub classes: Vec<Vec<u8>>,
    /// id 列。
    pub ids: Vec<Vec<u8>>,
}

/// セレクタ（C の `IfSelector` 相当）。
#[derive(Clone, Debug)]
pub struct Selector {
    /// 左→右の複合列。
    pub comps: Vec<Compound>,
    /// `combs[i]` = comps[i] と comps[i+1] の間の結合子。
    pub combs: Vec<u8>,
    /// specificity `(ids<<16)|(classes<<8)|types`。
    pub spec: u32,
}

fn parse_compound(c: &mut Cur, out: &mut Compound) -> bool {
    let mut any = false;
    out.has_tag = false;
    out.tag = tags::TAG_UNKNOWN;
    out.tag_name.clear();
    out.classes.clear();
    out.ids.clear();
    loop {
        let ch = c.peek();
        if ch == b'*' {
            if any {
                return false;
            }
            any = true;
            c.i += 1;
            continue;
        }
        if ch == b'.' {
            c.i += 1;
            let id = c.ident();
            if id.is_empty() {
                return false;
            }
            out.classes.push(id.to_vec());
            any = true;
            continue;
        }
        if ch == b'#' {
            c.i += 1;
            let id = c.ident();
            if id.is_empty() {
                return false;
            }
            out.ids.push(id.to_vec());
            any = true;
            continue;
        }
        if ident_start(ch) || ch == b'\\' {
            if ch == b'\\' {
                return false;
            }
            if out.has_tag {
                return false;
            }
            let t = c.ident();
            out.has_tag = true;
            out.tag = tags::tag_id(t);
            if out.tag == tags::TAG_UNKNOWN {
                out.tag_name = t.to_vec();
            }
            any = true;
            continue;
        }
        break;
    }
    if !any {
        return false;
    }
    true
}

/// カンマ区切りのセレクタ群をパース。C の `parse_selector_list` 相当。
fn parse_selector_list(raw: &[u8]) -> Vec<Selector> {
    let mut sels: Vec<Selector> = Vec::new();
    let mut c = Cur { p: raw, i: 0 };
    while c.i < c.p.len() {
        let s = c.i;
        let mut depth = 0usize;
        let mut in_str = 0u8;
        while c.i < c.p.len() {
            let ch = c.p[c.i];
            if in_str != 0 {
                if ch == in_str {
                    in_str = 0;
                }
            } else if ch == b'"' || ch == b'\'' {
                in_str = ch;
            } else if ch == b'(' || ch == b'[' {
                depth += 1;
            } else if ch == b')' || ch == b']' {
                depth = depth.saturating_sub(1);
            } else if ch == b',' && depth == 0 {
                break;
            }
            c.i += 1;
        }
        let one = trim(&raw[s..c.i]);
        if c.i < c.p.len() && c.p[c.i] == b',' {
            c.i += 1;
        }
        if one.is_empty() {
            continue;
        }

        let mut comps: Vec<Compound> = Vec::new();
        let mut combs: Vec<u8> = Vec::new();
        let mut valid = true;
        let mut p = Cur { p: one, i: 0 };
        let mut pending_comb = CX_DESCENDANT;
        let mut need_compound = true;
        while p.i < p.p.len() {
            if Cur::ws(p.peek()) || (p.peek() == b'/' && p.peek2() == b'*') {
                p.skip_ws_comments();
                if p.i < p.p.len() && p.peek() != b'>' && !comps.is_empty() && !need_compound {
                    pending_comb = CX_DESCENDANT;
                    need_compound = true;
                }
                continue;
            }
            if p.peek() == b'>' {
                if need_compound || comps.is_empty() {
                    valid = false;
                    break;
                }
                p.i += 1;
                pending_comb = CX_CHILD;
                need_compound = true;
                continue;
            }
            if p.peek() == b'+'
                || p.peek() == b'~'
                || p.peek() == b':'
                || p.peek() == b'['
                || p.peek() == b','
            {
                valid = false;
                break;
            }
            let mut comp = Compound::default();
            if !parse_compound(&mut p, &mut comp) {
                valid = false;
                break;
            }
            if !comps.is_empty() {
                if !need_compound {
                    valid = false;
                    break;
                }
                combs.push(pending_comb);
            }
            comps.push(comp);
            need_compound = false;
        }
        if valid && need_compound && !comps.is_empty() {
            valid = false;
        }
        if valid && !comps.is_empty() {
            let mut ids = 0u32;
            let mut cls = 0u32;
            let mut typ = 0u32;
            for c_ in &comps {
                ids += c_.ids.len() as u32;
                cls += c_.classes.len() as u32;
                if c_.has_tag {
                    typ += 1;
                }
            }
            let spec = (ids << 16) | (cls << 8) | typ;
            sels.push(Selector { comps, combs, spec });
        }
    }
    sels
}

// ================= スタイルシート =================

/// ルール（C の `IfRule` 相当）。
#[derive(Clone, Debug)]
pub struct Rule {
    /// セレクタ列。
    pub sels: Vec<Selector>,
    /// 宣言列。
    pub decls: Vec<Decl>,
    /// decl 単位で一意な単調 base。
    pub order: u32,
}

/// スタイルシート（C の `IfStyleSheet` 相当）。
#[derive(Clone, Debug, Default)]
pub struct StyleSheet {
    /// ルール列。
    pub rules: Vec<Rule>,
    /// 棄却ルール数。
    pub n_dropped_rules: u32,
    /// 棄却宣言数。
    pub n_dropped_decls: u32,
    /// 消費した order の排他上端。
    pub order_end: u32,
}

/// スタイルシートをパース。C の `if_css_parse` 相当。
pub fn parse_stylesheet(css: &[u8], order_base: u32) -> StyleSheet {
    let mut sh = StyleSheet {
        rules: Vec::new(),
        n_dropped_rules: 0,
        n_dropped_decls: 0,
        order_end: order_base,
    };
    let mut order = order_base;
    let mut c = Cur { p: css, i: 0 };

    while c.i < c.p.len() {
        c.skip_ws_comments();
        if c.i >= c.p.len() {
            break;
        }

        // @-rule は丸ごと棄却
        if c.p[c.i] == b'@' {
            while c.i < c.p.len() && c.p[c.i] != b'{' && c.p[c.i] != b';' {
                c.i += 1;
            }
            if c.i < c.p.len() && c.p[c.i] == b'{' {
                let mut depth = 1usize;
                c.i += 1;
                while c.i < c.p.len() && depth != 0 {
                    if c.p[c.i] == b'{' {
                        depth += 1;
                    } else if c.p[c.i] == b'}' {
                        depth -= 1;
                    }
                    c.i += 1;
                }
            } else if c.i < c.p.len() {
                c.i += 1;
            }
            sh.n_dropped_rules += 1;
            continue;
        }

        // プリリュード（セレクタ群）: '{' まで
        let s = c.i;
        let mut depth = 0usize;
        let mut in_str = 0u8;
        while c.i < c.p.len() {
            let ch = c.p[c.i];
            if in_str != 0 {
                if ch == in_str {
                    in_str = 0;
                }
            } else if ch == b'"' || ch == b'\'' {
                in_str = ch;
            } else if ch == b'(' || ch == b'[' {
                depth += 1;
            } else if ch == b')' || ch == b']' {
                depth = depth.saturating_sub(1);
            } else if (ch == b'{' || ch == b'}') && depth == 0 {
                break;
            }
            c.i += 1;
        }
        let prelude = trim(&css[s..c.i]);
        if c.i >= c.p.len() || c.p[c.i] != b'{' {
            if c.i < c.p.len() {
                c.i += 1;
            }
            sh.n_dropped_rules += 1;
            continue;
        }
        c.i += 1; // '{'

        // ブロック中身: 対応 '}' まで
        let bs = c.i;
        depth = 0;
        in_str = 0;
        while c.i < c.p.len() {
            let ch = c.p[c.i];
            if in_str != 0 {
                if ch == in_str {
                    in_str = 0;
                } else if ch == b'\\' {
                    c.i += 1;
                }
            } else if ch == b'"' || ch == b'\'' {
                in_str = ch;
            } else if ch == b'(' || ch == b'[' || ch == b'{' {
                depth += 1;
            } else if ch == b')' || ch == b']' {
                depth = depth.saturating_sub(1);
            } else if ch == b'}' {
                if depth == 0 {
                    break;
                }
                depth -= 1;
            }
            c.i += 1;
        }
        let body = &css[bs..c.i];
        if c.i < c.p.len() {
            c.i += 1; // '}'
        }

        let sels = parse_selector_list(prelude);
        let mut sink = DeclSink {
            decls: Vec::new(),
            dropped: 0,
        };
        parse_decl_block(&mut sink, body);
        sh.n_dropped_decls += sink.dropped;

        if sels.is_empty() || sink.decls.is_empty() {
            if sels.is_empty() {
                sh.n_dropped_rules += 1;
            }
            continue;
        }
        let rule = Rule {
            sels,
            decls: sink.decls,
            order,
        };
        order += rule.decls.len() as u32;
        sh.rules.push(rule);
    }
    sh.order_end = order;
    sh
}

// ================= マッチャ =================

fn elem_parent(dom: &Dom, n: NodeId) -> Option<NodeId> {
    let mut p = dom.node(n).parent;
    while let Some(pid) = p {
        if dom.node(pid).kind == NodeKind::Element {
            return Some(pid);
        }
        p = dom.node(pid).parent;
    }
    None
}

fn match_compound(dom: &Dom, n: NodeId, cp: &Compound) -> bool {
    let node = dom.node(n);
    if node.kind != NodeKind::Element {
        return false;
    }
    if cp.has_tag {
        if cp.tag != tags::TAG_UNKNOWN {
            if node.tag != cp.tag {
                return false;
            }
        } else if node.tag != tags::TAG_UNKNOWN || !str_eq_ci(&node.name, &cp.tag_name) {
            return false;
        }
    }
    for cls in &cp.classes {
        if !dom.has_class(n, cls) {
            return false;
        }
    }
    for id in &cp.ids {
        match dom.attr(n, b"id") {
            Some(v) if v == id.as_slice() => {}
            _ => return false,
        }
    }
    true
}

fn match_at(dom: &Dom, n: NodeId, sel: &Selector, i: usize) -> bool {
    if !match_compound(dom, n, &sel.comps[i]) {
        return false;
    }
    if i == 0 {
        return true;
    }
    let comb = sel.combs[i - 1];
    if comb == CX_CHILD {
        return match elem_parent(dom, n) {
            Some(p) => match_at(dom, p, sel, i - 1),
            None => false,
        };
    }
    let mut p = elem_parent(dom, n);
    while let Some(pid) = p {
        if match_at(dom, pid, sel, i - 1) {
            return true;
        }
        p = elem_parent(dom, pid);
    }
    false
}

/// セレクタがノードにマッチするか。C の `if_css_match_selector` 相当。
pub fn match_selector(dom: &Dom, n: NodeId, sel: &Selector) -> bool {
    if sel.comps.is_empty() {
        return false;
    }
    match_at(dom, n, sel, sel.comps.len() - 1)
}

// ================= カスケード =================

const UA_SHEET: &str = "html,body{display:block}\
body{margin:8px;font-size:16px;color:#000;background-color:#fff;line-height:1.2}\
head,title,meta,link,style,script,input,select,textarea,button,object,iframe,\
param,source,track,video,audio,canvas,option{display:none}\
div,p,pre,blockquote,address,center,figure,figcaption,header,footer,nav,main,\
section,article,aside,form,dl,noscript{display:block}\
h1{display:block;font-size:2em;font-weight:bold;margin-top:0.67em;margin-bottom:0.67em}\
h2{display:block;font-size:1.5em;font-weight:bold;margin-top:0.83em;margin-bottom:0.83em}\
h3{display:block;font-size:1.17em;font-weight:bold;margin-top:1em;margin-bottom:1em}\
h4,h5,h6{display:block;font-weight:bold;margin-top:1.33em;margin-bottom:1.33em}\
p{margin-top:1em;margin-bottom:1em}\
ul,ol{display:block;margin-top:1em;margin-bottom:1em;padding-left:40px}\
li{display:list-item}\
dd{display:block;margin-left:40px}\
dt{display:block}\
pre{white-space:pre;margin-top:1em;margin-bottom:1em}\
blockquote{display:block;margin-left:40px;margin-right:40px;margin-top:1em;margin-bottom:1em}\
b,strong{font-weight:bold}\
i,em,cite,dfn,var{font-style:italic}\
s,strike{text-decoration:line-through}\
u{text-decoration:underline}\
a{color:#0000ee;text-decoration:underline}\
mark{background-color:#ffff00;color:#000}\
hr{display:block;margin-top:0.5em;margin-bottom:0.5em}\
small,sub,sup{font-size:0.83em}\
big{font-size:1.17em}\
table,thead,tbody,tr{display:block}\
td,th{display:block}\
th{font-weight:bold}\
caption{display:block;text-align:center}";

const ORIGIN_UA: u8 = 0;
const ORIGIN_AUTHOR: u8 = 1;
const ORIGIN_INLINE: u8 = 2;

/// 表示種別: inline（C の `IF_D_INLINE`）。
pub const D_INLINE: u8 = 0;
/// 表示種別: block。
pub const D_BLOCK: u8 = 1;
/// 表示種別: list-item。
pub const D_LIST_ITEM: u8 = 2;
/// 表示種別: none。
pub const D_NONE: u8 = 3;

/// テキスト整列: left（C の `IF_TA_LEFT`）。
pub const TA_LEFT: u8 = 0;
/// テキスト整列: center。
pub const TA_CENTER: u8 = 1;
/// テキスト整列: right。
pub const TA_RIGHT: u8 = 2;

/// 空白処理: normal（C の `IF_WS_NORMAL`）。
pub const WS_NORMAL: u8 = 0;
/// 空白処理: pre。
pub const WS_PRE: u8 = 1;

/// 計算済みスタイル（C の `IfStyle` 相当）。
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Style {
    /// 文字色（RGBA8）。
    pub color: u32,
    /// 背景色（RGBA8。alpha 0 は透過）。
    pub bg: u32,
    /// フォントサイズ（px 解決済み）。
    pub font_size: f32,
    /// 行高（px。0 = auto）。
    pub line_height: f32,
    /// 幅。
    pub width: Len,
    /// 高さ。
    pub height: Len,
    /// マージン T R B L。
    pub margin: [Len; 4],
    /// パディング T R B L。
    pub padding: [Len; 4],
    /// ボーダー幅（px）T R B L。
    pub border_w: [f32; 4],
    /// ボーダー色。
    pub border_color: u32,
    /// 表示種別（`D_*`）。
    pub display: u8,
    /// テキスト整列（`TA_*`）。
    pub text_align: u8,
    /// 空白処理（`WS_*`）。
    pub white_space: u8,
    /// 太字。
    pub bold: bool,
    /// 斜体。
    pub italic: bool,
    /// 下線。
    pub underline: bool,
    /// 打ち消し線。
    pub strike: bool,
}

struct Winner {
    spec: u32,
    order: u32,
    important: bool,
    origin: u8,
    decl: Decl,
}

fn winner_beats(w: &Winner, cur: &Option<Winner>) -> bool {
    match cur {
        None => true,
        Some(c) => {
            if w.important != c.important {
                return w.important;
            }
            if w.origin != c.origin {
                return w.origin > c.origin;
            }
            if w.spec != c.spec {
                return w.spec > c.spec;
            }
            w.order > c.order
        }
    }
}

fn collect_from_sheet(
    dom: &Dom,
    n: NodeId,
    sh: &StyleSheet,
    origin: u8,
    win: &mut [Option<Winner>],
) {
    for rule in &sh.rules {
        let mut best_spec = 0u32;
        let mut matched = false;
        for sel in &rule.sels {
            if match_selector(dom, n, sel) {
                matched = true;
                if sel.spec > best_spec {
                    best_spec = sel.spec;
                }
            }
        }
        if !matched {
            continue;
        }
        for (d, decl) in rule.decls.iter().enumerate() {
            let w = Winner {
                spec: best_spec,
                order: rule.order + d as u32,
                important: decl.important,
                origin,
                decl: decl.clone(),
            };
            let p = decl.prop as usize;
            if winner_beats(&w, &win[p]) {
                win[p] = Some(w);
            }
        }
    }
}

/// px へ解決。C の `if_css_resolve_len` 相当。
pub fn resolve_len(l: Len, self_fs: f32, root_fs: f32) -> f32 {
    match l.unit {
        U_PX => l.v,
        U_PT => l.v * (96.0 / 72.0),
        U_EM => l.v * self_fs,
        U_REM => l.v * root_fs,
        _ => 0.0,
    }
}

fn kw_font_size(v: &[u8], parent: f32) -> f32 {
    if str_eq_ci(v, b"xx-small") {
        9.0
    } else if str_eq_ci(v, b"x-small") {
        10.0
    } else if str_eq_ci(v, b"small") {
        13.0
    } else if str_eq_ci(v, b"medium") {
        16.0
    } else if str_eq_ci(v, b"large") {
        18.0
    } else if str_eq_ci(v, b"x-large") {
        24.0
    } else if str_eq_ci(v, b"xx-large") {
        32.0
    } else if str_eq_ci(v, b"smaller") {
        parent / 1.2
    } else if str_eq_ci(v, b"larger") {
        parent * 1.2
    } else {
        -1.0
    }
}

const APPLY_ORDER: [u16; P_N] = [
    P_FONT_SIZE,
    P_LINE_HEIGHT,
    P_FONT_WEIGHT,
    P_FONT_STYLE,
    P_WHITE_SPACE,
    P_TEXT_ALIGN,
    P_COLOR,
    P_BACKGROUND_COLOR,
    P_TEXT_DECORATION,
    P_DISPLAY,
    P_MARGIN_TOP,
    P_MARGIN_RIGHT,
    P_MARGIN_BOTTOM,
    P_MARGIN_LEFT,
    P_PADDING_TOP,
    P_PADDING_RIGHT,
    P_PADDING_BOTTOM,
    P_PADDING_LEFT,
    P_BORDER_TOP_WIDTH,
    P_BORDER_RIGHT_WIDTH,
    P_BORDER_BOTTOM_WIDTH,
    P_BORDER_LEFT_WIDTH,
    P_BORDER_COLOR,
    P_WIDTH,
    P_HEIGHT,
];

/// 1 要素の決定解決（pure 値関数。C の `compute_node` 一点化の写し: eager walk /
/// 直接マップ / IfStyleLazy の 3 経路がこの同一手続きを共有する。戻り値は intern 前の
/// 自由態 style）。
fn compute_style(
    dom: &Dom,
    n: NodeId,
    parent_st: Option<&Style>,
    root_fs: f32,
    sheets: &[&StyleSheet],
) -> Style {
    // 1) 初期値 + 継承
    let mut st = Style {
        color: 0,
        bg: 0,
        font_size: 0.0,
        line_height: 0.0,
        width: LEN_AUTO,
        height: LEN_AUTO,
        margin: [Len { v: 0.0, unit: U_PX }; 4],
        padding: [Len { v: 0.0, unit: U_PX }; 4],
        border_w: [0.0; 4],
        border_color: 0,
        display: D_INLINE,
        text_align: TA_LEFT,
        white_space: WS_NORMAL,
        bold: false,
        italic: false,
        underline: false,
        strike: false,
    };
    st.color = parent_st.map_or(rgba8(0, 0, 0, 255), |p| p.color);
    st.font_size = parent_st.map_or(16.0, |p| p.font_size);
    st.bold = parent_st.is_some_and(|p| p.bold);
    st.italic = parent_st.is_some_and(|p| p.italic);
    st.underline = parent_st.is_some_and(|p| p.underline);
    st.strike = parent_st.is_some_and(|p| p.strike);
    st.text_align = parent_st.map_or(TA_LEFT, |p| p.text_align);
    st.white_space = parent_st.map_or(WS_NORMAL, |p| p.white_space);
    if parent_st.is_some_and(|p| p.line_height > 0.0) {
        st.line_height = parent_st.unwrap().line_height;
    }

    // 2) 勝者収集
    let mut win: Vec<Option<Winner>> = (0..P_N).map(|_| None).collect();
    for (s, sh) in sheets.iter().enumerate() {
        let origin = if s == 0 { ORIGIN_UA } else { ORIGIN_AUTHOR };
        collect_from_sheet(dom, n, sh, origin, &mut win);
    }

    // 3) inline style
    if let Some(style_attr) = dom.attr(n, b"style") {
        if !style_attr.is_empty() {
            let decls = parse_decls(style_attr);
            for d in decls {
                let w = Winner {
                    spec: 0xFFFFFF,
                    order: 0xFFFFFF,
                    important: d.important,
                    origin: ORIGIN_INLINE,
                    decl: d.clone(),
                };
                let p = d.prop as usize;
                if winner_beats(&w, &win[p]) {
                    win[p] = Some(w);
                }
            }
        }
    }

    // 4) 適用
    let parent_fs = parent_st.map_or(16.0, |p| p.font_size);
    for &p in APPLY_ORDER.iter() {
        let w = match &win[p as usize] {
            Some(w) => w,
            None => continue,
        };
        let d = &w.decl;
        match p {
            P_FONT_SIZE => {
                let mut v = -1.0f32;
                if d.vkind == V_LEN {
                    let l = Len {
                        v: d.num,
                        unit: d.unit,
                    };
                    v = if l.unit == U_PCT {
                        parent_fs * l.v / 100.0
                    } else {
                        resolve_len(l, parent_fs, root_fs)
                    };
                } else if d.vkind == V_RAW {
                    v = kw_font_size(&d.text, parent_fs);
                }
                if v > 0.0 && v < 10000.0 {
                    st.font_size = v;
                }
            }
            P_LINE_HEIGHT => {
                if d.vkind == V_LEN {
                    let l = Len {
                        v: d.num,
                        unit: d.unit,
                    };
                    let v = if l.unit == U_PCT {
                        st.font_size * l.v / 100.0
                    } else {
                        resolve_len(l, st.font_size, root_fs)
                    };
                    if v > 0.0 && v < 10000.0 {
                        st.line_height = v;
                    }
                } else if d.vkind == V_RAW {
                    if str_eq_ci(&d.text, b"normal") {
                        st.line_height = 0.0;
                    } else {
                        // 無単位数値: font-size の倍率
                        let mut acc = 0.0f32;
                        let mut ok = true;
                        let mut digits = false;
                        let mut k = 0usize;
                        while k < d.text.len() {
                            let ch = d.text[k];
                            if ch.is_ascii_digit() {
                                acc = acc * 10.0 + (ch - b'0') as f32;
                                digits = true;
                            } else if ch == b'.' && digits {
                                k += 1;
                                let mut frac = 0.1f32;
                                while k < d.text.len() && d.text[k].is_ascii_digit() {
                                    acc += frac * (d.text[k] - b'0') as f32;
                                    frac *= 0.1;
                                    k += 1;
                                }
                                k -= 1;
                            } else {
                                ok = false;
                                break;
                            }
                            k += 1;
                        }
                        if ok && digits {
                            st.line_height = acc * st.font_size;
                        }
                    }
                }
            }
            P_FONT_WEIGHT => {
                if d.vkind == V_RAW {
                    let v = trim(&d.text);
                    if str_eq_ci(v, b"bold") || str_eq_ci(v, b"bolder") {
                        st.bold = true;
                    } else if str_eq_ci(v, b"normal") || str_eq_ci(v, b"lighter") {
                        st.bold = false;
                    } else {
                        let mut num: i64 = 0;
                        let mut ok = true;
                        for &b in v {
                            if !b.is_ascii_digit() {
                                ok = false;
                                break;
                            }
                            num = num * 10 + (b - b'0') as i64;
                        }
                        if ok && !v.is_empty() {
                            st.bold = num >= 600;
                        }
                    }
                }
            }
            P_FONT_STYLE => {
                if d.vkind == V_RAW {
                    let v = trim(&d.text);
                    st.italic = str_eq_ci(v, b"italic") || str_eq_ci(v, b"oblique");
                }
            }
            P_WHITE_SPACE => {
                if d.vkind == V_RAW {
                    let v = trim(&d.text);
                    if str_eq_ci(v, b"pre") || str_eq_ci(v, b"pre-wrap") {
                        st.white_space = WS_PRE;
                    } else if str_eq_ci(v, b"normal") || str_eq_ci(v, b"pre-line") {
                        st.white_space = WS_NORMAL;
                    }
                }
            }
            P_TEXT_ALIGN => {
                if d.vkind == V_RAW {
                    let v = trim(&d.text);
                    if str_eq_ci(v, b"center") {
                        st.text_align = TA_CENTER;
                    } else if str_eq_ci(v, b"right") || str_eq_ci(v, b"end") {
                        st.text_align = TA_RIGHT;
                    } else if str_eq_ci(v, b"left") || str_eq_ci(v, b"start") {
                        st.text_align = TA_LEFT;
                    }
                }
            }
            P_COLOR => {
                if d.vkind == V_COLOR {
                    st.color = d.color;
                }
            }
            P_BACKGROUND_COLOR => {
                if d.vkind == V_COLOR {
                    st.bg = d.color;
                }
            }
            P_TEXT_DECORATION => {
                if d.vkind == V_RAW {
                    let v = &d.text;
                    let mut i = 0usize;
                    st.underline = false;
                    st.strike = false;
                    while i < v.len() {
                        while i < v.len() && Cur::ws(v[i]) {
                            i += 1;
                        }
                        let s = i;
                        while i < v.len() && !Cur::ws(v[i]) {
                            i += 1;
                        }
                        let w = &v[s..i];
                        if str_eq_ci(w, b"underline") {
                            st.underline = true;
                        } else if str_eq_ci(w, b"line-through") {
                            st.strike = true;
                        } else if str_eq_ci(w, b"none") {
                            st.underline = false;
                            st.strike = false;
                        }
                    }
                }
            }
            P_DISPLAY => {
                if d.vkind == V_RAW {
                    let v = trim(&d.text);
                    if str_eq_ci(v, b"none") {
                        st.display = D_NONE;
                    } else if str_eq_ci(v, b"inline") {
                        st.display = D_INLINE;
                    } else if str_eq_ci(v, b"block") {
                        st.display = D_BLOCK;
                    } else if str_eq_ci(v, b"list-item") {
                        st.display = D_LIST_ITEM;
                    } else if str_eq_ci(v, b"inline-block") {
                        st.display = D_INLINE;
                    } else if str_eq_ci(v, b"table")
                        || str_eq_ci(v, b"table-row")
                        || str_eq_ci(v, b"table-cell")
                        || str_eq_ci(v, b"table-row-group")
                        || str_eq_ci(v, b"table-header-group")
                    {
                        st.display = D_BLOCK;
                    }
                }
            }
            P_MARGIN_TOP | P_MARGIN_RIGHT | P_MARGIN_BOTTOM | P_MARGIN_LEFT => {
                let idx = (p - P_MARGIN_TOP) as usize;
                if d.vkind == V_LEN {
                    st.margin[idx] = Len {
                        v: d.num,
                        unit: d.unit,
                    };
                }
            }
            P_PADDING_TOP | P_PADDING_RIGHT | P_PADDING_BOTTOM | P_PADDING_LEFT => {
                let idx = (p - P_PADDING_TOP) as usize;
                if d.vkind == V_LEN {
                    st.padding[idx] = Len {
                        v: d.num,
                        unit: d.unit,
                    };
                }
            }
            P_BORDER_TOP_WIDTH
            | P_BORDER_RIGHT_WIDTH
            | P_BORDER_BOTTOM_WIDTH
            | P_BORDER_LEFT_WIDTH => {
                let idx = (p - P_BORDER_TOP_WIDTH) as usize;
                if d.vkind == V_LEN {
                    let v = resolve_len(
                        Len {
                            v: d.num,
                            unit: d.unit,
                        },
                        st.font_size,
                        root_fs,
                    );
                    if (0.0..10000.0).contains(&v) {
                        st.border_w[idx] = v;
                    }
                }
            }
            P_BORDER_COLOR => {
                if d.vkind == V_COLOR {
                    st.border_color = d.color;
                }
            }
            P_WIDTH if d.vkind == V_LEN => {
                st.width = Len {
                    v: d.num,
                    unit: d.unit,
                };
            }
            P_HEIGHT if d.vkind == V_LEN => {
                st.height = Len {
                    v: d.num,
                    unit: d.unit,
                };
            }
            _ => {}
        }
    }

    st
}

// ================= 決定メモ化（C の IfStCacheEnt / st_resolve_memo / st_intern 写し） =================

/// 直接マップキャッシュのサイズ（C の `IF_STCACHE_BITS`=14 = 16384 写し）。
const STCACHE_BITS: u32 = 14;
const STCACHE_SIZE: usize = 1 << STCACHE_BITS;

/// `Len` のビット同一性キー。
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct LenKey {
    v: u32,
    unit: u8,
}

impl From<Len> for LenKey {
    fn from(l: Len) -> Self {
        LenKey {
            v: l.v.to_bits(),
            unit: l.unit,
        }
    }
}

/// `Style` のビット同一性キー（C の memset0 + memcmp 等価を fieldwise ビット抜き出しで
/// 構造保証。f32 は to_bits、bool は 0/1。値が等しい style は必ず同キー = exact memo の根拠）。
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct StyleKey {
    color: u32,
    bg: u32,
    font_size: u32,
    line_height: u32,
    width: LenKey,
    height: LenKey,
    margin: [LenKey; 4],
    padding: [LenKey; 4],
    border_w: [u32; 4],
    border_color: u32,
    display: u8,
    text_align: u8,
    white_space: u8,
    flags: u8,
}

impl From<&Style> for StyleKey {
    fn from(s: &Style) -> Self {
        let mut flags = 0u8;
        if s.bold {
            flags |= 1;
        }
        if s.italic {
            flags |= 2;
        }
        if s.underline {
            flags |= 4;
        }
        if s.strike {
            flags |= 8;
        }
        StyleKey {
            color: s.color,
            bg: s.bg,
            font_size: s.font_size.to_bits(),
            line_height: s.line_height.to_bits(),
            width: LenKey::from(s.width),
            height: LenKey::from(s.height),
            margin: [
                LenKey::from(s.margin[0]),
                LenKey::from(s.margin[1]),
                LenKey::from(s.margin[2]),
                LenKey::from(s.margin[3]),
            ],
            padding: [
                LenKey::from(s.padding[0]),
                LenKey::from(s.padding[1]),
                LenKey::from(s.padding[2]),
                LenKey::from(s.padding[3]),
            ],
            border_w: [
                s.border_w[0].to_bits(),
                s.border_w[1].to_bits(),
                s.border_w[2].to_bits(),
                s.border_w[3].to_bits(),
            ],
            border_color: s.border_color,
            display: s.display,
            text_align: s.text_align,
            white_space: s.white_space,
            flags,
        }
    }
}

/// C の `IfStyleIntern` 写し（computed style の値 dedup。idx が値の正準名 = parent
/// 同一性の測り方）。memcmp 等価は StyleKey の bit equality。
/// key→idx は負荷率 0.75 で ×2 成長する開放番地（C と同機構。SipHash ではなく
/// u32 語の乗算混合で ~5ns/probe — lazy 解決はノードごとに pk 逆引きを打つため
/// ここが最頻コストとなる。近似・失敗は無し: probe は必ず StyleKey 完全一致を検査）。
pub(crate) struct StyleIntern {
    styles: Vec<(Style, StyleKey)>,
    /// idx+1（0 = 空）。
    tab: Vec<u32>,
    mask: usize,
}

impl StyleIntern {
    pub(crate) fn new() -> Self {
        StyleIntern {
            styles: Vec::new(),
            tab: vec![0; 1024],
            mask: 1023,
        }
    }

    /// u32 語列の乗算混合（C st_hash 相当の役割。分布のみが要件で値は無関係）。
    fn hash_of(k: &StyleKey) -> usize {
        let words: [u32; 24] = [
            k.color,
            k.bg,
            k.font_size,
            k.line_height,
            k.width.v,
            k.width.unit as u32,
            k.height.v,
            k.height.unit as u32,
            k.margin[0].v,
            k.margin[0].unit as u32,
            k.margin[1].v,
            k.margin[1].unit as u32,
            k.margin[2].v,
            k.margin[2].unit as u32,
            k.margin[3].v,
            k.margin[3].unit as u32,
            k.padding[0].v,
            k.padding[0].unit as u32,
            k.padding[1].v,
            k.padding[1].unit as u32,
            k.padding[2].v,
            k.padding[2].unit as u32,
            k.padding[3].v,
            k.padding[3].unit as u32,
        ];
        let mut h = (k.border_w[0] as u64) ^ ((k.border_color as u64) << 32);
        h = h.wrapping_mul(0x9E3779B97F4A7C15);
        for (i, &w) in words.iter().enumerate() {
            let mut x = (w as u64) ^ ((k.border_w[i & 3] as u64) << 21);
            x ^= (k.display as u64) << 8
                | (k.text_align as u64) << 16
                | (k.white_space as u64) << 24
                | (k.flags as u64) << 1;
            x = x.wrapping_mul(0x9E3779B97F4A7C15);
            x ^= x >> 29;
            h ^= x;
            h = h.wrapping_mul(0xBF58476D1CE4E5B9);
        }
        h ^= h >> 31;
        h as usize
    }

    /// C の `st_intern` 写し（同一値は挿入せず既存 idx を返す）。
    pub(crate) fn intern(&mut self, st: &Style) -> u32 {
        let k = StyleKey::from(st);
        if (self.styles.len() as u64) * 4 >= (self.tab.len() as u64) * 3 {
            // 負荷率 0.75 で ×2（C 写し）
            let ncap = self.tab.len() * 2;
            let mut nt = vec![0u32; ncap];
            let nmask = ncap - 1;
            for (i, &(_, sk)) in self.styles.iter().enumerate() {
                let mut j = Self::hash_of(&sk) & nmask;
                while nt[j] != 0 {
                    j = (j + 1) & nmask;
                }
                nt[j] = i as u32 + 1;
            }
            self.tab = nt;
            self.mask = nmask;
        }
        let mut j = Self::hash_of(&k) & self.mask;
        loop {
            let e = self.tab[j];
            if e == 0 {
                let idx = self.styles.len() as u32;
                self.styles.push((*st, k));
                self.tab[j] = idx + 1;
                return idx;
            }
            if self.styles[e as usize - 1].1 == k {
                return e - 1;
            }
            j = (j + 1) & self.mask;
        }
    }

    /// 値 → idx の逆引き（pk 写像用。未登録の外来値は intern して一貫化）。
    fn idx_of(&mut self, st: &Style) -> u32 {
        self.intern(st)
    }

    /// idx → 値のコピー（layout hot path からの参照用）。
    pub(crate) fn value(&self, idx: u32) -> Style {
        self.styles[idx as usize].0
    }

    /// idx → display バイトの直読み（100B の値コピーを避ける layout hot path 用）。
    pub(crate) fn display_at(&self, idx: u32) -> u8 {
        self.styles[idx as usize].0.display
    }

    /// idx → wrap 用の表示属性直読み（font_size, line_height, white_space, text_align）。
    pub(crate) fn metrics_at(&self, idx: u32) -> (f32, f32, u8, u8) {
        let s = &self.styles[idx as usize].0;
        (s.font_size, s.line_height, s.white_space, s.text_align)
    }

    /// intern 表を値ベクタとして取り出す（`Layout::stab` 構築用）。
    pub(crate) fn into_values(self) -> Vec<Style> {
        self.styles.into_iter().map(|(s, _)| s).collect()
    }
}

/// 直接マップエントリ（C の `IfStCacheEnt` 写し）。pk は parent style の intern idx
/// への写像（C の parent ポインタに相当。値同値性で正準化できるよう intern 化で
/// 逆引きする。memcmp 同値性の構造保証は StyleKey の bit equality）。
#[derive(Clone, Copy)]
struct CacheEnt {
    k2: u64,
    /// parent idx + 1（0 = parent 無し。空エントリは k2==0 で判定 = C の
    /// 「k2 下位 bit 常 1 で 0 非合法」の写し）。
    pk: u64,
    st: u32,
}

const EMPTY_ENT: CacheEnt = CacheEnt {
    k2: 0,
    pk: 0,
    st: 0,
};

/// メモキーの k2 成分（C の st_resolve_memo と同一規則）: 既知タグは `(tag<<1)|1`、
/// 未知タグは名前バッファのアドレス（値同一なら同アドレス、違えば miss のみ =
/// 保守的。DOM は解決中に不変なので ABA は起きない）。
fn k2_of(kind: NodeKind, tag: Tag, name: &[u8]) -> u64 {
    if kind == NodeKind::Element && tag != tags::TAG_UNKNOWN {
        ((tag as u64) << 1) | 1
    } else {
        name.as_ptr() as usize as u64
    }
}

/// C の `st_resolve_memo`（tab 経路）写し: (parent 値, k2) で直接マップを引き、
/// 衝突は必ず verify（pk/k2 の完全一致）→ 再計算（損失ゼロの exact memo）。
struct StyleCache {
    tab: Vec<CacheEnt>,
    intern: StyleIntern,
}

impl StyleCache {
    fn new() -> Self {
        StyleCache {
            tab: vec![EMPTY_ENT; STCACHE_SIZE],
            intern: StyleIntern::new(),
        }
    }

    /// C のハッシュ写し: (pk>>4)*2654435761 + k2*40503。
    fn hash_of(pk: u64, k2: u64) -> usize {
        let h = (pk >> 4)
            .wrapping_mul(2654435761)
            .wrapping_add(k2.wrapping_mul(40503));
        (h as usize) & (STCACHE_SIZE - 1)
    }

    /// parent 値 → pk 写像（StyleLazy との共通部）。intern map の逆引きで正準化
    /// する（未登録の外来 style は intern して以後一貫させる。加法のみで副作用なし）。
    fn pk_of(&mut self, parent: Option<&Style>) -> u64 {
        match parent {
            None => 0,
            Some(p) => self.intern.idx_of(p) as u64 + 1,
        }
    }

    /// 解決して intern idx を返す。inline style 持ちは memo 通路ごとバイパス
    /// （値が結果に効くため。C ゲートの写し）。parent は継承元の style 値。
    fn resolve(
        &mut self,
        dom: &Dom,
        n: NodeId,
        parent: Option<&Style>,
        rfs: f32,
        sheets: &[&StyleSheet],
    ) -> u32 {
        let node = dom.node(n);
        let inline = dom.attr(n, b"style").is_some();
        if !inline && node.kind == NodeKind::Element {
            let pk = self.pk_of(parent);
            let k2 = k2_of(node.kind, node.tag, &node.name);
            let h = Self::hash_of(pk, k2);
            let e = self.tab[h];
            if e.pk == pk && e.k2 == k2 && e.k2 != 0 {
                return e.st;
            }
            let st = compute_style(dom, n, parent, rfs, sheets);
            let idx = self.intern.intern(&st);
            self.tab[h] = CacheEnt { k2, pk, st: idx };
            idx
        } else {
            let st = compute_style(dom, n, parent, rfs, sheets);
            self.intern.intern(&st)
        }
    }

    /// idx → style 値の参照。
    fn value(&self, idx: u32) -> Style {
        self.intern.styles[idx as usize].0
    }
}

/// 直接マップ + 1/2 スロットメモの遅延解決器（C の `IfStyleLazy` 写し）。
/// UA シート固定。`style_lazy_ok` の前提（md_ws_stripped && !has_style）でのみ使用する。
pub struct StyleLazy {
    cache: StyleCache,
    ua: StyleSheet,
    /// 1 番スロット（pk, k2, st。k2==0 は空）。C の m_pk/m_k2/m_st 写し。
    m1_pk: u64,
    m1_k2: u64,
    m1_st: u32,
    /// 2 番スロット（body 直下の相互追い出し吸収。C の 2 番スロット写し）。
    m2_pk: u64,
    m2_k2: u64,
    m2_st: u32,
}

impl Default for StyleLazy {
    fn default() -> Self {
        Self::new()
    }
}

impl StyleLazy {
    /// C の `if_style_lazy_init` 写し（UA シートは ctx ごとに自前で parse）。
    pub fn new() -> Self {
        StyleLazy {
            cache: StyleCache::new(),
            ua: parse_stylesheet(UA_SHEET.as_bytes(), 0),
            m1_pk: 0,
            m1_k2: 0,
            m1_st: 0,
            m2_pk: 0,
            m2_k2: 0,
            m2_st: 0,
        }
    }

    /// C の `if_style_lazy_get` 写し。parent は継承元 style（None = ルート）。
    /// pk（parent 値の intern idx 写像）は呼出 1 回計算（LRU/ctab が共用）。
    pub fn get(&mut self, dom: &Dom, n: NodeId, parent: Option<&Style>, rfs: f32) -> Style {
        // 外来 parent 値は intern して sid 化（pk の意味は `get_id` と厳密に同じ）。
        let psid = parent.map(|p| self.cache.intern.idx_of(p));
        let idx = self.get_id(dom, n, psid, rfs);
        self.cache.value(idx)
    }

    /// `get` の intern idx 返し版（layout の hot path 用）。parent は直前の
    /// `get_id` が返した継承元の sid（None = ルート）。pk は `sid+1` で 1 回も
    /// 値の hash を打たない（C の parent ポインタ hash に相当する O(1) 化。
    /// 値同一性 ⇒ intern 表で sid 一意なので、値から逆引きした pk と厳密一致）。
    pub fn get_id(&mut self, dom: &Dom, n: NodeId, parent: Option<u32>, rfs: f32) -> u32 {
        let node = dom.node(n);
        let inline = dom.attr(n, b"style").is_some();
        if !inline {
            let pk = parent.map(|s| s as u64 + 1).unwrap_or(0);
            let k2 = k2_of(node.kind, node.tag, &node.name);
            // 1 番スロット
            if self.m1_k2 == k2 && self.m1_pk == pk && k2 != 0 {
                return self.m1_st;
            }
            // 2 番スロット: ヒット時は 1 番へ昇格スワップ（C 写し）
            if self.m2_k2 == k2 && self.m2_pk == pk && k2 != 0 {
                let st2 = self.m2_st;
                self.m2_st = self.m1_st;
                self.m2_pk = self.m1_pk;
                self.m2_k2 = self.m1_k2;
                self.m1_st = st2;
                self.m1_pk = pk;
                self.m1_k2 = k2;
                return st2;
            }
            // 直接マップ（pk は 1/2 番スロットと同一の 1 回計算分を使い回す）
            let h = StyleCache::hash_of(pk, k2);
            let e = self.cache.tab[h];
            let idx = if e.pk == pk && e.k2 == k2 && e.k2 != 0 {
                e.st
            } else {
                let pst: Option<Style> = parent.map(|sid| self.cache.value(sid));
                let st = {
                    let uar: &StyleSheet = &self.ua;
                    let sheets: &[&StyleSheet] = std::slice::from_ref(&uar);
                    compute_style(dom, n, pst.as_ref(), rfs, sheets)
                };
                let idx = self.cache.intern.intern(&st);
                self.cache.tab[h] = CacheEnt { k2, pk, st: idx };
                idx
            };
            // 2 番 ← 1 番、1 番 ← 新規（C 写し）
            self.m2_st = self.m1_st;
            self.m2_pk = self.m1_pk;
            self.m2_k2 = self.m1_k2;
            self.m1_st = idx;
            self.m1_pk = pk;
            self.m1_k2 = k2;
            idx
        } else {
            // inline style 持ち: memo 通路ごとバイパスして直接 compute+intern（C 写し）
            let pst: Option<Style> = parent.map(|sid| self.cache.value(sid));
            let st = {
                let uar: &StyleSheet = &self.ua;
                let sheets: &[&StyleSheet] = std::slice::from_ref(&uar);
                compute_style(dom, n, pst.as_ref(), rfs, sheets)
            };
            self.cache.intern.intern(&st)
        }
    }

    /// idx → 値のコピー（layout が seg 物理属性を読むとき等に使用）。
    pub fn value(&self, idx: u32) -> Style {
        self.cache.value(idx)
    }

    /// idx → display バイトの直読み（値コピーを避ける layout hot path 用）。
    pub fn display_at(&self, idx: u32) -> u8 {
        self.cache.intern.display_at(idx)
    }

    /// idx → wrap 用の表示属性直読み（font_size, line_height, white_space, text_align）。
    pub fn metrics_at(&self, idx: u32) -> (f32, f32, u8, u8) {
        self.cache.intern.metrics_at(idx)
    }

    /// intern 表を値ベクタとして取り出す（`Layout::stab` 構築用。sid 体系を保持したまま移譲）。
    pub fn into_stab(self) -> Vec<Style> {
        self.cache.intern.into_values()
    }
}

/// C の `if_md_style_lazy_ok` 写し: lazy は md fast-DOM（author シート無し）専用。
/// `IF_STYLE_LAZY=0` の kill switch も同一規約。
pub fn style_lazy_ok(dom: &Dom) -> bool {
    if !dom.md_ws_stripped || dom.has_style {
        return false;
    }
    match std::env::var_os("IF_STYLE_LAZY") {
        Some(v) => v != "0",
        None => true,
    }
}

fn collect_author_sheets(dom: &Dom, n: NodeId, out: &mut Vec<StyleSheet>, order: &mut u32) {
    let node = dom.node(n);
    if node.kind == NodeKind::Element && node.tag == tags::tag_id(b"style") {
        let css = dom.text_content(n);
        if !css.is_empty() {
            let sh = parse_stylesheet(&css, *order);
            *order = sh.order_end + 1;
            out.push(sh);
        }
        return;
    }
    let mut c = node.first_child;
    while let Some(cid) = c {
        collect_author_sheets(dom, cid, out, order);
        c = dom.node(cid).next_sibling;
    }
}

fn first_elem_child(dom: &Dom, n: NodeId) -> Option<NodeId> {
    let mut c = dom.node(n).first_child;
    while let Some(cid) = c {
        if dom.node(cid).kind == NodeKind::Element {
            return Some(cid);
        }
        c = dom.node(cid).next_sibling;
    }
    None
}

#[allow(clippy::too_many_arguments)]
fn compute_walk(
    dom: &Dom,
    n: NodeId,
    parent_st: Option<&Style>,
    root_fs: f32,
    sheets: &[&StyleSheet],
    out: &mut [Option<Style>],
    cache: &mut Option<StyleCache>,
) {
    if dom.node(n).kind != NodeKind::Element {
        return;
    }
    // C の st_resolve_memo 一点化の写し: cache 有り（UA 専用時）なら決定メモ化経路。
    let st = match cache {
        Some(c) => {
            let idx = c.resolve(dom, n, parent_st, root_fs, sheets);
            c.value(idx)
        }
        None => compute_style(dom, n, parent_st, root_fs, sheets),
    };
    out[n as usize] = Some(st);
    let child_rfs = if dom.node(n).tag == tags::tag_id(b"html") {
        st.font_size
    } else {
        root_fs
    };
    let mut c = dom.node(n).first_child;
    while let Some(cid) = c {
        if dom.node(cid).kind == NodeKind::Element {
            compute_walk(dom, cid, Some(&st), child_rfs, sheets, out, cache);
        }
        c = dom.node(cid).next_sibling;
    }
}

/// DOM に計算済みスタイルを付与し、`NodeId` と並行の `Vec<Option<Style>>` を返す。
/// C の `if_style_apply` 相当。非要素は `None`。
pub fn apply_styles(dom: &Dom) -> Vec<Option<Style>> {
    let mut out: Vec<Option<Style>> = vec![None; dom.nodes.len()];
    let ua = parse_stylesheet(UA_SHEET.as_bytes(), 0);
    let mut author: Vec<StyleSheet> = Vec::new();
    let mut order = 100000u32;
    if dom.has_style {
        collect_author_sheets(dom, dom.root, &mut author, &mut order);
    }
    let mut sheets: Vec<&StyleSheet> = Vec::with_capacity(author.len() + 1);
    sheets.push(&ua);
    for sh in &author {
        sheets.push(sh);
    }
    if let Some(html) = first_elem_child(dom, dom.root) {
        // C の if_style_apply の写し: author シート無しのときだけ決定メモ化を有効化
        // （UA シートはタグセレクタのみで、memo キー (parent 値, tag|name) が完全）。
        let mut cache = if author.is_empty() {
            Some(StyleCache::new())
        } else {
            None
        };
        compute_walk(dom, html, None, 16.0, &sheets, &mut out, &mut cache);
    }
    out
}

// ================= computed style dump =================

/// C の `%.6g`（6 桁有効数字・末尾ゼロ除去・指数表記切替）を再現する。
fn fmt_g6(v: f64) -> String {
    if v == 0.0 {
        return if v.is_sign_negative() {
            "-0".to_string()
        } else {
            "0".to_string()
        };
    }
    if v.is_nan() {
        return "nan".to_string();
    }
    if v.is_infinite() {
        return if v > 0.0 {
            "inf".to_string()
        } else {
            "-inf".to_string()
        };
    }
    let neg = v < 0.0;
    let a = v.abs();
    // 6 桁有効数字の科学表記から指数を確実に取り出す（log10 の精度問題を回避）
    let sci = format!("{:.5e}", a);
    let (mant, exp_str) = sci.split_once('e').unwrap();
    let exp: i32 = exp_str.parse().unwrap();
    let strip = |s: &str| -> String {
        if !s.contains('.') {
            return s.to_string();
        }
        let t = s.trim_end_matches('0');
        t.strip_suffix('.').unwrap_or(t).to_string()
    };
    if !(-4..6).contains(&exp) {
        let m = strip(mant);
        format!(
            "{}{}e{}{:02}",
            if neg { "-" } else { "" },
            m,
            if exp < 0 { "-" } else { "+" },
            exp.abs()
        )
    } else {
        let after = (5 - exp).max(0) as usize;
        let s = format!("{:.*}", after, a);
        let s = strip(&s);
        format!("{}{}", if neg { "-" } else { "" }, s)
    }
}

fn sd_len(out: &mut String, l: Len) {
    if l.unit == U_AUTO {
        out.push_str("auto");
        return;
    }
    out.push_str(&fmt_g6(l.v as f64));
    out.push_str(match l.unit {
        U_PX => "px",
        U_EM => "em",
        U_REM => "rem",
        U_PT => "pt",
        _ => "%",
    });
}

fn sd_len4(out: &mut String, v: &[Len; 4]) {
    for (i, l) in v.iter().enumerate() {
        if i > 0 {
            out.push(' ');
        }
        sd_len(out, *l);
    }
}

fn sd_node(
    dom: &Dom,
    n: NodeId,
    styles: &[Option<Style>],
    out: &mut String,
    depth: usize,
    n_styled: &mut u32,
) {
    if dom.node(n).kind != NodeKind::Element {
        return;
    }
    for _ in 0..depth {
        out.push_str("  ");
    }
    out.push('<');
    out.push_str(std::str::from_utf8(&dom.node(n).name).unwrap_or("?"));
    out.push('>');
    match &styles[n as usize] {
        None => out.push_str(" (no style)\n"),
        Some(st) => {
            *n_styled += 1;
            out.push_str(" display=");
            out.push_str(match st.display {
                D_BLOCK => "block",
                D_LIST_ITEM => "list-item",
                D_NONE => "none",
                _ => "inline",
            });
            out.push_str(" text-align=");
            out.push_str(match st.text_align {
                TA_CENTER => "center",
                TA_RIGHT => "right",
                _ => "left",
            });
            out.push_str(" white-space=");
            out.push_str(if st.white_space == WS_PRE {
                "pre"
            } else {
                "normal"
            });
            out.push_str(" font-size=");
            out.push_str(&fmt_g6(st.font_size as f64));
            out.push_str("px line-height=");
            if st.line_height > 0.0 {
                out.push_str(&fmt_g6(st.line_height as f64));
                out.push_str("px");
            } else {
                out.push_str("auto");
            }
            out.push_str(" width=");
            sd_len(out, st.width);
            out.push_str(" height=");
            sd_len(out, st.height);
            out.push_str(" margin=");
            sd_len4(out, &st.margin);
            out.push_str(" padding=");
            sd_len4(out, &st.padding);
            out.push_str(" border-width=");
            for i in 0..4 {
                if i > 0 {
                    out.push(' ');
                }
                out.push_str(&fmt_g6(st.border_w[i] as f64));
            }
            out.push_str(&format!(
                " border-color=#{:08x} color=#{:08x} background=#{:08x}",
                st.border_color, st.color, st.bg
            ));
            out.push_str(&format!(
                " bold={} italic={} underline={} strike={}\n",
                st.bold as u8, st.italic as u8, st.underline as u8, st.strike as u8
            ));
        }
    }
    let mut c = dom.node(n).first_child;
    while let Some(cid) = c {
        sd_node(dom, cid, styles, out, depth + 1, n_styled);
        c = dom.node(cid).next_sibling;
    }
}

/// computed style dump（devtools 観測点）。C の `if_style_dump` 相当。
pub fn dump_styles(dom: &Dom, styles: &[Option<Style>]) -> String {
    if dom.root >= dom.nodes.len() as NodeId {
        return "(empty dom)\n".to_string();
    }
    let mut out = String::from("#styles\n");
    let mut n_styled = 0u32;
    let mut c = dom.node(dom.root).first_child;
    while let Some(cid) = c {
        sd_node(dom, cid, styles, &mut out, 0, &mut n_styled);
        c = dom.node(cid).next_sibling;
    }
    // `nodes=` は解析時カウンタ（script 実行による事後増分を含まない。C と同規約）
    out.push_str(&format!("; nodes={} styled={}\n", dom.n_nodes, n_styled));
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::html_tree::parse_html;

    /// eager 全面走査と lazy 要所解決の全ノード値一致（C の IfStyleLazy 同値性の写し元。
    /// UA シートのみ = lazy 前提の文書群。DFS pre-order で解決する = layout の訪問規約）。
    #[test]
    fn lazy_matches_eager_walk() {
        let docs: &[&[u8]] = &[
            b"<!doctype html><p>hello <b>bold</b> world</p>",
            b"<ul><li>a<li>b<li>c</ul><ol><li>x</ol>",
            b"<h1>T</h1><h2>S</h2><pre>pre\ntext</pre><blockquote>q</blockquote>",
            b"<table><tr><td>1<td>2<tr><td>3</table><caption>c</caption>",
            b"<div><span style=\"color:#f00\">inline</span><b style=\"font-size:20px\">b</b></div>",
            b"<my-widget>x</my-widget><x-unknown><y-never/></x-unknown>",
            b"<div><ul><li><b>deep <i>deeper</i></b></li></ul></div>",
            b"<p style=\"display:none\">hidden <b>child</b></p><p>shown</p>",
            b"<a href=\"#\">link</a> and <em style=\"background:blue\">e</em>",
            b"<center>c</center><font>f</font><sub>s</sub><sup>S</sup>",
        ];
        for doc in docs {
            let dom = parse_html(doc);
            let eager = apply_styles(&dom);
            assert!(
                !dom.has_style,
                "lazy 前提（author 無し）: {:?}",
                String::from_utf8_lossy(doc)
            );
            let mut lz = StyleLazy::new();
            // layout の DFS pre-order 写しで全 ELEMENT を解決して突合
            fn walk(
                dom: &crate::dom::Dom,
                lz: &mut StyleLazy,
                n: crate::dom::NodeId,
                pst: Option<Style>,
                eager: &[Option<Style>],
            ) {
                let node = dom.node(n);
                let st = if node.kind == crate::dom::NodeKind::Element {
                    let got = lz.get(dom, n, pst.as_ref(), 16.0);
                    let want = eager[n as usize].unwrap_or_else(|| {
                        panic!(
                            "eager None @ {:?} {n:?}",
                            String::from_utf8_lossy(&node.name)
                        )
                    });
                    assert_eq!(
                        got,
                        want,
                        "lazy != eager @ {:?}",
                        String::from_utf8_lossy(&node.name)
                    );
                    Some(got)
                } else {
                    pst
                };
                let mut c = node.first_child;
                while let Some(cid) = c {
                    walk(dom, lz, cid, st, eager);
                    c = dom.node(cid).next_sibling;
                }
            }
            walk(&dom, &mut lz, dom.root, None, &eager);
        }
    }

    /// md fast-DOM で lazy render の全パイプライン一致（行スイープの両モード）。
    #[test]
    fn lazy_render_matches_eager_render() {
        let md = b"# Title\n\npara **bold** _it_ `code` text\n\n- a\n- b\n- c\n\n1. x\n2. y\n\n> quote\n\n```\npre block\n```\n\n| h1 | h2 |\n|----|----|\n| a  | b  |\n\n---\n\n[link](http://example.com) tail\n";
        let dom = crate::md::md_to_dom(md).expect("md fast-DOM");
        assert!(dom.md_ws_stripped);
        assert!(style_lazy_ok(&dom));
        let styles = apply_styles(&dom);
        for ansi in [false, true] {
            for width in [20, 60, 100] {
                let lay_e = crate::layout::layout_build(&dom, &styles, width);
                let lay_l = crate::layout::layout_build_lazy(&dom, width);
                let out_e = crate::render::render_emit_sweep(&dom, &lay_e, ansi);
                let out_l = crate::render::render_emit_sweep(&dom, &lay_l, ansi);
                assert_eq!(out_e, out_l, "render byte-diff: ansi={ansi} width={width}");
            }
        }
    }

    /// inline style 持ちはメモ通路を通らない契約 + 同一文書で 2 インスタンスが
    /// 同じ値を返すこと（intern/キャッシュのページ独立性）。
    #[test]
    fn lazy_inline_style_gate_and_ctx_independence() {
        let doc = b"<p>a</p><p style=\"color:#00f\">b</p><p>c</p><p>d</p>";
        let dom = parse_html(doc);
        let mut lz1 = StyleLazy::new();
        let mut lz2 = StyleLazy::new();
        let eager = apply_styles(&dom);
        fn body_children(dom: &crate::dom::Dom) -> Vec<crate::dom::NodeId> {
            let mut out = Vec::new();
            let mut c = dom.node(dom.root).first_child;
            while let Some(cid) = c {
                let n = dom.node(cid);
                if n.kind == crate::dom::NodeKind::Element && n.tag == tags::tag_id(b"html") {
                    let mut g = n.first_child;
                    while let Some(gid) = g {
                        let gn = dom.node(gid);
                        if gn.kind == crate::dom::NodeKind::Element
                            && gn.tag == tags::tag_id(b"body")
                        {
                            let mut k = gn.first_child;
                            while let Some(kid) = k {
                                if dom.node(kid).kind == crate::dom::NodeKind::Element {
                                    out.push(kid);
                                }
                                k = dom.node(kid).next_sibling;
                            }
                        }
                        g = gn.next_sibling;
                    }
                }
                c = n.next_sibling;
            }
            out
        }
        let pst = None; // 値突合のみ（parent 規約は lazy_matches_eager_walk で検証済）
        for (i, &nid) in body_children(&dom).iter().enumerate() {
            let want = eager[nid as usize].unwrap();
            // inline style 持ち（idx 1）もメモバイパスで同値に解決される
            if i == 1 {
                let got = lz1.get(&dom, nid, None, 16.0);
                // parent=None は本来の規約と違う注入: p の親連鎖が無いので
                // 値は inline 指定の効く部分以外 eager と同値であることだけ確認
                assert_eq!(got.color, 0x0000FFFF);
                assert_eq!(got.display, want.display);
            } else {
                let g1 = lz1.get(&dom, nid, pst.as_ref(), 16.0);
                let g2 = lz2.get(&dom, nid, pst.as_ref(), 16.0);
                assert_eq!(g1, g2);
            }
        }
    }

    #[test]
    fn color_parse() {
        assert_eq!(css_color(b"#fff"), Some(0xFFFFFFFF));
        assert_eq!(css_color(b"#ff0000"), Some(0xFF0000FF));
        assert_eq!(css_color(b"#0000ff80"), Some(0x0000FF80));
        assert_eq!(css_color(b"red"), Some(0xFF0000FF));
        assert_eq!(css_color(b"BLUE"), Some(0x0000FFFF));
        assert_eq!(css_color(b"rgb(1,2,3)"), Some(0x010203FF));
        assert_eq!(css_color(b"rgba(1,2,3,4)"), Some(0x01020304));
        assert_eq!(css_color(b"transparent"), Some(0));
        assert_eq!(css_color(b"#ff"), None);
        assert_eq!(css_color(b"rgb(1,2)"), None);
        assert_eq!(css_color(b"nosuchcolor"), None);
    }

    fn styled(html: &str) -> (Dom, Vec<Option<Style>>) {
        let dom = parse_html(html.as_bytes());
        let styles = apply_styles(&dom);
        (dom, styles)
    }

    #[test]
    fn ua_default_h1() {
        let (dom, styles) = styled("<h1>x</h1>");
        let h1 = dom.find_tag_dfs(tags::tag_id(b"h1")).unwrap();
        let st = styles[h1 as usize].unwrap();
        assert_eq!(st.display, D_BLOCK);
        assert!(st.bold);
        assert!(st.font_size > 31.0 && st.font_size < 33.0);
    }

    #[test]
    fn specificity_id_wins() {
        let (dom, styles) = styled(
            "<style>p { color: red }.a { color: #00ff00 }p.a { color: blue }#x { color: rgb(9,9,9) }</style>\
             <p class=a id=x>hello</p><p>two</p>",
        );
        let body = dom.find_tag_dfs(tags::tag_id(b"body")).unwrap();
        let mut c = dom.node(body).first_child;
        let p1 = loop {
            let cid = c.unwrap();
            if dom.node(cid).tag == tags::tag_id(b"p") {
                break cid;
            }
            c = dom.node(cid).next_sibling;
        };
        assert_eq!(styles[p1 as usize].unwrap().color, 0x090909FF);
        let p2 = dom.node(p1).next_sibling.unwrap();
        assert_eq!(styles[p2 as usize].unwrap().color, 0xFF0000FF);
    }

    #[test]
    fn important_beats_inline() {
        let (dom, styles) = styled(
            "<style>div p { color: #111111 }div > p { color: #222222 !important }</style>\
             <div><p style=\"color: #333333\">a</p><span><p>b</p></span></div>",
        );
        let div = dom.find_tag_dfs(tags::tag_id(b"div")).unwrap();
        let c = dom.node(div).first_child;
        let p1 = c.unwrap(); // 直接子 p
        let span = dom.node(p1).next_sibling.unwrap();
        let p2 = dom.node(span).first_child.unwrap();
        assert_eq!(styles[p1 as usize].unwrap().color, 0x222222FF);
        assert_eq!(styles[p2 as usize].unwrap().color, 0x111111FF);
    }

    #[test]
    fn shorthand_and_inherit() {
        let (dom, styles) = styled(
            "<style>div{margin:1px 2px 3px 4px;border:1px solid #123456;padding:8px}</style>\
             <div><em style=\"display:none\">hide</em><b>show</b></div>",
        );
        let div = dom.find_tag_dfs(tags::tag_id(b"div")).unwrap();
        let st = styles[div as usize].unwrap();
        assert_eq!(st.margin[0].v, 1.0);
        assert_eq!(st.margin[1].v, 2.0);
        assert_eq!(st.margin[2].v, 3.0);
        assert_eq!(st.margin[3].v, 4.0);
        assert_eq!(st.border_w[0], 1.0);
        assert_eq!(st.border_w[3], 1.0);
        assert_eq!(st.border_color, 0x123456FF);
        assert_eq!(st.padding[0].v, 8.0);
        assert_eq!(st.padding[3].v, 8.0);
        let em = dom.node(div).first_child.unwrap();
        assert_eq!(styles[em as usize].unwrap().display, D_NONE);
    }

    #[test]
    fn matcher_backtracking() {
        let dom = parse_html(
            b"<section><div class=x><p><span><b class=y>t</b></span></p></div></section>",
        );
        let sh = parse_stylesheet(b"section div p span .y{color:red}", 0);
        let b = dom.find_tag_dfs(tags::tag_id(b"b")).unwrap();
        assert_eq!(sh.rules.len(), 1);
        assert!(match_selector(&dom, b, &sh.rules[0].sels[0]));
        let sh2 = parse_stylesheet(b"div > b{color:red}", 0);
        assert!(!match_selector(&dom, b, &sh2.rules[0].sels[0]));
    }

    #[test]
    fn dump_oracle() {
        let (dom, styles) = styled("<p style=\"margin:2px 3%; color:#0f0\">x</p>");
        let got = dump_styles(&dom, &styles);
        let exp = "#styles\n<html> display=block text-align=left white-space=normal font-size=16px line-height=auto width=auto height=auto margin=0px 0px 0px 0px padding=0px 0px 0px 0px border-width=0 0 0 0 border-color=#00000000 color=#000000ff background=#00000000 bold=0 italic=0 underline=0 strike=0\n  <head> display=none text-align=left white-space=normal font-size=16px line-height=auto width=auto height=auto margin=0px 0px 0px 0px padding=0px 0px 0px 0px border-width=0 0 0 0 border-color=#00000000 color=#000000ff background=#00000000 bold=0 italic=0 underline=0 strike=0\n  <body> display=block text-align=left white-space=normal font-size=16px line-height=19.2px width=auto height=auto margin=8px 8px 8px 8px padding=0px 0px 0px 0px border-width=0 0 0 0 border-color=#00000000 color=#000000ff background=#ffffffff bold=0 italic=0 underline=0 strike=0\n    <p> display=block text-align=left white-space=normal font-size=16px line-height=19.2px width=auto height=auto margin=2px 3% 2px 3% padding=0px 0px 0px 0px border-width=0 0 0 0 border-color=#00000000 color=#00ff00ff background=#00000000 bold=0 italic=0 underline=0 strike=0\n; nodes=6 styled=4\n";
        assert_eq!(got, exp);
    }

    #[test]
    fn fmt_g6_matches() {
        // C の `printf("%.6g")` との対照（f32 → f64 昇格後の値）
        assert_eq!(fmt_g6(16.0), "16");
        assert_eq!(fmt_g6(19.2), "19.2");
        assert_eq!(fmt_g6(8.0), "8");
        assert_eq!(fmt_g6(0.0), "0");
        assert_eq!(fmt_g6(2.0), "2");
        assert_eq!(fmt_g6(3.0), "3");
        assert_eq!(fmt_g6(0.67), "0.67");
        assert_eq!(fmt_g6(1.5), "1.5");
        assert_eq!(fmt_g6(123.456), "123.456");
        assert_eq!(fmt_g6(1000000.0), "1e+06");
        assert_eq!(fmt_g6(0.000083), "8.3e-05");
        assert_eq!(fmt_g6(0.1f32 as f64), "0.1");
        assert_eq!(fmt_g6(13.333333f32 as f64), "13.3333");
        assert_eq!(fmt_g6(0.3f32 as f64), "0.3"); // 元値 0.30000001f32 は 0.3f32 と bit 同値（half-ULP 未満）のため lint 指摘どおり素直な形に
        assert_eq!(fmt_g6((-5.0f32) as f64), "-5");
        assert_eq!(fmt_g6(0.00001f32 as f64), "1e-05");
        assert_eq!(fmt_g6(99999.0), "99999");
        assert_eq!(fmt_g6(999999.0), "999999");
        assert_eq!(fmt_g6(24.0f32 as f64), "24");
        assert_eq!(fmt_g6(40.0f32 as f64), "40");
    }
}
