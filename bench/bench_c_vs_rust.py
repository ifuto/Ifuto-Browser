#!/usr/bin/env python3
"""C vs Rust 多角ベンチドライバ（bench/data-*.json を吐く。BENCH.md 方法論準拠）。

計測規約:
  - paired interleaved A/B。対 i が偶数なら C→Rust、奇数なら Rust→C の順で
    ドリフトを相殺する。報告値は median（と min/max、符号検定の勝数）。
  - 全パイプライン計測で stdout byte 一致と scrub 済み stderr 一致を同時検証し、
    「計測が適合性サンプルも兼ねる」形にする（scrub は tools/diff_fuzz_cli.py と同一物）。
  - 起動速度は subprocess wall time（perf_counter）。/usr/bin/time 非存在のため
    BENCH.md 規約どおり親プロセス計測。
  - fuzz 等の CPU -heavy 処理と同時に走らせないこと（帯騒音 ±5–15ms）。

使い方:
  python3 bench/bench_c_vs_rust.py [--out bench/data-YYYYMMDD.json]
事前条件:
  python3 tools/gen_idm.py 2 && python3 tools/gen_idm.py 16   # コーパス決定再生成
"""
import json, os, re, statistics, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from diff_fuzz_cli import scrub_stats  # 同一 scrub（二重管理を避ける）

C_BIN = os.path.join(ROOT, "build/ifuto")
R_BIN = os.path.join(ROOT, "rust/target/release/ifuto")
M2 = os.path.join(ROOT, ".arena/idm/idm-2mb.md")
M16 = os.path.join(ROOT, ".arena/idm/idm-16mb.md")

TINY_HTML = (b"<!doctype html><title>t</title><p>hello <b>world</b> "
             b"\xe5\x90\xbe\xe8\xbc\xa9\xe3\x81\xaf\xe7\x8c\xab\xe3\x81\xa7\xe3\x81\x82\xe3\x82\x8b"
             b"</p>\n")

RE_PHASE = re.compile(
    rb"read=([\d.]+)ms parse=([\d.]+)ms style=([\d.]+)ms layout=([\d.]+)ms "
    rb"render=([\d.]+)ms total=([\d.]+)ms")
RE_META = re.compile(
    rb"nodes=(\d+) parse_errors=(\d+) grid=(\S+) links=(\d+) peak_rss_kb=(\d+)")


def run_binary(binary, args, stdin_data=None, env_extra=None):
    """(wall_ms, rc, stdout, stderr) を返す。wall は親プロセス wall。"""
    env = dict(os.environ)
    env.pop("IFUTO_MD_SLOW", None)
    if env_extra:
        env.update(env_extra)
    t0 = time.perf_counter()
    p = subprocess.run([binary] + args, input=stdin_data, capture_output=True,
                       env=env, timeout=120)
    wall = (time.perf_counter() - t0) * 1000.0
    return wall, p.returncode, p.stdout, p.stderr


def parse_stats(stderr):
    m = RE_PHASE.search(stderr)
    ph = dict(zip(("read", "parse", "style", "layout", "render", "total"),
                  (float(x) for x in m.groups()))) if m else {}
    mm = RE_META.search(stderr)
    meta = dict(zip(("nodes", "parse_errors", "grid", "links", "peak_rss_kb"),
                    (int(mm.group(1)), int(mm.group(2)), mm.group(3).decode(),
                     int(mm.group(4)), int(mm.group(5))))) if mm else {}
    return ph, meta


def paired(bench_name, args_fn, n_pairs, stdin_fn=None, env_fn=None):
    """対 i を交互順で C/Rust を実行。args_fn/env_fn は側 ("c"/"r") とモードに依存。

    戻り値: {"pairs": [ {side: {"wall_ms":…, "phases":…, "meta":…}, ...,
                          "stdout_equal": bool, "stderr_scrubbed_equal": bool} ]}
    """
    pairs = []
    for i in range(n_pairs):
        order = ("c", "r") if i % 2 == 0 else ("r", "c")
        rec = {}
        cout = rout = cerr = rerr = None
        for side in order:
            wall, rc, out, err = run_binary(
                C_BIN if side == "c" else R_BIN, args_fn(side),
                stdin_data=stdin_fn(side) if stdin_fn else None,
                env_extra=env_fn(side) if env_fn else None)
            ph, meta = parse_stats(err)
            rec[side] = {"wall_ms": round(wall, 3), "rc": rc,
                         "phases": ph, "meta": meta}
            if side == "c":
                cout, cerr = out, err
            else:
                rout, rerr = out, err
        rec["stdout_equal"] = (cout == rout)
        rec["stderr_scrubbed_equal"] = (scrub_stats(cerr) == scrub_stats(rerr))
        rec["rc_equal"] = (rec["c"]["rc"] == rec["r"]["rc"])
        pairs.append(rec)
        print("  %s pair %d/%d done (C %.1fms / R %.1fms)%s" % (
            bench_name, i + 1, n_pairs,
            rec["c"]["wall_ms"], rec["r"]["wall_ms"],
            "" if rec["stdout_equal"] and rec["stderr_scrubbed_equal"]
            else "  *** EXACTNESS VIOLATION ***"), flush=True)
    return {"pairs": pairs}


def summarize(pairs, key_fn, ka="c", kb="r"):
    """key_fn(rec_side)->float で両側系列の median/min/max と符号勝数を出す。

    通常は C("c") vs Rust("r")、fastdom では fast vs slow の比較に ka/kb で流用。"""
    cs = [key_fn(p[ka]) for p in pairs]
    rs = [key_fn(p[kb]) for p in pairs]
    wins = sum(1 for p in pairs if key_fn(p[ka]) < key_fn(p[kb]))
    ties = sum(1 for p in pairs if key_fn(p[ka]) == key_fn(p[kb]))
    return {
        ka: {"median": statistics.median(cs), "min": min(cs), "max": max(cs)},
        kb: {"median": statistics.median(rs), "min": min(rs), "max": max(rs)},
        "sign_%s_wins" % ka: wins, "sign_%s_wins" % kb: len(pairs) - wins - ties,
        "ties": ties, "n": len(pairs),
    }


def bench_startup(n_pairs=150):
    """tiny HTML render のプロセス wall。300 連プロセス（150 対）。

    /dev/stdin パイプは reader 実装の fseek 観測を歪め得るため実ファイルを使う
    （内容は本スクリプト内の定数 = 決定的、実行毎に再生成）。"""
    tiny = "/tmp/ifuto_bench_tiny.html"
    with open(tiny, "wb") as f:
        f.write(TINY_HTML)
    print("[startup] %d pairs..." % n_pairs, flush=True)
    return paired("startup", lambda s: ["--no-ansi", tiny], n_pairs)


def bench_pipeline(path, n_pairs, extra_args=None, label="pipeline"):
    args = ["--no-ansi", "--stats"] + (extra_args or []) + [path]
    print("[%s] %s x %d pairs..." % (label, os.path.basename(path), n_pairs), flush=True)
    return paired(label, lambda s: args, n_pairs)


def bench_fastdom(path, n_pairs):
    """IFUTO_MD_SLOW=1（HTML 往復の旧経路） vs 既定 fast-DOM。両バイナリで。"""
    print("[fastdom] %s x %d pairs..." % (os.path.basename(path), n_pairs), flush=True)
    args = ["--no-ansi", "--stats", path]
    out = {}
    for side, binary in (("c", C_BIN), ("r", R_BIN)):
        pairs = []
        for i in range(n_pairs):
            order = (0, 1) if i % 2 == 0 else (1, 0)  # 0=fast, 1=slow
            rec = {}
            for mode in order:
                env = {"IFUTO_MD_SLOW": "1"} if mode == 1 else None
                wall, rc, so, se = run_binary(binary, args, env_extra=env)
                ph, meta = parse_stats(se)
                rec["slow" if mode == 1 else "fast"] = {
                    "wall_ms": round(wall, 3), "phases": ph, "meta": meta}
            rec["stdout_equal"] = None  # 同一バイナリの慢速/快速 stdout 一致は下で
            pairs.append(rec)
            wall_f = rec["fast"]["wall_ms"]; wall_s = rec["slow"]["wall_ms"]
            print("  fastdom-%s pair %d/%d (fast %.1fms / slow %.1fms)" % (
                side, i + 1, n_pairs, wall_f, wall_s), flush=True)
        out[side] = {"pairs": pairs}
    # fast/slow stdout 一致検証（両バイナリで: kill switch が出力を変えないことの機械証明）
    eq = {"c": 0, "r": 0}
    for side, binary in (("c", C_BIN), ("r", R_BIN)):
        _, _, so_f, _ = run_binary(binary, args)
        _, _, so_s, _ = run_binary(binary, args, env_extra={"IFUTO_MD_SLOW": "1"})
        eq[side] = 1 if so_f == so_s else 0
    out["stdout_equal_fast_slow"] = eq
    return out


def env_info():
    def sh(cmd):
        return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout.strip()
    return {
        "uname": sh("uname -a"),
        "cpu_model": sh("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-").strip(),
        "nproc": sh("nproc"),
        "mem": sh("grep -m1 MemTotal /proc/meminfo"),
        "gcc": sh("gcc --version | head -1"),
        "rustc": sh("%s --version" % os.path.expandvars(
            "$HOME/akl-toolchain/cargo/bin/rustc") if os.path.exists(os.path.expandvars(
            "$HOME/akl-toolchain/cargo/bin/rustc")) else "rustc"),
        "python": sh("python3 --version"),
        "git_head": sh("git -C %s rev-parse HEAD" % ROOT),
        "git_branch": sh("git -C %s branch --show-current" % ROOT),
        "git_dirty_files": sh("git -C %s status --short | wc -l" % ROOT),
        "debian": sh("cat /etc/debian_version 2>/dev/null"),
    }


def binary_shape():
    def sha256(p):
        return subprocess.run(["sha256sum", p], capture_output=True, text=True
                              ).stdout.split()[0]
    def strip_size(p):
        tmp = "/tmp/strip_probe.bin"
        subprocess.run(["cp", p, tmp], check=True)
        subprocess.run(["strip", tmp], check=True)
        s = os.path.getsize(tmp)
        os.unlink(tmp)
        return s
    def ldd(p):
        return subprocess.run(["ldd", p], capture_output=True, text=True).stdout.strip()
    out = {}
    for side, p in (("c", C_BIN), ("r", R_BIN)):
        out[side] = {"path": os.path.relpath(p, ROOT),
                     "bytes": os.path.getsize(p), "stripped_bytes": strip_size(p),
                     "sha256": sha256(p), "ldd": ldd(p)}
    return out


def main():
    out_path = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else \
        os.path.join(ROOT, "bench", "data-%s.json" % time.strftime("%Y%m%d"))
    for p in (C_BIN, R_BIN, M2, M16):
        if not os.path.exists(p):
            sys.exit("missing %s (gen_idm.py でコーパス再生成せよ)" % p)
    data = {"started": time.strftime("%Y-%m-%d %H:%M:%S %z"),
            "env": env_info(), "shape": binary_shape(),
            "corpus": {"idm-2mb": {"path": os.path.relpath(M2, ROOT),
                                   "bytes": os.path.getsize(M2)},
                       "idm-16mb": {"path": os.path.relpath(M16, ROOT),
                                    "bytes": os.path.getsize(M16)}},
            "benches": {}}

    def save():
        with open(out_path, "w") as f:
            json.dump(data, f, ensure_ascii=False, indent=1)

    for name, thunk in (
        ("startup", lambda: bench_startup(150)),
        ("pipeline_2mb", lambda: bench_pipeline(M2, 13)),
        ("pipeline_16mb", lambda: bench_pipeline(M16, 7)),
        ("ansi_2mb", lambda: paired("ansi_2mb", lambda s: ["--stats", M2], 7)),
        ("fastdom_2mb", lambda: bench_fastdom(M2, 7)),
        ("fastdom_16mb", lambda: bench_fastdom(M16, 5)),
    ):
        data["benches"][name] = thunk()
        save()  # 逐次保存: サマリ段のバグで生計測を失わない

    # サマリ計算（total/wall/RSS/段別）
    summ = {}
    for name in ("pipeline_2mb", "pipeline_16mb", "ansi_2mb"):
        b = data["benches"][name]
        summ[name] = {
            "wall": summarize(b["pairs"], lambda s: s["wall_ms"]),
            "rss_kb": summarize(b["pairs"], lambda s: s["meta"]["peak_rss_kb"]),
            "phases": {ph: summarize(b["pairs"], lambda s, ph=ph: s["phases"][ph])
                       for ph in ("read", "parse", "style", "layout", "render", "total")},
            "exact": {"stdout": sum(1 for p in b["pairs"] if p["stdout_equal"]),
                      "stderr_scrubbed": sum(1 for p in b["pairs"]
                                             if p["stderr_scrubbed_equal"]),
                      "n": len(b["pairs"])},
            "meta_c": b["pairs"][0]["c"]["meta"], "meta_r": b["pairs"][0]["r"]["meta"],
        }
    b = data["benches"]["startup"]
    summ["startup"] = {"wall": summarize(b["pairs"], lambda s: s["wall_ms"]),
                       "exact": {"stdout": sum(1 for p in b["pairs"] if p["stdout_equal"]),
                                 "n": len(b["pairs"])}}
    for name in ("fastdom_2mb", "fastdom_16mb"):
        b = data["benches"][name]
        summ[name] = {"stdout_equal_fast_slow": b["stdout_equal_fast_slow"]}
        for side in ("c", "r"):
            pairs = b[side]["pairs"]
            summ[name][side] = {
                # slow を "c" 側・fast を "r" 側に見立てると sign_*_wins が
                # 「その経路が速かった回数」としてそのまま読める
                "total": summarize(pairs, lambda s: s["phases"]["total"],
                                   ka="slow", kb="fast"),
                "parse": summarize(pairs, lambda s: s["phases"]["parse"],
                                   ka="slow", kb="fast"),
                "rss_kb": summarize(pairs, lambda s: s["meta"]["peak_rss_kb"],
                                    ka="slow", kb="fast"),
            }
    data["summary"] = summ
    data["finished"] = time.strftime("%Y-%m-%d %H:%M:%S %z")
    with open(out_path, "w") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
    print("WROTE %s" % out_path)


if __name__ == "__main__":
    main()
