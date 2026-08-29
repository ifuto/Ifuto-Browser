# 海外高速化研究 調査メモ（ifuto 高速化のための一次情報集）

2026-08-29 実施。目的は 16MB パイプライン total（read→parse→layout→render→出力）の
短縮に使える学術・技術一次情報の棚卸し。各項目に「ifuto への適用可否」を
**プロジェクト制約**（依存ゼロ・ifuto-core `#![forbid(unsafe_code)]`・
SIMD intrinsic 使用禁止→u64 SWAR 代替・観測出力 byte-exact 維持）との照合で
明記する。推定と断言は分けた（推定には【推定】と記す）。

---

## 1. パース（parse）系

### 1.1 simdjson — 2 段パースと構造化文字インデックス
- Langdale & Lemire, "Parsing Gigabytes of JSON per Second", VLDB Journal 28(6), 2019
- <https://arxiv.org/abs/1902.08318>（実装 <https://github.com/simdjson/simdjson>）
- 要点: 高速化の本質は SIMD そのものよりも (a) **stage 1（構造文字の位置索引化・
  UTF-8 検証）と stage 2（意味解析）を分離**し、(b) ホットループを**分岐ほぼゼロの
  データ並列**に変形し、(c) バイト分類を**テーブル引き**に落とす設計。
  先行最速実装（RapidJSON）の 1/4 以下の命令数、単コア 2-3 GB/s。
- ifuto 適用: **SIMD intrinsic は規約で禁止**だが、設計の 3 原則（構造索引の先行 /
  分岐レス化 / 分類テーブル化）は SWAR（u64 レジスタ内並列）でそのまま写せる。
  我々の md/html 走査に「delim 位置ビットマップを先に作り、意味処理はビットを
  なめる」2 段化の余地がある。**優先度: 高**。SWAR 置換は既に scan 系で実績あり。

### 1.2 Mison — 構造ビットマップ索引・制御フローのデータフロー化
- Li, Katsipoulakis, Chandramouli, Goldstein, Kossmann, "Mison: A Fast JSON Parser
  for Data Analytics", PVLDB 10(10), 2017, <https://doi.org/10.14778/3115404.3115416>
- 要点: 構造文字（: , 等）のビットマップ索引を構築し、「制御フローをデータフローに
  変える」ことで**本質的に予測不能な分岐を消す**。高選択度クエリで 2 GB/s 級。
- ifuto 適用: md の行境界・テーブル区切り・inline delimiter（`[*_~` 等）を
  **1 パスでビットマップ化→各ビット位置を消費**する形。### 現状との差分:
  inline_span は逐バイト状態機械で、delimiter 事前索引化の余地【推定: -10〜20%】。
  **優先度: 高（12-c 以降の parse 主戦略）**。

### 1.3 Pison — チャンク並列 + 投機的索引構築
- Jiang, Qiu, Zhao, "Scalable Structural Index Construction for JSON Analytics",
  PVLDB 14(4), 2020, <https://vldb.org/pvldb/vol14/p694-zhao.pdf>
- 要点: 入力をチャンク分割 → 各コアが**任意状態からの再同期可能な索引構築**を
  並列に走らせ、境界は投機+検証で接合。serial 版比 4.2×、16 コアで Mison 比 9.8×
  （bulk レコード）。スレッド数弱スケーリングも報告。
- ifuto 適用: 現行 2-slice parse の一般化（3-way 以上）の理論的裏付け。
  「チャンク境界で全状態を仮定して走り、真の状態に収束した仮定を採用」する
  restartable 設計は md の行状態機械（行頭状態のみ持つ単純 FSM）に向く。
  ただし本環境は 1 物理コア 2 HT で並列化の天井が低い。**優先度: 中（将来）**。

### 1.4 Mytkowicz ら — データ並列有限状態機械
- Mytkowicz, Musuvathi, Schulte, "Data-Parallel Finite-State Machines", ASPLOS 2014
- 要点: lexing FSM の並列化に **enumerative speculation**（チャンクを全初期状態で
  走らせ後で真の遷移列に整合するものを採択）。FO 分割不能な FSM でも通用。
- ifuto 適用: html_tok の >2-way 分割への理論枠組み。状態数が小さい
  （トークナイザ状態）ため投機コストも小さい。**優先度: 中（将来）**。

### 1.5 Keiser & Lemire — 1 命令/バイト未満の UTF-8 検証
- "Validating UTF-8 In Less Than One Instruction Per Byte", SPE 51(5), 2021
- <https://arxiv.org/abs/2010.03090>（実装 <https://github.com/simdutf/simdutf>）
- 要点: FSM ではなく **vectorized classification**（先頭バイト長・継続範囲・過剰長
  等をテーブル引きで一度に判定）。lookup 法で 10-66 GiB/s。FSM 法のメモリ待ちと
  分岐を両方消す設計。
- 併読: Lemire & Muła, "Transcoding billions of Unicode characters per second with
  SIMD instructions"（UTF-8↔16 相互変換、GB/s 級）。
- ifuto 適用: `utf8.rs` は既に band 判定の SWAR 寄り実装だが、**検証を入力全体に
  1 回（read 直後）に集約**し、span 単位の再検証を消す構成転換の根拠になる。
  charset 経路（SJIS/EUC テーブル）の precomputed 化とも同思想。**優先度: 中**。

---

## 2. メモリ・アロケータ系

### 2.1 Mimalloc — free-list sharding と局所性
- Leijen, Zorn, de Moura, "Mimalloc: Free List Sharding in Action", MSR-TR-2019-18 /
  APLAS 2019, <https://www.microsoft.com/en-us/research/wp-content/uploads/2019/06/mimalloc-tr-v1.pdf>
- 要点: page（64KiB）単位の free list 分散で**確保の空間局所性**を上げ、
  redis で tcmalloc/jemalloc 比 +7〜14%。「シンプルな sharded free list への
  置換だけで 1GiB ヒープ +25%」の報告（§2）。alloc 単価より**配置の局所性**が
  アプリ全体の速度を支配するという主張。
- 典故: Grunwald, Zorn, Henderson, "Improving the cache locality of memory
  allocation", PLDI 1993（allocator が参照局所性を決める先駆報告）。
- ifuto 適用: アロケータ差し替えは依存禁止で不可。**が教訓は採用済みの方向と一致**:
  段別 arena（text_arena、seg_arena）+ Node 配列（連続 SoA 的 layout）で
  「確保位置の連続性」を自前管理している。残務は「小粒 malloc をさらに bump 化」
  （11・12-a/b の延長の 12-c）。**優先度: 高（継続戦略の理論的根拠）**。

### 2.2 Drepper — プログラマが知るべきメモリの全て
- Ulrich Drepper, "What Every Programmer Should Know About Memory", Red Hat, 2007
- <https://people.freebsd.org/~lstewart/articles/cpumemory.pdf> 他
- 要点: L1/L2/LLC 階層、キャッシュライン単位の読み書き、ライトバックと RFO、
  HW prefetcher は**順次アクセスに最適**・間接参照に無力。ソフトウェア側の
  最適化は (a) 局所性のあるデータ構造、(b) 逐次アクセスパターン、
  (c) 必要時点より先のプリフェッチ、に集約される。
- ifuto 適用: 設計原則の一次情報。Node 80B 化（10-e）・行 sweep 一本化（10-a）・
  2-slice の連続走査はこの教えどおり。ポインタチェイスを残す箇所（DOM の
  sibling リンク経由走査）の**子配列化**が理論上の次の一手。**優先度: 高（設計指針）**。

### 2.3 Data-Oriented Design（Acton）
- Mike Acton, "Data-Oriented Design and C++", CppCon 2014
- <https://www.youtube.com/watch?v=rX0ItVEVjHc>（要約例:
  <https://alessandrominali.github.io/data_oriented_design_canonical_example.html>）
- 要点: 「まずキャッシュミスを数えよ」。AoS→SoA 変換で同じ計算が数倍速くなる
  標準例（cache line あたりの loop 数分析法）。抽象化はデータ変換モデルの後。
- ifuto 適用: 学術論文ではないが業界標準の設計規範。layout の行バッファ群
  （lines/deco/seg）の配列構造はほぼ SoA。DOM Node のフィールド使用率を
  「ホットループが触る列」の観点で再棚卸しする分析手続きを輸入。**優先度: 中**。

---

## 3. ブラウザ並列化系

### 3.1 Servo — 段別並列化と Rust
- Anderson, Bergstrom, Goregaokar et al., "Experience Report: Developing the Servo
  Web Browser Engine using Rust", 2015, <https://arxiv.org/abs/1505.07383>
- 併読: "Engineering the Servo Web Browser Engine using Rust"
  <https://plsyssec.github.io/cse291y-fall25/papers/servo.pdf>
- 要点: parse（HTML/CSS）・layout・script・render を**それぞれ独立に並列化**
  （data/task parallelism 併用）し、コンポーネント間はメッセージパッシングで
  板挟みにしない。selector 照合は Bloom filter による祖先絞り込み。
  Rust の所有権が「fearless parallelism」を可能にした、との体験報告。
- ifuto 適用: 我々の 2-slice parse / body shard layout / 2-way render sweep は
  同じ段別並列思想の縮小版。Rust の `thread::scope` + RefCell 局所スクラッチで
  メモリ安全に書けている点も一致。パイプライン重畳（read と parse のオーバーラップ）
  は未実装だが byte-exact 規約の下で逐語同値を保てる形のみ検討。**優先度: 中**。

### 3.2 Berkeley 並列ブラウザ — 属性文法と投機的レイアウト
- Meyerovich & Bodik, "Fast and parallel webpage layout", WWW 2010,
  <https://homes.cs.washington.edu/~bodik/ucb/browser.html>
- Jones et al., "Parallelizing the Web Browser", HotPar 2009
- 要点: CSS レイアウト意味論を**属性文法**として形式化し、自動的に並列スケジュール
  を合成。float を含むページ向けに**投機レイアウト**（仮定で走り矛盾で再実行）。
  セレクタ照合の locality 改善アルゴリズムで 4 コア 60×。
- ifuto 適用: 属性文法の全量形式化は規模外。ただし「接合可能な整数セル幾何
  （ours: y+=hA 連結規約）は shard 並列できる」という我々の 10-h 設計は同じ洞察。
  投機レイアウト ⇒ 我们的には「ヒント有文書のみ shard 発動」で限定済。**優先度: 参考**。

---

## 4. 走査・マッチング系

### 4.1 Hyperscan — 多パターン同時照合
- Intel, "Introduction to Hyperscan", <https://www.intel.com/content/www/us/en/developer/articles/technical/introduction-to-hyperscan.html>
  （repo <https://github.com/intel/hyperscan>）
- 要点: 数万の正規表現を**同時に**ストリーム照合。SIMD シフト/OR による
  オートマトン合成。送り側コストはフィードバックレス。
- ifuto 適用: 「複数の候補バイトのどれか出現」を 1 パスで探す走査は SWAR に翻訳
  可能（各文字クラスの u64 マスクを OR 合成）。md inline delimiter 群・
  HTML の `<&` 両検出 等。**優先度: 中（SWAR マスク合成の指針）**。

### 4.2 SWAR — レジスタ内並列
- Sean Eron Anderson, "Bit Twiddling Hacks"
  <https://graphics.stanford.edu/~seander/bithacks.html>、概説:
  <https://www.cs.uaf.edu/courses/cs441/notes/simd/>
- 要点: 汎用レジスタを小さな SIMD として使う（バイト対称の減算/MSB 検出）。
  intrinsic なしでコンパイラに auto-vectorize されやすい形という美点もある。
- ifuto 適用: **既に採用**（scan 系）。本メモの他技術（Mison 的ビットマップ化・
  Hyperscan 的同時照合）は全て SWAR に翻訳して適用する方針。**規約と両立**。

---

## 5. I/O 系

### 5.1 mmap vs read — ファイル入力の置き所
- 実測比較: <https://github.com/david-slatinek/c-read-vs.-mmap>（256MB/1GB で
  mmap が read 比 17-21% 高速）、機構解説:
  <https://umair.eu.org/why-mmap-is-faster-than-system-calls/>
  （read の ~60% は copyout、syscall 境界 ~15%。MAP_POPULATE で fault 前払い可能）
- 要点: read はカーネル→ユーザへの**コピー **+ syscall 境界コスト。mmap は
  ゼロコピーで fault 駆動（ただし fault 自体の待ちは消えない＝parse 段に移るだけ。
  **「C の read 0.02ms」は mmap の見せかけであり、fault 税は parse 段で払っている**）。
- ifuto 適用: mmap は std に存在せず libc 呼出=unsafe 禁止領域。採用不可。
  現実解は (a) 1 回の read で容量予約済みバッファへ、(b) **fault 前払いの局所化**
  （巨大 memcpy より strided touch の方が fault 間隔で並列化に有利、【推定】）、
  (c) read を parse スレッドとオーバーラップ。**優先度: 高（read 段 7.7ms の是正）**。

---

## 6. ビルド・コード配置系

### 6.1 BOLT / PGO — プロファイル誘導のコード配置
- Panchenko et al., "BOLT: A Practical Binary Optimizer for Data Centers and Beyond",
  <https://arxiv.org/abs/1807.06735>、basic block 並べ替え改善:
  Newell & Pupyrev <https://arxiv.org/abs/1809.04676>
- 要点: LBR プロファイルでホット基本ブロックを連結配置し i-cache/iTLB ミスを削減。
  大規模バイナリで数%〜2 桁%の改善。rustc 同等物は PGO
  （`-Cprofile-generate/use`）で外部ライブラリではなくビルドツール行使。
- ifuto 適用: 依存ライブラリではないので規約違反ではないが、**ベンチ再現性と
  配布バイナリの一貫性**の観点で運用設計が要る（プロファイル収集手順の固定、
  コーパス代表性）。現時点では保留、まずアルゴリズム側を詰める。**優先度: 低（将来）**。

---

## 7. 参考（関連するが直接適用外）

- FAD.js（JIT 投機的 JSON アクセス、Bonetta et al. PVLDB 2016）: 実行時 schema 推測
  で解析を必要経路のみに限定。我々の fast-DOM（slim 属性）に思想先行あり。
- FishStore（Xie et al. 2019）: パースとフィルタの融合。
- snmalloc（Liétar et al. ISMM 2019）: スレッド間所有移転ワークロード向け
  メッセージパッシング型 allocator。shard 後処理の参考情報。

---

## 8. 適用マトリクス（優先順位つき）

| # | 技術 | 出典 | 制約適合 | ifuto 適用先 | 期待効果【推定】 | 優先度 |
|---|---|---|---|---|---|---|
| 1 | 構造索引先行 + 分岐レス + テーブル分類 | simdjson (§1.1) | SWAR 翻訳で可 | parse（md/html 走査の 2 段化） | parse −10〜20% | 高 |
| 2 | 構造ビットマップ索引 | Mison (§1.2) | SWAR 翻訳で可 | inline delimiter/行境界 | parse −10〜20% | 高 |
| 3 | page 局所性（arena/bump 継続） | Mimalloc (§2.1) | 既方針と一致 | parse 確保のさらなる bump 化 | 継続効果 | 高 |
| 4 | 逐次アクセス・子配列化 | Drepper (§2.2) | 可 | DOM sibling 走査→配列 | layout 数% | 高 |
| 5 | read: 容量予約 1 回 read / fault 局所化 / 重畳 | mmap 対比研究 (§5.1) | unsafe 不可の制約内で近似 | read 段 | 数 ms | 高 |
| 6 | 複数候補バイト同時照合 | Hyperscan (§4.1) | SWAR 翻訳で可 | charset/走査の統合 | 数% | 中 |
| 7 | UTF-8 検証の集約化 | Keiser&Lemire (§1.5) | SWAR 翻訳で可 | read 直後 1 回化 | 小〜数% | 中 |
| 8 | 段別並列化の拡張 | Servo (§3.1) / Pison (§1.3) / DP-FSM (§1.4) | 可（2 HT 天井） | >2-slice 一般化 | 環境依存・小 | 中 |
| 9 | SoA 棚卸し | DOD (§2.3) | 可 | layout バッファ群 | 手法輸入 | 中 |
| 10 | 属性文法・投機レイアウト | Bodik (§3.2) | 規模外 | 参考 | — | 参考 |
| 11 | PGO/BOLT | BOLT (§6.1) | ツール行使（要運用設計） | 全体 | 数% | 低（将来） |

※ 番号 1〜3 は既存の SWAR/arena 戦線の直接延長であり、まずそこから着手するのが
   合理的（車輪の再発明をせず、証明された理論を自前実装に落とす）。

### 制約との整合の確認
- 外部ライブラリ不使用: 全て上記は**概念の移植**でありコード借用なし（規約クリア）。
- unsafe 禁止（ifuto-core）: SWAR・BEAM 的 arena・テーブル分類は全て safe Rust で
  書ける。mmap/`unreachable_unchecked` は制約上不可のため代替設計（§5.1、および
  到達不能性は網羅的 match + `#[cold]` + イテレータ合成でコンパイラに証明させる）。
- SIMD intrinsic 禁止: 全て u64 SWAR に置換（§4.2）。auto-vectorize しやすい
  形を選ぶことで無料のベクトル化も期待できる。
