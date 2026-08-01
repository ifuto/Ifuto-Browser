#!/usr/bin/env python3
"""IDM（巨大 Markdown）計測コーパス生成器。決定的（乱数シード固定・再生成で同一内容）。

なぜツールが必要か: 2026-08 セッションでコーパスが環境消失と共に死んだ。
計測は「同じ入力の再生成」を前提にしなければ再現ベンチにならないため、
生成器をリポジトリに固定する。出力先 .arena/ は gitignore 済み。

使い方: python3 tools/gen_idm.py SIZE_MB [OUT]
例: python3 tools/gen_idm.py 16 .arena/idm/idm-16mb.md
"""
import sys

BLOCKS = [
    "## セクション {n}\n\n段落テキスト。**太字** と *斜体* と `code` を混ぜる。"
    "日本語の文章も入れる: 吾輩は猫である。名前はまだ無い。{n} 回目の繰り返し。\n\n",
    "### 表 {n}\n\n| col1 | col2 | col3 |\n|---|---|---|\n"
    "| alpha-{n} | beta-{n} | gamma-{n} |\n| d | e | f-{n} |\n| g-{n} | h | i |\n\n",
    "```c\n#include <stdio.h>\nint main(void) {{ printf(\"block {n}\\n\"); return 0; }}\n```\n\n",
    "- 項目 {n}a\n- 項目 {n}b\n  - ネスト {n}x\n  - ネスト {n}y\n- 項目 {n}c\n\n",
    "> 引用ブロック {n}。SSH は全人類の味方。\n> 二行目の引用です。\n\n<hr>\n\n",
    "#{n}? heading ではない。見出し {n}\n======\n\n本文の行。インラインで "
    "[リンク {n}](https://example.com/{n}) も入れる。\n\n",
]


def main():
    mb = float(sys.argv[1])
    out = sys.argv[2] if len(sys.argv) > 2 else ".arena/idm/idm-%gmb.md" % mb
    target = int(mb * 1024 * 1024)
    n = 0
    written = 0
    with open(out, "w", encoding="utf-8") as f:
        f.write("# IDM ベンチ用巨大 Markdown（自動生成・決定的）\n\n")
        while written < target:
            b = BLOCKS[n % len(BLOCKS)].format(n=n)
            f.write(b)
            written += len(b.encode("utf-8"))
            n += 1
    print("%s: %d bytes, %d blocks" % (out, written, n))


if __name__ == "__main__":
    main()
