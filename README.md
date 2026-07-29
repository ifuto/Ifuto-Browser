# Ifuto Browser

史上最強の軽量ブラウザを目指す、全部自作のブラウザエンジンプロジェクト（C11・依存ゼロ）。
現在地: **v-chrome slice-1** — エンジン（HTML/CSS サブセット → レイアウト → 端末描画）+
TUI クローム（タブ帯・オムニボックス・スクロール/リンク巡回・メモリメーター）。

```
HTML(untrusted) → tokenizer → DOM → CSS cascade → layout → cell-grid render → ANSI
                                          ↑ 1 タブ 1 arena（C1: 閉じる=destroy、正確なメモリ表示）
```

## 実測（計測法: BENCH.md / `make bench` / `make tuibench`）

| 指標 | v0.1 (CLI) | v-chrome slice-1 (TUI 込み) |
|---|---|---|
| バイナリ（stripped・LTO） | 80 KB | **121 KB**（天井 200 KB） |
| コールドスタート | 1.1 ms（spawn 込み） | **1.26 ms**（fork/exec→TUI 初描画、median） |
| 空タブ UI 常駐 RSS | — | **888 KB**（天井 4 MB） |
| アンロード済み 50 タブ メタ | — | **14.7 KB**（天井 2 MB） |
| TUI アイドル CPU / 描画 | — | **0 % / 0 B**（INV-5: read ブロックのみ） |
| 18KB 文書 | 1 ms / ピーク RSS 1.9 MB | 変わらず（同一エンジン） |
| 2MB ストレス文書 | 86 ms / ピーク RSS 52 MB | 同上（内訳と改善計画は BENCH.md） |

## ビルドと実行

```sh
make            # build/ifuto（リリース: -O2 LTO stripped）
make test       # 単体テスト 1640 checks（ASan+UBSan+LSan 常時）
make uitest     # TUI を疑似端末越しに駆動する e2e スモーク
make golden     # 描画の厳密 diff テスト
make fuzz       # mutation fuzz + ASan
make bench      # サイズ/起動/時間/RSS の測定（CLI）
make tuibench   # v-chrome 天井の実測検証（冷間開始/RSS/idle/タブメタ）
make conformance # WPT tree-construction 採点（現 60.0%, 1036/1726）

./build/ifuto --ui                       # 対話 TUI（空タブで開始。tty 必要）
./build/ifuto --ui file.html             # ファイルをロードして開始
./build/ifuto tests/pages/hello.html     # ワンショット ANSI カラー描画
./build/ifuto --no-ansi --width 100 file.html
./build/ifuto --dump-dom file.html       # パイプライン各段の検査
./build/ifuto --dump-layout --stats file.html
cat file.html | ./build/ifuto -
```

TUI キー（`?` で常時確認可能）: `o` オムニボックス（相対パスは現タブのディレクトリ基準、
`://` なら v0.3 待ちの通知）、`Enter` 開く / フォーカス中リンクを開く、`Esc` キャンセル、
`t` 新規タブ（空白）、`w` 閉じる、`]`/`[` タブ送り、`1`–`9` タブ直ジャンプ、
`Tab`/`S-Tab` リンク巡回、`j/k`・`d/u`・`PgUp/PgDn`・`g/G`・矢印/Home/End スクロール、
`r` リロード、`q` 終了（複数タブ時は二連打）。

対応するもの（明示）: HTML サブセット（暗黙タグ・rawtext・文字参照）・CSS
サブセット（type/class/id/universal、子孫・子結合子、カスケード+重要度+inline style）・
ブロック/インラインレイアウト・全角折り返し・リスト・罫線・pre・リンク収集。

**対応していないもの（嘘をつかない）**: JS、ネットワーク、画像デコード、フォント描画、
テーブル真正レイアウト、flex/grid、マルチプロセス・サンドボックス、GPU。
これらは ARCHITECTURE.md §6 のロードマップに検証基準つきで並んでいる。

## ドキュメント

- [ARCHITECTURE.md](ARCHITECTURE.md) — 設計固定点・不変条件・明示近似・残存攻撃面・ロードマップ
- [BENCH.md](BENCH.md) — 計測ベースライン（ラチェット規則つき）

## プロジェクトの規律（抜粋）

- 依存ゼロ（libc のみ）。警告ゼロ。ASan+UBSan 常時。
- 未検証のコードを成果に数えない。壊れた計測値は報告しない。
- 「軽量」は数値で語る。trade-off は列挙してから決める。
