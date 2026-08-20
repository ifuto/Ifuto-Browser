//! ゼロコピー文字列スライス補助（C の `src/strutil.h` 相当）。
//!
//! C 実装の `IfStr`（`{ const char *p; u32 n; }` の 12B スライス）は、Rust では
//! 標準の `&[u8]` スライスにそのまま対応する。ポインタ + 長さの手動管理が不要に
//! なり、境界外アクセス・dangling・NUL 終端の誤仮定が構造的に消える。
//!
//! 本モジュールは C の `strutil.h` の各 `static inline` 補助関数を Rust の
//! フリー関数として移植する（`if_str_*` → `str_*`）。

/// ASCII 小文字化（`A..=Z` → `a..=z`。C の `if_ascii_lower` 相当）。
#[inline]
pub fn ascii_lower(c: u8) -> u8 {
    if c.is_ascii_uppercase() {
        c + 32
    } else {
        c
    }
}

/// スライス等値（C の `if_str_eq` 相当）。空は空と等しい。
#[inline]
pub fn str_eq(a: &[u8], b: &[u8]) -> bool {
    a == b
}

/// ASCII 限定の大文字小文字無視比較（タグ名・属性名・HTTP ヘッダ用。
/// C の `if_str_eq_ci` 相当）。
#[inline]
pub fn str_eq_ci(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    a.iter().zip(b).all(|(x, y)| ascii_lower(*x) == ascii_lower(*y))
}

/// 空白のみか（C の `if_str_is_ws_only` 相当）。
#[inline]
pub fn is_ws_only(s: &[u8]) -> bool {
    s.iter()
        .all(|&c| matches!(c, b' ' | b'\t' | b'\n' | b'\r' | b'\x0c'))
}

/// 部分バイト列検索（C の `if_str_contains` 相当。needle はスライス）。
#[inline]
pub fn contains(hay: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() {
        return true;
    }
    if needle.len() > hay.len() {
        return false;
    }
    hay.windows(needle.len()).any(|w| w == needle)
}

/// 前後空白（space/tab/nl/cr/ff）を除いた部分スライスを返す（C の `if_str_trim` 相当）。
pub fn trim(s: &[u8]) -> &[u8] {
    let mut a = 0;
    let mut b = s.len();
    while a < b && matches!(s[a], b' ' | b'\t' | b'\n' | b'\r' | b'\x0c') {
        a += 1;
    }
    while b > a && matches!(s[b - 1], b' ' | b'\t' | b'\n' | b'\r' | b'\x0c') {
        b -= 1;
    }
    &s[a..b]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ascii_lower_cases() {
        assert_eq!(ascii_lower(b'A'), b'a');
        assert_eq!(ascii_lower(b'Z'), b'z');
        assert_eq!(ascii_lower(b'a'), b'a');
        assert_eq!(ascii_lower(b'0'), b'0');
    }

    #[test]
    fn str_eq_ci_matches() {
        assert!(str_eq_ci(b"Shift_JIS", b"shift_jis"));
        assert!(str_eq_ci(b"", b""));
        assert!(!str_eq_ci(b"abc", b"abd"));
        assert!(!str_eq_ci(b"abc", b"ab"));
    }

    #[test]
    fn contains_matches() {
        assert!(contains(b"hello world", b"o w"));
        assert!(contains(b"abc", b""));
        assert!(!contains(b"abc", b"abcd"));
        assert!(!contains(b"abc", b"z"));
    }

    #[test]
    fn trim_matches() {
        assert_eq!(trim(b"  abc  "), b"abc");
        assert_eq!(trim(b"\t\nx\r"), b"x");
        assert_eq!(trim(b"   "), b"");
    }
}
