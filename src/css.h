/* Ifuto — CSS サブセット（パース + カスケード）
 *
 * スコープ（明示的に削る。H4「使わない機能は実装しない」）:
 *   対応: type/class/id/universal セレクタ、子孫・子結合子、グループ化、
 *         display/color/background-color/font-size/font-weight/font-style/
 *         text-decoration/margin/padding/border(solid のみ)/width/height/
 *         text-align/line-height/white-space、shorthand: margin/padding/border/background
 *   非対応: @規則、pseudo、属性セレクタ、兄弟結合子、ショートハンド font/flex/grid/…
 *           → ルールまたは宣言ごと安全側に棄却（n_dropped で可視化）
 *
 * カスケード: (important, origin, specificity, order) の辞書順。
 *   origin: UA=0 < author=1 < inline=2 （author !important > inline 通常 を正しく表現できる）
 *
 * 偏差（spec-out。v0.1 受理）:
 *   - text-decoration を継承扱い（本来は伝播だが近似）
 *   - inline-block → inline、table 系 display → block に丸める
 *   - bolder/lighter は bold/normal に丸める
 */
#ifndef IFUTO_CSS_H
#define IFUTO_CSS_H

#include "common.h"
#include "strutil.h"
#include "arena.h"
#include "dom.h"

/* ---- 値 ---- */
typedef enum { IF_V_LEN, IF_V_IDENT, IF_V_COLOR, IF_V_RAW } IfValKind;
typedef enum { IF_U_PX, IF_U_EM, IF_U_REM, IF_U_PT, IF_U_PCT, IF_U_AUTO } IfLenUnit;

typedef struct { float v; u8 unit; } IfLen; /* unit=IF_U_AUTO は「auto」 */
#define IF_LEN_AUTO ((IfLen){ 0.0f, IF_U_AUTO })

typedef struct {
    u16 prop;        /* IfPropId */
    u8 important;
    u8 vkind;        /* IfValKind */
    u8 unit;         /* IfLenUnit (IF_V_LEN) */
    float num;       /* IF_V_LEN の数値 */
    u32 color;       /* IF_V_COLOR: 0xRRGGBBAA */
    IfStr text;      /* IF_V_IDENT / IF_V_RAW */
} IfDecl;

enum {
    IF_P_DISPLAY, IF_P_COLOR, IF_P_BACKGROUND_COLOR,
    IF_P_FONT_SIZE, IF_P_FONT_WEIGHT, IF_P_FONT_STYLE, IF_P_TEXT_DECORATION,
    IF_P_MARGIN_TOP, IF_P_MARGIN_RIGHT, IF_P_MARGIN_BOTTOM, IF_P_MARGIN_LEFT,
    IF_P_PADDING_TOP, IF_P_PADDING_RIGHT, IF_P_PADDING_BOTTOM, IF_P_PADDING_LEFT,
    IF_P_BORDER_TOP_WIDTH, IF_P_BORDER_RIGHT_WIDTH, IF_P_BORDER_BOTTOM_WIDTH, IF_P_BORDER_LEFT_WIDTH,
    IF_P_BORDER_COLOR,
    IF_P_WIDTH, IF_P_HEIGHT, IF_P_TEXT_ALIGN, IF_P_LINE_HEIGHT, IF_P_WHITE_SPACE,
    IF_P_N
};

/* ---- セレクタ ---- */
typedef enum { IF_CX_DESCENDANT, IF_CX_CHILD } IfCombinator;

typedef struct {
    bool has_tag;
    u16 tag;         /* 既知タグなら ID。未知なら tag_name を CI 比較 */
    IfStr tag_name;
    IfStr *classes; u32 n_classes;
    IfStr *ids;     u32 n_ids;
} IfCompound;

typedef struct {
    IfCompound *comps;  /* 左→右 */
    u8 *combs;          /* combs[i] = comps[i] と comps[i+1] の間の結合子 */
    u32 n_comps;
    u32 spec;           /* (ids<<16)|(classes<<8)|types で事前計算 */
} IfSelector;

typedef struct {
    IfSelector *sels; u32 n_sels;
    IfDecl *decls;    u32 n_decls;
    u32 order;         /* decl 単位で一意な単調 base（order + decl_index が勝者の全順序キー） */
} IfRule;

/* ---- RuleSet 風セレクタインデックス（Blink RuleSet 相当のバケツ戦略） ----
 * 右端 compound の最強特徴（id > class > tag > universal）で各 (rule, sel) を
 * 単一バケツに格納。要素側は自身の特徴が指すバケツ + universal だけを全マッチすれば、
 * マッチ集合は全走査と一致する（バケツ外のエントリは右端 compound が必ず不成立）。
 * キー照合は matcher と完全同一規則: id/class は memcmp 完全一致（case-sensitive）、
 * tag は canonical lowercase（既知タグは静的名、未知タグは ASCII-lowercase 複製）。 */
typedef struct { u32 rule, sel; } IfSelEntry;
typedef struct { u32 hash; IfStr key; u32 start, len; } IfSelBucket; /* hash 昇順ソート */
typedef struct {
    IfSelEntry *pool;                 /* 全エントリ（バケツごと連続スライス、arena 所有） */
    u32 n_pool;                       /* 0 = 未構築（OOM フォールバックで naive 経路） */
    IfSelBucket *id_b;  u32 n_id;
    IfSelBucket *cl_b;  u32 n_cl;
    IfSelBucket *tg_b;  u32 n_tg;
    u32 univ_start, univ_len;         /* pool 内の universal スライス */
} IfRuleSet;

typedef struct IfStyleSheet {
    IfRule *rules; u32 n_rules;
    u32 n_dropped_rules;
    u32 n_dropped_decls;
    u32 order_end;                    /* このシートが消費した order の排他上端（シート間隙間の決定値） */
    IfRuleSet rs;
} IfStyleSheet;

/* ---- 計算済みスタイル ---- */
enum { IF_D_INLINE, IF_D_BLOCK, IF_D_LIST_ITEM, IF_D_NONE };
enum { IF_TA_LEFT, IF_TA_CENTER, IF_TA_RIGHT };
enum { IF_WS_NORMAL, IF_WS_PRE };

typedef struct IfStyle {
    u32 color, bg;            /* RGBA8。bg の alpha 0 は透過 */
    float font_size;          /* px（解決済み） */
    float line_height;        /* px（0 = auto → font_size*1.2 相当） */
    IfLen width, height;
    IfLen margin[4], padding[4]; /* T R B L */
    float border_w[4];        /* px */
    u32 border_color;
    u8 display;               /* IF_D_* */
    u8 text_align;            /* IF_TA_* */
    u8 white_space;           /* IF_WS_* */
    bool bold, italic, underline, strike;
} IfStyle;

/* ---- API ---- */
IfStyleSheet *if_css_parse(IfArena *a, IfStr css, u32 order_base);
/* inline style 属性用: 宣言列のみパース */
u32 if_css_parse_decls(IfArena *a, IfStr text, IfDecl **out);
bool if_css_color(IfStr s, u32 *out);                    /* #hex/rgb()/色名 → RGBA8 */
float if_css_resolve_len(IfLen l, float self_fs, float root_fs); /* px へ（PCT/EM は呼び出し側で分母を掛ける） */

/* DOM に計算済みスタイルを付与する（UA シート + <style> 要素 + inline style）。 */
void if_style_apply(IfArena *a, IfDom *dom);

bool if_css_match_selector(const IfNode *n, const IfSelector *sel);

/* 監査・差分検証用の kill switch（既定 0 = RuleSet 索引経路）。1 にすると全ノードで
 * 旧来の全走査マッチになる。索引の候補集合 ≡ 全走査のマッチ集合であることを
 * テストが on/off で機械監査するために存在（CoJIT の on/off oracle と同型）。 */
void if_css_set_naive_matching(int enabled);

/* 観測用: 直近 if_style_apply の intern unique 数（computed style interning。
 * テスト/計測が「dedup が効いているか」を機械検査するためのフック） */
extern u32 if_css_intern_last;

/* ---- lazy computed style（md fast-DOM → 線形 layout の CLI 出力路専用） ----
 * 前提: author シート無し（md では <style> は TEXT 化される）かつ md_ws_stripped DOM。
 * computed style は (tag, parent_st, root_fs) の純粋関数なので、layout の DFS 訪問
 * タイミングで必要箇所だけ解決しても、if_style_apply の全面走査と全ノード同値になる
 * （compute_node と intern の規則をそのまま共有。詳細は css.c の st_resolve_memo 参照）。
 * ctx は arena 局所（並列 shard が別 arena を別 ctx で触る設計のため競合しない）。 */
typedef struct { uintptr_t parent; uintptr_t key2; const IfStyle *st; } IfStCacheEnt;
#define IF_STCACHE_BITS 14
#define IF_STCACHE_SIZE (1u << IF_STCACHE_BITS)
typedef struct { IfStyle **tab; u32 cap, n; IfArena *a; } IfStyleIntern;
typedef struct IfStyleLazy {
    IfArena *arena;
    const IfStyleSheet *sheet;  /* UA シート（この arena 内の全コピー） */
    IfStyleIntern in;
    IfStCacheEnt *ctab;         /* IF_STCACHE_SIZE の直接マップ（arena calloc） */
    float rfs;                  /* rem 基準。html 解決後に呼出側が 1 度だけ確定 */
    /* 1 エントリメモ（st_resolve_memo と同一キー規則 (parent_st, tag|name)）。
     * (pk,k2)→st の写像は UA シートのみの lazy 前提の下で関数的（intern 安定）な
     * ため ctab 追い出しと無関係に成立。兄弟走査は同キー連打が支配的（li*, td*, p*） */
    uintptr_t m_pk, m_k2;     /* m_k2==0 は空（k2 の下位 bit は常に 1 埋まりで 0 非合法） */
    const IfStyle *m_st;
    uintptr_t m_pkb, m_k2b;   /* 2 番スロット（LRU。body 直下の交互アクセスを吸収） */
    const IfStyle *m_stb;
} IfStyleLazy;
void  if_style_lazy_init(IfStyleLazy *lz, IfArena *a);
/* ELEMENT 専用（TEXT 等は呼ばない。従来パスでも非 ELEMENT は style NULL のまま） */
const IfStyle *if_style_lazy_get(IfStyleLazy *lz, IfNode *n, const IfStyle *parent_st, float rfs);
/* lazy 前提条件（md fast-DOM、author sheet 無し） + kill switch IF_STYLE_LAZY=0 */
bool  if_md_style_lazy_ok(const IfDom *dom);

#endif
