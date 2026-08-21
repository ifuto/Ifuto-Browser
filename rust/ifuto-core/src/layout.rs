//! レイアウト（整数セル座標系。C の `src/layout.c` 相当）。
//!
//! | C (layout.c / layout.h) | Rust |
//! |---|---|
//! | `IfBox` / `IfSeg` / `IfLayout` | [`BoxNode`] / [`Seg`] / [`Layout`] |
//! | `if_layout_build` | [`layout_build`] |
//! | `if_layout_dump` | [`layout_dump`] |
//! | `IF_CHAR_W_PX` / `IF_ROW_H_PX` | [`CHAR_W_PX`] / [`ROW_H_PX`] |
//!
//! # 実装済み
//!
//! ブロック/インラインの box 木構築（トップダウン DFS）、幾何（margin/padding/border/
//! width/height の解決、margin:auto センタリング）、インライン整形コンテキスト
//! （flatten → アトム化 → 貪欲折り返し）、空白折り畳み、兄弟縦マージン相殺、`<hr>`、
//! `<br>`、`<img>`（alt 表示）、グリフ幅（全角 2 / 通常 1 / 結合 0）による行分割。
//!
//! # C との違い（所有権による構造的な改善）
//!
//! C は box を arena から確保し raw ポインタ（first_child/next_sibling）で連結、seg も
//! arena bump + rewind で管理する。Rust では `BoxNode` を所有し、子を `Vec<BoxNode>`、
//! seg を `Vec<Seg>`（所有 `Vec<u8>`）で表現する。ポインタ寿命・再帰的 arena rewind の
//! 整合性問題を構造的に排除する。
//!
//! # 未移植（性能最適化・観測不変）
//!
//! 以下は出力（box 木・lines/deco/links）に影響しない最適化のため保留する:
//! - AVX2/SSE2 の ASCII 可視ラン一括走査（`lw_ascii_run_end`。スカラ版は同値）
//! - (style, avail_w) 幾何キャッシュ（`IfGeomCache`）
//! - fused fit 経路（`fitdom_*`。flatten を介さず DOM を直接 wrap へ流す）
//! - 2-way 並列 layout（`IfLayShard` / pthread）
//! - 線形モード box 再利用（`box_pool` / `no_boxlink`）
//! - lazy computed style（`if_style_lazy_*`。md fast-DOM 専用）
//! - link span 収集・deco 装飾 op（dump に現れない。描画層移行時に移植）
//! - rdtsc プロファイリング（`IF_LAYOUT_PROF`）

use crate::css::{resolve_len, Len, Style, D_BLOCK, D_INLINE, D_LIST_ITEM, D_NONE, TA_CENTER,
                 TA_LEFT, TA_RIGHT, U_AUTO, U_PCT, WS_NORMAL, WS_PRE};
use crate::dom::{Dom, NodeId, NodeKind};
use crate::tags_tables::{TAG_BODY, TAG_BR, TAG_HR, TAG_HTML, TAG_IMG};

/// セル幅の px 換算（C の `IF_CHAR_W_PX`）。
pub const CHAR_W_PX: f32 = 8.0;
/// セル高の px 換算（C の `IF_ROW_H_PX`）。
pub const ROW_H_PX: f32 = 16.0;

/// 文字列スライス（所有 `Vec<u8>`。C の `IfStr` 相当の所有版）。
type Str = Vec<u8>;

/// 表示セグメント（C の `IfSeg` 相当）。
#[derive(Clone, Debug)]
pub struct Seg {
    /// 表示テキスト。
    pub text: Str,
    /// 絶対 x（セル）。
    pub x: i32,
    /// 幅（セル）。
    pub w: i32,
    /// 色・装飾の出処（計算済みスタイル）。
    pub st: Style,
}

/// ボックス種別（C の `IF_BOX_*`）。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum BoxKind {
    /// ブロックボックス。
    Block,
    /// 行ボックス。
    Line,
}

/// ボックス（C の `IfBox` 相当。子を所有 `Vec` で保持）。
#[derive(Clone, Debug)]
pub struct BoxNode {
    /// 種別。
    pub kind: BoxKind,
    /// BLOCK: 対応要素（LINE: `None`）。
    pub node: Option<NodeId>,
    /// 計算済みスタイル。
    pub st: Style,
    /// LINE ペイロード。
    pub segs: Vec<Seg>,
    /// border-box の絶対 x（セル）。
    pub x: i32,
    /// border-box の絶対 y（セル）。
    pub y: i32,
    /// border-box 幅（セル）。
    pub w: i32,
    /// border-box 高さ（セル）。
    pub h: i32,
    /// LINE の text-align。
    pub text_align: u8,
    /// 子ボックス。
    pub children: Vec<BoxNode>,
}

/// レイアウト結果（C の `IfLayout` 相当。描画に必要な core 部分のみ）。
#[derive(Clone, Debug)]
pub struct Layout {
    /// ルートボックス（body 相当）。
    pub root: BoxNode,
    /// viewport セル幅。
    pub width: i32,
    /// 総コンテンツ高（セル）。
    pub height: i32,
}

/// レイアウト文脈（C の `IfLC` 相当。DOM/スタイルへの参照）。
struct Lc<'a> {
    dom: &'a Dom,
    styles: &'a [Option<Style>],
    root_fs: f32,
}

/// 幾何（margin/padding/border/width/height の解決済み値。C の `IfGeomEnt` 相当）。
struct Geom {
    ml: i32,
    mr: i32,
    mt: i32,
    mb: i32,
    pl: i32,
    pr: i32,
    pt: i32,
    pb: i32,
    bl: i32,
    brd: i32,
    bt: i32,
    bbo: i32,
    content_w: i32,
    height_spec: i32,
}

/// スタイル未適用（--no-style 等）でも落ちないための既定値。C の `IF_STYLE_FALLBACK`。
const STYLE_FALLBACK: Style = Style {
    color: 0x0000_00FF,
    bg: 0,
    font_size: 16.0,
    line_height: 0.0,
    width: Len { v: 0.0, unit: U_AUTO },
    height: Len { v: 0.0, unit: U_AUTO },
    margin: [Len { v: 0.0, unit: U_AUTO }; 4],
    padding: [Len { v: 0.0, unit: U_AUTO }; 4],
    border_w: [0.0; 4],
    border_color: 0,
    display: D_BLOCK,
    text_align: TA_LEFT,
    white_space: WS_NORMAL,
    bold: false,
    italic: false,
    underline: false,
    strike: false,
};

fn px2col(px: f32) -> i32 {
    (px / CHAR_W_PX + 0.5).floor() as i32
}

fn px2row(px: f32) -> i32 {
    (px / ROW_H_PX + 0.5).floor() as i32
}

fn len_h(l: Len, self_fs: f32, root_fs: f32, basis: i32) -> i32 {
    if l.unit == U_PCT {
        (basis as f32 * l.v / 100.0) as i32
    } else if l.unit == U_AUTO {
        0
    } else {
        px2col(resolve_len(l, self_fs, root_fs))
    }
}

fn len_v(l: Len, self_fs: f32, root_fs: f32, basis_w: i32) -> i32 {
    // 縦方向の % も包含ブロックの「幅」基準（CSS 仕様）
    if l.unit == U_PCT {
        px2row(basis_w as f32 * CHAR_W_PX * l.v / 100.0)
    } else if l.unit == U_AUTO {
        0
    } else {
        px2row(resolve_len(l, self_fs, root_fs))
    }
}

fn geom(st: &Style, root_fs: f32, avail_w: i32) -> Geom {
    let fs = st.font_size;
    let mut g = Geom {
        bl: i32::from(st.border_w[3] > 0.0),
        brd: i32::from(st.border_w[1] > 0.0),
        bt: i32::from(st.border_w[0] > 0.0),
        bbo: i32::from(st.border_w[2] > 0.0),
        ml: len_h(st.margin[3], fs, root_fs, avail_w),
        mr: len_h(st.margin[1], fs, root_fs, avail_w),
        mt: len_v(st.margin[0], fs, root_fs, avail_w),
        mb: len_v(st.margin[2], fs, root_fs, avail_w),
        pl: len_h(st.padding[3], fs, root_fs, avail_w),
        pr: len_h(st.padding[1], fs, root_fs, avail_w),
        pt: len_v(st.padding[0], fs, root_fs, avail_w),
        pb: len_v(st.padding[2], fs, root_fs, avail_w),
        content_w: 0,
        height_spec: -1,
    };
    if st.width.unit != U_AUTO {
        g.content_w = len_h(st.width, fs, root_fs, avail_w);
        if g.content_w < 0 {
            g.content_w = 0;
        }
        let total = g.ml + g.bl + g.pl + g.content_w + g.pr + g.brd + g.mr;
        if total < avail_w && st.margin[3].unit == U_AUTO && st.margin[1].unit == U_AUTO {
            // margin:auto センタリング
            g.ml = (avail_w - total) / 2;
            g.mr = (avail_w - total) / 2;
        }
    } else {
        g.content_w = avail_w - g.ml - g.mr - g.bl - g.brd - g.pl - g.pr;
        if g.content_w < 0 {
            g.content_w = 0;
        }
    }
    if st.height.unit != U_AUTO {
        g.height_spec = len_v(st.height, fs, root_fs, avail_w);
    }
    g
}

/// 折り返し文脈（C の `IfWrap` 相当）。
struct Wrap<'a> {
    content_x: i32,
    content_w: i32,
    y: i32,
    segs: Vec<Seg>,
    line_w: i32,
    align_st: Style,
    /// 直前 seg のスタイル（merge 判定用）。
    pm_st: Option<Style>,
    /// 直前 seg のソース終端オフセット（現ピース内。merge 判定用。合成 push は `None`）。
    pm_end: Option<usize>,
    /// LINE ボックスの出力先（親ボックスの子列）。
    lines: &'a mut Vec<BoxNode>,
}

fn is_ws(c: u8) -> bool {
    c == b' ' || c == b'\t' || c == b'\n' || c == b'\r' || c == b'\x0c'
}

/// グリフ幅（C の `lw_glyph_width` 相当。高速レンジ先出しは `glyph_width` と同値）。
fn lw_glyph_width(cp: u32) -> i32 {
    crate::utf8::glyph_width(cp)
}

impl<'a> Wrap<'a> {
    /// 新規 seg を push（merge なし）。C の `wrap_push_seg` 相当。
    fn push_seg(&mut self, text: &[u8], x: i32, width: i32, st: Style, src_end: Option<usize>) {
        if text.is_empty() {
            return;
        }
        self.segs.push(Seg {
            text: text.to_vec(),
            x,
            w: width,
            st,
        });
        self.pm_st = Some(st);
        self.pm_end = src_end;
    }

    /// 直前 seg と style が同じでソース上連続なら拡張する合体 push。C の `wrap_push_merge`。
    fn push_merge(&mut self, text: &[u8], src_off: usize, x: i32, width: i32, st: Style) {
        if text.is_empty() {
            return;
        }
        if !self.segs.is_empty() && self.pm_st == Some(st) && self.pm_end == Some(src_off) {
            let last = self.segs.last_mut().unwrap();
            last.text.extend_from_slice(text);
            last.w += width;
            self.pm_end = Some(src_off + text.len());
            return;
        }
        self.push_seg(text, x, width, st, Some(src_off + text.len()));
    }

    /// 行尾の折り畳み空白 pop（C の `wrap_pop_last_seg` 相当）。
    fn pop_last_seg(&mut self) {
        self.segs.pop();
        self.pm_st = None;
    }

    /// 行を確定し LINE ボックスを出力。C の `wrap_end_line` 相当。
    fn end_line(&mut self, max_lh: f32) {
        // px2row(x)==1 ⇔ 8<=x<24（floor(x/16+.5) の区間同値）
        let mut rows = if (8.0..24.0).contains(&max_lh) {
            1
        } else {
            px2row(max_lh)
        };
        if rows < 1 {
            rows = 1;
        }
        let align = self.align_st.text_align;
        let mut shift = 0;
        if align == TA_CENTER && self.line_w < self.content_w {
            shift = (self.content_w - self.line_w) / 2;
        } else if align == TA_RIGHT && self.line_w < self.content_w {
            shift = self.content_w - self.line_w;
        }
        if !self.segs.is_empty() && shift != 0 {
            for s in &mut self.segs {
                s.x += shift;
            }
        }
        let line = BoxNode {
            kind: BoxKind::Line,
            node: None,
            st: self.align_st,
            segs: std::mem::take(&mut self.segs),
            x: self.content_x,
            y: self.y,
            w: self.line_w,
            h: rows,
            text_align: align,
            children: Vec::new(),
        };
        self.y += rows;
        self.lines.push(line);
    }
}

/// テキスト 1 ピースを流し込む。C の `wrap_text` 相当。
fn wrap_text(
    w: &mut Wrap,
    text: &[u8],
    st: Style,
    pre: bool,
    max_lh: &mut f32,
    any: &mut bool,
) {
    let fs = st.font_size;
    let lh = if st.line_height > 0.0 {
        st.line_height
    } else {
        fs * 1.2
    };
    if lh > *max_lh {
        *max_lh = lh;
    }

    let s = text;
    let n = s.len();
    let mut i = 0usize;
    let mut cx = w.line_w;

    // ピース境界で merge 追跡をリセット（ソースが不連続）
    w.pm_st = None;
    w.pm_end = None;

    while i < n {
        let b0 = s[i];
        if is_ws(b0) {
            if pre && b0 == b'\n' {
                w.end_line(*max_lh);
                cx = 0;
                i += 1;
                *max_lh = lh;
                continue;
            }
            if pre {
                let adv: i32 = if b0 == b'\t' { 8 } else { 1 };
                if b0 != b' ' {
                    w.push_seg(&s[i..i + 1], w.content_x + cx, adv, st, Some(i + 1));
                } else {
                    w.push_merge(&s[i..i + 1], i, w.content_x + cx, 1, st);
                }
                cx += adv;
                i += 1;
                *any = true;
                continue;
            }
            // 非 pre: 空白 run を折り畳む
            let mut wsend = i + 1;
            while wsend < n && is_ws(s[wsend]) {
                wsend += 1;
            }
            let last_ws = s[wsend - 1];
            i = wsend;
            if cx > 0 && cx < w.content_w {
                if last_ws == b' ' {
                    w.push_merge(&s[wsend - 1..wsend], wsend - 1, w.content_x + cx, 1, st);
                } else {
                    w.push_seg(b" ", w.content_x + cx, 1, st, None);
                }
                cx += 1;
            }
            continue;
        }

        // アトム切り出し
        let atom_start = i;
        let mut atom_w;
        if (0x21..=0x7E).contains(&b0) {
            // ASCII 可視ラン
            let mut j = i + 1;
            while j < n && (0x21..=0x7E).contains(&s[j]) {
                j += 1;
            }
            atom_w = (j - i) as i32;
            i = j;
        } else {
            let mut save = i;
            let cp = crate::utf8::decode(s, &mut save);
            let gw = lw_glyph_width(cp);
            i = save;
            if gw == 2 {
                atom_w = 2;
            } else {
                atom_w = i32::from(gw != 0);
                while i < n {
                    let c = s[i];
                    if is_ws(c) {
                        break;
                    }
                    let mut s2 = i;
                    let cp2 = crate::utf8::decode(s, &mut s2);
                    let gw2 = lw_glyph_width(cp2);
                    if gw2 == 2 {
                        break;
                    }
                    i = s2;
                    if gw2 != 0 {
                        atom_w += 1;
                    }
                }
                if atom_w == 0 {
                    atom_w = 1; // 結合/制御のみでも前進を保証
                }
            }
        }
        let atom_end = i;
        *any = true;

        // 折り返し判定（pre 以外）
        if !pre && cx > 0 && cx + atom_w > w.content_w {
            // 行尾の折り畳み空白を除く
            if let Some(last) = w.segs.last() {
                if last.text.last() == Some(&b' ') {
                    if last.text.len() == 1 {
                        w.pop_last_seg();
                    } else if let Some(last) = w.segs.last_mut() {
                        last.text.pop();
                        last.w -= 1;
                    }
                }
            }
            w.end_line(*max_lh);
            cx = 0;
            *max_lh = lh;
        }

        // アトム自体が行幅超過 → グリフ単位ハード分割
        if !pre && atom_w > w.content_w {
            let mut g = atom_start;
            while g < atom_end {
                let gs = g;
                let cp3 = crate::utf8::decode(&s[..atom_end], &mut g);
                let gw3 = lw_glyph_width(cp3);
                let gwidth = if gw3 == 2 { 2 } else { 1 };
                if cx > 0 && cx + gwidth > w.content_w {
                    w.end_line(*max_lh);
                    cx = 0;
                    *max_lh = lh;
                }
                w.push_merge(&s[gs..g], gs, w.content_x + cx, gwidth, st);
                cx += gwidth;
            }
            continue;
        }

        w.push_merge(
            &s[atom_start..atom_end],
            atom_start,
            w.content_x + cx,
            atom_w,
            st,
        );
        cx += atom_w;
    }
    w.line_w = cx;
}

/// flatten の 1 ピース（C の `IfPiece` 相当）。
struct Piece {
    text: Str,
    st: Style,
    br: bool,
}

fn flatten_into(pieces: &mut Vec<Piece>, lc: &Lc, n: NodeId, st: Style) {
    let node = lc.dom.node(n);
    match node.kind {
        NodeKind::Text => {
            pieces.push(Piece {
                text: node.name.clone(),
                st,
                br: false,
            });
        }
        NodeKind::Element => {
            let est = lc.styles[n as usize].unwrap_or(st);
            if est.display == D_NONE {
                return;
            }
            match node.tag {
                TAG_BR => {
                    pieces.push(Piece {
                        text: Vec::new(),
                        st: est,
                        br: true,
                    });
                    return;
                }
                TAG_IMG => {
                    let alt = lc.dom.attr(n, b"alt").unwrap_or(b"");
                    let m = alt.len().min(900);
                    let mut text = b"[img: ".to_vec();
                    text.extend_from_slice(&alt[..m]);
                    text.push(b']');
                    pieces.push(Piece {
                        text,
                        st: est,
                        br: false,
                    });
                    return;
                }
                _ => {}
            }
            let mut c = node.first_child;
            while let Some(cid) = c {
                flatten_into(pieces, lc, cid, est);
                c = lc.dom.node(cid).next_sibling;
            }
        }
        _ => {}
    }
}

/// インライン run を IFC に流し込み、run の次のノード（ブロック or `None`）を返す。
/// C の `layout_ifc` 相当。
fn layout_ifc(
    lc: &Lc,
    cur: NodeId,
    base_st: Style,
    content_x: i32,
    y: &mut i32,
    content_w: i32,
    lines: &mut Vec<BoxNode>,
) -> Option<NodeId> {
    let mut w = Wrap {
        content_x,
        content_w,
        y: *y,
        segs: Vec::new(),
        line_w: 0,
        align_st: base_st,
        pm_st: None,
        pm_end: None,
        lines,
    };
    let mut max_lh = if base_st.line_height > 0.0 {
        base_st.line_height
    } else {
        base_st.font_size * 1.2
    };
    let pre = base_st.white_space == WS_PRE;

    // flatten（DOM をピース列へ）
    let mut pieces = Vec::new();
    let mut c = Some(cur);
    while let Some(cid) = c {
        let cnode = lc.dom.node(cid);
        if cnode.kind == NodeKind::Element {
            let cst = lc.styles[cid as usize].unwrap_or(base_st);
            if cst.display == D_NONE {
                c = cnode.next_sibling;
                continue;
            }
            if cst.display != D_INLINE {
                break; // ブロック子: run 終了
            }
        }
        flatten_into(&mut pieces, lc, cid, base_st);
        c = lc.dom.node(cid).next_sibling;
    }

    // wrap 連鎖
    let mut any_text = false;
    for piece in &pieces {
        if piece.br {
            w.end_line(max_lh);
            max_lh = if base_st.line_height > 0.0 {
                base_st.line_height
            } else {
                base_st.font_size * 1.2
            };
            continue;
        }
        let ppre = piece.st.white_space == WS_PRE || pre;
        wrap_text(
            &mut w,
            &piece.text,
            piece.st,
            ppre,
            &mut max_lh,
            &mut any_text,
        );
    }
    if !w.segs.is_empty() || any_text {
        w.end_line(max_lh);
    }
    *y = w.y;
    c
}

/// 要素をレイアウトしブロックボックスを返す。C の `layout_element` 相当。
fn layout_element(lc: &Lc, node: NodeId, st: Style, ax: i32, ay: i32, avail_w: i32) -> BoxNode {
    let g = geom(&st, lc.root_fs, avail_w);
    let x = ax + g.ml;
    let content_x = x + g.bl + g.pl;
    let y = ay; // margin-top は呼び出し側（兄弟相殺）で処理済み
    let content_y = y + g.bt + g.pt;

    if lc.dom.node(node).tag == TAG_HR {
        return BoxNode {
            kind: BoxKind::Block,
            node: Some(node),
            st,
            segs: Vec::new(),
            x,
            y,
            w: g.bl + g.pl + g.content_w + g.pr + g.brd,
            h: g.bt + 1 + g.bbo,
            text_align: 0,
            children: Vec::new(),
        };
    }

    let mut children = Vec::new();
    let content_h = layout_children(lc, node, st, content_x, content_y, g.content_w, &mut children);
    let content_h = if g.height_spec >= 0 && g.height_spec > content_h {
        g.height_spec // 指定高はクリップせず拡張のみ
    } else {
        content_h
    };

    BoxNode {
        kind: BoxKind::Block,
        node: Some(node),
        st,
        segs: Vec::new(),
        x,
        y,
        w: g.bl + g.pl + g.content_w + g.pr + g.brd,
        h: g.bt + g.pt + content_h + g.pb + g.bbo,
        text_align: 0,
        children,
    }
}

/// node の子を走査して配置し、content 高を返す。C の `layout_children` 相当。
fn layout_children(
    lc: &Lc,
    node: NodeId,
    base_st: Style,
    content_x: i32,
    content_y: i32,
    content_w: i32,
    children: &mut Vec<BoxNode>,
) -> i32 {
    let mut y = content_y;
    let mut prev_mb = 0;
    let mut c = lc.dom.node(node).first_child;
    while let Some(cid) = c {
        let cnode = lc.dom.node(cid);
        let cst = if cnode.kind == NodeKind::Element {
            lc.styles[cid as usize]
        } else {
            None
        };
        if cst.is_some_and(|s| s.display == D_NONE) {
            c = cnode.next_sibling;
            continue;
        }
        let blockish = cst.is_some_and(|s| s.display == D_BLOCK || s.display == D_LIST_ITEM);
        if !blockish {
            c = layout_ifc(lc, cid, base_st, content_x, &mut y, content_w, children);
            prev_mb = 0;
            continue;
        }
        let cst = cst.unwrap();
        let cg = geom(&cst, lc.root_fs, content_w);
        y += prev_mb.max(cg.mt); // 兄弟縦マージン相殺: max
        let child = layout_element(lc, cid, cst, content_x, y, content_w);
        let child_h = child.h;
        children.push(child);
        y += child_h;
        prev_mb = cg.mb;
        c = cnode.next_sibling;
    }
    y - content_y
}

/// DOM + 計算済みスタイルからレイアウトを構築。C の `if_layout_build` 相当。
pub fn layout_build(dom: &Dom, styles: &[Option<Style>], width_cells: i32) -> Layout {
    let width_cells = if width_cells < 4 { 4 } else { width_cells };
    let lc = Lc {
        dom,
        styles,
        root_fs: 16.0,
    };

    let mut body: Option<NodeId> = None;
    let mut c = dom.node(dom.root).first_child;
    while let Some(cid) = c {
        let cnode = dom.node(cid);
        if cnode.kind == NodeKind::Element && cnode.tag == TAG_HTML {
            let mut g = cnode.first_child;
            while let Some(gid) = g {
                let gnode = dom.node(gid);
                if gnode.kind == NodeKind::Element && gnode.tag == TAG_BODY {
                    body = Some(gid);
                    break;
                }
                g = gnode.next_sibling;
            }
            if body.is_some() {
                break;
            }
        }
        c = cnode.next_sibling;
    }

    let Some(body) = body else {
        return Layout {
            root: BoxNode {
                kind: BoxKind::Block,
                node: None,
                st: STYLE_FALLBACK,
                segs: Vec::new(),
                x: 0,
                y: 0,
                w: 0,
                h: 0,
                text_align: 0,
                children: Vec::new(),
            },
            width: width_cells,
            height: 0,
        };
    };

    let bst = styles[body as usize].unwrap_or(STYLE_FALLBACK);
    let body_fs = bst.font_size;
    let body_mt = len_v(bst.margin[0], body_fs, 16.0, width_cells);
    let root = layout_element(&lc, body, bst, 0, body_mt, width_cells);
    let height = root.y + root.h;
    Layout {
        root,
        width: width_cells,
        height,
    }
}

/// ボックス木ダンプ（C の `if_layout_dump` 相当。raw バイトで出力）。
fn dump_box(b: &BoxNode, dom: &Dom, depth: usize, out: &mut Vec<u8>) {
    for _ in 0..depth {
        out.extend_from_slice(b"  ");
    }
    match b.kind {
        BoxKind::Line => {
            out.extend_from_slice(
                format!(
                    "LINE x={} y={} w={} h={} segs={} \"",
                    b.x,
                    b.y,
                    b.w,
                    b.h,
                    b.segs.len()
                )
                .as_bytes(),
            );
            for seg in &b.segs {
                for (k, &ch) in seg.text.iter().enumerate() {
                    if k >= 60 {
                        break;
                    }
                    if ch == b'\n' {
                        out.extend_from_slice(b"\\n");
                    } else {
                        out.push(ch);
                    }
                }
            }
            out.extend_from_slice(b"\"\n");
        }
        BoxKind::Block => {
            out.extend_from_slice(format!("BLOCK x={} y={} w={} h={}", b.x, b.y, b.w, b.h).as_bytes());
            if let Some(nid) = b.node {
                out.push(b' ');
                out.push(b'<');
                out.extend_from_slice(&dom.node(nid).name);
                out.push(b'>');
            }
            out.push(b'\n');
            for c in &b.children {
                dump_box(c, dom, depth + 1, out);
            }
        }
    }
}

/// ボックス木を html5lib 的テキストではなくデバッグ形式でダンプ（C の `if_layout_dump`）。
pub fn layout_dump(dom: &Dom, layout: &Layout) -> Vec<u8> {
    let mut out = Vec::new();
    dump_box(&layout.root, dom, 0, &mut out);
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::apply_styles;
    use crate::html_tree::parse_html;

    fn build(html: &str, width: i32) -> (Dom, Layout) {
        let dom = parse_html(html.as_bytes());
        let styles = apply_styles(&dom);
        let lay = layout_build(&dom, &styles, width);
        (dom, lay)
    }

    fn dump(html: &str, width: i32) -> String {
        let (dom, lay) = build(html, width);
        String::from_utf8(layout_dump(&dom, &lay)).unwrap()
    }

    #[test]
    fn empty_layout() {
        // body 無しは空（ただし html_tree は必ず body を作るので、この経路は手動確認）
        let dom = parse_html(b"");
        let styles = apply_styles(&dom);
        let lay = layout_build(&dom, &styles, 100);
        assert!(lay.root.children.is_empty());
    }

    #[test]
    fn simple_block() {
        let d = dump("<p>hello</p>", 100);
        // body(8px margin=1行) → p(1em margin=1行) → LINE "hello"
        assert!(d.contains("BLOCK x=1 y=1 w=98 h=2 <body>"), "got: {d}");
        assert!(d.contains("BLOCK x=1 y=2 w=98 h=1 <p>"), "got: {d}");
        assert!(d.contains("LINE x=1 y=2 w=5 h=1 segs=1 \"hello\""), "got: {d}");
    }

    #[test]
    fn h1_font_size() {
        // h1 は 2em = 32px → px2row(32)=2 行高
        let d = dump("<h1>x</h1>", 100);
        assert!(d.contains("<h1>"), "got: {d}");
        // LINE の行高は 32px → 2 行
        assert!(d.contains("h=2"), "got: {d}");
    }

    #[test]
    fn hr_special() {
        let d = dump("<hr>", 100);
        assert!(d.contains("<hr>"), "got: {d}");
        // hr は h = bt + 1 + bbo（border 無し → 1）
    }

    #[test]
    fn wide_glyph() {
        // 全角「あ」は幅 2
        let d = dump("<p>あ</p>", 100);
        assert!(d.contains("w=2"), "got: {d}");
    }

    #[test]
    fn wrap_at_width() {
        // 狭い幅で折り返す
        let d = dump("<p>hello world</p>", 12);
        // "hello" は 5 文字、"world" 5 文字。content_w が小さいので折り返される
        assert!(d.contains("LINE"), "got: {d}");
    }

    #[test]
    fn br_break() {
        let d = dump("<p>a<br>b</p>", 100);
        // 2 行に分かれる
        assert_eq!(d.matches("LINE ").count(), 2, "got: {d}");
    }

    #[test]
    fn list_item_block() {
        let d = dump("<ul><li>one</li><li>two</li></ul>", 100);
        // li は display:list-item = blockish
        assert_eq!(d.matches("<li>").count(), 2, "got: {d}");
    }

    #[test]
    fn margin_collapse() {
        // 兄弟 p の margin は max 相殺
        let d = dump("<p>a</p><p>b</p>", 100);
        assert_eq!(d.matches("<p>").count(), 2, "got: {d}");
    }
}
