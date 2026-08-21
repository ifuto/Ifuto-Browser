//! クロームモデルの純粋関数（C の `src/chrome.c` の検索・照合相当）。
//!
//! | C (chrome.c) | Rust |
//! |---|---|
//! | `ci_contains` | [`ci_contains`] |
//! | `if_chrome_find_tabs` | [`find_tabs`] |
//!
//! # 実装済み
//!
//! 大小無視 ASCII の部分一致（title/url/group 検索）。UTF-8 マルチバイトはバイト比較
//! （C と同じく ASCII のみ小文字化）。
//!
//! # 未移植（状態機械・最終統合）
//!
//! - `tab_load` / `if_chrome_open` / `if_chrome_close` 等: タブ管理の状態機械
//!   （net/tls/script/ext を束ねるオーケストレータ）。最終統合で移植する。
//! - `if_chrome_resolve` / `if_chrome_link_move`: fs 注入・タブ状態に依存。

/// 大小無視 ASCII の部分一致（C の `ci_contains` 相当）。
///
/// `needle` が空、または `hay` より長いなら false。ASCII `A-Z` のみ小文字化して比較
/// （UTF-8 マルチバイトはバイト比較 = C と同一）。
pub fn ci_contains(hay: &[u8], needle: &[u8]) -> bool {
    let n = needle.len();
    let h = hay.len();
    if n == 0 || n > h {
        return false;
    }
    (0..=h - n).any(|i| hay[i..i + n].eq_ignore_ascii_case(needle))
}

/// 検索対象タブ（C の `IfTab` の title/url/group 相当）。
#[derive(Clone, Debug, Default)]
pub struct TabSearch {
    /// タイトル。
    pub title: String,
    /// URL。
    pub url: String,
    /// グループ（`None` = 無し）。
    pub group: Option<String>,
}

/// title/url/group のいずれかに `query` を含むタブの index を返す。C の
/// `if_chrome_find_tabs` 相当（最大 `max` 件、文書順）。
pub fn find_tabs(tabs: &[TabSearch], query: &[u8], max: usize) -> Vec<usize> {
    let mut out = Vec::new();
    if max == 0 || query.is_empty() {
        return out;
    }
    for (i, t) in tabs.iter().enumerate() {
        if out.len() >= max {
            break;
        }
        if ci_contains(t.title.as_bytes(), query)
            || ci_contains(t.url.as_bytes(), query)
            || t
                .group
                .as_deref()
                .is_some_and(|g| ci_contains(g.as_bytes(), query))
        {
            out.push(i);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ci_contains_basic() {
        assert!(ci_contains(b"Alpha", b"alpha"));
        assert!(ci_contains(b"Alpha", b"ALPHA"));
        assert!(ci_contains(b"Alpha", b"ph"));
        assert!(!ci_contains(b"Alpha", b"beta"));
        assert!(!ci_contains(b"Alpha", b""));
        assert!(!ci_contains(b"a", b"ab")); // needle が長い
        // UTF-8 はバイト比較（大文字化は ASCII のみ）
        assert!(ci_contains("日本語".as_bytes(), "日本語".as_bytes()));
        assert!(!ci_contains("日本語".as_bytes(), "日x".as_bytes()));
    }

    #[test]
    fn find_tabs_basic() {
        let tabs = [
            TabSearch { title: "Alpha".into(), url: "/a.html".into(), group: None },
            TabSearch { title: "Beta".into(), url: "/b.html".into(), group: Some("work".into()) },
            TabSearch { title: "Gamma".into(), url: "/c.html".into(), group: None },
        ];
        assert_eq!(find_tabs(&tabs, b"alpha", 16), vec![0]);
        assert_eq!(find_tabs(&tabs, b"ALPHA", 16), vec![0]);
        assert_eq!(find_tabs(&tabs, b"work", 16), vec![1]);
        assert_eq!(find_tabs(&tabs, b"b.html", 16), vec![1]);
        assert_eq!(find_tabs(&tabs, b"zzz", 16), Vec::<usize>::new());
        assert_eq!(find_tabs(&tabs, b"", 16), Vec::<usize>::new());
        // max 制限
        assert_eq!(find_tabs(&tabs, b"a", 1), vec![0]);
        assert_eq!(find_tabs(&tabs, b"a", 0), Vec::<usize>::new());
    }
}
