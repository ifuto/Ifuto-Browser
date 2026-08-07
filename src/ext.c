/* Ifuto — 拡張スキャナ/ランナー E1（規則の正は docs/EXTENSIONS.md と ext.h）。
 * ここに書くのは FS 結線と効果適用のみ。ケイパビリティ解釈の変更は
 * EXTENSIONS.md の改訂なしに行わない（doc と実装を同コミットで更新する規則）。 */
#define _POSIX_C_SOURCE 200809L /* opendir/readdir/strdup */
#include "ext.h"
#include "ext_manifest.h"
#include "chrome.h"
#include "akl/akl.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>

static const char *g_ext_dir;
void if_ext_set_dir(const char *dir) { g_ext_dir = dir; }
const char *if_ext_dir(void) { return g_ext_dir; }

#define IF_EXT_PATH_CAP (IF_STORE_DIR_CAP + 384)
#define IF_EXT_EFFECT_CAP 960

static void ext_report_fail(FILE *rp, const char *name, const char *why) {
    /* 1 行不変条件: 理由文字列の最初の \n で打ち切る（akl_error は複数行になりうる） */
    if (name && name[0]) fprintf(rp, "[ext] %s FAILED: %.128s\n", name, why);
    else fprintf(rp, "[ext] (unnamed) FAILED: %.128s\n", why);
}

/* 拡張 1 個の評価と効果適用。成功=true */
static bool ext_run_one(struct IfChrome *c, FILE *rp, const char *dirpath,
                        const char *dirname, IfArena *scratch) {
    char manpath[IF_EXT_PATH_CAP];
    int mn = snprintf(manpath, sizeof manpath, "%s/%s/manifest.txt", dirpath, dirname);
    if (mn < 0 || (size_t)mn >= sizeof manpath) { ext_report_fail(rp, dirname, "path too long"); return false; }

    IfChrome *cc = c; /* fs read: manifest */
    IfStr man = cc->fs.read_file(scratch, manpath, cc->fs.ctx);
    if (!man.p || man.n == 0) return false; /* manifest なし = 拡張でない（黙殺） */

    IfExtManifest mf;
    char err[IF_EXT_ERR_CAP];
    if (!if_ext_manifest_parse(man, &mf, err, sizeof err)) {
        ext_report_fail(rp, dirname, err); return false;
    }
    /* エントリ読み出し（akl_eval は C 文字列 API: 埋め込み NUL は拒否、
     * サイズ上限は呼び出し側で engine 上限（AKL_MAX_SRC=4MB）に合わせて拒否） */
    char entpath[IF_EXT_PATH_CAP];
    int en = snprintf(entpath, sizeof entpath, "%s/%s/%s", dirpath, dirname, mf.entry);
    if (en < 0 || (size_t)en >= sizeof entpath) { ext_report_fail(rp, mf.name, "path too long"); return false; }
    IfStr src = cc->fs.read_file(scratch, entpath, cc->fs.ctx);
    if (!src.p || src.n == 0) { ext_report_fail(rp, mf.name, "entry unreadable"); return false; }
    if (src.n > (4u << 20)) { ext_report_fail(rp, mf.name, "entry too large"); return false; }
    if (memchr(src.p, 0, src.n)) { ext_report_fail(rp, mf.name, "entry contains NUL"); return false; }
    char *csrc = (char *)if_arena_alloc(scratch, (u64)src.n + 1);
    memcpy(csrc, src.p, src.n); csrc[src.n] = 0;

    /* 拡張ごと独立 AklRT。budget は製品既定のまま（akl_tune を呼ばない =
     * 拡張が自身の制限を緩める経路は設計しない）。insn は akl_new の既定 10M */
    AklRT *rt = akl_new();
    if (!rt) { ext_report_fail(rp, mf.name, "akl_new failed"); return false; }
    AklVal out;
    bool ok = akl_eval(rt, csrc, &out);
    if (!ok) {
        const char *e = akl_error(rt);
        ext_report_fail(rp, mf.name, e && e[0] ? e : "eval failed");
        akl_free(rt);
        return false;
    }
    /* 戻り値効果スキーマ: 宣言ケイパビリティは最終式文の値（String 必須）へ流れる */
    bool applied = true;
    if (mf.perm != IF_EXT_PERM_NONE) {
        u32 vlen = 0;
        const char *v = akl_as_str(rt, out, &vlen); /* rt 所有（VM 停止中 = GC 不発） */
        if (!v) applied = false;
        else {
            char eff[IF_EXT_EFFECT_CAP + 1];
            u32 cn = vlen < IF_EXT_EFFECT_CAP ? vlen : IF_EXT_EFFECT_CAP;
            memcpy(eff, v, cn); eff[cn] = 0;
            for (u32 i = 0; i < cn; i++) if (eff[i] == '\n' || eff[i] == '\r') eff[i] = ' ';
            if (mf.perm == IF_EXT_PERM_STATUS) if_chrome_toast(c, eff);
            else fprintf(rp, "[ext:%s] %s\n", mf.name, eff);
        }
    }
    akl_free(rt);
    if (!applied) { ext_report_fail(rp, mf.name, "result must be String for declared permission"); return false; }
    fprintf(rp, "[ext] %s v%s loaded (perm: %s)\n", mf.name, mf.version, if_ext_perm_name(mf.perm));
    return true;
}

static int ext_name_cmp(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

i32 if_ext_scan_and_run(struct IfChrome *c, const char *dir, FILE *report, bool explicit_req) {
    FILE *rp = report ? report : stderr;
    if (!dir || !dir[0]) return 0;
    DIR *dp = opendir(dir);
    if (!dp) {
        if (explicit_req) fprintf(rp, "[ext] %s: cannot open\n", dir);
        return 0;
    }
    /* 決定性: readdir 順は FS 依存なので全件収集して qsort してから処理 */
    char **names = NULL;
    size_t n_names = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue; /* "." ".." ・隠し */
        if (n_names == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            char **nn = (char **)realloc(names, ncap * sizeof *nn);
            if (!nn) break;
            names = nn; cap = ncap;
        }
        names[n_names] = strdup(de->d_name);
        if (!names[n_names]) break;
        n_names++;
    }
    closedir(dp);
    qsort(names, n_names, sizeof *names, ext_name_cmp);

    IfArena scratch;
    if_arena_init(&scratch, 1 << 18);
    i32 loaded = 0;
    for (size_t i = 0; i < n_names; i++) {
        if (ext_run_one(c, rp, dir, names[i], &scratch)) loaded++;
        free(names[i]);
    }
    free(names);
    if_arena_destroy(&scratch);
    return loaded;
}
