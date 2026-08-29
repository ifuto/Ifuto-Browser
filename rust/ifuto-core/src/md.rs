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

use crate::dom::{Attr, Dom, NodeId, NodeKind};
use crate::strutil::str_eq_ci;
use crate::tags::Tag;
use crate::tags_tables::{
    TAG_A, TAG_BLOCKQUOTE, TAG_BODY, TAG_BR, TAG_CODE, TAG_EM, TAG_H1, TAG_H2, TAG_H3, TAG_H4,
    TAG_H5, TAG_H6, TAG_HEAD, TAG_HR, TAG_HTML, TAG_IMG, TAG_LI, TAG_OL, TAG_P, TAG_PRE,
    TAG_SECTION, TAG_STRONG, TAG_STYLE, TAG_SUP, TAG_TABLE, TAG_TBODY, TAG_TD, TAG_TH, TAG_THEAD,
    TAG_TR, TAG_UL,
};

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

/// 変換エンジンの出力先（C `md.c` の `Mo` 両 backend 共通動詞の相当品）。
///
/// string backend（[`StrOut`]）は従来どおりのバイト列を吐き、DOM 直構築 backend
/// （[`DomOut`]）は `Mo` の dom モードと同一の遷移で [`Dom`] を構築する。
/// taint 規約（T1..T5。C `md.c` 冒頭の契約コメント相当）は [`DomOut`] に集約する。
///
/// 呼び出し側（inline/block 層）はこの動詞集合だけを使うため、両 backend の
/// 定義により string 出力と DOM 出力の観測同値性が保たれる（C と同じ設計）。
trait Emit {
    /// `mo_open_push` 相当: 属性なし要素の open+push。
    fn open_push(&mut self, tag: Tag, name: &'static str);
    /// `mo_open_void` 相当: void 要素（hr/br/img 属性なし）。push しない。
    fn open_void(&mut self, tag: Tag, name: &'static str);
    /// `mo_open` 相当: pend（属性つき要素）開始。`attr` → `open_end`/`open_end_void`。
    fn open_pend(&mut self, tag: Tag, name: &'static str);
    /// `mo_attr` 相当: pend に属性を追加（dom は ≤4 個で切り詰め。C と同じ）。
    fn attr(&mut self, name: &'static str, value: &[u8]);
    /// `mo_open_end` 相当: pend を確定してスタックへ push。
    fn open_end(&mut self);
    /// `mo_open_end_void` 相当: void 要素として確定（push しない）。
    fn open_end_void(&mut self);
    /// `mo_close` 相当。
    fn close(&mut self, name: &str);
    /// `mo_text` 相当: 本文スライス（string は `&<>` escape、dom は ws-sink 判定つき
    /// run へ追加）。
    fn text(&mut self, s: &[u8]);
    /// `mo_text_ch` 相当: ブロック間の 1 文字（dom は ws-sink 判定つき run へ追加）。
    fn text_ch(&mut self, c: u8);
    /// `mo_raw_ch` 相当: escape リテラル等の「生 1 文字」（string も escape しない。
    /// dom は `'<'`/`'&'` で T1 taint：string 側に出すと文法外になる Input があるため）。
    fn raw_ch(&mut self, c: u8);
}

// ---- string backend（C の `Mo.is_dom=false`）----

/// 出力バッファ（C の string backend `Mo.str` 相当。`Vec<u8>` に直接追記）。
struct StrOut {
    buf: Vec<u8>,
}

impl StrOut {
    fn new() -> Self {
        StrOut { buf: Vec::new() }
    }
}

impl Emit for StrOut {
    fn open_push(&mut self, _tag: Tag, name: &'static str) {
        self.buf.push(b'<');
        self.buf.extend_from_slice(name.as_bytes());
        self.buf.push(b'>');
    }
    fn open_void(&mut self, _tag: Tag, name: &'static str) {
        self.open_push(_tag, name);
    }
    fn open_pend(&mut self, _tag: Tag, name: &'static str) {
        self.buf.push(b'<');
        self.buf.extend_from_slice(name.as_bytes());
    }
    fn attr(&mut self, name: &'static str, value: &[u8]) {
        // ` name="v"`（v は `&<"` escape。C `mo_attr` string 相当）
        self.buf.push(b' ');
        self.buf.extend_from_slice(name.as_bytes());
        self.buf.extend_from_slice(b"=\"");
        for &c in value {
            match c {
                b'&' => self.buf.extend_from_slice(b"&amp;"),
                b'<' => self.buf.extend_from_slice(b"&lt;"),
                b'"' => self.buf.extend_from_slice(b"&quot;"),
                _ => self.buf.push(c),
            }
        }
        self.buf.push(b'"');
    }
    fn open_end(&mut self) {
        self.buf.push(b'>');
    }
    fn open_end_void(&mut self) {
        self.buf.push(b'>');
    }
    fn close(&mut self, name: &str) {
        self.buf.extend_from_slice(b"</");
        self.buf.extend_from_slice(name.as_bytes());
        self.buf.push(b'>');
    }
    fn text(&mut self, s: &[u8]) {
        for &c in s {
            match c {
                b'&' => self.buf.extend_from_slice(b"&amp;"),
                b'<' => self.buf.extend_from_slice(b"&lt;"),
                b'>' => self.buf.extend_from_slice(b"&gt;"),
                _ => self.buf.push(c),
            }
        }
    }
    fn text_ch(&mut self, c: u8) {
        self.buf.push(c); // string backend は生（C `mo_text_ch` 相当）
    }
    fn raw_ch(&mut self, c: u8) {
        self.buf.push(c); // string backend は生（C `mo_raw_ch` 相当）
    }
}

// ---- DOM 直構築 backend（C の `Mo.is_dom=true`。fast-DOM）----

/// ノード数上限（C の `IF_MAX_DOM_NODES`。T5）。
const MAX_DOM_NODES: usize = 4 * 1000 * 1000;
/// 開いている要素スタックの上限（C の `Mo.stk[128]`。T3）。
const STK_CAP: usize = 128;

/// Markdown → DOM 直構築器。C `md.c` の `Mo`（dom モード）相当。
///
/// taint 規約（観測したら全体を `None` に倒し、呼び出し側が string backend →
/// HTML パーサの 2 段経路へフォールバックする。正しさは常に本パーサ側に集約）:
/// - T1. string 側が文法外になる生文字（escape リテラルの `'<'`/`'&'`）
/// - T2. 開いている `<a>` の内側で `<a>` を開く（adoption agency 発火で木が変わる）
/// - T3. ネスト上限 128 / close 不一致
/// - T4. 入力 NUL（トークナイザと意味が分かれる）→ エントリで早期 `None`
/// - T5. ノード数上限 `MAX_DOM_NODES`
///
/// # C との違い（所有権による構造的な改善）
///
/// C は text run を「借用モード（persistent 範囲の連続切片）/複製モード」で自動
/// 切替し、属性値も persistent 範囲外なら arena に定着させる。Rust では run を
/// 所有 `Vec<u8>` に常時複製し、TEXT/属性とも所有 `Vec<u8>` で確定する。借用可否の
/// 範囲管理（`mo_range`/`mo_persistent` 台帳）を構造的に排除する。
struct DomOut {
    dom: Dom,
    /// 現在の open 要素（`Mo.cur`）。
    cur: NodeId,
    /// 開いている要素スタック（先頭 2 要素は html/body。`Mo.stk`）。
    stk: Vec<NodeId>,
    /// text run アキュムレータ（C の借用/複製 run を単一の所有バッファに簡約）。
    run: Vec<u8>,
    tainted: bool,
    /// `IF_MD_F_SLIM_ATTRS`: 保持属性を A[href]/IMG[alt] に限定。
    slim_attrs: bool,
    /// pend（`g_pend` 相当。attr 呼出で最大 4 件まで蓄積）。
    pend_tag: Tag,
    pend_name: &'static str,
    pend_attrs: Vec<(&'static str, Vec<u8>)>,
}

impl DomOut {
    /// skeleton（document root + quirks=true + n_errors=1 + html/head/body。
    /// doctype なし。C `if_md_parse_fast_serial_f` の初期条件と同一）。
    fn new(slim_attrs: bool) -> Self {
        let mut dom = Dom::new(); // document root 1 ノード
        dom.quirks = true;
        dom.n_errors = 1;
        let html = dom.alloc_node(NodeKind::Element);
        {
            let n = dom.node_mut(html);
            n.tag = TAG_HTML;
            n.name = crate::dom::NameStr::from_static(b"html");
        }
        let root = dom.root;
        let head = dom.alloc_node(NodeKind::Element);
        {
            let n = dom.node_mut(head);
            n.tag = TAG_HEAD;
            n.name = crate::dom::NameStr::from_static(b"head");
        }
        let body = dom.alloc_node(NodeKind::Element);
        {
            let n = dom.node_mut(body);
            n.tag = TAG_BODY;
            n.name = crate::dom::NameStr::from_static(b"body");
        }
        let mut o = DomOut {
            dom,
            cur: body,
            stk: vec![html, body],
            run: Vec::new(),
            tainted: false,
            slim_attrs,
            pend_tag: 0,
            pend_name: "",
            pend_attrs: Vec::new(),
        };
        o.attach(root, html);
        o.attach(html, head);
        o.attach(html, body);
        o
    }

    /// C `mattach` 相当（子を末尾に接続。parent の浅い値は既に正しい）。
    fn attach(&mut self, parent: NodeId, child: NodeId) {
        self.dom.node_mut(child).parent = Some(parent);
        match self.dom.node(parent).last_child {
            Some(last) => self.dom.node_mut(last).next_sibling = Some(child),
            None => self.dom.node_mut(parent).first_child = Some(child),
        }
        self.dom.node_mut(parent).last_child = Some(child);
    }

    /// C `mnew` 相当（T5: 上限到達で taint して `None`）。
    fn mnew(&mut self, kind: NodeKind) -> Option<NodeId> {
        if self.dom.nodes.len() >= MAX_DOM_NODES {
            self.tainted = true;
            return None;
        }
        Some(self.dom.alloc_node(kind))
    }

    /// C `run_flush` 相当（run を TEXT ノードとして cur 直下に確定）。
    /// tainted 後の中間状態は呼び出し側が必ず `None` に倒すので、run は捨てる。
    fn run_flush(&mut self) {
        if self.run.is_empty() {
            return;
        }
        if self.tainted {
            self.run.clear();
            return;
        }
        // フェーズ 11: 本文は Dom::set_node_text に集約（>22B は Dom の text_arena
        // へ bump 追記 + (off,len) 転用。旧 path はテキストノード 1 枚ごとに
        // 1 malloc = 16MB md で ~60 万回の alloc 交通だった。run の確保量は
        // clear で再利用するのは従来どおり）。
        let Some(nid) = self.mnew(NodeKind::Text) else {
            return;
        };
        self.dom.set_node_text(nid, &self.run);
        self.run.clear();
        let cur = self.cur;
        self.attach(cur, nid);
    }

    /// C `mo_ws_sink` 相当: cur が「純ブロック容器」（直下の ws-only TEXT は描画に
    /// 寄与しない）なら true。集合は `layout::ws_sink_parent` と完全一致が規約。
    fn ws_sink(&self) -> bool {
        matches!(
            self.dom.node(self.cur).tag,
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

    /// C `mo_ws_sink` 判定用の ws 集合（` \n\t\r\f`）。
    fn is_ws(c: u8) -> bool {
        matches!(c, b' ' | b'\n' | b'\t' | b'\r' | 0x0C)
    }

    /// C `mo_elem_store` 相当（pend の確定。push=false なら void 要素）。
    fn elem_store(&mut self, push: bool) {
        self.run_flush(); // flush は mode≠0 のときだけ（C と同じ順序）
        if self.tainted {
            return;
        }
        let Some(nid) = self.mnew(NodeKind::Element) else {
            return;
        };
        let tag = self.pend_tag;
        {
            let slim = self.slim_attrs;
            let pend_attrs = std::mem::take(&mut self.pend_attrs);
            let name = crate::dom::NameStr::from_bytes(self.pend_name.as_bytes());
            {
                let n = self.dom.node_mut(nid);
                n.tag = tag;
                n.name = name;
            }
            if !pend_attrs.is_empty() {
                let filtered: Vec<Attr> = pend_attrs
                    .iter()
                    .filter(|(an, _)| {
                        // slim: レンダリング経路で読まれる属性のみ保持（C と同一規約）
                        !slim || (tag == TAG_A && *an == "href") || (tag == TAG_IMG && *an == "alt")
                    })
                    .map(|(an, av)| Attr {
                        name: an.as_bytes().to_vec(),
                        value: av.clone(),
                    })
                    .collect();
                self.dom.set_attrs(nid, filtered);
            }
        }
        if tag == TAG_STYLE {
            self.dom.has_style = true; // md emitter は生成しないが規約として監視
        }
        let cur = self.cur;
        self.attach(cur, nid);
        if push {
            if self.stk.len() >= STK_CAP {
                self.tainted = true; // T3
                return;
            }
            self.stk.push(nid);
            self.cur = nid;
        }
    }
}

impl Emit for DomOut {
    fn open_push(&mut self, tag: Tag, name: &'static str) {
        if self.tainted {
            return;
        }
        // T2 対象外タグ専用（C の `mo_open_push` は T2 検査を持たない）。
        debug_assert!(tag != TAG_A);
        self.run_flush();
        let Some(nid) = self.mnew(NodeKind::Element) else {
            return;
        };
        {
            let n = self.dom.node_mut(nid);
            n.tag = tag;
            n.name = crate::dom::NameStr::from_bytes(name.as_bytes());
        }
        let cur = self.cur;
        self.attach(cur, nid);
        if self.stk.len() >= STK_CAP {
            self.tainted = true; // T3
            return;
        }
        self.stk.push(nid);
        self.cur = nid;
    }

    fn open_void(&mut self, tag: Tag, name: &'static str) {
        if self.tainted {
            return;
        }
        self.run_flush();
        let Some(nid) = self.mnew(NodeKind::Element) else {
            return;
        };
        {
            let n = self.dom.node_mut(nid);
            n.tag = tag;
            n.name = crate::dom::NameStr::from_bytes(name.as_bytes());
        }
        let cur = self.cur;
        self.attach(cur, nid);
    }

    fn open_pend(&mut self, tag: Tag, name: &'static str) {
        if tag == TAG_A && !self.tainted {
            // T2: 開いている <a> の内側で <a> を開くと fallback。
            if self.stk.iter().any(|&k| self.dom.node(k).tag == TAG_A) {
                self.tainted = true;
            }
        }
        self.pend_tag = tag;
        self.pend_name = name;
        self.pend_attrs.clear();
    }

    fn attr(&mut self, name: &'static str, value: &[u8]) {
        if self.pend_attrs.len() < 4 {
            // C `MoPend.an/av[4]` の切り詰め（超過分は黙って捨てる）
            self.pend_attrs.push((name, value.to_vec()));
        }
    }

    fn open_end(&mut self) {
        self.elem_store(true);
    }

    fn open_end_void(&mut self) {
        self.elem_store(false);
    }

    fn close(&mut self, name: &str) {
        self.run_flush();
        if self.tainted {
            return;
        }
        let Some(&top) = self.stk.last() else {
            self.tainted = true; // 到達不能のはず（emitter は常に対応させる。C と同じ）
            return;
        };
        if *self.dom.node(top).name != *name.as_bytes() {
            self.tainted = true;
            return;
        }
        self.stk.pop();
        self.cur = *self.stk.last().unwrap_or(&self.dom.root);
    }

    fn text(&mut self, s: &[u8]) {
        if self.tainted || s.is_empty() {
            return;
        }
        if self.ws_sink() && s.iter().all(|&c| Self::is_ws(c)) {
            // 純ブロック容器直下の ws-only chunk は chunk 単位で棄却（描画不寄与。INV）
            return;
        }
        self.run.extend_from_slice(s);
    }

    fn text_ch(&mut self, c: u8) {
        if self.tainted {
            return;
        }
        if self.ws_sink() && Self::is_ws(c) {
            // 純ブロック容器直下の ws-only 断片は DOM 化しない（C と同じ）
            return;
        }
        self.run.push(c);
    }

    fn raw_ch(&mut self, c: u8) {
        if self.tainted {
            return;
        }
        if c == b'<' || c == b'&' {
            self.tainted = true; // T1: 文字列側が文法外になる
            return;
        }
        self.run.push(c);
    }
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
    while b > a && (s[b - 1] == b' ' || s[b - 1] == b'\t' || s[b - 1] == b'\n' || s[b - 1] == b'\r')
    {
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
/// 次の特殊文字位置（無ければ `s.len()`）。C の `scan_special` 相当。
/// チャンク判定 + チャンク内位置確定の 2 段構成（C の SSE2/AVX2 版 `scan_special_*`
/// と同じく「まとめて棄却」を主経路にする。IS_SPECIAL_LUT の 256B 直表で分岐を消す）。
fn scan_special(s: &[u8], from: usize) -> usize {
    let mut i = from;
    let n = s.len();
    // 32B チャンク: 特殊文字が無ければ一括で進める（本文の大部分）
    while i + 32 <= n {
        let mut any = false;
        for &b in &s[i..i + 32] {
            any |= IS_SPECIAL_LUT[b as usize];
        }
        if any {
            break;
        }
        i += 32;
    }
    while i < n && !IS_SPECIAL_LUT[s[i] as usize] {
        i += 1;
    }
    i
}

/// 特殊文字直表（`is_special` の分岐連鎖を 1 回の LUT 参照に置き換える。
/// 集合は `is_special` と厳密に同一）。非 const fn の matches! から機械生成した不変表。
static IS_SPECIAL_LUT: [bool; 256] = {
    let mut t = [false; 256];
    let specials: &[u8] = b"\\`*_~![<&>";
    let mut i = 0;
    while i < specials.len() {
        t[specials[i] as usize] = true;
        i += 1;
    }
    t
};

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
fn try_link<E: Emit>(out: &mut E, fn_: &mut Fn, s: &[u8], i: usize) -> Option<usize> {
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
        out.open_push(TAG_SUP, "sup");
        out.open_pend(TAG_A, "a");
        out.attr("href", hrv.as_bytes());
        out.attr("id", idv.as_bytes());
        out.open_end();
        out.text(num.to_string().as_bytes()); // C: dom では mo_text(nb)、string は b_puts
        out.close("a");
        out.close("sup");
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
    out.open_pend(TAG_A, "a");
    out.attr("href", dest);
    out.open_end();
    inline_span(out, fn_, text);
    out.close("a");
    Some(k + 1)
}

fn inline_span<E: Emit>(out: &mut E, fn_: &mut Fn, s: &[u8]) {
    let mut i = 0;
    while i < s.len() {
        let sp0 = scan_special(s, i);
        if sp0 > i {
            out.text(&s[i..sp0]);
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
                        b'\\'
                            | b'`'
                            | b'*'
                            | b'_'
                            | b'{'
                            | b'}'
                            | b'['
                            | b']'
                            | b'('
                            | b')'
                            | b'#'
                            | b'+'
                            | b'-'
                            | b'.'
                            | b'!'
                            | b'~'
                            | b'|'
                            | b'<'
                            | b'>'
                    ) {
                        out.raw_ch(n2);
                        i += 2;
                        continue;
                    }
                }
                out.text(&s[i..i + 1]);
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
                    s[i + run..]
                        .iter()
                        .position(|&c| c == b'`')
                        .map(|p| p + i + run)
                };
                match close_pos {
                    None => {
                        out.text(&s[i..i + 1]);
                        i += 1;
                    }
                    Some(cl) => {
                        out.open_pend(TAG_CODE, "code");
                        out.open_end();
                        out.text(&s[i + run..cl]);
                        out.close("code");
                        i = cl + run;
                    }
                }
            }
            b'*' | b'_' => {
                if i + 1 < s.len() && s[i + 1] == c {
                    let delim = if c == b'*' {
                        b"**".as_slice()
                    } else {
                        b"__".as_slice()
                    };
                    if let Some(cl) = find_close(s, i + 2, delim) {
                        if cl > i + 2 {
                            out.open_push(TAG_STRONG, "strong");
                            inline_span(out, fn_, &s[i + 2..cl]);
                            out.close("strong");
                            i = cl + 2;
                            continue;
                        }
                    }
                }
                let delim = if c == b'*' {
                    b"*".as_slice()
                } else {
                    b"_".as_slice()
                };
                if let Some(cl) = find_close(s, i + 1, delim) {
                    if cl > i + 1 {
                        out.open_push(TAG_EM, "em");
                        inline_span(out, fn_, &s[i + 1..cl]);
                        out.close("em");
                        i = cl + 1;
                        continue;
                    }
                }
                out.text(&s[i..i + 1]);
                i += 1;
            }
            b'~' => {
                if i + 1 < s.len() && s[i + 1] == b'~' {
                    if let Some(cl) = find_close(s, i + 2, b"~~") {
                        if cl > i + 2 {
                            // C は del を IF_TAG_UNKNOWN で出す（タグ表に del は無い）
                            out.open_push(crate::tags::TAG_UNKNOWN, "del");
                            inline_span(out, fn_, &s[i + 2..cl]);
                            out.close("del");
                            i = cl + 2;
                            continue;
                        }
                    }
                }
                out.text(&s[i..i + 1]);
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
                            out.open_pend(TAG_IMG, "img");
                            out.attr("src", &rest[ds..ke]);
                            out.attr("alt", &rest[1..k]);
                            out.open_end_void();
                            adv0 = ke + 1;
                        }
                    }
                    if adv0 != 0 {
                        i = i + 1 + adv0;
                        continue;
                    }
                }
                out.text(&s[i..i + 1]);
                i += 1;
            }
            b'[' => {
                if let Some(adv) = try_link(out, fn_, &s[i..], 0) {
                    i += adv;
                } else {
                    out.text(&s[i..i + 1]);
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
                        out.open_pend(TAG_A, "a");
                        out.attr("href", url);
                        out.open_end();
                        out.text(url);
                        out.close("a");
                        i = j + 1;
                        continue;
                    }
                }
                out.text(&s[i..i + 1]);
                i += 1;
            }
            _ => {
                out.text(&s[i..i + 1]);
                i += 1;
            }
        }
    }
}

// ================= ブロック層 =================

/// 段落行を連結（ハードブレーク対応）。C の `emit_para_lines` 相当。
fn emit_para_lines<E: Emit>(out: &mut E, fn_: &mut Fn, ls: &[Ln], lo: usize, hi: usize) {
    out.open_push(TAG_P, "p");
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
                out.open_void(TAG_BR, "br");
            } else {
                out.text_ch(b' ');
            }
        }
        inline_span(out, fn_, x);
        prev_hard = hard;
    }
    out.close("p");
    out.text_ch(b'\n');
}

fn blocks_win<E: Emit>(out: &mut E, fn_: &mut Fn, ls: &[Ln], lo: usize, hi: usize, depth: usize) {
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
                const HTAG: [Tag; 6] = [TAG_H1, TAG_H2, TAG_H3, TAG_H4, TAG_H5, TAG_H6];
                let nm = HNM[hh - 1];
                out.open_push(HTAG[hh - 1], nm);
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
                out.close(nm);
                out.text_ch(b'\n');
                i += 1;
                continue;
            }
        }
        // hr
        if (cs == b'-' || cs == b'*' || cs == b'_' || cs == b'\t') && ln_is_hr(l) {
            out.open_void(TAG_HR, "hr");
            out.text_ch(b'\n');
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
            out.open_push(TAG_PRE, "pre");
            out.open_pend(TAG_CODE, "code");
            if !lang.is_empty() {
                let cv = format!("lang-{}", String::from_utf8_lossy(lang));
                out.attr("class", cv.as_bytes());
            }
            out.open_end();
            i += 1;
            while i < hi {
                let cl = ls[i];
                if let Some((s2, _)) = ln_fence(cl) {
                    if s2 == fsym {
                        i += 1;
                        break;
                    }
                }
                out.text(cl);
                out.text_ch(b'\n');
                i += 1;
            }
            out.close("code");
            out.close("pre");
            out.text_ch(b'\n');
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
                let wq: Vec<Ln> = (i..j)
                    .map(|k| {
                        let w = ln_quote(ls[k]);
                        &ls[k][w..]
                    })
                    .collect();
                out.open_push(TAG_BLOCKQUOTE, "blockquote");
                out.text_ch(b'\n');
                blocks_win(out, fn_, &wq, 0, cnt, depth + 1);
                out.close("blockquote");
                out.text_ch(b'\n');
            } else {
                // 深度飽和: flatten
                let mut flat = Vec::new();
                for &line in &ls[i..j] {
                    let w = ln_quote(line);
                    flat.extend_from_slice(&line[w..]);
                    flat.push(b'\n');
                }
                out.open_push(TAG_BLOCKQUOTE, "blockquote");
                out.text_ch(b'\n');
                out.open_push(TAG_P, "p");
                inline_span(out, fn_, &flat);
                out.close("p");
                out.text_ch(b'\n');
                out.close("blockquote");
                out.text_ch(b'\n');
            }
            i = j;
            continue;
        }
        // list
        if (cs == b'-' || cs == b'*' || cs == b'+' || cs.is_ascii_digit())
            && ln_list_item(l).is_some()
        {
            let mk = ln_list_item(l).unwrap();
            let ordered = mk.ordered;
            let base = mk.indent;
            out.open_push(
                if ordered { TAG_OL } else { TAG_UL },
                if ordered { "ol" } else { "ul" },
            );
            out.text_ch(b'\n');
            while i < hi {
                match ln_list_item(ls[i]) {
                    Some(m2) if m2.ordered == ordered && m2.indent == base => {
                        out.open_push(TAG_LI, "li");
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
                        out.close("li");
                        out.text_ch(b'\n');
                    }
                    _ => break,
                }
            }
            out.close(if ordered { "ol" } else { "ul" });
            out.text_ch(b'\n');
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
            out.open_push(TAG_TABLE, "table");
            out.text_ch(b'\n');
            out.open_push(TAG_THEAD, "thead");
            out.open_push(TAG_TR, "tr");
            for head in &heads[..nh] {
                out.open_push(TAG_TH, "th");
                inline_span(out, fn_, head);
                out.close("th");
            }
            out.close("tr");
            out.close("thead");
            out.text_ch(b'\n');
            out.open_push(TAG_TBODY, "tbody");
            out.text_ch(b'\n');
            i += 2;
            while i < hi && !ln_blank(ls[i]) {
                if !ls[i].contains(&b'|') {
                    break;
                }
                let mut cells = Vec::new();
                let nc = split_cells(ls[i], &mut cells).min(32);
                out.open_push(TAG_TR, "tr");
                for cell in &cells[..nc] {
                    out.open_push(TAG_TD, "td");
                    inline_span(out, fn_, cell);
                    out.close("td");
                }
                out.close("tr");
                out.text_ch(b'\n');
                i += 1;
            }
            out.close("tbody");
            out.text_ch(b'\n');
            out.close("table");
            out.text_ch(b'\n');
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
                    || ((xcs == b'-' || xcs == b'*' || xcs == b'_' || xcs == b'\t') && ln_is_hr(x))
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

fn blocks_str<E: Emit>(out: &mut E, fn_: &mut Fn, s: &[u8], depth: usize) {
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
    let mut out = StrOut::new();
    let mut fn_ = Fn::new();
    run_blocks(&mut out, &mut fn_, input);
    out.buf
}

/// 高速経路: Markdown → DOM 直構築。C の `if_md_parse_fast_f` 相当。
///
/// taint（T1..T5）を観測したら `None` を返して中間物を捨てる（呼び出し側が
/// `md_to_html` + `parse_html` の従来 2 段経路で処理する。正しさは常に本パーサ側に
/// 集約）。成功時の `Dom` は `md_ws_stripped=true`（純ブロック容器直下の ws-only
/// TEXT を意図的に剥がした DOM。layout が同値補正を行う）。
///
/// `slim_attrs=true`（C の `IF_MD_F_SLIM_ATTRS`）で保持属性を A[href]/IMG[alt] に
/// 限定する（レンダリング経路専用の最適化。観測点経路は false を渡す）。
pub fn md_to_dom_opts(input: &[u8], slim_attrs: bool) -> Option<Dom> {
    // T4: input NUL はトークナイザと意味が分かれるので fallback
    if input.contains(&0) {
        return None;
    }
    // 並列 2-slice（C の `if_md_parse_fast_f` 並列経路の写し）。分割点の安全性
    // 条件は `md_par_scan` が機械検査し、接合規約は C と同一 — 生成 DOM は
    // 単走査と逐語同値（C の oracle sha256 と同じ検証盤を差分 fuzz でも固定）。
    if input.len() >= (1 << 20) && md_par_on() && !input.contains(&b'\r') {
        let t_scan = std::time::Instant::now();
        let sp = md_par_scan(input);
        if std::env::var_os("IF_MD_PROF").is_some() {
            eprintln!(
                "[mdprof] scan={:.2}ms",
                t_scan.elapsed().as_secs_f64() * 1e3
            );
        }
        if let Some(split) = sp {
            if split > 0 {
                return md_to_dom_2slice(input, slim_attrs, split);
            }
        }
    }
    md_to_dom_opts_serial(input, slim_attrs)
}

/// 単走査の DOM 直構築（C の `if_md_parse_fast_serial_f` 相当）。
fn md_to_dom_opts_serial(input: &[u8], slim_attrs: bool) -> Option<Dom> {
    let mut out = DomOut::new(slim_attrs);
    // ノード数は入力にほぼ比例する（16MB md ≒ 1.6M）。倍増成長の move 収支
    // （最終量の ~2 倍コピー）を 1 回の予約で消す。余剰時の無駄を抑えるため
    // 保守的な係数（~10B/ノード）を使う。
    out.dom.nodes.reserve(input.len() / 10);
    // フェーズ 11: text_arena も予約（実測で md 本文 ~48% が >22B の長文として
    // arena に入る = 入力の ~1/2。bump 倍増の realloc コピー税を 1 回の予約で
    // 構造消去。不足時は従来どおり倍増、過剰分は未タッチで RSS 無害）。
    out.dom.reserve_text_arena(input.len() / 2);
    let mut fn_ = Fn::new();
    run_blocks(&mut out, &mut fn_, input);
    out.run_flush();
    if out.tainted {
        return None;
    }
    out.dom.md_ws_stripped = true;
    // 解析時カウンタの凍結（C `dom->n_nodes` = mo_node 計数と同集合。ws-sink で
    // 生成自体を剥がした TEXT は両実装とも不算入）
    out.dom.n_nodes = out.dom.nodes.len() as u32;
    Some(out.dom)
}

/// `md_to_dom_opts(input, false)` の短絡形（観測点経路。slim なし）。
pub fn md_to_dom(input: &[u8]) -> Option<Dom> {
    md_to_dom_opts(input, false)
}

/// `from` 以降で最初の `b` の位置（SWAR: 8B 語の zero-byte 検出。std 縛りで
/// memchr crate / SIMD intrinsic が使えないための safe 代替。C の SIMD 走査相当）。
fn find_byte(s: &[u8], from: usize, b: u8) -> Option<usize> {
    const HIBITS: u64 = 0x8080_8080_8080_8080;
    const LOBITS: u64 = 0x0101_0101_0101_0101;
    let n = s.len();
    let mut i = from;
    let rep = u64::from_ne_bytes([b; 8]);
    while i + 8 <= n {
        let w = u64::from_ne_bytes(s[i..i + 8].try_into().unwrap());
        let x = w ^ rep;
        if x.wrapping_sub(LOBITS) & !x & HIBITS != 0 {
            let mut j = i;
            while j < i + 8 {
                if s[j] == b {
                    return Some(j);
                }
                j += 1;
            }
        }
        i += 8;
    }
    while i < n {
        if s[i] == b {
            return Some(i);
        }
        i += 1;
    }
    None
}

/// 並列スイッチ。既定は **論理 CPU ≥ 2 なら ON**（10-g の drain/scan 撲滅で 2HT
/// 環境でも採算が黒字化したことを bench で固定。1 HT 環境では serial に落ちるので
/// かつての大損（merge 税 +48ms）経路は踏まない）。殺しスイッチ `IF_MD_PAR=0`、
/// 強制 ON は `IF_MD_PAR=1`（C の規約と同一）。観測挙動（byte 列）は不変。
fn md_par_on() -> bool {
    match std::env::var_os("IF_MD_PAR") {
        Some(v) => v == "1",
        None => std::thread::available_parallelism().is_ok_and(|x| x.get() >= 2),
    }
}

/// C の `md_par_scan` 写し: 並列 2-slice の安全な分割点（文書中央以降で、直前行が
/// 空行・当該行が fence 外の非空行である最初のブロック境界。byte オフセット）を返す。
/// 安全性の前提は C と同一規則で本関数内でも拒否する:
/// - `[^` が文書のどこかに在れば分割放棄（footnote 番号付け規約が半区間で変わるため）
/// - fence（```/~~~）の内側は分割不可（fence 状態を入口側から追跡）
/// - 分割候補は `off >= n/2`、直前行空行、当該行は非空かつ非 fence 開始
fn md_par_scan(s: &[u8]) -> Option<usize> {
    let n = s.len();
    // `[^` の大域拒否（C は SIMD 部+scalar 尾部の全走査で拒否。条件は同一）
    {
        let mut i = 1usize;
        while i < n {
            match find_byte(s, i, b'^') {
                Some(at) => {
                    if s[at - 1] == b'[' {
                        return None;
                    }
                    i = at + 1;
                }
                None => break,
            }
        }
    }
    let mut in_fence = false;
    let mut fsym = 0u8;
    let mut prev_blank = false;
    let mut off = 0usize;
    while off < n {
        let e = find_byte(s, off, b'\n').unwrap_or(n);
        let l = &s[off..e];
        let blank = ln_blank(l);
        if in_fence {
            if !blank {
                if let Some((s2, _)) = ln_fence(l) {
                    if s2 == fsym {
                        in_fence = false;
                    }
                }
            }
        } else if !blank {
            if let Some((s2, _)) = ln_fence(l) {
                in_fence = true;
                fsym = s2;
            } else if prev_blank && off >= n / 2 {
                return Some(off);
            }
        }
        prev_blank = blank;
        off = e + 1;
    }
    None
}

/// 2-slice 並列 DOM 直構築（C の `MdSliceJob` 接合経路の写し）。
/// A 側 [0, split) と B 側 [split, len) を同一パーサで独立に走らせ、body 子列を
/// 鎖の継ぎ目 2 書きで O(1) 接合する。NodeId は B 側全リンクをオフセット写像する。
/// Fn（footnote 台帳）は側ごとに新規 = C と同一（`[^` 拒否済みのため観測差なし）。
fn md_to_dom_2slice(input: &[u8], slim_attrs: bool, split: usize) -> Option<Dom> {
    // 分解計測（設計判断用・IF_MD_PROF=1 のときのみ stderr。通常経路は無出力）
    let prof = std::env::var_os("IF_MD_PROF").is_some();
    let tp = |label: &str, t0: std::time::Instant| {
        if prof {
            eprintln!("[mdprof] {label}={:.2}ms", t0.elapsed().as_secs_f64() * 1e3);
        }
        std::time::Instant::now()
    };
    let mut t = std::time::Instant::now();
    let b_input = &input[split..];
    // scaffold の id 規約（DomOut::new と一致。root=0, html=1, head=2, body/stub=3、
    // 内容ノードは 4 以降が DFS 順で連続）。B 側の stub body 直下は parent=3 となって
    // おり、A 側 body も id=3 なので写像は stub→body で恒等に写る。
    const SKIP: u32 = 4;
    let b_res = std::thread::scope(|sc| {
        let hb = sc.spawn(|| {
            let mut out = DomOut::new(slim_attrs);
            out.dom.nodes.reserve(b_input.len() / 10);
            out.dom.reserve_text_arena(b_input.len() / 2);
            let mut fn_ = Fn::new();
            run_blocks(&mut out, &mut fn_, b_input);
            out.run_flush();
            (out.dom, out.tainted)
        });
        let mut out_a = DomOut::new(slim_attrs);
        // 10-g: 全文書係数で予約し、merge 時の成長（134MB 再確保+コピー）を構造消去
        // する（serial と同じ ~10B/ノード見積。B 内容を接合しても capacity は足りる）。
        out_a.dom.nodes.reserve(input.len() / 10);
        // 11: B の arena を吸収する A 側は全文書係数で 1 回予約（splice 時の
        // concat 倍増を消す。実測係数 ~1/2）。
        out_a.dom.reserve_text_arena(input.len() / 2);
        let mut fn_a = Fn::new();
        run_blocks(&mut out_a, &mut fn_a, &input[..split]);
        out_a.run_flush();
        let j = match hb.join() {
            Ok(v) => v,
            Err(e) => std::panic::resume_unwind(e),
        };
        let tainted = out_a.tainted || j.1;
        (out_a.dom, j.0, tainted)
    });
    t = tp("parse", t);
    let (mut dom_a, mut dom_b, tainted) = b_res;
    let total = dom_a.nodes.len() + dom_b.nodes.len() - SKIP as usize;
    if tainted || total >= MAX_DOM_NODES {
        return None; // 2 段経路が同じ結論へ至る（T5 含む。C と同じ）
    }
    // B 内容ノードの転記とリンク写像。parent==stub(3) は A 側 body=3 と同一数値で
    // 恒等に写る（first_child/next_sibling/parent の全 Option<NodeId> が対象）
    let base = dom_a.nodes.len() as u32;
    // 副テーブル（attrs / extra）も併合し、B 側ハンドルにデルタを足す
    // （ハンドル 0 は「無し」で不変。C の arena 共有 O(1) 接合に対応する部分）。
    let (d_attrs, d_extra) = dom_a.merge_side_from(&mut dom_b);
    t = tp("side", t);
    let (bf0, bl0) = (dom_b.nodes[3].first_child, dom_b.nodes[3].last_child);
    if dom_b.nodes.len() > SKIP as usize {
        // 10-g: 旧 per-item push（+ mid-loop 成長）を撲滅。capacity は入口の全文書
        // 予約で保証済み → remap を移動ループに融合し B を 1 パスだけ読む。
        // 分岐の内訳: first_child/next_sibling には scaffold id（0..4）が絶対に現れ得
        // ない（scaffold は root/html/head/body で、内容ノードの子・兄弟にはならない
        // = serial と同じ構造規約）ため stub 判定が要るのは parent のみ。
        // 余計な中間確保（split_off）は作らない: unsafe なしで [SKIP..] をそのまま
        // 流し込める最速の safe 経路は drain→extend（TrustedLen で予約は増分ゼロ。
        // 実測: in-place remap+pure drain=38.5ms、split_off+append=77.6ms、fused は
        // 30ms で 3 変種中最速。V3 は split_off の中間 65MB 確保+copy が致命）。
        let d = base - SKIP;
        const STUB: u32 = SKIP - 1;
        // フェーズ 11: B 側 text_arena を連結し、B の Text 転用 pun
        // （kind==Text && extra_idx!=0 のとき attrs_idx=arena off / extra_idx=len）
        // は off に arena_delta を足す（len は平行不変）。pun ノードには
        // attrs_tab/extra_tab の remap を効かせてはいけないため算術で排他化
        // （t=1: +=arena_delta のみ / t=0: 従来 remap。分岐は入れない）。
        let arena_delta = dom_a.text_arena.len() as u32;
        dom_a.text_arena.extend_from_slice(&dom_b.text_arena);
        // 迷走分岐の撲滅: parent==STUB（body 直子の割合は入力依存で予測不能）と
        // attrs/extra の 0 判定（~38% 採取）は cmov 級の算術に置き換える。
        dom_a
            .nodes
            .extend(dom_b.nodes.drain(SKIP as usize..).map(|mut n| {
                if let Some(p) = n.parent {
                    debug_assert!(p == STUB || p >= SKIP);
                    n.parent = Some(p + d * u32::from(p != STUB));
                }
                n.first_child = n.first_child.map(|c| {
                    debug_assert!(c >= SKIP);
                    c + d
                });
                n.next_sibling = n.next_sibling.map(|s| {
                    debug_assert!(s >= SKIP);
                    s + d
                });
                let t = u32::from(n.kind == NodeKind::Text && n.extra_idx != 0);
                let s = 1 - t;
                n.attrs_idx += arena_delta * t + d_attrs * s * u32::from(n.attrs_idx != 0);
                n.extra_idx += d_extra * s * u32::from(n.extra_idx != 0);
                n
            }));
    }
    t = tp("drain", t);
    let bf = map_opt(bf0, base, SKIP);
    let bl = map_opt(bl0, base, SKIP);
    match (dom_a.node(3).first_child, bf) {
        (_, None) => {}
        (None, f) => {
            let b = dom_a.node_mut(3);
            b.first_child = f;
            b.last_child = bl;
        }
        (Some(_), f) => {
            let last = dom_a
                .node(3)
                .last_child
                .expect("first_child あるなら last もある");
            dom_a.node_mut(last).next_sibling = f;
            dom_a.node_mut(3).last_child = bl;
        }
    }
    tp("splice", t);
    dom_a.has_script |= dom_b.has_script;
    dom_a.has_style |= dom_b.has_style;
    dom_a.has_selectedcontent |= dom_b.has_selectedcontent;
    dom_a.md_ws_stripped = true;
    // 10-h: layout shard の二分ヒント（C の `dom->md_body_mid` 写し。body 直下の
    // B 側先頭子 = splice 境界。serial ≡ 2-slice の DOM 同値性を逸脱しない範囲の
    // 純粋な性能ヒントで、DOM そのものの観測値には影響しない）
    dom_a.md_body_mid = bf.unwrap_or(0);
    dom_a.n_nodes = dom_a.nodes.len() as u32;
    Some(dom_a)
}

/// B 側 NodeId の写像（stub(3) は A 側 body(3) へ恒等、内容ノードは base へ平行移動）。
fn map_opt(id: Option<NodeId>, base: u32, skip: u32) -> Option<NodeId> {
    id.map(|x| {
        if x == skip - 1 {
            x // stub body = A body（同一数値）
        } else {
            debug_assert!(x >= skip);
            x - skip + base
        }
    })
}

fn run_blocks<E: Emit>(out: &mut E, fn_: &mut Fn, input: &[u8]) {
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
        out.open_pend(TAG_SECTION, "section");
        out.attr("class", b"footnotes");
        out.open_end();
        out.text_ch(b'\n');
        out.open_void(TAG_HR, "hr");
        out.text_ch(b'\n');
        out.open_push(TAG_OL, "ol");
        out.text_ch(b'\n');
        let refs = fn_.refs.clone();
        for id in refs {
            let di = fn_.find_def(&id);
            let idv = format!("fn-{}", String::from_utf8_lossy(&id));
            let hrv = format!("#fr-{}", String::from_utf8_lossy(&id));
            out.open_pend(TAG_LI, "li");
            out.attr("id", idv.as_bytes());
            out.open_end();
            let txt = di.map_or(Vec::new(), |d| fn_.defs[d].1.clone());
            inline_span(out, fn_, &txt);
            out.text_ch(b' ');
            out.open_pend(TAG_A, "a");
            out.attr("href", hrv.as_bytes());
            out.open_end();
            out.text("\u{21A9}".as_bytes()); // ↩
            out.close("a");
            out.close("li");
            out.text_ch(b'\n');
        }
        out.close("ol");
        out.text_ch(b'\n');
        out.close("section");
        out.text_ch(b'\n');
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

    /// 2-slice ≡ 逐次の機械固定（`dom.dump()` の byte 比較）。
    /// fuzz の差分オラクルは 1MB 未満の文書で発動し、`md_to_dom_2slice` の与件
    /// （`input.len() >= 1MB`）には恒常的に届かないため、本テストが splice・
    /// NodeId 写像・Fn 独立性の唯一の直接検査となる。構築物は fence/list/
    /// blockquote/table/link/img/hr を断面に含む（分割点は fence 外の空行境界）。
    #[test]
    fn twoslice_equals_serial() {
        let block: &[u8] = b"# t\n\npara with *em* and `code` and [x](/u) tail\n\n- a\n- b\n\n> q\n\n| h1 | h2 |\n| --- | --- |\n| 1 | 2 |\n\n```rust\ncode block\n```\n\n![alt](/i.png)\n\n---\n\n";
        // Miri 下では 8KB に縮小（本テストは md_to_dom_2slice を直接呼ぶため
        // 1MB の dispatch 閾値は不関係。等価性を検査する論理経路は同一）。
        let target = if cfg!(miri) {
            (1 << 13) + 12345
        } else {
            (1 << 20) + 12345
        };
        let mut big = Vec::with_capacity(target + block.len());
        while big.len() < target {
            big.extend_from_slice(block);
        }
        assert!(md_par_scan(&big).is_some(), "分割点が見つかる構文のはず");
        let ser = md_to_dom_opts_serial(&big, true).expect("serial");
        let split = md_par_scan(&big).unwrap();
        let par = md_to_dom_2slice(&big, true, split).expect("2slice");
        assert_eq!(
            ser.dump(),
            par.dump(),
            "2-slice と逐次で DOM が逐語一致しなければならない"
        );
        // slim=false（全属性保持経路）でも同値
        let ser2 = md_to_dom_opts_serial(&big, false).expect("serial ns");
        let par2 = md_to_dom_2slice(&big, false, split).expect("2slice ns");
        assert_eq!(ser2.dump(), par2.dump());
    }

    /// 2-slice の安全性前提（`[^` 拒否）の機械固定: footnote 文書では
    /// `md_par_scan` が分割点を返さない（呼び出し側は逐次経路に落ちる。
    /// C の number 規約が半区間で変わるのを防ぐ拒否条件の写し）。
    #[test]
    fn par_scan_rejects_footnote() {
        let mut big = Vec::new();
        // Miri 下では 8KB に縮小（`[^` の大域拒否はサイズ不変の性質）。
        let target = if cfg!(miri) {
            (1 << 13) + 101
        } else {
            (1 << 20) + 101
        };
        while big.len() < target {
            big.extend_from_slice(b"para x\n\n");
        }
        big.extend_from_slice(b"has note[^a] here\n\n[^a]: def\n");
        assert!(md_par_scan(&big).is_none());
    }

    /// fence 内に「空行 + 非空行」の条件が揃っても分割しない（C と同じ前提で
    /// 安全性を機械固定。fence 全体が [n/2, n) を埋める配置）。
    #[test]
    fn par_scan_never_splits_inside_fence() {
        let mut big = Vec::new();
        // Miri 下では 8KB に縮小（fence 追跡・分割点条件の論理経路は同一）。
        let bound: usize = if cfg!(miri) { 1 << 13 } else { 1 << 20 };
        big.extend_from_slice(b"# head\n\n");
        while big.len() < bound / 2 {
            big.extend_from_slice(b"pre pad\n\n");
        }
        big.extend_from_slice(b"```\n");
        while big.len() < bound + 7777 {
            big.extend_from_slice(b"in code\n\n");
        }
        big.extend_from_slice(b"```\n");
        let split = md_par_scan(&big);
        if let Some(p) = split {
            // 分割が見つかる場合、それは fence の外でなければならない
            assert!(p >= big.len() || p <= bound / 2, "fence 内分割は禁物");
        }
    }

    /// フェーズ 11 ラチェット: >22B テキストは Dom の text_arena 転用
    /// （name=空, attrs_idx=off, extra_idx=len）で保持され `text_of` が解決
    /// する。短いテキストは従来どおり inline。機構が黙って退行しないよう
    /// 表現形式そのものを機械固定する（観測バイトの同値は oracle/fuzz が保証）。
    #[test]
    fn text_arena_representation() {
        let long = "x".repeat(64);
        let src = format!("# t\n\n{}\n\n短い\n", long);
        let dom = md_to_dom_opts(src.as_bytes(), true).expect("fastdom");
        let mut saw_arena = 0usize;
        let mut saw_inline = 0usize;
        let mut arena_payload = 0usize;
        for (i, n) in dom.nodes.iter().enumerate() {
            if n.kind != NodeKind::Text {
                continue;
            }
            let t = dom.text_of(i as u32);
            if n.extra_idx != 0 {
                saw_arena += 1;
                assert!(n.name.is_empty(), "arena 転用時 name は空規約");
                arena_payload += t.len();
                assert_eq!(n.extra_idx as usize, t.len(), "len 一致");
            } else {
                saw_inline += 1;
                assert_eq!(&*n.name, t, "inline は name に収容");
            }
            if t.len() == 64 {
                assert_eq!(t, long.as_bytes(), "本文 byte 一致");
                assert_ne!(n.extra_idx, 0, "64B は必ず arena 転用");
            }
        }
        assert!(saw_arena >= 1, "長文テキストが arena 転用されていること");
        assert!(saw_inline >= 1, "短文テキストが inline 収容であること");
        assert_eq!(dom.text_arena.len(), arena_payload, "arena に無駄がない");
    }
}
