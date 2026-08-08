/* font16 lookup 整合（値は一切ピン留めしない。構造不変条件のみ機械固定）:
 *  1. F16_EXTRA_CP は厳密昇順（二分探索の前提）
 *  2. F16_EXTRA_SLOT は全て F16_GLYPHS 添字圏内（u8 桁落ち回帰はこれで検出）
 *  3. lookup の往復整合: 収録 cp は非 NULL かつ呼出毎に同一ポインタ、
 *     INDEX が指す slot は圏内、未収録帯は確実に NULL を返す */
#include "tests.h"
#include "../src/gui/font16.h"

void test_font16_lookup(void) {
    enum { N_GLYPHS = (int)(sizeof(F16_GLYPHS) / sizeof(F16_GLYPHS[0])) };

    /* 1: 二分探索の前提（昇順） */
    for (uint32_t i = 1; i < (uint32_t)F16_N_EXTRA; i++)
        CHECK(F16_EXTRA_CP[i - 1] < F16_EXTRA_CP[i]);

    /* 2+3a: EXTRA 側往復（slot 圏内 & lookup と一致） */
    for (uint32_t i = 0; i < (uint32_t)F16_N_EXTRA; i++) {
        CHECK((int)F16_EXTRA_SLOT[i] < N_GLYPHS);
        const uint16_t *g = f16_lookup(F16_EXTRA_CP[i]);
        CHECK(g == F16_GLYPHS[F16_EXTRA_SLOT[i]]);
        CHECK(g == f16_lookup(F16_EXTRA_CP[i])); /* 冪等 */
    }

    /* 3b: 0x3000 帯（INDEX 経路）の往復 */
    for (uint32_t cp = F16_BASE; cp < F16_BASE + F16_SPAN; cp++) {
        uint8_t s = F16_INDEX[cp - F16_BASE];
        if (s == F16_NONE) { CHECK(f16_lookup(cp) == NULL); continue; }
        /* slot は uint8_t で N_GLYPHS(>255) に型レベルで収まる（bounds CHECK は恒真
         * のため警告になる）— 実効判定は下の f16_lookup 一致 CHECK が担う */
        CHECK(f16_lookup(cp) == F16_GLYPHS[s]);
    }

    /* 3c: 明に未収録の帯は NULL（過剰ヒットしない） */
    CHECK(f16_lookup(0x9FFF) == NULL);
    CHECK(f16_lookup(0x10FFFF) == NULL);
    CHECK(f16_lookup(0x20) == NULL);   /* ASCII は font16 の責務外 */
    CHECK(f16_lookup(0x4E01) == NULL); /* 丁（未収録漢字） */

    /* 既知の桁落ち回帰点（slot>=256 初到達の 2 グリフ。形状値は gen 工程の
     * 目視検査が所有し、ここでは「正しく拾える」構造のみ固定） */
    CHECK(f16_lookup(0x8868) != NULL);  /* 表: u8 時代に slot 256 で 0 に桁落ち */
    CHECK(f16_lookup(0x898B) != NULL);  /* 見: slot 257 で 1 に桁落ち */
    CHECK(f16_lookup(0x8868) != f16_lookup(0x898B));
}
