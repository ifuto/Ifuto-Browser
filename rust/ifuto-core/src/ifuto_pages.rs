//! 内部ページ生成（ifuto://settings / history / memory / about。C の `src/ifuto_pages.c` 相当）。
//!
//! | C (ifuto_pages.h / ifuto_pages.c) | Rust |
//! |---|---|
//! | `if_ifuto_page` | [`ifuto_page`] |
//!
//! # 実装済み
//!
//! 静的な HTML テンプレート + ローカル値の差し込みによる 4 内部ページ + unknown ページ。
//! 外部入力が入るのは履歴の title/url のみで、そこは `& < >` を escape する。
//!
//! # C との違い（状態注入）
//!
//! C は `IfChrome *`（タブ一覧・store・raster 判定結果）と fs を直接読む。Rust では
//! 純関数化のため、履歴テキスト・タブ一覧・raster 判定結果を引数として注入する。
//! 出力 HTML は C と byte 一致（差分 fuzz で実証）。
//!
//! # 既知の C の quirk（忠実再現）
//!
//! 履歴の url は属性値としても `& < >` のみ escape（`"` は escape されない。ヘッダ
//! コメントは「&<>\" を退避」と書くが実コードは `"` を退避しない）。

/// タブの会計情報（C の `IfTab` の id / doc arena / view arena / title 相当）。
#[derive(Clone, Debug, Default)]
pub struct TabInfo {
    /// タブ ID。
    pub id: u32,
    /// doc arena の reserved バイト。
    pub doc_bytes: u64,
    /// view arena の reserved バイト。
    pub view_bytes: u64,
    /// タイトル。
    pub title: String,
}

/// raster backend 判定結果（C の `IfRasterPick` 相当。注入データ）。
#[derive(Clone, Debug, Default)]
pub struct RasterPick {
    /// 選択 kernel index（[0, n_cand)。範囲外は「既定」表示）。
    pub selected: i32,
    /// 候補名。
    pub name: Vec<String>,
    /// 均一色 MB/s。
    pub mb_uniform: Vec<f64>,
    /// 非均一色 MB/s。
    pub mb_mixed: Vec<f64>,
    /// score（0.7*uniform + 0.3*mixed）。
    pub score: Vec<f64>,
    /// microbench 総時間（us）。
    pub bench_us: u64,
    /// GPU ノードの注記。
    pub gpu_note: String,
}

/// HTML ビルダ（C の `HB` 相当。`Vec<u8>` に直接追記、4MB 上限）。
struct Hb {
    buf: Vec<u8>,
}

impl Hb {
    fn new() -> Self {
        Hb { buf: Vec::new() }
    }

    fn put(&mut self, s: &[u8]) {
        if self.buf.len() + s.len() + 1 > 4 * 1024 * 1024 {
            return; // 内部ページの上限（壊れても巨大化しない）
        }
        self.buf.extend_from_slice(s);
    }

    fn s(&mut self, s: &str) {
        self.put(s.as_bytes());
    }

    fn u32(&mut self, v: u32) {
        self.s(&v.to_string());
    }

    fn u64(&mut self, v: u64) {
        self.s(&v.to_string());
    }

    /// 整数 KB → `N KB (M MB)`（C の `hb_kb`。MB は `>> 20` = 2^20 基準）。
    fn kb(&mut self, bytes: u64) {
        self.u64(bytes / 1024);
        self.s(" KB (");
        self.u64(bytes >> 20);
        self.s(" MB)");
    }

    /// 本文用エスケープ（`& < >`）。
    fn esc(&mut self, s: &[u8]) {
        for &c in s {
            match c {
                b'&' => self.s("&amp;"),
                b'<' => self.s("&lt;"),
                b'>' => self.s("&gt;"),
                _ => self.put(&[c]),
            }
        }
    }
}

const PAGE_HEAD: &str = "<html><head><title>ifuto://";
const PAGE_STYLE: &str = "</title></head><body><h1>";
const NAV: &str = "</h1><p><a href=\"ifuto://settings\">settings</a> | \
<a href=\"ifuto://history\">history</a> | \
<a href=\"ifuto://memory\">memory</a> | \
<a href=\"ifuto://about\">about</a></p><hr>";

fn page_settings(b: &mut Hb) {
    b.s("settings");
    b.s(PAGE_STYLE);
    b.s("Ifuto 設定");
    b.s(NAV);
    b.s("<h2>エンジン（akl = Aklus JS）</h2><pre>");
    b.s("同梱形態          : 同一リポジトリで build/akl として単独配布（make install-akl で導入可）\n");
    b.s("ブラウザ内実行    : DOM 結合は v0.4 台帳（現行は ifuto 本体に未リンク＝220KB 天井維持）\n");
    b.s("JIT               : 永久不採用（実行可能書き込みページは構造的にゼロ）\n");
    b.s("CoJIT（AOT 特化） : 既定 ON（kill switch: akl_set_cojit / docs: BENCH.md）\n");
    b.s("budget 既定       : 命令 10M、try 深さ 1024、スタック段 4096\n");
    b.s("</pre><h2>セキュリティ</h2><pre>");
    b.s("akl 単体ランナー   : seccomp-BPF サンドボックス強制（既定 ON。--no-sandbox で明示解除）\n");
    b.s("ブラウザプロセス   : sandbox primitive 実装済み（src/sandbox.c。chrome profile 適用は v0.2 台帳）\n");
    b.s("パース多層防御     : 全入力は共通パーサ + budget fail-stop（docs: ARCHITECTURE.md）\n");
    b.s("</pre><h2>メモリ方針</h2><pre>");
    b.s("ifuto://memory にタブごとの arena 会計（詳細）\n");
    b.s("slim-DOM           : 実ブラウズ経路で既定 ON（描画に関係ないものは DOM しない）\n");
    b.s("viewport 窓        : grid は文書全体を保持しない（行窓・再利用）\n");
    b.s("巨大文書           : 512MB 入力 budget（超過は OOM ではなく綺麗な fail。docs: BENCH.md 巨大 IDM 計測）\n");
    b.s("</pre><h2>描画</h2><pre>");
    b.s("CPU/GPU 自動判定   : GUI 起動時（TUI ではこの表示の初回表示時）マイクロベンチで\n");
    b.s("                     最速の raster fill kernel を選択。GPU は純 libc 方針により非接続\n");
    b.s("                     （この端末での決定と全候補の実測値は ifuto://memory の表示欄）\n");
    b.s("</pre><h2>切替手段</h2><pre>");
    b.s("akl CoJIT OFF      : アプリ埋込 API akl_set_cojit(rt, 0)（監査・差分検証用途）\n");
    b.s("CSS 索引 OFF       : if_css_set_naive_matching(1)（同）\n");
    b.s("store 場所         : ifuto --show-paths（INV-9）\n");
    b.s("</pre>");
}

fn page_history(b: &mut Hb, history: Option<&[u8]>) {
    b.s("history");
    b.s(PAGE_STYLE);
    b.s("履歴");
    b.s(NAV);
    if let Some(tsv) = history {
        if !tsv.is_empty() {
            // 末尾から最大 100 件（lines[0]=最新行）
            let end = tsv.len();
            let mut lines: Vec<usize> = Vec::new(); // 各行の先頭オフセット
            let mut le = end;
            while le > 0 && tsv[le - 1] == b'\n' {
                le -= 1; // 末尾改行を落とす
            }
            while le > 0 && lines.len() < 100 {
                let mut ls = le;
                while ls > 0 && tsv[ls - 1] != b'\n' {
                    ls -= 1;
                }
                lines.push(ls); // 行本体は [ls, le)
                le = ls;
                while le > 0 && tsv[le - 1] == b'\n' {
                    le -= 1;
                }
            }
            b.s("<p>直近 ");
            b.u32(lines.len() as u32);
            b.s(" 件（新しい順。store: history.tsv）</p><ul>");
            for &ls in &lines {
                let mut lend = ls;
                while lend < end && tsv[lend] != b'\n' {
                    lend += 1;
                }
                // epoch \t title \t url
                let mut t1 = None;
                let mut t2 = None;
                for (q, &ch) in tsv[ls..lend].iter().enumerate() {
                    if ch == b'\t' {
                        let q = ls + q;
                        if t1.is_none() {
                            t1 = Some(q);
                        } else if t2.is_none() {
                            t2 = Some(q);
                            break;
                        }
                    }
                }
                let (Some(t1), Some(t2)) = (t1, t2) else { continue };
                b.s("<li>[");
                b.esc(&tsv[ls..t1]); // epoch
                b.s("] <a href=\"");
                b.esc(&tsv[t2 + 1..lend]); // url（属性）
                b.s("\">");
                b.esc(&tsv[t1 + 1..t2]); // title
                b.s("</a> ");
                b.esc(&tsv[t2 + 1..lend]); // url（可視テキスト）
                b.s("</li>");
            }
            b.s("</ul><hr><p>クリア: 現在は UI 経路なし（store ファイルを削除。ifuto --show-paths で場所を提示）＝誤爆しない設計</p>");
            return;
        }
    }
    b.s("<p>履歴はまだありません（またはストア無効）。</p>");
}

/// C の `%10.0f`（右揃え幅 10）。`format!("{:10.0}")` と同値。
fn fmt_f10(v: f64) -> String {
    format!("{:10.0}", v)
}

fn page_memory(b: &mut Hb, tabs: &[TabInfo], rp: &RasterPick) {
    b.s("memory");
    b.s(PAGE_STYLE);
    b.s("メモリ会計");
    b.s(NAV);
    b.s("<table><tr><th>tab</th><th>doc arena</th><th>view arena</th><th>title</th></tr>");
    let mut tot_doc = 0u64;
    let mut tot_view = 0u64;
    for t in tabs {
        tot_doc += t.doc_bytes;
        tot_view += t.view_bytes;
        b.s("<tr><td>");
        b.u32(t.id);
        b.s("</td><td>");
        b.kb(t.doc_bytes);
        b.s("</td><td>");
        b.kb(t.view_bytes);
        b.s("</td><td>");
        b.esc(t.title.as_bytes());
        b.s("</td></tr>");
    }
    b.s("</table><p>合計 doc: ");
    b.kb(tot_doc);
    b.s(" / view: ");
    b.kb(tot_view);
    b.s("</p>");
    b.s("<p>方針（ユーザ法則）: 「メモリは使わなければ使わないほど良い」\
— arena はタブ寿命で保持し、view は再レイアウトで破棄・再構築。\
巨大 IDM の正確な係数（実測）は BENCH.md の「巨大 IDM 計測」節。</p>");

    // raster backend 決定欄
    let n_cand = rp.name.len();
    let sel_name = if rp.selected >= 0 && (rp.selected as usize) < n_cand {
        rp.name[rp.selected as usize].as_str()
    } else {
        "(bench 不能のため既定)"
    };
    b.s("<hr><h2>raster backend（起動時 microbench 決定）</h2><pre>");
    b.s("選択: ");
    b.s(sel_name);
    b.s("  [bench 総時間 ");
    b.u64(rp.bench_us);
    b.s(" us, バッファ 512 KiB, 加重 0.7*白bg+0.3*非均一]\n");
    b.s("kernel            白bg(均一)      非均一色        score\n");
    for k in 0..n_cand {
        b.s("  ");
        let nm = rp.name[k].as_str();
        b.s(nm);
        b.s(if k as i32 == rp.selected { " *" } else { "  " });
        let w = nm.chars().count();
        for _ in w..15 {
            b.s(" ");
        }
        b.s(" ");
        b.s(&format!("{} MB/s", fmt_f10(rp.mb_uniform[k])));
        b.s("  ");
        b.s(&format!("{} MB/s", fmt_f10(rp.mb_mixed[k])));
        b.s("  ");
        b.s(&fmt_f10(rp.score[k]));
        b.s("\n");
    }
    b.s("GPU : ");
    b.s(&rp.gpu_note);
    b.s("\n（数値は起動ごとにこの端末で実測。他機種・他環境との比較根拠には使わない）</pre>");
}

fn page_about(b: &mut Hb) {
    b.s("about");
    b.s(PAGE_STYLE);
    b.s("Ifuto について");
    b.s(NAV);
    b.s("<pre>");
    b.s("Ifuto Browser — 史上最強の軽量ブラウザ（自己完結 C11、ldd = linux-vdso/libc/ld (+libm) のみ）\n");
    b.s("akl (Aklus)  : 自作 JS エンジン。C11・JIT なし・seccomp 既定・CoJIT(AOT 特化)。単独インストール可\n");
    b.s("CSS          : RuleSet 索引（Blink 戦略相当、実測 23.32x vs 全走査）\n");
    b.s("WPT tree-construction 適合率: 97.3% (1679/1726)\n");
    b.s("一次情報     : README.md / ARCHITECTURE.md / BENCH.md / CHROME_SCOPE.md\n");
    b.s("             docs/BLINK_COMPAT.md / docs/V8_COMPAT.md / docs/AKL_COMPAT.md / docs/SANDBOX.md\n");
    b.s("</pre>");
}

/// `ifuto://` 内部ページの HTML を生成。C の `if_ifuto_page` 相当。
/// 非 ifuto:// URL は `None`。履歴・タブ・raster 判定結果は注入データ。
pub fn ifuto_page(
    url: &str,
    history: Option<&[u8]>,
    tabs: &[TabInfo],
    rp: &RasterPick,
) -> Option<Vec<u8>> {
    let pg = url.strip_prefix("ifuto://")?;
    let mut b = Hb::new();
    b.s(PAGE_HEAD);
    match pg {
        "settings" => page_settings(&mut b),
        "history" => page_history(&mut b, history),
        "memory" => page_memory(&mut b, tabs, rp),
        "about" => page_about(&mut b),
        _ => {
            b.s("unknown");
            b.s(PAGE_STYLE);
            b.s("未知の内部ページ");
            b.s(NAV);
            b.s("<p>ifuto://settings ifuto://history ifuto://memory ifuto://about があります。</p>");
        }
    }
    b.s("</body></html>");
    Some(b.buf)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn settings_page() {
        let html = ifuto_page("ifuto://settings", None, &[], &RasterPick::default()).unwrap();
        let s = String::from_utf8(html).unwrap();
        assert!(s.starts_with("<html><head><title>ifuto://settings</title></head><body><h1>Ifuto 設定"));
        assert!(s.contains("JIT               : 永久不採用"));
        assert!(s.ends_with("</body></html>"));
    }

    #[test]
    fn about_page() {
        let html = ifuto_page("ifuto://about", None, &[], &RasterPick::default()).unwrap();
        let s = String::from_utf8(html).unwrap();
        assert!(s.contains("WPT tree-construction 適合率: 97.3% (1679/1726)"));
    }

    #[test]
    fn unknown_page() {
        let html = ifuto_page("ifuto://nope", None, &[], &RasterPick::default()).unwrap();
        let s = String::from_utf8(html).unwrap();
        assert!(s.contains("未知の内部ページ"));
    }

    #[test]
    fn non_ifuto_none() {
        assert!(ifuto_page("http://x", None, &[], &RasterPick::default()).is_none());
    }

    #[test]
    fn history_escapes() {
        // url/title に & < > を含む（" は C と同様 escape しない）
        let tsv = "100\tA&B\t<a>\n200\tT<U>\t<b>\n";
        let html = ifuto_page("ifuto://history", Some(tsv.as_bytes()), &[], &RasterPick::default()).unwrap();
        let s = String::from_utf8(html).unwrap();
        // 最新が先頭
        assert!(s.contains("<li>[200] <a href=\"&lt;b&gt;\">T&lt;U&gt;</a> &lt;b&gt;</li>"), "got: {s}");
        assert!(s.contains("<li>[100] <a href=\"&lt;a&gt;\">A&amp;B</a> &lt;a&gt;</li>"), "got: {s}");
    }

    #[test]
    fn history_empty() {
        let html = ifuto_page("ifuto://history", Some(b""), &[], &RasterPick::default()).unwrap();
        let s = String::from_utf8(html).unwrap();
        assert!(s.contains("履歴はまだありません"));
    }

    #[test]
    fn memory_table() {
        let tabs = [TabInfo {
            id: 1,
            doc_bytes: 2048,
            view_bytes: 1048576,
            title: "T&<i>".to_string(),
        }];
        let rp = RasterPick {
            selected: 1,
            name: vec!["a".to_string(), "bb".to_string()],
            mb_uniform: vec![100.0, 200.0],
            mb_mixed: vec![50.0, 60.0],
            score: vec![85.0, 158.0],
            bench_us: 1234,
            gpu_note: "GPU ノード未検出。CPU raster のみ".to_string(),
        };
        let html = ifuto_page("ifuto://memory", None, &tabs, &rp).unwrap();
        let s = String::from_utf8(html).unwrap();
        assert!(s.contains("<tr><td>1</td><td>2 KB (0 MB)</td><td>1024 KB (1 MB)</td><td>T&amp;&lt;i&gt;</td></tr>"), "got: {s}");
        assert!(s.contains("選択: bb  [bench 総時間 1234 us"));
        assert!(s.contains("GPU : GPU ノード未検出。CPU raster のみ"));
    }
}
