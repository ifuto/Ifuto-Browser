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

# --- 5. Miri（未定義動作検出。nightly で実行。ログも result.md へ） ---
# timeout は 600 → 1500 に拡大（2026-08-28: workspace 全件通過に 600s では不足し、
# ifuto-core 183 件の途中で time kill されていた。データ重い掃引テストは cfg(miri)
# で縮小済みだが、sysroot 構築 + 330 件の解釈実行は分単位が常態）。
cd rust && ../trigger/tc timeout 1500 cargo +nightly miri test --workspace > /tmp/miri.log 2>&1; rc=$?; { echo ""; echo "### miri 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -80 /tmp/miri.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0

# --- 6. Kani（機械的証明。スカラー純粋関数のみ） ---
cd rust && ../trigger/tc timeout 900 cargo kani --workspace

# --- 7. cargo-geiger（unsafe 使用量カウント。未導入なら許容） ---
cd rust && (../trigger/tc timeout 300 cargo geiger --workspace 2>/dev/null || true)

# --- 8. cargo-tarpaulin（カバレッジ。未導入なら許容） ---
cd rust && (../trigger/tc timeout 300 cargo tarpaulin --workspace 2>&1 | tail -12 || true)

# --- 9. cargo-fuzz（ファジング基盤の存在確認。未導入なら許容） ---
../trigger/tc cargo fuzz --version 2>/dev/null || echo "cargo-fuzz not installed (skipped)"

# --- 10. Flux / MIRAI / Prusti（存在確認のみ。未導入なら許容） ---
../trigger/tc flux --version 2>/dev/null || echo "flux not installed (skipped)"
../trigger/tc mirai --version 2>/dev/null || echo "mirai not installed (skipped)"
../trigger/tc cargo prusti --version 2>/dev/null || ../trigger/tc prusti-rustc --version 2>/dev/null || echo "prusti not installed (skipped)"

# --- 実行ログ: 2026-08-15 git ブランチ方式 + bytecode 型修正後 ---

# --- 実行ログ: 2026-08-15 rust-src 補完 ---

# --- 実行ログ: 2026-08-28 フェーズ 8-z/9-a 反映（8-z byte-exact + chrome 純粋部 + akl-ffi leak 全廃）後の最終確認 ---

# 2026-08-28 再実行: フェーズ 10-a（render 行スイープ一本化）+ akl-ffi into_raw 化（Miri/Tree Borrows 適合）反映後の検証。特に CMD 5 Miri の結果を確認する。

# 2026-08-28 再々実行: フェーズ 10-b（style lazy 構造消去）+ Miri float intrinsic 近似ずれ対処（2 件 ignore）反映後。CMD 5 Miri の緑化を確認する。

# 2026-08-28 再実行: フェーズ 10-c（layout 座標化）+ 10-d（parse NameStr/2-slice）反映後の検証。CMD 5 Miri（2-slice の std::thread::scope / NameStr の Drop 走査）と CMD 3 test（340 緑）に注目。

# 2026-08-28 再実行: akl-ffi コールバックアダプタの Stacked Borrows 根治（参照不生成の生経路化: 180062d）反映後。CMD 5 Miri の緑化を確認（残存指摘があれば同箇所で再発する）。

# 2026-08-28 再実行: akl_native_register &str 往復の SB 違反根治反映後。CMD 5 Miri の full green を確認。

# 2026-08-28 再実行: handle_roundtrip Box::leak 廃止（Miri 漏洩検査）反映後。CMD 5 Miri の full green を確認。
