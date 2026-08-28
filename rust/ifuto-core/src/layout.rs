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
//! - link span 収集（dump に現れない。リンク出力は store 層で担保）
//! - rdtsc プロファイリング（`IF_LAYOUT_PROF`）
//!
//! 描画層移行で実装済み: 行スイープ用フラット streams（C の `lines_head` /
//! `deco[]` 相当の [`RLine`] / [`Deco`]）。seg の所有は `Layout::seg_arena` に
//! 集約し、box 木と streams が区間を共有する（C の `w->seg_base` 共有と同値）。

use crate::css::{
    resolve_len, Len, Style, D_BLOCK, D_INLINE, D_LIST_ITEM, D_NONE, TA_CENTER, TA_LEFT, TA_RIGHT,
    U_AUTO, U_PCT, WS_NORMAL, WS_PRE,
};
use crate::dom::{Dom, NodeId, NodeKind};
use crate::tags::Tag;
use crate::tags_tables::{
    TAG_A, TAG_BLOCKQUOTE, TAG_BODY, TAG_BR, TAG_HR, TAG_HTML, TAG_IMG, TAG_LI, TAG_OL,
    TAG_SECTION, TAG_TABLE, TAG_TBODY, TAG_THEAD, TAG_TR, TAG_UL,
};

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

/// 行レコード（C の `IfRLine` 相当）。seg の実体は `Layout::seg_arena` が
/// 所有し、box 木の LINE BoxNode と区間を共有する（C の `w->seg_base`
/// ポインタ共有と同値で二重所有はしない）。
#[derive(Clone, Copy, Debug)]
pub struct RLine {
    /// 行 y（セル。行首の絶対座標）。
    pub y: i32,
    /// C の `IF_LF_DIRECT_BYTES` 相当（wrap 時の quirk 畳み込み済み）。
    pub direct: bool,
    /// `seg_arena` 内の seg 区間の始端。
    pub seg_lo: u32,
    /// `seg_arena` 内の seg 区間の終端（半開 [lo, hi)）。
    pub seg_hi: u32,
}

/// 装飾 op 種別（C の `IF_DECO_*`）。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum DecoKind {
    /// 背景塗り（cp は触らず bg のみ後勝ち上書き）。
    Bg,
    /// 罫線ボーダ（辺 + 角のグリフ）。
    Border,
    /// `<hr>` 罫線（1 行の ─ ラン）。
    Hline,
    /// li マーカー（"• " or "N."）。
    Marker,
}

/// 装飾 op（C の `IfDeco` 相当）。**追記順が paint/受理順**（Box 開で追記され、
/// Box 閉で h が後埋めされる C の規約をそのまま持つ）。
#[derive(Clone, Copy, Debug)]
pub struct Deco {
    /// 種別。
    pub kind: DecoKind,
    /// 開始セル x。
    pub x: i32,
    /// 開始行 y。
    pub y: i32,
    /// 幅（セル）。
    pub w: i32,
    /// 高さ（セル。`h <= 0` は paint 時に 1 扱い = C の `dh = max(h,1)`）。
    pub h: i32,
    /// BG: 未変換の `st.bg` RGBA。BORDER: `st.border_color` RGBA。
    pub argb: u32,
    /// BORDER の辺旗（1|2|4|8 = 上右下左。C `d->tlen` の sides 写し）。
    pub sides: u8,
    // ---- MARKER 専用（C は d->st へのポインタ + d->text[12]/tlen） ----
    /// MARKER の発行 pen 原料: `st.color`（未変換 RGBA）。
    pub m_color: u32,
    /// MARKER の発行 pen 原料: `st.bg`（未変換 RGBA）。
    pub m_bg: u32,
    /// MARKER の装飾旗（bold=1|italic=2|uline=4|strike=8。render 側で pen 化）。
    pub m_flags: u8,
    /// MARKER 生バイト（"• " = 4B or "N." ≤ 11B。C `d->text[12]`）。
    pub text: [u8; 12],
    /// MARKER 生バイト長。
    pub tlen: u8,
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
    /// LINE ペイロードの seg 区間の始端（`Layout::seg_arena` 内。
    /// BLOCK では常に `0`）。
    pub seg_lo: u32,
    /// LINE ペイロードの seg 区間の終端（半開 [lo, hi)。BLOCK では常に `0`）。
    pub seg_hi: u32,
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
    /// LINE のみ: C の `IF_LF_DIRECT_BYTES` 相当。**wrap 走査のタイミングまで含めて**
    /// C と同じ規則で畳まれる（glyph スキャンは wrap 確定より先に走り、kill は
    /// スキャン時点の現行ラインにだけ効く。折り返し境界に乗った 0 幅/不正グリフは
    /// 次行では検査をすり抜ける — C の quirk で、このフラグはそのまま再現する。
    /// 発行側はこのフラグを見て raw bytes を出す）。
    pub direct: bool,
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
    /// 行スイープ用フラット行列（C の `lines_head` chunked list 相当。
    /// 生成順 = y 単調非減少。垂直フローのみの空手形）。
    pub lines: Vec<RLine>,
    /// 行スイープ用装飾 op 列（C の `deco[]` 相当。追記順 = paint 順）。
    pub deco: Vec<Deco>,
    /// 全文書の seg アリーナ（LINE BoxNode と RLine が区間で共有する唯一の所有）。
    pub seg_arena: Vec<Seg>,
}

/// レイアウト文脈（C の `IfLC` 相当。DOM/スタイルへの参照）。
struct Lc<'a> {
    dom: &'a Dom,
    styles: &'a [Option<Style>],
    root_fs: f32,
    // --- seg merge 忠実化のための C arena 追跡モデル（下記 Prov 参照） ---
    /// 合成文字列アリーナの bump 位置写し（`syn_alloc` のみが進める）。
    syn_pos: std::cell::Cell<usize>,
    /// C `pieces_scratch_cap` 写し（grow イベント検知用）。
    pieces_cap: std::cell::Cell<u32>,
    /// C `links_cap` / `n_links` 写し（collect_link の grow 検知用）。
    links_cap: std::cell::Cell<u32>,
    n_links: std::cell::Cell<u32>,
    /// C `prec_scratch_cap` 写し。
    prec_cap: std::cell::Cell<u32>,
    /// テキストノード 1 枚ごとの由来 id 発行器。
    prov_seq: std::cell::Cell<u32>,
    // --- 描画層 streams（C の lines_head / deco[] / seg arena 相当） ---
    /// フラット行列（RefCell 経由で再帰中に追記。C の生ポインタ追記と同順）。
    rlines: std::cell::RefCell<Vec<RLine>>,
    /// 装飾 op 列（同上）。
    deco: std::cell::RefCell<Vec<Deco>>,
    /// seg アリーナ（行確定時に `Wrap::segs` から移動する唯一の所有先）。
    seg_arena: std::cell::RefCell<Vec<Seg>>,
}

/// C の `deco_add` 相当。追記 index を返す（h 後埋め用）。
fn deco_push(lc: &Lc, d: Deco) -> u32 {
    let mut v = lc.deco.borrow_mut();
    v.push(d);
    (v.len() - 1) as u32
}

/// C の `lay->deco[idx].h = box->h` 相当（`u32::MAX` = 未追記は no-op）。
fn deco_patch_h(lc: &Lc, idx: u32, h: i32) {
    if idx != u32::MAX {
        lc.deco.borrow_mut()[idx as usize].h = h;
    }
}

/// seg merge の由来位置クラス（C の `wrap_push_merge` の `pm_end == p` ポインタ
/// 比較の追跡モデル）。C の merge は「同一バッファ上で直前 seg の終端ポインタ ==
/// 今回の先頭ポインタ」でのみ成立する。バッファ実体ごとの位置クラス:
///
/// - `Src(id)`: テキストノード 1 枚の文字列。同一スライス内のオフセット隣接のみ
///   成立し、ピース跨ぎは id 不一致で必ず不成立（実ポインタ比較と同値: DOM の
///   隣接テキストノードはパーサ/run accumulator が併合済みで、独立バッファの
///   ポインタが隣り合うことはあり得ない）。
/// - `Syn`: 合成文字列（img 占位 `[img: …]` 等）を置くアリーナのオフセット。
///   C は placeholder を arena bump + 8B アラインで確保するため、「直前
///   placeholder 長 %8 == 0 かつ間に他の arena alloc が無い」とき次の
///   placeholder とポインタ隣接し、2 つの占位が 1 seg に合体する（dump-layout の
///   `segs=N` にだけ観測される C の allocator 人工物）。`syn_alloc` が bump 位置を、
///   `flat_grow_model`（pieces/links/prec の grow イベント）と `syn_foreign` が
///   介入 alloc を追跡する。
/// - `Never`: 静的文字列（折り畳み空白 " " 等）。絶対に merge しない。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Prov {
    Src(u32),
    Syn,
    Never,
}

/// `push_merge` の由来 span。C のポインタ対 `(p, len)` の追跡モデルを 1 値に束ねる
/// （merge 判定は `pm_end == p` ⇔ `prev.end == next.start` のみに使う）。
#[derive(Clone, Copy)]
struct ProvSpan {
    prov: Prov,
    start: usize,
    end: usize,
}

impl ProvSpan {
    fn new(prov: Prov, start: usize, end: usize) -> Self {
        ProvSpan { prov, start, end }
    }
}

impl<'a> Lc<'a> {
    /// テキストノード 1 枚に由来 id を発行。
    fn fresh_prov(&self) -> Prov {
        let id = self.prov_seq.get();
        self.prov_seq.set(id + 1);
        Prov::Src(id)
    }

    /// 合成文字列のアリーナ確保（8B アライン bump）。(start, end) を返す。
    fn syn_alloc(&self, len: usize) -> (usize, usize) {
        let start = (self.syn_pos.get() + 7) & !7;
        self.syn_pos.set(start + len);
        (start, start + len)
    }

    /// 合成文字列以外の arena アロケーション（bump 前進 = 隣接鎖の切断）。
    /// 量の追跡は不要で「正に進んだ」ことだけが merge 可否に効く。
    fn syn_foreign(&self) {
        self.syn_pos.set(((self.syn_pos.get() + 7) & !7) + 8);
    }

    /// C `if_arena_grow` の写し: need > cap で cap 倍増 + arena alloc 畳み込み。
    fn grow_model(&self, cap: &std::cell::Cell<u32>, need: u32) {
        let cur = cap.get();
        if need <= cur {
            return;
        }
        let mut nc = if cur == 0 { 8 } else { cur };
        while nc < need {
            nc *= 2;
        }
        cap.set(nc);
        self.syn_foreign();
    }
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
    width: Len {
        v: 0.0,
        unit: U_AUTO,
    },
    height: Len {
        v: 0.0,
        unit: U_AUTO,
    },
    margin: [Len {
        v: 0.0,
        unit: U_AUTO,
    }; 4],
    padding: [Len {
        v: 0.0,
        unit: U_AUTO,
    }; 4],
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
    /// 直前 seg の由来位置キー（由来クラス, 終端オフセット。merge 判定用。
    /// C `pm_end`（ポインタ）の追跡モデル。`Prov::Never` の push では `None`）。
    pm_key: Option<(Prov, usize)>,
    /// 現行ラインの全グリフが `IF_LF_DIRECT_BYTES` 条件を満たす（C の同名。次行は
    /// `end_line` で `true` に再初期化 — kill が現行にだけ効く C の規約をそのまま持つ）。
    direct_all: bool,
    /// LINE ボックスの出力先（親ボックスの子列）。
    lines: &'a mut Vec<BoxNode>,
    /// 行スイープ用フラット行列（C の行確定時 IfRLine 追記と同点で記録）。
    rlines: &'a std::cell::RefCell<Vec<RLine>>,
    /// seg の最終所有先（行確定時に `self.segs` を drain して移す）。
    seg_arena: &'a std::cell::RefCell<Vec<Seg>>,
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
    fn push_seg(&mut self, text: &[u8], x: i32, width: i32, st: Style, prov: Prov, end: usize) {
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
        self.pm_key = if prov == Prov::Never {
            None
        } else {
            Some((prov, end))
        };
    }

    /// 直前 seg と style・由来が同じで位置上連続なら拡張する合体 push。
    /// C の `wrap_push_merge`（`pm_st == st && pm_end == p` のポインタ比較）の写し。
    fn push_merge(&mut self, text: &[u8], sp: ProvSpan, x: i32, width: i32, st: Style) {
        if text.is_empty() {
            return;
        }
        if !self.segs.is_empty()
            && self.pm_st == Some(st)
            && sp.prov != Prov::Never
            && self.pm_key == Some((sp.prov, sp.start))
        {
            let last = self.segs.last_mut().unwrap();
            last.text.extend_from_slice(text);
            last.w += width;
            self.pm_key = Some((sp.prov, sp.end));
            return;
        }
        self.push_seg(text, x, width, st, sp.prov, sp.end);
    }

    /// 行尾の折り畳み空白 pop（C の `wrap_pop_last_seg` 相当）。
    fn pop_last_seg(&mut self) {
        self.segs.pop();
        self.pm_st = None; // 陳腐化防止（次 push は必ず push_seg 経由で再設定）
        self.pm_key = None;
    }

    /// デコード済み 1 グリフの byte-direct 妥当性を畳み込む。C の `wrap_note_direct` 相当。
    /// 条件: `gw>0`（`gw==0` はセルを生成しないためバイト列とセル列が乖離）かつ
    /// U+FFFD 置換発生時は元バイトが正に EF BF BD（enc∘dec 恒等の唯一の許容例）。
    fn note_direct(&mut self, base: &[u8], from: usize, to: usize, cp: u32, gw: i32) {
        if gw == 0 {
            self.direct_all = false;
        }
        if cp == crate::utf8::REPLACEMENT
            && !(to - from == 3
                && base[from] == 0xEF
                && base[from + 1] == 0xBF
                && base[from + 2] == 0xBD)
        {
            self.direct_all = false;
        }
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
        let direct = self.direct_all;
        self.direct_all = true; // 次行は既定で有効（無効化は note_direct で畳む）
                                // seg を共有アリーナへ移し、box と RLine の両方に区間を配る
                                // （C の `w->seg_base` が line box と IfRLine に共有される構造の写し）。
        let seg_lo;
        let seg_hi;
        {
            let mut arena = self.seg_arena.borrow_mut();
            seg_lo = arena.len() as u32;
            arena.append(&mut self.segs);
            seg_hi = arena.len() as u32;
        }
        // C は行確定時に y 加算「前」の w->y を IfRLine に記録する（box y と同値）。
        self.rlines.borrow_mut().push(RLine {
            y: self.y,
            direct,
            seg_lo,
            seg_hi,
        });
        let line = BoxNode {
            kind: BoxKind::Line,
            node: None,
            st: self.align_st,
            seg_lo,
            seg_hi,
            x: self.content_x,
            y: self.y,
            w: self.line_w,
            h: rows,
            text_align: align,
            direct,
            children: Vec::new(),
        };
        self.y += rows;
        self.lines.push(line);
    }
}

/// テキスト 1 ピースを流し込む。C の `wrap_text` 相当。
#[allow(clippy::too_many_arguments)]
fn wrap_text(
    w: &mut Wrap,
    text: &[u8],
    st: Style,
    pre: bool,
    max_lh: &mut f32,
    any: &mut bool,
    prov: Prov,
    base: usize,
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

    // C にはピース境界での pm リセットは無い（merge 追跡は由来位置の等値性に
    // 委ねる。異なる由来は Prov 比較で必ず不成立、同一由来内ではオフセット連続性が
    // 実ポインタ隣接と同値）。`base + o` がピース内オフセット o の由来位置。
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
                // ' '(0x20) のみセル列とバイト列が一致（\t は前進 8、他は gw==0 で非発行セル）
                if b0 != b' ' {
                    w.direct_all = false;
                    w.push_seg(&s[i..i + 1], w.content_x + cx, adv, st, prov, base + i + 1);
                } else {
                    w.push_merge(
                        &s[i..i + 1],
                        ProvSpan::new(prov, base + i, base + i + 1),
                        w.content_x + cx,
                        1,
                        st,
                    );
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
                    w.push_merge(
                        &s[wsend - 1..wsend],
                        ProvSpan::new(prov, base + wsend - 1, base + wsend),
                        w.content_x + cx,
                        1,
                        st,
                    );
                } else {
                    w.push_seg(b" ", w.content_x + cx, 1, st, Prov::Never, 0);
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
            w.note_direct(s, i, save, cp, gw);
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
                    w.note_direct(s, i, s2, cp2, gw2);
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
                w.note_direct(s, gs, g, cp3, gw3);
                let gwidth = if gw3 == 2 { 2 } else { 1 };
                if cx > 0 && cx + gwidth > w.content_w {
                    w.end_line(*max_lh);
                    cx = 0;
                    *max_lh = lh;
                }
                w.push_merge(
                    &s[gs..g],
                    ProvSpan::new(prov, base + gs, base + g),
                    w.content_x + cx,
                    gwidth,
                    st,
                );
                cx += gwidth;
            }
            continue;
        }

        w.push_merge(
            &s[atom_start..atom_end],
            ProvSpan::new(prov, base + atom_start, base + atom_end),
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
    /// 由来位置クラス（merge 追跡。テキストは `Src(固有 id)`、img 占位は `Syn`）。
    prov: Prov,
    /// 由来位置の起点（`Syn` では合成アリーナの start。`Src` では 0）。
    base: usize,
}

/// C の `flat_push` 相当（pieces scratch の grow イベントを追跡モデルに畳む）。
fn flat_push(pieces: &mut Vec<Piece>, lc: &Lc, p: Piece) {
    lc.grow_model(&lc.pieces_cap, pieces.len() as u32 + 1);
    pieces.push(p);
}

fn flatten_into(pieces: &mut Vec<Piece>, n_prec: &mut u32, lc: &Lc, n: NodeId, st: Style) {
    let node = lc.dom.node(n);
    match node.kind {
        NodeKind::Text => {
            let prov = lc.fresh_prov();
            flat_push(
                pieces,
                lc,
                Piece {
                    text: node.name.clone(),
                    st,
                    br: false,
                    prov,
                    base: 0,
                },
            );
        }
        NodeKind::Element => {
            let est = lc.styles[n as usize].unwrap_or(st);
            if est.display == D_NONE {
                return;
            }
            match node.tag {
                TAG_BR => {
                    flat_push(
                        pieces,
                        lc,
                        Piece {
                            text: Vec::new(),
                            st: est,
                            br: true,
                            prov: Prov::Never,
                            base: 0,
                        },
                    );
                    return;
                }
                TAG_IMG => {
                    let alt = lc.dom.attr(n, b"alt").unwrap_or(b"");
                    let m = alt.len().min(900);
                    let mut text = b"[img: ".to_vec();
                    text.extend_from_slice(&alt[..m]);
                    text.push(b']');
                    // C は placeholder を layout arena に 8B アライン bump 確保する
                    // （連続 placeholder がポインタ隣接し得る = merge の追跡対象）。
                    let (base, _) = lc.syn_alloc(text.len());
                    flat_push(
                        pieces,
                        lc,
                        Piece {
                            text,
                            st: est,
                            br: false,
                            prov: Prov::Syn,
                            base,
                        },
                    );
                    return;
                }
                TAG_A => {
                    // C の collect_link + flat_push_prec の grow イベント写し
                    // （links/prec 配列の arena alloc が合成 placeholder の隣接鎖を切る）。
                    let ln0 = lc.n_links.get();
                    let href_ok = lc.dom.attr(n, b"href").is_some_and(|h| !h.is_empty());
                    if href_ok {
                        lc.grow_model(&lc.links_cap, ln0 + 1);
                        lc.n_links.set(ln0 + 1);
                    }
                    // rec = !no_boxlink && n_links != ln0。本経路（non-linear 写し）では
                    // no_boxlink=false 固定のため rec == href_ok。
                    let rec = href_ok;
                    let mut c = node.first_child;
                    while let Some(cid) = c {
                        flatten_into(pieces, n_prec, lc, cid, est);
                        c = lc.dom.node(cid).next_sibling;
                    }
                    if rec && *n_prec < 4096 {
                        lc.grow_model(&lc.prec_cap, *n_prec + 1);
                        *n_prec += 1;
                    }
                    return;
                }
                _ => {}
            }
            let mut c = node.first_child;
            while let Some(cid) = c {
                flatten_into(pieces, n_prec, lc, cid, est);
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
        pm_key: None,
        direct_all: true,
        lines,
        rlines: &lc.rlines,
        seg_arena: &lc.seg_arena,
    };
    let mut max_lh = if base_st.line_height > 0.0 {
        base_st.line_height
    } else {
        base_st.font_size * 1.2
    };
    let pre = base_st.white_space == WS_PRE;

    // flatten（DOM をピース列へ）
    let mut pieces = Vec::new();
    let mut n_prec = 0u32; // ブロック内の <a> span 収集数（C の f->n_prec 写し）
    let mut c = Some(cur);
    while let Some(cid) = c {
        let cnode = lc.dom.node(cid);
        if cnode.kind == NodeKind::Element {
            // C の run walk は「style が貼っていない要素はゲート不発」で
            // flatten を継続する（--no-style では block 指定の要素すら
            // run を終わらせない = 全文が 1 つの IFC に融合する quirk。
            // style 未適用時の再現に必要）。自身の style が無い要素は
            // 継承値でゲートせず、Some のときだけ D_NONE/D_INLINE を見る。
            if let Some(cst) = lc.styles[cid as usize] {
                if cst.display == D_NONE {
                    c = cnode.next_sibling;
                    continue;
                }
                if cst.display != D_INLINE {
                    break; // ブロック子: run 終了
                }
            }
        }
        flatten_into(&mut pieces, &mut n_prec, lc, cid, base_st);
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
            piece.prov,
            piece.base,
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
    let full_w = g.bl + g.pl + g.content_w + g.pr + g.brd;

    // 行スイープ用装飾 op（DFS=paint 順: 親の装飾は子より先に追記される。
    // C layout.c の deco_add 群の写し。h は box 確定後に後埋め）
    let deco_bg = if (st.bg & 0xFF) >= 128 {
        deco_push(
            lc,
            Deco {
                kind: DecoKind::Bg,
                x,
                y,
                w: full_w,
                h: 0, // 後埋め
                argb: st.bg,
                sides: 0,
                m_color: 0,
                m_bg: 0,
                m_flags: 0,
                text: [0; 12],
                tlen: 0,
            },
        )
    } else {
        u32::MAX
    };

    if lc.dom.node(node).tag == TAG_HR {
        let bh = g.bt + 1 + g.bbo;
        // paint_shell の HR: bt を除いた行へ罫線（既定フルペン。C と同一）
        deco_push(
            lc,
            Deco {
                kind: DecoKind::Hline,
                x,
                y: y + g.bt,
                w: full_w,
                h: 1,
                argb: 0,
                sides: 0,
                m_color: 0,
                m_bg: 0,
                m_flags: 0,
                text: [0; 12],
                tlen: 0,
            },
        );
        deco_patch_h(lc, deco_bg, bh);
        return BoxNode {
            kind: BoxKind::Block,
            node: Some(node),
            st,
            seg_lo: 0,
            seg_hi: 0,
            x,
            y,
            w: full_w,
            h: bh,
            text_align: 0,
            direct: false,
            children: Vec::new(),
        };
    }

    let deco_bd = if (g.bl | g.brd | g.bt | g.bbo) != 0 {
        let sides =
            ((g.bt & 1) | ((g.brd & 1) << 1) | ((g.bbo & 1) << 2) | ((g.bl & 1) << 3)) as u8;
        deco_push(
            lc,
            Deco {
                kind: DecoKind::Border,
                x,
                y,
                w: full_w,
                h: 0, // 後埋め
                argb: st.border_color,
                sides,
                m_color: 0,
                m_bg: 0,
                m_flags: 0,
                text: [0; 12],
                tlen: 0,
            },
        )
    } else {
        u32::MAX
    };

    let mut children = Vec::new();
    let content_h = layout_children(
        lc,
        node,
        st,
        content_x,
        content_y,
        g.content_w,
        &mut children,
    );
    let content_h = if g.height_spec >= 0 && g.height_spec > content_h {
        g.height_spec // 指定高はクリップせず拡張のみ
    } else {
        content_h
    };

    let bh = g.bt + g.pt + content_h + g.pb + g.bbo;
    deco_patch_h(lc, deco_bg, bh);
    deco_patch_h(lc, deco_bd, bh);
    BoxNode {
        kind: BoxKind::Block,
        node: Some(node),
        st,
        seg_lo: 0,
        seg_hi: 0,
        x,
        y,
        w: full_w,
        h: bh,
        text_align: 0,
        direct: false,
        children,
    }
}

/// C layout.c の MARKER deco 追記（配置は li box 左端手前/上端。x,y はこの時点で
/// 確定済み）。呼び出し側の事前条件: `c->tag == IF_TAG_LI && display == LIST_ITEM`。
/// `li_ord` は「この親で自分より前の LIST_ITEM li 数+1」（draw_marker 同値で O(1)）。
fn deco_marker_push(
    lc: &Lc,
    li: NodeId,
    cst: Style,
    content_x: i32,
    ml: i32,
    y: i32,
    li_ord: &mut u32,
) {
    *li_ord += 1;
    // 直近の祖先 ul/ol を探す（ネストした li の外には出ない）。render の draw_marker と同値
    let mut list = TAG_UL;
    let mut p = lc.dom.node(li).parent;
    while let Some(pid) = p {
        let pn = lc.dom.node(pid);
        if pn.kind == NodeKind::Element && (pn.tag == TAG_UL || pn.tag == TAG_OL) {
            list = pn.tag;
            break;
        }
        if pn.kind == NodeKind::Element && pn.tag == TAG_LI {
            break;
        }
        p = pn.parent;
    }
    let m_flags = (cst.bold as u8)
        | ((cst.italic as u8) << 1)
        | ((cst.underline as u8) << 2)
        | ((cst.strike as u8) << 3);
    let bx = content_x + ml;
    let (mx, w, text, tlen) = if list == TAG_UL {
        let mut mx = bx - 2;
        if mx < 0 {
            mx = bx; // C の clamp は 0 ではなく bx（quirk 保存）
        }
        let mut t = [0u8; 12];
        t[..4].copy_from_slice(b"\xE2\x80\xA2 "); // "• "
        (mx, 2, t, 4u8)
    } else {
        // C snprintf("%u.") 相当（u32 最大 10 桁 + '.' = 11B ≤ 12 で truncate しない）
        let nb = format!("{}.", li_ord);
        let m = nb.len();
        let mut mx = bx - (m as i32 + 1);
        if mx < 0 {
            mx = 0; // ol は 0 clamp（ul と非対称。C どおり）
        }
        let mut t = [0u8; 12];
        t[..m].copy_from_slice(nb.as_bytes());
        (mx, m as i32, t, m as u8)
    };
    deco_push(
        lc,
        Deco {
            kind: DecoKind::Marker,
            x: mx,
            y,
            w,
            h: 1,
            argb: 0,
            sides: 0,
            m_color: cst.color,
            m_bg: cst.bg,
            m_flags,
            text,
            tlen,
        },
    );
}

/// 「純ブロック容器」タグ集合。C `layout.c` の `ws_sink_parent` 相当（`md.c` の
/// `mo_ws_sink` と同じ集合）。md fast-DOM はこれら直下の ws-only TEXT を剥がすが、
/// 旧 DOM では当該 ws TEXT が ifc 経由で `prev_mb` を 0 にしていたので、剥がし後も
/// 同じ容器の兄弟では相殺を無効化して逐語同値を保つ（`layout_children` 参照）。
fn ws_sink_parent(tag: Tag) -> bool {
    matches!(
        tag,
        TAG_BODY
            | TAG_BLOCKQUOTE
            | TAG_TABLE
            | TAG_THEAD
            | TAG_TBODY
            | TAG_TR
            | TAG_UL
            | TAG_OL
            | TAG_SECTION
    )
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
    let mut li_ord = 0u32; // ol 番号: この親の LIST_ITEM li を出現順に数える（C の li_ord 写し）
                           // ws 相殺補正は親タグのみの関数 → ループ不変（C と同じ構成）
    let sinkp = lc.dom.md_ws_stripped && ws_sink_parent(lc.dom.node(node).tag);
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
        if cnode.tag == TAG_LI && cst.display == D_LIST_ITEM {
            deco_marker_push(lc, cid, cst, content_x, cg.ml, y, &mut li_ord);
        }
        let child = layout_element(lc, cid, cst, content_x, y, content_w);
        let child_h = child.h;
        children.push(child);
        y += child_h;
        prev_mb = cg.mb;
        if sinkp {
            // 旧 DOM では sink 容器直下の ws TEXT が ifc 経由で必ず prev_mb を 0 に
            // していた → 剥がし後の同値補正（C `layout.c` / `md.c` 参照）
            prev_mb = 0;
        }
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
        syn_pos: std::cell::Cell::new(0),
        pieces_cap: std::cell::Cell::new(0),
        links_cap: std::cell::Cell::new(0),
        n_links: std::cell::Cell::new(0),
        prec_cap: std::cell::Cell::new(0),
        prov_seq: std::cell::Cell::new(0),
        rlines: std::cell::RefCell::new(Vec::new()),
        deco: std::cell::RefCell::new(Vec::new()),
        seg_arena: std::cell::RefCell::new(Vec::new()),
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
                seg_lo: 0,
                seg_hi: 0,
                x: 0,
                y: 0,
                w: 0,
                h: 0,
                text_align: 0,
                direct: false,
                children: Vec::new(),
            },
            width: width_cells,
            lines: Vec::new(),
            deco: Vec::new(),
            seg_arena: Vec::new(),
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
        lines: lc.rlines.into_inner(),
        deco: lc.deco.into_inner(),
        seg_arena: lc.seg_arena.into_inner(),
    }
}

/// ボックス木ダンプ（C の `if_layout_dump` 相当。raw バイトで出力）。
fn dump_box(b: &BoxNode, dom: &Dom, arena: &[Seg], depth: usize, out: &mut Vec<u8>) {
    for _ in 0..depth {
        out.extend_from_slice(b"  ");
    }
    match b.kind {
        BoxKind::Line => {
            let segs = &arena[b.seg_lo as usize..b.seg_hi as usize];
            out.extend_from_slice(
                format!(
                    "LINE x={} y={} w={} h={} segs={} \"",
                    b.x,
                    b.y,
                    b.w,
                    b.h,
                    segs.len()
                )
                .as_bytes(),
            );
            for seg in segs {
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
            out.extend_from_slice(
                format!("BLOCK x={} y={} w={} h={}", b.x, b.y, b.w, b.h).as_bytes(),
            );
            if let Some(nid) = b.node {
                out.push(b' ');
                out.push(b'<');
                out.extend_from_slice(&dom.node(nid).name);
                out.push(b'>');
            }
            out.push(b'\n');
            for c in &b.children {
                dump_box(c, dom, arena, depth + 1, out);
            }
        }
    }
}

/// ボックス木を html5lib 的テキストではなくデバッグ形式でダンプ（C の `if_layout_dump`）。
pub fn layout_dump(dom: &Dom, layout: &Layout) -> Vec<u8> {
    let mut out = Vec::new();
    dump_box(&layout.root, dom, &layout.seg_arena, 0, &mut out);
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
        assert!(
            d.contains("LINE x=1 y=2 w=5 h=1 segs=1 \"hello\""),
            "got: {d}"
        );
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
