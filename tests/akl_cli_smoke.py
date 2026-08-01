#!/usr/bin/env python3
"""akl CLI（単体ランナー）の e2e スモーク。
製品法則を製品経路で検査する:
  1. while(1){} は budget で必ず死ぬ（旧 1e12 既定で「一生終わらない」事故の回帰防止）
  2. bench/js は CLI 製品既定 budget（500M ops）で全て収束する
  3. --budget の上書きが効く（0 = エンジン既定 10M で arith は打切られる）
  4. --no-cojit で結果値が同一（CoJIT は意味を変えない、速さだけ）
  5. stdout → /dev/null で落ちない（seccomp TCGETS 限定許可の回帰防止）
  6. --no-sandbox の明示解除が効く
Usage: python3 tests/akl_cli_smoke.py ./build/akl
"""
import subprocess, sys, os, time

CLI = sys.argv[1] if len(sys.argv) > 1 else "./build/akl"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(CLI and os.path.abspath(__file__))))
JS = os.path.join(ROOT, "bench", "js")
fails = []
checks = [0]

def chk(name, cond, detail=""):
    checks[0] += 1
    if cond:
        print("ok   %s" % name)
    else:
        fails.append(name)
        print("FAIL %s %s" % (name, detail))

def run(args, timeout=40, devnull=False, inp=None):
    t0 = time.monotonic()
    kw = dict(stdout=subprocess.DEVNULL if devnull else subprocess.PIPE,
              stderr=subprocess.PIPE, timeout=timeout)
    r = subprocess.run([CLI] + args, input=inp, **kw)
    return r, time.monotonic() - t0

# 1. 無限ループ: budget kill（デフォルト 500M ops = この CPU で数秒）
proj = os.path.join(ROOT, ".arena", "akl_smoke_inf.js")
os.makedirs(os.path.dirname(proj), exist_ok=True)
open(proj, "w").write("while (1) {}\n")
t0 = time.monotonic()
r = subprocess.run([CLI, proj], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
dt = time.monotonic() - t0
chk("infinite loop killed by budget", r.returncode != 0 and b"budget" in r.stderr,
    "rc=%d stderr=%s" % (r.returncode, r.stderr[:120]))
chk("infinite loop dies fast (%.1fs < 55s)" % dt, dt < 55.0)

# 2. bench/js 全収束（製品既定 budget）
for b in ["empty", "tiny", "fib30", "arith", "branch", "strcat_flat", "strcat_grow"]:
    p = os.path.join(JS, b + ".js")
    if not os.path.isfile(p):
        continue
    r, _ = run([p])
    chk("bench/%s converges under default budget" % b, r.returncode == 0,
        r.stderr[:120].decode(errors="replace"))

# 3. --budget 上書き（0 → エンジン既定 10M で arith は打切）
arith = os.path.join(JS, "arith.js")
r, _ = run(["--budget", "0", arith])
chk("--budget 0 = engine default (arith dies)", r.returncode != 0 and b"budget" in r.stderr,
    "rc=%d %s" % (r.returncode, r.stderr[:120]))
r, _ = run(["--budget", "200000000", arith])
chk("--budget 200M runs arith", r.returncode == 0, r.stderr[:120].decode(errors="replace"))

# 4. --no-cojit は値を変えない（速さ差の検査は bench 側）
tiny = os.path.join(JS, "tiny.js")
r1, _ = run([tiny])
r2, _ = run(["--no-cojit", tiny])
chk("--no-cojit same result value", r1.returncode == 0 and r1.stdout == r2.stdout,
    "%r vs %r" % (r1.stdout, r2.stdout))

# 5. stdout → DEVNULL（glibc isatty 判定の ioctl(TCGETS) を seccomp で限定許可。
#    広い ioctl 許可をしないまま /dev/null 出力は生きること。旧実装は SIGSYS）
r, _ = run([tiny], devnull=True)
chk("stdout to /dev/null is not SIGSYS-killed", r.returncode == 0,
    "rc=%d %s" % (r.returncode, r.stderr[:120]))

# 6. --no-sandbox（明示解除のみ有効であること。既定 ON は上の全テストが証明）
r, _ = run(["--no-sandbox", tiny])
chk("--no-sandbox accepted", r.returncode == 0, r.stderr[:120].decode(errors="replace"))

print("----")
print("akl_cli_smoke: %d checks, %d failures" % (checks[0], len(fails)))
sys.exit(1 if fails else 0)
