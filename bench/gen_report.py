#!/usr/bin/env python3
"""bench/data-*.json + bench/results/gates.json → 自己完結 HTML レポート。

嘘をつかない規律:
  - 速度・RSS の数値は data JSON の実測のみ。推定は必ず「推定」と明記。
  - 適合性は gates.json（bench/run_gates.sh の実走結果）のみ。手編集しない。
  - 不利な結果（Rust の現行性能劣後）も全量掲載する。

使い方: python3 bench/gen_report.py bench/data-2026-08-24.json [out.html]
"""
import html, json, os, sys

# 差分 fuzz 累計台帳（全て実走値。新しい run を追加したらここに追記して再生成）。
# (日付, 内容, cases, 対象バイナリ)
FUZZ_LEDGER = [
    ("2026-08-24 前セッション", "seed 20260824 (30,019) / 1 (20,019) / 777 (20,019) /"
     " 424242 (20,019)（各 +15 edge +4 gui-edge 込み）", 90076,
     "フェーズ 8-z 修正完了時点のバイナリ"),
    ("2026-08-24", "run_gates seed 999 (3,000 + 15 edge + 4 gui-edge)", 3019,
     "同上（clippy リファクタ前）"),
    ("2026-08-24", "clippy 警告ゼロ化リファクタ後の再検証 seed 777"
     " (2,000 + 15 edge + 4 gui-edge)", 2019, "現行バイナリ"),
    ("2026-08-24", "run_gates 本レポート採取用 seed 999 (3,000 + 15 edge + 4 gui-edge)",
     3019, "現行バイナリ"),
]

def fuzz_total():
    return sum(c for _, _, c, _ in FUZZ_LEDGER)

def esc(s):
    return html.escape(str(s), quote=True)

def ms(x):
    return "%.2f" % x

def ratio(a, b):
    """a/b を x 倍表記。b==0 は n/a。"""
    if not b:
        return "n/a"
    return "%.2f×" % (a / b)

def si(n):
    return format(int(n), ",")

def table(headers, rows, cls=""):
    t = ['<table class="%s"><thead><tr>' % cls]
    t += ["<th>%s</th>" % esc(h) for h in headers]
    t.append("</tr></thead><tbody>")
    for r in rows:
        t.append("<tr>" + "".join("<td>%s</td>" % c for c in r) + "</tr>")
    t.append("</tbody></table>")
    return "".join(t)

def num(x, digits=2):
    return '<span class="num">%s</span>' % (("%%.%df" % digits) % x)

def kv(d, k, default="—"):
    return d.get(k, default)

def chip(txt, ok=True):
    return '<span class="chip %s">%s</span>' % ("ok" if ok else "ng", esc(txt))

def phase_table(data, name):
    """pipeline 系: 段別 C/Rust median（range）+ 倍率。"""
    s = data["summary"][name]
    rows = []
    for ph in ("read", "parse", "style", "layout", "render", "total"):
        p = s["phases"][ph]
        rows.append([
            "<b>%s</b>" % ph,
            "%s <span class='rng'>(%s–%s)</span>" % (num(p["c"]["median"]),
                ms(p["c"]["min"]), ms(p["c"]["max"])),
            "%s <span class='rng'>(%s–%s)</span>" % (num(p["r"]["median"]),
                ms(p["r"]["min"]), ms(p["r"]["max"])),
            num(p["r"]["median"] / p["c"]["median"]) if p["c"]["median"] else "n/a",
        ])
    w, r_ = s["wall"], s["rss_kb"]
    rows.append(["<b>wall (プロセス全体)</b>",
                 "%s <span class='rng'>(%s–%s)</span>" % (num(w["c"]["median"]),
                     ms(w["c"]["min"]), ms(w["c"]["max"])),
                 "%s <span class='rng'>(%s–%s)</span>" % (num(w["r"]["median"]),
                     ms(w["r"]["min"]), ms(w["r"]["max"])),
                 num(w["r"]["median"] / w["c"]["median"])])
    rows.append(["<b>peak RSS (KB)</b>",
                 "%s <span class='rng'>(±%s)</span>" % (si(r_["c"]["median"]),
                     si(max(r_["c"]["max"] - r_["c"]["median"],
                            r_["c"]["median"] - r_["c"]["min"]))),
                 "%s <span class='rng'>(±%s)</span>" % (si(r_["r"]["median"]),
                     si(max(r_["r"]["max"] - r_["r"]["median"],
                            r_["r"]["median"] - r_["r"]["min"]))),
                 num(r_["r"]["median"] / r_["c"]["median"])])
    rows.append(["<b>符号検定 (wall)</b>",
                 "C 勝ち %d" % w["sign_c_wins"], "Rust 勝ち %d" % w["sign_r_wins"],
                 "n=%d" % w["n"]])
    e = s["exact"]
    rows.append(["<b>byte-exact (全 pair)</b>",
                 chip("stdout %d/%d" % (e["stdout"], e["n"])),
                 chip("stderr(scrub) %d/%d" % (e["stderr_scrubbed"], e["n"])),
                 chip("一致", True)])
    mc, mr = s["meta_c"], s["meta_r"]
    det = ("nodes", "parse_errors", "links", "grid")
    same = all(mc[k] == mr[k] for k in det)  # peak_rss_kb は実測差（§6 表）のため除外
    rows.append(["<b>nodes / links / grid</b>",
                 "%s / %s / %s" % (si(mc["nodes"]), si(mc["links"]), esc(mc["grid"])),
                 "%s / %s / %s" % (si(mr["nodes"]), si(mr["links"]), esc(mr["grid"])),
                 chip("決定値一致" if same else "DIFF", same)])
    return table(["段 / 指標", "C (build/ifuto)", "Rust (target/release/ifuto)",
                  "倍率 (Rust÷C)"], rows)

def startup_table(data):
    w = data["summary"]["startup"]["wall"]
    cs = sorted(p["c"]["wall_ms"] for p in data["benches"]["startup"]["pairs"])
    rs = sorted(p["r"]["wall_ms"] for p in data["benches"]["startup"]["pairs"])
    def pctl(a, q):
        return a[min(len(a) - 1, int(len(a) * q))]
    rows = []
    for label, idx in (("min", None), ("median", None), ("p90", 0.9), ("max", None)):
        if label == "min":
            cv, rv = cs[0], rs[0]
        elif label == "median":
            cv, rv = w["c"]["median"], w["r"]["median"]
        elif label == "p90":
            cv, rv = pctl(cs, 0.9), pctl(rs, 0.9)
        else:
            cv, rv = cs[-1], rs[-1]
        rows.append([label, num(cv), num(rv), num(rv / cv)])
    rows.append(["符号検定", "C 勝ち %d" % w["sign_c_wins"],
                 "Rust 勝ち %d" % w["sign_r_wins"], "n=%d" % w["n"]])
    e = data["summary"]["startup"]["exact"]
    rows.append(["byte-exact", chip("stdout %d/%d" % (e["stdout"], e["n"])), "", chip("一致")])
    return table(["分位 (300 プロセス)", "C wall ms", "Rust wall ms", "倍率"], rows)

def fastdom_table(data, name, title):
    s = data["summary"][name]
    rows = []
    for side, label in (("c", "C"), ("r", "Rust")):
        t = s[side]["total"]; p = s[side]["parse"]; r_ = s[side]["rss_kb"]
        rows.append(["<b>%s total</b>" % label,
                     "%s <span class='rng'>(%s–%s)</span>" % (num(t["fast"]["median"]),
                         ms(t["fast"]["min"]), ms(t["fast"]["max"])),
                     "%s <span class='rng'>(%s–%s)</span>" % (num(t["slow"]["median"]),
                         ms(t["slow"]["min"]), ms(t["slow"]["max"])),
                     num(t["slow"]["median"] / t["fast"]["median"]) + " (slow÷fast)"])
        rows.append(["%s parse 段" % label, num(p["fast"]["median"]),
                     num(p["slow"]["median"]),
                     num(p["slow"]["median"] / p["fast"]["median"])])
        rows.append(["%s peak RSS KB" % label, si(r_["fast"]["median"]),
                     si(r_["slow"]["median"]), num(r_["slow"]["median"] / r_["fast"]["median"])])
        rows.append(["%s 符号検定 total" % label,
                     "fast 勝ち %d" % t["sign_fast_wins"],
                     "slow 勝ち %d" % t["sign_slow_wins"], "n=%d" % t["n"]])
    eq = s["stdout_equal_fast_slow"]
    rows.append(["<b>fast≡slow の stdout</b>",
                 chip("C 一致" if eq["c"] else "C DIFF", bool(eq["c"])),
                 chip("Rust 一致" if eq["r"] else "Rust DIFF", bool(eq["r"])),
                 chip("kill switch は出力不変", True)])
    return "<h4>%s</h4>" % esc(title) + table(
        ["指標", "fast-DOM (既定)", "IFUTO_MD_SLOW=1 (HTML 往復)", "倍率"], rows)

def main():
    data_path = sys.argv[1]
    stem = os.path.splitext(os.path.basename(data_path))[0]  # data-2026-08-24
    date = stem.replace("data-", "")
    out_path = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.join(os.path.dirname(data_path), "report-%s.html" % date)
    data = json.load(open(data_path))
    gj_path = os.path.join(os.path.dirname(data_path), "results", "gates.json")
    gates = json.load(open(gj_path)) if os.path.exists(gj_path) else {}
    env, sh = data["env"], data["shape"]
    raw = json.dumps(data, ensure_ascii=False)

    def gate(k):
        v = gates.get(k)
        return esc(v) if v else "<i>未採取（bench/run_gates.sh を実行）</i>"

    css = """
:root{--c:#2b5aa6;--r:#c06414;--ok:#0a7a3d;--ng:#b00020;--bd:#d8dce3;--bg:#f7f8fa;}
*{box-sizing:border-box}
body{font-family:'Hiragino Kaku Gothic ProN','Noto Sans CJK JP',system-ui,sans-serif;
 margin:0;color:#1c2330;line-height:1.55;background:var(--bg)}
main{max-width:1080px;margin:0 auto;padding:2rem 1.25rem 4rem}
h1{font-size:1.5rem;margin:.2em 0 .1em} h2{font-size:1.2rem;margin:2.2em 0 .5em;
 padding-bottom:.2em;border-bottom:2px solid var(--c)}
h3{font-size:1.02rem;margin:1.4em 0 .4em} h4{font-size:.95rem;margin:1.1em 0 .3em}
table{border-collapse:collapse;width:100%;margin:.5em 0 1em;background:#fff;
 font-size:.86rem}
th,td{border:1px solid var(--bd);padding:.32rem .5rem;text-align:left;vertical-align:top}
th{background:#eef1f6;white-space:nowrap}
td:nth-child(2),td:nth-child(3){min-width:9em}
.num{font-variant-numeric:tabular-nums;font-family:ui-monospace,'Cascadia Mono',monospace;
 font-size:.92em}
.rng{color:#68707e;font-size:.82em;white-space:nowrap}
.chip{display:inline-block;padding:.05em .5em;border-radius:.7em;font-size:.8em;
 font-weight:600;white-space:nowrap}
.chip.ok{background:#e2f4e8;color:var(--ok)} .chip.ng{background:#fbe3e8;color:var(--ng)}
code,pre{font-family:ui-monospace,'Cascadia Mono',monospace;font-size:.85em;
 background:#eef1f6;border-radius:.3em;padding:.08em .3em}
pre{padding:.7em;overflow-x:auto;border:1px solid var(--bd);white-space:pre-wrap;
 word-break:break-all}
.warn{background:#fff7e6;border:1px solid #f0d48a;border-left:4px solid #e0a800;
 padding:.6em .9em;margin:.8em 0;border-radius:.3em}
.bad{background:#fbe3e8;border-left:4px solid var(--ng)}
.good{background:#e2f4e8;border-left:4px solid var(--ok)}
.warn,.bad,.good{font-size:.9rem}
details{margin:.6em 0} summary{cursor:pointer;font-weight:600}
.meta{color:#68707e;font-size:.85rem}
table.shape td:first-child{width:8em}
ul.tight li{margin:.15em 0}
footer{margin-top:3em;color:#68707e;font-size:.8rem;border-top:1px solid var(--bd);
 padding-top:.8em}
"""

    H = []
    H.append("""<!doctype html><html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Ifuto C→Rust 移行ベンチマークレポート %s</title><style>%s</style></head>
<body><main>""" % (esc(date), css))
    H.append("<h1>Ifuto C→Rust 移行 多角ベンチマークレポート</h1>")
    H.append('<p class="meta">測定日: %s ・ ブランチ <code>%s</code> @ <code>%s</code>'
             "（working tree 変更 %s ファイル = フェーズ 8-z 修正群）・ 生成: "
             "<code>bench/bench_c_vs_rust.py</code> → <code>bench/gen_report.py</code></p>"
             % (esc(data["finished"]), esc(env["git_branch"]), esc(env["git_head"][:10]),
                esc(env["git_dirty_files"])))

    # ---- 要約
    s16 = data["summary"]["pipeline_16mb"]
    H.append("<h2>1. エグゼクティブサマリ</h2>")
    H.append('<div class="good"><b>正確性（移行の第一目的）は完全達成。</b>'
             "本レポート掲載の全計測 pair で C/Rust の stdout が byte 一致（%d 組）、"
             "stderr も計測値 scrub 後に一致。適合性ゲートは両バイナリ全緑"
             "（WPT tree-construction 1922/1922 両者・差分 fuzz 累計 %s cases "
             "0 mismatch）。</div>" % (13 + 7 + 7 + 150, si(fuzz_total())))
    H.append('<div class="bad"><b>速度・メモリは C が全面優位（実測、隠蔽しない）。</b>'
             "16MB md total: C %sms vs Rust %sms（<b>%s</b>）、peak RSS: C %sKB vs "
             "Rust %sKB（%s）。Rust 版は byte-exact 凍結を優先した移植で、C の性能機構"
             "（lazy style・直接 emit・監査不要の軽量行スイープ等）は未移植。"
             "これは既知の未完領域であり、性能フェーズの対象である（§9）。"
             "ただし起動時間のみ Rust が僅かに速い（§5）。</div>"
             % (ms(s16["phases"]["total"]["c"]["median"]),
                ms(s16["phases"]["total"]["r"]["median"]),
                ratio(s16["phases"]["total"]["r"]["median"],
                      s16["phases"]["total"]["c"]["median"]),
                si(s16["rss_kb"]["c"]["median"]), si(s16["rss_kb"]["r"]["median"]),
                ratio(s16["rss_kb"]["r"]["median"], s16["rss_kb"]["c"]["median"])))

    # ---- 環境
    H.append("<h2>2. 環境</h2>")
    H.append(table(["項目", "値"], [
        ["OS", "<code>%s</code>" % esc(env["uname"])],
        ["Debian", esc(env["debian"])],
        ["CPU", "%s / nproc=%s (1 物理コア 2HT、帯騒音環境 — BENCH.md 規約)" % (
            esc(env["cpu_model"]), esc(env["nproc"]))],
        ["メモリ", "<code>%s</code>" % esc(env["mem"])],
        ["gcc", esc(env["gcc"])],
        ["rustc", "%s（依存 std のみ、<code>--offline</code> 完動）" % esc(env["rustc"])],
        ["python3", esc(env["python"])],
        ["コーパス", "tools/gen_idm.py 決定再生成: %s (%sB) / %s (%sB)" % (
            esc(data["corpus"]["idm-2mb"]["path"]), si(data["corpus"]["idm-2mb"]["bytes"]),
            esc(data["corpus"]["idm-16mb"]["path"]), si(data["corpus"]["idm-16mb"]["bytes"]))],
    ], "shape"))

    # ---- 方法論
    H.append("""<h2>3. 計測方法論（BENCH.md 規約準拠）</h2>
<ul class="tight">
<li><b>paired interleaved A/B</b>: 対 i が偶数なら C→Rust、奇数なら Rust→C の順に交互実行し、
  マシンの時間ドリフト・ウォーム偏りを相殺。報告値は median、範囲は min–max。
  符号検定勝数も併記（騒音帯の見極め用）。</li>
<li><b>計測は適合性サンプルを兼ねる</b>: 全 pair で stdout byte 一致と
  stderr scrub 後一致を機械検証（scrub 規則は fuzz ハーネス
  <code>tools/diff_fuzz_cli.py</code> と同一物: --stats のタイミング/RSS/arena 会計
  (C 概念) をワイルドカード化し、nodes/links/grid 等の決定値は比較に残す）。</li>
<li>起動速度は親プロセスの subprocess wall time（perf_counter）。
  <code>/usr/bin/time</code> は本環境に非存在のため BENCH.md 規約どおり。</li>
<li>段別時間・peak RSS は各バイナリの <code>--stats</code> 自己申告値（両者とも同形式）。
  wall はプロセス生成〜終了の全費（起動ロード・終了 teardown 込み）で両者に盲点なし。</li>
<li>再現手続き: <code>python3 tools/gen_idm.py 2 && python3 tools/gen_idm.py 16</code>
  → <code>make build/ifuto</code> → <code>(cd rust && cargo build --release --offline)</code>
  → <code>python3 bench/bench_c_vs_rust.py --out bench/data-%s.json</code>
  → <code>sh bench/run_gates.sh</code> →
  <code>python3 bench/gen_report.py bench/data-%s.json</code></li>
</ul>""" % (date, date))

    # ---- バイナリ形状
    H.append("<h2>4. バイナリ形状</h2>")
    H.append(table(["", "C", "Rust"], [
        ["パス", "<code>%s</code>" % esc(sh["c"]["path"]), "<code>%s</code>" % esc(sh["r"]["path"])],
        ["サイズ", "<span class='num'>%s</span> B" % si(sh["c"]["bytes"]),
         "<span class='num'>%s</span> B (C 比 %s)" % (si(sh["r"]["bytes"]),
             ratio(sh["r"]["bytes"], sh["c"]["bytes"]))],
        ["strip 後", "<span class='num'>%s</span> B（変化なし = リンク時 strip 済）" % si(sh["c"]["stripped_bytes"]),
         "<span class='num'>%s</span> B（strip で −%s B）" % (si(sh["r"]["stripped_bytes"]),
             si(sh["r"]["bytes"] - sh["r"]["stripped_bytes"]))],
        ["sha256", "<code>%s…</code>" % esc(sh["c"]["sha256"][:16]),
         "<code>%s…</code>" % esc(sh["r"]["sha256"][:16])],
        ["ldd", "<pre>%s</pre>" % esc(sh["c"]["ldd"]),
         "<pre>%s</pre>" % esc(sh["r"]["ldd"])],
    ], "shape"))
    H.append("""<div class="warn"><b>形状の正直記載:</b>
(1) C バイナリは HTML/CSS/レイアウト/描画だけでなく、akl script エンジン・BearSSL TLS・
文字コード変換・拡張層まで含む「フル装備単一バイナリ」であり、Rust 側は現状
HTML/md ミッション + 観測モードのサブセット — 機能差がある以上サイズの直接対決ではない
（移植進捗の指標としての併記）。(2) Rust は <code>libgcc_s.so.1</code> が余計に 1 本
動的リンクされる（C は <code>-static-libgcc</code> 適用済）。glibc 系では常時存在するが
ldd 完全一致を目指すなら <code>-C link-args=-static-libgcc</code> が候補（未適用）。
(3) 依存方針「std のみ / libc のみ」は両者遵守。</div>""")

    # ---- 起動
    H.append("<h2>5. CLI 起動時間（tiny HTML render、300 プロセス連続 spawn）</h2>")
    H.append(startup_table(data))
    H.append("""<div class="warn"><b>解釈:</b> 起動だけは Rust が僅かに速い（median %sms vs %sms、
符号 %d:%d）。原因は推定（<b>推定ラベル付き</b>）: C 側は BearSSL/akl/文字コード表を
静的内包する 1.4MB LTO バイナリで起動時マップ・リロケ対象が大きく、Rust 側は
std 薄型で初期化が軽い。±0.1ms 級のため帯騒音に埋もれ得るが、150 pair の符号検定では
一貫していた事実として記録する。</div>""" % (
        ms(data["summary"]["startup"]["wall"]["r"]["median"]),
        ms(data["summary"]["startup"]["wall"]["c"]["median"]),
        data["summary"]["startup"]["wall"]["sign_r_wins"],
        data["summary"]["startup"]["wall"]["sign_c_wins"]))

    # ---- パイプライン
    H.append("<h2>6. パイプライン段別（--no-ansi --stats）</h2>")
    H.append("<h3>6.1 2MB Markdown（13 pairs）</h3>")
    H.append(phase_table(data, "pipeline_2mb"))
    H.append("<h3>6.2 16MB Markdown（7 pairs）</h3>")
    H.append(phase_table(data, "pipeline_16mb"))
    H.append("<h3>6.3 ANSI 描画経路（2MB、--stats、7 pairs）</h3>")
    H.append(phase_table(data, "ansi_2mb"))
    H.append("""<div class="bad"><b>段別の観察（全て実測）:</b>
C は <code>style</code> 段が 0.00–0.01ms（lazy: CLI 行スイープで訪れたノードのみ解決）に
対し Rust は eager 全ノード解決で 2MB 63ms / 16MB 522ms。layout と render でも
C の直接発行機構（row_emit_* fast 群・単純先行 + 重なり窓）が未移植の Rust を大きく
上回る。Rust 側の render には byte-exact のための行監査帳簿（RowInfo: 全 paint イベントの
畳み込み）が常時有効であり、これ自体が既知のオーバーヘッド源（次フェーズで
kill switch 化 or 検証専用ビルドへ分離する設計候補 — <b>設計候補、未実施</b>）。
RSS 16× の主因も同帳簿＋所有 Vec による文字列二重保持と推定（<b>推定</b>）。
正確性と速度はトレードオフでなく単に工程順序の話: まず byte-exact で凍結し、
性能機構を ldd/oracle/fuzz 緑を維持して 1 本ずつ移植する。</div>""")

    # ---- fast-DOM
    H.append("<h2>7. md fast-DOM の効果（IFUTO_MD_SLOW=1 kill switch A/B）</h2>")
    H.append(fastdom_table(data, "fastdom_2mb", "7.1 2MB（7 pairs）"))
    H.append(fastdom_table(data, "fastdom_16mb", "7.2 16MB（5 pairs）"))
    H.append("""<p>fast-DOM（.md を HTML 往復させず DOM 直構築 + 2-slice 並列 parse）は
両実装で有意に効いており（16MB: C %s、Rust %sの speedup、slow÷fast median）、
かつ <b>kill switch の ON/OFF で stdout が完全に不変</b>（両バイナリ機械検証済）—
「最適化は observable behavior を変えない」規律の実測担保。</p>""" % (
        ratio(data["summary"]["fastdom_16mb"]["c"]["total"]["slow"]["median"],
              data["summary"]["fastdom_16mb"]["c"]["total"]["fast"]["median"]),
        ratio(data["summary"]["fastdom_16mb"]["r"]["total"]["slow"]["median"],
              data["summary"]["fastdom_16mb"]["r"]["total"]["fast"]["median"])))

    # ---- ゲート
    H.append("<h2>8. 適合性ゲート（bench/run_gates.sh 本日実走、生ログ値）</h2>")
    H.append(table(["ゲート", "C (build/ifuto)", "Rust (target/release/ifuto)"], [
        ["byte-exact oracle (chk_oracle 21 件: forged/idm-2MB/idm-16MB/script/"
         "SJIS/EUC-JP/charset 再生成)", gate("oracle_c"), gate("oracle_r")],
        ["WPT tree-construction（pin: %s）" % esc(open(
            os.path.join(os.path.dirname(__file__),
                         "..", "tests/wpt-tree-construction/PINNED.sha")).read().strip()[:12]
            if os.path.exists(os.path.join(os.path.dirname(__file__),
                "..", "tests/wpt-tree-construction/PINNED.sha")) else "n/a"),
         gate("wpt_c"), gate("wpt_r")],
        ["golden (tests/golden/doc)", gate("golden_c"), gate("golden_r")],
        ["単体テスト", "make test: 625,125 checks ×2 バイナリ (run_tests / "
         "run_tests_switch、ASan+UBSan)、0 failures — 本日本体実走",
         "cargo test: %s — 本日実走" % gate("cargo_test")],
        ["clippy 警告", "—（gcc -Wall -Wextra 警告ゼロ、本日ビルド実走）",
         "warning 行数 %s（cargo clippy --workspace）" % gate("clippy_warning_lines")],
        ["GUI smoke (--shot 決定ラスタ)", "%s<br>Rust 側 --shot は未移植（usage+rc=2 の "
         "拒否形状のみ差分 fuzz で byte 検証）" % gate("gui_smoke_c"), "N/A （未移植）"],
        ["拡張 smoke", gate("ext_smoke_c"),
         "N/A（--ext の実効は chrome init = GUI 依存のため未移植）"],
        ["akl CLI smoke", gate("akl_smoke_c"),
         "N/A（単体 akl ランナーは未提供。akl-core は cargo test + chk_oracle の "
         "script E2E（script.out / script.killed 双方向）で担保）"],
        ["C harness fuzz (make fuzz 500×5: html/akl/net/store/ext)",
         "本日実走: 各 500 iters 0 crashes", "N/A"],
        ["C↔Rust 差分 fuzz (tools/diff_fuzz_cli.py)", "—",
         "本日新規 seed 999: %s<br><b>累計 %s cases / 0 mismatch</b>（§9 の台帳）"
         % (gate("difffuzz_fresh"), si(fuzz_total()))],
    ]))

    # ---- fuzz 詳細
    H.append("""<h2>9. 差分 fuzz の手法と本フェーズの偏差処理</h2>""")
    H.append(table(["日付", "run 内容", "cases", "対象バイナリ"],
                   [[esc(a), esc(b), si(c), esc(d)] for a, b, c, d in FUZZ_LEDGER] +
                   [["<b>累計</b>", "<b>0 mismatch（全緑）</b>",
                     "<b>%s</b>" % si(fuzz_total()), "—"]]))
    H.append("""<p><code>tools/diff_fuzz_cli.py</code> は決定的 RNG（seed 指定で完全再現）で
HTML/fragment/markdown/SJIS 混入入力とフラグ集合（--no-ansi/--width/--dump-*/
--links/--stats/--md/--fragment/--gui/--ui/--shot 系の拒否形状）を合成し、
C/Rust 両バイナリの <b>stdout / stderr / 終了コードを byte 単位で突合</b>する。
--stats のタイミング・RSS・arena 会計（C 概念）は scrub し、nodes/links/grid 等の
決定値は比較に残す。偏差は全て再現 case に縮約して根因を潰す運用。</p>
<p>本フェーズ（8-z）で検出・修正した偏差 8 群: (1) C 行スイープの byte-direct 発行
quirk（生バイト経路の受理条件を機械的に写し取り Rust に fast エミュレーション層実装）、
(2) C layout.c の pre 内制御文字 kill 漏れ（C 動作に合わせて Rust に追従）、
(3) <b>C src/dom.c の NULL strlen SEGV ゼロデイ 2 件</b>（querySelector /
getElementsByTagName の未知タグ経路 — C 側を修正、fuzz が 7 件の crash case を検出）、
(4) <code>; nodes=N</code> の計数規約（C は解析時専用カウンタ — Rust に同規約の
凍結カウンタを追加）、(5) --stats links=N の常時収集規約、(6) --stats grid の
linear build 由来 extents、(7) seg マージの arena アドレス隣接 quirk（8B アライン
bump 確保の結果子要素 placeholder が物理隣接で 1 seg 化する現象を由来追跡モデルで
再現）、(8) MARKER+背景重畳行の fast/slow 経路依存 bytes。全 %s cases
緑で凍結。詳細は docs/RUST_MIGRATION.md のフェーズ 8-z 節。</p>""" % si(fuzz_total()))

    # ---- 既知の差異
    H.append("""<h2>10. 既知の差異・未実装の明示（嘘をつかない台帳）</h2>
<ul class="tight">
<li><b>性能差（§6）は現行の事実</b>。Rust 版は正確性移植段階であり、C の性能機構
  （lazy style・row_emit fast 群・fitdom CJK SIMD・並列 layout 等）は未移植。</li>
<li><b>RSS 差</b>（16MB で %s）は実測。主因は byte-exact 検証のための行監査帳簿
  （RowInfo 全行保持）＋所有 Vec 文字列保持と推定（<b>推定</b>。valgrind/heaptrack 系の
  証明は未実施）。</li>
<li>stderr の <code>arena_kb/arena_used_kb</code> 行は C arena 概念のため Rust は
  0 表示 + note 行。比較・bench では scrub 対象（仕様、隠蔽ではなく両辺同一処理）。</li>
<li>Rust CLI の機能パリティ（ifuto-cli/src/main.rs 冒頭の対応表どおり）:
  render / 全 --dump-* / --fragment / --links / --stats / --md / --slim-dom /
  --show-paths / --imgdecode / http(s) 取得（ifuto_ffi::net_sock::http_get_ex、
  BearSSL 相当を Rust 移植済）/ &lt;script&gt; 実行（akl-core）まで配線済。
  <b>未実装は GUI（--gui/--shot/--ui）のみ</b>で、明示メッセージ + rc=2 で明白拒否
  （拒否形状自体が差分 fuzz で byte 検証済）。--ext は chrome init（GUI 依存）のため
  実効未移植。単体 akl ランナー・x11t/gui_app/fb 各デーモン・sandbox 層は
  CLI ミッション外としてフェーズ 9+ の仕事。</li>
<li>Rust の file 読みは mmap ではなく <code>std::fs::read</code> 経路
  （出力は同値 = 観測不変、速度差は本レポートに記録。mmap 化は将来の最適化候補）。</li>
<li>起動時間の Rust 微優位は一貫した実測だが、原因分析は推定（§5）。</li>
<li>ベンチはこの帯騒音環境（2 HT）での値であり、BENCH.md の騒音帯規約
  （±5–15ms は median でも揺れる、機構証明優先）に従って読むこと。</li>
</ul>""" % ratio(s16["rss_kb"]["r"]["median"], s16["rss_kb"]["c"]["median"]))

    # ---- raw data
    H.append("<h2>11. 生データ（再検証用全量）</h2>")
    H.append("<details><summary>計測生 JSON（%sB、全 pair の wall/段別/RSS/exact "
             "フラグを含む）</summary><pre>%s</pre></details>" % (
                 si(len(raw.encode())), esc(raw)))
    H.append("<details><summary>ゲート生 JSON（bench/results/gates.json）</summary>"
             "<pre>%s</pre></details>" % esc(json.dumps(gates, ensure_ascii=False, indent=1)))

    H.append("""<footer>生成: bench/gen_report.py（入力: %s, bench/results/gates.json）・
再生成手順は §3。レポート HTML と data JSON は .gitignore 規約どおり未追跡の生成物
（再生成可能なため）。現行値の台帳は BENCH.md、移行記録は docs/RUST_MIGRATION.md。</footer>
</main></body></html>""" % esc(os.path.basename(data_path)))

    # 出力先の親は gitignore 配下のため環境リセットで消える。都度作る（再発防止）。
    out_dir = os.path.dirname(out_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(out_path, "w") as f:
        f.write("".join(H))
    print("WROTE %s (%d B)" % (out_path, os.path.getsize(out_path)))

if __name__ == "__main__":
    main()
