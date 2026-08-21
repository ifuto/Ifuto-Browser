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

/// URL の上限（C の `IF_URL_CAP`）。
pub const URL_CAP: usize = 4096;
/// タイトルの上限（C の `IF_TITLE_CAP`）。
pub const TITLE_CAP: usize = 256;
/// グループ名の上限（C の `IF_GROUP_CAP`）。
pub const GROUP_CAP: usize = 64;
/// 履歴の最大バイト数（C の `IF_HISTORY_MAX_BYTES`）。
pub const HISTORY_MAX_BYTES: usize = 512 * 1024;

/// 保存対象タブ（C の `IfTab` のセッション保存に使うフィールド）。
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct SaveTab {
    /// タブ id。
    pub id: i32,
    /// URL（空 = 空白タブ = 保存対象外）。
    pub url: String,
    /// タイトル（空 = 保存しない）。
    pub title: String,
    /// グループ名（空 = 保存しない）。
    pub group: String,
    /// スクロール位置（>0 のときのみ保存）。
    pub scroll: i32,
}

/// 保存側の無害化（`\t \n \r` → 空白、`maxlen` 打ち切り）。C の `gb_safe` 相当。
fn push_safe(out: &mut Vec<u8>, s: &str, maxlen: usize) {
    for &ch in s.as_bytes().iter().take(maxlen) {
        out.push(if ch == b'\t' || ch == b'\n' || ch == b'\r' {
            b' '
        } else {
            ch
        });
    }
}

/// `session.txt` を生成（C の `if_store_session_save` のシリアライズ部分）。
///
/// 空白タブ（url 空）は保存しない。`active_index` はタブ列の index（-1 = なし）。
/// active は「その index のタブが保存対象ならその id、でなければ先頭の保存対象 id」。
pub fn serialize_session(tabs: &[SaveTab], active_index: i32) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(b"ifuto-session 1\n");
    let mut first_managed: i32 = -1;
    for t in tabs {
        if t.url.is_empty() {
            continue;
        }
        if first_managed < 0 {
            first_managed = t.id;
        }
        out.extend_from_slice(b"url ");
        out.extend_from_slice(t.id.to_string().as_bytes());
        out.push(b' ');
        push_safe(&mut out, &t.url, URL_CAP);
        out.push(b'\n');
        if !t.title.is_empty() {
            out.extend_from_slice(b"title ");
            out.extend_from_slice(t.id.to_string().as_bytes());
            out.push(b' ');
            push_safe(&mut out, &t.title, TITLE_CAP);
            out.push(b'\n');
        }
        if !t.group.is_empty() {
            out.extend_from_slice(b"group ");
            out.extend_from_slice(t.id.to_string().as_bytes());
            out.push(b' ');
            push_safe(&mut out, &t.group, GROUP_CAP);
            out.push(b'\n');
        }
        if t.scroll > 0 {
            out.extend_from_slice(b"scroll ");
            out.extend_from_slice(t.id.to_string().as_bytes());
            out.push(b' ');
            out.extend_from_slice(t.scroll.to_string().as_bytes());
            out.push(b'\n');
        }
    }
    // active: 保存対象の current タブ、無ければ先頭の保存対象
    let cur_id = if active_index >= 0 && (active_index as usize) < tabs.len() {
        let t = &tabs[active_index as usize];
        if !t.url.is_empty() {
            Some(t.id)
        } else {
            None
        }
    } else {
        None
    };
    let active = cur_id.or(if first_managed >= 0 {
        Some(first_managed)
    } else {
        None
    });
    if let Some(id) = active {
        out.extend_from_slice(b"active ");
        out.extend_from_slice(id.to_string().as_bytes());
        out.push(b'\n');
    }
    out.extend_from_slice(b"end\n");
    out
}

/// 履歴の 1 行を生成（C の `if_store_history_add` の行生成部分）。
/// 書式: `epoch \t title \t url \n`（title/url は無害化）。
pub fn history_line(now: i64, title: &str, url: &str) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(now.to_string().as_bytes());
    out.push(b'\t');
    push_safe(&mut out, title, TITLE_CAP);
    out.push(b'\t');
    push_safe(&mut out, url, URL_CAP);
    out.push(b'\n');
    out
}

/// 履歴が最大バイトを超えたときの縮退（後半を残す）。C の縮退ロジック相当。
/// 縮退不要なら `None`。
pub fn shrink_history(text: &[u8]) -> Option<Vec<u8>> {
    if text.len() <= HISTORY_MAX_BYTES {
        return None;
    }
    let keep = text.len() - HISTORY_MAX_BYTES / 2;
    // keep 以降の最初の '\n' の次からを残す
    text[keep..]
        .iter()
        .position(|&c| c == b'\n')
        .map(|nl| text[keep + nl + 1..].to_vec())
}

/// 行 `title<TAB>url[\n]` の URL 部が `url` と一致するか。C の `line_url_eq` 相当。
fn line_url_eq(line: &[u8], url: &[u8]) -> bool {
    let Some(tab) = line.iter().position(|&c| c == b'\t') else {
        return false;
    };
    let mut u = &line[tab + 1..];
    if u.last() == Some(&b'\n') {
        u = &u[..u.len() - 1];
    }
    u == url
}

/// ブックマークのフィルタ + 追加。C の `bmrk_write_filtered` 相当。
///
/// `remove_url`: `Some` なら一致 URL の行を除去。`add`: `Some((title, url))` なら末尾に追記。
pub fn filter_bookmarks(
    text: &[u8],
    remove_url: Option<&str>,
    add: Option<(&str, &str)>,
) -> Vec<u8> {
    let mut out = Vec::new();
    let mut p = text;
    while !p.is_empty() {
        let (line, rest) = match p.iter().position(|&c| c == b'\n') {
            Some(i) => (&p[..i + 1], &p[i + 1..]),
            None => (p, &[][..]),
        };
        p = rest;
        let remove = remove_url.is_some_and(|u| line_url_eq(line, u.as_bytes()));
        if !remove {
            out.extend_from_slice(line);
        }
    }
    if let Some((title, url)) = add {
        push_safe(&mut out, title, TITLE_CAP);
        out.push(b'\t');
        push_safe(&mut out, url, URL_CAP);
        out.push(b'\n');
    }
    out
}

/// ブックマークのトグル。C の `if_store_bookmark_toggle` 相当。
/// 戻り値 `(新しいテキスト, added)`（added = 追加されたか）。
/// url が空なら C 同様に即「変更なし・未追加」（`return false` 相当）を返す。
pub fn toggle_bookmark(text: &[u8], title: &str, url: &str) -> (Vec<u8>, bool) {
    if url.is_empty() {
        return (text.to_vec(), false);
    }
    // 存在判定
    let present = {
        let mut p = text;
        let mut found = false;
        while !p.is_empty() {
            let (line, rest) = match p.iter().position(|&c| c == b'\n') {
                Some(i) => (&p[..i + 1], &p[i + 1..]),
                None => (p, &[][..]),
            };
            p = rest;
            if line_url_eq(line, url.as_bytes()) {
                found = true;
                break;
            }
        }
        found
    };
    if present {
        (filter_bookmarks(text, Some(url), None), false)
    } else {
        (filter_bookmarks(text, None, Some((title, url))), true)
    }
}

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

    // ---- 書き面（シリアライズ）のテスト ----

    #[test]
    fn serialize_session_basic() {
        let tabs = [
            SaveTab {
                id: 1,
                url: "https://a/".into(),
                title: "A".into(),
                group: String::new(),
                scroll: 0,
            },
            SaveTab {
                id: 2,
                url: "https://b/".into(),
                title: String::new(),
                group: "work".into(),
                scroll: 500,
            },
        ];
        let out = serialize_session(&tabs, 1);
        assert_eq!(
            String::from_utf8(out).unwrap(),
            "ifuto-session 1\n\
url 1 https://a/\n\
title 1 A\n\
url 2 https://b/\n\
group 2 work\n\
scroll 2 500\n\
active 2\n\
end\n"
        );
    }

    #[test]
    fn serialize_session_skips_blank() {
        // 空白タブは保存しない。active が空白なら先頭保存対象に落ちる
        let tabs = [
            SaveTab {
                id: 1,
                url: String::new(),
                title: String::new(),
                group: String::new(),
                scroll: 0,
            },
            SaveTab {
                id: 2,
                url: "https://x/".into(),
                title: "X".into(),
                group: String::new(),
                scroll: 0,
            },
        ];
        let out = serialize_session(&tabs, 0);
        assert_eq!(
            String::from_utf8(out).unwrap(),
            "ifuto-session 1\nurl 2 https://x/\ntitle 2 X\nactive 2\nend\n"
        );
    }

    #[test]
    fn serialize_session_sanitizes() {
        let tabs = [SaveTab {
            id: 1,
            url: "a\tb\nc\rd".into(),
            title: "t\nx".into(),
            group: "g\ty".into(),
            scroll: 3,
        }];
        let out = serialize_session(&tabs, 0);
        assert_eq!(
            String::from_utf8(out).unwrap(),
            "ifuto-session 1\nurl 1 a b c d\ntitle 1 t x\ngroup 1 g y\nscroll 1 3\nactive 1\nend\n"
        );
    }

    #[test]
    fn history_line_basic() {
        assert_eq!(history_line(1750000000, "Title", "https://a/"), b"1750000000\tTitle\thttps://a/\n");
        // 無害化
        assert_eq!(history_line(1, "t\tx", "u\nv"), b"1\tt x\tu v\n");
    }

    #[test]
    fn test_toggle_bookmark() {
        let text = b"Google\thttps://google.com\nGitHub\thttps://github.com\n";
        // 追加
        let (new, added) = toggle_bookmark(text, "Ifuto", "https://ifuto.jp");
        assert!(added);
        assert_eq!(
            String::from_utf8(new).unwrap(),
            "Google\thttps://google.com\nGitHub\thttps://github.com\nIfuto\thttps://ifuto.jp\n"
        );
        // 除去
        let (new2, added2) = toggle_bookmark(text, "X", "https://github.com");
        assert!(!added2);
        assert_eq!(String::from_utf8(new2).unwrap(), "Google\thttps://google.com\n");
    }

    #[test]
    fn test_shrink_history() {
        // 縮退不要
        assert_eq!(shrink_history(b"short"), None);
        // 縮退（後半の最初の行境界から）
        let mut big = Vec::new();
        for i in 0..100_000 {
            big.extend_from_slice(format!("{i}\thttps://x/{i}\n").as_bytes());
        }
        let shrunk = shrink_history(&big).unwrap();
        assert!(shrunk.len() <= HISTORY_MAX_BYTES / 2 + 1000);
        // 行境界で切れている（末尾が '\n' または途中行の完全な形）
        assert!(!shrunk.is_empty());
    }
}
