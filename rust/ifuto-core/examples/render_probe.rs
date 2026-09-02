//! render_probe — render 段コストの逐層解剖（フェーズ 12-i 計測器具）。
//!
//! `render_emit_sweep` の実コストを「全段」「byte-direct 単体」「事前確保版」
//! 「trim 無効版」「copy 無効版」に分解して in-process 計測する。
//! バイト同一性は見ない（計測専用。発行バイト列の正しさは既存検証が担う）。
//!
//! 使い方: `render_probe <file.md>`。直列は IF_RENDER_PAR=0。

use ifuto_core::dom::Dom;
use ifuto_core::layout::Layout;
use std::hint::black_box;
use std::time::Instant;

const N: usize = 11;

fn med_ms(mut f: impl FnMut() -> usize) -> (f64, usize) {
    let mut ts = Vec::with_capacity(N);
    let mut keep = 0usize;
    for _ in 0..N {
        let t0 = Instant::now();
        keep = f();
        ts.push(t0.elapsed().as_secs_f64() * 1000.0);
    }
    ts.sort_by(|a, b| a.partial_cmp(b).unwrap());
    (ts[N / 2], keep)
}

/// v1: emit_row の preamble を排した裸の direct 写し（seg 走査 + 隙間 + trim + \n）。
fn direct_v1(dom: &Dom, lay: &Layout) -> Vec<u8> {
    let mx = lay.width.max(1);
    let mut out = Vec::with_capacity(1 << 20);
    for line in &lay.lines {
        let mark = out.len();
        let segs = &lay.seg_arena[line.seg_lo as usize..line.seg_hi as usize];
        let mut pos = 0i32;
        for s in segs {
            if s.x < pos || s.x + s.w > mx {
                continue;
            }
            if s.x > pos {
                let g = (s.x - pos) as usize;
                out.resize(out.len() + g, b' ');
            }
            out.extend_from_slice(lay.seg_text(dom, s));
            pos = s.x + s.w;
        }
        while out.len() > mark && out.last() == Some(&b' ') {
            out.pop();
        }
        out.push(b'\n');
        black_box(mark);
    }
    out
}

/// v2: v1 + 出力を厳密事前確保（再配置 copy の帰属切り分け）。
fn direct_v2(dom: &Dom, lay: &Layout, cap: usize) -> Vec<u8> {
    let mx = lay.width.max(1);
    let mut out = Vec::with_capacity(cap);
    for line in &lay.lines {
        let mark = out.len();
        let segs = &lay.seg_arena[line.seg_lo as usize..line.seg_hi as usize];
        let mut pos = 0i32;
        for s in segs {
            if s.x < pos || s.x + s.w > mx {
                continue;
            }
            if s.x > pos {
                let g = (s.x - pos) as usize;
                out.resize(out.len() + g, b' ');
            }
            out.extend_from_slice(lay.seg_text(dom, s));
            pos = s.x + s.w;
        }
        while out.len() > mark && out.last() == Some(&b' ') {
            out.pop();
        }
        out.push(b'\n');
        black_box(mark);
    }
    out
}

/// v3: v2 から trim を除去（trim ループの帰属）。
fn direct_v3(dom: &Dom, lay: &Layout, cap: usize) -> Vec<u8> {
    let mx = lay.width.max(1);
    let mut out = Vec::with_capacity(cap);
    for line in &lay.lines {
        let segs = &lay.seg_arena[line.seg_lo as usize..line.seg_hi as usize];
        let mut pos = 0i32;
        for s in segs {
            if s.x < pos || s.x + s.w > mx {
                continue;
            }
            if s.x > pos {
                let g = (s.x - pos) as usize;
                out.resize(out.len() + g, b' ');
            }
            out.extend_from_slice(lay.seg_text(dom, s));
            pos = s.x + s.w;
        }
        out.push(b'\n');
    }
    out
}

/// v4: v2 から seg バイト copy を除去（テキスト引き+trim 残し。copy 帰属）。
fn direct_v4(dom: &Dom, lay: &Layout, cap: usize) -> Vec<u8> {
    let mx = lay.width.max(1);
    let mut out = Vec::with_capacity(cap);
    for line in &lay.lines {
        let mark = out.len();
        let segs = &lay.seg_arena[line.seg_lo as usize..line.seg_hi as usize];
        let mut pos = 0i32;
        for s in segs {
            if s.x < pos || s.x + s.w > mx {
                continue;
            }
            if s.x > pos {
                let g = (s.x - pos) as usize;
                out.resize(out.len() + g, b' ');
            }
            let t = lay.seg_text(dom, s);
            black_box(t.as_ptr());
            black_box(t.len());
            pos = s.x + s.w;
        }
        while out.len() > mark && out.last() == Some(&b' ') {
            out.pop();
        }
        out.push(b'\n');
    }
    out
}

fn main() {
    let path = std::env::args().nth(1).unwrap();
    let input = std::fs::read(&path).unwrap();
    let dom = ifuto_core::md::md_to_dom_opts(&input, true)
        .or_else(|| ifuto_core::md::md_to_dom_opts(&input, false))
        .expect("dom");
    let lay = ifuto_core::layout::layout_build_lazy_linear(&dom, 100);
    eprintln!(
        "lines={} deco={} height={} width={}",
        lay.lines.len(),
        lay.deco.len(),
        lay.height,
        lay.width
    );

    // ---- cold 帰属: 初回冷身 sweep の minor fault 計数（12-i） ----
    fn minflt() -> u64 {
        let st = std::fs::read_to_string("/proc/self/stat").unwrap();
        // field 10 (1-based, comm 考慮: 最後の ')' 以後が field 3 始まり)
        let after = st.rsplit(')').next().unwrap();
        let f: Vec<&str> = after.split_whitespace().collect();
        // field10 = f[7] (f[0]=field3 state)
        f[7].parse().unwrap()
    }
    let f0 = minflt();
    let t0 = Instant::now();
    let o_cold = ifuto_core::render::render_emit_sweep(&dom, &lay, false);
    let t_cold = t0.elapsed().as_secs_f64() * 1000.0;
    let f1 = minflt();
    eprintln!(
        "cold sweep: {:.3}ms minflt+={} ({} 4KB pages)",
        t_cold,
        f1 - f0,
        (f1 - f0)
    );
    black_box(o_cold.len());

    let par = std::env::var_os("IF_RENDER_PAR").is_none_or(|v| v != "0");
    let (t_full, out_len) = med_ms(|| {
        let o = ifuto_core::render::render_emit_sweep(&dom, &lay, false);
        black_box(o.len())
    });
    eprintln!(
        "full sweep (par={}): {:.3}ms  out={}B  ({:.1} ns/row)",
        par,
        t_full,
        out_len,
        t_full * 1e6 / lay.height.max(1) as f64
    );
    let nl = lay.lines.len().max(1) as f64;
    let (t1, o1) = med_ms(|| black_box(direct_v1(&dom, &lay).len()));
    eprintln!(
        "v1 bare direct     : {:.3}ms ({:.1} ns/line) out={}",
        t1,
        t1 * 1e6 / nl,
        o1
    );
    let (t2, o2) = med_ms(|| black_box(direct_v2(&dom, &lay, out_len).len()));
    eprintln!(
        "v2 +presized       : {:.3}ms ({:.1} ns/line) out={}",
        t2,
        t2 * 1e6 / nl,
        o2
    );
    let (t3, o3) = med_ms(|| black_box(direct_v3(&dom, &lay, out_len).len()));
    eprintln!(
        "v3 -trim           : {:.3}ms ({:.1} ns/line) out={}",
        t3,
        t3 * 1e6 / nl,
        o3
    );
    let (t4, o4) = med_ms(|| black_box(direct_v4(&dom, &lay, out_len).len()));
    eprintln!(
        "v4 -copy(ptr only) : {:.3}ms ({:.1} ns/line) out={}",
        t4,
        t4 * 1e6 / nl,
        o4
    );

    // ---- W系: 書き出し経路の帰属（12-i: CLI の acc_emit は sweep+write 合算のため） ----
    use std::io::Write as _;
    let out = ifuto_core::render::render_emit_sweep(&dom, &lay, false);
    let (w0, _) = med_ms(|| {
        std::io::stdout().write_all(black_box(&out)).unwrap();
        out.len()
    });
    eprintln!("W0 stdout().write_all: {:.3}ms ({}B)", w0, out.len());
    let (w1, _) = med_ms(|| {
        let mut f = std::fs::File::create("/dev/null").unwrap();
        f.write_all(black_box(&out)).unwrap();
        out.len()
    });
    eprintln!("W1 File(/dev/null) direct: {:.3}ms", w1);
    {
        let mut f = std::fs::File::create("/dev/null").unwrap();
        let (w2, _) = med_ms(|| {
            f.write_all(black_box(&out)).unwrap();
            out.len()
        });
        eprintln!("W2 File(/dev/null) reused: {:.3}ms", w2);
    }
    let (w3, _) = med_ms(|| {
        let o = ifuto_core::render::render_emit_sweep(&dom, &lay, false);
        let mut f = std::fs::File::create("/dev/null").unwrap();
        f.write_all(black_box(&o)).unwrap();
        o.len()
    });
    eprintln!("W3 sweep+/dev/null write : {:.3}ms", w3);
}
