//! Ifuto Browser — 統合 CLI（C の `src/main.c` の完全置換。フェーズ 8-z）。
//!
//! 全観測モード（render / --dump-* / --fragment / --links / --stats / --md /
//! --slim-dom / --show-paths / --imgdecode）+ http(s) 取得 + `<script>` 実行を
//! Rust 単体で再現する。**既知のうつわ（正直に開示）**: GUI（`--gui` / `--shot` /
//! `--ui`）は未移植で、明示メッセージ + 終了コード 2 で明白に拒否する
//! （黙って異なる解釈をしない = INV-3 と同じ原則。GUI は C バイナリ `build/ifuto`
//! が担う。フェーズ 9 で移植予定）。
//!
//! ## C との対応
//!
//! | C (main.c) | Rust |
//! |---|---|
//! | `read_all`（mmap / fread / stdin / http） | [`read_all`] |
//! | `to_utf8_html`（A1 正規化関門） | [`to_utf8_html`] |
//! | `show_paths`（INV-9） | [`show_paths`] |
//! | `--imgdecode`（PNG/BMP → PPM） | `img_decode_mode` |
//! | dump-tokens ループ | [`dump_tokens`] |
//! | `--links` のリンク収集 | [`collect_links`] |
//! | `--stats`（5 段計測 + VmHWM 自己報告） | `print_stats` |
//!
//! 既知の偏差（正直に開示）:
//! - file 読みは mmap ではなく `std::fs::read` 経路（出力は同値 = 観測不変、速度差は
//!   ベンチに記録。mmap 化は将来の最適化）。
//! - `--stats` の `arena_kb` / `arena_used_kb` は C の arena 会計に対応する概念が
//!   Rust 側に無い（所有 `Vec` のため reserved/used の区別が存在しない）ので、
//!   ラベル形状を保ち値は 0.0 とし、別行で明示する。

use ifuto_core::dom::{Dom, NodeId, NodeKind};
use ifuto_core::{charset, css, html_tree, image, layout, md, render};
use std::io::{Read, Write};
use std::process::exit;
use std::time::Instant;

/// C の `IF_MAX_INPUT_BYTES`（512MB）。
const MAX_INPUT_BYTES: u64 = 512 * 1024 * 1024;

#[derive(PartialEq, Clone, Copy, Debug)]
enum Mode {
    Render,
    Dom,
    Layout,
    Tokens,
    Wptdom,
    Styles,
    Gui,
    Imgdecode,
}

/// C の `usage()` と byte 一致のテキスト。
const USAGE: &[u8] = b"ifuto v0.1 \xe2\x80\x94 the strongest lightweight browser (core slice)\n\
usage: ifuto [options] FILE | http://URL | -\n\
\x20 --width N        viewport cell width (default 100)\n\
\x20 --no-ansi        plain text output (no SGR colors)\n\
\x20 --no-style       skip stylesheet application\n\
\x20 --dump-dom       print DOM tree\n\
\x20 --dump-layout    print box tree\n\
\x20 --dump-tokens    print HTML tokens\n\
\x20 --dump-wptdom    print DOM in html5lib tree-construction format\n\
\x20 --fragment CTX   parse HTML fragment with context CTX (\"body\" / \"svg path\"...)\n\
\x20 --dump-styles    print computed styles per element (devtools)\n\
\x20 --gui            interactive GUI (single supported UI; TUI \xe3\x81\xaf\xe5\xbb\x83\xe6\xad\xa2)\n\
\x20 --shot OUT.ppm   headless full-page raster to PPM (GUI \xe6\xa4\x9c\xe8\xa8\xbc\xe7\xb5\x8c\xe8\xb7\xaf)\n\
\x20 --md             force Markdown parsing (auto for .md/.markdown files)\n\
\x20 --slim-dom       drop display-irrelevant subtrees (script/template) from DOM\n\
\x20 --links          print collected links\n\
\x20 --stats          print timing/memory stats to stderr\n\
\x20 --ext DIR        load extensions from DIR at chrome init (GUI/--shot\xe3\x80\x82E1)\n\
\x20 --show-paths     list persisted-data paths (INV-9; no side effects)\n\
\x20 --imgdecode     decode PNG/BMP file to PPM on stdout (image tooling)\n";

fn usage(w: &mut dyn Write) {
    let _ = w.write_all(USAGE);
}

/// C の `atoi`（先頭空白・符号・数字 prefix）相当。
fn atoi(s: &str) -> i32 {
    let b = s.as_bytes();
    let mut i = 0;
    while i < b.len()
        && (b[i] == b' '
            || b[i] == b'\t'
            || b[i] == b'\n'
            || b[i] == 11
            || b[i] == 12
            || b[i] == 13)
    {
        i += 1;
    }
    let mut neg = false;
    if i < b.len() && (b[i] == b'+' || b[i] == b'-') {
        neg = b[i] == b'-';
        i += 1;
    }
    let mut v: i64 = 0;
    while i < b.len() && b[i].is_ascii_digit() {
        v = v.saturating_mul(10).saturating_add((b[i] - b'0') as i64);
        i += 1;
    }
    let v = if neg { -v } else { v };
    v.clamp(i32::MIN as i64, i32::MAX as i64) as i32
}

/// C の `show_paths()`（INV-9。副作用ゼロ）。
fn show_paths() -> i32 {
    let dir = match ifuto_core::store::resolve_store_dir() {
        Some(d) => d,
        None => {
            println!("ifuto: no data dir (IFUTO_HOME / XDG_DATA_HOME / HOME are unset)");
            return 0;
        }
    };
    println!("data dir: {dir}");
    for name in ["session.txt", "history.tsv", "bookmarks.tsv"] {
        let p = format!("{dir}/{name}");
        // C の %-14s（左揃え 14 幅）再現
        let padded = format!("{name:<14}");
        match std::fs::metadata(&p) {
            Ok(md) => println!("  {padded} {p} ({} bytes)", md.len()),
            Err(_) => println!("  {padded} {p} (absent)"),
        }
    }
    0
}

/// C の `read_all` 相当。http/https は `net_sock::http_get_ex`、それ以外は
/// ファイル（"`-`" = stdin）。失敗時は C と同一メッセージで exit。
/// 戻り値は (input, content_type)。
fn read_all(path: &str) -> (Vec<u8>, Option<Vec<u8>>) {
    if path.starts_with("http://") || path.starts_with("https://") {
        return match ifuto_ffi::net_sock::http_get_ex(path) {
            Ok((body, _status, ctype)) => (body, ctype),
            Err(err) => {
                eprintln!("ifuto: cannot fetch {path}: {err}");
                exit(1);
            }
        };
    }
    if path != "-" {
        // サイズ検査: C は mmap 前に IF_MAX_INPUT_BYTES を超えたら即死する
        if let Ok(md) = std::fs::metadata(path) {
            if md.is_file() && md.len() > MAX_INPUT_BYTES {
                eprintln!("ifuto: input too large");
                exit(1);
            }
        }
        match std::fs::read(path) {
            Ok(mut b) => {
                b.shrink_to_fit();
                (b, None)
            }
            Err(_) => {
                eprintln!("ifuto: cannot open {path}");
                exit(1);
            }
        }
    } else {
        let mut b = Vec::new();
        let mut stdin = std::io::stdin();
        loop {
            let mut chunk = [0u8; 65536];
            match stdin.read(&mut chunk) {
                Ok(0) => break,
                Ok(n) => {
                    b.extend_from_slice(&chunk[..n]);
                    if b.len() as u64 > MAX_INPUT_BYTES {
                        eprintln!("ifuto: input too large");
                        exit(1);
                    }
                }
                Err(_) => {
                    eprintln!("ifuto: read error on {path}");
                    exit(1);
                }
            }
        }
        (b, None)
    }
}

/// C の `to_utf8_html`（docs/CHARSET.md の A1 正規化関門）相当。
fn to_utf8_html(ctype: Option<&[u8]>, input: Vec<u8>) -> Vec<u8> {
    let (enc, bom) = charset::sniff(ctype, &input);
    if enc == charset::Enc::Utf8 {
        if bom && input.len() >= 3 {
            return input[3..].to_vec();
        }
        input
    } else {
        charset::decode(&input, enc)
    }
}

/// C の dump-tokens ループ相当（`--dump-tokens`）。
fn dump_tokens(input: &[u8]) -> Vec<u8> {
    use ifuto_core::html_tok::{TokKind, Tokenizer};
    let mut out = Vec::new();
    let mut t = Tokenizer::new(input);
    loop {
        let tok = t.next();
        if tok.kind == TokKind::Eof {
            break;
        }
        let k = match tok.kind {
            TokKind::Text => "TEXT",
            TokKind::Start => "START",
            TokKind::End => "END",
            TokKind::Comment => "COMMENT",
            TokKind::Doctype => "DOCTYPE",
            TokKind::Eof => unreachable!(),
        };
        let mut field = k.as_bytes().to_vec();
        field.resize(8, b' ');
        out.extend_from_slice(&field);
        out.push(b' ');
        let mut field = tok.tag_raw.to_vec();
        if field.len() < 12 {
            field.resize(12, b' ');
        }
        out.extend_from_slice(&field);
        if tok.kind == TokKind::Text {
            out.extend_from_slice(b" \"");
            out.extend_from_slice(&tok.text[..tok.text.len().min(48)]);
            out.push(b'"');
        }
        for a in &tok.attrs {
            out.push(b' ');
            out.extend_from_slice(&a.name);
            out.extend_from_slice(b"=\"");
            out.extend_from_slice(&a.value[..a.value.len().min(32)]);
            out.push(b'"');
        }
        out.push(b'\n');
    }
    out
}

/// C の `--links`（`lay->links` の順 = 文書順）相当のリンク収集。
///
/// C の収集規則（実測で確認 2026-08-24）:
/// - レイアウトは `<body>` から歩く（head 配下の `<a>` は対象外）。
/// - `<a>` で href 属性が存在し空でないものを文書順に収集。
/// - `display:none` の部分木は flatten されない（収集されない）。
///   本関数は DOM を文書順 DFS して同じ集合を作る（レイアウトを経由しないため
///   分岐を 1 本化できる。収集順 = 文書順であることは差分 fuzz が機械検査する）。
fn collect_links(
    dom: &Dom,
    mut st_of: impl FnMut(&Dom, NodeId) -> Option<css::Style>,
) -> Vec<Vec<u8>> {
    use ifuto_core::tags_tables::{TAG_A, TAG_BODY, TAG_HTML};
    let mut out = Vec::new();
    let mut stack: Vec<NodeId> = Vec::new();
    // body 発見（C の layout 起点と同じルール = layout_build の body 捜索と同一）
    let mut roots: Vec<NodeId> = Vec::new();
    let mut c = dom.node(dom.root).first_child;
    while let Some(cid) = c {
        let cnode = dom.node(cid);
        if cnode.kind == NodeKind::Element && cnode.tag == TAG_HTML {
            let mut g = cnode.first_child;
            while let Some(gid) = g {
                let gnode = dom.node(gid);
                if gnode.kind == NodeKind::Element && gnode.tag == TAG_BODY {
                    roots.push(gid);
                    break;
                }
                g = gnode.next_sibling;
            }
            if !roots.is_empty() {
                break;
            }
        }
        c = cnode.next_sibling;
    }
    for r in roots.into_iter().rev() {
        stack.push(r);
    }
    while let Some(nid) = stack.pop() {
        let n = dom.node(nid);
        if n.kind != NodeKind::Element {
            continue;
        }
        // スタイル適用済みで display:none なら部分木ごと対象外（自身は除外済みで到達しない）。
        let hidden = st_of(dom, nid).is_some_and(|s| s.display == css::D_NONE);
        if hidden {
            continue;
        }
        if n.tag == TAG_A {
            if let Some(href) = dom.attr(nid, b"href") {
                if !href.is_empty() {
                    out.push(href.to_vec());
                }
            }
        }
        // 子を逆順に積んで文書順 pop（Rust Node は片方向連結のため一時 Vec で反転）
        let mut kids: Vec<NodeId> = Vec::new();
        let mut c = n.first_child;
        while let Some(cid) = c {
            kids.push(cid);
            c = dom.node(cid).next_sibling;
        }
        for &kid in kids.iter().rev() {
            stack.push(kid);
        }
    }
    out
}

/// /proc/self/status の VmHWM（KB 自己報告。C の `--stats` と同じ取得法）。
fn peak_rss_kb() -> u64 {
    let s = match std::fs::read_to_string("/proc/self/status") {
        Ok(s) => s,
        Err(_) => return 0,
    };
    for line in s.lines() {
        if let Some(rest) = line.strip_prefix("VmHWM:") {
            let kb: String = rest.chars().filter(|c| c.is_ascii_digit()).collect();
            return kb.parse().unwrap_or(0);
        }
    }
    0
}

/// /proc/self/statm の第 2 フィールド（ページ数）× 4（4KB ページ。
/// C の script 前後 RSS 観測と同じ取得法）。
fn statm_rss_kb() -> i64 {
    let s = match std::fs::read_to_string("/proc/self/statm") {
        Ok(s) => s,
        Err(_) => return 0,
    };
    let rss: i64 = s
        .split_whitespace()
        .nth(1)
        .and_then(|v| v.parse().ok())
        .unwrap_or(0);
    rss * 4
}

/// C の `--imgdecode` モード相当（PNG/BMP → PPM P6 RGB）。
fn img_decode_mode(path: Option<&str>) -> i32 {
    let path = match path {
        Some(p) => p,
        None => {
            usage(&mut std::io::stderr());
            return 2;
        }
    };
    let data = match std::fs::read(path) {
        Ok(d) => d,
        Err(_) => {
            eprintln!("ifuto: cannot read {path}");
            return 1;
        }
    };
    match image::decode(&data) {
        Ok(img) => {
            let mut out = Vec::with_capacity(32 + img.w as usize * img.h as usize * 3);
            out.extend_from_slice(format!("P6\n{} {}\n255\n", img.w, img.h).as_bytes());
            for px in img.px.chunks_exact(4) {
                out.extend_from_slice(&px[..3]);
            }
            let _ = std::io::stdout().write_all(&out);
            0
        }
        Err(e) => {
            eprintln!("ifuto: {e}");
            1
        }
    }
}

/// GUI 系モードの正直な拒否（嘘をつかない = 未実装は未実装と明記する）。
fn gui_unported() -> i32 {
    eprintln!("ifuto: GUI（--gui/--shot/--ui）は Rust 側未移植です（フェーズ 9 予定）。");
    eprintln!("ifuto: GUI を使う場合は C バイナリ build/ifuto を使用してください。");
    2
}

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    let mut width: i32 = 100;
    let mut ansi = true;
    let mut do_style = true;
    let mut links = false;
    let mut stats = false;
    let mut force_md = false;
    let mut slim = false;
    let mut mode = Mode::Render;
    let mut path: Option<String> = None;
    let mut shot: Option<String> = None;
    let mut frag_ctx: Option<String> = None;
    let mut legacy_ui = false;

    let mut i = 1;
    while i < argv.len() {
        let a = argv[i].as_str();
        match a {
            "--width" if i + 1 < argv.len() => {
                width = atoi(&argv[i + 1]);
                i += 1;
            }
            "--no-ansi" => ansi = false,
            "--no-style" => do_style = false,
            "--dump-dom" => mode = Mode::Dom,
            "--dump-layout" => mode = Mode::Layout,
            "--dump-tokens" => mode = Mode::Tokens,
            "--dump-wptdom" => mode = Mode::Wptdom,
            "--fragment" if i + 1 < argv.len() => {
                frag_ctx = Some(argv[i + 1].clone());
                i += 1;
            }
            "--dump-styles" => mode = Mode::Styles,
            "--gui" => mode = Mode::Gui,
            "--imgdecode" => mode = Mode::Imgdecode,
            "--shot" if i + 1 < argv.len() => {
                shot = Some(argv[i + 1].clone());
                i += 1;
            }
            "--ui" => {
                mode = Mode::Gui;
                legacy_ui = true;
            }
            "--links" => links = true,
            "--stats" => stats = true,
            "--md" => force_md = true,
            "--slim-dom" => slim = true,
            "--ext" if i + 1 < argv.len() => {
                // C は dir を拡張サブシステムに渡すだけ（CLI 経路では未使用）。
                i += 1;
            }
            "--show-paths" => exit(show_paths()),
            "--help" | "-h" => {
                usage(&mut std::io::stdout());
                exit(0);
            }
            _ => {
                if a.starts_with('-') && a.len() > 1 {
                    usage(&mut std::io::stderr());
                    exit(2);
                }
                path = Some(a.to_string());
            }
        }
        i += 1;
    }

    if path.is_none() && mode != Mode::Gui && shot.is_none() {
        usage(&mut std::io::stderr());
        exit(2);
    }
    if legacy_ui {
        eprintln!("ifuto: --ui(TUI) は完全廃止。GUI（--gui）へ移行しました");
    }
    if mode == Mode::Imgdecode {
        exit(img_decode_mode(path.as_deref()));
    }
    if shot.is_some() || mode == Mode::Gui {
        exit(gui_unported());
    }
    if !(4..=100000).contains(&width) {
        eprintln!("ifuto: bad --width");
        exit(2);
    }

    let path = path.unwrap();
    let t0 = Instant::now();
    let (mut input, ctype) = read_all(&path);
    let md_doc = force_md || md::path_is_md(&path);
    if !md_doc {
        input = to_utf8_html(ctype.as_deref(), input);
    }
    let t1 = Instant::now();

    // --fragment ガード（C: dump-wptdom / dump-dom 専用。read 後に判定する順も同一）
    if frag_ctx.is_some() && mode != Mode::Wptdom && mode != Mode::Dom {
        eprintln!("ifuto: --fragment は --dump-wptdom / --dump-dom 専用");
        exit(2);
    }

    // md（+GFM 表/脚注）は HTML に前段変換してから単一 WHATWG パーサへ（多層防御）。
    // tokens/wptdom は「HTML 段」の観測点なので、これらのモードでも変換後を対象にする。
    // v0.3: それ以外は DOM 直構築（fast-DOM。C `if_md_parse_fast_f` 相当）を先に試し、
    // taint 観測時は従来 2 段経路へフォールバックする（C main.c と同じ分岐規則。
    // `IFUTO_MD_SLOW` は「環境変数が存在すれば」2 段固定の kill switch。
    // --fragment は md 変換を噛まない（fragment は観測モード専用））。
    // Render（C の M_RENDER）では SLIM_ATTRS（保持属性を A[href]/IMG[alt] に限定。
    // links 収集と IMG alt 以外から属性を読まない経路でのみ観測不変）。
    let md_html: Vec<u8>;
    let mut dom_src: &[u8] = &input;
    let dom = match &frag_ctx {
        Some(ctx) => html_tree::parse_html_fragment_opts(dom_src, ctx, slim),
        None => {
            let mut d = None;
            if md_doc
                && mode != Mode::Tokens
                && mode != Mode::Wptdom
                && std::env::var_os("IFUTO_MD_SLOW").is_none()
            {
                d = md::md_to_dom_opts(&input, mode == Mode::Render);
            }
            match d {
                Some(fast_dom) => fast_dom,
                None => {
                    if md_doc {
                        md_html = md::md_to_html(&input);
                        dom_src = &md_html;
                    }
                    html_tree::parse_html_opts(dom_src, slim)
                }
            }
        }
    };
    let t2 = Instant::now();

    match mode {
        Mode::Wptdom => {
            let out = if frag_ctx.is_some() {
                dom.serialize_wpt_frag()
            } else {
                dom.serialize_wpt()
            };
            std::io::stdout().write_all(&out).unwrap();
            exit(0);
        }
        Mode::Tokens => {
            std::io::stdout().write_all(&dump_tokens(dom_src)).unwrap();
            exit(0);
        }
        Mode::Dom => {
            std::io::stdout().write_all(&dom.dump()).unwrap();
            exit(0);
        }
        _ => {}
    }

    // <script> akl 実行（style 適用前。失敗は script 単位で隔離・描画継続）
    let rss_before = statm_rss_kb();
    let tscr0 = Instant::now();
    let mut dom = dom;
    let mut log: Vec<u8> = Vec::new();
    let srep = ifuto_ffi::script_run(&mut dom, &mut log);
    let script_ms = tscr0.elapsed().as_secs_f64() * 1000.0;
    let rss_after = statm_rss_kb();
    if !log.is_empty() {
        let _ = std::io::stderr().write_all(&log);
    }

    // style（--no-style なら cascade 未実行の正直な姿）
    // lazy style: md fast-DOM × 行スイープ（C の M_RENDER 相当）では style 全面走査を
    // 消し、layout の DFS 訪問時に必要箇所だけ解決する（解決値は apply_styles と同値。
    // C の use_lazy_style = if_md_style_lazy_ok(dom) && !has_script && !has_style && M_RENDER
    // の写し。script 含有文書は script 後 DOM との同値性を eager で確定する）。
    let style_lazy = do_style
        && mode == Mode::Render
        && css::style_lazy_ok(&dom)
        && !dom.has_script
        && !dom.has_style;
    // lazy 時は styles 表自体が不要（C は if_style_apply を呼ばない）ため確保もしない
    // （1.6M × Option<Style> の死蔵 malloc+memset を構造消去）。eager/--no-style は従来どおり。
    let styles: Vec<Option<css::Style>> = if style_lazy {
        Vec::new()
    } else if do_style {
        css::apply_styles(&dom)
    } else {
        vec![None; dom.nodes.len()]
    };
    let t3 = Instant::now();

    if mode == Mode::Styles {
        let out = css::dump_styles(&dom, &styles);
        std::io::stdout().write_all(out.as_bytes()).unwrap();
        exit(0);
    }

    // 描画経路は線形モード（C の CLI no_boxlink 相当: box 木を連結しない）。
    // dump（Mode::Layout）だけが木を必要とするため tree ビルドを使う。
    let lay = match (mode == Mode::Layout, style_lazy) {
        (true, true) => layout::layout_build_lazy(&dom, width),
        (true, false) => layout::layout_build(&dom, &styles, width),
        (false, true) => layout::layout_build_lazy_linear(&dom, width),
        (false, false) => layout::layout_build_linear(&dom, &styles, width),
    };
    let t4 = Instant::now();

    if mode == Mode::Layout {
        std::io::stdout()
            .write_all(&layout::layout_dump(&dom, &lay))
            .unwrap();
        exit(0);
    }

    // 行スイープ経路（C の `if_render_emit_rows_sweep` 相当）に一本化。
    // 全グリッド構築は存在しないため grid 段は C と同じく限 0（render_split の
    // grid=0.00 表記を C の CLI と同一にする）。
    let acc_grid = 0.0f64; // C の CLI sweep 経路は grid=0.00 固定（観測一致）
    let tg1 = Instant::now();
    let out = render::render_emit_sweep(&dom, &lay, ansi);
    std::io::stdout().write_all(&out).unwrap();
    let acc_emit = tg1.elapsed().as_secs_f64() * 1000.0;
    let t5 = Instant::now();
    // C の CLI は linear build（no_boxlink: 子 BLOCK を木にリンクしない）のため、
    // stats の grid=WxH extent 歩行は root 自身までしか到達しない（children が
    // 存在する Rust 木をそのまま歩くと、右寄せ+ハード分割の overflow seg 分だけ
    // 過大になる）。C 観測値 = max(lay.width/height, root の x+w/y+h)。
    let (mx, my) = {
        let r = &lay.root;
        (
            lay.width.max(r.x + r.w).max(1),
            lay.height.max(r.y + r.h).max(1),
        )
    };

    // C はレイアウトが常にリンクを収集する（lay->n_links は --links 旗と無関係に
    // stats へ出る）。収集順・収集集合 = 文書順 DFS（差分 fuzz が機械検査）。
    let collected = if style_lazy {
        // lazy の display:none 判定は解決値を読む。UA シートのみ（lazy 前提）では
        // display は継承されず (tag|name, inline style) のみの関数 = parent 非依存で
        // 全面走査値と同値（layout の DFS で解決される値は同じ intern 系を通る）。
        // None parent での解決は display 判定専用として正当。
        let mut lz = css::StyleLazy::new();
        collect_links(&dom, move |dom2, nid| {
            Some(if dom2.node(nid).kind == NodeKind::Element {
                lz.get(dom2, nid, None, 16.0)
            } else {
                return None;
            })
        })
    } else {
        collect_links(&dom, |_, nid| styles.get(nid as usize).copied().flatten())
    };
    if links && !collected.is_empty() {
        let stdout = std::io::stdout();
        let mut w = stdout.lock();
        w.write_all("\nリンク:\n".as_bytes()).unwrap();
        for (i, href) in collected.iter().enumerate() {
            w.write_all(format!("[{}] ", i + 1).as_bytes()).unwrap();
            w.write_all(href).unwrap();
            w.write_all(b"\n").unwrap();
        }
    }

    if stats {
        let script_line = if srep.n_run > 0 {
            Some(format!(
                "ifuto stats: scripts={} errors={} skipped={} script_ms={:.2} resid_rss_kb={} (script 前後差 {:+})\n",
                srep.n_run,
                srep.n_errors,
                srep.n_skipped,
                script_ms,
                rss_after,
                rss_after - rss_before
            ))
        } else {
            None
        };
        let read_ms = (t1 - t0).as_secs_f64() * 1000.0;
        let parse_ms = (t2 - t1).as_secs_f64() * 1000.0;
        let style_ms = (t3 - t2).as_secs_f64() * 1000.0 - script_ms;
        let layout_ms = (t4 - t3).as_secs_f64() * 1000.0;
        let render_ms = (t5 - t4).as_secs_f64() * 1000.0;
        let total_ms = (t5 - t0).as_secs_f64() * 1000.0;
        let mut e = String::new();
        if let Some(l) = script_line {
            e.push_str(&l);
        }
        e.push_str(&format!(
            "ifuto stats: read={read_ms:.2}ms parse={parse_ms:.2}ms style={style_ms:.2}ms layout={layout_ms:.2}ms render={render_ms:.2}ms total={total_ms:.2}ms\n"
        ));
        e.push_str(&format!(
            "  nodes={} parse_errors={} grid={}x{} links={} peak_rss_kb={}\n",
            dom.n_nodes,
            dom.n_errors,
            mx,
            my,
            collected.len(),
            peak_rss_kb()
        ));
        e.push_str(&format!(
            "  render_split: grid={acc_grid:.2}ms emit={acc_emit:.2}ms\n"
        ));
        // arena 会計は C 概念（Rust 版は所有 Vec = reserved/used の区別なし）。
        // ラベル形状は保ち、値 0.0 + 明示行で正直に開示する。
        e.push_str("  arena_kb: parse=0.0 style=0.0 layout=0.0 render=0.0\n");
        e.push_str("  arena_used_kb: parse=0.0 style=0.0 layout=0.0 render=0.0 (reserved 比で無駄の監視)\n");
        e.push_str("  note: arena_kb は C arena 会計。Rust 版は所有 Vec のため対象外（0 表示）\n");
        let _ = std::io::stderr().write_all(e.as_bytes());
    }
    exit(0);
}
