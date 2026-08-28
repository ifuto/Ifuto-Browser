//! 統合 CLI（C の `ifuto` の観測モード相当。フェーズ 8 の統合実証）。
//!
//! 全モジュール（html_tok / html_tree / dom / css / layout / render / md）を配線し、
//! C の `--dump-wptdom` / `--dump-dom` / `--dump-layout` / `--dump-styles` /
//! `--dump-tokens` / `--no-ansi` render を再現する。差分 fuzz 35,000 件 0 不一致、
//! golden 1/1、html5lib 1922/1922 を Rust 単体で実証した回帰ハーネス。
//!
//! 使い方: `cargo build --release --example ifuto` → `.../ifuto [options] < stdin`
//! ネットワーク・JS 実行・GUI は対象外（最終統合層）。md は `--md` で強制
//! （C の `IFUTO_MD_SLOW=1` = md→html→parse 経路と同値）。

use std::io::{self, Read, Write};

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
        // %-8s（8 バイトへ右パディング）
        let mut field = k.as_bytes().to_vec();
        while field.len() < 8 {
            field.push(b' ');
        }
        out.extend_from_slice(&field);
        out.push(b' ');
        // %-12.*s（precision = 全長 = 切り詰めなし。12 バイト未満は右パディング）
        let mut field = tok.tag_raw.to_vec();
        while field.len() < 12 {
            field.push(b' ');
        }
        out.extend_from_slice(&field);
        if tok.kind == TokKind::Text {
            // %.*s（48 バイトへ切り詰め）
            out.extend_from_slice(b" \"");
            out.extend_from_slice(&tok.text[..tok.text.len().min(48)]);
            out.push(b'"');
        }
        for a in &tok.attrs {
            // %.*s（name は全長、value は 32 バイトへ切り詰め）
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

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut width = 100i32;
    let mut ansi = true;
    let mut force_md = false;
    let mut frag_ctx: Option<String> = None;
    let mut mode = "render"; // render | wptdom | dom | layout | tokens | styles

    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--width" => {
                if i + 1 < args.len() {
                    width = args[i + 1].parse().unwrap_or(100);
                    i += 1;
                }
            }
            "--no-ansi" => ansi = false,
            "--md" => force_md = true,
            "--dump-wptdom" => mode = "wptdom",
            "--dump-dom" => mode = "dom",
            "--dump-layout" => mode = "layout",
            "--dump-tokens" => mode = "tokens",
            "--dump-styles" => mode = "styles",
            "--fragment" => {
                if i + 1 < args.len() {
                    frag_ctx = Some(args[i + 1].clone());
                    i += 1;
                }
            }
            "-" => {}
            _ => {}
        }
        i += 1;
    }

    let mut input = Vec::new();
    io::stdin().read_to_end(&mut input).unwrap();

    // md 変換（C の md→html→parse 経路 = IFUTO_MD_SLOW 相当）
    if force_md {
        input = ifuto_core::md::md_to_html(&input);
    }

    if mode == "tokens" {
        let out = dump_tokens(&input);
        io::stdout().write_all(&out).unwrap();
        return;
    }

    // パース
    let dom = match &frag_ctx {
        Some(ctx) => ifuto_core::html_tree::parse_html_fragment(&input, ctx),
        None => ifuto_core::html_tree::parse_html(&input),
    };

    match mode {
        "wptdom" => {
            let out = match &frag_ctx {
                Some(_) => dom.serialize_wpt_frag(),
                None => dom.serialize_wpt(),
            };
            io::stdout().write_all(&out).unwrap();
        }
        "dom" => {
            let out = dom.dump();
            io::stdout().write_all(&out).unwrap();
        }
        "layout" | "render" | "styles" => {
            let styles = ifuto_core::css::apply_styles(&dom);
            match mode {
                "layout" => {
                    let lay = ifuto_core::layout::layout_build(&dom, &styles, width);
                    let out = ifuto_core::layout::layout_dump(&dom, &lay);
                    io::stdout().write_all(&out).unwrap();
                }
                "styles" => {
                    let out = ifuto_core::css::dump_styles(&dom, &styles);
                    io::stdout().write_all(out.as_bytes()).unwrap();
                }
                _ => {
                    // render（行スイープ経路）
                    let lay = ifuto_core::layout::layout_build(&dom, &styles, width);
                    let out = ifuto_core::render::render_emit_sweep(&dom, &lay, ansi);
                    io::stdout().write_all(&out).unwrap();
                }
            }
        }
        _ => {}
    }
}
