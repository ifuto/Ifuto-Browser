# GitHub Actions セットアップ手順（ユーザー作業）

ローカルサンドボックスは外部 HTTPS が全面遮断（rustup も apt も不可）のため、
Rust ツールチェーンと検証ツールの実行は **GitHub Actions に完全依存**している。

このリポジトリの bot トークンには `workflows` 権限が無く、
`.github/workflows/*.yml` の push が GitHub 側で拒否される（実測確認済み）。
**以下の 2 ファイルをユーザーが手動で配置する必要がある。**

## 配置手順

1. リポジトリの `.github/workflows/` に以下の 2 ファイルを作成（内容は
   このファイル末尾からコピー）:
   - `.github/workflows/env_trigger.yml`
   - `.github/workflows/trigger.yml`
2. push する（例: `git add .github && git commit -m "add actions" && git push`）
3. 動作確認:
   ```
   git add trigger/env_trigger.md && git commit -m "env trigger" && git push
   ```
   → 環境構築ワークフローが起動し、完了後 `env_status.md` が自動コミットされる。
4. 検証の実行:
   ```
   git add trigger/trigger.md && git commit -m "run verification" && git push
   ```
   → trigger.md のコマンド群が実行され、結果が `trigger/result.md` に追記される。

## ワークフロー設計（仕様）

### env_trigger.yml
- 発火: `trigger/env_trigger.md` の push（全ブランチ）
- 動作: `trigger/env_trigger.md` の `KEY=VALUE` 行を読み、`on` のツールをインストール
  - rustup stable/nightly、clippy、Kani（cargo-kani）、Miri、Rudra、cargo-audit、
    cargo-deny、cargo-geiger、Verus（VERUS=on 時のみ・約 1 時間）
- 出力: バージョン一覧を `env_status.md` に書いて自動コミット（トリガーパス外なので再発火しない）

### trigger.yml
- 発火: `trigger/trigger.md` の push（全ブランチ）
- 動作: trigger.md のコメント/空行を除いた各行を bash -c で順に実行
  （1 行 = 1 コマンド、失敗しても続行、終了コードを記録）
- 出力: 結果を `trigger/result.md` に追記して自動コミット。失敗があればジョブを失敗させる

### セキュリティ
- push イベントのみで発火（PR / fork からは発火しない）
- 実行内容はリポジトリ内ファイルのみ（外部入力なし）
- 使い捨て ubuntu-latest ランナー上で実行（ホストに影響なし）

## 現在の trigger.md の内容（Rust 移行フェーズ 0 検証）

```sh
cd rust && cargo build --workspace
cd rust && cargo test --workspace
cd rust && cargo clippy --workspace -- -D warnings
cd rust && cargo +nightly miri test --workspace
cd rust && cargo kani --workspace
cd rust && cargo geiger --workspace 2>/dev/null || true
```

---

## ファイル内容

### `.github/workflows/env_trigger.yml`

```yaml
# env_trigger.yml — 環境構築トリガー
#
# trigger/env_trigger.md を push すると、このワークフローが起動し、
# Rust ツールチェーンとセキュリティ検証ツール一式をインストールする。
#
# 使い方:
#   1. trigger/env_trigger.md の各ツールを on/off で切り替える
#   2. push する（例: git add trigger/env_trigger.md && git commit -m "env" && git push）
#   3. 結果は env_status.md に自動コミットされる（トリガーパス外なので再発火しない）
#
# セキュリティ:
#   - push イベントのみで発火（PR や fork からは発火しない）
#   - 環境構築は ubuntu-latest の使い捨てランナー上で行う（ホストに影響なし）
#   - インストールスクリプトは公式ソース（rustup.rs / crates.io / 公式リポジトリ）のみ

name: env-trigger

on:
  push:
    paths:
      - 'trigger/env_trigger.md'
    branches:
      - '**'

permissions:
  contents: write

jobs:
  setup:
    runs-on: ubuntu-latest
    timeout-minutes: 120
    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: トリガー設定を読む
        id: cfg
        run: |
          # trigger/env_trigger.md から KEY=VALUE 行を抽出（# コメントと空行は無視）
          if [ ! -f trigger/env_trigger.md ]; then
            echo "trigger/env_trigger.md がありません" >&2; exit 1
          fi
          grep -E '^[A-Z_]+=' trigger/env_trigger.md | sed 's/^/CFG_/' >> "$GITHUB_ENV"
          echo "--- 設定 ---"
          grep -E '^[A-Z_]+=' trigger/env_trigger.md
        shell: bash

      - name: rustup (stable + nightly)
        if: env.CFG_RUST_STABLE == 'on' || env.CFG_RUST_NIGHTLY == 'on'
        run: |
          curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable --profile minimal
          . "$HOME/.cargo/env"
          if [ "$CFG_RUST_NIGHTLY" = "on" ]; then rustup toolchain install nightly --profile minimal; fi
          rustc --version
          cargo --version

      - name: Clippy + rustfmt
        if: env.CFG_CLIPPY == 'on'
        run: |
          . "$HOME/.cargo/env"
          rustup component add clippy rustfmt

      - name: Kani (Rust 形式検証器)
        if: env.CFG_KANI == 'on'
        run: |
          . "$HOME/.cargo/env"
          cargo install --locked kani-verifier
          cargo kani --version

      - name: Miri (未定義動作検出)
        if: env.CFG_MIRI == 'on'
        run: |
          . "$HOME/.cargo/env"
          rustup component add miri --toolchain nightly
          cargo +nightly miri --version

      - name: Rudra (unsafe 静的解析)
        if: env.CFG_RUDRA == 'on'
        run: |
          # 注: rudra はメンテ停止中のため最新 rustc でビルドできないことがある。
          # 失敗してもジョブを止めない（Rust 検証は Kani/Miri/clippy が主軸）。
          . "$HOME/.cargo/env"
          cargo install rudra || echo "rudra install failed (known issue)"
          rudra --version || echo "rudra not available" 

      - name: cargo-audit (依存脆弱性スキャン)
        if: env.CFG_CARGO_AUDIT == 'on'
        run: |
          . "$HOME/.cargo/env"
          cargo install cargo-audit --locked
          cargo audit --version

      - name: cargo-deny (ライセンス/依存ポリシー)
        if: env.CFG_CARGO_DENY == 'on'
        run: |
          . "$HOME/.cargo/env"
          cargo install cargo-deny --locked
          cargo deny --version

      - name: cargo-geiger (unsafe 使用量カウント)
        if: env.CFG_CARGO_GEIGER == 'on'
        run: |
          . "$HOME/.cargo/env"
          cargo install cargo-geiger --locked
          cargo geiger --version

      - name: Verus (検証言語。ビルドに約 1 時間)
        if: env.CFG_VERUS == 'on'
        run: |
          sudo apt-get update -y
          sudo apt-get install -y curl python3 python3-pip ninja-build
          pip3 install --user vstatic 2>/dev/null || true
          git clone --depth 1 https://github.com/verus-lang/verus.git /tmp/verus
          cd /tmp/verus/source
          ./tools/setup.sh
          echo "Verus ビルド完了: /tmp/verus/source/target-verus/release/verus"

      - name: 動作確認（バージョン一覧）
        run: |
          . "$HOME/.cargo/env"
          {
            echo "# 環境構築結果 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"
            echo ""
            echo "| ツール | バージョン |"
            echo "|---|---|"
            echo "| rustc | $(rustc --version 2>/dev/null || echo '未導入') |"
            echo "| cargo | $(cargo --version 2>/dev/null || echo '未導入') |"
            echo "| kani | $(cargo kani --version 2>/dev/null || echo '未導入') |"
            echo "| miri | $(cargo +nightly miri --version 2>/dev/null || echo '未導入') |"
            echo "| rudra | $(rudra --version 2>/dev/null || echo '未導入') |"
            echo "| cargo-audit | $(cargo audit --version 2>/dev/null || echo '未導入') |"
            echo "| cargo-deny | $(cargo deny --version 2>/dev/null || echo '未導入') |"
            echo "| cargo-geiger | $(cargo geiger --version 2>/dev/null || echo '未導入') |"
          } > env_status.md

      - name: 結果をコミット
        run: |
          git config user.name "env-trigger-bot"
          git config user.email "actions@users.noreply.github.com"
          git add env_status.md
          if git diff --cached --quiet; then
            echo "変更なし（env_status.md は既に最新）"
          else
            git commit -m "env: 環境構築結果を更新 [skip ci]"
            git push
          fi
```

### `.github/workflows/trigger.yml`

```yaml
# trigger.yml — 汎用実行トリガー
#
# trigger/trigger.md を push すると、このワークフローが起動し、
# trigger.md に書かれたコマンド群を GitHub Actions ランナー上で順に実行する。
#
# 用途: ローカルサンドボックスでは実行できない作業
#   - 外部ネットワークアクセス（ツールのダウンロード、リポジトリ取得）
#   - 重いビルド（Verus 等）
#   - 大量リソースが必要な検証（Kani/Miri/Rudra のフル実行）
#
# trigger.md の書き方:
#   - `#` で始まる行はコメント
#   - 空行は無視
#   - それ以外の 1 行 = 1 コマンド（bash -c で実行。cwd はリポジトリルート）
#   - コマンドの終了コードが 0 以外でも続行し、最終的に失敗として報告する
#
# セキュリティ:
#   - push イベントのみで発火（PR / fork からは発火しない）
#   - 実行されるのはリポジトリ内の trigger.md の内容のみ（外部入力なし）
#   - 結果は trigger/result.md に自動追記コミットされる（トリガーパス外なので再発火しない）

name: trigger

on:
  push:
    paths:
      - 'trigger/trigger.md'
    branches:
      - '**'

permissions:
  contents: write

jobs:
  run-trigger:
    runs-on: ubuntu-latest
    timeout-minutes: 120
    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Rust ツールチェーンをセットアップ（ランナーは毎回クリーンのため自己完結）
        run: |
          curl --proto =https --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable --profile minimal
          . "$HOME/.cargo/env"
          rustup toolchain install nightly --profile minimal
          rustup component add clippy rustfmt
          rustup component add miri --toolchain nightly
          echo "RUSTUP_HOME=$HOME/.rustup" >> "$GITHUB_ENV"
          echo "CARGO_HOME=$HOME/.cargo" >> "$GITHUB_ENV"
          echo "$HOME/.cargo/bin" >> "$GITHUB_PATH"
          rustc --version
          cargo --version
          echo "Rust セットアップ完了"

      - name: Kani をインストール（cargo kani 用。数分かかる）
        run: |
          . "$HOME/.cargo/env"
          cargo install --locked kani-verifier 2>&1 | tail -3
          cargo kani --version || echo "kani install failed"

      - name: trigger.md の内容を実行
        id: run
        run: |
          set +e
          if [ ! -f trigger/trigger.md ]; then
            echo "trigger/trigger.md がありません" >&2; exit 1
          fi
          # コメント行と空行を除いたコマンド列
          grep -vE '^\s*(#|$)' trigger/trigger.md > /tmp/commands.txt
          if [ ! -s /tmp/commands.txt ]; then
            echo "実行するコマンドがありません（trigger.md はコメントのみ）"
            echo "commands=0" >> "$GITHUB_OUTPUT"
            exit 0
          fi
          n=0
          while IFS= read -r line; do
            n=$((n+1))
            echo "===== [$n] $line ====="
            bash -c "$line" > /tmp/out_$n.txt 2>&1
            rc=$?
            echo "----- exit=$rc -----"
            cat /tmp/out_$n.txt | tail -40
            echo "CMD_RESULT $n|$rc|$line" >> /tmp/cmd_results.txt
          done < /tmp/commands.txt
          echo "commands=$n" >> "$GITHUB_OUTPUT"
          # 全コマンドの結果を result.md に保存（後続ステップがコミット）
          {
            echo "# 実行結果 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"
            echo ""
            echo "source: trigger/trigger.md @ ${{ github.sha }}"
            echo ""
            while IFS='|' read -r idx rc cmd; do
              if [ "$rc" = "0" ]; then
                echo "- [x] [$idx] \`$cmd\` — exit 0"
              else
                echo "- [ ] [$idx] \`$cmd\` — **exit $rc**"
              fi
            done < /tmp/cmd_results.txt
          } > /tmp/result.md
          # 失敗があればジョブを失敗させる（result.md はコミットされる）
          if grep -q "exit [^0]" /tmp/result.md; then
            echo "status=fail" >> "$GITHUB_OUTPUT"
          else
            echo "status=ok" >> "$GITHUB_OUTPUT"
          fi
        shell: bash

      - name: 結果を trigger/result.md に追記コミット
        run: |
          mkdir -p trigger
          if [ -f trigger/result.md ]; then
            cat /tmp/result.md >> trigger/result.md
          else
            cp /tmp/result.md trigger/result.md
          fi
          git config user.name "trigger-bot"
          git config user.email "actions@users.noreply.github.com"
          git add trigger/result.md
          if git diff --cached --quiet; then
            echo "変更なし"
          else
            git commit -m "trigger: 実行結果を追記 [skip ci]"
            git push
          fi
        shell: bash

      - name: 失敗報告
        if: steps.run.outputs.status == 'fail'
        run: |
          echo "::error::trigger.md の一部コマンドが失敗しました。詳細は trigger/result.md を確認してください。"
          exit 1
```
