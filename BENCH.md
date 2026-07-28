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
