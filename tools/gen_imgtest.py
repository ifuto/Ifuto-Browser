#!/usr/bin/env python3
# gen_imgtest.py — tests/test_image.c が参照する /tmp/imgtest の画像を生成する。
#
# 背景: test_image.c は /tmp/imgtest/{t1,t2,t3,big}.png と t4.bmp を fopen する。
# /tmp は永続でないため、新しい環境では必ず先にこのスクリプトを実行すること:
#     python3 tools/gen_imgtest.py && make test
# 期待ピクセル値は tests/test_image.c 内のリテラルと 1:1 に対応させてある。
# 依存: Python 標準ライブラリのみ（zlib/struct）。Pillow 不要。

import os
import struct
import zlib

OUT = "/tmp/imgtest"


def png_write(path, w, h, rgba):
    """RGBA8 PNG を書く。各行にフィルタ 2 (Up) などを混ぜてデコーダの全フィルタ経路を踏む。"""
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        row = rgba[y * stride:(y + 1) * stride]
        # フィルタ種別を行ごとにローテーション（0=None,1=Sub,2=Up,3=Average,4=Paeth）
        f = y % 5
        raw.append(f)
        prev = rgba[(y - 1) * stride:y * stride] if y > 0 else bytes(stride)
        for i, b in enumerate(row):
            a = row[i - 4] if i >= 4 else 0
            up = prev[i]
            ul = prev[i - 4] if i >= 4 else 0
            if f == 0:
                raw.append(b)
            elif f == 1:
                raw.append((b - a) & 0xFF)
            elif f == 2:
                raw.append((b - up) & 0xFF)
            elif f == 3:
                raw.append((b - ((a + up) >> 1)) & 0xFF)
            else:
                p = a + up - ul
                pa, pb, pc = abs(p - a), abs(p - up), abs(p - ul)
                pr = a if (pa <= pb and pa <= pc) else (up if pb <= pc else ul)
                raw.append((b - pr) & 0xFF)

    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)  # 8bit RGBA
    data = (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))
    with open(path, "wb") as fp:
        fp.write(data)


def png_write_gray(path, w, h, gray):
    """グレー8bit PNG。tests の期待値は RGB 化済み（g,g,g,255）。"""
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(gray[y * w:(y + 1) * w])

    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)  # 8bit grayscale
    data = (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))
    with open(path, "wb") as fp:
        fp.write(data)


def bmp24_write(path, w, h, bgr_topdown):
    """24bpp BMP（ボトムアップ）。bgr_topdown は上から下の画素順で渡す。"""
    stride = (w * 3 + 3) & ~3
    img = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray(bgr_topdown[y * w * 3:(y + 1) * w * 3])
        row.extend(b"\x00" * (stride - w * 3))
        img.extend(row)
    off = 14 + 40
    hdr = b"BM" + struct.pack("<IHHI", off + len(img), 0, 0, off)
    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(img), 2835, 2835, 0, 0)
    with open(path, "wb") as fp:
        fp.write(hdr + info + bytes(img))


def main():
    os.makedirs(OUT, exist_ok=True)

    # t1.png: 2x2 RGBA（赤/緑/青/白）— test_image.c の t1[] と一致
    t1 = bytes([
        255, 0, 0, 255,  0, 255, 0, 255,
        0, 0, 255, 255,  255, 255, 255, 255,
    ])
    png_write(f"{OUT}/t1.png", 2, 2, t1)

    # t2.png: 4x4 グラデーション — px2[] の生成式と一致
    px2 = bytearray(4 * 4 * 4)
    for y in range(4):
        for x in range(4):
            o = (y * 4 + x) * 4
            px2[o] = (x * 60) % 256
            px2[o + 1] = (y * 60) % 256
            px2[o + 2] = (x + y) * 40 % 256
            px2[o + 3] = 255
    png_write(f"{OUT}/t2.png", 4, 4, bytes(px2))

    # t3.png: 3x2 グレー — g[] と一致
    g = [0, 128, 255, 10, 200, 100]
    png_write_gray(f"{OUT}/t3.png", 3, 2, bytes(g))

    # t4.bmp: 3x2 24bpp — bgr[] と一致
    bgr = bytes([
        255, 0, 0,  0, 255, 0,  0, 0, 255,
        255, 255, 0, 0, 255, 255, 255, 0, 255,
    ])
    bmp24_write(f"{OUT}/t4.bmp", 3, 2, bgr)

    # big.png: 100x100（サイズ検証のみ）
    big = bytearray(100 * 100 * 4)
    for i in range(100 * 100):
        big[i * 4] = (i * 7) % 256
        big[i * 4 + 1] = (i * 13) % 256
        big[i * 4 + 2] = (i * 29) % 256
        big[i * 4 + 3] = 255
    png_write(f"{OUT}/big.png", 100, 100, bytes(big))

    print(f"gen_imgtest: {OUT} に 5 ファイル生成完了")


if __name__ == "__main__":
    main()
