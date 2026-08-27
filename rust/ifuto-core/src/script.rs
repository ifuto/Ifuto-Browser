//! `<script>` 実行配線の純粋関数（C の `src/script.c` の style 属性操作相当）。
//!
//! | C (script.c) | Rust |
//! |---|---|
//! | `style_get_prop` | [`style_get_prop`] |
//! | `style_set_prop` | [`style_set_prop`] |
//!
//! # 実装済み
//!
//! `<element>.style` HANDLE（`CSSStyleDeclaration`）の背後の純粋操作:
//! style 属性文字列（`"prop:value;..."`）からのプロパティ値抽出（CI 名前照合・前後
//! 空白 trim）と、プロパティの設定（既存置換 / 追加・空セグメント重複防止）。
//!
//! # 未移植（FFI・最終統合）
//!
//! - `script_console_log` / `doc_*` / `elem_*` / `style_*`（AklHandleVTab の get/set/call）:
//!   akl-ffi（Rust JS エンジン）と DOM をクロスクレート配線する FFI コールバック。
//!   最終統合（chrome 移植時）に `ifuto-ffi` 層として実装する。
//! - `collect_scripts_rec` / `if_script_run`: JS エンジンの eval ループ。

use crate::strutil::str_eq_ci;

/// style 属性文字列から `prop` の値を抽出（`;` 区切り・CI 名前照合）。C の
/// `style_get_prop` 相当。戻り値は前後空白を trim した値（無ければ `None`）。
pub fn style_get_prop<'a>(style_attr: &'a [u8], prop: &[u8]) -> Option<&'a [u8]> {
    let mut i = 0usize;
    while i < style_attr.len() {
        // セグメント先頭の `;` / 空白を飛ばす
        while i < style_attr.len()
            && (style_attr[i] == b';' || style_attr[i] == b' ' || style_attr[i] == b'\t')
        {
            i += 1;
        }
        let ns = i;
        while i < style_attr.len() && style_attr[i] != b':' {
            i += 1;
        }
        if i >= style_attr.len() {
            break;
        }
        let ne = i;
        // 名前 CI 照合
        if &style_attr[ns..ne] == prop || str_eq_ci(&style_attr[ns..ne], prop) {
            i += 1; // ':' の後
            while i < style_attr.len() && (style_attr[i] == b' ' || style_attr[i] == b'\t') {
                i += 1;
            }
            let vs = i;
            while i < style_attr.len() && style_attr[i] != b';' {
                i += 1;
            }
            let mut ve = i;
            while ve > vs && (style_attr[ve - 1] == b' ' || style_attr[ve - 1] == b'\t') {
                ve -= 1;
            }
            return Some(&style_attr[vs..ve]);
        }
        // このセグメントを捨てる
        while i < style_attr.len() && style_attr[i] != b';' {
            i += 1;
        }
        if i < style_attr.len() {
            i += 1;
        }
    }
    None
}

/// style 属性に `prop: value` を設定（既存は置換・無ければ追加）。C の
/// `style_set_prop` 相当。戻り値は新しい style 属性文字列。
///
/// C のバッファ操作（`bl + seg_len + 1 <= cur.n` の保持条件・`i <= cur.n && bl < cur.n`
/// の `;` 付与・末尾 `;;` の折り畳み）を忠実に再現する。
pub fn style_set_prop(style_attr: &[u8], prop: &[u8], value: &[u8]) -> Vec<u8> {
    let cur = style_attr;
    let pl = prop.len();
    let mut buf: Vec<u8> = Vec::with_capacity(cur.len() + pl + value.len() + 2);
    let mut i = 0usize;
    while i < cur.len() {
        let seg_start = i;
        while i < cur.len() && cur[i] != b';' {
            i += 1;
        }
        let seg_end = i;
        if i < cur.len() {
            i += 1; // ';' を跨ぐ
        }
        // セグメント内の名前部を CI 照合
        let mut ns = seg_start;
        while ns < seg_end && (cur[ns] == b' ' || cur[ns] == b'\t') {
            ns += 1;
        }
        let mut ne = ns;
        while ne < seg_end && cur[ne] != b':' {
            ne += 1;
        }
        if ne - ns == pl && str_eq_ci(&cur[ns..ne], prop) {
            continue; // このセグメントは捨てる（置換）
        }
        // 保持（C のバッファ境界条件を再現）
        #[allow(clippy::int_plus_one)]
        if buf.len() + seg_end - seg_start + 1 <= cur.len() {
            buf.extend_from_slice(&cur[seg_start..seg_end]);
            if i <= cur.len() && buf.len() < cur.len() {
                buf.push(b';');
            }
        }
    }
    // 新しい prop: value を追記
    if !buf.is_empty() && buf[buf.len() - 1] != b';' {
        buf.push(b';');
    }
    if buf.len() >= 2 && buf[buf.len() - 1] == b';' && buf[buf.len() - 2] == b';' {
        buf.pop(); // 空セグメント重複防止
    }
    buf.extend_from_slice(prop);
    buf.push(b':');
    buf.extend_from_slice(value);
    buf
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn get_prop_basic() {
        assert_eq!(
            style_get_prop(b"color: blue;", b"color"),
            Some(&b"blue"[..])
        );
        assert_eq!(style_get_prop(b"color:blue", b"color"), Some(&b"blue"[..]));
        // CI
        assert_eq!(style_get_prop(b"COLOR: red", b"color"), Some(&b"red"[..]));
        // 複数 prop から該当を抽出（名前と ':' が密着した形のみ）
        assert_eq!(
            style_get_prop(b"margin:1px;color:green;padding:2px", b"color"),
            Some(&b"green"[..])
        );
        // 名前部の後ろの空白は trim しない（C の quirk: "color : green" は不一致）
        assert_eq!(style_get_prop(b"color : green", b"color"), None);
        // 無い
        assert_eq!(style_get_prop(b"color: blue;", b"background"), None);
        // 空 style
        assert_eq!(style_get_prop(b"", b"color"), None);
    }

    #[test]
    fn set_prop_basic() {
        // 空 style → 追加
        assert_eq!(style_set_prop(b"", b"color", b"blue"), b"color:blue");
        // 既存置換（C は末尾 ';' を付けない）
        assert_eq!(
            style_set_prop(b"color:red;", b"color", b"blue"),
            b"color:blue"
        );
        assert_eq!(
            style_set_prop(b"color:red", b"color", b"blue"),
            b"color:blue"
        );
        // CI 置換
        assert_eq!(
            style_set_prop(b"COLOR:red;", b"color", b"blue"),
            b"color:blue"
        );
        // 複数 prop の一部を置換（保持セグメントの ';' は残る）
        assert_eq!(
            style_set_prop(b"margin:1px;color:red;padding:2px;", b"color", b"blue"),
            b"margin:1px;padding:2px;color:blue"
        );
    }

    #[test]
    fn set_prop_matches_c_quirks() {
        // 空セグメントの重複防止（`;;` → `;`）
        let out = style_set_prop(b"a:1;;", b"color", b"blue");
        assert!(
            !out.windows(2).any(|w| w == b";;"),
            "got: {:?}",
            String::from_utf8_lossy(&out)
        );
    }
}
