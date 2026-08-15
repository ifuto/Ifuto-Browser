# trigger.md — 汎用実行トリガー
#
# このファイルを push すると、以下のコマンド群が GitHub Actions ランナー上で
# 順に実行される（1 行 = 1 コマンド、`#` はコメント、空行は無視）。
# 結果は trigger/result.md に自動追記される。
#
# ツールチェーンは「事前ビルド zip」方式（ユーザー提案）:
#   - 1 行目が trigger/toolchain.sh を実行し、GitHub Release（toolchain-v1）の
#     アーカイブを取得して展開する（無ければビルドしてリリースに置く）
#   - 以降の行は trigger/tc ラッパー（PATH/RUSTUP_HOME/CARGO_HOME 設定済み）で実行
# これにより毎回の cargo install（Kani で 5-10 分）が不要になり、1-2 分で検証に入れる。

# --- 1. ツールチェーン取得（無ければビルドして Release にアップロード） ---
bash trigger/toolchain.sh > /tmp/tc.log 2>&1; rc=$?; { echo ""; echo "### toolchain 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ)) rc=$rc"; echo '```'; tail -80 /tmp/tc.log; echo '```'; } >> trigger/result.md; test $rc -eq 0

# --- 2. build ---
cd rust && ../trigger/tc timeout 300 cargo build --workspace

# --- 3. test（全ユニットテスト） ---
cd rust && ../trigger/tc timeout 300 cargo test --workspace

# --- 4. clippy（警告 = エラー。ログを result.md に追記） ---
cd rust && ../trigger/tc timeout 300 cargo clippy --workspace -- -D warnings > /tmp/clippy.log 2>&1; rc=$?; { echo ""; echo "### clippy 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -50 /tmp/clippy.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0

# --- 5. Miri（未定義動作検出。nightly で実行） ---
cd rust && ../trigger/tc timeout 600 cargo +nightly miri test --workspace

# --- 6. Kani（機械的証明。スカラー純粋関数のみ） ---
cd rust && ../trigger/tc timeout 900 cargo kani --workspace

# --- 7. cargo-geiger（unsafe 使用量カウント。未導入なら許容） ---
cd rust && (trigger/tc timeout 300 cargo geiger --workspace 2>/dev/null || true)

# --- 8. cargo-tarpaulin（カバレッジ。未導入なら許容） ---
cd rust && (trigger/tc timeout 300 cargo tarpaulin --workspace 2>&1 | tail -12 || true)

# --- 9. cargo-fuzz（ファジング基盤の存在確認。未導入なら許容） ---
../trigger/tc cargo fuzz --version 2>/dev/null || echo "cargo-fuzz not installed (skipped)"

# --- 10. Flux / MIRAI / Prusti（存在確認のみ。未導入なら許容） ---
../trigger/tc flux --version 2>/dev/null || echo "flux not installed (skipped)"
../trigger/tc mirai --version 2>/dev/null || echo "mirai not installed (skipped)"
../trigger/tc cargo prusti --version 2>/dev/null || ../trigger/tc prusti-rustc --version 2>/dev/null || echo "prusti not installed (skipped)"
