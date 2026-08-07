#!/bin/sh
# 低予算 mutation fuzzer。種コーパスをランダム変異させ、ASan バイナリに喰わせる。
# 使い方: run_fuzz.sh <fuzzバイナリ> [イテレーション数]
# クラッシュを見つけたら artifacts/ に保存する。本格的な fuzzing は libFuzzer/AFL++ に移す（ROADMAP）。
set -u
BIN="${1:?usage: run_fuzz.sh BIN [N]}"
N="${2:-500}"
mkdir -p artifacts/fuzz
TMP=/tmp/ifuto_fuzz_input.html
CRASHES=0
export IFUZZ_BIN="$BIN" IFUZZ_TMP="$TMP"

python3 - "$N" <<'PYEOF'
import os, random, subprocess, sys

n_iters = int(sys.argv[1])
binary = os.environ["IFUZZ_BIN"]
tmp = os.environ["IFUZZ_TMP"]

seeds = []
seed_dir = os.environ.get("IFUZZ_SEEDS")
if seed_dir and os.path.isdir(seed_dir):
    for f in sorted(os.listdir(seed_dir)):
        seeds.append(open(os.path.join(seed_dir, f), "rb").read())
else:
    for d in ("tests/pages", "tests/golden"):
        if os.path.isdir(d):
            for f in os.listdir(d):
                if f.endswith(".html"):
                    seeds.append(open(os.path.join(d, f), "rb").read())
if not seeds:
    seeds = [b"<p>seed</p>"]

interesting = [b"<", b">", b"&", b"\r\n\r\n", b"chunked", b"http://", b"Content-Length:", b"\v", b"0x", b"/..", b'"', b"'", b"<!--", b"-->", b"</", b"{", b"}",
               b":", b";", b"@", b"<!-->", b"\x00", b"\xff", b"\xc3\x28", b"&#x110000;",
               b"<style>", b"</style>", b"!important", b"display:none", b"<li>", b"&#0;"]

def mutate(data: bytes) -> bytes:
    d = bytearray(data)
    for _ in range(random.randint(1, 12)):
        op = random.randint(0, 5)
        if not d:
            d = bytearray(random.choice(interesting))
        pos = random.randrange(len(d) + (1 if op == 1 else 0))
        if op == 0 and d:                       # バイト反転
            d[pos] ^= 1 << random.randint(0, 7)
        elif op == 1:                            # 興味深いトークン挿入
            d[pos:pos] = random.choice(interesting)
        elif op == 2 and d:                      # 削除
            del d[pos:pos + random.randint(1, 32)]
        elif op == 3 and d:                      # 複製
            d[pos:pos] = d[pos:pos + random.randint(1, 32)]
        elif op == 4 and d:                      # 切り詰め
            del d[pos:]
        else:                                    # ランダムバイト置換
            d[pos] = random.randrange(256)
    return bytes(d)

crashes = 0
for i in range(n_iters):
    inp = mutate(random.choice(seeds))
    with open(tmp, "wb") as fh:
        fh.write(inp)
    try:
        r = subprocess.run([binary, tmp], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=5)
        code = r.returncode
    except subprocess.TimeoutExpired:
        code = "TIMEOUT"
    if code != 0:
        crashes += 1
        path = f"artifacts/fuzz/crash-{i:05d}-code{code}.html"
        with open(path, "wb") as fh:
            fh.write(inp)
        print(f"CRASH[{code}] #{i} -> {path}")

print(f"fuzz done: {n_iters} iters, {crashes} crashes")
sys.exit(1 if crashes else 0)
PYEOF
