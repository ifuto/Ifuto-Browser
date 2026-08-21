//! セルグリッドレンダラ（ソフトウェアラスタ。C の `src/render_ansi.c` 相当）。
//!
//! | C (render.h / render_ansi.c) | Rust |
//! |---|---|
//! | `IfGrid` / `IfCell` | [`Grid`] / [`Cell`] |
//! | `if_render_grid` | [`render_grid`] |
//! | `if_render_emit` | [`render_emit`] |
//! | `if_render_extent` | [`render_extent`] |
//! | `if_rgba_to_ansi` | [`rgba_to_ansi`] |
//!
//! # 実装済み
//!
//! ボックスツリー → セルグリッド（背景塗り・罫線・`<hr>`・li マーカー・テキスト）、
//! グリッド → 発行バイト列（ansi=256 色 SGR / plain）。golden テスト（`tests/golden/`）
//! が固定する `--no-ansi` 出力の完全一致が回帰オラクル。
//!
//! # C との違い（所有権による構造的な改善）
//!
//! C はグリッドを arena から確保し `cells[(y-y_off)*w+x]` の手動 index で参照する。
//! Rust では `Vec<Cell>` + `get` で境界検査を保証する。発行は `Vec<u8>` に追記し、
//! C の行バッファ + fwrite フラッシュの手動管理を排除する。
//!
//! # 未移植（性能最適化・観測不変）
//!
//! 以下は発行バイト列に影響しない最適化のため保留する:
//! - 窓グリッド経路（`if_render_grid_rows_into(_cur)`）と厳密増加カーソル
//! - 行スイープ直接発行（`if_render_emit_rows_sweep`）・byte-direct/fast 経路
//! - 2-way 並列 sweep（pthread）
//! - `raster.c` の fill カーネル自動選択（全候補が bit-exact 同値。スカラ fill で十分）
//! - rdtsc プロファイリング

use crate::layout::{BoxKind, BoxNode, Layout};
use crate::css::{Style, D_LIST_ITEM};
use crate::tags_tables::{TAG_HR, TAG_LI, TAG_OL, TAG_UL};

/// fg/bg の「端末既定」値（C の `IF_CELL_DEFAULT`）。
pub const CELL_DEFAULT: u8 = 255;

/// セルフラグ: 太字（C の `IF_F_BOLD`）。
pub const F_BOLD: u8 = 1;
/// セルフラグ: 斜体。
pub const F_ITALIC: u8 = 2;
/// セルフラグ: 下線。
pub const F_ULINE: u8 = 4;
/// セルフラグ: 打ち消し線。
pub const F_STRIKE: u8 = 8;

/// セル（C の `IfCell` 相当）。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Cell {
    /// コードポイント（0 = 全角 2 セル目の継続セル）。
    pub cp: u32,
    /// 前景色（ANSI 256 index or `CELL_DEFAULT`）。
    pub fg: u8,
    /// 背景色。
    pub bg: u8,
    /// 装飾フラグ。
    pub flags: u8,
}

impl Default for Cell {
    fn default() -> Self {
        Cell {
            cp: b' ' as u32,
            fg: CELL_DEFAULT,
            bg: CELL_DEFAULT,
            flags: 0,
        }
    }
}

/// セルグリッド（C の `IfGrid` 相当。全高グリッド）。
#[derive(Clone, Debug)]
pub struct Grid {
    /// 幅（セル）。
    pub w: i32,
    /// 高さ（セル）。
    pub h: i32,
    /// セル列（`cells[y * w + x]`）。
    pub cells: Vec<Cell>,
}

impl Grid {
    fn at(&self, x: i32, y: i32) -> Option<&Cell> {
        if x < 0 || y < 0 || x >= self.w || y >= self.h {
            return None;
        }
        self.cells.get((y * self.w + x) as usize)
    }

    fn at_mut(&mut self, x: i32, y: i32) -> Option<&mut Cell> {
        if x < 0 || y < 0 || x >= self.w || y >= self.h {
            return None;
        }
        self.cells.get_mut((y * self.w + x) as usize)
    }
}

/// RGBA8 → ANSI 256 色 or `CELL_DEFAULT`。C の `if_rgba_to_ansi` 相当。
pub fn rgba_to_ansi(rgba: u32) -> u8 {
    let a = rgba & 0xFF;
    if a < 128 {
        return CELL_DEFAULT;
    }
    let r = rgba >> 24;
    let g = (rgba >> 16) & 0xFF;
    let b = (rgba >> 8) & 0xFF;
    // グレー特別経路: 立方体の端に寄らず灰色ランプへ
    if r == g && g == b {
        if r < 8 {
            return 16;
        }
        if r > 238 {
            return 15;
        }
        return (232 + ((r - 8) * 24) / 240) as u8;
    }
    let rq = (r * 5 + 127) / 255;
    let gq = (g * 5 + 127) / 255;
    let bq = (b * 5 + 127) / 255;
    (16 + 36 * rq + 6 * gq + bq) as u8
}

fn pen(st: Option<&Style>) -> (u8, u8, u8) {
    match st {
        None => (CELL_DEFAULT, CELL_DEFAULT, 0),
        Some(st) => {
            let mut flags = 0u8;
            if st.bold {
                flags |= F_BOLD;
            }
            if st.italic {
                flags |= F_ITALIC;
            }
            if st.underline {
                flags |= F_ULINE;
            }
            if st.strike {
                flags |= F_STRIKE;
            }
            (rgba_to_ansi(st.color), rgba_to_ansi(st.bg), flags)
        }
    }
}

fn put_cp(g: &mut Grid, x: i32, y: i32, cp: u32, st: Option<&Style>, bg_override: u32) {
    let Some(c) = g.at_mut(x, y) else { return };
    let (fg, bg, flags) = pen(st);
    c.cp = cp;
    c.fg = fg;
    c.bg = if bg_override != 0 {
        rgba_to_ansi(bg_override)
    } else {
        bg
    };
    c.flags = flags;
}

fn fill_bg(g: &mut Grid, x0: i32, y0: i32, w: i32, h: i32, bg: u32) {
    let idx = rgba_to_ansi(bg);
    if idx == CELL_DEFAULT && (bg & 0xFF) < 128 {
        return;
    }
    // 行スイープ経路の deco 有効高 dh = max(h,1) に一致（h=0 の空ボックスも 1 行塗る）
    let eh = h.max(1);
    let yy0 = y0.max(0);
    let yy1 = (y0 + eh).min(g.h);
    let xx0 = x0.max(0);
    let xx1 = (x0 + w).min(g.w);
    for y in yy0..yy1 {
        for x in xx0..xx1 {
            if let Some(c) = g.at_mut(x, y) {
                c.bg = idx;
            }
        }
    }
}

fn draw_text(g: &mut Grid, x: i32, y: i32, text: &[u8], st: Option<&Style>) {
    let (fg, bg, flags) = pen(st);
    let mut i = 0usize;
    let mut cx = x;
    while i < text.len() {
        let cp = crate::utf8::decode(text, &mut i);
        let gw = crate::utf8::glyph_width(cp);
        if gw == 0 {
            continue;
        }
        if let Some(c) = g.at_mut(cx, y) {
            c.cp = cp;
            c.fg = fg;
            c.bg = bg;
            c.flags = flags;
        }
        if gw == 2 {
            if let Some(c) = g.at_mut(cx + 1, y) {
                c.cp = 0;
                c.fg = fg;
                c.bg = bg;
                c.flags = flags;
            }
        }
        cx += gw;
    }
}

fn draw_hline(g: &mut Grid, x0: i32, x1: i32, y: i32, st: Option<&Style>) {
    for x in x0..x1 {
        put_cp(g, x, y, 0x2500, st, 0); // ─
    }
}

/// li マーカー（ul: "• "、ol: "N."）。C の行スイープ経路の MARKER deco 相当。
/// マーカーは `<li>` かつ `display:list-item` のときのみ（任意の `display:list-item`
/// 要素には描かない。sweep の `c->tag == IF_TAG_LI && display == LIST_ITEM` 条件と同値）。
fn draw_marker(
    g: &mut Grid,
    dom: &crate::dom::Dom,
    styles: &[Option<Style>],
    b: &BoxNode,
) {
    let Some(li) = b.node else { return };
    // 直近の祖先 ul/ol を探す（ネストした li の外には出ない）
    let mut list = TAG_UL;
    let mut p = dom.node(li).parent;
    while let Some(pid) = p {
        let pnode = dom.node(pid);
        if pnode.kind == crate::dom::NodeKind::Element && (pnode.tag == TAG_UL || pnode.tag == TAG_OL) {
            list = pnode.tag;
            break;
        }
        if pnode.kind == crate::dom::NodeKind::Element && pnode.tag == TAG_LI {
            break;
        }
        p = pnode.parent;
    }
    if list == TAG_UL {
        let mut mx = b.x - 2;
        if mx < 0 {
            mx = b.x;
        }
        draw_text(g, mx, b.y, b"\xE2\x80\xA2 ", Some(&b.st)); // "• "
        return;
    }
    // ol: 兄弟内の「list-item の li」を数える（sweep の li_ord と同値）
    let mut idx = 1u32;
    let mut s = dom.node(li).parent.and_then(|p| dom.node(p).first_child);
    while let Some(sid) = s {
        if sid == li {
            break;
        }
        let snode = dom.node(sid);
        if snode.kind == crate::dom::NodeKind::Element
            && snode.tag == TAG_LI
            && styles[sid as usize].is_some_and(|s| s.display == D_LIST_ITEM)
        {
            idx += 1;
        }
        s = snode.next_sibling;
    }
    let txt = format!("{idx}.");
    let m = txt.len() as i32;
    let mut mx = b.x - (m + 1);
    if mx < 0 {
        mx = 0;
    }
    draw_text(g, mx, b.y, txt.as_bytes(), Some(&b.st));
}

/// b 自身の装飾（背景/HR/罫線/li マーカー）を描く。子供に進むなら true。
/// C の行スイープ経路（SLOW）の deco 挿入順 = DFS で「marker(li) → 自 BG → 自 BORDER」
/// を再現する。これにより:
///
/// - 自 BORDER は marker を上書き（`<li style="border">` で ┌ が勝つ）
/// - 前方兄弟の BORDER は marker に上書きされる（`<dt style="border"><li>` で • が勝つ）
///
/// 既知の偏差: C の FAST 経路（罫線なし行）は marker を上層ランとして扱い、子孫 BG に
/// 上書きされないが、ここでは SLOW の deco 順（子孫 BG が marker の bg を上書き）に一致
/// させる（`<li><dl style="background">` の clamped marker の 1 ケースのみ FAST と乖離）。
fn paint_shell(g: &mut Grid, dom: &crate::dom::Dom, styles: &[Option<Style>], b: &BoxNode) -> bool {
    let st = &b.st;

    // 1) li マーカー（deco 挿入順で自 BG/BORDER より先）
    if b.node.is_some_and(|n| dom.node(n).tag == TAG_LI) && b.st.display == D_LIST_ITEM {
        draw_marker(g, dom, styles, b);
    }

    // 2) 背景
    if (st.bg & 0xFF) >= 128 {
        fill_bg(g, b.x, b.y, b.w, b.h, st.bg);
    }

    if b.node.is_some_and(|n| dom.node(n).tag == TAG_HR) {
        let off = if st.border_w[0] > 0.0 { 1 } else { 0 };
        draw_hline(g, b.x, b.x + b.w, b.y + off, None);
        return false;
    }

    // 3) 罫線（solid のみ。Unicode 罫線素片）
    let bc = st.border_color;
    let fg = rgba_to_ansi(bc);
    let any = st.border_w[0] > 0.0 || st.border_w[1] > 0.0 || st.border_w[2] > 0.0 || st.border_w[3] > 0.0;
    if any {
        // 行スイープ経路の deco 有効高 dh = max(h,1) に一致（左右罫線は h=0 でも 1 行）
        let eh = b.h.max(1);
        for x in b.x..b.x + b.w {
            if st.border_w[0] > 0.0 {
                if let Some(c) = g.at_mut(x, b.y) {
                    c.cp = 0x2500;
                    c.fg = fg;
                }
            }
            if st.border_w[2] > 0.0 {
                if let Some(c) = g.at_mut(x, b.y + b.h - 1) {
                    c.cp = 0x2500;
                    c.fg = fg;
                }
            }
        }
        for y in b.y..b.y + eh {
            if st.border_w[3] > 0.0 {
                if let Some(c) = g.at_mut(b.x, y) {
                    c.cp = 0x2502;
                    c.fg = fg;
                }
            }
            if st.border_w[1] > 0.0 {
                if let Some(c) = g.at_mut(b.x + b.w - 1, y) {
                    c.cp = 0x2502;
                    c.fg = fg;
                }
            }
        }
        let mut set = |x: i32, y: i32, cp: u32| {
            if let Some(c) = g.at_mut(x, y) {
                c.cp = cp;
            }
        };
        if st.border_w[0] > 0.0 && st.border_w[3] > 0.0 {
            set(b.x, b.y, 0x250C);
        }
        if st.border_w[0] > 0.0 && st.border_w[1] > 0.0 {
            set(b.x + b.w - 1, b.y, 0x2510);
        }
        if st.border_w[2] > 0.0 && st.border_w[3] > 0.0 {
            set(b.x, b.y + b.h - 1, 0x2514);
        }
        if st.border_w[2] > 0.0 && st.border_w[1] > 0.0 {
            set(b.x + b.w - 1, b.y + b.h - 1, 0x2518);
        }
    }
    true
}

fn paint_box(g: &mut Grid, dom: &crate::dom::Dom, styles: &[Option<Style>], b: &BoxNode) {
    match b.kind {
        BoxKind::Line => {
            for s in &b.segs {
                draw_text(g, s.x, b.y, &s.text, Some(&s.st));
            }
        }
        BoxKind::Block => {
            if paint_shell(g, dom, styles, b) {
                for c in &b.children {
                    paint_box(g, dom, styles, c);
                }
            }
        }
    }
}

fn grid_max_walk(b: &BoxNode, mx: &mut i32, my: &mut i32) {
    if b.kind == BoxKind::Line {
        for s in &b.segs {
            if s.x + s.w > *mx {
                *mx = s.x + s.w;
            }
        }
        if b.y + b.h > *my {
            *my = b.y + b.h;
        }
        return;
    }
    if b.x + b.w > *mx {
        *mx = b.x + b.w;
    }
    if b.y + b.h > *my {
        *my = b.y + b.h;
    }
    for c in &b.children {
        grid_max_walk(c, mx, my);
    }
}

/// ボックスツリーからセルグリッドを構築。C の `if_render_emit_rows_sweep` 相当の
/// **CLI 行スイープ経路**に一致させる（ゴールデンテストが固定する出力規約）。
///
/// 幅は `lay.width` に固定し、はみ出す seg（右寄せ + ハード分割の `line_w` 残存値に
/// 起因するオーバーフロー）は右端でクリップする。C の `if_render_grid`（全グリッド
/// 経路）は `grid_max_walk` で幅を拡張するが、CLI/golden が使う行スイープ経路は
/// `lay->width` で打ち切るため、こちらを正とする（拡張経路は観測不変の別実装）。
pub fn render_grid(dom: &crate::dom::Dom, styles: &[Option<Style>], lay: &Layout) -> Grid {
    let mx = lay.width.max(1);
    let my = lay.height.max(1);
    let mut g = Grid {
        w: mx,
        h: my,
        cells: vec![Cell::default(); (mx * my) as usize],
    };
    paint_box(&mut g, dom, styles, &lay.root);
    g
}

/// 文書行列 extent（C の `if_render_extent` 相当）。
pub fn render_extent(lay: &Layout) -> (i32, i32) {
    let mut x = lay.width;
    let mut y = lay.height;
    grid_max_walk(&lay.root, &mut x, &mut y);
    if x < 1 {
        x = 1;
    }
    if y < 1 {
        y = 1;
    }
    (x, y)
}

/// グリッドを発行バイト列へ。ansi=1 で 256 色 SGR、0 でプレーン。C の `if_render_emit`。
pub fn render_emit(g: &Grid, ansi: bool) -> Vec<u8> {
    let mut out = Vec::new();
    let mut cur = (CELL_DEFAULT, CELL_DEFAULT, 0u8); // fg, bg, flags

    for y in 0..g.h {
        let mut last = g.w - 1;
        if !ansi {
            while last >= 0 && g.at(last, y).is_some_and(|c| c.cp == b' ' as u32) {
                last -= 1;
            }
        }
        for x in 0..=last {
            let c = g.at(x, y).unwrap();
            if c.cp == 0 {
                continue; // 全角 2 セル目
            }
            if ansi {
                let p = (c.fg, c.bg, c.flags);
                if p != cur {
                    // reset → bold → italic → uline → strike → fg → bg
                    out.extend_from_slice(b"\x1b[0m");
                    if p.2 & F_BOLD != 0 {
                        out.extend_from_slice(b"\x1b[1m");
                    }
                    if p.2 & F_ITALIC != 0 {
                        out.extend_from_slice(b"\x1b[3m");
                    }
                    if p.2 & F_ULINE != 0 {
                        out.extend_from_slice(b"\x1b[4m");
                    }
                    if p.2 & F_STRIKE != 0 {
                        out.extend_from_slice(b"\x1b[9m");
                    }
                    if p.0 != CELL_DEFAULT {
                        out.extend_from_slice(b"\x1b[38;5;");
                        out.extend_from_slice(p.0.to_string().as_bytes());
                        out.push(b'm');
                    }
                    if p.1 != CELL_DEFAULT {
                        out.extend_from_slice(b"\x1b[48;5;");
                        out.extend_from_slice(p.1.to_string().as_bytes());
                        out.push(b'm');
                    }
                    cur = p;
                }
            }
            let cp = if c.cp != 0 { c.cp } else { b' ' as u32 };
            let mut enc = [0u8; 4];
            let n = crate::utf8::encode(cp, &mut enc);
            out.extend_from_slice(&enc[..n]);
        }
        // 行末リセット無条件
        if ansi {
            out.extend_from_slice(b"\x1b[0m");
            cur = (CELL_DEFAULT, CELL_DEFAULT, 0);
        }
        out.push(b'\n');
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::apply_styles;
    use crate::html_tree::parse_html;
    use crate::layout::layout_build;

    fn render(html: &str, width: i32, ansi: bool) -> Vec<u8> {
        let dom = parse_html(html.as_bytes());
        let styles = apply_styles(&dom);
        let lay = layout_build(&dom, &styles, width);
        let g = render_grid(&dom, &styles, &lay);
        render_emit(&g, ansi)
    }

    #[test]
    fn rgba_to_ansi_grayscale() {
        assert_eq!(rgba_to_ansi(0xFFFFFFFF), 15); // 白
        assert_eq!(rgba_to_ansi(0x000000FF), 16); // 黒
        assert_eq!(rgba_to_ansi(0xFF0000FF), 196); // 赤
        assert_eq!(rgba_to_ansi(0x0000FF7F), CELL_DEFAULT); // 半透明青（alpha<128）
        assert_eq!(rgba_to_ansi(0x0000FFFF), 21); // 青
    }

    #[test]
    fn golden_doc() {
        // tests/golden/doc.html の --no-ansi --width 40 出力
        let html = "<!doctype html>\n<title>Golden</title>\n<style>\nh2 { color: #00c; }\n.tag { background-color: #def; }\n.right { text-align: right; }\n</style>\n<h2>G &amp; T</h2>\n<p>alpha beta <b>gamma</b> delta <span class=\"tag\">marked</span></p>\n<hr>\n<pre>x  y\n  z</pre>\n<p class=\"right\">R</p>\n";
        let out = render(html, 40, false);
        let s = String::from_utf8(out).unwrap();
        assert!(s.contains("G & T"), "got: {s}");
        assert!(s.contains("alpha beta gamma delta marked"), "got: {s}");
        assert!(s.contains("──────────────────────────────────────"), "got: {s}");
        assert!(s.contains("x  y\n"), "got: {s}");
    }

    #[test]
    fn simple_text() {
        // body margin 8px=1 セル → 先頭に空白 1 個。p margin-top 1em=1 行。
        let out = render("<p>hello</p>", 40, false);
        let s = String::from_utf8(out).unwrap();
        assert_eq!(s, "\n\n hello\n", "got: {s:?}");
    }

    #[test]
    fn ansi_output_has_sgr() {
        let out = render("<p>hi</p>", 40, true);
        let s = String::from_utf8(out).unwrap();
        assert!(s.contains("\x1b[0m"), "got: {s:?}");
    }

    #[test]
    fn hline_and_extent() {
        let dom = parse_html(b"<hr>");
        let styles = apply_styles(&dom);
        let lay = layout_build(&dom, &styles, 40);
        let g = render_grid(&dom, &styles, &lay);
        // hr 行は ─ で埋まる（body ml=1 なので x=1 から）
        let out = render_emit(&g, false);
        let s = String::from_utf8(out).unwrap();
        assert!(s.trim().starts_with("────────────────"), "got: {s:?}");
        let (mx, my) = render_extent(&lay);
        assert!(mx >= 40 && my >= 1);
    }
}
