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

## 現行スナップショット（2026-08-08 計測・`build/ifuto` 453,512B）

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

### 2MB IDM（n=5、騒音帯）

total median **17.44ms**（同日 n=5 再計測 16.0–19.4）。parse 8.0 / layout 7.0 / render 2.4ms、
peak RSS 35,596 KB、nodes=206,290。

### CLI 起動（tiny HTML render 300 連プロセス wall）

min **1.40ms** / median **1.66ms** / p90 1.96ms。

### 適合性・検証の現行値

| ゲート | 現行値 |
|---|---|
| WPT tree-construction（`tests/wpt-tree-construction`、WPT master `5b6a1e6`） | **1922/1922 (100.0%)**、skip 12 = `#script-on` のみ（fragment 196 件は `--fragment CTX` で実行済） |
| 単体テスト（run_tests + run_tests_switch 双子、ASAN+UBSan） | **623,863 checks / 0 failures** ×2（+52 = `tests/test_script.c` script 実行配線オラクル） |
| 出力 byte-exact oracle（`tools/chk_oracle.sh`） | **14/14** |
| golden（`tests/run_golden.sh`） | 1/1 |
| GUI smoke（`tests/gui_smoke.py`、`--shot` 決定ラスタ） | 51 checks PASS（X 不在環境の proxi、GUI 実機は未検証と明記） |
| 拡張 smoke（`tests/ext_smoke.py`） | **12 checks**（console.log 凍結 v1 含む） |
| akl CLI smoke（`tests/akl_cli_smoke.py`） | 14 checks |
| fuzz（`make fuzz`、500 回×5 標的: html/akl/net/store/ext） | 0 crash |
| 警告 | 全ターゲット（REL/ASAN/tests）で**ゼロ** |

### 依存・形状

- ldd: `linux-vdso / libm / libc / ld` のみ（100% self-made C11、フォントデータも自作）。
- IfNode **80B**（template content は IfDom tpl_map rare-data、Chromium RareData と同型）。
  メモリの正直な指標はピーク RSS（THP 込、上表 225,124KB）。

## 有効な最適化機構（採択済・現在有効なものだけ）

- md fast-DOM: `.md` を DOM 直構築（HTML 往復消去。汚染時 2 段 fallback、CLI/GUI 共通）。
  2-slice 並列 parse（`md_body_mid` ヒント共有、serial≡sliced 差分オラクルで機械保証）。
  HTML/GUI 経路も fast-DOM 優先。
- IfNode 80B + IfDoctype/IfTplMap rare-data 化（16MB で parse RSS −14MB）。
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
9. commit → push → `ifuto-backup.bundle` 更新（リモート整合確認後のみ）

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

## 計測工学の教訓（再発防止・現在有効）

- ワークスペースは git リセットを受け得る → 復旧 bundle を repo 内に保持し、
  HEAD 祖先を `merge-base --is-ancestor` で点検してから作業する。
- `gprof -s` は混在 gmon を合併して call 数を虚増させる → 計測前に
  `rm -f /tmp/gmon.*` 必須。gprof は複数 run を合算する（run ≒ タブの読み違い事件）。
- `grep -c "error"` は警告中の "error" 文字も拾う → `grep -c "error:"` を使う
  （0 件で `grep -c` は exit 1 = 正常）。
- RUSAGE_CHILDREN の ru_maxrss は全子の累積 max → 子ごとの計測はプロセスを分ける。
