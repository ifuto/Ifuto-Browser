//! HTML トークナイザ（C の `src/html_tok.c` 相当。WHATWG tokenizer の実用形）。
//!
//! | C (html_tok.c / html_int.h) | Rust |
//! |---|---|
//! | `IfTokKind` | [`TokKind`] |
//! | `IfTok`（arena スライス参照） | [`Tok`]（所有 `Vec<u8>`） |
//! | `IfHtmlTok`（入力スライス + pos） | [`Tokenizer`]（`&[u8]` + pos） |
//! | `if_tok_init` / `if_tok_next` / `if_tok_set_raw` | [`Tokenizer::new`] / [`next`] / [`set_raw`] |
//!
//! # C との違い（所有権による構造的な改善）
//!
//! C は「文字参照を解決した文字列」を arena に確保し、ゼロコピー切片と arena 切片を
//! `IfStr` で混在させる（`if_resolved` の二重経路）。Rust では出力をすべて所有
//! `Vec<u8>` にし、手動の容量計算（`if_decoded_len` の 2 パス）とバッファ境界検査を
//! 構造的に排除する。**出力バイト列は C と完全一致**する（差分 fuzz で実証）。
//!
//! # 状態
//!
//! - `raw_tag`: rawtext/RCDATA モード（終了タグまで TEXT として読む）
//! - `raw_rcdata`: raw 内容で文字参照を解決する（title/textarea）
//! - `raw_frag`: fragment 直接 raw モード（終端スキャンせず EOF まで）
//! - `strip_lf`: raw 内容の先頭 LF を 1 つ捨てる
//! - `cdata_foreign` / `adcn_foreign`: foreign content の CDATA/U+0000 規則切替
//! - `plaintext`: `<plaintext>` 以降の残り全入力を 1 個の TEXT に
//! - `in_attr_ctx`: 属性値デコード中の ambiguous-amp 規則

use crate::entities;
use crate::tags::{self, Tag};
use crate::utf8;

/// トークン種別（C の `IfTokKind` 相当）。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum TokKind {
    /// テキスト。
    Text,
    /// 開始タグ。
    Start,
    /// 終了タグ。
    End,
    /// コメント（PI 含む）。
    Comment,
    /// DOCTYPE。
    Doctype,
    /// 入力終端。
    Eof,
}

/// 属性（名前は ASCII lowercase 正規化済み、値は文字参照デコード済み）。
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct Attr {
    /// 属性名（lowercase）。
    pub name: Vec<u8>,
    /// 属性値（文字参照デコード済み）。
    pub value: Vec<u8>,
}

/// トークン（C の `IfTok` 相当。所有 `Vec<u8>` で表現）。
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Tok {
    /// 種別。
    pub kind: TokKind,
    /// TEXT / COMMENT / DOCTYPE name（lowercase 済み）。
    pub text: Vec<u8>,
    /// START/END: 既知タグ ID（未知は `tags::TAG_UNKNOWN`）。
    pub tag: Tag,
    /// 生のタグ名（小文字正規化なし）。
    pub tag_raw: Vec<u8>,
    /// START: 属性列（重複は first-wins で除去済み）。
    pub attrs: Vec<Attr>,
    /// 自己終了タグ（`<foo/>`）。
    pub self_closing: bool,
    /// DOCTYPE の public id。
    pub dt_pub: Vec<u8>,
    /// DOCTYPE の system id。
    pub dt_sys: Vec<u8>,
    /// DOCTYPE name あり。
    pub dt_has_name: bool,
    /// DOCTYPE public id あり。
    pub dt_has_pub: bool,
    /// DOCTYPE system id あり。
    pub dt_has_sys: bool,
    /// COMMENT が Processing Instruction か。
    pub is_pi: bool,
    /// PI のターゲット。
    pub pi_target: Vec<u8>,
    /// TEXT: 空白/NUL でない実文字を含むか（frameset-ok 判定用）。
    pub text_had_real: bool,
}

impl Default for Tok {
    fn default() -> Self {
        Tok {
            kind: TokKind::Eof,
            text: Vec::new(),
            tag: tags::TAG_UNKNOWN,
            tag_raw: Vec::new(),
            attrs: Vec::new(),
            self_closing: false,
            dt_pub: Vec::new(),
            dt_sys: Vec::new(),
            dt_has_name: false,
            dt_has_pub: false,
            dt_has_sys: false,
            is_pi: false,
            pi_target: Vec::new(),
            text_had_real: false,
        }
    }
}

/// 属性数上限（C の `IF_MAX_ATTRS`）。
const MAX_ATTRS: usize = 256;

/// windows-1252 マッピング（WHATWG 数値文字参照の C1 補正表）。
const C1_MAP: [u32; 32] = [
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
];

/// ホワイトスペース（HTML 空白: space/tab/nl/ff/cr）。
#[inline]
fn is_hws(c: u8) -> bool {
    matches!(c, b' ' | b'\t' | b'\n' | b'\x0c' | b'\r')
}

/// ASCII 英字。
#[inline]
fn is_alpha(c: u8) -> bool {
    c.is_ascii_alphabetic()
}

/// ASCII 英数字。
#[inline]
fn is_alnum(c: u8) -> bool {
    c.is_ascii_alphanumeric()
}

/// HTML トークナイザ（C の `IfHtmlTok` 相当）。
pub struct Tokenizer<'a> {
    src: &'a [u8],
    pos: usize,
    raw_tag: Tag,
    raw_rcdata: bool,
    raw_frag: bool,
    strip_lf: bool,
    cdata_foreign: bool,
    adcn_foreign: bool,
    plaintext: bool,
    in_attr_ctx: bool,
    /// 回復したパースエラー数（統計用）。
    pub errors: u32,
}

#[allow(clippy::field_reassign_with_default)]
impl<'a> Tokenizer<'a> {
    /// 初期化（C の `if_tok_init` 相当）。
    pub fn new(input: &'a [u8]) -> Self {
        Tokenizer {
            src: input,
            pos: 0,
            raw_tag: tags::TAG_UNKNOWN,
            raw_rcdata: false,
            raw_frag: false,
            strip_lf: false,
            cdata_foreign: false,
            adcn_foreign: false,
            plaintext: false,
            in_attr_ctx: false,
            errors: 0,
        }
    }

    /// rawtext/RCDATA モードへ（C の `if_tok_set_raw` 相当）。
    pub fn set_raw(&mut self, tag: Tag) {
        self.raw_tag = tag;
        self.raw_rcdata = tags::is_rcdata(tag);
        self.strip_lf = false;
    }

    /// raw_frag モード（fragment 直接 raw。13.4）。
    pub fn set_raw_frag(&mut self) {
        self.raw_frag = true;
    }

    /// strip_lf（textarea の先頭 LF スキップ）。
    pub fn set_strip_lf(&mut self) {
        self.strip_lf = true;
    }

    /// foreign content（DATA text の U+0000 → U+FFFD 切替）。
    pub fn set_cdata_foreign(&mut self, v: bool) {
        self.cdata_foreign = v;
    }

    /// adjusted current node が非 HTML 名前空間（CDATA 許可判定）。
    pub fn set_adcn_foreign(&mut self, v: bool) {
        self.adcn_foreign = v;
    }

    /// `<plaintext>` 以降の残り全入力を 1 個の TEXT にする。
    pub fn set_plaintext(&mut self) {
        self.plaintext = true;
    }

    /// `[start, end)` を文字参照解決しつつデコード（所有 `Vec<u8>`）。
    /// C の `if_decoded_len` + `if_decode_into` + `if_resolved` を 1 パスに統合。
    fn resolve_range(&mut self, start: usize, end: usize) -> Vec<u8> {
        let mut out = Vec::new();
        let mut i = start;
        let src = self.src;
        while i < end {
            if src[i] == b'&' {
                if i + 1 < end && is_alnum(src[i + 1]) {
                    // 名前参照（1 または 2 cp）
                    if let Some((cps, np)) = self.named_ref(i + 1) {
                        for &cp in &cps {
                            let mut buf = [0u8; 4];
                            let n = utf8::encode(cp, &mut buf);
                            out.extend_from_slice(&buf[..n]);
                        }
                        i = np;
                        continue;
                    }
                } else {
                    // 数値参照（単一 cp）
                    if let Some((cp, np)) = self.charref(i + 1) {
                        let mut buf = [0u8; 4];
                        let n = utf8::encode(cp, &mut buf);
                        out.extend_from_slice(&buf[..n]);
                        i = np;
                        continue;
                    }
                }
            }
            // 通常の UTF-8 1 コードポイント
            let mut np = i;
            let cp = utf8::decode(&src[..end], &mut np);
            let mut buf = [0u8; 4];
            let n = utf8::encode(cp, &mut buf);
            out.extend_from_slice(&buf[..n]);
            i = np;
        }
        out
    }

    /// 名前参照を解決。戻り値 `(コードポイント列, 参照後の位置)`。
    /// C の `if_named_ref` 相当。
    fn named_ref(&mut self, amp_next: usize) -> Option<(Vec<u32>, usize)> {
        let i = amp_next;
        if i >= self.src.len() || !is_alnum(self.src[i]) {
            return None;
        }
        // alnum ランを収集（最長エンティティ名 31 文字 + 余裕）
        let mut run = 0usize;
        let mut buf = [0u8; 48];
        while i + run < self.src.len() && is_alnum(self.src[i + run]) && run + 1 < buf.len() {
            buf[run] = self.src[i + run];
            run += 1;
        }
        let buf = &buf[..run];

        // 1) 正式形: 長い n から下ろして "name;" 完全一致（flags bit0）
        for n in (2..=run).rev() {
            let Some(ei) = entities::find(&buf[..n]) else { continue };
            if entities::entry_flags(ei) & 1 == 0 {
                continue;
            }
            if i + n >= self.src.len() || self.src[i + n] != b';' {
                continue;
            }
            let cps = entities::entry_codepoints(ei);
            return Some((cps, i + n + 1));
        }
        // 2) legacy: 最長 prefix
        if let Some(ei) = entities::longest_legacy(buf) {
            let name_len = entities::entry_name_len(ei) as usize;
            let next = if i + name_len < self.src.len() {
                self.src[i + name_len]
            } else {
                0
            };
            let blocked = self.in_attr_ctx
                && (next.is_ascii_alphanumeric() || next == b'=');
            if !blocked {
                let cps = entities::entry_codepoints(ei);
                return Some((cps, i + name_len));
            }
        }
        None
    }

    /// 数値参照を解決。戻り値 `(codepoint, 参照後の位置)`。
    /// C の `if_charref` 相当。
    fn charref(&mut self, amp_next: usize) -> Option<(u32, usize)> {
        let i = amp_next;
        if i >= self.src.len() {
            return None;
        }
        if self.src[i] == b'#' {
            let mut j = i + 1;
            let mut hex = false;
            if j < self.src.len() && (self.src[j] == b'x' || self.src[j] == b'X') {
                hex = true;
                j += 1;
            }
            let start = j;
            let mut v: u64 = 0;
            while j < self.src.len() {
                let c = self.src[j];
                let d = if !hex && c.is_ascii_digit() {
                    (c - b'0') as u64
                } else if hex && c.is_ascii_hexdigit() {
                    if c <= b'9' {
                        (c - b'0') as u64
                    } else {
                        ((c | 32) - b'a' + 10) as u64
                    }
                } else {
                    break;
                };
                v = v.wrapping_mul(if hex { 16 } else { 10 }).wrapping_add(d);
                if v > 0x110000 {
                    v = 0x110000; // 飽和。数字自体は最後まで消費
                }
                j += 1;
            }
            if j == start {
                self.errors += 1;
                return None;
            }
            if j < self.src.len() && self.src[j] == b';' {
                j += 1;
            } else {
                self.errors += 1; // セミコロン欠落は回復可能エラー
            }
            let mut cp = v as u32;
            if cp == 0 || cp > 0x10FFFF || (0xD800..=0xDFFF).contains(&cp) {
                cp = utf8::REPLACEMENT;
            } else if (0x80..=0x9F).contains(&cp) {
                cp = C1_MAP[(cp - 0x80) as usize];
            }
            return Some((cp, j));
        }
        if is_alnum(self.src[i]) {
            if let Some((cps, np)) = self.named_ref(i) {
                if !cps.is_empty() {
                    return Some((cps[0], np)); // 2cp は resolve_range 経路で処理
                }
            }
        }
        None
    }

    /// U+0000 の規格処理。`to_fffd=true` は U+FFFD 置換、false は除去。
    /// C の `if_fix_nul` 相当（所有 `Vec<u8>` 版）。
    fn fix_nul(s: &[u8], to_fffd: bool) -> Vec<u8> {
        if !s.contains(&0) {
            return s.to_vec();
        }
        let mut out = Vec::with_capacity(s.len());
        for &b in s {
            if b == 0 {
                if to_fffd {
                    out.extend_from_slice(&[0xEF, 0xBF, 0xBD]);
                }
                // drop 時は何も書かない
            } else {
                out.push(b);
            }
        }
        out
    }

    /// 「空白（TAB/LF/FF/CR/SP）でも U+0000 でもない」実文字を含むか。
    /// C の `if_tok_real_text` 相当。
    fn real_text(s: &[u8]) -> bool {
        s.iter().any(|&c| !(c == 0 || is_hws(c)))
    }

    /// 空白のみか（rawtext/plaintext の text_had_real 用。NUL は実文字）。
    fn is_ws_only(s: &[u8]) -> bool {
        s.iter().all(|&c| is_hws(c))
    }

    /// `i` に `lit` が始まるか（CI）。`need_term` なら後続が ws/`/`/`>` であること。
    /// C の `raw_at` 相当。
    fn raw_at(&self, i: usize, lit: &[u8], need_term: bool) -> bool {
        let n = lit.len();
        if i + n > self.src.len() {
            return false;
        }
        for (k, &lc) in lit.iter().enumerate() {
            if self.src[i + k].to_ascii_lowercase() != lc {
                return false;
            }
        }
        if !need_term {
            return true;
        }
        let after = i + n;
        if after >= self.src.len() {
            return false; // EOF 直前は終端として認めない
        }
        is_hws(self.src[after]) || self.src[after] == b'/' || self.src[after] == b'>'
    }

    /// script data の終了タグ `<` の位置。見つからなければ len。
    /// C の `if_find_script_end` 相当。
    fn find_script_end(&self) -> usize {
        enum St {
            Data,
            Esc,
            Dbl,
        }
        let mut st = St::Data;
        let mut i = self.pos;
        while i < self.src.len() {
            match st {
                St::Data => {
                    if self.raw_at(i, b"<!--", false) {
                        st = St::Esc;
                        i += 4;
                        continue;
                    }
                    if self.raw_at(i, b"</script", true) {
                        return i;
                    }
                    i += 1;
                }
                St::Esc => {
                    if self.raw_at(i, b"-->", false) {
                        st = St::Data;
                        i += 3;
                        continue;
                    }
                    if self.raw_at(i, b"<script", true) {
                        st = St::Dbl;
                        i += 7;
                        continue;
                    }
                    if self.raw_at(i, b"</script", true) {
                        return i;
                    }
                    i += 1;
                }
                St::Dbl => {
                    if self.raw_at(i, b"</script", true) {
                        st = St::Esc;
                        i += 8;
                        continue;
                    }
                    if self.raw_at(i, b"<script", true) {
                        i += 7;
                        continue;
                    }
                    if self.raw_at(i, b"-->", false) {
                        st = St::Esc;
                        i += 3;
                        continue;
                    }
                    i += 1;
                }
            }
        }
        self.src.len()
    }

    /// raw_tag の終了タグ `</name` の位置。見つからなければ len。
    /// C の `if_find_raw_end` 相当。
    fn find_raw_end(&self) -> usize {
        let name = tags::tag_name(self.raw_tag).map(|s| s.as_bytes()).unwrap_or(b"");
        if name.is_empty() {
            return self.src.len();
        }
        let mut i = self.pos;
        while i + 2 + name.len() <= self.src.len() {
            if self.src[i] == b'<' && self.src[i + 1] == b'/' {
                let mut m = true;
                for (k, &nc) in name.iter().enumerate() {
                    if self.src[i + 2 + k].to_ascii_lowercase() != nc {
                        m = false;
                        break;
                    }
                }
                if m {
                    let after = i + 2 + name.len();
                    if after < self.src.len()
                        && (is_hws(self.src[after]) || self.src[after] == b'/' || self.src[after] == b'>')
                    {
                        return i;
                    }
                }
            }
            i += 1;
        }
        self.src.len()
    }

    /// rawtext/RCDATA トークン（C の `if_raw_token` 相当）。
    fn raw_token(&mut self) -> Tok {
        // textarea: 開始タグ直後の LF 1 個は無視
        if self.strip_lf {
            self.strip_lf = false;
            if self.pos < self.src.len() && self.src[self.pos] == b'\n' {
                self.pos += 1;
            }
        }
        let end = if self.raw_frag {
            self.src.len()
        } else if self.raw_tag == tags::tag_id(b"script") {
            self.find_script_end()
        } else {
            self.find_raw_end()
        };
        if end > self.pos {
            let mut tok = Tok::default();
            tok.kind = TokKind::Text;
            let text = if self.raw_rcdata {
                self.resolve_range(self.pos, end)
            } else {
                self.src[self.pos..end].to_vec()
            };
            let text = Self::fix_nul(&text, true);
            tok.text_had_real = !Self::is_ws_only(&text);
            tok.text = text;
            self.pos = end;
            return tok;
        }
        self.raw_tag = tags::TAG_UNKNOWN; // 終了タグは通常の字句解析へ
        self.next()
    }

    /// タグトークン（C の `if_tag_token` 相当）。
    fn tag_token(&mut self, is_end: bool) -> Tok {
        let mut tok = Tok::default();
        tok.kind = if is_end { TokKind::End } else { TokKind::Start };

        // タグ名
        let name_start = self.pos;
        while self.pos < self.src.len()
            && !is_hws(self.src[self.pos])
            && self.src[self.pos] != b'/'
            && self.src[self.pos] != b'>'
            && self.src[self.pos] != 0
        {
            self.pos += 1;
        }
        let name = &self.src[name_start..self.pos];
        tok.tag_raw = name.to_vec();
        tok.tag = tags::tag_id(name);

        let mut attrs: Vec<Attr> = Vec::new();
        loop {
            // 空白スキップ
            while self.pos < self.src.len() && is_hws(self.src[self.pos]) {
                self.pos += 1;
            }
            if self.pos >= self.src.len() {
                self.errors += 1;
                return Tok::default(); // EOF
            }
            let c = self.src[self.pos];
            if c == b'>' {
                self.pos += 1;
                tok.attrs = attrs;
                return tok;
            }
            if c == b'/' {
                self.pos += 1;
                if self.pos < self.src.len() && self.src[self.pos] == b'>' {
                    self.pos += 1;
                    tok.self_closing = true;
                    tok.attrs = attrs;
                    return tok;
                }
                self.errors += 1; // 迷いの '/' — 読み飛ばして継続
                continue;
            }
            // 属性名
            let as_ = self.pos;
            while self.pos < self.src.len() {
                let a = self.src[self.pos];
                if is_hws(a) || a == b'=' || a == b'>' || a == b'/' {
                    break;
                }
                if a == 0 {
                    self.errors += 1;
                }
                self.pos += 1;
            }
            let aname = &self.src[as_..self.pos];
            // 属性名は ASCII lowercase に正規化
            let aname_lc: Vec<u8> = aname.iter().map(|&c| c.to_ascii_lowercase()).collect();

            while self.pos < self.src.len() && is_hws(self.src[self.pos]) {
                self.pos += 1;
            }
            let mut aval: Vec<u8> = Vec::new();
            if self.pos < self.src.len() && self.src[self.pos] == b'=' {
                self.pos += 1;
                while self.pos < self.src.len() && is_hws(self.src[self.pos]) {
                    self.pos += 1;
                }
                if self.pos < self.src.len() {
                    let q = self.src[self.pos];
                    if q == b'"' || q == b'\'' {
                        self.pos += 1;
                        let vs = self.pos;
                        while self.pos < self.src.len() && self.src[self.pos] != q {
                            self.pos += 1;
                        }
                        self.in_attr_ctx = true;
                        let resolved = self.resolve_range(vs, self.pos);
                        aval = Self::fix_nul(&resolved, true);
                        self.in_attr_ctx = false;
                        if self.pos < self.src.len() {
                            self.pos += 1; // 閉じクォート
                        } else {
                            self.errors += 1;
                        }
                    } else {
                        let vs = self.pos;
                        while self.pos < self.src.len()
                            && !is_hws(self.src[self.pos])
                            && self.src[self.pos] != b'>'
                        {
                            self.pos += 1;
                        }
                        self.in_attr_ctx = true;
                        let resolved = self.resolve_range(vs, self.pos);
                        aval = Self::fix_nul(&resolved, true);
                        self.in_attr_ctx = false;
                    }
                }
            }
            if aname.is_empty() {
                self.errors += 1;
                if self.pos < self.src.len() {
                    self.pos += 1;
                }
                continue;
            }

            // 重複属性: first-wins
            if attrs.iter().any(|a| a.name == aname_lc) {
                self.errors += 1;
                continue;
            }

            if attrs.len() >= MAX_ATTRS {
                self.errors += 1;
                continue;
            }
            attrs.push(Attr {
                name: aname_lc,
                value: aval,
            });
        }
    }

    /// doctype 本体（"doctype" より後ろ）の解析。C の `if_parse_doctype_rest` 相当。
    fn parse_doctype_rest(&mut self, tok: &mut Tok, rest: &[u8]) {
        let mut p = 0usize;
        while p < rest.len() && is_hws(rest[p]) {
            p += 1;
        }
        let ns = p;
        while p < rest.len() && !is_hws(rest[p]) {
            p += 1;
        }
        if p > ns {
            let lc: Vec<u8> = rest[ns..p].iter().map(|&c| c.to_ascii_lowercase()).collect();
            tok.text = lc;
            tok.dt_has_name = true;
        }
        while p < rest.len() && is_hws(rest[p]) {
            p += 1;
        }
        if p >= rest.len() {
            return;
        }
        if Self::dt_kw(rest, p, b"public") {
            p += 6;
            while p < rest.len() && is_hws(rest[p]) {
                p += 1;
            }
            if p < rest.len() && (rest[p] == b'"' || rest[p] == b'\'') {
                p = Self::dt_quoted(rest, p, &mut tok.dt_pub);
                tok.dt_has_pub = true;
                while p < rest.len() && is_hws(rest[p]) {
                    p += 1;
                }
                if p < rest.len() && (rest[p] == b'"' || rest[p] == b'\'') {
                    let _ = Self::dt_quoted(rest, p, &mut tok.dt_sys);
                    tok.dt_has_sys = true;
                }
            } else {
                self.errors += 1; // missing public id
            }
            return;
        }
        if Self::dt_kw(rest, p, b"system") {
            p += 6;
            while p < rest.len() && is_hws(rest[p]) {
                p += 1;
            }
            if p < rest.len() && (rest[p] == b'"' || rest[p] == b'\'') {
                let _ = Self::dt_quoted(rest, p, &mut tok.dt_sys);
                tok.dt_has_sys = true;
            } else {
                self.errors += 1; // missing system id
            }
        }
    }

    /// doctype キーワード照合（CI）。C の `if_dt_kw` 相当。
    fn dt_kw(rest: &[u8], off: usize, kw: &[u8]) -> bool {
        if rest.len() < off + kw.len() {
            return false;
        }
        rest[off..off + kw.len()].eq_ignore_ascii_case(kw)
    }

    /// 引用符付き値を読む。C の `if_dt_quoted` 相当。
    fn dt_quoted(rest: &[u8], off: usize, out: &mut Vec<u8>) -> usize {
        let q = rest[off];
        let mut o = off + 1;
        let vs = o;
        while o < rest.len() && rest[o] != q {
            o += 1;
        }
        *out = rest[vs..o].to_vec();
        if o < rest.len() {
            o += 1; // 閉じ引用符
        }
        o
    }

    /// `<!--` の後ろ。コメントまたは doctype/bogus。C の `if_markup_decl` 相当。
    fn markup_decl(&mut self) -> Tok {
        let mut tok = Tok::default();

        if self.pos + 1 < self.src.len() && self.src[self.pos] == b'-' && self.src[self.pos + 1] == b'-'
        {
            self.pos += 2;
            // "<!-->" / "<!--->" の短絡形
            let mut i = self.pos;
            while i < self.src.len() && self.src[i] == b'-' {
                i += 1;
            }
            if i >= self.src.len() {
                self.errors += 1;
                tok.kind = TokKind::Comment;
                tok.text = Vec::new();
                return tok;
            }
            if self.src[i] == b'>' && i - self.pos <= 1 {
                self.pos = i + 1;
                self.errors += 1;
                tok.kind = TokKind::Comment;
                tok.text = Vec::new();
                return tok;
            }
            // "-->" または abrupt な "--!>"
            let start = self.pos;
            let mut j = start;
            while j + 2 < self.src.len() {
                if self.src[j] == b'-' && self.src[j + 1] == b'-' {
                    if self.src[j + 2] == b'>' {
                        tok.kind = TokKind::Comment;
                        tok.text = Self::fix_nul(&self.src[start..j], true);
                        self.pos = j + 3;
                        return tok;
                    }
                    if self.src[j + 2] == b'!'
                        && j + 3 < self.src.len()
                        && self.src[j + 3] == b'>'
                    {
                        self.errors += 1; // comment end bang state
                        tok.kind = TokKind::Comment;
                        tok.text = Self::fix_nul(&self.src[start..j], true);
                        self.pos = j + 4;
                        return tok;
                    }
                }
                j += 1;
            }
            // 閉じられないコメント EOF: 保留ダッシュ（末尾連続 '-' の先頭側から最大 2 個）は data に付かない
            self.errors += 1;
            let mut cend = self.src.len();
            if cend > start && self.src[cend - 1] == b'-' {
                cend -= 1;
                if cend > start && self.src[cend - 1] == b'-' {
                    cend -= 1;
                }
            }
            tok.kind = TokKind::Comment;
            tok.text = Self::fix_nul(&self.src[start..cend], true);
            self.pos = self.src.len();
            return tok;
        }

        // "<![CDATA[": 非 HTML 名前空間なら CDATA section（テキスト）
        if self.adcn_foreign
            && self.pos + 7 <= self.src.len()
            && &self.src[self.pos..self.pos + 7] == b"[CDATA["
        {
            let start = self.pos + 7;
            let mut j = start;
            while j + 2 < self.src.len()
                && !(self.src[j] == b']' && self.src[j + 1] == b']' && self.src[j + 2] == b'>')
            {
                j += 1;
            }
            let mut cdt = Tok::default();
            cdt.kind = TokKind::Text;
            if j + 2 < self.src.len() {
                let raw = &self.src[start..j];
                cdt.text_had_real = Self::real_text(raw);
                cdt.text = Self::fix_nul(raw, true);
                self.pos = j + 3;
            } else {
                self.errors += 1; // 閉じられない CDATA
                let raw = &self.src[start..self.src.len()];
                cdt.text_had_real = Self::real_text(raw);
                cdt.text = Self::fix_nul(raw, true);
                self.pos = self.src.len();
            }
            if cdt.text.is_empty() {
                return self.next();
            }
            return cdt;
        }

        // doctype or bogus
        let start = self.pos;
        while self.pos < self.src.len() && self.src[self.pos] != b'>' {
            self.pos += 1;
        }
        let body = &self.src[start..self.pos];
        if self.pos < self.src.len() {
            self.pos += 1; // '>'
        }
        let b7 = &body[..body.len().min(7)];
        if b7.eq_ignore_ascii_case(b"doctype") {
            tok.kind = TokKind::Doctype;
            let rest = if body.len() > 7 { &body[7..] } else { &[][..] };
            self.parse_doctype_rest(&mut tok, rest);
            return tok;
        }
        self.errors += 1;
        tok.kind = TokKind::Comment; // bogus comment
        tok.text = Self::fix_nul(body, true);
        tok
    }

    /// 次のトークンを返す（C の `if_tok_next` 相当）。
    #[allow(clippy::should_implement_trait)]
    pub fn next(&mut self) -> Tok {
        let mut tok = Tok::default();

        // <plaintext>: 残り全入力を 1 個の TEXT
        if self.plaintext && self.pos < self.src.len() {
            tok.kind = TokKind::Text;
            let raw = &self.src[self.pos..];
            let text = Self::fix_nul(raw, true);
            tok.text_had_real = !Self::is_ws_only(&text);
            tok.text = text;
            self.pos = self.src.len();
            return tok;
        }

        if self.raw_tag != tags::TAG_UNKNOWN && self.pos < self.src.len() {
            return self.raw_token();
        }
        if self.pos >= self.src.len() {
            return tok;
        }

        // テキスト走査: '<' まで
        if self.src[self.pos] != b'<' {
            let start = self.pos;
            while self.pos < self.src.len() && self.src[self.pos] != b'<' {
                if self.src[self.pos] == 0 {
                    self.errors += 1;
                }
                self.pos += 1;
            }
            tok.kind = TokKind::Text;
            let rs = self.resolve_range(start, self.pos);
            tok.text_had_real = Self::real_text(&rs);
            tok.text = Self::fix_nul(&rs, self.cdata_foreign);
            return tok;
        }

        // '<' の処理
        if self.pos + 1 >= self.src.len() {
            // 孤立 '<' で終端
            tok.kind = TokKind::Text;
            tok.text = b"<".to_vec();
            tok.text_had_real = true;
            self.pos = self.src.len();
            self.errors += 1;
            return tok;
        }
        let c1 = self.src[self.pos + 1];

        if is_alpha(c1) {
            self.pos += 1;
            return self.tag_token(false);
        }
        if c1 == b'/' {
            if self.pos + 2 >= self.src.len() {
                tok.kind = TokKind::Text;
                tok.text = b"</".to_vec();
                tok.text_had_real = true;
                self.pos = self.src.len();
                self.errors += 1;
                return tok;
            }
            if is_alpha(self.src[self.pos + 2]) {
                self.pos += 2;
                return self.tag_token(true);
            }
            if self.pos + 2 < self.src.len() && self.src[self.pos + 2] == b'>' {
                self.pos += 3; // "</>" は捨てる
                self.errors += 1;
                return self.next();
            }
            // "</" + その他 → bogus comment
            self.pos += 2;
            let start = self.pos;
            while self.pos < self.src.len() && self.src[self.pos] != b'>' {
                self.pos += 1;
            }
            tok.kind = TokKind::Comment;
            tok.text = self.src[start..self.pos].to_vec();
            if self.pos < self.src.len() {
                self.pos += 1;
            }
            self.errors += 1;
            return tok;
        }
        if c1 == b'!' {
            self.pos += 2;
            return self.markup_decl();
        }
        if c1 == b'?' {
            // Processing Instruction
            let q = self.pos + 1; // '?' の位置（bogus comment の data 先頭）
            let mut p = self.pos + 2;
            if p >= self.src.len() {
                // "<?": EOF で何も出さない
                self.pos = self.src.len();
                self.errors += 1;
                return self.next();
            }
            let c = self.src[p];
            let can_start = c.is_ascii_alphabetic() || c == b'_';
            if can_start {
                let ts = p;
                while p < self.src.len() {
                    let d = self.src[p];
                    if d.is_ascii_alphanumeric() || d == b'_' || d == b'-' {
                        p += 1;
                    } else {
                        break;
                    }
                }
                if p >= self.src.len() {
                    // EOF in target: 捨てる
                    self.pos = self.src.len();
                    self.errors += 1;
                    return self.next();
                }
                let target = &self.src[ts..p];
                let is_xml = target.len() >= 3
                    && target[0].eq_ignore_ascii_case(&b'x')
                    && target[1].eq_ignore_ascii_case(&b'm')
                    && target[2].eq_ignore_ascii_case(&b'l');
                let next = self.src[p];
                let term_ok = next == b'>' || next == b'?' || is_hws(next);
                if !is_xml && term_ok {
                    while p < self.src.len() && is_hws(self.src[p]) {
                        p += 1;
                    }
                    let ds = p;
                    while p < self.src.len() && self.src[p] != b'>' {
                        p += 1;
                    }
                    if p >= self.src.len() {
                        // EOF in data: 捨てる
                        self.pos = self.src.len();
                        self.errors += 1;
                        return self.next();
                    }
                    // 終端が "?>" なら '?' は data に含めない
                    let mut de = p;
                    if de > ds && self.src[de - 1] == b'?' {
                        de -= 1;
                    }
                    tok.kind = TokKind::Comment;
                    tok.is_pi = true;
                    tok.pi_target = target.to_vec();
                    tok.text = self.src[ds..de].to_vec();
                    self.pos = p + 1; // '>'
                    return tok;
                }
            }
            // bogus comment
            self.pos = q;
            let start = self.pos;
            while self.pos < self.src.len() && self.src[self.pos] != b'>' {
                self.pos += 1;
            }
            tok.kind = TokKind::Comment;
            tok.text = Self::fix_nul(&self.src[start..self.pos], true);
            if self.pos < self.src.len() {
                self.pos += 1;
            }
            self.errors += 1;
            return tok;
        }

        // '<' + その他 → '<' はリテラルテキスト
        tok.kind = TokKind::Text;
        tok.text = b"<".to_vec();
        tok.text_had_real = true;
        self.pos += 1;
        self.errors += 1;
        tok
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 全トークンを収集（EOF で停止）。
    fn collect(input: &[u8]) -> Vec<Tok> {
        let mut t = Tokenizer::new(input);
        let mut out = Vec::new();
        for _ in 0..10000 {
            let tok = t.next();
            let eof = tok.kind == TokKind::Eof;
            out.push(tok);
            if eof {
                break;
            }
        }
        out
    }

    #[test]
    fn simple_text() {
        let toks = collect(b"hello world");
        assert_eq!(toks.len(), 2); // TEXT + EOF
        assert_eq!(toks[0].kind, TokKind::Text);
        assert_eq!(toks[0].text, b"hello world");
        assert!(toks[0].text_had_real);
    }

    #[test]
    fn start_end_tags() {
        let toks = collect(b"<div>hi</div>");
        assert_eq!(toks[0].kind, TokKind::Start);
        assert_eq!(toks[0].tag_raw, b"div");
        assert_eq!(toks[1].kind, TokKind::Text);
        assert_eq!(toks[1].text, b"hi");
        assert_eq!(toks[2].kind, TokKind::End);
        assert_eq!(toks[2].tag_raw, b"div");
    }

    #[test]
    fn attributes_and_lowercase() {
        let toks = collect(b"<DIV CLASS=\"A\">x</DIV>");
        assert_eq!(toks[0].tag_raw, b"DIV");
        assert_eq!(toks[0].tag, tags::tag_id(b"div"));
        assert_eq!(toks[0].attrs.len(), 1);
        assert_eq!(toks[0].attrs[0].name, b"class"); // lowercase 正規化
        assert_eq!(toks[0].attrs[0].value, b"A");
    }

    #[test]
    fn charrefs() {
        let toks = collect(b"&amp; &#65; &#x41; &copy;");
        assert_eq!(toks[0].kind, TokKind::Text);
        assert_eq!(toks[0].text, "& A A ©".as_bytes());
    }

    #[test]
    fn two_codepoint_entity() {
        let toks = collect(b"&NotEqualTilde;");
        // U+2242 U+0338
        assert_eq!(toks[0].text, "\u{2242}\u{0338}".as_bytes());
    }

    #[test]
    fn numeric_ref_saturate() {
        // &#99999999999999999; は 0x110000 に飽和 → U+FFFD より大きいので REPLACEMENT
        let toks = collect(b"&#99999999999999999;");
        assert_eq!(toks[0].text, "\u{FFFD}".as_bytes());
    }

    #[test]
    fn rawtext_script() {
        // tree builder が <script> を見て set_raw する流れを再現
        let mut t = Tokenizer::new(b"<script>var x = 1 < 2;</script>after");
        let start = t.next();
        assert_eq!(start.kind, TokKind::Start);
        assert_eq!(start.tag_raw, b"script");
        t.set_raw(start.tag);
        let text = t.next();
        assert_eq!(text.kind, TokKind::Text);
        assert_eq!(text.text, b"var x = 1 < 2;");
        let end = t.next();
        assert_eq!(end.kind, TokKind::End);
        assert_eq!(end.tag_raw, b"script");
        let after = t.next();
        assert_eq!(after.kind, TokKind::Text);
        assert_eq!(after.text, b"after");
    }

    #[test]
    fn rcdata_title_resolves_refs() {
        let mut t = Tokenizer::new(b"<title>a &amp; b</title>");
        let start = t.next();
        assert_eq!(start.tag_raw, b"title");
        t.set_raw(start.tag);
        let text = t.next();
        // rcdata は文字参照を解決する
        assert_eq!(text.kind, TokKind::Text);
        assert_eq!(text.text, b"a & b");
    }

    #[test]
    fn comment() {
        let toks = collect(b"a<!-- comment -->b");
        assert_eq!(toks[0].kind, TokKind::Text);
        assert_eq!(toks[1].kind, TokKind::Comment);
        assert_eq!(toks[1].text, b" comment ");
    }

    #[test]
    fn pi() {
        let toks = collect(b"<?target data?>");
        assert_eq!(toks[0].kind, TokKind::Comment);
        assert!(toks[0].is_pi);
        assert_eq!(toks[0].pi_target, b"target");
        assert_eq!(toks[0].text, b"data");
    }

    #[test]
    fn doctype() {
        let toks = collect(b"<!DOCTYPE html>");
        assert_eq!(toks[0].kind, TokKind::Doctype);
        assert!(toks[0].dt_has_name);
        assert_eq!(toks[0].text, b"html");
    }

    #[test]
    fn doctype_public_system() {
        let toks = collect(b"<!DOCTYPE html PUBLIC \"-//W3C\" \"http://x\">");
        assert!(toks[0].dt_has_name);
        assert!(toks[0].dt_has_pub);
        assert!(toks[0].dt_has_sys);
        assert_eq!(toks[0].dt_pub, b"-//W3C");
        assert_eq!(toks[0].dt_sys, b"http://x");
    }

    #[test]
    fn void_elements() {
        let toks = collect(b"<br><hr><img src=x>");
        assert_eq!(toks[0].kind, TokKind::Start);
        assert_eq!(toks[0].tag_raw, b"br");
        assert_eq!(toks[1].tag_raw, b"hr");
        assert_eq!(toks[2].tag_raw, b"img");
    }

    #[test]
    fn self_closing() {
        let toks = collect(b"<br/><div/>");
        assert!(toks[0].self_closing);
        assert!(toks[1].self_closing);
    }

    #[test]
    fn nul_in_text_dropped() {
        // 既定（cdata_foreign=0）は NUL 除去
        let toks = collect(b"a\x00b");
        assert_eq!(toks[0].text, b"ab");
    }

    #[test]
    fn nul_in_attr_fffd() {
        // 属性値は NUL → U+FFFD
        let toks = collect(b"<div a=\"x\x00y\">");
        let expect: Vec<u8> = b"x".iter().chain(b"\xEF\xBF\xBD").chain(b"y").copied().collect();
        assert_eq!(toks[0].attrs[0].value, expect);
    }
}
