//! Markdown 変換層（C の `src/md.c` の **文字列 backend** 相当）。
//!
//! | C (md.h / md.c) | Rust |
//! |---|---|
//! | `if_md_to_html` | [`md_to_html`] |
//! | `if_path_is_md` | [`path_is_md`] |
//!
//! # 実装済み
//!
//! ATX 見出し / 段落 / 強調（strong/em）/ 打ち消し / インライン・フェンスコード /
//! リンク / 画像 / 引用（ネスト）/ ul・ol（インデント入れ子）/ hr / GFM パイプ表 /
//! 脚注（参照順 numbering）/ 自動リンク `<http://…>` / バックスラッシュ escape /
//! CRLF 正規化。生 HTML は通さない（全テキストは必ず escape）。
//!
//! # C との違い（所有権による構造的な改善）
//!
//! C は string backend が `realloc` ベースのステージングバッファ + `b_finish` で
//! arena へ定着させる 2 段構え。Rust では `Vec<u8>` に直接追記し、手動の容量計算と
//! 解放規約を排除する。行分割は `&[u8]` スライス（ゼロコピー）で表現する。
//!
//! # 未移植（性能最適化・観測不変）
//!
//! - **DOM 直構築 backend**（`if_md_parse_fast` / `if_md_parse_fast_f`）: `md→HTML→
//!   if_parse_html` と観測同値（ただし ws-only TEXT を剥がす `md_ws_stripped` 最適化を
//!   含む）の高速経路。文字列 backend の移植で `md_to_html + parse_html` の等価パスが
//!   完成するため、DOM 直構築は将来の最適化として保留。
//! - 2-way 並列 fast parse（`md_par_scan` / pthread）。
//! - SIMD 特殊文字走査（`scan_special_avx2`。スカラ版と同値）。
//! - rdtsc プロファイリング。

use crate::strutil::str_eq_ci;

/// 拡張子判定（`.md` / `.markdown`、case-insensitive）。C の `if_path_is_md` 相当。
pub fn path_is_md(path: &str) -> bool {
    match path.rfind('.') {
        None => false,
        Some(dot) => {
            let ext = &path[dot..];
            str_eq_ci(ext.as_bytes(), b".md") || str_eq_ci(ext.as_bytes(), b".markdown")
        }
    }
}

// ================= emitter =================

/// 出力バッファ（C の `B` 相当。`Vec<u8>` に直接追記）。
struct Out {
    buf: Vec<u8>,
}

impl Out {
    fn new() -> Self {
        Out { buf: Vec::new() }
    }
    fn put_str(&mut self, s: &str) {
        self.buf.extend_from_slice(s.as_bytes());
    }
    fn putc(&mut self, c: u8) {
        self.buf.push(c);
    }
}

/// テキストを escape して出力（`&` `<` `>`）。C の `mo_text`（string backend）相当。
fn text_escaped(out: &mut Out, s: &[u8]) {
    for &c in s {
        match c {
            b'&' => out.put_str("&amp;"),
            b'<' => out.put_str("&lt;"),
            b'>' => out.put_str("&gt;"),
            _ => out.putc(c),
        }
    }
}

/// 属性値を escape して出力（`&` `<` `"`）。C の `mo_attr`（string backend）相当。
fn attr_escaped(out: &mut Out, s: &[u8]) {
    for &c in s {
        match c {
            b'&' => out.put_str("&amp;"),
            b'<' => out.put_str("&lt;"),
            b'"' => out.put_str("&quot;"),
            _ => out.putc(c),
        }
    }
}

/// `<name>` を出力（開始タグ、属性なし）。C の `mo_open_push`（string）相当。
fn open(out: &mut Out, name: &str) {
    out.putc(b'<');
    out.put_str(name);
    out.putc(b'>');
}

/// `<name` を出力（属性付き開始タグの前半）。C の `mo_open`（string）相当。
fn open_head(out: &mut Out, name: &str) {
    out.putc(b'<');
    out.put_str(name);
}

/// 属性 ` name="value"` を出力。C の `mo_attr`（string）相当。
fn attr(out: &mut Out, name: &str, value: &[u8]) {
    out.putc(b' ');
    out.put_str(name);
    out.put_str("=\"");
    attr_escaped(out, value);
    out.putc(b'"');
}

/// `</name>` を出力。C の `mo_close`（string）相当。
fn close(out: &mut Out, name: &str) {
    out.put_str("</");
    out.put_str(name);
    out.putc(b'>');
}

// ================= 脚注 =================

/// 脚注テーブル（C の `Fn` 相当。文字列 backend は malloc/free。Rust は所有 `Vec`）。
struct Fn {
    defs: Vec<(Vec<u8>, Vec<u8>)>, // (id, text)
    refs: Vec<Vec<u8>>,            // 参照順（unique）
}

impl Fn {
    fn new() -> Self {
        Fn {
            defs: Vec::new(),
            refs: Vec::new(),
        }
    }

    fn find_def(&self, id: &[u8]) -> Option<usize> {
        self.defs.iter().position(|(d, _)| d == id)
    }

    /// 参照番号（1-based、初出順）。既出なら初出 index+1、未出なら追記して長さ。
    fn ref_number(&mut self, id: &[u8]) -> u32 {
        if let Some(i) = self.refs.iter().position(|r| r == id) {
            return (i + 1) as u32;
        }
        self.refs.push(id.to_vec());
        self.refs.len() as u32
    }

    fn add_def(&mut self, id: &[u8], text: &[u8]) {
        if self.find_def(id).is_some() {
            return;
        }
        self.defs.push((id.to_vec(), text.to_vec()));
    }
}

// ================= 行 =================

/// 行（C の `Ln` 相当。`&[u8]` スライス）。
type Ln<'a> = &'a [u8];

fn ln_blank(l: Ln) -> bool {
    l.iter().all(|&c| c == b' ' || c == b'\t')
}

/// 見出しレベル（0 = 非見出し）。C の `ln_heading` 相当。
fn ln_heading(l: Ln) -> usize {
    let mut i = 0;
    while i < l.len() && l[i] == b'#' && i < 6 {
        i += 1;
    }
    if i == 0 || i >= l.len() || (l[i] != b' ' && l[i] != b'\t') {
        return 0;
    }
    i
}

fn ln_is_hr(l: Ln) -> bool {
    let mut i = 0;
    while i < l.len() && (l[i] == b' ' || l[i] == b'\t') {
        i += 1;
    }
    if i >= l.len() {
        return false;
    }
    let c = l[i];
    if c != b'-' && c != b'*' && c != b'_' {
        return false;
    }
    let mut cnt = 0;
    while i < l.len() {
        if l[i] == c {
            cnt += 1;
        } else if l[i] != b' ' && l[i] != b'\t' {
            return false;
        }
        i += 1;
    }
    cnt >= 3
}

/// フェンス開き（0 = 非フェンス）。sym に fence 文字を返す。C の `ln_fence` 相当。
fn ln_fence(l: Ln) -> Option<(u8, usize)> {
    let mut i = 0;
    while i < l.len() && (l[i] == b' ' || l[i] == b'\t') {
        i += 1;
    }
    if i >= l.len() || (l[i] != b'`' && l[i] != b'~') {
        return None;
    }
    let c = l[i];
    let mut run = 0;
    while i < l.len() && l[i] == c {
        run += 1;
        i += 1;
    }
    if run < 3 {
        return None;
    }
    Some((c, run))
}

/// 引用符の幅（0 = 非引用）。C の `ln_quote` 相当。
fn ln_quote(l: Ln) -> usize {
    let mut i = 0;
    while i < l.len() && l[i] == b' ' {
        i += 1;
    }
    if i >= l.len() || l[i] != b'>' {
        return 0;
    }
    i += 1;
    if i < l.len() && l[i] == b' ' {
        i += 1;
    }
    i
}

/// リストマーカー。C の `ln_list_item` 相当。
struct LiMark {
    indent: usize,
    mwidth: usize,
    ordered: bool,
}

fn ln_list_item(l: Ln) -> Option<LiMark> {
    let mut i = 0;
    while i < l.len() && l[i] == b' ' {
        i += 1;
    }
    if i >= l.len() {
        return None;
    }
    if l[i] == b'-' || l[i] == b'*' || l[i] == b'+' {
        if i + 1 < l.len() && (l[i + 1] == b' ' || l[i + 1] == b'\t') {
            return Some(LiMark {
                indent: i,
                mwidth: i + 2,
                ordered: false,
            });
        }
        return None;
    }
    let ds = i;
    while i < l.len() && l[i].is_ascii_digit() {
        i += 1;
    }
    if i > ds
        && i - ds <= 9
        && i < l.len()
        && (l[i] == b'.' || l[i] == b')')
        && i + 1 < l.len()
        && (l[i + 1] == b' ' || l[i + 1] == b'\t')
    {
        return Some(LiMark {
            indent: ds,
            mwidth: i + 2,
            ordered: true,
        });
    }
    None
}

/// 脚注定義 `[^id]: text`。C の `ln_fndef` 相当。
fn ln_fndef<'a>(l: Ln<'a>) -> Option<(&'a [u8], &'a [u8])> {
    if l.len() < 5 || l[0] != b'[' || l[1] != b'^' {
        return None;
    }
    let mut i = 2;
    while i < l.len() && l[i] != b']' {
        i += 1;
    }
    if i >= l.len() || i + 1 >= l.len() || l[i + 1] != b':' {
        return None;
    }
    let mut ts = i + 2;
    while ts < l.len() && (l[ts] == b' ' || l[ts] == b'\t') {
        ts += 1;
    }
    let id = &l[2..i];
    let text = &l[ts..];
    if id.is_empty() {
        None
    } else {
        Some((id, text))
    }
}

/// `|` 区切りセル分割（trim、末尾空セル除去）。C の `split_cells` 相当。
fn split_cells(l: Ln, cells: &mut Vec<Vec<u8>>) -> usize {
    cells.clear();
    let mut i = 0;
    while i < l.len() && (l[i] == b' ' || l[i] == b'\t') {
        i += 1;
    }
    if i < l.len() && l[i] == b'|' {
        i += 1;
    }
    let mut st = i;
    let mut n = 0;
    for j in i..=l.len() {
        if j == l.len() || l[j] == b'|' {
            let cell = trim(&l[st..j]);
            cells.push(cell.to_vec());
            n += 1;
            st = j + 1;
        }
    }
    if n > 0 && cells[n - 1].is_empty() {
        cells.pop();
        n -= 1;
    }
    n
}

fn trim(s: &[u8]) -> &[u8] {
    let mut a = 0;
    let mut b = s.len();
    while a < b && (s[a] == b' ' || s[a] == b'\t' || s[a] == b'\n' || s[a] == b'\r') {
        a += 1;
    }
    while b > a && (s[b - 1] == b' ' || s[b - 1] == b'\t' || s[b - 1] == b'\n' || s[b - 1] == b'\r') {
        b -= 1;
    }
    &s[a..b]
}

fn ln_is_table_delim(l: Ln) -> bool {
    let mut cells = Vec::new();
    let n = split_cells(l, &mut cells);
    if n == 0 {
        return false;
    }
    for cell in cells {
        let mut j = 0;
        if j < cell.len() && cell[j] == b':' {
            j += 1;
        }
        let ds = j;
        while j < cell.len() && cell[j] == b'-' {
            j += 1;
        }
        if j - ds < 3 {
            return false;
        }
        if j < cell.len() && cell[j] == b':' {
            j += 1;
        }
        if j != cell.len() {
            return false;
        }
    }
    true
}

fn ln_indent(l: Ln) -> usize {
    let mut i = 0;
    while i < l.len() && l[i] == b' ' {
        i += 1;
    }
    i
}

// ================= inline =================

/// 特殊文字か。C の `scan_special` のスカラ版と同値。
fn is_special(c: u8) -> bool {
    matches!(c, b'\\' | b'`' | b'*' | b'_' | b'~' | b'!' | b'[' | b'<' | b'&' | b'>')
}

/// 次の特殊文字位置（無ければ `s.len()`）。C の `scan_special` 相当。
fn scan_special(s: &[u8], from: usize) -> usize {
    let mut i = from;
    while i < s.len() && !is_special(s[i]) {
        i += 1;
    }
    i
}

/// 閉じ区切り位置（無ければ `None`）。C の `find_close` 相当。
fn find_close(s: &[u8], from: usize, delim: &[u8]) -> Option<usize> {
    let mut i = from;
    while i + delim.len() <= s.len() {
        if s[i] == b'\\' {
            i += 1;
            continue;
        }
        if &s[i..i + delim.len()] == delim {
            return Some(i);
        }
        i += 1;
    }
    None
}

/// `[text](dest)` / `![alt](dest)` / `[^id]` / `<http://>` の判定。C の `try_link`。
/// 成功時は出力して `adv`（消費バイト数）を返す。
fn try_link(out: &mut Out, fn_: &mut Fn, s: &[u8], i: usize) -> Option<usize> {
    // footnote ref: [^id]
    if i + 1 < s.len() && s[i + 1] == b'^' {
        let mut j = i + 2;
        while j < s.len() && s[j] != b']' {
            j += 1;
        }
        if j >= s.len() {
            return None;
        }
        let id = &s[i + 2..j];
        if id.is_empty() {
            return None;
        }
        let seen = fn_.refs.iter().filter(|r| r.as_slice() == id).count();
        let num = fn_.ref_number(id);
        let idv = if seen > 0 {
            format!("fr-{}-2", String::from_utf8_lossy(id))
        } else {
            format!("fr-{}", String::from_utf8_lossy(id))
        };
        let hrv = format!("#fn-{}", String::from_utf8_lossy(id));
        open(out, "sup");
        open_head(out, "a");
        attr(out, "href", hrv.as_bytes());
        attr(out, "id", idv.as_bytes());
        out.putc(b'>');
        out.put_str(&num.to_string());
        close(out, "a");
        close(out, "sup");
        return Some(j + 1);
    }
    // 通常リンク: 対応 ] を探す（入れ子 [ ] は深さ勘定）
    let mut depth = 1usize;
    let ts = i + 1;
    let mut j = i + 1;
    while j < s.len() && depth != 0 {
        if s[j] == b'\\' {
            j += 2;
            continue;
        }
        if s[j] == b'[' {
            depth += 1;
        } else if s[j] == b']' {
            depth -= 1;
        }
        j += 1;
    }
    if depth != 0 || j >= s.len() || s[j] != b'(' {
        return None;
    }
    let text = &s[ts..j - 1];
    let ds = j + 1;
    let mut k = ds;
    while k < s.len() && s[k] != b')' {
        k += 1;
    }
    if k >= s.len() {
        return None;
    }
    let dest = &s[ds..k];
    open_head(out, "a");
    attr(out, "href", dest);
    out.putc(b'>');
    inline_span(out, fn_, text);
    close(out, "a");
    Some(k + 1)
}

fn inline_span(out: &mut Out, fn_: &mut Fn, s: &[u8]) {
    let mut i = 0;
    while i < s.len() {
        let sp0 = scan_special(s, i);
        if sp0 > i {
            text_escaped(out, &s[i..sp0]);
            i = sp0;
        }
        if i >= s.len() {
            break;
        }
        let c = s[i];
        match c {
            b'\\' => {
                if i + 1 < s.len() {
                    let n2 = s[i + 1];
                    if matches!(
                        n2,
                        b'\\' | b'`' | b'*' | b'_' | b'{' | b'}' | b'[' | b']' | b'(' | b')'
                            | b'#' | b'+' | b'-' | b'.' | b'!' | b'~' | b'|' | b'<' | b'>'
                    ) {
                        out.putc(n2);
                        i += 2;
                        continue;
                    }
                }
                text_escaped(out, &s[i..i + 1]);
                i += 1;
            }
            b'`' => {
                let mut run = 1;
                while i + run < s.len() && s[i + run] == b'`' {
                    run += 1;
                }
                // 閉じ区切りを探す
                let close_pos = if run <= 2 {
                    let delim = vec![b'`'; run];
                    find_close(s, i + run, &delim)
                } else {
                    s[i + run..].iter().position(|&c| c == b'`').map(|p| p + i + run)
                };
                match close_pos {
                    None => {
                        text_escaped(out, &s[i..i + 1]);
                        i += 1;
                    }
                    Some(cl) => {
                        open_head(out, "code");
                        out.putc(b'>');
                        text_escaped(out, &s[i + run..cl]);
                        close(out, "code");
                        i = cl + run;
                    }
                }
            }
            b'*' | b'_' => {
                if i + 1 < s.len() && s[i + 1] == c {
                    let delim = if c == b'*' { b"**".as_slice() } else { b"__".as_slice() };
                    if let Some(cl) = find_close(s, i + 2, delim) {
                        if cl > i + 2 {
                            open(out, "strong");
                            inline_span(out, fn_, &s[i + 2..cl]);
                            close(out, "strong");
                            i = cl + 2;
                            continue;
                        }
                    }
                }
                let delim = if c == b'*' { b"*".as_slice() } else { b"_".as_slice() };
                if let Some(cl) = find_close(s, i + 1, delim) {
                    if cl > i + 1 {
                        open(out, "em");
                        inline_span(out, fn_, &s[i + 1..cl]);
                        close(out, "em");
                        i = cl + 1;
                        continue;
                    }
                }
                text_escaped(out, &s[i..i + 1]);
                i += 1;
            }
            b'~' => {
                if i + 1 < s.len() && s[i + 1] == b'~' {
                    if let Some(cl) = find_close(s, i + 2, b"~~") {
                        if cl > i + 2 {
                            open(out, "del");
                            inline_span(out, fn_, &s[i + 2..cl]);
                            close(out, "del");
                            i = cl + 2;
                            continue;
                        }
                    }
                }
                text_escaped(out, &s[i..i + 1]);
                i += 1;
            }
            b'!' => {
                if i + 1 < s.len() && s[i + 1] == b'[' {
                    let rest = &s[i + 1..];
                    let mut k = 1;
                    while k < rest.len() {
                        if rest[k] == b'\\' {
                            k += 2;
                            continue;
                        }
                        if rest[k] == b']' {
                            break;
                        }
                        k += 1;
                    }
                    let mut adv0 = 0;
                    if k < rest.len() && k + 1 < rest.len() && rest[k + 1] == b'(' {
                        let ds = k + 2;
                        let mut ke = ds;
                        while ke < rest.len() && rest[ke] != b')' {
                            ke += 1;
                        }
                        if ke < rest.len() {
                            open_head(out, "img");
                            attr(out, "src", &rest[ds..ke]);
                            attr(out, "alt", &rest[1..k]);
                            out.putc(b'>');
                            adv0 = ke + 1;
                        }
                    }
                    if adv0 != 0 {
                        i = i + 1 + adv0;
                        continue;
                    }
                }
                text_escaped(out, &s[i..i + 1]);
                i += 1;
            }
            b'[' => {
                if let Some(adv) = try_link(out, fn_, &s[i..], 0) {
                    i += adv;
                } else {
                    text_escaped(out, &s[i..i + 1]);
                    i += 1;
                }
            }
            b'<' => {
                let mut j = i + 1;
                while j < s.len() && s[j] != b'>' {
                    j += 1;
                }
                if j < s.len() {
                    let url = &s[i + 1..j];
                    if (url.len() > 7 && &url[..7] == b"http://")
                        || (url.len() > 8 && &url[..8] == b"https://")
                    {
                        open_head(out, "a");
                        attr(out, "href", url);
                        out.putc(b'>');
                        text_escaped(out, url);
                        close(out, "a");
                        i = j + 1;
                        continue;
                    }
                }
                text_escaped(out, &s[i..i + 1]);
                i += 1;
            }
            _ => {
                text_escaped(out, &s[i..i + 1]);
                i += 1;
            }
        }
    }
}

// ================= ブロック層 =================

/// 段落行を連結（ハードブレーク対応）。C の `emit_para_lines` 相当。
fn emit_para_lines(out: &mut Out, fn_: &mut Fn, ls: &[Ln], lo: usize, hi: usize) {
    open(out, "p");
    let mut prev_hard = false;
    for (idx, &line) in ls[lo..hi].iter().enumerate() {
        let mut x = line;
        let mut trail = 0;
        while trail < x.len() && x[x.len() - 1 - trail] == b' ' {
            trail += 1;
        }
        let hard = trail >= 2;
        if hard {
            x = &x[..x.len() - trail];
        }
        if idx > 0 {
            if prev_hard {
                open(out, "br");
            } else {
                out.putc(b' ');
            }
        }
        inline_span(out, fn_, x);
        prev_hard = hard;
    }
    close(out, "p");
    out.putc(b'\n');
}

fn blocks_win(out: &mut Out, fn_: &mut Fn, ls: &[Ln], lo: usize, hi: usize, depth: usize) {
    let mut i = lo;
    while i < hi {
        let l = ls[i];
        if ln_blank(l) {
            i += 1;
            continue;
        }
        // 先頭非空白文字
        let mut sp = 0;
        while sp < l.len() && l[sp] == b' ' {
            sp += 1;
        }
        let cs = if sp < l.len() { l[sp] } else { 0 };

        // 脚注定義
        if sp == 0 && cs == b'[' {
            if let Some((id, text)) = ln_fndef(l) {
                fn_.add_def(id, text);
                i += 1;
                continue;
            }
        }
        // 見出し
        if sp == 0 && cs == b'#' {
            let hh = ln_heading(l);
            if hh > 0 {
                const HNM: [&str; 6] = ["h1", "h2", "h3", "h4", "h5", "h6"];
                let nm = HNM[hh - 1];
                open(out, nm);
                let mut k = hh;
                while k < l.len() && (l[k] == b' ' || l[k] == b'\t') {
                    k += 1;
                }
                let mut t = &l[k..];
                // 末尾空白 trim
                let mut e = t.len() as i32 - 1;
                while e >= 0 && (t[e as usize] == b' ' || t[e as usize] == b'\t') {
                    e -= 1;
                }
                let mut he = e;
                while he >= 0 && t[he as usize] == b'#' {
                    he -= 1;
                }
                if he < e && he >= 0 && (t[he as usize] == b' ' || t[he as usize] == b'\t') {
                    t = &t[..he as usize];
                } else {
                    t = &t[..(e + 1) as usize];
                }
                inline_span(out, fn_, t);
                close(out, nm);
                out.putc(b'\n');
                i += 1;
                continue;
            }
        }
        // hr
        if (cs == b'-' || cs == b'*' || cs == b'_' || cs == b'\t') && ln_is_hr(l) {
            open(out, "hr");
            out.putc(b'\n');
            i += 1;
            continue;
        }
        // fence
        if (cs == b'`' || cs == b'~' || cs == b'\t') && ln_fence(l).is_some() {
            let fsym = ln_fence(l).unwrap().0;
            let mut k = 0;
            while k < l.len() && l[k] == fsym {
                k += 1;
            }
            while k < l.len() && (l[k] == b' ' || l[k] == b'\t') {
                k += 1;
            }
            let lang = &l[k..];
            open(out, "pre");
            open_head(out, "code");
            if !lang.is_empty() {
                let cv = format!("lang-{}", String::from_utf8_lossy(lang));
                attr(out, "class", cv.as_bytes());
            }
            out.putc(b'>');
            i += 1;
            while i < hi {
                let cl = ls[i];
                if let Some((s2, _)) = ln_fence(cl) {
                    if s2 == fsym {
                        i += 1;
                        break;
                    }
                }
                text_escaped(out, cl);
                out.putc(b'\n');
                i += 1;
            }
            close(out, "code");
            close(out, "pre");
            out.putc(b'\n');
            continue;
        }
        // quote
        let q = if cs == b'>' { ln_quote(l) } else { 0 };
        if q > 0 {
            let mut j = i;
            while j < hi && ln_quote(ls[j]) > 0 {
                j += 1;
            }
            if depth < 8 {
                let cnt = j - i;
                // 各行の引用符を除いた副窓
                let wq: Vec<Ln> = (i..j).map(|k| {
                    let w = ln_quote(ls[k]);
                    &ls[k][w..]
                }).collect();
                open(out, "blockquote");
                out.putc(b'\n');
                blocks_win(out, fn_, &wq, 0, cnt, depth + 1);
                close(out, "blockquote");
                out.putc(b'\n');
            } else {
                // 深度飽和: flatten
                let mut flat = Vec::new();
                for &line in &ls[i..j] {
                    let w = ln_quote(line);
                    flat.extend_from_slice(&line[w..]);
                    flat.push(b'\n');
                }
                open(out, "blockquote");
                out.putc(b'\n');
                open(out, "p");
                inline_span(out, fn_, &flat);
                close(out, "p");
                out.putc(b'\n');
                close(out, "blockquote");
                out.putc(b'\n');
            }
            i = j;
            continue;
        }
        // list
        if (cs == b'-' || cs == b'*' || cs == b'+' || cs.is_ascii_digit()) && ln_list_item(l).is_some() {
            let mk = ln_list_item(l).unwrap();
            let ordered = mk.ordered;
            let base = mk.indent;
            open(out, if ordered { "ol" } else { "ul" });
            out.putc(b'\n');
            while i < hi {
                match ln_list_item(ls[i]) {
                    Some(m2) if m2.ordered == ordered && m2.indent == base => {
                        open(out, "li");
                        inline_span(out, fn_, &ls[i][m2.mwidth..]);
                        i += 1;
                        let mut j = i;
                        while j < hi && !ln_blank(ls[j]) && ln_indent(ls[j]) > base {
                            j += 1;
                        }
                        if j > i {
                            blocks_win(out, fn_, ls, i, j, depth);
                            i = j;
                        }
                        close(out, "li");
                        out.putc(b'\n');
                    }
                    _ => break,
                }
            }
            close(out, if ordered { "ol" } else { "ul" });
            out.putc(b'\n');
            continue;
        }
        // GFM 表
        let mut is_table = false;
        if i + 1 < hi {
            let dl = ls[i + 1];
            let mut dsp = 0;
            while dsp < dl.len() && (dl[dsp] == b' ' || dl[dsp] == b'\t') {
                dsp += 1;
            }
            if dsp < dl.len()
                && (dl[dsp] == b'|' || dl[dsp] == b'-' || dl[dsp] == b':')
                && ln_is_table_delim(dl)
            {
                let has_pipe = l.contains(&b'|');
                if has_pipe {
                    is_table = true;
                }
            }
        }
        if is_table {
            let mut heads = Vec::new();
            let nh = split_cells(l, &mut heads).min(32);
            open(out, "table");
            out.putc(b'\n');
            open(out, "thead");
            open(out, "tr");
            for head in &heads[..nh] {
                open(out, "th");
                inline_span(out, fn_, head);
                close(out, "th");
            }
            close(out, "tr");
            close(out, "thead");
            out.putc(b'\n');
            open(out, "tbody");
            out.putc(b'\n');
            i += 2;
            while i < hi && !ln_blank(ls[i]) {
                if !ls[i].contains(&b'|') {
                    break;
                }
                let mut cells = Vec::new();
                let nc = split_cells(ls[i], &mut cells).min(32);
                open(out, "tr");
                for cell in &cells[..nc] {
                    open(out, "td");
                    inline_span(out, fn_, cell);
                    close(out, "td");
                }
                close(out, "tr");
                out.putc(b'\n');
                i += 1;
            }
            close(out, "tbody");
            out.putc(b'\n');
            close(out, "table");
            out.putc(b'\n');
            continue;
        }
        // 段落
        let mut j = i;
        while j < hi {
            let x = ls[j];
            if ln_blank(x) {
                break;
            }
            if j > i {
                let mut xsp = 0;
                while xsp < x.len() && x[xsp] == b' ' {
                    xsp += 1;
                }
                let xcs = if xsp < x.len() { x[xsp] } else { 0 };
                let is_block_start = (xsp == 0 && xcs == b'#' && ln_heading(x) > 0)
                    || ((xcs == b'-' || xcs == b'*' || xcs == b'_' || xcs == b'\t')
                        && ln_is_hr(x))
                    || (xcs == b'>' && ln_quote(x) > 0)
                    || (xsp == 0 && xcs == b'[' && ln_fndef(x).is_some())
                    || ((xcs == b'`' || xcs == b'~' || xcs == b'\t') && ln_fence(x).is_some())
                    || ((xcs == b'-' || xcs == b'*' || xcs == b'+' || xcs.is_ascii_digit())
                        && ln_list_item(x).is_some())
                    || ((xcs == b'|' || xcs == b'-' || xcs == b':' || xcs == b'\t')
                        && ln_is_table_delim(x));
                if is_block_start {
                    break;
                }
            }
            j += 1;
        }
        emit_para_lines(out, fn_, ls, i, j);
        i = j;
    }
}

fn blocks_str(out: &mut Out, fn_: &mut Fn, s: &[u8], depth: usize) {
    // 行配列へ分割（ゼロコピー切片）
    let mut ls: Vec<Ln> = Vec::new();
    let mut st = 0usize;
    let mut p = 0usize;
    while p <= s.len() {
        let nl = s[p..].iter().position(|&c| c == b'\n').map(|off| p + off);
        let e = nl.unwrap_or(s.len());
        if e == s.len() && st == s.len() && !s.is_empty() {
            break;
        }
        ls.push(&s[st..e]);
        if nl.is_none() {
            break;
        }
        st = e + 1;
        p = e + 1;
    }
    blocks_win(out, fn_, &ls, 0, ls.len(), depth);
}

/// Markdown → HTML 変換。C の `if_md_to_html`（string backend）相当。
pub fn md_to_html(input: &[u8]) -> Vec<u8> {
    let mut out = Out::new();
    let mut fn_ = Fn::new();
    run_blocks(&mut out, &mut fn_, input);
    out.buf
}

fn run_blocks(out: &mut Out, fn_: &mut Fn, input: &[u8]) {
    // CR/CRLF → LF 正規化（'\r' が無ければゼロコピー）
    let normalized: Vec<u8>;
    let s: &[u8] = if input.contains(&b'\r') {
        normalized = {
            let mut v = Vec::with_capacity(input.len());
            let mut i = 0;
            while i < input.len() {
                let c = input[i];
                if c == b'\r' {
                    if i + 1 < input.len() && input[i + 1] == b'\n' {
                        i += 1;
                    }
                    v.push(b'\n');
                } else {
                    v.push(c);
                }
                i += 1;
            }
            v
        };
        &normalized
    } else {
        input
    };
    blocks_str(out, fn_, s, 0);

    // 脚注セクション（参照されたものだけ、参照順）
    if !fn_.refs.is_empty() {
        open_head(out, "section");
        attr(out, "class", b"footnotes");
        out.putc(b'>');
        out.putc(b'\n');
        open(out, "hr");
        out.putc(b'\n');
        open(out, "ol");
        out.putc(b'\n');
        let refs = fn_.refs.clone();
        for id in refs {
            let di = fn_.find_def(&id);
            let idv = format!("fn-{}", String::from_utf8_lossy(&id));
            let hrv = format!("#fr-{}", String::from_utf8_lossy(&id));
            open_head(out, "li");
            attr(out, "id", idv.as_bytes());
            out.putc(b'>');
            let txt = di.map_or(Vec::new(), |d| fn_.defs[d].1.clone());
            inline_span(out, fn_, &txt);
            out.putc(b' ');
            open_head(out, "a");
            attr(out, "href", hrv.as_bytes());
            out.putc(b'>');
            out.put_str("\u{21A9}"); // ↩
            close(out, "a");
            close(out, "li");
            out.putc(b'\n');
        }
        close(out, "ol");
        out.putc(b'\n');
        close(out, "section");
        out.putc(b'\n');
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn md(src: &str) -> String {
        String::from_utf8(md_to_html(src.as_bytes())).unwrap()
    }

    #[test]
    fn path_ext() {
        assert!(path_is_md("README.md"));
        assert!(path_is_md("/a/b/Guide.MARKDOWN"));
        assert!(!path_is_md("index.html"));
        assert!(!path_is_md("noext"));
    }

    #[test]
    fn heading() {
        assert_eq!(md("# Title"), "<h1>Title</h1>\n");
        assert_eq!(md("### H3 ###"), "<h3>H3</h3>\n");
        assert_eq!(md("x\n##H2 (not heading)"), "<p>x ##H2 (not heading)</p>\n");
    }

    #[test]
    fn para_and_break() {
        assert_eq!(md("alpha\nbeta"), "<p>alpha beta</p>\n");
        assert_eq!(md("alpha  \nbeta"), "<p>alpha<br>beta</p>\n");
    }

    #[test]
    fn inline_fmt() {
        assert_eq!(
            md("a **b** and *c* ~~d~~ `e<f`"),
            "<p>a <strong>b</strong> and <em>c</em> <del>d</del> <code>e&lt;f</code></p>\n"
        );
        assert_eq!(md("\\*no fmt\\*"), "<p>*no fmt*</p>\n");
        assert_eq!(md("**unclosed"), "<p>**unclosed</p>\n");
    }

    #[test]
    fn link_img_autolink() {
        assert_eq!(
            md("[t](u?v=1&k=2)"),
            "<p><a href=\"u?v=1&amp;k=2\">t</a></p>\n"
        );
        assert_eq!(md("[a *b*](d)"), "<p><a href=\"d\">a <em>b</em></a></p>\n");
        assert_eq!(
            md("![alt x](p.png)"),
            "<p><img src=\"p.png\" alt=\"alt x\"></p>\n"
        );
        assert_eq!(
            md("<https://example.jp/?a=1&b=2>"),
            "<p><a href=\"https://example.jp/?a=1&amp;b=2\">https://example.jp/?a=1&amp;b=2</a></p>\n"
        );
    }

    #[test]
    fn raw_html_escaped() {
        assert_eq!(
            md("<script>alert(1)</script>"),
            "<p>&lt;script&gt;alert(1)&lt;/script&gt;</p>\n"
        );
    }

    #[test]
    fn blockquote() {
        assert_eq!(
            md("> q **b**\n> line2"),
            "<blockquote>\n<p>q <strong>b</strong> line2</p>\n</blockquote>\n"
        );
        assert_eq!(
            md("> outer\n> > inner"),
            "<blockquote>\n<p>outer</p>\n<blockquote>\n<p>inner</p>\n</blockquote>\n</blockquote>\n"
        );
    }

    #[test]
    fn list() {
        assert_eq!(md("- a\n- b"), "<ul>\n<li>a</li>\n<li>b</li>\n</ul>\n");
        assert_eq!(md("1. a\n2. b"), "<ol>\n<li>a</li>\n<li>b</li>\n</ol>\n");
        assert_eq!(
            md("- a\n  - x\n  - y\n- b"),
            "<ul>\n<li>a<ul>\n<li>x</li>\n<li>y</li>\n</ul>\n</li>\n<li>b</li>\n</ul>\n"
        );
        assert_eq!(md("- a\n\ntail"), "<ul>\n<li>a</li>\n</ul>\n<p>tail</p>\n");
    }

    #[test]
    fn fence_and_hr() {
        assert_eq!(
            md("```c\nint x = 1 < 2;\n```"),
            "<pre><code class=\"lang-c\">int x = 1 &lt; 2;\n</code></pre>\n"
        );
        assert_eq!(md("```\n&raw\n"), "<pre><code>&amp;raw\n</code></pre>\n");
        assert_eq!(md("---"), "<hr>\n");
        assert_eq!(md("***"), "<hr>\n");
        assert_eq!(md("--- a"), "<p>--- a</p>\n");
    }

    #[test]
    fn table() {
        assert_eq!(
            md("| a | b |\n| --- | --- |\n| *1* | 2 |"),
            "<table>\n<thead><tr><th>a</th><th>b</th></tr></thead>\n<tbody>\n<tr><td><em>1</em></td><td>2</td></tr>\n</tbody>\n</table>\n"
        );
        assert_eq!(md("a|b\n--|--\n1|2"), "<p>a|b --|-- 1|2</p>\n");
    }

    #[test]
    fn footnote() {
        assert_eq!(
            md("text[^a] more[^b] again[^a]\n\n[^a]: first **n**\n[^b]: second"),
            "<p>text<sup><a href=\"#fn-a\" id=\"fr-a\">1</a></sup> more<sup><a href=\"#fn-b\" id=\"fr-b\">2</a></sup> again<sup><a href=\"#fn-a\" id=\"fr-a-2\">1</a></sup></p>\n\
<section class=\"footnotes\">\n<hr>\n<ol>\n\
<li id=\"fn-a\">first <strong>n</strong> <a href=\"#fr-a\">\u{21A9}</a></li>\n\
<li id=\"fn-b\">second <a href=\"#fr-b\">\u{21A9}</a></li>\n\
</ol>\n</section>\n"
        );
    }

    #[test]
    fn crlf_normalize() {
        assert_eq!(md("a\r\nb\r\n\r\nc"), "<p>a b</p>\n<p>c</p>\n");
    }
}
