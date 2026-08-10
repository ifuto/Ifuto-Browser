/* 画像デコーダ単体テスト（PNG/BMP）。テスト画像は /tmp/imgtest に Python で生成済み。 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "tests.h"
#include "../src/image.h"

static void check_img(const char *path, u32 want_w, u32 want_h,
                      const u8 *want_px, const char *want_desc) {
    (void)want_desc;
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = (u8 *)malloc(n ? (size_t)n : 1);
    if (fread(buf, 1, n, f) != (size_t)n) { CHECK(0); fclose(f); free(buf); return; }
    fclose(f);
    char err[128];
    IfImage *img = if_img_decode(buf, (u32)n, err, sizeof err);
    free(buf);
    if (!img) fprintf(stderr, "  [image] decode fail %s: %s\n", path, err);
    CHECK(img != NULL);
    if (!img) return;
    CHECK(img->w == want_w && img->h == want_h);
    if (want_px && img->w == want_w && img->h == want_h) {
        CHECK(memcmp(img->px, want_px, (size_t)want_w * want_h * 4) == 0);
    }
    if_img_free(img);
}

void test_image(void) {
    /* t1.png: 2x2 RGBA（赤/緑/青/白） */
    static const u8 t1[] = {
        255,0,0,255,  0,255,0,255,
        0,0,255,255,  255,255,255,255,
    };
    check_img("/tmp/imgtest/t1.png", 2, 2, t1, "2x2 RGBA");

    /* t2.png: 4x4 グラデーション（全フィルタ） */
    {
        u8 px2[4 * 4 * 4];
        for (u32 y = 0; y < 4; y++)
            for (u32 x = 0; x < 4; x++) {
                u32 o = (y * 4 + x) * 4;
                px2[o] = (x * 60) % 256;
                px2[o + 1] = (y * 60) % 256;
                px2[o + 2] = (x + y) * 40 % 256;
                px2[o + 3] = 255;
            }
        check_img("/tmp/imgtest/t2.png", 4, 4, px2, "4x4 all filters");
    }

    /* t3.png: 3x2 グレー */
    {
        u8 px3[3 * 2 * 4];
        u8 g[] = {0, 128, 255, 10, 200, 100};
        for (u32 i = 0; i < 6; i++) {
            px3[i * 4] = px3[i * 4 + 1] = px3[i * 4 + 2] = g[i];
            px3[i * 4 + 3] = 255;
        }
        check_img("/tmp/imgtest/t3.png", 3, 2, px3, "3x2 gray");
    }

    /* t4.bmp: 3x2 24bpp（BGR 入力 → RGB 出力） */
    {
        u8 px4[3 * 2 * 4];
        u8 bgr[] = {
            255,0,0,  0,255,0,  0,0,255,
            255,255,0, 0,255,255, 255,0,255,
        };
        for (u32 i = 0; i < 6; i++) {
            px4[i * 4] = bgr[i * 3 + 2];
            px4[i * 4 + 1] = bgr[i * 3 + 1];
            px4[i * 4 + 2] = bgr[i * 3];
            px4[i * 4 + 3] = 255;
        }
        check_img("/tmp/imgtest/t4.bmp", 3, 2, px4, "3x2 BMP24");
    }

    /* big.png: 100x100 全フィルタ混在（サイズ検証のみ） */
    check_img("/tmp/imgtest/big.png", 100, 100, NULL, "100x100");

    /* 壊れデータは NULL（明白に失敗） */
    {
        char err[128];
        IfImage *img = if_img_decode((const u8 *)"notanimage", 10, err, sizeof err);
        CHECK(img == NULL);
    }
    {
        char err[128];
        IfImage *img = if_img_decode((const u8 *)"\x89PNG\r\n\x1a\nXXXX", 12, err, sizeof err);
        CHECK(img == NULL);
    }
}
