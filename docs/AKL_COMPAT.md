# AKL_COMPAT.md — Aklus(akl) JS 言語カバレッジ（測定記録の唯一の正）

**質問への直接の回答: いいえ。akl は「V8 等にある発展的な JS をすべて」実行できません。**
akl は **意図的に切ったサブセット**であり、下表は `build/akl` での実測
（2026-08-08 再採、全ケース実走査。推定ゼロ）である。

**規則**: 未対応構文は **必ず SyntaxError/ReferenceError 等で明白に落ちる**
（「静かに違う答えを返す」状態を最悪のバグと定義する。2026-08-01 に `--i`/`++i` が
二重 unary として黙って誤答する経路を同定・lex 拒否に修正し回帰テスト化）。

## 動く（実測 OK）

| 構文 | 実測例 | 結果 |
|---|---|---|
| var/let/const・多重宣言 | `var a=1; let b=2; const c=3; a+b+c` | `6` |
| if / else / else-if | `if(0){1}else if(1){20}else{30}` | `20` |
| while | `var i=0; while(i<3){i=i+1;} i` | `3` |
| canonical for + break/continue | `for(var i=0;i<10;i=i+1){ if(i==7) break; ...}` | OK |
| 関数宣言・引数・再帰 | `function f(n){ if(n<2) return 1; return n*f(n-1); } f(6)` | `720` |
| 過剰引数の無視・return なし | `f(1,2,3)` / `return;` | `3` / `undefined` |
| 関数内からグローバル参照 | `var x=10; function g(){ return x; } g()` | `10` |
| try/catch/finally/throw（跨フレーム・再送出・finally-on-return） | `try { g(); } catch(e){ e*2 }` | OK |
| 未捕捉例外 | `throw 123` | `uncaught exception: 123` で明白に失敗 |
| 算術 `+ - * / %`（浮動・符号含む） | `7/2` | `3.5` |
| 比較 `< <= > >=` | `1 <= 2` | `true` |
| 等値 `== != === !==`（緩い等値の型変換含む） | `1 == "1"` / `1 !== "1"` | `true` / `true` |
| 論理 `&& \|\| !` | `!0 && (1 \|\| 0)` | `1` |
| 単項 `-` `+`（空白分離の二重 unary は合法） | `1 - -2` | `3` |
| typeof | `typeof 5` | `number` |
| 文字列連結・比較・エスケープ・Unicode | `"a"+"bc"` | `abc` |
| 数値リテラル（整数/浮動/16/2/8進/指数） | `0x10 + 0b11 + 0o10 + 1e3` | `1027` |
| null/undefined・宣言のみ var | `var a; a` | `undefined` |
| 強制変換 | `"5"+3` / `"5"*2` | `"53"` / `10` |
| 行/ブロックコメント | — | OK |
| budget fail-stop（命令/深さ/ヒープ） | `while (1) {}` | `instruction budget exhausted`（CLI 既定 500M ops ≒ 本機 1.3s で死亡） |
| オブジェクトリテラル `{k:v}`（IDENT/KW/STR キー、ネスト、trailing comma） | `var o={a:1,b:2}; o.a+o.b` | `3` |
| プロパティアクセス `.prop`（連鎖） | `var o={p:{q:41}}; o.p.q+1` | `42` |
| 代入 `o.x = v`（文としての値は右辺） | `var o={}; o.x=40; o.x+2` | `42` |
| メソッド呼出 `o.f()`（self は native のみ受ける。「this」は言語に非導入） | `function g(){return 3;} var o={}; o.f=g; o.f()` | `3` |
| 未定義プロパティ参照 | `({a:1}).missing` | `undefined` |
| 参照共有（obj は参照セマンティクス） | `var o={q:2}; var p=o; p.q=9; o.q` | `9` |
| identity 等価（`===`/`==` とも obj は同一 idx のみ等しい） | `var a={}; var b={}; a===b` | `false` |
| オブジェクトの ToString / typeof | `""+{a:1}` / `typeof {a:1}` | `[object Object]` / `object` |
| ホストネイティブ関数（`akl_native_register` 族、1024 insn 課金、self 伝播） | docs/EXTENSIONS.md §3-A の console.log が通過実例 | `hello 42 true [object Object]` |
| ホストネイティブ失敗規約（`akl_native_throw`） | — | eval は明白に失敗（黙った undefined を作らない） |
| 正規表現リテラル `/pat/flags`（i g m s u y） | `/^\\d+-(\\d+)$/.exec('12-34')[1]` | `34` |
| `RegExp(pat, flags)` / `new RegExp(pat, flags)`（RE の複製含む） | `new RegExp('ab','i').test('AB')` | `true` |
| `re.test` / `re.exec`（g/y で lastIndex 更新） | `var r=/a/g; r.exec('banana'); r.lastIndex` | `2` |
| `re.source` / `re.flags` / `re.global` / `re.ignoreCase` / `re.multiline` / `re.lastIndex` | `/a/gi.flags` | `gi` |
| `String.match`（g: 全マッチ配列 / 非 g: キャプチャ配列、無しは null） | `'12-34'.match(/(\\d+)-(\\d+)/)[2]` | `34` |
| `String.replace`（RE/文字列、関数 replacer、`$&` `` $` `` `$'` `$1..$99`） | `'abc'.replace(/(b)/,'[$1]')` | `a[b]c` |
| `String.split`（RE: キャプチャ含む。文字列: 従来通り） | `'a1b22c'.split(/(\\d+)/).length` | `5` |
| `String.search` | `'hello'.search(/l+/)` | `2` |
| 正規表現構文: 文字クラス・量詞（非貪欲含む）・グループ・選択・アンカー・`\\d \\w \\s \\b` 等 | — | OK（実測 112 ケースの単体テスト + t_v04_regex） |
| UTF-8 パターン・対象（コードポイント単位） | `/あ+/.test('あああ')` | `true` |
| ステップ上限（指数バックトラックの有界化: 500 万ステップ） | `'aaa...'(30個).replace(/(a+)+b/,'x')` | `RangeError` で明白に失敗 |
| `class B extends A`（メソッド継承・`super` メソッド呼び出し） | `class B extends A { m() { return super.m() + 1; } } new B().m()` | `2` |
| `super(...)` 親コンストラクタ呼び出し（引数伝播・合成 ctor の自動 super()） | `class B extends A { constructor(x) { super(x + 1); } } new B(41).get()` | `42` |
| 3 段以上の継承チェーン・`super` の多重呼び出し | `class C extends B extends A ... new C().m()` | OK |
| static メソッドの継承（`__super` チェーン解決） | `class A { static s(){return 7;} } class B extends A { } B.s()` | `7` |
| 派生クラスの合成コンストラクタ（extends 時は super() を自動呼び出し） | `class B extends A { } new B().n` | 親が初期化した `n` |

意味の精度はテスト固定: `0.1+0.2 === 0.30000000000000004`、`1/0 = Infinity`、
`-1/0 = -Infinity`、`NaN !== NaN`（IEEE 754/JIS X 3010 相当の double 厳密）、
剰余は JS 規格の fmod 系（被除数符号）。

## 動かない（実測で明白に失敗する。未対応一覧）

| 構文 | 実測エラー |
|---|---|
| 文頭 `{` の曖昧性 | ブロック文が無いので object literal として読み、`{a:1}` 単文は `expected ';'` で明白に失敗（JS とは別解釈、いずれも拒否） |
| `Math.floor` 等の標準組込オブジェクト | ✅ v0.3 で実装（Math 24 関数 + 定数 8 種） |
| `super` のプロパティ取得（`super.x = 1`）・`super[name]` | SyntaxError（super.m() と super(...) のみ対応） |
| async/await・generator・BigInt | SyntaxError |
| 正規表現の先読み/後読み `(?=..)` `(?!..)` `(?<=..)`・名前付きキャプチャ `(?<n>..)`・バックリファレンス `\\1` | SyntaxError（コンパイル時） |
| 文字クラス内の非 ASCII 文字・範囲（`[あ-ん]` 等）・`\\u{...}` | SyntaxError（コンパイル時） |
| `match`/`exec` 結果の `index`/`input` プロパティ | undefined（配列要素のみ。AKL_OK_ARR は名前付きプロパティ非対応） |
| `i` フラグの非 ASCII ケースフォールディング・`\\d`/`\\w` の非 ASCII 扱い | 非対応（ASCII のみ。`\\s` は Unicode 空白対応） |
| RegExp 独自プロパティ（`re.x = 1`） | 代入は無視（lastIndex のみ更新可） |
| オブジェクト spread `{...obj}` | SyntaxError（配列 spread は対応済み） |
| メソッド呼び出しの spread `o.m(...a)` | SyntaxError（関数呼び出しの spread は対応済み） |
| 分割代入の rest パターン `[a, ...rest]` | SyntaxError（基本分割代入は対応済み） |
| `String()` / `Number()` コンストラクタ・`Object.keys` 等 | ReferenceError |
| ~~高階関数 `[1,2].map(f)` / `forEach` / `filter`~~ | ✅ v0.3: `map`/`filter`/`forEach`/`some`/`every`/`find`/`findIndex`/`reduce`（VM 再入 akl_call 経由。コールバック fn(elem, idx, arr)、reduce は fn(acc, elem, idx, arr)） |
| `String()` / `Number()` コンストラクタ・`Object.keys` 等 | ReferenceError |
| `s.length` の代入・配列の `length` 代入 | 無視（length は読み取り専用） |
| 文字列メソッドの一部（`padStart`/`padEnd`/`localeCompare`/`charAt` 越え等） | `TypeError: not a function` |
| `JSON` の第 2 引数（replacer/reviver） | 無視（第 1 引数のみ処理） |

## ロードマップ（優先度順。完了時にこの表へ実測で追記する）

1. ✅ オブジェクトリテラル + プロパティアクセス（2026-08-08）
2. ✅ 配列・ブラケット・関数式 + クロージャ捕捉・`this`（2026-08-08。AKL_OK_ENV チェーン）
3. ✅ 三項演算子・switch・do-while・`++`/`--`・複合代入（2026-08-08）
4. ✅ ビット演算/シフト（2026-08-08）
5. ✅ Math / String / Array 組込 + parseInt/parseFloat/isNaN/isFinite（2026-08-08）
6. ✅ JSON.stringify / JSON.parse（2026-08-08）
7. ✅ 高階関数（2026-08-08: VM 再入 akl_call。outer スタックは root_stks として GC ルート化）
8. ✅ 演算子群 `**`/`void`/カンマ/`in`/`delete`/`?.`/`??`/`instanceof`（2026-08-08）
9. ✅ テンプレートリテラル・for-in/for-of・デフォルト引数（2026-08-08）
10. ✅ 分割代入・配列/呼び出し spread・`new`・`class`（2026-08-08。VM 再入とフレーム is_new）
11. ✅ 正規表現（2026-08-09: リテラル/RegExp グローバル/test/exec/match/search/replace/split。エンジンは別ファイル akl_regex.c、バックトラッキング VM + ステップ有界化）
12. ✅ class extends / super（2026-08-09: OP_MAKEFS で親クラスを関数 env にバインド、OP_SUPERGET/OP_CALLT で解決。継承コピーは __super チェーンを親→子順に。合成 ctor は extends 時 super() 自動呼び出し）
13. async/await（コルーチン基盤の設計が必要）

## V8 との位置づけ

- API 形状互換（C++ facade）は docs/V8_COMPAT.md が唯一の正。
- 速度比較（実測）は BENCH.md「akl vs V8」節: **vs V8 --jitless には 7 項目中 6 項目で速い、
  vs V8 full JIT のホット数値ループには 1.8–2.3× 遅い**（median of 3、隠さない）。
- JIT は永久に採用しない（実行可能書き込みページを構造的にゼロにする）。
  CoJIT は意味を変えない AOT 特化で、kill switch（`akl_set_cojit` / CLI `--no-cojit`）常設。
