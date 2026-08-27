//! クロームモデルの純粋関数（C の `src/chrome.c` 相当）。
//!
//! | C (chrome.c) | Rust |
//! |---|---|
//! | `ci_contains` | [`ci_contains`] |
//! | `if_chrome_find_tabs` | [`find_tabs`] |
//! | `if_chrome_resolve` | [`resolve`] |
//! | `if_chrome_scroll`（lay 存在経路） | [`scroll_apply`] |
//! | `if_chrome_scroll_to`（同） | [`scroll_to_apply`] |
//! | `if_chrome_link_move`（同） | [`link_move`] |
//! | `if_chrome_quit`（判定部） | [`quit_decide`] |
//! | `dup_cap` | [`dup_cap`] |
//!
//! # 実装済み
//!
//! 大小無視 ASCII の部分一致（title/url/group 検索）。UTF-8 マルチバイトはバイト比較
//! （C と同じく ASCII のみ小文字化）。パス解決（ws は ' ' と '\t' のみ・`"://"` は
//! 位置 2–8 のみスキーム扱い・絶対パスは cap 検査を先に、cwd 相対は exists を先に
//! 行う非対称・`snprintf` 4095 byte 截断の全 quirk を写し）。scroll clamp /
//! link 巡回 / quit 2 連打判定。`if_chrome_scroll` 等の `!t || !t->lay` ガードは
//! 呼び出し側の責務のため、Rust 側は「lay 存在経路」の純粋核のみを出す
//! （`t->scroll`/`t->doc_h`/`t->lay->n_links` を引数に取る形）。
//!
//! # 未移植（状態機械・最終統合）
//!
//! - `tab_load` / `if_chrome_open` / `if_chrome_close` / `if_chrome_switch` 等:
//!   タブ管理の状態機械（net/tls/script/ext を束ねるオーケストレータ）。
//!   最終統合（GUI フェーズ）で移植する。
//! - `if_chrome_cur_doc_bytes`: C arena 会計概念のため対象外。
//! - `autosave` / `if_chrome_restore` / `bookmark_cur` / `set_group`:
//!   store（fs 永続層）と結合した副作用系。store.rs 移植済み部品で統合時に組む。

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
///
/// フィールドは byte 列（C の char* そのまま。title/url は表示用に UTF-8 の
/// はずだが、検索は byte 比較のため UTF-8 妥当性を要求しない —
/// `unsafe` なしに非 UTF-8 入力も差分 fuzz の対象にできる）。
#[derive(Clone, Debug, Default)]
pub struct TabSearch {
    /// タイトル。
    pub title: Vec<u8>,
    /// URL。
    pub url: Vec<u8>,
    /// グループ（`None` = 無し）。
    pub group: Option<Vec<u8>>,
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
        if ci_contains(&t.title, query)
            || ci_contains(&t.url, query)
            || t.group.as_deref().is_some_and(|g| ci_contains(g, query))
        {
            out.push(i);
        }
    }
    out
}

/// `scroll` クランプ（C の `if_chrome_scroll` の lay 存在経路）。
///
/// `maxs = doc_h > vh ? doc_h - vh : 0`、加算後に `[0, maxs]` へ clamp。
/// `scroll + delta` の i32 オーバーフローは C では UB — Rust は `wrapping_add`
/// （実効 C ビルドと同値、パニック回避）。fuzz の信頼域は和が i32 に収まる範囲。
pub fn scroll_apply(scroll: i32, delta: i32, vh: i32, doc_h: i32) -> i32 {
    let maxs = if doc_h > vh {
        doc_h.wrapping_sub(vh)
    } else {
        0
    };
    // C は 2 段 clamp を逐次適用する（<0 → 0 の後 >maxs を再評価）。
    // wrap 域で maxs が負になると、`0 > maxs` で「負の scroll」が残る —
    // 早期 return で捌くと maxs への引き戻しを落とすため逐次形が必須（fuzz 検出）
    let mut s = scroll.wrapping_add(delta);
    if s < 0 {
        s = 0;
    }
    if s > maxs {
        s = maxs;
    }
    s
}

/// 絶対位置 `scroll` 設定（C の `if_chrome_scroll_to` の lay 存在経路）。
pub fn scroll_to_apply(pos: i32, vh: i32, doc_h: i32) -> i32 {
    let maxs = if doc_h > vh {
        doc_h.wrapping_sub(vh)
    } else {
        0
    };
    // scroll とは非対称: C の scroll_to は単一三項（pos<0 → 0 で maxs 再評価
    // なし）。こちらは負 maxs 域でも 0 が残る
    if pos < 0 {
        0
    } else if pos > maxs {
        maxs
    } else {
        pos
    }
}

/// quit 判定（C の `if_chrome_quit` の判定部）。
///
/// 戻り値 `(終了するか, 新しい quit_armed_at)`。タブ 1 枚以下なら無条件終了、
/// 複数なら 3 秒以内 2 連打のみ（`armed_at >= 0 && now - armed_at <= 3`）。
/// quirk: 時計逆行（now < armed_at）は負差で `<= 3` 成立 → 終了扱い（写し）。
/// 判定 false のときだけ `armed_at = now` + toast（toast 設定は副作用のため
/// 本関数はモデル外 — 呼び出し側が判定値で行う）。
pub fn quit_decide(n_tabs: i32, armed_at: i64, now: i64) -> (bool, i64) {
    if n_tabs <= 1 {
        return (true, armed_at);
    }
    if armed_at >= 0 && now.wrapping_sub(armed_at) <= 3 {
        return (true, armed_at);
    }
    (false, now)
}

/// リンク巡回（C の `if_chrome_link_move` の lay 存在経路）。
///
/// `n_links == 0` なら -1（C は `link_idx = -1` にもする）。`idx < 0` からは
/// `delta >= 0` で 0、負で `n - 1` に入る。負の剰余は C の切り捨て `%` と同値
/// （Rust の `%` も被除数符号に従う）。`idx + delta` の i32 溢れは `wrapping_add`
/// （C では UB）。
pub fn link_move(idx: i32, delta: i32, n_links: u32) -> i32 {
    if n_links == 0 {
        return -1;
    }
    // C は `(i32)n_links`（u32>2^31-1 は縮退 wrap。gcc 実効と同値）
    let n = n_links as i32;
    let mut i = idx;
    if i < 0 {
        i = if delta >= 0 { 0 } else { n.wrapping_sub(1) };
    } else {
        // wrapping_rem はINT_MIN % -1 == 0 をパニックなしで返す
        // （C 側は x86 SIGFPE の UB をガードで 0 に潰したものと同値）
        i = i.wrapping_add(delta).wrapping_rem(n);
    }
    if i < 0 {
        i = i.wrapping_add(n);
    }
    i
}

/// cap 付き複製の内容部（C の `dup_cap`）。`s[..min(strlen, cap-1)]` を返す。
///
/// 契約: `cap >= 1`。C は `cap == 0` で `n >= cap` 成立 → `cap - 1` が u32
/// underflow し 4GiB alloc + memcpy に進む即死領域（再現しない。差分 fuzz も
/// cap >= 1 のみ生成する）。
pub fn dup_cap(s: &[u8], cap: u32) -> Vec<u8> {
    assert!(
        cap >= 1,
        "dup_cap: cap=0 は C の u32 underflow 領域（信頼域外）"
    );
    let cap = cap as usize;
    let n = s.len();
    let m = if n >= cap { cap - 1 } else { n };
    s[..m].to_vec()
}

/// ws スキップ（C の `skip_ws`）。`' '`（0x20）と `'\t'`（0x09）のみ。
/// `'\n'` は捨てない（C 実装の写し — isspace 系ではない点に注意）。
fn skip_ws(s: &[u8]) -> &[u8] {
    let mut i = 0;
    while i < s.len() && (s[i] == b' ' || s[i] == b'\t') {
        i += 1;
    }
    &s[i..]
}

/// C `strstr(input, "://")` の最初の出現位置。
fn first_scheme(input: &[u8]) -> Option<usize> {
    input.windows(3).position(|w| w == b"://")
}

/// resolve の結果。`out` が書かれた場合はその内容（NUL 手前の実効バイト列）、
/// 書かれなかった経路（空入力 / スキーム / cap 超過 / 不一致）は `None`。
/// C ハーネスは out バッファにセンチネルを敷いて「書かれたか」を区別する。
pub type ResolveOut = Option<Vec<u8>>;

/// resolve 判定（C の `if_chrome_resolve` 完全形。fs.exists は注入）。
///
/// C との quirk 対応表:
/// - ws は `' '`/`'\t'` のみ先行除去（`'\n'` は残る → 空判定は除去後）。
/// - `"://"` 最初出現が位置 2..=8 なら rc=1（out 未書き。位置 0/1 や 9 以降は
///   スキーム扱いしない → パス判定へ落ちる）。
/// - 絶対パス: `n >= cap` を **exists より先に** 検査して rc=2。cap 通過時は
///   `memcpy(out, input, n+1)` を **exists 判定前に** 行うため、rc=2 でも
///   out は書き換わっている（探したが無かった、の証跡）。
/// - cwd 相対: `snprintf(cand, 4096, "%s/%s")` = 連結を **4095 byte で截断**
///   （UTF-8 境界は無関係の byte 截断）。存在する場合のみ `n >= cap` を検査
///   （絶対経路と順序が非対称 — quirk の写し）。
pub fn resolve(
    input: &[u8],
    cwd: &[u8],
    cap: u32,
    exists: &mut dyn FnMut(&[u8]) -> bool,
) -> (i32, ResolveOut) {
    let input = skip_ws(input);
    if input.is_empty() {
        return (2, None);
    }
    if let Some(p) = first_scheme(input) {
        if (2..=8).contains(&p) {
            return (1, None);
        }
    }
    let cap = cap as usize;
    if input[0] == b'/' {
        if input.len() >= cap {
            return (2, None);
        }
        // C は memcpy(out,…) してから exists(out,…) — out は rc=2 でも書き込み済み
        return if exists(input) {
            (0, Some(input.to_vec()))
        } else {
            (2, Some(input.to_vec()))
        };
    }
    // snprintf "%s/%s" の 4095 byte 截断を忠実に再現
    let mut cand = Vec::with_capacity(cwd.len() + 1 + input.len());
    cand.extend_from_slice(cwd);
    cand.push(b'/');
    cand.extend_from_slice(input);
    cand.truncate(4095);
    if exists(&cand) {
        if cand.len() >= cap {
            return (2, None);
        }
        return (0, Some(cand));
    }
    (2, None)
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
            TabSearch {
                title: b"Alpha".to_vec(),
                url: b"/a.html".to_vec(),
                group: None,
            },
            TabSearch {
                title: b"Beta".to_vec(),
                url: b"/b.html".to_vec(),
                group: Some(b"work".to_vec()),
            },
            TabSearch {
                title: b"Gamma".to_vec(),
                url: b"/c.html".to_vec(),
                group: None,
            },
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

    #[test]
    fn scroll_quirks() {
        // maxs = doc_h - vh（doc_h <= vh なら 0）
        assert_eq!(scroll_apply(10, 5, 20, 100), 15);
        assert_eq!(scroll_apply(95, 99, 20, 100), 80); // 上限 clamp
        assert_eq!(scroll_apply(0, -99, 20, 100), 0); // 下限 clamp
        assert_eq!(scroll_apply(0, 99, 200, 100), 0); // doc_h<=vh → maxs=0
        assert_eq!(scroll_apply(5, 99, 20, 20), 0); // doc_h==vh → maxs=0
                                                    // doc_h が負でも doc_h > vh 評価は符号付き i32 のまま
        assert_eq!(scroll_apply(3, 5, -1, -5), 0); // -5 > -1 は偽 → maxs=0
        assert_eq!(scroll_apply(0, 2, -5, -1), 2); // doc_h=-1 > vh=-5 → maxs=4 → 2 がそのまま
                                                   // scroll_to
        assert_eq!(scroll_to_apply(-3, 20, 100), 0);
        assert_eq!(scroll_to_apply(200, 20, 100), 80);
        assert_eq!(scroll_to_apply(50, 20, 100), 50);
        // wrap 域の負 maxs quirk（fuzz 検出: scroll は 2 段 clamp で負のまま
        // 残り、scroll_to 三項は pos<0 のみ 0 固定）
        assert_eq!(scroll_apply(0, -1, -64, i32::MAX - 1), -2147483586);
        assert_eq!(scroll_to_apply(0, -64, i32::MAX - 1), -2147483586);
        assert_eq!(scroll_to_apply(-5, -64, i32::MAX - 1), 0);
        // scroll の 2 段は pos<0 でも maxs 側に降りる
        assert_eq!(scroll_apply(-5, 0, -64, i32::MAX - 1), -2147483586);
    }

    #[test]
    fn quit_quirks() {
        // タブ 1 枚以下は無条件終了
        assert_eq!(quit_decide(0, -1, 100), (true, -1));
        assert_eq!(quit_decide(1, -1, 100), (true, -1));
        // 複数: 1 打目は arm のみ
        assert_eq!(quit_decide(2, -1, 100), (false, 100));
        // 2 連打（3 秒以内）で終了。境界 3 は含む
        assert_eq!(quit_decide(2, 100, 103), (true, 100));
        assert_eq!(quit_decide(2, 100, 104), (false, 104)); // 4 秒差は再 arm
                                                            // quirk: 時計逆行（負差）も <= 3 で終了扱い
        assert_eq!(quit_decide(2, 100, 95), (true, 100));
    }

    #[test]
    fn link_move_quirks() {
        assert_eq!(link_move(-1, 1, 0), -1); // n=0 は -1
        assert_eq!(link_move(-1, 1, 5), 0); // -1 から順方向は先頭
        assert_eq!(link_move(-1, -1, 5), 4); // -1 から逆方向は末尾
        assert_eq!(link_move(0, -1, 5), 4); // 負剰余は +=n（C の切捨て % と同値）
        assert_eq!(link_move(4, 1, 5), 0); // 巡回
        assert_eq!(link_move(0, 0, 5), 0); // delta=0
        assert_eq!(link_move(-1, 0, 5), 0); // -1 かつ delta=0 は 0（C と同値）
        assert_eq!(link_move(9, 1, 5), 0); // idx>=n の不変条件破綻入力でも同写し
                                           // 大きい負 delta: C は (idx+delta)%n で -n より小にはならない…
                                           // idx+delta = -7, n=5 → -7 % 5 = -2 → +5 = 3
        assert_eq!(link_move(0, -7, 5), 3);
    }

    #[test]
    fn dup_cap_quirks() {
        assert_eq!(dup_cap(b"hello", 64), b"hello");
        assert_eq!(dup_cap(b"hello", 4), b"hel"); // n>=cap → cap-1
        assert_eq!(dup_cap(b"hello", 1), b"");
        assert_eq!(dup_cap(b"", 4), b"");
    }

    #[test]
    fn resolve_quirks() {
        fn none(_: &[u8]) -> bool {
            false
        }
        fn all(_: &[u8]) -> bool {
            true
        }
        // ws は ' ' '\t' のみ。'\n' は残り空でない → cwd 相対パス処理へ
        assert_eq!(resolve(b"   \t", b"/tmp", 64, &mut none), (2, None));
        let (rc, out) = resolve(b"\n", b"/tmp", 64, &mut all);
        assert_eq!(rc, 0); // "/tmp/\n" が存在すれば成功
        assert_eq!(out.unwrap(), b"/tmp/\n");
        // スキーム位置: 下限 2・上限 8・超過 9・不足 1
        assert_eq!(resolve(b"ab://x", b"/tmp", 64, &mut all), (1, None)); // 位置 2
        assert_eq!(resolve(b"abcdefgh://x", b"/tmp", 64, &mut all), (1, None)); // 位置 8
        assert_eq!(resolve(b"a://x", b"/tmp", 64, &mut all).0, 0); // 位置 1 → パス扱い
        let (rc, out) = resolve(b"abcdefghi://x", b"/tmp", 64, &mut all); // 位置 9 → パス扱い
        assert_eq!(rc, 0);
        assert_eq!(out.unwrap(), b"/tmp/abcdefghi://x");
        // 絶対パス: cap 検査が exists より先
        assert_eq!(resolve(b"/ab", b"/tmp", 3, &mut all), (2, None)); // n=3 >= cap=3
                                                                      // 絶対パス: 存在しなくても out は書き済み（rc=2 + Some）
        let (rc, out) = resolve(b"/not/exist", b"/tmp", 64, &mut none);
        assert_eq!(rc, 2);
        assert_eq!(out.unwrap(), b"/not/exist");
        let (rc, out) = resolve(b"/ok", b"/tmp", 64, &mut all);
        assert_eq!(rc, 0);
        assert_eq!(out.unwrap(), b"/ok");
        // cwd 相対: 存在しないと out 未書き
        assert_eq!(resolve(b"rel.md", b"/tmp", 64, &mut none), (2, None));
        let (rc, out) = resolve(b"rel.md", b"/w", 64, &mut all);
        assert_eq!(rc, 0);
        assert_eq!(out.unwrap(), b"/w/rel.md");
        // cwd 相対: 存在しても n >= cap なら rc=2・out 未書き（絶対との非対称）
        let (rc, out) = resolve(b"rel.md", b"/w", 10, &mut all);
        assert_eq!((rc, out), (0, Some(b"/w/rel.md".to_vec()))); // n=9 < cap=10 → 成功
        let (rc, out) = resolve(b"rel.md", b"/w", 9, &mut all);
        assert_eq!(rc, 2); // n=9 >= cap=9
        assert!(out.is_none());
        // snprintf 截断: cwd+input が 4095 超 → 截断物で exists（cap は十分大）
        let mut big = vec![b'a'; 5000];
        big[4998] = b'b';
        let (rc, out) = resolve(&big, b"/d", 8192, &mut all);
        assert_eq!(rc, 0);
        assert_eq!(out.unwrap().len(), 4095);
    }
}
