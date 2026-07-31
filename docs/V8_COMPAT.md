# V8_COMPAT.md — Aklus(akl) JS Engine ↔ V8 API 互換カバレッジマップ

**このファイルが「V8 API 互換」の唯一の定義。** 互換の主張は下表の行だけ。
表にない API は互換でも非互換でもなく **サブセット外（未提供）** である。

## 背景と境界（正直な定義）

- V8 の公開 C++ API（include/v8*.h）は Context 多重化・Microtask・Module・
  GC scheduling・Snapshot・プラットフォーム抽象を含む巨大表面で、libstdc++ と
  ICU・プラットフォーム層を前提とする。akl の製品法則（100% self-made C11、
  JIT なし、ldd = linux-vdso/libc/(libm)/ld、libstdc++ 不導入）の下で
  ABI 互換は構造的に不可能であり非目標。
- したがって互換は **概念・型形状の C++ ヘッダ互換**（src/akl/v8.h、namespace v8）。
  header-only かつ libstdc++ シンボル不使用（`make cxxtest` が ldd を機械検査）。

## Coverage map

| V8 API | akl 側 | 状態 |
|---|---|---|
| `Isolate::New()/Dispose()` | 同名（AklRT を保持） | **形状互換**（CreateParams は未対応） |
| `Context::New(isolate)` / `Context::Scope` | 同名 | **形状互換**（単一 realm のため Scope はノーオペ） |
| `Local<T>`（IsEmpty/As） | 同名 | **形状互換**（8B cell 即値。ヒープ確保ゼロ） |
| `HandleScope` | 同名 | **形状のみ（RAII ノーオペ）**。根拠: akl の Local は GC ルートを張らない |
| `Value::IsNumber/IsBoolean/IsNull/IsUndefined` | 同名 | **同値** |
| `Value::IsString()` | `IsString(isolate)` | **形状偏差**（判定が engine obj 表参照のため Isolate 要） |
| `Value::NumberValue(ctx)/BooleanValue(ctx)` → `Maybe<T>` | 同名。`Maybe::FromMaybe/IsJust` | **同値** |
| `Number::New(iso, d)` / `Boolean::New(iso, b)` | 同名 | **同値** |
| `String::NewFromUtf8(iso, s[, len])` | 同名 | **互換**（engine GC ヒープに確保。未参照なら次回 GC 対象） |
| `String::Utf8Value`（data/length/非コピー） | 同名 | **互換**（malloc 写し。akl 文字列は外部 ptr を返さない設計） |
| `Undefined(iso)` / `Null(iso)` | 同名 | **同値** |
| `Script::Compile(ctx, src)` / `Script::Run(ctx)` | 同名 | **互換**。偏差: 構文エラーは Compile ではなく Run で顕在化（parse+run 一体エンジン） |
| `TryCatch` / `HasCaught` / `Reset` | 同名 | **互換**。捕捉対象は eval 失敗全般（構文・budget・未捕捉例外） |
| `TryCatch::Exception()`（例外「値」） | `ExceptionString()`（文字列） | **偏差**（akl は例外値を API に出さない。台帳: `akl_last_exception` API 候補） |
| `Isolate::Eval(src, &out)` | 存在 | akl 拡張（Script 形状を取らない最短経路。V8 に同名相当は無い） |

## サブセット外（未提供。互換と呼ばない）

Template/FunctionTemplate/ObjectTemplate、host Function 束縛・C++ コールバック経路
（DOM バインディング計画の本体）、`Message`/`StackTrace`、`Promise`/Microtask、`Module`、
`ArrayBuffer`/TypedArray、外部メモリ accounting、`Snapshot`/`CreateParams`、プラットフォーム
層（`V8::InitializePlatform` 等）、GC スケジューリング API、Persistent/Global handles、
`SetCaptureStackTrace`、--jitless 相当フラグ（akl は常時 no-JIT であり概念が要らない）。

## 実運用の不変条件（この層が侵さないもの）

- JIT なしはファサード層でも不変（実行可能書き込みページゼロ）。
- ldd: テストバイナリでさえ libstdc++ を引かない（`make cxxtest` が grep で機械検査）。
- 生成物の失敗規律: Compile/Run 失敗は empty return、値生成失敗は empty（err 設定済）。

## 検証

- `tests/cpp/v8_compat_smoke.cc`（33 checks）: 値往復・realm 独立性（別 Isolate の var は
  ReferenceError）・TryCatch 起動・Reset・Script 連続実行・失敗後の健全性を g++ C++11 で実動検証。
- 持込みで同定・修正した事前バグ: `akl_as_str` が前回 eval の残留 err で黙殺され
  ホストが読めなくなる問題（読取 API は自身の成否のみで報告する契約に修正）。
