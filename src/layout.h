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

/* IfBox._pad[0]（LINE のみ有意）: seg バイト列 ≡ 発行セル列の保証フラグ。
 * wrap 時に全グリフを検査済み: 全文字が gw>0 かつ enc(dec)恒等 なら、no-ansi 発行は
 * セル再デコードなしに seg の生バイトの連結で再構成できる（0x20 ギャップは emit 側で埋める）。 */
#define IF_LF_DIRECT_BYTES 0x01

typedef struct IfSeg {
    IfStr text;            /* 表示テキスト（DOM スライス or arena 複製） */
    i32 x;                 /* 絶対 x（セル） */
    i32 w;                 /* 幅（セル） */
    const IfStyle *st;     /* 色・装飾の出処 */
} IfSeg;

typedef struct IfBox {
    /* 64B ぴったり配置（ポインタ→i32→小物の順。構築時 tail は layout.c の frame スタック） */
    struct IfBox *first_child, *next_sibling;
    IfNode *node;          /* BLOCK: 対応要素（LINE: NULL＝無名） */
    const IfStyle *st;
    IfSeg *segs;           /* LINE ペイロード */
    i32 x, y, w, h;        /* border-box の絶対セル座標 */
    u32 n_segs;
    u8 kind;               /* IF_BOX_* */
    u8 text_align;
    u8 _pad[2];
} IfBox;

typedef struct { i32 x0, y0, x1, y1; } IfLSpan; /* 表示矩形（セル座標 [x0,x1)×[y0,y1)） */

typedef struct {
    u32 n;
    IfStr href;
    /* クリック判定用の表示矩形列。収集は「fused-fit 成功の単行 ifc」内の <a> に
     * 限定（複数行 wrap へ逃げた失敗経路・flatten 経路は未収集 → 台帳の残課題）。
     * 木構築モード（no_boxlink=0）限定。線形 CLI では収集しない（零コスト保証） */
    IfLSpan *spans;
    u32 n_spans;
} IfLink;

#include "render_sweep.h"

typedef struct IfRowOps IfRowOps; /* render_ansi.c: 行スイープ発行の遅延構築 op 列 */

/* 行スイープ直接発行のためのコンパクト行レコード（24B）。
 * sweep が読むのは y / segs / n_segs / flags のみ（全 src 監査済）で、
 * IfBox LINE（64B＋ポインタ間参照）は render 出力の決定に関与しない。 */
typedef struct IfRLine {
    const IfSeg *segs;
    u32 n_segs;
    i32 y;
    u16 flags;          /* IF_LF_DIRECT_BYTES 等（IfBox._pad[0] と同じ意味） */
    u16 _pad;
} IfRLine;

/* lines ログの格納: 値チャンクの連結リスト。
 * arena の成長規約（旧バッファは回収不能）では flat×2 成長チェーンの死蔵が
 * 支配的になる（実測 611k 行で ~50MB）ため、4096 項チャンクを継ぐ。
 * メリット: 成長死蔵 ~0、並列 shard 結合がポインタ接続（コピー・y 再計算なし）。 */
#define IF_LCHUNK_N 4096u
typedef struct IfLChunk {
    struct IfLChunk *next;
    u32 n;
    IfRLine v[IF_LCHUNK_N];
} IfLChunk;

typedef struct IfLayout {
    IfArena *arena;
    IfBox *root;
    i32 width;             /* viewport セル幅 */
    i32 height;            /* 総コンテンツ高（セル） */
    IfLink *links;
    u32 n_links;
    /* 行スイープ直接発行（CLI 全量 dump のグリッドレス経路）用のログ。
     * IfBox 構造は不変（GUI/テスト/ダンプは従来どおりボックスを歩く）。 */
    IfLChunk *lines_head;  /* wrap_end_line の生成順 = DFS 順 = y 単調非減少 */
    IfLChunk *lines_tail;
    u32 n_lines;
    IfDeco *deco;          /* 装飾 op（DFS=paint 順。border/bg は y は開始、h は後埋め） */
    u32 n_deco;
    u64 cap_deco;
    IfRowOps *rowops;      /* 初回 sweep で構築・再利用（再帰的な逐次発行 2 回目以降は O(1) 準備） */
} IfLayout;

/* dom+style からレイアウトを構築。width はセル。失敗しない（壊れた構造は空行に落ちる）。 */
IfLayout *if_layout_build(IfArena *arena, IfDom *dom, i32 width_cells);
/* 線形モード（CLI 行スイープ専用。BLOCK 箱を再利用し親接続を消す。全出力は同値） */
/* CLI 行スイープ専用。lazy_style=1 のとき style 未適用 DOM でも正しく解決する
 * （解決規則は if_style_apply と同値。css.h の IfStyleLazy 注釈参照） */
IfLayout *if_layout_build_linear(IfArena *arena, IfDom *dom, i32 width_cells, u8 lazy_style);

void if_layout_dump(const IfLayout *lay, void *out_FILE);

#endif
