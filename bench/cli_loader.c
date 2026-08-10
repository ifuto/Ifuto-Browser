/* akl_cli 用モジュールローダ: ファイルパス解決（base のディレクトリ基準）+ data: URI。
 * 契約は src/akl/akl.h の AklModuleLoader。out_src / out_id は malloc で確保し
 * AKL 側が free する。解決できない場合は両方 NULL を返す。 */
#if !defined(_XOPEN_SOURCE) && !defined(_POSIX_C_SOURCE)
#define _XOPEN_SOURCE 700 /* realpath / strdup */
#endif
#include "../src/akl/akl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <limits.h>

static char *cli_slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *b = (char *)malloc((size_t)sz + 1);
    if (!b) { fclose(f); return NULL; }
    if (sz && fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
    b[sz] = 0;
    fclose(f);
    return b;
}

void cli_module_loader(AklRT *rt, const char *spec, const char *base,
                       void *udata, char **out_src, char **out_id) {
    (void)rt; (void)udata;
    *out_src = NULL; *out_id = NULL;
    /* data: URI（テスト用。application/javascript / text/javascript のみ扱う） */
    if (strncmp(spec, "data:", 5) == 0) {
        const char *p = spec + 5;
        const char *comma = strchr(p, ',');
        if (!comma) return;
        size_t hlen = (size_t)(comma - p);
        if (hlen == 0) return; /* 空 MIME は拒否 */
        if (strncmp(p, "application/javascript", 22) != 0 &&
            strncmp(p, "text/javascript", 15) != 0) return; /* 非対応 MIME は解決不可 */
        size_t blen = strlen(comma + 1);
        char *b = (char *)malloc(blen + 1);
        if (!b) return;
        memcpy(b, comma + 1, blen); b[blen] = 0;
        *out_src = b;
        char *id = (char *)malloc(strlen(spec) + 1);
        if (!id) { free(b); return; }
        memcpy(id, spec, strlen(spec) + 1);
        *out_id = id;
        return;
    }
    /* ファイル解決: base の dirname 基準（base が無ければ spec のまま） */
    char path[PATH_MAX];
    if (base && spec[0] != '/') {
        char *bc = strdup(base);
        if (!bc) return;
        char *dir = dirname(bc);
        snprintf(path, sizeof path, "%s/%s", dir, spec);
        free(bc);
    } else {
        snprintf(path, sizeof path, "%s", spec);
    }
    char *src = cli_slurp(path);
    if (!src) return;
    /* realpath で正規化（./ や ../ の蓄積による同一モジュールの多重ロードを防ぐ。
     * 循環 import の検出は id 一致が前提）。失敗は解決不可扱い */
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) { free(src); return; }
    char *id = strdup(resolved);
    if (!id) { free(src); return; }
    *out_src = src;
    *out_id = id;
}
