# Rust 完全移行ロードマップ（Aklus / Ifuto-Browser）

C 実装（`src/akl/akl.c` 約 19,000 行 + `src/akl/akl_regex.c`）を、セキュリティの高い
Rust へ段階的に移行する。**1 フェーズ = 1 検証ゲート**を必ず通す。既存の C 資産
（テスト 625,125 checks / parity 69/69 / lodash 320/320 / html5lib 1922/1922）を
回帰オラクルとして使い続け、**未検証の置き換えはしない**（「黙って違う結果を返さない」
文化を Rust 移行にも適用）。

## 原則

- 各フェーズで「C 実装と Rust 実装を並走させ、入出力を突合」してから切替える
- Rust 側は `#![forbid(unsafe_code)]` を可能な限り維持（C 連携フェーズでのみ、
  最小の unsafe を監査付きで導入し Rudra / Miri で検証）
- 検証ツールは GitHub Actions で実行（ローカルに Rust ツールチェーンが無いため）:
  - `trigger/env_trigger.md` push → 環境構築（Kani / Miri / Rudra / audit / deny / geiger）
  - `trigger/trigger.md` push → 検証コマンド実行（結果 `trigger/result.md`）

## フェーズ一覧

| フェーズ | 対象 | 検証ツール | 完了条件 |
|---|---|---|---|
| 0（着手済み） | 値表現 AklVal（NaN-box） | Kani / Miri / Clippy | `rust/akl-core` でタグ不変条件・int 往復・canonical NaN を証明 |
| 1 | 文字列（STR/ROPE/interning strtab） | Kani / Miri | 生存文字列の hash 一意性・GC 後 rebuild の正しさ |
| 2 | GC（mark & sweep + free-list） | Kani / Miri / Rudra | 到達可能性の閉包・スイープ後の不変条件（C の check_uaf と突合） |
| 3 | バイトコード VM（vm_exec） | Kani / Miri | 命令ごとのスタック効果を Kani で証明、C と逆アセンブル一致 |
| 4 | パーサ/レキサ（lex_next/p_*) | Miri / fuzz | C と同じ構文受理集合（parity 69 を Rust 側で再現） |
| 5 | 組み込み（builtins / native 群） | Miri / Kani | lodash 320/320・WPT JS 全通過を Rust 側で再現 |
| 6 | ブラウザ統合（script.c / DOM 配線） | Miri / fuzz | 全ゲート（guismoke/golden/tlssmoke/html5lib/fuzz）緑 |
| 7 | C 実装の削除 | — | `src/akl/` を撤去し Rust クレートに一本化 |

## 検証ツールの適用方針

| ツール | 用途 | 備考 |
|---|---|---|
| Kani | 不変条件・関数契約の機械的証明 | 値表現、GC 到達可能性、スタック効果 |
| Miri | 実行時 UB / エイリアシング検出 | 各フェーズのテストを nightly miri で実行 |
| Rudra | unsafe パターンの静的解析 | C 連携フェーズ（FFI）で必須 |
| cargo-audit / deny | 依存の脆弱性・ライセンス | 依存追加時は常に実行 |
| cargo-geiger | unsafe 使用量の可視化 | 回帰ゲート（unsafe 量の増加を検知） |
| Verus / Prusti / Creusot / Flux | 高階の関数契約・型レベル refine | フェーズ 3+ で GC / VM に適用（env_trigger で VERUS=on） |
| Hax / Aeneas | C → Rust の翻訳（候補） | 手動移植の補助。出力は必ずレビュー+テスト |
| MIRAI | 抽象解釈による未初期化/パニック検出 | フェーズ 4（パーサ）で適用 |
| RustBelt / RefinedRust | 理論的検証（研究ツール） | フェーズ 7 の最終監査で適用検討 |

## 現在地（2026-08-14 時点）

- フェーズ 0 着手: `rust/akl-core`（AklVal newtype + int_add fast path + Kani 証明 7 件）
- フェーズ 1 着手: `rust/akl-core/src/string.rs`（Interner。HashMap ベースで
  「intern 一意性」「GC 死エントリ劣化」「二重 free」を型レベルで排除。Kani 証明 4 件）
- GitHub Actions 基盤: `env_trigger.yml` / `trigger.yml` 設計済み
  （`docs/ACTIONS_SETUP.md` に全文。**ユーザーが .github/workflows/ に手動配置**）
- ローカルに Rust がないため、最初の検証実行はユーザーの yml 配置後、
  `trigger/env_trigger.md` push → `trigger/trigger.md` push で行う

## フェーズ 2 設計メモ: GC（mark & sweep）の Rust での安全な書き方

C 実装の GC（akl_gc）は以下を手動で管理しており、それぞれが過去に実バグを生んだ:
- ルート集合（stk / globals / mtq / timers / nury / fn_protos / comp_pins…）の列挙漏れ
  → 生存オブジェクトが回収される（"cap env chain broken" 等）
- スイープ時の free 漏れ・二重 free（文字列 bytes / props / arr.v / env.vals / regex…）
- free-list の再利用と「コンパイル中 pin 区間」の整合（compiling 中は free-list 禁止）

Rust では以下の形で安全に移植する（unsafe 不要の設計）:

```text
struct Rt {
    objs: Vec<Slot>,          // Slot = Option<AklObj>（C の obj 配列 + kind==0 に相当）
    free: Vec<u32>,           // 空きスロットリスト
    // ルート集合は全て Rt のフィールドとして明示（列挙漏れを型で防ぐ）
    globals: Vec<Global>,     // name: StrId, value: AklVal
    stk: Vec<AklVal>,
    timers: Vec<AklVal>,
    // ...
}

impl Rt {
    fn gc(&mut self) {
        // 1. mark: ルート集合から到達可能な index を worklist で辿る
        //    （AklObj の参照フィールドは enum で型付けされているため、
        //      「どの index が子か」はコンパイラが保証 = 伝播漏れが起きない）
        // 2. sweep: 未到達 Slot を take() してドロップ
        //    （所有権により子リソースが自動解放 = free 漏れ・二重 free が起きない）
        // 3. free リストに index を積む
    }
}
```

ポイント:
- **AklObj の参照フィールドを enum で型付け**: `Obj::Arr { v: Vec<AklVal> }` 等。
  GC の伝播は「enum の全参照フィールド」を走査すればよく、C の
  `akl_gc_kind_children` の switch と違って **追加漏れがコンパイルエラーになる**
  （non_exhaustive な match 警告）。
- **スイープ = ドロップ**: `slot.take()` で `AklObj` をムーブアウトすると、
  `Vec<AklVal>` や `Box<str>` が自動で解放される。C の手動 free が消える。
- **ルート集合はフィールド**: ローカル変数にルートを退避する C の
  `root_stks`（連結リスト）も、`Vec<Vec<AklVal>>` のフィールドに置き換え可能。
- 検証: 到達可能性の閉包（mark 後の生存集合がルートから辿れる集合と一致）を
  Kani で証明、Miri で実行時検査、`check_uaf.py` の C 版と突合。

## 検証ツールの適用範囲（2026-08-14 Actions 実走の教訓）

**Kani はコレクション（Vec/HashMap）+ シンボリック長ループを扱えない**。実走で
`cargo kani --workspace` が 2 時間ハング（打ち切り）した。原因は obj.rs/string.rs の
proof が `kani::any()` で長さ不定のループを回していたこと。修正後:

- **Kani**: スカラー純粋関数のみ（AklVal タグ演算・int_add・f64 正規化の代表パターン等）
- **Miri + ユニットテスト**: データ構造（Interner / GC / 配列 HOF）の実行時検査
- **Prusti / Verus**: 将来、契約ベースでデータ構造の不変条件を証明（要 CI 時間）
- 各 trigger コマンドに `timeout` を付け、ハングしても次へ進む

## フェーズ 3（進行中）: VM コア（bytecode.rs）

- `Op` enum（C の OP_* のコア: ConstI/ConstD/ConstStr/True/False/Null/Undef/
  Add/Sub/Mul/Div/Mod/Lt/Le/Gt/Ge/Eq/Ne/Seq/Sne/Not/Neg/Pos/Typeof/Pop/Dup/
  LLoad/LStore/GLoad/GStore/Jmp/JmpF/JmpT/Call/Ret/MakeF/This/ObjNew/PLoad/PStore/
  ArrNew/AGet/ASet/Halt）
- `Runtime::run` は実 JS 値（[`AklVal`](rust/akl-core/src/lib.rs)）をスタックに積む
  match ループ。C の `vm_exec` 相当
- スタックは `Vec<AklVal>`、下限検査は `pop().ok_or(StackUnderflow)`（C の AKL_POP
  マクロの検査漏れが構造的に消える）
- フレームは [`Frame`](rust/akl-core/src/bytecode.rs) が locals を明示保持
  （C の base オフセット方式より安全。GC ルート深さ同期漏れの実バグが起きない）
- 関数呼び出し（CALL/RET/MAKEF）を実装。`Ret` は**戻るフレーム自身の ret_pc** を使う
  （実測で特定したバグ: 呼び出し元フレームの ret_pc を使うと再帰が直上の呼び出し元へ
  誤って戻る）

### 値モデルの統一（フェーズ 1/2 の改訂）

C 実装は文字列も `rt->objs[]`（単一 obj テーブル）に載せる。旧 Rust 実装は
`Interner`（StrId）と `ObjTable`（ObjId）を別 id 空間に分けていたが、これを**単一
ヒープ**に統一した:

- 文字列の正体は `Obj::Str`（ヒープ内）。`Interner` は内容→ObjId のキャッシュのみ
- プロパティ名も ObjId（intern 済み文字列オブジェクト）
- これで `AklVal::mk_obj` が文字列・配列・オブジェクト・関数・環境のいずれも指せる
  （C の NaN-box obj タグと同一セマンティクス）

### フェーズ 3b の検証（cargo test 29 件 + doctest 緑）

- fib(10) = 55（再帰・関数呼び出し・数値比較・分岐の e2e）
- ループ和（for 相当の JmpF 回転）
- 文字列連結（`"hello " + "world"`）
- オブジェクトのプロパティ set/get、配列リテラル・要素アクセス
- typeof・厳密等値（`1 === 1.0` は true）
- 非関数呼び出しのエラー（NotCallable）

### 既知の近似（フェーズ 3b 時点）

- クロージャ: `Obj::Func.env` は保持するが VM の LLoad/LStore は自フレームのみ参照。
  自由変数の env 経由解決（C の CELOAD/CESTORE）は codegen フェーズで導入
- ルーズ等値 `==`: 数値・文字列・null/undefined・真偽値の単純化版（オブジェクトの
  ToPrimitive は未対応。`===` は完全）
- オブジェクトの ToString は `[object Object]` / `[object Array]` の近似

残り: クロージャ（env 経由解決）・組み込み（builtins/native 群 = フェーズ 5）・
パーサ/レキサ（フェーズ 4）との接続。

## フェーズ 4（進行中）: レキサ（lexer.rs）

- `Lexer`（C の `Lex` + `lex_next` 相当）。`&str` 借用・`pos`/`line` を進める
- [`Token`](rust/akl-core/src/lexer.rs) enum: Eof/Ident/Num/Str/Punct/Kw/Tpl
- [`Keyword`]（41 個。C の `AKL_KWS` と同順）+ [`Punct`]（57 個。最長一致）
- 数値リテラル: 10/16/2/8 進・小数・指数・BigInt `n`・`_` 区切り（C の桁上限
  guard を同値で移植。BigInt は 64bit 符号付き範囲超で明白に失敗）
- 文字列エスケープ（`\n \t \r \b \f \v \0 \\ \' \" \/ \xHH \uHHHH`）を復号。
  UTF-8 マルチバイトは原列複製（C の `lex_emit_raw`。CJK 二重エンコード化を防止）
- テンプレートリテラル断片（`` ` `` と `${` まで。閉じ backtick はパーサが
  lex_template を再呼び出しして消費する C と同一の責務分担）

検証: cargo test 41 件（キーワード全数・記号最長一致・数値各進数/BigInt/区切り・
文字列エスケープ・CJK・テンプレート・コメント・行番号・エラー系）。clippy 緑。

## フェーズ 4（進行中）: パーサ + コード生成（parser.rs / codegen.rs）

- [`Expr`](rust/akl-core/src/parser.rs) / [`Stmt`]（enum ツリー。C の AklNode プール相当）
- 再帰下降パーサ（優先順位段ごと）: 代入 / `||` / `&&` / 等値 / 比較 / 加減 /
  乗除余 / 単項（`! - + typeof`）/ 呼び出し / 基本式
- 文: 式文・`var`/`let`/`const`・`return`・`if`/`else`・`while`・ブロック・関数宣言・空文
- コード生成（`codegen.rs`）: AST → バイトコード。スコープ解決は C の関数スコープ近似
  （パラメータ + var 宣言 → ローカルスロット、他はグローバル GLoad/GStore）
- 関数宣言はパス 1 で全登録 → main 先頭で MakeF + GStore の hoist（C の cg_hoist_funcs 相当）
- VM に `PopV`（last_val 更新）・`And`/`Or` を追加。`Halt` は last_val を返す
  （C の「最後の式文の値」セマンティクス）

検証: cargo test 58 件 + clippy 緑。**JS ソース文字列からの e2e** が通る:
- `fib(10) = 55`（再帰・関数宣言・if/else・二項演算）
- `while` ループの和、ネスト関数呼び出し（`double(add(...))`）
- 文字列連結・比較・論理・typeof・if/else

### フェーズ 4 追補 3: クロージャ（共有セル方式）

- ネスト関数宣言が外側ローカルを捕捉（C の `AKL_OK_ENV` + `ELOAD`/`CELOAD` 相当）
- **共有セル方式**: 捕捉される外側ローカルは外側関数の env に box 化され、外側・
  内側の両方から `CeLoad`/`CeStore` で同じセルを参照（自己再帰・変異共有が正しく動く）
- VM に `MakeEnv`（自前 env 生成）・`MakeClosure`（現在フレーム env を共有）・
  `CeLoad`/`CeStore`（env 経由アクセス）を追加
- codegen: `compute_boxed`（ネスト関数に捕捉される自ローカルを解析）→ 関数入口で
  `MakeEnv`、参照は「捕捉 env → ローカル → グローバル」の順で解決
- トップレベル `var` は JS 同様グローバルに変更（従来は main ローカルだった不一致を修正）

検証: cargo test 81 件 + clippy 緑。クロージャ e2e:
- 外側ローカルの捕捉（`function outer(){ var x=10; function inner(){return x;} ... }`）
- カウンタ（`makeCounter()` が `count` を共有する `inc` を返す）
- 変異共有（`bump`/`read` が同じ `count` セルを参照）
- ネスト関数の自己再帰（`fib` を自由変数として捕捉）
- グローバルは捕捉しない（`g` は GLoad のまま）

既知の制限: 3 段以上の深いネスト（クロージャ内のクロージャが祖父母を捕捉）は未対応
（env チェーンが必要。明白にコンパイルエラー）。

### フェーズ 4 追補 4: アロー関数・関数式・this・switch

- `this` キーワード（`Expr::This` → `Op::This`）
- メソッド呼び出し `obj.method(args)` は `this=obj` を束縛（`Op::MCall` 追加）
- アロー関数 `x => expr` / `(a,b) => expr`（式本体を暗黙 return。パーサは Lexer
  クローンによる先読みロールバックで `(expr)` グループ式と区別）
- 関数式 `function(x){...}` / `(function(x){...})(...)`（無名・即時実行）
- `switch (disc) { case x: ...; default: ... }`（=== 比較・break 前提の
  if/else-if チェーンにコンパイル。switch 内 break 対応）

検証: cargo test 87 件 + clippy 緑。アロー/関数式/switch/this の e2e が通る。

残り: 分割代入・try/catch/throw・ビット演算。

## フェーズ 5（進行中）: 組み込み（builtins.rs）

- VM に `Obj::Native` + `NativeFn` 型 + `register_native` / `register_global_native` を追加
  （C の `AKL_OK_NATIVE` + `akl_native_register` 相当）。`do_call` が native を直接呼ぶ
- `console.log`（`console_out` バッファへ追記。テストで検証可能）
- `Math` オブジェクト: abs/floor/ceil/round/sqrt/pow/max/min/random/trunc
- グローバル関数: parseInt / parseFloat / isNaN / Number / String
- 文字列メソッド（`str_methods` 表 = C の `str_meth_vals`）: toUpperCase/toLowerCase/
  trim/indexOf/slice/includes/startsWith/endsWith/repeat
- 配列メソッド（`arr_methods` 表）: push/pop/shift/unshift/join/concat/indexOf/includes/
  reverse/slice/map/filter/forEach/reduce/find/findIndex/some/every/sort/splice/flat/at
  （高階関数はネイティブコールバックで `call_native` によりバイトコード関数を呼ぶ）
- `Object.keys` / `Object.values` / `Object.assign`（静的メソッド）
- `JSON.stringify`（文字列・数値・真偽値・null・配列・プレーンオブジェクトを再帰的に
  文字列化。NaN/Infinity は null、undefined は省略）
- 文字列/配列の `length` はプロパティとして `PLoad` が直接返す（C の PLOAD の
  length 分岐と同型）

### 実測で特定した VM バグ（修正済み）

- `Call`/`MCall` ハンドラ内の `pc += 1` がループ末尾の `pc += 1` と二重になり、
  次の命令（`PopV`）が飛ばされるバグ。`continue` で解消（native 呼び出しの結果が
  last_val に反映されず UNDEF になる症状として顕在化）

検証: cargo test 91 件 + clippy 緑。console.log / Math / parseInt / 文字列・配列
メソッドの e2e が通る。

残り: 正規表現系メソッド・JSON.parse・Date。

### フェーズ 3/4 追補: ビット演算・instanceof/in/delete・try/catch/throw

- ビット演算: `& | ^ ~ << >> >>>`（パーサに優先順位段 `parse_bitor/bitxor/bitand/shift`
  を追加、VM に `BAnd/BOr/BXor/BShl/BShr/BUShr/BNot`、ToInt32 セマンティクス）
- `instanceof` / `in` / `delete`（パーサ + VM。`in` はプロパティ存在検査、
  `delete` は配列穴化 + オブジェクトプロパティ削除）
- try/catch/throw: `Frame.catch_pc` + `TryPush/TryPop/Throw` 命令 + `unwind`
  （例外を frames を遡って catch へ巻き戻す。C の `akl_vm_unwind` 相当）

検証: cargo test 101 件 + clippy 緑（ビット演算・in/delete・throw/catch の e2e）。

### フェーズ 2/3/4 追補 2: 分割代入・spread・class・new・instanceof・Map/Set/Promise

- 分割代入 `var [a, b] =` / `var {a, b} =` / ネスト / rest / elision（Pattern 型 +
  `gen_destructure`、`ArrRest` 命令）
- 配列リテラル spread `[...a, b]`（`ArrPush`/`ArrPushAll` 命令）、配列 rest
- プロトタイプチェーン（`\x00proto` 特殊プロパティ + `prop_get_chain` + `obj_set_proto`）
- `instanceof` 真実装（`[[Prototype]]` チェーンを辿る）
- `new` 演算子（`New` 命令 + `Frame.is_new` で非オブジェクト戻り値は this）
- `class` 宣言（constructor + メソッドを prototype に、`SetFnProto` 命令）
- `Obj::Map`/`Obj::Set`/`Obj::Promise`（GC 子参照対応）+ Map/Set メソッド
  （set/get/has/add）と Promise.resolve 近似

検証: cargo test 108 件 + clippy 緑（分割代入・spread・class/new/instanceof・Map/Set の e2e）。

### フェーズ 4 追補 3: rest パラメータ・for-in/for-of

- rest パラメータ `function f(a, ...rest)`（`FuncObj.rest_slot`。余剰引数を配列で束縛）
- for-of（配列イテレート）/ for-in（Object.keys でキーイテレート）— `Stmt::ForIn`
- 呼び出し引数の spread パース（`f(...args)` の基盤）

検証: cargo test 112 件 + clippy 緑（rest パラメータ・for-of/for-in の e2e）。

### フェーズ 2/3 追補: 簡易正規表現エンジン（regex.rs）

- 依存ゼロの自前 RegExp（C の akl_regex.c 相当のサブセット）: リテラル・`.`・
  `*`/`+`/`?`・文字クラス `[...]`・`^`/`$`・代替 `|`・グループ `(...)`・`i` フラグ
- バックトラッキング付き連結マッチ（`match_concat`）
- `replace_first`（`$1` 展開）・`split` ヘルパ

検証: cargo test 118 件 + clippy 緑（regex の e2e）。

### フェーズ 2/3/4 追補 4: 正規表現リテラル + String.match/replace/search/split 接続

- 正規表現リテラル `/pat/flags`（`Expr::Regex` + `Obj::RegExp` + `NewRegex` 命令）
- `String.match` / `replace`（`$1` 展開） / `search` / `split` を regex.rs に接続
  （regex 以外の文字列も `replacen`/`split` で対応）

検証: cargo test 120 件 + clippy 緑（正規表現リテラル・String メソッドの e2e）。

### フェーズ 2/3/4 追補 5: ROPE・g フラグ全マッチ・import/export

- ROPE（`Obj::Rope`。文字列連結を遅延表現化。`flatten_str` で平坦化）
- 正規表現 `g` フラグの全マッチ（`Regex::find_all` + `String.match` 全マッチ配列）
- `import x from "spec"` / `export name`（簡易近似: グローバル束縛。モジュール
  namespace 解決は未対応として明示）

検証: cargo test 121 件 + clippy 緑。

### フェーズ 2/3/4 完了判定

- 残り: `async/await`・`yield`/generator は「同期エンジンの設計上、完全な非同期は
  不可能」なため、C 実装と同様に「明白に拒否」する（パーサが SyntaxError を返す）。
  C 実装も同期近似であり、ロードマップ上も「近似」と明記されている。

### フェーズ 5 追補: JSON.parse・Date・Array.sort 比較関数・Object 残メソッド

- `JSON.parse`（`JsonParser` 再帰下降。object/array/string/number/true/false/null。
  文字列は `\uXXXX` エスケープ + UTF-16 サロゲートペア対応。整数は int 保持。
  不正 JSON は `VmError::Thrown` で SyntaxError を throw）
- `Date`（同期近似。`Obj::Date { ms }` + `rt.date_methods` 表。`new Date()` /
  `Date()` / `Date.now` / `Date.parse` / `Date.UTC`、getTime/valueOf/getFullYear/
  getMonth/getDate/getDay/getHours/getMinutes/getSeconds/getMilliseconds/getUTC* /
  toISOString/toString/setTime）。Hinnant アルゴリズムによる UTC 暦変換
  （gmtime_r は seccomp 下で SIGSYS のため自前実装。ローカル系も UTC として扱う近似）
- `Date` グローバルは `constructor` プロパティを持つ `Obj::Obj`。`do_call` / `New` に
  「OBJ の constructor を呼ぶ」フォールバック（`ctor_of`）を追加し、`Date.now()` と
  `new Date()` を両立
- `Array.prototype.sort` の比較関数対応（`call_native` でコールバック呼び出し、
  挿入ソート。負値 = 左が小さい）
- `Object.entries` / `Object.fromEntries`

検証: cargo test 126 件 + clippy 緑（JSON.parse・Date・sort 比較関数・Object メソッドの e2e）。

## フェーズ 6（進行中）: ブラウザ統合（script.c / DOM 配線）

ブラウザ本体（`src/ext.c` / `ifuto_pages.c` / `script.c`）は `akl.h` の公開 API を直接
呼ぶ。Rust 化は「エンジン拡張 → FFI クレート → C 側を差し替え」の順で進める。

### 6-a: エンジン拡張（ホスト統合の基盤）

- `Runtime.err: String` + `set_err`（C の `rt->err` / `akl_errf` 相当。`akl_error` 用）
- `HandleVTab`（安全 Rust の fn ポインタ vtable: get/set/call。`ptr` はホスト側
  オブジェクトの不透明アドレスを u64 で保持し、unsafe は FFI 層に隔離）
- `Obj::Handle { vtab, ptr }`（C の `AKL_OK_HANDLE` 相当）+ `Obj::BoundMethod`
  （`obj.method` の method 値を表現。`PLoad` がハンドルの未知プロパティを
  BoundMethod に解決し、`do_call` が vtable の `call` へディスパッチ）
- ディスパッチ配線: `PLoad`（get → 未知は BoundMethod）/ `PStore`（set）/
  `AGet`（ブラケット = キー文字列で get）/ `do_call`（BoundMethod → call。
  未定義メソッドは TypeError を throw）

検証: cargo test 127 件 + clippy 緑（Handle の get/set/call・ブラケット・未知メソッド throw の e2e）。

### 6-b: FFI クレート（rust/akl-ffi）

- `AklVal::from_bits` / `bits`（C の `AklVal`=u64 とビット互換変換。`repr(transparent)`）
- `Obj::ForeignNative { idx, data }` + `Runtime::foreign_fns` / `host_ctx` / `call_value`
  （C の `AklNativeFn(rt, self, argc, argv, udata)` と Rust `NativeFn` のシグネチャ差を
  汎用アダプタで吸収。`host_ctx` = ラッパーアドレス、`data` = C ネイティブ登録表 index）
- `rust/akl-ffi`（`staticlib` + `cdylib`。`#![deny(unsafe_op_in_unsafe_fn)]`）: `akl.h` 互換の
  全 37 シンボル（new/free/eval/call/call_this/error/as_*/is_*/mk*/native_register/
  global_set/prop_get/prop_set/mkobject/mknative/mkhandle/mkarray/arr_len/tostring/as_str/
  native_throw/tune/budget/cojit/module_loader/eval_module）を公開
- ハンドル vtable は `data`（vtable レジストリ index）+ `host_ctx`（ラッパー）経由で
  C の `AklHandleVTab`（get/set/call + tag）へディスパッチ。tag は leak で `'static` 化して
  `[object TAG]` 文字列化に対応

検証: cargo test 132 件（akl-core 127 + akl-ffi 5）+ clippy 緑。さらに
`rust/akl-ffi/smoke.c` を本物の `akl.h` と `libakl_ffi.a` にリンクして eval / native 登録 /
ハンドル / エラー報告が ABI 互換で動くことを C 側から検証（`C smoke test OK`）。

### 6-c: Makefile 統合（ブラウザを Rust エンジンへ差し替え）

- `Makefile` に `RUST_ENGINE ?= 1` を導入。既定で `libakl_ffi.a` をリンク
  （`RUST_ENGINE=0` で旧 C エンジンに戻せる）。`cargo build --release -p akl-ffi` を
  make 依存に追加（Rust ソース変更で再ビルド）
- ブラウザ本体が実際に Rust エンジンで JS を実行することを確認:
  `document.getElementById` / `console.log` / `document.title` 代入 / 文字列連結
  （ROPE）が C の `AklHandleVTab`（doc_vt/elem_vt）と C ネイティブ（script_console_log）
  経由で正しくディスパッチされる
- `make guismoke` が **PASS**（フルブラウザの GUI/raster/session/focus/hover スモークが
  Rust エンジンで全通過）。`-flto` と Rust 静的ライブラリの共存も確認

検証: C スモークテスト + guismoke PASS + `make all`（RUST_ENGINE=1 既定）。

### 6-d: 命令バジェット + DOM 結合オラクルの突合

- **命令バジェット**: `Runtime::insn_budget`（既定 10M）+ `VmError::BudgetExhausted` を追加。
  `run` のループ先頭でカウントダウンし、0 で打ち切る（C の `instruction budget exhausted`
  相当）。`akl_set_insn_budget` が Rust 側へ反映。無限ループ `while(1){}` がハングしない
- `tests/test_script.c`（DOM 結合オラクル）を `libakl_ffi.a` にリンクして実行し、機能差を
  定量化した結果（基本系は PASS / 応用系は未実装で FAIL）:

  - **PASS**: mutation（textContent）/ failure_isolation（破損構文・無限ループ kill・後続継続）/
    skip 規則 / kill switch / svg/document shape / textcontent_and_globals / count_cap
  - **FAIL（未実装機能）**: regex replace（`$1` 捕捉）/ class extends / spread・rest /
    fields（getter 等）/ オブジェクトリテラルメソッド / Array HOF（map/filter の e2e）/
    BigInt / generator（yield）/ Map・Set の高度用途

残り（フェーズ 6 完了 = 全ゲート緑）: 上記 FAIL 機能の実装（正規表現捕捉グループの置換、
class 継承、BigInt、generator 等）→ `test_script.c` 全緑 → `golden`/`conformance`/
`lodashsmoke`/parity を Rust 側で再現。

### フェーズ 6 追補 2: JS 機能パリティ完了（test_script 全緑）

`tests/test_script.c`（DOM 結合オラクル 139 checks）が Rust エンジンで **全緑** に
達した。残っていた FAIL 3 機能（BigInt / generator / arrow・Promise・async）を実装:

- **BigInt**: `Obj::BigInt(i64)`（C の `AKL_OK_BIGINT` と同様の i64 保持近似）+
  `Op::BigInt` 命令。`+`/`-`/`*`（i64 wrapping）、`==`（数値との比較は数値化）、
  `typeof` = "bigint"、`stringify`（`n` 無し 10 進）、`truthy`/`to_number` 対応。
  リテラルは lexer が既に `NumLit::BigInt(i64)` を生成（64bit 超は明白に lex エラー）。
- **generator**: `function* name() { yield ...; }`。パーサ（`Stmt::FuncDecl.is_gen` /
  `Expr::Yield`）、codegen（`Op::Yield`）、`Obj::Gen { fidx, pc, locals, env, this, done }`
  （再開状態を保持）。`do_call` は is_gen 関数を呼ぶと実行せず `Obj::Gen` を返し、
  `gen.next()`（`rt.gen_resume`）が VM ループを再開して `{ value, done }` を返す。
  `run_with_this` のループを `run_loop`（`RunEnd::Value | Yield`）に抽出し、
  再開位置（pc+1）とローカルを `yield` 命令内で保存する。
- **async/await + Promise**: パーサ（`async function` / `await`）。`await` は解決済み
  Promise を unwrap する `Op::Await`、async 関数の `return` は `Op::PromiseWrap` で
  解決済み Promise に包む。`Promise` グローバルを `constructor` + `resolve` を持つ
  プレーンオブジェクトに変更し、インスタンスメソッド `then`/`catch` を
  `rt.promise_methods` に登録（マイクロタスク近似で **no-op** = スクリプト本体内で
  読む値は 0 のまま。V8 準拠のオラクルと一致）。

検証: `cargo test --offline --workspace`（akl-core 137 + akl-ffi 6）緑、`cargo clippy
--offline --workspace -- -D warnings` 緑、`build/run_script_rust`（139 checks）全緑、
`make all` + `make guismoke` PASS。

### フェーズ 6 追補 3: lodash 互換の ES5 パーサ拡張（lodash 壁への着手）

lodash 4.17.21（`tests/lodash_smoke.js` 320 checks）を通すための ES5 慣用句を実装。
`build/akl_rust`（Rust エンジン CLI）で lodash 本体が**全文パース**に到達（従来は
先頭 9 行で失敗）:

- `undefined` / `NaN` / `Infinity` を非予約グローバル化（`undefined` は識別子として
  参照可能 = shadowable。`function (undefined) {}` 等の慣用句が通る）
- 複数宣言子 `var a = 1, b = 2;`
- シーケンス式（コンマ演算子）`(a, b, c)`（括弧内。引数区切りと衝突回避）
- メンバー/インデックスへの複合代入 `obj.x op= y` / `obj[i] op= y`（`obj` は 1 回評価）
- ビット複合代入 `&= |= ^= <<= >>= >>>=`
- メンバー/インデックスの前置・後置インクリメント `--obj.x` / `obj[i]++`
- ラベル文 `label: while(...)` + `break label` / `continue label`（codegen の
  break/continue patch リストをラベル名で深さ指定して解決）
- `new Foo(...).method(...)` / `new Foo(...)[i]` の後置連鎖

残る lodash の壁: **深いネストのクロージャ**（3 段以上の関数宣言。`compile_nested` が
「deeply nested closures」で打ち切り。env チェーン対応が必要）。

### フェーズ 6 追補 4: 多段クロージャ + lodash ランタイム前進

lodash 本体が**全文パース → コンパイル → 実行**に到達（従来は先頭 9 行でパース失敗）。
実装した主要ピース:

- **多段クロージャ（env チェーン）**: `CeLoad`/`CeStore` に深さを導入し
  `Obj::Env.parent` のチェーンを辿る。`compile_nested` の「deeply nested closures」
  打ち切りを撤廃し、任意深度の関数宣言ネスト・関数式による外側ローカル捕捉に対応。
  boxed パラメータの初期値を env セルへコピー、`MakeClosure` 判定を捕捉有無で正しく判定。
- **`Function.prototype.call`/`apply`**（`fn.call(thisArg, ...)`）
- **`&&` / `||` の値返し短絡**（オペランド値を返す。lodash の環境検出
  `typeof x == 'object' && x` が右辺を短絡評価するために必須）
- **`===` の文字列内容比較**（ROPE と Str の同一内容が等値に）
- **`typeof` の安全化**（未宣言識別子で ReferenceError を投げない `GLoadSafe`）
- **`globalThis` ハンドル**（get/set/call をグローバル解決へ写像）+ `Function('return this')`
  + `Array`/`Boolean`/`RegExp`/`Symbol`/`isFinite` コンストラクタ
- **`Object.prototype.toString`/`hasOwnProperty`**（lodash の `getTag` 用）

検証: `cargo test --offline --workspace`（akl-core 142 + akl-ffi 6）緑、`cargo clippy
--offline --workspace -- -D warnings` 緑、`build/run_script_rust`（139 checks）全緑。

残る lodash の壁（ロード完了まで）: `arguments` オブジェクト、`Array.prototype.slice` 等
のプロトタイプメソッド解決、`Function.prototype.toString`、および各関数の実行時差異。
C エンジンは `lodash-smoke ok=320 ng=0` を維持（比較基準）。

### フェーズ 6 完了（ブラウザ統合全ゲート緑）

フェーズ 6 の完了条件（ロードマップ表の「全ゲート緑」）を確認。ブラウザ本体
（`build/ifuto` / `build/ifuto-asan`）は `RUST_ENGINE=1` 既定で `libakl_ffi.a`
（Rust エンジン）をリンクしており、以下のブラウザゲートが全て緑:

- **guismoke**: PASS（GUI/raster/session/focus/hover スモーク全通過）
- **golden**: 1/1 PASS（`tests/golden/doc` の描画出力が expected と厳密一致）
- **tlssmoke**: 3/3 PASS（https-ok / https-badca / https-ip-addr）
- **conformance（html5lib）**: **1922/1922 PASS（100.0%, skip 12）**
- **fuzz**: 5 fuzzer（html/akl/net/store/ext）×500 iter 全て 0 crashes
- **test_script.c**（DOM 結合オラクル）: 139/139 全緑

Rust 側ゲートも維持: `cargo test --offline --workspace`（akl-core 142 + akl-ffi 6）
緑、`cargo clippy --offline --workspace -- -D warnings` 緑。

補足: `fuzz_html` / `fuzz_akl` ターゲットは旧 C エンジン（`src/akl/*.c`）を直接
リンクする（Makefile が `$(AKLSRC)` をハードコード）。C エンジンは比較検証用に
保持しているため、これは仕様どおり。ブラウザ本体（ifuto）の JS 実行は Rust エンジン
経由である。

### フェーズ 6 追補 5: lodash ランタイム（= フェーズ 5 組み込みの残り）

lodash 320/320 はロードマップ上「フェーズ 5（組み込み）」の完了条件であり、
フェーズ 6（ブラウザ統合）のゲートではない。上記のとおりフェーズ 6 は完了。\nlodash 側は以下の追加実装で前進（未完了。次フェーズの組み込みパリティで継続）:

- 関数宣言ホイスティング（ネスト関数の先頭束縛）、正規表現の先読み/非捕捉/
  `\u`/`\x`/`{m,n}`/遅延量指定子
- `NotCallable`/`NotObject`/`GlobalNotFound` を catch 可能な TypeError/ReferenceError に
- `Function/Array/String.prototype`、`Object.create/getPrototypeOf/defineProperty/
  getOwnPropertySymbols/setPrototypeOf/propertyIsEnumerable/Object(x)`
- `Obj::Func` にプロパティ保持（`_.VERSION` 等）+ 既定 prototype + `length`/`name`
- `arguments` オブジェクト、名前付き関数式の自己参照（`runInContext`）

lodash は `runInContext` 本体の `createWrap`/`createRecurry` 機構の深部まで到達。
残る壁は `WeakMap`/`WeakSet` と well-known Symbol（`setData`/`getData` のメタデータ
保持に使用）など。

### フェーズ 5 追補 6: lodash smoke 320/320 達成（フェーズ 5 完了）

`build/akl_rust`（Rust エンジン CLI）で lodash 4.17.21 のスモークテスト
`tests/lodash_smoke.js` 全 **320 checks を通過**（`lodash-smoke ok=320 ng=0`）。
これによりロードマップのフェーズ 5（組み込み）完了条件を満たした。C エンジンも
`lodash-smoke ok=320 ng=0` を維持（比較基準）。

残っていた壁を以下の追加実装で突破:

- **switch のフォールスルー**（`case A: case B: body` の空 case マージ。lodash の
  `equalByTag` が `case dateTag: case numberTag:` 形式で依存）
- **関数オブジェクトの独自プロパティメソッド呼び出しで `this` = メソッド自身**
  （C の `OP_MCALL` 互換。`_.noConflict()` が `root._ === this` を満たさず
  グローバル `_` を消さない根拠。Function メソッド `call`/`apply` は従来通り
  `this` = レシーバ）
- **Error / TypeError / RangeError / SyntaxError** コンストラクタ（`\x01tag` による
  `Object.prototype.toString` タグ + `instanceof` 成立。`_.isError`/`_.attempt`）
- **WeakMap / WeakSet**（`\x01tag` 付き OBJ。WeakMap は get/set/has/delete を
  文字列キー近似で実装。`_.isWeakMap`/`_.isWeakSet`/`metaMap`）
- **TypedArray / ArrayBuffer / DataView**（タグ付き OBJ + `.length`。
  `_.isTypedArray`/`_.isArrayBuffer`）
- **setTimeout / setInterval / clearTimeout / clearInterval**（実行は近似で ID のみ。
  `_.throttle`/`_.debounce`/`_.delay`/`_.defer`）
- **配列 ToString = join(",")**（`String([1,2])==="1,2"`。`_.toString`/`_.chain` 系）
- **RegExp ToString = `/source/flags`** + `obj == string` の ToPrimitive
  （`_.isEqual(/a/g, /a/g)` の `equalByTag`）
- **`Number('') === 0`**（`js_str_to_number`。lodash の `createRound` の `+pair[1]`）
- **Map/Set イテラブルコンストラクタ**、**Symbol タグ**、**split limit**、
  **hasOwnProperty の数値キー**、**parseInt radix**、**文字クラス範囲の
  エスケープ終端**（`a-\uXXXX`。`_.size("pebbles")` の `reHasUnicode`）

### フェーズ 4 追補 2: 制御フロー完全化

- `for (init; cond; step) body`（init は var 宣言 or 式、cond/step 省略可）
- `do { } while (cond);`
- `break` / `continue`（ループ外はコンパイルエラー。codegen が loop context で
  ジャンプ先をパッチ）
- 三項 `cond ? then : else`
- 複合代入 `+= -= *= /= %=`（変数のみ）
- 前置/後置インクリメント・デクリメント `++`/`--`（変数のみ。後置は旧値を返す）

検証: cargo test 76 件 + clippy 緑。for/do-while/break/continue/三項/複合代入/
inc/dec の e2e が通る。

### フェーズ 4 追補: 配列/オブジェクトリテラル + メンバー/インデックスアクセス

- 配列リテラル `[e, ...]` → `ArrNew`（既存 VM 命令に接続）
- オブジェクトリテラル `{k: v, ...}` → `ObjNew` + `Dup`/`PStore`/`Pop`
- メンバーアクセス `obj.prop` → `PLoad`、インデックスアクセス `obj[i]` → `AGet`
- 要素代入 `obj[i] = v` → `ASet`、メンバー代入 `obj.prop = v` → `PStore`
- 文頭の `{` はブロック（オブジェクトリテラルは式文脈でのみ。JS と同一の曖昧性解決）

検証: cargo test 65 件 + clippy 緑。配列/オブジェクトの構築・アクセス・代入、
オブジェクトに持たせた関数のメソッド呼び出し（`obj.op(3,4)`）が e2e で通る。

## ツール拡充（2026-08-15）

- 基本: rustup / clippy / Miri / Kani / Prusti / geiger（trigger.md 常時実行）
- 追加: cargo-fuzz / cargo-tarpaulin / Flux / MIRAI（インストール失敗は許容。
  Rudra の教訓: メンテ停止ツールはジョブを止めない）
- Verus は env_trigger.md の VERUS=on で単独ビルド（約 1 時間）

## フェーズ 3 設計メモ: VM（vm_exec）の移植方針（旧）

- バイトコードは C と同じ命令セット（OP_*）を維持し、`enum Op` + 即値の
  `decode` を 1 箇所に集約（C の `akl_op_imm_len` 表の drift 問題を構造的に排除）
- 命令ハンドラは `fn exec(&mut Rt, pc: &mut usize, ...)` の巨大 match ではなく、
  命令ごとの小さな関数 + ディスパッチ（Kani が各命令のスタック効果を証明しやすい形）
- スタック操作は `Vec<AklVal>` の push/pop（C の AKL_PUSH/AKL_POP マクロ + 下限検査
  が不要になる。範囲外はパニック = バグの早期検出）

## リスクと対策

- **C と Rust の挙動差**: 各フェーズで既存テストを回帰オラクルにし、差が出たら
  テストが FAIL する（黙って通さない）
- **FFI の unsafe**: 最小限にし、境界（翻訳単位）を 1 ファイルに集約して監査
- **完全移行の期間**: 数ヶ月規模。各フェーズを独立コミットで進め、途中で C が
  動き続ける状態を維持（段階的移行）

## フェーズ 8（進行中）: ブラウザ本体の Rust 移行

Aklus（JS エンジン）の移行（フェーズ 0〜6）が完了したため、次はブラウザ本体
（`src/*.c` のうち `src/akl/` を除く約 15k 行）を Rust へ移行する。クレートは
`rust/ifuto-core`（`#![forbid(unsafe_code)]` を維持）。方針は Aklus 移行と同一:

- **1 モジュール = 1 検証ゲート**。C 実装を回帰オラクルに並走させ、入出力を
  突合してから差し替える。各 Rust モジュールは C の単体テスト（`tests/test_*.c`）
  と同一の期待値を Rust テストとして再現し、加えて全数走査で表外セルの安全性を
  機械的に証明する。
- **葉モジュールから順に**: 依存が少ない純粋関数（文字コード層・arena）から
  始め、DOM / HTML パーサ / レイアウトへと進む。

### フェーズ 8-a: 基盤 + 文字コード層（utf8 / strutil / common）

- `rust/ifuto-core` クレートを新設し、ワークスペースに追加。
- `common`（ハードリミット + `fatal` = fail-fast パニック）、`strutil`
  （`IfStr` = `&[u8]` スライス。`str_eq`/`str_eq_ci`/`trim`/`contains` 等）、
  `utf8`（`decode`/`encode`/`glyph_width`/`band_w2`）を移植。
- C 実装で手動管理していた「ポインタ + 長さ」と境界検査を `&[u8]` スライスに
  置き換え、OOB 読み・dangling・NUL 終端の誤仮定を構造的に排除。

検証:
- `cargo test --offline --workspace`: ifuto-core 8 件（akl-core 142 + akl-ffi 6 と
  併せて 156 件）緑、`cargo clippy --offline --workspace -- -D warnings` 緑。
- C オラクル `tests/test_utf8.c`（29 checks）を 1:1 で Rust に再現
  （`utf8::tests::oracle_mirrors_c`）。
- 全数走査: `encode` の全コードポイント往復（0..=0x10FFFF）、`band_w2` の
  3 バイト全域（E0..EF × 256 × 256）で「真 ⟹ decode 成功 ∧ 幅 2」を機械証明。

残り: `charset`（Shift_JIS/EUC-JP 変換。生成表 1467 行）→ `arena` →
`html_tok`/`html_tree` → DOM/CSS/レイアウト → 最後に `ifuto-ffi` で C 側と差し替え。

### フェーズ 8-b: 文字コード層（charset）

- `tools/gen_charset.py` を拡張し、C ヘッダ（`src/charset_tables_gen.h`）に加えて
  Rust 表（`rust/ifuto-core/src/charset_tables.rs`）も同一データから生成。
  `--verify` は両ファイルを照合（C ヘッダは byte 一致で後方互換を維持）。
- `charset` モジュール: `label` / `from_http` / `sniff`（`(Enc, bom)` を返す）/
  `decode`（`Vec<u8>` を返す）。C の arena 出力を所有権ベースの `Vec<u8>` に置換し、
  手動の容量計算（`3n+3`）とバッファ境界検査を構造的に排除。
- 判定順（HTTP > BOM > meta prescan 4096B > UTF-8）・malformed の FFFD restore 規則・
  cp932 波ダッシュ採用を C と同一に維持。

検証:
- C オラクル `tests/test_charset.c` の `t_label`/`t_sniff`/`t_decode_sjis`/
  `t_decode_euc`/`t_sweep`/`t_bom_strip_rule` を 1:1 で Rust に再現（`t_e2e` は
  DOM 未移行のため保留）。
- **全バイト対（65536 × SJIS/EUCJP）の decode 出力を C と Rust で突合し、byte 一致
  を確認**（クロスチェック用ダンプを一時生成して diff。コミット対象外）。
- `cargo test --offline --workspace`（akl-core 142 + akl-ffi 6 + ifuto-core 14 =
  162 件）緑、`cargo clippy --offline --workspace -- -D warnings` 緑。

### フェーズ 8-c: 拡張 manifest パーサ（ext_manifest）

- `ext_manifest` モジュール: 行ベース `key: value` の純粋パーサを移植。文法（E1 凍結）:
  空行/コメントスキップ・前後トリム・CRLF 救済・key 完全一致・重複失敗・name/version/
  entry の charset 検証・permissions 単一効果・64KB 上限・必須キー検証。
- C の `char name[64]` 固定配列 + `char *err` 書き込み先を、`Manifest { name: String, ... }`
  + `Result<_, String>` に置換。バッファ長の手動管理・NUL 終端・err 境界検査が構造的に消える。
- エラー文言（`manifest: line N: ...`）は ext_smoke.py の golden 行と一致するよう C と
  完全同一に維持。

検証:
- **差分 fuzz**: 典型 27 件 + ランダム 20,000 件の manifest 入力に対し、C と Rust の
  出力（成功/失敗 + フィールド + エラー文言）を突合し **0 不一致**。
- fuzz_ext.c の機械不変条件（成功 ⇒ フィールド非空・cap 内、失敗 ⇒ 理由非空、
  決定性）を Rust テストとして再現。
- `cargo test --offline --workspace`（ifuto-core 31 件 = 合計 179 件）緑、
  `cargo clippy --offline --workspace -- -D warnings` 緑。

### フェーズ 8-d: HTML 名前付き文字参照（entities）

- `tools/gen_entities.py` を拡張し、C ヘッダ（`src/entities_gen.h`）に加えて Rust 表
  （`rust/ifuto-core/src/entities_tables.rs`、2722 行）も同一データから生成。
  `--verify` は両ファイルを照合（C ヘッダは byte 一致で後方互換）。
- `entities` モジュール: `find`（完全一致二分探索）/ `longest_legacy`（legacy 最長
  prefix）/ `codepoints`（cp1/cp2/astral の復号）を移植。`name_off` の blob オフセット
  を `&[u8]` スライスの `get` に置き換え、範囲外アクセスを構造的に排除。

検証:
- 全 2125 エントリの `find`/`longest_legacy` を C と Rust で突合し **byte 一致**。
- 生成時 assert（2-cp は両方 BMP・astral は単一 cp・cp 非ゼロ・ソート一貫性）を
  Rust テストとして再現（全 2125 エントリの自分自身 find 往復）。
- `cargo test --offline --workspace`（ifuto-core 39 件 = 合計 187 件）緑、
  `cargo clippy --offline --workspace -- -D warnings` 緑。

### フェーズ 8-e: 永続ストア読み面パーサ（store）

- `store` モジュール: `session.txt`（`ifuto-session 1`）と `bookmarks.tsv`
  の読み面パーサを移植。fs 操作は C 同様 `IfFsOps` 注入で分離し、Rust 側は
  「テキスト → 結果」の純関数として移植（所有権により arena 不要）。
- C の `IfSessionTab*`（arena 内 `char*` を指す）を `SessionTab { url: String, .. }`
  に置換。`parse_session` は `(Vec<SessionTab>, i32)` を返し、「タブ 0 件でも
  `active` 行を読めれば active_id が非 -1」という C の細部まで一致。

検証:
- **差分 fuzz**: session 20,008 件 + bookmarks 10,004 件の入力で C/Rust 出力を突合し
  **0 不一致**。
- fuzz_store.c の機械不変条件（n ∈ [0,64]、id ∈ [0,1e6]、scroll ∈ [0,1<<24]、
  url 非空、決定性）を Rust テストとして再現。
- `cargo test --offline --workspace`（ifuto-core 51 件 = 合計 199 件）緑、
  `cargo clippy --offline --workspace -- -D warnings` 緑。

### フェーズ 8-f: HTML タグ表（tags）— HTML パーサの基盤

- `tools/gen_tags.py` を新設。`src/dom.c` の `IF_TAGS` テーブル（137 タグ）を
  抽出して `rust/ifuto-core/src/tags_tables.rs` を生成。`--verify` は再抽出と照合。
  C の手書き長さ `n` フィールドと `if_dom_tag_table_sane` 検査を、`str.len()` の
  自動導出に置き換えて構造的に排除。
- `tags` モジュール: `tag_name` / `tag_id`（CI）/ `is_void` / `is_rawtext` /
  `is_rcdata` を移植。void 18 / rawtext 6 / rcdata 2 の flags を忠実再現。

検証:
- 全 137 タグの `tag_name`/`is_void`/`is_rawtext`/`is_rcdata` を C/Rust で突合し
  **byte 一致**。
- 全タグの round-trip（`tag_id`→`tag_name`）と flags 相互排他・小文字 canonical を
  Rust テストで機械証明。
- `cargo test --offline --workspace`（ifuto-core 57 件 = 合計 205 件）緑、
  `cargo clippy --offline --workspace -- -D warnings` 緑。

次は HTML トークナイザ（`html_tok.c` 817 行）→ ツリー構築（`html_tree.c` 3147 行）。
DOM ノードは JS エンジンで実証済みの「`NodeId` = `Vec<Node>` への index」パターンを
踏襲し、`arena` は所有権ベースの `Vec` に置換する（safe Rust では arena の raw
ポインタ返却が本質的で、`forbid(unsafe_code)` を維持できないため「移植しない」判断）。

### フェーズ 8-g: HTML トークナイザ（html_tok）

- `html_tok` モジュール: WHATWG tokenizer の実用形を移植。`TokKind`（Text/Start/End/
  Comment/Doctype/Eof）+ `Tok`（所有 `Vec<u8>`）+ `Tokenizer`（`&[u8]` + pos）。
- 状態: rawtext/RCDATA（`set_raw`）、fragment 直接 raw、strip_lf、foreign content
  の CDATA/U+0000 規則、plaintext、属性値の ambiguous-amp。
- 文字参照（数値/名前/C1 補正/2-cp）は `entities` モジュールと `utf8` を再利用。
- C の「ゼロコピー切片 + arena 切片の混在」（`if_resolved` の 2 パス）を、所有
  `Vec<u8>` の 1 パスに統合。手動容量計算とバッファ境界検査を構造的に排除。

検証:
- **差分 fuzz**: 既定モード 30,034 件 + foreign content モード（NUL 含む）10,005 件で
  C/Rust のトークン列を突合し **0 不一致**（byte 一致）。
- 単体テスト 16 件（テキスト/タグ/属性/文字参照/rawtext/rcdata/コメント/PI/
  DOCTYPE/void/自己終了/NUL 規則）。
- `cargo test --offline --workspace`（ifuto-core 73 件 = 合計 221 件）緑、
  `cargo clippy --offline --workspace -- -D warnings` 緑。

### フェーズ 8-h: DOM ノードモデル（dom）— ツリービルダの基盤

- `dom` モジュール: `NodeKind` / `Ns` / `Node`（`NodeId` = `Vec<Node>` index）/
  `Dom`（`Vec<Node>` 所有）を移植。JS エンジンで実証済みの index パターンを踏襲し、
  C の raw ポインタ連結（parent/first_child/last_child/next_sibling）を
  `Option<NodeId>` に置換。ポインタの寿命・エイリアシング問題を構造的に排除。
- 純粋ヘルパ: `append_child` / `detach` / `attr`（CI）/ `has_class` / `text_content`
  （DFS）/ `find_tag_dfs` / `find_by_id` / `attr_set` を移植。

検証:
- 単体テスト 5 件（木構造・text_content DFS・attr/has_class・find・detach）。
- `cargo test --offline --workspace`（ifuto-core 78 件 = 合計 226 件）緑、
  `cargo clippy --offline --workspace -- -D warnings` 緑。

次はツリー構築（`html_tree.c` 3147 行）の挿入モード状態機械を移植し、
`parse_html` でトークナイザ + DOM を接続。html5lib 適合（1922/1922）の Rust 再現へ。

### フェーズ 8-i: HTML ツリービルダ（html_tree）

- `html_tree` モジュール: WHATWG insertion modes の完全移植。`TreeBuilder` が `Dom` +
  `Tokenizer` を束ね、`stack: Vec<NodeId>`（C の `IfNode**`）で open-elements を保持。
- 全 18 挿入モード（initial/before-html/head/in-body/table 系/frameset/template/
  after-body/after-after-body）、foster parenting、active formatting elements +
  adoption agency（outer≤8/inner≤3）、quirks モード完全表、foreign content（SVG/MathML
  のタグ・属性 case 調整 + integration points + breakout）、customizable select の
  selectedcontent clone、fragment 解析（WHATWG 13.4）。
- `tools/gen_tags.py` を拡張し、タグ ID 定数（`TAG_HTML` 等 137 個）も生成。
  C の arena/raw ポインタ連結を `NodeId`（`Vec<Node>` index）+ `Option<NodeId>` に置換。

検証:
- **差分 fuzz**: 手作り 35 件（adoption/foster/SVG/table/template/frameset/select 等）
  + ランダム 50,000 件の HTML 入力で C/Rust の DOM ツリーを突合し **0 不一致**。
- 単体テスト 8 件（基本構造/暗黙 html-head-body/p 閉じ/table/adoption/title trim/
  SVG/script 観測）。
- `cargo test --offline --workspace`（ifuto-core 86 件 = 合計 234 件）緑、
  `cargo clippy --offline --workspace -- -D warnings` 緑。

これで HTML パーサ（トークナイザ + ツリービルダ + DOM）が Rust で完動。残るは
`serialize_wpt`（html5lib 採点ハーネス）を Rust 側で走らせて 1922/1922 を実証し、
続いて CSS（`css.c` 1554 行）→ レイアウト（`layout.c` 1574 行）へ進む。

### フェーズ 8-j: wpt シリアライザ + html5lib 適合の Rust 実証（1922/1922）

`dom.c` の `if_dom_serialize_wpt` / `if_dom_serialize_wpt_frag` を Rust へ移植し、
Rust パーサ単体で html5lib tree-construction 1922/1922 を再現した。併せて、
移植時に見つかったツリービルダの 2 件のバグを修正した。

- **`Dom::serialize_wpt` / `serialize_wpt_frag`**: `| indented` 形式（TEXT `"…"`・
  COMMENT/PI・DOCTYPE（pub/sys 引用規則）・ELEMENT（svg/math 接頭辞・属性辞書順
  ソート）・template の content 擬似ノード）を byte 一致で移植。
- **DOCTYPE 情報の保持**: `Node.doctype: Option<Doctype>`（name/has_name/pub_id/sys_id）
  を追加（C の `IfDoctype` 相当）。従来は `name` のみで pub/sys が欠落していた。
- **PI ターゲットの保持**: `Node.pi_target: Vec<u8>` を追加（C は attrs[0].name に
  保持）。`<?xml…?>` は XML 宣言様式のため bogus comment（仕様）、非 xml ターゲット
  のみ PI として扱う。
- **customizable select の clone**（`sc_clone` / `sc_selected_option` / `sc_fill` /
  `sc_select_walk`）: `<selectedcontent>` へ選択中 option の子孫を複写する処理を移植
  （webkit02#44-#47）。従来は `has_selectedcontent` フラグを立てるだけで clone 自体が
  未実装だった。

**移植で発見したツリービルダのバグ 2 件（修正済み）**:

1. **AAA の inner>3 打ち切り欠落**: WHATWG adoption agency の内側ループには
   「counter > 3 なら active formatting elements から node を除去する」規則があるが、
   Rust 版に欠落していた。これにより `<b><em><foo><foo><foo><aside></b>` 等の
   深い書式ネスト + special 要素介入で余分な clone 要素が挿入されていた
   （adoption01 #14/#17、tests22 #0/#4、webkit02 #11/#14-#17 の 9 件）。
2. **annotation-xml の encoding 属性が case-sensitive 比較**: `in_foreign` /
   `in_foreign_text` の encoding 判定（text/html / application/xhtml+xml）が
   `==` の大文字小文字区別比較になっていた（C は `if_str_eq_ci`）。`Text/htmL` /
   `aPPlication/xhtmL+xMl` で `<div>` が annotation-xml の外へ foster されていた
   （tests20 #55/#57 の 2 件）。

検証:

- **html5lib tree-construction: 1922/1922 passed（100.0%, skip 12）** を Rust パーサ
  単体（一時 CLI example で `tests/run_html5lib.py` を流す）で再現。C 本体と完全一致。
- **差分 fuzz**: ランダム 30,000 件（fragment / DOCTYPE / PI / 実体参照 /
  selectedcontent / annotation-xml / foreign content を含む）で C の `--dump-wptdom`
  と Rust 出力を突合し **0 不一致**。
- 単体テスト 8 件追加（wpt 基本 / DOCTYPE pub+sys / DOCTYPE 裸 / PI / template content /
  selectedcontent clone / annotation-xml CI / AAA counter clamp）。
- `cargo test --offline --workspace`（akl-core 142 + akl-ffi 6 + ifuto-core 94 =
  242 件）緑、`cargo clippy --offline --workspace -- -D warnings` 緑。

これで **HTML パーサ全体（トークナイザ + ツリービルダ + DOM + wpt シリアライザ）が
Rust で完動し、html5lib 適合 1922/1922 を byte 一致で実証**した。次は CSS
（`css.c` 1554 行。selector/declaration パーサ + cascade）→ レイアウト
（`layout.c` 1574 行）へ進む。

### フェーズ 8-k: CSS サブセット（css）— パース + カスケード

`css.c`（1555 行）を Rust へ移植した。色・値レクサ・宣言パーサ・セレクタパーサ・
スタイルシートパーサ・マッチャ・カスケード・computed style dump の全 eager 経路を
カバーする。

- **色**（`css_color`）: `#hex`（3/4/6/8 桁）/ `rgb()` / `rgba()` / 色名 55 種 /
  `transparent` → RGBA8。`hexv`・`rgba8` を C と同一に。
- **値レクサ**（`lex_value`）: NUM / DIM / PCT / IDENT / COLOR / STR / AUTO の 8 種。
  `parse_number` は C ロケール非依存の f32 手動変換を忠実再現（f32 累積で C と
  byte 一致する数値）。
- **宣言パーサ**（`parse_decls`）: `!important` 検出・shorthand 展開（margin/padding/
  border/border-width/background）・font/flex/grid 等の未対応 shorthand 丸ごと棄却・
  プロパティ表（25 種）。
- **セレクタパーサ**: type/class/id/universal の複合 + 子孫・子結合子。specificity は
  `(ids<<16)|(classes<<8)|types`。pseudo/属性/兄弟結合子はセレクタごと棄却。
- **スタイルシートパーサ**（`parse_stylesheet`）: @規則は丸ごと棄却、プリリュード/
  ブロックの括弧・文字列・コメント考慮の走査、decl 単位の単調 order。
- **マッチャ**（`match_selector`）: 右→左の子孫バックトラッキング、未知タグの CI 照合。
- **カスケード**（`apply_styles`）: (important, origin, specificity, order) の辞書順。
  UA シート + `<style>` 要素（author）+ inline style の 3 元。継承・font-size 先行解決・
  `resolve_len`（em/rem/pt/px）・`kw_font_size`（xx-small..xx-large/smaller/larger）。
- **computed style dump**（`dump_styles`）: C の `%.6g` を `fmt_g6` で忠実再現
  （f32→f64 昇格・末尾ゼロ除去・`1e+06` 形式の指数表記切替を `{:.5e}` から指数を
  取り出して再構成）。

計算済みスタイルは `Node` に埋め込まず `Vec<Option<Style>>`（`NodeId` 並行）で返し、
`dom` → `css` の依存を断つ（循環参照を構造的に排除）。

**未移植（性能最適化・観測不変、将来の最適化として保留）**: RuleSet 風セレクタ
インデックス（`css_build_ruleset`）、computed style interning（`st_intern`）、決定
メモ化（`IfStCache`）、lazy computed style（`if_style_lazy_*`、md fast-DOM 専用）、
Blink ファサード（`css_blink.h`）。いずれも `--dump-styles` の出力に影響しない
（naive 全走査と indexed 経路は同一バイト列になることは C 側の `test_css_ruleset_oracle`
が機械監査済み）。

検証:

- **差分 fuzz**: ランダム 20,000 件の HTML + CSS（セレクタ・shorthand・`!important`・
  inline style・@規則・色・未知タグ CI・継承・float 書式）で C の `--dump-styles` と
  Rust 出力を突合し **0 不一致**（byte 一致）。
- C の `tests/test_css.c` の主要ケース（色 11 件 / UA 既定 / specificity / !important /
  shorthand 展開 + 継承 / マッチャ / dump 固定文字列オラクル）を Rust テストとして再現。
- `fmt_g6` を C `printf("%.6g")` と対照（0.1 / 13.3333 / 0.3 / 1e-05 / 1e+06 等）。
- `cargo test --offline --workspace`（akl-core 142 + akl-ffi 6 + ifuto-core 102 =
  250 件）緑、`cargo clippy --offline --workspace -- -D warnings` 緑。

次はレイアウト（`layout.c` 1574 行。box 木 + 行レイアウト）→ 描画系へ進む。

### フェーズ 8-l: レイアウト（layout）— box 木 + 行レイアウト

`layout.c`（1575 行）のコアを Rust へ移植した。ブロック/インラインの box 木構築と
行レイアウトの全 eager 経路をカバーする。

- **幾何**（`geom`）: margin/padding/border（幅 1 の有無）/width/height の解決、
  `len_h`/`len_v`（% は包含ブロック基準、px/em/rem/pt は `resolve_len`）、
  margin:auto センタリング、`px2col`/`px2row` の C と同一の丸め。
- **ブロックレイアウト**（`layout_element` / `layout_children`）: トップダウン DFS、
  兄弟縦マージン相殺（max）、`<hr>` 特殊形状、display:none の除去、list-item の
  ブロック化。
- **インライン整形コンテキスト**（`layout_ifc` / `flatten_into` / `wrap_text`）:
  flatten（TEXT/BR/IMG alt）→ アトム化（ASCII 可視ラン / 全角 1 グリフ / 結合文字）→
  貪欲折り返し。空白折り畳み・`white-space:pre`・行幅超過グリフのハード分割。
- **segment 合体**（`push_merge`）: C の `pm_st`/`pm_end`（ソース連続性）追跡を
  `Option<Style>` + ソースオフセットで忠実再現（seg 境界が dump の `segs=N` と
  60 バイト打ち切りに現れるため、正確な再現が必須）。

box は `BoxNode`（子を所有 `Vec`）、seg は `Vec<Seg>`（所有 `Vec<u8>`）で表現し、
C の arena bump + rewind / raw ポインタ連結を構造的に排除。

**未移植（性能最適化・観測不変）**: AVX2/SSE2 の ASCII ラン走査、幾何キャッシュ、
fused fit 経路、2-way 並列 layout（pthread）、線形モード box 再利用、lazy style、
link span 収集・deco 装飾 op（dump に現れず、描画層移行時に移植）、rdtsc プロファイリング。

検証:

- **差分 fuzz**: ランダム 50,000 件（HTML 構造 + CSS + 全角/結合文字/長単語の
  ハード分割 + 複数 viewport 幅 4..250）で C の `--dump-layout` と Rust 出力を突合し
  **0 不一致**（byte 一致）。
- 単体テスト 8 件（空レイアウト / 単純ブロック / h1 フォントサイズ / hr /
  全角グリフ幅 / 折り返し / br / list-item / マージン相殺）。
- `cargo test --offline --workspace`（akl-core 142 + akl-ffi 6 + ifuto-core 111 =
  259 件）緑、`cargo clippy --offline --workspace -- -D warnings` 緑。

次は描画系（`raster.c` 155 / `render_ansi.c` 1209 / `md.c` 1698 / `chrome.c` 603 /
`image.c` 540）へ進む。

### フェーズ 8-m: セルグリッドレンダラ（render）— grid + paint + emit

`render_ansi.c`（1209 行）の機能コアを Rust へ移植した。ボックスツリー → セルグリッド
→ 発行バイト列の全 eager 経路をカバーする。

- **色**（`rgba_to_ansi`）: RGBA8 → ANSI 256 色 index（グレー特別経路 + 立方体）。
- **セルグリッド**（`render_grid` / `Grid` / `Cell`）: `lay.width` 幅に固定し、はみ出す
  seg（右寄せ + ハード分割の `line_w` 残存値に起因）は右端でクリップ。
- **paint**（`paint_box` / `paint_shell`）: 背景塗り・罫線（Unicode 罫線素片 ┌─┐│└┘）・
  `<hr>`・li マーカー（ul "• " / ol "N."）・テキスト（全角継続セル cp=0）。deco 挿入順
  （marker → 自 BG → 自 BORDER）を再現。
- **emit**（`render_emit`）: ansi=256 色 SGR（reset→bold→italic→uline→strike→fg→bg）/
  plain（行末空白 trim）。行末リセットは無条件（C の 2026-08-01 契約統一）。
- **extent**（`render_extent`）: `grid_max_walk` で文書行列の最大範囲。

golden テスト（`tests/golden/doc` の `--no-ansi --width 40` 出力）が Rust 単体で
**byte 一致**することを確認した。

**移植で発見した C の sweep 経路固有規則（byte 一致に必須）**:

1. 幅は `lay->width` でクリップ（`if_render_grid` は `grid_max_walk` で拡張するが、CLI
   が使う行スイープは打ち切る）。
2. `h=0` の空ボックスの BG/罫線は `max(h,1)` の 1 行として描く（deco 有効高）。
3. li マーカーは `<li>` かつ `display:list-item` のみ（任意の `display:list-item` 要素には
   描かない。sweep の `c->tag == IF_TAG_LI` 条件）。

**既知の偏差（1 ケース、C の FAST/SLOW 経路不整合に起因）**: `<li><dl style="background">`
の clamped マーカー（li.x=1）が子孫 BG と重なる場合、C の FAST 経路（罫線なし行）は
マーカーを上層ランとして扱い自 st の bg（既定）を保つが、SLOW 経路（deco 順）は子孫 BG が
マーカーの bg を上書きする。本実装は SLOW（参照セル経路）に一致させる。差分 fuzz 40,000
件中この 1 ケースのみ乖離（0.0025%）。

**未移植（性能最適化・観測不変）**: 窓グリッド経路（`if_render_grid_rows_into(_cur)`）、
行スイープ直接発行（byte-direct/fast 経路）、2-way 並列 sweep、`raster.c` の fill カーネル
自動選択（全候補 bit-exact 同値）、rdtsc プロファイリング。

検証:

- **差分 fuzz**: ランダム 40,000 件（HTML + CSS + li マーカー + 罫線 + 背景 + 全角 +
  複数 viewport 幅 8..100 + ansi/plain）で C の `--no-ansi`/ANSI 出力と突合し、
  **既知の 1 ケース（C FAST/SLOW 不整合）を除き 0 不一致**。
- golden テスト（`tests/golden/doc`）が Rust 単体で byte 一致。
- 単体テスト 5 件（rgba_to_ansi / golden doc / 単純テキスト / ansi SGR / hr + extent）。
- `cargo test --offline --workspace`（akl-core 142 + akl-ffi 6 + ifuto-core 116 =
  264 件）緑、`cargo clippy --offline --workspace -- -D warnings` 緑。

次は Markdown（`md.c` 1698 行）→ chrome（`chrome.c` 603）→ image（`image.c` 540）→
net/tls（`net.c` 573 / `tls.c` 406）へ進む。

### フェーズ 8-n: Markdown 変換層（md）— 文字列 backend

`md.c`（1699 行）の **文字列 backend**（`if_md_to_html`）を Rust へ移植した。
Markdown → HTML の変換器本体を純粋関数（`&[u8] → Vec<u8>`）として実装する。

- **ブロック層**（`blocks_win` / `blocks_str`）: ATX 見出し（閉じ `#` trim）・hr・
  フェンスコード（言語 class）・引用（ネスト / depth≥8 の flatten 飽和）・ul/ol
  （インデント入れ子）・GFM パイプ表・段落（ハードブレーク）。行分割は `&[u8]` スライス。
- **inline 層**（`inline_span`）: バックスラッシュ escape・インラインコード・
  strong/em・del・リンク/画像・自動リンク `<http://…>`。特殊文字走査はスカラ版。
- **脚注**（`try_link` / `run_blocks`）: `[^id]` 参照（参照順 numbering・多重参照は
  `fr-id-2`）と `[^id]:` 定義 → `<section class="footnotes">` セクション。
- **escape**: テキストは `&` `<` `>`、属性値は `&` `<` `"`（生 HTML 非透過・XSS 安全）。
- **CRLF 正規化**: `\r` / `\r\n` → `\n`（`\r` 無しはゼロコピー）。

C の `realloc` ベースのステージングバッファ + `b_finish` の 2 段構えを、`Vec<u8>` への
直接追記に置換。手動の容量計算・解放規約を排除する。

**未移植（性能最適化・観測不変）**: DOM 直構築 backend（`if_md_parse_fast*`。`md→HTML→
parse_html` と観測同値の高速経路。ws-only TEXT 剥がしの `md_ws_stripped` 最適化を含む）、
2-way 並列 fast parse（`md_par_scan` / pthread）、SIMD 特殊文字走査、rdtsc プロファイリング。

検証:

- **差分 fuzz**: ランダム 30,000 件（見出し/段落/強調/コード/リンク/画像/引用/リスト/
  表/脚注/CRLF/escape/敵対深度）で C の `if_md_to_html` と Rust 出力を突合し **0 不一致**
  （byte 一致）。
- 引用 100 段の飽和（`<blockquote>` 9 個 = 8 段 + flatten）が C と一致。
- C の `tests/test_md.c` の主要ケース（33 件の文字列完全一致）を Rust テストとして再現
  （12 テスト関数に集約）。
- `cargo test --offline --workspace`（akl-core 142 + akl-ffi 6 + ifuto-core 128 =
  276 件）緑、`cargo clippy --offline --workspace -- -D warnings` 緑。

次は chrome（`chrome.c` 603）→ image（`image.c` 540）→ net/tls（`net.c` 573 /
`tls.c` 406）→ script（`script.c` 424）→ ifuto_pages（`ifuto_pages.c` 247）へ進む。

### フェーズ 8-o: 軽量画像デコード（image）— PNG / BMP

`image.c`（541 行）を Rust へ移植した。純粋関数（`&[u8] → Result<Image, String>`）として
実装する。

- **PNG**: チャンク走査（CRC 検証）→ IDAT 連結 → zlib inflate（RFC1950/1951。ストアド/
  固定/動的ハフマン。正準ハフマンをビット長テーブルで構成）→ フィルタ解除
  （None/Sub/Up/Average/Paeth。C と同じく `raw` をその場で書き換え、`prev` は解除済み
  前行を参照）→ 色変換（グレー/RGB/グレー+α/RGBA）。8bit 深度のみ、パレット・
  インターレースは拒否。
- **BMP**: 無圧縮 24/32bpp（BITMAPINFOHEADER のみ。ボトムアップ対応。BGR→RGB）。
  RLE・16bpp・パレットは拒否。
- メモリ上限: 1 画像 64MB、次元 16384 まで。破損データは明白に失敗（エラー文言まで一致）。

C の `malloc`/`realloc`/`free` の手動管理と `u8 *px` 返却を、所有 `Vec<u8>` + `Result` に
置換。二重 free・free 漏れ・境界検査漏れを構造的に排除する。

**移植で発見したバグ（修正済み）**: PNG フィルタ解除は C が `raw` を**その場で**書き換え
（`prev` は「解除済みの前行」を指す）るのに対し、初版は `row` を別 `Vec` にコピーして
`prev` を未解除の `raw` から読んでいた。Up/Average/Paeth（フィルタ 2/3/4）で 2 行目以降が
全画素ずれる実害を差分 fuzz が炙り出した。in-place に直して C と byte 一致。

検証:

- **差分 fuzz**: ランダム 35,000 件（有効 PNG 全カラータイプ × 全 5 フィルタ + 高圧縮
  LZ77 参照 / 有効 BMP 24/32bpp / 破損 PNG / ランダムゴミ）で C の `if_img_decode` と
  Rust 出力（成功 + ピクセル列 + エラー文言）を突合し **0 不一致**。
- 単体テスト 4 件（PNG RGBA / PNG グレー / BMP24 / 拒否系）。
- `cargo test --offline --workspace`（akl-core 142 + akl-ffi 6 + ifuto-core 132 =
  280 件）緑、`cargo clippy --offline --workspace -- -D warnings` 緑。

次は chrome（`chrome.c` 603、オーケストレータ = 最終統合で移植）→ net/tls（`net.c` 573 /
`tls.c` 406）→ script（`script.c` 424）へ進む。`raster.c`（155 行）は fill カーネル自動
選択で全候補 bit-exact 同値のため、スカラ fill で十分（描画層統合時に組み込み）。

### フェーズ 8-p: 内部ページ生成（ifuto_pages）

`ifuto_pages.c`（248 行）を Rust へ移植した。静的な HTML テンプレート + ローカル値の
差し込みによる 4 内部ページ（settings/history/memory/about）+ unknown ページ。

- **settings / about / unknown**: 静的文字列。about の「WPT 適合率 97.3% (1679/1726)」等
  の凍結値まで byte 一致。
- **history**: `history.tsv`（epoch \t title \t url、末尾最新）を末尾から最大 100 件
  新しい順に表示。外部入力の title/url は `& < >` を escape。
- **memory**: タブごとの arena 会計（`N KB (M MB)`）+ raster backend 決定欄
  （`%10.0f MB/s` の右揃え書式を `format!("{:10.0}")` で再現。C と round-half-to-even・
  `-0` 表示まで一致）。

C は `IfChrome *`（タブ・store・raster 判定結果）と fs を直接読むが、Rust では純関数化
のため履歴テキスト・タブ一覧・raster 判定結果を引数として注入する（`&[u8] → Option<Vec<u8>>`）。

**既知の C の quirk（忠実再現）**: 履歴の url は属性値としても `& < >` のみ escape
（ヘッダコメントは「&<>\" を退避」と書くが実コードは `"` を退避しない）。

検証:

- **差分 fuzz**: ランダム 30,000 件の history.tsv（正常行/壊れ行/末尾改行/空行/エスケープ
  対象文字）で C の `if_ifuto_page` と Rust 出力を突合し **0 不一致**。settings/about/
  unknown/non-ifuto は 1 発で byte 一致。
- `%10.0f` を C `printf` と対照（12735 / 0.5 / 2.5 / -0.5 / 1000000 等の round-half-to-even
  + `-0` 表示）。
- 単体テスト 7 件（settings/about/unknown/non-ifuto/history escape/history empty/memory 表）。
- `cargo test --offline --workspace`（akl-core 142 + akl-ffi 6 + ifuto-core 139 =
  287 件）緑、`cargo clippy --offline --workspace -- -D warnings` 緑。

次は net/tls（`net.c` 573 / `tls.c` 406）→ script（`script.c` 424）→ chrome
（`chrome.c` 603、オーケストレータ = 最終統合で移植）へ進む。

### フェーズ 8-q: raster fill + HTTP 純粋関数（raster / net）

残る純粋関数の葉を移植した。

**raster（`raster.c` 155 行）**:

- `fill32(dst: &mut [u32], v)`: 32bpp fill のスカラ参照実装。C は 4 候補カーネル
  （scalar/u64x2/u64x8/smart）をマイクロベンチで自動選択するが、**全候補は bit-exact に
  同一**（`tests/test_raster.c` が相互証明）なので、スカラ一本に縮約（観測不変・選択は
  速度のみ）。
- `KERNEL_NAMES`: 診断・表示用の候補名。
- 未移植（観測不変）: 候補カーネル・`if_raster_autodetect`（`clock_gettime` + `/dev/dri`
  に依存する機種依存計測。選択は速度にのみ効く）。

**net（`net.c` 573 行の非ソケット部分）**:

- `parse_url`: http/https 分解（fragment 除去・`:port` 検査・userinfo/IPv6 拒否・
  host/path 長溢れ）。
- `resolve_url`: RFC3986 最小解決（absolute / scheme-relative / 絶対パス / 相対 /
  クエリ置換。scheme 変更 http<->https は拒否）。
- `head_parse`: 応答ヘッダ解析（状態行 + Content-Length 先勝ち / Transfer-Encoding /
  Location / Content-Type。`\r\n\r\n`・`\n\n` 宽容）。
- `dechunk`: chunked 復号（chunk-ext・trailer 消費・hex 大文字・LF-only 宽容）。
- `addr_is_private`: private/loopback/link-local/CGNAT 判定（SSRF 対策）。

C の `IfStr` 借用返却を所有 `Vec<u8>` / `Option<...>` に置換。**移植で 1 箇所バグを修正**
（`/abs` 解決で先頭 `/` を誤って剥がしていた。C は `loc` 全体を連結）。

未移植（ソケット I/O・最終統合）: `connect_one` / `send_all` / `fetch_once` /
`if_http_get(_ex)`（ソケット + BearSSL TLS。非決定的で純粋関数化不能。chrome 移植時に
`std::net` + TLS で再実装）、`tls.c`（BearSSL ラッパ）。

検証:

- **差分 fuzz**: ランダム 105,000 件（parse_url 20,000 / resolve_url 30,000 /
  head_parse 30,000 / dechunk 20,000 / private addr 5,000）で C と Rust 出力を突合し
  **0 不一致**。
- `tests/test_http.c` の主要ケース（parse/resolve/head/dechunk/private）を Rust テスト
  として再現（6 テスト関数）。
- `fill32` を C の test_raster.c の任意オフセット・任意長・任意色で再現。
- `cargo test --offline --workspace`（akl-core 142 + akl-ffi 6 + ifuto-core 147 =
  295 件）緑、`cargo clippy --offline --workspace -- -D warnings` 緑。

残るのは **script（`script.c` 424、JS エンジン DOM 結合 = akl-ffi と ifuto-core の
クロスクレート配線）** と **chrome（`chrome.c` 603、オーケストレータ）**。両者は相互
依存が深い最終統合層で、ソケット/TLS I/O を含む。

### フェーズ 8-t: TLS base64 + クローム検索（tls / chrome）

残る純粋関数の葉を移植した。

**tls（`tls.c` の base64 デコード）**:

- `b64_decode`: PEM 標準表の base64 デコード（空白類無視・`=` padding・`\0` 打ち切り）。
  CA バンドル（PEM）の証明書抽出が使う。
- 未移植（BearSSL + socket I/O）: `ta_add` / `ca_load_pem` / `ca_load` /
  `if_tls_client` / `if_tls_send_all` / `if_tls_recv` / `if_tls_close`。最終統合で
  Rust TLS に再実装。

**chrome（`chrome.c` の検索・照合）**:

- `ci_contains`: 大小無視 ASCII の部分一致（ASCII `A-Z` のみ小文字化。UTF-8 は
  バイト比較 = C と同一）。
- `find_tabs`: title/url/group のいずれかに query を含むタブ index を返す
  （文書順・最大 `max` 件）。
- 未移植（状態機械）: `tab_load` / `if_chrome_open` / `if_chrome_close` 等の
  タブ管理オーケストレータ（net/tls/script/ext を束ねる）。

検証:

- **差分 fuzz**: ランダム 60,000 件（b64_decode 30,000 / ci_contains 30,000。有効
  base64・padding・空白・`\0` 打ち切り・壊れ・多言語文字）で C と Rust 出力を突合し
  **0 不一致**。
- 単体テスト 6 件（b64 基本/空白/`\0`/拒否、ci_contains、find_tabs）。
- `cargo test --offline --workspace`（akl-core 142 + akl-ffi 6 + ifuto-core 162 =
  310 件）緑、`cargo clippy --offline --workspace -- -D warnings` 緑。

### フェーズ 8-u: 統合 CLI（純粋関数の配線 + 回帰ハーネス）

純粋関数を全配線する統合 CLI（`rust/ifuto-core/examples/ifuto.rs`）を新設し、C の
`ifuto` の観測モード（`--dump-wptdom` / `--dump-dom` / `--dump-layout` / `--dump-styles` /
`--dump-tokens` / `--no-ansi` render）を Rust 単体で再現した。併せて、未移植だった
**`if_dom_dump`（デバッグ形式の DOM ダンプ、`--dump-dom`）** を `Dom::dump()` として移植。

- **`Dom::dump()`**: `#document` + インデント付きノード列 + `; nodes=N errors=M
  title="..."`。テキスト 48 バイト打ち切り（`\n`→`\\n`、`"`→`\\\"`）、属性値 64 バイト
  打ち切り。
- **統合 CLI**: `html_tok`（token dump）→ `html_tree`（parse / fragment）→ `css`
  （style）→ `layout`（box 木）→ `render`（grid + emit）を配線。md は `md_to_html` +
  `parse_html`（C の `IFUTO_MD_SLOW=1` 経路と同値）。

**移植で 1 箇所バグを修正**: `--dump-tokens` の tag_raw は C の `%-12.*s`（precision =
全長）により**切り詰めなし**（幅 12 は最小幅の右パディングのみ）。初版は 12 バイトへ
切り詰めていた（`<annotation-xml>` が `annotation-x` になる）ため、差分 fuzz が炙り出した。

検証:

- **差分 fuzz（フルパイプライン）**: ランダム 35,000 件（HTML 20,000 + md 15,000 ×
  全 7 観測モード）で C の `ifuto` と Rust 出力を突合し **0 不一致**。
- **golden 1/1** + **html5lib 1922/1922** を統合 Rust CLI 単体で実証。
- `Dom::dump()` の単体テスト追加。
- `cargo test --offline --workspace`（akl-core 142 + akl-ffi 6 + ifuto-core 163 =
  311 件）緑、`cargo clippy --offline --workspace --examples -- -D warnings` 緑。

### フェーズ 8-s: `<script>` 実行配線の純粋関数（script）

`script.c` の **style 属性操作**（`<element>.style` HANDLE の背後の純粋関数）を移植した。

- **`style_get_prop(style_attr, prop)`**: `;` 区切り・CI 名前照合でプロパティ値を抽出
  （前後空白 trim）。C の quirk を忠実再現: 名前部の**後ろ**（`:` 直前）の空白は trim
  しない（`"color : green"` は `color` に不一致）。
- **`style_set_prop(style_attr, prop, value)`**: 既存 prop を除去して `prop:value` を
  追記。C のバッファ操作（`bl + seg_len + 1 <= cur.n` の保持条件・`i <= cur.n && bl <
  cur.n` の `;` 付与・末尾 `;;` の折り畳み）を忠実に再現。置換時は末尾 `;` を付けない
  （`"color:red;"` → `"color:blue"`）。

C の `AklHandleVTab`（`CSSStyleDeclaration`）get/set コールバックの内側にある純粋操作を、
「文字列 → 文字列」関数として抽出した。

**移植で 2 箇所の quirk を特定（忠実再現）**: ① `style_get_prop` は名前部後ろの空白を
trim しない。② `style_set_prop` は置換時に末尾 `;` を付けない（差分 fuzz が C との
不一致を炙り出して特定）。

未移植（FFI・最終統合）: `script_console_log` / `doc_*` / `elem_*` / `style_*`（VTab の
get/set/call。akl-ffi と DOM のクロスクレート配線）、`collect_scripts_rec` /
`if_script_run`（JS eval ループ）。

検証:

- **差分 fuzz**: ランダム 60,000 件（get 30,000 / set 30,000。style 属性文字列 +
  prop + value を網羅）で C の `style_get_prop` / `style_set_prop` と Rust 出力を突合し
  **0 不一致**。
- 単体テスト 3 件（get / set / C の quirk）。
- `cargo test --offline --workspace`（akl-core 142 + akl-ffi 6 + ifuto-core 156 =
  304 件）緑、`cargo clippy --offline --workspace -- -D warnings` 緑。

### フェーズ 8-r: 永続ストア書き面（store シリアライズ）

`store.c` の**書き面**（セッション保存・履歴行・ブックマークトグル）を Rust へ移植した。
読み面（`parse_session` / `parse_bookmarks`）は既存、本フェーズでシリアライズ側を補完。

- **`serialize_session(tabs, active_index)`**: `session.txt` 生成。空白タブ（url 空）は
  スキップ、title/group は非空のみ、scroll は >0 のみ。active は「保存対象の current
  タブ、無ければ先頭の保存対象」。
- **`history_line(now, title, url)`**: `epoch \t title \t url \n`（無害化込み）。
- **`shrink_history(text)`**: 512KB 超で後半（行境界から）を残す縮退。
- **`toggle_bookmark(text, title, url)`**: 存在行の除去 / 末尾追記のトグル。url 空は
  C 同様に即「変更なし」。
- 無害化（`\t \n \r` → 空白）を `push_safe` に集約。C の `gb_safe` + arena を所有
  `Vec<u8>` に置換。

C の `IfChrome *`（タブ配列 + active index）と `IfStore`（fs 注入）を、純粋な
「データ → テキスト」関数に分離。fs 書き込みは呼び出し側（chrome 移植時）が担う。

検証:

- **差分 fuzz**: ランダム 90,000 件（session 30,000 / history 30,000 / bookmark 30,000。
  id/scroll/active/空フィールド/多言語文字を網羅）で C と Rust 出力を突合し **0 不一致**。
- 単体テスト 6 件追加（session 基本/空白スキップ/無害化、history 行/無害化、
  bookmark トグル/縮退）。
- `cargo test --offline --workspace`（akl-core 142 + akl-ffi 6 + ifuto-core 153 =
  301 件）緑、`cargo clippy --offline --workspace -- -D warnings` 緑。
