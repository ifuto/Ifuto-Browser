# BLINK_COMPAT.md — Ifuto CSS Engine ↔ Blink 互換カバレッジマップ

**このファイルが「Blink API 互換」の唯一の定義。** 互換の主張は下表の行だけ。
表にない機能は互換でも非互換でもなく **サブセット外（未実装）** である。

## 背景と境界（正直な定義）

- Blink の C++ API 表面は数千シンボルに及び、DOM/Frame/CSSOM/描画パイプラインと
  不可分である。Ifuto の製品法則（100% self-made C11、ldd = linux-vdso/libc/ld、
  libstdc++ 不導入）の下で C++ シンボル互換は **構造的に不可能かつ非目標**。
- したがって互換は **概念・戦略・関数形状の C ファサード互換**（src/css_blink.h）。
  header-only inline であり製品バイナリへの肥大化は 0 バイト。

## Coverage map

| 概念 | Blink 側 | Ifuto 側 | 状態 |
|---|---|---|---|
| ルール索引バケツ戦略 | `RuleSet`（id/class/tag/universal バケツ、右端 compound 特徴キー、上位集合候補） | `IfRuleSet`（同一 4 バケツ、同一キー優先 id>class>tag>universal） | **戦略同等**（実測 23.32× vs 全走査、BENCH.md） |
| 候補上位集合の正しさ | RuleSet contract | 差分オラクル `test_css_ruleset_oracle`（on/off 220 seeds、全ノード bit 一致） | **検証済み** |
| マッチ方向 | `SelectorChecker::Match`＝右→左、結合子で祖先/親へ | `match_at`＝右→左（descendant バックトラック、child） | **同値** |
| カスケード全順序 | important → origin → specificity → 文書順 | `winner_beats`＝(important, origin, spec, order) 厳密辞書順 | **同値** |
| specificity ビット配置 | `(ids<<16)\|(classes<<8)\|types` | `IfSelector.spec` 同一配置 | **同値** |
| 文書順キー | tree-scoped order | decl 単位一意 `order`（旧実装は decl 衝突で後勝ち不成立 → 修正済、回帰テスト有） | **同値（仕様準拠）** |
| UA / author / inline origin | 3 段 origin | `IF_ORIGIN_UA/AUTHOR/INLINE` 3 段 | **同値** |
| スタイル再計算入口 | `StyleEngine::RecalcStyle` | `ifuto_style_recalc`（全量。増分 invalidation は台帳外） | **形状互換**（増分なし） |
| ComputedStyle 取得 | `Element::ComputedStyleRef` | `ifuto_computed_style`（NULL=未計算） | **形状互換** |
| シート作成 | `StyleSheetContents::Create` + parse | `ifuto_stylesheet_create_and_parse` | **形状互換** |
| 単体セレクタ照合 | `SelectorChecker::Match`（MatchRequest 経由の直接照合） | `ifuto_selector_matches` | **形状互換** |
| 失敗時の規律 | —（Blink は例外/DCHECK 文化） | パーサは壊れた部分を捨てて前進、`n_dropped_*` で可視化、索引 OOM は naive フォールバック（安全側） | Ifuto 固有（安全設計） |

## サブセット外（非互換ではなく未実装。誤って互換と呼ばない）

pseudo-classes/elements（`:hover`,`::before`,…）、属性セレクタ、兄弟結合子（`+`,`~`）、
`:has()`、@規則（`@media`,`@supports`,`@font-face`,…）、CSSOM 変更 API（cssRules 変異）、
invalidation sets / subtree recalc、bloom 祖先フィルタ（Blink `StyleFilter`。深い
descendant 多数で有効。Ifuto 台帳候補）、animations/transitions、custom properties（var()）、
shadow DOM / :host / ::slotted、namespaces、calc()、grid/flex レイアウトプロパティ、
viewport 単位、font shorthand 系統。各宣言・ルールは安全側に棄却され `n_dropped_*` で可視化。

## 検証

- `tests/test_css.c::test_css_ruleset_oracle`: 構造化ランダム 220 seeds ×（40 rule/36 elem 上限）
  で naive 全走査と索引走査の全ノード計算済みスタイル **bit 一致**（on/off 差分監査）。
- `bench/bench_css.c`: 2500 rules × 3000 elements 合成で **23.32×**（このコンテナ実測。
  naive 471.990ms → index 20.240ms、5 交互 round の min）。
- 索引経路がデフォルトで既存 CSS 単体テスト全件緑（17,540 checks 総計に含む）。
