# AKL_COMPAT.md — Aklus(akl) JS 言語カバレッジ（測定記録の唯一の正）

**質問への直接の回答: いいえ。akl は「V8 等にある発展的な JS をすべて」実行できません。**
akl は **意図的に切ったサブセット**であり、下表は `build/akl` での実測
（2026-08-08 再採、全ケース実走査。推定ゼロ）である。

**規則**: 未対応構文は **必ず SyntaxError/ReferenceError 等で明白に落ちる**
（「静かに違う答えを返す」状態を最悪のバグと定義する。2026-08-01 に `--i`/`++i` が
二重 unary として黙って誤答する経路を同定し、一旦 lex 拒否で封じた上で回帰テスト化。
2026-08-08 の v0.4→v0.3 統合で正式な前置/後置 `++`/`--` として実装し直し、
「対象は識別子・プロパティのみ、それ以外は明白な SyntaxError」という不変条件を
保ったまま昇格した）。

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
| 前置/後置 `++`/`--`（識別子・プロパティ双方。v0.4 統合） | `var i=5; i++` / `var o={x:5}; --o.x` | `5`（i は 6 に）/ `4` |
| 複合代入 `+= -= *= /= %=`（識別子は `x=x op y` に脱糖、既存融合命令をそのまま利用。プロパティは obj を 1 度だけ評価） | `var o={x:10}; o.x+=5; o.x` | `15` |
| 三項演算子 `?:`（右結合ネスト・両枝が代入式まで許容） | `1>2 ? 1 : 2` | `2` |
| ビット演算 `& \| ^ ~`（ToInt32 準拠） | `6 & 3` | `2` |
| シフト `<< >> >>>`（シフト量は `&31`、`>>>` は ToUint32） | `-1 >>> 28` | `15` |
| `do`-`while`（本体は最低 1 回実行） | `var n=0; do{n=n+1;}while(0); n` | `1` |
| `switch`/`case`/`default`（`===` 判別・フォールスルー・`default` の任意位置） | `switch(2){case 1:1;case 2:2;case 3:3+9;break;}` | `12` |
| switch 内 `continue` の外側ループへの透過（`break` は switch 自身のみを抜ける） | `for(...){switch(i){case 3:continue;default:acc+=i;}}` | JS 同様に continue は switch を素通り |

意味の精度はテスト固定: `0.1+0.2 === 0.30000000000000004`、`1/0 = Infinity`、
`-1/0 = -Infinity`、`NaN !== NaN`（IEEE 754/JIS X 3010 相当の double 厳密）、
剰余は JS 規格の fmod 系（被除数符号）。
ビット演算/シフトの ToInt32/ToUint32 も固定: `NaN|0 === 0`、`Infinity|0 === 0`、
`4294967296|0 === 0`（2^32 は 0 に畳む）、`2147483648|0 === -2147483648`（2^31 帯の折返し）、
`-1>>>0 === 4294967295`（ToUint32 域は int32 タグを超えるため double 化）。

## 動かない（実測で明白に失敗する。未対応一覧）

| 構文 | 実測エラー |
|---|---|
| 配列リテラル `[1,2,3]` / 要素アクセス `o["k"]`・`a[i]` | SyntaxError（lex 拒否。v0.4b 前の次課題） |
| ブラケットプロパティアクセス `o["k"]` | SyntaxError（`.` のみ） |
| `this`・メソッド shorthand・computed key・shorthand `{a}` | SyntaxError（`o.f()` の self は native 専用で `this` バインドは渡らない） |
| 文字列プロパティ `s.length`（および数値/bool のプロパティ） | `TypeError: property access on non-object value`（暗黙ボックス化はしない） |
| `Math.floor` 等の標準組込オブジェクト | `ReferenceError: Math is not defined`（ホスト登録のみ存在） |
| 文頭 `{` の曖昧性 | ブロック文が無いので object literal として読み、`{a:1}` 単文は `expected ';'` で明白に失敗（JS とは別解釈、いずれも拒否） |
| 関数式・IIFE・アロー関数 | SyntaxError |
| **クロージャ捕捉**（ネスト関数から外スコープの局所変数） | `ReferenceError: n is not defined` |
| for-in / for-of | SyntaxError |
| `&= \|= ^= <<= >>= >>>=`（複合代入はビット演算/シフトまで拡張していない。明示的非対応） | SyntaxError |
| `**`（べき乗演算子） | SyntaxError |
| リテラル以外への後置 `++`/`--`（数値/文字列/呼出式そのものなど） | `expected ';'` または `invalid increment/decrement target`（対象は識別子・プロパティのみ） |
| class・async/await・generator・template literal・BigInt | SyntaxError |
| `new`・`?.`・`??`・regex literal・spread・destructuring | SyntaxError |
| `void`・カンマ演算子・`instanceof`・`delete`・`in` | SyntaxError |

## ロードマップ（優先度順。完了時にこの表へ実測で追記する）

1. ✅ オブジェクトリテラル + プロパティアクセス（2026-08-08 実測で上表へ）
2. ✅ 三項演算子・switch・do-while・`++`/`--`・複合代入（2026-08-08、v0.4→v0.3 統合。上表参照）
3. ✅ ビット演算/シフト（double→int32/uint32 変換規則含む。2026-08-08、上表参照）
4. 配列リテラル + ブラケットアクセス（`[1,2,3]`、`a[i]`、`o["k"]`、`.length`）— 次
5. 関数式 + クロージャ捕捉（環境 record の導入）— 実装リスク最大（upvalue 機構を要する）

## V8 との位置づけ

- API 形状互換（C++ facade）は docs/V8_COMPAT.md が唯一の正。
- 速度比較（実測）は BENCH.md「akl vs V8」節: **vs V8 --jitless には 7 項目中 6 項目で速い、
  vs V8 full JIT のホット数値ループには 1.8–2.3× 遅い**（median of 3、隠さない）。
- JIT は永久に採用しない（実行可能書き込みページを構造的にゼロにする）。
  CoJIT は意味を変えない AOT 特化で、kill switch（`akl_set_cojit` / CLI `--no-cojit`）常設。
