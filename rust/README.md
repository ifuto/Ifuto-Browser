# Aklus Rust 移行（rust/）

Ifuto の自作 JS エンジン Aklus（`src/akl/akl.c`、C 約 19,000 行）のセキュリティ強化の
ための Rust 移行プロジェクト。**完全移行は段階的に進める**（ロードマップは
`docs/RUST_MIGRATION.md`）。

## クレート

| クレート | 内容 | 検証 |
|---|---|---|
| `akl-core` | 値表現（NaN-box AklVal）、タグ演算、int 加算 fast path | Kani（不変条件証明）/ Miri / Clippy |

## ローカルでの実行

```sh
cd rust
cargo test                    # ユニットテスト
cargo clippy -- -D warnings   # lint（警告 = エラー）
cargo +nightly miri test      # 未定義動作検出
cargo kani --workspace        # 機械的証明（Kani）
cargo geiger                  # unsafe 使用量カウント（このクレートは forbid(unsafe_code)）
```

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
