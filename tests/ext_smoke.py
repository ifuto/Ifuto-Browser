#!/usr/bin/env python3
"""拡張 E1（docs/EXTENSIONS.md）の e2e 機械オラクル。
完了条件（同書 §7）:
  1. while(1){} の拡張は budget で死に、本体・他拡張は継続する
  2. manifest の未知 permission はロード FAILED（ケイパビリティ既定拒否）
  3. manifest 異形入力は FAILED が構文付きで出る（深い fuzz は fuzz_ext が担う）
  4. [ext] ロード結果行は決定的（golden）
加えて製品性:
  5. status 効果は shot ラスタに可視（toast 表面化）— PPM 差分が bottom 行帯のみ
  6. IFUTO_NO_EXT=1 の救済スイッチが一切の [ext] 出力を止める
  7. 既定自動ロード（$IFUTO_HOME/ext）も同一機構で動く
Usage: python3 tests/ext_smoke.py ./build/ifuto
"""
import os, subprocess, sys, tempfile, shutil

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/ifuto"
fails = []
checks = [0]

def chk(name, cond, detail=""):
    checks[0] += 1
    if cond:
        print("ok   %s" % name)
    else:
        fails.append(name)
        print("FAIL %s %s" % (name, detail))

def mkext(root, name, manifest, entry):
    d = os.path.join(root, name)
    os.makedirs(d)
    open(os.path.join(d, "manifest.txt"), "w").write(manifest)
    if entry is not None:
        open(os.path.join(d, "main.js"), "w").write(entry)
    return d

def run(args, env_extra=None, timeout=60):
    env = dict(os.environ, IFUTO_NO_STORE="1")
    if env_extra:
        env.update(env_extra)
    return subprocess.run([BIN] + args, capture_output=True, timeout=timeout, env=env)

PAGE = "tests/golden/doc.html"

with tempfile.TemporaryDirectory(prefix="ifuto_ext_") as tmp:
    extdir = os.path.join(tmp, "x")
    os.makedirs(extdir)

    mkext(extdir, "hello", "name: hello\nversion: 0.1\nentry: main.js\npermissions: status\n", '"Hello from EXTENSION"')
    mkext(extdir, "killer", "name: killer\nversion: 0.1\nentry: main.js\npermissions: status\n", "while(1){}")
    mkext(extdir, "badcap", "name: badcap\nversion: 0.1\nentry: main.js\npermissions: net\n", '"x"')
    mkext(extdir, "broken", "name: broken\nentry: main.js\n", '"x"')          # version 欠落
    mkext(extdir, "noentry", "name: noentry\nversion: 1\nentry: ghost.js\npermissions: log\n", None)
    mkext(extdir, "chatty", "name: chatty\nversion: 2\nentry: main.js\n",
          'console.log("hello", 42, true, {a:1}); console.log(); "done"')

    # (A) 5 拡張混合: 各行が期待どおり
    r = run(["--ext", extdir, "--shot", os.path.join(tmp, "a.ppm"), PAGE])
    err = r.stderr.decode("utf-8", "replace")
    lines = [l for l in err.splitlines() if l.startswith("[ext]")]
    want = [
        "[ext] badcap FAILED: manifest: line 4: unknown permission \"net\"",
        "[ext] broken FAILED: manifest: required key missing (version )",
        "[ext] chatty v2 loaded (perm: none)",
        "[ext] hello v0.1 loaded (perm: status)",
        "[ext] killer FAILED: instruction budget exhausted",
        "[ext] noentry FAILED: entry unreadable",
    ]
    chk("[ext] golden lines (sorted, one per ext)", lines == want, "| got: %r" % lines)
    # console.log（凍結 v1: ToString 連結・1 行化・perm 非依存の常設面）
    clines = [l for l in err.splitlines() if l.startswith("[ext:chatty:console]")]
    chk("console.log ToString line", "[ext:chatty:console] hello 42 true [object Object]" in clines,
        "| got: %r" % clines)
    chk("console.log zero-arg line", "[ext:chatty:console] " in clines, "| got: %r" % clines)
    chk("browser continues after ext failures (rc=0)", r.returncode == 0, "rc=%d" % r.returncode)

    # (B) status 効果は raster に可視（PPM 差分が bottom 帯行のみ = toast 表面化の機構証明）
    r0 = run(["--shot", os.path.join(tmp, "b0.ppm"), PAGE])
    a = open(os.path.join(tmp, "a.ppm"), "rb").read()
    b = open(os.path.join(tmp, "b0.ppm"), "rb").read()
    chk("ppm header identical", a.split(b"\n", 3)[:3] == b.split(b"\n", 3)[:3])
    if len(a) == len(b):
        diffs = [i for i in range(len(a)) if a[i] != b[i]]
        # PPM: 1000x720 の bottom status 行（ROW 44-45 相当）= ファイル尾部 1/16 付近
        frac = [i / len(a) for i in diffs]
        chk("status toast changes ppm", len(diffs) > 100)
        chk("ppm diffs confined to bottom status band", diffs and min(frac) > 0.90,
            "| min frac=%.3f n=%d" % (min(frac) if frac else -1, len(diffs)))
    else:
        chk("ppm size identical", False, "%d vs %d" % (len(a), len(b)))

    # (C) klion: budget は壁時間スパイクを生まない（10M ops は現 CPU で一瞬）
    import time
    t0 = time.monotonic()
    r = run(["--ext", extdir, "--shot", os.path.join(tmp, "c.ppm"), PAGE])
    dt = time.monotonic() - t0
    chk("budget death is fast (<10s for while(1) ext)", dt < 10.0, "dt=%.1fs" % dt)

    # (D) 救済スイッチ
    r = run(["--ext", extdir, "--shot", os.path.join(tmp, "d.ppm"), PAGE],
            env_extra={"IFUTO_NO_EXT": "1"})
    chk("IFUTO_NO_EXT=1 silences all [ext] lines", b"[ext]" not in r.stderr)

    # (E) 既定自動ロード: IFUTO_HOME/ext 以下（store create により enabled）
    home = os.path.join(tmp, "home")
    auto = os.path.join(home, "ext", "auto1")
    os.makedirs(auto)
    open(os.path.join(auto, "manifest.txt"), "w").write(
        "name: auto1\nversion: 3.2\nentry: main.js\npermissions: log\n")
    open(os.path.join(auto, "main.js"), "w").write('"auto-log-marker"')
    env = dict(os.environ, IFUTO_HOME=home)
    r = subprocess.run([BIN, "--shot", os.path.join(tmp, "e.ppm"), PAGE],
                       capture_output=True, env=env, timeout=60)
    chk("default autoload runs (perm log)", b'[ext:auto1] auto-log-marker' in r.stderr,
        detail="| got: %r" % r.stderr[-200:])
    chk("autoload loaded line", b"[ext] auto1 v3.2 loaded (perm: log)" in r.stderr)

    # (F) 明示 --ext で存在しない dir = 1 行エラー（自動経路黙殺との区別契約）
    r = run(["--ext", os.path.join(tmp, "nosuchdir"), "--shot", os.path.join(tmp, "f.ppm"), PAGE])
    chk("explicit missing dir reports", b"[ext] %s: cannot open" % os.path.join(tmp, "nosuchdir").encode() in r.stderr)

print("----")
print("ext_smoke: %d checks, %d failed" % (checks[0], len(fails)))
sys.exit(1 if fails else 0)
