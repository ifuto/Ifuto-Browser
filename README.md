# Ifuto Browser

史上最強の軽量ブラウザを目指す、全部自作のブラウザエンジンプロジェクト（C11・依存ゼロ）。
現在地: **v0.1 コアスライス** — HTML/CSS サブセットのパース → レイアウト → 端末描画まで。

```
HTML(untrusted) → tokenizer → DOM → CSS cascade → layout → cell-grid render → ANSI
```

## 実測 v0.1（計測法: BENCH.md / `make bench`）

| 指標 | 値 |
|---|---|
| バイナリ（stripped・LTO） | **80 KB** |
| コールドスタート | **1.1 ms**（spawn 込み） |
| 18KB 文書 | 1 ms / ピーク RSS 1.9 MB |
| 2MB ストレス文書 | 86 ms / ピーク RSS 52 MB（内訳と改善計画は BENCH.md） |

## ビルドと実行

```sh
make            # build/ifuto（リリース: -O2 LTO stripped）
make test       # 単体テスト 1504 checks（ASan+UBSan 常時）
make golden     # 描画の厳密 diff テスト
make fuzz       # mutation fuzz + ASan
make bench      # サイズ/起動/時間/RSS の測定

./build/ifuto tests/pages/hello.html      # ANSI カラー描画
./build/ifuto --no-ansi --width 100 file.html
./build/ifuto --dump-dom file.html        # パイプライン各段の検査
./build/ifuto --dump-layout --stats file.html
cat file.html | ./build/ifuto -
```

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
