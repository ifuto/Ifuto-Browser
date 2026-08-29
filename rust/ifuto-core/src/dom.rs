//! DOM ノードモデル（C の `src/dom.c` / `dom.h` 相当のデータ構造と純粋ヘルパ）。
//!
//! | C (dom.c / dom.h) | Rust |
//! |---|---|
//! | `IfNode`（arena 内ポインタ連結） | [`Node`] + [`NodeId`]（`Vec<Node>` index） |
//! | `IfNodeKind` | [`NodeKind`] |
//! | `IfDom` | [`Dom`] |
//! | `if_dom_attr` / `if_dom_has_class` | [`Dom::attr`] / [`Dom::has_class`] |
//! | `if_dom_text_content` | [`Dom::text_content`] |
//! | `if_dom_find_tag_dfs` / `if_dom_find_by_id` | [`Dom::find_tag_dfs`] / [`Dom::find_by_id`] |
//! | `if_dom_attr_set` | [`Dom::attr_set`] |
//!
//! # C との違い（所有権による構造的な改善）
//!
//! C はノードを arena から確保し、`parent/first_child/last_child/next_sibling` を
//! raw ポインタで連結する（手動の連結維持・dangling 防止）。Rust では JS エンジンで
//! 実証済みの **`NodeId` = `Vec<Node>` への index** パターンを踏襲し、木の連結を
//! `Option<NodeId>` で表現する。ポインタの寿命・エイリアシング問題が構造的に消える。
//! （C の `IfDom` が `arena` を所有するのに対し、Rust の [`Dom`] は `Vec<Node>` を
//! 所有する。）

use crate::strutil::str_eq_ci;
use crate::tags::{self, Tag};

/// 属性（トークナイザの `Attr` をそのまま共有。名前は ASCII lowercase 正規化済み）。
pub use crate::html_tok::Attr;

/// DOM ノードの index（C の `IfNode*` 相当。所有 `Vec<Node>` への index）。
pub type NodeId = u32;

/// ノード種別（C の `IfNodeKind` 相当）。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum NodeKind {
    /// 文書ノード。
    Document,
    /// 要素。
    Element,
    /// テキスト。
    Text,
    /// コメント（PI 含む）。
    Comment,
    /// DOCTYPE。
    Doctype,
}

/// 名前空間（C の `IF_NS_*` 相当）。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Ns {
    /// HTML。
    Html,
    /// SVG。
    Svg,
    /// MathML。
    Mathml,
}

/// 小文字列最適化つきの不変名バッファ（`Node::name` 用）。
///
/// なぜ `Vec<u8>` ではないのか: 16MB md で ~1.6M ノードの `Vec<u8>` 名は
/// ~1.6M 回の malloc + memcpy を要し、parse 段のアロケーション収支（922MB 実測、
/// alloc_probe 調査器）の主因だった。C は arena bump に積むだけ（0 malloc）。
/// `Vec` のままでは言語の標準型に思考停止して性能を燃やすことになる。
///
/// 実装上の性質:
/// - `Static`: タグ名等の `'static` バイト列（0 alloc。ELEMENT 名の既定経路）
/// - `Inline`（≤ [`NAME_INLINE_CAP`]B）: 短いテキスト（0 alloc）
/// - `Heap`: 長文（1 alloc。`Box<[u8]>` で 16B。二度縮めしない）
///
/// 読み側は `Deref<Target=[u8]>` で旧 `Vec<u8>` と同じインタフェース
/// （`&node.name` で `&[u8]` 化、`.len()/.as_ptr()/[i]` 等は autoderef が解決）。
#[derive(Clone, Debug)]
pub struct NameStr(NameInner);

#[derive(Clone, Debug)]
enum NameInner {
    Inline { len: u8, buf: [u8; NAME_INLINE_CAP] },
    Static(&'static [u8]),
    Heap(Box<[u8]>),
}

/// Inline 収容の上限（enum 全体を 24B = 旧 `Vec<u8>` と同サイズに収める境界）。
pub const NAME_INLINE_CAP: usize = 22;

impl NameStr {
    /// 空。
    pub fn empty() -> Self {
        NameStr(NameInner::Inline {
            len: 0,
            buf: [0; NAME_INLINE_CAP],
        })
    }

    /// スライスから構築（短ければ inline、長ければ 1 alloc）。
    pub fn from_bytes(b: &[u8]) -> Self {
        if b.len() <= NAME_INLINE_CAP {
            let mut buf = [0u8; NAME_INLINE_CAP];
            buf[..b.len()].copy_from_slice(b);
            NameStr(NameInner::Inline {
                len: b.len() as u8,
                buf,
            })
        } else {
            NameStr(NameInner::Heap(b.into()))
        }
    }

    /// 静的バイト列から構築（0 alloc。タグ名等）。
    pub fn from_static(b: &'static [u8]) -> Self {
        NameStr(NameInner::Static(b))
    }

    /// 既存 `Vec<u8>` の確保をそのまま採用（再コピーなし）。
    pub fn from_vec(v: Vec<u8>) -> Self {
        if v.len() <= NAME_INLINE_CAP {
            Self::from_bytes(&v)
        } else {
            NameStr(NameInner::Heap(v.into_boxed_slice()))
        }
    }

    /// スライス参照（旧 `Vec::as_slice` 互換）。
    pub fn as_slice(&self) -> &[u8] {
        self
    }
}

impl PartialOrd for NameStr {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for NameStr {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        (**self).cmp(&**other)
    }
}

impl Default for NameStr {
    fn default() -> Self {
        Self::empty()
    }
}

impl std::ops::Deref for NameStr {
    type Target = [u8];
    fn deref(&self) -> &[u8] {
        match &self.0 {
            NameInner::Inline { len, buf } => &buf[..*len as usize],
            NameInner::Static(s) => s,
            NameInner::Heap(b) => b,
        }
    }
}

impl AsRef<[u8]> for NameStr {
    fn as_ref(&self) -> &[u8] {
        self
    }
}

impl std::borrow::Borrow<[u8]> for NameStr {
    fn borrow(&self) -> &[u8] {
        self
    }
}

impl PartialEq for NameStr {
    fn eq(&self, other: &Self) -> bool {
        **self == **other
    }
}
impl Eq for NameStr {}

/// `name == b"literal"` 形の旧 Vec 比較互換。
impl<const N: usize> PartialEq<&[u8; N]> for NameStr {
    fn eq(&self, other: &&[u8; N]) -> bool {
        **self == other[..]
    }
}

impl PartialEq<NameStr> for &[u8] {
    fn eq(&self, other: &NameStr) -> bool {
        *self == &**other
    }
}

/// DOCTYPE 情報（C の `IfDoctype` 相当。`NodeKind::Doctype` のみ有意）。
#[derive(Clone, Debug, Default)]
pub struct Doctype {
    /// DOCTYPE 名（lowercase 済み）。
    pub name: Vec<u8>,
    /// 名前あり。
    pub has_name: bool,
    /// public identifier。
    pub pub_id: Vec<u8>,
    /// system identifier。
    pub sys_id: Vec<u8>,
}

/// 稀データ（`NodeKind::Doctype` の doctype 情報 / COMMENT が PI のときの target）。
/// ほぼ全ノードで不使用のため副テーブル（`Dom::extra_tab`）に隔離する。
#[derive(Clone, Debug, Default)]
pub struct NodeExtra {
    /// DOCTYPE 情報。
    pub doctype: Option<Doctype>,
    /// PI ターゲット（C は attrs[0].name に保持）。
    pub pi_target: Vec<u8>,
}

/// DOM ノード（C の `IfNode` 相当）。
///
/// 【痩身化（フェーズ 10-e）】200B → 80B。属性（`Vec<Attr>` 24B + ヒープ）と
/// 稀データ（doctype `Option<Doctype>` 80B + pi_target `Vec` 24B）は走査が追う
/// メモリ量を増やすだけで大多数のノードでは不使用なため、`Dom` 側の副テーブルへ
/// 隔離し、ノードには 4B ハンドルのみ持たせる（0 = 無し）。C の arena/ポインタ
/// 間接と同じ「熱い構造体を小さく」思想の safe 版。読み書きは `Dom::attrs` /
/// `Dom::attrs_mut` / `Dom::doctype` / `Dom::pi_target` 等のアクセサ経由。
#[derive(Clone, Debug)]
pub struct Node {
    /// 種別。
    pub kind: NodeKind,
    /// ELEMENT のみ有意（`tags::Tag`）。
    pub tag: Tag,
    /// ELEMENT: 名前空間。
    pub ns: Ns,
    /// フラグ（`IF_NF_*`。当面未使用）。
    pub flags: u8,
    /// タグ名（ELEMENT）/ テキスト（TEXT, COMMENT）。
    pub name: NameStr,
    /// 属性副テーブル index（0 = 属性なし。N → `Dom::attrs_tab[N]`）。
    ///
    /// 【フェーズ 11 転用規約】`NodeKind::Text` のみ本 2 フィールドの意味が変わる:
    /// テキストが `NAME_INLINE_CAP` を超えるとき `name` は空に倒し、
    /// `attrs_idx` = `Dom::text_arena` へのオフセット、`extra_idx` = テキスト長
    /// （≠ 0）として使う。Text は属性も稀データも持たないため `attrs_idx` /
    /// `extra_idx` は常に空き。解決は必ず `Dom::text_of` 経由（直接参照禁止）。
    pub(crate) attrs_idx: u32,
    /// 稀データ副テーブル index（0 = 稀データなし。N → `Dom::extra_tab[N]`）。
    /// Text では上記の転用規約によりテキスト長として使われる。
    pub(crate) extra_idx: u32,
    /// 親。
    pub parent: Option<NodeId>,
    /// 先頭の子。
    pub first_child: Option<NodeId>,
    /// 末尾の子。
    pub last_child: Option<NodeId>,
    /// 次の兄弟。
    pub next_sibling: Option<NodeId>,
    /// template 要素の content フラグメント（rare-data。C の `if_dom_tpl_content` 相当）。
    pub tpl_content: Option<NodeId>,
}

impl Node {
    /// 新規ノード。
    fn new(kind: NodeKind) -> Self {
        Node {
            kind,
            tag: tags::TAG_UNKNOWN,
            ns: Ns::Html,
            flags: 0,
            name: NameStr::empty(),
            attrs_idx: 0,
            extra_idx: 0,
            parent: None,
            first_child: None,
            last_child: None,
            next_sibling: None,
            tpl_content: None,
        }
    }
}

/// 文書（C の `IfDom` 相当）。
#[derive(Clone, Debug)]
pub struct Dom {
    /// 全ノード（`NodeId` はここへの index）。
    pub nodes: Vec<Node>,
    /// 属性副テーブル（`Node::attrs_idx` 指し先。index 0 は未割当の placeholder）。
    /// 遅延割当（属性を持つノードが初出の瞬間まで 0 確保）。
    pub(crate) attrs_tab: Vec<Vec<Attr>>,
    /// 稀データ副テーブル（`Node::extra_idx` 指し先。index 0 は placeholder）。
    pub(crate) extra_tab: Vec<NodeExtra>,
    /// テキスト bump アリーナ（フェーズ 11）。`NodeKind::Text` の >22B 本文の
    /// 置き場。1 ノード 1 `Box`（md 16MB で ~60 万 malloc）を撲滅するため、
    /// C の arena bump と同じ「1 本の連続バッファ + (off,len)」に倒す。
    /// Text ノード側は `attrs_idx=off / extra_idx=len` に転用（`Node` の
    /// フィールドコメント参照）。読みは必ず `text_of`。
    pub(crate) text_arena: Vec<u8>,
    /// ルート（DOCUMENT ノード）。
    pub root: NodeId,
    /// クイークスモード（DOCTYPE 完全表で判定。limited-quirks は false）。
    pub quirks: bool,
    /// `<title>` のテキスト（見つからなければ空）。
    pub title: Vec<u8>,
    /// パーサが生成したノード数（C の `dom->n_nodes` 相当）。**増分専用の解析時
    /// カウンタ**で、解析完了時に `nodes.len()` で凍結する（Rust では解析中に
    /// 生成されるノードは全て計数対象 = C の `new_node`/`sc_clone` と同集合。
    /// 仮想 fragment context はノードを持たないため両実装とも不算入）。
    /// script 等による解析後のノード増減は `nodes.len()` にのみ現れ、
    /// 本カウンタは変わらない（`; nodes=N` 系の全観測点はこちらを出す）。
    pub n_nodes: u32,
    /// 回復したパースエラー数（統計用）。
    pub n_errors: u32,
    /// `<script>` 要素を観測（script 実行の走査スイッチ）。
    pub has_script: bool,
    /// `<style>` 要素を観測。
    pub has_style: bool,
    /// `<selectedcontent>` を観測（customizable select の走査スイッチ）。
    pub has_selectedcontent: bool,
    /// md fast-DOM（`md::md_to_dom_opts`）で構築され、純ブロック容器直下の
    /// ws-only TEXT が意図的に剥がされている（C の `dom->md_ws_stripped` 相当）。
    /// layout はこのビットを見て、当該容器内の兄弟マージン相殺を「ws TEXT が間に
    /// あった旧 DOM と逐語同じ」結果（相殺無効＝`prev_mb=0`）に補正する。
    /// 集合の定義は `layout::ws_sink_parent`（= C `md.c` の `mo_ws_sink` と同一）。
    pub md_ws_stripped: bool,
    /// 2-slice parse の中点（body 直下 B 側先頭子。C の `dom->md_body_mid` 写し）。
    /// 10-h body shard の二分ヒント。0 = 無し（serial parse / HTML 経路）。
    pub md_body_mid: NodeId,
}

impl Dom {
    /// 空の文書を生成（DOCUMENT ルート 1 個）。
    pub fn new() -> Self {
        let mut nodes = Vec::new();
        let root = nodes.len() as NodeId;
        nodes.push(Node::new(NodeKind::Document));
        Dom {
            nodes,
            root,
            attrs_tab: Vec::new(),
            extra_tab: Vec::new(),
            text_arena: Vec::new(),
            quirks: false,
            title: Vec::new(),
            n_nodes: 1,
            n_errors: 0,
            has_script: false,
            has_style: false,
            has_selectedcontent: false,
            md_ws_stripped: false,
            md_body_mid: 0,
        }
    }

    /// ノードを生成して返す（`NodeId`）。
    pub fn alloc_node(&mut self, kind: NodeKind) -> NodeId {
        let id = self.nodes.len() as NodeId;
        self.nodes.push(Node::new(kind));
        id
    }

    /// ノードへの参照。
    pub fn node(&self, id: NodeId) -> &Node {
        &self.nodes[id as usize]
    }

    /// ノードへの可変参照。
    pub fn node_mut(&mut self, id: NodeId) -> &mut Node {
        &mut self.nodes[id as usize]
    }

    /// `parent` の末尾に `child` を接続（C の `append_child` 相当）。
    pub fn append_child(&mut self, parent: NodeId, child: NodeId) {
        let prev_last = self.nodes[parent as usize].last_child;
        if let Some(last) = prev_last {
            self.nodes[last as usize].next_sibling = Some(child);
        } else {
            self.nodes[parent as usize].first_child = Some(child);
        }
        self.nodes[parent as usize].last_child = Some(child);
        self.nodes[child as usize].parent = Some(parent);
    }

    /// `parent` の `before` の直前に `child` を挿入（C の `insert_child_before` 相当）。
    pub fn insert_child_before(&mut self, parent: NodeId, child: NodeId, before: NodeId) {
        // 前兄弟を探す
        let mut prev: Option<NodeId> = None;
        let mut cur = self.nodes[parent as usize].first_child;
        while let Some(c) = cur {
            if c == before {
                break;
            }
            prev = Some(c);
            cur = self.nodes[c as usize].next_sibling;
        }
        self.nodes[child as usize].next_sibling = Some(before);
        match prev {
            Some(pv) => self.nodes[pv as usize].next_sibling = Some(child),
            None => self.nodes[parent as usize].first_child = Some(child),
        }
        self.nodes[child as usize].parent = Some(parent);
    }

    /// `child` を木から切り離す（親・兄弟の連結を更新。C の `detach` 相当）。
    pub fn detach(&mut self, child: NodeId) {
        let parent = self.nodes[child as usize].parent;
        if let Some(p) = parent {
            // 前兄弟を探して連結し直す
            let mut prev: Option<NodeId> = None;
            let mut cur = self.nodes[p as usize].first_child;
            while let Some(c) = cur {
                if c == child {
                    break;
                }
                prev = Some(c);
                cur = self.nodes[c as usize].next_sibling;
            }
            let next = self.nodes[child as usize].next_sibling;
            match prev {
                Some(pv) => self.nodes[pv as usize].next_sibling = next,
                None => self.nodes[p as usize].first_child = next,
            }
            if self.nodes[p as usize].last_child == Some(child) {
                self.nodes[p as usize].last_child = prev;
            }
        }
        self.nodes[child as usize].parent = None;
        self.nodes[child as usize].next_sibling = None;
    }

    /// 属性副テーブル index を確保（未割当なら遅延で slot を作る）。
    fn ensure_attrs_idx(&mut self, id: NodeId) -> usize {
        let cur = self.nodes[id as usize].attrs_idx;
        if cur != 0 {
            return cur as usize;
        }
        if self.attrs_tab.is_empty() {
            self.attrs_tab.push(Vec::new()); // slot 0 = placeholder
        }
        self.attrs_tab.push(Vec::new());
        let idx = (self.attrs_tab.len() - 1) as u32;
        self.nodes[id as usize].attrs_idx = idx;
        idx as usize
    }

    /// 稀データ副テーブル index を確保（`ensure_attrs_idx` と同じ遅延規約）。
    fn ensure_extra_idx(&mut self, id: NodeId) -> usize {
        let cur = self.nodes[id as usize].extra_idx;
        if cur != 0 {
            return cur as usize;
        }
        if self.extra_tab.is_empty() {
            self.extra_tab.push(NodeExtra::default()); // slot 0 = placeholder
        }
        self.extra_tab.push(NodeExtra::default());
        let idx = (self.extra_tab.len() - 1) as u32;
        self.nodes[id as usize].extra_idx = idx;
        idx as usize
    }

    /// 属性列の参照（属性なし = 空スライス）。
    pub fn attrs(&self, id: NodeId) -> &[Attr] {
        let i = self.nodes[id as usize].attrs_idx as usize;
        if i == 0 {
            &[]
        } else {
            &self.attrs_tab[i]
        }
    }

    /// 属性列の可変参照（必要なら副テーブル slot を遅延割当）。
    pub fn attrs_mut(&mut self, id: NodeId) -> &mut Vec<Attr> {
        let i = self.ensure_attrs_idx(id);
        &mut self.attrs_tab[i]
    }

    /// 属性列をまるごと差し替え（空を渡すと副テーブル slot は作らない）。
    pub fn set_attrs(&mut self, id: NodeId, attrs: Vec<Attr>) {
        if attrs.is_empty() && self.nodes[id as usize].attrs_idx == 0 {
            return;
        }
        let i = self.ensure_attrs_idx(id);
        self.attrs_tab[i] = attrs;
    }

    /// DOCTYPE 情報の参照（`NodeKind::Doctype` で無い / 未設定なら `None`）。
    pub fn doctype(&self, id: NodeId) -> Option<&Doctype> {
        let i = self.nodes[id as usize].extra_idx as usize;
        if i == 0 {
            None
        } else {
            self.extra_tab[i].doctype.as_ref()
        }
    }

    /// DOCTYPE 情報の設定（slot 遅延割当）。
    pub fn set_doctype(&mut self, id: NodeId, d: Doctype) {
        let i = self.ensure_extra_idx(id);
        self.extra_tab[i].doctype = Some(d);
    }

    /// PI ターゲットの参照（未設定 = 空スライス）。
    pub fn pi_target(&self, id: NodeId) -> &[u8] {
        let i = self.nodes[id as usize].extra_idx as usize;
        if i == 0 {
            &[]
        } else {
            &self.extra_tab[i].pi_target
        }
    }

    /// PI ターゲットの設定（slot 遅延割当。空なら slot は作らない）。
    pub(crate) fn set_pi_target(&mut self, id: NodeId, v: Vec<u8>) {
        if v.is_empty() {
            return;
        }
        let i = self.ensure_extra_idx(id);
        self.extra_tab[i].pi_target = v;
    }

    /// 2-slice 計の結合補助: `src` の副テーブルを末尾に連結し、`src` 側ノードの
    /// ハンドルに加算すべきデルタ `(attrs, extra)` を返す（0 は 0 のまま保つこと。
    /// `src` のテーブルは消費される。C の arena 差分接続に相当）。
    pub(crate) fn merge_side_from(&mut self, src: &mut Self) -> (u32, u32) {
        let attrs_delta = if src.attrs_tab.len() > 1 {
            if self.attrs_tab.is_empty() {
                self.attrs_tab.push(Vec::new());
            }
            let d = self.attrs_tab.len() as u32 - 1;
            self.attrs_tab.extend(src.attrs_tab.drain(1..));
            d
        } else {
            0
        };
        let extra_delta = if src.extra_tab.len() > 1 {
            if self.extra_tab.is_empty() {
                self.extra_tab.push(NodeExtra::default());
            }
            let d = self.extra_tab.len() as u32 - 1;
            self.extra_tab.extend(src.extra_tab.drain(1..));
            d
        } else {
            0
        };
        (attrs_delta, extra_delta)
    }

    /// 属性値を返す（大文字小文字無視の名前一致。無ければ `None`）。
    /// C の `if_dom_attr` 相当。
    pub fn attr(&self, n: NodeId, name_ci: &[u8]) -> Option<&[u8]> {
        self.attrs(n)
            .iter()
            .find(|a| str_eq_ci(&a.name, name_ci))
            .map(|a| a.value.as_slice())
    }

    /// `class` 属性に `cls` が含まれるか（空白区切り・case-sensitive）。
    /// C の `if_dom_has_class` 相当。
    pub fn has_class(&self, n: NodeId, cls: &[u8]) -> bool {
        let Some(v) = self.attr(n, b"class") else {
            return false;
        };
        let mut i = 0;
        while i < v.len() {
            while i < v.len() && v[i].is_ascii_whitespace() {
                i += 1;
            }
            let start = i;
            while i < v.len() && !v[i].is_ascii_whitespace() {
                i += 1;
            }
            if &v[start..i] == cls {
                return true;
            }
        }
        false
    }

    /// 子孫の TEXT ノードを UTF-8 のまま連結（C の `if_dom_text_content` 相当）。
    pub fn text_content(&self, n: NodeId) -> Vec<u8> {
        let mut out = Vec::new();
        self.text_content_rec(n, &mut out);
        out
    }

    fn text_content_rec(&self, n: NodeId, out: &mut Vec<u8>) {
        let mut c = self.nodes[n as usize].first_child;
        while let Some(cid) = c {
            let node = &self.nodes[cid as usize];
            if node.kind == NodeKind::Text {
                out.extend_from_slice(self.text_of(cid));
            } else {
                self.text_content_rec(cid, out);
            }
            c = node.next_sibling;
        }
    }

    /// 文書順で最初の ELEMENT（指定タグ。無ければ `None`）。C の `if_dom_find_tag_dfs` 相当。
    pub fn find_tag_dfs(&self, tag: Tag) -> Option<NodeId> {
        self.find_rec(self.root, tag, None)
    }

    /// `id` 属性一致の最初の ELEMENT（値は case-sensitive）。C の `if_dom_find_by_id` 相当。
    pub fn find_by_id(&self, id: &[u8]) -> Option<NodeId> {
        self.find_rec(self.root, tags::TAG_UNKNOWN, Some(id))
    }

    fn find_rec(&self, n: NodeId, tag: Tag, id: Option<&[u8]>) -> Option<NodeId> {
        let mut c = Some(n);
        while let Some(cid) = c {
            let node = &self.nodes[cid as usize];
            if node.kind == NodeKind::Element {
                match id {
                    Some(idv) => {
                        if let Some(v) = self.attr(cid, b"id") {
                            if v == idv {
                                return Some(cid);
                            }
                        }
                    }
                    None => {
                        if node.tag == tag {
                            return Some(cid);
                        }
                    }
                }
            }
            if let Some(fc) = node.first_child {
                if let Some(r) = self.find_rec(fc, tag, id) {
                    return Some(r);
                }
            }
            c = node.next_sibling;
        }
        None
    }

    /// 属性の設定（無ければ追加・既存は置換）。C の `if_dom_attr_set` 相当。
    pub fn attr_set(&mut self, n: NodeId, name: &[u8], value: &[u8]) {
        if name.is_empty() {
            return;
        }
        for a in self.attrs_mut(n).iter_mut() {
            if str_eq_ci(&a.name, name) {
                a.value = NameStr::from_bytes(value);
                return;
            }
        }
        self.attrs_mut(n).push(Attr {
            name: NameStr::from_bytes(name),
            value: NameStr::from_bytes(value),
        });
    }

    /// ELEMENT の子群を単一 TEXT 子に置換（`t` が空なら全子除去）。C の
    /// `if_dom_set_text` 相当。旧子孫は木から切り離す（C と同様、切断された
    /// サブツリーは `parent` を残したまま孤立する — 所有 `Vec` 上では到達不能に
    /// なるだけで構造は一致）。
    pub fn set_text(&mut self, n: NodeId, t: &[u8]) {
        if self.nodes[n as usize].kind != NodeKind::Element {
            return;
        }
        self.nodes[n as usize].first_child = None;
        self.nodes[n as usize].last_child = None;
        if t.is_empty() {
            return;
        }
        let tn = self.alloc_node(NodeKind::Text);
        self.set_node_text(tn, t);
        self.nodes[tn as usize].parent = Some(n);
        self.nodes[n as usize].first_child = Some(tn);
        self.nodes[n as usize].last_child = Some(tn);
    }

    /// Text ノードの本文を設定（フェーズ 11 の唯一の書き口）。
    /// ≤ `NAME_INLINE_CAP` なら従来どおり `name` に inline 収容（0 コピー完結）。
    /// 超える場合は `text_arena` へ bump 追記し、`name` を空 + `attrs_idx=off /
    /// extra_idx=len` に倒す（1 ノード 1 `Box` の malloc 交通を構造消去。
    /// C の arena bump と同型）。呼び出し時点で同フィールドは 0 前提
    /// （fresh ノード。再設定は `set_text` の切替経路のみ）。
    pub(crate) fn set_node_text(&mut self, n: NodeId, t: &[u8]) {
        debug_assert_eq!(self.nodes[n as usize].kind, NodeKind::Text);
        debug_assert_eq!(self.nodes[n as usize].attrs_idx, 0);
        debug_assert_eq!(self.nodes[n as usize].extra_idx, 0);
        if t.len() <= NAME_INLINE_CAP {
            self.nodes[n as usize].name = NameStr::from_bytes(t);
            return;
        }
        let off = self.text_arena.len() as u32;
        self.text_arena.extend_from_slice(t);
        let node = &mut self.nodes[n as usize];
        node.name = NameStr::empty();
        node.attrs_idx = off;
        node.extra_idx = t.len() as u32;
    }

    /// Text ノードの本文を読む（フェーズ 11 の唯一の読み口）。
    /// inline 収容なら `name`、arena 転用なら `text_arena` を解決する。
    /// COMMENT/Doctype 等の name 保持は従来どおり `node.name` を直接読むこと
    /// （本関数は Text 専用）。
    pub fn text_of(&self, n: NodeId) -> &[u8] {
        let node = &self.nodes[n as usize];
        debug_assert_eq!(node.kind, NodeKind::Text);
        let len = node.extra_idx as usize;
        if len != 0 {
            let off = node.attrs_idx as usize;
            &self.text_arena[off..off + len]
        } else {
            &node.name
        }
    }

    /// 計測器用: text_arena の現在バイト数（観測のみ。挙動には無関係）。
    pub fn text_arena_bytes(&self) -> usize {
        self.text_arena.len()
    }

    /// text_arena を予約（md parse が入力係数で一度だけ呼ぶ。bump 倍増の
    /// realloc コピー税を構造消去するための時間軸最適化。観測バイト不変）。
    pub(crate) fn reserve_text_arena(&mut self, additional: usize) {
        self.text_arena.reserve(additional);
    }

    /// `<title>` を設定（無ければ head 先頭に生成）。`self.title` も更新。
    /// C の `if_dom_title_set` 相当。head が無い（パーサ保証外）なら `None`。
    pub fn title_set(&mut self, t: &[u8]) -> Option<NodeId> {
        let title_tag = tags::tag_id(b"title");
        let ttl = match self.find_tag_dfs(title_tag) {
            Some(t) => t,
            None => {
                let head = self.find_tag_dfs(tags::tag_id(b"head"))?;
                let ttl = self.alloc_node(NodeKind::Element);
                {
                    let node = self.node_mut(ttl);
                    node.tag = title_tag;
                    node.ns = Ns::Html;
                    node.name = NameStr::from_static(b"title");
                }
                // head の先頭へ挿入（C: parent=head / next_sibling=旧 first_child）
                let head_first = self.nodes[head as usize].first_child;
                self.nodes[ttl as usize].parent = Some(head);
                self.nodes[ttl as usize].next_sibling = head_first;
                if self.nodes[head as usize].last_child.is_none() {
                    self.nodes[head as usize].last_child = Some(ttl);
                }
                self.nodes[head as usize].first_child = Some(ttl);
                ttl
            }
        };
        self.set_text(ttl, t);
        let fresh = self.find_tag_dfs(title_tag);
        self.title = match fresh {
            Some(f) => crate::strutil::trim(&self.text_content(f)).to_vec(),
            None => Vec::new(),
        };
        Some(ttl)
    }

    /// 最初にマッチする要素を文書順 DFS で返す（最小セレクタ）。C の
    /// `if_dom_query_selector` 相当。対応は単純セレクタ（tag / #id / .class）の
    /// 空白区切り子孫結合子列（上限 4）。
    pub fn query_selector(&self, sel: &[u8]) -> Option<NodeId> {
        let parts = sel_split(sel)?;
        self.qs_rec(self.root, &parts)
    }

    fn qs_rec(&self, cur: NodeId, parts: &[SelPart<'_>]) -> Option<NodeId> {
        if node_matches_full(self, cur, parts) {
            return Some(cur);
        }
        let mut c = self.node(cur).first_child;
        while let Some(cid) = c {
            if let Some(r) = self.qs_rec(cid, parts) {
                return Some(r);
            }
            c = self.node(cid).next_sibling;
        }
        None
    }

    /// `root` 配下の要素をタグ名で収集（文書順）。戻り値は `(総数, 先頭 cap 個)`。
    /// `tag` が空 or `"*"` なら全要素。C の `if_dom_elements_by_tag` 相当。
    pub fn elements_by_tag(&self, root: NodeId, tag: &[u8], cap: usize) -> (usize, Vec<NodeId>) {
        let any = tag.is_empty() || (tag.len() == 1 && tag[0] == b'*');
        let mut ctx = EbtCtx {
            tag_id: if any {
                tags::TAG_UNKNOWN
            } else {
                tags::tag_id(tag)
            },
            any,
            tag,
            cap,
            out: Vec::new(),
        };
        let total = self.ebt_rec(root, &mut ctx, 0);
        (total, ctx.out)
    }

    fn ebt_rec(&self, cur: NodeId, ctx: &mut EbtCtx<'_>, cnt: usize) -> usize {
        let node = self.node(cur);
        let mut cnt = cnt;
        if node.kind == NodeKind::Element {
            let matched = if ctx.any {
                true
            } else if ctx.tag_id != tags::TAG_UNKNOWN {
                node.tag == ctx.tag_id
            } else {
                // 未知タグ名照合（C の ebt_rec は if_tag_name(未知) を読む潜在クラッシュ。
                // 意図どおり要素の実名 `name` と CI 比較する）
                str_eq_ci(&node.name, ctx.tag)
            };
            if matched {
                if cnt < ctx.cap {
                    ctx.out.push(cur);
                }
                cnt += 1;
            }
        }
        let mut c = node.first_child;
        while let Some(cid) = c {
            cnt = self.ebt_rec(cid, ctx, cnt);
            c = self.node(cid).next_sibling;
        }
        cnt
    }

    /// html5lib tree-construction 形式（`| indented`）へシリアライズ。
    /// C の `if_dom_serialize_wpt` 相当。出力は `Vec<u8>`（バイト一致を保証）。
    pub fn serialize_wpt(&self) -> Vec<u8> {
        let mut out = Vec::new();
        self.ser_children(self.root, 0, &mut out);
        out
    }

    /// fragment 版の wpt シリアライズ（仮想 html root の子群を出力）。
    /// C の `if_dom_serialize_wpt_frag` 相当。
    pub fn serialize_wpt_frag(&self) -> Vec<u8> {
        let mut out = Vec::new();
        let mut c = self.node(self.root).first_child;
        while let Some(cid) = c {
            if self.node(cid).kind == NodeKind::Element {
                self.ser_children(cid, 0, &mut out);
                return out;
            }
            c = self.node(cid).next_sibling;
        }
        out
    }

    /// 子ノードを文書順に出力（C の `ser_children` 相当）。
    fn ser_children(&self, n: NodeId, depth: usize, out: &mut Vec<u8>) {
        let mut c = self.node(n).first_child;
        while let Some(cid) = c {
            self.ser_node(cid, depth, out);
            c = self.node(cid).next_sibling;
        }
    }

    /// インデント（深さごとに 2 スペース。C の `ser_indent` 相当）。
    fn ser_indent(out: &mut Vec<u8>, depth: usize) {
        for _ in 0..depth {
            out.extend_from_slice(b"  ");
        }
    }

    /// ノード 1 個を出力（C の `ser_node` 相当）。
    fn ser_node(&self, n: NodeId, depth: usize, out: &mut Vec<u8>) {
        let node = self.node(n);
        match node.kind {
            NodeKind::Text => {
                out.extend_from_slice(b"| ");
                Self::ser_indent(out, depth);
                out.push(b'"');
                out.extend_from_slice(self.text_of(n));
                out.extend_from_slice(b"\"\n");
            }
            NodeKind::Comment => {
                out.extend_from_slice(b"| ");
                Self::ser_indent(out, depth);
                let pi = self.pi_target(n);
                if !pi.is_empty() {
                    out.extend_from_slice(b"<?");
                    out.extend_from_slice(pi);
                    out.push(b' ');
                    out.extend_from_slice(&node.name);
                    out.extend_from_slice(b"?>\n");
                } else {
                    out.extend_from_slice(b"<!-- ");
                    out.extend_from_slice(&node.name);
                    out.extend_from_slice(b" -->\n");
                }
            }
            NodeKind::Doctype => {
                out.extend_from_slice(b"| ");
                Self::ser_indent(out, depth);
                match self.doctype(n) {
                    None
                    | Some(Doctype {
                        has_name: false, ..
                    }) => {
                        out.extend_from_slice(b"<!DOCTYPE >\n");
                    }
                    Some(d) => {
                        out.extend_from_slice(b"<!DOCTYPE ");
                        out.extend_from_slice(&d.name);
                        if d.pub_id.is_empty() && d.sys_id.is_empty() {
                            out.extend_from_slice(b">\n");
                        } else {
                            out.extend_from_slice(b" \"");
                            out.extend_from_slice(&d.pub_id);
                            out.extend_from_slice(b"\" \"");
                            out.extend_from_slice(&d.sys_id);
                            out.extend_from_slice(b"\">\n");
                        }
                    }
                }
            }
            NodeKind::Element => {
                out.extend_from_slice(b"| ");
                Self::ser_indent(out, depth);
                out.push(b'<');
                match node.ns {
                    Ns::Svg => out.extend_from_slice(b"svg "),
                    Ns::Mathml => out.extend_from_slice(b"math "),
                    Ns::Html => {}
                }
                out.extend_from_slice(&node.name);
                out.extend_from_slice(b">\n");
                let attrs = self.attrs(n);
                if !attrs.is_empty() {
                    // 属性は名前の辞書順（バイト比較）で出力（C の attr_name_cmp + qsort）
                    let mut sorted: Vec<&Attr> = attrs.iter().collect();
                    sorted.sort_by(|a, b| a.name.cmp(&b.name));
                    for a in sorted {
                        out.extend_from_slice(b"| ");
                        Self::ser_indent(out, depth + 1);
                        out.extend_from_slice(&a.name);
                        out.extend_from_slice(b"=\"");
                        out.extend_from_slice(&a.value);
                        out.extend_from_slice(b"\"\n");
                    }
                }
                // HTML ns の <template> のみ content 擬似ノード配下に子を出す
                if node.ns == Ns::Html && node.name == b"template" {
                    out.extend_from_slice(b"| ");
                    Self::ser_indent(out, depth + 1);
                    out.extend_from_slice(b"content\n");
                    let tc = node.tpl_content.unwrap_or(n);
                    self.ser_children(tc, depth + 2, out);
                } else {
                    self.ser_children(n, depth + 1, out);
                }
            }
            NodeKind::Document => {
                self.ser_children(n, depth, out);
            }
        }
    }

    /// デバッグ用の DOM ダンプ（C の `if_dom_dump` 相当。`--dump-dom` の出力）。
    ///
    /// `#document` + インデント付きノード列 + `; nodes=N errors=M title="..."`。
    /// テキストは 48 バイト打ち切り（`\n`→`\\n`、`"`→`\\\"`）、属性値は 64 バイト打ち切り。
    pub fn dump(&self) -> Vec<u8> {
        let mut out = Vec::new();
        if self.nodes.is_empty() {
            out.extend_from_slice(b"(empty dom)\n");
            return out;
        }
        out.extend_from_slice(b"#document\n");
        let mut c = self.node(self.root).first_child;
        while let Some(cid) = c {
            self.dump_node(cid, 0, &mut out);
            c = self.node(cid).next_sibling;
        }
        out.extend_from_slice(
            format!("; nodes={} errors={} title=\"", self.n_nodes, self.n_errors).as_bytes(),
        );
        out.extend_from_slice(&self.title);
        out.extend_from_slice(b"\"\n");
        out
    }

    /// ノード 1 個をデバッグ形式で出力（C の `dump_node` 相当）。
    fn dump_node(&self, n: NodeId, depth: usize, out: &mut Vec<u8>) {
        for _ in 0..depth {
            out.extend_from_slice(b"  ");
        }
        let node = self.node(n);
        match node.kind {
            NodeKind::Text => {
                let t = self.text_of(n);
                let shown = t.len().min(48);
                out.extend_from_slice(b"#text \"");
                for &c in &t[..shown] {
                    if c == b'\n' {
                        out.extend_from_slice(b"\\n");
                    } else if c == b'"' {
                        out.extend_from_slice(b"\\\"");
                    } else {
                        out.push(c);
                    }
                }
                if t.len() > 48 {
                    out.extend_from_slice("…".as_bytes());
                }
                out.extend_from_slice(b"\"\n");
            }
            NodeKind::Doctype => match self.doctype(n) {
                Some(d) if d.has_name => {
                    out.extend_from_slice(b"<!DOCTYPE ");
                    out.extend_from_slice(&d.name);
                    out.extend_from_slice(b">\n");
                }
                _ => out.extend_from_slice(b"<!DOCTYPE >\n"),
            },
            NodeKind::Comment => {
                let pi = self.pi_target(n);
                if !pi.is_empty() {
                    out.extend_from_slice(b"<?");
                    out.extend_from_slice(pi);
                    out.push(b' ');
                    out.extend_from_slice(&node.name);
                    out.extend_from_slice(b"?>\n");
                } else {
                    out.extend_from_slice(b"<!-- ");
                    out.extend_from_slice(&node.name);
                    out.extend_from_slice(b" -->\n");
                }
            }
            NodeKind::Element => {
                out.push(b'<');
                if node.name.is_empty() {
                    out.push(b'?');
                } else {
                    out.extend_from_slice(&node.name);
                }
                for a in self.attrs(n) {
                    out.push(b' ');
                    out.extend_from_slice(&a.name);
                    out.extend_from_slice(b"=\"");
                    let shown = a.value.len().min(64);
                    out.extend_from_slice(&a.value[..shown]);
                    out.push(b'"');
                }
                out.extend_from_slice(b">\n");
                let mut c = node.first_child;
                while let Some(cid) = c {
                    self.dump_node(cid, depth + 1, out);
                    c = self.node(cid).next_sibling;
                }
            }
            NodeKind::Document => {
                let mut c = node.first_child;
                while let Some(cid) = c {
                    self.dump_node(cid, depth, out);
                    c = self.node(cid).next_sibling;
                }
            }
        }
    }
}

// ---- 最小セレクタ（C の dom.c `SelPart` / `sel_part_parse` / `sel_split` /
//      `sel_part_matches` / `qs_rec` 相当） ----

/// `elements_by_tag` の走査文脈（DFS 再帰の引数を束ねる）。
struct EbtCtx<'a> {
    tag_id: Tag,
    any: bool,
    tag: &'a [u8],
    cap: usize,
    out: Vec<NodeId>,
}

/// 単純セレクタ 1 個（`div#main.nav`）。C の `SelPart` 相当。
#[derive(Clone, Copy)]
struct SelPart<'a> {
    tag: Option<&'a [u8]>,
    id: Option<&'a [u8]>,
    cls: Option<&'a [u8]>,
}

/// 複合セレクタ 1 つをパース（例: `div#main.nav`）。失敗で `None`。
fn sel_part_parse(s: &[u8]) -> Option<SelPart<'_>> {
    let n = s.len();
    let mut out = SelPart {
        tag: None,
        id: None,
        cls: None,
    };
    let mut i = 0usize;
    if i < n && (s[i] == b'#' || s[i] == b'.') {
        // タグなし（#id / .class から開始）
    } else {
        let st = i;
        while i < n && s[i] != b'#' && s[i] != b'.' {
            i += 1;
        }
        if i == st {
            return None;
        }
        out.tag = Some(&s[st..i]);
    }
    while i < n {
        let c = s[i];
        if c == b'#' {
            i += 1;
            let st = i;
            while i < n && s[i] != b'.' && s[i] != b'#' {
                i += 1;
            }
            if i == st || out.id.is_some() {
                return None; // 空 or 複数 id は非対応
            }
            out.id = Some(&s[st..i]);
        } else if c == b'.' {
            i += 1;
            let st = i;
            while i < n && s[i] != b'.' && s[i] != b'#' {
                i += 1;
            }
            if i == st || out.cls.is_some() {
                return None; // 空 or 複数 class は非対応
            }
            out.cls = Some(&s[st..i]);
        } else {
            return None;
        }
    }
    Some(out)
}

/// セレクタを空白区切りの複合列に分割（子孫結合子）。上限 4。失敗・空で `None`。
fn sel_split(sel: &[u8]) -> Option<Vec<SelPart<'_>>> {
    let mut parts = Vec::new();
    let mut i = 0usize;
    while i < sel.len() {
        while i < sel.len()
            && (sel[i] == b' ' || sel[i] == b'\t' || sel[i] == b'\n' || sel[i] == b'\r')
        {
            i += 1;
        }
        if i >= sel.len() {
            break;
        }
        let st = i;
        while i < sel.len()
            && !(sel[i] == b' ' || sel[i] == b'\t' || sel[i] == b'\n' || sel[i] == b'\r')
        {
            i += 1;
        }
        if parts.len() >= 4 {
            return None; // 過剰分割 = 非対応
        }
        parts.push(sel_part_parse(&sel[st..i])?);
    }
    if parts.is_empty() {
        return None;
    }
    Some(parts)
}

/// class 属性にトークンとして含まれるか（case-sensitive）。C の `sel_part_matches`
/// の class 分岐と同一の空白集合（space/tab/nl/cr/ff）。
fn has_class_token(class_attr: &[u8], cls: &[u8]) -> bool {
    let mut i = 0usize;
    while i < class_attr.len() {
        while i < class_attr.len()
            && (class_attr[i] == b' '
                || class_attr[i] == b'\t'
                || class_attr[i] == b'\n'
                || class_attr[i] == b'\r'
                || class_attr[i] == b'\x0c')
        {
            i += 1;
        }
        let st = i;
        while i < class_attr.len()
            && !(class_attr[i] == b' '
                || class_attr[i] == b'\t'
                || class_attr[i] == b'\n'
                || class_attr[i] == b'\r'
                || class_attr[i] == b'\x0c')
        {
            i += 1;
        }
        if &class_attr[st..i] == cls {
            return true;
        }
    }
    false
}

/// 複合セレクタ 1 個がノードにマッチするか。C の `sel_part_matches` 相当。
fn sel_part_matches(dom: &Dom, n: NodeId, p: &SelPart<'_>) -> bool {
    let node = dom.node(n);
    if node.kind != NodeKind::Element {
        return false;
    }
    if let Some(tag) = p.tag {
        // C は既知タグを `if_tag_name(n->tag)`、未知タグを `u.tag_name` で CI 比較する
        // 「意図」だが、未知タグ経路は `if_tag_name(UNKNOWN)=NULL` を strlen する潜在
        // クラッシュ。どちらも `Node.name`（実名）と同値なので統合して CI 照合する。
        if !str_eq_ci(&node.name, tag) {
            return false;
        }
    }
    if let Some(id) = p.id {
        match dom.attr(n, b"id") {
            Some(v) if v == id => {}
            _ => return false,
        }
    }
    if let Some(cls) = p.cls {
        match dom.attr(n, b"class") {
            Some(v) if has_class_token(v, cls) => {}
            _ => return false,
        }
    }
    true
}

/// 複合セレクタ列全体（右端が n 自身、左群は祖先に右から順にマッチ）の照合。
/// C の `qs_rec` のマッチ部 / `if_dom_selector_matches` 相当。
fn node_matches_full(dom: &Dom, n: NodeId, parts: &[SelPart<'_>]) -> bool {
    if !sel_part_matches(dom, n, &parts[parts.len() - 1]) {
        return false;
    }
    let mut pi = parts.len() - 1;
    let mut a = dom.node(n).parent;
    while pi > 0 {
        let Some(aid) = a else { break };
        let anode = dom.node(aid);
        if anode.kind == NodeKind::Element && sel_part_matches(dom, aid, &parts[pi - 1]) {
            pi -= 1;
        }
        a = anode.parent;
    }
    pi == 0
}

impl Default for Dom {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 要素を 1 個作って parent に接続する補助。
    fn make_elem(dom: &mut Dom, parent: NodeId, tag: Tag) -> NodeId {
        let n = dom.alloc_node(NodeKind::Element);
        {
            let node = dom.node_mut(n);
            node.tag = tag;
            node.name = NameStr::from_static(tags::tag_name(tag).unwrap_or("").as_bytes());
        }
        dom.append_child(parent, n);
        n
    }

    /// テキストを 1 個作って parent に接続する補助。
    fn make_text(dom: &mut Dom, parent: NodeId, text: &[u8]) -> NodeId {
        let n = dom.alloc_node(NodeKind::Text);
        {
            let node = dom.node_mut(n);
            node.name = NameStr::from_bytes(text);
        }
        dom.append_child(parent, n);
        n
    }

    #[test]
    fn tree_structure() {
        let mut d = Dom::new();
        let root = d.root;
        let html = make_elem(&mut d, root, tags::tag_id(b"html"));
        let head = make_elem(&mut d, html, tags::tag_id(b"head"));
        let body = make_elem(&mut d, html, tags::tag_id(b"body"));
        make_text(&mut d, body, b"hello");

        assert_eq!(d.node(html).first_child, Some(head));
        assert_eq!(d.node(html).last_child, Some(body));
        assert_eq!(d.node(head).next_sibling, Some(body));
        assert_eq!(d.node(body).parent, Some(html));
        // 子の順序: head, body
        assert_eq!(d.node(head).next_sibling, Some(body));
        assert_eq!(d.node(body).next_sibling, None);
    }

    #[test]
    fn text_content_dfs() {
        let mut d = Dom::new();
        let root = d.root;
        let body = make_elem(&mut d, root, tags::tag_id(b"body"));
        let p = make_elem(&mut d, body, tags::tag_id(b"p"));
        make_text(&mut d, p, b"a");
        let b = make_elem(&mut d, p, tags::tag_id(b"b"));
        make_text(&mut d, b, b"bold");
        make_text(&mut d, p, b"c");
        assert_eq!(d.text_content(body), b"aboldc");
    }

    #[test]
    fn attr_and_has_class() {
        let mut d = Dom::new();
        let root = d.root;
        let div = make_elem(&mut d, root, tags::tag_id(b"div"));
        d.attr_set(div, b"class", b"foo bar");
        d.attr_set(div, b"ID", b"x");
        assert_eq!(d.attr(div, b"class"), Some(&b"foo bar"[..]));
        assert_eq!(d.attr(div, b"CLASS"), Some(&b"foo bar"[..]));
        assert!(d.has_class(div, b"foo"));
        assert!(d.has_class(div, b"bar"));
        assert!(!d.has_class(div, b"baz"));
        // id は case-sensitive な値
        assert_eq!(d.attr(div, b"id"), Some(&b"x"[..]));
    }

    #[test]
    fn find_tag_and_id() {
        let mut d = Dom::new();
        let root = d.root;
        let html = make_elem(&mut d, root, tags::tag_id(b"html"));
        let body = make_elem(&mut d, html, tags::tag_id(b"body"));
        let div = make_elem(&mut d, body, tags::tag_id(b"div"));
        d.attr_set(div, b"id", b"main");

        assert_eq!(d.find_tag_dfs(tags::tag_id(b"div")), Some(div));
        assert_eq!(d.find_by_id(b"main"), Some(div));
        assert_eq!(d.find_by_id(b"missing"), None);
    }

    #[test]
    fn detach() {
        let mut d = Dom::new();
        let root = d.root;
        let html = make_elem(&mut d, root, tags::tag_id(b"html"));
        let a = make_elem(&mut d, html, tags::tag_id(b"p"));
        let b = make_elem(&mut d, html, tags::tag_id(b"p"));
        let c = make_elem(&mut d, html, tags::tag_id(b"p"));

        d.detach(b);
        // 順序: a, c
        assert_eq!(d.node(a).next_sibling, Some(c));
        assert_eq!(d.node(html).first_child, Some(a));
        assert_eq!(d.node(html).last_child, Some(c));
        assert_eq!(d.node(b).parent, None);
        assert_eq!(d.node(b).next_sibling, None);
    }

    #[test]
    fn set_text_replaces_children() {
        let mut d = Dom::new();
        let root = d.root;
        let body = make_elem(&mut d, root, tags::tag_id(b"body"));
        make_text(&mut d, body, b"old");
        let nested = make_elem(&mut d, body, tags::tag_id(b"p"));
        make_text(&mut d, nested, b"x");

        d.set_text(body, b"new");
        assert_eq!(d.text_content(body), b"new");
        assert_eq!(
            d.node(body).first_child,
            Some(d.node(body).last_child.unwrap())
        );
        assert_eq!(
            d.node(body).first_child.map(|c| d.node(c).kind),
            Some(NodeKind::Text)
        );

        // 空 text は全子除去
        d.set_text(body, b"");
        assert_eq!(d.node(body).first_child, None);
        assert_eq!(d.node(body).last_child, None);
    }

    #[test]
    fn query_selector_basic() {
        let mut d = Dom::new();
        let root = d.root;
        let html = make_elem(&mut d, root, tags::tag_id(b"html"));
        let body = make_elem(&mut d, html, tags::tag_id(b"body"));
        let div = make_elem(&mut d, body, tags::tag_id(b"div"));
        d.attr_set(div, b"id", b"main");
        d.attr_set(div, b"class", b"nav box");
        let p = make_elem(&mut d, div, tags::tag_id(b"p"));
        d.attr_set(p, b"class", b"para");

        assert_eq!(d.query_selector(b"div"), Some(div));
        assert_eq!(d.query_selector(b"#main"), Some(div));
        assert_eq!(d.query_selector(b".nav"), Some(div));
        assert_eq!(d.query_selector(b"div#main.nav"), Some(div));
        // 子孫結合子
        assert_eq!(d.query_selector(b"div p"), Some(p));
        assert_eq!(d.query_selector(b"#main .para"), Some(p));
        // 無い
        assert_eq!(d.query_selector(b".missing"), None);
        assert_eq!(d.query_selector(b"span"), None);
        // 空 / 不正
        assert_eq!(d.query_selector(b""), None);
        assert_eq!(d.query_selector(b"div##main"), None);
    }

    #[test]
    fn query_selector_unknown_tag_and_case() {
        let mut d = Dom::new();
        let root = d.root;
        let html = make_elem(&mut d, root, tags::tag_id(b"html"));
        let custom = make_elem(&mut d, html, tags::TAG_UNKNOWN);
        d.node_mut(custom).name = NameStr::from_static(b"my-widget");
        let body = make_elem(&mut d, html, tags::tag_id(b"body"));
        let div = make_elem(&mut d, body, tags::tag_id(b"div"));

        // 未知タグ名は CI 照合
        assert_eq!(d.query_selector(b"my-widget"), Some(custom));
        assert_eq!(d.query_selector(b"MY-WIDGET"), Some(custom));
        // 既知タグも CI
        assert_eq!(d.query_selector(b"DIV"), Some(div));
    }

    #[test]
    fn elements_by_tag_collects() {
        let mut d = Dom::new();
        let root = d.root;
        let html = make_elem(&mut d, root, tags::tag_id(b"html"));
        let body = make_elem(&mut d, html, tags::tag_id(b"body"));
        let a = make_elem(&mut d, body, tags::tag_id(b"p"));
        let b2 = make_elem(&mut d, body, tags::tag_id(b"div"));
        let c = make_elem(&mut d, b2, tags::tag_id(b"p"));

        let (total, got) = d.elements_by_tag(root, b"p", 1024);
        assert_eq!(total, 2);
        assert_eq!(got, vec![a, c]);
        // cap で打ち切り
        let (total, got) = d.elements_by_tag(root, b"p", 1);
        assert_eq!(total, 2);
        assert_eq!(got, vec![a]);
        // wildcard（html, body, p, div, p の 5 要素）
        let (total, got) = d.elements_by_tag(root, b"*", 1024);
        assert_eq!(total, 5);
        assert_eq!(got.len(), 5);
        // 未知タグ名 CI
        let custom = make_elem(&mut d, body, tags::TAG_UNKNOWN);
        d.node_mut(custom).name = NameStr::from_static(b"x-foo");
        let (total, got) = d.elements_by_tag(root, b"x-foo", 1024);
        assert_eq!(total, 1);
        assert_eq!(got, vec![custom]);
    }

    #[test]
    fn title_set_updates() {
        let mut d = Dom::new();
        let root = d.root;
        let html = make_elem(&mut d, root, tags::tag_id(b"html"));
        let head = make_elem(&mut d, html, tags::tag_id(b"head"));
        let _body = make_elem(&mut d, html, tags::tag_id(b"body"));

        // 既存 title なし → head 先頭に生成
        let t = d.title_set(b"  Hello  ").unwrap();
        assert_eq!(d.node(t).tag, tags::tag_id(b"title"));
        assert_eq!(d.node(head).first_child, Some(t));
        assert_eq!(d.title, b"Hello"); // trim 済み

        // 既存 title を更新
        d.title_set(b"World");
        assert_eq!(d.title, b"World");
        assert_eq!(d.node(head).first_child, Some(t)); // 再生成しない
    }

    /// C の `if_dom_dump` のデバッグ形式（`--dump-dom`）を固定。
    #[test]
    fn dump_debug_format() {
        let mut d = Dom::new();
        let root = d.root;
        let html = make_elem(&mut d, root, tags::tag_id(b"html"));
        let body = make_elem(&mut d, html, tags::tag_id(b"body"));
        make_text(&mut d, body, b"hello\nworld");
        d.title = b"T".to_vec();
        d.n_nodes = 4; // 解析時カウンタを模す（手構築は C 同様カウントされない）
        d.n_errors = 3;

        let out = d.dump();
        let s = String::from_utf8(out).unwrap();
        assert!(s.starts_with("#document\n"), "got: {s:?}");
        assert!(s.contains("<html>\n"), "got: {s:?}");
        assert!(s.contains("  <body>\n"), "got: {s:?}");
        assert!(s.contains("#text \"hello\\nworld\""), "got: {s:?}");
        assert!(
            s.ends_with("; nodes=4 errors=3 title=\"T\"\n"),
            "got: {s:?}"
        );
    }

    /// 痩身化ラチェット（フェーズ 10-e）: Node は属性・稀データの副テーブル化で
    /// 200B → 80B（C IfNode ~40B の 2 倍）。これ以上の肥大化は機械的に拒否する。
    #[test]
    fn node_size_ratchet() {
        assert!(
            std::mem::size_of::<super::Node>() <= 80,
            "Node が痩身化ラチェット(80B)超過: {}",
            std::mem::size_of::<super::Node>()
        );
    }

    /// 副テーブルの読み書き・遅延割当・merge デルタ写像の規約を固定。
    #[test]
    fn side_tables_roundtrip() {
        let mut d = Dom::new();
        let a = d.alloc_node(NodeKind::Element);
        let b = d.alloc_node(NodeKind::Element);
        assert!(d.attrs(a).is_empty() && d.attrs(b).is_empty()); // 0 確保
        assert!(d.doctype(a).is_none() && d.pi_target(a).is_empty());
        d.attr_set(a, b"href", b"/x");
        d.attr_set(a, b"HREF", b"/y"); // 既存置換（case-insensitive）
        assert_eq!(d.attrs(a).len(), 1);
        assert_eq!(d.attr(a, b"href"), Some(&b"/y"[..]));
        assert!(d.attrs(b).is_empty()); // 他ノードに漏れない
        let mut e = Dom::new();
        let _ = e.alloc_node(NodeKind::Element);
        let n2 = e.alloc_node(NodeKind::Element);
        e.attr_set(n2, b"alt", b"z");
        e.set_doctype(
            n2,
            Doctype {
                name: b"html".to_vec(),
                has_name: true,
                ..Default::default()
            },
        );
        let (da, _dx) = d.merge_side_from(&mut e);
        assert_eq!(d.attrs_tab.len() - 1, 2); // 本体 1 + 合併 1
        assert_eq!(d.extra_tab.len() - 1, 1);
        assert_eq!(da, 1); // d 側に既存 1 slot → B ハンドルは +1
    }
}
