# ARCHITECTURE.md — Ifuto Browser

「史上最強の軽量ブラウザ」を**考えなしの全部入りではなく、効用関数の最大化**として建てる。
この文書は設計の固定点・不変条件・明示した近似・残存攻撃面の唯一の真実源（single source of truth）である。

```
入力（untrusted なバイト列。常に敵意を仮定）
  → UTF-8 デコード（FFFD 回復）          src/utf8.c
  → HTML トークナイザ                    src/html_tok.c
  → ツリービルダ → DOM                   src/html_tree.c, src/dom.c
  → CSS パース → カスケード              src/css.c
  → レイアウト（整数セル座標）           src/layout.c
  → ペイント（セルグリッド・ソフトラスタ）src/render_ansi.c
  → 発行（ANSI/プレーン）・CLI           src/main.c
```

## 1. 効用関数とウェイト（意思決定基準）

`w_footprint = w_correctness = 0.20` を双頭とし、`w_security = 0.16`。
速度・互換性・足場の軽さのトレードオフは列で示す:

| 選択 | 採用 | 棄却 | 根拠 |
|---|---|---|---|
| エンジン | 全部自作（C11） | Chromium 系・litehtml・WebView | ユーザ指示 + フットプリント支配 |
| メモリ | ページ単位 arena | per-object malloc/free + 所有権地獄 | dangling を構造的に不可能にする |
| プロセス | 単一プロセス | サイト分離マルチプロセス | v0.1 の攻撃面最小化のため。v0.5 で見直す（§6） |
| GPU | バックエンド境界 + ソフトラスタ先行 | いきなり Vulkan/D3D12/Metal | この Linux コンテナで検証不能なコードは書かない |
| JS | なし | QuickJS ではない（まだ） | DOM API が固まるまでは攻撃面だけ増える |

## 2. 不変条件（コードに課された法）

1. **パーサは止まらない**: あらゆる経路でカーソルは単調前進。無限ループは構造的に禁止。
2. **arena ポインタはページ寿命**: 個別 free なし、dangling なし。OOM/異常サイズは `if_fatal` で即死（黙った破壊より fail-fast）。
3. **型レベルで無効状態を排除**: IfLen の unit 列挙、IfStyle の display 列挙。文字列比較で状態を表さない。
4. **untrusted 入力の上限**: `IF_MAX_INPUT_BYTES`（64MB）、`IF_MAX_DOM_NODES`、スタック深さ上限。
5. **style 未適用でも layout/render は落ちない**（`IF_STYLE_FALLBACK`）。
6. **測定の捏造禁止**: ベンチはハーネス付き・方法は BENCH.md に明記。壊れた計測は報告しない。

## 3. 明示した近似（嘘をつかないための台帳）

| 近似 | 影響 | 脱却計画 |
|---|---|---|
| レイアウトは整数セル座標（8px×16px グリッド） | 端末では正確。Sub-px 表現不能 | px 精度レイアウトを GPU 化と同時に別経路で追加（IfBox API は維持） |
| 縦マージン相殺は兄弟間のみ | 親子貫通ケースで ±1 行 | v0.2 |
| マーカーは親 padding 帯に描画（2〜4 セル） | `list-style-position: inside` 未対応 | v0.2 |
| テーブルは display:block として積む（ツリー側も tbody/tr 暗黙挿入なし） | 表が縦に崩れる | table 挿入モード群（v0.2 継続課題） |
| ~~`<title>`/`<textarea>` は RAWTEXT~~ → **RCDATA 化済み**（文字参照デコード + textarea 先頭 LF 規則） | 解消 | 済 |
| `display:inline-block` → inline、table 系 display → block | 一部サイトで組版差 | v0.3+ |
| ~~コメント/doctype は DOM に残さない~~ → **保持に変更**（PI `<?target data?>` も PI として区別） | 解消 | 済 |
| foreign content（SVG/MathML） | コアは実装済み: ns 入域/復帰・integration points・breakout・case 調整・CDATA・serializer prefix。未実装: `<select>` 等の foreign 内特殊規則、annotation-xml の encoding 以外、`</br>` 等の細則 | v0.2 継続 |
| template は通常要素（content フラグメント分離なし、serializer で擬似表示） | template.dat が取りこぼす | v0.2 継続課題（content 分離） |
| 奇行系: quirks モードなし、foster parenting なし、adoption agency は近似 | 壊れた HTML でのツリー差異 | tree-construction 採点中（§6.1）。table modes + foster + AAA が最大の残塊 |
| 入力 CR/CRLF → LF 正規化なし | `\r` 含む入力でツリー差異の可能性 | v0.2 継続（入力前処理に正規化層） |
| isindex/`<title>` の body→head 移動等非推奨規則 | 該当テストのみ失敗 | 非推奨要素は後回し（実害なしと判断） |
| フォントは端末依存・等幅のみ、斜体/太字は SGR 属性 | 実フォントレンダリングなし | v0.3（GPU/ソフトピクセル） |
| 画像・メディアはプレースホルダ | 実デコードなし | v0.3 |
| http/https 未取得（ファイルのみ） | ネットワークなし | v0.2: HTTP/1.1 → v0.3: TLS(BearSSL 等の battle-tested 物。自作 TLS は禁止） |

## 4. セキュリティモデル（防御的。現状の残存攻撃面を隠さない）

**信頼境界**: 外部コンテンツ → エンジン → 端末エスケープ出力。

- パーサ/レイアウト/レンダラは全経路 ASan+UBSan+stack-protector+fuzz 常時。
- **現状の残存攻撃面（正直に）**:
  - 出力端末への制御シーケンス混入: HTML 中の ESC 等の制御文字はグリフ幅 0 として**描画せず捨てる**が、
    デコード済みの C0/C1 が `put_cp` 経路に入らないことを fuzz で継続確認が必要（要テスト追加）。
  - DoS: 巨大/深い文書はカウンタ上限で打ち切る。`IF_MAX_DOM_NODES` 超過で切断（描画は継続）。
  - 単一プロセスのため任意コード実行まで至るバグは即ユーザ端末。だからこそ fuzz・sanitizer を文化にする。
  - CSS セレクタのバックトラック爆発: depth 4096 上限内だが、悪意あるネスト `div div div … {}` は
    要素×深さで増える。サイト分離移行時に per-セレクタ評価上限を入れる（ROADMAP）。
- **やらないこと**: 自作 TLS、自作暗号、外部コードの盲目的取り込み、exploit の開発。

## 5. バックエンド境界（GPU への脱却路）

```
layout（座標系は差し替え可能）──→ [backend 境界: 矩形/文字/罫線の描画プリミティブ列]
                                     ├─ v0.1: セルグリッド（ソフト）render_ansi.c  ← 現在ここ
                                     ├─ v0.3: ソフトピクセルラスタ（フォントラスタライズ込み）
                                     ├─ v0.3: Vulkan（Linux で実測可能になってから）
                                     ├─ v0.4: D3D12（Windows CI ホスト）
                                     └─ v0.4: Metal 4（macOS CI ホスト）
```
境界の設計原則: 描画命令列は**値の列**（ツリー参照をbackendに持ち込まない）。
これがなければ Vulkan/D3D12/Metal の三又は抽象化詐欺になるところを、実装が 1 つでも検証できない時点で
インタフェースを凍結するのは早すぎる一般化——**まず 2 個目の実装（ソフトピクセル）を作る時に凍結する**。

## 6. ロードマップ（マイルストーンと検証基準）

| 版 | 内容 | 完了の客観基準 |
|---|---|---|
| v0.1 ✅ | 垂直スライス: パース〜端末描画。テスト・fuzz・ベンチ・ゴールデン | 本コミット |
| v0.2 | **適合性マイルストーン（進行中）**: WPT tree-construction 採点ハーネス `make conformance`（`tests/wpt-tree-construction/`、WPT@0acb81f ピン留め・ベンダー済 61 ファイル 1,934 テスト）。公開スコアの推移: 41.4%（714/1726, 初期採点）→ 49.0%（PI・RCDATA・comment/doctype・終了タグ規則）→ 56.5%（foreign content コア）→ **60.0%（1036/1726, 2026-07-28 現在）**。分母は fragment(#document-fragment) 208 件を skip した実行可能件数。残塊: table 挿入モード群+foster parenting+AAA（最大）、template content、frameset、script-escape 状態、select/option。以降: マージン相殺親子貫通、テーブルレイアウト、HTTP/1.1 クライアント（plaintext のみ。防御的パーサ付き） | 合格率の単調増加（後退は台帳に理由を記す） |
| v-chrome ✅ | **TUI クローム（ユーザ決定で v0.2 より前倒し、2026-07-29）**: slice-1 = タブ・オムニボックス・スクロール・リンクフォーカス・ステータス帯（C1 メモリ計装）。slice-2 = 永続化（C2: session/history/bookmarks を tmp→rename→fsync で単一 dir 原子管理、遅延ロード復元 50 tab 0.11 ms 実測）・`?`タブ検索（INV-8 完全形）・`@`グループ最小形（#11 の TUI 表現）・`--show-paths`（INV-9）。天井は CHROME_SCOPE §1、全項目実測済（BENCH.md）。残: グループ折りたたみ・プリセット（INV-4）・履歴/ブックマーク連携のオムニボックス候補（INV-3）・ダッシュボード（§8 照合の opt-in 標準プリセット）は slice-3+ | 天井全項目を実測で満たす + PTY e2e 15 checks 緑 |
| v0.3 | ソフトピクセルラスタ + backend 境界凍結 + 画像デコード（まず BMP/PNG 静的、ImageMagick には頼らない）+ Vulkan（このコンテナで headless 検証可能なら） | 同一文書のセル版・ピクセル版で視覚一貫 |
| v0.4 | QuickJS 埋め込み（DOM 最小 API: querySelector・textContent・style 書換）。CI ホスト準備後 D3D12/Metal 4 | JS からのレイアウト再計算が end-to-end で動く |
| v0.5 | プロセス分離（サイト分離）＋ seccomp サンドボックス。footprint/RSS への影響を数値で示してから適用 | 攻撃面評価を文書化、境界テスト追加 |
| 常時 | footprint ラチェット・fuzz 拡大・依存ゼロ維持 | BENCH.md 更新 |

## 7. 性能モデル（Amdahl の現在地）

v0.1 の支配項は render（~43ms/2MB）→ layout（~23ms）。parse 10ms・style 8ms は未支配。
クリティカルパス外の最適化はしない。次に攻めるなら: render emit バッチ化・セル構造の bit-pack・空白 seg 併合。
平均ではなく大文書のワーストケースを見るのがこのプロジェクトの作法。

## 8. 開発規律

- ビルドは `-Wall -Wextra -Wshadow -Wstrict-prototypes -Wwrite-strings` 警告ゼロ維持。
- 常時ランタイム検証: `make test`（ASan+UBSan）、`make golden`（厳密 diff）、`make fuzz`（mutation + ASan）。
- この環境で検証不能なもの（GUI/GPU/他 OS）は「未検証」と明記して先送りする。**未検証コードを成果に数えない**。
