# EXTENSIONS.md — Ifuto 拡張機能 設計（唯一の正・段階投入）

> 現在地: **E1 shipped（2026-08-07）**。
> E1 の実機構は §2-§5 に凍結記載（設計時の「呼べるホスト関数」案は
> **akl に native 登録層が存在しない**ことが実装調査で判明し、
> **戻り値効果（return-value effects）スキーマ**に置き換わった。§3 参照）。
> 規則「未検証コードは成果に数えない」により、 shipped を名乗る段は
> 全て機械オラクル付き（§7 の完了条件 = ext_smoke / fuzz_ext / make fuzz）。

## 0. 目的と非目標

目的: ユーザーのローカル信頼モデル下で、自作 JS エンジン akl による
ブラウザ振る舞いの拡張を可能にし、**作りやすさ・導入のしやすさ・安全性**
（優先度台帳 P4）を同時に満たす。

非目標（明示。これらを約束しない）:
- WebExtensions API との完全互換（mozable の資産は持ち込めない。概念互換まで）
- 公開拡張レジストリ・署名/審査チェーン（導入はローカルコピーのみ。§5）
- 拡張からのネットワーク通信（当面ケイパビリティが存在しない。§3 二重防御）
- DOM 全面書込（v0.4 配列 DOM 直結後に再査定。ARCHITECTURE §7.1 連動）
- ホットリロード（状態不変条件が複雑すぎる。再起動ロードのみ。§6）

## 1. 土台となる既存の機械保証（棚卸し。全て実在・計測済み）

拡張設計は **新しい信頼機構を作らない**。以下の既存保証の内側に閉じ込める:

| 機構 | 値 | 根拠 |
|---|---|---|
| 命令 budget（fail-stop）| eval あたり既定 10M ops（製品 CLI 500M） | `akl_set_insn_budget`、`while(1){}` が budget で死ぬことは akl_cli_smoke が機械固定 |
| ヒープ硬上限 | 16MB（`AKL_MAX_HEAP_MB`） | akl.c enum。`akl_tune` は宿主のみ・拡張からは到達不能（§4） |
| オブジェクト数上限 | 100,000 | 同上 |
| ソース長上限 | 4MB（`AKL_MAX_SRC`） | 同上 |
| AST ノード上限 | 200,000（`AKL_MAX_NODES`） | 同上 |
| call 深度固定 | 256（`AKL_MAX_DEPTH`。akl_tune でも不動 = 構造的保護） | 同上 |
| VM スタック / locals | 65,536 / 1,024 | 同上 |
| メモリ安全性の構造 | NaN-boxed 値（u64）+ ヒープ参照は RT 所有配列の **u32 index**（生ポインタを API 面に出さない = UAF 構造排除） | akl.h 設計不変条件 |
| W^X | JIT 不在・runtime codegen なし（CoJIT は AOT 特化のみ）= RWX 新規生成経路が存在しない | akl.h / SANDBOX.md |
| syscall 面 | seccomp-BPF `IF_SB_AKL`: open/socket/proc 系は全て kill-process（errno soft-fail 不採用）。ファイル slurp 後に適用、非対応 kernel は rc=2 で死ぬ | sandbox.c / SANDBOX.md |
| **IO プリミティブ構造不在** | akl.c には fopen/open/socket 系が**ゼロ**（機械 grep 監査可能）。評価はそもそも IO 不能 = サンドボックスの基底層 | akl.c 全量 / E1 台帳 |
| fuzz | `fuzz_akl` に加え `fuzz_ext`（manifest 異形。`make fuzz` 連結。ASan+UBSan 0 crash 運用） | fuzz/ |

これらの **合流**: 拡張コードは「実行時間・メモリ・再帰深度・syscall」の四面全てで
宿主的 fail-stop が保証済み。設計で新たに証明すべきは manifest ローダーと
ケイパビリティ結線の 2 面のみ（§7 E1 のオラクル）。

## 2. 拡張の形態

```
$IFUTO_HOME/ext/<name>/
    manifest.txt      # 行ベース "key: value"（※ manifest.json ではない）
    <entry>.js        # akl 言語（AKL_COMPAT.md の範囲）
```

- **manifest は JSON ではない**。本コードベースに JSON パーサは存在せず、
  パーサを新規作成すると新たな fuzz 面を背負う。行ベース `key: value`
  で表現可能な範囲に限定する。ファイル名を `manifest.json` と名乗らないのは
  互換の嘘をつかないため。実装は `src/ext_manifest.c`（純粋関数・fuzz 単体監査）。
- **E1 凍結文法**（実装と完全一致。銘々の制約は防御ではなく仕様である）:
  - 行: 空行と `#` 始まりはスキップ。他は `key : value`（前後空白 trim。
    行末の単一 `\r` は除去 = CRLF 救済。session パーサとは規則が異なる点に注意）
  - key: `name` `version` `entry` `permissions` のみ。未知キー・重複キーは失敗
  - name: `[A-Za-z0-9_.-]{1,63}`（表示安全を charset で構造保証）
  - version: 同 charset `{1,23}`
  - entry: basename のみ（`'/' '\\'` は charset が構造排除・先頭 `.` も明示拒否）
    ・charset 同・≤120
  - permissions: `,` 区切り。各トークンは `status` / `log` のみ（E1）。
    未知トークンは失敗。**E1 は単一効果規則: 2 つ以上の有効宣言は失敗**
  - 必須: `name` `version` `entry`（permissions は省略可 = 評価のみ・効果なし）
  - サイズ: manifest ≤ 64KB、entry ≤ 4MB・埋め込み NUL 拒否（akl_eval は C 文字列 API）
- **budget 上書きキーは存在しない**（拡張が自身の制限を緩める経路を設計に置かない）。

## 3. 権限モデル（capability。既定拒否）

**E1 機構: 戻り値効果（return-value effects）スキーマ**。
設計時は「宣言ケイパビリティのホスト関数をバインドする」形を想定していたが、
実装調査で **akl にホスト関数登録層が存在しない**（native/builtin 機構ゼロ）
ことが確定した。そこで E1 はケイパビリティを**呼べる関数ではなく、
「entry の最終式文の値」が流れる先**として定義する:

- 呼べるホスト関数は存在しない。存在しないことを機構にする
  （結線ミスで socket 関数が漏れる、という故障クラスが根無しになる）
- 未宣言ケイパビリティの行使は **manifest 解析段で失敗**する
  （unknown permission = ロード FAILED。実行に到らせない）

段階投入するケイパビリティ（各段の意味論はその段の完了時に本書へ追記して凍結）:

| 段 | 宣言名 | 意味論（E1 凍結） | 副作用面 |
|---|---|---|---|
| E1 | `log` | 最終式文の値（String 必須）を `[ext:<name>] <値>`（1 行・≤960B・改行は空白化）で stderr へ | stderr のみ |
| E1 | `status` | 最終式文の値（String 必須）を起動時トーストとして 1 度表示（chrome 共通通知流路） | 自前 chrome 状態のみ |
| E2 | `page.readText` | （未実装・意味論未定） | — |
| E3 | `page.injectStyle` | （未実装・意味論未定） | — |

- **ネットワーク権限は設計上存在しない**。akl には IO プリミティブ自体が無く、
  E1 はホスト関数を一切結線しないため、評価中に syscall へ到達する機構が無い。
  カーネル層 seccomp（IF_SB_AKL 系・現行は akl 単体ランナーに適用済み、
  chrome 適用は v0.4 台帳）はこの「不在」の上に乗る第二層として将来も機能する。
- 宣言 perm とバインド表の **双方向一致** は manifest 解析で機械検査
  （未知宣言 = FAILED、複数宣言 = FAILED。黙って欠落させない）。

## 4. 分離と実行モデル

- **1 拡張 = 1 AklRT**。ヒープは RT ごとに閉鎖、ハンドルは RT ローカル u32 のため
  cross-RT 参照は構築不能（追加の隔離機構が要らない形を選ぶ）。
- budget は akl 製品既定をそのまま（§1 の表。`akl_tune` をローダが呼ぶ経路は無い）。
- 実行タイミング: E1 は chrome init 時に全拡張を辞書順で 1 回ずつ評価
  （readdir 順は FS 依存のため収集→qsort で決定性を構造保証）。
  イベント駆動（ページ読込完了等）は意味論未凍結 = E2 以降。
- **fail-stop の粒度**: 拡張の構文エラー・例外・budget 超過は *当該拡張の死* であり、
  ifuto 本体・他拡張は継続する（ext_smoke ①が機械固定）。
  ホスト関数不結線（§3）により拡張評価がプロセス死型の seccomp 違反を
  引く経路は E1 には存在しない（IF_SB_AKL 許可集合を超える syscall は
  評価中に発生し得ない = heap 成長の mmap 等のみ）。
- 救済スイッチ: `IFUTO_NO_EXT=1` で拡張読み込み全停止（挙動疑義時の切断手段）。

## 5. 導入 UX（しやすさ）

- 開発ベタ指定: `ifuto --ext DIR`（ディレクトリを直接指す。package 化不要。
  DIR が開けなければ `[ext] <dir>: cannot open` 1 行 = 明示指定は黙殺しない）。
- 恒常導入: `$IFUTO_HOME/ext/<name>/` へコピーするだけ（chrome init で自動
  走査。不在は黙殺）。削除はディレクトリ削除だけで完全
  （導入台帳を別途持たない = 台帳不整合という故障が存在しない）。
- **ロード結果は必ず可視化**（サイレント失敗禁止。`[ext]` 系出力フォーマット
  は ext_smoke の golden が機械固定）:

  ```
  [ext] hello v0.1 loaded (perm: status)      ← ロード成功
  [ext] badcap FAILED: manifest: line 4: unknown permission "net"
  [ext:logger] count=42                       ← log ケイパビリティの効果行
  ```

- 走査対象でない path 前置条件: サブディレクトリに manifest.txt が無ければ
  その項目は拡張でない（黙殺。README 置き場などを混在させられる）。

## 6. 開発者体験（作りやすさ）

- akl の言語範囲は `docs/AKL_COMPAT.md` が唯一の正。ケイパビリティ一覧は本書 §3 が正
  （実装時に doc とコードを同コミットで更新 = 乖離をコミット単位で不可能にする）。
- エラー文言: manifest 失敗は `manifest: line N: <理由>`（行番号構造付き）、
  評価失敗は `akl_error()` をそのまま使う。いずれも `[ext] <name> FAILED:` の
  1 行に正規化（理由文字列の最初の改行で打切り = 行構造の機械不変条件）。
- デバッグ: `--ext DIR` + 標準の `--dump-*` 観測点の組み合わせ。
  `--shot` は拡張の status 効果（トースト）を raster に表面化するため
  ヘッドレスで end-to-end を確認できる（ext_smoke がその機械利用）。
- 最小サンプル（tests/ext_smoke.py が実動を毎回立証している = 腐らないサンプル）:
  ```js
  // manifest.txt: name: hello / version: 0.1 / entry: main.js / permissions: status
  "Hello from EXTENSION"          // 最終式文の String が起動トーストになる
  ```

## 7. 段階ロードマップ（各段の完了条件 = 機械オラクル）

| 段 | 内容 | 完了条件（全既存ゲート不変に加えて） |
|---|---|---|
| **E0** | 本書。設計のみ | コードなし（2026-08-07 コミット） |
| **E1 (shipped)** | manifest ローダー + `--ext`/既定自動ロード + 戻り値効果 status/log | ① while(1) 拡張が budget で死に本体継続 ② manifest 未知 permission がロード FAILED ③ manifest 異形 fuzz（`fuzz_ext` を `make fuzz` に連結）④ `[ext]` 行の golden — 全て `tests/ext_smoke.py`（10 checks）+ fuzz 系で機械固定済み |
| E2 | `page.readText` + ページイベント（load 完了時評価） | ⑤ 読取結果が dump-dom 系オラクルと一致 ⑥ イベント順序 golden（意味論は実装時に §3 へ追記凍結） |
| E3 | `page.injectStyle` | ⑦ 注入前後の発行 byte-exact 差分が注入分のみと一致 |

E1 の実装面積（監査目次）: `src/ext_manifest.c`（純粋パーサ。fuzz 単体）、
`src/ext.c`（走査/評価/効果適用。akl 直結）、`src/chrome.c`（init 結線 +
`if_chrome_toast` 公開窓口）、`src/main.c`（`--ext`）、`src/gui/gui_app.c`
（shot でも paint 前に toast を表面化。対話と同一契約）。ifuto 本体は libc/libm
のみのまま（ldd 監査済み。akl は C11 自前のため依存増なし）。

各段は独立に revert 可能なコミット系列にする（拡張機構は本体の 150ms 制約・
WPT 100%・RSS 台帳を侵蝕しない = 各段で BENCH 再測定を台帳記録）。

## 8. 正直な境界（誇張しない欄）

- E1（2026-08-07 shipped）の効果は **status トースト 1 回 / stderr 1 行** のみ。
  ページ内容を読む/書く経路・ネット・永続化・相互通信は一切無い
  （実装調査で akl にホスト関数登録層が無いことが確定し、E1 は
  「呼べる API ゼロ = 戻り値効果」に設計変更した。§3 参照）。
- manifest.txt は行ベースの制約として値に改行・`:` を含められない
  （entry 名や name には実害なし。将来 JSON パーサが別用途で入れば再査定）。
- 拡張同士の通信・モジュール import は未定義（設計しない = 攻撃面にしない）。
- ストア正面の socket 全面禁止により「拡張の自動更新」はできない
  （配布はユーザが curl 等で取得 → ローカルコピー。信頼判断もユーザにある）。
- **ホスト関数を呼べる API（E2 以降の構想形）は、akl に native 登録層を
  新設するところから始まる独立エンジン課題**である。層の設計（関数値・
  budget 課金・GC 整合）と機械オラクルが先に要る。E2/E3 はその上にのみ置く。
- E3 の DOM 書込は v0.4 配列 DOM 直結（ARCHITECTURE v0.4b / §7.1）の設計確定後に
  再査定する。それ以前に書込 API を約束しない。
