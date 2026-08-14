# trigger.md — 汎用実行トリガー
#
# このファイルを push すると、以下のコマンド群が GitHub Actions ランナー上で
# 順に実行される（1 行 = 1 コマンド、`#` はコメント、空行は無視）。
# 結果は trigger/result.md に自動追記される。
#
# ローカルサンドボックスでできないこと（外部アクセス・重い検証）をここに書く。

# --- Rust 移行フェーズ 0-2: akl-core の全検証 ---

cd rust && cargo build --workspace
cd rust && cargo test --workspace
cd rust && cargo clippy --workspace -- -D warnings
cd rust && cargo +nightly miri test --workspace
cd rust && cargo kani --workspace
cd rust && cargo geiger --workspace 2>/dev/null || true
