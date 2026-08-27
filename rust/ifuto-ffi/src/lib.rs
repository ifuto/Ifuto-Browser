//! `<script>` 実行配線（C の `src/script.c` の `if_script_run` + AklHandleVTab 相当）。
//!
//! Aklus JS エンジン（`akl-core`、`#![forbid(unsafe_code)]`）と DOM（`ifuto-core`）を
//! クロスクレート配線する最終統合層のうち、**script 実行**を担う。C の `script.c` が
//! `AklHandleVTab`（`doc_vt` / `elem_vt` / `style_vt`）の C コールバック + モジュール
//! 静的グローバル（`g_arena` / `g_dom` / `g_log`）で DOM へ触れていた部分を、safe Rust
//! の `thread_local!` コンテキスト（[`Ctx`]）+ `HandleVTab`（`ptr = NodeId`）で置き換える。
//!
//! # C との対応
//!
//! | C (script.c) | Rust |
//! |---|---|
//! | `script_console_log`（`[script:console]` native） | [`script_console_log`] |
//! | `doc_vt`（HTMLDocument） | [`DOC_VT`] |
//! | `elem_vt`（HTMLElement） | [`ELEM_VT`] |
//! | `style_vt`（CSSStyleDeclaration） | [`STYLE_VT`] |
//! | `collect_scripts_rec` / `if_script_run` | [`script_run`] |
//!
//! # 所有権モデル
//!
//! C は DOM ノードを raw ポインタ（`IfNode*`）で JS 側へ渡す（`HANDLE` の `ptr`）が、
//! Rust では `ptr` に `NodeId`（`Vec<Node>` への index）を詰める。`get`/`set`/`call`
//! コールバックは `thread_local!` の [`Ctx`]（eval 期間中のみ `Dom` を所有）を借用して
//! ノードを参照・変異する。`script_run` が開始時に `Dom` を `Ctx` へ移し、終了時に
//! 取り戻す（`Dom: Default` なので `std::mem::take` で空文書と交換）。
//!
//! C の「akl eval は本プロセスで同時 1 実行のみ」という前提を `thread_local!` が
//! そのまま維持する（`RefCell` で借用を検査）。

#![deny(unsafe_op_in_unsafe_fn)]
// unsafe は [`bearssl`]（BearSSL の C FFI 境界）にのみ存在し、`// SAFETY:` コメント
// 付きで集約する（akl-ffi と同じ「境界を 1 ファイルに集約」方針）。それ以外のモジュール
// （script 実行配線・net ソケット）は safe Rust のみ。

use akl_core::builtins::install_builtins;
use akl_core::bytecode::{HandleVTab, Runtime, VmError};
use akl_core::codegen::compile;
use akl_core::obj::Obj;
use akl_core::parser::Parser;
use akl_core::AklVal;
use ifuto_core::dom::{Dom, NodeId, NodeKind, Ns};
use ifuto_core::script::{style_get_prop, style_set_prop};
use ifuto_core::strutil::str_eq_ci;
use ifuto_core::tags_tables::{TAG_BODY, TAG_HTML, TAG_SCRIPT};
use std::cell::RefCell;

/// BearSSL（TLS 1.2 クライアント）の unsafe FFI 境界。C の `src/tls.c` のソケット側を
/// `std::net::TcpStream` 駆動で再実装する。
pub mod bearssl;

/// net.c のソケット層（http / https の取得）。`std::net` + [`bearssl`]。
pub mod net_sock;

/// 1 文書で実行する `<script>` の上限（C の `IF_SCRIPT_MAX_RUN`）。
const MAX_RUN: usize = 128;
/// 1 script のソース上限（C の `IF_SCRIPT_MAX_SRC` = 4MB）。
const MAX_SRC: usize = 4 * 1024 * 1024;
/// `console.log` の連結上限（C の `IF_SCRIPT_LOG_CAP`）。
const LOG_CAP: usize = 960;

/// `<script>` 実行の集計（C の `IfScriptReport` 相当）。
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct ScriptReport {
    /// 実行した script 数（空でない inline script。size/NUL 検査で失敗した分も含む）。
    pub n_run: u32,
    /// エラー数（bootstrap 失敗 + eval 失敗）。
    pub n_errors: u32,
    /// スキップ数（外部 src / 未知 type / 空 script）。
    pub n_skipped: u32,
}

/// eval 期間中のコンテキスト（C の `g_arena` / `g_dom` / `g_log` 相当）。
struct Ctx {
    dom: Dom,
    log: Vec<u8>,
}

thread_local! {
    static CTX: RefCell<Option<Ctx>> = const { RefCell::new(None) };
}

fn with_dom<T>(f: impl FnOnce(&Dom) -> T) -> T {
    CTX.with(|c| f(&c.borrow().as_ref().unwrap().dom))
}

fn with_dom_mut<T>(f: impl FnOnce(&mut Dom) -> T) -> T {
    CTX.with(|c| f(&mut c.borrow_mut().as_mut().unwrap().dom))
}

fn log_bytes(bytes: &[u8]) {
    CTX.with(|c| {
        c.borrow_mut()
            .as_mut()
            .unwrap()
            .log
            .extend_from_slice(bytes)
    });
}

/// バイト列から JS 文字列値を作る（非 UTF-8 は C の `akl_mkstring` と同じく空文字列）。
fn mk_string(rt: &mut Runtime, bytes: &[u8]) -> AklVal {
    let s = std::str::from_utf8(bytes).unwrap_or("");
    match rt.intern(s) {
        Some(id) => AklVal::mk_obj(id),
        None => AklVal::UNDEF,
    }
}

/// ハンドル値（`ptr = NodeId`）を作る。
fn mk_handle(rt: &mut Runtime, vtab: &'static HandleVTab, ptr: u64) -> AklVal {
    match rt.heap.alloc(Obj::Handle { vtab, data: 0, ptr }) {
        Ok(id) => AklVal::mk_obj(id),
        Err(_) => AklVal::UNDEF,
    }
}

/// `akl_as_str`（文字列なら内容、非文字列なら NULL）相当。
fn as_str(rt: &Runtime, v: AklVal) -> Option<String> {
    if !v.is_obj() {
        return None;
    }
    let id = v.get_obj();
    match rt.heap.get(id) {
        Some(Obj::Str(_)) | Some(Obj::Rope { .. }) => Some(rt.flatten_str(v)),
        _ => None,
    }
}

/// `akl_tostring` + `akl_as_str`（JS ToString 強制）相当。失敗で `None`。
fn to_string(rt: &mut Runtime, v: AklVal) -> Option<String> {
    let id = rt.stringify(v).ok()?;
    Some(rt.flatten_str(AklVal::mk_obj(id)))
}

/// C の `call` コールバックが `akl_native_throw` 後に `*out` を書かず `true` を返す
/// 挙動（`handle_call_adapter` が `out=0` を返す = double 0.0）を再現する。
fn c_throw() -> AklVal {
    AklVal::from_bits(0)
}

/// `[script:console]` を出力する `console.log` native（C の `script_console_log`）。
fn script_console_log(rt: &mut Runtime, _this: AklVal, args: &[AklVal]) -> Result<AklVal, VmError> {
    let mut buf: Vec<u8> = Vec::new();
    for (i, arg) in args.iter().enumerate() {
        let s = match to_string(rt, *arg) {
            Some(s) => s,
            None => return Ok(AklVal::UNDEF), // budget 枯渇: 何も出さない（C と同一）
        };
        let b = s.as_bytes();
        if i > 0 && buf.len() < LOG_CAP {
            buf.push(b' ');
        }
        let room = LOG_CAP - buf.len();
        let cn = b.len().min(room);
        buf.extend_from_slice(&b[..cn]);
        if buf.len() >= LOG_CAP {
            break;
        }
    }
    for c in buf.iter_mut() {
        if *c == b'\n' || *c == b'\r' {
            *c = b' ';
        }
    }
    log_bytes(b"[script:console] ");
    log_bytes(&buf);
    log_bytes(b"\n");
    Ok(AklVal::UNDEF)
}

// ---- document / element HANDLE vtab ----

static DOC_VT: HandleVTab = HandleVTab {
    tag: "HTMLDocument",
    get: doc_get,
    set: doc_set,
    call: doc_call,
};
static ELEM_VT: HandleVTab = HandleVTab {
    tag: "HTMLElement",
    get: elem_get,
    set: elem_set,
    call: elem_call,
};
static STYLE_VT: HandleVTab = HandleVTab {
    tag: "CSSStyleDeclaration",
    get: style_get,
    set: style_set,
    call: no_call,
};

fn no_call(
    _rt: &mut Runtime,
    _data: u64,
    _ptr: u64,
    _name: &str,
    _args: &[AklVal],
) -> Option<AklVal> {
    None
}

fn doc_get(rt: &mut Runtime, _data: u64, _ptr: u64, name: &str) -> Option<AklVal> {
    match name {
        "title" => {
            let t = with_dom(|d| d.title.clone());
            Some(mk_string(rt, &t))
        }
        "body" => {
            let b = with_dom(|d| d.find_tag_dfs(TAG_BODY));
            Some(match b {
                Some(n) => mk_handle(rt, &ELEM_VT, n as u64),
                None => AklVal::NULL,
            })
        }
        "documentElement" => {
            let h = with_dom(|d| d.find_tag_dfs(TAG_HTML));
            Some(match h {
                Some(n) => mk_handle(rt, &ELEM_VT, n as u64),
                None => AklVal::NULL,
            })
        }
        _ => None,
    }
}

fn doc_set(rt: &mut Runtime, _data: u64, _ptr: u64, name: &str, v: AklVal) -> bool {
    if name != "title" {
        return false;
    }
    let Some(s) = to_string(rt, v) else {
        return true;
    };
    with_dom_mut(|d| {
        d.title_set(s.as_bytes());
    });
    true
}

fn doc_call(
    rt: &mut Runtime,
    _data: u64,
    _ptr: u64,
    name: &str,
    args: &[AklVal],
) -> Option<AklVal> {
    match name {
        "getElementById" => {
            if args.len() != 1 {
                return Some(c_throw());
            }
            let Some(id) = as_str(rt, args[0]) else {
                return Some(c_throw());
            };
            let e = with_dom(|d| d.find_by_id(id.as_bytes()));
            Some(match e {
                Some(n) => mk_handle(rt, &ELEM_VT, n as u64),
                None => AklVal::NULL,
            })
        }
        "querySelector" => {
            if args.len() != 1 {
                return Some(c_throw());
            }
            let Some(sel) = as_str(rt, args[0]) else {
                return Some(c_throw());
            };
            if sel.len() >= 512 {
                return Some(c_throw());
            }
            let e = with_dom(|d| d.query_selector(sel.as_bytes()));
            Some(match e {
                Some(n) => mk_handle(rt, &ELEM_VT, n as u64),
                None => AklVal::NULL,
            })
        }
        "getElementsByTagName" => {
            if args.len() != 1 {
                return Some(c_throw());
            }
            let Some(tag) = as_str(rt, args[0]) else {
                return Some(c_throw());
            };
            if tag.len() >= 64 {
                return Some(c_throw());
            }
            let (_total, nodes) = with_dom(|d| d.elements_by_tag(d.root, tag.as_bytes(), 1024));
            let mut vals = Vec::with_capacity(nodes.len());
            for n in nodes {
                vals.push(mk_handle(rt, &ELEM_VT, n as u64));
            }
            Some(match rt.heap.alloc(Obj::Arr(vals)) {
                Ok(id) => AklVal::mk_obj(id),
                Err(_) => AklVal::UNDEF,
            })
        }
        _ => None,
    }
}

fn elem_get(rt: &mut Runtime, _data: u64, ptr: u64, name: &str) -> Option<AklVal> {
    let n = ptr as NodeId;
    match name {
        "textContent" => {
            let t = with_dom(|d| d.text_content(n));
            Some(mk_string(rt, &t))
        }
        "id" => {
            let v = with_dom(|d| d.attr(n, b"id").map(|v| v.to_vec()).unwrap_or_default());
            Some(mk_string(rt, &v))
        }
        "tagName" => {
            let (name, ns) = with_dom(|d| {
                let node = d.node(n);
                (node.name.clone(), node.ns)
            });
            let mut up = name;
            if up.len() >= 64 {
                up.truncate(63);
            }
            if ns == Ns::Html {
                for c in up.iter_mut() {
                    if c.is_ascii_lowercase() {
                        *c = c.to_ascii_uppercase();
                    }
                }
            }
            Some(mk_string(rt, &up))
        }
        "style" => Some(mk_handle(rt, &STYLE_VT, n as u64)),
        _ => None,
    }
}

fn elem_set(rt: &mut Runtime, _data: u64, ptr: u64, name: &str, v: AklVal) -> bool {
    if name != "textContent" {
        return false;
    }
    let Some(s) = to_string(rt, v) else {
        return true;
    };
    with_dom_mut(|d| d.set_text(ptr as NodeId, s.as_bytes()));
    true
}

fn elem_call(
    rt: &mut Runtime,
    _data: u64,
    ptr: u64,
    name: &str,
    args: &[AklVal],
) -> Option<AklVal> {
    match name {
        "getAttribute" => {
            if args.len() != 1 {
                return Some(c_throw());
            }
            let Some(an) = as_str(rt, args[0]) else {
                return Some(c_throw());
            };
            if an.len() >= 128 {
                return Some(c_throw());
            }
            let v = with_dom(|d| {
                d.attr(ptr as NodeId, an.as_bytes())
                    .map(|v| v.to_vec())
                    .unwrap_or_default()
            });
            Some(mk_string(rt, &v))
        }
        "setAttribute" => {
            if args.len() != 2 {
                return Some(c_throw());
            }
            let (Some(an), Some(vn)) = (as_str(rt, args[0]), as_str(rt, args[1])) else {
                return Some(c_throw());
            };
            if an.len() >= 128 {
                return Some(c_throw());
            }
            with_dom_mut(|d| d.attr_set(ptr as NodeId, an.as_bytes(), vn.as_bytes()));
            Some(AklVal::UNDEF)
        }
        _ => None,
    }
}

fn style_get(rt: &mut Runtime, _data: u64, ptr: u64, name: &str) -> Option<AklVal> {
    if name.len() >= 128 {
        return None;
    }
    let n = ptr as NodeId;
    let cur = with_dom(|d| d.attr(n, b"style").map(|v| v.to_vec()).unwrap_or_default());
    let v = style_get_prop(&cur, name.as_bytes()).unwrap_or(b"");
    Some(mk_string(rt, v))
}

fn style_set(rt: &mut Runtime, _data: u64, ptr: u64, name: &str, v: AklVal) -> bool {
    if name.len() >= 128 {
        return false;
    }
    let Some(s) = to_string(rt, v) else {
        return true;
    };
    let n = ptr as NodeId;
    with_dom_mut(|d| {
        let cur = d.attr(n, b"style").map(|v| v.to_vec()).unwrap_or_default();
        let new_attr = style_set_prop(&cur, name.as_bytes(), s.as_bytes());
        d.attr_set(n, b"style", &new_attr);
    });
    true
}

// ---- script 収集（C の `collect_scripts_rec` 相当） ----

fn collect_scripts_rec(dom: &Dom, n: NodeId, out: &mut Vec<NodeId>, cnt: usize) -> usize {
    let mut cnt = cnt;
    let mut c = Some(n);
    while let Some(cid) = c {
        let node = dom.node(cid);
        if node.kind == NodeKind::Element && node.tag == TAG_SCRIPT && node.ns == Ns::Html {
            if cnt < MAX_RUN {
                out.push(cid);
            }
            cnt += 1;
        }
        if let Some(fc) = node.first_child {
            cnt = collect_scripts_rec(dom, fc, out, cnt);
        }
        c = node.next_sibling;
    }
    cnt
}

// ---- eval 補助（akl-ffi の `akl_eval` と同一のエラー文言構築） ----

fn vm_err_string(e: &VmError) -> String {
    match e {
        VmError::StackUnderflow => "stack underflow".into(),
        VmError::LocalOob => "local out of bounds".into(),
        VmError::GlobalNotFound => "ReferenceError: identifier is not defined".into(),
        VmError::NotCallable => "TypeError: not a function".into(),
        VmError::JumpOob => "jump out of bounds".into(),
        VmError::Oom => "out of memory".into(),
        VmError::NotObject => "TypeError: not an object".into(),
        VmError::BudgetExhausted => "instruction budget exhausted".into(),
        VmError::Thrown(v) => format!("uncaught exception: {v:?}"),
    }
}

fn eval_error_string(rt: &Runtime, e: &VmError) -> String {
    match e {
        VmError::Thrown(v) if v.is_obj() => {
            let s = rt.flatten_str(*v);
            if !s.is_empty() {
                s
            } else {
                vm_err_string(e)
            }
        }
        _ => vm_err_string(e),
    }
}

/// ソースを eval する（`akl_eval` 相当）。失敗でエラー文言。
fn eval_script(rt: &mut Runtime, src: &str) -> Result<(), String> {
    let program = match Parser::new(src).parse_program() {
        Ok(p) => p,
        Err(e) => return Err(format!("SyntaxError: {}", e.0)),
    };
    let fidx = match compile(rt, &program) {
        Ok(f) => f,
        Err(e) => return Err(format!("SyntaxError: {}", e.0)),
    };
    match rt.run(fidx, &[]) {
        Ok(_) => Ok(()),
        Err(e) => Err(eval_error_string(rt, &e)),
    }
}

// ---- 本体（C の `if_script_run` 相当） ----

/// DOM の `<script>` を実行し、`dom` を変異しつつ `log` へ `[script:console]` /
/// `[script] FAILED` 行を追記する。戻り値は集計。
///
/// `dom` / `log` は eval 期間中 [`Ctx`] へ移され、終了時に書き戻される（`Dom: Default`
/// による `std::mem::take`）。評価は単一スレッド（`thread_local!` 前提）。
pub fn script_run(dom: &mut Dom, log: &mut Vec<u8>) -> ScriptReport {
    let mut report = ScriptReport::default();
    if !dom.has_script {
        return report;
    }
    // kill switch（IF_SCRIPT=0 で完全 no-op）
    if std::env::var("IF_SCRIPT")
        .map(|s| s == "0")
        .unwrap_or(false)
    {
        return report;
    }

    let (total, list) = collect_scripts(dom);
    if list.is_empty() {
        return report;
    }

    let dom_taken = std::mem::take(dom);
    let log_taken = std::mem::take(log);
    CTX.with(|c| {
        *c.borrow_mut() = Some(Ctx {
            dom: dom_taken,
            log: log_taken,
        })
    });

    if total > MAX_RUN {
        log_bytes(
            format!("[script] script count truncated at {MAX_RUN} (found {total})\n").as_bytes(),
        );
    }

    let mut rt = Runtime::new();
    if install_builtins(&mut rt).is_err() {
        log_bytes(b"[script] FAILED: akl_new failed\n");
        report.n_errors += 1;
    } else if !bootstrap(&mut rt) {
        log_bytes(b"[script] bootstrap FAILED\n");
        report.n_errors += 1;
    } else {
        run_scripts(&mut rt, &list, &mut report);
    }

    CTX.with(|c| {
        let ctx = c.borrow_mut().take().unwrap();
        *dom = ctx.dom;
        *log = ctx.log;
    });
    report
}

fn collect_scripts(dom: &Dom) -> (usize, Vec<NodeId>) {
    let mut out = Vec::new();
    let total = collect_scripts_rec(dom, dom.root, &mut out, 0);
    (total, out)
}

/// `console` / `document` の組込（C の bootstrap）。失敗で `false`。
fn bootstrap(rt: &mut Runtime) -> bool {
    let Some(console_id) = rt.intern("console") else {
        return false;
    };
    let Ok(console) = rt.heap.alloc(Obj::Obj(Vec::new())) else {
        return false;
    };
    let Ok(log) = rt.register_native(script_console_log) else {
        return false;
    };
    let Some(log_name) = rt.intern("log") else {
        return false;
    };
    if rt.heap.prop_set(console, log_name, log).is_err() {
        return false;
    }
    let Some(doc_id) = rt.intern("document") else {
        return false;
    };
    let doc_handle = mk_handle(rt, &DOC_VT, 0);
    if doc_handle == AklVal::UNDEF {
        return false;
    }
    rt.global_set(console_id, AklVal::mk_obj(console));
    rt.global_set(doc_id, doc_handle);
    true
}

fn run_scripts(rt: &mut Runtime, list: &[NodeId], report: &mut ScriptReport) {
    for &sn in list {
        // 外部 script（src 属性）は v1 非対象
        let src = with_dom(|d| d.attr(sn, b"src").map(|v| v.to_vec()).unwrap_or_default());
        if !src.is_empty() {
            report.n_skipped += 1;
            continue;
        }
        // type 属性（module / text/javascript のみ）
        let ty = with_dom(|d| d.attr(sn, b"type").map(|v| v.to_vec()).unwrap_or_default());
        if !ty.is_empty() && !str_eq_ci(&ty, b"module") && !str_eq_ci(&ty, b"text/javascript") {
            report.n_skipped += 1;
            continue;
        }
        let txt = with_dom(|d| d.text_content(sn));
        if txt.is_empty() {
            report.n_skipped += 1;
            continue;
        }
        report.n_run += 1;
        if txt.len() > MAX_SRC {
            report.n_errors += 1;
            log_bytes(b"[script] FAILED: source budget exhausted (>4MB)\n");
            continue;
        }
        if txt.contains(&0) {
            report.n_errors += 1;
            log_bytes(b"[script] FAILED: source contains NUL\n");
            continue;
        }
        let src_str = std::str::from_utf8(&txt).unwrap_or("");
        if let Err(msg) = eval_script(rt, src_str) {
            // C: %.128s で 128 バイト打ち切り + \n \r を空白化
            let mut w: Vec<u8> = msg.into_bytes();
            if w.len() > 128 {
                w.truncate(128);
            }
            for c in w.iter_mut() {
                if *c == b'\n' || *c == b'\r' {
                    *c = b' ';
                }
            }
            log_bytes(b"[script] FAILED: ");
            log_bytes(&w);
            log_bytes(b"\n");
            report.n_errors += 1;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ifuto_core::html_tree;

    /// HTML をパースして script を実行し、(wpt シリアライズ, log, report) を返す。
    fn run(html: &str) -> (String, String, ScriptReport) {
        let mut dom = html_tree::parse_html(html.as_bytes());
        let mut log = Vec::new();
        let report = script_run(&mut dom, &mut log);
        (
            String::from_utf8(dom.serialize_wpt()).unwrap(),
            String::from_utf8(log).unwrap(),
            report,
        )
    }

    #[test]
    fn textcontent_mutation() {
        let (wpt, log, rep) = run("<html><body><div id='a'>old</div>\
             <script>document.getElementById('a').textContent = 'new';</script></body></html>");
        assert!(wpt.contains("\"new\""), "wpt = {wpt}");
        assert_eq!(
            rep,
            ScriptReport {
                n_run: 1,
                n_errors: 0,
                n_skipped: 0
            }
        );
        assert!(log.is_empty(), "log = {log:?}");
    }

    #[test]
    fn title_set_and_console() {
        let (wpt, log, rep) = run("<html><head><title>old</title></head><body>\
             <script>document.title='T'; console.log('hello', 42);</script></body></html>");
        // title_set で <title> の本文が "T" に置換される（wpt 形式は title を木で出す）
        assert!(wpt.contains("<title>"), "wpt = {wpt}");
        assert!(wpt.contains("\"T\""), "wpt = {wpt}");
        assert!(log.contains("[script:console] hello 42\n"), "log = {log:?}");
        assert_eq!(rep.n_errors, 0);
    }

    #[test]
    fn failure_isolation() {
        // 壊れた script は失敗するが後続は継続し、描画は進む
        let (_wpt, log, rep) = run("<html><body><script>this is { not valid</script>\
             <script>document.body.setAttribute('x','y')</script></body></html>");
        assert!(
            log.contains("[script] FAILED: SyntaxError:"),
            "log = {log:?}"
        );
        assert_eq!(rep.n_errors, 1);
        assert_eq!(rep.n_run, 2);
    }

    #[test]
    fn skip_external_and_type() {
        let (_wpt, _log, rep) = run("<html><body>\
             <script src='http://x/y.js'></script>\
             <script type='application/json'>{}</script>\
             <script></script>\
             <script type='module'>var a=1;</script></body></html>");
        assert_eq!(rep.n_skipped, 3); // src / unknown type / 空
        assert_eq!(rep.n_run, 1); // module のみ実行
    }

    #[test]
    fn style_and_tag_name_and_attr() {
        let (_wpt, _log, rep) = run(
            "<html><body><div id='d' style='color: red'></div>\
             <script>var d=document.getElementById('d');\
             d.style.color='blue';\
             d.setAttribute('data-x','1');\
             console.log(d.tagName, d.getAttribute('data-x'), d.style.color);</script></body></html>",
        );
        assert_eq!(rep.n_errors, 0);
    }

    #[test]
    fn kill_switch_and_no_script() {
        // script 非含有は走査ゼロ
        let (_wpt, log, rep) = run("<html><body><p>hi</p></body></html>");
        assert_eq!(rep, ScriptReport::default());
        assert!(log.is_empty());
    }
}
