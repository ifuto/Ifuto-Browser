# env_trigger.md — 環境構築トリガー設定
#
# このファイルを push すると、GitHub Actions が Rust ツールチェーンと
# セキュリティ検証ツールを ubuntu-latest ランナーにインストールする。
# 結果は env_status.md に自動コミットされる。
#
# 各ツールの on/off を切り替えて push するだけで再実行できる。
# 例: VERUS=on にして push すると Verus（検証言語）のビルドが始まる（約 1 時間）。

RUST_STABLE=on
RUST_NIGHTLY=on
CLIPPY=on
KANI=on
MIRI=on
# Rudra はメンテ停止中のため最新 rustc でビルド不能（Actions 実走で確認）。off にすると
# env_trigger.yml の該当ステップがスキップされ、ジョブ全体は成功する。
RUDRA=off
CARGO_AUDIT=on
CARGO_DENY=on
CARGO_GEIGER=on
# ビルドに約 1 時間かかるため、必要な時だけ on にすること
VERUS=off

# 最終発火: 2026-08-14（ユーザーが yml を配置した後の確認）
