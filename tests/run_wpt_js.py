#!/usr/bin/env python3
"""WPT JS テストランナー（AKL 用）。testharness サブセット + WPT テストを連結して
akl_cli で実行し、PASS/FAIL を集計する。

使い方: python3 tests/run_wpt_js.py [AKL_CLI] [--dir DIR] [--filter SUBSTR]
  AKL_CLI   既定 build/akl（製品ビルド）。--cli で明示可
  --dir     テストディレクトリ（既定 tests/wpt_js/tests）
  --filter  ファイル名フィルタ

WPT テストは無改変で実行する。未対応 API（fetch 等）は ReferenceError として
そのテストが FAIL する（黙ってスキップしない）。テストは tests/wpt_js/tests/ に
WPT リポジトリ（web-platform-tests）からコピーしたもの（LICENSE.wpt 参照）。
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.dirname(os.path.abspath(__file__))
HARNESS = os.path.join(HERE, "wpt_js", "harness.js")
DEFAULT_DIR = os.path.join(HERE, "wpt_js", "tests")


def main():
    cli = os.path.join(ROOT, "build", "akl")
    tdir = DEFAULT_DIR
    filt = ""
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--cli" and i + 1 < len(args):
            cli = args[i + 1]; i += 2
        elif args[i] == "--dir" and i + 1 < len(args):
            tdir = args[i + 1]; i += 2
        elif args[i] == "--filter" and i + 1 < len(args):
            filt = args[i + 1]; i += 2
        elif not args[i].startswith("--"):
            cli = args[i]; i += 1
        else:
            i += 1
    if not os.path.isfile(cli):
        print("akl binary not found: %s (run: make build/akl)" % cli, file=sys.stderr)
        return 2
    if not os.path.isfile(HARNESS):
        print("harness not found: %s" % HARNESS, file=sys.stderr)
        return 2
    with open(HARNESS, encoding="utf-8") as f:
        harness = f.read()

    files = sorted(f for f in os.listdir(tdir) if f.endswith(".any.js"))
    if filt:
        files = [f for f in files if filt in f]
    if not files:
        print("no tests found in %s (filter=%r)" % (tdir, filt), file=sys.stderr)
        return 2

    total_pass = total_fail = total_load_fail = 0
    print("== WPT JS (AKL) ==")
    for fn in files:
        path = os.path.join(tdir, fn)
        with open(path, encoding="utf-8") as f:
            test_src = f.read()
        # テスト末尾に dump を積む（async/promise の完了はマイクロタスク drain 内）
        src = harness + "\n" + test_src + "\nqueueMicrotask(__dump);\n0;"
        with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False,
                                         encoding="utf-8") as tf:
            tf.write(src)
            tmp_path = tf.name
        try:
            p = subprocess.run([cli, "--no-sandbox", tmp_path],
                               capture_output=True, text=True, timeout=120)
        finally:
            os.unlink(tmp_path)
        if p.returncode != 0 and "FAIL" not in p.stdout and "PASS" not in p.stdout:
            # 実行自体が失敗（構文エラー等）: テストロード失敗として記録
            err = (p.stderr or p.stdout or "").strip().splitlines()
            last = err[-1] if err else "?"
            print("  LOADFAIL %s :: %s" % (fn, last))
            total_load_fail += 1
            continue
        fpass = ffail = 0
        for ln in p.stdout.splitlines():
            if ln.startswith("PASS "):
                fpass += 1
            elif ln.startswith("FAIL "):
                ffail += 1
                print("  FAIL %s :: %s" % (fn, ln[5:]))
        total_pass += fpass
        total_fail += ffail
        print("  %-45s pass=%d fail=%d" % (fn, fpass, ffail))
    print("== WPT JS summary: pass=%d fail=%d loadfail=%d ==" %
          (total_pass, total_fail, total_load_fail))
    return 0 if total_fail == 0 and total_load_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
