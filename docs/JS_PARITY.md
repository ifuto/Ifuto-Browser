# JS パリティ台帳: V8 と同じ結果を出すための実測追跡（v0.5、2026-08-10）

方針（ユーザ指令 2026-08-10）: 「基本的には、すべての実行内容が V8 と同じ結果になるようにし、
構文の取りこぼしがあってはならない」。

検証方法: `tools/parity.py` が AKL と node (V8) の両方で同一スニペットを実行し出力を突合する。
**この台帳は parity.py の実測のみを正とする**。未対応は「明白に失敗」（SyntaxError /
ReferenceError）であり、黙って違う結果を返してはならない（修正中もこの規約を守る）。

## 現状サマリ（parity.py 実測: 69 スニペット）

- 一致: **62**（2026-08-12 の native 例外捕捉化 + ToNumber 修正で 60 → 62）
- 不一致（誤った結果を返す）: 7 → 残課題（下表）
- 未実装（ReferenceError / 明白失敗）: 0（下表の 3 項目はすべて大規模実装待ち）

## 残課題（誤った結果 or 未実装）— 次ターン以降

| 項目 | AKL | V8 | 規模 |
|---|---|---|---|
| let/const のブロックスコープ `{ let a = 1; } a` | 1（漏れる） | ReferenceError | 大（スコープ管理 + TDZ） |
| `let a = 1; { let a = 2; } a` | 2 | 1 | 同上 |
| Promise のマイクロタスク `Promise.resolve().then(f); x` | 同期実行 | 非同期（x は 0） | 大（マイクロタスクキュー） |
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
| uncaught 表示 | 未捕捉の Error OBJ は `uncaught exception: Error: boom` 形式（toString 近似） | **修正済み（2026-08-12）** |
| CLI 表示 | オブジェクト完了値は JS ToString で表示（旧フォールバックは "[function]" で紛らわしかった） | **修正済み（2026-08-12）** |
| Symbol | `Symbol()` / `Symbol.iterator` | 未（大規模: キー体系拡張） |
| Proxy | `Proxy` / `Reflect` | 未（大規模: トラップ dispatch） |
| プロトタイプ | `Object.getPrototypeOf` / `Object.prototype` / `instanceof` チェーン | 未（大規模: 継承モデル） |
| ラッパー | `new String()` のラッパー挙動 | 未（現在は変換関数と同値） |

## 既知の近似（V8 と意図的に異なる・文書化）

- 文字列の `.length` は UTF-16 code unit でなく **code point 数**（AKL_COMPAT に明記）
- Promise は同期解決近似（マイクロタスクキューは残課題）
- Date のローカル系メソッドは UTC として扱う（TZ 非依存。TZ=UTC 環境なら V8 と一致）
- eval はグローバル近似（直接 eval のローカルスコープ参照は非対応）
- BigInt は 64bit 符号付き整数（任意精度は将来）。BigInt + Number は Number 変換（V8 は TypeError）
- `new Number(5)` 等のラッパー OBJ は数値変換されない（`+new Number(5)` は NaN。V8 は 5。ラッパー種別を持たないため）
- catch 束縛は関数スコープ近似（V8 の catch ブロックスコープは let/const スコープ実装と共に将来対応。捕捉値は正しく catch 本体に届く）
- generator は全同期実行 + yield 値蓄積（遅延評価は将来）
- モジュール export はスナップショット（live binding は将来）
- `with` / `import.meta` / トップレベル await / `new.target` は明示拒否

## 規約

- 新規構文・組込は parity.py にスニペットを追加してから実装（実測が唯一の正）
- 「黙って違う結果」は禁止。未対応は必ず明白失敗
- この台帳の状態列は実装と同時に更新する
