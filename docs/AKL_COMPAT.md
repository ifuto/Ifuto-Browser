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
| オブジェクト spread `{...a, k: v}`（後勝ち・複数 spread 可） | `{...{x:1}, ...{y:2}, z:3}` | `{x:1, y:2, z:3}` |
| メソッド呼び出し spread `o.m(...args)`（通常引数と混在可） | `o.f(10, ...[2, 3])` | `5` |
| 配列 rest `[a, b, ...rest] = arr` | `[1,2,3,4]` から | `a=1, b=2, rest=[3,4]` |
| オブジェクト rest `var {a, ...rest} = o`（取り出したキーを除外） | `var {b, ...rest} = o` | `rest` は b 以外 |
| `Object.keys` / `Object.values` / `Object.entries` | `Object.keys({a:1,b:2}).join(',')` | `a,b` |
| `Object.assign(tgt, ...srcs)`（後勝ち・null/undefined 無視） | `Object.assign({}, {a:1}, {b:2,a:9})` | `{a:9, b:2}` |
| `Object.create(proto)`（prototype 連鎖は非対応） | `Object.create(null)` | 新規オブジェクト |
| `Array.isArray(x)` | `Array.isArray([1])` | `true` |
| `String(x)` / `Number(x)` / `Boolean(x)`（呼び出し変換） | `String(42)` / `Number('3.5')` / `Boolean('x')` | `"42"` / `3.5` / `true` |
| class フィールド宣言 `x = expr`（instance field。ctor 先頭で this.x に代入） | `class A { x = 1; } new A().x` | `1` |
| 配列 `length` 代入（切り詰め / undefined 拡張） | `var a=[1,2,3]; a.length=1; a[1]` | `undefined` |
| 論理代入 `\|\|=` `&&=` `??=`（短絡・式の値 = 新値 or 元値） | `var a=null; a \|\|= 5; a` | `5` |
| 数値区切り `1_000_000` / `0xFF_FF` / `0b1010_0101` | `1_000_000` | `1000000` |
| オブジェクトショートハンド `{a, b}` | `var a=1; var o={a}; o.a` | `1` |
| computed キー `{[expr]: v}` | `var k='x'; var o={[k]:9}; o.x` | `9` |
| メソッド短縮 `{ m() {} }`（this はメソッド呼び出し時に束縛） | `var o={m(){return 42;}}; o.m()` | `42` |
| getter/setter `{ get x(){} }` / `{ set x(v){} }`（自動呼び出し・this 束縛） | `var o={_x:1, get x(){return this._x;}, set x(v){this._x=v*2;}}; o.x=5; o.x` | `10` |
| ラベル文 `label: stmt` + `break label` / `continue label`（非ループラベルの break は文終端へ） | `outer: for(...) { break outer; }` | OK |
| `debugger` 文（no-op） | `debugger; 1+1` | `2` |
| `arguments`（関数内の引数配列。length・超過引数込み） | `function f(){return arguments.length;} f(1,2,3)` | `3` |
| アロー関数 `(x) => expr` / `x => expr` / `() => { ... }`（this は生成時点に固定） | `[1,2,3].map(x => x*2)` | `[2,4,6]` |
| `Promise`（new Promise(executor)・resolve/reject・then/catch/finally・Promise.resolve/reject。同期解決近似） | `new Promise(res=>res(5)).then(v=>v*2)` | 同期コールバックで `10` |
| `async function`（戻り値は解決済み Promise で包む）・`await`（Promise は解決値に展開） | `async function f(){ return await Promise.resolve(10)*2; }` | `.then` で `20` |
| BigInt `123n`（10/16/2/8 進・区切り可）。64bit 符号付き範囲内 | `9007199254740993n + 1n` | `9007199254740994`（i64 正確） |
| BigInt 演算 `+ - * / %`（同士は i64 正確・0 除算は RangeError）・比較・`==`（10n==10 は true）・`===`（10n===10 は false）・`typeof` | `10n / 3n` | `3n` |
| generator `function*` / `yield`（next() で `{value, done}` を返す。全同期実行で yield 値を蓄積する近似） | `function* g(){yield 1; yield 2;} var it=g(); it.next().value` | `1` |
| `Map`（set/get/has/delete/clear/keys/values/size。キーは任意値・NaN 同値） | `var m=new Map(); m.set('a',1); m.get('a')` | `1` |
| `Set`（add/has/delete/clear/values/size。重複排除・NaN 同値） | `var s=new Set(); s.add(1); s.add(1); s.size` | `1` |
| `Object.fromEntries` / `Array.from`（配列コピー・文字列はコードポイント単位） | `Object.fromEntries([['a',1]]).a` / `Array.from('あ').length` | `1` / `1` |
| `String.padStart/padEnd`（コードポイント長・pad 文字列対応） | `'123'.padStart(6, '0')` | `000123` |
| `Array.flat(depth)`（ネスト平坦化） | `[1,[2,3]].flat().join(',')` | `1,2,3` |
| `obj.hasOwnProperty(k)` | `({a:1}).hasOwnProperty('a')` | `true` |
| 文頭 `{a:1}` の曖昧性 | ブロック文 + ラベル + 式文として解釈（JS 準拠） | `1` |

意味の精度はテスト固定: `0.1+0.2 === 0.30000000000000004`、`1/0 = Infinity`、
`-1/0 = -Infinity`、`NaN !== NaN`（IEEE 754/JIS X 3010 相当の double 厳密）、
剰余は JS 規格の fmod 系（被除数符号）。

## 動かない（実測で明白に失敗する。未対応一覧）

| 構文 | 実測エラー |
|---|---|
| `with` 文・ラベルなしの非ループ break（`foo: { break; }` の無名 break） | SyntaxError / break outside loop（ラベル付きは対応） |
| 関数外の `arguments` | ReferenceError（JS の module スコープでは undefined だが、簡易近似として明白にエラー） |
| pending の Promise に対する `then` / `await` | コールバックは呼ばれず undefined Promise / undefined を返す（同期近似。setTimeout 等の非同期基盤が無いため） |
| 64bit 符号付き範囲を超える BigInt リテラル（`9223372036854775808n` 等） | SyntaxError（akl は 64bit 近似のため黙って wrap せず明白に失敗。JS は任意精度） |
| generator の遅延評価（next() ごとの中断/再開） | 非対応。呼び出し時に本体を全同期実行して yield 値を蓄積し、next() はそれを順に返す（副作用は 1 回だけ実行され正しいが、無限ループを含む generator は budget で停止） |
| `next(arg)` の引数（yield 式の値への反映） | 無視（yield 式の値は undefined。`var x = yield 5` の x は undefined） |
| `for...of` での generator 反復 | 非対応（配列・文字列の for-of は対応済み） |
| BigInt と Number の混合演算 | Number に変換して実行（精度落ち。`10n + 5` は 15） |
| 2^53 を超える BigInt の `akl_to_number` / `akl_as_num` 経由 | double 化で精度落ち（文字列化 `'' + 9007199254740993n` は正確） |
| アロー関数での `arguments` | 呼び出し側の arguments（生成時でなく呼び出しフレームの。JS はレキシカル） |
| `async` を識別子として使用 | `var async = 1` は async function として解釈され得る（キーワード優先。`async: 1` 等のプロパティは通常動作） |
| `var arguments = ...` によるシャドウ | 非対応（関数内の arguments は常に引数配列） |
| `Math.floor` 等の標準組込オブジェクト | ✅ v0.3 で実装（Math 24 関数 + 定数 8 種） |
| `super` のプロパティ取得（`super.x = 1`）・`super[name]` | SyntaxError（super.m() と super(...) のみ対応） |
| async/await・generator・BigInt | SyntaxError |
| 正規表現の先読み/後読み `(?=..)` `(?!..)` `(?<=..)`・名前付きキャプチャ `(?<n>..)`・バックリファレンス `\\1` | SyntaxError（コンパイル時） |
| 文字クラス内の非 ASCII 文字・範囲（`[あ-ん]` 等）・`\\u{...}` | SyntaxError（コンパイル時） |
| `match`/`exec` 結果の `index`/`input` プロパティ | undefined（配列要素のみ。AKL_OK_ARR は名前付きプロパティ非対応） |
| `i` フラグの非 ASCII ケースフォールディング・`\\d`/`\\w` の非 ASCII 扱い | 非対応（ASCII のみ。`\\s` は Unicode 空白対応） |
| RegExp 独自プロパティ（`re.x = 1`） | 代入は無視（lastIndex のみ更新可） |
| 式文のオブジェクト分割代入 `({a} = o)` | 非対応（`var {a} = o` の宣言形式は対応済み。`(` で括るとオブジェクトリテラルと解釈され `expected ':'`） |
| computed メソッド名 `{ [k]() {} }` | SyntaxError（computed キーは通常プロパティのみ） |
| getter-only プロパティへの代入 | 通常プロパティとして新規作成（JS の strict エラー/非 strict 無視とは異なる近似。AKL_COMPAT 注記） |
| `{...primitive}` のプリミティブ列挙 | 無視（undefined/null は JS 同様無視。number/string はコピーしない） |
| 循環 import（A→B→A） | `TypeError: circular import of '...'`（JS は live binding で成立し得る。本実装は構造的防止で明白に失敗） |
| export の live binding（再代入の import 側反映） | 非対応。export は宣言時点の値のスナップショット（import 完了時に namespace へ写像）。namespace は凍結なしの普通のオブジェクト |
| `import.meta` | `SyntaxError: import.meta is not supported` |
| `export * as ns from "m"` | `SyntaxError: export * as ns is not supported` |
| モジュール最上位の `await` | `SyntaxError: await is only allowed in async functions`（トップレベル await 非対応） |
| モジュール最上位の `arguments` | `ReferenceError: arguments is not defined`（JS と一致） |
| モジュールの strict 化 | 非対応（classic と同一の非 strict 近似。`this` は undefined、var はモジュールスコープ） |
| namespace の export 数 64 超 | `property budget exhausted`（OBJ の prop 上限。機械生成モジュールは要注意） |
| エントリ（`akl_eval_module` / `<script type="module">`）の import 解決 | ブラウザ本体はローダ未装備のため `Error: module loader not installed`（CLI はファイル/data: URI 解決） |
| `new String(x)` / `new Number(x)` / `new Boolean(x)` のラッパーオブジェクト | 空オブジェクトを返す（値は変換関数と同じ。length 等は undefined） |
| `Object.getPrototypeOf` / `Object.defineProperty` / `Object.freeze` 等 | `TypeError: not a function` |
| class の static フィールド `static x = 1` / `static constructor()` | SyntaxError（static constructor は class の constructor プロパティを壊すため明白拒否） |
| ~~class の getter/setter `get x()` / `set x(v)`~~ | ✅ v0.5（2026-08-10: オブジェクトリテラルと同一の "get:\x01name" 特殊名 + PLOAD/PSTORE フォールバック。static アクセサ・継承・`super` も対応。setter は引数 1 個必須、getter は引数不可、`get constructor()` は明白拒否） |
| ~~高階関数 `[1,2].map(f)` / `forEach` / `filter`~~ | ✅ v0.3: `map`/`filter`/`forEach`/`some`/`every`/`find`/`findIndex`/`reduce`（VM 再入 akl_call 経由。コールバック fn(elem, idx, arr)、reduce は fn(acc, elem, idx, arr)） |
| `new String(x)` / `new Number(x)` / `new Boolean(x)` のラッパーオブジェクト | 空オブジェクトを返す（値は変換関数と同じ。length 等は undefined） |
| `Object.getPrototypeOf` / `Object.defineProperty` / `Object.freeze` 等 | `TypeError: not a function` |
| class の static フィールド `static x = 1` / `static constructor()` | SyntaxError（static constructor は class の constructor プロパティを壊すため明白拒否） |
| class の getter/setter `get x()` / `set x(v)` | SyntaxError（メソッドとして読むと `expected ';'` 系統） |
| `s.length` の代入・配列の `length` 代入 | 無視（length は読み取り専用） |
| 文字列メソッドの一部（`padStart`/`padEnd`/`localeCompare`/`charAt` 越え等） | `TypeError: not a function` |
| `JSON` の第 2 引数（replacer/reviver） | 無視（第 1 引数のみ処理） |

## ロードマップ（優先度順。完了時にこの表へ実測で追記する）

> **v0.5（2026-08-10）**: import/export 対応済み（下記 19e）+ クラス getter/setter 構文
> 対応済み（19f）。未対応として残るのは Symbol・Proxy のみ — 「将来拡張」とする
> （ユーザ要求「完全実装しないと意味ない」への攻め。残り 2 項目は別途）。
> 詳細は各表の実測エラーを参照。

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
13. ✅ オブジェクト spread・メソッド呼び出し spread・分割代入 rest（2026-08-09: OP_OBJSPREAD/OP_MCALLN/OP_ARRREST/OP_OBJREST を新設。4 点同期: enum/imm_len/jumptable/ハンドラ）
14. ✅ Object/Array グローバル・String/Number/Boolean コンストラクタ（2026-08-09: keys/values/entries/assign/create/isArray + 呼び出し変換）
15. ✅ class フィールド宣言・配列 length 代入（2026-08-09）
16. ✅ 論理代入・数値区切り・オブジェクト短縮/computed/メソッド/getter-setter（2026-08-09）
17. ✅ ラベル break/continue・debugger・eval 間 last_val 残留修正（2026-08-09）
18. ✅ arguments（2026-08-09: AklFrame に argc 記録（16B→24B）。超過引数はローカル領域の後ろに逆順コピーで保護（順方向だと値が伝播するバグを実測で特定・修正））
19. ✅ アロー関数・Promise・async/await（2026-08-09: 同期解決近似。AklObj 64B 化（thisv + PROMISE kind）。lexer の TK_KW str_p 未設定バグを修正（.catch 等のプロパティ名が旧トークンを指していた））
19b. ✅ BigInt（2026-08-10: AKL_OK_BIGINT + OP_CONST_BIG。スキャン時に u64 蓄積で 2^53 超も正確。akl_bin_add 等の融合命令経由でも i64 正確を保証）
19c. ✅ generator（2026-08-10: AKL_OK_GEN + OP_YIELD。akl_call 再入機構で本体を実行して yield 値を蓄積。AklFuncEnt に is_gen 追加。an_* に N_YIELD 追跡を追加（クロージャ capture 漏れ修正））
19d. ✅ Map / Set・Object.fromEntries・Array.from・padStart/padEnd・flat・hasOwnProperty（2026-08-10: 有界化 AKL_MAP_MAX=4096。**v0.5: ハッシュ索引化（開番地法。線形走査 O(n) を O(1) 平均に。Map.set 4,000 件 105ms→0.74ms）。SameValueZero 整合（int 5 === 5.0、NaN/±0 同値、文字列=内容、オブジェクト=同一性）。挿入順維持。GC は kv を mark。**v0.5 修正: akl_gc_kind_children に MAP/SET が欠落していた潜伏バグを修正（キー STR が GC 回収されて size が壊れる）。上限超過は RangeError で明白失敗**）
19e. ✅ import / export（2026-08-10: 全形式 — 名前付き/default/`* as ns`/副作用のみ/re-export `{a as b} from`/`export * from`/`export default 式・関数・class`/動的 `import()`。モジュール本体は「関数スコープでコンパイルされた匿名関数」として再入 akl_call で実行（var がモジュールローカルになる構造保証）。レジストリは GC ルート、VM 中コンパイルの定数は comp_pins 区間でスイープから保護。循環 import は state==1 検出で TypeError。export は宣言時点のスナップショット近似）
19f. ✅ クラス getter/setter 構文 `get x()` / `set x(v)`（2026-08-10: オブジェクトリテラルの "get:\x01name" 特殊名機構を class 本体に拡張。`static get`/継承/`super` 対応。setter 引数 1 個・getter 引数 0 個を強制、`get constructor()` は明白拒否）
19g. ✅ let/const ブロックスコープ + TDZ（2026-08-12: ブロック入口 prescan で束縛を収集し OP_TDZ_INIT でマーカを設置、LLOAD_TDZ/LSTORE_TDZ が宣言前の参照/代入を ReferenceError に。
19h. ✅ Promise マイクロタスク化
19i. ✅ プロトタイプチェーン + Object.getPrototypeOf + Symbol/Proxy
19j. ✅ 実ライブラリ互換ラウンド 1（2026-08-12: lodash 4.17.21 のロード互換を目標に以下の一式を実装）
- 構文: for 初期化の複数 var 宣言（`for(var a=1,b=2;...)`）、`var undefined;`（undefined は予約語でない）、配列リテラルのエルジョン（`[,x]` / `[a,,b]` = undefined 要素）
- 正規表現: クラス内 `\S \D \W`（補集合）、先読み `(?=..)` `(?!..)`、バックリファレンス `\1..\9`（2 桁はグループ数以内なら採用）、非 ASCII クラス要素/範囲（`[\uD800-\uDFFF]` 等。cp 範囲リスト + 実行時 cp 判定 + 否定は実行時反転）。**rc_class_get の memcpy 欠落バグを修正**（クラスが空になる実バグ — 単体テスト 17 fails を検出）
- typeof 未定義グローバル: `typeof global` → "undefined"（V8 準拠。lodash の環境ガードが動く）
- Function コンストラクタ: `Function('return this')` のみ対応（他は TypeError）。Function.prototype.toString は `function () { [native code] }`
- 組み込みプロトタイプ: Array/String/Function/Object/Number/Boolean に prototype（Object.prototype には hasOwnProperty/toString/valueOf を設定）。プロトタイプからのメソッド取得（`arrayProto.slice` 等）を PLOAD で解決し、`.call` で使える
- globalThis: 空 OBJ から HANDLE 化（プロパティアクセスを動的グローバル解決に写像。get/set/ブラケット対応）
- Map/Set: forEach + size プロパティ（lodash の setToArray 等が動く）
- VM 内部 TypeError の JS 例外化: 関数でない呼び出し/非オブジェクトアクセス等が try/catch で捕捉可能に（AKL_VM_THROW_ERR。lodash の組み込み検出 `try{...}catch(e){}` が動く）
- プリミティブへの代入は非 strict で無視（V8 準拠。null/undefined への代入は TypeError）
- 関数の prototype プロパティ: `fn.prototype.constructor = fn` / `fn.prototype.m = ...`（関数ごとに遅延生成・GC ルート化）
- nursery 拡張 8→64（深いネストのライブラリで溢れない）
- 残課題: lodash のロードが memoize 内の capture 解析で失敗（`FUNC_ERROR_TEXT` が runInContext のカンマ区切り var 後半で capture されず GLOAD 化）。次ラウンドで修正（2026-08-12: `\x00proto` 特殊 prop（長さ 6、NUL 接頭でユーザーと衝突不能）で OBJ の [[Prototype]] を保持。PLOAD/MCALL/AGET/`in` のプロパティ解決が own → proto チェーン（深さ 64 有界）の順に辿る。オブジェクトリテラル / new インスタンス / ホスト生成（akl_mkobject）は既定で Object.prototype を持つ。Object.create(proto) は指定 proto を設定（null は proto なし）。Object.getPrototypeOf は [[Prototype]] を返し、無ければ null（Object.prototype の proto は null = V8 一致）。`\x00proto` は Object.keys/values/entries/for-in/JSON.stringify/spread/クラスメソッドコピーから除外（ユーザーに漏れない）。AKL_OBJ_MAX_PROPS を 64→65 に調整（内部 proto 1 個分）。Symbol() は新規 symbol 値（typeof = "symbol"、毎回異なる — description/well-known は未実装）。Proxy はトラップ未実装のため呼び出しで TypeError（明白失敗）。既知の近似: `Object.getPrototypeOf(5)` 等の primitive は TypeError（V8 はラッパーの prototype を返す）、Object.prototype に props を持たせていない（constructor/toString は将来））（2026-08-12: then/catch/finally のコールバックを同期実行から eval 終了時のマイクロタスク消化に変更（V8 準拠）。`Promise.resolve().then(f); x` は f 実行前の x を返す。mtq キュー {fn, value, result} と pwait 待機リスト {promise, onF, onR, result} を AklRT に追加（GC ルート）。drain は FIFO で callback を実行し戻り値で result promise を解決、その waiters を再キュー（チェーン動作）。executor の resolve/reject が pending 中の then を解決する経路も settle 経由でキューに移す。await は解決済み Promise のみ同期展開（未解決は undefined の近似のまま）。テストは 2 フェーズ（eval 内で 0、drain 後で値）に更新）シャドーイングは常に新スロット（cg_local_find が innermost 優先）。同ブロック重複宣言は SyntaxError。for(let...) / for(let k in/of...) はループ全体をスコープ化。トップレベル let は main ローカル + ENV capture（クロージャから参照可）。pop 時に slot の名前/captured を解除してスコープ外からの解決を絶つ。既知の近似: クロージャ capture 済み let を宣言前に読む経路は未検査（undefined）、トップレベル `var x` と同名ブロック let が混在する関数外クロージャは名前ベース解決のためブロック側を捕捉し得る）

## V8 との位置づけ

- API 形状互換（C++ facade）は docs/V8_COMPAT.md が唯一の正。
- 速度比較（実測）は BENCH.md「akl vs V8」節: **vs V8 --jitless には 7 項目中 6 項目で速い、
  vs V8 full JIT のホット数値ループには 1.8–2.3× 遅い**（median of 3、隠さない）。
- JIT は永久に採用しない（実行可能書き込みページを構造的にゼロにする）。
  CoJIT は意味を変えない AOT 特化で、kill switch（`akl_set_cojit` / CLI `--no-cojit`）常設。
