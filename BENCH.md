# BENCH.md — Ifuto 計測ベースライン

**軽量は測定可能か、嘘つきかのどちらかである。** このファイルは Ifuto の公式ベースライン。

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
- bench_v8x 値不変（fib22=17711 / arith=905003 / mixed=7.9996e+07）。
- 天井監視: ifuto 219,776 B は旧 TUI 天井 200 KB を **+19,776 B 超過**。成因は md.c 追加で
  機能マイルストーンに伴うもの（悪化ではないが）— v0.2 系の新天井 240 KB をここに設定し、
  以後のコミットはこれに拘束される。

## 2026-07-31: V8x v0.1（GC + ROPE + 融合命令）— QuickJS 比「軽さ・速さ」全軸クリア、V8 --jitless 比も全軸クリア

構成: mark-sweep GC（adaptive pacing, ルート=VM スタックスナップショット+globals+nursery+last_val）、
ROPE 文字列（`V8X_OK_ROPE`、償却 O(1) 連結・遅延 flatten・深さ 4096 上限）、
融合命令群（LINC/GINC、CJMPF_L/G、CJMPF_MODG/L/GG、CJMPF_MULGG、GMULC/LMULC、*CI、
*CI+store 再融合 `*_G/_L`、3 アドレス `*_GX/_LX`、GADD_P/LADD_P/GADD_G、LADD_LL、
RET_L、STORE_PV 系、`for` ループ回転 + LOOPINC_G）。
融合の意味保持は「汎用路が非融合命令列と同じ関数を同順序で呼ぶ」構造共有で証明
（v8x_cist_compute / v8x_binfv_compute を単独命令と再融合命令が always_inline で共有）。
検証: 単体 1,875 checks×2 dispatch 0 fail・fuzz_v8x 500 0 crash・WPT 97.0% 不変・tui_smoke PASS。

### 比較表（実測・同一実行セットの median。±10% 程度の実行間変動あり）

時間（プロセス wall ms、起動込み。小さいほど良い）:
| bench | v8x | qjs | node --jitless | node(full JIT) |
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

RSS（rssrun 子 rusage）: v8x **2.0〜2.2 MB** / qjs 2.8〜3.0 MB / nodejit 41.5 MB / node 41.8〜46 MB。
バイナリ（stripped）: v8x_cli **98 KB** / qjs 1,027 KB（0.095×）/ node 107 MB。

guard（常時アラーム）: `make guard`（bench/v8x_guards.json。絶対閾値: バイナリ ≤128KB・
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
- v8x_cist_compute のような共有ヘルパは `__attribute__((always_inline)) inline` を
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

## 2026-07-29: V8x v0.0（自作 JS エンジン、C11・JIT なし）初回測定

採用仕様: 字句→recursive-descent（AST プール）→one-pass codegen→**全検証 verifier**
→スタック VM。値は NaN-box 8B、ヒープ参照は obj 配列への u32 index、命令・呼出深度・
解析深度・ヒープバイト・ノード数・引数列・ソース長の全 budget を fail-stop で管理。
JIT は構造的に不在（W^X 全域、実行可能書き込みページ 0）。

### dispatch 実測（bench/bench_v8x.c, REL, 7 プロセス×7 回の median-of-medians, 本コンテナ）
| workload | computed-goto | switch | goto/switch |
|---|---|---|---|
| fib(22) recursive | 2.567 ms | 3.324 ms | **1.295×** |
| arith loop 100k | 9.198 ms | 10.418 ms | **1.133×** |
| mixed stmt loop 20k | 2.583 ms | 2.902 ms | **1.123×** |
| str concat 2k | 0.391 ms | 0.393 ms | 1.005× |

裁定: **computed-goto を既定**（GCC/Clang で有効、他は switch フォールバック。
`V8X_TEST_SWITCH_DISPATCH` で強制切替可）。dispatch 支配の算術系で +12〜30%。
文字列系はインターンハッシュ支配で差なし。理論（間接分岐の局所化）と一致。
両モードとも同一単体テスト全緑（run_tests / run_tests_switch の双子運用で固定）。

### サイズ会計（実測）
- スタンドアロン差分（空 main との diff, stripped/LTO/gc-sections）: **45,392 B**。
- ifuto（195,192 B）に全 v8x シンボル保持で仮リンク: **244,376 B（+49,184 B）**。
- 内訳（-O2 単体 .o）: .text.vm_exec 18,919 B（42%）、lex_next 2,816、
  p_stmt 2,674、cg_stmt 2,436、v8x_eval 2,221、p_unary 1,862、他 ≈16 KB。
- **200 KB 天井との関係**: v0.0 は本体未リンクで天井維持（195,192 B 不変）。
  v0.4 統合時に天井を再設定する。削減材料の候補（未実施・見積もりも未計測）:
  dtoa の自前化で %g/%.0f printf 経路を剥がす、エラーメッセージの ID 化、
  vm_exec の命令削減。再設定するときはこの 3 案の実測値を添えて判断する。

### 検証
- 単体: **1,813 checks / 0 fail**（v8x 追加分 118。1695 → 1813）。
  NaN canonical、±Inf、int32→double 境界、INT32_MIN % -1 の UB 回避、
  短絡評価の生値、0.1+0.2 の厳密 binary64、ToInt なし連結 ToString（往復最短精度）、
  グローバル定数 NaN/Infinity の const 保護、parse depth・命令・深度・ヒープの
  各 budget の fail-stop、budget 枯渇後の eval 健全性、rt 間のグローバル独立性。
- fuzz: fuzz_html 500 + **fuzz_v8x 500** = 1,000 iter / 0 crash（ASAN/UBSAN）。
- WPT tree-construction: **88.1%（1,521/1,726）不変**（エンジン側差分ゼロの確認）。
- 既知の v0.0 境界（台帳）: 配列/オブジェクトリテラル、三項 `?:`、`++`/`--`、
  複合代入 `+=`、arguments/this/new/prototype、ASI 完全形。let/const は関数スコープ近似。
  dtoa は往復最短精度（15→17g 探索）で JS と整数帯・最短表記を一致させたが、
  指数非化帯の境界（1e21 前後の形式）までの完全一致は未保証。
