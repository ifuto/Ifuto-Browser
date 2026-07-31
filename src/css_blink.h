/* Ifuto CSS Engine — Blink API ファサード（C, header-only）
 *
 * 目的: 超高性能 CSS エンジン（src/css.c, RuleSet 索引）を、Blink の概念・
 * 関数形状に沿った名前で露出する。これは **C ABI のファサード** であり、
 * C++ シンボル互換ではない（製品バイナリに libstdc++ を持ち込まない製品法則:
 * ldd は linux-vdso/libc/ld のみ）。header-only inline のため生成バイトは 0。
 *
 * 対応関係の唯一の正は docs/BLINK_COMPAT.md の coverage map。
 * 「互換」の定義（このヘッダが約束するもの）:
 *   - 概念互換: StyleEngine / StyleSheetContents / ComputedStyle / RuleSet /
 *     ElementRuleCollector に相当する責務が同名で存在する
 *   - 戦略互換: RuleSet のバケツ分割（id/class/tag/universal、右端特徴キー、
 *     候補上位集合）、右→左マッチ、カスケード全順序 (important, origin, spec, order)、
 *     specificity の bit 配置 (ids<<16)|(classes<<8)|types は Blink と同一
 *   - 非互換（docs に明記）: pseudo 類・属性セレクタ・兄弟結合子・@規則・
 *     CSSOM 変更 API・invalidation・bloom 祖先フィルタ・animation/transition・
 *     custom properties・shadow DOM 等はサブセット外
 */
#ifndef IFUTO_CSS_BLINK_H
#define IFUTO_CSS_BLINK_H

#include "css.h"

/* ---- 型マップ（blink::X ≈ IfutoX） ---- */
typedef IfDom           IfutoDocument;            /* ≈ blink::Document */
typedef IfNode          IfutoElement;             /* ≈ blink::Element（非要素ノードも同型） */
typedef IfStyle         IfutoComputedStyle;       /* ≈ blink::ComputedStyle（サブセット） */
typedef IfStyleSheet    IfutoStyleSheetContents;  /* ≈ blink::StyleSheetContents */
typedef IfRuleSet       IfutoRuleSet;             /* ≈ blink::RuleSet */
typedef IfSelector      IfutoCSSSelector;         /* ≈ blink::CSSSelector */

/* ≈ blink::StyleSheetContents::Create(CSSParserContext) + ParseString
 * order_base はカスケードの文書順キーの開始値（Ifuto ではシート間 order を明示管理） */
static inline IfutoStyleSheetContents *
ifuto_stylesheet_create_and_parse(IfArena *a, IfStr css_text, u32 order_base) {
    return if_css_parse(a, css_text, order_base);
}

/* ≈ blink::StyleEngine::RecalcStyle（Document 全体のスタイル再計算。
 * Ifuto は増分 invalidation を持たない全量再計算。DOM 変更大きさに比例しない部分は
 * RuleSet 索引が担保（perf 特性は docs/BENCH.md） */
static inline void ifuto_style_recalc(IfArena *a, IfutoDocument *document) {
    if_style_apply(a, document);
}

/* ≈ blink::Element::ComputedStyleRef / Document::GetComputedStyle(element)。
 * NULL は未計算（ifuto_style_recalc 未実行 or 非要素） */
static inline const IfutoComputedStyle *
ifuto_computed_style(const IfutoElement *element) {
    return element ? element->style : NULL;
}

/* ≈ blink::MatchRequest + SelectorChecker による単体照合。
 * Blink の SelectorChecker::Match と同じ右→左戦略 */
static inline bool
ifuto_selector_matches(const IfutoElement *element, const IfutoCSSSelector *selector) {
    return if_css_match_selector(element, selector);
}

/* ≈ ElementRuleCollector のモード切替に相当する監査スイッチ。
 * 1 で索引を止め全走査（デバッグ/差分検証専用。実運用は 0） */
static inline void ifuto_ruleset_set_naive_matching(int enabled) {
    if_css_set_naive_matching(enabled);
}

#endif
