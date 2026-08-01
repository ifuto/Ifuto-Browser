#!/usr/bin/env python3
# Ifuto — md fast path 差分オラクル（決定的生成・機械比較）
# 生成した md 文書群について fast(ifuto) vs slow(IFUTO_MD_SLOW=1) の
# --dump-dom / --no-ansi / ansi 出力が全一致することを検証する。
import os, random, subprocess, sys, tempfile

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/ifuto"
N = int(sys.argv[2]) if len(sys.argv) > 2 else 300
rng = random.Random(0x1F07)

SPECIALS = list("`*_~![]<>\\&#|-+.\"' ") + ["×", "あ", "\t"]
WORDS = ["alpha", "beta", "猫", "吾輩は猫である", "x&y", "<b>", "a|b", "  indented",
         "**bold**", "*em*", "`code`", "~~del~~", "[l](http://u)", "![i](src)",
         "[^f]", "\\<", "\\*", "\\&", "<http://auto>", "<ftp://no>", "#notheading",
         "\\", "[]", "!", "\\x", "***", "---", "===", ">"]
def rw(n): return "".join(rng.choice(WORDS if rng.random()<0.6 else SPECIALS) for _ in range(n))

def gen_doc():
    lines = []
    for _ in range(rng.randrange(1, 40)):
        r = rng.random()
        if r < 0.12: lines.append("#" * rng.randrange(1, 8) + " " + rw(4))
        elif r < 0.2: lines.append(rng.choice(["-", "*", "_", "---", "***", "___", "--", "- - -"]) + " " * rng.randrange(0, 3))
        elif r < 0.3: lines.append(rng.choice(["- ", "* ", "+ ", "1. ", "9) ", "123. "]) + rw(5))
        elif r < 0.36: lines.append(("  " * rng.randrange(0, 4)) + rng.choice(["- ", "2. "]) + rw(5))
        elif r < 0.42: lines.append(">" * rng.randrange(1, 3) + " " + rw(5))
        elif r < 0.5:
            f = rng.choice(["```", "~~~", "````"])
            lines.append(f + rng.choice(["", "c", "py js", "x y"]))
            for _ in range(rng.randrange(0, 5)): lines.append(rw(6))
            if rng.random() < 0.8: lines.append(f[:3])
        elif r < 0.56:
            cells = "|".join(rw(2) for _ in range(rng.randrange(1, 4)))
            lines.append("|" + cells + "|")
            lines.append("|" + "|".join("-" * rng.randrange(1, 5) for _ in range(3)) + "|")
            for _ in range(rng.randrange(0, 4)): lines.append("|" + "|".join(rw(2) for _ in range(3)) + "|")
        elif r < 0.62: lines.append("[^" + rng.choice(["a", "b", "f", "x y", ""]) + "]: " + rw(5))
        elif r < 0.7: lines.append(rw(8))
        elif r < 0.76: lines.append("    " + rw(4))
        elif r < 0.8: lines.append("[^" + rng.choice(["a", "f"]) + "] " + rw(4))
        elif r < 0.86: lines.append("[" + rw(3) + "](" + rng.choice(["http://u", "v w", ""]) + ")" + rw(3))
        else: lines.append("")
    return "\n".join(lines) + rng.choice(["", "\n", "\n\n"])

env_slow = dict(os.environ, IFUTO_MD_SLOW="1")
fails = 0
with tempfile.TemporaryDirectory() as td:
    for i in range(N):
        doc = gen_doc().encode()
        fp = os.path.join(td, "t.md")
        open(fp, "wb").write(doc)
        for args in (["--dump-dom"], ["--no-ansi"], [], ["--no-ansi", "--width", "40"]):
            a = subprocess.run([BIN] + args + [fp], capture_output=True)
            b = subprocess.run([BIN] + args + [fp], capture_output=True, env=env_slow)
            if a.stdout != b.stdout or a.returncode != b.returncode:
                fails += 1
                print(f"MISMATCH case {i} args={args}")
                open(os.path.join(td, f"fail{i}.md"), "wb").write(doc)
                open(f"/tmp/md_diff_fail_{i}.md", "wb").write(doc)
                open(f"/tmp/md_diff_fail_{i}.fast", "wb").write(a.stdout)
                open(f"/tmp/md_diff_fail_{i}.slow", "wb").write(b.stdout)
                if fails > 4: print("too many; stop"); sys.exit(1)
print(f"md_diff: {N} docs x 4 views -> {fails} mismatch")
sys.exit(1 if fails else 0)
