//! chrome モデル純粋部の Rust 側ダンプドライバ（差分 fuzz 用観測点）。
//!
//! `tools/zz_chrome_dump.c`（C オラクル）と行フォーマットを byte 完全一致させ、
//! `tools/zz_chrome_diff.py` が全行突合する。規約は C 側コメントを参照。
//! （数値は 10 進、文字列は hex。fs.exists は FNV-1a 64bit %4==0 の決定論予言者）
//! `TabSearch` は byte 列保持のため非 UTF-8 入力もそのまま流せる。

use ifuto_core::chrome;
use std::io::{self, BufRead, Write};

/// unhex（C 側と同一規約: 2 文字ずつ、無効文字/奇数端数で打ち切り）
fn unhex(s: &str, out: &mut Vec<u8>) {
    let b = s.as_bytes();
    let mut i = 0;
    while i + 1 < b.len() {
        let hi = (b[i] as char).to_digit(16);
        let lo = (b[i + 1] as char).to_digit(16);
        match (hi, lo) {
            (Some(h), Some(l)) => out.push(((h << 4) | l) as u8),
            _ => break,
        }
        i += 2;
    }
}

fn tohex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

/// C 側 oracle_exists と同一式（FNV-1a 64bit の下位 2bit が 0 のとき存在）
fn oracle_exists(path: &[u8]) -> bool {
    let mut h: u64 = 0xcbf29ce484222325;
    for &p in path {
        h ^= p as u64;
        h = h.wrapping_mul(0x100000001b3);
    }
    (h & 3) == 0
}

fn pi64(t: Option<&str>) -> i64 {
    t.and_then(|s| s.parse::<i64>().ok()).unwrap_or(0)
}

fn main() {
    let stdin = io::stdin();
    let mut out = io::BufWriter::new(io::stdout());

    for line in stdin.lock().lines() {
        let line = line.unwrap();
        let mut it = line.split_whitespace();
        let op = match it.next() {
            Some(o) => o,
            None => continue,
        };
        match op {
            "CI" => {
                let mut h = Vec::new();
                let mut n = Vec::new();
                unhex(it.next().unwrap_or(""), &mut h);
                unhex(it.next().unwrap_or(""), &mut n);
                writeln!(out, "CI {}", chrome::ci_contains(&h, &n) as i32).unwrap();
            }
            // FT <max> <ntab> <query-hex> [t u g]×ntab（g が "-" なら NULL）
            "FT" => {
                let max = pi64(it.next());
                let ntab = pi64(it.next());
                if !(0..=64).contains(&ntab) {
                    // C ドライバと同一の域外拒否
                    writeln!(out, "BAD {line}").unwrap();
                    continue;
                }
                let mut q = Vec::new();
                unhex(it.next().unwrap_or(""), &mut q);
                let mut tabs = Vec::new();
                let mut bad = false;
                for _ in 0..ntab.max(0) {
                    let (mut t, mut u) = (Vec::new(), Vec::new());
                    let g = match (it.next(), it.next(), it.next()) {
                        (Some(th), Some(uh), Some(gh)) => {
                            unhex(th, &mut t);
                            unhex(uh, &mut u);
                            gh
                        }
                        _ => {
                            bad = true;
                            break;
                        }
                    };
                    let group = if g == "-" {
                        None
                    } else {
                        let mut v = Vec::new();
                        unhex(g, &mut v);
                        Some(v)
                    };
                    tabs.push(chrome::TabSearch {
                        title: t,
                        url: u,
                        group,
                    });
                }
                if bad {
                    writeln!(out, "BAD {line}").unwrap();
                    continue;
                }
                let idx = if max <= 0 {
                    Vec::new()
                } else {
                    chrome::find_tabs(&tabs, &q, max as usize)
                };
                let list: Vec<String> = idx.iter().map(|i| i.to_string()).collect();
                // C ドライバと同一書式（"FT " + カンマ区切り。0 件は "FT " のみ）
                writeln!(out, "FT {}", list.join(",")).unwrap();
            }
            "DUP" => {
                let mut s = Vec::new();
                unhex(it.next().unwrap_or(""), &mut s);
                let cap = it.next().unwrap_or("0").parse::<u64>().unwrap_or(0) as u32;
                let r = chrome::dup_cap(&s, cap);
                writeln!(out, "DUP {}", tohex(&r)).unwrap();
            }
            "SCR" => {
                let scroll = pi64(it.next()) as i32;
                let delta = pi64(it.next()) as i32;
                let vh = pi64(it.next()) as i32;
                let doc_h = pi64(it.next()) as i32;
                writeln!(
                    out,
                    "SCR {}",
                    chrome::scroll_apply(scroll, delta, vh, doc_h)
                )
                .unwrap();
            }
            "SCT" => {
                let pos = pi64(it.next()) as i32;
                let vh = pi64(it.next()) as i32;
                let doc_h = pi64(it.next()) as i32;
                writeln!(out, "SCT {}", chrome::scroll_to_apply(pos, vh, doc_h)).unwrap();
            }
            "QUI" => {
                let nt = pi64(it.next()) as i32;
                let armed = pi64(it.next());
                let now = pi64(it.next());
                let (rc, new_armed) = chrome::quit_decide(nt, armed, now);
                writeln!(out, "QUI {} {}", rc as i32, new_armed).unwrap();
            }
            "LNK" => {
                let idx = pi64(it.next()) as i32;
                let delta = pi64(it.next()) as i32;
                let nl = pi64(it.next()) as u32;
                writeln!(out, "LNK {}", chrome::link_move(idx, delta, nl)).unwrap();
            }
            "RES" => {
                let mut input = Vec::new();
                let mut cwd = Vec::new();
                unhex(it.next().unwrap_or(""), &mut input);
                unhex(it.next().unwrap_or(""), &mut cwd);
                let cap = it.next().unwrap_or("0").parse::<u64>().unwrap_or(0) as u32;
                let (rc, written) = chrome::resolve(&input, &cwd, cap, &mut oracle_exists);
                match written {
                    None => writeln!(out, "RES {rc} -").unwrap(),
                    Some(v) => writeln!(out, "RES {} {}", rc, tohex(&v)).unwrap(),
                }
            }
            _ => writeln!(out, "BAD {line}").unwrap(),
        }
        out.flush().unwrap();
    }
}
