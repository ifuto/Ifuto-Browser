/* Ifuto — DOM。
 *
 * 設計:
 *   - 全ノードはページ arena から確保。生存期間はページと一体。
 *   - よく使う性質はタグ ID（列挙）で O(1) 判定し、未知タグは文字列で保持。
 *   - class 照合はカスケード側で属性値を都度分割（文書規模なら十分速い。測定して悪ければ索引化）。
 */
#ifndef IFUTO_DOM_H
#define IFUTO_DOM_H

#include "common.h"
#include "strutil.h"
#include "arena.h"

typedef enum {
    IF_TAG_UNKNOWN = 0,
    IF_TAG_HTML, IF_TAG_HEAD, IF_TAG_BODY, IF_TAG_TITLE, IF_TAG_META, IF_TAG_LINK,
    IF_TAG_STYLE, IF_TAG_SCRIPT, IF_TAG_DIV, IF_TAG_SPAN, IF_TAG_P, IF_TAG_A,
    IF_TAG_B, IF_TAG_I, IF_TAG_U, IF_TAG_S, IF_TAG_EM, IF_TAG_STRONG, IF_TAG_CODE,
    IF_TAG_PRE, IF_TAG_BLOCKQUOTE, IF_TAG_H1, IF_TAG_H2, IF_TAG_H3, IF_TAG_H4,
    IF_TAG_H5, IF_TAG_H6, IF_TAG_UL, IF_TAG_OL, IF_TAG_LI, IF_TAG_DL, IF_TAG_DT,
    IF_TAG_DD, IF_TAG_TABLE, IF_TAG_THEAD, IF_TAG_TBODY, IF_TAG_TFOOT, IF_TAG_TR, IF_TAG_TD,
    IF_TAG_TH, IF_TAG_CAPTION, IF_TAG_IMG, IF_TAG_BR, IF_TAG_HR, IF_TAG_FORM,
    IF_TAG_INPUT, IF_TAG_BUTTON, IF_TAG_SELECT, IF_TAG_OPTION, IF_TAG_LABEL,
    IF_TAG_TEXTAREA, IF_TAG_HEADER, IF_TAG_FOOTER, IF_TAG_NAV, IF_TAG_MAIN,
    IF_TAG_SECTION, IF_TAG_ARTICLE, IF_TAG_ASIDE, IF_TAG_FIGURE, IF_TAG_FIGCAPTION,
    IF_TAG_ADDRESS, IF_TAG_SMALL, IF_TAG_BIG, IF_TAG_SUB, IF_TAG_SUP, IF_TAG_MARK,
    IF_TAG_TIME, IF_TAG_Q, IF_TAG_CITE, IF_TAG_ABBR, IF_TAG_DFN, IF_TAG_KBD,
    IF_TAG_SAMP, IF_TAG_VAR, IF_TAG_FONT, IF_TAG_CENTER, IF_TAG_STRIKE, IF_TAG_TT,
    IF_TAG_WBR, IF_TAG_NOSCRIPT, IF_TAG_IFRAME, IF_TAG_OBJECT, IF_TAG_PARAM,
    IF_TAG_SOURCE, IF_TAG_TRACK, IF_TAG_VIDEO, IF_TAG_AUDIO, IF_TAG_CANVAS,
    /* foreign content の構造に関わるタグ（表示名は挿入時に case 調整される） */
    IF_TAG_SVG, IF_TAG_MATH, IF_TAG_MI, IF_TAG_MO, IF_TAG_MN, IF_TAG_MS,
    IF_TAG_MTEXT, IF_TAG_ANNOTATION_XML, IF_TAG_FOREIGNOBJECT, IF_TAG_DESC,
    IF_TAG_MGLYPH, IF_TAG_MALIGNMARK,
    IF_TAG_LISTING, IF_TAG_PLAINTEXT, IF_TAG_XMP, IF_TAG_NOEMBED, IF_TAG_NOFRAMES,
    IF_TAG_RUBY, IF_TAG_RP, IF_TAG_RT, IF_TAG_RTC, IF_TAG_RB,
    IF_TAG_FRAMESET, IF_TAG_FRAME, IF_TAG_OPTGROUP, IF_TAG_LEGEND, IF_TAG_FIELDSET,
    IF_TAG_BASE, IF_TAG_COL, IF_TAG_COLGROUP, IF_TAG_AREA, IF_TAG_MAP, IF_TAG_EMBED,
    IF_TAG_DIR, IF_TAG_MENU, IF_TAG_APPLET, IF_TAG_MARQUEE, IF_TAG_BASEFONT,
    IF_TAG_KEYGEN,
    IF_TAG_TEMPLATE, /* WPT で頻出。末尾追加（既存タグ ID の安定性を守る） */
    IF_TAG_NOBR,     /* AAA (adoption agency) の対象書式要素。末尾追加 */
    IF_TAG_DETAILS, IF_TAG_DIALOG, IF_TAG_HGROUP, IF_TAG_SEARCH, IF_TAG_SUMMARY,
    IF_TAG_BGSOUND, /* in-head の void（tests19: <bgsound> は head の子） */
    IF_TAG_IMAGE,   /* in-body で img へ改名（<image/> WPT quirk）。末尾追加 */
    IF_TAG_N_TAGS
} IfTag;

/* 要素の名前空間（foreign content）。DOMAPI は持たない最小表現。 */
enum { IF_NS_HTML = 0, IF_NS_SVG = 1, IF_NS_MATHML = 2 };

typedef struct {
    IfStr name;   /* スライス（比較は CI） */
    IfStr value;  /* 文字参照はデコード済み（ページ arena 所有 or 入力スライス） */
} IfAttr;

typedef enum { IF_NODE_DOCUMENT, IF_NODE_ELEMENT, IF_NODE_TEXT, IF_NODE_COMMENT, IF_NODE_DOCTYPE } IfNodeKind;

struct IfStyle; /* css.h で定義（相互 include 回避） */

/* DOCTYPE ノードの付帯情報（全ノードに埋めず別体——IfNode を太らせない） */
typedef struct IfDoctype {
    IfStr name, pub, sys;
    u8 has_name, has_pub, has_sys;
} IfDoctype;

/* IfNode.flags ビット */
#define IF_NF_SLIM 0x01 /* slim-DOM で剃られた領域の印（接続・本文を構築しない） */
/* TEXT ノードの内容分類（md parse が確定。描画に必要な幅を 0 追加走査で還元する）
 *  - ASCII_VIS: 全バイト 0x21-0x7E → 可視幅 == n（空白・ゼロ幅・全角を含まない）
 *  - CJK3W2: n%3==0 かつ全 3 バイト組が if_utf8_band_w2 → 可視幅 == 2*(n/3)
 * 0（mixed）は常に安全側（layout の従来経路）。分類述語は layout の走査と同一定義。 */
#define IF_NF_TXTCLS_ASCII_VIS 0x02
#define IF_NF_TXTCLS_CJK3W2   0x04
#define IF_NF_TXTCLS_MASK     0x06

/* 法則「画面描画に関係ないものは DOM しない」の実装スイッチ（dom.c）。
 * true のとき template 配下の子孫・本文を DOM に構築しない
 * （script は v0.3 で実行対象になったため剃らない: slim_root_tag 参照。
 * tree 構築の状態機械は完全に回す: stack 規則不変。適合性 Track B は
 * false=full DOM が既定。実ブラウズ経路 TUI/GUI は true）。
 * style は cascade が本文を読むため残す（描画に関係する）。 */
extern bool if_dom_slim;

typedef struct IfNode {
    IfNodeKind kind;
    u16 tag;             /* ELEMENT のみ有意 */
    u8 ns;               /* ELEMENT: IF_NS_* */
    u8 flags;            /* IF_NF_* */
    /* tag_name(ELEMENT) / text(TEXT,COMMENT) / dtype(DOCTYPE) は kind で排他 → union 化
     * （メモリ則: 112B→88B。PI は COMMENT で target+data を同時保持する必要があるため、
     *   target は attrs[0].name に入れる＝COMMENT は attrs を使わない規約）。 */
    union { IfStr tag_name; IfStr text; IfDoctype *dtype; } u;
    IfAttr *attrs;
    u32 n_attrs;
    const struct IfStyle *style; /* カスケード後に付与（intern 所有・不変。監査: 全消費者 read-only） */
    struct IfNode *parent, *first_child, *last_child, *next_sibling;
} IfNode;
/* template の「content」文書フラグメントは全ノード中きわめて稀（0〜数個/文書）なため
 * IfNode から追い出し IfDom 側の rare-data 写像で持つ（88B→80B。16MB md で
 * 1.6M ノード×8B ≈ −12.3MiB の parse arena 削減。Chromium RareData と同型の思想）。
 * 参照は if_dom_tpl_content / if_dom_tpl_set_content のみ（フィールド直アクセス廃止）。 */
typedef struct IfTplMapEnt { IfNode *tpl, *content; } IfTplMapEnt;

typedef struct {
    IfArena *arena;      /* 所有 */
    IfNode *root;        /* DOCUMENT ノード */
    u32 n_nodes;
    u32 n_errors;        /* パーサが回復したエラー数（統計用） */
    bool quirks;         /* DOCTYPE 完全表で判定（limited-quirks は false＝spec 上 no-quirks 同効） */
    IfStr title;         /* <title> のテキスト（見つからなければ empty） */
    u8 has_selectedcontent; /* parse 中に <selectedcontent> を観測（post-pass clone の走査スイッチ） */
    u8 has_style;         /* <style> 要素を観測（author sheet 収集の走査スイッチ） */
    u8 has_script;        /* HTML ns <script> 要素を観測（script 実行の走査スイッチ。
                           * 0 なら走査自体を行わない = script 非含有文書はゼロコスト） */
    u8 md_ws_stripped;    /* md fast path: 純粋ブロック容器直下の ws-only TEXT を生成しなかった
                           * （layout は当該容器で兄弟相殺を旧 DOM 同値に補正する） */
    IfNode *md_body_mid;  /* md 2-slice パースの分割境界 = body 直下の「B 側先頭子」
                           * （レイアウト並列化の子数計数/中点ウォークを構造消去するための
                           *   ヒント。非 slice 経路・HTML では NULL。性能のみで生出力不変） */
    /* template rare-data 写像（開放番地法。cap=0 は未構築 = 文書に template 無し） */
    IfTplMapEnt *tpl_map;
    u32 tpl_map_n, tpl_map_cap;
} IfDom;

/* 入力はドキュメント寿命中生存していること（ページ arena にコピーして呼ぶのが安全）。 */
IfDom *if_parse_html(IfArena *arena, IfStr input);

/* fragment 解析（WHATWG 13.4）。ctx は html5lib-tests の #document-fragment 値
 * （"body" / "svg path" / "math mi" / "svg foreignObject" 形式）。生成される
 * 文書は Document 直下に仮想 html root を持ち、fragment 結果はその子群。 */
IfDom *if_parse_html_fragment(IfArena *arena, IfStr input, const char *ctx);

const char *if_tag_name(u16 tag);                 /* canonical lowercase 名 or NULL */
u16         if_tag_id(IfStr name);                /* 既知タグの ID、未知は IF_TAG_UNKNOWN */
bool        if_tag_is_void(u16 tag);
bool        if_tag_is_rawtext(u16 tag);

IfStr       if_dom_attr(const IfNode *n, const char *name_ci); /* なければ empty */
bool        if_dom_has_class(const IfNode *n, IfStr cls);

/* template rare-data: content フラグメントの側車写像（IfNode のみから分離。上記参照）。
 * tpl 未登録なら NULL。set は template 開始時に 1 度だけ（再 set は上書き） */
IfNode     *if_dom_tpl_content(const IfDom *d, const IfNode *tpl);
void        if_dom_tpl_set_content(IfDom *d, IfNode *tpl, IfNode *content);

/* 子要素をドキュメント順に列挙 */
static inline IfNode *if_node_first_elem_child(IfNode *n) {
    for (IfNode *c = n ? n->first_child : NULL; c; c = c->next_sibling)
        if (c->kind == IF_NODE_ELEMENT) return c;
    return NULL;
}

/* text ノードを UTF-8 のまま子孫から連結取得（dom.c）。arena に新規確保。 */
IfStr if_dom_text_content(IfArena *a, const IfNode *n);

/* ---- script 実行（v0.3, src/script.c）専用の最小 DOM 変更面。
 * 木の操作は「子群を単一 TEXT に置換」「head 先頭への title 生成」のみに限定し、
 * 削除/移動の一般 API は設けない（木の不変条件を狭く保つ。正本: docs/SCRIPTING.md）。 */
IfNode *if_dom_find_tag_dfs(const IfDom *d, u16 tag); /* 文書順最初の ELEMENT（無ければ NULL） */
IfNode *if_dom_find_by_id(const IfDom *d, IfStr id);  /* id 属性一致の最初の ELEMENT（値は case-sensitive、無ければ NULL） */
void    if_dom_set_text(IfArena *a, IfNode *n, IfStr t); /* ELEMENT 子群を単一 TEXT 子に置換（t.n==0 は全子除去） */
IfNode *if_dom_title_set(IfArena *a, IfDom *d, IfStr t); /* <title> 設定（無ければ head 先頭に生成）。d->title も更新 */

/* デバッグ用のツリー印字（out は FILE*）。表示用のためポインタは void。 */
void if_dom_dump(const IfDom *dom, void *out_FILE);

bool if_tag_is_rcdata(u16 tag);
bool if_dom_tag_table_sane(void); /* タグ表長さ整合性（テスト用） */

/* html5lib-tests ツリー構築形式（"| indented"）での DOM シリアライズ。
 * tree-construction 採点ハーネス用。 */
void if_dom_serialize_wpt(const IfDom *dom, void *out_FILE);

/* fragment（#document-fragment）版: 仮想 html root の子群だけを出す */
void if_dom_serialize_wpt_frag(const IfDom *dom, void *out_FILE);

#endif
