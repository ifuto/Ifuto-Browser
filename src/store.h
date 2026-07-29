/* Ifuto — 永続ストア層（C2: 単一ディレクトリ + tmp→rename→fsync の原子書換）
 *
 * 不変条件（CHROME_SCOPE 由来）:
 *   - 全てローカル完結（INV-2/INV-7: ネットワーク経路は構造的に存在しない）。
 *   - フォーマットはフラットテキスト（SQLite 不採用の根拠は §5）。
 *   - cwd に勝手に書かない（IFUTO_HOME > XDG_DATA_HOME/ifuto > ~/.local/share/ifuto）。
 *   - INV-9: `--show-paths` で全パスを提示する。
 *
 * レイヤ純粋性: fs 操作は全て IfFsOps 注入（単体テストは in-memory fake）。
 */
#ifndef IFUTO_STORE_H
#define IFUTO_STORE_H

#include "common.h"
#include "strutil.h"
#include "arena.h"

/* fs 抽象（chrome.c/tui.c/tests が共用する単一の窓口）。
 * 位置初期化互換のため ctx より後ろに拡張フィールドを置く（省略時 0=NULL）。 */
typedef struct IfFsOps {
    bool  (*exists)(const char *path, void *ctx);          /* 通常ファイル */
    IfStr (*read_file)(IfArena *a, const char *path, void *ctx);
    void *ctx;
    /* ---- slice-2 拡張: ストア書き込み系。NULL ならストア無効 ---- */
    bool (*write_file)(const char *path, const void *buf, size_t n, void *ctx);
    bool (*append)(const char *path, const void *buf, size_t n, void *ctx);
    bool (*mkpath)(const char *dir, void *ctx);            /* 再帰 mkdir */
} IfFsOps;

#define IF_STORE_DIR_CAP 1024
#define IF_HISTORY_MAX_BYTES (512u * 1024u)

/* ストアファイル名（INV-9 表示と内部実装の単一定義） */
#define IF_STORE_SESS_NAME "session.txt"
#define IF_STORE_HIST_NAME "history.tsv"
#define IF_STORE_BMRK_NAME "bookmarks.tsv"

typedef struct IfChrome IfChrome; /* 前方宣言（store.h は chrome.h に依存しない） */

/* セッション復元のパース結果。URL/title/group は arena 内文字列。group=NULL = 無し */
typedef struct IfSessionTab {
    i32 id;
    char *url;
    char *title;
    char *group;
    i32 scroll;
} IfSessionTab;

typedef struct IfStore {
    const IfFsOps *fs; /* 借用（IfChrome が所有） */
    char dir[IF_STORE_DIR_CAP];
    bool enabled;      /* dir 解決 or fs 未対応なら false（静かに無効化） */
    u32 hist_appends;  /* 縮退判定の負荷制御（32 回毎に容量点検） */
} IfStore;

/* dir 解決。create=false なら mkdir を行わない（--show-paths 用の無副作用解決）。
 * 成功=true で s->enabled 設定。失敗時 enabled=false（ストアは静かに無効） */
bool if_store_init(IfStore *s, const IfFsOps *fs, bool create);

/* 各ストアの絶対パスを out へ（INV-9 表示にも利用） */
void if_store_path(const IfStore *s, const char *name, char *out, u32 cap);

/* セッション保存: 現タブ列を tmp→rename→fsync で原子上書き。
 * 読みはメタのみ arena へパースして返し、タブ実体の再構築は chrome 側でやる
 * （構築中に save の自動発火が起きないよう責務を分離）。 */
bool if_store_session_save(const IfStore *s, const IfChrome *c);
i32  if_store_session_parse(const IfStore *s, IfArena *a,
                            IfSessionTab **tabs_out, i32 *active_id);

/* 履歴: 追記。512KB を超えたら後半 1/2 を残して原子的に縮退 */
bool if_store_history_add(IfStore *s, i64 now, const char *title, const char *url);

/* ブックマーク: URL 完全一致のトグル。戻り値=書き込み成功、*added=現在状態 */
bool if_store_bookmark_toggle(IfStore *s, const char *title, const char *url, bool *added);

/* ブックマーク一覧を arena に読む。戻り値=件数。titles/urls は長さ max の出力量 */
i32 if_store_bookmarks_list(const IfStore *s, IfArena *a, IfStr *titles, IfStr *urls, i32 max);

#endif
