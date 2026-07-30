#!/usr/bin/env python3
"""Debug helper: run one .dat file through ifuto --dump-wptdom and show
full diffs for failing tests. Usage: dbg_test.py <file.dat> [test_index...]"""
import subprocess
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_html5lib import parse_dat  # noqa: E402


def main():
    path = sys.argv[1]
    want_idx = {int(x) for x in sys.argv[2:]} if len(sys.argv) > 2 else None
    binary = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", os.environ.get("IFUTO_BIN","build/ifuto"))
    tests = parse_dat(path)
    nfail = 0
    for i, t in enumerate(tests):
        if t.get("fragment") or t.get("script_on"):
            continue
        r = subprocess.run([binary, "--dump-wptdom", "-"], input=t["data"],
                           capture_output=True, text=True)
        got = r.stdout.rstrip("\n")
        want = t["expected"].rstrip("\n")
        if got != want:
            nfail += 1
            if want_idx is None or i in want_idx:
                print(f"===== test #{i} FAIL =====")
                print("#data:", repr(t["data"]))
                if t.get("errors"):
                    print("#errors:", t["errors"][:2])
                gl, wl = got.split("\n"), want.split("\n")
                import difflib
                for line in difflib.unified_diff(wl, gl, "want", "got",
                                                 lineterm="", n=2):
                    print(line)
                print()
    print(f"total failing: {nfail}")


if __name__ == "__main__":
    main()
