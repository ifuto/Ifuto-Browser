# Ifuto Browser — 他ブラウザとのベンチ比較（2026-08-07 調査）

## 前提（比較の誠実性について）

- **ifuto 側の数値は全て実測**（この箱 = 1 物理コア × 2HT の弱コンテナ、C11 自作エンジン、
  JIT なし・GPU なし・ネットワークスタックなし）。測定法は BENCH.md。
- **他ブラウザ側は公開ベンチ記事の引用値**で、各記事の測定機は M2/M3/M4 級 Mac や
  デスクトップ PC（本箱の数倍の単コア性能）が多い。機材差を消せる構造比較（「何に
  メモリ/時間を払っているか」）と、同じ物差しが作れる項目（MB 単位の処理律速）に絞って論じる。
- 表示は「実測 / 引用 / 推定」のラベル厳守。ifuto の未対応機能領域は詐称せず「測定不能」と明記する。

## 1. 巨大文書パイプライン（本プロジェクトの主戦場）

**被験文書**: 16,777,395B の Markdown（131,090 ブロック、1,616,804 DOM ノード相当）。

| 指標 | ifuto（実測・median） | 大手ブラウザ（引用・機材は数倍強い） |
|---|---|---|
| 全工程 total | **123.10ms**（warm 帯 120.1–123.7, n=7） | — |
| parse | 55.0ms → **3.3ms/MB ≈ 304MB/s** | Chrome「Parse HTML」**約 7ms/MB ≈ 152MB/s**（DevTools 計測、MacBook 級）(1)(2) |
| layout + style | 52.6ms（style は lazy 化で 0.0ms） | peterbe 119KB 文書: style 43ms + layout 386ms（parse 94ms の 4.6 倍が style+layout に溶ける実例）(3) |
| render | 15.5ms | — |
| 全工程律速 | **7.4ms/MB ≈ 136MB/s** | parse だけで 6–7ms/MB。style/layout/paint を加えた全工程は **推定** 0.5–2s 級（パターン(3)の比率を当てた推定。実測公開値なし） |

根拠と注意:
- Chrome の「parse 6–7ms/MB」はネイティブ Blink が強い CPU で出す値。**ifuto は数倍弱い
  1 物理コアで parse 3.4ms/MB**。ハード諸元を揃えると構造的に 2 倍以上の差がある。
- style+layout が document 処理の真の支配項である点は (3) の実測（94 → 43+386ms）が
  示すとおりで、ここに ifuto の「lazy computed style（style 段 0ms）」が効いている。
- 現実の巨大文書の証左: 7MB の HTML を DOM 挿入したケースで Chrome/Edge が
  out-of-memory になる報告（2018、Stack Overflow）(4)。ifuto はその 2.4 倍の文書を
  123ms/225MB で完走する。

## 2. メモリ

| 指標 | ifuto（実測） | 大手ブラウザ（引用） |
|---|---|---|
| アイドル常駐 RSS | **1.43MB**（空タブ UI） | Chrome **380–612MB**、Firefox **310–727MB**(5)(6)(7) |
| 未ロード 50 タブのメタ | **14.7KB ≈ 294B/タブ** | Chrome アイドル 1 タブ **約 80MB**（Site Isolation の生レンダラ 50–90MB が下限）(5) |
| 16.7MB 文書 全工程 peak RSS | **225.1MB**（入力の 13.5 倍） | 1.6M ノード級 DOM は **推定** 0.5–1.5GB 級（Blink/V8 の実測プロキシに基づく推定。公開実測なし） |
| 50 タブ時 | （上記メタ 14.7KB + 遅延ロード） | Chrome **約 6.5GB** / Firefox **約 3.8GB**(6)、別系統測定では Chrome 14.4GB・Firefox 8.8GB(7) |

## 3. 起動・サイズ

| 指標 | ifuto（実測） | 大手ブラウザ（引用） |
|---|---|---|
| コールドスタート | **1.55ms**（fork/exec→初描画） | Chrome **0.70s**（11 種中最速）〜 Firefox 1.93s(8)。別測定 Safari 0.6s / Edge 0.9s(9) |
| セッション復元 50 タブ | **0.11ms**（遅延ロード込み） | —（公開値なし。秒級の実感値が一般的） |
| バイナリ/インストール | **443.9KB 単一ファイル**（453,512 B。JIT なし自作 JS エンジン + `<script>` 実行含む） | Firefox **55MB**、Opera 70MB、Brave 85MB、Chrome 90MB、Edge 95MB(10) |

## 4. 負けている領域（正直に列挙する）

| 領域 | 大手ブラウザ | ifuto |
|---|---|---|
| Speedometer 3.1（Web アプリ応答性） | Chrome 42.7 / Safari 41.9 / Edge 40.8 / Firefox 35.7(11)。M4 で Chrome 52.35 が過去最高(11) | **測定不能**（`<script>` akl 実行 + 最小 DOM バインド v1 は 2026-08-08 に接続済だが、frameworks・イベント・querySelector 等の全面 API が前提のため） |
| WPT tree-construction 適合 | 3 大エンジンはほぼ全通過（引用の wpt.fyi 系統） | **100.0%（1,922/1,922、fragment 196 件含む。skip は script-on 12 のみ）** — 2026-08-08 到達 |
| JS 実行 / 画像・動画 / HTTP 通信 / GPU 合成 / セキュリティサンドボックス | 全保有 | **未実装または限定**（軽量法則による意図的範囲外） |

## 5. 構造分析（なぜこうなるか）

大手が払っているコスト——Site Isolation のプロセス毎レンダラ（50–90MB/タブの下限(5)）、
V8/JSC の JIT ウォームアップ、GPU 合成パイプライン、Compositing 用サーフェス——は
セキュリティと表現力の対価であり、文書レンダリング律速そのものには寄与しない。
ifuto の逆側の設計——**1 タブ 1 arena のバンプアロケータ・カーソル連結リスト（×2 成長の
死蔵根絶）・lazy computed style・セルグリッド直接ラスタ・slim-DOM**——は
文書処理の律速（parse/layout/描画）だけに全振りしている。
結果として「16MB 文書 145ms/233MB」と「同文書で Chrome 系が OOM 既報(4)」という
**桁の差**が出る。一方で (4)(11) のとおり機能面の遅れは明確で、両者は同じ商品ではない。

### 出典
1. DebugBear「How Quickly Can Chrome Parse HTML Code?」(2025-11) — 1MB あたり 7ms/152MB/s（低スペック端末 88ms） https://www.debugbear.com/blog/html-parser-throughput
2. Web Performance Calendar「Exploring Large HTML Documents On The Web」(2025-12) — MacBook 約 6ms/MB https://calendar.perfplanet.com/2025/exploring-large-html-documents-on-the-web/
3. peterbe.com「How much HTML is too much for optimal web performance」(2018) — Parse 94 / Style 43 / Layout 386ms（119KB, 4x スロットル） https://www.peterbe.com/plog/how-much-html-is-too-much-webperf
4. Stack Overflow「How can I speedup adding large amounts of complex HTML to the DOM」(2018) — 7MB HTML で Chrome/Edge OOM https://stackoverflow.com/questions/49496716/
5. Supercharge「Chrome RAM Per Tab in 2026? We Measured」(2026-06) — アイドルタブ ~80MB、レンダラ下限 50–90MB、200 タブ実測 https://www.superchargebrowser.com/library/chrome-ram-usage-per-tab-2026/
6. Supercharge「Firefox vs Chrome RAM Usage」(2026-06) — 50 タブ Chrome ~6.5GB / Firefox ~3.8GB https://www.superchargebrowser.com/library/firefox-vs-chrome-ram-usage-2026/
7. TabGroupVault「Does Firefox Use Less RAM Than Chrome?」(2026-05 測定) — Chrome idle 612MB、50 タブ 14,414MB etc https://tabgroupvault.com/blog/chrome-vs-firefox-vs-edge-tabs
8. ZDNet「I speed-tested 11 browsers」(2025-02) — 起動 Chrome 0.70s 最速 https://www.zdnet.com/home-and-office/work-life/i-speed-tested-11-browsers-and-the-fastest-might-surprise-you/
9. speed-drain「Best Web Browsers 2026」— Cold Start / Install Size / Page Load 一覧 https://speed-drain.com/blog/best-web-browsers-2026-comparison/
10. 同上 (9) の Install Size 欄。
11. SupaSidebar「The Fastest Browser for Mac in 2026」— Speedometer 3.1 (Chrome 42.7 / Safari 41.9 / Edge 40.8 / Firefox 35.7、M4 で Chrome 52.35) https://supasidebar.com/blog/fastest-browser-mac-2026
