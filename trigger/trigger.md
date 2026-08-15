# trigger.md — 汎用実行トリガー
#
# このファイルを push すると、以下のコマンド群が GitHub Actions ランナー上で
# 順に実行される（1 行 = 1 コマンド、`#` はコメント、空行は無視）。
# 結果は trigger/result.md に自動追記される。
#
# 各行は独立した bash -c で実行される。ツールチェーン整備もここで行う
# （ランナーは毎回クリーン。ubuntu-latest には stable Rust がプリインストール）。
# 全コマンドに timeout を付け、ハングしても次へ進む（Kani のハング事故対策）。

# --- 1. ツールチェーン整備（nightly / miri / clippy / prusti） ---
export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; rustup toolchain install nightly --profile minimal && rustup component add clippy rustfmt && rustup component add miri --toolchain nightly && rustc --version && cargo --version

# --- 2. Kani インストール（cargo kani 用。初回 5-10 分。失敗時も続行） ---
export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; timeout 600 bash -c 'cargo install --locked kani-verifier 2>&1 | tail -5; cargo kani --version || echo "kani unavailable"'

# --- 3. build（スカラー検証のベース） ---
export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo build --workspace

# --- 4. test（全ユニットテスト） ---
export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo test --workspace

# --- 5. clippy（警告 = エラー。ログを result.md に追記してコミットに載せる） ---
export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo clippy --workspace -- -D warnings > /tmp/clippy.log 2>&1; rc=$?; { echo ""; echo "### clippy 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -50 /tmp/clippy.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0

# --- 6. Miri（未定義動作検出。nightly で実行） ---
export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 600 cargo +nightly miri test --workspace

# --- 7. Kani（機械的証明。スカラー純粋関数のみ。ハング防止の timeout 付き） ---
export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 900 cargo kani --workspace

# --- 8. Prusti（契約検証。viper/z3 依存で初回インストールに数分） ---
export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; timeout 900 bash -c 'cargo install --locked prusti --version 1.0.0 2>&1 | tail -3 || echo "prusti install failed (version pin)"; cargo prusti --version 2>/dev/null || true'

# --- 9. cargo-geiger（unsafe 使用量カウント。未導入なら許容） ---
export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && (timeout 300 cargo geiger --workspace 2>/dev/null || true)


# --- 実行ログ: 2026-08-15 clippy 修正後（identity_op / missing_docs / collapsible_if / ObjError） ---
