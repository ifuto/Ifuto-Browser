# Ifuto スクリプト実行凍結仕様（v0.3 — `<script>` akl 実行配線）

正本は本ファイルのみ。`src/script.h` / `src/script.c` / `src/akl/akl.h`（AklHandleVTab）
のコメントは本書の要約であり、仕様を変更する時はコードと本書を**同じコミット**で改訂する。

## 1. 実行の構造

- 対象: **HTML 名前空間の `<script>` 要素のみ**（svg ns script は収集しない）。
- 評価順: 文書順。1 ページにつき **1 つの AklRT**（グローバル共有はブラウザ本家と同じ意味論。
  `var` は script を跨いで見える。失敗した script の `var` は残らない — 本家 spec 同型）。
- 実行点: style 適用前（DOM 変更が style/layout/render に反映される本家順序）。
  CLI の dump 系観測モード（`--dump-wptdom` / `--dump-tokens` / `--dump-dom`）は
  実行点より上流で return するため **script は走らない**（文字列観測オラクル不変）。
- JIT は恒久禁止（akl 方針。バイトコードインタプリタのみ）。
- **Markdown 文書では script は一切実行しない（2026-08-08 実測凍結）**: md は
  インライン HTML をテキストとしてエスケープ保持する法則（生 HTML 素通し禁止）
  により `<script>` 要素が DOM 上に構築されない（fast/2 段両経路で実測確認）。
  よって md では `has_script` が立たず、収集・実行コストは構造的に発生しない。

## 2. スキップ規則（明白に数える: `IfScriptReport.n_skipped`）

以下は実行せずエラーでもない（明白スキップ）:

1. `src` 属性が非空（外部スクリプト取得は v1 非対象）
2. `type` 属性が非空かつ `text/javascript`（大小無視）以外（`module`・`text/vbscript` 等）
3. 本文が空（`<script></script>`）

## 3. 失敗隔離（fail-stop 粒度 = script）

- 構文エラー / 実行時例外 / **instruction budget 枯渇**（無限ループ爆弾）は
  その script のみ打切り。**後続 script と描画は継続**。
  各失敗は 1 行 `[script] FAILED: <理由>`（改行畳み・理由 128 文字打切り）を log へ。
- 本文に NUL を含む、または本文が 4MB 超は評価せず失敗として数える。
- 1 ページの実行上限は **128 script**。超過分は走らず、
  `[script] script count truncated at 128 (found N)` を 1 行で明白に報告（母数 N は正確に数える）。

## 4. kill switch

- `IF_SCRIPT=0` で完全 no-op（走査・log 出力・report カウントの全てが発生しない）。
- 加えて `IfDom.has_script`（parse 時観測スイッチ）で script 非含有文書は
  **走査自体を行わない = ゼロコスト**（16MB bench ミッション防御の構造証明）。

## 5. ページへ露出するオブジェクト（v1 固定面）

- `console.log(...)` — 1 行出力: `[script:console] v1 v2 ...`（空白結合・改行畳み・960B 打切り）。
  値は akl の tostring 規約。拡張 console（`[ext:NAME]`）とは prefix で区別。
- `document`
  - `document.title` — 取得: `<title>` の trim 済みテキスト。設定: head 内 title 要素を更新
    （無ければ head があれば生成し `IfDom.title` を同期）。
  - `document.body` / `document.documentElement` — 要素 HANDLE（無ければ null）。
  - `document.getElementById(id)` — 一致要素 HANDLE（無ければ null。id 値は case-sensitive）。
  - `document.querySelector(sel)` — 最小 CSS セレクタで最初の一致要素（無ければ null）。
    対応: `tag` / `.class` / `#id` / 複合（`div#main.nav`） / 子孫結合子（空白区切り、
    例 `div section p`）。**非対応（null を返す明白拒否）**: `>` 子結合子・カンマ・
    属性セレクタ・疑似クラス・複数 class。
  - `document.getElementsByTagName(name)` — 一致要素の配列（文書順。`*` は全要素。
    上限 1024 件で切詰め。未知タグ名も文字列 CI 照合）。
- 要素 HANDLE
  - `.textContent` — 取得: 子孫 TEXT 連結。設定: 子群を単一 TEXT に置換（値は JS ToString 経由）。
  - `.id`（属性値文字列）、`.tagName`（HTML ns は大文字）。
  - `.getAttribute(name)` / `.setAttribute(name, value)` — 属性の取得/設定
    （値は JS ToString 経由・DOM arena へ複製。name は 127 バイト上限）。
  - `.style` — CSSStyleDeclaration HANDLE。プロパティ get/set が style 属性を操作
    （例: `el.style.color = 'blue'` は style 属性を更新し、以後のスタイル適用に反映）。
    既存プロパティは置換・無ければ追加。非対応プロパティも文字列として保持。
- HANDLE の未定義プロパティ読みは **undefined**、未定義メソッド呼出しは
  **TypeError: not a function**、setter 不在/拒否は TypeError。`typeof handle === "object"`、
  文字列化は `"[object HTMLDocument]"` / `"[object HTMLElement]"`。
- 配列高階関数（`map`/`filter`/`forEach`/`some`/`every`/`find`/`findIndex`/`reduce`）は
  スクリプト内で使用可能（コールバックはクロージャ・ネスト・例外伝播に対応）。
- **非対象（v1 で明白拒否）**: src 属性取得・イベント・`innerHTML`・DOM 走査 API
  （children/parentNode 等）・classList。akl 言語側の非対応機能は docs/AKL_COMPAT.md が
  正本（例: `String()` コンストラクタは未実装 — 文字列化は `''+v` を使う）。

## 6. 寿命・構造安全の証明（HANDLE ptr 規約）

- HANDLE の `ptr`（IfDom*/IfNode*）は DOM arena 所有。GC は ptr を触らない（akl.h 契約）。
- script RT は `if_script_run` 内で **必ず破棄** され、DOM arena はパイプライン終端
  （GUI では tab 閉鎖）まで生きる → dangling はスケジュール上構築不能。
- script.c の module statics（g_arena 等）は「1 プロセス同時 1 eval」の構造前提に依存
  （md/layout 並列ワーカーは akl に一切触れない）。
- akl 側 GC 規約: vtab コールバック内で生成した値は nursery で一時保護され、
  呼出し直後の PUSH/束縛で根付く（akl.c OP_PLOAD/OP_PSTORE/OP_MCALL HANDLE 分岐の gc_sp 同期）。

## 7. 計測の正直性

- CLI `--stats`: script 実行時間は独立枠（`scripts= errors= skipped= script_ms=`、
  **script 実行時のみ追加行**）。主行の `style=` からは script 分を差引いて帰属を汚さない。
  `total=` は真の wall time のまま（script 非含有文書は has_script 早期 return で既存値と一致）。
- budget は akl 製品既定（10M insn / ヒープ 16MB 等。`akl_tune` しない）。

## 8. テスト台帳

- `tests/test_script.c`: DOM 変更の可視性・失敗隔離・スキップ規則・kill switch・
  has_script ゼロコスト・svg 除外・console 行規約・128 打切り・
  **GC churn E2E**（300k iter ≈37MB garbage を 16MB 既定ヒープに通す: GC 不発火なら
  heap budget で eval 失敗するため、成功 ≡ GC 複数回発火＋handle 生存＋IfNode* 有効）。
- `tests/test_akl.c` `t_handles`: AklHandleVTab の基本面凍結（typeof/tostring/
  unknown→undefined/store rejected/not a function/native_throw/C 側実体共有 identity）と
  **heap 1MB 絞りの GC flood**（50k iter ≈6MB ≈ cap 6 倍 throughput: 成功 ≡ GC 複数回
  発火かつ get コールバック内確保・mcall 戻り値ハンドルの rooting が正しい）。
  両者とも ASan+UBSan+LSan 常時の双子バイナリで検査。
- E2E byte-exact オラクル `oracle/script.html`（`tools/chk_oracle.sh` 16-17 件目系。現行 21 件）:
  mutation が描画に出る（script.out）／`IF_SCRIPT=0` で出ない（script.killed）双方向。
- `tests/test_html.c` slim-DOM オラクル: **script は本文ごと残る／template は従来通り剃る**
  （法則「画面描画に関係ないものは DOM しない」の補足: script は v0.3 で「描画に無関係」
  ではなくなった = 例外ではなく法則の正しい適用）。
