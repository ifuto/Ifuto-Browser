# BENCH.md — Ifuto 計測台帳（現行値のみ）

このファイルは **現在のビルドの計測値と測定方法だけ** を置く。古い計測・過去の
履歴・後退の記録は置かない（遷移は `git log` を見よ）。数値は全て実測。
推定・引用は必ずラベルを付ける。測定は本コンテナ（1 物理コア 2HT、帯騒音あり）
で `--no-ansi --stats` の表記どおり。

## 計測規約（再現手続き）

- コーパス: `tools/gen_idm.py` 決定生成（`.arena/idm/idm-16mb.md` = 16,777,395B /
  nodes=1,616,804 / links=21,848。消失時は再生成可）。
- 採用基準: paired interleaved A/B median（7–13 対）+ 符号検定。**機構的に厳密優位
  かつ byte-exact かつメモリ増なし** の場合のみ弱証拠でも採用可（その旨を記す）。
- 帯騒音は ±5–15ms で揺れる。±3ms 級は median でも溶けるため、**mcount/gprof
  等の機構証明を優先**して採用可否を決める。
- 起動速度は ` /bin/true` 経路での最小/中央値に subprocess wall time を使う
  （`/usr/bin/time` 非存在、bash 組込の `time` のみ）。

## 現行スナップショット（2026-08-08 計測・`build/ifuto` 675,216B）

直前差分（HTTPS/TLS, BearSSL 静的リンク, src/tls.c + vendor/bearssl）:
+172,552B（TLS 1.2 クライアント + x509 検証。--gc-sections で使用シンボルのみ）。
ldd = vdso/libm/libc/ld のまま（静的リンク）。A1 文字コード層は +49,152B。
md ミッション経路は関門外（`if (!md_doc)` ゲート）のまま、A1 後の n=7 再計測は
median **122.23ms**・peak_rss 224,988KB（帯内一致・回帰なし。同時並行 fuzz 計測の
215ms 汚染値は棄却・静粛状態再計測で確定。台帳として方法論違反を記録する）。


（前回 441,224B から +12,288B: `<script>` akl 実行配線（src/script.c）+ DOM
最小変更面 + AklHandleVTab。**script 実行配線の pipeline 影響は構造的ゼロ** =
`IfDom.has_script` parse 観測スイッチで script 非含有文書は走査自体を行わない
（本 corpus は script 0 件を grep 確認済。median は 122.97→123.10ms で帯内一致）。

### 16MB IDM パイプライン（`--no-ansi --stats`、n=7、騒音帯）

| 段 | median | 範囲 |
|---|---|---|
| read | 0.02ms | 0.01–0.02 |
| parse | 55.0ms | 53.3–72.1（並列 2-slice 経路） |
| style | 0.00ms | lazy（行スイープ） |
| layout | 52.6ms | 51.1–61.6（2-way 並列） |
| render | 15.5ms | 15.4–16.7（単純先行） |
| **total** | **123.10ms** | warm 帯 120.1–123.7（同日 n=7 再計測） |
| peak RSS | 225,096 KB | ±0.4MB |

**ミッション「騒音帯でも 16MB total ≤150ms」は継続達成中**（warm 帯全 run ≤150ms、
median 123.10ms。環境アイドル直後のコールド初回のみ帯外（145–167ms 実測、従来同様の
ウォーム要因で 2 回目以降に収束）。帯の定義と運用は git log の方針確立コミットを参照）。

### 入力 compaction（v0.3 本丸・GUI 専用。2026-08-08 実測台帳）

**採用設計: 取り込み時複製（copy-on-ingest）**。GUI ロード時 `if_dom_copy_strings`
（dom.h 台帳）を立て、DOM が保持する入力切片を誕生点（text/comment/attr/doctype/
md 借用判定）で arena 複製 → 一時入力 arena を parse 後即破棄。

事前計測（スクラッチ調査器、凍結判定規則つき）: live slice = 入力の 54–56%、
dead = 44–46%（16MB md と同等 HTML で一致）。判定規則「中間（30–70%）→ GUI 限定
実装」に従い GUI のみ配線。CLI はフラグ不変の既定側（不変条件）。

walk 型代替案（parse 後に DOM 全走査で差替）は**実測棄却**: cold sweep が 16MB で
**+62ms** を要し法則「速度全体」に反する。ingest 版は cache-hot 複製で以下。

| 指標（GUI `--shot`・16MB md・interleaved A/B n=5） | prev | ingest 版 | Δ |
|---|---|---|---|
| VmHWM（poll と ru_maxrss の一致確認済） | 609,288 KB | 605,876 KB | **−3,412 KB** |
| wall（median。帯ノイズ内、上限 ~+25ms） | 485.0ms | 506.6ms | +21.6ms（有意でない弱証拠。walk 版 +62ms からは改善） |

- 理論 dead 7.0MB に対し実現 −3.4MB の乖離理由（正直記載）: 小切片複製の arena
  アライン損と、peak 位相が grid build 側にあること。50 ロード済みタブ換算で
  **約 −170MB** が本意の効用（省メモリ法則 = truth が多タブで効く領域）。
- CLI ミッション経路は 1 行も変更されていないことを固定: flag OFF の A/B 7 対
  （嵐帯 ±10ms 中、中央差 −4.2ms、符号 3:4 = 回帰の有意なし。弱証拠として明記）
  + oracle 21/21 byte-exact（+SJIS/EUC-JP E2E 4 件 + 変換表再生成一致）。
- 機械オラクル: `tests/test_compact.c`（free 後アクセスの ASan heap-use-after-free
  検出。walk 版開発中に tpl_map 開放番地法の走査バグを即検出した実績つき）＋
  フラグ OFF のゼロコピー不変条件テスト。

### 2MB IDM（n=5、騒音帯）

total median **17.44ms**（同日 n=5 再計測 16.0–19.4）。parse 8.0 / layout 7.0 / render 2.4ms、
peak RSS 35,596 KB、nodes=206,290。

### CLI 起動（tiny HTML render 300 連プロセス wall）

min **1.40ms** / median **1.66ms** / p90 1.96ms。

### C vs Rust 比較（2026-08-31、移行フェーズ 12-h（マーカー確保消去フェーズ）。`bench/bench_c_vs_rust.py` 実測）

paired interleaved A/B（対ごとに実行順を交互化）で C=`build/ifuto` /
Rust=`rust/target/release/ifuto` を比較。
**全計測ペアで stdout byte 一致 + stderr(scrub 後) 一致を同時検証済**（各ベンチ 177/177。
fast-DOM 有無の fast≡slow stdout 一致も両実装で検証: c:1 r:1）。
ldd: C = vdso/libm/libc/ld、Rust = 同 + libgcc_s（C は -static-libgcc 適用済）。
フェーズ 10-e = **Node 痩身化（200B → 80B）**、10-f = **layout アロケーション段撲滅
（allocs 1.29M → 44k）**、10-g = **2-slice parse 実益化（既定 ON 転換）**。
フェーズ 10-h = **body shard（2-way 並列 layout）移植**: C `layout_shard_run_body`
接合規約の写し。線形+lazy+md 由来 DOM で `md_body_mid` ヒントを持つ大文書のみ発動、
A=[first..mid)/B=[mid..) を独立 Lc で走らせ、整数セル幾何の `y+=hA` 接合 + stab
値同定併合 + syn_text 8B アライン整合で serial と厳密同値を構成（`IF_LAYOUT_PAR=0`
が殺しスイッチ。C と同規約の既定 ON）。
フェーズ 10-i = **render 2-way 並列 sweep**（no-ansi 限定、C render_ansi.c 同名機構の
写し。行 y で二等分、B は r_split 時点の deco 状態を追記順再構成。kill switch
`IF_RENDER_PAR=0`）。
フェーズ 11 = **テキストアリーナ（Text pun fields）**: >22B Text 本文を per-node
`Box`（16MB md で ~109k malloc）から `Dom.text_arena` bump + `attrs_idx=off /
extra_idx=len` 転用へ。parse allocs **764,792→655,552（−14.3%）**。書き口
`set_node_text` / 読み口 `text_of` 各 1 本に集約、`Deref` 無傷（要素名系 ~80 サイト
非侵略）。arena は入力×~1/2 を 1 回予約（実測係数 0.48）。
フェーズ 12-a = **md ブロック機構のゼロコピー化**: `split_cells` を `Vec<&[u8]>`
スライス化 + `ln_is_table_delim` ゼロアロケスキャナ化（テーブル処理が真の主犯
~415k/655k。parse allocs **655,552→262,270（−60.0%）**）+ `Fn<'a>` 借用化
（脚注機構の衛生リファクタ。コーパス脚注 0 件で効果ゼロ、perf 主張なし同梱）。
フェーズ 12-b = **属性機構のゼロアロケ化**: `Attr.name/value: Vec<u8>` を 10-d の
`NameStr`（24B、≤22B inline 収容）へ。md 経路は属性 1 件 3 確保（pend `to_vec` +
name `to_vec` + `clone`）だったのを、name=`from_static` 無コピー・value=pend
`NameStr` を move で **0〜1 確保**（≤22B 値は inline で 0、長い href 等 1）。共通
`Attr`（html_tok）ごと差し替えたため HTML 経路の短名/短値も inline 化。
副次: `merge_attrs` の borrow 回避用 全属性名 clone Vec を消去（逐次借用で実は不要
だった無駄）。parse allocs **262,270→196,710（−25.0%）**、11 前 764,792 から累計
**−74.3%**。仲裁 3-way n=21 interleaved: parse 104.74→**101.71ms（−2.9%）**、
total 238.56→236.14ms（−1.0%）。byte-exact ×6・oracle 21/21・test 345 緑・clippy
0・差分 fuzz +3,019（seed 31415）で 0 mismatch。残 = split_cells 成長 22,024 +
inline_span 群 ~65k（12-c 照準）。
フェーズ 12-d = **blocks 機構の再スキャン削減**: table 事前検査の `|` memchr
先行化 + fence/list の二重走査 1 回化。仲裁 3-way n=21: **parse −3.03%・
total −2.95%**。対 C 精読値 parse 2.048×・total 1.970×。
フェーズ 12-e = **layout 解剖フェーズ**（コード不変）: 形状マトリクス測定・
ASCII ラン SWAR 化の検証棄却・照準を per-atom/per-row 固定費に確定。
フェーズ 12-f = **seg 二重書き消去（C `IfWrap` 機構への忠実化）**: seg を
scratch `Vec` へ push →行末 `seg_arena` append していた旧構造（= 全 seg 二度書き
16MB で 4.3〜34.8MB + 行ごと RefCell borrow ×2）を、C と同じ **arena 直接確定 +
`line_lo` 区切り**へ改修。`rlines`/`seg_arena` は IFC 期間中 `RefMut` 保持
（borrow 税 per-line→per-IFC）、`pm_key` を `Option<(u32,u32)>` 8B 比較へ痩身、
`segs_scratch` 廃止。layout 段 仲裁 3-way n=21 で **136.78→132.13ms（−3.40%）**、
形状別は 5/5 改善（table −10.9%/head −5.3%/plain −4.3%/fence −2.7%/blocks
−0.3%）で幅度は二重書き量の順位と整合（機構帰属の裏付け）。
**設計失敗の記録**: diff fuzz seed 55555 が --no-style 経路の R 側パニック
（確定済み前行 seg の誤 trim で RLine 区間逆転）を検出。scratch 構造が暗黙に
保っていた不変条件（cx>0 ⇒ 現行行に seg あり）は `<br>` 直後の stale line_w で
偽になるため、C `n_segs>0` 同値の明示ガード `len > line_lo` を trim 経路に追加
して修復（最小再現 `a  <br>`+`b`*99 = 106B を oracle 登録: 21→24）。
フェーズ 12-g = **Winner 座標化（C `IfDecl*` 保持の写し）**: style 解決の勝者
スロットが宣言を `decl.clone()` で値保持していたのを `(sheet, rule, decl)` 添字へ
（memo miss ごとの ~5B Vec 確保嵐を構造消去。blocks layout allocs **128,946 →
43,544（−66%）**、alloc_bt で帰属特定）。blocks layout −2.81%・total −1.47%
（interleaved n=21、共に符号 15:6、独立 2 測定で同向き）。検証棄却: 32B チャンク
`iter().all()` の auto-vectorize 誘導は objdump でベクトル化ゼロを確認し棄却
（haszero/SIMD 模倣 3 連敗。early-exit 系は本環境の LLVM ではベクトル化されない）。
**環境騒音の注意書き**: 共有ホスト負荷で C 16MB total が 90-223ms の帯で揺れる
（2026-08-31 は BASE 中央値が run ごとに 182→716ms に崩れる日で、±2% 級の配対は
由来不明の揺れに溶ける。table 形状の読みは暴れたため不採用とした）。
フェーズ 12-h = **ol マーカーの String 確保消去**: `format!("{}.", li_ord)` が
<li> ごとに 8B String を確保していたのをスタック itoa 化（C snprintf 写し。
帰属は alloc_bt のフィルタ拡張で特定 — inline 化で消える型フレームを拾う恒久改修）。
blocks layout allocs 43,544 → **854（−98%）**、12-g からの累計 128,946 → 854
（**−99.3%**。残存は UA シート parse 定数 + amortized Vec 成長 = head 水準）。
時間は帯内（glibc tcache が速く ~0.2-0.6ms 規模）のため確保指標のみを主張する。
併せて `bench/gen_report.py` に出力親の makedirs を追加（results 消失再発の根治）。
フェーズ 12-i = **render sink streaming 化**: 出力 Vec の全量 materialize をやめ
行末単位 flush の sink へ（C IfBB FILE 直書き写し。cold で ~26MB 分の初回
ページ割当 ~13ms を構造消去 — 帰属は minflt 計数で確定）。仲裁 n=21 で
render **−26.0%（符号 21:0）**・段 R/C 1.78→1.32・RSS16 236,716→229,260KB。
倍率は paired 値として有効、絶対 ms の過去直較は ±20% 帯で読むこと。
跨 run 直較は不可。精読値は同日 3-way 仲裁 n=21 を正とする。

| 指標 | C | Rust | 倍率 (Rust÷C) |
|---|---|---|---|
| 16MB total（7 対 median） | 128.64ms | 274.88ms | 2.14× |
| 16MB 段別 read/parse/style/layout/render ms | 0.04 / 56.49 / 0.01 / 49.06 / 23.66 | 8.99 / 120.93 / 0.07 / 110.27 / 29.01 | — |
| 16MB wall（7 対 median、外部計時） | 135.09ms | 351.27ms | 2.60× |
| 16MB peak RSS | 212,860KB | 229,212KB | **1.08×**（12-i で 1.11→1.08 に改善） |
| 2MB total（13 対 median） | 16.81ms | 33.73ms | **2.01×** |
| 2MB 段別同上 ms | 0.02 / 7.26 / 0.01 / 6.56 / 3.11 | 0.91 / 14.39 / 0.04 / 15.35 / 3.30 | — |
| 2MB wall | 19.08ms | 43.20ms | 2.26× |
| 2MB peak RSS | 34,212KB | 34,604KB | 1.01× |
| ANSI 2MB total（7 対 median） | 40.98ms | 60.86ms | **1.49×** |
| ANSI 2MB render 段（同上） | 26.89ms | 28.24ms | **1.05×**（12-i の streaming で render は C と同帯域） |
| ANSI 2MB peak RSS | 32,420KB | 34,412KB | **1.06×**（1.75 から劇的改善 = 巨大 ANSI out Vec の materialize 消失） |
| 起動 wall（150 対 median） | 1.7350ms | 1.7320ms | 0.998×（符号 C:R = 68:82。−3μs。系列 …→92:58→68:82 と両方向反転継続 = jitter 帯） |
| md fast-DOM 有無 total（slow÷fast、16MB） | 599.2÷127.2ms = 4.71× | 1,117.4÷270.1ms = 4.14× | — |
| md fast-DOM 有無 total（slow÷fast、2MB） | 73.0÷15.9ms = 4.58× | 139.9÷41.0ms = 3.42× | — |

段別対比（16MB、median。a ラン）: parse 2.14× / layout 2.25× / render **1.23×**。
フェーズ 12-i = render sink streaming 化: render 段は C と同帯域（2MB 1.061・
ANSI 1.050）。**残の照準は parse 2.14× と layout 2.25× に移動**。
フェーズ 10-e〜12-i の内容と残照準は `docs/RUST_MIGRATION.md` を参照。
多角レポート全量（環境・手法・ゲート・生 JSON）: `bench/results/report-20260902a.html`
（gitignore 対象の生成物。生データは `bench/data-20260902a.json`、再生成手順はレポート §3）。

※ 併記レポート（同日の連続フェーズ）: 12-c=`report-20260829i.html`、
12-d=`report-20260829j.html`、12-e=`report-20260829k.html`、12-f=`report-20260829l.html`、
12-g=`report-20260831a.html`（いずれも data JSON は git 管理）。

#### 並列レイテンシーの正直な台帳（taskset ピン留め 1-HT / 2-HT A/B、2026-08-29 更新）

本環境は 1 物理コア 2 HT（2 vCPU）。taskset で HT を固定した採算計測（interleaved
median n=9、16MB）+ layout shard A/B（16MB、median n=3+7）:

| config | 16MB parse | 16MB layout | 16MB total |
|---|---|---|---|
| Rust 10-h（parse par + layout shard、既定）@2HT | 123.95 | 117.57 | 308.78 |
| Rust layout serial（IF_LAYOUT_PAR=0、parse par ON）@2HT | ~126.9 | 165.2 | ~342 |
| Rust 全 serial（IF_MD_PAR=0 + IF_LAYOUT_PAR=0）@2HT | 133.88 | 173.84 | 367.91 |
| Rust auto @1HT（serial に自己降格） | 132.79 | ~160 | ~350 |
| C auto（2-slice + layout shard）@2HT | 47.11 | 46.79 | 116.83 |
| C layout serial（IF_LAYOUT_PAR=0、parse 2-slice ON）@2HT | 6.46〜44級 | 11.07（2MB）/ 80.02級 | — |
| C serial @1HT | 103.88 | 129.18 | 268.39 |

- **10-h body shard は 2HT で layout −55ms（165.2 → 110.5ms、1.50×）の黒字**。
  C の同機構（hint 有 80.02 → 45.0、1.78×）より HT 効率が低いのは、shard 間で
  streams（lines/deco/seg_arena/syn_text/stab）を複製して後接合する設計=Rust の
  `sid` 間接と Vec 所有の価格（C は arena ポインタ吸収 `if_arena_absorb` で O(1)）。
  接合税は stab remap + seg 写像 ≒ 数 ms/16MB。RSS 価格: 16MB で 224,904 → 240,404KB
  （B 側 streams の一時保持分。総計の縮小（−37ms 中央値）を優先してこの価格を採択）。
- layout shard 内部モデル差（syn 占位カウンタの shard 局所再発火）は C の
  per-shard arena と同型で観測出力不感。観測同値は `shard_layout_equals_serial`
  単体テスト（canonical 射影比較）+ diff fuzz + 全オラクルで機械固定。
- serial ≡ 2-slice ≡ C parallel ≡ shard の全経路 byte 一致は下表オラクルで検証済。

### 適合性・検証の現行値

| ゲート | 現行値 |
|---|---|
| WPT tree-construction（`tests/wpt-tree-construction`、WPT master `5b6a1e6`） | **1922/1922 (100.0%)** × 両バイナリ（C / Rust）、skip 12 = `#script-on` のみ（fragment 196 件は `--fragment CTX` で実行済） |
| TLS smoke（`make tlssmoke`、自己署名 CA + openssl s_server E2E） | 3 checks（https 取得 / CA 不一致 cert 拒否 / IP SAN 非対応の明示） |
| 単体テスト（run_tests + run_tests_switch 双子、ASAN+UBSan） | **625,125 checks / 0 failures** ×2（script 実行配線 + HANDLE GC 機械証明 + 入力 compaction + 文字コード層 60 checks オラクル込み。2026-08-24 再計測） |
| Rust ワークスペース（`cargo test --offline` / `--release`） | **345 passed / 0 failed**（akl-core 142 + akl-ffi 6 + ifuto-core 188 + ifuto-cli 9。2026-08-29、フェーズ 11 で +1: text_arena_representation） |
| 出力 byte-exact oracle（`tools/chk_oracle.sh`） | **24/24 × 両バイナリ**（+4 = Shift_JIS/EUC-JP E2E、+1 = 変換表再生成一致、+3 = 12-f fuzz 回帰 `oracle/fuzz-br-trim.html`（--no-style trim 経路 3 モード）。idm コーパスは gen_idm.py 再生成で復元） |
| golden（`tests/run_golden.sh`） | 1/1 × 両バイナリ |
| C↔Rust 差分 fuzz（`tools/diff_fuzz_cli.py`） | **累計 225,266 cases / 0 mismatch**（seed 20260824/1/777/424242/999/31337/20260828/555/4242/20260829/314159/271828/8888/7777/31415/2718281/55555/12344321/987654/20260902×他。stdout/stderr/rc byte 突合、--stats は計測値 scrub・決定値比較。2026-09-02 追加分（12-g〜12-i）を含む。seed 55555 は 12-f 手術の不具合（--no-style trim 経路）を検出 → 修復後 0） |
| chrome 純粋部 C↔Rust 差分 fuzz（`tools/zz_chrome_diff.py`） | **累計 240,000 cases / 0 mismatch**（seed 1/7/42/999/777/424242/20260826。1 入力行=1 出行 byte 突合、両 driver の rc≠0 も不一致扱い。ASan+UBSan 30,000 ×2 で rc=0/stderr 空。2026-08-26） |
| GUI smoke（`tests/gui_smoke.py`、`--shot` 決定ラスタ） | PASS（C のみ。X 不在環境の proxi、GUI 実機は未検証と明記。Rust 側 --shot は未移植拒否形状のみ fuzz 検証） |
| 拡張 smoke（`tests/ext_smoke.py`） | **12 checks**（C のみ。console.log 凍結 v1 含む） |
| akl CLI smoke（`tests/akl_cli_smoke.py`） | 14 checks（C のみ。Rust 側は akl-core 単体テスト + oracle script E2E で担保） |
| fuzz（`make fuzz`、500 回×5 標的: html/akl/net/store/ext） | 0 crash |
| 警告 | 全ターゲット（REL/ASAN/tests）で**ゼロ**。Rust も `cargo clippy --workspace --all-targets -- -D warnings` + `cargo fmt --all --check` 緑（生成テーブルは生成器焼き込みの `#[rustfmt::skip]` で対象外化。2026-08-26） |

### 依存・形状

- ldd: `linux-vdso / libm / libc / ld` のみ（100% self-made C11、フォントデータも自作）。
- IfNode **69B**（2026-08-10: IfStr 12B packed + kind u8 + IfNode/union packed。
  80B から −11B/ノード。16MB md 実測で parse arena −18MB）。
  メモリの正直な指標はピーク RSS（THP 込、上表 225,124KB）。

## 有効な最適化機構（採択済・現在有効なものだけ）

- md fast-DOM: `.md` を DOM 直構築（HTML 往復消去。汚染時 2 段 fallback、CLI/GUI 共通）。
  2-slice 並列 parse（`md_body_mid` ヒント共有、serial≡sliced 差分オラクルで機械保証）。
  HTML/GUI 経路も fast-DOM 優先。
- IfNode 80B → **69B**（2026-08-10: IfStr を 12B packed 化、IfNode を packed + kind u8 化、
  union packed。ノード個別 alloc は if_arena_alloc_a(align 8) 経由 — 16B アラインだと
  69B が 80B スロットを消費し削減ゼロになるため、8B アライン経路を新設し 72B 消費に。
  md の slab 方式はステップ 69B が直接効く。16MB md: parse arena −18MB（実測）。
  x86-64 unaligned アクセスはネイティブ、速度は paired で無劣化（26.8ms 前後不変））。
- arena 初期ブロック 8MB → 2MB（2026-08-10: big 2MB doc でも used 3.4MB に過ぎず
  reserved 過剰。2MB は THP 1 枚にアラインされ fault の THP 化は同効。速度 paired 無劣化。
  仮想メモリ 32MB → 8MB。`--stats` に arena_used_kb 追加（reserved 比の無駄監視））。
- lazy style（CLI 行スイープは DFS 訪問時のみ解決、style 段 0.0ms）。memo 2 スロット。
- fitdom CJK SIMD（SSE2/AVX2 dual、`__builtin_cpu_supports` dispatch）、render 単純先行+
  重なり窓、セル発行の直接 emit、arena 8MB ブロック + madvise、属性配列の一回正確確保。
- pending table text 常駐バッファ、カスケード RuleSet 索引・シャード B リンク span、
  pm_st/pm_end 巻き上げ。

## 不採用（再挑戦禁止。機構的根拠が確定している）

arena 16MB、-O3、POPULATE_WRITE 一括化、-march=native、style 2-way 並列、arena ×2/×4
成長、fitdom CJK AVX2 追加並列、render 先行の更なる分割（重なり窓 ~2ms が天井）。
真 render ストリーミングは ~7ms 余地推定で park（中〜高リスク、byte-exact 制約）。

## kill switch / 観測点（現行有効なものだけ）

- `IF_MD_PAR=0`（md 2-slice→serial）、`IF_LAYOUT_PAR` / `IF_LAYOUT_LIN`、
  `IF_STYLE_LAZY=0`、`IF_CSS_NAIVE`、`IFUTO_MD_SLOW`、`IF_MD_SIMD`。
- 観測点: `--stats`（read/parse/style/layout/render/total・nodes/links/peak_rss）、
  `--dump-tokens/--dump-dom/--dump-layout/--dump-styles/--dump-wptdom`、
  `--fragment CTX`（fragment 解析の観測点、WHATWG 13.4）。
- `--show-paths`（INV-9 副作用ゼロ）、`--slim-dom`、`--ext DIR`。

## 検証チェーン（コミット前の必須ゲート）

1. `make build/ifuto`（警告ゼロ）→ `sh tools/chk_oracle.sh ./build/ifuto`
2. `rm -f build/run_tests build/run_tests_switch && make …` + 両者実行（ヘッダ変更時は必ず rm から）
3. `sh tests/run_golden.sh ./build/ifuto`
4. `python3 tests/gui_smoke.py ./build/ifuto`
5. `python3 tests/run_html5lib.py ./build/ifuto tests/wpt-tree-construction`
6. `python3 tests/ext_smoke.py ./build/ifuto`
7. `make build/ifuto-asan`（警告ゼロ）→ 主要経路の ASAN+LSan 走査
8. `make fuzz`（500 iters × 5 標的）
9. Rust 側変更時: `cargo test --offline && cargo clippy --offline --workspace --all-targets -- -D warnings && cargo fmt --all --check`。`chrome.c`/`chrome.rs` 変更時は `python3 tools/zz_chrome_diff.py 20000`（0 mismatch）も必須
10. commit → push → `ifuto-backup.bundle` 更新（リモート整合確認後のみ）

## akl エンジン差分（2026-08-08: native 登録層 + オブジェクトモデルの導入）

`bench_akl` A/B（c757246 ビルド vs 本ビルド、paired interleaved 7 対、computed-goto、median-of-medians）:

| workload | before | after | 差 | 判定 |
|---|---|---|---|---|
| fib(22) recursive | 1.474ms | 1.502ms | +1.9% | 6/7（弱証拠） |
| arith loop 100k | 1.658ms | 1.672ms | +0.8% | 6/7（弱証拠） |

機序記録（採否の根拠）: 初回インライン実装では arith +5.4% / fib +1.7%（7/7 有意）
の退行。native 呼出機構の機械語がホットループの I$/レイアウトを汚す形（2026-07-31
例外 cold 分離事件と同型）と同定し、`akl_vm_native_call` を `__attribute__((noinline,
cold))` 隔離＋ CALL の `is_objv` 二重評価を kind 一本化で上表まで復元。残差は
CALL 命令の native 判定 1 分岐（fib は CALL ホットのため現れ、arith は騒音域）。
機能追加の必然コストとして採用・台帳記録。バイトコード列差分はゼロ（新 opcode は
既存構文からは出ない。verify の else-if 鎖に 2 分岐追加のみ=compile 時微増）。

## akl メモリ（2026-08-10、ユーザ方針「Aklus も省メモリはする」）

実測（akl_cli --rss、プロセス単体、このマシン）: 空 eval 1,544KB / Map 4,096 キー +2,600KB /
10 万文字連結 +2,700KB / fib30 不変。ピーク RSS は「生存オブジェクト量 × 64B + 生存 STR +
malloc アローナ」にほぼ比例し、無駄は小さい（実測で確認）。

採用（メモリ管理の正しさ = 省メモリの基盤）:
- **akl_gc_kind_children に AKL_OK_MAP / AKL_OK_SET を追加**（v0.4 導入時の潜伏バグ。
  MAP/SET のキー・値 STR が mark されずスイープで回収され、free スロット再利用で
  「同一 index の別 STR」に化けて Map のキー比較が壊れる。実測: キー 3,500 個超で
  size が重複扱いになり増えなくなる（GC 発火 obj 数 4,096 と一致）。データ破損級）。
- **Map/Set 上限（4,096）超過を明白失敗化**: 従来は grow 失敗を黙って無視（size が
  4,096 で止まる）。RangeError で fail-stop に変更。
- **PLOAD/PSTORE の getter/setter 探索 UAF 修正**: akl_intern（新規 STR → obj 配列
  realloc）の後に古いオブジェクトポインタを使っていた（ASan heap-use-after-free で
  検出。50000 オブジェクト生成 + 代入で顕在化）。

不採用（実測で棄却）:
- GC 時 ROPE フラット化: 連結ループで毎 GC フラット化 → heap_bytes 課金が増え続け
  GC 頻発スパイラル（10 万連結で maxrss 2.9MB → 18.7MB 悪化）。撤回。
- ROPE 深さ上限 4096 → 1024/2048: フラット化頻度が上がり strcat 10 万連結が
  +58%（1024）悪化。ROPE ノード 64B × 4096 = 256KB は許容と判断し 4096 維持。
- eval 終了時の obj 配列 cap 縮小（realloc）: glibc の realloc 縮小は OS にページを
  返さず、A/B 実測で常駐 RSS 44KB しか減らない。再拡張コストだけ残る。撤回。
- eval 終了時の VM スタック縮小: 初期 8KB のまま拡張が稀で効果なし。撤回。

観測基盤: `--stats` に `resid_rss_kb`（script 実行前後の常駐 RSS 差）を追加。
maxrss（ピーク）と常駐を分けて測れるようにした。今後の省メモリ施策はこの差分で判定する。

採用（AklObj 縮小 + Map/Set ハッシュ化）:
- **Map の keys/vals 2 本配列を交互ペア配列 [k0,v0,k1,v1,...] に統合**（AklObj 64B → 56B、
  8B/オブジェクト減。Map 1 個あたりの管理も 1 本化）。実測: Map 4,096 キーで
  maxrss −150KB、速度変化なし（fib30 74.2ms / strcat 4.3ms）。GC mark・Map メソッド
  全 7 種・teardown をペア配列対応。
- **Map/Set にハッシュ索引（開番地法、load ≤ 0.5）を追加**: Map/Set の GC 回収バグ修正
  （39e0d24）により線形走査の O(n) コストが顕在化した（Map.set 4,000 件 105ms —
  バグ時は死んだエントリとの高速比較で実費が隠れていた）。ハッシュ化で
  **0.74ms（142 倍）**。キーは SameValueZero と整合（文字列=内容、int/double 正規化、
  NaN/±0 同値、オブジェクト=index）。挿入順は kv 配列で維持（JS の Map 反復順序）。
  delete は末尾スワップ + 全再ハッシュ（稀）。data は [hash][kv] 1 ブロックで
  AklObj 56B 維持。script_multi2（Map 4,000 + 配列 5,000）は修正前 150ms →
  **58ms**（バグ有りの元々 108ms より速い）。

## akl 速度最適化（2026-08-10、ユーザ方針「Aklus は速度重視」）

- ADD の int32 fast path を VM ハンドラ内にインライン化（SUB/MUL/MOD と同型。従来は
  akl_bin_add 呼び出し経由で、fib 等の数値再帰では gprof 上 16.7% が呼出費だった。
  1,346,268 回の関数呼び出しを削除）。
- CALL ハンドラの冗長参照削除: `rt->objs[fidx]` の 2 回目取得（akl_get_obj + obj 表参照）を
  既に取得済みの `fo->code_off` に置換。
- 実測（median of 9、同一マシン）: fib30 82.2 → **79.4ms**（−3.4%）。arith/loop/primes は
  騒音域（有意差なし — 既に融合命令で VM の ADD を使わないため）。vs V8 full JIT の
  fib 比は 8.9x → **2.0x**（vsx.py 実測。C4 ガードは全 PASS 維持）。

## akl 速度最適化 round 2（2026-08-13、ユーザ方針「Aklus の高速化めっちゃやる」）

- **トップレベル関数宣言の事前登録 prescan**（cg_hoist_funcs 冒頭）: 関数本体の
  コンパイル時点で宣言名が未登録のため、本体からの参照が実行時ハッシュ検索 GLOAD
  （gprof 実測: fib30 で 53.9M 回 = 呼び出しごとに 1 回）になっていた。prescan で
  登録し直結 GLOAD_S に。**fib30 82.4 → 74.8ms（−9.2%、8/9 ペア）**。
- **式融合 LADDC / LSUBC / LMODC**（新命令 3 種）: `ローカル op 定数` の式評価を
  LLOAD;*CI の 2 命令から 1 命令に（MUL は既存 LMULC）。fib の `n-1`/`n-2` が対象。
  計算は akl_cist_compute 共有で逐語一致。**fib30 74.8 → 70.7ms**（対修正前 81.9ms
  で **−13.7%、9/9 ペア**）。callseq −1.8%、loop −2.6%（6/7）。nest/primes/strcat は
  帯内差なし（退行なし確認）。
- **budget チェックの実コスト計測**: 毎命令の AKL_BUDGET を外した実験ビルドで
  fib30 不変（+1.1% でむしろ遅い = 分岐予測が効いて無料）→ 現状維持（安全側）。
- **computed goto vs switch ディスパッチ**: switch 版（AKL_TEST_SWITCH_DISPATCH）は
  fib −5.9% だが loop +16%、nest +18% 悪化。8 ベンチ合計で computed goto が **3.0%
  有利** → 現状維持。
- **GCALL（GLOAD_S;CALL 融合）は試作→撤収**: 命令数は 10→8 に減るが、fn スロット
  確保の引数シフトがディスパッチ削減を上回り速度無意味（fib −0.6% 帯内、callseq
  +2.0% 悪化）。さらに argc≥2 でシフト式が誤る実バグも確認（callseq が全引数 a0 に
  化け 0 を返す）→ 設計ごと撤収。教訓: スタックマシンで「push を省く融合」は
  レイアウト修正コストが勝る。
- **対 V8 比較（同一マシン実測）**: fib(30) AKL **70.7ms** vs node --jitless 83.7ms
  （AKL が jitless を 15% 上回る）。vsx.py C4 fib も 10.6ms vs 12.7ms で PASS 化
  （round 1 時は FAIL）。V8 full JIT（10.2ms）には JIT なし物理で 7x（OPEN 継続）。

## akl 速度最適化 round 3（2026-08-13、ロード/コンパイル系）

- **字句解析テーブル化**: punct 57 種・キーワード 42 種を毎トークン全走査していた
  （P_N × strlen+memcmp を 2 回。lodash ロードの gprof で lex_next 39% = 最大の
  ボトルネック）。初回 1 回構築のルックアップに置換: 1 文字 punct は 256 エントリ
  O(1)、多文字 punct は先頭文字で候補を絞った最長一致（"==" が "===" を潰さない
  従来順序を維持）、キーワードは内容 FNV ハッシュの開番地。**lodash ロード
  24.4 → 15.5ms（−36.4%、11/11 ペア）**。実行時ベンチは不変（lex はコンパイル時のみ）。
- **文字列インターン strtab**: akl_intern が全オブジェクト線形走査だったのを内容
  ハッシュの開番地（1-based、負荷 ≤0.5 で再構築）に。GC が文字列を回収し得るため
  akl_gc 末尾で破棄 → 次回 intern で生存文字列のみ再構築（GC はまれなので無視可）。
  OOM 時は線形フォールバック（一意性は線形走査が保証 — プロパティ名は index 一致の
  ため intern の一意性は正しさに必須）。ロード内訳の intern は gprof 上 3% まで低下。
- **GCALL 試作→撤収の記録**: 「GLOAD_S;CALL 融合」は命令数 10→8 になるが、fn スロット
  確保の引数シフトがディスパッチ削減を上回り速度無意味（fib −0.6% 帯内、callseq
  +2.0% 悪化）。argc≥2 でシフト式が誤る実バグも確認（callseq が全引数 a0 に化け 0 を
  返す）→ 設計ごと撤収。スタックマシンで「push を省く融合」はレイアウト修正コストが
  勝る、という教訓として記録。

## akl 速度最適化 round 4（2026-08-13、コンパイル系 2 段目）

- **cap_names 二分探索化（v0.9c）**: ローカル追加（cg_local_add）と capture 判定
  （cg_captured / cg_captured_probe）が毎回 cap_names を線形走査していた。lodash の
  runInContext は 297 キャプチャで、コード生成のローカル追加 110 万回 × O(n_cap) が
  gprof 上 13%（ロード 2.0ms）。an_walk 完了後に cap_names を 1 回ソート
  （env_idx = 配列内位置の不変条件はソート後も成立）し、3 箇所を二分探索に。
  **lodash ロード 15.3 → 15.2ms、nest ベンチ −4.0%（11/13）、callseq −1.5%（12/13）**。
  退行なし（callseq +4.8% は n=13 再計測でノイズと確定）。
- **strtab 残置案（v0.9b）の撤収記録**: GC 後のエントリ残置 + 条件付き rebuild は
  開番地クラスタに死エントリが溜まり検索が O(挿入数) に劣化、test_script_gc_churn
  （300k 連結ループ）が実質ハングした。開番地は削除エントリを「空」にできないため
  残置と相性が悪い（教訓: 開番地ハッシュの寿命管理は「全破棄 or 世代管理」のみ）。
  v0.9 の無条件破棄に復帰（lodash ロード性能は不変）。

- **strtab rebuild 削減（v0.9d）**: GC で文字列を 1 つも回収しなかった場合は
  strtab の全エントリが生存しているため残置（rebuild 不要）。回収 1 個以上のみ破棄。
  検索中に死エントリへ出会うことは構造的にない（破棄後は rebuild が生存のみで再構築）。
  v0.9b の失敗（回収ありでも残置）とは異なり、開番地クラスタは常に死エントリ 0 を維持。
  **lodash ロード −2.9%（9/13）**。test_script_gc_churn のハング再発なし（ASan ビルドで確認）。

- **strtab 残置案（v0.9e）も撤収**: プローブ上限 64 付き残置（v0.9b の改良版）を試作。
  GC で死エントリがクラスタを作ると、**新規 intern（見つからない検索）が毎回 64 プローブ
  → rebuild 頻発** になり、test_akl（g_rt 共有・数百 eval 連続・文字列 churn 多数）が
  250 秒超（正常 3 秒）に劣化。教訓: 開番地ハッシュは「死エントリをスキップする検索」と
  「新規挿入が毎回起きる intern」の組み合わせで、どんな上限でもクラスタ再形成が頻発する。
  **v0.9d（GC で 1 個でも文字列を回収したら全破棄 → 次回 intern が生存のみで再構築）が
  最終形**。GC 1 回あたり rebuild 1 回がこの環境の最適解（実測: 全テスト 3 秒、lodash
  ロード −36% は維持）。

## akl 速度最適化 round 5（2026-08-13、コンパイル系 3 段目）

- **cg_local_find 後ろから検索（v0.9f）**: ローカル名の線形検索を前から→後ろからに。
  直近宣言（関数の頻出ローカル i/t/u 等）が最頻参照で、ヒットまでのステップが短縮。
  locals 内の同名は cg_local_add が再利用するため通常 1 つ、複数でも「後勝ち」= JS の
  var シャドーイングと一致（結果不変）。**lodash ロード −5.7%（8/9）、loop ベンチ
  −10.7%（12/13）、fib −8.5%（9/13）**。コンパイル時間が小さいベンチの支配要因のため
  実行系ベンチほど効く。
- **strtab 残置の最終結論（v0.9b/v0.9e 撤収の記録）**: 開番地ハッシュは GC の死エントリ
  と相性が悪い。残置（v0.9b: 無上限）はクラスタ劣化で O(n) ハング、プローブ上限付き
  （v0.9e: 64）でも新規 intern 多数環境（test_akl の g_rt 共有 数百 eval）で rebuild が
  頻発し 3 秒 → 250 秒超。**v0.9d（GC で文字列回収時に全破棄 → 次回 intern が生存のみで
  再構築）が最終形**: GC 1 回あたり rebuild 1 回が最適で、実測 3 秒・lodash −36% 維持。

## akl 速度最適化 round 6（2026-08-13、intern ハッシュ軽量化）

- **str_hash 先頭 16B 限定（v0.9g）**: 文字列インターンの FNV ハッシュを全長→先頭
  16 バイトに。ハッシュはバケット選定専用で一意性は memcmp が保証するため、異内容の
  衝突はプローブ延長で解決されるだけで正しさは不変（同一内容は必ず同ハッシュ）。
  長い識別子/文字列（lodash の minified 名やメソッド名）のハッシュコストを削減。
  **lodash ロード −1.8%（8/11）、fib −1.2%**。退行なし。
- 累計（round 2–6）: lodash ロード 24.4 → **14.0ms（−43%）**、fib30 82.4 → **71.5ms**
  （−13.2%、V8 jitless 83.7ms を上回る）、loop ベンチ −10.7%。

### callseq C4 実力差の分析（2026-08-13、未解消・台帳）

vsx.py C4 callseq（akl ≤ 1.05×jitless）が 1.10–1.18x で FAIL 傾向（計測揺れで
PASS/FAIL が入れ替わる）。直接計測（同一マシン、300K 呼び出し）: akl 14.1ms vs
node --jitless 12.0ms。内訳を隔離実験で分解:

- **CALL 1 回あたり**: akl 26ns vs jitless 23ns（差 3ns）。CALL ハンドラの全検査を
  除去した実験ビルドでも 14.1 → 13.85ms（−0.3ms）しか減らない = 検査はほぼ無料で、
  フレーム構築（引数レイアウト維持・UNDEF 埋め・frames 記録）が支配的。
- **ループ本体**: インライン展開版（CALL なし）で akl 6.3ms vs jitless 5.2ms
  （3ns/命令 vs 2.5ns/命令）。回転 8 命令（LOOPINC_G 化済み）で構造上の無駄なし。
- 結論: no-JIT 物理の範囲の構造差（スタックマシン vs レジスタマシン）で、今ラウンド
  では解消不能と判断。今後の候補: フレームのスタック統合（x86 式）、var 即初期化の
  UNDEF 埋め省略（try なし関数限定の def-use 証明）、ループ不変 GLOAD ホイスト。

## 計測工学の教訓（再発防止・現在有効）

- ワークスペースは git リセットを受け得る → 復旧 bundle を repo 内に保持し、
  HEAD 祖先を `merge-base --is-ancestor` で点検してから作業する。
- `gprof -s` は混在 gmon を合併して call 数を虚増させる → 計測前に
  `rm -f /tmp/gmon.*` 必須。gprof は複数 run を合算する（run ≒ タブの読み違い事件）。
- `grep -c "error"` は警告中の "error" 文字も拾う → `grep -c "error:"` を使う
  （0 件で `grep -c` は exit 1 = 正常）。
- RUSAGE_CHILDREN の ru_maxrss は全子の累積 max → 子ごとの計測はプロセスを分ける。
