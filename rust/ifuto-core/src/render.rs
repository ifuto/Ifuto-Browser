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
//! # byte-direct 発行（C の fast 経路との byte 一致）
//!
//! C の行スイープは direct 旗付き行をセルモデルを経ずに生バイトで発行する
//! （`row_emit_direct`/`row_emit_fast`/`row_emit_ansi_fast`）。wrap の検査 quirk
//! （折り返し次行に紛れた 0 幅/不正グリフは direct を殺さない）により、quirk 行の
//! 生バイト列はセル再エンコードと一致しない。本実装は paint 時に行ごとの監査帳簿
//! （[`RowInfo`]）を畳み、quirk 行のみ C の受理条件を機械的に写して raw 発行する
//! （不受理はセル経路 ≡ C slow）。quirk の無い行は raw ≡ セル再エンコードのため
//! 登録せず常にセル経路で一致する。
//!
//! # 未移植（性能最適化・観測不変）
//!
//! 以下は発行バイト列に影響しない最適化のため保留する:
//! - 窓グリッド経路（`if_render_grid_rows_into(_cur)`）と厳密増加カーソル
//! - 2-way 並列 sweep（pthread）
//! - `raster.c` の fill カーネル自動選択（全候補が bit-exact 同値。スカラ fill で十分）
//! - rdtsc プロファイリング

use crate::css::{Style, D_LIST_ITEM};
use crate::layout::{BoxKind, BoxNode, Layout};
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

/// byte-direct 1 seg（C の `IfRRun` IF_RR_BYTES 相当）。
#[derive(Clone, Debug)]
struct DirectSeg {
    /// 開始セル x。
    x: i32,
    /// 占有セル幅。
    w: i32,
    /// seg の pen（C の `seg_pen`。ansi 発行の遷移用）。
    pen: (u8, u8, u8),
    /// 生バイト列（quirk を含み得るためセル再エンコードと一致しないことがある）。
    raw: Vec<u8>,
}

/// 行 LINE ボックスの発行用ペイロード（C の fast 経路受理時の LINE 相当）。
///
/// wrap の direct 検査には C 由来の quirk（kill されなかった 0 幅/不正グリフが折り返し
/// 次行に紛れ込む）があり得るため、seg 発行は常に **raw bytes**（quirk が無ければ
/// raw ≡ セル再エンコードなので byte 不変）。受理時は seg セルのペイント済み色素が
/// 後勝ち deco で上書きされている可能性を排除し、C と同じく seg 自身の pen で出す。
#[derive(Clone, Debug, Default)]
struct DirectLine {
    /// seg 列（x 昇順）。
    segs: Vec<DirectSeg>,
    /// この LINE の paint に使われた draw_text 呼び出し数（= segs.len()）。
    n_paints: u32,
    /// wrap 時の direct 旗（C の `IF_LF_DIRECT_BYTES`。fast 受理の必須条件）。
    direct: bool,
}

/// li マーカーの発行用ペイロード（C の MARKER run 相当）。
#[derive(Clone, Debug)]
struct MarkerRun {
    /// 占有セル範囲 [x, x+w)。
    x: i32,
    w: i32,
    /// マーカー自身の pen（C の `seg_pen(d->st)`。fast 経路ではrun が最上位層）。
    pen: (u8, u8, u8),
    /// 生バイト（"• " or "N."）。
    raw: Vec<u8>,
}

/// 行ごとの paint 内訳（fast 経路採否の監査帳簿。C の sweep active deco 台帳相当）。
#[derive(Clone, Debug, Default)]
struct RowInfo {
    /// `draw_text` 呼び出し数（li マーカー含む）。
    text_paints: u32,
    /// この行に乗った LINE ボックス数（C の `peek1.y == r` 堕落判定相当）。
    lines_on_row: u32,
    /// この行を覆う BG 塗り数（C の ansi `n_bg > 32` 判定相当）。
    bg_n: u32,
    /// BORDER 等の未対応 cp 装飾が入った（採用不可。HLINE/BG は含めない）。
    other_paint: bool,
    /// li マーカーの run 列（登録順 = C の deco 追記順）。
    markers: Vec<MarkerRun>,
    /// `<hr>` 罫線の占有セル範囲（[x0,x1)。C の HLINE deco の [x, x+w)）。
    hline_spans: Vec<(i32, i32)>,
    /// この行の LINE ペイロード（同行に 2 個目が来たら None + 毒殺旗）。
    row_line: Option<DirectLine>,
    /// 行 LINE 毒殺旗（C の「同行複数 line は堕落して二度と fast に載らない」）。
    row_line_bad: bool,
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
    /// 行監査帳簿（byte-direct 発行の採否判定用）。
    rinfo: Vec<RowInfo>,
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
        // C の ansi n_bg 計数は x 範囲の空否を見ず「deco が行を覆う」だけで畳む
        g.rinfo[y as usize].bg_n += 1;
        for x in xx0..xx1 {
            if let Some(c) = g.at_mut(x, y) {
                c.bg = idx;
            }
        }
    }
}

fn draw_text(g: &mut Grid, x: i32, y: i32, text: &[u8], st: Option<&Style>) {
    if y >= 0 && y < g.h {
        g.rinfo[y as usize].text_paints += 1;
    }
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

/// li マーカーの run ペイロードを帳簿へ（fast 経路の採否・重複検査用）。
fn mark_marker_run(g: &mut Grid, y: i32, x0: i32, w: i32, pen: (u8, u8, u8), raw: &[u8]) {
    if y >= 0 && y < g.h {
        g.rinfo[y as usize].markers.push(MarkerRun {
            x: x0,
            w,
            pen,
            raw: raw.to_vec(),
        });
    }
}

fn draw_hline(g: &mut Grid, x0: i32, x1: i32, y: i32, st: Option<&Style>) {
    if y >= 0 && y < g.h {
        g.rinfo[y as usize].hline_spans.push((x0, x1));
    }
    for x in x0..x1 {
        put_cp(g, x, y, 0x2500, st, 0); // ─
    }
}

/// li マーカー（ul: "• "、ol: "N."）。C の行スイープ経路の MARKER deco 相当。
/// マーカーは `<li>` かつ `display:list-item` のときのみ（任意の `display:list-item`
/// 要素には描かない。sweep の `c->tag == IF_TAG_LI && display == LIST_ITEM` 条件と同値）。
fn draw_marker(g: &mut Grid, dom: &crate::dom::Dom, styles: &[Option<Style>], b: &BoxNode) {
    let Some(li) = b.node else { return };
    // 直近の祖先 ul/ol を探す（ネストした li の外には出ない）
    let mut list = TAG_UL;
    let mut p = dom.node(li).parent;
    while let Some(pid) = p {
        let pnode = dom.node(pid);
        if pnode.kind == crate::dom::NodeKind::Element
            && (pnode.tag == TAG_UL || pnode.tag == TAG_OL)
        {
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
        mark_marker_run(g, b.y, mx, 2, pen(Some(&b.st)), b"\xE2\x80\xA2 ");
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
    // C の MARKER deco は w = m（"N." のみ。後続空白セルは run 範囲に含まない）
    mark_marker_run(g, b.y, mx, m, pen(Some(&b.st)), txt.as_bytes());
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
    let any = st.border_w[0] > 0.0
        || st.border_w[1] > 0.0
        || st.border_w[2] > 0.0
        || st.border_w[3] > 0.0;
    if any {
        // 行スイープ経路の deco 有効高 dh = max(h,1) に一致（左右罫線は h=0 でも 1 行）
        let eh = b.h.max(1);
        for y in b.y..b.y + eh {
            if y >= 0 && y < g.h {
                g.rinfo[y as usize].other_paint = true;
            }
        }
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
            if b.y >= 0 && b.y < g.h {
                let ri = &mut g.rinfo[b.y as usize];
                ri.lines_on_row += 1;
                // C の fast 経路受理時に必要な LINE ペイロードを無条件で帳簿へ
                // （受理の完全判定は発行時に run 列へ展開して行う）。
                let dl = DirectLine {
                    segs: b
                        .segs
                        .iter()
                        .map(|s| DirectSeg {
                            x: s.x,
                            w: s.w,
                            pen: pen(Some(&s.st)),
                            raw: s.text.clone(),
                        })
                        .collect(),
                    n_paints: b.segs.len() as u32,
                    direct: b.direct,
                };
                if ri.row_line.is_some() || ri.row_line_bad {
                    ri.row_line = None;
                    ri.row_line_bad = true; // 同行複数 LINE → C は fast を降りる
                } else {
                    ri.row_line = Some(dl);
                }
            }
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
        rinfo: vec![RowInfo::default(); my as usize],
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

/// SGR 遷移の発行（C の `pen_emit`。reset→bold→italic→uline→strike→fg→bg）。
fn emit_pen(out: &mut Vec<u8>, p: (u8, u8, u8), cur: &mut (u8, u8, u8)) {
    if p == *cur {
        return;
    }
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
    *cur = p;
}

/// fast 経路の 1 run（x 昇順ソート済みで発行）。C の `IfRRun/IfRRunA` 相当。
enum RunRef<'a> {
    /// LINE の seg（raw bytes + 自身 pen）。
    Seg(&'a DirectSeg),
    /// li マーカー（raw bytes + 自身 pen）。
    Marker(&'a MarkerRun),
    /// `<hr>` 罫線（占有セル幅。開始 x はタプル側。clip 可の唯一の run）。
    Hline(i32),
}

/// 監査帳簿から当該行が C の fast 経路（`row_emit_direct`/`row_emit_fast`/
/// `row_emit_ansi_fast`）を通るか判定し、通る場合のみ発行用 run 列（x 昇順）を返す。
/// 受理条件は C の機械的写し（違反時は None ≡ C slow セル経路との一致を保つ）:
///
/// - 同行の LINE が 0 か 1 個（C の `peek1.y == r` 堕落判定）、居れば direct 旗必須
/// - 行のテキスト paint が seg + マーカー分だけ（他の paint 源が無い帳簿自己一致）
/// - BORDER 等の未対応 deco が無い（BG/MARKER/HLINE は受理）
/// - ansi 時は BG 数 <= 32 かつ MARKER の glyph 検査（全 gw>0・正準置換・幅和==w）
/// - run 列 ≤ 64・x ソート・非重複・bytes run は右端 clip 整合（HLINE のみ clip 可）
fn fast_plan<'a>(g: &'a Grid, y: i32, ansi: bool) -> Option<Vec<(i32, RunRef<'a>)>> {
    let ri = &g.rinfo[y as usize];
    if ri.other_paint || ri.row_line_bad || ri.lines_on_row > 1 {
        return None;
    }
    if ansi && ri.bg_n > 32 {
        return None;
    }
    let line = ri.row_line.as_ref();
    match line {
        Some(dl) => {
            if !dl.direct {
                return None; // direct 旗なし LINE は C も slow へ
            }
            if ri.text_paints != dl.n_paints + ri.markers.len() as u32 {
                return None;
            }
        }
        None => {
            if ri.text_paints != ri.markers.len() as u32 {
                return None;
            }
        }
    }
    if ansi {
        for m in &ri.markers {
            let t = &m.raw;
            let mut i = 0usize;
            let mut wsum = 0i32;
            while i < t.len() {
                let from = i;
                let cp = crate::utf8::decode(t, &mut i);
                let gw = crate::utf8::glyph_width(cp);
                if gw <= 0 {
                    return None;
                }
                if cp == crate::utf8::REPLACEMENT
                    && !(i - from == 3
                        && t[from] == 0xEF
                        && t[from + 1] == 0xBF
                        && t[from + 2] == 0xBD)
                {
                    return None;
                }
                wsum += gw;
            }
            if wsum != m.w {
                return None;
            }
        }
    }
    // run 列の構成（stable 挿入ソート = C と同順）と受理検査（写し）
    let mx = g.w;
    let mut runs: Vec<(i32, RunRef<'a>)> = Vec::with_capacity(
        ri.markers.len() + ri.hline_spans.len() + line.map_or(0, |dl| dl.segs.len()),
    );
    if let Some(dl) = line {
        for s in &dl.segs {
            runs.push((s.x, RunRef::Seg(s)));
        }
    }
    for m in &ri.markers {
        runs.push((m.x, RunRef::Marker(m)));
    }
    for h in &ri.hline_spans {
        runs.push((h.0, RunRef::Hline(h.1 - h.0)));
    }
    if runs.len() > 64 {
        return None;
    }
    for a in 1..runs.len() {
        let t = runs.remove(a);
        let mut c = a;
        while c > 0 && runs[c - 1].0 > t.0 {
            c -= 1;
        }
        runs.insert(c, t);
    }
    let mut pos = 0i32;
    for (x, run) in &runs {
        if *x < pos {
            return None; // 重複はセル経路へ
        }
        if *x > pos {
            let ge = if *x > mx { mx } else { *x };
            if ge - pos < 0 {
                return None;
            }
            pos = *x;
        }
        if pos >= mx {
            break; // viewport 右端でクリップ
        }
        match run {
            RunRef::Hline(w) => {
                pos += w; // clip 後も pos は元の w で進む（C と同一規則）
            }
            RunRef::Seg(s) => {
                if pos + s.w > mx {
                    return None; // 右端を跨ぐ bytes run は再構成不能
                }
                pos += s.w;
            }
            RunRef::Marker(m) => {
                if pos + m.w > mx {
                    return None;
                }
                pos += m.w;
            }
        }
    }
    Some(runs)
}

/// 受理済み fast 行の発行。ギャップはセル（BG ピースと byte 同値）、run は自身の pen で
/// 生バイトを出す（C の runs 直行と同一バイト列）。no-ansi の行末 trim は C と同じく
/// バイト 0x20 単位で後処理する。
fn emit_fast_row(
    out: &mut Vec<u8>,
    g: &Grid,
    y: i32,
    runs: &[(i32, RunRef)],
    ansi: bool,
    cur: &mut (u8, u8, u8),
) {
    let row_mark = out.len();
    let mx = g.w;
    let mut pos = 0i32;
    let put_gap = |out: &mut Vec<u8>, from: i32, to: i32, cur: &mut (u8, u8, u8)| {
        let mut cx = from;
        while cx < to {
            let c = g.at(cx, y).unwrap();
            if c.cp != 0 {
                if ansi {
                    emit_pen(out, (c.fg, c.bg, c.flags), cur);
                }
                let mut enc = [0u8; 4];
                let n = crate::utf8::encode(c.cp, &mut enc);
                out.extend_from_slice(&enc[..n]);
            }
            cx += 1;
        }
    };
    for (x, run) in runs {
        let ge = if *x > mx { mx } else { *x };
        if ge > pos {
            put_gap(out, pos, ge, cur);
        }
        pos = *x;
        if pos >= mx {
            break;
        }
        match run {
            RunRef::Seg(s) => {
                if ansi {
                    emit_pen(out, s.pen, cur);
                }
                out.extend_from_slice(&s.raw);
                pos += s.w;
            }
            RunRef::Marker(m) => {
                if ansi {
                    emit_pen(out, m.pen, cur);
                }
                out.extend_from_slice(&m.raw);
                pos += m.w;
            }
            RunRef::Hline(w0) => {
                let mut w2 = *w0;
                if pos + w2 > mx {
                    w2 = mx - pos; // セル単位 clip は HLINE だけ許容
                }
                if w2 > 0 {
                    // w<=0 の空ランは細胞を持たない → pen 遷移も発生しない
                    if ansi {
                        emit_pen(out, (CELL_DEFAULT, CELL_DEFAULT, 0), cur);
                    }
                    for _ in 0..w2 {
                        out.extend_from_slice(b"\xE2\x94\x80"); // ─
                    }
                }
                pos += w0;
            }
        }
    }
    if ansi {
        if mx > pos {
            put_gap(out, pos, mx, cur); // 末尾ギャップ
        }
        out.extend_from_slice(b"\x1b[0m");
        *cur = (CELL_DEFAULT, CELL_DEFAULT, 0);
    } else {
        while out.len() > row_mark && out.last() == Some(&b' ') {
            out.pop();
        }
    }
    out.push(b'\n');
}

/// グリッドを発行バイト列へ。ansi=1 で 256 色 SGR、0 でプレーン。C の `if_render_emit`
/// （行スイープ経路。byte-direct 行は raw 発行、他はセル再エンコードで C と byte 一致）。
pub fn render_emit(g: &Grid, ansi: bool) -> Vec<u8> {
    let mut out = Vec::new();
    let mut cur = (CELL_DEFAULT, CELL_DEFAULT, 0u8); // fg, bg, flags

    for y in 0..g.h {
        if let Some(runs) = fast_plan(g, y, ansi) {
            emit_fast_row(&mut out, g, y, &runs, ansi, &mut cur);
            continue;
        }
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
                emit_pen(&mut out, (c.fg, c.bg, c.flags), &mut cur);
            }
            let mut enc = [0u8; 4];
            let n = crate::utf8::encode(c.cp, &mut enc);
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
        assert!(
            s.contains("──────────────────────────────────────"),
            "got: {s}"
        );
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
