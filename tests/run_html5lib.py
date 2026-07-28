#!/usr/bin/env python3
"""ifuto tree-construction 採点ハーネス（html5lib-tests / WPT .dat 形式）。

使い方: python3 tests/run_html5lib.py <ifuto バイナリ> <.dat ディレクトリ>

形式: テストは #data / #errors / [#new-errors] / [#document-fragment] /
[#script-on|#script-off] / #document の各セクション。テスト区切りは
「空行 + #data」またはファイル末尾（テキスト内の空行曖昧性をこの先読みで避ける）。

スキップ規則（分母から除外し、SKIP として計数）:
  - #document-fragment あり → フラグメント解析は未実装
  - #script-on あり → スクリプト有効 UA のみの期待のため
比較: | indented 形式の完全一致（末尾改行だけ正規化）。
"""
import os
import subprocess
import sys

SECTION_MARKERS = ("#data", "#errors", "#new-errors", "#document-fragment",
                   "#script-on", "#script-off", "#document")


def parse_dat(path):
    """1 ファイルをテスト dict のリストに分解。"""
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    lines = text.split("\n")
    tests = []
    cur = None  # dict: {sec: [lines]}
    sec = None
    for i, line in enumerate(lines):
        if line in SECTION_MARKERS:
            if cur is None:
                cur = {}
            sec = line
            cur.setdefault(sec, [])
            continue
        if sec is None:
            continue
        # #document セクションの終端: 空行の「次」が #data なら test 終了
        if (sec == "#document" and line == "" and i + 1 < len(lines)
                and lines[i + 1] == "#data"):
            tests.append(cur)
            cur = None
            sec = None
            continue
        cur[sec].append(line)
    if cur is not None:
        tests.append(cur)
    out = []
    for t in tests:
        data = "\n".join(t.get("#data", []))
        doc_lines = t.get("#document", [])
        # 末尾の区切り空行を落とす（テキスト行自体の空白は残す）
        while doc_lines and doc_lines[-1] == "":
            doc_lines.pop()
        out.append({
            "data": data,
            "expected": "\n".join(doc_lines),
            "fragment": "#document-fragment" in t,
            "script_on": "#script-on" in t,
        })
    return out


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    binary, datdir = sys.argv[1], sys.argv[2]
    files = sorted(f for f in os.listdir(datdir) if f.endswith(".dat"))
    total = passed = skipped = 0
    per_file = []
    fails_by_file = {}
    for fn in files:
        tests = parse_dat(os.path.join(datdir, fn))
        fp = fs = ft = 0
        first_fail = None
        for t in tests:
            if t["fragment"] or t["script_on"]:
                skipped += 1
                fs += 1
                continue
            ft += 1
            r = subprocess.run([binary, "--dump-wptdom", "-"], input=t["data"],
                               capture_output=True, text=True, timeout=10)
            got = r.stdout.rstrip("\n")
            want = t["expected"].rstrip("\n")
            if got == want:
                passed += 1
                fp += 1
            else:
                if first_fail is None:
                    first_fail = t
        total += ft
        per_file.append((fn, fp, ft, fs))
        if fp < ft:
            fails_by_file[fn] = (ft - fp, first_fail)

    for fn, fp, ft, fs in per_file:
        pct = (100.0 * fp / ft) if ft else 0.0
        print(f"  {fn:44s} {fp:4d}/{ft:<4d} ({pct:5.1f}%)  skip={fs}")
    print("----")
    pct = (100.0 * passed / total) if total else 0.0
    print(f"tree-construction: {passed}/{total} passed ({pct:.1f}%), skipped {skipped}")
    worst = sorted(fails_by_file.items(), key=lambda kv: -kv[1][0])[:6]
    if worst:
        print("worst files:")
        for fn, (nfail, _) in worst:
            print(f"  {fn}: {nfail} failures")
        fn, (_, t) = worst[0]
        print(f"---- first failing test in {fn} ----")
        print("#data:", repr(t["data"][:200]))
        r = subprocess.run([binary, "--dump-wptdom", "-"], input=t["data"],
                           capture_output=True, text=True)
        print("-- got --")
        print(r.stdout[:600])
        print("-- want --")
        print(t["expected"][:600])
    return 0


if __name__ == "__main__":
    sys.exit(main())
