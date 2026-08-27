# Aklus / Ifuto Rust 移行（rust/）

Ifuto の自作 JS エンジン Aklus（`src/akl/akl.c`、C 約 19,000 行）のセキュリティ強化と、
ブラウザ本体（HTML/CSS/layout/render/CLI）の C→Rust 移行プロジェクト。
**完全移行は段階的に進める**（ロードマップと全フェーズ記録は `docs/RUST_MIGRATION.md`）。

現在地（2026-08-24、フェーズ 8-z）: **CLI 全観測モードが Rust 単体で完動し、
C バイナリを回帰オラクルとした差分 fuzz で累計 98,133 cases / 0 mismatch
（stdout/stderr/rc byte-exact）**。未移植は GUI（`--gui` / `--shot` / `--ui`、
明示拒否で byte 検証済）のみ。速度・メモリは現状 C 優位（性能機構は次フェーズ。
実測の全量は `bench/report-2026-08-24.html`、要約は `BENCH.md`）。

## クレート

| クレート | 内容 | 検証 |
|---|---|---|
| `akl-core` | 値表現（NaN-box AklVal）、文字列インターン、オブジェクトモデル + GC、VM | Kani（不変条件証明）/ Miri / Clippy |
| `akl-ffi` | C 連携層（akl バインディングの受け渡し） | 単体テスト / Clippy / LSan 緑 |
| `ifuto-core` | HTML トークナイザ・ツリービルダ・CSS・layout・render・md・charset・store・chrome 純粋部 等 | 176 単体テスト / WPT 1922 / 差分 fuzz |
| `ifuto-cli` | 統合 CLI（C `src/main.c` の完全置換。GUI 以外の全観測モード） | 差分 fuzz byte-exact / golden |
| `ifuto-ffi` | 最終統合層（`<script>` 実行配線 + net_sock / bearssl TLS ソケット） | oracle script E2E / TLS |

## ローカルでの実行

サンドボックスの Rust ツールチェーンは `toolchain-bin` ブランチに分割格納されている
（環境リセットで `$HOME/akl-toolchain` が消えたら復元）:

```sh
git fetch origin toolchain-bin:refs/remotes/origin/toolchain-bin
for i in 00 01 02 03 04 05 06 07 08; do git show origin/toolchain-bin:part_$i; done \
  | zcat | tar -x -C $HOME
export RUSTUP_HOME=$HOME/akl-toolchain/rustup CARGO_HOME=$HOME/akl-toolchain/cargo
export PATH=$HOME/akl-toolchain/cargo/bin:$PATH

cd rust
cargo test --offline                    # ユニットテスト（333 件）
cargo clippy --offline --workspace -- -D warnings   # lint（警告 = エラー）
cargo build --release --offline         # rust/target/release/ifuto
cargo +nightly miri test                # 未定義動作検出
cargo kani --workspace                  # 機械的証明（Kani）
cargo geiger                            # unsafe 使用量カウント
```

C バイナリとの差分検証: `python3 tools/diff_fuzz_cli.py 3000 --seed 999`
（stdout/stderr/rc の byte 突合。`--stats` は計測値 scrub・決定値比較）。

## フェーズ進捗

| フェーズ | モジュール | 内容 | Kani 証明 |
|---|---|---|---|
| 0 | `lib.rs` | AklVal（NaN-box newtype）、int_add fast path | 7 件（タグ排他・往復・canonical NaN・overflow 判定） |
| 1 | `string.rs` | Interner（HashMap ベース。一意性を型で保証） | 4 件（一意性・往復・単射・空文字列） |
| 2 | `obj.rs` | Obj enum + ObjTable + mark&sweep GC | 3 件（GC 安全性・free-list 整合・alloc 妥当性） |

### 各フェーズが C の実バグをどう構造的に排除するか

- フェーズ 0: 「演算結果 double がタグ空間に衝突する」経路 → `from_f64` の
  NaN canonical 化を Kani で証明
- フェーズ 1: 「同一文字列が 2 つの intern id を持つ」（v0.10 実測: "TypeError" が
  id 292/293 に分裂）→ HashMap の get-or-insert が全単射を保証
- フェーズ 2: 「コールバック中 GC 後の use-after-free」（v0.10 実測: 配列 HOF で
  ASan 検出）→ `&Rt` / `&mut Rt` の借用規則でコンパイル不能。子参照の列挙漏れも
  match の網羅性でコンパイルエラー

## GitHub Actions

ローカル検証を優先し、最終確認は GitHub Actions で行う（ローカルと同一ツールチェーン
`toolchain-bin` を `trigger/toolchain.sh` が取得する）:

- `trigger/trigger.md` を push → 検証コマンド群をランナー上で実行
  （結果は `trigger/result.md` に自動追記）

## 設計方針

- `#![forbid(unsafe_code)]`: このクレートに unsafe を置かない（Rudra / geiger で監査可能）
- ビットレイアウトは C 実装と 1:1 対応（回帰テストで突合可能）
- 不変条件は Kani の `#[kani::proof]` で機械的に証明（`akl-core/src/lib.rs` 末尾）
