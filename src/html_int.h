/* tokenizer ↔ tree builder 間の内部インタフェース（公開しない） */
#ifndef IFUTO_HTML_INT_H
#define IFUTO_HTML_INT_H

#include "common.h"
#include "strutil.h"
#include "arena.h"
#include "dom.h"

typedef enum {
    TOK_TEXT, TOK_START, TOK_END, TOK_COMMENT, TOK_DOCTYPE, TOK_EOF
} IfTokKind;

typedef struct {
    IfTokKind kind;
    IfStr text;        /* TEXT / COMMENT / DOCTYPE(name, lowercase 済み) */
    u16 tag;           /* START/END: 既知タグ ID（未知は UNKNOWN で tag_raw を見る） */
    IfStr tag_raw;     /* 生のタグ名スライス */
    IfAttr *attrs;     /* START: ページ arena 所有、重複は first-wins で除去済み */
    u32 n_attrs;
    bool self_closing;
    /* DOCTYPE のみ（has_* が立っている値のみ有効） */
    IfStr dt_pub, dt_sys;
    u8 dt_has_name, dt_has_pub, dt_has_sys;
    /* COMMENT のみ: Processing Instruction（<?target data?>）だった場合 */
    u8 is_pi;
    IfStr pi_target;
} IfTok;

typedef struct {
    const u8 *src;
    u32 len;
    u32 pos;
    IfArena *arena;
    u16 raw_tag;       /* 0 以外: rawtext/RCDATA モード（その要素の終了タグまで TEXT として読む） */
    u8 raw_rcdata;     /* raw 内容で文字参照を解決する（title/textarea） */
    u8 strip_lf;       /* raw 内容の先頭 LF を 1 つ捨てる（textarea の仕様） */
    u8 cdata_foreign;  /* tree builder が設定: 現在位置が foreign content（CDATA 許可） */
    u8 plaintext;      /* <plaintext> 以降: 残り全入力を 1 個の TEXT にする */
    u8 in_attr_ctx;    /* 属性値デコード中ほど立つ（名前参照の ambiguous-amp 規則用） */
    u32 errors;
} IfHtmlTok;

void  if_tok_init(IfHtmlTok *t, IfArena *arena, IfStr input);
IfTok if_tok_next(IfHtmlTok *t);
void  if_tok_set_raw(IfHtmlTok *t, u16 tag);

#endif
