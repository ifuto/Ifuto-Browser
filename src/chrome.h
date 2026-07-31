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
#include "store.h" /* IfFsOps の単一定義はこちら（INV-10: 単一fs窓口） */

#define IF_TABS_MAX 64
#define IF_URL_CAP 4096
#define IF_TITLE_CAP 256
#define IF_OMNI_CAP 2048
#define IF_GROUP_CAP 64

typedef struct IfTab {
    i32 id;
    char *url;     /* malloc、IF_URL_CAP まで。空タブは "" */
    char *title;   /* malloc、IF_TITLE_CAP まで（表示用に UTF-8 のまま） */
    char *group;   /* malloc、IF_GROUP_CAP まで。NULL=無グループ（#11 最小形） */
    IfArena *doc;  /* 文書所有 arena（dom+style）。NULL=未ロード（セッション復元で多発） */
    IfArena *view; /* layout+grid（再レイアウトで破棄・再構築） */
    IfDom *dom;
    IfLayout *lay;
    /* v0.2: 全面 grid は保持しない（viewport 相対方式、render.h の規約参照）。
     * 迷子の代わりに doc 全体の総行高をキャッシュする（scroll clamp 用） */
    i32 doc_h;
    i32 scroll;    /* 表示先頭行 */
    i32 link_idx;  /* フォーカス中リンク（-1=なし） */
    bool dirty;    /* リサイズで再レイアウトが必要 */
} IfTab;

typedef enum { CM_NORMAL = 0, CM_OMNIBOX, CM_HELP, CM_SEARCH, CM_BOOKMARKS } IfChromeMode;

typedef struct IfChrome {
    IfFsOps fs;
    IfStore store;   /* 永続層（C2）。enabled=false なら無効で静かに no-op */
    i64 now;         /* 呼び出し側注入（テスト/純粋性のため time() を直接読まない） */
    IfArena engine_scratch; /* 読み込み一時（load で使い回し） */
    IfTab *tabs[IF_TABS_MAX];
    i32 n_tabs;
    i32 active;
    i32 next_id;
    IfChromeMode mode;
    /* omnibox */
    char omni[IF_OMNI_CAP];
    u32 omni_len;
    /* 検索結果のスナップショット（CM_SEARCH。最大 9 件表示＝1..9 直ジャンプ） */
    i32 search_hits[16];
    i32 n_search_hits;
    /* quit 確認（ now は time(NULL) 注入） */
    i64 quit_armed_at;
    /* toast（次アクションで消える） */
    char toast[160];
    u8 toast_len;
} IfChrome;

void  if_chrome_init(IfChrome *c, IfFsOps fs);
void  if_chrome_destroy(IfChrome *c);

/* タブ操作。open は path をロードして新タブ（新タブをアクティブに）。失敗時 false で toast 設定。
 * 状態変化（open/close/new/switch/group）は即座に session.txt へ保存する（crash 復元のため）。 */
bool  if_chrome_open(IfChrome *c, const char *path, i32 width);
IfTab *if_chrome_new_blank(IfChrome *c);
bool  if_chrome_close(IfChrome *c, i32 width); /* 現タブを閉じる。最後の1枚は空白化 */
void  if_chrome_switch(IfChrome *c, i32 idx, i32 width); /* 未ロードは切替時に遅延ロード */
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

/* セッション復元: session.txt をメタのみ再現（doc=NULL）。active タブのみ即ロード。
 * 戻り値=復元したタブ数（0=セッションなし/失敗）。自動保存は発火しない。 */
i32  if_chrome_restore(IfChrome *c, i32 width);

/* グループ割当（@ コマンドのモデル側）。name=NULL/"" = 解除。結果は session に即保存 */
void if_chrome_set_group(IfChrome *c, const char *name);

/* タブ検索（#5 / INV-8 完全形の土台）: title+url への大小無視部分一致で
 * タブ index を返す。0 件以下/上限超過は截断。 */
i32 if_chrome_find_tabs(const IfChrome *c, const char *query, i32 *idx_out, i32 max);

/* ブックマーク: 現タブのトグル（存在すれば除去、無ければ追加）。結果は toast に。 */
void if_chrome_bookmark_cur(IfChrome *c);

/* quit: true で終了。タブ複数時は 3 秒以内の 2 連打のみ確定（CHROME_SCOPE #18 の簡易版） */
bool if_chrome_quit(IfChrome *c, i64 now);

/* リンク: idx は 0..n-1 へクランプ/循環。-1 からも開始 */
i32 if_chrome_link_move(IfChrome *c, i32 delta);

/* メモリ計装（C1: 現タブの文書 arena 正確予約量。KB 表示用にバイトで返す） */
u64 if_chrome_cur_doc_bytes(IfChrome *c);

/* 実 fs 実装（本番: 読み側は read_only だが書き系も含む完全実装） */
bool  if_fs_exists_real(const char *path, void *ctx);
IfStr if_fs_read_real(IfArena *a, const char *path, void *ctx);
bool  if_fs_write_real(const char *path, const void *buf, size_t n, void *ctx);
bool  if_fs_append_real(const char *path, const void *buf, size_t n, void *ctx);
bool  if_fs_mkpath_real(const char *dir, void *ctx);

#endif
