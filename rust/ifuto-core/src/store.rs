//! 永続ストア層の読み面パーサ（C の `src/store.c` の session / bookmarks 相当）。
//!
//! フォーマットはフラットテキスト。本モジュールは**純粋なテキストパーサ**のみを
//! 扱う（fs 操作は C 同様 `IfFsOps` 注入で分離しており、Rust 側では「テキストを
//! 渡して結果を受け取る」純関数として移植する。所有権により arena が不要になる）。
//!
//! # session.txt（`ifuto-session 1`）
//!
//! - 1 行目: マジック `ifuto-session 1`
//! - 各行: `url <id> <url>`（新タブ）/ `title <id> <t>` / `group <id> <g>` /
//!   `scroll <id> <n>` / `active <id>` / `end`（停止点。残りは捨てる）
//! - 未知行は黙殺（前向き互換）。id は `[0, 1000000]`、scroll は `[0, 1<<24]` に
//!   クランプ。マジック不一致・空は失敗。
//!
//! # bookmarks.tsv
//!
//! 各行 `title<TAB>url`。TAB が無い行は無視。

/// タブ数上限（C の `IF_TABS_MAX`）。
pub const TABS_MAX: usize = 64;

/// 復元タブ（C の `IfSessionTab` 相当。URL/title/group は所有 `String`）。
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct SessionTab {
    /// タブ id。
    pub id: i32,
    /// URL（常に非空）。
    pub url: String,
    /// タイトル（無ければ None）。
    pub title: Option<String>,
    /// グループ（無ければ None）。
    pub group: Option<String>,
    /// スクロール位置（クランプ済み `[0, 1<<24]`）。
    pub scroll: i32,
}

/// 行の `kind` 接頭辞（`"url "` 等）を検証し、`id` と残りを返す。
/// 不一致・id 非数字・id 過大・rest 必須で後続が無い場合は `None`。
fn pre_id<'a>(line: &'a [u8], kind: &[u8]) -> Option<(i32, &'a [u8])> {
    if !line.starts_with(kind) || line.get(kind.len()) != Some(&b' ') {
        return None;
    }
    let p = &line[kind.len() + 1..];
    let mut id: i64 = 0;
    let mut got = false;
    let mut idx = 0;
    while let Some(&c) = p.get(idx) {
        if c.is_ascii_digit() {
            id = id * 10 + (c - b'0') as i64;
            if id > 1_000_000 {
                return None; // id 過大: 異常行
            }
            got = true;
            idx += 1;
        } else {
            break;
        }
    }
    if !got {
        return None;
    }
    if p.get(idx) != Some(&b' ') {
        return None;
    }
    Some((id as i32, &p[idx + 1..]))
}

/// `session.txt` の内容を解析する。戻り値は `(タブ列, active_id)`。
///
/// C の `if_store_session_parse` と完全一致: マジック不一致・空・タブ 0 件は
/// 空タブ列を返す（`n == 0`）。ただし `active_id` はループ中に `active` 行で
/// 更新されるため、タブ 0 件でも「`active` 行だけ読めた」場合は非 -1 になり得る
/// （C と同一の挙動）。
pub fn parse_session(text: &[u8]) -> (Vec<SessionTab>, i32) {
    let mut active_id: i32 = -1;

    // 1 行目: マジック
    let first_nl = match text.iter().position(|&c| c == b'\n') {
        Some(i) => i,
        None => return (Vec::new(), -1),
    };
    if &text[..first_nl] != b"ifuto-session 1" {
        return (Vec::new(), -1);
    }
    let mut p = &text[first_nl + 1..];

    let mut tabs: Vec<SessionTab> = Vec::new();

    while !p.is_empty() {
        let (line, rest) = match p.iter().position(|&c| c == b'\n') {
            Some(i) => (&p[..i], &p[i + 1..]),
            None => (p, &[][..]),
        };
        p = rest;
        if line.is_empty() {
            continue;
        }
        if line == b"end" {
            break; // 構造的停止点。残りは捨てる
        }

        if let Some((id, rest)) = pre_id(line, b"url") {
            if tabs.len() < TABS_MAX {
                tabs.push(SessionTab {
                    id,
                    url: String::from_utf8_lossy(rest).into_owned(),
                    title: None,
                    group: None,
                    scroll: 0,
                });
            }
        } else if let Some((id, rest)) = pre_id(line, b"title") {
            if let Some(t) = tabs.iter_mut().find(|t| t.id == id) {
                t.title = Some(String::from_utf8_lossy(rest).into_owned());
            }
        } else if let Some((id, rest)) = pre_id(line, b"group") {
            if let Some(t) = tabs.iter_mut().find(|t| t.id == id) {
                t.group = Some(String::from_utf8_lossy(rest).into_owned());
            }
        } else if let Some((id, rest)) = pre_id(line, b"scroll") {
            if let Some(t) = tabs.iter_mut().find(|t| t.id == id) {
                let s = String::from_utf8_lossy(rest);
                let v: i64 = s.trim().parse().unwrap_or(0);
                t.scroll = v.clamp(0, 1 << 24) as i32;
            }
        } else if let Some(rest) = line.strip_prefix(b"active ") {
            let s = String::from_utf8_lossy(rest);
            if let Ok(v) = s.trim().parse::<i64>() {
                if v > 0 && v <= 1_000_000 {
                    active_id = v as i32;
                }
            }
        }
        // 未知行は黙殺
    }

    (tabs, active_id)
}

/// `bookmarks.tsv` の内容を解析する。戻り値は `(title, url)` の列（最大 `max` 件）。
pub fn parse_bookmarks(text: &[u8], max: usize) -> Vec<(String, String)> {
    let mut out = Vec::new();
    let mut p = text;
    while !p.is_empty() && out.len() < max {
        let (line, rest) = match p.iter().position(|&c| c == b'\n') {
            Some(i) => (&p[..i], &p[i + 1..]),
            None => (p, &[][..]),
        };
        p = rest;
        if let Some(tab) = line.iter().position(|&c| c == b'\t') {
            let title = String::from_utf8_lossy(&line[..tab]).into_owned();
            let url = String::from_utf8_lossy(&line[tab + 1..]).into_owned();
            out.push((title, url));
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn session_basic() {
        let (tabs, active) = parse_session(
            b"ifuto-session 1\nurl 1 https://a/\ntitle 1 A\nactive 1\nend\n",
        );
        assert_eq!(active, 1);
        assert_eq!(tabs.len(), 1);
        assert_eq!(tabs[0].id, 1);
        assert_eq!(tabs[0].url, "https://a/");
        assert_eq!(tabs[0].title.as_deref(), Some("A"));
        assert_eq!(tabs[0].group, None);
        assert_eq!(tabs[0].scroll, 0);
    }

    #[test]
    fn session_group_and_scroll() {
        let (tabs, _) = parse_session(
            b"ifuto-session 1\nurl 2 https://b/\ngroup 2 work\nscroll 2 500\nend\n",
        );
        assert_eq!(tabs[0].group.as_deref(), Some("work"));
        assert_eq!(tabs[0].scroll, 500);
    }

    #[test]
    fn session_magic_mismatch() {
        assert_eq!(parse_session(b"garbage\nurl 1 x\n").0, Vec::new());
        assert_eq!(parse_session(b"").0, Vec::new());
        assert_eq!(parse_session(b"ifuto-session 2\nurl 1 x\n").0, Vec::new());
    }

    #[test]
    fn session_no_tabs() {
        // タブ 0 件は失敗（C と同じ）
        assert_eq!(parse_session(b"ifuto-session 1\nend\n").0, Vec::new());
    }

    #[test]
    fn session_unknown_lines_ignored() {
        let (tabs, _) = parse_session(
            b"ifuto-session 1\nfutureline foo bar\nurl 1 https://a/\nend\n",
        );
        assert_eq!(tabs.len(), 1);
    }

    #[test]
    fn session_id_overflow_rejected() {
        // id > 1000000 は異常行 → 無視され、タブ 0 件になるので失敗
        assert_eq!(
            parse_session(b"ifuto-session 1\nurl 9999999 https://x/\nend\n").0,
            Vec::new()
        );
        // 有効行と混在する場合は異常行だけ無視される
        let (tabs, _) =
            parse_session(b"ifuto-session 1\nurl 1 https://ok/\nurl 9999999 https://x/\nend\n");
        assert_eq!(tabs.len(), 1);
        assert_eq!(tabs[0].url, "https://ok/");
    }

    #[test]
    fn session_scroll_clamped() {
        let (tabs, _) =
            parse_session(b"ifuto-session 1\nurl 1 https://a/\nscroll 1 999999999\nend\n");
        assert_eq!(tabs[0].scroll, 1 << 24);
    }

    #[test]
    fn session_tabs_max() {
        // 64 タブ上限を超える url 行は無視
        let mut buf = b"ifuto-session 1\n".to_vec();
        for i in 0..(TABS_MAX + 10) {
            buf.extend_from_slice(format!("url {i} https://x/{i}\n").as_bytes());
        }
        buf.extend_from_slice(b"end\n");
        let (tabs, _) = parse_session(&buf);
        assert_eq!(tabs.len(), TABS_MAX);
    }

    #[test]
    fn bookmarks_basic() {
        let b = parse_bookmarks(b"Google\thttps://google.com\nGitHub\thttps://github.com\n", 64);
        assert_eq!(b.len(), 2);
        assert_eq!(b[0], ("Google".to_string(), "https://google.com".to_string()));
        assert_eq!(b[1], ("GitHub".to_string(), "https://github.com".to_string()));
    }

    #[test]
    fn bookmarks_ignores_no_tab() {
        let b = parse_bookmarks(b"no-tab-line\nA\tB\n", 64);
        assert_eq!(b, vec![("A".to_string(), "B".to_string())]);
    }

    #[test]
    fn bookmarks_max_limit() {
        let b = parse_bookmarks(b"a\t1\nb\t2\nc\t3\n", 2);
        assert_eq!(b.len(), 2);
    }

    /// fuzz_store.c の機械不変条件を再現:
    /// - n ∈ {0} ∪ [1, TABS_MAX]
    /// - 各 tab: id ∈ [0, 1e6]、scroll ∈ [0, 1<<24]、url 非空
    /// - active ∈ {-1} ∪ [1, 1e6]
    /// - 決定性（同一入力 2 回で一致）
    #[test]
    fn fuzz_invariants_and_determinism() {
        let cases: &[&[u8]] = &[
            b"ifuto-session 1\nurl 1 https://a/\nend\n",
            b"ifuto-session 1\nurl 1 https://a/\ntitle 1 x\nscroll 1 100\nactive 1\nend\n",
            b"",
            b"garbage",
            b"ifuto-session 1\n",
        ];
        for &src in cases {
            let r1 = parse_session(src);
            let r2 = parse_session(src);
            assert_eq!(r1, r2, "非決定的: {src:?}");
            let (tabs, active) = r1;
            assert!(tabs.len() <= TABS_MAX);
            assert!(active == -1 || (1..=1_000_000).contains(&active));
            for t in &tabs {
                assert!((0..=1_000_000).contains(&t.id));
                assert!((0..=(1 << 24)).contains(&t.scroll));
                assert!(!t.url.is_empty());
            }
        }
    }
}
