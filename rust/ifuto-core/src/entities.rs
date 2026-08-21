//! HTML 名前付き文字参照（named character references）の解決（C の `src/html_tok.c`
//! の `if_named_ref` が使う検索プリミティブ + `entities_gen.h` 相当）。
//!
//! WHATWG named character references の表（`tools/gen_entities.py` が CPython
//! `html.entities` = WHATWG entities.json から生成）に対する純粋な検索関数。
//!
//! # C との対応
//!
//! | C (entities_gen.h / html_tok.c) | Rust |
//! |---|---|
//! | `IF_ENT_NAMED` / `IF_ENT_NAMES` / `IF_ENT_AUX32` | [`entities_tables`] |
//! | `ent_cp1` / `ent_cp2` | [`Entry::codepoints`] |
//! | `if_ent_find(s, n)` | [`find`] |
//! | `if_ent_longest_legacy(s, s_len)` | [`longest_legacy`] |
//!
//! # C との違い（境界検査の構造化）
//!
//! C は `name_off` を `IF_ENT_NAMES` blob へオフセットして `memcmp` する（範囲外
//! アクセスは生成時のオフセット正しさに依存）。Rust では [`NAMES`] を `&[u8]`
//! スライスとして `get` で安全に参照する。

use crate::entities_tables::{AUX32, NAMED, NAMES};

/// 1 エントリの参照（`(name_off, name_len, flags, cp1, cp2)`）。
type Raw = (u16, u8, u8, u16, u16);

/// flags ビット。
const FLAG_SEMI: u8 = 1; // 正式形（';' あり）
const FLAG_LEGACY: u8 = 2; // legacy（裸）

/// 名前を参照（`NAMES` の `off..off+len`）。
fn name_of(e: &Raw) -> &[u8] {
    let (off, len, ..) = *e;
    let off = off as usize;
    let len = len as usize;
    NAMES.get(off..off + len).unwrap_or(&[])
}

/// エントリのコードポイント列（1 または 2 個）を返す。
/// - `cp2 == 0`: 単一 cp（`cp1`）
/// - `cp1 == 0xFFFF`: astral 単一 cp（本体は `AUX32[cp2]`）
/// - それ以外: 2 cp（両方 BMP）
pub fn codepoints(e: &Raw) -> Vec<u32> {
    let (_off, _len, _flags, cp1, cp2) = *e;
    if cp1 == 0xFFFF {
        // astral 単一 cp
        let idx = cp2 as usize;
        let astral = AUX32.get(idx).copied().unwrap_or(0);
        vec![astral]
    } else if cp2 == 0 {
        vec![cp1 as u32]
    } else {
        vec![cp1 as u32, cp2 as u32]
    }
}

/// 完全一致探索: 返り値は index or `None`（C の `if_ent_find` 相当）。
pub fn find(s: &[u8]) -> Option<usize> {
    let mut lo = 0usize;
    let mut hi = NAMED.len();
    while lo < hi {
        let mid = (lo + hi) / 2;
        let e = &NAMED[mid];
        let nm = name_of(e);
        // 比較: 短い方を memcmp し、長さで tie-break
        let m = nm.len().min(s.len());
        let c = nm[..m].cmp(&s[..m]);
        let c = if c == std::cmp::Ordering::Equal {
            nm.len().cmp(&s.len())
        } else {
            c
        };
        match c {
            std::cmp::Ordering::Less => lo = mid + 1,
            std::cmp::Ordering::Greater => hi = mid,
            std::cmp::Ordering::Equal => return Some(mid),
        }
    }
    None
}

/// legacy 用: `s` 前方の legacy 許容エントリで最長名前を探す。
/// 同一 prefix 群はソート上隣接するので、挿入点近傍のみ走査する（C の
/// `if_ent_longest_legacy` 相当）。
pub fn longest_legacy(s: &[u8]) -> Option<usize> {
    if s.is_empty() {
        return None;
    }
    let first = s[0];
    // 第 1 文字が first 以上の最初の位置を二分探索
    let mut lo = 0usize;
    let mut hi = NAMED.len();
    while lo < hi {
        let mid = (lo + hi) / 2;
        let nm = name_of(&NAMED[mid]);
        if nm.first().copied().unwrap_or(0) < first {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    // 第 1 文字が等しい区間だけ走査
    let mut best: Option<usize> = None;
    let mut best_len = 0usize;
    for (i, e) in NAMED.iter().enumerate().skip(lo) {
        let nm = name_of(e);
        if nm.first().copied().unwrap_or(0) != first {
            break;
        }
        let (_off, len, flags, _cp1, _cp2) = *e;
        let len = len as usize;
        if (flags & FLAG_LEGACY) != 0 && len <= s.len() && nm == &s[..len] && len > best_len {
            best = Some(i);
            best_len = len;
        }
    }
    best
}

/// 名前に対応するエントリ（完全一致 or legacy 最長）を引いてコードポイント列を返す。
/// 見つからなければ `None`。`s` は `&...;` の `&` と `;` の間の alnum ラン。
pub fn resolve(s: &[u8]) -> Option<Vec<u32>> {
    // 1) 正式形（';' あり完全一致）
    if let Some(i) = find(s) {
        let e = &NAMED[i];
        if (e.2 & FLAG_SEMI) != 0 {
            return Some(codepoints(e));
        }
    }
    // 2) legacy（最長 prefix）
    if let Some(i) = longest_legacy(s) {
        return Some(codepoints(&NAMED[i]));
    }
    None
}

/// エントリ index の flags（bit0 = formal ';' あり、bit1 = legacy 裸）。
/// 範囲外は 0。
pub fn entry_flags(i: usize) -> u8 {
    NAMED.get(i).map(|e| e.2).unwrap_or(0)
}

/// エントリ index の名前長。範囲外は 0。
pub fn entry_name_len(i: usize) -> u8 {
    NAMED.get(i).map(|e| e.1).unwrap_or(0)
}

/// エントリ index のコードポイント列。範囲外は空。
pub fn entry_codepoints(i: usize) -> Vec<u32> {
    NAMED.get(i).map(codepoints).unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 正式形エントリを名前から直接引く（テスト用）。
    fn find_by_name(name: &str) -> Option<Vec<u32>> {
        let i = find(name.as_bytes())?;
        Some(codepoints(&NAMED[i]))
    }

    #[test]
    fn single_bmp() {
        // &amp; → U+0026（正式形）
        assert_eq!(find_by_name("amp"), Some(vec![0x26]));
        // &AElig; → U+00C6
        assert_eq!(find_by_name("AElig"), Some(vec![0xC6]));
        // &lt; → U+003C
        assert_eq!(find_by_name("lt"), Some(vec![0x3C]));
    }

    #[test]
    fn single_astral() {
        // &Afr; は astral（U+1D504）。cp1==0xFFFF 経由
        let i = find(b"Afr").unwrap();
        let e = &NAMED[i];
        assert_eq!(e.3, 0xFFFF);
        assert_eq!(codepoints(e), vec![0x1D504]);
    }

    #[test]
    fn two_codepoints() {
        // &NotEqualTilde; → U+2242 U+0338（2 cp）
        let i = find(b"NotEqualTilde").unwrap();
        let e = &NAMED[i];
        assert_eq!(e.4, 0x338); // cp2 非 0 = 2 cp
        let cps = codepoints(e);
        assert_eq!(cps.len(), 2);
    }

    #[test]
    fn find_missing() {
        assert!(find(b"not_an_entity").is_none());
        assert!(find(b"zzzz").is_none());
    }

    #[test]
    fn legacy_longest() {
        // &amp は legacy（';' 無し）でも &amp → amp
        assert_eq!(longest_legacy(b"amp"), Some(find(b"amp").unwrap()));
        // "copy" は legacy で &copy; の正式形。裸 "copy" は copy に解決
        let i = longest_legacy(b"copy").unwrap();
        assert_eq!(codepoints(&NAMED[i]), vec![0xA9]);
    }

    #[test]
    fn resolve_formal_then_legacy() {
        // 正式形（';' あり）が優先
        assert_eq!(resolve(b"amp"), Some(vec![0x26]));
        // 未知 → None
        assert_eq!(resolve(b"zzz"), None);
    }

    /// 全エントリ（2125）に対する健全性 + 自己一貫性の機械証明。
    #[test]
    fn exhaustive_roundtrip() {
        let n = NAMED.len();
        assert_eq!(n, 2125, "エントリ数が生成時と乖離");
        // 全エントリ名で find が自分自身を返す（ソート順の一貫性）
        for i in 0..n {
            let nm = name_of(&NAMED[i]);
            let found = find(nm).expect("自分自身を find できない");
            assert_eq!(found, i, "エントリ {i} ({nm:?}) の find が {found} を返した");
        }
        // 全エントリのコードポイント列が 1..=2 個かつ非ゼロ
        for i in 0..n {
            let cps = codepoints(&NAMED[i]);
            assert!(cps.len() == 1 || cps.len() == 2, "エントリ {i} が {cps:?}");
            assert!(cps.iter().all(|&c| c > 0));
        }
    }

    /// 表の不変条件（生成時 assert の再現）:
    /// - 2-cp エントリは両方 BMP（cp1 != 0xFFFF かつ cp2 != 0）
    /// - astral は単一 cp（cp1 == 0xFFFF、cp2 は AUX32 index）
    /// - 単一 BMP は cp2 == 0
    #[test]
    fn astral_and_twocp_disjoint() {
        for e in NAMED {
            let (_o, _l, _f, cp1, cp2) = *e;
            if cp1 == 0xFFFF {
                // astral 単一 cp: cp2 は AUX32 index（範囲内）
                assert!((cp2 as usize) < AUX32.len());
            } else if cp2 != 0 {
                // 2-cp: 両方 BMP（cp1 はここで BMP 確定、cp2 も BMP）
                assert!(cp1 <= 0xFFFF && cp2 <= 0xFFFF);
            }
        }
    }
}
