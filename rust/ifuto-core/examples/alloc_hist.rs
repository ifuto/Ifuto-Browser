//! 一時診断: 段別のアロケーション計数（コミット対象外の調査器）。
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicU64, Ordering};

static NALLOC: AtomicU64 = AtomicU64::new(0);
static NBYTES: AtomicU64 = AtomicU64::new(0);
static HIST: [AtomicU64; 64] = [const { AtomicU64::new(0) }; 64];

struct Counting;
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        NALLOC.fetch_add(1, Ordering::Relaxed);
        NBYTES.fetch_add(l.size() as u64, Ordering::Relaxed);
        let sz = l.size();
        let b = if sz == 0 {
            0
        } else {
            63 - sz.leading_zeros() as usize
        };
        HIST[b].fetch_add(1, Ordering::Relaxed);
        unsafe { System.alloc(l) }
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        unsafe { System.dealloc(p, l) }
    }
}

#[global_allocator]
static A: Counting = Counting;

fn snap() -> (u64, u64) {
    (
        NALLOC.load(Ordering::Relaxed),
        NBYTES.load(Ordering::Relaxed),
    )
}

fn snap_hist() -> [u64; 64] {
    let mut a = [0u64; 64];
    for (b2, c) in HIST.iter().enumerate() {
        a[b2] = c.load(Ordering::Relaxed);
    }
    a
}

fn phist(label: &str, from: &[u64; 64], to: &[u64; 64]) {
    eprintln!("hist[{label}] (bucket_low = count):");
    let mut base = 1u64;
    for b in 0..64 {
        let d = to[b].saturating_sub(from[b]);
        if d > 0 {
            eprintln!("  {:>9}: {}", base, d);
        }
        base = base.saturating_mul(2);
    }
}

fn main() {
    let path = std::env::args().nth(1).unwrap();
    let input = std::fs::read(path).unwrap();
    let t0 = snap();
    let dom = ifuto_core::md::md_to_dom_opts(&input, true).expect("fast dom");
    let t1 = snap();
    let h1 = snap_hist();
    let lay = ifuto_core::layout::layout_build_lazy_linear(&dom, 100);
    let t2 = snap();
    let h2 = snap_hist();
    phist("layout", &h1, &h2);
    let mut texts = 0u64;
    let mut elems = 0u64;
    let mut na = 0u64;
    let mut nimg = 0u64;
    let mut ntb = 0u64;
    let mut nli = 0u64;
    for n in &dom.nodes {
        match n.kind {
            ifuto_core::dom::NodeKind::Text => texts += 1,
            ifuto_core::dom::NodeKind::Element => {
                elems += 1;
                let nm: &[u8] = &n.name;
                if nm == b"a" {
                    na += 1;
                } else if nm == b"img" {
                    nimg += 1;
                } else if nm == b"table" {
                    ntb += 1;
                } else if nm == b"li" {
                    nli += 1;
                }
            }
            _ => {}
        }
    }
    eprintln!("a={} img={} table={} li={}", na, nimg, ntb, nli);
    eprintln!(
        "elems={} texts={} rlines={} deco={} segs={} syn={} stab={}",
        elems,
        texts,
        lay.lines.len(),
        lay.deco.len(),
        lay.seg_arena.len(),
        lay.syn_text.len(),
        lay.stab.len()
    );
    let out = ifuto_core::render::render_emit_sweep(&dom, &lay, false);
    let t3 = snap();
    eprintln!("nodes={}", dom.nodes.len());
    eprintln!("parse : allocs={} bytes={}", t1.0 - t0.0, t1.1 - t0.1);
    eprintln!("layout: allocs={} bytes={}", t2.0 - t1.0, t2.1 - t1.1);
    eprintln!("render: allocs={} bytes={}", t3.0 - t2.0, t3.1 - t2.1);
    eprintln!("out_bytes={}", out.len());
    eprintln!("size-histogram (2^b..2^(b+1)-1 bytes):");
    let mut base = 1u64;
    for c in HIST.iter() {
        let n = c.load(Ordering::Relaxed);
        if n > 0 {
            eprintln!(
                "  [{:>7}..{:>7}] = {}",
                base,
                (base << 1).saturating_sub(1),
                n
            );
        }
        base = base.saturating_mul(2);
    }
}
