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

    # リンクフォーカス可視化（IF_SHOT_FOCUS フック。対話 Tab/Enter と同一 paint 経路）
    flinks = os.path.join(tmp, "focus.html")
    with open(flinks, "w") as f:
        f.write("<!doctype html><title>Focus</title><h1>links</h1>"
                "<p><a href=\"a.html\">open sesame page link</a> and text</p>")
    def shot(out, focus=None):
        env = dict(os.environ)
        if focus is not None:
            env["IF_SHOT_FOCUS"] = focus
        else:
            env.pop("IF_SHOT_FOCUS", None)
        r = subprocess.run([gui, "--shot", out, flinks], env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return r.returncode
    base_p = os.path.join(tmp, "focus.base.ppm")
    f0_p = os.path.join(tmp, "focus.f0.ppm")
    f0b_p = os.path.join(tmp, "focus.f0b.ppm")
    oob_p = os.path.join(tmp, "focus.oob.ppm")
    check(shot(base_p) == 0, "[focus] base shot exit 0")
    check(shot(f0_p, "0") == 0, "[focus] focus=0 shot exit 0")
    check(shot(f0b_p, "0") == 0, "[focus] focus=0 (2nd) shot exit 0")
    check(shot(oob_p, "999") == 0, "[focus] focus=999 shot exit 0")
    check(sha256(f0_p) == sha256(f0b_p), "[focus] focused raster deterministic")
    check(sha256(f0_p) != sha256(base_p), "[focus] focus changes raster")
    check(sha256(oob_p) == sha256(base_p), "[focus] out-of-range focus == unfocused")
    # 差分はセル行帯（16px）の 1 帯のみに限定される（単行リンクの span 矩形契約）
    w, h, pb = read_ppm(base_p)
    w2, h2, pf = read_ppm(f0_p)
    bands = set()
    if (w2, h2) == (w, h):
        n = w * h
        for i in range(n):
            o, q = i * 3, i * 3
            if pb[o:o+3] != pf[q:q+3]:
                bands.add((i // w) // 16)
        check(len(bands) == 1 and bands.issubset(set(range(2, 44))),
              f"[focus] diff confined to one doc cell-row band {sorted(bands)}")
    else:
        check(False, "[focus] dims stable")

    print("----")
    if FAIL:
        print(f"gui_smoke: FAIL ({len(FAIL)})")
        return 1
    print("gui_smoke: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
