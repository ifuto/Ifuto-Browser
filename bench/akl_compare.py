#!/usr/bin/env python3
"""Akl vs QuickJS(軽さ) / vs V8(速さ: --jitless と full) の実測比較ハーネス。
推定値は一切使わない。全数値はローカル実測のみ。

通常モード: 表を表示し bench/results/latest.json に保存。
guard モード: bench/akl_guards.json の閾値と照合し、1件でも超過があれば
「ANOMALY: ...」を表示して exit 1（常時監視アラーム）。
参照エンジンが見つからない相対閾値は SKIP と明示表示（PASS とは数えない）。"""
import argparse, json, os, shutil, statistics, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JS = os.path.join(ROOT, "bench", "js")
RESULTS = os.path.join(ROOT, "bench", "results")
BENCHES = ["empty", "tiny", "fib30", "arith", "branch", "strcat_flat", "strcat_grow"]

def find_engines():
    e = {"akl": [os.path.join(ROOT, "build", "akl_cli")]}
    qjs = os.environ.get("QJS", "/home/user/ref/quickjs/qjs")
    if os.path.isfile(qjs) and os.access(qjs, os.X_OK):
        e["qjs"] = [qjs]
    node = shutil.which("node")
    if node:
        e["nodejit"] = [node, "--jitless"]
        e["node"] = [node]
    return e

def wall_median(cmd, rounds):
    ts = []
    for _ in range(rounds):
        t0 = time.perf_counter()
        r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if r.returncode != 0:
            sys.stderr.write("FAIL %s: %s\n" % (" ".join(cmd), r.stderr.decode(errors="replace")[:200]))
            return None
        ts.append((time.perf_counter() - t0) * 1000.0)
    return statistics.median(ts)

RSSRUN = os.path.join(ROOT, "build", "rssrun")

def maxrss_kb(cmd):
    """build/rssrun（C 製 fork/exec/wait4 ラッパ）で子の ru_maxrss を取る。
    python ラッパ経由だと fork 後 exec 前の python ページが混入して
    全エンジンに ~10MB の虚偽ベースラインが載る（実測済み）ため python 直測は禁止。"""
    if not os.path.isfile(RSSRUN):
        sys.stderr.write("RSS SKIP: build/rssrun がありません（cc -std=c11 -O2 -o build/rssrun bench/rssrun.c）\n")
        return -1
    out = subprocess.run([RSSRUN] + cmd, capture_output=True, text=True)
    if out.returncode != 0:
        sys.stderr.write("RSS FAIL: %s\n" % " ".join(cmd))
        return -1
    for tok in out.stdout.strip().split():
        # 形式: "wall_ms X rss_kb Y"
        pass
    parts = out.stdout.strip().split()
    try:
        return int(parts[parts.index("rss_kb") + 1])
    except (ValueError, IndexError):
        sys.stderr.write("RSS PARSE FAIL: %s -> %s\n" % (" ".join(cmd), out.stdout[:120]))
        return -1

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--rss", action="store_true", help="RSS も測定（fib30 のみ重いので選択的）")
    ap.add_argument("--json", action="store_true", help="latest.json に保存")
    ap.add_argument("--guard", type=str, default="", help="閾値 JSON と照合して違反時 exit 1")
    args = ap.parse_args()

    eng = find_engines()
    if "akl" not in eng or not os.path.isfile(eng["akl"][0]):
        print("build/akl がありません: make build/akl"); return 2
    print("engines:", {k: " ".join(v) for k, v in eng.items()})

    res = {"time_ms": {}, "rss_kb": {}, "size_bytes": {}}
    for b in BENCHES:
        path = os.path.join(JS, b + ".js")
        res["time_ms"][b] = {}
        for name, cmd0 in eng.items():
            r = wall_median(cmd0 + [path], args.rounds)
            res["time_ms"][b][name] = round(r, 3) if r is not None else None
    if args.rss:
        for b in ["empty", "tiny", "fib30", "strcat_grow"]:
            res["rss_kb"][b] = {name: maxrss_kb(cmd0 + [os.path.join(JS, b + ".js")]) for name, cmd0 in eng.items()}

    os.makedirs(RESULTS, exist_ok=True)
    qjs_stripped = os.path.join(RESULTS, "qjs.stripped")
    if "qjs" in eng:
        shutil.copy2(eng["qjs"][0], qjs_stripped)
        subprocess.run(["strip", "--strip-all", qjs_stripped], check=False)
        res["size_bytes"]["qjs_stripped"] = os.path.getsize(qjs_stripped)
        res["size_bytes"]["qjs_raw"] = os.path.getsize(eng["qjs"][0])
    res["size_bytes"]["akl_cli"] = os.path.getsize(eng["akl"][0])

    hdr = "%-12s" % "bench" + "".join("%12s" % n for n in eng)
    print(hdr); print("-" * len(hdr))
    for b in BENCHES:
        print("%-12s" % b + "".join("%11.3fms" % res["time_ms"][b][n] if res["time_ms"][b][n] is not None else "%12s" % "FAIL" for n in eng))
    if res["rss_kb"]:
        print("\nRSS (KB):")
        print("%-12s" % "bench" + "".join("%12s" % n for n in eng))
        for b, row in res["rss_kb"].items():
            print("%-12s" % b + "".join("%10dKB" % row[n] for n in eng))
    print("\nsize:", {k: ("%d KB" % (v // 1024)) for k, v in res["size_bytes"].items()})
    if "qjs" in eng:
        print("\nratio vs qjs (time, >1 は akl が遅い):")
        for b in BENCHES:
            a, c = res["time_ms"][b]["akl"], res["time_ms"][b]["qjs"]
            print("  %-12s %s" % (b, "%.3f" % (a / c) if (a is not None and c) else "FAIL"))
    if "nodejit" in eng:
        print("ratio vs V8 --jitless:")
        for b in BENCHES:
            a, c = res["time_ms"][b]["akl"], res["time_ms"][b]["nodejit"]
            print("  %-12s %s" % (b, "%.3f" % (a / c) if (a is not None and c) else "FAIL"))
    if "node" in eng:
        print("ratio vs V8 full (JIT)")
        for b in BENCHES:
            a, c = res["time_ms"][b]["akl"], res["time_ms"][b]["node"]
            print("  %-12s %s" % (b, "%.3f" % (a / c) if (a is not None and c) else "FAIL"))

    if args.json:
        out = os.path.join(RESULTS, "latest.json")
        with open(out, "w") as f:
            json.dump({"engines": {k: " ".join(v) for k, v in eng.items()}, **res}, f, indent=2)
        print("saved", out)

    if not args.guard:
        return 0
    with open(args.guard) as f:
        g = json.load(f)
    fails = []
    skips = []
    def chk(cond, label, detail):
        if cond is None:
            skips.append(label)
        elif not cond:
            fails.append("%s: %s" % (label, detail))
    # 絶対閾値（参照エンジン不要。常時有効）
    ab = g.get("absolute", {})
    if "akl_cli_max_bytes" in ab:
        chk(res["size_bytes"]["akl_cli"] <= ab["akl_cli_max_bytes"],
            "ABS akl_cli size", "%d > %d" % (res["size_bytes"]["akl_cli"], ab["akl_cli_max_bytes"]))
    if "time_ms_max" in ab:
        for b, lim in ab["time_ms_max"].items():
            chk(res["time_ms"][b]["akl"] <= lim, "ABS akl time %s" % b, "%.3f > %s" % (res["time_ms"][b]["akl"], lim))
    if res["rss_kb"] and "rss_kb_max" in ab:
        for b, lim in ab["rss_kb_max"].items():
            if b in res["rss_kb"]:
                chk(res["rss_kb"][b]["akl"] <= lim, "ABS akl RSS %s" % b, "%d > %d" % (res["rss_kb"][b]["akl"], lim))
    # 相対閾値（参照エンジンが要る）
    rel = g.get("relative", {})
    for axis, spec in rel.items():
        ref = spec["ref"]; kind = spec["kind"]
        if ref not in eng:
            for b in spec.get("max_ratio", {}):
                skips.append("REL %s %s (ref %s 不在)" % (axis, b, ref))
            continue
        for b, lim in spec.get("max_ratio", {}).items():
            if kind == "time":
                va, vb = res["time_ms"][b].get("akl"), res["time_ms"][b].get(ref)
                v = (va / vb) if (va is not None and vb) else float("inf")
            elif kind == "rss":
                if not res["rss_kb"] or b not in res["rss_kb"]:
                    skips.append("REL %s %s (rss 未測定: --rss を付ける)" % (axis, b)); continue
                v = res["rss_kb"][b]["akl"] / res["rss_kb"][b][ref]
            elif kind == "size":
                if "qjs_stripped" not in res["size_bytes"]:
                    skips.append("REL %s (qjs_stripped 不在)" % axis); continue
                v = res["size_bytes"]["akl_cli"] / res["size_bytes"]["qjs_stripped"]
            else:
                continue
            chk(v <= lim, "REL %s %s" % (axis, b), "ratio %.3f > %.3f" % (v, lim))
    for s in skips:
        print("SKIP:", s)
    if fails:
        print("\n*** ANOMALY DETECTED (guard 違反) ***")
        for f_ in fails:
            print("ANOMALY:", f_)
        return 1
    print("guard: ALL PASS")
    return 0

if __name__ == "__main__":
    sys.exit(main())
