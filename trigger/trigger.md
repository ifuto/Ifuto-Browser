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
# 【運用変更 2026-08-29: 普段は実行しない】workspace 全件の解釈実行は 3000s キャップを超過し、
# run 全体 52分07秒 の 96%（3000s）を Miri 打ち切りが占有した実績のため（run 33239097931:
# ifuto-ffi 188 件通過 1117s の後、ifuto-core/akl 未着手で time kill → exit 1。UB 検出は 0 件）。
# そもそも ifuto-core は #![forbid(unsafe_code)] で Miri の検査対象（unsafe）を持たず、
# UB リスクは unsafe 含有 crate（akl-core/akl-ffi/ifuto-ffi）にのみ存在する。
# よって運用: 普段の trigger では下行は無効化（コメントアウト）とし、
# unsafe 含有 crate に差分がある時だけ有効化して実行する（scoped でも 20 分超を見込むこと）。
# 有効化時の CMD:
# cd rust && ../trigger/tc timeout 2400 cargo +nightly miri test -p akl-core -p akl-ffi -p ifuto-ffi > /tmp/miri.log 2>&1; rc=$?; { echo ""; echo "### miri 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -80 /tmp/miri.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0
echo "miri: skipped（運用規約: unsafe 含有 crate 差分時のみ有効化。trigger.md CMD 5 コメント参照）"

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

# 2026-08-28 再実行: Miri time kill 対策（掃引 cfg(miri) 縮小 + timeout 1500）反映後。CMD 5 Miri の full green を result.md 行レベルで確認する。

# 2026-08-28 再実行: md 2-slice Miri 8KB 化 + timeout 3000 反映後。CMD 5 Miri の full green を result.md 行レベルで確認する。

# 2026-08-29 実行: フェーズ 10-e（Node 痩身化 200B→80B）反映後の検証。特に CMD 5 Miri（副テーブル/merge_side_from/8KB化テスト群）と CMD 3 test=342 緑に注目。

# 2026-08-29 実行: フェーズ 10-f（layout アロケーション段撲滅: ifc pieces/segs スクラッチ + Winner スクラッチ）反映後の検証。特に CMD 5 Miri（RefCell スクラッチ take/返却経路）と CMD 3 test 緑に注目。

# 2026-08-29 実行: フェーズ 10-g（2-slice parse 実益化・既定ON転換）反映後の検証。特に CMD 5 Miri（drain ext drain/スレッド境界の新経路）と CMD 3 test 緑に注目。

# 2026-08-29 実行: フェーズ 10-h（body shard 2-way 並列 layout 移植）反映後の検証。特に CMD 5 Miri（thread::scope + RefCell 専有 streams の新経路、shard_layout_equals_serial）と CMD 3 test=343 緑に注目。

# 2026-08-29 実行: フェーズ 10-i（render 2-way 並列 sweep）反映後の検証。特に CMD 5 Miri（sweep range 分割スレッド）と CMD 3 test=344 緑に注目。

# 2026-08-29 実行: フェーズ 11（テキストアリーナ: text pun fields + arena 予約）反映後の検証。特に CMD 5 Miri（text_of/pun 転用、splice の排他算術 remap）と CMD 3 test=345 緑に注目。

# 2026-08-29 実行: フェーズ 12-a（split_cells スライス化 + ln_is_table_delim ゼロアロケ化 + Fn 借用化）反映後の検証。
# CMD 3 test=345 緑に注目。CMD 5 Miri は運用変更（上記）により skip —— 12-a 差分は ifuto-core（forbid(unsafe_code)）のみで
# unsafe 含有 crate は無差分。前 run 33239097931 の Miri exit 1 は timeout 打ち切り（UB 検出 0）であり、
# 解釈実行全件掃引の常設は構造的に遅すぎるため常設廃止（run 52分07秒 → 本 profile では 2-5 分見込み）。

# 2026-08-29 実行: フェーズ 12-b（属性機構ゼロアロケ化: Attr NameStr 化 + elem_store move 化 + merge_attrs 一時Vec消去）反映後の
# 検証。CMD 3 test=345 緑に注目。CMD 5 Miri は引き続き skip が正（12-b 差分も unsafe 非含有 crate のみ。
# html_tok/dom/md/html_tree の Attr 表現変更だが全て ifuto-core: forbid(unsafe_code)）。

# 【運用規約】trigger 発火後は run 完了（result.md 追記）まで trigger.md 以外の push を避けること。
# 旧 append 実装は run 中の push で non-fast-forward となり結果ブロック喪失（10-e, 10-g/h/i で実績）。
# 根治版（最新先頭取り直し+retry 化）の修正は .github/workflows 配下のため GitHub App トークンの
# workflows スコープ不足で push 不可（2026-08-29 確認）。ローカル修正は /tmp/trigger.yml.proposed に退避。
