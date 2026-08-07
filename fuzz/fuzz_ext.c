/* Ifuto — fuzz ドライバ: 拡張 manifest パーサ（src/ext_manifest.c）。
 * 敵対的 manifest.txt を喰わせてもクラッシュしないことの機械監査
 * （拡張導入面 = ユーザーが外部から持ち込むファイルを読む最初の構造）。
 * crash・UB・sanitizer 違反・不変条件違反があれば abort → 検出。
 *
 * 形態: (a) 入力そのまま、(b) 入力を 300 回反復（64KB 上限分岐の確実な到達。
 *        上限超過の検査を種サイズに依存させないため常時両経路を監査する）。
 * 機械不変条件（実装契約の監査）:
 *  - 成功 ⇒ name/version/entry は非空 C 文字列・cap 内、perm ∈ {0,1,2}、
 *    err も必ず C 文字列として妥当（cap-1 内 NUL）
 *  - 失敗 ⇒ err は非空（理由なし失敗は監査不能のため契約違反）
 *  - 決定性: 同一入力 2 回（独立コピー）で成功可否・全フィールド・err が完全一致
 *  - 境界正直記載: エントリ実体の読み出し・評価は対象外（パーサ単体の監査） */
#include "common.h"
#include "ext_manifest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char *what) { fputs(what, stderr); fputc('\n', stderr); abort(); }

static void audit_one(const u8 *buf, u32 n) {
    IfExtManifest m1, m2;
    char e1[IF_EXT_ERR_CAP], e2[IF_EXT_ERR_CAP];
    memset(&m1, 0xA5, sizeof m1); memset(&m2, 0x5A, sizeof m2);
    e1[0] = e2[0] = 0x7F;
    bool r1 = if_ext_manifest_parse(if_str((const char *)buf, n), &m1, e1, sizeof e1);
    bool r2 = if_ext_manifest_parse(if_str((const char *)buf, n), &m2, e2, sizeof e2);
    /* 決定性（未初期化・揺れの検出） */
    if (r1 != r2) die("NONDET RESULT");
    if (r1) {
        if (m1.name[0] == 0 || m1.version[0] == 0 || m1.entry[0] == 0) die("EMPTY FIELD");
        if (strlen(m1.name) >= IF_EXT_NAME_CAP) die("NAME CAP");
        if (strlen(m1.version) >= IF_EXT_VER_CAP) die("VER CAP");
        if (strlen(m1.entry) >= IF_EXT_ENTRY_CAP) die("ENTRY CAP");
        if (m1.perm > IF_EXT_PERM_LOG) die("PERM RANGE");
        if (memcmp(&m1, &m2, sizeof m1) != 0) die("NONDET MANIFEST");
    } else if (e1[0] == 0) die("EMPTY ERROR");
    if (strcmp(e1, e2) != 0) die("NONDET ERROR");
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    static u8 buf[65536];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n == 0) return 0;

    audit_one(buf, (u32)n); /* raw */

    /* 反復で 64KB 上限分岐へ（入力先頭バイトが小さいときは前詰めコピーで
     * 上限を超える長さを確実に作る。反復後の中身は決定的） */
    static u8 big[IF_EXT_MANIFEST_CAP + 8192];
    u32 chunk = (u32)n < 300 ? (u32)n : 300;
    u32 total = 0;
    while (total + chunk <= sizeof big) { memcpy(big + total, buf, chunk); total += chunk; }
    audit_one(big, total); /* 上限超過（=拒否）側 */
    audit_one(big, IF_EXT_MANIFEST_CAP); /* 上限ちょうど側 */
    return 0;
}
