//! 一時診断: 段別のアロケーション計数（コミット対象外の調査器）。
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicU64, Ordering};

static NALLOC: AtomicU64 = AtomicU64::new(0);
static NBYTES: AtomicU64 = AtomicU64::new(0);

struct Counting;
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        NALLOC.fetch_add(1, Ordering::Relaxed);
        NBYTES.fetch_add(l.size() as u64, Ordering::Relaxed);
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

fn main() {
    let path = std::env::args().nth(1).unwrap();
    let input = std::fs::read(path).unwrap();
    let t0 = snap();
    let dom = ifuto_core::md::md_to_dom_opts(&input, true).expect("fast dom");
    let t1 = snap();
    let lay = ifuto_core::layout::layout_build_lazy_linear(&dom, 100);
    let t2 = snap();
    let out = ifuto_core::render::render_emit_sweep(&dom, &lay, false);
    let t3 = snap();
    eprintln!("nodes={}", dom.nodes.len());
    eprintln!("parse : allocs={} bytes={}", t1.0 - t0.0, t1.1 - t0.1);
    eprintln!("layout: allocs={} bytes={}", t2.0 - t1.0, t2.1 - t1.1);
    eprintln!("render: allocs={} bytes={}", t3.0 - t2.0, t3.1 - t2.1);
    eprintln!("out_bytes={}", out.len());
}
