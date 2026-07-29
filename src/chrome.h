/* Ifuto — TUI クローム モデル（純粋層。tty/font 非依存で単体テスト可能）
 *
 * 不変条件（CHROME_SCOPE 由来）:
 *   - C1: タブ文書寿命 = arena スコープ。クローズ/アンロードは arena destroy（正確・定数時間）。
 *   - メタデータ（url/title/IfTab）は文書 arena に入れない（unload で残すため malloc 管理、4KB 上限）。
 *   - 新タブは空白（INV-1）。タイマー駆動は禁止（toast は次アクションで消える、INV-5）。
 */
#ifndef IFUTO_CHROME_H
#define IFUTO_CHROME_H

#include "common.h"
#include "strutil.h"
#include "arena.h"
#include "dom.h"
#include "layout.h"
#include "render.h"

#define IF_TABS_MAX 64
#define IF_URL_CAP 4096
#define IF_TITLE_CAP 256
#define IF_OMNI_CAP 2048

typedef struct IfFsOps {
    bool  (*exists)(const char *path, void *ctx);   /* 通常ファイル */
    /* path を arena に読み込む（ディレクトリ/不可は NULL） */
    IfStr (*read_file)(IfArena *a, const char *path, void *ctx);
    void *ctx;
} IfFsOps;

typedef struct IfTab {
    i32 id;
    char *url;     /* malloc、IF_URL_CAP まで。空タブは "" */
    char *title;   /* malloc、IF_TITLE_CAP まで（表示用に UTF-8 のまま） */
    IfArena *doc;  /* 文書所有 arena（dom+style）。NULL=未ロード */
    IfArena *view; /* layout+grid（再レイアウトで破棄・再構築） */
    IfDom *dom;
    IfLayout *lay;
    IfGrid *grid;
    i32 scroll;    /* 表示先頭行 */
    i32 link_idx;  /* フォーカス中リンク（-1=なし） */
    bool dirty;    /* リサイズで再レイアウトが必要 */
} IfTab;

typedef enum { CM_NORMAL = 0, CM_OMNIBOX, CM_HELP } IfChromeMode;

typedef struct IfChrome {
    IfFsOps fs;
    IfArena engine_scratch; /* 読み込み一時（load で使い回し） */
    IfTab *tabs[IF_TABS_MAX];
    i32 n_tabs;
    i32 active;
    i32 next_id;
    IfChromeMode mode;
    /* omnibox */
    char omni[IF_OMNI_CAP];
    u32 omni_len;
    /* quit 確認（ now は time(NULL) 注入） */
    i64 quit_armed_at;
    /* toast（次アクションで消える） */
    char toast[160];
    u8 toast_len;
} IfChrome;

void  if_chrome_init(IfChrome *c, IfFsOps fs);
void  if_chrome_destroy(IfChrome *c);

/* タブ操作。open は path をロードして新タブ（新タブをアクティブに）。失敗時 false で toast 設定。 */
bool  if_chrome_open(IfChrome *c, const char *path, i32 width);
IfTab *if_chrome_new_blank(IfChrome *c);
bool  if_chrome_close(IfChrome *c);           /* 現タブを閉じる。最後の1枚は空白化 */
void  if_chrome_switch(IfChrome *c, i32 idx);
bool  if_chrome_reload(IfChrome *c, i32 width);
void  if_chrome_relayout(IfChrome *c, i32 width); /* 現タブのみ（他は dirty マーク） */

IfTab *if_chrome_cur(IfChrome *c);

/* スクロール: クランプして新位置を返す。vh=可視行数 */
i32 if_chrome_scroll(IfChrome *c, i32 delta, i32 vh);
void if_chrome_scroll_to(IfChrome *c, i32 pos, i32 vh);

/* オムニボックス解決: input → 実在ファイルパスを out へ。
 * 0=成功 / 1=URL スキーム（ネットワークは v0.3）/ 2=見つからない */
i32  if_chrome_resolve(IfChrome *c, const char *input, const char *cwd,
                       char *out, u32 cap);

/* quit: true で終了。タブ複数時は 3 秒以内の 2 連打のみ確定（CHROME_SCOPE #18 の簡易版） */
bool if_chrome_quit(IfChrome *c, i64 now);

/* リンク: idx は 0..n-1 へクランプ/循環。-1 からも開始 */
i32 if_chrome_link_move(IfChrome *c, i32 delta);

/* メモリ計装（C1: 現タブの文書 arena 正確予約量。KB 表示用にバイトで返す） */
u64 if_chrome_cur_doc_bytes(IfChrome *c);

/* 実 fs 実装（生产） */
bool  if_fs_exists_real(const char *path, void *ctx);
IfStr if_fs_read_real(IfArena *a, const char *path, void *ctx);

#endif
