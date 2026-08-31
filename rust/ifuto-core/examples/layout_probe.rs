//! layout 形状計数器（12-f 帰属用）: seg 総数・行数・seg/行分布・layout 段
//! allocs/bytes を出す。seg_arena への二重書き見積り = segs*24B が手術前の
//! 余分な memcpy 量。
use std::alloc::{GlobalAlloc, Layout as ALayout, System};
use std::sync::atomic::{AtomicU64, Ordering};

static NALLOC: AtomicU64 = AtomicU64::new(0);
static NBYTES: AtomicU64 = AtomicU64::new(0);

struct Counting;
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: ALayout) -> *mut u8 {
        NALLOC.fetch_add(1, Ordering::Relaxed);
        NBYTES.fetch_add(l.size() as u64, Ordering::Relaxed);
        unsafe { System.alloc(l) }
    }
    unsafe fn dealloc(&self, p: *mut u8, l: ALayout) {
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

fn main() {
    let path = std::env::args()
        .nth(1)
        .expect("usage: layout_probe <file.md>");
    let input = std::fs::read(path).unwrap();
    let dom = ifuto_core::md::md_to_dom_opts(&input, true).expect("fast dom");
    let t1 = snap();
    let lay = ifuto_core::layout::layout_build_lazy_linear(&dom, 100);
    let t2 = snap();

    let n_lines = lay.lines.len();
    let n_segs = lay.seg_arena.len();
    let mut empty_lines = 0u64;
    let mut max_spl = 0u64;
    for l in &lay.lines {
        let k = (l.seg_hi - l.seg_lo) as u64;
        if k == 0 {
            empty_lines += 1;
        }
        if k > max_spl {
            max_spl = k;
        }
    }
    let syn_bytes = lay.syn_text.len();
    eprintln!(
        "lines={} segs={} empty_lines={} max_segs_per_line={} seg_bytes={} doublewrite_bytes={} syn_bytes={}",
        n_lines,
        n_segs,
        empty_lines,
        max_spl,
        n_segs * 24,
        n_segs * 24,
        syn_bytes
    );
    eprintln!("layout: allocs={} bytes={}", t2.0 - t1.0, t2.1 - t1.1);
}
