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

/// DOM ノード（C の `IfNode` 相当）。
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
    pub name: Vec<u8>,
    /// 属性列（ELEMENT）。
    pub attrs: Vec<Attr>,
    /// DOCTYPE 情報（`NodeKind::Doctype` のみ有意）。
    pub doctype: Option<Doctype>,
    /// PI ターゲット（COMMENT が処理命令のときのみ非空。C は attrs[0].name に保持）。
    pub pi_target: Vec<u8>,
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
            name: Vec::new(),
            attrs: Vec::new(),
            doctype: None,
            pi_target: Vec::new(),
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
            quirks: false,
            title: Vec::new(),
            n_nodes: 1,
            n_errors: 0,
            has_script: false,
            has_style: false,
            has_selectedcontent: false,
            md_ws_stripped: false,
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

    /// 属性値を返す（大文字小文字無視の名前一致。無ければ `None`）。
    /// C の `if_dom_attr` 相当。
    pub fn attr(&self, n: NodeId, name_ci: &[u8]) -> Option<&[u8]> {
        let node = &self.nodes[n as usize];
        node.attrs
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
                out.extend_from_slice(&node.name);
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
        let node = &mut self.nodes[n as usize];
        for a in node.attrs.iter_mut() {
            if str_eq_ci(&a.name, name) {
                a.value = value.to_vec();
                return;
            }
        }
        node.attrs.push(Attr {
            name: name.to_vec(),
            value: value.to_vec(),
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
        self.nodes[tn as usize].name = t.to_vec();
        self.nodes[tn as usize].parent = Some(n);
        self.nodes[n as usize].first_child = Some(tn);
        self.nodes[n as usize].last_child = Some(tn);
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
                    node.name = b"title".to_vec();
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
                out.extend_from_slice(&node.name);
                out.extend_from_slice(b"\"\n");
            }
            NodeKind::Comment => {
                out.extend_from_slice(b"| ");
                Self::ser_indent(out, depth);
                if !node.pi_target.is_empty() {
                    out.extend_from_slice(b"<?");
                    out.extend_from_slice(&node.pi_target);
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
                match node.doctype.as_ref() {
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
                if !node.attrs.is_empty() {
                    // 属性は名前の辞書順（バイト比較）で出力（C の attr_name_cmp + qsort）
                    let mut sorted: Vec<&Attr> = node.attrs.iter().collect();
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
                let shown = node.name.len().min(48);
                out.extend_from_slice(b"#text \"");
                for &c in &node.name[..shown] {
                    if c == b'\n' {
                        out.extend_from_slice(b"\\n");
                    } else if c == b'"' {
                        out.extend_from_slice(b"\\\"");
                    } else {
                        out.push(c);
                    }
                }
                if node.name.len() > 48 {
                    out.extend_from_slice("…".as_bytes());
                }
                out.extend_from_slice(b"\"\n");
            }
            NodeKind::Doctype => match &node.doctype {
                Some(d) if d.has_name => {
                    out.extend_from_slice(b"<!DOCTYPE ");
                    out.extend_from_slice(&d.name);
                    out.extend_from_slice(b">\n");
                }
                _ => out.extend_from_slice(b"<!DOCTYPE >\n"),
            },
            NodeKind::Comment => {
                if !node.pi_target.is_empty() {
                    out.extend_from_slice(b"<?");
                    out.extend_from_slice(&node.pi_target);
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
                for a in &node.attrs {
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
            node.name = tags::tag_name(tag).unwrap_or("").as_bytes().to_vec();
        }
        dom.append_child(parent, n);
        n
    }

    /// テキストを 1 個作って parent に接続する補助。
    fn make_text(dom: &mut Dom, parent: NodeId, text: &[u8]) -> NodeId {
        let n = dom.alloc_node(NodeKind::Text);
        {
            let node = dom.node_mut(n);
            node.name = text.to_vec();
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
        d.node_mut(custom).name = b"my-widget".to_vec();
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
        d.node_mut(custom).name = b"x-foo".to_vec();
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
}
