/* Ifuto — <script> akl 実行配線（v0.3。contract: src/script.h / docs/SCRIPTING.md）。
 *
 * 構造的安全の要点（ここを変更する時は docs/SCRIPTING.md も同時改訂）:
 *  - HANDLE の ptr（IfDom, IfNode）は DOM arena 所有。script RT は style 適用前に
 *    必ず破棄され、DOM arena はパイプライン終端まで生きる → ptr の dangling は
 *    スケジュール上構築不能（akl.h の HANDLE 規約を main.c/chrome.c の配線が保証）。
 *  - g_arena/g_dom/g_log/g_scratch は module-static: akl eval は本プロセスで
 *    同時 1 実行のみ（md/layout の並列ワーカーは akl に一切触れない＝競合構造排除）。
 *  - text のコピー方向は akl ヒープ → dom arena（逆は akl ヒープへ複製）で寿命整合。
 */
#include "script.h"
#include "arena.h"
#include "akl/akl.h"
#include <stdlib.h>
#include <string.h>

#define IF_SCRIPT_MAX_RUN 128u
#define IF_SCRIPT_MAX_SRC (4u * 1024u * 1024u)
#define IF_SCRIPT_LOG_CAP 960u

static IfArena *g_arena;   /* dom 所有 arena（set 系の確保先） */
static IfArena *g_scratch; /* text 連結・C 文字列化の短命 arena */
static IfDom   *g_dom;
static FILE    *g_log;

/* ---- console.log（[script:console] 1 行規約。拡張 console と同型、prefix だけ別） ---- */
static AklVal script_console_log(AklRT *rt, AklVal self, int argc, const AklVal *argv, void *udata) {
    (void)self; (void)udata;
    char buf[IF_SCRIPT_LOG_CAP + 1];
    size_t n = 0;
    for (int i = 0; i < argc; i++) {
        AklVal sv = akl_tostring(rt, argv[i]); /* 直後に as_str コピー（pin 規約遵守） */
        if (akl_error(rt)[0]) return akl_mkundefined(); /* budget 枯渇: native_err 連動済 */
        u32 ln = 0;
        const char *b = akl_as_str(rt, sv, &ln);
        if (!b) { akl_native_throw(rt, "console.log: tostring failed"); return akl_mkundefined(); }
        if (i && n < IF_SCRIPT_LOG_CAP) buf[n++] = ' ';
        u32 room = IF_SCRIPT_LOG_CAP - (u32)n;
        u32 cn = ln < room ? ln : room;
        memcpy(buf + n, b, cn);
        n += cn;
        if (n >= IF_SCRIPT_LOG_CAP) break;
    }
    buf[n] = 0;
    for (size_t i = 0; i < n; i++) if (buf[i] == '\n' || buf[i] == '\r') buf[i] = ' ';
    fprintf(g_log ? g_log : stderr, "[script:console] %s\n", buf);
    return akl_mkundefined();
}

/* ---- document / element HANDLE vtab ---- */
static const AklHandleVTab doc_vt, elem_vt;

static bool doc_get(AklRT *rt, void *ptr, const char *name, u32 len, AklVal *out) {
    (void)ptr;
    if (len == 5 && memcmp(name, "title", 5) == 0) {
        IfStr t = g_dom ? g_dom->title : if_str(NULL, 0);
        *out = akl_mkstring(rt, t.p ? t.p : "", t.n);
        return true;
    }
    if (len == 4 && memcmp(name, "body", 4) == 0) {
        IfNode *b = g_dom ? if_dom_find_tag_dfs(g_dom, IF_TAG_BODY) : NULL;
        *out = b ? akl_mkhandle(rt, &elem_vt, b) : akl_mknull();
        return true;
    }
    if (len == 15 && memcmp(name, "documentElement", 15) == 0) {
        IfNode *h = g_dom ? if_dom_find_tag_dfs(g_dom, IF_TAG_HTML) : NULL;
        *out = h ? akl_mkhandle(rt, &elem_vt, h) : akl_mknull();
        return true;
    }
    return false; /* unknown prop → undefined */
}

static bool doc_set(AklRT *rt, void *ptr, const char *name, u32 len, AklVal v) {
    (void)ptr;
    if (!(len == 5 && memcmp(name, "title", 5) == 0)) return false;
    AklVal sv = akl_tostring(rt, v); /* JS ToString 経由（数値・真偽も文字列化） */
    if (akl_error(rt)[0]) return true; /* budget 枯渇: native_err 連動済 */
    u32 ln = 0;
    const char *b = akl_as_str(rt, sv, &ln);
    if (!b) { akl_native_throw(rt, "document.title: tostring failed"); return true; }
    if (g_arena && g_dom) if_dom_title_set(g_arena, g_dom, if_str(b, ln)); /* dom arena へコピー保持 */
    return true;
}

static bool doc_call(AklRT *rt, void *ptr, const char *name, u32 len,
                     int argc, const AklVal *argv, AklVal *out) {
    (void)ptr;
    if (len == 14 && memcmp(name, "getElementById", 14) == 0) {
        if (argc != 1) { akl_native_throw(rt, "getElementById expects 1 argument"); return true; }
        u32 ln = 0;
        const char *b = akl_as_str(rt, argv[0], &ln);
        if (!b) { akl_native_throw(rt, "getElementById expects a string"); return true; }
        /* b は akl ヒープ参照だが find_by_id は確保を行わない（GC 不発火）ので有効 */
        IfNode *e = g_dom ? if_dom_find_by_id(g_dom, if_str(b, ln)) : NULL;
        *out = e ? akl_mkhandle(rt, &elem_vt, e) : akl_mknull();
        return true;
    }
    return false; /* 未定義メソッド → TypeError: not a function */
}

static const AklHandleVTab doc_vt = { "HTMLDocument", doc_get, doc_set, doc_call };

static bool elem_get(AklRT *rt, void *ptr, const char *name, u32 len, AklVal *out) {
    IfNode *n = (IfNode *)ptr;
    if (len == 11 && memcmp(name, "textContent", 11) == 0) {
        IfStr t = if_dom_text_content(g_scratch, n); /* 短命ビュー → akl ヒープへ複製 */
        *out = akl_mkstring(rt, t.p ? t.p : "", t.n);
        return true;
    }
    if (len == 2 && memcmp(name, "id", 2) == 0) {
        IfStr v = if_dom_attr(n, "id");
        *out = akl_mkstring(rt, v.p ? v.p : "", v.n);
        return true;
    }
    if (len == 7 && memcmp(name, "tagName", 7) == 0) {
        const char *tn_ = if_tag_name(n->tag);
        char up[64];
        size_t tl = strlen(tn_);
        if (tl >= sizeof up) tl = sizeof up - 1;
        for (size_t i = 0; i < tl; i++)
            up[i] = (char)((n->ns == IF_NS_HTML && tn_[i] >= 'a' && tn_[i] <= 'z') ? (char)(tn_[i] - 32) : tn_[i]);
        *out = akl_mkstring(rt, up, (u32)tl); /* JS: HTML 要素の tagName は大文字 */
        return true;
    }
    return false;
}

static bool elem_set(AklRT *rt, void *ptr, const char *name, u32 len, AklVal v) {
    IfNode *n = (IfNode *)ptr;
    if (!(len == 11 && memcmp(name, "textContent", 11) == 0)) return false;
    AklVal sv = akl_tostring(rt, v);
    if (akl_error(rt)[0]) return true;
    u32 ln = 0;
    const char *b = akl_as_str(rt, sv, &ln);
    if (!b) { akl_native_throw(rt, "textContent: tostring failed"); return true; }
    if (g_arena) if_dom_set_text(g_arena, n, if_str(b, ln)); /* dom arena へコピー（b は一時寿命） */
    return true;
}

static const AklHandleVTab elem_vt = { "HTMLElement", elem_get, elem_set, NULL };

/* 文書順 DFS（script 収集。深さは tree 側 IF_MAX_STACK_DEPTH 制限で再帰安全） */
static u32 collect_scripts_rec(IfNode *n, IfNode **out, u32 cap, u32 cnt) {
    for (IfNode *c = n; c; c = c->next_sibling) {
        if (c->kind == IF_NODE_ELEMENT && c->tag == IF_TAG_SCRIPT && c->ns == IF_NS_HTML) {
            if (cnt < cap) out[cnt] = c;
            cnt++; /* 超過分も数える（切詰め警告の正確な母数） */
        }
        if (c->first_child) cnt = collect_scripts_rec(c->first_child, out, cap, cnt);
    }
    return cnt;
}

IfScriptReport if_script_run(IfArena *dom_arena, IfDom *dom, FILE *log) {
    IfScriptReport rep = { 0, 0, 0 };
    if (!dom_arena || !dom || !dom->root) return rep;
    if (!dom->has_script) return rep; /* 観測スイッチ: script 非含有文書は走査自体ゼロ */
    const char *ks = getenv("IF_SCRIPT");
    if (ks && ks[0] == '0' && ks[1] == 0) return rep; /* kill switch（完全 no-op） */
    FILE *lg = log ? log : stderr;

    IfNode *list[IF_SCRIPT_MAX_RUN];
    u32 total = collect_scripts_rec(dom->root, list, IF_SCRIPT_MAX_RUN, 0);
    u32 ncol = total < IF_SCRIPT_MAX_RUN ? total : IF_SCRIPT_MAX_RUN;
    if (!ncol) {
        if (total > IF_SCRIPT_MAX_RUN)
            fprintf(lg, "[script] script count truncated at %u (found %u)\n", IF_SCRIPT_MAX_RUN, total);
        return rep;
    }
    if (total > IF_SCRIPT_MAX_RUN)
        fprintf(lg, "[script] script count truncated at %u (found %u)\n", IF_SCRIPT_MAX_RUN, total);

    IfArena sc;
    if_arena_init(&sc, 1 << 20); /* script テキスト連結・C 文字列化（最大 4MB/script で打切り） */
    g_arena = dom_arena; g_scratch = &sc; g_dom = dom; g_log = lg;

    AklRT *rt = akl_new();
    if (!rt) { fprintf(lg, "[script] FAILED: akl_new failed\n"); if_arena_destroy(&sc); rep.n_errors++; return rep; }
    /* document/console 組込（VM 停止中 = 最初の akl_eval 前。失敗は bootstrap FAILED で明白に） */
    AklVal console = akl_mkobject(rt);
    bool cok = akl_is_object(rt, console) &&
               akl_prop_set(rt, console, "log", akl_mknative(rt, script_console_log, NULL)) &&
               akl_global_set(rt, "console", console) &&
               akl_global_set(rt, "document", akl_mkhandle(rt, &doc_vt, dom));
    if (!cok) {
        fprintf(lg, "[script] bootstrap FAILED: %.128s\n", akl_error(rt));
        akl_free(rt); if_arena_destroy(&sc);
        g_arena = NULL; g_scratch = NULL; g_dom = NULL; g_log = NULL;
        rep.n_errors++;
        return rep;
    }

    for (u32 i = 0; i < ncol; i++) {
        IfNode *sn = list[i];
        IfStr src = if_dom_attr(sn, "src");
        if (src.p && src.n) { rep.n_skipped++; continue; } /* 外部 script 取得は v1 非対象（明白に数える） */
        IfStr ty = if_dom_attr(sn, "type");
        if (ty.p && ty.n && !if_str_eq_ci(ty, if_str("text/javascript", 15))) { rep.n_skipped++; continue; }
        IfStr txt = if_dom_text_content(&sc, sn);
        if (!txt.p || !txt.n) { rep.n_skipped++; continue; } /* 空 script（<script></script>） */
        rep.n_run++;
        if (txt.n > IF_SCRIPT_MAX_SRC) { rep.n_errors++; fprintf(lg, "[script] FAILED: source budget exhausted (>4MB)\n"); continue; }
        if (memchr(txt.p, 0, txt.n))   { rep.n_errors++; fprintf(lg, "[script] FAILED: source contains NUL\n"); continue; }
        char *cs = (char *)if_arena_alloc(&sc, (u64)txt.n + 1);
        memcpy(cs, txt.p, txt.n); cs[txt.n] = 0;
        if (!akl_eval(rt, cs, NULL)) {
            char w[129];
            snprintf(w, sizeof w, "%.128s", akl_error(rt));
            w[128] = 0;
            for (char *q = w; *q; q++) if (*q == '\n' || *q == '\r') *q = ' ';
            fprintf(lg, "[script] FAILED: %s\n", w);
            rep.n_errors++; /* fail-stop 粒度 = script。後続と描画は継続 */
        }
    }
    akl_free(rt); /* DOM arena 解体より必ず先（HANDLE ptr 規約の構造保証） */
    if_arena_destroy(&sc);
    g_arena = NULL; g_scratch = NULL; g_dom = NULL; g_log = NULL;
    return rep;
}
