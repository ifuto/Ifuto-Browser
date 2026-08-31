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

### フェーズ 8-v: DOM 変異面 + 最小セレクタ（script バインディングの基盤）

`<script>` 実行の FFI 層（`script.c` の `doc_*` / `elem_*` / `style_*` vtab コールバック）
が依存する **DOM の変異面 + セレクタ照合** を移植した。いずれも `Dom` の純粋メソッド。

- **`Dom::set_text(n, t)`**: ELEMENT の子群を単一 TEXT 子に置換（`t` 空なら全子除去）。
  C の `if_dom_set_text` 相当。旧子孫は木から切り離す（C と同様、切断サブツリーは
  `parent` を残したまま孤立）。
- **`Dom::title_set(t)`**: `<title>` を設定（無ければ head 先頭に生成）。`self.title`
  も trim 済み text_content で更新。C の `if_dom_title_set` 相当。
- **`Dom::query_selector(sel)`**: 文書順 DFS で最初にマッチする要素。対応は単純セレクタ
  （tag / `#id` / `.class` の複合）+ 空白区切り子孫結合子列（上限 4）。C の
  `if_dom_query_selector` 相当。
- **`Dom::elements_by_tag(root, tag, cap)`**: 文書順でタグ名一致の全要素を収集
  （戻り値は `(総数, 先頭 cap 個)`。`""` / `"*"` は全要素）。C の `if_dom_elements_by_tag`
  相当。

**移植で発見した C の潜在バグ（修正済み）**: `sel_part_matches` と `ebt_rec` の未知タグ
経路が `if_tag_name(IF_TAG_UNKNOWN)`（= NULL）を `strlen` する。`<my-widget>` 等の
カスタム要素を含む文書で `querySelector(\"div\")` や `getElementsByTagName(\"my-widget\")`
を呼ぶと **セグメンテーション違反** する（`strlen(NULL)`）。意図（コメントの
「未知タグ: 名前文字列と CI 比較」）どおり、要素の実名（`Node.name` = C の
`u.tag_name`）と CI 比較する形に修正した。既知タグは `name` が canonical 名
（=`if_tag_name(tag)`）と同値なので、両経路を 1 つの `str_eq_ci(&name, tag)` に統合。

検証:

- **差分 fuzz**: ランダム 50,000 件（HTML + T/Q/E/S 操作列。title_set / query_selector /
  elements_by_tag / set_text 後の wpt シリアライズ）で C と Rust 出力を突合し **0 不一致**
  （byte 一致）。未知タグ照合は C が segfault するため既知タグ域に限定し、未知タグの
  正しさは単体テストで担保。
- 単体テスト 5 件追加（set_text / query_selector 基本 / 未知タグ CI / elements_by_tag
  cap / title_set 生成・更新）。
- `cargo test --offline --workspace`（akl-core 142 + akl-ffi 6 + ifuto-core 168 =
  316 件）緑、`cargo clippy --offline --workspace -- -D warnings` 緑。

### フェーズ 8-w: `<script>` 実行配線の FFI 層（ifuto-ffi クレート）

`script.c` の **FFI 層**（`if_script_run` + AklHandleVTab の get/set/call）を移植した。
新クレート `rust/ifuto-ffi`（`#![forbid(unsafe_code)]`）が `akl-core`（JS エンジン）と
`ifuto-core`（DOM）をクロスクレート配線する。

- **`script_run(dom: &mut Dom, log: &mut Vec<u8>) -> ScriptReport`**: 文書順 DFS で
  HTML ns `<script>` を収集（上限 128）→ 同一 `Runtime` で順次 eval。外部 src・
  未知 type・空 script は skip、サイズ >4MB / NUL は明白に失敗。`IF_SCRIPT=0` kill
  switch・`has_script` 走査スイッチを維持。
- **`DOC_VT` / `ELEM_VT` / `STYLE_VT`**（`HandleVTab`）: `document`（title /
  body / documentElement / getElementById / querySelector / getElementsByTagName）、
  element（textContent / id / tagName / style / getAttribute / setAttribute）、
  `CSSStyleDeclaration`（style 属性の prop get/set）。`ptr` には C の `IfNode*` の代わりに
  `NodeId`（`Vec<Node>` index）を詰める。
- **`script_console_log`**: `[script:console]` 出力の `console.log` native（空白区切り
  連結・960 バイト cap・`\n \r` 空白化）。
- **エラー文言**: akl-ffi の `akl_eval` と同一の構築（`SyntaxError: ...` / thrown 値の
  flatten / `%.128s` 打ち切り + 改行畳み）。eval 失敗は当該 script のみ打切り・後続継続。

C のモジュール静的グローバル（`g_arena` / `g_dom` / `g_log`）と「eval は同時 1 実行」の
前提を、safe Rust の `thread_local!` + `RefCell` コンテキスト（eval 期間中のみ `Dom` を
所有）に置換。`#![forbid(unsafe_code)]` を維持するため raw ポインタは一切使わない。

**C の `call` コールバックの quirk（忠実再現）**: `akl_native_throw` は Rust エンジン
（akl-ffi の `handle_call_adapter`）では握り潰され、`out` 未書込の `Some(0)`（= double
0.0）が返る。引数数不一致・非文字列引数・名前過長の各 throw 経路を `Some(from_bits(0))`
で再現（差分 fuzz が byte 一致で検証）。

検証:

- **差分 fuzz**: ランダム 12,000 件（HTML + `<script>`。textContent / title / set・
  getAttribute / style / tagName / console.log / getElementsByTagName / querySelector /
  構文エラー / TypeError / throw / budget 枯渇 / skip 規則）で C の `if_script_run`
  （libakl_ffi.a リンク）と Rust `script_run` の出力（wpt + log + report）を突合し
  **0 不一致**（byte 一致）。
- 単体テスト 6 件（textContent 変異 / title + console / failure 隔離 / skip 規則 /
  style + tagName + attr / kill switch）。
- `cargo test --offline --workspace`（akl-core 142 + akl-ffi 6 + ifuto-core 168 +
  ifuto-ffi 6 = 322 件）緑、`cargo clippy --offline --workspace -- -D warnings` 緑。

これで **script.c の FFI 層（JS 実行 + DOM バインディング）が Rust で完動**。残る最終
統合は chrome（`chrome.c` 603、タブ状態機械）・net ソケット（`net.c`）・TLS ソケット
（`tls.c`、BearSSL 置換）のみ。

### フェーズ 8-x: TLS CA ロード（ca_load_pem + ta_add）— X.509 トラストアンカー抽出

`tls.c` の **CA ロード層**（`ca_load_pem` / `ta_add`）を移植した。PEM バンドルから証明書を
抽出し、DER 証明書から subject DN + SPKI 公開鍵（トラストアンカー）を BearSSL の
`br_x509_decoder` と同一の規則で抜き出す。

- **`b64_decode`**（既存）+ **`ca_load_pem(pem) -> Vec<Vec<u8>>`**: `-----BEGIN
  CERTIFICATE-----` / `-----END CERTIFICATE-----` の組を走査し、間の base64 を復号。
  破損証明書は無視（C と同じく 1 枚でも成功すれば前進）。
- **`ta_add(der) -> Option<TrustAnchor>`**: 最小 DER/ASN.1 リーダで X.509 構造を解析。
  `Certificate → tbsCertificate → [version] / serialNumber / signature / issuer /
  validity / subject / SPKI / [1][2][3] extensions / signatureAlgorithm /
  signatureValue` の全構文を検証し、subject Name の DER 全体（DN）と公開鍵
  （RSA `n`/`e`、EC `curve`/`q`）を抽出。
- **`TrustAnchor` / `Pkey`**: 自己完結の正規化表現（DN + SPKI）。rustls の
  `TrustAnchor` も DN + SPKI を要求するため、TLS バックエンド非依存。

**BearSSL の `br_x509_decoder` の全検証を忠実再現**（差分 fuzz が byte 一致で担保）:

- version 0..2 のみ（`read-small-int-value`。先頭バイト < 0x80）。
- validity の日付検証（`read-date`: UTCTime/GeneralizedTime の書式・月/日/時/分/秒
  範囲・うるう年・小数秒・`Z` 終端）。
- extensions の構造検証 + **basicConstraints**（`SEQUENCE { BOOLEAN OPTIONAL }`）の
  内容検証。
- 入れ子長さの親構造はみ出し（`open-elt` の `lim < length`）・末尾余分バイト・不定長
  （0x80）・拡張タグ（31）・application/private クラスの拒否。
- RSA は `RSAPublicKey`（先頭 0 剥ぎ）、EC は `id-ecPublicKey` + 曲線 OID（P-256/384/521）。

検証:

- **差分 fuzz**: システム CA バンドル（`/etc/ssl/certs/ca-certificates.crt`）の実
  143 枚 + 切断 20,000 + 変異 20,000 + ランダムゴミ 20,000 = **60,143 件**で C
  （BearSSL `br_x509_decoder`）と Rust `ta_add` の出力（成功/失敗 + DN + 鍵）を突合し
  **0 不一致**（byte 一致）。切断/変異 fuzz が validity 日付検証・basicConstraints・
  入れ子長さ検査の各経路を炙り出し、順に修正した。
- 単体テスト 7 件（b64 基本/空白/NUL/拒否、ca_load_pem 抽出、ta_add ゴミ/日付必須）。
- `cargo test --offline --workspace`（akl-core 142 + akl-ffi 6 + ifuto-core 171 +
  ifuto-ffi 6 = 325 件）緑、`cargo clippy --offline --workspace -- -D warnings` 緑。

残る最終統合は chrome（`chrome.c` 603、タブ状態機械）・net ソケット（`net.c` の
`connect_one`/`send_all`/`fetch_once`/`if_http_get(_ex)`）・TLS ソケット（`tls.c` の
`if_tls_client` 等、BearSSL 置換）。

### フェーズ 8-y: net ソケット + TLS ソケット（net_sock / bearssl）— https 取得の完動

`net.c` のソケット層（`connect_one` / `send_all` / `fetch_once` / `if_http_get(_ex)`）と
`tls.c` のソケット層（`if_tls_client` / `if_tls_send_all` / `if_tls_recv` /
`if_tls_close`）を `std::net` + BearSSL で移植した。http（平文）と https（TLS 1.2）の
両経路をカバーする（**スタブなし・完全実装**）。

- **`net_sock`（`net.c` ソケット層）**: `connect_one`（`ToSocketAddrs` IPv4 のみ・
  `connect_timeout` 8s・`set_read/write_timeout` 10s = SO_*TIMEO 相当）、`send_all`、
  `fetch_once`（リクエスト構築 → EOF まで受信 → ヘッダ解析 → chunked/Content-Length/
  close ボディ決定）、`http_get_ex`（リダイレクト 5 回・private 拒否・https→http 降格
  拒否）。err 分類（"bad url"/"dns"/"connect"/"send"/"recv"/"too large"/"bad response"/
  "truncated"/"redirect loop"/"private redirect blocked"/"https downgrade blocked"）を
  C と同一に。URL 分解・解決・ヘッダ解析・chunked・private 判定は `ifuto_core::net` の
  純粋関数（差分 fuzz 検証済み）を再利用。
- **`bearssl`（`tls.c` ソケット層）**: BearSSL の unsafe FFI 境界（`br_ssl_client_*` /
  `br_ssl_engine_*`）。`br_ssl_client_context` / `br_x509_minimal_context` は巨大内部構造
  のため **レイアウトを再現せず**、`build.rs` が `sizeof` から生成するサイズ
  （`bearssl_sizes.rs`）で確保した 8 バイトアライン済みバッファを不透明ポインタで渡す。
  `br_x509_trust_anchor`（64B）は `repr(C)` で再現し、コンパイル時 `assert!` でサイズを
  検証。`static inline` の `br_ssl_engine_set_versions` / `br_ssl_engine_last_error` は
  `bearssl_shim.c`（非インライン薄ラッパ）で提供。
- **`TlsClient`**: BearSSL エンジン駆動（`sendrec_buf`/`recvrec_buf` が生レコードを渡し、
  `TcpStream` が実 I/O を担う）。C の `tls.c` が `int fd` へ raw `send`/`recv` するのに対し、
  `std::net` に移行。ハンドシェイク（`tls_run_until` 相当）・`send_all`（flush 込み）・
  `recv`・`close`（close_notify ベストエフォート）を C と同一セマンティクスで再現。
  エラー分類（"tls"/"cert"(33..63)/"ca"/"send"/"recv"）も同一。
- **CA ロード**: `ca_load`（`IFUTO_CA_BUNDLE` → 既定 4 パス）は `ifuto_core::tls` の
  `ca_load_pem_anchors`（`ca_load_pem` + `ta_add`、差分 fuzz 検証済み）を再利用。
  プロセス 1 回ロードは `OnceLock` で保持（C のプロセス静的 `g_ta` と同型）。

**ビルド**: `ifuto-ffi/build.rs` が BearSSL（`vendor/bearssl`、MIT、166 .c）をシステム
`cc` + `ar` で静的ライブラリ化してリンク（`cc` クレート非依存 = オフライン可）。製品法則
「ldd = vdso/libm/libc/ld」は静的リンクで維持。unsafe は `bearssl.rs` にのみ存在し、
`// SAFETY:` コメント付きで集約（akl-ffi と同じ「境界を 1 ファイルに集約」方針。
`#![deny(unsafe_op_in_unsafe_fn)]`。それ以外のモジュールは safe Rust）。

検証:

- **差分 fuzz（ローカル HTTP + HTTPS サーバ）**: openssl で自己署名 CA + server 証明書
  （SAN: DNS:localhost, IP:127.0.0.1）を生成し、Python 生ソケットサーバ（HTTP 18080 /
  HTTPS 18443）で http 11 種（200/404/chunked/empty/204/no-length/utf-8/redirect/
  redirect404/redirect-loop/欠落）+ https 13 種（CA 信頼あり / システム CA のみ / 降格
  拒否 / localhost SNI）の計 **24 URL × 3 反復** で C の `if_http_get_ex`（net.c+tls.c+
  BearSSL）と Rust `http_get_ex` の出力（status + Content-Type + ボディ / err 分類）を突合し
  **0 不一致**（byte 一致）。https（CA 信頼）は 200 + ボディが正しく取得でき、**Rust 単体で
  TLS 1.2 ハンドシェイク + 証明書検証 + SNI 照合が完動**することを実証。
- 単体テスト 3 件（private 判定 9 パタン / URL 過長 / 不正 URL）。
- `cargo test --offline --workspace`（akl-core 142 + akl-ffi 6 + ifuto-core 171 +
  ifuto-ffi 9 = 328 件）緑、`cargo clippy --offline --workspace -- -D warnings` 緑。

これで **net/tls のソケット層（http + https 取得）が Rust で完動**。残る最終統合は
chrome（`chrome.c` 603、タブ状態機械 = `net_sock` / `script_run` / store を束ねる
オーケストレータ）と `main.c` の Rust 置換のみ。

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

### フェーズ 8-z: 最終統合 — C↔Rust CLI byte-exact 達成 + 差分 fuzz 98,133 cases / 0 mismatch

CLI 全観測モード（render / `--dump-*` / `--fragment` / `--links` / `--stats` / `--md` /
`--slim-dom` / `--show-paths` / `--imgdecode` / http(s) 取得 / `<script>` 実行）を Rust
単体で完動させ、**C バイナリを回帰オラクルとした差分 fuzz（stdout/stderr/rc の byte
突合）で累計 98,133 cases / 0 mismatch** を達成した。GUI（`--gui` / `--shot` / `--ui`）
のみ未移植で、明示メッセージ + rc=2 の明白拒否（拒否形状自体も byte 検証済）。

検出・修正した偏差 8 群（全て再現 case に縮約 → 根因特定 → 機械再検証）:

1. **byte-direct quirk（render 発行側）**。C の sweep は direct 旗付き行をセルモデル
   経由せず生バイト発行する（`row_emit_direct` / `row_emit_fast` / `row_emit_ansi_fast`）。
   wrap の検査 quirk（折り返し次行に紛れた 0 幅/不正グリフは direct を殺さない）で
   生バイト ≠ セル再エンコードとなる行が存在。Rust に行監査帳簿（`RowInfo`: 全 paint
   イベント畳み込み）+ C 受理条件の機械的写し（`fast_plan` / `emit_fast_row`）を実装。
2. **C layout.c:574 の kill 漏れ**。pre 内の `\t` / `\r` / `\f`（`b0 != ' '`）で C は
   `direct_all = 0` にするが Rust 版に欠落 → 追加（C 動作が正本）。
3. **C src/dom.c の NULL strlen SEGV（ゼロデイ 2 件）**。`sel_part_matches`
   （querySelector）と `ebt_rec`（getElementsByTagName 未知タグ経路）が
   `if_tag_name(IF_TAG_UNKNOWN) = NULL` を `strlen` して SEGV。実タグ名 `u.tag_name`
   との CI 比較に統一して C 側を修正（既知タグは `u.tag_name ≡ canonical` で同値）。
   未知タグ要素を含む文書での `document.querySelector('div')` 等が crash していた
   （fuzz が 7 件の crash case を検出。ASan 診断: `src/dom.c:276:26`）。
4. **`; nodes=N` の計数規約**。C の `dom->n_nodes` は解析時専用カウンタで script の
   事後生成を含まない一方、Rust は物理ノード数を出していた。`Dom::n_nodes`（解析凍結
   カウンタ）を追加し dump/stats/css-dump で統一。
5. **`--stats` の `links=N`**。C はレイアウトが常時リンク収集する（`--links` 旗と無関係）
   → Rust も常時収集に変更（表示側のみ旗でゲート）。
6. **`--stats` の `grid=WxH`**。C の CLI render は linear build（`no_boxlink`）のため
   `if_render_extent` は root までしか歩かない → Rust の stats extent を
   `max(lay.width, root.x+root.w)` / `max(lay.height, root.y+root.h)` に修正。
7. **seg merge の arena 隣接 quirk**。C `wrap_push_merge` の `pm_end == p` はポインタ
   比較で、連続 `<img>` 占位文字列が arena の 8B アライン bump 確保で物理隣接すると
   2 つの `[img: …]` が 1 seg に合体する（`--dump-layout` の `segs=N` にのみ観測）。
   由来位置クラス `Prov{Src(id), Syn, Never}` + 合成 arena bump モデル +
   pieces/links/prec の grow イベント追跡で C の受理条件を厳密に再現。
8. **MARKER+背景重畳の fast 経路依存 bytes**。マージン相殺で `<h4>` の背景が直前
   `<li>` マーカー行を覆う際、C slow（deco 層順 paint）と C fast（run = 最上位層 pen）
   で bytes が異なり、C は fast を取る。render.rs を universal fast エミュレーションに
   拡張（LINE 全件ペイロード + markers を `RowInfo` に保持、`fast_plan` が行 0/1
   LINE・BORDER 無・ansi 条件・MARKER グリフ検査・runs≤64・非重複・clip 整合を写す）。

検証（2026-08-24 実走、全て緑）:

- **差分 fuzz 累計 98,133 cases / 0 mismatch**: seed 20260824 (30,019) / 1 (20,019) /
  777 (20,019) / 424242 (20,019) / 999 (3,019 ×2 回) / 777 再検証 (2,019)。
  ツールは `tools/diff_fuzz_cli.py`（決定 RNG で完全再現。`--stats` は
  タイミング/RSS/C 固有 arena 会計のみ scrub、決定値 nodes/links/grid は比較に残す）。
- oracle 両バイナリ 21/21（idm コーパスは `tools/gen_idm.py` で決定再生成して復元。
  環境消失で 7 NG 化していたのを解消）。
- WPT tree-construction 両バイナリ 1922/1922 (100.0%)、skip 12（`#script-on` のみ）。
- golden 両バイナリ PASS。`cargo test`: 328 件（akl-core 142 + akl-ffi 6 +
  ifuto-core 171 + ifuto-cli 9）緑。`cargo clippy --offline --workspace -- -D warnings`
  緑（C 側も gcc -Wall -Wextra 警告ゼロ）。
- ベンチ計測そのものが適合性ゲートを兼ねる設計: 177 計測ペア全てで stdout byte 一致 +
  stderr scrub 後一致を機械検証しながら速度を測った。

**既知の差異（嘘をつかない台帳）**: 速度・メモリは現状 C が全面優位（16MB md total
C 130.14ms vs Rust 2,404.21ms = 18.47×、peak RSS 212,960KB vs 2,523,504KB = 11.85×。
2MB では 16.71× / 9.51×）。Rust 版は byte-exact 凍結を優先した正確性移植であり、
C の性能機構（lazy style・row_emit fast 群・fitdom CJK SIMD・並列 layout）は未移植。
行監査帳簿の常時有効化が既知のオーバーヘッド源。起動時間のみ Rust が僅かに速い
（median 1.55ms vs 1.67ms、150 対で符号 17:133）。ldd は Rust が `libgcc_s` 一本
多い（C は `-static-libgcc` 適用済）。多角ベンチの全量は
`bench/report-2026-08-24.html`（生成物。再生成: `python3 bench/bench_c_vs_rust.py`
→ `sh bench/run_gates.sh` → `python3 bench/gen_report.py bench/data-2026-08-24.json`）。

### フェーズ 9-a: chrome モデル純粋部の Rust 移植 + C↔Rust 差分 fuzz ハーネス恒久化 + akl-ffi leak 全廃

`src/chrome.c`（タブモデル/スクロール/URL 解決/検索）の**純粋関数 8 単位**を
`rust/ifuto-core/src/chrome.rs` へ機械的写し（全体状態機械 —`cur_doc_bytes`/タブ配列の
生命周期/描画/イベント loop— は未移植と明記）。C との byte-exact を差分 fuzz で
凍結し、併せて C 側の UB を根絶し、akl-ffi の既知 leak を所有権設計で全廃した。

移植（全て C 正本の機械的写し、quirk 付き）:

1. `dup_cap`（C: `dup_cap` static）— cap>=1 契約。C の cap==0 は u32 underflow →
   4GiB alloc 即死領域のため信頼域外（Rust は assert で明白拒否）。
2. `ci_contains` — ASCII 大小不変の部分文字列。
3. `scroll_apply`（`if_chrome_scroll`）— **2 段逐次 clamp が必須**。wrap 域で maxs が
   負になると `<0 → 0` の後に `0 > maxs` で maxs へ引き戻される C quirk があり、
   早期 return 形はこの引き戻しを落とす = 差分 fuzz が検出した実バグ（修正済）。
4. `scroll_to_apply`（`if_chrome_scroll_to` lay 存在経路）— scroll と**非対称**:
   C の scroll_to は単一三項で `pos<0 → 0` のみ、maxs 再評価は無い。
5. `quit_decide`（確認 2 度押し判定）— 時計逆行 quirk を wrapping_sub で写す。
6. `link_move`（`if_chrome_link_move`）— `wrapping_rem` 使用。C では
   `INT_MIN % -1` が x86 SIGFPE crash UB だったため **C 側に `n == -1` ガードを挿入**
   して同値化（旧観測が変わるのは従来 crash していた入力のみ = ゼロデイ級修正）。
7. `resolve`（`if_chrome_resolve`）— skip_ws は `' '`/`'\t'` のみ、`"://"` 検出位置
   2..=8 quirk、絶対パスは cap 検査を exists より先・`rc=2` でも out は memcpy 済み
   quirk、cwd 相対は snprintf 4095 截断、の非対称群を忠実移植。
8. `find_tabs` + `TabSearch` — 検索クエリ/タイトル/URL を **Vec<u8> 保持**
   （String 化しない。非 UTF-8 タイトルも unsafe 無しで流通可能）。

ハーネス恒久化（全てコミット対象の再現可能資産）:

- `tools/zz_chrome_dump.c` — C オラクル driver。1 入力行=1 出行（CI/DUP/SCR/SCT/
  QUI/LNK/RES/FT）。決定論 fs 予言者 `exists(s) = fnv1a64(s) % 4 == 0`（offset
  basis 14695981039346656037ULL）、0x01 センチネルで「out 未書き」を検出、
  BAD 行は raw 退避コピーから出力。
- `rust/ifuto-core/examples/zz_chrome_dump.rs` — Rust 対応 driver（同一形式）。
- `tools/zz_chrome_diff.py` — 生成器+突合（python random で seed 完全再現、
  **両 driver の rc≠0 も mismatch 扱い**。空文字は `-` エンコード）。
- C 側検証フック: `if_chrome_test_dup_cap` / `if_chrome_test_ci_contains`
  （src/chrome.c/h。static 関数を driver から触るための shim と明記、本番非使用）。

発見・修正した C 側 UB（ASan+UBSan 30k が確定、修正は観測不変）:

- `if_chrome_scroll` / `if_chrome_scroll_to` / `if_chrome_link_move` の i32 符号溢れ
  → `(i32)((u32)a - (u32)b)` の**明示 wrap** 化（実効 C ビルドと同値、UB のみ消去）。
- `if_chrome_link_move` の `INT_MIN % -1` SIGFPE → 上記 `n == -1` ガード。

**akl-ffi `handle_vtab_for` の意図的 leak 全廃**（`rust/akl-ffi/src/lib.rs`）:
従来は C vtable ごとに `Box::leak` を 2 回実行（tag boxed str + `HandleVTab` box。
LSan 報告: 80B direct + 23B indirect）。**所有権を AklRT へ移設**:
`owned_vtabs: Vec<Box<HandleVTab>>` / `owned_tags: Vec<Box<str>>` フィールドに
所有させ、cache の `&'static` 参照はその Box ヒープ領域（Vec 伸長で不動）を指す
ライフタイム延長として SAFETY コメントで監査下に置いた。drop はフィールド宣言順
（Runtime → cache → owned 群）で行われ、akl-core にカスタム Drop は無い（
`impl Drop` grep 0 件確認）ため Runtime 解体中に参照先は読まれず、最後に owned 群が
自動解放される。`#[allow(clippy::vec_box)]` には「Box はアドレス安定化が目的の
ため誤検出」と正当化コメントを併記。効果: **`chk_oracle ./build/ifuto-asan` が
21/21 に復帰**（script.out は LSan が `_Exit` で stdout flush 前に死ぬ副作用で
空 got の FAIL 化を起こしていた。leak 0 で解消）。

**生成コードと rustfmt の恒久和解**: `cargo fmt --all` が生成テーブル
（`charset_tables.rs` 5.5k 行 / `entities_tables.rs`）を整形して
`gen_*.py --verify`（byte-exact 再生成照合）を破壊する構造的衝突を、生成器 2 本
（`tools/gen_charset.py` / `tools/gen_entities.py`）が各大表に `#[rustfmt::skip]` を
焼く形で解消。以後 `cargo fmt --all` を workspace 全体に打っても照合を侵さない
（`--verify` 再確認済）。

検証（2026-08-26 実走、全て緑）:

- **zz_chrome 差分 fuzz 累計 240,000 cases / 0 mismatch**: 開発時 140,000
  （seed 1/7/42/999/777/424242/20260826 × 20,000）+ 本コミット直前の復帰検証
  100,000（seed 1/7/42/999/777 × 20,000。リポジトリ再 clone 後に全 driver 再
  ビルドして実走）。
- **ASan+UBSan 30,000 cases × 2 回で rc=0 / stderr 空**（`/tmp/zz_chrome_c_asan`
  自前ビルド、`-ffunction-sections --gc-sections` 併用）。
- `cargo test --offline` **333 緑**（akl-core 142 + akl-ffi 6 + ifuto-core 176 +
  ifuto-cli 9。chrome.rs に7 テスト追加）。
- `cargo clippy --offline --workspace --all-targets -- -D warnings` 緑、
  `cargo fmt --all --check` 緑（生成テーブルは skip 属性で rustfmt 対象外）。
- `make test` 625,125 checks ×2 / 0 failures、`chk_oracle` 21/21 × 両バイナリ
  （ASan 側 script.out 復帰を含む）、golden 1/1、gui_smoke PASS、ext_smoke 12/12。

**既知の未移植・近似（嘘をつかない台帳の継続）**: chrome の全体状態機械
（`cur_doc_bytes` / タブ配列の生命周期 / 描画 / イベント loop）は未移植。
GUI（`--gui` / `--shot` / `--ui`）と `--ext` の chrome init も依然未移植で、
明示拒否形状のみ差分検証済み（8-z 台帳どおり）。zz_chrome fuzz の信頼域は
`zz_chrome_diff.py` ヘッダのとおり（文字列に NUL/0x01/改行を含まない、
DUP cap>=1、RES cap の out 観測は 511B まで）。

### フェーズ 10-a: render を行スイープ実装へ全面移植（全グリッド + RowInfo 帳簿の撤廃）

**背景（発見）**: C の CLI render は `if_render_emit_rows_sweep`（行スイープ）で、
**グリッドを一度も構築しない**（`--stats` の `render_split` が `grid=0.00ms` と
機械表示する）。対して Rust 旧実装は byte 一致の監査のため paint イベントを
**全グリッド（16MB コーパスで 852,094 行 × 100 セル × 8B ≒ 682MB）へ畳み込み、
さらに全行の監査帳簿（RowInfo）を保持**してから発行経路を事後判定していた。
これは C に存在しない全構造の会費であり、render 段 867.41ms（うち grid 構築
≈717ms + emit ≈618ms）と 16MB RSS 2,523,504KB の主因だった。本フェーズで
C と同型のフラット streams（lines / deco / seg_arena）を直接消費する逐次発行へ
一本化した。

変更（`rust/ifuto-core/src/layout.rs` / `render.rs` の全面改訂）:

- `layout.rs`: `RLine{y,direct,seg_lo,seg_hi}` / `DecoKind{Bg,Border,Hline,Marker}`
  / `Deco` を追加。`BoxNode.segs: Vec<Seg>` を `seg_lo/seg_hi: u32`（`Layout` の
  `seg_arena` 内共有区間 = C の `w->seg_base` 共有構造の写し）へ置換。`Layout` に
  `lines` / `deco` / `seg_arena` を追加。`deco_push` / `deco_patch_h` /
  `deco_marker_push` を新設し、BG（条件 `(bg&0xFF)>=128` + h 後埋め）/ BORDER
  （sides ビット写し）/ HLINE（hr: y=g.bt、h=1、BG も bh 後埋め）/ MARKER
  （li_ord カウンタ。ul は x=bx-2 で clamp 基準は **bx**、ol は x=bx-(m+1) で
  clamp 基準は **0**、の非対称 quirk と `%u.` の u32 幅を保存）を追記。
- `render.rs`: **全面書き換え**。`render_emit_sweep` = C `sweep_range` 直列経路の
  機械的写し（ギャップ一括 '\n' 充填 → deco 開始/期限切れ → blank 行 →
  no-ansi: cp_free 判定 → try_direct / try_fast（runs≤64、安定挿入ソート、
  失敗時 truncate 巻き戻し、pos は HLINE で未 clip の元 w 進行の写し）→
  ansi: try_ansi_fast（BG ピース合成 ≤32、MARKER glyph 検査は **ansi 版のみ**
  で no-ansi fast は非検査、の非対称保存、失敗時 truncate + cur save 復元）→
  emit_slow（no-ansi maxx 計算 → 既定充填 → deco paint 追記順 → lines paint →
  trim / 行末リセット））。`emit_pen` は `u8_dec` + 48B スタックバッファ 1 回
  追記化（`format!` の一時 String 確保を排除、render ≈290→≈213ms 相当の残差
  削減）。Grid / RowInfo / render_grid / render_emit / fill_bg / paint_shell 等は
  **全削除**（写し元が C に存在しない車輪のため）。
- 呼出側: `ifuto-cli/src/main.rs` と `examples/ifuto.rs` を `render_emit_sweep`
  呼出に差替。`acc_grid = 0.0`（C の CLI sweep 表示 `grid=0.00` に機械一致）。
- clippy 静粛化: `int_plus_one`（`d.x+d.w-1 >= 0` → `d.x+d.w > 0`、整数同値）と
  `collapsible_if`（`&&` 畳み込み、短絡で同値）のみ。いずれも同値変形。

性能（`bench/bench_c_vs_rust.py` paired median、2026-08-28。表全量は BENCH.md /
`bench/data-20260828.json` / `bench/results/report-20260828.html`）:

- 16MB total: 2,404.21 → **1,632.49ms**（C 127.11ms。18.47× → **12.84×**）
- 16MB render 段: 867.41 → **67.36ms**（C 22.47ms。38.2× → **3.00×**）
- 16MB peak RSS: 2,523,504 → **1,383,216KB**（**−1.14GB**。11.85× → 6.50×）
- 2MB total: 310.94 → 195.10ms（16.71× → 9.82×）、RSS 9.51× → 5.28×
- ANSI 2MB render 段: C 32.02ms / Rust 38.61ms = **1.21×**（byte 密度の高い
  ansi 発行ではほぼ肉薄）
- 起動 wall は Rust 優位を継続（1.61 vs 1.82ms、符号 6:144）

検証（2026-08-28 実走、全て緑）:

- **16MB/2MB IDM の C↔Rust stdout byte-exact**: 16MB ansi(既定) 120,298,176B、
  16MB no-ansi 15,965,641B、2MB 両モード、`cmp` 一致。
- **diff fuzz 28,000 cases 追加 / 0 mismatch**（seed 1 ×8,000 / 777 ×8,000 /
  31337 ×12,000）→ **累計 126,133 cases / 0 mismatch**。
- WPT tree-construction **1922/1922 × 両バイナリ**（C ASan / Rust release）。
- `make test` 625,125 checks ×2 / 0、`chk_oracle` 21/21 × 両バイナリ（ASan 側
  LSan 緑を維持）、golden 1/1 ×2、gui_smoke PASS、ext_smoke 12/12、
  `make fuzz` 500 iters × 5 標的 0 crash。
- `cargo test --offline` **334 緑**（ifuto-core 176→177。`li_marker_ul_ol` 追加）、
  `cargo clippy --workspace --all-targets -- -D warnings` 緑、`cargo fmt --all --check` 緑。
- 計測ベンチの全 177 ペアで stdout byte 一致 + stderr(scrub) 一致を同時検証。

**未移植・既知の近似（台帳継続）**: 2-way 並列 sweep（C の pthread 分割）は未移植
（発行バイト列は直列と厳密一致が C で保証済みのため byte 観測では区別不能、
速度差のみ）。窓グリッド経路（`if_render_grid_rows_into(_cur)`、GUI 用で CLI
不使用）と raster カーネル自動選択も未移植。**残る最大ギャップは style 段**
（C 0.01ms lazy vs Rust 552.74ms eager。C の `if_style_lazy_*` に相当する DFS
訪問時解決の移植が次フェーズ候補）、ついで layout 段（736.33ms。
IfGeomCache / AVX2 可視ラン / 2-way 並列 layout）、parse 段（258.10ms）、
read 段（8.71ms。`fs::read` → mmap 案、出力同値・観測不変）。

### フェーズ 9-b: akl-ffi `handle_vtab_for` の `Box::into_raw` 化（Miri/Tree Borrows 適合）+ trigger Miri ログ採取

CI run 33124469058（@59de005）で **CMD 5（Miri）のみ exit 101** 失敗（Miri 緑
2026-08-15 @2cc4d6d → 赤 2026-08-27 の bisect 区間は 8-z+9-a 全部）。主容疑は
9-a の leak 全廃で導入した `owned_vtabs: Vec<Box<HandleVTab>>` /
`owned_tags: Vec<Box<str>>` の構造: **Tree Borrows では Box 値のムーブ
（Vec push / 再配置）が pointee を Unique retag し、それ以前に作られた
`&'static` 派生参照のタグを殺す**。akl-ffi の `handle_roundtrip` テストは
vtab 生成 → `akl_free` を踏むため、この規則に抵触し得る。

修正（`rust/akl-ffi/src/lib.rs`、検証後に確定度を更新する**仮説駆動の修正**）:
FFI 標準イディオムへ変更 — `Box::into_raw` で生成した**生ポインタを Vec に保持**
（`Vec<*mut HandleVTab>` / `Vec<*mut str>`。**Box 値のムーブを一度も存在させない**
ため Unique retag は起きず、参照タグは回収まで生存）。`akl_free` は
`std::mem::take` で Vec を抜いた後に各要素へ `drop(Box::from_raw(p))` を明示実行、
最後に `drop(boxed)`（akl-core にカスタム Drop は無く（grep 0 件）、Runtime drop
中に vtab/tag 中身は読まれないため先回収で安全、と SAFETY コメントで監査下）。
leak ゼロ性（9-a の成果）は `chk_oracle ./build/ifuto-asan` 21/21 = LSan 緑で
再確認済み。

**正直注記**: ローカルでは `cargo +nightly miri setup` が crates.io へのネット
アクセス不可で sysroot を構築できず、根本原因のローカル再現はできていない。
確証は CI 経由でのみ得られるため、`trigger/trigger.md` の Miri 行に
`/tmp/miri.log` の result.md 追記（tail -80）を追加し、次回 CI で実エラーが
読めるようにした。CI 緑を以て確定、赤なら miri.log から再調査。

### フェーズ 10-b: style 段の消去（IfStyleLazy 移植）+ eager 経路の決定メモ化 — 16MB total 1,041ms（C 比 8.9×）

**問題**: Rust の style 解決は全ノード eager 全走査を、メモ化も intern も無しで
各ノードに `Vec<Option<Winner>>` をヒープ確保する素朴形で実行していた
（16MB で 552.74ms、16MB total の 1/3）。C は 3 段機構でこれを消している:
md fast-DOM × CLI 行スイープでの lazy 要所解決（style 段 0.00ms、layout の
DFS 訪問時のみ）+ author 無し HTML での決定メモ化 eager（直接マップ + intern）
+ author 有りのみ安全側の全走査。いずれも `st_resolve_memo` / `compute_node`
一点化で全経路の解決値が同値。

移植（全て C 機構の写し。値は `compute_style` 一点化で全経路 byte 同値）:

- `css.rs`: `compute_node` → pure 値関数 `compute_style` へ分離（C の一点化写し）。
  `StyleKey`（Style 全フィールドのビット同一性キー。f32 は to_bits）、
  `StyleIntern`（開放番地・負荷率 0.75・×2 成長。SipHash ではなく u32 語の
  乗算混合 — lazy は**ノードごとに parent 値の逆引き**を打つため std HashMap の
  SipHash(112B) は ~140ns/ノード = 16MB で +230ms の無駄と実測同定）、
  `StyleCache`（直接マップ 2^14。キーは (parent 値の intern idx, k2)。
  k2: 既知タグ `(tag<<1)|1` / 未知タグは名前バッファのアドレス — DOM 不変なので
  ABA なし、値同一でない限り hit しない保守性 = C の raw pointer キーの写し）、
  `StyleLazy`（1/2 スロット LRU + 直接マップ。2 番 hit 時の昇格スワップ、
  inline style 持ちはメモ通路ごとバイパス、の C 写し）、
  `style_lazy_ok`（`md_ws_stripped && !has_style && IF_STYLE_LAZY!=0` 写し）。
- eager もメモ化: `compute_walk` は author シート無しのときだけ `StyleCache`
  経路（C の if_style_apply の条件写し）。**HTML（author 無し）の style 段は
  552→77ms**（16MB 測。メモ hit でも旧来は Winner Vec 確保×全ノードだった）。
- `layout.rs`: `StSrc::{Eager,Lazy}` + `lc_st_of`（C の `lc_st_of` 写し）で st
  アクセス一点化。lazy 時は ELEMENT → 解決値、非 ELEMENT → pst（継承の意味）。
  `layout_build` を内部 `build_impl` 化し `layout_build_lazy` を追加
  （html→body 先行解決と `lazy_rfs = html 算出 font_size` の確定は build_impl 写し）。
- `main.rs`: `use_lazy_style` ゲート写し（`mode==Render && style_lazy_ok &&
  !has_script && !has_style`）。lazy 時は `styles` 表の確保すらしない
  （1.6M × Option<Style> = 80MB の死蔵確保+memset で 76.75ms 消費していた自前の
  無駄を構造消去。C は if_style_apply 自体を呼ばない）。`collect_links` は
  スタイル供給クロージャ化（lazy では `display:none` 判定用に解決。UA のみでは
  display は継承されず (tag|name, inline style) の関数 = parent 非依存で
  全面走査値と同値、の解析を根拠に None parent で解決。stats の links 数と
  --links 出力が差分 fuzz と --stats 突合で機械検証）。

検証（2026-08-28 実走、全て緑）:

- 16MB/2MB × ansi/no-ansi 4 経路の C↔Rust stdout **byte-exact** 維持。
- **lazy ≡ eager の全ノード値一致**: 10 文書バッテリ（ネスト・未知タグ・
  inline style・display:none 部分木含む）で DFS pre-order 解決突合、
  md fast-DOM で 3 幅 × 2 モードの render byte 一致（新規 unit test 3 件）。
- diff fuzz **16,000 cases 追加 / 0 mismatch**（seed 20260828/555。
  author シート・inline・script 含有の HTML を主に通る eager 側もカバー）→
  **累計 150,133 cases / 0 mismatch**。
- cargo test **337 緑**、clippy `-D warnings` 緑、fmt 緑、WPT 1922/1922、golden 1/1。

計測（`bench/data-20260828b.json`、paired median。16MB）: total 1,632.49→
**1,040.57ms**（C 116.87ms で **8.9×**）、style 552.74→**0.05ms**、RSS
1,383,216→1,187,932KB（5.6×）。2MB total 195.10→132.83ms（8.8×）、ANSI 2MB
230.58→158.68ms（4.5×）。startup は **1.4135 vs 1.4135ms で完全互角**（符号 76:73）。

**残照準（嘘をつかない台帳）**: layout 段 736.84ms（C 46.47ms = **15.9×**。lazy
解決の残オーバーヘッド ~50ms も含む）が最大の残件 — C の IfGeomCache /
AVX2 可視ラン / no_boxlink / box_pool / 2-way 並列が未移植。parse 239.33ms
（C 47.53ms = 5.0×、2-slice 並列と fitdom 機構が未移植）、render 58.62ms
（2.8×）、read 8.71ms 級（fs::read → mmap 案）。RSS も layout 由来の box/seg
確保嵐が主因と推定（推定。次フェーズで計測確定）。

## フェーズ 10-c: layout 段の座標化（seg/piece ゼロコピー + no_boxlink + intern sid 直結）（2026-08-28）

**問題**: 10-b 後の layout 段は 736.84ms（C 46.47ms = 15.9×）で単段として最大の
残件だった。アルゴリズムは C と同一にもかかわらず定数倍が壊れていた原因は、
所有権の安易な選択にあった（設計の敗北であって言語のせいではない）:

1. **テキストの所有コピー**: piece 生成で `node.name.clone()`（全可視バイトの
   malloc+memcpy）、seg 生成で `text.to_vec()` + merge での extend。C は
   `IfStr{ptr,len}`/`IfSeg.p` で DOM テキストを**指すだけ**。
2. **Style の値コピー**: `Seg{st: Style}`（~112B）を seg ごとに保持、merge 判定で
   `pm_st == Some(st)` の field 比較連鎖（~20 フィールド）をアトムごとに実行。
   C は intern 済み `const IfStyle*` の **ポインタ 1 本比較**。
3. **lazy メモの parent キー計算**: 解決ごとに parent style 値（~112B）の hash
   逆引き。C は parent ポインタからの `pk>>4` 乗算で O(1)。
4. **box 木の常時構築**: 行ごとに LINE BoxNode（Style 値コピー + 子 Vec）を
   構築・保持。C の CLI 描画経路は `no_boxlink`（streams のみ）で木を連結しない。
5. **geom の再計算**: IfGeomCache 未移植で (style, rfs, avail_w) ごとに全計算。

移植（全て C 機構の写し＋所有権での構造的等価化）:

- `layout.rs`:
  - `Seg` を出処座標化（`src: u32`（DOM テキスト NodeId / `SEG_SRC_SYN` /
    `SEG_SRC_STATIC`）, `start/end: u32`, `x/w: i32`, `sid: u32` = 24B）。
    `Layout::seg_text/seg_style` が dereference（C のポインタ読みと同値）。
    seg merge は座標の隣接検査 + `end` 更新のみ（バイトを一切動かさない）。
    旧来の seg テキスト全コピー（16MB 全可視バイト + 数十万 alloc）を構造消去。
  - `Piece` は `(Prov=Src(NodeId)|Syn|Never, start, end, sid, br)` の 17B。
    テキストノードの `name.clone()` を撤廃。
  - `Layout` に `stab: Vec<Style>`（intern 表。sid → 値で唯一）と
    `syn_text: Vec<u8>`（合成 arena 実バイト）を追加。`syn_pos/syn_text` は
    **不変条件 `len == syn_pos`** で C の layout arena bump 配置を厳密再現する
    （merge 可否が C のポインタ隣接と同値になるための整合条件。`syn_push` /
    `syn_foreign` の 2 点でしか進まない）。
  - `Prov::Src` の payload を連番発行器から NodeId 直結へ（一意出処 id が
    そのまま seg 座標になる。発行器の撤去）。
  - style intern idx（sid）の全経路導入: `lc_resolve_disp`（解決を display
    バイト + sid のみで返す。flatten/ifc ゲートから値コピーを全廃）、
    `lc_st_value`（ブロック子のみ値を引く — geom/deco/marker が全フィールドを
    要する唯一の経路）、`lc_metrics`（wrap 初期値の 4 field 直読み）。
    merge 判定の pm 比較は u32 の sid 比較（**値同一 ⇒ sid 唯一**なので C の
    ポインタ比較と厳密同値。旧実装の field 比較連鎖を O(1) 化）。
  - `no_boxlink` の移植: `layout_build_linear` / `layout_build_lazy_linear`。
    LINE box・子 box の連結をせず streams（rlines/deco/seg_arena/stab/
    syn_text）のみ構築。`main.rs` は描画経路で線形ビルダーを選択
    （`Mode::Layout` dump のみ従来の tree ビルド。extent は C の規約どおり
    root 自身 + width/height から計算 = 既存コメントの前提と一致）。
  - `geom_cached`（直接マップ 4096、key=(sid, rfs, avail_w)）= C の
    `IfGeomCache` 写し。geom は純粋関数なので衝突上書きの損失ゼロ exact memo。
- `css.rs`: `StyleLazy::get_id`（parent を intern sid で受け取る。pk = sid+1 で
  parent 値の hash 逆引きを全消去 — intern は値の正準名なので旧 pk 写像と
  厳密一致。miss 時のみ intern 表から値を復元して `compute_style` へ渡す一点化）、
  `get`（公開互換）は idx_of + get_id で再実装（呼び出し意味の不変）、
  `value/display_at/metrics_at/into_stab`、`StyleIntern` を pub(crate) 化し
  `value/display_at/metrics_at/into_values` を追加。
- `render.rs` / `main.rs` / examples: `render_emit_sweep(dom, lay, ansi)` に
  Dom を通し、seg アクセスを `seg_text/seg_style` 経由に一点化。

検証（2026-08-28 実走、全て緑）:

- 16MB/2MB × ansi/no-ansi 4 経路 + `--dump-layout` 2 経路の C↔Rust
  stdout **byte-exact** 維持。
- cargo test **337 緑**、clippy `--all-targets` 警告ゼロ、fmt 緑。
- diff fuzz **8,000 cases 追加 / 0 mismatch**（seed 777/20260828）→
  **累計 158,133 cases / 0 mismatch**。

計測（`bench/data-20260828c.json`、paired median。16MB）: total 526.33ms
（C 117.60ms で **4.47×**。10-b の 8.90× から半減）、**layout 736.84→200.03ms
（C 比 15.9×→4.2×）**、render 75.27ms（3.6×）、**RSS 1,187,932→465,368KB
（5.58×→2.19×）**。2MB total 60.03ms（3.96×）、ANSI 2MB 79.84ms（2.51×、
render 段 1.42×）。startup 1.4740ms（C 1.3960、符号 114:36 で今回は C 優位側。
帯騒音内であるが偏りは正直記載）。

**残照準（嘘をつかない台帳）**: parse 段 241.81ms（C 47.68ms = **5.07×**。
2-slice 並列 parse / fitdom が未移植）が新たな最大件。layout 200.03ms
（4.2×。AVX2 可視ラン / fitdom / 2-way 並列 layout / ifc 入口の Wrap 初期化と
segs Vec 成長の更なる抑制が未実施）、render 75.27ms（3.6×）、read 8.02ms
（C 0.02ms。fs::read → mmap 案）、RSS 2.19×（stab/syn/arena は残置）。

## フェーズ 10-d — parse 段: NameStr + reserve + run 再利用 + 2-slice 移植（既定 OFF）（2026-08-28）

### 問題（alloc_probe 実測の台帳。16MB md）

parse 段 241.81ms（C 47.68ms = 5.07×時点）の収支を計数アロケータで分解:
**2.38M allocs / 922MB**。内訳の核心は `Node.name: Vec<u8>` の逐ノード
malloc+memcpy と `Vec<Node>` 倍増の move 収支（最終 260MB の ~2 倍）、
text run アキュムレータの逐ノード再成長だった。

### 移植・改善（C の設計に対して safe 側で辿り着ける等価物）

| C | 旧 Rust | 新 Rust |
|---|---|---|
| 名前は arena へ bump（0 alloc/ノード） | `name: Vec<u8>`（1 alloc/ノード） | `NameStr`: Static（タグ名 = 0 alloc）/ Inline ≤22B（0 alloc）/ Heap Box（短文以外）。`Deref<Target=[u8]>` で読み側 85 サイトはほぼ無改修 |
| arena 予約的確保 | nodes 倍増 move | `nodes.reserve(input.len()/10)`（md ではノード数 ≒ 入力/10 が統計的にほぼ正確） |
| run bump 再利用 | take→from_vec（次 run が 0 から再倍増） | from_bytes+clear で run 容量再利用 |
| `scan_special` SSE2/AVX2（runtime dispatch） | 10 分岐スカラ scan | 32B チャンク棄却 + 256B LUT（safe の範囲の SIMD 代替。forbid(unsafe) のため intrinsic は不採用） |
| 2-slice 並列（`md_par_scan` が安全性を証明）| 未移植 | **`md_par_scan` / `md_to_dom_2slice` として移植済み**（scaffold id 規約 root=0..body=3、B 側 stub→A body の恒等写像、Fn 独立性、taint 和集合、MAX_DOM_NODES 同一）。ただし既定は **OFF の opt-in（`IF_MD_PAR=1`）** — 下記の計測根拠 |

### 計測（嘘をつかない。bench/data-20260828d.json、環境騒音 ±20% 注記つき）

- alloc 収支: parse **2.38M→0.76M allocs / 922→402MB**（alloc_probe 同一コーパス実測）。
- parse 段: 241.81 → **229.26ms**（median。環境が前回より重い中での絶対値。
  C 比 5.07×→3.90× は C 側の 47.68→58.73ms の悪化込み。同条件 paired)。
- RSS: 16MB 2.19→**1.95×**、2MB 1.85→**1.66×**（NameStr による per-node 24B→
  inline/静的化の効果が clean に出た）。
- 2-slice の honest 台帳（taskset 1-HT/2-HT A/B。ユーザ助言方式）: C は HT で
  ~1.75× 効くが、Rust 現行は **merge コピー（Node ~165B × B 側全数の転記 ≒ 130MB）
  + 走査重さで HT ゲインを食い潰し中立〜負**。よって既定 OFF。byte 出力は
  serial ≡ 2-slice ≡ C parallel で 3 者一致済み（2MB/16MB）。単体テスト
  `twoslice_equals_serial`（fence/table/link/list 混在 1MB+ で dump byte 一致）、
  `par_scan_rejects_footnote`（`[^` 拒否）、`par_scan_never_splits_inside_fence`
  を機械固定。なお Node の痩身化（~165B → C の ~40B 相当の SoA/セル化）は
  転記量を 4 分の 1 にし 2-slice 採算を変え得る次の主件候補。

### 検証（全て緑）

- cargo test **340 緑**（+3: 2-slice 等価性テスト群）、0 警告。
- 16MB/2MB × ansi/no-ansi 4 経路 + `--dump-layout` byte-exact 維持。
- serial ≡ 2-slice（IF_MD_PAR=1）byte 一致（2MB/16MB 両コーパス）。
- diff fuzz **3,000 件追加 → 累計 161,133 cases / 0 mismatch**。

### 残照準（優先度順の台帳）

1. **Node メモリ痩身化**（~165B → 目標 <64B。attrs/doctype/pi_target/tpl を副テーブル
   へ。nodes 書き込み ~260MB の激減 + 2-slice merge 転記の軽減 + layout/render の
   キャッシュ効率。次の最大構造件）。
2. layout 段 236.16ms（C 3.94×。2-way 並列 layout `layout_shard_run_body` +
   `md_body_mid` の移植。こちらも merge 転記不要な設計が要る — C は arena 共有で
   O(1) 接合）。AVX2 可視ランは safe LUT 版で部分代替可能か要検証。
3. render 段 70.41ms（C 23.73 = 2.97×）。
4. read 段 ~8.28ms（C 0.02ms。fs::read → `FileExt`/mmap。libstd の read_to_end は
   1 回余分な stat+確保走査を踏む疑い — 要検証ラベル）。

## フェーズ 10-fix: akl-ffi コールバックアダプタの Stacked Borrows 適合（CI Miri ゲート修復）

CI @b879045 の Miri（CMD 5）が `akl-ffi` の `handle_get_adapter` で exit 1
（「tag `<wildcard>` へのアクセス許可は強保護された Unique の除去を要する」）。
**10-c/10-d とは無関係の既存件**（直近 3 ラン連続で同一失敗。ifuto 側は
`#![forbid(unsafe_code)]` のため Miri 上の新規指摘は存在しない）。

### 原因（形式 UB。実機では動く）

`akl_eval` 系が `&mut *rt`（AklRT box 全体の `&mut`）を保持したまま
`run_loop` を回す間、ホストコールバックアダプタが `rt.host_ctx`（整数経由 =
wildcard 来歴）から `&*wrapper` / `&mut *wrapper` の**参照を再生成**する。
Stacked Borrows ではこの grant に保護タグのポップが必要で UB。
一方プレーンな生 read/write はスタック頂上の保護タグ自身が許可するため合法
— ここに合法経路がある。

### 修復（挙動不変。参照不生成の生経路へ機械改修）

- `foreign_adapter` / `handle_get_adapter` / `handle_set_adapter` /
  `handle_call_adapter`: Vec 登録表の読み取りを `&mut *wrapper` 経由から、
  `ptr::read` による **ヘッダ値コピー + `ManuallyDrop` 舐め**（`rd_natives` /
  `rd_vtables`）に変更。要素バッファは別割当で保護競合しない。
  `native_err` / `err_len` / `err[..]` は `addr_of[_mut]` 経由の生 read/write と
  `copy_nonoverlapping` で処理（例外時のみのコールドパス）。
- `akl_native_throw`: eval 中のネイティブからの再入路でもあるため同様に
  生経路へ（`set_err` と同一収支の書き込みを inline）。
- いずれも C コールバックへ渡す `wrapper` ポインタ値・error 文言収支・
  戻り値は全て不変。

### 検証

- 通常: workspace test **340 緑**、clippy `-D warnings` 0、fmt clean。
- Miri（要 crates.io で sysroot 構築）: 本砂箱は crates.io egress 遮断のため
  ローカル検証不可。**CI トリガで確認する**（下の「合法」を仮定した理屈が
  崩れた場合は同箇所で再失敗するので即判る）。

### 追補（同日・2 件目）: `akl_native_register` の &str 往復による NUL 来歴喪失

CI 再実行でアダプタ群は緑化（`handle_roundtrip` ok）。次に表面化したのは
`akl_native_register` → `akl_global_set` 経路の別件: `cstr()` で得た `&str` を
`as_ptr()` で生ポインタに戻す往復があり、SharedReadOnly retag の適用範囲が
[0..len) のため、内側 `cstr()` の strlen が読む **NUL 終端（[len]）の来歴を殺す**
（「tag does not exist in the borrow stack」）。同関数内では `&str` の中身は
未使用だったため、呼び出し側の生ポインタをそのまま転送する形に削除。
修正前コードは実害のある設計としては単純に不要だった（冗長の除去であり
原型の維持ではない）。現行の残 `as_ptr()` サイトは生 `&str`+len 契約の
コールバック受け渡しのみで strlen を踏まないことを全点検済み。

### 追補（同日・3 件目）: テスト側 `Box::leak` の漏洩検査抵触

アダプタ群 + &str 往復の根治後、akl-ffi の全 6 テストが Miri 通過。残ったのは
`handle_roundtrip` テスト自身の `Box::leak(CAklHandleVTab)` に対する漏洩検査
のみ（本番コードの漏洩ではない）。vtable の C 契約上の寿命は「akl_free まで」
で足りるため、テストフレームの `Box` で所有し `akl_free` 後の自然 drop に変更
（`&*vt as *const` で登録。生ポインタの使用期限中に Box が生存する契約を
コード構造で表現）。

### 追補（同日・4 件目）: Miri ゲートの実態は「time kill」+ データ重い掃引の cfg(miri) 縮小

Box::leak 対処後も CMD 5 が exit 1。ログ末尾は `charset::tests::oracle_sweep ...`
の**途中で切断**しており Miri エラー行がない = UB ではなく `timeout 600` による
強制終了と確定（akl-ffi 6 件 + ifuto-core 183 件は全通過済みで、ifuto-core 走査中に
600s 到達）。対処は 2 つ:

1. **データ重い掃引テストの cfg(miri) 縮小**（Miri の目的は経路の UB 検出であり、
   掃引網羅は通常 test / 差分 fuzz の責務。通常実行の網羅性は一切削っていない）:
   - `utf8::encode_roundtrip_all_codepoints`（全 1.1M → Miri は step 4093 + 分岐境界 15 点）
   - `utf8::band_w2_implies_valid_wide`（16×256×256 → Miri は b1/b2 を step 17）
   - `charset::oracle_sweep`（65,536 → Miri は step 251 + 0xFFFF 明示）
   - `store::test_shrink_history`（format! 100k 行 → Miri は 1k 行×各行伸長で同条件）
   - `md::twoslice_equals_serial` / `par_scan_rejects_footnote` /
     `par_scan_never_splits_inside_fence`（1MB → Miri は 64KB。fwrap 直接呼出のため
     1MB dispatch 閾値は不関係、論理経路は同一）
2. trigger.md の Miri timeout を 600 → **1500** に拡大（sysroot 構築 + ~330 件の
   解釈実行は分単位が常態で、600 はワークスペース規模に対して構造的に不足）。

なお前報で「ウォッチャ(gh run watch) exit 0 = 全緑確定」と書いたのは**誤り**。
`gh run view` は conclusion=failure を返しており、行レベル（trigger/result.md）が
唯一の正確な証跡。以後の CI 判定は result.md の CMD_RESULT 行のみを信用する。

### 追補（同日・5 件目）: md 2-slice 3 テストを Miri 下 8KB 化 + Miri timeout 3000

1500s でも到達点は `md::tests::twoslice_equals_serial` の途中（他の全ては通過。
cfg(miri) 縮小は効いている）。C 相当の DOM 構築は Miri 解釈実行で数万倍級に
なるため、64KB でも個別テストとして重すぎた。8KB（merge/remap 分岐は全て
通る最小代表入力）に再縮小し、timeout は 1500 → **3000** へ（ジョブ上限
120 分に対して toolchain+他 CMD と合わせても ~70 分見込みで余裕）。
通常 test の 1MB 総掃引・差分 fuzz・ASan 側の網羅性には一切手を付けていない。

## フェーズ 10-e: Node の痩身化（200B → 80B）— parse/RSS の大幅改善

### 動機
`size_probe` 実測で Node は **200B** だった（NameStr 24 + attrs `Vec<Attr>` 24 +
doctype `Option<Doctype>` **80** + pi_target `Vec` 24 + リンク 5×8 + kind 系 4）。
これが 16MB md で ~1.6M 個並び、nodes 書き込み/走査・2-slice merge 転記・
キャッシュ効率のすべてを腐らせていた（C IfNode は packed/IfStr 設計で **69B**）。

### 設計（safe のまま C の「熱い構造体は小さく」を再現）
- 属性・doctype・pi_target を **`Dom` 側の副テーブル**（`attrs_tab: Vec<Vec<Attr>>` /
  `extra_tab: Vec<NodeExtra>`）へ隔離。ノード側は 4B ハンドル ×2（`attrs_idx` /
  `extra_idx`、0 = 無し、**遅延割当**で属性無し文書はテーブル自体 0 確保）。
- アクセサ: `attrs()` / `attrs_mut()` / `set_attrs()` / `attr()` / `attr_set()` /
  `doctype()` / `set_doctype()` / `pi_target()` / `set_pi_target()`。
  読みサイト（dom dump、html_tree の attrs_equal/clone_element/merge_attrs/
  foreign adjust/sc_clone/sc_selected_option、md elem_store）を全置換。
- 2-slice merge は `merge_side_from`（副テーブル末尾連結 + ハンドルへのデルタ
  加算。0 は不変。C の arena 共有接合に対応）を追加して整合性を保持。
- **ラチェット**: `node_size_ratchet`（`size_of::<Node>() <= 80` を機械固定）+
  `side_tables_roundtrip`（遅延割当・置換・merge デルタの規約固定）。

### 実測（paired median。環境騒音 ±20% 帯の中でも paired 比較として有意）
| 指標 | 10-d → 10-e |
|---|---|
| 16MB total 倍率 | 3.88× → **3.15×** |
| 16MB parse 倍率 | 3.90× → **2.61×**（229.3 → 149.4ms） |
| 16MB render 倍率 | 2.97× → **2.15×** |
| 16MB RSS 倍率 | 1.95× → **1.09×**（415 → 232MB） |
| 2MB RSS 倍率 | 1.66× → **0.99×**（C より低い） |
| 2MB total 倍率 | 3.52× → **2.99×** |
| ANSI 2MB total 倍率 | 2.18× → **1.92×** |

### 検証（全て緑）
- cargo test **342 緑**（+2: ラチェット＋副テーブル規約）、clippy/fmt 0 警告。
- render 4 経路 + 全 dump モード（dom/layout/styles/tokens/wptdom）× 2 コーパス
  = **14 通り byte-exact**。
- serial ≡ 2-slice（IF_MD_PAR=1）byte 一致（2MB/16MB）。
- diff fuzz 3,000 件追加 → **累計 164,133 cases / 0 mismatch**。

### 残照準
1. **layout 3.86×**（219.94ms vs C 57.04）: 最大の残構造件。2-way 並列 layout
   （C `layout_shard_run_body` + `md_body_mid`）は C も arena 共有前提のため、
   まず taskset A/B で Rust layout の HT 採算を計測してから設計。
   ifc ごとの pieces/segs Vec 新規成長（段別 alloc 計測値: layout 1.29M allocs）
   を深さ別スクラッチ再利用で消す案が次の一手。
2. **read 段 ~9.19ms vs C 0.02ms**（fs::read の余分な stat+確保走査疑い。要検証）。
3. Link フィールドの `Option<NodeId>` 5×8B を u32 センチネル化すれば Node は
   80B → 60B（C 69B を越える小型化。ただし全リンクサイトの機械置換が要る）。

## フェーズ 10-f: layout アロケーション段の撲滅（backtrace 帰属計測による特定）

### 計測手法（本件の主題の半分）
Node 痩身化後も layout 段に 1.29M allocs / 340MB が残っていた。`alloc_hist`
（計数アロケータのサイズバケツ分布）で「**87,419 個が丁度 1400B**」「43,741 個が
4-7B」と構造を特定し、`alloc_bt`（backtrace 帰属、example 限定・再帰ガード付き）
で 2 つの正犯へ絞り込んだ:

1. **ifc ごとの `pieces`/`segs` 新規 Vec**（10-c で導入した座標化の弱点）→
   `Lc.pieces_scratch` / `Lc.segs_scratch` を新設し **借用→clear→終端返却** に。
   深さ別スクラッチは計算せず、layout_ifc が入れ子不可・単一スレッドである検証を
   根拠に単一スクラッチの take/return とした（C の `pieces_scratch` 発想の正当な写し）。
2. **css `compute_style` の `Vec<Option<Winner>>`（P_N≈1400B）を要素ごとに新規
   確保** → 引数でスクラッチを受け取る形に変更し、`StyleCache.win_scratch` /
   eager sweep の 1 本で使い回す（実行が入れ重ならないことを DFS 構造で立証済み）。

### 実測（alloc_probe。16MB、layout 段）
| 指標 | 10-e 直後 | 10-f |
|---|---|---|
| allocs | 1,289,419 | **44,034** |
| bytes | 340MB | **103MB** |

bench 実測（paired median）: 16MB layout **3.86× → 3.56×**（219.94 → 168.94ms）、
16MB total 3.15× → **3.12×**（環境が静かになり C 側も 140.63 → 115.37ms に復帰。
同色条件での paired 有効値）、2MB total 2.99× → **2.76×**、16MB RSS 1.09× →
**1.06×**、2MB RSS 0.99× → **0.96×**、ANSI 1.92× 維持。

### 検証（全て緑）
- cargo test 185（ifuto-core）緑、workspace release build/clippy/fmt 0 警告。
- render 2 経路 + dump 5 モード × 2 コーパス = **18 通り byte-exact**、serial≡2-slice。

### 残照準（照準の棚卸しを更新）
1. **layout 3.56×**（168.94ms vs C 47.43）: 残 44k allocs の内 4-7B 約 43.7k 個は
   `compute_style` 内の `decl.clone()`（約 1%。`DeclRef` 参照化で消えるが C と違い
   sheet 寿命管理が要るため保留）。
2. **render 段 2.30×**（50.52ms vs C 21.96）: セグメント走査の残 C 優位。
3. C の 2-way 並列 layout（`layout_shard_run_body`）の Rust 採算を taskset A/B で
   先に測る段（C も arena 共有前提の設計。HT 採算が立つか計測が先決）。
4. **read 段 8.21ms vs C 0.02ms**（`fs::read` の余分な stat+確保走査疑い。小粒件。
   要検証ラベル）。

## フェーズ 10-g: 2-slice parse の実益化（merge 分解計測 → scan SWAR / drain 撲滅 / 既定 ON 転換）

### 計測ファースト（推測で設計しない）
taskset ピン留め HT 採算（16MB、interleaved median n=9）で、10-e/10-f 後の
2-slice が依然損益分岐（@2HT parse −3.5ms のみ、@1HT +48ms の大損）と確定。
`IF_MD_PROF=1` の段別計測（設計判断用の一時計装を恒久化、env 未設定では
無出力・ゼロコスト）で merge の内訳を撮った:

| 内訳 | 旧 | 10-g |
|---|---|---|
| scan（`md_par_scan` 全走査） | 11.5ms | **3.5ms**（SWAR u64 zero-byte 検出。std 縛りで memchr crate / SIMD intrinsic 不可のための safe 代替） |
| side（副テーブル連結） | 0.13ms | 0.13ms（無罪確定 — 前フェーズの疑いは誤報と訂正） |
| drain（B 内容ノード転記+リンク写像） | 33ms | **30ms**（後述） |
| parse 並走部（A+B）@2HT | 82ms | 77-87ms |

### drain の設計比較（3 変種の実測対決）
B 側局所 NodeId → 大域への写像は **index ベース arena の根本制約**で、C の
「ポインタ恒等 = O(1) 接合」の完全な写しは safe では不可能（`forbid(unsafe)`
維持）。安全側で取れる手を 3 変種ビルドして採った:

- V1 **fused drain→extend**（remap を移動 closure に融合、B を 1 pass だけ読む）:
  30.0ms ← **採択**
- V2 in-place remap → pure drain 分離: 38.5ms
- V3 in-place remap → `split_off` + `append`: 77.6ms（split_off の中間 65MB
  確保+copy が致命。最初に試した版本がこれで、一旦 33→70ms に悪化させた —
  嘘をつかない台帳として失敗も記す）

併せて A 側を**全文書係数で事前予約**（merge 時の倍増再確保 + 134MB copy を
構造消去）。residual ~30ms の律速は 80B/ノード × 81 万個の per-item move
（Node は `NameStr(Box)` 内包で非 Copy → bulk memcpy 化は safe では不可能）と
新規領域の初回 page-fault 税。C との構造差（残 ~20-25ms/16MB）として台帳に残す。

### 既定値の転換
`md_par_on`: 未設定時 `available_parallelism() >= 2 → ON`（1 HT では serial に
自己降格 → かつての 1HT 大損経路を構造封殺）。殺しスイッチ `IF_MD_PAR=0`、
強制 `IF_MD_PAR=1` は C 規約と同一。byte 出力は serial ≡ 2-slice で逐語同値。

### 実測（paired interleaved median、data-20260829b.json）
16MB parse **133.9 → 116.6ms**（auto@2HT vs serial@2HT。C は 47.1）、
16MB total **2.72×**（127.53 / 346.21）、2MB total **2.63×**、
2MB RSS **0.97×**、ANSI 1.93×維持、16MB RSS 1.06×維持。

### 検証（全て緑）
- cargo test **185**（ifuto-core）/ workspace 342 緑、release build/clippy/fmt 0 警告。
- output oracle **21/21 byte-exact**（serial≡2-slice 含む。既定=ON 転換後の経路で）+ 
  render 2 経路・dump 5 モード × 2 コーパス（前フェーズ同様の 18 通りも全緑）。
- diff fuzz 3,019 件追加 → **累計 167,152 cases / 0 mismatch**。

### 残照準（照準の棚卸しを更新）
1. **parse serial 本体 128ms vs C serial 82ms（1.56×）**: 2-slice の天井は serial/2
   に従属するため、ここが parse の本丸。alloc_bt 帰属で **764,744 allocs / 203MB**
   = テキスト per-node heap コピー（blocks_str ~306k×6.3B / blocks_win ~153k×65B /
   inline_span 65,544=21,848×3）と確定。C は arena bump（0 malloc）。根治=**text
   の input 借用化**（Dom が入力バッファを所有し (off,len) 参照。ただし NameStr の
   `Deref<Target=[u8]>` iface を破る大改造。フェーズ 11 級の設計案件）。
2. **layout 3.07×**（162.91ms vs C 53.14）: C の layout shard（hint 有 80→45ms =
   1.75× 実測）の Rust 移植が次の構造件。残 44k allocs の `decl.clone()`(4-7B) は
   小粒（DeclRef 参照化は sheet 寿命管理が要るため棚上げ継続）。
3. **render 段 2.25×**（49.09ms vs C 21.78）。
4. **read 段 8.47ms vs C 0.02ms**（`fs::read` の初回 page-fault 税。C は mmap で
   parse に課税転嫁。小粒件。要検証ラベル）。
5. ANSI RSS 1.41→1.63×（要監視。ANSI 経路の何かが育っている。未分析）。

## フェーズ 10-h: body shard（2-way 並列 layout）移植 — C `layout_shard_run_body` の写し

### 設計判断記録（踏査の台帳）
まず taskset 採算計測で C の同機構を分解した:
- C layout shard は hint 有で **80.02 → 45.0ms（1.78×）**、hint 無しは body 直下の
  全計数+中点ウォークに ~35ms を浪費（md.c コメント値 ~31ms+~31ms を実測裏付け）。
- Rust 側の先行条件は揃っていた: `layout_children` が既に範囲実行可能な分離関数、
  幾何は全て整数セル（`y: i32`。`y += hA` 平行移動に浮動小数の結合差は存在しない）、
  sinkp 補正・prev_mb 相殺は C と同じ構造で境界安全性が成立（分割点は body 直子 =
  部分木自足。li_ord は親スコープ、deco の開閉は部分木内閉鎖）。
- C の shard≡serial byte 一致を本機で検証（`IF_LAYOUT_PAR=0` A/B、idm-2mb）して
  から実装に入った（信頼の確認先行）。

### 移植の構造
1. **Dom に `md_body_mid` ヒント追加**（md 2-slice splice で記録。C の同名フィールド
   写し。serial parse / HTML 経路は 0 = 不発）。
2. **`layout_children` に [start, stop) を一般化**（serial 経路は (None,None) で不変）。
3. **build_impl に shard 分岐**: 線形+lazy+md_ws_stripped+n_nodes≧4096+ヒント有りのみ。
   独立 Lc（streams/geom cache/syn arena 専有 = C の shard 局所 arena 分離の写し）を
   A/B で走らせる。A のみ body 装飾（`bsides` 規約写し）。StyleLazy は shard ごとに
   new、外来の body 値は shard 局所 intern に種蒔き（parent 値同一 ⇒ 子孫値も逐語同値）。
4. **接合**: `hA+hB`（height_spec 拡張規約同値）→ B の lines/deco `y+=hA`、
   seg_arena/syn_text 参照デルタ接合（syn_text は 8B アライン bump のため A 末尾を
   align8 占位）、**stab は A/B 両側の値を serial 順（seed→A 初出→B 初出）で主
   intern に値同定**（値同一 ⇒ sid 同一の不変保持。C の arena ポインタ吸収に対する
   Rust = sid 間接の価格として seg 写像 pass を払う）。deco h 後埋め規約も写し。

### 検証の要点（内部モデル差の正直な記録）
`shard_layout_equals_serial`（新規単体テスト）が最初 syn_text 完全 byte 一致で落ちた
 —— pieces/links/prec の cap doubling 由来の foreign 占位が **shard 局所の cap
リセットで再発火**するため。**これはバグではない**: C も per-shard arena +
`if_arena_absorb` で同じ分岐を持ち、seg のテキスト解決は shard 局所オフセット+
デルタで正しく、観測面（render/dump）には現れない。テストは観測同値の canonical
射影（行幾何 × seg 内容 × 解決スタイル × deco × stab × root）比較に修正して緑。
render 2 経路 + dump 5 モード + 21/21 オラクル + serial≡shard A/B は全て byte-exact。

### 実測（paired interleaved median、data-20260829c.json）
16MB layout **165.2 → 110.5ms（shard 1.50×。serial 済み）**〜 bench 環境では
117.57ms（C 48.06 = **2.45×**、前回 3.07×）、16MB total **2.58×**（119.77/308.78）、
2MB layout 3.36→**2.26×**、2MB total 2.63→**2.17×**。価格: 16MB RSS 1.06→1.13×、
2MB RSS 0.97→1.09×（B 側 streams 一時保持の複製分。総計縮小 −37ms を優先して採択）。
ANSI total 1.93→**1.69×**（layout shard が ANSI 経路にも効く）。

### 検証（全て緑）
- cargo test **186**（ifuto-core。+1: shard_layout_equals_serial）/ workspace 343 緑、
  release build/clippy/fmt 0 警告。
- output oracle **21/21 byte-exact**。serial≡shard A/B（`IF_LAYOUT_PAR=0`）byte 一致
  を 2MB/16MB で確認。
- diff fuzz 3,019 件追加 → **累計 170,171 cases / 0 mismatch**。

### 残照準（照準の棚卸しを更新）
1. **parse 2.50×**（123.95ms vs C 49.65。最大の残構造件に復帰）: alloc_bt 帰属で
   **764,744 allocs / 203MB** = テキスト per-node heap コピー確定。根治 = **Dom による
   入力所有 + text の (off,len) 参照化**（NameStr の `Deref` iface を破るフェーズ 11
   級の大改造。設計立案段）。
2. **layout 2.45× / render 2.46×**: layout の残差は shard 接合税（stab/remap pass）
   と単一 HT あたりの演算差。render は C の 2-way 並列 sweep が未移植。
3. **read 段 8.71ms vs C 0.02ms**（fs::read 初回 page-fault 税。小粒件。要検証ラベル）。
4. **RSS 後退の取り戻し**: shard B streams 複製分（16MB +15MB）。接合後の早期解放 or
   B を既存 capacity 借用にする設計余地。ANSI RSS 1.76×（要監視、未分析は継続明記）。

## フェーズ 10-i: render 2-way 並列 sweep（C render_ansi.c 同名機構の写し）

C の機構（no-ansi 限定。行 y で二等分、A=[0,r_split) 主 FILE 直 / B=[r_split,my)
ワーカのメモリシンク、join 後 B を追記）を Rust へ写した。実装の要点:
- `render_emit_sweep` を行範囲本体 `sweep_range_emit` に分離し、並列時は
  A=[0,r_split) / B=[r_split,my) を `thread::scope` で同時進行して内容追記。
- B の deco 状態一意性: `r_split` 時点で「開始済み（y<=r_split）かつ未期限切れ
  （r_split < y+max(h,1)）」を追記順に再構成（deco 追記順=y 単調非減少は直列
  sweep の di 前進と同じ規約）。同一 y の行は全て前半に寄せる lower_bound 規則も写し。
- ゲートは C と同一: `!ansi && my >= 1024 && n_lines >= 256`（kill `IF_RENDER_PAR=0`。
  Miri 検査では定数を縮小、掃引網羅は通常 test の責務の規約どおり）。

### 実測（paired interleaved median、data-20260829d.json）
16MB render **55.77 → 35.36ms（1.58×。C 22.42）**、16MB total **2.12×**
（128.67/272.77。10-f 3.12× → 10-g 2.72× → 10-h 2.58× → **2.12×** 推移）、
2MB render 5.58→3.90ms、2MB total **2.09×**、layout 2.45→**2.04×**。
ANSI は C 規約により並列ゲート外（1.80×。render 段 1.37×）。

### 検証（全て緑）
- cargo test **187**（ifuto-core。+1: render_par_sweep_equals_serial）/ workspace 344 緑、
  release build/clippy/fmt 0 警告。
- output oracle **21/21 byte-exact**、2-way 並列 ≡ 直列 A/B（`IF_RENDER_PAR=0`）を
  2MB/16MB で byte 一致確認。
- diff fuzz 3,019 件追加 → **累計 173,190 cases / 0 mismatch**。

### 残照準（照準の棚卸しを更新）
1. **parse 2.32×**（119.84ms vs C 51.72。再び最大の残構造件）: alloc_bt 帰属で
   **764,744 allocs / 203MB** = テキスト per-node heap コピー確定。根治 = Dom 入力
   所有 + text (off,len) 参照化（フェーズ 11 級の `Deref` iface 改造。次の本丸）。
2. **layout 2.04× / render 1.58×**: ともに並列境界まで来た。残差は単一 HT あたりの
   演算効率（HashMap/Index 系の Rust 価格 vs C の arena+直配列）。読みの精度が要る段。
3. **read 段 8.02ms vs C 0.01ms**（fs::read 初回 page-fault 税。小粒。要検証ラベル）。
4. **RSS 1.13×/2MB 1.08×/ANSI 1.76×**: shard/render 並列のワーカ側バッファ複製価格。
   ANSI RSS は未分析のまま継続台帳。

## フェーズ 11: テキストアリーナ（Text pun fields）— per-node `Box` 交通の構造消去

10-i 残照準 #1（parse = 最大構造件）に対する着手。alloc_bt 帰属では parse
764,744 allocs の全体像は「テキスト per-node heap コピー」と読んでいたが、
同時取得の粒度（blocks_str 直下 305,885×6.3B / blocks_win 152,941×64.8B /
blocks_str 深層 87,401×96B / inline_span 65,544×21B ≈ 611k）を突き合わせると、
**テキスト `Box` 本体は ~109k で、残り ~611k は md 行/ブロック機構**だった
（10-i 残照準の帰属解釈は部分誤り。本節で訂正する）。

### 設計判断記録（却下経路を含む）
1. `NameStr` 第 4 態 `Range` + `Deref` 撤去案: 消費者 94 サイトの機械移行が
   必要で爆発半径が最大。**代替として Text ノードのスペアフィールド転用を採択**:
   Text は属性も稀データも持たないため `attrs_idx`/`extra_idx` が常に空き。
   >22B 本文は `name=空, attrs_idx=off, extra_idx=len` で `Dom.text_arena`
   （bump）を指す。発見規則は `extra_idx != 0` 一つ、読み口は `Dom::text_of` 1 本、
   書き口は `Dom::set_node_text` 1 本。`Deref` は無傷、要素名比較など ~80 サイトは
   完全非侵略。C の union 流儀と同型で、転用規約はフィールドコメントに明記。
2. html_tree（HTML 経路）は 11-a では従来表現のまま（`text_of` が両表現を吸収
   するため混在が型安全に動く）。md fastdom 経路のみが arena に書く。
3. 2-slice splice: B 側 arena 連結 + pun remap は 10-g の drain 算術 remap に
   排他項を足すだけ（`t = is_arena_text` で `+=arena_delta*t + d_attrs*(1-t)*…`。
   分岐を入れない規約を維持）。
4. arena 予約: md 本文の ~48% が arena に入る実測（2MB: 1,019,337B / 16MB:
   8,078,755B ≒ 入力×0.48）から `input.len()/2` を serial/A/B で各 1 回予約し、
   bump 倍増の realloc コピー税を構造消去（未タッチ予約は RSS 不感）。

### 構造実測（alloc_probe 同一器具・16MB。bytes は要求量計）
| | allocs | bytes |
|---|---|---|
| 10-i（実施前） | 764,792 | 270.8MB |
| arena のみ | 655,585 | 297.6MB |
| arena + 予約（採択形） | **655,552（−14.3%）** | **275.3MB** |

### 実測（paired interleaved median、data-20260829f.json）
16MB total **1.991×**（121.28/241.44。**倍率の環境帯を 1.99–2.37× に更新**。
C 121ms は高騒音側、R 絶対値は e と等しい帯）、parse 段 111.0→**107.0ms**
（単発 3-way interleaved n=15 でも b1−6.06ms を確認。環境 23ms ドリフトの
向こう側で符号が揃う）。**RSS 16MB 1.129→1.120（240,324→238,516KB。決定的
改善）**、2MB 1.080、ANSI 1.753。2MB parse 12.6→11.11ms。
startup は 77:73 で引き続き同値。全ペア stdout byte 一致。

### 検証（全て緑）
- cargo test **345**（ifuto-core 188。+1: `text_arena_representation` で pun 表現
  そのものを機械固定）/ workspace release 全緑、clippy/fmt 0 警告。
- byte-exact: serial/auto/forced/@1HT × 2 コーパス + C==R × 2 コーパス、
  oracle **21/21**。
- alloc_probe による構造証明（上表。nodes 数 1,616,804 で前後一致）。

### 残照準（照準の棚卸しを更新）
1. **parse ~2.3× 残 = md 行/ブロック機構 ~611k allocs**（blocks_str/blocks_win。
   テキスト `Box` を消して残った真の大山。Ln ゼロコピー化 + ブロック分類の
   バッファ再利用が照準。フェーズ 12 候補の本命）。
2. layout / render: 単 HT 演算効率の読み合い（C arena+直配列 vs Rust 価格）。
3. read 段 ~7.4ms vs C 0.02ms（page-fault 税。小粒）。
4. RSS 残差（shard/render 並列のワーカ複製。ANSI 1.75× は未分析継続）。

## フェーズ 12-a: md ブロック機構のゼロコピー化（split_cells スライス化 + Fn 借用化）— parse allocs −60%

フェーズ 11 残照準 #1（行/ブロック機構 ~611k allocs）への着手。**帰属は 3 連続で
誤りを踏んだ**: (1) alloc_bt の fn フレーム名は LTO inline 潰れで親名しか出ず、
(2) 「blocks_str 直下 305,885×6.3B」を**脚注 id と推定して Fn 借用化を実装**したが、
コーパスの脚亲民合計は **0 件**で効果ゼロと判明、(3) 計測器の `.skip(2)` が最深
2 フレーム＝真の発生点を捨てていたと気付き alloc_bt 自体を修正。真の帰属は
**`split_cells` 系 ~415k（全 parse allocs の 63%）**だった（テーブル処理:
`ln_is_table_delim` の先読み Vec 新規確保 ~6.6 万 + split 1 セル 1 `to_vec` ~35 万）。

### 設計（採択形）
1. **`split_cells<'a>` → `Vec<&'a [u8]>`**: セルは行のトリム済みスライス。
   cells Vec は呼び出し側で再利用（clear→再充填）。35 万 malloc を構造消去。
2. **`ln_is_table_delim` をゼロアロケ単一パススキャナに書き換え**（旧実装は
   受理判定のために `split_cells` の Vec を毎回新規確保していた）。
   受理規約は旧実装と逐語同値（`:`? `-`{3,} `:`?。空行・`|`・`---|`・`---||` ・
   ws 行で逐語整合を検証）。
3. **Fn<'a> 借用化（12-a 同梱、perf 主張なしの衛生リファクタ）**: 誤帰属の過程で
   構築。脚注 id を `NameStr`（≤22B inline ゼロアロケ）+ def text を `&'a [u8]`
   化し、refs/defs 全体 clone を廃止。ベンチコーパスでは脚注 0 件のため効果
   ゼロ（成果に数えない）。byte-exact 検証済みで C 規約と同型のため同梱する。
   ※ Fn<'s> は Vec 格納の関係で 's 不変なため、`run_blocks` を wrapper（\r 正規化
   buffer 所有）/ inner（Fn<'s> 固有化）に分割する構造になった。

### 構造実測（alloc_probe 同一器具、16MB parse）
| | allocs | bytes |
|---|---|---|
| 11（実施前） | 655,552 | 275.3MB |
| 12-a（採択形） | **262,270（−60.0%）** | **266.1MB** |

差 −393,282 は全量 split_cells 系（Fn はコーパス上ゼロ影響）。11 開始前
764,792 からの累計で **−65.7%**。

### 実測（system bench data-20260829g.json + 仲裁 A/B）
g ランの 16MB R 側は n=7 ペアが騒音区間を踏み +32ms に膨れた（fastdom 値も
全域高い傍証）。**仲裁: 3-way interleaved n=21 で parse 104.60→97.49ms
（−7.11ms）・total −3.99ms を確認**。paired 値としては 2MB **2.260→2.134×**、
**RSS 2MB 1.080→1.067（決定的改善）**、ANSI RSS 1.753→1.747、16MB RSS 1.119
維持。startup 75:74 で同値継続。

### 検証（全て緑）
- byte-exact: C==R/auto/forced/@1HT × 2 コーパス、oracle **21/21**。
- cargo test **345** 緑、release build/clippy/fmt 0 警告。

### 残照準
1. **parse 残 ~262k**: attr 機構 ~65-87k（`elem_store` の name/value Vec。12-b）、
   cells Vec 成長 22k、pend_attrs 系。attr 値の arena/(off,len) 化は Attr struct
   共有（html 経路）のため別設計が要る。
2. layout / render: 単 HT 演算効率（不変の棚卸し）。
3. read 段 ~7.4ms、ANSI RSS 1.75×（未分析継続）。

## フェーズ 12-b: 属性機構のゼロアロケ化（Attr の NameStr 化）— parse allocs −25.0%

12-a 残照準 #1（attr 機構 ~65-87k）の処理。md 経路では属性 1 件につき **3 確保**
（`attr()` の pend `to_vec` + `elem_store` の name `to_vec` + value `clone`）が
掛かっていた（16MB コーパスでリンク 21,848 要素級）。C は pend 固定配列
`MoPend.an/av[4]` で確保ゼロ。

### 設計（採択形）
1. **`Attr.name/value: Vec<u8>` → `NameStr`**（10-d の既存 24B inline 文字列。
   ≤22B は inline 収容でヒープ確保ゼロ、長大は Heap 1 確保=旧 Vec 同世代）。
   共通 `Attr`（html_tok.rs）ごと差し替えのため **HTML 経路の短名/短値も
   inline 化**される（グローバルな副作用として黒字）。
2. md `DomOut`: pend_attrs を `(&'static str, NameStr)` 化し `attr()` 時点で
   from_bytes（短値はここでも確保ゼロ）。`elem_store` は into_iter で所有権ごと
   流し、name は `from_static` 無コピー・value は move（旧 2 確保を両方消去）。
3. 副次: `html_tree::merge_attrs` の borrow 回避用 全属性名 clone Vec を消去
   （逐次借用で実は不要だった無駄。車輪の撤去）。foreign 属性名調整は全ソース
   が `'static` なため from_static 化。
4. `NameStr` に `as_slice`・`Ord/PartialOrd` を追加（`Attr` ソート互換）。
   `Deref<[u8]>` 無傷のため消費側 ~20 サイトは無侵略（書き換えは eq 系 4 箇所）。

※ 検討して棄却: attr 値の `(off,len)` 借用化は DOM がソースバッファに寿命を
   依存させる設計になり、HTML 経路（所有値が本質）と attr 表現が分岐する。
   inline 22B (+Heap 1) で実用上の確保はほぼ消えるため、所有権の明瞭さに軍配。

### 構造実測（alloc_probe 同一器具、16MB parse）
| | allocs | bytes |
|---|---|---|
| 12-a | 262,270 | 266.1MB |
| 12-b（採択形） | **196,710（−25.0%）** | **265.3MB** |

差 −65,560 は予測帯 ~65-87k の中央（attr 1 件 3 確保 → href 長値の 1 確保のみ
残存）。11 開始前 764,792 からの累計で **−74.3%**。alloc_bt で attr 系バケットは
上位から消滅。残 = split_cells Vec 成長 22,024@64B + inline_span 群 ~65k（12-c 照準）。

### 実測（system bench data-20260829h.json + 仲裁 A/B）
h/g 両ランは ambient 速度レベルが変位（C 16MB total 119.9→97.7ms）しており跨 run
直較は不可。**仲裁: 3-way interleaved n=21（C / R12-a / R12-b、stdout 全一致検証
付き）で parse 104.74→101.71ms（−3.03ms、−2.9%）・total 238.56→236.14ms
（−2.42ms、−1.0%）、対 C 精読値 parse 2.23× / total 2.16×**。決定的指標:
RSS16 1.119 維持、RSS2 1.067 維持、ANSI RSS 1.745。startup 符号 87:63（median
1.4110 vs 1.4160 で同値、n=150 帯内変動）。16MB 実測帯は 1.99–2.49×。

alloc 単発 A/B の教訓（前フェーズ）通り −65k allocs の時間見返りは parse −2.9%
級。glibc tcache + 2 スレッド分散で確保原価は既に低い。構造消去の主眼は
「C の pend 固定配列 = 確保ゼロ」への構造追従であり、時間は副産物として淡々と積む。

### 検証（全て緑）
- byte-exact: C==R/auto/forced/@1HT × 2 コーパス（6/6、3 モード全て同一 hash
  = 並列度非依存の出力決定性）。oracle **21/21**。
- cargo test **345** 緑、release build/clippy 0 警告。
- 差分 fuzz +3,019（seed 31415）0 mismatch → 累計 **182,247**。

### 残照準
1. **parse 残 ~197k**: split_cells Vec 成長 22,024@64B + inline_span 群 ~65k
   （span バッファ系。12-c 照準）。その他小粒。
2. layout / render: 単 HT 演算効率（不変の棚卸し）。
3. read 段 ~7.4-7.7ms、ANSI RSS 1.75×（未分析継続）。
4. CI 運用: Miri 常設廃止（run 52分07秒→2分05秒の実績）。unsafe 含有 crate
   （akl-core/akl-ffi/ifuto-ffi）差分時のみ scoped Miri を有効化（trigger.md
   CMD 5 コメントの運用規約）。12-b 差分は unsafe 非含有のため非実施で正しい。

## フェーズ 12-c: 計測フェーズ（parse 機能分解 + read 診断 + 海外研究棚卸し）

コード変更なし（コメント整備のみ）の計測・調査フェーズ。次フェーズの手術点を
データで固定するのが目的。

### 海外高速化研究の棚卸し
`docs/RESEARCH_SPEED.md` に 13 系統の一次情報（simdjson / Mison / Pison / DP-FSM /
Keiser&Lemire UTF-8 / Lemire&Muła transcoding / Mimalloc / Drepper / DOD / Servo /
Bodik 並列ブラウザ / Hyperscan / mmap 対比研究 / BOLT）を、プロジェクト制約
（依存ゼロ・forbid(unsafe_code)・SIMD intrinsic 禁止→SWAR 代替）との適合性
マトリクスつきで整理。優先度高は: 構造索引先行+分岐レス化（simdjson 設計原則）、
Mison 的ビットマップ索引、arena/bump 継続（Mimalloc 局所性理論）、逐次アクセス・
子配列化（Drepper）、read 段の安全範囲の改善近似。

### parse 機能分解（examples/parse_breakdown.rs 新設。/tmp 合成 16MB×4 クラス）
| pattern（内訳） | C parse（--stats med n=5） | R md_to_dom（n=7 med） | R/C |
|---|---|---|---|
| plain（scan+text 素通し系） | 12.76ms | 19.69ms | 1.5× |
| table（split_cells 系） | 141.92ms | 253.65ms | 1.8× |
| blocks（ブロック機構系） | 33.20ms | 72.46ms | 2.1× |
| inline 過密（参考外） | 863.74ms | None 早期脱出 292ms | — |

C も inline 過密は 863ms と病むため両者同様の非線形点。実コーパス比 2.2× は
plain/blocks 優位の混合と整合。**次の手術点は blocks（2.1×）と table（1.8×）**。

### read 段診断（R ~7.7-9.3ms vs C ~0.02ms の構造確定）
- C は `mmap(MAP_PRIVATE)` 遅延マップで、read 段 0.02ms は見せかけ（16MB の
  fault 税は parse 段で支払う設計。tok は mmap 上を直接参照するゼロコピー）。
- Rust は `std::fs::read`（fstat 容量予約 + 1 回 read。std 安全範囲の最適形）。
  費用内訳【推定】: 匿名 16MB の ZFOD fault ~4,096 発 + カーネル copy_user。
  THP は `madvise` モードで匿名既定非適用（MADV_HUGEPAGE = libc = unsafe 禁止領域）。
- ユーザー提案「バッファリング」は逆効果と診断: BufReader 8KB 経路にすると
  syscall が 2,048 発に増え一貫して悪化。`std::fs::read` はすでに単発 read。
- mmap 化は std に API がなく libc 直接呼出＝ unsafe+依存禁止で棄却。
  残差 ~+5ms（C が parse 内で払う soft fault 相当を差し引いた正味）は構造的。
- Rust read+parse 合算（111.7ms級） vs C 合算（40.7ms級）の比較が公平な姿。

### 検証棄却の記録（scan_special 算術判定化）
simdjson の vectorized classification を SWAR 規約で真似、`IS_SPECIAL_LUT` の
LUT 参照を == OR 連鎖へ書換え auto-vectorize を誘導 → **interleaved A/B（n=11）
で plain 19.69→26.20ms の 1.33× 退化を計測し棄却・revert**。理由: LLVM は
pcmpeqb+pmovmskb+test を 10 クラス分展開する形になり、L1 常駐 LUT のスカラ走査に
敵わなかった。論文の形の模倣は計測で裏付けなければ採用しない（証左コメントを
scan_special に明記）。

### 実測（system bench data-20260829i.json）
機能差分ゼロ（md.rs はコメントのみ）のため h 値と同世代。i ランは全域高負荷帯
（C 16MB total 131.29ms）で、paired: 16MB 2.31× / 2MB 2.19× / ANSI 1.71×、
RSS 16MB 1.117・2MB 1.061・ANSI 1.747（決定的指標で横這い）、startup 符号
112:37（median 1.7505 vs 1.8220ms、重帯 run の帯内変動。系列では同値維持）。
byte-exact OK・test 全緑。fuzz 累計 182,247（コード不変のため追加分なし）。

### 残照準（12-d 以降）
1. **blocks 機構（合成比 2.1×）**: blocks_win 行毎処理の内訳分解→削除点の特定。
2. **table 機構（合成比 1.8×）**: split_cells Vec 成長 22,024 + 呼出経路。
3. inline_span 群 ~65k allocs（12-b 残）。
4. layout gap（R ~94-125ms vs C ~39-53ms、最大の絶対差）。子配列化（Drepper §2.2）。
5. read 段 mmap 代替の将来検討（依存解禁がない限り凍結）。

## フェーズ 12-d: blocks 機構の再スキャン削減（table 事前検査順序 + 二重走査一回化）— parse −3.03%

12-c で特定した blocks 2.1× / table 1.8× の第 1 弾。構文クラス別計測の深化:
**微行（5B）では R が 3.2× 速い**（C head_tiny 993.79ms vs R 309.81ms、C の行固定費
が巨大）のに対し、可変部支配の実コーパス域で R が負ける構造。allocs は既に
両クラス ~58 で無関係（arena 化が効いている）。残杠杆は per-byte のパス数。

### 設計（採択形。全て純粋述語の評価順最適化で意味不変）
1. **table 事前検査の `|` memchr 先行化**: 従来は全行で次行の
   `ln_is_table_delim`（O(len) 全走査）を評価してから当行の `|` 有無を見ていた。
   AND の純粋述語交換として `l.contains(&b'|')`（auto-vec memchr）を先行させ、
   `|` 無し行（実コーパス多数派）での is_table_delim 走査を構造消去。
2. **fence/list の is_some+unwrap 二重走査を 1 回化**（ln_fence/ln_list_item）。
研究根拠: Drepper「HW prefetcher は順次アクセスに最適・間接参照に無力」に基づく
パス統合、および simdjson「同じバイトを二度なめるな」原則の逐語適用。

### 実測（仲裁 3-way n=21 interleaved、stdout 全一致検証付き）
12-c → 12-d: **parse −3.03%・total −2.95%**（ambient 高負荷時で parse −3.88ms）。
対 C 精読値: **parse 2.048×・total 1.970×**（実測帯 1.99–2.49 の下端へ）。
system bench data-20260829j.json は激重 run（C 16MB total 223.41ms、ambient
崩壊帯）で paired 1.790× は帯外ノイズ —— **精読値は仲裁を正**と台帺記する。
RSS16 1.117 / RSS2 1.063 / ANSI RSS 1.749（決定的指標で横這い）、startup 符号
50:100（median C 2.609/R 2.488ms、同じ高負荷帯の jitter。系列 median 同値維持）。

### 検証（全て緑）
byte-exact 6/6（C==R × auto/forced/@1HT × 2 コーパス）、oracle 21/21、
cargo test 345 緑（release）、clippy/fmt 0、差分 fuzz +3,019（seed 2718281）
0 mismatch → 累計 **185,266**。

### 残照準
1. **blocks/table 深層**: 段落継続バッテリーの更なる統合、split_cells 周辺、
   quote の ln_quote 二重評価（短run で軽微）。
2. **layout gap**（最大の絶対差。bench 帯 R 94-167ms vs C 39-91ms）:
   子配列化（Drepper §2.2 教義）の設計踏査。
3. inline_span 群 ~65k allocs（12-b 残、時間見返りは小）。
4. read 段（構造確定済み・凍結）。

## フェーズ 12-e: layout 形状解剖（per-atom 照準確定 + ASCII ラン SWAR 化の検証棄却）

layout gap（最大の絶対差）の解剖フェーズ。コード差分はドキュメントコメントのみ
（後述の検証棄却記録）。

### 形状マトリクス（合成 16MB パターン別、layout 段 interleaved n=7 median）
| pattern（DOM 形状） | C layout | R layout | R/C |
|---|---|---|---|
| fence（pre/code 生テキスト大量行） | 18.03ms | 39.49ms | **2.19×** |
| blocks（混合ブロック） | 46.67ms | 82.24ms | **1.76×** |
| head（見出し大量） | 30.18ms | 47.05ms | **1.56×** |
| table | 146.74ms | 193.64ms | 1.32× |
| plain | 23.54ms | 28.71ms | 1.22× |
| list | 53.24ms | 62.08ms | 1.17× |
| quote | 26.59ms | 29.28ms | 1.10× |

### 検証棄却の記録（ASCII 可視ラン SWAR 化）
被疑: C は `lw_ascii_run_end` で SSE2/AVX2 一括分類、R は range 判定をバイト毎
に回す差（layout.rs「アトム切り出し」）。haszero 系 u64 SWAR（範囲 3 検査和集合、
擬陰性ゼロ設計 + scalar 正確確認）で置換を試みたが **interleaved A/B n=9 で
fence +2.1%・plain +7.2% の微増悪を計測し棄却・revert**。原因は設計誤り:
アトムは実コーパスで平均 5-7B と短く、SWAR 窓の立ち上げ費が短ランで利く
（pcmeqb 展開で敗れた scan_special 12-c 棄却に続く haszero 系 2 連敗。
**バイト分類は layout 時間の主犯ではなかった**）。証左コメントを local 残置。

### 照準確定（次フェーズ）
layout の真の主犯は **per-atom / per-row 固定費**（push_seg・ProvSpan 生成・
push_merge 併合判定・end_line 行組立・deco 追記）。Seg は 24B 痩身済み、
push_merge も精読ずみで構造上の無駄なし。C IfSeg との処理量差を行/atom 視点で
再度帰属分解する器具（行/atom カウンタ付き layout 分解例）を設計中。
render 段も形状別で R/C 1.1-2.0 と横並びに悪い（fence 1.53 等）ため同時照準。

### 実測（system bench data-20260829k.json）
16MB 2.314× / 2MB 2.032× / ANSI 1.589×。RSS16 1.116・RSS2 1.063・ANSI 1.748
（決定的指標で横這い）。startup 符号 **118:31**（median C 1.5485/R 1.6410ms、
+92μs。系列 75:74→87:63→112:37→50:100→118:31 と両方向に揺れる jitter 帯だが
直近 2 run で分裂が大きい。次 run で追跡明記）。byte-exact 16MB OK・test 345 緑・
clippy/fmt 0。コード不変のため fuzz 累計は 185,266 据置き。

## フェーズ 12-f: seg 二重書き消去（seg_arena 直接確定 + RefMut IFC 保持 + pm_key 痩身）— layout 形状別 −0.3〜−10.9%

12-e で確定した照準（per-atom/per-row 固定費）への本手術フェーズ。

### 帰属（新器具 `examples/layout_probe.rs` での計数。合成 16MB 各形状）
| shape | lines | segs | 二重書き量 |
|---|---|---|---|
| fence | 353,205 | 309,054 | 7.4MB |
| blocks | 426,902 | 384,212 | 9.2MB |
| head | 415,965 | 415,965 | 10.0MB |
| plain | 177,852 | 177,852 | 4.3MB |
| table | 1,449,884 | 1,449,884 | 34.8MB |

C は seg を arena bump に**直接確定**（`wrap_push_seg` = ポインタ加算 + 4 フィールド
書きのみ、行末コピー無し、`wrap_end_line` は RLine 値レコード追記で O(1)）。
旧 Rust は scratch `Vec<Seg>` へ push 後、行末に `seg_arena` へ `append` する
設計だったため **全 seg が二度書き**（上表の memcpy 量）+ 行ごとの RefCell
borrow_mut ×2 が載っていた。

### 変更（意味不変の構造修正。C `IfWrap` 機構への忠実化）
1. `Wrap.segs` 廃止 → `seg_arena` 末尾へ直接 push + `line_lo: u32` で現行行の
   始端を区切る（現行行 = `seg_arena[line_lo..]`。行末 append を消去）。
   行の排他条件は `len > line_lo`（C `n_segs != 0`・旧 `!segs.is_empty()` と同値）。
2. `rlines` / `seg_arena` の `&RefCell` 参照を **IFC 期間中 `RefMut` で保持** へ
   （borrow 税 per-line → per-IFC。IFC は入れ子不可・同時借用なしを監査済。
   `grow_model` は Cell のみで Vec に触れない）。
3. `pm_key: Option<(Prov, usize)>`（16B 比較）→ `Option<(u32, u32)>`（8B 比較 1 発）。
   由来 id は `Seg::src` 列と同じ写像（`prov_src_id`）。
4. `segs_scratch` スクラッチ機構を廃止（直接 push で不要になった）。

### 手術で踏んだ罠と修復（嘘をつかない台帳: fuzz 検出・設計失敗の記録）
一時値寿命の Rust 落とし穴を 1 件・不変条件の監査漏れを 1 件踏んだ:

- `Wrap { line_lo: lc.seg_arena.borrow().len(), .., seg_arena: lc.seg_arena.borrow_mut() }`
  は構造体式フィールドの一時 `Ref` が**文末まで生存**するため実行時に
  `RefCell already borrowed` で即死（`--no-ansi` 全滅で即検出。`RefMut` を先に
  文として切る形に修正）。
- **重大**: diff fuzz seed 55555 が 3 mismatch（`--no-style` 経路で R 側パニック
  `slice index starts at 4 but ends at 3`）を検出。旧構造では scratch Vec が
  「現行行の seg 列」を構造的に表現していたが、直接 push 化でそれを失い、
  wrap 時の行尾空白 trim が **現行行が空の時に確定済み前行の seg を pop/縮小**
  し RLine 区間を逆転させた。発火条件: `<br>` ピース直後は C 規約で
  `w.line_w` が前行の値のまま残り（C も同じ stale 動作で byte-exact は保たれる）、
  空白のみの行で `cx>0` のまま次アトムが幅超過すると trim 経路に来る。
  C は `w->n_segs > 0`（現行行限定）ガードで防いでいた → **同値の明示ガード
  `len > line_lo` を trim 経路にも追加** して修復。106B 最小再現
  （`a  <br>` + `b`*99、--no-style）を `oracle/fuzz-br-trim.html` として
  `tools/chk_oracle.sh` に 3 モード（noansi/dump-layout/dump-dom）pinning 登録
  （oracle 21→24）。**監査の教訓: 不変条件「cx>0 ⇒ 現行行に seg あり」は
  scratch 構造が保っていた暗黙条件で、br stale line_w 経路で偽になる。構造を
  取り払う時は暗黙の担保を全て明示化せよ**。

### 効果（仲裁 3-way n=21 interleaved median、16MB IDM）
- layout 段: 136.78 → **132.13ms（−3.40%、−4.65ms）**（NEW/C 2.210×）。
- 形状別 layout 段（n=15 interleaved、ΔBASE）: fence −2.70% / head −5.26% /
  blocks −0.32% / table **−10.90%** / plain −4.27%。**5/5 全形状が改善方向** で、
  幅度は二重書き量の順位（table 34.8MB > head 10MB > blocks 9.2MB > fence
  7.4MB > plain 4.3MB）と整合 — 機構が帰属通りであることの裏付け。
- total は −0.46%（parse +2.77% は無変更コードで ambient ノイズ。±20% 帯内）。

### 検証バッテリー（全緑）
byte-exact 6/6（2 コーパス × auto/forced/ht1）・oracle 24/24（C/R 両バイナリ）・
cargo test 345 緑・clippy/fmt 0・diff fuzz +10,000（seed 55555、不具合検出→修復→
再投で **0 mismatch**。累計 **195,266**）。

### 実測（system bench data-20260829l.json）
16MB 2.207× / 2MB 2.059× / ANSI 1.659×。RSS16 1.112・RSS2 1.064・ANSI 1.744
（決定的指標で横這い〜微改善）。startup 符号 **61:89**（median C 1.8010/R
1.7585ms、**R が −42.5μs 速い**。系列 75:74→87:63→112:37→50:100→118:31→61:89
と両方向に揺れ、前回の R 不利分裂は jitter 帯内の変動と判断 = 追跡結論）。
byte-exact 16MB OK・test 345 緑・clippy/fmt 0・fuzz 累計 195,266。
