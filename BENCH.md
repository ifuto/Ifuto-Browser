# BENCH.md — Ifuto 計測ベースライン

**軽量は測定可能か、嘘つきかのどちらかである。** このファイルは Ifuto の公式ベースライン。
全数値は「このコンテナ（2 core / 4GB / Debian 12 / gcc 12.2）での実測」。他環境との絶対比較は無意味。
ラチェット規則: **同じハーネスで monotonic に改善させる。悪化したらコミット理由を説明責任あり。**

測定法は `bench/bench.sh`（`make bench`）。RSS は `--stats` 自己報告
（`/proc/self/status` VmHWM。外部ポーリングや ru_maxrss はこの環境で壊れているため採用しない）。

## v0.1 天井（自前のゲート）

| 指標 | 天井 | 実測 | 判定 |
|---|---|---|---|
| バイナリ（stripped） | < 300 KB | **80,248 B** | ✅ 3.7× 余裕 |
| コールドスタート | < 20 ms | **1.1 ms** | ✅ 18× 余裕 |
| サンプル文書 RSS | < 8 MB | **1.88 MB**（18KB 文書） | ✅ |

## ストレス実測（2MB 文書: 5000 段落 + CSS、width=120）

| フェーズ | 時間 | arena 累積 |
|---|---|---|
| read | 1.9 ms | — |
| parse | 9.8 ms | 6.7 MB |
| style | 8.3 ms | 8.4 MB |
| layout | 22.7 ms | 32.5 MB |
| render | 43.4 ms | 51.2 MB |
| **total** | **86 ms** | peak RSS **52 MB** |

## 既知の非効率（改善の優先順、ROADMAP v0.2 と対応）

1. **grid セル 8B/セル**（big で ~15MB）→ bit-packing で 5〜6B へ。render フェーズ支配項。
2. **IfSeg 32B/seg + 空白セグ分離**（big で ~13MB）→ 空白を後続語 seg に併合で個数半減。
3. **render emit の per-cell SGR 状態機械** → ランごとのまとめ書き出し（render 43ms の多分数割）。
4. **style O(規則×要素)**（big で 8ms）→ タイプ/クラス索引バケット。まだ支配項ではないので後送り（Amdahl）。

## 変更履歴

- 2026-07-28: v0.1 ベースライン確立。`ru_maxrss` はこのコンテナで `/bin/true` すら 10MB と報告する
  壊れた値であることを確認し、自己報告 VmHWM 方式に切替。

## v-chrome slice-1 天井（採択: CHROME_SCOPE §1、TUI クローム初回）

測定ハーネス: `make tuibench`（`bench/bench_tui.py` = 疑似端末 PTY 越し、
`bench/bench_tabmeta.c` = モデル直接用）。実測 2026-07-29、同コンテナ。

| 指標 | 天井 | slice-1 実測 | slice-2 実測 | 判定 |
|---|---|---|---|---|
| バイナリ合計（engine+chrome, stripped, LTO） | ≤ 200 KB | 121,392 B | **133,752 B** | ✅ 1.5× 余裕 |
| 空タブ UI 常駐 RSS（初回描画後 VmHWM） | ≤ 4 MB | 888 KB | **1,428 KB** | ✅ 2.8× 余裕 |
| コールドスタート→最初の描画バイト | ≤ 25 ms | 1.26 ms | **1.55 ms median** (n=7) | ✅ 16× |
| アンロード済み 50 タブのメタデータ | ≤ 2 MB | 14,720 B | **14,720 B**（不変、リーク傾き 0 B/195 周） | ✅ 139× |
| **50 タブセッション復元（遅延ロード込み）** | ≤ 100 ms | — | **0.11 ms**（active 即ロード含む厳しい定義） | ✅ 900× |
| TUI アイドル CPU / 描画出力 | 0 % / 0 B | 0.00 % / 0 B | **0.00 % / 0 B** | ✅ |

slice-2 のコスト分析（正直な注記）:
- 空タブ RSS +540 KB と冷間 +0.3 ms はストア初期化（mkpath）＋起動時 autosave
  の一時バッファ（session 生成 arena 32KB・GenBuf）＋履歴 append の syscalls。
  VmHWM は「ピーク」なので一時的な確保も計上される。2.8× 余裕の内側なので
  最適化は後送り（Amdahl 的に支配項ではない）。
- 測定ハーネスは PTY 常時ドレイナスレッド化（write ブロックによる偽陰性を排除。
  tui_smoke.py と同じ教訓: 10KB 超フレーム write がブロックされると app が
  入力を読まず、バイトロストの見せかけ症状になる）。
- 同コミットで単体テスト **1675 checks / 0 fail**（fake-fs によるストア往復・
  遅延ロード・検索・ブックマーク冪等の全パス）、PTY e2e **15 checks PASS**
  （セッション復元/グループ/検索/ブックマーク/--show-paths 含む）、golden PASS、
  fuzz 500iter 0 crash、WPT tree-construction **60.0% (1036/1726) 維持**。
