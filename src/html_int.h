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
    IfStr text;        /* TEXT / COMMENT / DOCTYPE(name) */
    u16 tag;           /* START/END: 既知タグ ID（未知は UNKNOWN で tag_raw を見る） */
    IfStr tag_raw;     /* 生のタグ名スライス */
    IfAttr *attrs;     /* START: ページ arena 所有、重複は first-wins で除去済み */
    u32 n_attrs;
    bool self_closing;
} IfTok;

typedef struct {
    const u8 *src;
    u32 len;
    u32 pos;
    IfArena *arena;
    u16 raw_tag;       /* 0 以外: rawtext モード（その要素の終了タグまで TEXT として読む） */
    u32 errors;
} IfHtmlTok;

void  if_tok_init(IfHtmlTok *t, IfArena *arena, IfStr input);
IfTok if_tok_next(IfHtmlTok *t);
void  if_tok_set_raw(IfHtmlTok *t, u16 tag);

#endif
