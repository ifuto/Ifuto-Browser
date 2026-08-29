//! parse_breakdown — parse 段コストの機能クラス分解（フェーズ 12-c 計測器具）。
//!
//! 合成入力をクラス別に生成して `/tmp/pb-*.md` に書き出し、CLI の --stats で
//! C/Rust を同型比較できるようにする。加えて Rust 側は in-process で
//! `md_to_dom` 時間を直接計測する（CLI の parse 段より細かい器具）。
//! ※ 生成ロジックは片側のみ。ファイルは CLI 双方が同じものを読むので生成差は出ない。

use std::time::Instant;

fn mb_pattern(name: &str, mut put: impl FnMut(&mut Vec<u8>, usize)) -> Vec<u8> {
    // ~16 MiB に届くまで行単位で繰り返す。最後に dump する。
    let mut v = Vec::with_capacity(17 << 20);
    let mut n = 0usize;
    while v.len() < (16 << 20) {
        put(&mut v, n);
        n += 1;
    }
    let path = format!("/tmp/pb-{name}.md");
    std::fs::write(&path, &v).expect("write");
    eprintln!("{name}: {} bytes -> {path}", v.len());
    v
}

fn main() {
    // 1. plain: 特殊文字ゼロの本文行（scan 素通し + text 排出）
    let plain = mb_pattern("plain", |v, n| {
        v.extend_from_slice(b"the quick brown fox jumps over the lazy dog again and again packing words into a plain line of prose text");
        if n % 8 == 7 {
            v.extend_from_slice(b"\n\n");
        } else {
            v.push(b'\n');
        }
    });
    // 2. inline: インライン構文過密（dispatch 重い）
    let inl = mb_pattern("inline", |v, n| {
        v.extend_from_slice(b"text *em* **strong** `code` [link](/u/x) ![img](/i.png) &amp; <https://x.jp> ~~del~~ _u_\\*esc ");
        if n % 8 == 7 {
            v.push(b'\n');
        }
        v.push(b'\n');
    });
    // 3. table: 表セル分割 + 区切り走査（split_cells 系）
    let tab = mb_pattern("table", |v, n| {
        if n % 20 == 0 {
            v.extend_from_slice(b"| col a | col b | col c | col d |\n| --- | --- | --- | --- |\n");
        } else {
            v.extend_from_slice(b"| alpha1234567 | beta | gamma delta | epsilon |\n");
        }
    });
    // 4. blocks: 見出し/引用/リスト/fence のブロック機構（block dispatch 重い）
    let blk = mb_pattern("blocks", |v, n| match n % 12 {
        0 => v.extend_from_slice(b"## heading two\n"),
        1 => v.extend_from_slice(b"> quote line with some content inside it\n"),
        2 => v.extend_from_slice(b"- list item one two three\n"),
        3 => v.extend_from_slice(b"1. ordered item four five six\n"),
        4 => v.extend_from_slice(b"```\ncode line without specials\nlet x = 1;\n```\n"),
        5 => v.extend_from_slice(b"\n"),
        _ => v.extend_from_slice(b"normal paragraph line for blocks class\n"),
    });

    // 5. head: 見出しのみ
    let hd = mb_pattern("head", |v, n| {
        v.extend_from_slice(b"## heading number seven with some words\n");
        if n % 3 == 2 {
            v.push(b'\n');
        }
    });
    // 6. quote: 引用のみ
    let qt = mb_pattern("quote", |v, n| {
        v.extend_from_slice(b"> quoted prose content with enough words to be a real line\n");
        if n % 8 == 7 {
            v.push(b'\n');
        }
    });
    // 7. list: 非順序リストのみ
    let li = mb_pattern("list", |v, n| {
        v.extend_from_slice(b"- list item with a bunch of plain words inside\n");
        if n % 10 == 9 {
            v.push(b'\n');
        }
    });
    // 8. fence: コード塊のみ（raw text 経路）
    let fc = mb_pattern("fence", |v, n| match n % 9 {
        0 => v.extend_from_slice(b"```\n"),
        8 => v.extend_from_slice(b"```\n\n"),
        _ => v.extend_from_slice(b"let variable_name = compute(something, another, 42);\n"),
    });

    // 9. head_tiny: 微行見出し（行固定費の剥き出し）
    let ht = mb_pattern("head_tiny", |v, _n| {
        v.extend_from_slice(b"## t\n");
    });
    // 10. para1: 微行段落 1 行 1 段落（段落機構の行固定費）
    let p1 = mb_pattern("para1", |v, _n| {
        v.extend_from_slice(b"x\n\n");
    });

    for (name, buf) in [
        ("plain", &plain),
        ("inline", &inl),
        ("table", &tab),
        ("blocks", &blk),
        ("head", &hd),
        ("quote", &qt),
        ("list", &li),
        ("fence", &fc),
        ("head_tiny", &ht),
        ("para1", &p1),
    ] {
        let mut ts = Vec::new();
        for _ in 0..7 {
            let t0 = Instant::now();
            match ifuto_core::md::md_to_dom(buf) {
                Some(dom) => {
                    std::hint::black_box(dom.n_nodes);
                }
                None => eprintln!(
                    "{name}: md_to_dom = None（taint 拒否。CLI 出力は空規約のはず、C と要突合）"
                ),
            }
            ts.push(t0.elapsed().as_secs_f64() * 1000.0);
        }
        ts.sort_by(|a, b| a.partial_cmp(b).unwrap());
        eprintln!(
            "R md_to_dom {name}: med {:.2}ms (min {:.2} max {:.2})  ns/B={:.2}",
            ts[ts.len() / 2],
            ts[0],
            ts[ts.len() - 1],
            ts[ts.len() / 2] * 1e6 / buf.len() as f64
        );
    }
}
