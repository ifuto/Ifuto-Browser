# Ifuto Browser

史上最強の軽量ブラウザを目指す、全部自作のブラウザエンジンプロジェクト（C11・依存ゼロ。
**Chromium 由来コードは 0 バイト**。リンクするのは libc と libm のみ）。
現在地: **v0.3-dev** — エンジン + **GUI 一本化（2026-08-01: TUI は完全廃止。
UI は raw X11 GUI のみ。`--shot` で同一ラスタパイプラインのヘッドレス検証）** +
Markdown 表示（`.md` 自動検出）+
**slim-DOM**（画面描画に関係ない script/template 配下は DOM に構築しない。実ブラウズ経路で既定）+
**viewport 窓グリッド**（grid は文書全体ではなく可視窓のみ materialize）+
ローカル永続化（セッション・履歴・ブックマーク。全て tmp→rename→fsync の原子書換）。

```
HTML/MD(untrusted) → [md→html] → tokenizer → DOM ─slim 剃り→ CSS cascade → layout
      → cell-grid render ─→ ANSI (CLI --no-ansi / 検証)
                          └→ 5x7 raster strip → XPutImage (GUI)   ↑ 1 タブ 1 arena
```

## 実測（計測法: BENCH.md / `make bench` / `make tuibench`）

| 指標 | v0.1 (CLI) | 現在（v0.2, TUI + ストア込み） |
|---|---|---|
| バイナリ ifuto（stripped・LTO） | 80 KB | **214.6 KB**（219,776 B。新天井 240 KB。増分主体は md.c） |
| バイナリ ifuto-gui（同上 GUI） | — | **190.5 KB**（195,088 B。ldd は vdso/libc/ld のみ） |
| コールドスタート | 1.1 ms（spawn 込み） | **1.55 ms**（fork/exec→TUI 初描画、median） |
| 空タブ UI 常駐 RSS | — | **1.43 MB**（天井 4 MB） |
| アンロード済み 50 タブ メタ | — | **14.7 KB**（天井 2 MB） |
| 50 タブのセッション復元 | — | **0.11 ms**（天井 100 ms、遅延ロード込み） |
| TUI アイドル CPU / 描画 | — | **0 % / 0 B**（INV-5: read ブロックのみ） |
| 18KB 文書 | 1 ms / ピーク RSS 1.9 MB | 変わらず（同一エンジン） |
| 2MB ストレス文書 | 86 ms / ピーク RSS 52 MB | 同上（内訳と改善計画は BENCH.md） |

## ビルドと実行

```sh
make            # build/ifuto（リリース: -O2 LTO stripped。GUI 統合済み単一バイナリ）
make test       # 単体テスト 608k checks ×2 dispatch（ASan+UBSan+LSan 常時）
make guismoke   # GUI を X なしで検証（--shot ラスタ + 画素検査）
make golden     # 描画の厳密 diff テスト
make fuzz       # mutation fuzz + ASan
make bench      # サイズ/起動/時間/RSS の測定（CLI）
make conformance # WPT tree-construction 採点（現 97.3%, 1679/1726, 2026-07-31）

./build/ifuto --gui local_page.html        # GUI 起動（X11 必要。Ctrl+L/T/W/Q, 矢印, PgUp/PgDn）
./build/ifuto --shot out.ppm page.md       # ヘッドレス: 同パイプラインでフルラスタ PPM
./build/ifuto notes.md                     # Markdown は拡張子で自動検出（--md で強制）
./build/ifuto --slim-dom --no-ansi p.html  # CLI で slim-DOM（既定は full/適合保証）

./build/ifuto --show-paths               # 永続データのパス一覧（INV-9、副作用ゼロ）
./build/ifuto tests/pages/hello.html     # ワンショット ANSI カラー描画
./build/ifuto --no-ansi --width 100 file.html
./build/ifuto --dump-dom file.html       # パイプライン各段の検査
./build/ifuto --dump-layout --stats file.html
cat file.html | ./build/ifuto -
```

GUI キー: `Ctrl+L` オムニボックス、Enter 確定 / Esc 解除、`Ctrl+T` 新規タブ、
`Ctrl+W` 閉じる、Ctrl+Tab タブ送り、`j/k`・矢印・PgUp/PgDn スクロール、`q`/`Ctrl+Q` 終了。
（旧 TUI は 2026-08-01 に完全廃止。`--ui` は互換のため GUI 起動へ誘導する）

永続データ（全て `$IFUTO_HOME` > `$XDG_DATA_HOME/ifuto` > `~/.local/share/ifuto` の単一
ディレクトリ、`--show-paths` で確認可能）: `session.txt`（構造変化ごとに原子保存、
起動時に遅延ロード復元）、`history.tsv`（open 追記、512KB 超で縮退）、
`bookmarks.tsv`（URL 完全一致トグル）。cwd には一切書かない。

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
