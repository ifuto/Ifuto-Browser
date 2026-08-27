#!/usr/bin/env python3
"""tools/zz_chrome_diff.py — chrome モデル純粋部の C↔Rust 差分 fuzz。

C オラクル（tools/zz_chrome_dump.c → /tmp/zz_chrome_c）と Rust 移植
（rust/ifuto-core/examples/zz_chrome_dump.rs）に同一の決定論入力列を流し、
stdout 全行を byte 突合する。両ドライバは 1 入力行 = 1 出行の対応を持つため、
不一致行番号 = 不一致入力の再現行になる。

使い方:
    python3 tools/zz_chrome_diff.py [Nケース] [--seed S]
再現:
    同じ seed と N で完全に同じ入力列が再生成される（乱数は python random）。

信頼域（両実装で同一の運用ドメイン。逸脱入力は生成しない）:
    - 文字列引数は NUL・0x01・改行を含まない（C の文字列規約と driver センチネル）。
    - DUP の cap >= 1（C は cap==0 で u32 underflow → 4GiB alloc 即死領域）。
    - SCR/LNK/QUI の算術は 2026-08-26 に C 側を明示 wrap 化済み（UB 排除・観測不変）。
      以後は i32 極端値も信頼域（両側 wrap で一致）。
    - RES の cap は生成域 {0..8192}（driver の out=512B センチネル検出と整合:
      cap<=511 のみ「書かれた」観測が可能。cap>511 は rc のみが意味を持つ）。
"""

import argparse
import os
import random
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CDRV = "/tmp/zz_chrome_c"
RDRV = os.path.join(ROOT, "rust/target/release/examples/zz_chrome_dump")

# 0x01・NUL・0x0A を避ける（driver 規約）。タブ・多バイト・記号を混ぜる
ALPHA = [
    "a", "b", "z", "A", "B", "Z", "0", "9", "5", "/", ":", ".", "-", "_", "~",
    " ", "\t", "吾", "輩", "猫", "é", "\xff", "\xc3", "\x80", "@", "#", ">", "<",
]


def hx(bs: bytes) -> str:
    return bs.hex()


def hxE(bs: bytes) -> str:
    """空文字列をトークン消失させないためのエンコード（"-" は hex に出ない）。
    両 driver の unhex は "-" を無効文字で打ち切り = 空文字列と同値になる。"""
    return bs.hex() if bs else "-"


def rbytes(rng: random.Random, lo: int = 0, hi: int = 40) -> bytes:
    n = rng.randint(lo, hi)
    return "".join(rng.choice(ALPHA) for _ in range(n)).encode("utf-8", "surrogateescape")


def gen_ops(rng: random.Random, n: int):
    ops = []
    for _ in range(n):
        kind = rng.randrange(100)
        if kind < 12:  # CI
            h = rbytes(rng, 0, 60)
            nd = rbytes(rng, 0, 20)
            if rng.random() < 0.5 and nd and h:
                # hit を誘発: hay の一部を needle に
                st = rng.randint(0, len(h))
                ln = min(len(nd), len(h) - st)
                nd = h[st:st + ln] if ln else nd
            ops.append("CI %s %s" % (hxE(h), hxE(nd)))
        elif kind < 20:  # DUP
            s = rbytes(rng, 0, 300)
            cap = rng.choice([1, 2, 3, 4, 5, 8, 16, 64, 255, 256, rng.randint(1, 300)])
            ops.append("DUP %s %d" % (hxE(s), max(1, cap)))
        elif kind < 36:  # SCR / SCT
            scroll = rng.choice([rng.randint(-1 << 20, 1 << 20),
                                 (1 << 31) - 2, -(1 << 31) + 1, 0])
            delta = rng.choice([rng.randint(-1 << 16, 1 << 16),
                                (1 << 31) - 2, -(1 << 31), 1, -1])
            vh = rng.choice([-64, -1, 0, 1, 8, 20, 24, 40, 80,
                             rng.randint(-100, 4096)])
            doc_h = rng.choice([-64, -1, 0, 1, 10, 100, 500,
                                rng.randint(-100, 100000), (1 << 31) - 2])
            if rng.random() < 0.5:
                ops.append("SCR %d %d %d %d" % (scroll, delta, vh, doc_h))
            else:
                pos = rng.randint(-1 << 20, 1 << 20)
                ops.append("SCT %d %d %d" % (pos, vh, doc_h))
        elif kind < 46:  # QUI
            nt = rng.choice([-2, -1, 0, 1, 2, 3, 5, 64, 65])
            armed = rng.choice([-3, -1, 0, 1, 50, 100, 1 << 40, -(1 << 40)])
            now = rng.choice([0, 1, 3, 100, 101, 103, 104, 1 << 40,
                              rng.randint(0, 1 << 40)])
            ops.append("QUI %d %d %d" % (nt, armed, now))
        elif kind < 58:  # LNK
            nl = rng.choice([0, 1, 2, 3, 5, 9, 64, 1000, (1 << 31) - 1,
                             1 << 31, (1 << 32) - 1, rng.randint(0, 64)])
            idx = rng.choice([-5, -3, -1, 0, 1, 2, 3, 4, 8, 63, 999,
                              (1 << 31) - 2, -(1 << 31) + 2])
            delta = rng.choice([-10, -7, -2, -1, 0, 1, 2, 7, 10,
                                (1 << 31) - 2, -(1 << 31) + 2, -(1 << 31)])
            ops.append("LNK %d %d %d" % (idx, delta, nl))
        elif kind < 78:  # RES
            r = rng.random()
            if r < 0.25:  # スキーム注入（位置 0,1,2,8,9 重点）
                pos = rng.choice([0, 1, 2, 8, 9, 3, 5, 30])
                head = rbytes(rng, pos, pos)
                inp = head + b"://" + rbytes(rng, 0, 10)
            elif r < 0.45:  # 絶対パス
                inp = b"/" + rbytes(rng, 0, 120)
                if rng.random() < 0.2:  # 絶対パス内にも ://（順序 quirk）
                    inp = b"/ab://x" if rng.random() < 0.5 else b"/x://" + \
                        rbytes(rng, 0, 4)
            elif r < 0.55:  # ws 先行
                inp = rng.choice([b" ", b"\t", b"  \t "]) + rbytes(rng, 0, 30)
            elif r < 0.62:  # 空/ws のみ
                inp = rng.choice([b"", b" ", b"\t\t", b"   "])
            elif r < 0.70:  # 巨大入力（snprintf 4095 截断の観測）
                inp = rbytes(rng, 4000, 5200)
            else:
                inp = rbytes(rng, 1, 200)
            cw = rng.choice([b"", b"/", b"/w", b"/tmp", b"rel", b"a/b/c",
                             rbytes(rng, 0, 24),
                             rbytes(rng, 4000, 4100) if rng.random() < 0.2 else b"."])
            cap = rng.choice([0, 1, 2, 3, 4, 5, 8, 16, 64, 511, 512, 513,
                              4095, 4096, 8192, rng.randint(0, 9000)])
            ops.append("RES %s %s %d" % (hxE(inp), hxE(cw), cap))
        elif kind < 90:  # FT
            maxv = rng.choice([0, 1, 2, 3, 4, 9, 16, -1])
            nt = rng.randint(0, 6)
            q = rbytes(rng, 1, 12)
            parts = []
            for _i in range(nt):
                t = rbytes(rng, 0, 40)
                u = rbytes(rng, 0, 60)
                if rng.random() < 0.6:  # hit 誘発（query かその大小反転を埋め込む）
                    needle = q
                    if rng.random() < 0.3:
                        needle = bytes((c ^ 0x20) if (65 <= c <= 90)
                                       or (97 <= c <= 122) else c for c in q)
                    if rng.random() < 0.5:
                        t = t[:rng.randint(0, len(t))] + needle + \
                            t[rng.randint(0, len(t)):]
                    else:
                        u = u[:rng.randint(0, len(u))] + needle + \
                            u[rng.randint(0, len(u)):]
                g = "-" if rng.random() < 0.3 else hxE(rbytes(rng, 1, 12))
                parts += [hxE(t), hxE(u), g]
            ops.append("FT %d %d %s %s" % (maxv, nt, hxE(q), " ".join(parts)))
        else:  # CI の エッジ: needle 長 > hay、空
            h = rbytes(rng, 0, 8)
            nd = rbytes(rng, 9, 20) if rng.random() < 0.5 else b""
            ops.append("CI %s %s" % (hxE(h), hxE(nd)))
    return ops


def build_drivers():
    """必要なら C/Rust ドライバを組み立てる（冪等）。"""
    if not os.path.exists(CDRV):
        srcs = ["tools/zz_chrome_dump.c", "src/chrome.c", "src/common.c"]
        cmd = (["gcc", "-std=c11", "-O1", "-Wall", "-Wextra",
                "-Wno-unused-parameter", "-ffunction-sections", "-Isrc"] +
               [os.path.join(ROOT, s) for s in srcs] +
               ["-Wl,--gc-sections", "-o", CDRV])
        subprocess.run(cmd, check=True, cwd=ROOT)
        print("built", CDRV, file=sys.stderr)
    if not os.path.exists(RDRV):
        env = dict(os.environ)
        cargo = os.path.expanduser("~/cargo/bin/cargo")
        if not os.path.exists(cargo):
            cargo = "cargo"
        env.setdefault("RUSTUP_HOME", os.path.expanduser("~/rustup"))
        env.setdefault("CARGO_HOME", os.path.expanduser("~/cargo"))
        subprocess.run([cargo, "build", "--release", "--offline", "--example",
                        "zz_chrome_dump"], check=True,
                       cwd=os.path.join(ROOT, "rust"), env=env)
        print("built", RDRV, file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("n", nargs="?", type=int, default=20000)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    build_drivers()
    rng = random.Random(args.seed)
    ops = gen_ops(rng, args.n)
    data = ("\n".join(ops) + "\n").encode()
    outs = []
    rcs = []
    for drv in (CDRV, RDRV):
        p = subprocess.run([drv], input=data, capture_output=True, timeout=600)
        outs.append(p.stdout)
        rcs.append(p.returncode)
    # 出力同一でも procecss 死は不一致扱いする（途中まで一致は成果ではない）
    if outs[0] == outs[1] and rcs == [0, 0]:
        print("zz_chrome_diff: %d cases (seed %d) → 0 mismatch (%d B identical)"
              % (args.n, args.seed, len(outs[0])))
        return 0
    # 1 入力 1 出力の対応から不一致行を同定
    cl = outs[0].split(b"\n")
    rl = outs[1].split(b"\n")
    print("MISMATCH: C %d lines vs Rust %d lines" % (len(cl), len(rl)))
    shown = 0
    # 再現容易性のため入力を /tmp に退避
    with open("/tmp/zz_chrome_keep.bin", "wb") as f:
        f.write(data)
    for i, (a, b) in enumerate(zip(cl, rl)):
        if a != b:
            print("line %d:\n  in: %r\n  C : %r\n  R : %r"
                  % (i, ops[i] if i < len(ops) else b"?", a, b))
            shown += 1
            if shown >= 5:
                break
    print("入力列は /tmp/zz_chrome_keep.bin（両 driver にそのまま流せる）")
    return 1


if __name__ == "__main__":
    sys.exit(main())
