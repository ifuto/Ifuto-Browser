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

- `Op` enum（C の OP_* のコアサブセット: ConstI/LLoad/LStore/GLoadS/GStoreS/Add/Sub/
  Mul/CJmpfL/Jmp/Pop/Dup/Halt）
- `Vm::step` は 1 命令実行（`Vec<i64>` スタック。C の AKL_POP マクロの下限検査が
  `get()` の Option で構造的に安全）
- `Op::stack_effect` は pop/push 数を 1 箇所に集約（C の akl_op_imm_len 表の
  drift 問題を enum match の網羅性で排除）
- `verify_stack` は C の akl_verify 相当の静的スタック検査
- `verify_control_flow`（3b 着手）: ジャンプ先整合（全 `Jmp`/`CJmpfL` の tgt が
  0..=len）+ 制御フローグラフ上のスタック深さ整合（worklist 走査）。C の
  `akl_verify` の「ジャンプ先は命令境界かつ範囲内」に相当。分岐合流の深さ不一致は
  `VmError::ControlFlowMismatch` として検出（順次走査の verify_stack では見逃す
  不正バイトコードを構造的に排除）
- Kani 証明 8 件: stack_effect 有界 / Cmp 全数 / checked_add 正確性 /
  verify_stack の検出 / verify_control_flow の jump-oob・mismatch・balanced
  （全てスカラー範囲）

残り（フェーズ 3b）: 関数呼び出し（CALL/RET/frames）・文字列/オブジェクト値を扱う命令。

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
