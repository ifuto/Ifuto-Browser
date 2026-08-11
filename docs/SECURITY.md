# セキュリティ監査: 既知ブラウザ脆弱性クラスへの対処（v0.5、2026-08-10）

既存の V8 / Chromium / ブラウザエンジンで報告・悪用された脆弱性クラスを調査し、
Ifuto / AKL が同じクラスの問題を**構造的に**持たないことを監査した記録。
「不便にならない」= 実用機能（ローカル開発サーバ・標準 JS 構文・性能）を壊さない範囲で固める。

## 調査した実在の脆弱性

| CVE / 報告 | クラス | 内容 |
|---|---|---|
| CVE-2025-13223 / 13224 / 10585 / 6554（2025 年に悪用） | Type confusion | V8 がデータを誤った型として解釈しメモリ破壊 → RCE |
| CVE-2026-2441（2026-02 悪用） | Use-after-free in CSS | CSSFontFeatureValuesMap 反復中に map を mutate → 生ポインタ dangling |
| CVE-2026-11645 | OOB read/write | V8 の配列境界外アクセス |
| CVE-2024-38999 ほか多数 | Prototype pollution | `__proto__` / `constructor.prototype` 汚染で全オブジェクトに影響 |
| ReDoS 一般 | バックトラッキング爆発 | 悪意正規表現で CPU を焼く |
| DNS rebinding / SSRF 一般 | 名前解決の悪用 | public ページから private ネットワークへ誘導 |
| 画像デコーダ・フォントパーサの過去 CVE | パーサ境界 | libpng / libfreetype 系の OOB |

## 各クラスへの構造的防御

### 1. Type confusion（V8 最大の攻撃面）
- AKL は **JIT を持たない**（実行可能書き込みページが構造的にゼロ）。V8 の
  type confusion の主因である「最適化時の型情報のずれ」が存在しない。
- 値は NaN-boxed 8B（`AklVal`）。タグ空間は上位 0xFFFF 帯で、**演算結果は
  `akl_canon` により canonical NaN（0x7FF8000000000000）に正規化**され、
  タグ空間に衝突する double が生成される経路が存在しない（akl.h の設計不変条件）。
- 全ての値アクセスは kind スイッチ経由（暗黙の型再解釈なし）。

### 2. Use-after-free（CVE-2026-2441 等）
- ヒープ参照は**ポインタではなく obj 配列への u32 index**（dangling 生ポインタを
  API 面に出さない — akl.h 設計不変条件）。GC は index 不変・free スロット再利用。
- ただし C 実装内部で `&rt->objs[i]` の一時ポインタを保持したまま
  `akl_obj_new` / `akl_mkstr` / `akl_intern`（obj 配列 realloc）を呼ぶと
  dangling になる — **v0.5 監査で 13 箇所を発見・修正**（下記）。CVE-2026-2441 と
  同型の「反復中に構造を mutate する」パターン（join/slice/entries/keys のループ内生成）。
- 修正は全て「index 経由の使用直前再取得」に置換（コスト: obj 配列アクセス 1 回/反復。
  性能影響なし — 標準ベンチ不変を paired で確認）。
- **ASan で実証**: 修正前 HEAD はストレステストで heap-use-after-free を検出
  （akl_to_string の配列 ToString 経路）、修正後はクリーン。

修正一覧（src/akl/akl.c）:
`akl_to_string`（ARR パス）/ `akl_m_regex_exec` / `akl_m_regex_test` /
`akl_m_arr_join` / `akl_m_arr_slice` / `akl_m_arr_concat` / `akl_m_arr_flat` /
`akl_m_obj_entries` / `akl_m_map_keys` / `akl_m_map_values` / `akl_m_set_values` /
`akl_m_gen_next` / VM ハンドラ `NEW` / `PLOAD` / `PSTORE` / `AGET` / `IN` /
`KEYSOF` / `MCALLN` / `MAKEFS`

### 3. Out-of-bounds（CVE-2026-11645 等）
- 配列アクセスは全経路で境界検査（VM `AGET`/`ASET`・配列メソッド・`a[i]`）。
- バイトコードは実行前に verifier が走査（opcode/即値/ジャンプ先/locals 参照/命令開始）。
- ROPE 平坦化は全長検査 + 深さ上限 4096 + 反復 DFS（C 再帰なし）。
- HTML/CSS/MD/画像パーサは fuzz（500 iters × 5 標的）+ ASan 常時。

### 4. Prototype pollution（CVE-2024-38999 等）
- **AKL はプロトタイプ継承を持たない**。`__proto__` / `constructor` /
  `prototype` は通常のプロパティ名に過ぎず、他オブジェクトに影響しない。
- 実測（tests/test_akl.c 回帰）: JSON.parse・代入・Object.assign の全経路で
  `({}).polluted === undefined` を確認。

### 5. ReDoS（バックトラッキング爆発）
- 自作 regex エンジン（src/akl/akl_regex.c）に**実行ステップ上限
  RX_STEP_LIMIT = 500 万**（+ パターン長 4096 / 命令数 65536 / 再帰深さ 2048 上限）。
- 実測: `/(a+)+$/` に 64 文字 + 失敗末尾 → 即座に `RangeError: regexp execution
  limit exceeded` で停止（ページ全体がハングしない）。
- 上限は「黙って非マッチ」でなく明白失敗（AKL_COMPAT に文書化）。

### 6. DNS rebinding / SSRF（net.c 強化）
- **リダイレクト経由の private ネットワーク接続をブロック**（`if_addr_is_private`:
  127/8・10/8・172.16/12・192.168/16・169.254/16・100.64/10 CGNAT・0/8）。
- ただし**チェーン追跡方式**: 最初の接続が private（= ローカル開発サーバを直接開いた）
  なら以後のリダイレクトも private 許可。public で始まったチェーンは private への
  リダイレクトを `private redirect blocked` で拒否（DNS rebinding / SSRF の経路を塞ぐ）。
- **https → http 降格を拒否**（`https downgrade blocked`。mixed 降格の構造的防止）。
- トップレベル（ユーザ直接入力）の private URL は従来通り許可 = ローカル開発を壊さない。
- 接続は 8s 上限・送受信 10s 上限・IPv4 のみ・userinfo/IPv6 bracket 拒否（既存）。

### 7. 画像・フォント
- 画像デコーダは自作（libpng 等に依存しない。過去の libpng CVE の影響を受けない）。
  PNG はチャンク CRC 検証 + 64MB/16384px 上限、fuzz 済み。
- フォントは自前ビットマップ（libfreetype 非依存。freetype CVE の影響を受けない）。

## 検証ゲート（この監査のコミット時）
- `make test` 625,033 checks × 2（computed-goto / switch）0 failures
- ASan+UBSan: UAF ストレステスト（10 操作 × 各数百〜数千回）クリーン。
  修正前 HEAD では同一テストが heap-use-after-free を検出（実証済み）
- oracle 21/21・guismoke PASS（ローカル redir 含む）・golden 1/1・tlssmoke ok=3
- tree-construction 1922/1922（100%）・fuzz 5 標的 500 iters 0 crash
- 性能不変（paired）: fib30 75.9 / arith 82.4 / strcat 4.3 / callseq 11.3ms

## UAF 機械検出ツール（tools/check_uaf.py、2026-08-10）

「使用直前再取得」規約の機械検出をゲート化した。`make test` の一部として常時実行される。

- 検出: `AklObj *NAME = &rt->objs[...]` で取得した一時ポインタを保持したまま、
  obj 配列を realloc し得る関数（akl_obj_new / akl_mkstr / akl_intern /
  akl_to_string / akl_mkarray / akl_mkobject / akl_mkstring / akl_promise_make /
  akl_map_make / akl_set_make / akl_gc / akl_vm_frame_hidden / akl_mkhandle）を
  呼び、その後に NAME-> を使用している箇所。
- 安全と判定してスキップするパターン:
  - 使用直前の再取得（`NAME = &rt->objs[...]`）がある
  - realloc が return 文内（return で関数終了）
  - realloc の後、AKL_NEXT()（VM ハンドラの無条件終了）までに使用がない（到達不能）
  - 関数呼び出しの引数内の使用（呼び出し前に評価。**ただしループ本体内は除く** —
    2 回目以降の反復では前回の realloc が済んでいるため危険。join 型 UAF で実証）
- 条件付き return は到達不能判定に使わない（保守側: 条件が偽なら後続に到達し得る）。
- 自己テスト `--self-test` 付き（検出 2 パターン / 許容 4 パターン）。

**ツール導入時の成果**: 導入初回で `akl_m_arr_join`（ループ内の要素読取が前回反復の
ToString 生成で dangling）と `akl_m_arr_concat`（akl_obj_new 後の o 使用）の
**実 UAF 2 件を再発見・修正**。前ターンの手動監査で見落としていた（join は separator
経由・concat は未修正のまま残っていた）。CALLT / MCALLN の排他分岐、PLOAD の REGEX
lastIndex は値の先読み・再取得で明示的に安全化。

## 既知の残課題（正直な開示）
- ページ内スクリプトの `fetch()` / `XMLHttpRequest` は非対応（v1 対象）— 実装時に
  CORS / Private Network Access 相当を設計する必要がある。
- 外部 `<script src>` 取得も v1 対象（現在は明示スキップ）— 取得時に同一の
  private チェーン規則を適用すること。
- ツールは関数呼び出しをまたぐ解析をしない（単純保守）。警告が出たら人間が確認する。
