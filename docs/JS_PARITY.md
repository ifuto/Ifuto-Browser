# JS パリティ台帳: V8 と同じ結果を出すための実測追跡（v0.5、2026-08-10）

方針（ユーザ指令 2026-08-10）: 「基本的には、すべての実行内容が V8 と同じ結果になるようにし、
構文の取りこぼしがあってはならない」。

検証方法: `tools/parity.py` が AKL と node (V8) の両方で同一スニペットを実行し出力を突合する。
**この台帳は parity.py の実測のみを正とする**。未対応は「明白に失敗」（SyntaxError /
ReferenceError）であり、黙って違う結果を返してはならない（修正中もこの規約を守る）。

## 現状サマリ（parity.py 実測: 69 スニペット）

- 一致: **66**（2026-08-12 の Promise マイクロタスク化で 65 → 66）
- 不一致（誤った結果を返す）: 3 → 残課題（下表）
- 未実装（ReferenceError / 明白失敗）: 0（下表の 2 項目はすべて大規模実装待ち）

## 残課題（誤った結果 or 未実装）— 次ターン以降

| 項目 | AKL | V8 | 規模 |
|---|---|---|---|
| `typeof Symbol` / `typeof Proxy` | ReferenceError | function | 大（キー体系 / トラップ） |
| `Object.getPrototypeOf({}) === Object.prototype` | ReferenceError | true | 大（継承モデル） |

## 未実装（取りこぼし）— 実装順

| グループ | 項目 | 状態 |
|---|---|---|
| 関数宣言ホイスティング | スコープ先頭でバインド（V8 準拠） | **修正済み（2026-08-10）** |
| `+` 演算子 | オブジェクトは ToString 連結（`[] + []` = ""、`[1]+[2]` = "12"） | **修正済み（2026-08-10）** |
| Date | `Date.now()` / `new Date(ms)` / `getTime()` / `toISOString()` / `parse` / `UTC` / 全 getter / setTime / toString | **修正済み（2026-08-10）** |
| Error | `Error` / `TypeError` / `RangeError` / `SyntaxError`（name/message/toString） | **修正済み（2026-08-10）** |
| Function | `call` / `apply` / `bind`（束縛 this/args、bound への call/apply、再 bind） | **修正済み（2026-08-10）** |
| Number | `Number.parseInt` / `isInteger` / `isNaN` / `isFinite`、`(1).toString(radix)` / `toFixed` / `valueOf` | **修正済み（2026-08-10）** |
| String | `fromCharCode` / `fromCodePoint` / `at` / `toString` | **修正済み（2026-08-10）** |
| Array | `at` / `of` / `sort`（既定文字列化 + 比較関数） | **修正済み（2026-08-10）** |
| プリミティブ | `(1).toString()` / `true.toString()` / `'x'.toString()`、非 object load は undefined | **修正済み（2026-08-10）** |
| globalThis | `globalThis` | **修正済み（2026-08-10）** |
| eval | `eval('1+2')`（グローバル近似） | **修正済み（2026-08-10）** |
| クラス | `typeof class` は function、new なし呼び出しは TypeError | **修正済み（2026-08-10）** |
| 数値変換 | ToNumber のオブジェクト経路（`+[]` = 0、`+[1]` = 1、`+{}` = NaN、`{} + []` 文頭 = 0） | **修正済み（2026-08-12）** |
| native 例外 | `JSON.parse('{')` 等の例外が try/catch で捕捉可能（`e.name` = "SyntaxError"） | **修正済み（2026-08-12）** |
| 例外の値伝搬 | `a.map(function(){ throw 42 })` の catch は元の値 42（HOF 再入経由でも Error OBJ の identity 保持） | **修正済み（2026-08-12）** |
| 捕捉シャドーイング | `function g(){ try{...}catch(e){ throw e } }` が祖先の同名 catch 束縛を誤 capture しない | **修正済み（2026-08-12）** |
| let/const スコープ | ブロックスコープ分離（`{ let a = 1; } a` → ReferenceError）、シャドーイング（`let a=1; { let a=2; } a` → 1） | **修正済み（2026-08-12）** |
| TDZ | 宣言前の参照/代入は ReferenceError（`{ a; let a; }`）、同ブロック重複宣言は SyntaxError | **修正済み（2026-08-12）** |
| for の let | `for (let i=0;...)` はループ全体をスコープ化、`for (let k in/of ...)` も対応 | **修正済み（2026-08-12）** |
| トップレベル let | クロージャが ENV capture で捕捉（`let x=10; function g(){ return x; }` → 10） | **修正済み（2026-08-12）** |
| Promise マイクロタスク | `Promise.resolve().then(f); x` は f 実行前の x（V8 と一致。旧実装は同期実行） | **修正済み（2026-08-12）** |
| uncaught 表示 | 未捕捉の Error OBJ は `uncaught exception: Error: boom` 形式（toString 近似） | **修正済み（2026-08-12）** |
| CLI 表示 | オブジェクト完了値は JS ToString で表示（旧フォールバックは "[function]" で紛らわしかった） | **修正済み（2026-08-12）** |
| Symbol | `Symbol()` / `Symbol.iterator` | 未（大規模: キー体系拡張） |
| Proxy | `Proxy` / `Reflect` | 未（大規模: トラップ dispatch） |
| プロトタイプ | `Object.getPrototypeOf` / `Object.prototype` / `instanceof` チェーン | 未（大規模: 継承モデル） |
| ラッパー | `new String()` のラッパー挙動 | 未（現在は変換関数と同値） |

## 既知の近似（V8 と意図的に異なる・文書化）

- 文字列の `.length` は UTF-16 code unit でなく **code point 数**（AKL_COMPAT に明記）
- Promise: 解決状態は同期（`new Promise` executor は同期実行）、then コールバックは
  eval 終了時のマイクロタスク消化（V8 準拠）。await は解決済み Promise を同期展開する
  近似（未解決 Promise の await は undefined — 中断/再開は未実装）
- Date のローカル系メソッドは UTC として扱う（TZ 非依存。TZ=UTC 環境なら V8 と一致）
- eval はグローバル近似（直接 eval のローカルスコープ参照は非対応）
- BigInt は 64bit 符号付き整数（任意精度は将来）。BigInt + Number は Number 変換（V8 は TypeError）
- `new Number(5)` 等のラッパー OBJ は数値変換されない（`+new Number(5)` は NaN。V8 は 5。ラッパー種別を持たないため）
- catch 束縛は関数スコープ近似（捕捉値は正しく catch 本体に届く。catch 外から同名参照は
  関数スコープの束縛が見える点のみ V8 と異なる — let/const のブロックスコープ実装は済）
- TDZ はブロックスコープ束縛のみ検査（クロージャ capture 済みの let を宣言前に読む経路は
  未検査で undefined — 稀な組合せ。AKL_COMPAT に明記）
- generator は全同期実行 + yield 値蓄積（遅延評価は将来）
- モジュール export はスナップショット（live binding は将来）
- `with` / `import.meta` / トップレベル await / `new.target` は明示拒否

## 規約

- 新規構文・組込は parity.py にスニペットを追加してから実装（実測が唯一の正）
- 「黙って違う結果」は禁止。未対応は必ず明白失敗
- この台帳の状態列は実装と同時に更新する
