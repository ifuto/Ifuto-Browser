#!/usr/bin/env python3
"""HT 採算プローブ: taskset で 1HT/2HT を固定し、2-slice の採算を独り勝ち脱がずに測る。

Configs (16MB corpus, --no-ansi --stats):
  R-serial-1ht : Rust IF_MD_PAR=0  taskset -c 0
  R-par2-1ht   : Rust IF_MD_PAR=1  taskset -c 0     (HT 無しの純 merge オーバーヘッド)
  R-serial-2ht : Rust IF_MD_PAR=0  taskset -c 0,1   (現行既定)
  R-par2-2ht   : Rust IF_MD_PAR=1  taskset -c 0,1   (候補既定)
  C-serial-1ht / C-par2-1ht / C-serial-2ht / C-par2-2ht : C 参照
ローテーション interleave でドリフト相殺。parse/layout/total の median を出す。
"""
import os, re, statistics, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
C_BIN = os.path.join(ROOT, "build/ifuto")
R_BIN = os.path.join(ROOT, "rust/target/release/ifuto")
M16 = os.path.join(ROOT, ".arena/idm/idm-16mb.md")
M2 = os.path.join(ROOT, ".arena/idm/idm-2mb.md")

RE_PHASE = re.compile(
    rb"read=([\d.]+)ms parse=([\d.]+)ms style=([\d.]+)ms layout=([\d.]+)ms "
    rb"render=([\d.]+)ms total=([\d.]+)ms")

CONFIGS = [
    ("R-serial-1ht", R_BIN, "0", "0"),
    ("R-par2-1ht",   R_BIN, "1", "0"),
    ("R-serial-2ht", R_BIN, "0", "0,1"),
    ("R-par2-2ht",   R_BIN, "1", "0,1"),
    ("C-serial-1ht", C_BIN, "0", "0"),
    ("C-par2-1ht",   C_BIN, "1", "0"),
    ("C-serial-2ht", C_BIN, "0", "0,1"),
    ("C-par2-2ht",   C_BIN, "1", "0,1"),
]

def run(binary, par, cpus, path):
    env = dict(os.environ)
    env["IF_MD_PAR"] = par
    t0 = time.perf_counter()
    p = subprocess.run(["taskset", "-c", cpus, binary, "--no-ansi", "--stats", path],
                       capture_output=True, env=env, timeout=180)
    wall = (time.perf_counter() - t0) * 1000.0
    m = RE_PHASE.search(p.stderr)
    ph = dict(zip(("read", "parse", "style", "layout", "render", "total"),
                  (float(x) for x in m.groups()))) if m else {}
    return wall, ph

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else M16
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 9
    data = {name: [] for name, *_ in CONFIGS}
    # warmup: 各 config 1 回
    for name, binary, par, cpus in CONFIGS:
        run(binary, par, cpus, path)
    for i in range(n):
        order = CONFIGS if i % 2 == 0 else list(reversed(CONFIGS))
        for name, binary, par, cpus in order:
            wall, ph = run(binary, par, cpus, path)
            rec = dict(ph); rec["wall"] = wall
            data[name].append(rec)
            print(f"  [{i}] {name}: parse={ph.get('parse', float('nan')):.2f} "
                  f"layout={ph.get('layout', float('nan')):.2f} "
                  f"total={ph.get('total', float('nan')):.2f} wall={wall:.2f}",
                  end="\r" if name != CONFIGS[-1][0] else "\n")
    print(f"\n== medians (n={n}, corpus={os.path.basename(path)}) ==")
    print(f"{'config':<14} {'read':>8} {'parse':>8} {'style':>8} {'layout':>8} "
          f"{'render':>8} {'total':>8} {'wall':>9}")
    for name, *_ in CONFIGS:
        rows = data[name]
        med = {k: statistics.median(r[k] for r in rows if k in r)
               for k in ("read", "parse", "style", "layout", "render", "total", "wall")}
        print(f"{name:<14} {med['read']:8.2f} {med['parse']:8.2f} {med['style']:8.2f} "
              f"{med['layout']:8.2f} {med['render']:8.2f} {med['total']:8.2f} {med['wall']:9.2f}")

if __name__ == "__main__":
    main()
