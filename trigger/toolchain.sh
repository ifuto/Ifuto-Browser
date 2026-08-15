#!/usr/bin/env bash
# =====================================================================
# toolchain.sh — Rust 検証ツールチェーンの「事前ビルド zip」管理（git ブランチ方式）
#
# アイデア（ユーザー提案）: ツールを毎回 cargo install でコンパイルするのではなく、
# 一度ビルドして tar.gz にし、分割して git ブランチ toolchain-bin に置く。
# 次回以降は git fetch で取得して展開するだけで使える。
#   - Actions ランナー: このスクリプトが取得 or ビルド（checkout の認証で push）
#   - サンドボックス:  git fetch origin toolchain-bin で取得 → trigger/fetch-toolchain.sh
#
# なぜ git ブランチか: Actions の GITHUB_TOKEN は gh CLI に渡らない（実測）が、
# checkout が設定する git の extraheader 認証はそのまま git push に使える。
# また GitHub Release のアセット CDN はサンドボックスから遮断されているが、
# github.com への git は通る（サンドボックスでも取得可能）。
# =====================================================================
set -uo pipefail

TOOL_TAG="${TOOL_TAG:-toolchain-v1}"
TOOL_DIR="${TOOL_DIR:-$HOME/akl-toolchain}"
ARCHIVE="akl-toolchain.tar.gz"
BIN_BRANCH="toolchain-bin"

mkdir -p "$TOOL_DIR"
REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || echo "$PWD")"

# ---------------- 1. 既存ブランチがあれば取得 ----------------
if git -C "$REPO_ROOT" fetch origin "$BIN_BRANCH:refs/remotes/origin/$BIN_BRANCH" 2>&1 | tail -1 >/dev/null 2>&1; then
  NPARTS=$(git -C "$REPO_ROOT" ls-tree --name-only "origin/$BIN_BRANCH" 2>/dev/null | grep -c '^part_' || true)
  if [ "$NPARTS" -gt 0 ]; then
    echo "==> ブランチ $BIN_BRANCH にアーカイブ（$NPARTS 分割）があります。取得して展開します"
    mkdir -p "$TOOL_DIR/parts"
    for f in $(git -C "$REPO_ROOT" ls-tree --name-only "origin/$BIN_BRANCH" | grep '^part_'); do
      git -C "$REPO_ROOT" show "origin/$BIN_BRANCH:$f" > "$TOOL_DIR/parts/$f" || { echo "FATAL: $f の取得失敗"; exit 1; }
    done
    cat "$TOOL_DIR"/parts/part_* > "$TOOL_DIR/$ARCHIVE"
    rm -rf "$TOOL_DIR/parts"
    tar xzf "$TOOL_DIR/$ARCHIVE" -C "$TOOL_DIR" || { echo "FATAL: 展開失敗"; exit 1; }
    if [ -d "$TOOL_DIR/.kani" ] && [ ! -e "$HOME/.kani" ]; then
      ln -s "$TOOL_DIR/.kani" "$HOME/.kani" 2>/dev/null || true
    fi
    echo "==> 展開完了"
    "$TOOL_DIR/cargo/bin/rustc" --version || echo "(rustc が見つかりません)"
    exit 0
  fi
fi

# ---------------- 2. ビルド（初回のみ） ----------------
echo "==> ブランチ $BIN_BRANCH が無いためビルドします（初回のみ・約 10 分）"
export RUSTUP_HOME="$TOOL_DIR/rustup"
export CARGO_HOME="$TOOL_DIR/cargo"
export PATH="$CARGO_HOME/bin:$PATH"

curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable --profile minimal
rustup toolchain install nightly --profile minimal
rustup component add clippy rustfmt
rustup component add miri --toolchain nightly
rustc --version
cargo --version

cargo install kani-verifier 2>&1 | tail -3 || echo "kani: INSTALL FAILED"
cargo kani --version 2>&1 || echo "kani: unavailable"

cargo install cargo-tarpaulin 2>&1 | tail -2 || echo "tarpaulin: skip"
cargo install cargo-geiger --locked 2>&1 | tail -2 || echo "geiger: skip"
cargo install cargo-fuzz 2>&1 | tail -2 || echo "cargo-fuzz: skip"
cargo install --git https://github.com/flux-rs/flux flux 2>&1 | tail -2 || echo "flux: skip"
cargo install mirai 2>&1 | tail -2 || echo "mirai: skip"
cargo install --locked prusti --version 1.0.0 2>&1 | tail -2 || echo "prusti: skip"

# ---------------- 3. 必須チェック ----------------
if [ ! -x "$TOOL_DIR/cargo/bin/rustc" ]; then
  echo "FATAL: rustc がありません（rustup インストール失敗）"
  exit 1
fi

# ---------------- 4. アーカイブ作成 ----------------
rm -rf "$TOOL_DIR/cargo/registry" "$TOOL_DIR/cargo/git" 2>/dev/null || true
if [ -d "$HOME/.kani" ]; then cp -r "$HOME/.kani" "$TOOL_DIR/.kani"; fi
echo "==> アーカイブ作成"
tar czf "$TOOL_DIR/$ARCHIVE" -C "$TOOL_DIR" rustup cargo .kani 2>/dev/null || \
  tar czf "$TOOL_DIR/$ARCHIVE" -C "$TOOL_DIR" rustup cargo
ls -lh "$TOOL_DIR/$ARCHIVE"

# ---------------- 5. 分割して git ブランチへ push ----------------
SPLIT_DIR="$TOOL_DIR/parts"
rm -rf "$SPLIT_DIR"; mkdir -p "$SPLIT_DIR"
split -b 90m -d -a 2 "$TOOL_DIR/$ARCHIVE" "$SPLIT_DIR/part_"
echo "==> 分割: $(ls "$SPLIT_DIR" | wc -l) 個"

git -C "$REPO_ROOT" worktree add "$TOOL_DIR/wt" "$BIN_BRANCH" 2>/dev/null || \
  git -C "$REPO_ROOT" worktree add "$TOOL_DIR/wt" -b "$BIN_BRANCH"
rm -f "$TOOL_DIR/wt"/part_*
cp "$SPLIT_DIR"/part_* "$TOOL_DIR/wt/"
if ( cd "$TOOL_DIR/wt" && \
     git config user.name "toolchain-bot" && \
     git config user.email "actions@users.noreply.github.com" && \
     git add part_* && \
     git commit -m "toolchain $TOOL_TAG parts [skip ci]" && \
     git push origin "$BIN_BRANCH" ); then
  echo "==> ブランチ $BIN_BRANCH へのアップロード完了"
else
  echo "WARNING: ブランチ push に失敗（次回は再ビルドになります）"
fi
git -C "$REPO_ROOT" worktree remove "$TOOL_DIR/wt" --force 2>/dev/null || true
