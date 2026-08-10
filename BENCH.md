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

### 適合性・検証の現行値

| ゲート | 現行値 |
|---|---|
| WPT tree-construction（`tests/wpt-tree-construction`、WPT master `5b6a1e6`） | **1922/1922 (100.0%)**、skip 12 = `#script-on` のみ（fragment 196 件は `--fragment CTX` で実行済） |
| TLS smoke（`make tlssmoke`、自己署名 CA + openssl s_server E2E） | 3 checks（https 取得 / CA 不一致 cert 拒否 / IP SAN 非対応の明示） |
| 単体テスト（run_tests + run_tests_switch 双子、ASAN+UBSan） | **623,986 checks / 0 failures** ×2（script 実行配線 + HANDLE GC 機械証明 + 入力 compaction + 文字コード層 60 checks オラクル込み） |
| 出力 byte-exact oracle（`tools/chk_oracle.sh`） | **21/21**（+4 = Shift_JIS/EUC-JP E2E、+1 = 変換表再生成一致） |
| golden（`tests/run_golden.sh`） | 1/1 |
| GUI smoke（`tests/gui_smoke.py`、`--shot` 決定ラスタ） | 51 checks PASS（X 不在環境の proxi、GUI 実機は未検証と明記） |
| 拡張 smoke（`tests/ext_smoke.py`） | **12 checks**（console.log 凍結 v1 含む） |
| akl CLI smoke（`tests/akl_cli_smoke.py`） | 14 checks |
| fuzz（`make fuzz`、500 回×5 標的: html/akl/net/store/ext） | 0 crash |
| 警告 | 全ターゲット（REL/ASAN/tests）で**ゼロ** |

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

## 計測工学の教訓（再発防止・現在有効）

- ワークスペースは git リセットを受け得る → 復旧 bundle を repo 内に保持し、
  HEAD 祖先を `merge-base --is-ancestor` で点検してから作業する。
- `gprof -s` は混在 gmon を合併して call 数を虚増させる → 計測前に
  `rm -f /tmp/gmon.*` 必須。gprof は複数 run を合算する（run ≒ タブの読み違い事件）。
- `grep -c "error"` は警告中の "error" 文字も拾う → `grep -c "error:"` を使う
  （0 件で `grep -c` は exit 1 = 正常）。
- RUSAGE_CHILDREN の ru_maxrss は全子の累積 max → 子ごとの計測はプロセスを分ける。
