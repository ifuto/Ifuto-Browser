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

| 指標 | 天井 | 実測 | 判定 |
|---|---|---|---|
| バイナリ合計（engine+chrome, stripped, LTO） | ≤ 200 KB | **121,392 B** | ✅ 1.65× 余裕 |
| 空タブ UI 常駐 RSS（初回描画後 VmHWM） | ≤ 4 MB | **888 KB** | ✅ 4.5× 余裕 |
| コールドスタート→最初の描画バイト | ≤ 25 ms | **median 1.26 ms** (n=7, min 1.22 / max 1.32) | ✅ 19× 余裕 |
| アンロード済み 50 タブのメタデータ | ≤ 2 MB | **14,720 B**（294.4 B/tab, malloc usable 相当込み） | ✅ 139× 余裕 |
| TUI アイドル CPU | 0 % | **0.00 %**（1.2 s 無入力の utime+stime 増分 0） | ✅ |
| アイドル中描画出力（INV-5 厳密形） | 0 B | **0 B** | ✅ |
| 50 タブセッション復元（遅延ロード） | ≤ 100 ms | 未測定 — slice-2 のセッションストア依存 | ⏸ |

測定法の正直な注記:
- コールドスタート値は Popen 直前→PTY 最初バイト。**fork/exec/動的リンク/初期描画を
  全て含む上限値**であり、真の UI 常駐時間はこの値以下（推定、分解未測定）。
- 空タブ RSS は `ifuto --ui` を PATH 実行した PTY 子プロセスの VmHWM。
- 50 タブメタは mallinfo2 uordblks 差分（glibc は free 後も uordblks が戻らない
  アーティファクトあり。最小実験で確認済み）。リーク判定は**周回傾き**:
  create/destroy 200 周で in-use 増分 **0 B**（切片ノイズ 4KB 以内を許容）。
  加えて `make test` のクローム系テストは ASan+LSan 通過（終了時リーク検出ゼロ）。
- 同コミットで単体テスト 1640 checks / 0 fail、PTY e2e スモーク（`make uitest`）
  PASS、golden PASS、fuzz 500 iter 0 crash、WPT tree-construction 60.0%
  (1036/1726, skipped 208) 維持。
