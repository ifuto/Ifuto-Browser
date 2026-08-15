#!/usr/bin/env bash
# fetch-toolchain.sh — サンドボックス用: git ブランチ toolchain-bin から
# ツールチェーンを取得して展開する（github.com への git は通るが Release CDN は遮断のため）。
# 使い方: bash trigger/fetch-toolchain.sh [展開先]
set -euo pipefail
TOOL_DIR="${1:-/tmp/akl-toolchain}"
REPO_ROOT="$(git rev-parse --show-toplevel)"
BR="toolchain-bin"

echo "==> $BR ブランチを取得します（760MB 前後・初回は時間がかかる）"
git -C "$REPO_ROOT" fetch origin "$BR:refs/remotes/origin/$BR" 2>&1 | tail -1 2>&1 | tail -1 || { echo "FATAL: fetch 失敗"; exit 1; }
mkdir -p "$TOOL_DIR/parts"
for f in $(git -C "$REPO_ROOT" ls-tree --name-only "origin/$BR" | grep '^part_'); do
  git -C "$REPO_ROOT" show "origin/$BR:$f" > "$TOOL_DIR/parts/$f"
done
cat "$TOOL_DIR"/parts/part_* > "$TOOL_DIR/akl-toolchain.tar.gz"
rm -rf "$TOOL_DIR/parts"
tar xzf "$TOOL_DIR/akl-toolchain.tar.gz" -C "$TOOL_DIR"
if [ -d "$TOOL_DIR/.kani" ] && [ ! -e "$HOME/.kani" ]; then
  ln -s "$TOOL_DIR/.kani" "$HOME/.kani" 2>/dev/null || true
fi
echo "==> 展開完了: $TOOL_DIR"
"$TOOL_DIR/cargo/bin/rustc" --version
echo "使い方: RUSTUP_HOME=$TOOL_DIR/rustup CARGO_HOME=$TOOL_DIR/cargo PATH=$TOOL_DIR/cargo/bin:\$PATH cargo test"
