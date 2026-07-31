#!/usr/bin/env python3
"""ifuto-gui ヘッドレス黒盒検証（X 不要の --shot ラスタ経路）。

契約:
  1. --shot OUT.ppm PAGE は exit 0 で P6 PPM を吐く
  2. 寸法は既定 1000x720（GUI_DEF_W_PX x GUI_DEF_H_PX セル丸め後）
    3. 色数 >= 6（chrome 部 + 文書部が実際に塗られている。飾りの少ない素の
       HTML でも tabstrip/omni 強調/status/本文/白 で 6 色は乗る）
  4. 非白画素率 > 5%（真っ白 = 描画破綻を検出）
  5. 決定性: 同一入力 2 回の sha256 が一致（ラスタが揺れない）
  6. script/template 満載ページ（slim-DOM 経路）でも落ちずに描画する
"""
import hashlib
import os
import subprocess
import sys
import tempfile

FAIL = []


def check(cond, label):
    print(("ok  " if cond else "FAIL") + "  " + label)
    if not cond:
        FAIL.append(label)


def read_ppm(path):
    d = open(path, "rb").read()
    assert d[:2] == b"P6", "not P6"
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    maxv = int(parts[2])
    assert maxv == 255
    return w, h, parts[3]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def color_stats(px):
    colors = set()
    nonwhite = 0
    n = len(px) // 3
    for i in range(0, n * 3, 3):
        c = px[i:i+3]
        colors.add(bytes(c))
        if c != b"\xff\xff\xff":
            nonwhite += 1
    return len(colors), nonwhite / max(n, 1)


def main():
    gui = sys.argv[1] if len(sys.argv) > 1 else "./build/ifuto-gui"
    tmp = tempfile.mkdtemp(prefix="guismoke.")

    # 検証入力 1: 装飾つき Markdown（chrome 既定の .md 変換経路を通る）
    md = os.path.join(tmp, "page.md")
    with open(md, "w") as f:
        f.write("# GuiSmoke\n\n**bold** `code` [lnk](http://e.example)\n\n"
                "| a | b |\n|---|---|\n| 1 | 2 |\n\n"
                "> quoted\n\n- item one\n- item two\n\n"
                "```c\nint main(void){return 0;}\n```\n")
    # 検証入力 2: slim-DOM 経路（script/template だらけでも死なない）
    html = os.path.join(tmp, "heavy.html")
    with open(html, "w") as f:
        f.write("<!doctype html><title>Heavy</title><h1>Heavy page</h1>"
                "<p>visible alpha text</p>"
                "<script>var future = 'x'.repeat(4000);</script>"
                "<template><div>invisible tile</div></template>"
                "<p>visible omega text</p>")

    for name, page in (("md", md), ("heavy", html)):
        out1 = os.path.join(tmp, name + ".1.ppm")
        out2 = os.path.join(tmp, name + ".2.ppm")
        for out in (out1, out2):
            r = subprocess.run([gui, "--shot", out, page],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            check(r.returncode == 0, f"[{name}] --shot exit 0")
            check(os.path.exists(out), f"[{name}] ppm written")
        w, h, px = read_ppm(out1)
        check((w, h) == (1000, 720), f"[{name}] dims {w}x{h} == 1000x720")
        ncolors, nw = color_stats(px)
        check(ncolors >= 6, f"[{name}] distinct colors {ncolors} >= 6")
        check(nw > 0.05, f"[{name}] non-white coverage {nw:.1%} > 5%")
        check(sha256(out1) == sha256(out2), f"[{name}] deterministic raster")

    # usage 違反で落ち方も契約通りか（未知フラグは exit 2）
    r = subprocess.run([gui, "--bogus"], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    check(r.returncode == 2, "unknown flag exits 2")

    print("----")
    if FAIL:
        print(f"gui_smoke: FAIL ({len(FAIL)})")
        return 1
    print("gui_smoke: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
