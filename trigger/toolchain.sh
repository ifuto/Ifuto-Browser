#!/usr/bin/env bash
# =====================================================================
# toolchain.sh — Rust 検証ツールチェーンの「事前ビルド zip」管理
#
# アイデア（ユーザー提案）: ツールを毎回 cargo install でコンパイルするのではなく、
# 一度ビルドして tar.gz にし、GitHub Release に置く。次回以降はダウンロードして
# 展開するだけで使える（Kani のビルド 5-10 分 → 展開 1-2 分）。
#
# 動作:
#   1. Release "$TOOL_TAG" が存在する → ダウンロードして $TOOL_DIR に展開
#   2. 存在しない → rustup/nightly/miri/clippy/kani/tarpaulin/geiger/cargo-fuzz/
#      flux/mirai/prusti をビルドし、tar.gz にして Release を作成
#
# 使い方（GitHub Actions ランナー上）: bash trigger/toolchain.sh
# TOOL_TAG を変えたら新しいアーカイブをビルドし直す（例: toolchain-v2）
# =====================================================================
set -uo pipefail

TOOL_TAG="${TOOL_TAG:-toolchain-v1}"
TOOL_DIR="${TOOL_DIR:-$HOME/akl-toolchain}"
ARCHIVE="akl-toolchain.tar.gz"
REPO="${GITHUB_REPOSITORY:-}"

mkdir -p "$TOOL_DIR"
cd "$TOOL_DIR"

# ---------------- 1. 既存リリースがあればダウンロード ----------------
if gh release view "$TOOL_TAG" ${REPO:+-R "$REPO"} >/dev/null 2>&1; then
  echo "==> Release $TOOL_TAG が見つかりました。ダウンロードして展開します"
  gh release download "$TOOL_TAG" ${REPO:+-R "$REPO"} -p "$ARCHIVE" -D . --clobber
  tar xzf "$ARCHIVE"
  # .kani（CBMC 等）は $HOME 直下に symlink する
  if [ -d "$TOOL_DIR/.kani" ] && [ ! -e "$HOME/.kani" ]; then
    ln -s "$TOOL_DIR/.kani" "$HOME/.kani" 2>/dev/null || true
  fi
  echo "==> 展開完了"
  "$TOOL_DIR/cargo/bin/rustc" --version || echo "(rustc が見つかりません)"
  exit 0
fi

# ---------------- 2. ビルド（初回のみ） ----------------
echo "==> Release $TOOL_TAG が無いためビルドします（初回のみ・数十分）"
export RUSTUP_HOME="$TOOL_DIR/rustup"
export CARGO_HOME="$TOOL_DIR/cargo"
export PATH="$CARGO_HOME/bin:$PATH"

# rustup stable + nightly
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable --profile minimal
rustup toolchain install nightly --profile minimal
rustup component add clippy rustfmt
rustup component add miri --toolchain nightly
rustc --version
cargo --version

# 必須: Kani（形式検証）
cargo install kani-verifier 2>&1 | tail -3 || echo "kani: INSTALL FAILED"
cargo kani --version 2>&1 || echo "kani: unavailable"

# 補助（失敗しても続行。アーカイブには入るものだけ入る）
cargo install cargo-tarpaulin 2>&1 | tail -2 || echo "tarpaulin: skip"
cargo install cargo-geiger --locked 2>&1 | tail -2 || echo "geiger: skip"
cargo install cargo-fuzz 2>&1 | tail -2 || echo "cargo-fuzz: skip"
cargo install --git https://github.com/flux-rs/flux flux 2>&1 | tail -2 || echo "flux: skip"
cargo install mirai 2>&1 | tail -2 || echo "mirai: skip"
cargo install --locked prusti --version 1.0.0 2>&1 | tail -2 || echo "prusti: skip"

# CBMC（kani が初回実行時に落とす）があれば一緒に固める
if [ -d "$HOME/.kani" ]; then
  cp -r "$HOME/.kani" "$TOOL_DIR/.kani"
fi

# ---------------- 3. 必須チェック ----------------
if [ ! -x "$TOOL_DIR/cargo/bin/rustc" ]; then
  echo "FATAL: rustc がありません（rustup インストール失敗）"
  exit 1
fi
if ! "$TOOL_DIR/cargo/bin/cargo" kani --version >/dev/null 2>&1; then
  echo "WARNING: kani が利用できません（後続の kani 検証は失敗します）"
fi

# ---------------- 4. アーカイブ作成 + リリース ----------------
echo "==> アーカイブ作成"
tar czf "$TOOL_DIR/$ARCHIVE" -C "$TOOL_DIR" rustup cargo .kani 2>/dev/null || \
  tar czf "$TOOL_DIR/$ARCHIVE" -C "$TOOL_DIR" rustup cargo
ls -lh "$TOOL_DIR/$ARCHIVE"

gh release create "$TOOL_TAG" "$TOOL_DIR/$ARCHIVE" ${REPO:+-R "$REPO"} \
  --title "Akl toolchain $TOOL_TAG" \
  --notes "事前ビルド済み Rust 検証ツールチェーン（stable/nightly + miri + clippy + kani + tarpaulin + geiger + cargo-fuzz + flux + mirai + prusti）。trigger/toolchain.sh が自動で取得・展開する。"
echo "==> Release $TOOL_TAG 作成完了"
