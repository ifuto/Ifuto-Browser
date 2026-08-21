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

use crate::tags::{self, Tag};
use crate::strutil::str_eq_ci;

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

/// 属性（名前は ASCII lowercase 正規化済み）。
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Attr {
    /// 属性名。
    pub name: Vec<u8>,
    /// 属性値（文字参照デコード済み）。
    pub value: Vec<u8>,
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
    /// タグ名（ELEMENT）/ テキスト（TEXT, COMMENT）/ DOCTYPE name。
    pub name: Vec<u8>,
    /// 属性列（ELEMENT）。
    pub attrs: Vec<Attr>,
    /// 親。
    pub parent: Option<NodeId>,
    /// 先頭の子。
    pub first_child: Option<NodeId>,
    /// 末尾の子。
    pub last_child: Option<NodeId>,
    /// 次の兄弟。
    pub next_sibling: Option<NodeId>,
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
            parent: None,
            first_child: None,
            last_child: None,
            next_sibling: None,
        }
    }
}

/// 文書（C の `IfDom` 相当）。
#[derive(Clone, Debug)]
pub struct Dom {
    /// 全ノード（`NodeId` はここへの index）。
    nodes: Vec<Node>,
    /// ルート（DOCUMENT ノード）。
    pub root: NodeId,
    /// クイークスモード（DOCTYPE 完全表で判定。limited-quirks は false）。
    pub quirks: bool,
    /// `<title>` のテキスト（見つからなければ空）。
    pub title: Vec<u8>,
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
        let Some(v) = self.attr(n, b"class") else { return false };
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
}
