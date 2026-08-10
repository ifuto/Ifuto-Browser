/* image.c — Ifuto 軽量画像デコード（BMP / PNG）
 *
 * PNG: チャンク走査 → IDAT 連結 → zlib inflate（RFC1950/1951）→ フィルタ解除 → 色変換。
 * deflate は ストアド / 固定ハフマン / 動的ハフマンの全ブロック型に対応。
 * シンボル長の最大は 288（リテラル/長さ）+ 32（距離）で、ハフマンコードは
 * 長さ制限 15 ビット（RFC1951 準拠）の正準ハフマンをビット長テーブルで構成する。
 *
 * 制限（明白拒否・AKL_COMPAT 相当）:
 *   - 16bit 深度・パレット・インターレース
 *   - 巨大画像（64MB 超）・破損データ
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "image.h"

#define IMG_MAX_BYTES (64u << 20)
#define IMG_MAX_DIM 16384u

/* ================= ビットリーダー ================= */

typedef struct {
    const u8 *p;
    u32 n;
    u32 pos;   /* バイト位置 */
    u32 bit;   /* 次のビット位置 0..7 */
    u32 acc;   /* 蓄積（最大 32bit） */
    u32 acc_n;
    bool err;
} BitR;

static u32 br_byte(BitR *b) {
    if (b->pos >= b->n) { b->err = true; return 0; }
    return b->p[b->pos++];
}
/* n ビット（LSB first。RFC1951 の deflate は LSB 順） */
static u32 br_bits(BitR *b, u32 n) {
    while (b->acc_n < n) {
        b->acc |= br_byte(b) << b->acc_n;
        b->acc_n += 8;
        if (b->err) return 0;
    }
    u32 v = b->acc & ((1u << n) - 1);
    b->acc >>= n;
    b->acc_n -= n;
    return v;
}
static u32 br_align_byte(BitR *b) {
    b->acc = 0;
    b->acc_n = 0;
    return 0;
}

/* ================= deflate ================= */

#define HLIT_MAX 288
#define HDIST_MAX 32

typedef struct {
    u16 count[16];   /* 長さごとの符号数 */
    u16 first[16];   /* 長さごとの最初のコード */
    u16 sym[16][HLIT_MAX + HDIST_MAX]; /* 長さごとのシンボル列（順序 = コード順） */
    u16 n;
} Huff;

/* 正準ハフマンを length 配列から構成 */
static bool huff_build(Huff *h, const u8 *lens, u16 n) {
    memset(h->count, 0, sizeof h->count);
    memset(h->first, 0, sizeof h->first);
    h->n = n;
    for (u32 i = 0; i < n; i++) {
        if (lens[i] > 15) return false;
        if (lens[i]) h->count[lens[i]]++;
    }
    u32 code = 0;
    for (u32 bits = 1; bits <= 15; bits++) {
        code = (code + h->count[bits - 1]) << 1;
        h->first[bits] = (u16)code;
    }
    u16 fill[16] = {0};
    for (u32 i = 0; i < n; i++) {
        u32 l = lens[i];
        if (l) {
            u16 c = h->first[l] + fill[l]++;
            if (c >= (1u << l)) return false; /* 過密（壊れ） */
            h->sym[l][c - h->first[l]] = (u16)i;
        }
    }
    return true;
}

/* 1 シンボル復号。戻り: シンボル or UINT16_MAX（エラー） */
static u32 huff_sym(BitR *b, const Huff *h) {
    u32 code = 0;
    for (u32 l = 1; l <= 15; l++) {
        code = (code << 1) | br_bits(b, 1);
        if (b->err) return UINT16_MAX;
        u32 idx = code - h->first[l];
        if (code >= h->first[l] && idx < h->count[l])
            return h->sym[l][idx];
    }
    return UINT16_MAX;
}

/* 長さ/距離の追加ビット */
static const u8 LEN_EXTRA[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const u32 LEN_BASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const u8 DIST_EXTRA[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
static const u32 DIST_BASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};

/* 固定ハフマン（RFC1951 3.2.6） */
static void huff_fixed_lit(Huff *h) {
    u8 lens[288];
    for (u32 i = 0; i < 144; i++) lens[i] = 8;
    for (u32 i = 144; i < 256; i++) lens[i] = 9;
    for (u32 i = 256; i < 280; i++) lens[i] = 7;
    for (u32 i = 280; i < 288; i++) lens[i] = 8;
    huff_build(h, lens, 288);
}
static void huff_fixed_dist(Huff *h) {
    u8 lens[32];
    for (u32 i = 0; i < 32; i++) lens[i] = 5;
    huff_build(h, lens, 32);
}

/* deflate 全体を展開。out に結果を書き、*out_len に長さ。 */
static bool inflate(const u8 *src, u32 src_n, u8 *out, u32 out_cap, u32 *out_len) {
    BitR b;
    memset(&b, 0, sizeof b);
    b.p = src;
    b.n = src_n;
    *out_len = 0;
    bool final = false;
    Huff lit, dist, clen;
    while (!final && !b.err) {
        final = br_bits(&b, 1) != 0;
        u32 type = br_bits(&b, 2);
        if (type == 0) { /* ストアド */
            br_align_byte(&b);
            u32 len = br_byte(&b) | (br_byte(&b) << 8);
            u32 nlen = br_byte(&b) | (br_byte(&b) << 8);
            if ((len ^ 0xFFFF) != nlen) { b.err = true; break; }
            if (*out_len + len > out_cap) { b.err = true; break; }
            for (u32 i = 0; i < len; i++) out[(*out_len)++] = (u8)br_byte(&b);
        } else if (type == 1) { /* 固定ハフマン */
            huff_fixed_lit(&lit);
            huff_fixed_dist(&dist);
            goto dyn_common;
        } else if (type == 2) { /* 動的ハフマン */
            u32 hlit = br_bits(&b, 5) + 257;
            u32 hdist = br_bits(&b, 5) + 1;
            u32 hclen = br_bits(&b, 4) + 4;
            static const u8 CL_ORDER[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            u8 cl_lens[19] = {0};
            for (u32 i = 0; i < hclen; i++) cl_lens[CL_ORDER[i]] = (u8)br_bits(&b, 3);
            if (!huff_build(&clen, cl_lens, 19)) { b.err = true; break; }
            u8 lens[HLIT_MAX + HDIST_MAX];
            u32 lidx = 0;
            while (lidx < hlit + hdist) {
                u32 sym = huff_sym(&b, &clen);
                if (sym == UINT16_MAX) { b.err = true; break; }
                if (sym < 16) {
                    lens[lidx++] = (u8)sym;
                } else if (sym == 16) {
                    u32 rep = br_bits(&b, 2) + 3;
                    if (lidx == 0) { b.err = true; break; }
                    u8 prev = lens[lidx - 1];
                    for (u32 i = 0; i < rep; i++) {
                        if (lidx >= hlit + hdist) { b.err = true; break; }
                        lens[lidx++] = prev;
                    }
                } else if (sym == 17) {
                    u32 rep = br_bits(&b, 3) + 3;
                    for (u32 i = 0; i < rep; i++) {
                        if (lidx >= hlit + hdist) { b.err = true; break; }
                        lens[lidx++] = 0;
                    }
                } else {
                    u32 rep = br_bits(&b, 7) + 11;
                    for (u32 i = 0; i < rep; i++) {
                        if (lidx >= hlit + hdist) { b.err = true; break; }
                        lens[lidx++] = 0;
                    }
                }
            }
            if (b.err) break;
            if (!huff_build(&lit, lens, hlit)) { b.err = true; break; }
            if (!huff_build(&dist, lens + hlit, hdist)) { b.err = true; break; }
            goto dyn_common;
        } else {
            b.err = true;
            break;
        }
        continue;
    dyn_common:
        for (;;) {
            u32 sym = huff_sym(&b, &lit);
            if (sym == UINT16_MAX) { b.err = true; break; }
            if (sym < 256) {
                if (*out_len >= out_cap) { b.err = true; break; }
                out[(*out_len)++] = (u8)sym;
            } else if (sym == 256) {
                break; /* ブロック終端 */
            } else {
                u32 li = sym - 257;
                if (li >= 29) { b.err = true; break; }
                u32 len = LEN_BASE[li] + br_bits(&b, LEN_EXTRA[li]);
                u32 dsym = huff_sym(&b, &dist);
                if (dsym == UINT16_MAX || dsym >= 30) { b.err = true; break; }
                u32 d = DIST_BASE[dsym] + br_bits(&b, DIST_EXTRA[dsym]);
                if (d > *out_len) { b.err = true; break; }
                if (*out_len + len > out_cap) { b.err = true; break; }
                u32 src_i = *out_len - d;
                for (u32 i = 0; i < len; i++) out[(*out_len)++] = out[src_i + i];
            }
        }
        if (b.err) break;
    }
    return !b.err;
}

/* ================= PNG ================= */

typedef struct {
    const u8 *p;
    u32 n;
    u32 pos;
    u32 crc;
} PngR;

static u32 png_read_be32(PngR *r) {
    if (r->pos + 4 > r->n) { r->pos = r->n + 1; return 0; }
    u32 v = ((u32)r->p[r->pos] << 24) | ((u32)r->p[r->pos + 1] << 16) |
            ((u32)r->p[r->pos + 2] << 8) | r->p[r->pos + 3];
    r->pos += 4;
    return v;
}
static u8 png_read_byte(PngR *r) {
    if (r->pos >= r->n) { r->pos = r->n + 1; return 0; }
    return r->p[r->pos++];
}

static const u32 CRC_TAB[256] = {
    0x00000000,0x77073096,0xEE0E612C,0x990951BA,0x076DC419,0x706AF48F,0xE963A535,0x9E6495A3,
    0x0EDB8832,0x79DCB8A4,0xE0D5E91E,0x97D2D988,0x09B64C2B,0x7EB17CBD,0xE7B82D07,0x90BF1D91,
    0x1DB71064,0x6AB020F2,0xF3B97148,0x84BE41DE,0x1ADAD47D,0x6DDDE4EB,0xF4D4B551,0x83D385C7,
    0x136C9856,0x646BA8C0,0xFD62F97A,0x8A65C9EC,0x14015C4F,0x63066CD9,0xFA0F3D63,0x8D080DF5,
    0x3B6E20C8,0x4C69105E,0xD56041E4,0xA2677172,0x3C03E4D1,0x4B04D447,0xD20D85FD,0xA50AB56B,
    0x35B5A8FA,0x42B2986C,0xDBBBC9D6,0xACBCF940,0x32D86CE3,0x45DF5C75,0xDCD60DCF,0xABD13D59,
    0x26D930AC,0x51DE003A,0xC8D75180,0xBFD06116,0x21B4F4B5,0x56B3C423,0xCFBA9599,0xB8BDA50F,
    0x2802B89E,0x5F058808,0xC60CD9B2,0xB10BE924,0x2F6F7C87,0x58684C11,0xC1611DAB,0xB6662D3D,
    0x76DC4190,0x01DB7106,0x98D220BC,0xEFD5102A,0x71B18589,0x06B6B51F,0x9FBFE4A5,0xE8B8D433,
    0x7807C9A2,0x0F00F934,0x9609A88E,0xE10E9818,0x7F6A0DBB,0x086D3D2D,0x91646C97,0xE6635C01,
    0x6B6B51F4,0x1C6C6162,0x856530D8,0xF262004E,0x6C0695ED,0x1B01A57B,0x8208F4C1,0xF50FC457,
    0x65B0D9C6,0x12B7E950,0x8BBEB8EA,0xFCB9887C,0x62DD1DDF,0x15DA2D49,0x8CD37CF3,0xFBD44C65,
    0x4DB26158,0x3AB551CE,0xA3BC0074,0xD4BB30E2,0x4ADFA541,0x3DD895D7,0xA4D1C46D,0xD3D6F4FB,
    0x4369E96A,0x346ED9FC,0xAD678846,0xDA60B8D0,0x44042D73,0x33031DE5,0xAA0A4C5F,0xDD0D7CC9,
    0x5005713C,0x270241AA,0xBE0B1010,0xC90C2086,0x5768B525,0x206F85B3,0xB966D409,0xCE61E49F,
    0x5EDEF90E,0x29D9C998,0xB0D09822,0xC7D7A8B4,0x59B33D17,0x2EB40D81,0xB7BD5C3B,0xC0BA6CAD,
    0xEDB88320,0x9ABFB3B6,0x03B6E20C,0x74B1D29A,0xEAD54739,0x9DD277AF,0x04DB2615,0x73DC1683,
    0xE3630B12,0x94643B84,0x0D6D6A3E,0x7A6A5AA8,0xE40ECF0B,0x9309FF9D,0x0A00AE27,0x7D079EB1,
    0xF00F9344,0x8708A3D2,0x1E01F268,0x6906C2FE,0xF762575D,0x806567CB,0x196C3671,0x6E6B06E7,
    0xFED41B76,0x89D32BE0,0x10DA7A5A,0x67DD4ACC,0xF9B9DF6F,0x8EBEEFF9,0x17B7BE43,0x60B08ED5,
    0xD6D6A3E8,0xA1D1937E,0x38D8C2C4,0x4FDFF252,0xD1BB67F1,0xA6BC5767,0x3FB506DD,0x48B2364B,
    0xD80D2BDA,0xAF0A1B4C,0x36034AF6,0x41047A60,0xDF60EFC3,0xA867DF55,0x316E8EEF,0x4669BE79,
    0xCB61B38C,0xBC66831A,0x256FD2A0,0x5268E236,0xCC0C7795,0xBB0B4703,0x220216B9,0x5505262F,
    0xC5BA3BBE,0xB2BD0B28,0x2BB45A92,0x5CB36A04,0xC2D7FFA7,0xB5D0CF31,0x2CD99E8B,0x5BDEAE1D,
    0x9B64C2B0,0xEC63F226,0x756AA39C,0x026D930A,0x9C0906A9,0xEB0E363F,0x72076785,0x05005713,
    0x95BF4A82,0xE2B87A14,0x7BB12BAE,0x0CB61B38,0x92D28E9B,0xE5D5BE0D,0x7CDCEFB7,0x0BDBDF21,
    0x86D3D2D4,0xF1D4E242,0x68DDB3F8,0x1FDA836E,0x81BE16CD,0xF6B9265B,0x6FB077E1,0x18B74777,
    0x88085AE6,0xFF0F6A70,0x66063BCA,0x11010B5C,0x8F659EFF,0xF862AE69,0x616BFFD3,0x166CCF45,
    0xA00AE278,0xD70DD2EE,0x4E048354,0x3903B3C2,0xA7672661,0xD06016F7,0x4969474D,0x3E6E77DB,
    0xAED16A4A,0xD9D65ADC,0x40DF0B66,0x37D83BF0,0xA9BCAE53,0xDEBB9EC5,0x47B2CF7F,0x30B5FFE9,
    0xBDBDF21C,0xCABAC28A,0x53B39330,0x24B4A3A6,0xBAD03605,0xCDD70693,0x54DE5729,0x23D967BF,
    0xB3667A2E,0xC4614AB8,0x5D681B02,0x2A6F2B94,0xB40BBE37,0xC30C8EA1,0x5A05DF1B,0x2D02EF8D,
};
/* 累積可能な CRC-32 中間形式（zlib crc32 と同一）。開始値 0xFFFFFFFF、完了時 ^0xFFFFFFFF */
static u32 png_crc_raw(u32 crc, const u8 *p, u32 n) {
    for (u32 i = 0; i < n; i++) crc = (crc >> 8) ^ CRC_TAB[(crc ^ p[i]) & 0xFF];
    return crc;
}
/* PNG チャンク CRC: tag(4) + data(n) */
static u32 png_crc(const u8 *tag, const u8 *data, u32 n) {
    u32 crc = png_crc_raw(0xFFFFFFFFu, tag, 4);
    if (n) crc = png_crc_raw(crc, data, n);
    return crc ^ 0xFFFFFFFFu;
}

/* Paeth 予測子 */
static u8 paeth(u8 a, u8 b, u8 c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* PNG デコード。img は確保済み（w*h*4）。 */
static bool png_decode(const u8 *data, u32 len, u32 w, u32 h, u8 ct, u8 *out, char *err, u32 err_cap) {
    /* 1 ピクセルあたりバイト数 */
    u32 bpp;
    switch (ct) {
    case 0: bpp = 1; break;  /* グレー */
    case 2: bpp = 3; break;  /* RGB */
    case 4: bpp = 2; break;  /* グレー+α */
    case 6: bpp = 4; break;  /* RGBA */
    default:
        snprintf(err, err_cap, "png: unsupported color type %u", ct);
        return false;
    }
    u32 stride = w * bpp;
    u64 raw_len = (u64)(stride + 1) * h;
    if (raw_len > IMG_MAX_BYTES) { snprintf(err, err_cap, "png: image too large"); return false; }

    /* IDAT 連結 */
    PngR r;
    memset(&r, 0, sizeof r);
    r.p = data;
    r.n = len;
    r.pos = 8; /* シグネチャ後 */
    u8 *idat = (u8 *)malloc((u64)raw_len);
    if (!idat) { snprintf(err, err_cap, "png: oom"); return false; }
    u32 idat_n = 0;
    bool got_iend = false;
    bool ok = false;
    while (r.pos + 8 <= r.n) {
        u32 clen = png_read_be32(&r);
        char tag[5] = {0};
        for (u32 i = 0; i < 4; i++) tag[i] = (char)png_read_byte(&r);
        if (r.pos > r.n || r.pos + clen > r.n) break;
        u32 data_off = r.pos; /* チャンクデータ開始 */
        u32 crc_off = data_off + clen;
        r.pos = crc_off; /* crc はチャンクデータの直後にある */
        u32 crc = png_read_be32(&r);
        if (r.pos > r.n) break;
        /* CRC 検証（タグ + データ） */
        {
            u32 ccrc = png_crc((const u8 *)tag, clen ? data + data_off : NULL, clen);
            if (ccrc != crc) break; /* 破損 → IDAT 不足 → inflate 失敗で NULL */
        }
        if (memcmp(tag, "IDAT", 4) == 0) {
            if (idat_n + clen > (u32)raw_len) { /* IDAT は分割され得るが総量は raw 超えられない */
                /* 実際は圧縮データなので raw_len を超え得る。上限は IMG_MAX_BYTES */
                if (idat_n + clen > IMG_MAX_BYTES) break;
                u8 *ni = (u8 *)realloc(idat, idat_n + clen);
                if (!ni) { free(idat); snprintf(err, err_cap, "png: oom"); return false; }
                idat = ni;
            }
            memcpy(idat + idat_n, data + data_off, clen);
            idat_n += clen;
        } else if (memcmp(tag, "IEND", 4) == 0) {
            got_iend = true;
            break;
        }
        r.pos = crc_off + 4;
    }
    if (!got_iend || idat_n < 2) { free(idat); snprintf(err, err_cap, "png: missing IEND"); return false; }

    /* zlib: 2 バイトヘッダ（CMF/FLG）→ deflate */
    u8 *raw = (u8 *)malloc((size_t)raw_len);
    if (!raw) { free(idat); snprintf(err, err_cap, "png: oom"); return false; }
    u32 raw_n = 0;
    bool inf_ok = inflate(idat + 2, idat_n - 2, raw, (u32)raw_len, &raw_n);
    free(idat);
    if (!inf_ok || raw_n != raw_len) {
        free(raw);
        snprintf(err, err_cap, "png: inflate failed");
        return false;
    }

    /* フィルタ解除 → RGBA */
    for (u32 y = 0; y < h; y++) {
        u8 f = raw[y * (stride + 1)];
        const u8 *row = raw + y * (stride + 1) + 1;
        const u8 *prev = y ? raw + (y - 1) * (stride + 1) + 1 : NULL;
        u8 *px = out + (u64)y * w * 4;
        /* フィルタをその場で適用（raw の行を書き換えながら） */
        if (f == 1) { /* Sub */
            for (u32 i = bpp; i < stride; i++) ((u8 *)row)[i] = (u8)(row[i] + row[i - bpp]);
        } else if (f == 2) { /* Up */
            if (prev) for (u32 i = 0; i < stride; i++) ((u8 *)row)[i] = (u8)(row[i] + prev[i]);
        } else if (f == 3) { /* Average */
            for (u32 i = 0; i < stride; i++) {
                u8 a = i >= bpp ? row[i - bpp] : 0;
                u8 b = prev ? prev[i] : 0;
                ((u8 *)row)[i] = (u8)(row[i] + ((a + b) >> 1));
            }
        } else if (f == 4) { /* Paeth */
            for (u32 i = 0; i < stride; i++) {
                u8 a = i >= bpp ? row[i - bpp] : 0;
                u8 b = prev ? prev[i] : 0;
                u8 c = (i >= bpp && prev) ? prev[i - bpp] : 0;
                ((u8 *)row)[i] = (u8)(row[i] + paeth(a, b, c));
            }
        } else if (f != 0) {
            free(raw);
            snprintf(err, err_cap, "png: bad filter %u", f);
            return false;
        }
        /* RGBA へ */
        for (u32 x = 0; x < w; x++) {
            u32 o = x * 4;
            switch (ct) {
            case 0: px[o] = px[o + 1] = px[o + 2] = row[x]; px[o + 3] = 255; break;
            case 2: px[o] = row[x * 3]; px[o + 1] = row[x * 3 + 1]; px[o + 2] = row[x * 3 + 2]; px[o + 3] = 255; break;
            case 4: px[o] = px[o + 1] = px[o + 2] = row[x * 2]; px[o + 3] = row[x * 2 + 1]; break;
            case 6: px[o] = row[x * 4]; px[o + 1] = row[x * 4 + 1]; px[o + 2] = row[x * 4 + 2]; px[o + 3] = row[x * 4 + 3]; break;
            }
        }
    }
    free(raw);
    ok = true;
    return ok;
}

/* ================= BMP ================= */

static bool bmp_decode(const u8 *data, u32 len, u32 *w, u32 *h, u8 **px, char *err, u32 err_cap) {
    if (len < 54 || data[0] != 'B' || data[1] != 'M') {
        snprintf(err, err_cap, "bmp: not a BMP file");
        return false;
    }
    u32 data_off = (u32)data[10] | ((u32)data[11] << 8) | ((u32)data[12] << 16) | ((u32)data[13] << 24);
    u32 bw = (u32)data[18] | ((u32)data[19] << 8) | ((u32)data[20] << 16) | ((u32)data[21] << 24);
    i32 h_raw = (i32)((u32)data[22] | ((u32)data[23] << 8) | ((u32)data[24] << 16) | ((u32)data[25] << 24));
    u16 bpp = (u16)(data[28] | (data[29] << 8));
    u32 comp = (u32)data[30] | ((u32)data[31] << 8) | ((u32)data[32] << 16) | ((u32)data[33] << 24);
    if (bw == 0 || bw > IMG_MAX_DIM || h_raw == 0 || h_raw > (i32)IMG_MAX_DIM) {
        snprintf(err, err_cap, "bmp: bad dimensions");
        return false;
    }
    if (comp != 0) { snprintf(err, err_cap, "bmp: compressed BMP not supported"); return false; }
    bool flip = h_raw > 0; /* 正の高さ = ボトムアップ */
    u32 bh = (u32)(h_raw < 0 ? -h_raw : h_raw);
    u32 bytespp = bpp / 8;
    if (bpp != 24 && bpp != 32) { snprintf(err, err_cap, "bmp: only 24/32bpp supported"); return false; }
    if ((u64)bw * bh * 4 > IMG_MAX_BYTES) { snprintf(err, err_cap, "bmp: too large"); return false; }
    u32 stride = ((bw * bytespp + 3) / 4) * 4;
    if (data_off + (u64)stride * bh > len) { snprintf(err, err_cap, "bmp: truncated"); return false; }
    u8 *out = (u8 *)malloc((u64)bw * bh * 4);
    if (!out) { snprintf(err, err_cap, "bmp: oom"); return false; }
    for (u32 y = 0; y < bh; y++) {
        u32 src_y = flip ? (bh - 1 - y) : y;
        const u8 *row = data + data_off + (u64)src_y * stride;
        u8 *px = out + (u64)y * bw * 4;
        for (u32 x = 0; x < bw; x++) {
            u32 o = x * bytespp;
            px[x * 4] = row[o + 2];      /* R */
            px[x * 4 + 1] = row[o + 1];  /* G */
            px[x * 4 + 2] = row[o];      /* B */
            px[x * 4 + 3] = bpp == 32 ? row[o + 3] : 255;
        }
    }
    *w = bw;
    *h = bh;
    *px = out;
    return true;
}

/* ================= 公開 API ================= */

IfImage *if_img_decode(const u8 *data, u32 len, char *err_buf, u32 err_cap) {
    if (err_cap) err_buf[0] = 0;
    if (!data || len < 8) {
        if (err_cap) snprintf(err_buf, err_cap, "image: too short");
        return NULL;
    }
    /* PNG シグネチャ: 89 50 4E 47 0D 0A 1A 0A */
    if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        PngR r;
        memset(&r, 0, sizeof r);
        r.p = data;
        r.n = len;
        r.pos = 8;
        /* IHDR */
        u32 clen = png_read_be32(&r);
        char tag[5] = {0};
        for (u32 i = 0; i < 4; i++) tag[i] = (char)png_read_byte(&r);
        if (clen != 13 || memcmp(tag, "IHDR", 4) != 0 || r.pos + 13 > r.n) {
            if (err_cap) snprintf(err_buf, err_cap, "png: bad IHDR");
            return NULL;
        }
        u32 w = png_read_be32(&r);
        u32 h = png_read_be32(&r);
        u8 depth = png_read_byte(&r);
        u8 ct = png_read_byte(&r);
        u8 comp = png_read_byte(&r);
        u8 filt = png_read_byte(&r);
        u8 inter = png_read_byte(&r);
        if (w == 0 || h == 0 || w > IMG_MAX_DIM || h > IMG_MAX_DIM) {
            if (err_cap) snprintf(err_buf, err_cap, "png: bad dimensions");
            return NULL;
        }
        if (depth != 8) {
            if (err_cap) snprintf(err_buf, err_cap, "png: only 8-bit depth supported");
            return NULL;
        }
        if (comp != 0 || filt != 0 || inter != 0) {
            if (err_cap) snprintf(err_buf, err_cap, "png: compression/filter/interlace not supported");
            return NULL;
        }
        u64 total = (u64)w * h * 4;
        if (total > IMG_MAX_BYTES) {
            if (err_cap) snprintf(err_buf, err_cap, "png: image too large");
            return NULL;
        }
        IfImage *img = (IfImage *)malloc(sizeof(IfImage));
        if (!img) { if (err_cap) snprintf(err_buf, err_cap, "png: oom"); return NULL; }
        img->w = w;
        img->h = h;
        img->px = (u8 *)malloc((size_t)total);
        if (!img->px) { free(img); if (err_cap) snprintf(err_buf, err_cap, "png: oom"); return NULL; }
        if (!png_decode(data, len, w, h, ct, img->px, err_buf, err_cap)) {
            free(img->px);
            free(img);
            return NULL;
        }
        return img;
    }
    /* BMP */
    {
        u32 w, h;
        u8 *px;
        if (!bmp_decode(data, len, &w, &h, &px, err_buf, err_cap)) return NULL;
        IfImage *img = (IfImage *)malloc(sizeof(IfImage));
        if (!img) { free(px); if (err_cap) snprintf(err_buf, err_cap, "bmp: oom"); return NULL; }
        img->w = w;
        img->h = h;
        img->px = px;
        return img;
    }
}

void if_img_free(IfImage *img) {
    if (!img) return;
    free(img->px);
    free(img);
}
