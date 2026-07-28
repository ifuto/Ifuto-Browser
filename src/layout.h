/* Ifuto — レイアウト（整数セル座標系）
 *
 * 不変条件:
 *   - 座標はセル単位の整数（px→cell の丸めは IF_CHAR_W_PX/IF_ROW_H_PX の一箇所のみ）。
 *   - ボックスツリーはページ arena 所有。個別解放・再配置は存在しない。
 *   - LINE ボックスは BLOCK の直接の子として混在しうる（無名ブロックの簡略化）。
 *   - seg の x は絶対座標（wrap 時に確定）。paint 側に座標計算を持ち込まない。
 */
#ifndef IFUTO_LAYOUT_H
#define IFUTO_LAYOUT_H

#include "common.h"
#include "arena.h"
#include "dom.h"
#include "css.h"

#define IF_CHAR_W_PX 8.0f
#define IF_ROW_H_PX 16.0f

enum { IF_BOX_BLOCK, IF_BOX_LINE };

typedef struct IfSeg {
    IfStr text;            /* 表示テキスト（DOM スライス or arena 複製） */
    i32 x;                 /* 絶対 x（セル） */
    i32 w;                 /* 幅（セル） */
    const IfStyle *st;     /* 色・装飾の出処 */
} IfSeg;

typedef struct IfBox {
    u8 kind;               /* IF_BOX_* */
    IfNode *node;          /* BLOCK: 対応要素（LINE: NULL＝無名） */
    const IfStyle *st;
    i32 x, y, w, h;        /* border-box の絶対セル座標 */
    struct IfBox *first_child, *last_child, *next_sibling; /* last_child: O(1) 追加のため */
    /* LINE ペイロード */
    IfSeg *segs;
    u32 n_segs;
    u8 text_align;
} IfBox;

typedef struct {
    u32 n;
    IfStr href;
} IfLink;

typedef struct IfLayout {
    IfArena *arena;
    IfBox *root;
    i32 width;             /* viewport セル幅 */
    i32 height;            /* 総コンテンツ高（セル） */
    IfLink *links;
    u32 n_links;
} IfLayout;

/* dom+style からレイアウトを構築。width はセル。失敗しない（壊れた構造は空行に落ちる）。 */
IfLayout *if_layout_build(IfArena *arena, IfDom *dom, i32 width_cells);

void if_layout_dump(const IfLayout *lay, void *out_FILE);

#endif
