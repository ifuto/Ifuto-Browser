//! 行スイープレンダラ（C の `src/render_ansi.c` **CLI 行スイープ経路**の機械的写し）。
//!
//! | C (render_ansi.c) | Rust |
//! |---|---|
//! | `if_render_emit_rows_sweep` + `sweep_range` | [`render_emit_sweep`] |
//! | `row_emit_direct` / `row_emit_fast` / `row_emit_ansi_fast` | `try_direct` / `try_fast` / `try_ansi_fast` |
//! | slow セル行経路（`row_paint_dec`/`row_paint_text` + 発行） | `emit_slow` / `row_paint_dec` / `row_paint_text` |
//! | `IfLCur`（判定用カーソル） | `Sweep.li`（`Vec<RLine>` 上の添字） |
//! | `IfPen` | `(u8, u8, u8)` |
//! | `if_render_extent` | [`render_extent`] |
//! | `if_rgba_to_ansi` | [`rgba_to_ansi`] |
//!
//! # アーキテクチャ（フェーズ 10-a で全グリッド経路から移行）
//!
//! 旧実装は paint イベントを全文書グリッド（`cells[y*w+x]`）+ 行ごとの監査帳簿
//! （`RowInfo`）へ畳み込んでから発行経路を事後判定していた。これは byte 一致の
//! ためには正確だが、C が行カーソル + deco active 集合だけで発行できるのに対し
//! 全行へ帳簿の会費を課す設計だった。本実装は C と同じフラット streams
//! （[`crate::layout::Layout::lines`] / [`crate::layout::Layout::deco`]) を直接
//! 消費して行を逐次発行する。全グリッド確保・帳簿・paint 全走査は存在しない。
//!
//! 発行規約は C との byte 一致が契約（golden / diff fuzz / WPT が機械監査）:
//! no-ansi の行末空白 trim・行末リセット無条件・行区切りのみ発行のギャップ一括充填・
//! byte-direct（direct 旗付き行を raw bytes で直行）/fast（runs 合流発行）/
//! ansi-fast（BG ピース合成直行）の受理条件・失敗時の slow 降格、全て同値。
//!
//! # 未移植（性能最適化・観測不変）
//!
//! - 2-way 並列 sweep（C の pthread 分割。発行バイト列は直列と厳密一致が C で
//!   保証済みのため、直列実装は byte 観測で並列実装と区別不能。速度差のみ）
//! - 窓グリッド経路（`if_render_grid_rows_into(_cur)`。GUI 用で CLI 不使用）
//! - `raster.c` の fill カーネル自動選択・rdtsc プロファイリング

use crate::css::Style;
use crate::layout::{BoxKind, BoxNode, Deco, DecoKind, Layout};

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

/// 既定 pen（C の `PDEF`）。
const PDEF: (u8, u8, u8) = (CELL_DEFAULT, CELL_DEFAULT, 0);

/// セル（C の `IfRCell` 相当。行バッファ 1 枚だけを使い回す）。
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

/// 計算済みスタイル → セル pen（C の `seg_pen` 相当）。
fn pen(st: Option<&Style>) -> (u8, u8, u8) {
    match st {
        None => PDEF,
        Some(st) => pen_raw(st.color, st.bg, {
            let mut f = 0u8;
            if st.bold {
                f |= F_BOLD;
            }
            if st.italic {
                f |= F_ITALIC;
            }
            if st.underline {
                f |= F_ULINE;
            }
            if st.strike {
                f |= F_STRIKE;
            }
            f
        }),
    }
}

/// 未変換 RGBA + 装飾旗 → セル pen（deco MARKER の保存原料からの変換用。
/// `seg_pen` と同式を 1 点化するためこちらが正本、`pen` はこちらへの委譲）。
fn pen_raw(color: u32, bg: u32, flags: u8) -> (u8, u8, u8) {
    (rgba_to_ansi(color), rgba_to_ansi(bg), flags)
}

/// v ∈ [0,255] の十進化（C の `u8_dec` 写し。snprintf("%u") と同一バイト列を
/// スタックに出す。pen 遷移は行内で最も熱い 1 発行で、`format!` の一時 String
/// 確保を許さない）
fn u8_dec(p: &mut [u8; 3], v: u8) -> usize {
    if v >= 100 {
        p[0] = b'0' + v / 100;
        p[1] = b'0' + (v % 100) / 10;
        p[2] = b'0' + v % 10;
        3
    } else if v >= 10 {
        p[0] = b'0' + v / 10;
        p[1] = b'0' + v % 10;
        2
    } else {
        p[0] = b'0' + v;
        1
    }
}

/// SGR 遷移の発行（C の `pen_emit`。reset→bold→italic→uline→strike→fg→bg）。
fn emit_pen(out: &mut Vec<u8>, p: (u8, u8, u8), cur: &mut (u8, u8, u8)) {
    if p == *cur {
        return;
    }
    let (fg, bg, flags) = p;
    // 1 回の SGR 遷移は最大 4+4*4+12+12 = 44B。一時バッファに組んで 1 回で追記する
    // （extend_from_slice の回数自体も熱い）
    let mut tmp = [0u8; 48];
    let mut n = 0usize;
    tmp[n..n + 4].copy_from_slice(b"\x1b[0m");
    n += 4;
    if flags & F_BOLD != 0 {
        tmp[n..n + 4].copy_from_slice(b"\x1b[1m");
        n += 4;
    }
    if flags & F_ITALIC != 0 {
        tmp[n..n + 4].copy_from_slice(b"\x1b[3m");
        n += 4;
    }
    if flags & F_ULINE != 0 {
        tmp[n..n + 4].copy_from_slice(b"\x1b[4m");
        n += 4;
    }
    if flags & F_STRIKE != 0 {
        tmp[n..n + 4].copy_from_slice(b"\x1b[9m");
        n += 4;
    }
    if fg != CELL_DEFAULT {
        tmp[n..n + 7].copy_from_slice(b"\x1b[38;5;");
        n += 7;
        let mut d = [0u8; 3];
        let dn = u8_dec(&mut d, fg);
        tmp[n..n + dn].copy_from_slice(&d[..dn]);
        n += dn;
        tmp[n] = b'm';
        n += 1;
    }
    if bg != CELL_DEFAULT {
        tmp[n..n + 7].copy_from_slice(b"\x1b[48;5;");
        n += 7;
        let mut d = [0u8; 3];
        let dn = u8_dec(&mut d, bg);
        tmp[n..n + dn].copy_from_slice(&d[..dn]);
        n += dn;
        tmp[n] = b'm';
        n += 1;
    }
    out.extend_from_slice(&tmp[..n]);
    *cur = p;
}

/// ---- fast 発行の 1 ラン（C の `IfRRun` / `IfRRunA` 相当） ----
enum Run<'a> {
    /// seg / MARKER の生バイト（自身 pen 付き）。
    Bytes {
        x: i32,
        w: i32,
        p: &'a [u8],
        pen: (u8, u8, u8),
    },
    /// `<hr>` 罫線（占有セル幅。セル単位 clip 可の唯一の run）。
    Hline { x: i32, w: i32 },
}

impl Run<'_> {
    fn x(&self) -> i32 {
        match *self {
            Run::Bytes { x, .. } => x,
            Run::Hline { x, .. } => x,
        }
    }
}

/// ---- 行スイープ本体（C の `sweep_range` 直列経路相当） ----
struct Sweep<'a> {
    lay: &'a Layout,
    /// seg テキスト座標の dereference 用（C の IfSeg ポインタ読みと同値）。
    dom: &'a crate::dom::Dom,
    /// viewport セル幅（C の `mx`。`lay->width` 固定）。
    mx: i32,
    /// 発行バッファ（C の IfBB。追記巻き戻しは `truncate` で行う）。
    out: Vec<u8>,
    /// 現行 pen（ansi の行を跨ぐ遷移追跡）。
    cur: (u8, u8, u8),
    /// deco active 集合（`lay.deco` の添字。追記順保持・期限切れは惰性除去）。
    active: Vec<usize>,
    /// deco 走査位置（未消費の先頭）。
    di: usize,
    /// lines 走査位置（C の IfLCur。`Vec` 上の添字なので seek 不要）。
    li: usize,
    /// slow 行バッファ（`mx` セルを使い回す。C の `IfRCell row[]`）。
    row: Vec<Cell>,
}

/// レイアウトの行スイープ発行。C の `if_render_emit_rows_sweep`（直列 sweep_range）
/// 相当。ansi=1 で 256 色 SGR、0 でプレーン。
pub fn render_emit_sweep(dom: &crate::dom::Dom, lay: &Layout, ansi: bool) -> Vec<u8> {
    let mx = lay.width.max(1);
    let my = lay.height.max(1);
    let mut s = Sweep {
        lay,
        dom,
        mx,
        // 出力は行×(幅+装飾) に比例するため重めに事前確保（再配置の削減のみ。
        // バイト列には無関係）
        out: Vec::with_capacity((my as usize).saturating_mul(16).min(1 << 22)),
        cur: PDEF,
        active: Vec::new(),
        di: 0,
        li: 0,
        row: vec![Cell::default(); mx as usize],
    };
    let mut r = 0i32;
    while r < my {
        r = s.emit_row(r, my, ansi);
    }
    s.out
}

impl<'a> Sweep<'a> {
    /// 行 r を発行し、次に発行すべき行番号を返す（ギャップ一括充填で r が跳ぶ）。
    /// C の `sweep_range` の for ループ本体の写し。
    fn emit_row(&mut self, r: i32, my: i32, ansi: bool) -> i32 {
        let lay = self.lay;
        let lines = &lay.lines[..];
        let deco = &lay.deco[..];

        // ---- ギャップ一括充填（no-ansi 専用） ----
        if !ansi {
            let mut nl_y = if self.li < lines.len() {
                lines[self.li].y
            } else {
                my
            };
            if nl_y > my {
                nl_y = my;
            }
            if nl_y > r {
                // (a) active の非 BG deco、(b) 新たに開始する非 BG deco が無ければ
                // 各行の発行は '\n' のみ（BG は no-ansi で不可視。C と同値）
                let mut clean = true;
                for &k in &self.active {
                    if deco[k].kind != DecoKind::Bg {
                        clean = false;
                        break;
                    }
                }
                if clean {
                    let mut d2 = self.di;
                    while d2 < deco.len() && deco[d2].y < nl_y {
                        if deco[d2].kind != DecoKind::Bg {
                            clean = false;
                            break;
                        }
                        d2 += 1;
                    }
                }
                if clean {
                    let gap = (nl_y - r) as usize;
                    self.out.resize(self.out.len() + gap, b'\n');
                    return nl_y;
                }
            }
        }

        // ---- deco の開始（y <= r）は追記順に active へ ----
        while self.di < deco.len() && deco[self.di].y <= r {
            let d = &deco[self.di];
            let idx = self.di;
            self.di += 1;
            let dh = d.h.max(1);
            if r < d.y + dh {
                self.active.push(idx);
            }
        }
        // ---- 期限切れ除去（順序保持の compact） ----
        let mut k = 0;
        while k < self.active.len() {
            let d = &deco[self.active[k]];
            let dh = d.h.max(1);
            if !(d.y <= r && r < d.y + dh) {
                self.active.remove(k);
            } else {
                k += 1;
            }
        }

        let has_line = self.li < lines.len() && lines[self.li].y == r;
        if !has_line && self.active.is_empty() {
            // ブランク行（発行規約どおり）
            if ansi {
                self.out.resize(self.out.len() + self.mx as usize, b' ');
                self.out.extend_from_slice(b"\x1b[0m");
                self.cur = PDEF;
            }
            self.out.push(b'\n');
            return r + 1;
        }

        // ---- no-ansi: byte-direct / fast 直行 ----
        if !ansi {
            let mut cp_free = true;
            let mut fastable = true;
            for &k in &self.active {
                match deco[k].kind {
                    DecoKind::Bg => {}
                    DecoKind::Marker | DecoKind::Hline => cp_free = false,
                    DecoKind::Border => {
                        cp_free = false;
                        fastable = false;
                    }
                }
            }
            if !has_line && cp_free {
                // BG のみの行は no-ansi では見えない（fill_bg は cp を触らない）
                self.out.push(b'\n');
                return r + 1;
            }
            if has_line && fastable {
                let multi = lines.get(self.li + 1).is_some_and(|l| l.y == r);
                if cp_free && !multi && self.try_direct() {
                    return r + 1;
                }
                if self.try_fast(r) {
                    return r + 1;
                }
            }
        }

        // ---- ANSI: byte-direct + BG/MARKER/HLINE 合成可能な行は直行 ----
        if ansi && self.try_ansi_fast(r) {
            return r + 1;
        }

        // ---- slow セル経路 ----
        self.emit_slow(r, ansi);
        r + 1
    }

    /// no-ansi byte-direct（C の `row_emit_direct`）。成功時のみ lines カーソルを進める。
    fn try_direct(&mut self) -> bool {
        let line = self.lay.lines[self.li];
        if !line.direct {
            return false;
        }
        let mark = self.out.len();
        let segs = &self.lay.seg_arena[line.seg_lo as usize..line.seg_hi as usize];
        let mut pos = 0i32;
        for s in segs {
            // C の __builtin_expect 判定: x < pos（非単調/重複）・右端跨ぎは堕落
            if s.x < pos || s.x + s.w > self.mx {
                self.out.truncate(mark);
                return false;
            }
            if s.x > pos {
                let g = (s.x - pos) as usize;
                self.out.resize(self.out.len() + g, b' ');
            }
            self.out.extend_from_slice(self.lay.seg_text(self.dom, s));
            pos = s.x + s.w;
        }
        while self.out.len() > mark && self.out.last() == Some(&b' ') {
            self.out.pop();
        }
        self.out.push(b'\n');
        self.li += 1;
        true
    }

    /// no-ansi runs 合流発行（C の `row_emit_fast`）。受理条件は C の写し。
    fn try_fast(&mut self, r: i32) -> bool {
        let lay = self.lay;
        let lines = &lay.lines[..];
        let deco = &lay.deco[..];
        let line = lines[self.li];
        if !line.direct {
            return false;
        }
        if lines.get(self.li + 1).is_some_and(|l| l.y == r) {
            return false; // 同行複数行は堕落
        }

        // ラン集合: segs（x 単調）+ marker/hline。BORDER は未対応、BG は no-ansi で無影響
        let mut runs: Vec<Run> = Vec::new();
        {
            let segs = &lay.seg_arena[line.seg_lo as usize..line.seg_hi as usize];
            for s in segs {
                if runs.len() == 64 {
                    return false;
                }
                runs.push(Run::Bytes {
                    x: s.x,
                    w: s.w,
                    p: lay.seg_text(self.dom, s),
                    pen: pen(Some(lay.seg_style(s))),
                });
            }
        }
        for &k in &self.active {
            let d = &deco[k];
            match d.kind {
                DecoKind::Bg => continue,
                DecoKind::Marker => {
                    if runs.len() == 64 {
                        return false;
                    }
                    runs.push(Run::Bytes {
                        x: d.x,
                        w: d.w,
                        p: &d.text[..d.tlen as usize],
                        pen: pen_raw(d.m_color, d.m_bg, d.m_flags),
                    });
                }
                DecoKind::Hline => {
                    if runs.len() == 64 {
                        return false;
                    }
                    runs.push(Run::Hline { x: d.x, w: d.w });
                }
                DecoKind::Border => return false, // セル経路へ
            }
        }
        // x 昇順へ挿入ソート（segs は既に単調、deco は小数。C と同一アルゴリズム）
        for a in 1..runs.len() {
            let mut c = a;
            while c > 0 && runs[c - 1].x() > runs[c].x() {
                runs.swap(c - 1, c);
                c -= 1;
            }
        }

        let mark = self.out.len();
        let mut pos = 0i32;
        for run in &runs {
            if run.x() < pos {
                self.out.truncate(mark);
                return false; // 重複はセル経路へ
            }
            if run.x() > pos {
                let ge = if run.x() > self.mx { self.mx } else { run.x() };
                let gap = ge - pos;
                if gap < 0 {
                    self.out.truncate(mark);
                    return false;
                }
                self.out.resize(self.out.len() + gap as usize, b' ');
                pos = run.x();
            }
            if pos >= self.mx {
                break; // viewport 右端でクリップ
            }
            match run {
                Run::Hline { w, .. } => {
                    let mut w2 = *w;
                    if pos + w2 > self.mx {
                        w2 = self.mx - pos;
                    }
                    for _ in 0..w2.max(0) {
                        self.out.extend_from_slice(b"\xE2\x94\x80"); // ─
                    }
                    pos += w; // clip 後も pos は元の w で進む（C と同一規則）
                }
                Run::Bytes { w, p, .. } => {
                    if pos + w > self.mx {
                        self.out.truncate(mark);
                        return false; // 右端を跨ぐ bytes run は再構成不能
                    }
                    self.out.extend_from_slice(p);
                    pos += w;
                }
            }
        }
        while self.out.len() > mark && self.out.last() == Some(&b' ') {
            self.out.pop();
        }
        self.out.push(b'\n');
        self.li += 1;
        true
    }

    /// ANSI runs 合流 + BG ピース合成（C の `row_emit_ansi_fast`）。受理条件は C の写し。
    fn try_ansi_fast(&mut self, r: i32) -> bool {
        let lay = self.lay;
        let lines = &lay.lines[..];
        let deco = &lay.deco[..];
        let has_line = self.li < lines.len() && lines[self.li].y == r;
        let mut line = None;
        if has_line {
            let l = lines[self.li];
            if !l.direct {
                return false;
            }
            if lines.get(self.li + 1).is_some_and(|n| n.y == r) {
                return false; // 同行複数行は堕落
            }
            line = Some(l);
        }

        // (1) BG 合成: [0,mx) の bg ピース列（active 追記順・後勝ち。<=32 枚まで受理）
        // ピースは (x0, x1, bg)。x 昇順・非重複を不変条件とする。
        let mut pc: Vec<(i32, i32, u8)> = vec![(0, self.mx, CELL_DEFAULT)];
        {
            let n_bg = self
                .active
                .iter()
                .filter(|&&k| deco[k].kind == DecoKind::Bg)
                .count();
            if n_bg > 32 {
                return false; // 異常系は slow へ
            }
            for &k in &self.active {
                let d = &deco[k];
                if d.kind != DecoKind::Bg {
                    continue;
                }
                let idx = rgba_to_ansi(d.argb);
                if idx == CELL_DEFAULT && (d.argb & 0xFF) < 128 {
                    continue; // row_paint_dec と同一 skip
                }
                let x0 = d.x.max(0);
                let x1 = (d.x + d.w).min(self.mx);
                if x1 <= x0 {
                    continue;
                }
                let mut tmp: Vec<(i32, i32, u8)> = Vec::with_capacity(pc.len() + 2);
                for &(px0, px1, pbg) in &pc {
                    if px1 <= x0 || px0 >= x1 {
                        tmp.push((px0, px1, pbg));
                        continue;
                    }
                    if px0 < x0 {
                        tmp.push((px0, x0, pbg));
                    }
                    tmp.push((px0.max(x0), px1.min(x1), idx));
                    if px1 > x1 {
                        tmp.push((x1, px1, pbg));
                    }
                }
                pc = tmp;
            }
        }

        // (2) ラン集合: segs（x 単調）+ MARKER/HLINE（row_emit_fast と同一手順 + pen）
        let mut runs: Vec<Run> = Vec::new();
        if let Some(l) = line {
            let segs = &lay.seg_arena[l.seg_lo as usize..l.seg_hi as usize];
            for s in segs {
                if runs.len() == 64 {
                    return false;
                }
                runs.push(Run::Bytes {
                    x: s.x,
                    w: s.w,
                    p: lay.seg_text(self.dom, s),
                    pen: pen(Some(lay.seg_style(s))),
                });
            }
        }
        for &k in &self.active {
            let d = &deco[k];
            match d.kind {
                DecoKind::Bg => continue, // 合成済み
                DecoKind::Marker => {
                    // text の glyph 検査（wrap_note_direct と同条件。幅和==w も必須）
                    let t = &d.text[..d.tlen as usize];
                    let mut i = 0usize;
                    let mut wsum = 0i32;
                    while i < t.len() {
                        let from = i;
                        let cp = crate::utf8::decode(t, &mut i);
                        let gw = crate::utf8::glyph_width(cp);
                        if gw <= 0 {
                            return false;
                        }
                        if cp == crate::utf8::REPLACEMENT
                            && !(i - from == 3
                                && t[from] == 0xEF
                                && t[from + 1] == 0xBF
                                && t[from + 2] == 0xBD)
                        {
                            return false;
                        }
                        wsum += gw;
                    }
                    if wsum != d.w {
                        return false;
                    }
                    if runs.len() == 64 {
                        return false;
                    }
                    runs.push(Run::Bytes {
                        x: d.x,
                        w: d.w,
                        p: t,
                        pen: pen_raw(d.m_color, d.m_bg, d.m_flags),
                    });
                }
                DecoKind::Hline => {
                    if runs.len() == 64 {
                        return false;
                    }
                    runs.push(Run::Hline { x: d.x, w: d.w });
                }
                DecoKind::Border => return false,
            }
        }
        for a in 1..runs.len() {
            let mut c = a;
            while c > 0 && runs[c - 1].x() > runs[c].x() {
                runs.swap(c - 1, c);
                c -= 1;
            }
        }

        // (3) 発行: ラン間ギャップはピース駆動、ランは pen 遷移 + 生バイト（巻き戻し可能）
        let mark = self.out.len();
        let save = self.cur;
        let mut pos = 0i32;
        for run in &runs {
            if run.x() < pos {
                self.out.truncate(mark);
                self.cur = save;
                return false;
            }
            if run.x() > pos {
                let ge = if run.x() > self.mx { self.mx } else { run.x() };
                if ge > pos {
                    self.gap_emit_pieces(&pc, pos, ge);
                }
                pos = run.x();
            }
            if pos >= self.mx {
                break;
            }
            match run {
                Run::Hline { w, .. } => {
                    let mut w2 = *w;
                    if pos + w2 > self.mx {
                        w2 = self.mx - pos; // セル単位 clip は HLINE だけ許容
                    }
                    if w2 > 0 {
                        // w<=0 の空ランは細胞を持たない → pen 遷移も発生しない
                        emit_pen(&mut self.out, PDEF, &mut self.cur);
                        for _ in 0..w2 {
                            self.out.extend_from_slice(b"\xE2\x94\x80");
                        }
                    }
                    pos += w;
                }
                Run::Bytes { w, p, pen, .. } => {
                    if pos + w > self.mx {
                        self.out.truncate(mark);
                        self.cur = save;
                        return false;
                    }
                    emit_pen(&mut self.out, *pen, &mut self.cur);
                    self.out.extend_from_slice(p);
                    pos += w;
                }
            }
        }
        if self.mx > pos {
            self.gap_emit_pieces(&pc, pos, self.mx); // 末尾ギャップ
        }
        self.out.extend_from_slice(b"\x1b[0m\n");
        self.cur = PDEF;
        if has_line {
            self.li += 1;
        }
        true
    }

    /// BG ピース列の [a,b) 区間を空白 + pen 遷移で発行（C の `gap_emit_pieces`）。
    fn gap_emit_pieces(&mut self, pc: &[(i32, i32, u8)], a: i32, b: i32) {
        for &(x0, x1, bg) in pc {
            if x1 <= a {
                continue;
            }
            if x0 >= b {
                break; // ピースは x 昇順・非重複
            }
            let s = x0.max(a);
            let e = x1.min(b);
            emit_pen(&mut self.out, (CELL_DEFAULT, bg, 0), &mut self.cur);
            self.out.resize(self.out.len() + (e - s) as usize, b' ');
        }
    }

    /// slow セル経路（C の sweep_range 末尾の行構成+発行の写し）。
    fn emit_slow(&mut self, r: i32, ansi: bool) {
        let lay = self.lay;
        let lines = &lay.lines[..];
        let deco = &lay.deco[..];
        let mut maxx = self.mx;
        let has_line = self.li < lines.len() && lines[self.li].y == r;
        if !ansi {
            maxx = 0;
            // この行の全 LINE（現行 + 追従する同行）と 非 BG deco から幅を確定
            let mut j = self.li;
            while j < lines.len() && lines[j].y == r {
                let l = lines[j];
                for s in &lay.seg_arena[l.seg_lo as usize..l.seg_hi as usize] {
                    if s.x + s.w > maxx {
                        maxx = s.x + s.w;
                    }
                }
                j += 1;
            }
            let _ = has_line;
            for &k in &self.active {
                let d = &deco[k];
                if d.kind == DecoKind::Bg {
                    continue; // bg は no-ansi 出力に影響しない
                }
                if d.x + d.w > maxx {
                    maxx = d.x + d.w;
                }
            }
            if maxx > self.mx {
                maxx = self.mx;
            }
        }

        // 行構成: [0,maxx) 既定充填 → deco（追記順）→ lines
        for c in &mut self.row[..maxx as usize] {
            *c = Cell::default();
        }
        let w = maxx;
        for &k in &self.active {
            row_paint_dec(&mut self.row, w, &deco[k], r);
        }
        while self.li < lines.len() && lines[self.li].y == r {
            let l = lines[self.li];
            for s in &lay.seg_arena[l.seg_lo as usize..l.seg_hi as usize] {
                let (fg, bg, flags) = pen(Some(lay.seg_style(s)));
                row_paint_text(
                    &mut self.row,
                    w,
                    s.x,
                    lay.seg_text(self.dom, s),
                    (fg, bg, flags),
                );
            }
            self.li += 1;
        }

        // 発行（trim・行末リセットの規約は C と同一）
        let mut last = maxx - 1;
        if !ansi {
            while last >= 0 && self.row[last as usize].cp == b' ' as u32 {
                last -= 1;
            }
        }
        for x in 0..=last {
            let c = self.row[x as usize];
            if c.cp == 0 {
                continue; // 全角 2 セル目
            }
            if ansi {
                emit_pen(&mut self.out, (c.fg, c.bg, c.flags), &mut self.cur);
            }
            let mut enc = [0u8; 4];
            let n = crate::utf8::encode(c.cp, &mut enc);
            self.out.extend_from_slice(&enc[..n]);
        }
        if ansi {
            self.out.extend_from_slice(b"\x1b[0m");
            self.cur = PDEF;
        }
        self.out.push(b'\n');
    }
}

/// テキストの行バッファへの paint（C の `row_paint_text`）。pen はフル上書き。
fn row_paint_text(row: &mut [Cell], w: i32, x: i32, text: &[u8], pen: (u8, u8, u8)) {
    let (fg, bg, flags) = pen;
    let mut i = 0usize;
    let mut cx = x;
    while i < text.len() {
        let cp = crate::utf8::decode(text, &mut i);
        let gw = crate::utf8::glyph_width(cp);
        if gw == 0 {
            continue;
        }
        if cx >= 0 && cx < w {
            let c = &mut row[cx as usize];
            c.cp = cp;
            c.fg = fg;
            c.bg = bg;
            c.flags = flags;
        }
        if gw == 2 && cx + 1 >= 0 && cx + 1 < w {
            let c = &mut row[(cx + 1) as usize];
            c.cp = 0;
            c.fg = fg;
            c.bg = bg;
            c.flags = flags;
        }
        cx += gw;
    }
}

/// cp 一括ランの paint（C の `row_paint_cprun`）。keep_pen=false は bg/flags を既定に戻す。
fn row_paint_cprun(
    row: &mut [Cell],
    w: i32,
    mut x0: i32,
    mut x1: i32,
    cp: u32,
    fg: u8,
    keep_pen: bool,
) {
    if x0 < 0 {
        x0 = 0;
    }
    if x1 > w {
        x1 = w;
    }
    let mut x = x0;
    while x < x1 {
        let c = &mut row[x as usize];
        c.cp = cp;
        c.fg = fg;
        if !keep_pen {
            c.bg = CELL_DEFAULT;
            c.flags = 0;
        }
        x += 1;
    }
}

/// deco 1 個の行バッファへの paint（C の `row_paint_dec`）。
fn row_paint_dec(row: &mut [Cell], w: i32, d: &Deco, r: i32) {
    match d.kind {
        DecoKind::Bg => {
            let idx = rgba_to_ansi(d.argb);
            if idx == CELL_DEFAULT && (d.argb & 0xFF) < 128 {
                return; // fill_bg と同値の早期 return
            }
            let x0 = d.x.max(0);
            let x1 = (d.x + d.w).min(w);
            let mut x = x0;
            while x < x1 {
                row[x as usize].bg = idx;
                x += 1;
            }
        }
        DecoKind::Border => {
            let fg = rgba_to_ansi(d.argb);
            let sides = d.sides;
            let y0 = d.y;
            let y1 = d.y + d.h - 1;
            if sides & 1 != 0 && r == y0 {
                row_paint_cprun(row, w, d.x, d.x + d.w, 0x2500, fg, true);
            }
            if sides & 4 != 0 && r == y1 {
                row_paint_cprun(row, w, d.x, d.x + d.w, 0x2500, fg, true);
            }
            if sides & 8 != 0 {
                row_paint_cprun(row, w, d.x, d.x + 1, 0x2502, fg, true);
            }
            if sides & 2 != 0 {
                row_paint_cprun(row, w, d.x + d.w - 1, d.x + d.w, 0x2502, fg, true);
            }
            // 角（cp のみ上書き。fg は辺で既に塗られている前提の C 規約）
            if sides & 1 != 0 && sides & 8 != 0 && r == y0 && d.x >= 0 && d.x < w {
                row[d.x as usize].cp = 0x250C;
            }
            if sides & 1 != 0 && sides & 2 != 0 && r == y0 && d.x + d.w > 0 && d.x + d.w - 1 < w {
                row[(d.x + d.w - 1) as usize].cp = 0x2510;
            }
            if sides & 4 != 0 && sides & 8 != 0 && r == y1 && d.x >= 0 && d.x < w {
                row[d.x as usize].cp = 0x2514;
            }
            if sides & 4 != 0 && sides & 2 != 0 && r == y1 && d.x + d.w > 0 && d.x + d.w - 1 < w {
                row[(d.x + d.w - 1) as usize].cp = 0x2518;
            }
        }
        DecoKind::Hline => {
            row_paint_cprun(row, w, d.x, d.x + d.w, 0x2500, CELL_DEFAULT, false);
        }
        DecoKind::Marker => {
            let p = pen_raw(d.m_color, d.m_bg, d.m_flags);
            row_paint_text(row, w, d.x, &d.text[..d.tlen as usize], p);
        }
    }
}

fn grid_max_walk(b: &BoxNode, mx: &mut i32, my: &mut i32) {
    if b.kind == BoxKind::Line {
        if b.x + b.w > *mx {
            *mx = b.x + b.w;
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
        render_emit_sweep(&dom, &lay, ansi)
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
        let out = render_emit_sweep(&dom, &lay, false);
        let s = String::from_utf8(out).unwrap();
        // hr 行は ─ で埋まる（body ml=1 なので x=1 から）
        assert!(s.trim().starts_with("────────────────"), "got: {s:?}");
        let (mx, my) = render_extent(&lay);
        assert!(mx >= 40 && my >= 1);
    }

    #[test]
    fn li_marker_ul_ol() {
        // ul は "• "、ol は "N." の MARKER deco 経路（no-ansi fast の run 合流）
        let out = render(
            "<ul><li>a</li><li>b</li></ul><ol><li>c</li></ol>",
            30,
            false,
        );
        let s = String::from_utf8(out).unwrap();
        assert!(s.contains("• a"), "got: {s:?}");
        assert!(s.contains("• b"), "got: {s:?}");
        assert!(s.contains("1. c"), "got: {s:?}");
    }
}
