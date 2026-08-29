//! 一時診断: layout 段のアロケーションを呼出元シンボル別に集計する。
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Mutex;

static NALLOC: AtomicU64 = AtomicU64::new(0);
static NBYTES: AtomicU64 = AtomicU64::new(0);
static ON: AtomicBool = AtomicBool::new(false);
static SITES: Mutex<Option<HashMap<String, (u64, u64)>>> = Mutex::new(None);

thread_local! {
    static GUARD: Cell<bool> = const { Cell::new(false) };
}

struct Bt;
unsafe impl GlobalAlloc for Bt {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        NALLOC.fetch_add(1, Ordering::Relaxed);
        NBYTES.fetch_add(l.size() as u64, Ordering::Relaxed);
        if ON.load(Ordering::Relaxed) && l.size() >= 1 {
            let inside = GUARD.with(|g| g.replace(true));
            if !inside {
                let bt = std::backtrace::Backtrace::force_capture();
                let txt = format!("{bt}");
                let mut key = String::with_capacity(256);
                for frame in txt
                    .lines()
                    .filter(|ln| ln.contains("ifuto_core"))
                    .skip(0)
                    .take(9)
                {
                    key.push_str(frame.trim());
                    key.push('\n');
                }
                if let Ok(mut g) = SITES.lock() {
                    let m = g.get_or_insert_with(HashMap::new);
                    let e = m.entry(key).or_insert((0, 0));
                    e.0 += 1;
                    e.1 += l.size() as u64;
                }
                GUARD.with(|g| g.set(false));
            }
        }
        unsafe { System.alloc(l) }
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        unsafe { System.dealloc(p, l) }
    }
}

#[global_allocator]
static A: Bt = Bt;

fn main() {
    let path = std::env::args().nth(1).unwrap();
    let stage = std::env::args().nth(2).unwrap_or_else(|| "layout".into());
    let input = std::fs::read(path).unwrap();
    if stage == "parse" {
        ON.store(true, Ordering::Relaxed);
    }
    let dom = ifuto_core::md::md_to_dom_opts(&input, true).expect("fast dom");
    if stage == "parse" {
        ON.store(false, Ordering::Relaxed);
        eprintln!("nodes={}", dom.nodes.len());
        dump_sites();
        return;
    }
    {
        let mut g = SITES.lock().unwrap();
        *g = Some(HashMap::new());
    }
    ON.store(true, Ordering::Relaxed);
    let lay = ifuto_core::layout::layout_build_lazy_linear(&dom, 100);
    ON.store(false, Ordering::Relaxed);
    eprintln!("rlines={} segs={}", lay.lines.len(), lay.seg_arena.len());
    dump_sites();
}

fn dump_sites() {
    let g = SITES.lock().unwrap();
    let m = g.as_ref().unwrap();
    let mut v: Vec<_> = m.iter().collect();
    v.sort_by_key(|(_, (n, _))| std::cmp::Reverse(*n));
    for (k, (n, b)) in v.iter().take(10) {
        eprintln!("== allocs={} bytes={} ==\n{}", n, b, k);
    }
}
