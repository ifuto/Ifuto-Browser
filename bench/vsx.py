#!/usr/bin/env python3
"""vsx — Akl vs QuickJS(lightness) vs V8>=9(speed) クロスエンジン計測・番兵。

設計原則（CHROME_SCOPE の掟）:
  * 推定値は書かない。全数値はこのマシンこの瞬間の実測（median of N 子プロセス）。
  * wall 時間・peak RSS はエンジンを子プロセスとして外側から測る（言語中立）。
  * エンジン間で stdout（ベンチ最終値）を突合し、一致しなければ正確性異常として即 FAIL。
    （速度だけの番兵は値の嘘を検出できないため、差分テスタを兼ねる）

使い方:
  python3 bench/vsx.py report [--quick] [--bench NAME]   ... 表を出すだけ（exit 0）
  python3 bench/vsx.py guard  [--quick]                  ... 不成立なら ANOMALY 行 + exit 1

環境変数: AKL_CLI(既定 build/akl) QJS(既定 /tmp/qjs-official/qjs, 無ければ qjs 系軸 SKIP)
          NODE(既定 node)
"""
import json, os, statistics, subprocess, sys, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JSDIR = os.path.join(ROOT, "bench", "js")
BENCHES = ["fib", "loop", "nest", "primes", "callseq", "strcat", "small", "empty"]
HOT = ["fib", "loop", "nest", "primes", "callseq", "strcat"]  # JIT が本気を出す領域
STARTUP = ["small", "empty"]                                   # 起動+parse 支配領域

HELPER = r'''
import json, resource, subprocess, sys, time
cmd = json.loads(sys.argv[1])
t0 = time.perf_counter()
p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
dt = (time.perf_counter() - t0) * 1000.0
rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss  # KiB (Linux)
print(json.dumps({"wall": dt, "rss": rss, "out": p.stdout.decode("utf-8", "replace"),
                  "rc": p.returncode, "err": p.stderr.decode("utf-8", "replace")[-400:]}))
'''

RSSRUN = os.path.join(ROOT, "build", "rssrun")

def run_one(cmd):
    r = subprocess.run([sys.executable, "-c", HELPER, json.dumps(cmd)],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    j = json.loads(r.stdout.decode())
    # RSS（と wall）は rssrun（C 製 fork/exec/wait4）で取り直す。python helper 経由は
    # fork 後 exec 前の python ページが ru_maxrss に混入し全エンジンに ~10MB の
    # 虚偽ベースラインが載る（実測確認済み。C2 の fib flake の真因）。stdout は突合に
    # 使うので helper 側を保持する
    if os.path.isfile(RSSRUN):
        rr = subprocess.run([RSSRUN] + cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        if rr.returncode == 0:
            parts = rr.stdout.decode().split()
            try:
                j["wall"] = float(parts[parts.index("wall_ms") + 1])
                j["rss"] = int(parts[parts.index("rss_kb") + 1])
            except (ValueError, IndexError):
                pass
    return j

def med_run(cmd, reps):
    ws, rss, out = [], 0, None
    for _ in range(reps):
        j = run_one(cmd)
        if j["rc"] != 0:
            return {"wall": None, "rss": None, "out": None, "err": j["err"]}
        ws.append(j["wall"]); rss = max(rss, j["rss"]); out = j["out"]
    return {"wall": statistics.median(ws), "rss": rss, "out": out, "err": ""}

def paired_net(bench_cmd, empty_cmd, reps):
    """ベースライン引き算は「独立 median 同士の差」だと empty 系と bench 系の
    測定時刻が離れており、spawn レイテンシの系統的ドリフト（負荷変動）が
    そのまま疑似 net 差になる（実測で nodejit の empty 中央値が 26-31ms 揺れ、
    2.5-6.6ms の偽 net 揺らぎ = C4 の false FAIL を招いた）。
    各 rep で empty の直後に bench を打ち、rep 内で差を取ってから median を取る。"""
    nets = []
    for _ in range(reps):
        j0 = run_one(empty_cmd)
        j1 = run_one(bench_cmd)
        if j0["rc"] != 0 or j1["rc"] != 0:
            return None
        nets.append(max(j1["wall"] - j0["wall"], 0.05))
    return statistics.median(nets)

def norm(out):
    """stdout を数値なら float に正規化して突合用キーを返す"""
    s = (out or "").strip()
    lines = [l for l in s.splitlines() if l.strip()]
    if not lines:
        return ("str", s)
    tail = lines[-1].strip()
    try:
        return ("num", float(tail))
    except ValueError:
        return ("str", tail if len(tail) < 4096 else (len(tail), tail[:64], tail[-64:]))

def get_engines():
    cli = os.environ.get("AKL_CLI", os.path.join(ROOT, "build", "akl_cli"))
    qjs = os.environ.get("QJS", "/tmp/qjs-official/qjs")
    node = os.environ.get("NODE", shutil.which("node") or "node")
    src_cache = {}
    def src_of(b):
        if b not in src_cache:
            with open(os.path.join(JSDIR, b + ".js"), "r", encoding="utf-8") as f:
                src_cache[b] = f.read()
        return src_cache[b]
    eng = {}
    if os.path.isfile(cli) and os.access(cli, os.X_OK):
        # 製品既定値（budget 打切りあり）のまま走らせる。bench/js は全て既定内に収束する。
        eng["akl"] = lambda b: [cli, os.path.join(JSDIR, b + ".js"), "1"]
    if os.path.isfile(qjs) and os.access(qjs, os.X_OK):
        eng["qjs"] = lambda b: [qjs, "-e", src_of(b) + "\nprint(typeof __R==='undefined'?0:__R);"]
    eng["v8jitless"] = lambda b: [node, "--jitless", "--no-warnings", "-e",
                                  src_of(b) + "\nconsole.log(typeof __R==='undefined'?0:__R);"]
    eng["v8full"] = lambda b: [node, "--no-warnings", "-e",
                               src_of(b) + "\nconsole.log(typeof __R==='undefined'?0:__R);"]
    return eng

def stripped_size(path, tag):
    if not path or not os.path.isfile(path):
        return None
    dst = "/tmp/vsx_sz_%s" % tag
    shutil.copyfile(path, dst)
    try:
        subprocess.run(["strip", dst], check=False)
    except Exception:
        pass
    return os.path.getsize(dst)

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "report"
    quick = "--quick" in sys.argv
    only = None
    if "--bench" in sys.argv:
        only = sys.argv[sys.argv.index("--bench") + 1]
    benches = [only] if only else BENCHES
    eng = get_engines()
    if "akl" not in eng:
        print("FATAL: build/akl not found (run: make build/akl)"); return 2
    have_qjs = "qjs" in eng
    if not have_qjs:
        print("NOTE: QuickJS not found -> qjs 軸は SKIP (QJS=/path/to/qjs で有効化)")
    print("engines: %s" % ", ".join(eng))

    # ---- 測定 ----
    R = {}  # R[bench][engine] = result
    for b in benches:
        R[b] = {}
        reps = 21 if b == "empty" else (5 if quick else 9)
        for e in eng:
            R[b][e] = med_run(eng[e](b), reps)
            if b != "empty":
                R[b][e]["net"] = paired_net(eng[e](b), eng[e]("empty"), reps)

    # ---- 正確性差分（全エンジン出力突合）----
    anomalies = []
    print("\n=== correctness cross-check (stdout last line, normalized) ===")
    for b in benches:
        keys = {e: norm(R[b][e]["out"]) for e in eng if R[b][e]["out"] is not None}
        ref = keys.get("akl")
        ok = all(k == ref for k in keys.values()) and ref is not None
        stat = "OK " if ok else "MISMATCH"
        print("  %-8s %-8s ref(akl)=%r others=%s" %
              (b, stat, ref if ref and len(repr(ref)) < 48 else repr(ref)[:48],
               {e: (k if k == ref else k) for e, k in keys.items() if e != "akl"}))
        if not ok:
            # 正確性異常は最優先 ANOMALY（どのエンジンが壊れたかの切り分けは出力で行う）
            anomalies.append("correctness mismatch on %s: %s" % (b, keys))

    # ---- 表: wall / rss ----
    def fm(x, unit, w=9, p=1):
        return ("%%%d.%df%s" % (w, p, unit)) % x if x is not None else ("%9s" % "ERR") + unit
    print("\n=== wall ms (median of subprocess runs; incl. process spawn) ===")
    hdr = "%-8s" % "bench" + "".join("%14s" % e for e in eng)
    print(hdr)
    for b in benches:
        row = "%-8s" % b
        for e in eng:
            row += fm(R[b][e]["wall"], "ms", 13) if R[b][e]["wall"] is not None else "%14s" % "ERR"
        print(row)
    # 起動定数差分: net = wall(bench) - wall(empty)（spawn+engine init を相殺した純仕事量の近似）
    wall = lambda b, e: R[b][e]["wall"]
    rss = lambda b, e: R[b][e]["rss"]
    def netwall(b, e):
        if R[b][e].get("net") is not None:
            return R[b][e]["net"]  # 同 rep 内差分（ドリフト排除済み）
        w, w0 = wall(b, e), wall("empty", e)
        if w is None or w0 is None:
            return None
        return max(w - w0, 0.05)
    if not only or "empty" in benches:
        print("\n=== net ms (wall - empty baseline; engine work only, spawn 相殺) ===")
        print(hdr)
        for b in benches:
            if b == "empty":
                continue
            row = "%-8s" % b
            for e in eng:
                x = netwall(b, e)
                row += fm(x, "ms", 13) if x is not None else "%14s" % "n/a"
            print(row)
    print("\n=== peak RSS KiB (max over reps, child rusage) ===")
    print(hdr)
    for b in benches:
        row = "%-8s" % b
        for e in eng:
            r = R[b][e]["rss"]
            row += ("%13dK" % r) if r is not None else "%14s" % "ERR"
        print(row)

    # ---- 比率（対 akl）----
    print("\n=== ratio vs akl (net wall / rss; >1 on wall 以外... rss>1 => akl が軽い) ===")
    for b in benches:
        cells = []
        v = R[b]["akl"]
        for e in eng:
            if e == "akl" or v["wall"] in (None, 0) or R[b][e]["wall"] is None:
                continue
            nv, ne = netwall(b, "akl"), netwall(b, e)
            ntxt = " net %.2fx" % (ne / nv) if (nv and ne) else ""
            cells.append("%s%s rss %.2fx" % (e, ntxt, (R[b][e]["rss"] or 1) / (v["rss"] or 1)))
        print("  %-8s %s" % (b, " | ".join(cells)))

    # ---- バイナリサイズ ----
    sizes = {"akl_cli": stripped_size(os.environ.get("AKL_CLI", os.path.join(ROOT, "build", "akl_cli")), "akl"),
             "qjs": stripped_size(os.environ.get("QJS", "/tmp/qjs-official/qjs"), "qjs") if have_qjs else None,
             "node": stripped_size(os.environ.get("NODE", shutil.which("node") or "node"), "node")}
    print("\n=== stripped binary size ===")
    for k, v in sizes.items():
        print("  %-8s %s" % (k, ("%d B (%.1f KiB)" % (v, v / 1024.0)) if v else "n/a"))

    # ---- 番兵判定 ----
    print("\n=== guards (hard gates; fail => ANOMALY, exit 1) ===")
    G = []
    def chk(name, ok, detail):
        G.append((name, ok, detail))
        print("  [%s] %-52s %s" % ("PASS" if ok else "FAIL", name, detail))
    aklsz, qjssz = sizes["akl_cli"], sizes["qjs"]
    if qjssz:
        chk("C1 size(akl_cli) <= 0.40*size(qjs)", aklsz <= 0.40 * qjssz,
            "%d vs 0.40*%d=%d" % (aklsz, qjssz, int(0.40 * qjssz)))
    else:
        chk("C1 size absolute floor: size(akl_cli) <= 300 KiB", aklsz <= 300 * 1024,
            "%d B" % (aklsz or 0))
    for b in benches:
        if R[b]["akl"]["wall"] is None:
            anomalies.append("akl crashed/failed on bench %s: %s" % (b, R[b]["akl"]["err"]))
            continue
    for b in benches:
        if wall(b, "akl") is None:
            continue
        if have_qjs and rss(b, "qjs") is not None:
            chk("C2 %-6s rss(akl) <= rss(qjs)" % b, rss(b, "akl") <= rss(b, "qjs"),
                "%dK vs %dK" % (rss(b, "akl"), rss(b, "qjs")))
        if rss(b, "v8jitless") is not None:
            chk("C3 %-6s rss(akl) <= 0.50*rss(v8)" % b, rss(b, "akl") <= 0.5 * rss(b, "v8jitless"),
                "%dK vs 0.5*%dK" % (rss(b, "akl"), rss(b, "v8jitless")))
        if b in HOT and netwall(b, "v8jitless") is not None and netwall(b, "akl") is not None:
            chk("C4 %-6s net(akl) <= 1.05*net(v8 --jitless)" % b,
                netwall(b, "akl") <= 1.05 * netwall(b, "v8jitless"),
                "%.1fms vs %.1fms (net)" % (netwall(b, "akl"), netwall(b, "v8jitless")))
        if b in HOT and have_qjs and netwall(b, "qjs") is not None and netwall(b, "akl") is not None:
            chk("C5 %-6s net(akl) <= 1.05*net(qjs)" % b,
                netwall(b, "akl") <= 1.05 * netwall(b, "qjs"),
                "%.1fms vs %.1fms (net)" % (netwall(b, "akl"), netwall(b, "qjs")))
        if b in STARTUP and wall(b, "v8full"):
            chk("C6 %-6s wall(akl) <= wall(v8 full) [startup axis]" % b,
                wall(b, "akl") <= wall(b, "v8full"),
                "%.1fms vs %.1fms" % (wall(b, "akl"), wall(b, "v8full")))
    print("\n=== report-only (aspirational, NOT currently gating) ===")
    for b in HOT:
        if b in R and netwall(b, "akl") and netwall(b, "v8full"):
            gap = netwall(b, "akl") / netwall(b, "v8full")
            print("  R1 %-6s net(akl)/net(v8 full JIT) = %.2fx  (parity => 1.0; %s)"
                  % (b, gap, "OPEN — no-JIT physics; attack steps ledgered in BENCH.md"
                     if gap > 1.0 else "MET"))

    failed = [n for n, ok, _ in G if not ok] + anomalies
    print("\n=== VSX VERDICT: %s ===" % ("PASS" if not failed else "FAIL"))
    for f in failed:
        print("  !!! ANOMALY: %s" % f)
    if mode == "guard" and failed:
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
