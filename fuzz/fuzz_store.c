/* Ifuto — fuzz ドライバ: 永続ストアの読み面パーサ（src/store.c）。
 * 破損・敵対的な autosave（session.txt / bookmarks.tsv）を喰わせても
 * クラッシュしないことの機械監査（破損した session で起動時に死ぬ =
 * 全起動が連鎖して死ぬ実害がある）。crash・UB・sanitizer 違反・不変条件
 * 違反があれば abort → 検出。静かに 0 で抜けるのが正。
 *
 * 形態（先頭バイト & 3 で分岐）:
 *  0: session.txt としてそのまま（magic 拒否路の深さも見る）
 *  1: "ifuto-session 1\n" を前置して session（magic 後の行パーサへ敵対本文を到達）
 *  2: bookmarks.tsv としてそのまま
 *  3: 1 と 2 の両方を同一入力で（1 入力 2 監査）
 *
 * 機械不変条件（実装契約の監査）:
 *  session_parse:
 *   - 戻り値 n ∈ {0} ∪ [1, IF_TABS_MAX]、n==0 ⇒ tabs stays NULL、n>0 ⇒ tabs 非 NULL
 *   - 各 tab: id ∈ [0, 1000000]、scroll ∈ [0, 1<<24]、url != NULL
 *   - active_id ∈ {-1} ∪ [1, 1000000]
 *   - 決定性: 同一入力 2 回パースで全フィールド一致（未初期化/エイリアス揺れ検出）
 *  bookmarks_list:
 *   - 戻り値 ∈ [0, max]、titles[i]/urls[i] は読込バッファ [txt.p, txt.p+n) 内の
 *     射程整合スライス（p+n がバッファをはみ出さない）
 *   - 決定性: 同上
 *
 * 境界（誇張しない）: history 縮退経路（書込結合）・tmp→rename は対象外。
 * 読み面 2 パーサのみ。 */
#include "common.h"
#include "arena.h"
#include "store.h"
#include "chrome.h" /* IF_TABS_MAX（session パーサ契約の一部） */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* fs 注入（read のみ。書き面は NULL = 読み面監査に限定） */
static const u8 *g_blob;
static u32 g_blob_n;
static IfStr g_last; /* bookmarks 射程監査用: 直近 read の返却 */

static bool fuzz_exists(const char *path, void *ctx) {
    (void)path; (void)ctx; return true;
}
static IfStr fuzz_read(IfArena *a, const char *path, void *ctx) {
    (void)path; (void)ctx;
    u8 *m = (u8 *)if_arena_alloc(a, g_blob_n ? g_blob_n : 1);
    if (g_blob_n) memcpy(m, g_blob, g_blob_n);
    g_last = if_str((const char *)m, g_blob_n);
    return g_last;
}

static void die(const char *what) { fputs(what, stderr); fputc('\n', stderr); abort(); }

static void audit_session(const IfStore *st) {
    IfArena a1, a2;
    if_arena_init(&a1, 1 << 16); if_arena_init(&a2, 1 << 16);
    IfSessionTab *t1 = (IfSessionTab *)(void *)0x1, *t2 = NULL;
    i32 act1 = -999, act2 = -999;
    i32 n1 = if_store_session_parse(st, &a1, &t1, &act1);
    i32 n2 = if_store_session_parse(st, &a2, &t2, &act2);

    if (n1 < 0 || n1 > IF_TABS_MAX) die("SESS N RANGE");
    if (n1 == 0 && t1 != NULL) die("SESS NULL-DISCIPLINE");
    if (n1 > 0 && !t1) die("SESS TABS NULL");
    if (!(act1 == -1 || (act1 >= 1 && act1 <= 1000000))) die("SESS ACTIVE RANGE");
    for (i32 i = 0; i < n1; i++) {
        if (t1[i].id < 0 || t1[i].id > 1000000) die("SESS ID RANGE");
        if (t1[i].scroll < 0 || t1[i].scroll > (1 << 24)) die("SESS SCROLL RANGE");
        if (!t1[i].url) die("SESS URL NULL");
    }
    /* 決定性（未初期化・arena エイリアス揺れの検出） */
    if (n2 != n1 || act2 != act1) die("SESS NONDET N");
    for (i32 i = 0; i < n1; i++) {
        const IfSessionTab *x = &t1[i], *y = &t2[i];
        if (x->id != y->id || x->scroll != y->scroll) die("SESS NONDET FIELD");
        if ((x->title != NULL) != (y->title != NULL)) die("SESS NONDET TITLE NULL");
        if ((x->group != NULL) != (y->group != NULL)) die("SESS NONDET GROUP NULL");
        if (x->title && strcmp(x->title, y->title) != 0) die("SESS NONDET TITLE");
        if (x->group && strcmp(x->group, y->group) != 0) die("SESS NONDET GROUP");
        if (strcmp(x->url, y->url) != 0) die("SESS NONDET URL");
    }
    if_arena_destroy(&a1); if_arena_destroy(&a2);
}

#define BM_MAX 64

static void audit_bookmarks(const IfStore *st) {
    IfArena a1, a2;
    if_arena_init(&a1, 1 << 16); if_arena_init(&a2, 1 << 16);
    IfStr ti1[BM_MAX], ur1[BM_MAX], ti2[BM_MAX], ur2[BM_MAX];
    i32 m1 = if_store_bookmarks_list(st, &a1, ti1, ur1, BM_MAX);
    IfStr txt1 = g_last;
    i32 m2 = if_store_bookmarks_list(st, &a2, ti2, ur2, BM_MAX);

    if (m1 < 0 || m1 > BM_MAX) die("BM N RANGE");
    for (i32 i = 0; i < m1; i++) {
        /* スライス射程: read バッファ [txt.p, txt.p+txt.n) 内整合 */
        if ((const u8 *)ti1[i].p < (const u8 *)txt1.p ||
            (const u8 *)ti1[i].p + ti1[i].n > (const u8 *)txt1.p + txt1.n)
            die("BM TITLE SPAN");
        if ((const u8 *)ur1[i].p < (const u8 *)txt1.p ||
            (const u8 *)ur1[i].p + ur1[i].n > (const u8 *)txt1.p + txt1.n)
            die("BM URL SPAN");
    }
    if (m2 != m1) die("BM NONDET N");
    for (i32 i = 0; i < m1; i++)
        if (ti1[i].n != ti2[i].n || ur1[i].n != ur2[i].n ||
            memcmp(ti1[i].p, ti2[i].p, ti1[i].n) != 0 ||
            memcmp(ur1[i].p, ur2[i].p, ur1[i].n) != 0)
            die("BM NONDET FIELD");
    if_arena_destroy(&a1); if_arena_destroy(&a2);
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    static u8 buf[65536];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n == 0) return 0;

    u8 mode = buf[0] & 3;
    IfFsOps fs = { fuzz_exists, fuzz_read, NULL, NULL, NULL, NULL };
    IfStore st;
    memset(&st, 0, sizeof st);
    st.fs = &fs;
    st.enabled = true;
    memcpy(st.dir, "/tmp/fuzz-store", 16);

    static u8 body[65536 + 17];
    if (mode == 0 || mode == 2) {
        g_blob = buf + 1; g_blob_n = (u32)n - 1;
        if (mode == 0) audit_session(&st); else audit_bookmarks(&st);
    } else {
        /* magic 前置で行パーサの深部へ（17 = "ifuto-session 1\n" + 1） */
        memcpy(body, "ifuto-session 1\n", 16);
        memcpy(body + 16, buf + 1, n - 1);
        g_blob = body; g_blob_n = (u32)n - 1 + 16;
        audit_session(&st);
        if (mode == 3) { g_blob = buf + 1; g_blob_n = (u32)n - 1; audit_bookmarks(&st); }
    }
    return 0;
}
