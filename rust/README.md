# Aklus Rust 移行（rust/）

Ifuto の自作 JS エンジン Aklus（`src/akl/akl.c`、C 約 19,000 行）のセキュリティ強化の
ための Rust 移行プロジェクト。**完全移行は段階的に進める**（ロードマップは
`docs/RUST_MIGRATION.md`）。

## クレート

| クレート | 内容 | 検証 |
|---|---|---|
| `akl-core` | 値表現（NaN-box AklVal）、文字列インターン、オブジェクトモデル + GC | Kani（不変条件証明）/ Miri / Clippy |

## ローカルでの実行

```sh
cd rust
cargo test                    # ユニットテスト
cargo clippy -- -D warnings   # lint（警告 = エラー）
cargo +nightly miri test      # 未定義動作検出
cargo kani --workspace        # 機械的証明（Kani）
cargo geiger                  # unsafe 使用量カウント（このクレートは forbid(unsafe_code)）
```

## フェーズ進捗

| フェーズ | モジュール | 内容 | Kani 証明 |
|---|---|---|---|
| 0 | `lib.rs` | AklVal（NaN-box newtype）、int_add fast path | 7 件（タグ排他・往復・canonical NaN・overflow 判定） |
| 1 | `string.rs` | Interner（HashMap ベース。一意性を型で保証） | 4 件（一意性・往復・単射・空文字列） |
| 2 | `obj.rs` | Obj enum + ObjTable + mark&sweep GC | 3 件（GC 安全性・free-list 整合・alloc 妥当性） |

### 各フェーズが C の実バグをどう構造的に排除するか

- フェーズ 0: 「演算結果 double がタグ空間に衝突する」経路 → `from_f64` の
  NaN canonical 化を Kani で証明
- フェーズ 1: 「同一文字列が 2 つの intern id を持つ」（v0.10 実測: "TypeError" が
  id 292/293 に分裂）→ HashMap の get-or-insert が全単射を保証
- フェーズ 2: 「コールバック中 GC 後の use-after-free」（v0.10 実測: 配列 HOF で
  ASan 検出）→ `&Rt` / `&mut Rt` の借用規則でコンパイル不能。子参照の列挙漏れも
  match の網羅性でコンパイルエラー

## GitHub Actions

ローカルサンドボックスには Rust ツールチェーンが無いため、検証は GitHub Actions で行う:

1. `trigger/env_trigger.md` を push → 環境構築ワークフローが Rust/Kani/Miri/Rudra 等を
   インストール（結果は `env_status.md`）
2. `trigger/trigger.md` を push → 上記の検証コマンド群をランナー上で実行
   （結果は `trigger/result.md`）

## 設計方針

- `#![forbid(unsafe_code)]`: このクレートに unsafe を置かない（Rudra / geiger で監査可能）
- ビットレイアウトは C 実装と 1:1 対応（回帰テストで突合可能）
- 不変条件は Kani の `#[kani::proof]` で機械的に証明（`akl-core/src/lib.rs` 末尾）
