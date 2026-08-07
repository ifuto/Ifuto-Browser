# BENCH.md — Ifuto 計測ベースライン

**軽量は測定可能か、嘘つきかのどちらかである。** このファイルは Ifuto の公式ベースライン。

## 2026-08-07: HTTP/1.1 取得（src/net.c）— パイプライン非接触の証跡

- 機能: CHROME_SCOPE 台帳最終ブロック「http は未対応」の撤廃（詳細は同ファイル）。
  150ms ミッションとの整合をここに固定する。
- ホットパイルへの接触面積: CLI ファイル経路の差分は read_all 冒頭の
  `strncmp(path, "http://", 7)` 1 回のみ（parse/style/layout/render のコードは不変）。
- 証跡: 16MB `--no-ansi` sha256 = d11680089da0fc6e58308fa19622865b19919e261b9caafa484baf4bc7321ab4、
  ANSI = e13eca16d35b8f0462d1698be10750f931599924d8f177414b3a13c68478d126
  （いずれも前ターン台帳と一致 = byte-exact）。5 発実測（現低速帯）:
  total 163.09/171.77/171.82/212.50/217.43ms — 帯ベースライン（~180-220ms）内。
  静寂帯判定値は前ターン台帳（145.2ms）を維持。
- 検証: run_tests/run_tests_switch 両種 609,198 checks 0 fail（test_http +81）、
  chk_oracle 12/12、golden 1/1、gui_smoke PASS（+12 = 44 checks。loopback サーバ
  実ソケット経由の file 版ラスタ完全一致を含む）、ifuto-asan で live fetch
  4 形態（CL/301/chunked/close）+ 16MB + dump-dom + --shot http 全クリーン。

### 同日追記: GUI リンクホバー（パイプライン非接触）
- x11t.c MotionNotify + gui_app.c のみ（エンジン層ゼロ差分）。16MB byte-exact
  sha256 = d11680089da0fc6e58308fa19622865b19919e261b9caafa484baf4bc7321ab4 一致。
  gui_smoke +7 = 51 checks（IF_SHOT_HOVER 契約: 差分 2 帯限定・範囲外==無ホバー
  バイト一致・2 回确定性）。run_tests 両種 609,198 不変、ASAN shot/hover-shot
  /16MB クリーン。

## 2026-08-07: flatten 経路リンク span 収集（複数行リンクのクリック可視化）+ shard B span y 未シフト修理

- 機能: 複数行 wrap へ逃げた <a>（fused 失敗 → flatten 経路）の表示矩形を行ごとに
  収集。piece 区間 [p0,p1) を flatten 時に DFS preorder で記録（IfLinkPrec）、
  wrap 連鎖で大域 seg 添字（w.seg_hi + w.n_segs）へ写像、行ログとの厳密交差で
  行別矩形を解決。span 追記は link_span_add に一点化。GUI の全リンクが
  クリック/フォーカス可能になった（レイアウト的に行またぎしないリンクのみ、の
  制約を解消）。
- 併発修理（本機能の前提調査で同定した既存欠陥）: 並列 shard の tree モード
  マージで shard B の span y が未シフトだった（lines/deco/box と同じ対象・
  同じ量の hA 補正を追加）。回帰テストは serial/parallel の span 厳密一致
  （修正除去で FAIL することを機械確認済み）。
- コスト規律: 線形 CLI は no_boxlink ゲートで収集自体が不発（prec 未記録・
  piece ループ分岐は f.n_prec==0 で完全死コード化）。hot path に残るのは
  wrap_end_line の seg_hi 1 store/行のみ。動的割当なし（prec は pieces_scratch
  と同規約のスクラッチ再利用）。
- 機械オラクル: 16MB 出力は no-ansi / ANSI とも新旧 sha256 一致（byte-exact）。
  chk_oracle 12/12、run_tests 両種 609,037 checks、golden、gui_smoke 32、
  ASAN 4 シナリオ + tree 経路直行プローブの ASAN/UBSan 全クリーン。
- 帯内 A/B（--no-ansi --stats、paired 7 組）: old median 177.87 / new median 165.93ms、
  pair 勝敗 4/7（有意判定不能。帯騒音 ±20ms 級のため退化なしは騒音内で確認。
  機構的増分上限 ≈0.2ms/852k 行 = 1 store/行のみ）。

## 2026-08-07: ANSI emit セルモデル経由の消去（row_emit_ansi_fast。前節の「要調査項目」解決）

- 構造確定（前節の疑いの答え）: ANSI の全 852,093 行が slow 全細胞経路に落ちていた
  原因は **body の白 BG deco が全行で active**（UA シート body{background-color:#fff}
  由来）で、no-ansi の fast path が全て `!ansi` 条件だったこと。BG は no-ansi では
  無影響だが ANSI ではギャップ細胞の pen を変えるため、受理条件を広げるには
  「BG 区間合成（active 追記順・後勝ち）→ bg ピース列駆動のギャップ発行」が必要。
- 実装: `row_emit_ansi_fast`（render_ansi.c）。受理 deco は BG/MARKER/HLINE のみ
  （BORDER は keep_pen 合成がセル依存のため slow 維持）。MARKER text は受理時に
  wrap_note_direct と同条件の glyph 検査 + 幅和==w を課す。pen 遷移は slow 走査と
  同一順（reset→bold→italic→uline→strike→fg→bg）に pen_emit で一点化。失敗は全量
  巻き戻しで slow へ（cell 合成の再解釈はしない安全側規約）。
- **機械オラクル**: 16MB 既定（ANSI）出力 120MB の **sha256 が新旧完全一致**
  （e13eca16…）。--no-ansi 16MB も一致（d116800…）。chk_oracle 12/12
  （idm-2mb.ansi 含む）、run_tests 両種 609,017 checks、golden、gui_smoke 32、
  ASAN 4 シナリオ全 exit 0・エラー 0。
- **実測（低速帯・paired A/B 7 組・total/emit とも 7/7 で new 優位）**:
  既定 ANSI total: old median 834.88 → new median 242.08ms。
  emit: old median 687.68 → new median 93.37ms。
  RENDERPROF（rdtsc、構造読み）: slow 2,121 cy/行 → adir 297 cy/行（7.1x）。
  slow 残存 0 行。静寂帯での再計測は有効帯で実施予定。
- メモリ: 動的割当なし（runs[64] ≒2.5KB・pc[66×2] ≒1.6KB のスタックのみ。
  「使わなければ使わないほど良い」公準維持）。

## 2026-08-07: GUI リンクのマウスクリック（IfLSpan ヒットテスト）+ 低速帯の記録

- 機能: IfLink に IfLSpan 表示矩形列（木構築モードの fused-fit 成功・単行 ifc 内 <a>
  のみ収集。線形 CLI は no_boxlink で収集自体を skip）。GUI 左クリックでヒットテスト
  → 相対 join / 絶対パスで遷移（http(s)/anchor は未取得 → ステータス表示）。
- 計測影響（CLI 16MB 線形経路）: 差分は collect の 2 store/リンクのみ。
  base/new バイナリの 16MB 出力は **byte-exact 一致**。帯内 A/B（--no-ansi --stats）
  base median 182.3ms / new median 179.2ms で退化なし（帯騒音 ±20ms 級、
  機構的限界 ≈0.1ms のため符号検定は不問）。peak RSS 差分 median +16KB（arena
  成長の capacity 余剰が吸収）。
- **帯環境メモ（再現条件の留保）**: 2026-08-07 02:00 帯は全体低速。
  静寂帯 145.2ms を記録した旧バイナリ（/tmp 残存）を再実行しても total 1490ms、
  すなわち退化はコードではなく箱。**150ms 判定は静寂帯でのみ有効**と台帳に明記する。
- 発見（未改修・次候補）: 既定（ANSI, isatty 無検査で常時 ON）の emit は 120MB 生成で
  同帯 710-820ms（旧バイナリでも 723ms、IF_RENDER_PAR=0 でも不変 → 並列起因でない）。
  一方 --no-ansi は 16-18ms で静寂帯 21ms と同等。dd 対照（120MB を bs=4K-1M で
  /dev/null へ 20-33ms）より write システムコール律速ではない。ANSI 経路の
  バイト生成効率（SGR 状態遷移・行バッファ運用）に実質非効率が残存する疑い。
  150ms ミッションの公準メトリクス（--no-ansi）外のため優先度は次点だが、
  既定 CLI の実用速度として要調査項目に上げる。

## 2026-08-06: GUI 自前全角フォント（61f8acf）+ arena 16MB ブロック不採用

- gui: 自前 16x16 全角フォント（gen_font16.py 8x8 手設計 → 2x 拡大・濁点合成、215 字）導入。
  バイナリ +4.1KB（301,816B。214.6KB 天井は v0.2 時点の記録で lazy style 等で既に超過、
  本フォント起因の増分は 297,720→301,816B のみ。天井の再設定は次回台帳整理で実測確定）。
- 不採用（計測で反証・再挑戦禁止）: arena 既定ブロック 8→16MB。
  paired median +2.63ms **悪化**（11 ペア中 3 のみ改善、符号検定で棄却）。
  ブロック単位の fault 集中化で 8MB 時点が既に最適点。reserved 増も重ねて不採用。
- 不採用: `-O2 → -O3`（REL）。2 回 paired（中央値 −2.47/−0.59ms、符号計 13/22）で
  有意でない。O3 バイナリは oracle 12/12 で出力同一確認済だが速度証拠が弱く不採用。
  なお当日の箱は ±40ms 級の騒音帯（median 変動が単体実行差を啜る）で、
  微小差の timing 計測は静寂帯まで凍結する。
- 環境教訓: 2026-08-06 13:34 sandbox 再起動で git が Initial commit へ巻き戻り。
  認証復旧後に作業ツリーから消失 4 コミットを再構成して push（内容ロスゼロ、
  chk_oracle 12/12 で検証）。再発時は remote=唯一の真実として即 fetch→再構成。

## 2026-08-03（午後追記）: 150ms ライン到達 — lazy style・IfRLine チャンク・引用コピー消去

現在地（同一箱・午前と同じ計測規約、median of 7、`--no-ansi --stats`）:

| stage | ms (median) | 目標 | 状態 |
|---|---|---|---|
| read | 0.01 | — | mmap 相当 |
| parse | 61.4 | 50 | 引用ストリップのコピー消去込み。2-way 並列（HT ~1.4x） |
| style | 0.0 | 40 | **段消去**（lazy style で layout 走査に畳み込み） |
| layout | 74.9 | 40 | lazy style 吸収 + IfRLine でも残る本体 |
| render | 15.2 | 10 | rline カーソル化込み |
| **total** | **151.6** | **150** | **≪達成圏≫（quiet 帯 141.9-149.6。騒音帯は 175 まで揺れる）** |

午後の確定分（全てオラクル sha256・run_tests×2・golden・gui・ASAN で機械ロック）:
- **lazy computed style（style 段の消去）**: computed style は (tag, parent_st, root_fs)
  の純粋関数（UA シートはタグセレクタのみ + md に author sheet は存在し得ない）。
  全面走査（15ms・874k visits）をやめ layout の DFS 訪問時に参照箇所だけ解決。
  解決規則は st_resolve_memo に一点化（compute_walk と同一手続き）。並列 shard は
  arena 別 ctx。**paired median −18.1ms**（ELEMENT 二重解決の 1 回化込み）。
  style=0ms 表記は「畳み込み」であり、決して style 計算をサボっていない（値は同値）。
- **lines ログの IfRLine(24B) 値チャンク連結リスト化**: sweep が読むのは
  y/segs/n_segs/flags のみと全 src 監査で確定 → IfBox LINE（64B×611k）と IfBox**
  ログを消去。串刺し arena の ×2 成長チェーン死蔵も併発で根絶。
  **layout arena 267,948 → 214,017KB（−54MB）。paired median +10.4ms 高速。**
- **引用ストリップの中間コピー全廃**: `> ` 剥がし＋b_finish＋再分割（コピー 2 系統）を
  Ln 副窓の直接再帰に置換（同値性はコード内証明注記）。ブロック構造コスト ~2-4ms。
- **slim attrs（M_RENDER 限定）**: A[href]/IMG[alt] 以外を pend→arena 複製しない。
  時間効果は誤差内（コーパスの属性体量が小さい）だがメモリ最小化法則として採用。
- 不採用（計測で反証、記録だけ残す）: **MADV_POPULATE_WRITE 事前 populate** は
  paired median +8ms 悪化（全 fault を直列化し並列 shard との overlap を殺す）。
  **-march=native** は ±0（AVX2 は既に要点で手動導入済み+可搬性維持のため不採用）。

memory 指標（同一バイナリ系統の arena_kb 系列: 決定的）:
parse 151,552 → 147,456KB、style 155,648 → 147,456、layout **272,044 → 214,017**。
peak RSS も同系で縮小（arena 272→214MB 減に追従）。

## 2026-08-03: 16MB IDM パイプライン総力戦 — fitdom 融合・並列 render・並列 parse/layout

目標（ユーザー指示）: total < 150ms、内訳目安 parse 50 / style 40 / layout 40 / render 10。
現在地（このコンテナ 1 物理コア×2HT 安住状態、median of 7、`--no-ansi --stats`）:

| stage | ms (median) | 目標 | 状態 |
|---|---|---|---|
| read | 0.01 | — | mmap 相当 |
| parse | 59.7 | 50 | 2-way 並列済（HT 限界 ~1.4x） |
| style | 15.0 | 40 | **目標内**（UA-only memo + intern。18 unique styles） |
| layout | 95.8 | 40 | 残課題の本体。2-way 並列済（~1.75x が効く） |
| render | 16.7 | 10 | seg 直行 N 本化 + 2-way 並列 sweep で 24.8→16.7 |
| **total** | **187.2** | **150** | **残 −37ms（layout が −56 の壁）** |

本日の確定分（全て oracle 12/12 sha256・run_tests×2 608,259/0・golden・gui_smoke・ASAN で機械ロック）:
- **fitdom（IFC の DOM 直接走査融合）**: flatten/pieces 配列を成功時（92.2%）に全廃。
  失敗時は seg LIFO rewind + 全状態復帰で従来経路へ無痕フォールバック。
- **並列 render**: sweep を [r0,r1) 区間独立化（分割点での deco/li 状態一意性を証明）、
  no-ansi のみ 2-way。/dev/null への write はユーザ領域を読まないため flush 実質 0cy。
- **geom (st,w) 1-entry 直前メモ**、fitdom 内 ASCII ラン SSE2 一括分類、
  **if_utf8_band_w2**（CJK 幅 2 を lead バイト確定。全 512K 妥当 3B 組を網羅検証し
  subset 性を機械証明。U+3040 穴を修復）、ok3 圏内 direct2 の恒等 no-op 論証による除去、
  mnew 重複判定除去、段落継続ゲートの死コード（'|' 全走査）除去。
- **style 2-way 並列は不採用**（正直な記録として残す）: body 直下 2 分割 + 専用
  intern/cache/arena で値同値（visits 一致まで検証）まで作ったが、218k の部分木呼出コスト
  （実測 ~220cy/call）が serial walk の利得を上回り **serial 18ms に対し ~37ms と 2 倍劣後**。
  全コード撤去。threading が効くのは遅延束縛型（layout/parse）であって、メモ化済みの
  軽量走査では呼出粒度の壁が支配になる——計測が負けを教えた案例。

環境注記: このベンチ箱は 1 物理コア×2HT（LCG spin で裸スケール 0.99x を確認済）で、
wall clock は隣接テナントで ±10-30% 揺れる。報告は quiet 状態の median、最適化判定は
rdtsc ゾーンの cycles を一次情報とする。HT が効くのは遅延束縛型（layout ~1.75x）、
 dependency-chain 型は伸びない（LCG 0.99x）。

## 2026-08-01: raster backend 起動時自動判定（CPU/GPU 判定の正直な実装）— 5.2× 高速 kernel を起動ごとに選定

ifuto://settings の約束「CPU/GPU 自動判定」を実装。GPU は製品法則（ldd = linux-vdso/libc/ld (+libm)
のみ）上 DRM ioctl 非接続なので、判定の実体は「CPU raster fill kernel を起動時 microbench で
この端末実測選定 + GPU ノード有無の検出表示」。決定と全候補の実測値は ifuto://memory に起動ごと表示。

候補 4 kernel（fill 32bpp、全て scalar 基準と bit-exact 一致を tests/test_raster.c が
オフセット 0-8 × 長 0-65 × 色 8 種で相互証明、610k checks）:

| kernel | 実装 | 白bg(均一) | 非均一色 | score |
|---|---|---|---|---|
| u32x1(scalar) | 1px/store | 12,737 MB/s | 12,619 MB/s | 12,702 |
| u64x2(8B) | 8B store ×2 unroll | 49,302 | 47,107 | 48,643 |
| **u64x8(64B)** | 64B/iter | **65,535** | **65,607** | **65,556** ← 選択 |
| smart(memset) | 均一色→glibc memset | 50,903 | 45,638 | 49,324 |

（このコンテナ 2c/4G、-O2 -flto 製品フラグ相当、512KiB バッファ warm、median of 3 runs
ではなく min-of-2trials × reps。起動ごとに再測定するため機種差はこの表ではなく実行時決定が正）

- score = 0.7×白bg + 0.3×非均一（ページ bg 全面塗りが支配的な加重）。
- microbench 総コスト: **約 7ms/起動**（calibration つき 0.4ms/trial×2、8 測定）。GUI main() 先頭で起動時に
  実行。TUI バイナリは冷間起動を守るため ifuto://memory 初回表示時に遅延実行（冪等 cache 済）。
- 効果の実体: fb_rect（GUI の全 bg/glyph 塗り）が選択 kernel 経由。scalar 比 **+30%〜5.2×**
  （色と幅に依存。1000px 幅 1 行は 4KB で 65GB/s なら 60ns/行）。
- smart(memset) は理論上均一色で最有力だが、この機械では u64x8 が上（glibc memset の小規模
  オーバーヘッドと store port 飽和她)。逆転する機種ではそちらが選ばれる——それが実行時選定の存在意義。
- 検定: gui_smoke（deterministic raster / non-white coverage）PASS、ldd 両バイナリ純粋維持。

## 2026-08-01: 巨大 IDM（Markdown 文書）メモリ正確計測 — 係数と 3.3GB 外挿、および正直な現在地

ユーザ要求: 「通常ブラウザで 3.3GB のメモリを食う MD ファイルをこのブラウザで読んだ際、
メモリがどのくらい減少するかの**正確な数値**」。

**先に正直な結論**: 現行設計では 3.3GB 入力は **512MB budget で綺麗に拒否** する
（rc=1、`ifuto: input too large`。OOM panic でも暗黒 swap でもない設計上の fail-stop。
600MB 入力で実測検証済）。開けない以上「3.3GB で何 GB 減った」の実測は存在せず、
あるのは **測定済み区間の係数からの外挿（推定）** のみであり、推定は推定と明記する。
**現在地では巨大文書の RSS/入力比は通常ブラウザの前提値（1.0×）より悪い**（下表）。
本丸対策は v0.3 台帳: 入力 compaction + md→DOM ストリーミング + style 配列 chunked 化。

### 実測（このコンテナ、2026-08-01。C 製 rssrun の ru_maxrss、単発。前セッション値との偏差 <0.3% で再現性確認済）

| 入力 (Markdown) | render RSS (KB) | domdump RSS (KB) |
|---:|---:|---:|
| 16 MiB  | 743,028 | 405,652 |
| 64 MiB  | 1,754,500 | 1,110,676 |
| 128 MiB | 2,396,484 *(前セッション) * | 1,752,612 |
| 192 MiB | 2,972,728 | 2,328,896 *(前セッション)* |
| 256 MiB | — | 3,036,300 |

- 傾き（実測テール、domdump 64→256MiB）: **約 10,029 KB/入力MiB ≈ 9.8 MiB/MiB**
- 傾き（render 64→192MiB）: **約 9,517 KB/入力MiB ≈ 9.3 MiB/MiB**
- 切片は約 220–400MB 級（md→HTML 中間表現・DOM・style 配列の固定構造）。

### 3.3GB 入力への外挿（**推定**。実測ではない）

domdump 傾きで 256→3379MiB を伸ばすと ≈ 34.4M KB ≒ **約 35 GB RSS（推定）**。
このコンテナの物理 RAM (4GB) を大きく超過 → 現行は途中で死ぬのではなく
**512MB budget が先に綺麗に止める**（fail-stop が設計通り効いている証左でもある）。
通常ブラウザ側の「3.3GB」はユーザ提示の前提値であり、こちらで実測した値ではない
（この環境に chrome/firefox は存在しない）。

### この係数を良くする本丸（v0.3 台帳、未着手・順序付き）

1. 入力 compaction（DOM から生テキストへの逆参照を捨て、入力 mmap を手放す）
2. md→HTML 中間表現をストリーミング直結（全量の中間文字列を作らない）
3. style/layout 配列の chunked 化（IF_MAX_ARENA_ALLOC 単塊を避ける）

各項は「実測で係数が下がった」時点で初めて本ファイルに改善として記録する（推定の事前記載はしない）。

## 2026-08-01: akl vs V8（node v22.22.3、実測 median of 3、サブプロセス wall time）

参照: bench/akl_compare.py。akl は CLI 製品既定（CoJIT ON、seccomp 込み）。

| bench | akl | V8 --jitless (Ignition) | V8 full (TurboFan) | vs jitless | vs full |
|---|---:|---:|---:|---:|---:|
| empty  | 1.701ms | 28.920ms | 22.218ms | **0.059** | **0.077** |
| tiny   | 1.348ms | 22.804ms | 22.572ms | **0.059** | **0.060** |
| fib30  | 75.858ms | 113.684ms | 36.615ms | **0.667** | 2.072 |
| arith  | 84.530ms | 116.850ms | 50.165ms | **0.723** | 1.685 |
| branch | 44.708ms | 77.338ms | 28.836ms | **0.578** | 1.550 |
| strcat_flat | 4.174ms | 28.657ms | 33.744ms | **0.146** | **0.124** |
| strcat_grow | 3.389ms | 27.009ms | 26.016ms | **0.125** | **0.130** |

読み方（正直な欄）:
- **vs V8 インタプリタ（--jitless）: 7 項目中 7 項目で akl が速い**（<1 で akl 速）。
  arith は CoJIT S2（末尾 D 回転）で 1.009 → **0.723 に逆転**。
- **vs V8 JIT: ホット数値ループ（fib30/arith/branch）では 1.55–2.07× 負け**（旧 1.8–2.3× から縮小）。
  JIT はホットループに effect する構造上この結果は予定調和であり、隠さない。
- **CoJIT S2（2026-08-01）**: 末尾 形 D（for-update `g = g + d` の DUP/store/POP 連鎖）を
  LOOPINC_G/L（non-V 版: last_val 不変）へ回転写像。トップレベル/関数内 for が対象。
  等価性は差分オラクル（tests/test_akl.c: continue/break/入れ子/last_val 保証の 9 系 +
  乱択 400 系統を cojit on/off で完全一致要求）＋ switch/computed-goto 両 dispatch で検証済み。
  実測効果: arith 115→84ms、branch 87→45ms、fib30 不変（再帰は対象外）。
- **rssrun 計測系の wall（本稿冒頭の要求: 全て ≤100ms に）**: empty 1.4 / tiny 1.3 /
  fib30 70.5 / arith 83.1 / branch 44.4 / strcat_flat 4.2 / strcat_grow 2.4 ms（median of 5、実測）。


## 2026-07-31: CSS RuleSet 索引（Blink RuleSet 戦略）— 合成カスケード 23.32×、カスケード order バグ同定・修正

- **RuleSet 索引**（src/css.c）: 右端 compound の最強特徴（id>class>tag>universal）で
  (rule, sel) を単一バケツへ格納し、要素側は自身の特徴が指すバケツ + universal のみ全マッチ
  （Blink RuleSet と同一戦略）。バケツは distinct ぴったり確保 + hash 昇順ソート + 二分探索
  （slack ゼロ、エントリ 8B、構築は qsort 一回）。kill switch `if_css_set_naive_matching` で
  旧全走査に切替可能（監査用）。
- **カスケード文書順の一意化（索引差分監査が炙り出した既存バグ）**: rule 単位 `order++` では
  `order = rule_base + decl_idx` がルール間で衝突し、同 spec 同重要度のタイで「後勝ち」が
  成立し得なかった（反復順序依存 = 索引化で結果が変わる原因）。decl 単位ストライド +
  `order_end` で一意化し CSS 仕様の後勝ちに修正（回帰テスト: `.a{bg;color:red}.b{color:blue}` → blue）。
- **Blink API 互換**: src/css_blink.h（header-only、0 バイト、C ABI）+ docs/BLINK_COMPAT.md に
  coverage map を定義。戦略・カスケード順序・specificity ビット配置は同値、pseudo/属性/
  兄弟/@規則等はサブセット外と明記（「互換」の範囲を誇張しない）。

### 実測（bench/bench_css.c、2500 rules × 3000 elements 合成、5 交互 round min、このコンテナ）
| 経路 | 時間 | 比 |
|---|---|---|
| naive 全走査 | 471.990 ms | 1.00× |
| RuleSet 索引（既定） | **20.240 ms** | **23.32×** |

### 検証（このターン、CSS）
- on/off 差分オラクル 220 seeds（構造化ランダム sheet×DOM、全ノード計算済みスタイル bit 一致）。
  単体計 **17,543 checks×2 dispatch 0 fail**（CSS oracle/facade 追加分含む）。
- ASAN が索引構築の universal 無キー初期化漏れ（qsort cmp のゴミ key memcmp）を一度摘発 → 修正済
  （安全側の規律: universal は無キー `{NULL,0}` 契約に統一）。
- fuzz 500+500 0 crash、conformance 97.3% 不変、tui/gui smoke PASS、guard ALL PASS。
- バイナリ: ifuto 223,872 B（天井 240KB 内、索引・audit 追加で +4,096 B）、ifuto-gui 199,192 B、
  ldd 不変条件維持。

## 2026-07-31: Akl v0.2 — JS 例外 + CoJIT（検証駆動 AOT 特化）+ cold 分離回帰ポストモーテム

構成（このターンの到達点）:
- **JS 例外 v0.1**: `throw` / `try` / `catch(e)` / `catch`（ES2019 束縛省略）/ `finally`。
  cross-frame 巻き戻し、rethrow、return 経路の finally（返り値は finally 実行前に確定）、
  未捕捉はホスト致命的エラー（budget/OOM と同格の fail-fast）。**v0.1 の明示的限界**
  （誤動作より明白な拒否）: break/continue が try 境界を跨ぐのは compile エラー、
  TypeError 等のホスト致命的は JS から throw 不可能、catch 束縛は関数 locals を共有再利用。
- **CoJIT v0.1**（利用者提案「静的検証で必要部分のみ最適化するコンパクト JIT」の採用形）:
  codegen 後のバイトコードに対し `CJMPF_{G,L}; body; [更新]; JMP; exit:` な while 形・
  関数内部形を検出し、回転して `LOOPINC_{G,L}V`（inc+cmp+branch+last_val 保持の融合）へ
  書き換える **AOT 特化**。runtime codegen はゼロ＝JIT 禁止の不変条件に抵触しない
  （実行可能書き込みページは依然ゼロ、profile 収集もゼロ、生成物は検証済みの既存命令のみ）。
  **ハッキング耐性（利用者要求「色々してもろて」の回答としての構造）**:
  1. 特化器は失敗しても致命的にならない（検出しなければ汎用命令のまま残る＝安全側）;
  2. 特化後ストリームは必ず `akl_verify` whole-scan を再通過（事後セルフチェック。
     実際この自己検査が trampoline の local-slot 領域違反を一度摘発して設計を修正）;
  3. テストに on/off 2 インスタンスの差分オラクル＋ C 側独立予測との三方向一致
     （xorshift32 構造化ランダム 400 seed。特化器が意味を変えた瞬間に赤くなる機械監査）。
  適用 tail は A: `LINC/GINC;JMP`、B: 関数内 global 更新列、C: local 更新列。
  発火数は `akl_cojit_count`、kill switch は `akl_set_cojit`。
- **cold 分離（回帰ポストモーテム）**: 例外機構を dispatch マクロで inline 展開した初版は
  バイトコード列が完全同一（op 名列 diff=0）にもかかわらず **arith +24% / branch +42% /
  fib30 +11% 悪化**（新旧バイナリ交互計測で確定、nodejit 側はほぼ不変）。原因は冷コードが
  vm_exec の I$/分岐予測/インライン予算を侵食する機械語レイアウト問題（下記 121→138→111ms
  前例と同型）。`akl_vm_unwind/ret_step/try_push/try_leave/fin_end` を noinline+cold に
  隔離（RET は `n_tries==0` の旧来完全 inline 高速経路を保持）して回復。

### 実測（このコンテナ、2026-07-31、cold 分離後・交互計測 median、base=c00528b）
| 指標 | akl | 対 base | 対 node --jitless（guard） |
|---|---|---|---|
| arith 5M loop | 115.1-123.7 ms | **1.000×**（回復。悪化版は 1.235×） | 0.967 / 1.039（閾 1.05 PASS） |
| branch 5M | 59.2-59.8 ms | 1.058×（悪化版は 1.422×） | 0.721 / 0.704（PASS） |
| fib30 | 69.8-78.8 ms | 0.958× | 0.714 台（PASS） |
| bench_akl arith 100k | **2.413 ms**（前セッション 3.048） | 自前基準で改善 | — |
| bench_akl fib(22) | **1.494 ms**（同 1.670） | 改善 | — |

CoJIT の正直な効き（-O2、while 形・関数内形に限る）: while_arith(global) ±1.01×、
while_fib 1.02×、関数内 1.05〜1.18×。**天井破りではなくカバレッジ一般化**（canonical-for 由来の
AST 融合と同等速度に while 形を引き上げる）。vs V8 full JIT の残差（arith 2.3× 級）には触れない
— 本丸は generalized stmt-stencil mega-fusion 側（台帳不変）。

### このターンが同定・修正した既存バグ（テストで回帰固定）
1. codegen 失敗ロールバックが `n_funcs--` だけでネスト関数エントリを funcs 表に残し、
   後続 eval の領域表が skew（"verify: outside function"）。`n_funcs = main_idx` に修正。
2. **`var a=0,b=0;` カンマ宣言が最後の宣言しか残さなかった**（ReferenceError 化）。
   parser を scratch 収集→commit に修正。`var ca=1,cb=2` 等の回帰テスト追加。

### 検証（このターン）
- 単体 **2,440 checks×2 dispatch 0 fail**（例外 19、CoJIT 行列+400 seed oracle、カンマ回帰等）。
- fuzz 500+500 0 crash、guard ALL PASS（qjs 相対は参照バイナリ不在で SKIP 明示）、
  conformance 97.3% 不変、tui/gui smoke PASS。
- bench_akl 計算値不変（fib22=17711 / arith=905003 / mixed=7.9996e+07）。

## 2026-07-31: v0.2 — GUI（生 X11）+ Markdown + slim-DOM + viewport 窓グリッド

構成（このターンの到達点）:
- **ifuto-gui v0.2**: 生 X11 プロトコル実装（Xlib/XCB 不使用、ヘッダも不要）。自前 5x7
  ビットマップフォント、全面バックバッファ非保持の **8 セル行ストリップ・ストリーミング描画**。
  タブ帯/オムニボックス（フォーカス+キャレット）/ステータスバー、キー駆動（Ctrl+L/T/W/Q、
  Ctrl+Tab、矢印/jk、PgUp/PgDn）。`--shot OUT.ppm PAGE` = X 不要の同一ラスタ検証経路。
- **Markdown 変換器 `src/md.c`**（ink level = Markdown 以上の法則）: 見出し/段落/強勢/
  コード/リンク/引用/リスト/罫線/フェンスコード/GFM テーブル/脚注。生 HTML 素通し禁止
  （全テキスト escape＝多層防御は共通パーサの規則に委譲）。再帰は index 窓、深度上限つき。
- **slim-DOM**（法則「画面描画に関係ないものは DOM しない」、`if_dom_slim`）:
  script/template の子孫・本文を DOM に構築しない（状態機械は完全に回す。root は marker
  として残す。style は cascade が本文を読むので残す＝描画に関係あるため）。実ブラウズ経路
  （chrome＝TUI/GUI）で既定 ON、CLI は `--slim-dom`、適合ハーネスは従来通り full DOM。
- **viewport 窓グリッド**（`if_render_grid_rows_into` + `IfGrid.y_off`）: grid は
  `[scroll, scroll+vh)` だけ materialize。文書長に比例しない（TUI/GUI 共通、容量はキャッシュ再利用）。

### 実測（このコンテナ、2026-07-31）
| 指標 | 値 | 注 |
|---|---|---|
| バイナリ build/ifuto | **219,776 B** | v0.2-pre 195,192 比 +24,584 B（md.c 24,993 B 源＝本体） |
| バイナリ build/ifuto-gui | **195,088 B** | ldd: linux-vdso/libc/ld のみ（不変条件維持。libm も gc-sections 剥落） |
| IfCell / IfNode | 8 B / 112 B | sizeof 実測（構造体会計の基礎値） |
| viewport 窓 grid | 10,000 行×120 桁文書≙ フル 9.6 MB → 窓 vh=40 で **38.4 KB（1/250）** | 計算会計（8 B/cell）。文書長不変 |
| slim-DOM（合成 1.89 MB 頁: script×2000 + template×2000） | dump DOM 行 10,012→**8,012（−20%）**、RSS 13.0→12.5 MB（−4%）、wall 31.2→30.7 ms | `/tmp` 上合成頁、python resource 計測 |
| GUI ラスタ決定性 | sha256 2 回一致（gui_smoke 17 checks PASS） | tests/gui_smoke.py |

**正直な分析（slim-DOM の効きの境界）**: テキストノードは入力バッファへのゼロコピー参照のため、
剃った script 本文のバイトは入力 arena がタブ寿命で保持し続ける＝現状の剃りは**ノードヘッダ
（112 B/個）と下流通過（layout/cascade/dump の走査回避）**に効く。script 巨大 1 本の頁では
tokenizer のトークン緩衝がピークを支配し RSS 差は出ない（16 MB 単一 script で 55.8 MB 同値を実測）。
**入力 compaction（可視参照だけ新 arena に写し旧 arena を破棄＋ポインタ付替）が次の本丸**で、
script 比重の大きい実在頁で 10 MB 級の削減が見込める。v0.3 台帳へ。
また fuzz は full DOM 経路中心なので slim 変異系統（script/template 混じり構造）の追加も台帳。

### 検証（このターン）
- 単体 **1,989 checks×2 dispatch 0 fail**（slim-DOM 16 checks、md 〜40、uichrome 追従）。
- fuzz 500+500 iter 0 crash（ASAN/UBSAN）。guard ALL PASS。tuibench 系は不変。
- WPT tree-construction **1,679/1,726（97.3%）不変**（slim は適合ハーネス非適用を確認）。
- tui_smoke PASS、gui_smoke PASS、GUI 実ラスタ PNG 目視 QA（グリフ鏡像バグ修正済）。
- bench_akl 値不変（fib22=17711 / arith=905003 / mixed=7.9996e+07）。
- 天井監視: ifuto 219,776 B は旧 TUI 天井 200 KB を **+19,776 B 超過**。成因は md.c 追加で
  機能マイルストーンに伴うもの（悪化ではないが）— v0.2 系の新天井 240 KB をここに設定し、
  以後のコミットはこれに拘束される。

## 2026-07-31: Akl v0.1（GC + ROPE + 融合命令）— QuickJS 比「軽さ・速さ」全軸クリア、V8 --jitless 比も全軸クリア

構成: mark-sweep GC（adaptive pacing, ルート=VM スタックスナップショット+globals+nursery+last_val）、
ROPE 文字列（`AKL_OK_ROPE`、償却 O(1) 連結・遅延 flatten・深さ 4096 上限）、
融合命令群（LINC/GINC、CJMPF_L/G、CJMPF_MODG/L/GG、CJMPF_MULGG、GMULC/LMULC、*CI、
*CI+store 再融合 `*_G/_L`、3 アドレス `*_GX/_LX`、GADD_P/LADD_P/GADD_G、LADD_LL、
RET_L、STORE_PV 系、`for` ループ回転 + LOOPINC_G）。
融合の意味保持は「汎用路が非融合命令列と同じ関数を同順序で呼ぶ」構造共有で証明
（akl_cist_compute / akl_binfv_compute を単独命令と再融合命令が always_inline で共有）。
検証: 単体 1,875 checks×2 dispatch 0 fail・fuzz_akl 500 0 crash・WPT 97.0% 不変・tui_smoke PASS。

### 比較表（実測・同一実行セットの median。±10% 程度の実行間変動あり）

時間（プロセス wall ms、起動込み。小さいほど良い）:
| bench | akl | qjs | node --jitless | node(full JIT) |
|---|---|---|---|---|
| empty | **1.16** | 1.45 | 24.9 | 23.6 |
| tiny | **1.20** | 1.55 | 22.7 | 24.6 |
| fib30 | **69.1** | 85.6 | 115.6 | 33.4 |
| arith | **116.3** | 168.3 | 118.9 | 50.0 |
| branch | **55.8** | 88.4 | 77.2 | 28.8 |
| strcat_flat | **4.48** | 5.15 | 28.4 | 27.2 |
| strcat_grow | **2.21** | 3.16 | 24.9 | 25.8 |

読み方: vs qjs = 0.63〜0.87×（全 7 軸で勝ち）。vs V8 --jitless = 0.047〜0.98×（全軸で勝ち）。
vs V8 full JIT = 起動/小仕事 0.05〜0.09×、文字列 0.09〜0.16× で勝ち、
ホット数値ループのみ 1.9〜2.3× で負け（下記台帳）。

RSS（rssrun 子 rusage）: akl **2.0〜2.2 MB** / qjs 2.8〜3.0 MB / nodejit 41.5 MB / node 41.8〜46 MB。
バイナリ（stripped）: akl_cli **98 KB** / qjs 1,027 KB（0.095×）/ node 107 MB。

guard（常時アラーム）: `make guard`（bench/akl_guards.json。絶対閾値: バイナリ ≤128KB・
RSS ≤3.5MB・時間天井。相対閾値: vs qjs ≤1.05 全軸・vs nodejit ≤1.05 全軸）。1 件逸脱で exit 1。
兄弟 harness `bench/vsx.py`（C1-C6）も **VERDICT: PASS ×3 連続**（RSS 軸は rssrun 化、
net 軸は rep 内ペア減算に改修済み。下記「測定工学」参照）。

### 未解決の台帳（嘘をつかない欄）

1. **vs V8 full JIT のホット数値ループ**（fib30 2.07×、arith 2.33×、branch 1.94×）:
   TurboFan の型特化 JIT による領域で、no-JIT の C11 インタプリタという制約上の物理限界。
   JIT は利用者要件で禁止されているため、ここは「 V8 の no-JIT 層（Ignition = node --jitless）
   に全軸で勝つ」ことを v0.1 の勝利条件とする（過去ターンでも同じ裁定）。将来の正当な攻め筋:
   compile-time 単一呼出しの AOT インライン化（コード生成を伴わないので JIT 禁止に抵触しない）、
   monomorphic callsite キャッシュ。どちらも v0.2 台帳。
2. `strcat`/`small` 等のサブ ms 軸はプロセス spawn ノイズ支配。閾値化は ±解像度のある
   ベンチ反復数に増やした上で行う（strcat.js 3k→100k、callseq.js 100k→300k）。
3. 融合命令はベンチ形状ではなく「代入・比較・剰余・ループ増分の一般形状」に対してのみ
   発行する設計。特定ファイル向け特化（定数畳み込み等）は行っていない。

### 測定工学（再発防止の記録）

- **python の `resource.getrusage(RUSAGE_CHILDREN)` は RSS 測定に使ってはいけない**:
  fork 後 exec 前の python ページが子の ru_maxrss に混入し、全エンジンに ~10.4 MB の
  虚偽ベースラインが載る（このハーネスでも fib の C2 が 10,644 vs 10,312 で一度 flake）。
  RSS は `build/rssrun`（C 製 fork/exec/wait4 ラッパ）経由のみ。
- **net（起動差分）は「独立 median 同士の引き算」で取ってはいけない**: 負荷ドリフト時に
  empty 中央値（node ~26-31ms）が ±4ms 揺れ、2.5-6.6ms の偽 net 揺らぎで false FAIL する。
  各 rep で empty→bench を連続実行し rep 内で差を取ってから median（vsx.py 改修済み）。
- akl_cist_compute のような共有ヘルパは `__attribute__((always_inline)) inline` を
  付けないと -O2 でも関数呼出し化され、arith で +17% の回帰になった（実測 121→138→111ms）。

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

| 指標 | 天井 | slice-1 実測 | slice-2 実測 | v0.2 途中（81.6%）実測 | 判定 |
|---|---|---|---|---|---|
| バイナリ合計（engine+chrome, stripped, LTO） | ≤ 200 KB | 121,392 B | 133,752 B | **195,192 B**（+61,440: パーサ正確性の代価、内訳は下記） | ✅ 1.02×。余裕 +4,808 B のみ（監視対象） |
| 空タブ UI 常駐 RSS（初回描画後 VmHWM） | ≤ 4 MB | 888 KB | 1,428 KB | **1,732 KB** | ✅ 2.4× 余裕 |
| コールドスタート→最初の描画バイト | ≤ 25 ms | 1.26 ms | 1.55 ms median (n=7) | **1.31 ms median** (n=7) | ✅ 19× |
| アンロード済み 50 タブのメタデータ | ≤ 2 MB | 14,720 B | 14,720 B | **14,720 B**（不変、リーク傾き 0 B/195 周） | ✅ 139× |
| **50 タブセッション復元（遅延ロード込み）** | ≤ 100 ms | — | 0.11 ms | **0.13 ms**（active 即ロード含む厳しい定義） | ✅ 770× |
| TUI アイドル CPU / 描画出力 | 0 % / 0 B | 0.00 % / 0 B | 0.00 % / 0 B | **0.00 % / 0 B** | ✅ |

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

- 2026-07-29（v0.2 適合性 81.6% 時点のエンジン成長会計）:
  - バイナリ 133,752 → **195,192 B**（+61,440 B）。内訳: 全エンティティ表
    **30.8 KB**（圧縮後。後述）+ table 挿入モード群/foster parenting/AFE+AAA/
    script-escape 状態機械/ruby・select・frameset・template-content のパーサ増分
    ≈ +30 KB。重さはすべて w_correctness の対価であり chrome 側（slice-1/2）は
    不変。天井 200 KB とのマージンは **+4,808 B** —— 以降の v0.2 増分は
    「表を増やさない・コードのみ」「増やすなら先にどこかを縮める」規則で運用する。
  - エンティティ表の圧縮記録: 初期生成は 12 B/行（off,len,flags,cp1,aux1,cp2,aux2）
    + NUL 区切り名 blob で 41.1 KB、バイナリが 207,480 B に達し**天井違反**。
    検証の上で不変条件を特定（① 2-cp エントリは両方 BMP、② astral は必ず単一 cp、
    ③ cp=0 は存在しない → 生成器で assert 強制）し、8 B/行
    （cp1==0xFFFF 時のみ cp2 を aux32 index とするエスケープ）+ NUL 除去 blob で
    **30.8 KB**（−25%）。bsearch 形は維持（正確性・監査性を速度無関係に優先）。
  - ストレス 2MB 文書 total 86 → 93〜94 ms（+8%）: AAA と実体参照解決のコスト。
    88.1% 到達時点で 98 ms（+14% 累計）: scope バリアの ns 判定と frameset-ok 規則の追加。
    render 支配項は不変で、parse 側の per-token 判定増が約 +4 ms。
    支配項は render フェーズのまま（変わらず）。parse 9.8 ms の増分は大きくない。
  - small 文書 avg は 1〜2 ms で揺れる（±1ms はこのコンテナのノイズ床）。
  - 空タブ VmHWM 1,428 → 1,732 KB: .rodata のエンティティ表への初回ページインと
    パーサ増分コード（ピーク計測なので初回フォルトが乗る）。天井内のため後追いのみ。

## 2026-07-29: Akl v0.0（自作 JS エンジン、C11・JIT なし）初回測定

採用仕様: 字句→recursive-descent（AST プール）→one-pass codegen→**全検証 verifier**
→スタック VM。値は NaN-box 8B、ヒープ参照は obj 配列への u32 index、命令・呼出深度・
解析深度・ヒープバイト・ノード数・引数列・ソース長の全 budget を fail-stop で管理。
JIT は構造的に不在（W^X 全域、実行可能書き込みページ 0）。

### dispatch 実測（bench/bench_akl.c, REL, 7 プロセス×7 回の median-of-medians, 本コンテナ）
| workload | computed-goto | switch | goto/switch |
|---|---|---|---|
| fib(22) recursive | 2.567 ms | 3.324 ms | **1.295×** |
| arith loop 100k | 9.198 ms | 10.418 ms | **1.133×** |
| mixed stmt loop 20k | 2.583 ms | 2.902 ms | **1.123×** |
| str concat 2k | 0.391 ms | 0.393 ms | 1.005× |

裁定: **computed-goto を既定**（GCC/Clang で有効、他は switch フォールバック。
`AKL_TEST_SWITCH_DISPATCH` で強制切替可）。dispatch 支配の算術系で +12〜30%。
文字列系はインターンハッシュ支配で差なし。理論（間接分岐の局所化）と一致。
両モードとも同一単体テスト全緑（run_tests / run_tests_switch の双子運用で固定）。

### サイズ会計（実測）
- スタンドアロン差分（空 main との diff, stripped/LTO/gc-sections）: **45,392 B**。
- ifuto（195,192 B）に全 akl シンボル保持で仮リンク: **244,376 B（+49,184 B）**。
- 内訳（-O2 単体 .o）: .text.vm_exec 18,919 B（42%）、lex_next 2,816、
  p_stmt 2,674、cg_stmt 2,436、akl_eval 2,221、p_unary 1,862、他 ≈16 KB。
- **200 KB 天井との関係**: v0.0 は本体未リンクで天井維持（195,192 B 不変）。
  v0.4 統合時に天井を再設定する。削減材料の候補（未実施・見積もりも未計測）:
  dtoa の自前化で %g/%.0f printf 経路を剥がす、エラーメッセージの ID 化、
  vm_exec の命令削減。再設定するときはこの 3 案の実測値を添えて判断する。

### 検証
- 単体: **1,813 checks / 0 fail**（akl 追加分 118。1695 → 1813）。
  NaN canonical、±Inf、int32→double 境界、INT32_MIN % -1 の UB 回避、
  短絡評価の生値、0.1+0.2 の厳密 binary64、ToInt なし連結 ToString（往復最短精度）、
  グローバル定数 NaN/Infinity の const 保護、parse depth・命令・深度・ヒープの
  各 budget の fail-stop、budget 枯渇後の eval 健全性、rt 間のグローバル独立性。
- fuzz: fuzz_html 500 + **fuzz_akl 500** = 1,000 iter / 0 crash（ASAN/UBSAN）。
- WPT tree-construction: **88.1%（1,521/1,726）不変**（エンジン側差分ゼロの確認）。
- 既知の v0.0 境界（台帳）: 配列/オブジェクトリテラル、三項 `?:`、`++`/`--`、
  複合代入 `+=`、arguments/this/new/prototype、ASI 完全形。let/const は関数スコープ近似。
  dtoa は往復最短精度（15→17g 探索）で JS と整数帯・最短表記を一致させたが、
  指数非化帯の境界（1e21 前後の形式）までの完全一致は未保証。
