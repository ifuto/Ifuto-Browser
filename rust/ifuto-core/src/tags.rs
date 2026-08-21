//! HTML タグ表（C の `src/dom.c` の `IF_TAGS` + 検索プリミティブ相当）。
//!
//! | C (dom.c / dom.h) | Rust |
//! |---|---|
//! | `IfTag` 列挙 / `u16 tag` | [`Tag`]（`u16`） |
//! | `IF_TAGS` 表（name + 手書き長 n + flags） | [`tags_tables::TAGS`]（`(&str, u8)`） |
//! | `if_tag_name(tag)` | [`tag_name`] |
//! | `if_tag_id(name)` | [`tag_id`] |
//! | `if_tag_is_void/rawtext/rcdata` | [`is_void`] / [`is_rawtext`] / [`is_rcdata`] |
//! | `if_dom_tag_table_sane` | （不要。`str.len()` から長さ導出） |
//!
//! # C との違い（手書き長の構造的排除）
//!
//! C の表は name 文字列と「手書きの長さ `n`」を別々に持ち、`if_dom_tag_table_sane`
//! で `strlen(s) == n` を検査する（手書き長さミスを恒久検査）。Rust では
//! `&str` の長さが `str.len()` から自動導出されるため、この検査が不要になる。

use crate::strutil::str_eq_ci;
use crate::tags_tables::{F_RCDATA, F_RAW, F_VOID, TAGS};

/// タグ ID（C の `u16 tag` / `IfTag` 相当。`0` = UNKNOWN）。
pub type Tag = u16;

/// 未知タグ（C の `IF_TAG_UNKNOWN`）。
pub const TAG_UNKNOWN: Tag = 0;

/// タグ総数（`IF_TAG_N_TAGS` 相当）。
pub const N_TAGS: usize = 137;

/// canonical lowercase 名（未知・範囲外は `None`）。C の `if_tag_name` 相当。
pub fn tag_name(tag: Tag) -> Option<&'static str> {
    if tag == TAG_UNKNOWN {
        return None;
    }
    TAGS.get(tag as usize).map(|(n, _)| *n).filter(|n| !n.is_empty())
}

/// 既知タグの ID（未知は [`TAG_UNKNOWN`]）。名前は大文字小文字無視。C の `if_tag_id` 相当。
pub fn tag_id(name: &[u8]) -> Tag {
    for (i, (n, _)) in TAGS.iter().enumerate().skip(1) {
        if str_eq_ci(name, n.as_bytes()) {
            return i as Tag;
        }
    }
    TAG_UNKNOWN
}

/// void 要素か（`<meta>` 等、閉じタグ不要）。C の `if_tag_is_void` 相当。
pub fn is_void(tag: Tag) -> bool {
    tag != TAG_UNKNOWN && TAGS.get(tag as usize).is_some_and(|(_, f)| f & F_VOID != 0)
}

/// rawtext 要素か（`<script>` 等、文字参照を解釈しない）。C の `if_tag_is_rawtext` 相当。
pub fn is_rawtext(tag: Tag) -> bool {
    TAGS.get(tag as usize).is_some_and(|(_, f)| f & F_RAW != 0)
}

/// rcdata 要素か（`<title>` 等）。C の `if_tag_is_rcdata` 相当。
pub fn is_rcdata(tag: Tag) -> bool {
    TAGS.get(tag as usize).is_some_and(|(_, f)| f & F_RCDATA != 0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn table_length_and_first() {
        assert_eq!(TAGS.len(), N_TAGS);
        assert_eq!(TAGS[0], ("", 0)); // UNKNOWN
        assert_eq!(TAGS[1].0, "html");
    }

    #[test]
    fn tag_name_roundtrip() {
        // 既知タグ
        assert_eq!(tag_name(1), Some("html"));
        assert_eq!(tag_name(4), Some("title"));
        // 未知
        assert_eq!(tag_name(TAG_UNKNOWN), None);
        // 範囲外
        assert_eq!(tag_name(9999), None);
    }

    #[test]
    fn tag_id_ci() {
        assert_eq!(tag_id(b"html"), 1);
        assert_eq!(tag_id(b"HTML"), 1);
        assert_eq!(tag_id(b"DiV"), tag_id(b"div"));
        assert_eq!(tag_id(b"unknown-tag"), TAG_UNKNOWN);
        assert_eq!(tag_id(b""), TAG_UNKNOWN);
    }

    #[test]
    fn void_raw_rcdata() {
        // void: meta, br, img, input
        assert!(is_void(tag_id(b"meta")));
        assert!(is_void(tag_id(b"br")));
        assert!(is_void(tag_id(b"img")));
        assert!(is_void(tag_id(b"input")));
        assert!(!is_void(tag_id(b"div")));
        // rawtext: script, style, iframe, xmp
        assert!(is_rawtext(tag_id(b"script")));
        assert!(is_rawtext(tag_id(b"style")));
        assert!(!is_rawtext(tag_id(b"title")));
        // rcdata: title, textarea
        assert!(is_rcdata(tag_id(b"title")));
        assert!(is_rcdata(tag_id(b"textarea")));
        assert!(!is_rcdata(tag_id(b"script")));
    }

    /// 全タグの round-trip と flags の自己整合性を機械証明。
    #[test]
    fn exhaustive_roundtrip() {
        for (i, (n, _f)) in TAGS.iter().enumerate() {
            if i == 0 {
                continue; // UNKNOWN
            }
            let id = tag_id(n.as_bytes());
            assert_eq!(id, i as Tag, "tag {i} ({n}) の tag_id が {id}");
            assert_eq!(tag_name(id), Some(*n));
        }
        // 表の name は全て小文字 canonical（C の「canonical lowercase name」）
        for (n, _f) in TAGS.iter().skip(1) {
            assert!(n.bytes().all(|b| !b.is_ascii_uppercase()), "{n} が小文字でない");
        }
    }

    /// void / rawtext / rcdata の相互排他（1 タグに複数フラグは立たない）。
    #[test]
    fn flags_mutually_exclusive() {
        for (n, f) in TAGS.iter().skip(1) {
            let c = [f & F_VOID != 0, f & F_RAW != 0, f & F_RCDATA != 0]
                .iter()
                .filter(|b| **b)
                .count();
            assert!(c <= 1, "{n} に複数フラグ");
        }
    }
}
