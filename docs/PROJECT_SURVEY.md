# Ifuto Browser — プロジェクト全貌調査書

調査日: 2026-08-08 / 対象ブランチ: `arena/019fe090-ifuto-browser`（HEAD `2ba2435`）
調査方法: 全ソース・全ドキュメント・全テスト・全ツールの精読 + 実ビルド/実測による現状確認

---

## 1. プロジェクトの正体

**「史上最強の軽量ブラウザ」を目指す、全部自作のブラウザエンジン + GUI ブラウザ。**
言語は **C11 のみ・依存ライブラリゼロ**（リンクは libc と libm だけ。Chromium 由来コード 0 バイト）。

| 項目 | 値 |
|---|---|
| 現在地 | **v0.3-dev**（v0.1 ✅ / v0.2 ✅ / v-chrome ✅ を完走、v0.3 進行中） |
| バイナリ | `build/ifuto` 単一ファイル **502,664 B**（stripped・LTO・GUI 統合済） |
| ldd | `linux-vdso / libm / libc / ld` のみ |
| 製品の最終形 | GUI ブラウザ（2026-07-29 ユーザ決定）。TUI は 2026-08-01 完全廃止 |
| ソース規模 | 本体 ~20,200 行（akl JS エンジン 5,053 行が最大単体） |
| テスト | 単体 623,986 checks × 双子バイナリ（computed-goto / switch 両 dispatch） |
| 品質ゲート | WPT tree-construction **1922/1922 (100.0%)**、oracle 21/21、fuzz 5 標的、警告ゼロ |

### 中核思想（効用関数: w_footprint = w_correctness = 0.20 双頭、w_security = 0.16）
- **全部自作**（Chromium 系・litehtml・WebView を棄却）
- **ページ単位 arena**（個別 free なし → dangling を構造的に不可能に）
- **単一プロセス**（v0.5 でサイト分離へ）
- **JS は自作 akl**（JIT 恒久禁止 → W^X 全域を構造保証）
- **嘘をつかない**: 未実装は未実装と明記（対応しないもの一覧を README に掲載）

---

## 2. パイプライン全体像

```
入力（untrusted バイト列。常に敵意を仮定）
  → UTF-8 デコード（FFFD 回復）              src/utf8.c
  → 文字コード正規化（SJIS系/EUC-JP→UTF-8）  src/charset.c      [v0.3 A1]
  → Markdown → HTML 変換（多層防御）          src/md.c           [.md 自動検出]
  → HTML トークナイザ                        src/html_tok.c
  → ツリービルダ → DOM                        src/html_tree.c + src/dom.c
  → <script> akl 実行（style 適用前・文書順）  src/script.c + src/akl/akl.c [v0.3]
  → CSS パース → カスケード（lazy 可）         src/css.c（RuleSet 索引 = Blink 戦略、23.32×）
  → レイアウト（整数セル座標 8px×16px）        src/layout.c
  → ペイント                                  src/render_ansi.c（セルグリッド/行スイープ）
       ├─ CLI/検証: ANSI 出力
       └─ GUI: 5x7/16x16 自前フォント → XPutImage（生 X11 プロトコル） src/gui/
  → HTTP/1.1 取得（plaintext）                src/net.c
  → 永続化（session/history/bookmarks）        src/store.c（tmp→rename→fsync 原子書換）
```

---

## 3. ソースマップ（全ファイル解説）

### 基盤層
| ファイル | 行数 | 役割 |
|---|---|---|
| `src/common.h` | 33 | 型エイリアス・ハードリミット（`IF_MAX_INPUT_BYTES` 512MB / `IF_MAX_DOM_NODES` 4M / スタック 4096）。OOM は `if_fatal` 即死 |
| `src/arena.c/h` | 136 | ページ単位 arena。ブロック 8MB（CLI は 4MB THP 直取り）。1GB 上限 |
| `src/utf8.c/h` | 125 | UTF-8 デコード（FFFD 回復）・可視幅判定（CJK 2 幅・ASCII 可視ラン分類） |

### パース層
| ファイル | 行数 | 役割 |
|---|---|---|
| `src/html_tok.c` | ~1,000 | WHATWG トークナイザ。全実体参照 2,125 名（`entities_gen.h` 機械生成）・数値参照 C1 補正・rawtext/RCDATA 状態機械・script data エスケープ状態 |
| `src/html_tree.c` | ~3,900 | ツリービルダ。**WPT tree-construction 1922/1922 = 100.0%**。quirks・foster parenting・adoption agency・table 挿入モード群・foreign content（SVG/MathML）完全表・template content 分離・fragment 解析（WHATWG 13.4） |
| `src/dom.c/h` | ~800 | DOM 型。IfNode **80B**（kind で union・template content は rare-data 分離）。slim-DOM スイッチ・入力 compaction（`if_dom_copy_strings`、GUI のみ） |
| `src/css.c/h` | ~2,200 | CSS サブセット。RuleSet 風 4 バケツ索引（id/class/tag/universal、**Blink と同戦略**、実測 23.32×）。カスケード（important→origin→specificity→文書順）。computed style intern + memo。**lazy style**（DFS 訪問時解決、16MB 文書で style 段 0.0ms） |
| `src/md.c/h` | ~2,100 | Markdown → 表示。**fast-DOM 直構築**（HTML 往復消去、2-slice 並列 parse、serial≡sliced 差分オラクル）。GFM 表・脚注。生 HTML 素通し禁止（多層防御） |
| `src/charset.c/h` | ~500 | [v0.3 A1] Shift_JIS 系 / EUC-JP → UTF-8 正規化。判定: HTTP charset > BOM > meta prescan(4096B) > UTF-8 既定。変換表 `charset_tables_gen.h` は python codec 生成・再生成一致オラクル凍結。波ダッシュ 6 件は cp932 採用と明記 |
| `src/entities_gen.h` / `src/charset_tables_gen.h` | 3,600+ | 機械生成テーブル（手編集禁止） |

### レイアウト・レンダ
| ファイル | 行数 | 役割 |
|---|---|---|
| `src/layout.c/h` | ~2,300 | 整数セル座標レイアウト。IfBox 64B。ブロック/インライン・全角折り返し・リスト・罫線・pre・リンク収集（クリック判定矩形）。2-way 並列 shard 結合。行スイープ用コンパクト行レコード `IfRLine`（24B） |
| `src/render_ansi.c` | ~1,700 | セルグリッドラスタ + **行スイープ直接発行**（グリッドレス経路、CLI 既定）。ANSI 256 色 SGR / プレーン出力。窓グリッド（viewport のみ materialize）+ 厳密増加窓カーソル |
| `src/raster.c/h` | ~180 | ピクセルラスタ（GUI 用の描画プリミティブ・RGBA→ANSI） |
| `src/render.h` / `render_sweep.h` | ~110 | GPU バックエンド境界（描画命令列は値の列。Vulkan/D3D12/Metal への脱却路） |

### GUI 層（生 X11 プロトコル。Xlib/XCB 不リンク）
| ファイル | 行数 | 役割 |
|---|---|---|
| `src/gui/x11t.c/h` | ~550 | X11 プロトコル直接実装: 接続・XAuth cookie・ウィンドウ作成・XPutImage・イベントループ・keycode→keysym |
| `src/gui/gui_app.c` | ~790 | ブラウザ UI: タブ帯・オムニボックス・ステータスバー・スクロール（差分コピー）・リンク hover/クリック・`--shot` ヘッドレスラスタ |
| `src/gui/fb.c/h` | ~200 | 自前ピクセルフォント描画（太字=横オフセット重ね、斜体=oblique shear） |
| `src/gui/font5x7.h` / `font16.h` | ~500 | 自前フォントデータ: 5x7 ASCII + 16x16 全角（かな全量・CJK 句読点・**漢字 103 字 = 第 5 陣まで**、`tools/gen_font16.py` 手設計・外部フォント持ち込みゼロ） |

### ブラウザ機能層
| ファイル | 行数 | 役割 |
|---|---|---|
| `src/chrome.c/h` | ~700 | タブ管理・セッション復元（restore-first・遅延ロード）・URL resolve・リロード・リンクフォーカス移動・履歴/ブックマーク連携 |
| `src/store.c/h` | ~470 | 永続化: `session.txt` / `history.tsv` / `bookmarks.tsv`。`$IFUTO_HOME` > `$XDG_DATA_HOME/ifuto` > `~/.local/share/ifuto`。tmp→rename→fsync 原子書換。`IFUTO_NO_STORE` kill switch。cwd に一切書かない |
| `src/net.c/h` | ~600 | HTTP/1.1 クライアント（plaintext のみ）: URL 分解・DNS・connect・リダイレクト（ループ検出）・防御的 4 パーサ + `fuzz_net` |
| `src/ifuto_pages.c/h` | ~400 | 内部ページ（新規タブ・履歴・ブックマーク等）の HTML 生成。外部入力は全てエスケープ |
| `src/sandbox.c/h` | ~180 | **自作 seccomp-BPF**（libseccomp 不使用）: allowlist 非一致は `SECCOMP_RET_KILL_PROCESS`。`IF_SB_AKL` は build/akl に既定 ON 強制適用。非対応カーネルは rc=2 で fail-stop |

### akl（自作 JS エンジン）
| ファイル | 行数 | 役割 |
|---|---|---|
| `src/akl/akl.c` | 5,053 | 単一ファイルエンジン。**NaN-boxed 8B 値 + ヒープ参照は u32 index**（生ポインタを API 面に出さない = UAF 構造排除）。lexer → 再帰下降 → one-pass codegen → 全検証 verifier → スタック VM（computed-goto 既定、switch とデュアル） |
| `src/akl/akl.h` | 137 | 公開 API: eval / 値検査 / 値生成 / **ネイティブ登録層**（v0.3: `akl_native_register`・`akl_mkobject`・`akl_prop_set`・`akl_native_throw` 明白失敗規約） / **ホストハンドル**（`AklHandleVTab` = DOM バインド用不透明参照、GC 非管理・ptr 寿命は宿主が組織） |
| `src/akl/v8.h` | 322 | V8 API 概念互換の C++ ヘッダ（header-only・libstdc++ 不使用、`make cxxtest` が ldd 機械検査）。互換範囲は docs/V8_COMPAT.md の表のみ |

akl の特徴: CoJIT（静的検証駆動 AOT 特化、runtime codegen ゼロ = JIT 禁止に非抵触）・mark-sweep GC + nursery・JS 例外（throw/try/catch/finally、cross-frame）・予算 fail-stop（命令 10M/ヒープ 16MB/obj 100k/深さ 256 固定）・ROPE 文字列・定数演算 CI 融合・mod magic-multiply。非対応（明白拒否）: 配列・ブラケットアクセス・`this`・Math 等の組込（docs/AKL_COMPAT.md が実測表）。

### スクリプト実行・拡張
| ファイル | 行数 | 役割 |
|---|---|---|
| `src/script.c/h` | ~250 | `<script>` akl 実行配線: 文書順・1 頁 1 AklRT・style 適用前・失敗は script 単位で隔離・128 script 上限・`IF_SCRIPT=0` kill switch・`has_script` parse 観測で非含有文書は走査ゼロ。最小 DOM バインド（`document.title`/`body`/`documentElement`/`getElementById`、要素 `.textContent`/`.id`/`.tagName`、`console.log`）。正本: docs/SCRIPTING.md |
| `src/ext.c/h` + `src/ext_manifest.c/h` | ~300 | 拡張機能 E1（shipped 2026-08-07）: `$IFUTO_HOME/ext/<name>/manifest.txt`（行ベース `key: value`、JSON ではない）+ `<entry>.js`（akl 言語）。**戻り値効果スキーマ**（呼べるホスト関数は存在しない = ケイパビリティ構造排除）・permissions は `status`/`log` のみ・単一効果規則。正本: docs/EXTENSIONS.md |

### フロントエンド
| ファイル | 行数 | 役割 |
|---|---|---|
| `src/main.c` | 346 | CLI。mmap ゼロコピー読み・`--shot`・`--dump-*` 観測点群・`--fragment`・`--stats`（段別タイミング + VmHWM 自己報告）・`--show-paths`（INV-9） |

---

## 4. テスト・検証体制（文化: 未検証のコードを成果に数えない）

### 検証チェーン（コミット前必須ゲート、BENCH.md §検証チェーン）
1. `make build/ifuto`（警告ゼロ）→ `tools/chk_oracle.sh`（**byte-exact オラクル 21 件**）
2. `make test` = **623,986 checks × 2 dispatch**（ASan+UBSan+LSan 常時）
3. `tests/run_golden.sh`（描画厳密 diff）
4. `tests/gui_smoke.py`（`--shot` 決定ラスタ、**51 checks**）
5. `tests/run_html5lib.py`（**WPT tree-construction 1922/1922 = 100.0%**、skip は script-on 12 のみ）
6. `tests/ext_smoke.py`（12 checks）+ `tests/akl_cli_smoke.py`（14 checks）
7. `make fuzz`（mutation fuzz 500 iters × **5 標的**: html/akl/net/store/ext）
8. コミット → push → backup bundle 更新

### テストスイート構成（tests/）
| ファイル | 内容 |
|---|---|
| `test_akl.c` | 言語実装（arith/制御/関数/文字列/等価/例外/CoJIT/GC/オブジェクト/ネイティブ/HANDLE GC flood） |
| `test_html.c` | トークナイザ/ツリービルダ/slim-DOM オラクル/fragment |
| `test_css.c` | セレクタ・カスケード + **RuleSet on/off 差分オラクル**（220 seeds 全ノード bit 一致） |
| `test_layout.c` | レイアウト + 行スイープ発行バイト列差分オラクル + リンク矩形 |
| `test_md.c` / `test_md_slice_oracle.c` | MD 変換 + serial≡sliced 並列オラクル |
| `test_script.c` | DOM 変更可視性・失敗隔離・kill switch・GC churn E2E（300k iter） |
| `test_compact.c` | 入力 compaction の ASan use-after-free 検出 |
| `test_charset.c` | SJIS/EUC-JP デコード 60 checks オラクル（codec 交叉検証） |
| `test_uichrome.c` / `test_font16.c` / `test_raster.c` / `test_http.c` / `test_arena.c` / `test_utf8.c` | 各層単体 |

### その他の機械保証
- `tools/chk_oracle.sh`: forged / 2MB / 16MB × out/ansi/w40/w160/dom/links/styles + serial≡sliced + script ON/OFF 双方向 + SJIS/EUC-JP E2E 4 件 + 変換表再生成一致 = **21/21**
- `bench/vsx.py`: akl vs QuickJS vs V8(node) クロスエンジン番兵（stdout 突合で「速度だけの嘘」を検出）
- `bench/akl_compare.py --guard`: akl_guards.json 絶対/相対閾値からの逸脱で exit 1
- `bench/js/`: fib/loop/nest/primes/callseq/strcat/small/empty の共通ベンチ群
- 双子バイナリ（computed-goto / switch dispatch）で片側だけの不具合を封殺

---

## 5. ベンチマーク現状（2026-08-08 実測、BENCH.md）

| 指標 | 値 |
|---|---|
| バイナリ | 502,664 B（ldd = vdso/libm/libc/ld） |
| コールドスタート | min 1.40ms / median 1.66ms（spawn 込み） |
| 空タブ UI 常駐 RSS | 1.43MB（天井 4MB） |
| 50 タブ セッション復元 | 0.11ms（遅延ロード込み、メタ 14.7KB） |
| 18KB 文書 | ms 級 / ピーク RSS 1.9MB |
| 2MB ストレス文書 | 17.5ms / 35.7MB（騒音帯実測） |
| 16MB IDM（131,090 ブロック / 1.6M ノード） | total 123.10ms / peak 225MB（parse 55.0 / layout 52.6 / render 15.5 / style 0.0ms） |
| GUI idle CPU | 0%（read ブロックのみ） |
| 入力 compaction（GUI） | 16MB で VmHWM −3.4MB、50 タブ換算 −170MB |

支配項は parse→layout。不採用（再挑戦禁止）: arena 16MB・-O3・POPULATE_WRITE 一括化・-march=native・style 2-way 並列。

---

## 6. セキュリティモデル

- 信頼境界: 外部コンテンツ → エンジン → 端末エスケープ出力
- 全経路 ASan+UBSan+stack-protector+fuzz 常時（UBSan runtime note 0 を確認済）
- akl: JIT 不在（W^X 全域）、IO プリミティブ構造不在（akl.c に fopen/open/socket ゼロ = 機械 grep 監査可能）、予算 fail-stop 四面（時間/メモリ/深さ/syscall）
- seccomp-BPF: akl ランナーは既定 ON 強制適用。allowlist 非一致は KILL_PROCESS（errno soft-fail 不採用）
- 残存攻撃面（正直に開示）: 出力端末への制御シーケンス混入・DoS（カウンタ上限で打ち切り）・単一プロセス・CSS セレクタバックトラック（depth 4096 上限内）
- やらないこと: 自作 TLS・自作暗号・外部コード盲目的取り込み・exploit 開発

---

## 7. ロードマップ（ARCHITECTURE.md §6）

| 版 | 内容 | 状態 |
|---|---|---|
| v0.1 | 垂直スライス: パース〜端末描画 | ✅ |
| v0.2 | 適合性マイルストーン: WPT 100.0%・fragment・全実体参照・HTTP/1.1・文字コード層（A1） | ✅（2026-08-07 完了 + A1 2026-08-08） |
| v-chrome | GUI クローム: 生 X11 単一 UI・タブ/オムニ/永続化・自前フォント（漢字 103 字）・`--shot` 検証 | ✅ |
| v0.3 | `<script>` akl 実行（**前倒し完了 2026-08-08**）・ソフトピクセルラスタ + backend 境界凍結・画像デコード（BMP/PNG 静的）・Vulkan（headless 検証可能なら） | 進行中。**DOM バインディング本体（querySelector 等）が次** |
| v0.4 | Akl 本体: 配列/オブジェクト ✅（v0.2）、ネイティブ層+オブジェクト ✅（v0.3 前倒し）、DOM バインディング拡張・src 取得（net.c と連結） | 進行中 |
| v0.4b | array-DOM（IfNode を u32 index リンクの巨大配列へ。計測値を出してから移行） | 未着手 |
| v0.5 | プロセス分離（サイト分離）+ seccomp サンドボックス | 未着手 |
| 常時 | footprint ラチェット・fuzz 拡大・依存ゼロ維持 | 運用中 |

---

## 8. 開発規律・文化（このプロジェクトの掟）

1. **依存ゼロ**（libc のみ）・警告ゼロ・ASan+UBSan 常時
2. **未検証のコードを成果に数えない**。壊れた計測値は報告しない
3. 「軽量」は数値で語る。trade-off は列挙してから決める
4. 仕様の正本は docs/ の凍結文書（SCRIPTING / CHARSET / EXTENSIONS / AKL_COMPAT / V8_COMPAT / BLINK_COMPAT / SANDBOX）。コード変更と文書改訂は**同じコミット**
5. 再挑戦禁止リスト（計測で棄却済みの最適化は二度やらない）
6. kill switch 文化: IF_MD_PAR / IF_LAYOUT_PAR / IF_STYLE_LAZY / IF_CSS_NAIVE / IF_SCRIPT / IFUTO_NO_STORE / IFUTO_MD_SLOW
7. 決定は感情でなく A/B 実測 + 符号検定 + 機械オラクル（byte-exact）で下す
8. 環境消失への耐性: コーパスは決定的生成器（tools/gen_idm.py）・backup bundle を repo 内保持

---

## 9. 調査時に実施した検証（本セッション実測）

| ゲート | 結果 |
|---|---|
| `make`（リリースビルド） | ✅ 502,664 B・警告ゼロ・ldd 確認 |
| `make test`（双子バイナリ） | ✅ 623,986 checks / 0 failures ×2 |
| `tools/chk_oracle.sh` | ✅ 21/21（初回 14/21 は `.arena/idm` コーパス消失による環境要因 → `tools/gen_idm.py` で再生成後 21/21 復帰を確認） |
| `make guismoke` | ✅ 51 checks PASS |
| `make golden` | ✅ 1/1 |
| WPT tree-construction | ✅ 1922/1922 (100.0%)、skip 12 |
| `--shot` ラスタ | ✅ 1000×720 PPM 生成（13 色・期待配色: 白背景/ダーク UI/アクセント青/赤見出し） |
| script 実行 E2E | ✅ `document.getElementById`→textContent 変更・title 変更・console.log が描画に反映 |

**結論**: 単一コミット（2ba2435）に凝縮された、設計文書・実装・検証インフラが一体になった完結したプロジェクト。全ゲート緑。次の一手はロードマップ上「DOM バインディング本体（querySelector 等）と `<script>` 実行配線の拡張」「v0.3 ソフトピクセルラスタ/backend 境界凍結」「v0.4b array-DOM」。
