//! HTML ツリービルダ（C の `src/html_tree.c` 相当。WHATWG insertion modes）。
//!
//! | C (html_tree.c) | Rust |
//! |---|---|
//! | `IfMode` | [`Mode`] |
//! | `IfTB`（arena ポインタ連結 + stack） | [`TreeBuilder`]（`Vec<NodeId>`） |
//! | `step_in_body` 等の挿入モード群 | [`TreeBuilder::step`] と各 `step_*` |
//! | `if_parse_html` / `if_parse_html_fragment` | [`parse_html`] / [`parse_html_fragment`] |
//!
//! # 実装済み
//!
//! quirks モード判定（DOCTYPE 完全表）、foster parenting、table 挿入モード群、
//! active formatting elements + adoption agency、frameset モード群、foreign content、
//! template 挿入モード、customizable select。
//!
//! # C との違い（所有権による構造的な改善）
//!
//! C はノードを arena から確保し raw ポインタで連結、スタックも `IfNode**` 配列。
//! Rust では [`crate::dom::Dom`] の `NodeId`（`Vec<Node>` index）をスタックに積み、
//! 木の連結は `Option<NodeId>`。ポインタ寿命・エイリアシング問題を構造的に排除。
//! テキスト・属性値はトークナイザが所有 `Vec<u8>` で返すため arena が不要。

use crate::dom::{Attr, Dom, Doctype, NodeId, NodeKind, Ns};
use crate::html_tok::{Tok, TokKind, Tokenizer};
use crate::tags::{self, Tag};
use crate::strutil::str_eq_ci;

/// 挿入モード（C の `IfMode` 相当）。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Mode {
    Initial,
    BeforeHtml,
    BeforeHead,
    InHead,
    InHeadNoscript,
    AfterHead,
    InBody,
    AfterBody,
    AfterAfterBody,
    InTable,
    InCaption,
    InColumnGroup,
    InTableBody,
    InRow,
    InCell,
    InFrameset,
    AfterFrameset,
    AfterAfterFrameset,
    InTemplate,
}

/// スコープ種別（C の `IfScopeKind` 相当）。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum ScopeKind {
    Default,
    Button,
    ListItem,
    Table,
}

/// ノード数上限（C の `IF_MAX_DOM_NODES`）。
const MAX_DOM_NODES: u32 = 4_000_000;
/// スタック深さ上限（C の `IF_MAX_STACK_DEPTH`）。
const MAX_STACK_DEPTH: u32 = 4096;

/// AFE（active formatting elements）の 1 エントリ。
#[derive(Clone, Copy, Debug)]
struct AfeEntry {
    node: NodeId,
    marker: bool,
}

/// fragment の仮想 context（DOM 不接続・n_nodes 不算入）。
#[derive(Clone, Debug)]
struct FragCtx {
    ns: Ns,
    tag: Tag,
}

/// ツリービルダ（C の `IfTB` 相当）。
pub struct TreeBuilder<'a> {
    dom: Dom,
    tok: Tokenizer<'a>,
    mode: Mode,
    stack: Vec<NodeId>,
    html: Option<NodeId>,
    head: Option<NodeId>,
    body: Option<NodeId>,
    form: Option<NodeId>,
    stopped: bool,
    skip_lf: bool,
    no_foreign: bool,
    foster: bool,
    pend: Vec<u8>,
    pend_nonws: bool,
    frameset_ok: bool,
    tpl_modes: Vec<Mode>,
    afe: Vec<AfeEntry>,
    frag: bool,
    frag_ctx: Option<FragCtx>,
}

use crate::tags_tables::*;

impl<'a> TreeBuilder<'a> {
    /// 新規ビルダー。
    fn new(dom: Dom, tok: Tokenizer<'a>) -> Self {
        TreeBuilder {
            dom,
            tok,
            mode: Mode::Initial,
            stack: Vec::new(),
            html: None,
            head: None,
            body: None,
            form: None,
            stopped: false,
            skip_lf: false,
            no_foreign: false,
            foster: false,
            pend: Vec::new(),
            pend_nonws: false,
            frameset_ok: true, // WHATWG: 初期値 "ok"
            tpl_modes: Vec::new(),
            afe: Vec::new(),
            frag: false,
            frag_ctx: None,
        }
    }

    // ---- 基本スタック操作 ----

    fn push(&mut self, n: NodeId) {
        if (self.stack.len() as u32) < MAX_STACK_DEPTH {
            self.stack.push(n);
        } else {
            self.dom.n_errors += 1;
        }
    }

    fn top(&self) -> NodeId {
        *self.stack.last().unwrap_or(&self.dom.root)
    }

    fn pop(&mut self) {
        self.stack.pop();
    }

    /// adjusted current node（fragment case で stack が仮想 html root だけの間は context）。
    /// `None` = 仮想 context（frag_ctx）。
    fn acn(&self) -> Option<NodeId> {
        if self.frag && self.stack.len() <= 1 {
            None
        } else {
            Some(self.top())
        }
    }

    /// acn の (ns, tag)。frag_ctx のときは仮想 context の値。
    fn acn_ns_tag(&self) -> (Ns, Tag) {
        match self.acn() {
            Some(id) => {
                let n = self.dom.node(id);
                (n.ns, n.tag)
            }
            None => {
                let fc = self.frag_ctx.as_ref().unwrap();
                (fc.ns, fc.tag)
            }
        }
    }

    // ---- ノード生成・挿入 ----

    /// 要素ノード生成（C の `make_element` 相当）。
    fn make_element(&mut self, tok: &Tok) -> NodeId {
        let mut has_script = false;
        let mut has_style = false;
        let mut has_selectedcontent = false;
        let n = self.dom.alloc_node(NodeKind::Element);
        {
            let node = self.dom.node_mut(n);
            node.tag = tok.tag;
            if tok.tag == TAG_SCRIPT {
                has_script = true;
            }
            if tok.tag == tags::TAG_UNKNOWN {
                node.name = tok.tag_raw.iter().map(|&c| c.to_ascii_lowercase()).collect();
                if tok.tag_raw.len() == 15 && str_eq_ci(&node.name, b"selectedcontent") {
                    has_selectedcontent = true;
                }
            } else {
                node.name = tags::tag_name(tok.tag).unwrap_or("").as_bytes().to_vec();
                if tok.tag == TAG_STYLE {
                    has_style = true;
                }
            }
            node.attrs = tok.attrs.clone();
        }
        if has_script {
            self.dom.has_script = true;
        }
        if has_style {
            self.dom.has_style = true;
        }
        if has_selectedcontent {
            self.dom.has_selectedcontent = true;
        }
        n
    }

    /// 適切な挿入位置（C の `place` 相当）。
    fn place(&self) -> (NodeId, Option<NodeId>) {
        let target = self.top();
        // (parent, before)
        let mut parent = target;
        let mut before: Option<NodeId> = None;
        if !self.foster {
            if self.dom.node(target).tag == TAG_TEMPLATE {
                if let Some(tc) = self.dom.node(target).tpl_content {
                    parent = tc;
                }
            }
            return (parent, before);
        }
        match self.dom.node(target).tag {
            TAG_TABLE | TAG_TBODY | TAG_TFOOT | TAG_THEAD | TAG_TR => {}
            _ => {
                if self.dom.node(target).tag == TAG_TEMPLATE {
                    if let Some(tc) = self.dom.node(target).tpl_content {
                        parent = tc;
                    }
                }
                return (parent, before);
            }
        }
        let mut iti: i32 = -1;
        let mut ita: i32 = -1;
        for i in (0..self.stack.len()).rev() {
            let tg = self.dom.node(self.stack[i]).tag;
            if iti < 0 && tg == TAG_TEMPLATE {
                iti = i as i32;
            }
            if ita < 0 && tg == TAG_TABLE {
                ita = i as i32;
            }
        }
        if iti >= 0 && (ita < 0 || iti > ita) {
            let tpl = self.stack[iti as usize];
            let tcf = self.dom.node(tpl).tpl_content;
            parent = tcf.unwrap_or(tpl);
            return (parent, before);
        }
        if ita < 0 {
            parent = self.body.or(self.html).unwrap_or(target);
            return (parent, before);
        }
        let tab = self.stack[ita as usize];
        if self.dom.node(tab).parent.is_some() {
            parent = self.dom.node(tab).parent.unwrap();
            before = Some(tab);
            return (parent, before);
        }
        parent = if ita > 0 {
            self.stack[ita as usize - 1]
        } else {
            self.dom.root
        };
        (parent, before)
    }

    fn append_placed(&mut self, n: NodeId) {
        let (parent, before) = self.place();
        match before {
            Some(b) => self.dom.insert_child_before(parent, n, b),
            None => self.dom.append_child(parent, n),
        }
    }

    fn insert_element(&mut self, tok: &Tok, do_push: bool) {
        let n = self.make_element(tok);
        self.append_placed(n);
        if do_push {
            self.push(n);
        }
    }

    // ---- スコープ ----

    fn has_open(&self, tag: Tag) -> bool {
        self.stack.iter().any(|&n| self.dom.node(n).tag == tag)
    }

    fn scope_barrier(&self, n: NodeId, k: ScopeKind) -> bool {
        let node = self.dom.node(n);
        if k == ScopeKind::Table {
            return node.ns == Ns::Html
                && (node.tag == TAG_HTML || node.tag == TAG_TABLE || node.tag == TAG_TEMPLATE);
        }
        if node.ns == Ns::Html {
            if k == ScopeKind::Button && node.tag == TAG_BUTTON {
                return true;
            }
            if k == ScopeKind::ListItem && (node.tag == TAG_OL || node.tag == TAG_UL) {
                return true;
            }
            return matches!(
                node.tag,
                TAG_APPLET
                    | TAG_CAPTION
                    | TAG_HTML
                    | TAG_TABLE
                    | TAG_TD
                    | TAG_TH
                    | TAG_MARQUEE
                    | TAG_OBJECT
                    | TAG_TEMPLATE
                    | TAG_SELECT
            );
        }
        if node.ns == Ns::Mathml {
            return matches!(
                node.tag,
                TAG_MI | TAG_MO | TAG_MN | TAG_MS | TAG_MTEXT | TAG_ANNOTATION_XML
            );
        }
        if node.ns == Ns::Svg {
            return node.tag == TAG_FOREIGNOBJECT || node.tag == TAG_DESC || node.tag == TAG_TITLE;
        }
        false
    }

    fn has_in_scope2(&self, tag: Tag, k: ScopeKind) -> bool {
        for i in (0..self.stack.len()).rev() {
            let x = self.stack[i];
            let node = self.dom.node(x);
            if node.ns == Ns::Html && node.tag == tag {
                return true;
            }
            if self.scope_barrier(x, k) {
                return false;
            }
        }
        false
    }

    fn has_in_table_scope(&self, tag: Tag) -> bool {
        self.has_in_scope2(tag, ScopeKind::Table)
    }

    fn pop_until(&mut self, tag: Tag) {
        while !self.stack.is_empty() {
            let n = self.top();
            let (tg, ns) = (self.dom.node(n).tag, self.dom.node(n).ns);
            self.pop();
            if tg == tag && ns == Ns::Html {
                break;
            }
        }
    }

    fn gen_implied(&mut self, except: Tag) {
        while !self.stack.is_empty() {
            let n = self.top();
            let node = self.dom.node(n);
            if node.ns != Ns::Html {
                return;
            }
            let tg = node.tag;
            if tg == except {
                return;
            }
            match tg {
                TAG_DD | TAG_DT | TAG_LI | TAG_OPTGROUP | TAG_OPTION | TAG_P | TAG_RP
                | TAG_RT => {
                    self.pop();
                }
                _ => return,
            }
        }
    }

    fn gen_implied_thorough(&mut self) {
        while !self.stack.is_empty() {
            let n = self.top();
            let node = self.dom.node(n);
            if node.ns != Ns::Html {
                return;
            }
            match node.tag {
                TAG_CAPTION
                | TAG_COLGROUP
                | TAG_DD
                | TAG_DT
                | TAG_LI
                | TAG_OPTGROUP
                | TAG_OPTION
                | TAG_P
                | TAG_RP
                | TAG_RT
                | TAG_TBODY
                | TAG_TD
                | TAG_TFOOT
                | TAG_TH
                | TAG_THEAD
                | TAG_TR => {
                    self.pop();
                }
                _ => return,
            }
        }
    }

    fn clear_back(&mut self, ctx: &[Tag]) {
        while !self.stack.is_empty() && !ctx.contains(&self.dom.node(self.top()).tag) {
            self.pop();
        }
    }

    fn close_p_if_open(&mut self) {
        if !self.has_in_scope2(TAG_P, ScopeKind::Button) {
            return;
        }
        self.gen_implied(TAG_P);
        if self.dom.node(self.top()).tag != TAG_P {
            self.dom.n_errors += 1;
        }
        self.pop_until(TAG_P);
    }

    fn implied_close(&mut self, a: Tag, b2: Tag, barrier1: Tag, barrier2: Tag) {
        for i in (0..self.stack.len()).rev() {
            let tg = self.dom.node(self.stack[i]).tag;
            if tg == a || (b2 != tags::TAG_UNKNOWN && tg == b2) {
                while self.stack.len() > i {
                    self.pop();
                }
                return;
            }
            if (barrier1 != tags::TAG_UNKNOWN && tg == barrier1)
                || (barrier2 != tags::TAG_UNKNOWN && tg == barrier2)
            {
                return;
            }
        }
    }

    fn close_heading_if_open(&mut self) {
        if self.stack.is_empty() {
            return;
        }
        let tg = self.dom.node(self.top()).tag;
        if (TAG_H1..=TAG_H6).contains(&tg) {
            self.pop();
        }
    }

    fn closes_p(tag: Tag) -> bool {
        matches!(
            tag,
            TAG_P
                | TAG_DIV
                | TAG_H1
                | TAG_H2
                | TAG_H3
                | TAG_H4
                | TAG_H5
                | TAG_H6
                | TAG_UL
                | TAG_OL
                | TAG_DL
                | TAG_PRE
                | TAG_BLOCKQUOTE
                | TAG_TABLE
                | TAG_FORM
                | TAG_FIGURE
                | TAG_FIGCAPTION
                | TAG_ADDRESS
                | TAG_ARTICLE
                | TAG_ASIDE
                | TAG_FOOTER
                | TAG_HEADER
                | TAG_HR
                | TAG_MAIN
                | TAG_NAV
                | TAG_SECTION
                | TAG_CENTER
                | TAG_DETAILS
                | TAG_DIALOG
                | TAG_HGROUP
                | TAG_SEARCH
                | TAG_SUMMARY
                | TAG_LISTING
                | TAG_PLAINTEXT
                | TAG_XMP
                | TAG_FIELDSET
                | TAG_DIR
                | TAG_MENU
        )
    }

    // ---- テキスト ----

    fn append_text(&mut self, text: &[u8]) {
        if text.is_empty() {
            return;
        }
        let (parent, before) = self.place();
        // before がある（foster の兄挿入）時は「直前の兄が TEXT ならそこに連結」。
        let merge_to: Option<NodeId> = if let Some(b) = before {
            let mut s = self.dom.node(parent).first_child;
            let mut prev = None;
            while let Some(c) = s {
                if c == b {
                    break;
                }
                prev = Some(c);
                s = self.dom.node(c).next_sibling;
            }
            prev.filter(|&p| self.dom.node(p).kind == NodeKind::Text)
        } else {
            self.dom
                .node(parent)
                .last_child
                .filter(|&l| self.dom.node(l).kind == NodeKind::Text)
        };
        if let Some(m) = merge_to {
            let old = self.dom.node(m).name.clone();
            let mut merged = old;
            merged.extend_from_slice(text);
            self.dom.node_mut(m).name = merged;
            return;
        }
        let n = self.dom.alloc_node(NodeKind::Text);
        self.dom.node_mut(n).name = text.to_vec();
        match before {
            Some(b) => self.dom.insert_child_before(parent, n, b),
            None => self.dom.append_child(parent, n),
        }
    }

    fn pend_add(&mut self, text: &[u8]) {
        if text.is_empty() {
            return;
        }
        self.pend.extend_from_slice(text);
        if text.iter().any(|&c| !c.is_ascii_whitespace()) {
            self.pend_nonws = true;
        }
    }

    fn pend_reset(&mut self) {
        self.pend.clear();
        self.pend_nonws = false;
    }

    fn pend_flush(&mut self) {
        if self.pend.is_empty() {
            return;
        }
        let keep = std::mem::take(&mut self.pend);
        let nonws = self.pend_nonws;
        self.pend_nonws = false;
        if !nonws {
            let f = self.foster;
            self.foster = false;
            self.append_text(&keep);
            self.foster = f;
            return;
        }
        let tableish = matches!(
            self.dom.node(self.top()).tag,
            TAG_TABLE | TAG_TBODY | TAG_TFOOT | TAG_THEAD | TAG_TR
        );
        let f = self.foster;
        self.foster = f || tableish;
        let tok = Tok {
            kind: TokKind::Text,
            text: keep,
            ..Tok::default()
        };
        self.step_in_body(tok);
        self.foster = f;
    }

    // ---- template ----

    fn tpl_start(&mut self, tok: &Tok) {
        self.afe_insert_marker();
        self.insert_element(tok, true);
        let tpl = self.top();
        if self.dom.node(tpl).tpl_content.is_none() {
            let c = self.dom.alloc_node(NodeKind::Document);
            self.dom.node_mut(tpl).tpl_content = Some(c);
        }
        self.tpl_modes.push(Mode::InTemplate);
        self.mode = Mode::InTemplate;
    }

    fn tpl_end(&mut self) {
        if !self.has_open(TAG_TEMPLATE) {
            self.dom.n_errors += 1;
            return;
        }
        self.gen_implied_thorough();
        if self.dom.node(self.top()).tag != TAG_TEMPLATE {
            self.dom.n_errors += 1;
        }
        self.pop_until(TAG_TEMPLATE);
        self.afe_clear_to_marker();
        self.tpl_modes.pop();
        self.reset_mode();
    }

    fn tpl_route(&mut self, m: Mode) {
        self.mode = m;
        if let Some(last) = self.tpl_modes.last_mut() {
            *last = m;
        }
    }

    fn tpl_content_has_real(&self) -> bool {
        let mut tpl = None;
        for &n in self.stack.iter().rev() {
            let node = self.dom.node(n);
            if node.tag == TAG_TEMPLATE && node.ns == Ns::Html {
                tpl = Some(n);
                break;
            }
        }
        let tc = tpl.and_then(|n| self.dom.node(n).tpl_content);
        let mut c = tc.and_then(|tc| self.dom.node(tc).first_child);
        while let Some(cid) = c {
            let node = self.dom.node(cid);
            if node.kind == NodeKind::Element {
                if node.tag == tags::TAG_UNKNOWN {
                    return true;
                }
                match node.tag {
                    TAG_TR | TAG_TD | TAG_TH | TAG_TBODY | TAG_THEAD | TAG_TFOOT
                    | TAG_CAPTION | TAG_COLGROUP | TAG_COL | TAG_TABLE | TAG_TEMPLATE
                    | TAG_SCRIPT | TAG_STYLE | TAG_META | TAG_LINK | TAG_NOFRAMES => {}
                    _ => return true,
                }
            }
            c = node.next_sibling;
        }
        false
    }

    fn step_in_template(&mut self, tok: Tok) {
        if tok.kind == TokKind::Eof {
            if !self.has_open(TAG_TEMPLATE) {
                self.stopped = true;
                return;
            }
            self.pop_until(TAG_TEMPLATE);
            self.afe_clear_to_marker();
            self.tpl_modes.pop();
            self.reset_mode();
            self.step(tok);
            return;
        }
        if tok.kind == TokKind::End && tok.tag == TAG_TEMPLATE {
            self.tpl_end();
            return;
        }
        if tok.kind != TokKind::Start {
            self.step_in_body(tok);
            return;
        }
        match tok.tag {
            TAG_CAPTION | TAG_COLGROUP | TAG_TBODY | TAG_TFOOT | TAG_THEAD => {
                if self.tpl_content_has_real() {
                    self.dom.n_errors += 1;
                    return;
                }
                self.tpl_route(Mode::InTable);
                self.step(tok);
            }
            TAG_COL => {
                if self.tpl_content_has_real() {
                    self.dom.n_errors += 1;
                    return;
                }
                self.tpl_route(Mode::InColumnGroup);
                self.step(tok);
            }
            TAG_TR => {
                if self.tpl_content_has_real() {
                    self.dom.n_errors += 1;
                    return;
                }
                self.tpl_route(Mode::InTableBody);
                self.step(tok);
            }
            TAG_TD | TAG_TH => {
                if self.tpl_content_has_real() {
                    self.dom.n_errors += 1;
                    return;
                }
                self.tpl_route(Mode::InRow);
                self.step(tok);
            }
            TAG_FRAME | TAG_FRAMESET => {
                self.dom.n_errors += 1;
            }
            _ => self.step_in_body(tok),
        }
    }

    // ---- reset mode ----

    fn frag_ctx_mode(&self) -> Mode {
        let fc = self.frag_ctx.as_ref().unwrap();
        if fc.ns == Ns::Html {
            match fc.tag {
                TAG_TEMPLATE => *self.tpl_modes.last().unwrap_or(&Mode::InTemplate),
                TAG_TD | TAG_TH => Mode::InCell,
                TAG_TR => Mode::InRow,
                TAG_TBODY | TAG_THEAD | TAG_TFOOT => Mode::InTableBody,
                TAG_CAPTION => Mode::InCaption,
                TAG_COLGROUP => Mode::InColumnGroup,
                TAG_TABLE => Mode::InTable,
                TAG_FRAMESET => Mode::InFrameset,
                TAG_HTML => {
                    if self.head.is_some() {
                        Mode::AfterHead
                    } else {
                        Mode::BeforeHead
                    }
                }
                _ => Mode::InBody,
            }
        } else {
            Mode::InBody
        }
    }

    fn reset_mode(&mut self) {
        for i in (0..self.stack.len()).rev() {
            let tg = self.dom.node(self.stack[i]).tag;
            if i == 0 {
                if self.frag {
                    self.mode = self.frag_ctx_mode();
                    return;
                }
                self.mode = if self.head.is_some() {
                    Mode::AfterHead
                } else {
                    Mode::BeforeHead
                };
                return;
            }
            match tg {
                TAG_TD | TAG_TH => {
                    self.mode = Mode::InCell;
                    return;
                }
                TAG_TR => {
                    self.mode = Mode::InRow;
                    return;
                }
                TAG_TBODY | TAG_THEAD | TAG_TFOOT => {
                    self.mode = Mode::InTableBody;
                    return;
                }
                TAG_CAPTION => {
                    self.mode = Mode::InCaption;
                    return;
                }
                TAG_COLGROUP => {
                    self.mode = Mode::InColumnGroup;
                    return;
                }
                TAG_TABLE => {
                    self.mode = Mode::InTable;
                    return;
                }
                TAG_TEMPLATE => {
                    self.mode = *self.tpl_modes.last().unwrap_or(&Mode::InBody);
                    return;
                }
                TAG_HEAD => {
                    self.mode = Mode::InHead;
                    return;
                }
                TAG_BODY => {
                    self.mode = Mode::InBody;
                    return;
                }
                _ => continue,
            }
        }
        self.mode = if self.head.is_some() {
            Mode::AfterHead
        } else {
            Mode::BeforeHead
        };
    }

    fn close_cell(&mut self) {
        if self.has_in_table_scope(TAG_TD) {
            self.gen_implied(TAG_TD);
            if self.dom.node(self.top()).tag != TAG_TD {
                self.dom.n_errors += 1;
            }
            self.pop_until(TAG_TD);
        } else if self.has_in_table_scope(TAG_TH) {
            self.gen_implied(TAG_TH);
            if self.dom.node(self.top()).tag != TAG_TH {
                self.dom.n_errors += 1;
            }
            self.pop_until(TAG_TH);
        }
        self.afe_clear_to_marker();
        self.mode = Mode::InRow;
    }

    // ---- AFE + AAA ----

    fn is_formatting(tag: Tag) -> bool {
        matches!(
            tag,
            TAG_A
                | TAG_B
                | TAG_BIG
                | TAG_CODE
                | TAG_EM
                | TAG_FONT
                | TAG_I
                | TAG_NOBR
                | TAG_S
                | TAG_SMALL
                | TAG_STRIKE
                | TAG_STRONG
                | TAG_TT
                | TAG_U
        )
    }

    fn is_special(&self, n: NodeId) -> bool {
        let node = self.dom.node(n);
        if node.kind != NodeKind::Element {
            return false;
        }
        if node.ns == Ns::Mathml {
            return matches!(node.tag, TAG_MI | TAG_MO | TAG_MN | TAG_MS | TAG_MTEXT | TAG_ANNOTATION_XML);
        }
        if node.ns == Ns::Svg {
            return node.tag == TAG_FOREIGNOBJECT || node.tag == TAG_DESC || node.tag == TAG_TITLE;
        }
        matches!(
            node.tag,
            TAG_ADDRESS
                | TAG_APPLET
                | TAG_AREA
                | TAG_ARTICLE
                | TAG_ASIDE
                | TAG_BASE
                | TAG_BASEFONT
                | TAG_BLOCKQUOTE
                | TAG_BODY
                | TAG_BR
                | TAG_BUTTON
                | TAG_CAPTION
                | TAG_CENTER
                | TAG_COL
                | TAG_COLGROUP
                | TAG_DD
                | TAG_DIR
                | TAG_DIV
                | TAG_DL
                | TAG_DT
                | TAG_EMBED
                | TAG_FIELDSET
                | TAG_FIGCAPTION
                | TAG_FIGURE
                | TAG_FOOTER
                | TAG_FORM
                | TAG_FRAME
                | TAG_FRAMESET
                | TAG_H1
                | TAG_H2
                | TAG_H3
                | TAG_H4
                | TAG_H5
                | TAG_H6
                | TAG_HEAD
                | TAG_HEADER
                | TAG_HR
                | TAG_HTML
                | TAG_IFRAME
                | TAG_IMG
                | TAG_INPUT
                | TAG_KEYGEN
                | TAG_LI
                | TAG_LINK
                | TAG_LISTING
                | TAG_MAIN
                | TAG_MARQUEE
                | TAG_MENU
                | TAG_META
                | TAG_NAV
                | TAG_NOEMBED
                | TAG_NOFRAMES
                | TAG_NOSCRIPT
                | TAG_OBJECT
                | TAG_OL
                | TAG_P
                | TAG_PARAM
                | TAG_PLAINTEXT
                | TAG_PRE
                | TAG_SCRIPT
                | TAG_SECTION
                | TAG_SELECT
                | TAG_SOURCE
                | TAG_STYLE
                | TAG_TABLE
                | TAG_TBODY
                | TAG_TD
                | TAG_TEMPLATE
                | TAG_TEXTAREA
                | TAG_TFOOT
                | TAG_DETAILS
                | TAG_DIALOG
                | TAG_HGROUP
                | TAG_SEARCH
                | TAG_SUMMARY
                | TAG_TH
                | TAG_THEAD
                | TAG_TITLE
                | TAG_TR
                | TAG_TRACK
                | TAG_UL
                | TAG_WBR
                | TAG_XMP
        )
    }

    fn stack_find_node(&self, n: NodeId) -> Option<usize> {
        self.stack.iter().position(|&x| x == n)
    }

    fn node_in_default_scope(&self, n: NodeId) -> bool {
        for i in (0..self.stack.len()).rev() {
            let x = self.stack[i];
            if x == n {
                return true;
            }
            if self.scope_barrier(x, ScopeKind::Default) {
                return false;
            }
        }
        false
    }

    fn has_in_default_scope_tag(&self, tag: Tag) -> bool {
        self.has_in_scope2(tag, ScopeKind::Default)
    }

    fn attrs_equal(&self, a: NodeId, b: NodeId) -> bool {
        let na = &self.dom.node(a).attrs;
        let nb = &self.dom.node(b).attrs;
        if na.len() != nb.len() {
            return false;
        }
        for x in na {
            let hit = nb
                .iter()
                .any(|y| str_eq_ci(&x.name, &y.name) && str_eq_ci(&x.value, &y.value));
            if !hit {
                return false;
            }
        }
        true
    }

    fn afe_push(&mut self, n: NodeId) {
        // Noah's Ark: 同一 tag/ns/attrs の未閉鎖が 3 件目で最古を除去
        let mut eq = 0;
        let mut oldest: Option<usize> = None;
        for i in (0..self.afe.len()).rev() {
            if self.afe[i].marker {
                break;
            }
            let x = self.afe[i].node;
            if self.dom.node(x).tag == self.dom.node(n).tag
                && self.dom.node(x).ns == self.dom.node(n).ns
                && self.attrs_equal(x, n)
            {
                eq += 1;
                oldest = Some(i);
            }
        }
        if eq >= 3 {
            if let Some(o) = oldest {
                self.afe.remove(o);
            }
        }
        self.afe.push(AfeEntry { node: n, marker: false });
    }

    fn afe_insert_marker(&mut self) {
        self.afe.push(AfeEntry {
            node: self.dom.root,
            marker: true,
        });
    }

    fn afe_clear_to_marker(&mut self) {
        while self.afe.last().is_some_and(|e| !e.marker) {
            self.afe.pop();
        }
        if self.afe.last().is_some_and(|e| e.marker) {
            self.afe.pop();
        }
    }

    fn afe_find_tag(&self, tag: Tag) -> Option<usize> {
        for i in (0..self.afe.len()).rev() {
            if self.afe[i].marker {
                return None;
            }
            if self.dom.node(self.afe[i].node).tag == tag {
                return Some(i);
            }
        }
        None
    }

    fn afe_has_node(&self, n: NodeId) -> bool {
        for i in (0..self.afe.len()).rev() {
            if self.afe[i].marker {
                return false;
            }
            if self.afe[i].node == n {
                return true;
            }
        }
        false
    }

    fn afe_find_node(&self, n: NodeId) -> Option<usize> {
        for i in (0..self.afe.len()).rev() {
            if self.afe[i].marker {
                break;
            }
            if self.afe[i].node == n {
                return Some(i);
            }
        }
        None
    }

    fn afe_remove_at(&mut self, i: usize) {
        if i < self.afe.len() {
            self.afe.remove(i);
        }
    }

    fn afe_insert_at(&mut self, pos: usize, n: NodeId) {
        let pos = pos.min(self.afe.len());
        self.afe.insert(pos, AfeEntry { node: n, marker: false });
    }

    fn clone_element(&mut self, src: NodeId) -> NodeId {
        let n = self.dom.alloc_node(NodeKind::Element);
        let (tag, ns, name, attrs) = {
            let s = self.dom.node(src);
            (s.tag, s.ns, s.name.clone(), s.attrs.clone())
        };
        {
            let node = self.dom.node_mut(n);
            node.tag = tag;
            node.ns = ns;
            node.name = name;
            node.attrs = attrs;
        }
        n
    }

    fn move_children(&mut self, src: NodeId, dst: NodeId) {
        let mut c = self.dom.node(src).first_child;
        let mut moved = Vec::new();
        while let Some(cid) = c {
            moved.push(cid);
            c = self.dom.node(cid).next_sibling;
        }
        for cid in moved {
            self.dom.detach(cid);
            self.dom.append_child(dst, cid);
        }
    }

    fn afe_reconstruct(&mut self) {
        if self.afe.is_empty() {
            return;
        }
        let last = self.afe.len() - 1;
        if self.afe[last].marker || self.stack_find_node(self.afe[last].node).is_some() {
            return;
        }
        let mut start = 0;
        for k in (0..last).rev() {
            if self.afe[k].marker || self.stack_find_node(self.afe[k].node).is_some() {
                start = k + 1;
                break;
            }
        }
        for k in start..self.afe.len() {
            let c = self.clone_element(self.afe[k].node);
            self.append_placed(c);
            self.push(c);
            self.afe[k].node = c;
        }
    }

    fn any_other_end_tag(&mut self, tag: Tag, end_name: &[u8]) {
        for i in (0..self.stack.len()).rev() {
            let node_id = self.stack[i];
            let node = self.dom.node(node_id);
            let m = if tag != tags::TAG_UNKNOWN {
                node.tag == tag && node.ns == Ns::Html
            } else {
                node.kind == NodeKind::Element
                    && node.tag == tags::TAG_UNKNOWN
                    && node.ns == Ns::Html
                    && str_eq_ci(&node.name, end_name)
            };
            if m {
                self.gen_implied(tag);
                if self.dom.node(self.top()).tag != tag {
                    self.dom.n_errors += 1;
                }
                while self.stack.len() > i {
                    self.pop();
                }
                return;
            }
            if self.is_special(node_id) {
                self.dom.n_errors += 1;
                return;
            }
        }
        self.dom.n_errors += 1;
    }

    fn end_in_scope(&mut self, tag: Tag, k: ScopeKind, except_self: bool) {
        if !self.has_in_scope2(tag, k) {
            self.dom.n_errors += 1;
            return;
        }
        self.gen_implied(if except_self { tag } else { tags::TAG_UNKNOWN });
        if self.dom.node(self.top()).tag != tag {
            self.dom.n_errors += 1;
        }
        self.pop_until(tag);
    }

    fn end_hgroup(&mut self) {
        let h = [TAG_H1, TAG_H2, TAG_H3, TAG_H4, TAG_H5, TAG_H6];
        if !h.iter().any(|&x| self.has_open(x)) {
            self.dom.n_errors += 1;
            return;
        }
        self.gen_implied(tags::TAG_UNKNOWN);
        let tt = self.dom.node(self.top()).tag;
        if !(TAG_H1..=TAG_H6).contains(&tt) {
            self.dom.n_errors += 1;
        }
        for _ in 0..6 {
            if self.stack.is_empty() {
                break;
            }
            let tg = self.dom.node(self.top()).tag;
            if (TAG_H1..=TAG_H6).contains(&tg) {
                self.pop();
                break;
            }
            self.pop();
        }
    }

    fn stack_remove_at(&mut self, idx: usize) {
        if idx < self.stack.len() {
            self.stack.remove(idx);
        }
    }

    fn stack_insert_after(&mut self, idx: usize, n: NodeId) {
        self.stack.insert(idx + 1, n);
    }

    /// Adoption Agency Algorithm（WHATWG 厳密版）。
    fn adoption(&mut self, tag: Tag) {
        if !self.stack.is_empty()
            && self.dom.node(self.top()).tag == tag
            && !self.afe_has_node(self.top())
        {
            self.pop();
            return;
        }
        for _outer in 0..8 {
            let fi = match self.afe_find_tag(tag) {
                Some(fi) => fi,
                None => {
                    self.any_other_end_tag(tag, b"");
                    return;
                }
            };
            let fe = self.afe[fi].node;
            let fs = match self.stack_find_node(fe) {
                Some(fs) => fs,
                None => {
                    self.dom.n_errors += 1;
                    self.afe_remove_at(fi);
                    return;
                }
            };
            if !self.node_in_default_scope(fe) {
                self.dom.n_errors += 1;
                return;
            }
            if fe != self.top() {
                self.dom.n_errors += 1;
            }
            // furthest block = fe より上（index 大）で最初の special
            let mut fb: Option<usize> = None;
            for i in (fs + 1)..self.stack.len() {
                if self.is_special(self.stack[i]) {
                    fb = Some(i);
                    break;
                }
            }
            let fb = match fb {
                Some(fb) => fb,
                None => {
                    while self.stack.len() > fs {
                        self.pop();
                    }
                    self.afe_remove_at(fi);
                    return;
                }
            };
            let furthest = self.stack[fb];
            let ancestor = if fs > 0 {
                self.stack[fs - 1]
            } else {
                self.dom.root
            };
        let mut bookmark = fi + 1;
        let mut last_node = furthest;
        let mut ni = fb;
        let mut inner = 0u32;
        loop {
            inner += 1; // 12.1
            if ni == 0 {
                break;
            }
            ni -= 1;
            let mut node = self.stack[ni];
            if node == fe {
                break;
            }
            // 12.4: inner > 3 なら AFE から node を除去（WHATWG 打ち切り規則）
            if inner > 3 {
                if let Some(rm) = self.afe_find_node(node) {
                    self.afe_remove_at(rm);
                }
            }
            // 12.5: AFE に無い node は stack から除去して次へ
            let a2 = self.afe_find_node(node);
            if a2.is_none() {
                self.stack_remove_at(ni);
                continue;
            }
            let a2 = a2.unwrap();
            let clone = self.clone_element(node);
            self.afe[a2].node = clone;
            self.stack[ni] = clone;
            node = clone;
            if last_node == furthest {
                bookmark = a2 + 1;
            }
            self.dom.detach(last_node);
            self.dom.append_child(node, last_node);
            last_node = node;
        }
            self.dom.detach(last_node);
            let (parent, before) = self.place_for(ancestor);
            match before {
                Some(b) => self.dom.insert_child_before(parent, last_node, b),
                None => self.dom.append_child(parent, last_node),
            }
            let fclone = self.clone_element(fe);
            self.move_children(furthest, fclone);
            self.dom.append_child(furthest, fclone);
            if let Some(i) = self.afe_find_node(fe) {
                self.afe_remove_at(i);
            }
            let bookmark = bookmark.min(self.afe.len());
            self.afe_insert_at(bookmark, fclone);
            if let Some(fs2) = self.stack_find_node(fe) {
                self.stack_remove_at(fs2);
            }
            match self.stack_find_node(furthest) {
                Some(fb2) => self.stack_insert_after(fb2, fclone),
                None => self.push(fclone),
            }
        }
    }

    fn place_for(&self, target: NodeId) -> (NodeId, Option<NodeId>) {
        let mut parent = target;
        let mut before: Option<NodeId> = None;
        if self.dom.node(target).tag == TAG_TEMPLATE {
            if let Some(tc) = self.dom.node(target).tpl_content {
                parent = tc;
            }
        }
        match self.dom.node(target).tag {
            TAG_TABLE | TAG_TBODY | TAG_TFOOT | TAG_THEAD | TAG_TR => {
                let mut iti: i32 = -1;
                let mut ita: i32 = -1;
                for i in (0..self.stack.len()).rev() {
                    let tg = self.dom.node(self.stack[i]).tag;
                    if iti < 0 && tg == TAG_TEMPLATE {
                        iti = i as i32;
                    }
                    if ita < 0 && tg == TAG_TABLE {
                        ita = i as i32;
                    }
                }
                if iti >= 0 && (ita < 0 || iti > ita) {
                    let tpl = self.stack[iti as usize];
                    let tcf = self.dom.node(tpl).tpl_content;
                    parent = tcf.unwrap_or(tpl);
                    return (parent, None);
                }
                if ita < 0 {
                    parent = self.body.or(self.html).unwrap_or(target);
                    return (parent, None);
                }
                let tab = self.stack[ita as usize];
                if self.dom.node(tab).parent.is_some() {
                    parent = self.dom.node(tab).parent.unwrap();
                    before = Some(tab);
                    return (parent, before);
                }
                parent = if ita > 0 {
                    self.stack[ita as usize - 1]
                } else {
                    self.dom.root
                };
                (parent, None)
            }
            _ => (parent, before),
        }
    }

    fn synth_start(tag: Tag) -> Tok {
        Tok {
            kind: TokKind::Start,
            tag,
            ..Tok::default()
        }
    }

    fn attr_is_ci(tok: &Tok, name: &[u8], val: &[u8]) -> bool {
        tok.attrs
            .iter()
            .any(|a| str_eq_ci(&a.name, name) && str_eq_ci(&a.value, val))
    }

    // ---- quirks ----

    fn quirks_pub_prefix(pub_: &[u8]) -> bool {
        const P: &[&str] = &[
            "+//silmaril//dtd html pro v0r11 19970101//",
            "-//advasoft ltd//dtd html 3.0 aswedit + extensions//",
            "-//as//dtd html 3.0 aswedit + extensions//",
            "-//ietf//dtd html 2.0 level 1//",
            "-//ietf//dtd html 2.0 level 2//",
            "-//ietf//dtd html 2.0 strict level 1//",
            "-//ietf//dtd html 2.0 strict level 2//",
            "-//ietf//dtd html 2.0 strict//",
            "-//ietf//dtd html 2.0//",
            "-//ietf//dtd html 2.1e//",
            "-//ietf//dtd html 3.0//",
            "-//ietf//dtd html 3.2 final//",
            "-//ietf//dtd html 3.2//",
            "-//ietf//dtd html 3//",
            "-//ietf//dtd html level 0//",
            "-//ietf//dtd html level 1//",
            "-//ietf//dtd html level 2//",
            "-//ietf//dtd html level 3//",
            "-//ietf//dtd html strict level 0//",
            "-//ietf//dtd html strict level 1//",
            "-//ietf//dtd html strict level 2//",
            "-//ietf//dtd html strict level 3//",
            "-//ietf//dtd html strict//",
            "-//ietf//dtd html//",
            "-//metrius//dtd metrius presentational//",
            "-//microsoft//dtd internet explorer 2.0 html strict//",
            "-//microsoft//dtd internet explorer 2.0 html//",
            "-//microsoft//dtd internet explorer 2.0 tables//",
            "-//microsoft//dtd internet explorer 3.0 html strict//",
            "-//microsoft//dtd internet explorer 3.0 html//",
            "-//microsoft//dtd internet explorer 3.0 tables//",
            "-//netscape comm. corp.//dtd html//",
            "-//netscape comm. corp.//dtd strict html//",
            "-//o'reilly and associates//dtd html 2.0//",
            "-//o'reilly and associates//dtd html extended 1.0//",
            "-//o'reilly and associates//dtd html extended relaxed 1.0//",
            "-//softquad software//dtd hotmetal pro 6.0::19990601::extensions to html 4.0//",
            "-//softquad//dtd hotmetal pro 4.0::19971010::extensions to html 4.0//",
            "-//spyglass//dtd html 2.0 extended//",
            "-//sq//dtd html 2.0 hotmetal + extensions//",
            "-//sun microsystems corp.//dtd hotjava html//",
            "-//sun microsystems corp.//dtd hotjava strict html//",
            "-//w3c//dtd html 3 1995-03-24//",
            "-//w3c//dtd html 3.2 draft//",
            "-//w3c//dtd html 3.2 final//",
            "-//w3c//dtd html 3.2//",
            "-//w3c//dtd html 3.2s draft//",
            "-//w3c//dtd html 4.0 frameset//",
            "-//w3c//dtd html 4.0 transitional//",
            "-//w3c//dtd html experimental 19960712//",
            "-//w3c//dtd html experimental 970421//",
            "-//w3c//dtd w3 html//",
            "-//w3o//dtd w3 html 3.0//",
            "-//webtechs//dtd mozilla html 2.0//",
            "-//webtechs//dtd mozilla html//",
        ];
        for &p in P {
            let pb = p.as_bytes();
            // pub は ascii-insensitive 前方一致
            if pub_.len() >= pb.len()
                && pub_[..pb.len()]
                    .iter()
                    .zip(pb)
                    .all(|(a, b)| a.to_ascii_lowercase() == *b)
            {
                return true;
            }
        }
        false
    }

    fn quirks_pub_exact(pub_: &[u8]) -> bool {
        const E: &[&str] = &[
            "-//w3o//dtd w3 html strict 3.0//en//",
            "-/w3c/dtd html 4.0 transitional/en",
            "html",
        ];
        let lower: Vec<u8> = pub_.iter().map(|&c| c.to_ascii_lowercase()).collect();
        E.iter().any(|&e| lower == e.as_bytes())
    }

    fn doctype_is_quirks(tok: &Tok) -> bool {
        if !tok.dt_has_name {
            return true;
        }
        if !str_eq_ci(&tok.text, b"html") {
            return true;
        }
        if tok.dt_has_pub {
            let pub_ = &tok.dt_pub;
            if Self::quirks_pub_prefix(pub_) || Self::quirks_pub_exact(pub_) {
                return true;
            }
            if !tok.dt_has_sys {
                let lc: Vec<u8> = pub_.iter().map(|&c| c.to_ascii_lowercase()).collect();
                let a = b"-//w3c//dtd html 4.01 frameset//";
                let b = b"-//w3c//dtd html 4.01 transitional//";
                if lc.starts_with(a) || lc.starts_with(b) {
                    return true;
                }
            }
        }
        if tok.dt_has_sys {
            let s = &tok.dt_sys;
            let ibm = b"http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd";
            if str_eq_ci(s, ibm) {
                return true;
            }
        }
        if tok.dt_has_pub && !tok.dt_has_sys {
            return true;
        }
        false
    }

    // ---- 各挿入モード ----

    fn step_initial(&mut self, tok: Tok) {
        match tok.kind {
            TokKind::Doctype => {
                self.dom.quirks = Self::doctype_is_quirks(&tok);
                self.mode = Mode::BeforeHtml;
                let d = self.dom.alloc_node(NodeKind::Doctype);
                self.dom.node_mut(d).doctype = Some(Doctype {
                    name: tok.text.clone(),
                    has_name: tok.dt_has_name,
                    pub_id: tok.dt_pub.clone(),
                    sys_id: tok.dt_sys.clone(),
                });
                self.dom.append_child(self.dom.root, d);
            }
            TokKind::Comment => {
                let c = self.make_comment(&tok);
                self.dom.append_child(self.dom.root, c);
            }
            TokKind::Text => {
                let ws = Self::peel_leading_ws(&tok.text);
                if ws == tok.text.len() {
                    return;
                }
                self.dom.quirks = true;
                self.dom.n_errors += 1;
                self.mode = Mode::BeforeHtml;
                let mut tok2 = tok;
                tok2.text = tok2.text[ws..].to_vec();
                self.step(tok2);
            }
            _ => {
                self.dom.quirks = true;
                self.dom.n_errors += 1;
                self.mode = Mode::BeforeHtml;
                self.step(tok);
            }
        }
    }

    fn make_comment(&mut self, tok: &Tok) -> NodeId {
        let n = self.dom.alloc_node(NodeKind::Comment);
        self.dom.node_mut(n).name = tok.text.clone();
        if tok.is_pi && !tok.pi_target.is_empty() {
            self.dom.node_mut(n).pi_target = tok.pi_target.clone();
        }
        n
    }

    fn ensure_html(&mut self) {
        if self.html.is_none() {
            let h = self.make_element(&Self::synth_start(TAG_HTML));
            self.dom.append_child(self.dom.root, h);
            self.html = Some(h);
            self.push(h);
        }
    }

    fn ensure_head(&mut self) {
        self.ensure_html();
        if self.head.is_none() {
            let h = self.make_element(&Self::synth_start(TAG_HEAD));
            let parent = self.top();
            self.dom.append_child(parent, h);
            self.head = Some(h);
            self.push(h);
        }
    }

    fn ensure_body(&mut self) {
        self.ensure_html();
        if self.head.is_none() {
            self.ensure_head();
        }
        while !self.stack.is_empty() && self.dom.node(self.top()).tag == TAG_HEAD {
            self.pop();
        }
        if self.body.is_none() {
            let b = self.make_element(&Self::synth_start(TAG_BODY));
            let html = self.html.unwrap();
            self.dom.append_child(html, b);
            self.body = Some(b);
            self.push(b);
        }
    }

    fn step_before_html(&mut self, tok: Tok) {
        if tok.kind == TokKind::Comment {
            let c = self.make_comment(&tok);
            self.dom.append_child(self.dom.root, c);
            return;
        }
        if tok.kind == TokKind::Text {
            let ws = Self::peel_leading_ws(&tok.text);
            if ws == tok.text.len() {
                return;
            }
            self.mode = Mode::BeforeHead;
            let mut tok2 = tok;
            tok2.text = tok2.text[ws..].to_vec();
            self.step(tok2);
            return;
        }
        if tok.kind == TokKind::Start && tok.tag == TAG_HTML {
            if self.html.is_none() {
                let h = self.make_element(&tok);
                self.dom.append_child(self.dom.root, h);
                self.html = Some(h);
                self.push(h);
            }
            self.mode = Mode::BeforeHead;
            return;
        }
        self.mode = Mode::BeforeHead;
        self.step(tok);
    }

    fn step_before_head(&mut self, tok: Tok) {
        if tok.kind == TokKind::Text {
            let ws = Self::peel_leading_ws(&tok.text);
            if ws == tok.text.len() {
                return;
            }
            self.mode = Mode::InHead;
            self.ensure_head();
            let mut tok2 = tok;
            tok2.text = tok2.text[ws..].to_vec();
            self.step(tok2);
            return;
        }
        if tok.kind == TokKind::Comment {
            let c = self.make_comment(&tok);
            let parent = self.html.unwrap_or(self.dom.root);
            self.dom.append_child(parent, c);
            return;
        }
        if tok.kind == TokKind::End
            && tok.tag != TAG_HEAD
            && tok.tag != TAG_HTML
            && tok.tag != TAG_BODY
            && tok.tag != TAG_BR
        {
            self.dom.n_errors += 1;
            return;
        }
        if tok.kind == TokKind::Start && tok.tag == TAG_HEAD {
            self.ensure_html();
            let h = self.make_element(&tok);
            let parent = self.top();
            self.dom.append_child(parent, h);
            self.head = Some(h);
            self.push(h);
            self.mode = Mode::InHead;
            return;
        }
        self.mode = Mode::InHead;
        self.ensure_head();
        self.step(tok);
    }

    fn remove_from_stack(&mut self, n: NodeId) {
        if let Some(i) = self.stack.iter().position(|&x| x == n) {
            self.stack.remove(i);
        }
    }

    fn step_in_head(&mut self, tok: Tok) {
        if tok.kind == TokKind::Comment {
            let c = self.make_comment(&tok);
            let parent = self.top();
            self.dom.append_child(parent, c);
            return;
        }
        if tok.kind == TokKind::Text {
            let ws_len = Self::peel_leading_ws(&tok.text);
            if ws_len > 0 {
                self.append_text(&tok.text[..ws_len]);
            }
            if ws_len == tok.text.len() {
                return;
            }
            self.mode = Mode::AfterHead;
            if self.dom.node(self.top()).tag == TAG_HEAD {
                self.pop();
            }
            let mut tok2 = tok;
            tok2.text = tok2.text[ws_len..].to_vec();
            self.step(tok2);
            return;
        }
        if tok.kind == TokKind::Start {
            match tok.tag {
                TAG_TITLE | TAG_STYLE | TAG_SCRIPT => {
                    let tag = tok.tag;
                    self.insert_element(&tok, true);
                    self.tok.set_raw(tag);
                }
                TAG_BASE | TAG_BASEFONT | TAG_BGSOUND | TAG_META | TAG_LINK => {
                    self.insert_element(&tok, false);
                }
                TAG_HEAD => {
                    self.dom.n_errors += 1;
                }
                TAG_TEMPLATE => self.tpl_start(&tok),
                TAG_NOSCRIPT => {
                    self.insert_element(&tok, true);
                    self.mode = Mode::InHeadNoscript;
                }
                TAG_NOFRAMES => {
                    let tag = tok.tag;
                    self.insert_element(&tok, true);
                    self.tok.set_raw(tag);
                }
                _ => {
                    self.mode = Mode::AfterHead;
                    if self.dom.node(self.top()).tag == TAG_HEAD {
                        self.pop();
                    }
                    self.step(tok);
                }
            }
            return;
        }
        if tok.kind == TokKind::End {
            if tok.tag == TAG_HEAD {
                if self.dom.node(self.top()).tag == TAG_HEAD {
                    self.pop();
                }
                self.mode = Mode::AfterHead;
                return;
            }
            if tok.tag == TAG_TEMPLATE {
                self.tpl_end();
                return;
            }
            if matches!(
                tok.tag,
                TAG_TITLE | TAG_STYLE | TAG_SCRIPT | TAG_TEXTAREA | TAG_NOSCRIPT
            ) {
                if self.dom.node(self.top()).tag == tok.tag {
                    self.pop();
                }
                return;
            }
            if tok.tag != TAG_BR && tok.tag != TAG_HTML && tok.tag != TAG_BODY {
                self.dom.n_errors += 1;
                return;
            }
            self.mode = Mode::AfterHead;
            if self.dom.node(self.top()).tag == TAG_HEAD {
                self.pop();
            }
            self.step(tok);
            return;
        }
        if tok.kind == TokKind::Eof {
            self.mode = Mode::AfterHead;
            if self.dom.node(self.top()).tag == TAG_HEAD {
                self.pop();
            }
            self.step(tok);
        }
    }

    fn step_in_head_noscript(&mut self, tok: Tok) {
        if tok.kind == TokKind::Comment {
            let c = self.make_comment(&tok);
            let (parent, before) = self.place();
            match before {
                Some(b) => self.dom.insert_child_before(parent, c, b),
                None => self.dom.append_child(parent, c),
            }
            return;
        }
        if tok.kind == TokKind::Text && tok.text.iter().all(|&c| c.is_ascii_whitespace()) {
            self.append_text(&tok.text);
            return;
        }
        if tok.kind == TokKind::Doctype {
            self.dom.n_errors += 1;
            return;
        }
        if tok.kind == TokKind::Start {
            match tok.tag {
                TAG_HTML => {
                    self.step_in_body(tok);
                    return;
                }
                TAG_HEAD | TAG_NOSCRIPT => {
                    self.dom.n_errors += 1;
                    return;
                }
                TAG_BASEFONT | TAG_BGSOUND | TAG_LINK | TAG_META | TAG_NOFRAMES
                | TAG_SCRIPT | TAG_STYLE => {
                    self.step_in_head(tok);
                    if self.mode == Mode::InHead {
                        self.mode = Mode::InHeadNoscript;
                    }
                    return;
                }
                _ => {}
            }
        } else if tok.kind == TokKind::End {
            if tok.tag == TAG_NOSCRIPT {
                if self.dom.node(self.top()).tag == TAG_NOSCRIPT {
                    self.pop();
                }
                self.mode = Mode::InHead;
                return;
            }
            if tok.tag != TAG_BR {
                self.dom.n_errors += 1;
                return;
            }
        }
        self.dom.n_errors += 1;
        if self.dom.node(self.top()).tag == TAG_NOSCRIPT {
            self.pop();
        }
        self.mode = Mode::InHead;
        self.step(tok);
    }

    fn step_after_head(&mut self, tok: Tok) {
        if tok.kind == TokKind::Comment {
            let c = self.make_comment(&tok);
            let parent = self.top();
            self.dom.append_child(parent, c);
            return;
        }
        if tok.kind == TokKind::Text {
            let ws_len = Self::peel_leading_ws(&tok.text);
            if ws_len > 0 {
                self.append_text(&tok.text[..ws_len]);
            }
            if ws_len == tok.text.len() {
                return;
            }
            self.ensure_body();
            self.frameset_ok = true;
            self.mode = Mode::InBody;
            let mut tok2 = tok;
            tok2.text = tok2.text[ws_len..].to_vec();
            self.step(tok2);
            return;
        }
        if tok.kind == TokKind::Start && tok.tag == TAG_BODY {
            self.ensure_html();
            while !self.stack.is_empty() && self.dom.node(self.top()).tag == TAG_HEAD {
                self.pop();
            }
            self.frameset_ok = false;
            if self.body.is_some() {
                self.dom.n_errors += 1;
                self.merge_attrs(self.body.unwrap(), &tok);
                self.mode = Mode::InBody;
                return;
            }
            let b = self.make_element(&tok);
            let html = self.html.unwrap();
            self.dom.append_child(html, b);
            self.body = Some(b);
            self.push(b);
            self.mode = Mode::InBody;
            return;
        }
        if tok.kind == TokKind::Start && tok.tag == TAG_HEAD {
            self.dom.n_errors += 1;
            return;
        }
        if tok.kind == TokKind::Start && tok.tag == TAG_FRAMESET {
            self.ensure_html();
            while !self.stack.is_empty() && self.dom.node(self.top()).tag == TAG_HEAD {
                self.pop();
            }
            self.insert_element(&tok, true);
            self.mode = Mode::InFrameset;
            return;
        }
        if tok.kind == TokKind::Start
            && matches!(
                tok.tag,
                TAG_BASE
                    | TAG_BASEFONT
                    | TAG_BGSOUND
                    | TAG_LINK
                    | TAG_META
                    | TAG_NOFRAMES
                    | TAG_SCRIPT
                    | TAG_STYLE
                    | TAG_TEMPLATE
                    | TAG_TITLE
            )
        {
            self.dom.n_errors += 1;
            if let Some(head) = self.head {
                self.push(head);
                self.step_in_head(tok);
                self.remove_from_stack(head);
            } else {
                self.step_in_head(tok);
            }
            return;
        }
        if tok.kind == TokKind::End && tok.tag == TAG_TEMPLATE {
            if let Some(head) = self.head {
                self.push(head);
                self.step_in_head(tok);
                self.remove_from_stack(head);
            } else {
                self.step_in_head(tok);
            }
            return;
        }
        if tok.kind == TokKind::End && tok.tag == TAG_HTML {
            self.ensure_body();
            self.mode = Mode::InBody;
            self.step(tok);
            return;
        }
        if tok.kind == TokKind::End && tok.tag != TAG_BODY && tok.tag != TAG_BR {
            self.dom.n_errors += 1;
            return;
        }
        self.ensure_body();
        self.mode = Mode::InBody;
        self.step(tok);
    }

    fn merge_attrs(&mut self, dst: NodeId, tok: &Tok) {
        let existing: Vec<Vec<u8>> = self.dom.node(dst).attrs.iter().map(|a| a.name.clone()).collect();
        for a in &tok.attrs {
            if existing.iter().any(|n| str_eq_ci(n, &a.name)) {
                continue;
            }
            self.dom.node_mut(dst).attrs.push(a.clone());
        }
    }

    fn step_in_body(&mut self, tok: Tok) {
        match tok.kind {
            TokKind::Text => {
                if self.in_foreign_text() {
                    self.append_text(&tok.text);
                    if tok.text_had_real {
                        self.frameset_ok = false;
                    }
                    return;
                }
                self.afe_reconstruct();
                self.append_text(&tok.text);
                if tok.text_had_real {
                    self.frameset_ok = false;
                }
                return;
            }
            TokKind::Comment => {
                let c = self.make_comment(&tok);
                let (parent, before) = self.place();
                match before {
                    Some(b) => self.dom.insert_child_before(parent, c, b),
                    None => self.dom.append_child(parent, c),
                }
                return;
            }
            TokKind::Doctype => {
                self.dom.n_errors += 1;
                return;
            }
            TokKind::Eof => {
                if self.has_open(TAG_TEMPLATE) {
                    self.dom.n_errors += 1;
                    self.pop_until(TAG_TEMPLATE);
                    self.tpl_modes.pop();
                    self.reset_mode();
                    self.step(tok);
                    return;
                }
                self.stopped = true;
                return;
            }
            TokKind::End | TokKind::Start => {}
        }

        if tok.kind == TokKind::Start {
            let mut t = tok.tag;
            let mut tok = tok;
            if t == TAG_IMAGE {
                self.dom.n_errors += 1;
                tok.tag = TAG_IMG;
                t = TAG_IMG;
            }
            if self.frag
                && self.stack.len() <= 1
                && t == TAG_INPUT
                && self.frag_ctx.as_ref().is_some_and(|fc| fc.ns == Ns::Html && fc.tag == TAG_SELECT)
            {
                self.dom.n_errors += 1;
                return;
            }
            if self.in_foreign(&tok) && !self.no_foreign {
                self.foreign_step(&tok);
                return;
            }
            if t == TAG_SVG || t == TAG_MATH {
                let n = self.make_element(&tok);
                self.dom.node_mut(n).ns = if t == TAG_SVG { Ns::Svg } else { Ns::Mathml };
                self.foreign_adjust(n);
                self.append_placed(n);
                if !tok.self_closing {
                    self.push(n);
                }
                return;
            }
            if t == TAG_HTML {
                self.dom.n_errors += 1;
                if self.html.is_some() && !self.has_open(TAG_TEMPLATE) {
                    let html = self.html.unwrap();
                    self.merge_attrs(html, &tok);
                }
                return;
            }
            if t == TAG_BODY || t == TAG_HEAD {
                self.dom.n_errors += 1;
                if t == TAG_BODY
                    && self.stack.len() >= 2
                    && self.dom.node(self.stack[1]).tag == TAG_BODY
                    && !self.has_open(TAG_TEMPLATE)
                {
                    self.frameset_ok = false;
                    let body = self.stack[1];
                    self.merge_attrs(body, &tok);
                }
                return;
            }
            if t == TAG_FRAMESET {
                self.dom.n_errors += 1;
                if self.stack.len() < 2 || self.dom.node(self.stack[1]).tag != TAG_BODY {
                    return;
                }
                if !self.frameset_ok {
                    return;
                }
                let body = self.stack[1];
                self.dom.detach(body);
                self.body = None;
                while self.stack.len() > 1 {
                    self.pop();
                }
                self.insert_element(&tok, true);
                self.mode = Mode::InFrameset;
                return;
            }
            if t == TAG_TABLE {
                if !self.dom.quirks {
                    self.close_p_if_open();
                }
                self.insert_element(&tok, true);
                self.pend_reset();
                self.mode = Mode::InTable;
                self.frameset_ok = false;
                return;
            }
            if t == TAG_FORM {
                if self.form.is_some() {
                    self.dom.n_errors += 1;
                    return;
                }
                self.close_p_if_open();
                self.insert_element(&tok, true);
                self.form = Some(self.top());
                return;
            }
            if t == TAG_TEMPLATE {
                self.tpl_start(&tok);
                return;
            }
            if t == TAG_SELECT {
                if self.has_in_default_scope_tag(TAG_SELECT) {
                    self.dom.n_errors += 1;
                    self.pop_until(TAG_SELECT);
                    return;
                }
                self.insert_element(&tok, true);
                self.frameset_ok = false;
                return;
            }
            if t == TAG_OPTION {
                if self.has_in_default_scope_tag(TAG_SELECT) {
                    self.gen_implied(TAG_OPTGROUP);
                    if self.has_in_default_scope_tag(TAG_OPTION) {
                        self.dom.n_errors += 1;
                    }
                } else if self.dom.node(self.top()).tag == TAG_OPTION {
                    self.pop();
                }
                self.afe_reconstruct();
                self.insert_element(&tok, true);
                return;
            }
            if t == TAG_OPTGROUP {
                if self.has_in_default_scope_tag(TAG_SELECT) {
                    self.gen_implied(tags::TAG_UNKNOWN);
                    if self.has_in_default_scope_tag(TAG_OPTION)
                        || self.has_in_default_scope_tag(TAG_OPTGROUP)
                    {
                        self.dom.n_errors += 1;
                    }
                } else if self.dom.node(self.top()).tag == TAG_OPTION {
                    self.pop();
                }
                self.afe_reconstruct();
                self.insert_element(&tok, true);
                return;
            }
            if Self::is_formatting(t) {
                if t == TAG_A {
                    if let Some(fi) = self.afe_find_tag(TAG_A) {
                        let old = self.afe[fi].node;
                        self.dom.n_errors += 1;
                        self.adoption(TAG_A);
                        if let Some(i) = self.afe_find_node(old) {
                            self.afe_remove_at(i);
                        }
                        if let Some(i) = self.stack_find_node(old) {
                            self.stack_remove_at(i);
                        }
                    }
                }
                self.afe_reconstruct();
                if t == TAG_NOBR && self.has_in_default_scope_tag(TAG_NOBR) {
                    self.dom.n_errors += 1;
                    self.adoption(TAG_NOBR);
                    self.afe_reconstruct();
                }
                self.insert_element(&tok, true);
                let top = self.top();
                self.afe_push(top);
                return;
            }
            if matches!(t, TAG_BASE | TAG_BASEFONT | TAG_BGSOUND | TAG_LINK | TAG_META) {
                self.insert_element(&tok, false);
                return;
            }
            if t == TAG_BUTTON {
                if self.has_in_scope2(TAG_BUTTON, ScopeKind::Default) {
                    self.dom.n_errors += 1;
                    self.gen_implied(tags::TAG_UNKNOWN);
                    self.pop_until(TAG_BUTTON);
                }
                self.afe_reconstruct();
                self.insert_element(&tok, true);
                self.frameset_ok = false;
                return;
            }
            if t == TAG_PLAINTEXT {
                self.close_p_if_open();
                self.insert_element(&tok, true);
                self.tok.set_plaintext();
                return;
            }
            if !self.has_open(TAG_TEMPLATE) {
                match t {
                    TAG_CAPTION | TAG_COL | TAG_COLGROUP | TAG_FRAME | TAG_TBODY
                    | TAG_TD | TAG_TFOOT | TAG_TH | TAG_THEAD | TAG_TR => {
                        self.dom.n_errors += 1;
                        return;
                    }
                    _ => {}
                }
            }
            if Self::closes_p(t) {
                self.close_p_if_open();
            }
            if (t == TAG_OBJECT || t == TAG_APPLET || t == TAG_MARQUEE) && !tok.self_closing {
                self.afe_reconstruct();
                self.insert_element(&tok, true);
                self.afe_insert_marker();
                self.frameset_ok = false;
                return;
            }
            if t == TAG_LI {
                self.frameset_ok = false;
                self.implied_close(TAG_LI, tags::TAG_UNKNOWN, TAG_UL, TAG_OL);
                self.close_p_if_open();
            }
            if t == TAG_DT || t == TAG_DD {
                self.frameset_ok = false;
                self.implied_close(TAG_DT, TAG_DD, TAG_DL, tags::TAG_UNKNOWN);
                self.close_p_if_open();
            }
            if matches!(t, TAG_RB | TAG_RP | TAG_RT | TAG_RTC) {
                if self.has_in_default_scope_tag(TAG_RUBY) {
                    let keep_rtc = t == TAG_RP || t == TAG_RT;
                    while !self.stack.is_empty() {
                        let ct = self.dom.node(self.top()).tag;
                        if keep_rtc && ct == TAG_RTC {
                            break;
                        }
                        match ct {
                            TAG_DD | TAG_DT | TAG_LI | TAG_OPTGROUP | TAG_OPTION | TAG_P
                            | TAG_RP | TAG_RT | TAG_RB | TAG_RTC => {
                                self.pop();
                            }
                            _ => break,
                        }
                    }
                }
                self.insert_element(&tok, !tok.self_closing);
                return;
            }
            if matches!(t, TAG_PRE | TAG_LISTING | TAG_XMP | TAG_TEXTAREA) {
                self.skip_lf = true;
            }
            if matches!(
                t,
                TAG_PRE | TAG_LISTING | TAG_XMP | TAG_TEXTAREA | TAG_IFRAME
            ) {
                self.frameset_ok = false;
            }
            if t == TAG_XMP {
                self.afe_reconstruct();
            }
            if (TAG_H1..=TAG_H6).contains(&t) {
                self.close_heading_if_open();
            }
            match t {
                TAG_P
                | TAG_DIV
                | TAG_UL
                | TAG_OL
                | TAG_DL
                | TAG_PRE
                | TAG_LISTING
                | TAG_BLOCKQUOTE
                | TAG_ADDRESS
                | TAG_ARTICLE
                | TAG_ASIDE
                | TAG_FOOTER
                | TAG_HEADER
                | TAG_MAIN
                | TAG_NAV
                | TAG_SECTION
                | TAG_FIELDSET
                | TAG_FIGURE
                | TAG_FIGCAPTION
                | TAG_CENTER
                | TAG_DETAILS
                | TAG_DIALOG
                | TAG_DIR
                | TAG_MENU
                | TAG_HGROUP
                | TAG_SEARCH
                | TAG_SUMMARY
                | TAG_LI
                | TAG_DD
                | TAG_DT
                | TAG_H1
                | TAG_H2
                | TAG_H3
                | TAG_H4
                | TAG_H5
                | TAG_H6
                | TAG_RB
                | TAG_RP
                | TAG_RT
                | TAG_RTC => {
                    self.insert_element(&tok, true);
                    return;
                }
                _ => {}
            }
            if tags::is_void(t) {
                if t == TAG_INPUT {
                    if self.has_in_default_scope_tag(TAG_SELECT) {
                        self.dom.n_errors += 1;
                        self.pop_until(TAG_SELECT);
                    }
                    self.afe_reconstruct();
                    let ty = tok
                        .attrs
                        .iter()
                        .find(|a| str_eq_ci(&a.name, b"type"))
                        .map(|a| a.value.as_slice());
                    if !str_eq_ci(ty.unwrap_or(b""), b"hidden") {
                        self.frameset_ok = false;
                    }
                } else {
                    if matches!(t, TAG_AREA | TAG_BR | TAG_EMBED | TAG_IMG | TAG_KEYGEN | TAG_WBR)
                    {
                        self.frameset_ok = false;
                        self.afe_reconstruct();
                    }
                    if t == TAG_HR {
                        if self.has_in_default_scope_tag(TAG_SELECT) {
                            self.gen_implied(tags::TAG_UNKNOWN);
                            if self.has_in_default_scope_tag(TAG_OPTION)
                                || self.has_in_default_scope_tag(TAG_OPTGROUP)
                            {
                                self.dom.n_errors += 1;
                            }
                        }
                        self.frameset_ok = false;
                    }
                }
                self.insert_element(&tok, false);
                return;
            }
            if tags::is_rawtext(t) || tags::is_rcdata(t) {
                self.insert_element(&tok, true);
                self.tok.set_raw(t);
                return;
            }
            self.afe_reconstruct();
            self.insert_element(&tok, true);
            return;
        }

        // TOK_END
        if self.in_foreign(&tok) && !self.no_foreign {
            self.foreign_step(&tok);
            return;
        }
        let t = tok.tag;
        if t == TAG_BODY {
            self.mode = Mode::AfterBody;
            return;
        }
        if t == TAG_HTML {
            if !self.has_in_scope2(TAG_BODY, ScopeKind::Default) {
                self.dom.n_errors += 1;
                return;
            }
            self.mode = Mode::AfterBody;
            self.step(tok);
            return;
        }
        if t == TAG_BR {
            let br = Self::synth_start(TAG_BR);
            self.step_in_body(br);
            return;
        }
        if t == TAG_FORM && self.form.is_some() {
            let node = self.form.unwrap();
            self.form = None;
            let mut in_scope = false;
            for i in (0..self.stack.len()).rev() {
                let s = self.stack[i];
                if s == node {
                    in_scope = true;
                    break;
                }
                if self.scope_barrier(s, ScopeKind::Default) {
                    break;
                }
            }
            if !in_scope {
                self.dom.n_errors += 1;
                return;
            }
            self.gen_implied(tags::TAG_UNKNOWN);
            if self.top() != node {
                self.dom.n_errors += 1;
            }
            if let Some(i) = self.stack_find_node(node) {
                self.stack_remove_at(i);
            }
            return;
        }
        if t == TAG_TEMPLATE {
            self.tpl_end();
            return;
        }
        if Self::is_formatting(t) {
            self.adoption(t);
            return;
        }
        if t == TAG_P {
            if !self.has_in_scope2(TAG_P, ScopeKind::Button) {
                self.dom.n_errors += 1;
                let p = Self::synth_start(TAG_P);
                self.insert_element(&p, true);
            }
            self.close_p_if_open();
            return;
        }
        if t == TAG_LI {
            self.end_in_scope(t, ScopeKind::ListItem, true);
            return;
        }
        if t == TAG_DD || t == TAG_DT {
            self.end_in_scope(t, ScopeKind::Default, true);
            return;
        }
        if (TAG_H1..=TAG_H6).contains(&t) {
            self.end_hgroup();
            return;
        }
        if t == TAG_APPLET || t == TAG_MARQUEE || t == TAG_OBJECT {
            if !self.has_open(t) {
                self.dom.n_errors += 1;
                return;
            }
            self.gen_implied(tags::TAG_UNKNOWN);
            if self.dom.node(self.top()).tag != t {
                self.dom.n_errors += 1;
            }
            self.pop_until(t);
            self.afe_clear_to_marker();
            return;
        }
        match t {
            TAG_ADDRESS
            | TAG_ARTICLE
            | TAG_ASIDE
            | TAG_BLOCKQUOTE
            | TAG_BUTTON
            | TAG_CENTER
            | TAG_DIR
            | TAG_DIV
            | TAG_DL
            | TAG_FIELDSET
            | TAG_FIGCAPTION
            | TAG_FIGURE
            | TAG_FOOTER
            | TAG_HEADER
            | TAG_LISTING
            | TAG_MAIN
            | TAG_MENU
            | TAG_NAV
            | TAG_OL
            | TAG_PRE
            | TAG_SECTION
            | TAG_UL
            | TAG_DETAILS
            | TAG_DIALOG
            | TAG_HGROUP
            | TAG_SEARCH
            | TAG_SUMMARY
            | TAG_SELECT => {
                self.end_in_scope(t, ScopeKind::Default, false);
                return;
            }
            _ => {}
        }
        self.any_other_end_tag(t, &tok.tag_raw);
    }

    // ---- table 系モード ----

    fn step_in_table(&mut self, tok: Tok) {
        self.pend_flush();
        match tok.kind {
            TokKind::Text => {
                self.pend_add(&tok.text);
                return;
            }
            TokKind::Comment => {
                let c = self.make_comment(&tok);
                let (parent, before) = self.place();
                match before {
                    Some(b) => self.dom.insert_child_before(parent, c, b),
                    None => self.dom.append_child(parent, c),
                }
                return;
            }
            TokKind::Doctype => {
                self.dom.n_errors += 1;
                return;
            }
            TokKind::Eof => {
                self.step_in_body(tok);
                return;
            }
            _ => {}
        }
        if tok.kind == TokKind::Start {
            match tok.tag {
                TAG_CAPTION => {
                    self.clear_back(&[TAG_TABLE, TAG_TEMPLATE, TAG_HTML]);
                    self.afe_insert_marker();
                    self.insert_element(&tok, true);
                    self.mode = Mode::InCaption;
                }
                TAG_COLGROUP => {
                    self.clear_back(&[TAG_TABLE, TAG_TEMPLATE, TAG_HTML]);
                    self.insert_element(&tok, true);
                    self.mode = Mode::InColumnGroup;
                }
                TAG_COL => {
                    self.clear_back(&[TAG_TABLE, TAG_TEMPLATE, TAG_HTML]);
                    let cg = Self::synth_start(TAG_COLGROUP);
                    self.insert_element(&cg, true);
                    self.mode = Mode::InColumnGroup;
                    self.step_in_colgroup(tok);
                }
                TAG_TBODY | TAG_THEAD | TAG_TFOOT => {
                    self.clear_back(&[TAG_TABLE, TAG_TEMPLATE, TAG_HTML]);
                    self.insert_element(&tok, true);
                    self.mode = Mode::InTableBody;
                }
                TAG_TR => {
                    self.clear_back(&[TAG_TABLE, TAG_TEMPLATE, TAG_HTML]);
                    let tb = Self::synth_start(TAG_TBODY);
                    self.insert_element(&tb, true);
                    self.mode = Mode::InTableBody;
                    self.step_in_table_body(tok);
                }
                TAG_TD | TAG_TH => {
                    self.dom.n_errors += 1;
                    self.clear_back(&[TAG_TABLE, TAG_TEMPLATE, TAG_HTML]);
                    let tb = Self::synth_start(TAG_TBODY);
                    self.insert_element(&tb, true);
                    self.mode = Mode::InTableBody;
                    let tr = Self::synth_start(TAG_TR);
                    self.step_in_table_body(tr);
                    self.step_in_row(tok);
                }
                TAG_TABLE => {
                    self.dom.n_errors += 1;
                    if !self.has_in_table_scope(TAG_TABLE) {
                        return;
                    }
                    self.pop_until(TAG_TABLE);
                    self.reset_mode();
                    self.step(tok);
                }
                TAG_STYLE | TAG_SCRIPT | TAG_TEMPLATE => self.step_in_head(tok),
                TAG_INPUT => {
                    if Self::attr_is_ci(&tok, b"type", b"hidden") {
                        self.insert_element(&tok, false);
                        return;
                    }
                    self.foster_body(tok);
                }
                TAG_FORM => {
                    if self.form.is_some() {
                        return;
                    }
                    self.insert_element(&tok, true);
                    self.form = Some(self.top());
                    self.pop();
                }
                _ => self.foster_body(tok),
            }
            return;
        }
        // TOK_END
        if tok.tag == TAG_TABLE {
            if !self.has_in_table_scope(TAG_TABLE) {
                self.dom.n_errors += 1;
                return;
            }
            self.pop_until(TAG_TABLE);
            self.reset_mode();
            return;
        }
        if tok.tag == TAG_TEMPLATE {
            self.step_in_head(tok);
            return;
        }
        match tok.tag {
            TAG_BODY | TAG_CAPTION | TAG_COL | TAG_COLGROUP | TAG_HTML | TAG_TBODY
            | TAG_TD | TAG_TFOOT | TAG_TH | TAG_THEAD | TAG_TR => {
                self.dom.n_errors += 1;
            }
            _ => self.foster_body(tok),
        }
    }

    fn foster_body(&mut self, tok: Tok) {
        self.dom.n_errors += 1;
        self.foster = true;
        self.step_in_body(tok);
        self.foster = false;
    }

    fn step_in_caption(&mut self, tok: Tok) {
        if tok.kind == TokKind::End && tok.tag == TAG_CAPTION {
            if !self.has_in_table_scope(TAG_CAPTION) {
                self.dom.n_errors += 1;
                return;
            }
            self.pop_until(TAG_CAPTION);
            self.mode = Mode::InTable;
            return;
        }
        if tok.kind == TokKind::Start {
            match tok.tag {
                TAG_CAPTION | TAG_COL | TAG_COLGROUP | TAG_TBODY | TAG_TD | TAG_TFOOT
                | TAG_TH | TAG_THEAD | TAG_TR => {
                    if !self.has_in_table_scope(TAG_CAPTION) {
                        self.dom.n_errors += 1;
                        return;
                    }
                    self.pop_until(TAG_CAPTION);
                    self.mode = Mode::InTable;
                    self.step(tok);
                    return;
                }
                _ => {}
            }
        }
        if tok.kind == TokKind::End && tok.tag == TAG_TABLE {
            if !self.has_in_table_scope(TAG_CAPTION) {
                self.dom.n_errors += 1;
                return;
            }
            self.pop_until(TAG_CAPTION);
            self.mode = Mode::InTable;
            self.step(tok);
            return;
        }
        if tok.kind == TokKind::End {
            match tok.tag {
                TAG_BODY | TAG_COL | TAG_COLGROUP | TAG_HTML | TAG_TBODY | TAG_TD
                | TAG_TFOOT | TAG_TH | TAG_THEAD | TAG_TR => {
                    self.dom.n_errors += 1;
                    return;
                }
                _ => {}
            }
        }
        self.step_in_body(tok);
    }

    fn step_in_colgroup(&mut self, tok: Tok) {
        if tok.kind == TokKind::Eof {
            if self.has_open(TAG_TEMPLATE) {
                self.dom.n_errors += 1;
                self.pop_until(TAG_TEMPLATE);
                self.tpl_modes.pop();
                self.reset_mode();
                self.step(tok);
                return;
            }
            if self.dom.node(self.top()).tag == TAG_COLGROUP {
                self.pop();
                self.mode = Mode::InTable;
                self.step(tok);
                return;
            }
            self.stopped = true;
            return;
        }
        if tok.kind == TokKind::Text {
            let mut i = 0;
            while i < tok.text.len() && tok.text[i].is_ascii_whitespace() {
                i += 1;
            }
            if i > 0 {
                self.append_text(&tok.text[..i]);
            }
            if i < tok.text.len() {
                let mut tok2 = tok;
                tok2.text = tok2.text[i..].to_vec();
                self.colgroup_anything(tok2);
            }
            return;
        }
        if tok.kind == TokKind::Comment {
            let c = self.make_comment(&tok);
            let (parent, before) = self.place();
            match before {
                Some(b) => self.dom.insert_child_before(parent, c, b),
                None => self.dom.append_child(parent, c),
            }
            return;
        }
        if tok.kind == TokKind::Doctype {
            self.dom.n_errors += 1;
            return;
        }
        if tok.kind == TokKind::Start {
            if tok.tag == TAG_HTML {
                self.step_in_body(tok);
                return;
            }
            if tok.tag == TAG_COL {
                self.insert_element(&tok, false);
                return;
            }
            if tok.tag == TAG_TEMPLATE {
                self.step_in_head(tok);
                return;
            }
            self.colgroup_anything(tok);
            return;
        }
        if tok.kind == TokKind::End {
            if tok.tag == TAG_COLGROUP {
                if self.dom.node(self.top()).tag == TAG_COLGROUP {
                    self.pop();
                    self.mode = Mode::InTable;
                } else {
                    self.dom.n_errors += 1;
                }
                return;
            }
            if tok.tag == TAG_COL {
                self.dom.n_errors += 1;
                return;
            }
            if tok.tag == TAG_TEMPLATE {
                self.step_in_head(tok);
                return;
            }
            self.colgroup_anything(tok);
        }
    }

    fn colgroup_anything(&mut self, tok: Tok) {
        if self.dom.node(self.top()).tag == TAG_COLGROUP {
            self.pop();
            self.mode = Mode::InTable;
            self.step(tok);
        } else {
            self.dom.n_errors += 1;
        }
    }

    fn any_tbf_in_scope(&self) -> bool {
        self.has_in_table_scope(TAG_TBODY)
            || self.has_in_table_scope(TAG_THEAD)
            || self.has_in_table_scope(TAG_TFOOT)
    }

    fn leave_tbody_reprocess(&mut self, tok: Tok) {
        self.clear_back(&[TAG_TBODY, TAG_TFOOT, TAG_THEAD, TAG_TEMPLATE, TAG_HTML]);
        self.pop();
        self.mode = Mode::InTable;
        self.step(tok);
    }

    fn step_in_table_body(&mut self, tok: Tok) {
        if tok.kind == TokKind::Start {
            match tok.tag {
                TAG_TR => {
                    self.clear_back(&[TAG_TBODY, TAG_TFOOT, TAG_THEAD, TAG_TEMPLATE, TAG_HTML]);
                    self.insert_element(&tok, true);
                    self.mode = Mode::InRow;
                    return;
                }
                TAG_TD | TAG_TH => {
                    self.dom.n_errors += 1;
                    let tr = Self::synth_start(TAG_TR);
                    self.step_in_table_body(tr);
                    self.step_in_row(tok);
                    return;
                }
                TAG_CAPTION | TAG_COL | TAG_COLGROUP | TAG_TBODY | TAG_THEAD | TAG_TFOOT => {
                    if !self.any_tbf_in_scope() {
                        self.dom.n_errors += 1;
                        return;
                    }
                    self.leave_tbody_reprocess(tok);
                    return;
                }
                _ => {}
            }
        } else if tok.kind == TokKind::End {
            match tok.tag {
                TAG_TBODY | TAG_THEAD | TAG_TFOOT => {
                    if !self.has_in_table_scope(tok.tag) {
                        self.dom.n_errors += 1;
                        return;
                    }
                    self.clear_back(&[TAG_TBODY, TAG_TFOOT, TAG_THEAD, TAG_TEMPLATE, TAG_HTML]);
                    self.pop();
                    self.mode = Mode::InTable;
                    return;
                }
                TAG_TABLE => {
                    if !self.any_tbf_in_scope() {
                        self.dom.n_errors += 1;
                        return;
                    }
                    self.clear_back(&[TAG_TBODY, TAG_TFOOT, TAG_THEAD, TAG_TEMPLATE, TAG_HTML]);
                    self.pop();
                    self.mode = Mode::InTable;
                    self.step(tok);
                    return;
                }
                TAG_BODY | TAG_CAPTION | TAG_COL | TAG_COLGROUP | TAG_HTML | TAG_TR
                | TAG_TD | TAG_TH => {
                    self.dom.n_errors += 1;
                    return;
                }
                _ => {}
            }
        }
        self.step_in_table(tok);
    }

    fn end_tr_reprocess(&mut self, tok: Tok) {
        self.clear_back(&[TAG_TR, TAG_TEMPLATE, TAG_HTML]);
        self.pop();
        self.mode = Mode::InTableBody;
        self.step(tok);
    }

    fn step_in_row(&mut self, tok: Tok) {
        if tok.kind == TokKind::Start {
            match tok.tag {
                TAG_TD | TAG_TH => {
                    self.clear_back(&[TAG_TR, TAG_TEMPLATE, TAG_HTML]);
                    self.afe_insert_marker();
                    self.insert_element(&tok, true);
                    self.mode = Mode::InCell;
                    return;
                }
                TAG_TR => {
                    self.dom.n_errors += 1;
                    if !self.has_in_table_scope(TAG_TR) {
                        return;
                    }
                    self.end_tr_reprocess(tok);
                    return;
                }
                TAG_CAPTION | TAG_COL | TAG_COLGROUP | TAG_TBODY | TAG_THEAD | TAG_TFOOT => {
                    if !self.has_in_table_scope(TAG_TR) {
                        self.dom.n_errors += 1;
                        return;
                    }
                    self.end_tr_reprocess(tok);
                    return;
                }
                TAG_TABLE => {
                    self.dom.n_errors += 1;
                    if !self.has_in_table_scope(TAG_TR) {
                        return;
                    }
                    self.end_tr_reprocess(tok);
                    return;
                }
                _ => {}
            }
        } else if tok.kind == TokKind::End {
            match tok.tag {
                TAG_TR => {
                    if !self.has_in_table_scope(TAG_TR) {
                        self.dom.n_errors += 1;
                        return;
                    }
                    self.clear_back(&[TAG_TR, TAG_TEMPLATE, TAG_HTML]);
                    self.pop();
                    self.mode = Mode::InTableBody;
                    return;
                }
                TAG_TABLE => {
                    if !self.has_in_table_scope(TAG_TR) {
                        self.dom.n_errors += 1;
                        return;
                    }
                    self.end_tr_reprocess(tok);
                    return;
                }
                TAG_TBODY | TAG_THEAD | TAG_TFOOT => {
                    if !self.has_in_table_scope(tok.tag) {
                        self.dom.n_errors += 1;
                        return;
                    }
                    self.end_tr_reprocess(tok);
                    return;
                }
                TAG_BODY | TAG_CAPTION | TAG_COL | TAG_COLGROUP | TAG_HTML | TAG_TD
                | TAG_TH => {
                    self.dom.n_errors += 1;
                    return;
                }
                _ => {}
            }
        }
        self.step_in_table(tok);
    }

    fn step_in_cell(&mut self, tok: Tok) {
        if tok.kind == TokKind::End && (tok.tag == TAG_TD || tok.tag == TAG_TH) {
            if !self.has_in_table_scope(tok.tag) {
                self.dom.n_errors += 1;
                return;
            }
            self.gen_implied(tok.tag);
            if self.dom.node(self.top()).tag != tok.tag {
                self.dom.n_errors += 1;
            }
            self.pop_until(tok.tag);
            self.afe_clear_to_marker();
            self.mode = Mode::InRow;
            return;
        }
        if tok.kind == TokKind::Start {
            match tok.tag {
                TAG_CAPTION | TAG_COL | TAG_COLGROUP | TAG_TBODY | TAG_TD | TAG_TH
                | TAG_THEAD | TAG_TFOOT | TAG_TR => {
                    self.dom.n_errors += 1;
                    if !self.has_in_table_scope(TAG_TD) && !self.has_in_table_scope(TAG_TH) {
                        return;
                    }
                    self.close_cell();
                    self.step(tok);
                    return;
                }
                _ => {}
            }
        }
        if tok.kind == TokKind::End {
            match tok.tag {
                TAG_TABLE | TAG_TBODY | TAG_TFOOT | TAG_THEAD | TAG_TR => {
                    if !self.has_in_table_scope(tok.tag) {
                        self.dom.n_errors += 1;
                        return;
                    }
                    self.close_cell();
                    self.step(tok);
                    return;
                }
                TAG_BODY | TAG_CAPTION | TAG_COL | TAG_COLGROUP | TAG_HTML => {
                    self.dom.n_errors += 1;
                    return;
                }
                _ => {}
            }
        }
        self.step_in_body(tok);
    }

    // ---- frameset モード ----

    fn frameset_text(&mut self, s: &[u8]) {
        let nws = s.iter().filter(|&&c| c.is_ascii_whitespace()).count();
        if nws == s.len() {
            self.append_text(s);
            return;
        }
        let buf: Vec<u8> = s.iter().copied().filter(|&c| c.is_ascii_whitespace()).collect();
        self.append_text(&buf);
        self.dom.n_errors += 1;
    }

    fn step_in_frameset(&mut self, tok: Tok) {
        if tok.kind == TokKind::Text {
            self.frameset_text(&tok.text);
            return;
        }
        if tok.kind == TokKind::Comment {
            let c = self.make_comment(&tok);
            let (parent, before) = self.place();
            match before {
                Some(b) => self.dom.insert_child_before(parent, c, b),
                None => self.dom.append_child(parent, c),
            }
            return;
        }
        if tok.kind == TokKind::Doctype {
            self.dom.n_errors += 1;
            return;
        }
        if tok.kind == TokKind::Start && tok.tag == TAG_HTML {
            self.step_in_body(tok);
            return;
        }
        if tok.kind == TokKind::Start && tok.tag == TAG_FRAMESET {
            self.insert_element(&tok, true);
            return;
        }
        if tok.kind == TokKind::Start && tok.tag == TAG_FRAME {
            self.insert_element(&tok, false);
            return;
        }
        if tok.kind == TokKind::Start && tok.tag == TAG_NOFRAMES {
            self.step_in_head(tok);
            return;
        }
        if tok.kind == TokKind::End && tok.tag == TAG_FRAMESET {
            if !self.stack.is_empty() && self.dom.node(self.top()).tag == TAG_FRAMESET {
                self.pop();
                if !self.frag {
                    self.mode = Mode::AfterFrameset;
                }
                return;
            }
            if self.frag {
                self.dom.n_errors += 1;
                return;
            }
            if self.stack.len() <= 1 {
                self.stopped = true;
                return;
            }
            self.dom.n_errors += 1;
            return;
        }
        self.dom.n_errors += 1;
    }

    fn step_after_frameset(&mut self, tok: Tok) {
        if tok.kind == TokKind::Text {
            self.frameset_text(&tok.text);
            return;
        }
        if tok.kind == TokKind::Comment {
            let c = self.make_comment(&tok);
            let (parent, before) = self.place();
            match before {
                Some(b) => self.dom.insert_child_before(parent, c, b),
                None => self.dom.append_child(parent, c),
            }
            return;
        }
        if tok.kind == TokKind::Doctype {
            self.dom.n_errors += 1;
            return;
        }
        if tok.kind == TokKind::Start && tok.tag == TAG_HTML {
            self.step_in_body(tok);
            return;
        }
        if tok.kind == TokKind::End && tok.tag == TAG_HTML {
            self.mode = Mode::AfterAfterFrameset;
            return;
        }
        if tok.kind == TokKind::Start && tok.tag == TAG_NOFRAMES {
            self.step_in_head(tok);
            return;
        }
        if tok.kind == TokKind::Eof {
            self.stopped = true;
            return;
        }
        self.dom.n_errors += 1;
    }

    fn step_after_after_frameset(&mut self, tok: Tok) {
        if tok.kind == TokKind::Text {
            self.frameset_text(&tok.text);
            return;
        }
        if tok.kind == TokKind::Comment {
            let c = self.make_comment(&tok);
            self.dom.append_child(self.dom.root, c);
            return;
        }
        if tok.kind == TokKind::Doctype {
            self.dom.n_errors += 1;
            return;
        }
        if tok.kind == TokKind::Start && tok.tag == TAG_HTML {
            self.step_in_body(tok);
            return;
        }
        if tok.kind == TokKind::Start && tok.tag == TAG_NOFRAMES {
            self.step_in_head(tok);
            return;
        }
        if tok.kind == TokKind::Eof {
            self.stopped = true;
            return;
        }
        self.dom.n_errors += 1;
    }

    // ---- foreign content ----

    fn in_foreign(&self, tok: &Tok) -> bool {
        if self.stack.is_empty() {
            return false;
        }
        let node = self.acn();
        let (ns, tag) = self.acn_ns_tag();
        if ns == Ns::Html {
            return false;
        }
        if tok.kind == TokKind::Start {
            let is_math_ip = ns == Ns::Mathml
                && matches!(tag, TAG_MI | TAG_MO | TAG_MN | TAG_MS | TAG_MTEXT);
            if is_math_ip && tok.tag != TAG_MGLYPH && tok.tag != TAG_MALIGNMARK {
                return false;
            }
            if ns == Ns::Mathml && tag == TAG_ANNOTATION_XML {
                if tok.tag == TAG_SVG {
                    return false;
                }
                if let Some(nid) = node {
                    let enc = self.dom.attr(nid, b"encoding");
                    if enc.is_some_and(|e| {
                        str_eq_ci(e, b"text/html") || str_eq_ci(e, b"application/xhtml+xml")
                    }) {
                        return false;
                    }
                }
            }
            let is_html_ip = ns == Ns::Svg
                && (tag == TAG_FOREIGNOBJECT || tag == TAG_DESC || tag == TAG_TITLE);
            if is_html_ip {
                return false;
            }
            return true;
        }
        if tok.kind == TokKind::End {
            return true;
        }
        false
    }

    fn in_foreign_text(&self) -> bool {
        if self.stack.is_empty() {
            return false;
        }
        let (ns, tag) = self.acn_ns_tag();
        if ns == Ns::Html {
            return false;
        }
        let is_math_ip = ns == Ns::Mathml
            && matches!(tag, TAG_MI | TAG_MO | TAG_MN | TAG_MS | TAG_MTEXT);
        let is_html_ip = ns == Ns::Svg
            && (tag == TAG_FOREIGNOBJECT || tag == TAG_DESC || tag == TAG_TITLE);
        if is_math_ip || is_html_ip {
            return false;
        }
        if ns == Ns::Mathml && tag == TAG_ANNOTATION_XML {
            if let Some(nid) = self.acn() {
                let enc = self.dom.attr(nid, b"encoding");
                if enc.is_some_and(|e| {
                    str_eq_ci(e, b"text/html") || str_eq_ci(e, b"application/xhtml+xml")
                }) {
                    return false;
                }
            }
        }
        true
    }

    fn is_breakout_start(tok: &Tok) -> bool {
        match tok.tag {
            TAG_B
            | TAG_BIG
            | TAG_BLOCKQUOTE
            | TAG_BODY
            | TAG_BR
            | TAG_CENTER
            | TAG_CODE
            | TAG_DD
            | TAG_DIV
            | TAG_DL
            | TAG_DT
            | TAG_EM
            | TAG_H1
            | TAG_H2
            | TAG_H3
            | TAG_H4
            | TAG_H5
            | TAG_H6
            | TAG_HEAD
            | TAG_HR
            | TAG_I
            | TAG_IMG
            | TAG_LI
            | TAG_LISTING
            | TAG_MENU
            | TAG_META
            | TAG_NOBR
            | TAG_OL
            | TAG_P
            | TAG_PRE
            | TAG_RUBY
            | TAG_S
            | TAG_SMALL
            | TAG_SPAN
            | TAG_STRONG
            | TAG_STRIKE
            | TAG_SUB
            | TAG_SUP
            | TAG_TABLE
            | TAG_TT
            | TAG_U
            | TAG_UL
            | TAG_VAR => true,
            TAG_FONT => tok
                .attrs
                .iter()
                .any(|a| str_eq_ci(&a.name, b"color") || str_eq_ci(&a.name, b"face") || str_eq_ci(&a.name, b"size")),
            _ => false,
        }
    }

    fn foreign_adjust(&mut self, n: NodeId) {
        if self.dom.node(n).ns == Ns::Svg {
            const SVG: &[(&str, &str)] = &[
                ("altglyph", "altGlyph"),
                ("altglyphdef", "altGlyphDef"),
                ("altglyphitem", "altGlyphItem"),
                ("animatecolor", "animateColor"),
                ("animatemotion", "animateMotion"),
                ("animatetransform", "animateTransform"),
                ("clippath", "clipPath"),
                ("feblend", "feBlend"),
                ("fecolormatrix", "feColorMatrix"),
                ("fecomponenttransfer", "feComponentTransfer"),
                ("fecomposite", "feComposite"),
                ("feconvolvematrix", "feConvolveMatrix"),
                ("fediffuselighting", "feDiffuseLighting"),
                ("fedisplacementmap", "feDisplacementMap"),
                ("fedistantlight", "feDistantLight"),
                ("fedropshadow", "feDropShadow"),
                ("feflood", "feFlood"),
                ("fefunca", "feFuncA"),
                ("fefuncb", "feFuncB"),
                ("fefuncg", "feFuncG"),
                ("fefuncr", "feFuncR"),
                ("fegaussianblur", "feGaussianBlur"),
                ("feimage", "feImage"),
                ("femerge", "feMerge"),
                ("femergenode", "feMergeNode"),
                ("femorphology", "feMorphology"),
                ("feoffset", "feOffset"),
                ("fepointlight", "fePointLight"),
                ("fespecularlighting", "feSpecularLighting"),
                ("fespotlight", "feSpotLight"),
                ("fetile", "feTile"),
                ("feturbulence", "feTurbulence"),
                ("foreignobject", "foreignObject"),
                ("glyphref", "glyphRef"),
                ("lineargradient", "linearGradient"),
                ("radialgradient", "radialGradient"),
                ("textpath", "textPath"),
            ];
            let name = self.dom.node(n).name.clone();
            for &(lc, canon) in SVG {
                if str_eq_ci(&name, lc.as_bytes()) {
                    self.dom.node_mut(n).name = canon.as_bytes().to_vec();
                    break;
                }
            }
        }
        // attr 名の調整
        const SVG_ATTR_LC: &[&str] = &[
            "attributename",
            "attributetype",
            "basefrequency",
            "baseprofile",
            "calcmode",
            "clippathunits",
            "diffuseconstant",
            "edgemode",
            "filterunits",
            "glyphref",
            "gradienttransform",
            "gradientunits",
            "kernelmatrix",
            "kernelunitlength",
            "keypoints",
            "keysplines",
            "keytimes",
            "lengthadjust",
            "limitingconeangle",
            "markerheight",
            "markerunits",
            "markerwidth",
            "maskcontentunits",
            "maskunits",
            "numoctaves",
            "pathlength",
            "patterncontentunits",
            "patterntransform",
            "patternunits",
            "pointsatx",
            "pointsaty",
            "pointsatz",
            "preservealpha",
            "preserveaspectratio",
            "primitiveunits",
            "refx",
            "refy",
            "repeatcount",
            "repeatdur",
            "requiredextensions",
            "requiredfeatures",
            "specularconstant",
            "specularexponent",
            "spreadmethod",
            "startoffset",
            "stddeviation",
            "stitchtiles",
            "surfacescale",
            "systemlanguage",
            "tablevalues",
            "targetx",
            "targety",
            "textlength",
            "viewbox",
            "viewtarget",
            "xchannelselector",
            "ychannelselector",
            "zoomandpan",
        ];
        const SVG_ATTR_CANON: &[&str] = &[
            "attributeName",
            "attributeType",
            "baseFrequency",
            "baseProfile",
            "calcMode",
            "clipPathUnits",
            "diffuseConstant",
            "edgeMode",
            "filterUnits",
            "glyphRef",
            "gradientTransform",
            "gradientUnits",
            "kernelMatrix",
            "kernelUnitLength",
            "keyPoints",
            "keySplines",
            "keyTimes",
            "lengthAdjust",
            "limitingConeAngle",
            "markerHeight",
            "markerUnits",
            "markerWidth",
            "maskContentUnits",
            "maskUnits",
            "numOctaves",
            "pathLength",
            "patternContentUnits",
            "patternTransform",
            "patternUnits",
            "pointsAtX",
            "pointsAtY",
            "pointsAtZ",
            "preserveAlpha",
            "preserveAspectRatio",
            "primitiveUnits",
            "refX",
            "refY",
            "repeatCount",
            "repeatDur",
            "requiredExtensions",
            "requiredFeatures",
            "specularConstant",
            "specularExponent",
            "spreadMethod",
            "startOffset",
            "stdDeviation",
            "stitchTiles",
            "surfaceScale",
            "systemLanguage",
            "tableValues",
            "targetX",
            "targetY",
            "textLength",
            "viewBox",
            "viewTarget",
            "xChannelSelector",
            "yChannelSelector",
            "zoomAndPan",
        ];
        const FOREIGN_ATTR: &[(&str, &str)] = &[
            ("xlink:actuate", "xlink actuate"),
            ("xlink:arcrole", "xlink arcrole"),
            ("xlink:href", "xlink href"),
            ("xlink:role", "xlink role"),
            ("xlink:show", "xlink show"),
            ("xlink:title", "xlink title"),
            ("xlink:type", "xlink type"),
            ("xml:lang", "xml lang"),
            ("xml:space", "xml space"),
            ("xmlns:xlink", "xmlns xlink"),
        ];
        let ns = self.dom.node(n).ns;
        let attrs = self.dom.node(n).attrs.clone();
        let adjusted: Vec<Attr> = attrs
            .into_iter()
            .map(|mut a| {
                if ns == Ns::Mathml && str_eq_ci(&a.name, b"definitionurl") {
                    a.name = b"definitionURL".to_vec();
                } else if ns == Ns::Svg {
                    if let Some(pos) = SVG_ATTR_LC
                        .iter()
                        .position(|&lc| str_eq_ci(&a.name, lc.as_bytes()))
                    {
                        a.name = SVG_ATTR_CANON[pos].as_bytes().to_vec();
                    }
                }
                // foreign attr（接頭辞系は case-sensitive）
                for &(from, to) in FOREIGN_ATTR {
                    if a.name == from.as_bytes() {
                        a.name = to.as_bytes().to_vec();
                        break;
                    }
                }
                a
            })
            .collect();
        self.dom.node_mut(n).attrs = adjusted;
    }

    fn foreign_insert(&mut self, tok: &Tok) {
        let mut ft = tok.clone();
        if ft.tag == TAG_TEMPLATE {
            ft.tag = tags::TAG_UNKNOWN;
        }
        let n = self.make_element(&ft);
        let ns = self.acn_ns_tag().0;
        self.dom.node_mut(n).ns = ns;
        self.foreign_adjust(n);
        self.append_placed(n);
        if !tok.self_closing {
            self.push(n);
        }
    }

    fn tok_end_name(tok: &Tok) -> Vec<u8> {
        if tok.tag != tags::TAG_UNKNOWN {
            if let Some(s) = tags::tag_name(tok.tag) {
                return s.as_bytes().to_vec();
            }
        }
        tok.tag_raw.clone()
    }

    fn foreign_step(&mut self, tok: &Tok) {
        if tok.kind == TokKind::Start {
            if Self::is_breakout_start(tok) {
                self.dom.n_errors += 1;
                self.no_foreign = true;
                while !self.stack.is_empty() {
                    let t2 = self.top();
                    let (ns, tag) = {
                        let n = self.dom.node(t2);
                        (n.ns, n.tag)
                    };
                    let is_html_ip = ns == Ns::Svg
                        && (tag == TAG_FOREIGNOBJECT || tag == TAG_DESC || tag == TAG_TITLE);
                    let is_math_ip = ns == Ns::Mathml
                        && matches!(tag, TAG_MI | TAG_MO | TAG_MN | TAG_MS | TAG_MTEXT);
                    if ns == Ns::Html || is_html_ip || is_math_ip {
                        break;
                    }
                    self.pop();
                }
                let tok2 = tok.clone();
                self.step(tok2);
                self.no_foreign = false;
                return;
            }
            self.foreign_insert(tok);
            return;
        }
        if tok.tag == TAG_BR || tok.tag == TAG_P {
            self.dom.n_errors += 1;
            self.no_foreign = true;
            while !self.stack.is_empty() && self.dom.node(self.top()).ns != Ns::Html {
                self.pop();
            }
            let tok2 = tok.clone();
            self.step(tok2);
            self.no_foreign = false;
            return;
        }
        let name = Self::tok_end_name(tok);
        if !self.stack.is_empty() && str_eq_ci(&self.dom.node(self.top()).name, &name) {
            self.pop();
            return;
        }
        self.dom.n_errors += 1;
        for i in (0..self.stack.len()).rev() {
            let e = self.stack[i];
            if self.dom.node(e).ns == Ns::Html {
                self.no_foreign = true;
                let tok2 = tok.clone();
                self.step(tok2);
                self.no_foreign = false;
                return;
            }
            if str_eq_ci(&self.dom.node(e).name, &name) {
                while self.stack.len() > i {
                    self.pop();
                }
                return;
            }
        }
    }

    // ---- メインディスパッチャ ----

    fn step(&mut self, tok: Tok) {
        let mut tok = tok;
        if self.skip_lf {
            self.skip_lf = false;
            if tok.kind == TokKind::Text && tok.text.first() == Some(&b'\n') {
                tok.text = tok.text[1..].to_vec();
                if tok.text.is_empty() {
                    return;
                }
            }
        }
        if tok.kind == TokKind::Text
            && !self.stack.is_empty()
            && (tags::is_rawtext(self.dom.node(self.top()).tag)
                || tags::is_rcdata(self.dom.node(self.top()).tag))
        {
            self.append_text(&tok.text);
            return;
        }
        if tok.kind == TokKind::End
            && !self.stack.is_empty()
            && (tags::is_rawtext(self.dom.node(self.top()).tag)
                || tags::is_rcdata(self.dom.node(self.top()).tag))
            && self.dom.node(self.top()).tag == tok.tag
            && self.dom.node(self.top()).ns == Ns::Html
        {
            self.pop();
            return;
        }
        if matches!(
            self.mode,
            Mode::InTable
                | Mode::InCaption
                | Mode::InColumnGroup
                | Mode::InTableBody
                | Mode::InRow
                | Mode::InCell
        ) && tok.kind != TokKind::Text
        {
            self.pend_flush();
        }
        if (tok.kind == TokKind::Start || tok.kind == TokKind::End)
            && self.in_foreign(&tok)
            && !self.no_foreign
        {
            self.foreign_step(&tok);
            return;
        }
        match self.mode {
            Mode::Initial => self.step_initial(tok),
            Mode::BeforeHtml => self.step_before_html(tok),
            Mode::BeforeHead => self.step_before_head(tok),
            Mode::InHead => self.step_in_head(tok),
            Mode::InHeadNoscript => self.step_in_head_noscript(tok),
            Mode::AfterHead => self.step_after_head(tok),
            Mode::InBody => self.step_in_body(tok),
            Mode::InTable => self.step_in_table(tok),
            Mode::InCaption => self.step_in_caption(tok),
            Mode::InColumnGroup => self.step_in_colgroup(tok),
            Mode::InTableBody => self.step_in_table_body(tok),
            Mode::InRow => self.step_in_row(tok),
            Mode::InCell => self.step_in_cell(tok),
            Mode::InFrameset => self.step_in_frameset(tok),
            Mode::AfterFrameset => self.step_after_frameset(tok),
            Mode::AfterAfterFrameset => self.step_after_after_frameset(tok),
            Mode::InTemplate => self.step_in_template(tok),
            Mode::AfterBody => {
                if tok.kind == TokKind::Comment {
                    let c = self.make_comment(&tok);
                    let parent = self.html.unwrap_or(self.dom.root);
                    self.dom.append_child(parent, c);
                    return;
                }
                if tok.kind == TokKind::Text && tok.text.iter().all(|&c| c.is_ascii_whitespace()) {
                    self.step_in_body(tok);
                    return;
                }
                if tok.kind == TokKind::Eof {
                    self.stopped = true;
                    return;
                }
                if tok.kind == TokKind::End && tok.tag == TAG_HTML {
                    if self.frag {
                        self.dom.n_errors += 1;
                        return;
                    }
                    self.mode = Mode::AfterAfterBody;
                    return;
                }
                self.dom.n_errors += 1;
                self.mode = Mode::InBody;
                self.step(tok);
            }
            Mode::AfterAfterBody => {
                if tok.kind == TokKind::Comment {
                    let c = self.make_comment(&tok);
                    self.dom.append_child(self.dom.root, c);
                    return;
                }
                if tok.kind == TokKind::Text && tok.text.iter().all(|&c| c.is_ascii_whitespace()) {
                    self.step_in_body(tok);
                    return;
                }
                if tok.kind == TokKind::Eof {
                    self.stopped = true;
                    return;
                }
                self.dom.n_errors += 1;
                self.mode = Mode::InBody;
                self.step(tok);
            }
        }
    }

    fn peel_leading_ws(s: &[u8]) -> usize {
        let mut n = 0;
        while n < s.len() && (s[n] == b' ' || s[n] == b'\t' || s[n] == b'\n' || s[n] == b'\x0c' || s[n] == b'\r') {
            n += 1;
        }
        n
    }

    fn frag_init(&mut self, ctx: &str) {
        let (ns, name) = if let Some(rest) = ctx.strip_prefix("svg ") {
            (Ns::Svg, rest)
        } else if let Some(rest) = ctx.strip_prefix("math ") {
            (Ns::Mathml, rest)
        } else {
            (Ns::Html, ctx)
        };
        self.frag = true;
        let tag = tags::tag_id(name.as_bytes());
        self.frag_ctx = Some(FragCtx { ns, tag });

        let h = self.make_element(&Self::synth_start(TAG_HTML));
        self.dom.append_child(self.dom.root, h);
        self.html = Some(h);
        self.push(h);

        if ns == Ns::Html && tag == TAG_TEMPLATE {
            self.tpl_modes.push(Mode::InTemplate);
        }
        self.mode = self.frag_ctx_mode();
    }

    /// 文書を構築し `Dom` を返す（`ctx` は fragment 解析時の context、通常は `None`）。
    fn parse(mut self, ctx: Option<&str>) -> Dom {
        if let Some(ctx) = ctx {
            self.frag_init(ctx);
            let fc = self.frag_ctx.as_ref().unwrap();
            if fc.ns == Ns::Html {
                let ct = fc.tag;
                if matches!(
                    ct,
                    TAG_TITLE
                        | TAG_TEXTAREA
                        | TAG_STYLE
                        | TAG_XMP
                        | TAG_IFRAME
                        | TAG_NOEMBED
                        | TAG_NOFRAMES
                        | TAG_SCRIPT
                ) {
                    self.tok.set_raw(ct);
                    self.tok.set_raw_frag();
                } else if ct == TAG_PLAINTEXT {
                    self.tok.set_plaintext();
                }
            }
        }

        while !self.stopped {
            self.tok.set_cdata_foreign(self.in_foreign_text());
            let adcn = match self.acn() {
                Some(n) => {
                    let node = self.dom.node(n);
                    node.kind == NodeKind::Element && node.ns != Ns::Html
                }
                None => self.frag_ctx.as_ref().is_some_and(|fc| fc.ns != Ns::Html),
            };
            self.tok.set_adcn_foreign(adcn);
            let tok = self.tok.next();
            let eof = tok.kind == TokKind::Eof;
            self.step(tok);
            if eof {
                break;
            }
            if self.dom.nodes.len() as u32 > MAX_DOM_NODES {
                self.dom.n_errors += 1;
                break;
            }
        }
        self.dom.n_errors += self.tok.errors;

        // title 回収
        if let Some(head) = self.head {
            let mut c = self.dom.node(head).first_child;
            while let Some(cid) = c {
                let node = self.dom.node(cid);
                if node.kind == NodeKind::Element && node.tag == TAG_TITLE {
                    let tc = self.dom.text_content(cid);
                    self.dom.title = Self::trim_bytes(&tc).to_vec();
                    break;
                }
                c = node.next_sibling;
            }
        }
        // customizable select: <selectedcontent> への選択中 option の clone
        // （C の sc_select_walk。観測時のみ走査）
        if self.dom.has_selectedcontent {
            let root = self.dom.root;
            sc_select_walk(&mut self.dom, root);
        }
        self.dom
    }

    fn trim_bytes(s: &[u8]) -> &[u8] {
        let mut a = 0;
        let mut b = s.len();
        while a < b && s[a].is_ascii_whitespace() {
            a += 1;
        }
        while b > a && s[b - 1].is_ascii_whitespace() {
            b -= 1;
        }
        &s[a..b]
    }
}

/// HTML 文書をパースして `Dom` を構築（C の `if_parse_html` 相当）。
pub fn parse_html(input: &[u8]) -> Dom {
    let dom = Dom::new();
    let tok = Tokenizer::new(input);
    let tb = TreeBuilder::new(dom, tok);
    tb.parse(None)
}

/// fragment 解析（WHATWG 13.4。`ctx` は `"body"` / `"svg path"` 等）。
pub fn parse_html_fragment(input: &[u8], ctx: &str) -> Dom {
    let dom = Dom::new();
    let tok = Tokenizer::new(input);
    let tb = TreeBuilder::new(dom, tok);
    tb.parse(Some(ctx))
}

// ---- customizable select: <selectedcontent> への選択中 option の clone ----
// C の sc_clone / sc_selected_option / sc_fill / sc_select_walk 相当。
// <select> 内の各 <selectedcontent> に、選択中 option（selected 属性を持つ最後の
// option、無ければ先頭 option）の子孫全体の複写を挿入する（webkit02#44-#47）。

/// ノードを子孫ごと複写する（C の `sc_clone` 相当。`tpl_content` は複写しない）。
fn sc_clone(dom: &mut Dom, s: NodeId) -> NodeId {
    let n = dom.alloc_node(dom.node(s).kind);
    let (tag, ns, flags, name, attrs, doctype, pi_target) = {
        let sn = dom.node(s);
        (
            sn.tag,
            sn.ns,
            sn.flags,
            sn.name.clone(),
            sn.attrs.clone(),
            sn.doctype.clone(),
            sn.pi_target.clone(),
        )
    };
    {
        let node = dom.node_mut(n);
        node.tag = tag;
        node.ns = ns;
        node.flags = flags;
        node.name = name;
        node.attrs = attrs;
        node.doctype = doctype;
        node.pi_target = pi_target;
    }
    let children: Vec<NodeId> = {
        let mut v = Vec::new();
        let mut c = dom.node(s).first_child;
        while let Some(cid) = c {
            v.push(cid);
            c = dom.node(cid).next_sibling;
        }
        v
    };
    let mut tail: Option<NodeId> = None;
    for cid in children {
        let cc = sc_clone(dom, cid);
        dom.node_mut(cc).parent = Some(n);
        match tail {
            Some(t) => {
                dom.node_mut(t).next_sibling = Some(cc);
                tail = Some(cc);
            }
            None => {
                dom.node_mut(n).first_child = Some(cc);
                tail = Some(cc);
            }
        }
    }
    dom.node_mut(n).last_child = tail;
    n
}

/// 選択中 option（最後の selected 属性持ち、無ければ先頭 option。C の `sc_selected_option`）。
fn sc_selected_option(dom: &Dom, sel: NodeId) -> Option<NodeId> {
    let mut first = None;
    let mut chosen = None;
    let mut c = dom.node(sel).first_child;
    while let Some(cid) = c {
        let node = dom.node(cid);
        if node.kind == NodeKind::Element && node.tag == TAG_OPTION && node.ns == Ns::Html {
            if first.is_none() {
                first = Some(cid);
            }
            if node.attrs.iter().any(|a| str_eq_ci(&a.name, b"selected")) {
                chosen = Some(cid);
            }
        }
        c = node.next_sibling;
    }
    chosen.or(first)
}

/// `<selectedcontent>` を option の子孫 clone で満たす（C の `sc_fill` 相当）。
fn sc_fill(dom: &mut Dom, n: NodeId, opt: NodeId) {
    let is_sc = {
        let node = dom.node(n);
        node.kind == NodeKind::Element
            && node.ns == Ns::Html
            && node.tag == tags::TAG_UNKNOWN
            && str_eq_ci(&node.name, b"selectedcontent")
    };
    if is_sc {
        dom.node_mut(n).first_child = None;
        dom.node_mut(n).last_child = None;
        let children: Vec<NodeId> = {
            let mut v = Vec::new();
            let mut c = dom.node(opt).first_child;
            while let Some(cid) = c {
                v.push(cid);
                c = dom.node(cid).next_sibling;
            }
            v
        };
        let mut tail: Option<NodeId> = None;
        for cid in children {
            let cc = sc_clone(dom, cid);
            dom.node_mut(cc).parent = Some(n);
            match tail {
                Some(t) => {
                    dom.node_mut(t).next_sibling = Some(cc);
                    tail = Some(cc);
                }
                None => {
                    dom.node_mut(n).first_child = Some(cc);
                    tail = Some(cc);
                }
            }
        }
        dom.node_mut(n).last_child = tail;
        return;
    }
    let children: Vec<NodeId> = {
        let mut v = Vec::new();
        let mut c = dom.node(n).first_child;
        while let Some(cid) = c {
            v.push(cid);
            c = dom.node(cid).next_sibling;
        }
        v
    };
    for cid in children {
        sc_fill(dom, cid, opt);
    }
}

/// 文書内の `<select>` を走査して selectedcontent を満たす（C の `sc_select_walk`）。
fn sc_select_walk(dom: &mut Dom, n: NodeId) {
    {
        let node = dom.node(n);
        if node.kind == NodeKind::Element && node.tag == TAG_SELECT && node.ns == Ns::Html {
            if let Some(opt) = sc_selected_option(dom, n) {
                sc_fill(dom, n, opt);
            }
        }
    }
    let children: Vec<NodeId> = {
        let mut v = Vec::new();
        let mut c = dom.node(n).first_child;
        while let Some(cid) = c {
            v.push(cid);
            c = dom.node(cid).next_sibling;
        }
        v
    };
    for cid in children {
        sc_select_walk(dom, cid);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::dom::{Dom, NodeId, NodeKind};

    /// 文書順でテキストを収集（text_content と同じ）。
    fn text(dom: &Dom, n: NodeId) -> String {
        String::from_utf8_lossy(&dom.text_content(n)).into_owned()
    }

    #[test]
    fn basic_structure() {
        let dom = parse_html(b"<html><head><title>My Title</title></head><body><p>Hello</p></body></html>");
        assert_eq!(dom.title, b"My Title");
        // body のテキスト
        let body = dom.find_tag_dfs(tags::tag_id(b"body")).unwrap();
        assert_eq!(text(&dom, body), "Hello");
    }

    #[test]
    fn implied_html_head_body() {
        let dom = parse_html(b"<p>hi</p>");
        // html/head/body が暗黙生成される
        let html = dom.find_tag_dfs(tags::tag_id(b"html")).unwrap();
        assert_eq!(dom.node(html).kind, NodeKind::Element);
        assert!(dom.find_tag_dfs(tags::tag_id(b"body")).is_some());
        let body = dom.find_tag_dfs(tags::tag_id(b"body")).unwrap();
        assert_eq!(text(&dom, body), "hi");
    }

    #[test]
    fn p_closes_p() {
        let dom = parse_html(b"<p>a<p>b");
        // 2 個の p が兄弟になる
        let body = dom.find_tag_dfs(tags::tag_id(b"body")).unwrap();
        let mut p_count = 0;
        let mut c = dom.node(body).first_child;
        while let Some(cid) = c {
            if dom.node(cid).tag == tags::tag_id(b"p") {
                p_count += 1;
            }
            c = dom.node(cid).next_sibling;
        }
        assert_eq!(p_count, 2);
    }

    #[test]
    fn table_structure() {
        let dom = parse_html(b"<table><tr><td>1<td>2</table>");
        // tbody が暗黙生成され、td は 2 個
        let table = dom.find_tag_dfs(tags::tag_id(b"table")).unwrap();
        assert_eq!(text(&dom, table), "12");
    }

    #[test]
    fn adoption_agency() {
        let dom = parse_html(b"<b>1<i>2</b>3</i>");
        // 整形要素の誤ネストが adoption agency で修復される
        let body = dom.find_tag_dfs(tags::tag_id(b"body")).unwrap();
        assert_eq!(text(&dom, body), "123");
    }

    #[test]
    fn title_trimmed() {
        let dom = parse_html(b"<title>  spaced  </title>");
        assert_eq!(dom.title, b"spaced");
    }

    #[test]
    fn svg_foreign() {
        let dom = parse_html(b"<svg><circle/></svg>");
        let svg = dom.find_tag_dfs(tags::tag_id(b"svg")).unwrap();
        assert_eq!(dom.node(svg).ns, Ns::Svg);
        // circle は svg の子
        assert!(dom.node(svg).first_child.is_some());
    }

    #[test]
    fn has_script_observed() {
        let dom = parse_html(b"<script>var x = 1;</script>");
        assert!(dom.has_script);
    }

    /// wpt 形式シリアライズの出力を文字列として得る補助。
    fn wpt(input: &[u8]) -> String {
        String::from_utf8(parse_html(input).serialize_wpt()).unwrap()
    }

    #[test]
    fn wpt_basic() {
        let s = wpt(b"<p>hello</p>");
        assert_eq!(
            s,
            "| <html>\n|   <head>\n|   <body>\n|     <p>\n|       \"hello\"\n"
        );
    }

    #[test]
    fn wpt_doctype_public_system() {
        let s = wpt(b"<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0//EN\" \"http://x\">");
        assert!(s.starts_with(
            "| <!DOCTYPE html \"-//W3C//DTD XHTML 1.0//EN\" \"http://x\">\n"
        ));
    }

    #[test]
    fn wpt_doctype_bare() {
        let s = wpt(b"<!DOCTYPE>");
        assert!(s.starts_with("| <!DOCTYPE >\n"));
    }

    #[test]
    fn wpt_pi() {
        // <?xml...?> は XML 宣言様式のため bogus comment 扱い（HTML 仕様）。
        // PI として扱われるのは非 xml ターゲットのみ。
        let s = wpt(b"<?target data?>");
        assert!(s.contains("| <?target data?>\n"));
    }

    #[test]
    fn wpt_template_content() {
        let s = wpt(b"<template><div>x</div></template>");
        assert!(s.contains("|     <template>\n|       content\n|         <div>\n"));
    }

    #[test]
    fn wpt_selectedcontent_clone() {
        let s = wpt(b"<select><button><selectedcontent></button><option>X<option selected>Y");
        // 選択中 option（selected 持ちの最後）の子 "Y" が selectedcontent へ clone される
        assert!(s.contains("|         <selectedcontent>\n|           \"Y\"\n"));
    }

    #[test]
    fn wpt_annotation_xml_encoding_ci() {
        // encoding は case-insensitive 一致（tests20 #55/#57）
        let s = wpt(b"<math><annotation-xml encoding=\"Text/htmL\"><div>");
        assert!(s.contains("|         <div>\n"));
    }

    #[test]
    fn wpt_adoption_counter_clamp() {
        // AAA の inner>3 打ち切り（adoption01 #14 / webkit02 #14 系の余分な clone 防止）
        let s = wpt(b"<b><em><foo><foo><foo><aside></b>");
        assert!(!s.contains("|     <em>\n|       <aside>"));
        assert!(s.contains("|     <aside>\n|       <b>\n"));
    }
}
