# trigger.md — 汎用実行トリガー
#
# このファイルを push すると、以下のコマンド群が GitHub Actions ランナー上で
# 順に実行される（1 行 = 1 コマンド、`#` はコメント、空行は無視）。
# 結果は trigger/result.md に自動追記される。
#
# 各行は独立した bash -c で実行されるため、PATH 設定・cd は各行に前置する。
# ツールチェーン整備もここで行う（ランナーは毎回クリーン。ubuntu-latest には
# stable Rust がプリインストール済みなので、nightly/miri/clippy/kani を追加する）。

# --- 1. ツールチェーン整備（nightly / miri / clippy） ---
export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; rustup toolchain install nightly --profile minimal && rustup component add clippy rustfmt && rustup component add miri --toolchain nightly && rustc --version && cargo --version

# --- 2. Kani インストール（初回は数分かかる） ---
export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cargo install --locked kani-verifier 2>&1 | tail -3 && cargo kani --version

# --- 3. build ---
export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && cargo build --workspace

# --- 4. test ---
export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && cargo test --workspace

# --- 5. clippy（警告 = エラー） ---
export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && cargo clippy --workspace -- -D warnings

# --- 6. Miri（未定義動作検出。nightly で実行） ---
export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && cargo +nightly miri test --workspace

# --- 7. Kani（機械的証明） ---
export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && cargo kani --workspace

# --- 8. cargo-geiger（unsafe 使用量カウント。未導入なら許容） ---
export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && (cargo geiger --workspace 2>/dev/null || true)
