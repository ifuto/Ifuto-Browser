#!/usr/bin/env python3
"""WPT tree-construction 全失敗列挙（run_html5lib.py はファイル先頭 1 件のみ表示のため）。

使い方: python3 tests/wpt_fails.py <ifuto バイナリ> [ファイル名フィルタ...]
出力: FAIL ごとに <file>#<index> と #data（先頭 120 字）。
      --verbose を付けると got/want の差分も出す。
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_html5lib import parse_dat


def main():
    binary = sys.argv[1]
    verbose = "--verbose" in sys.argv[2:]
    filters = [a for a in sys.argv[2:] if not a.startswith("--")]
    datdir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "wpt-tree-construction")
    files = sorted(f for f in os.listdir(datdir) if f.endswith(".dat"))
    if filters:
        files = [f for f in files if any(x in f for x in filters)]
    total_fail = 0
    for fn in files:
        for idx, t in enumerate(parse_dat(os.path.join(datdir, fn))):
            if t["fragment"] or t["script_on"]:
                continue
            r = subprocess.run([binary, "--dump-wptdom", "-"],
                               input=t["data"], capture_output=True,
                               text=True, timeout=10)
            got = r.stdout.rstrip("\n")
            want = t["expected"].rstrip("\n")
            if got != want:
                total_fail += 1
                print(f"== {fn}#{idx} :: {t['data'][:120]!r}")
                if verbose:
                    print("-- got --")
                    print(got)
                    print("-- want --")
                    print(want)
                    print()
    print(f"---- total failing: {total_fail}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
