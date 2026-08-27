#!/usr/bin/env python3
"""C/Rust CLI 差分 fuzz（フェーズ 8-z 回帰ハーネス）。

C バイナリ（build/ifuto）と Rust バイナリ（rust/target/release/ifuto）を、
ランダム生成文書 × フラグ行列で並走させ、stdout / stderr / 終了コードを
byte 一致で突合する。`--stats` 実行のみタイミング値を正規化して比較する。

使い方: python3 tools/diff_fuzz_cli.py [N_CASES] [--seed S] [--ratio R]
既定: 20,000 件。不一致があれば詳細を表示して exit 1。
"""
import os, random, re, subprocess, sys, tempfile

C_BIN = os.environ.get("IFUTO_C", "./build/ifuto")
R_BIN = os.environ.get("IFUTO_R", "./rust/target/release/ifuto")

# ---------------- 文書生成 ----------------

TAGS = ["p", "div", "span", "b", "i", "em", "strong", "u", "s", "h1", "h2", "h3",
        "h4", "ul", "ol", "li", "dl", "dt", "dd", "pre", "code", "blockquote",
        "hr", "br", "img", "a", "table", "tr", "td", "th", "thead", "tbody",
        "caption", "select", "option", "textarea", "template", "style", "script",
        "title", "svg", "math", "mi", "annotation-xml", "center", "font",
        "article", "section", "blockquote", "sub", "sup", "small", "mark",
        "my-widget", "x-unknown", "article"]

TEXTS = ["hello world", "日本語のテキスト", "混在abc漢字", "a" * 40,
         "吾輩は猫である。名前はまだ無い。", "lorem ipsum dolor sit amet",
         "tabs\tand\nnewlines", "&amp; &#65; &NotEqualTilde; &unknownxyz; &#x4E00;",
         "combining  é unicode", "全角、句読点。カタカナひらがな",
         "1234567890", "verylongword" * 5, " ", "\n", "日本語のみの文",
         "E=mc^2 <not-a-tag> {lbrace}", "県"]

SCRIPTS = [
    "console.log('hello');",
    "document.title = 'changed'; console.log(document.title);",
    "var e = document.getElementById('x'); if (e) e.textContent = 'mutated';",
    "console.log(document.getElementsByTagName('p').length);",
    "throw new Error('boom');",
    "while(1){}",  # 命令バジェット枯渇（kill される）
    "var broken = ;",  # 構文エラー
    "console.log(document.querySelector('div').tagName);",
    "document.body.textContent = 'wiped';",
    "console.log(1 + 1); console.log('a' + 'b');",
    "var el = document.getElementById('x');",
    "",
]
STYLES = [
    "p { color: red; }", "div.x { background: #abc; }", "#y { border: 1px solid; }",
    "* { font-size: 2px; }", "p { display: none !important; }",
    "li { margin: 2em; padding-left: 3px; }",
    "a[href] { color: blue; }", "p, div { text-align: right; }",
    "span { white-space: pre; }", "@media print { p { color: red; } }",
    "h1,h2,h3 { background: rgb(1,2,3); }", "p { margin: 0 0 0 0; }",
]
DOCTYPES = ["<!doctype html>", "<!DOCTYPE HTML>", "", "<!doctype html SYSTEM 'x'>",
            "<!doctype foo>", "<!--boot--><!doctype html>"]

def text(rng):
    t = rng.choice(TEXTS)
    if rng.random() < 0.3:
        t += rng.choice(TEXTS)
    return t

def attrs(rng, tag):
    out = []
    if tag == "a":
        if rng.random() < 0.9:
            out.append(('href', rng.choice(["https://example.com/", "/rel", "p?q=1&r=2",
                                            "", "https://www.whatwg.org/", "#frag",
                                            "http://長い.example/" + "a" * 200])))
        if rng.random() < 0.2:
            out.append(('name', 'anchor'))
    if tag == "img":
        out.append(('src', 'x.png'))
        if rng.random() < 0.8:
            out.append(('alt', rng.choice(["あると", "alt text", "日", ""])))
    if tag in ("div", "p", "span", "li", "section") and rng.random() < 0.3:
        out.append(('class', rng.choice(["x", "x y", "cls-hy phen"])))
    if rng.random() < 0.15:
        out.append(('id', rng.choice(["x", "y", "z9"])))
    if rng.random() < 0.2:
        out.append(('style', rng.choice(["display:none", "color:red", "text-align:right",
                                          "background:#123456; margin:2px", ""])))
    if tag == "annotation-xml" and rng.random() < 0.8:
        out.append(('encoding', rng.choice(["text/html", "application/xhtml+xml",
                                            "Text/htmL", "x"])))
    if tag == "script" and rng.random() < 0.2:
        out.append(('type', rng.choice(["text/javascript", "module", "text/x"])))
    return "".join(' %s="%s"' % (k, v) for k, v in out)

EMPTY_OK = {"br", "hr", "img", "input"}

def gen_node(rng, depth):
    r = rng.random()
    if r < 0.35 or depth > 5:
        return text(rng)
    tag = rng.choice(TAGS)
    if tag in EMPTY_OK:
        return "<%s%s>" % (tag, attrs(rng, tag))
    if tag == "script":
        return "<script%s>%s</script>" % (attrs(rng, tag), rng.choice(SCRIPTS))
    if tag == "style":
        return "<style>%s</style>" % rng.choice(STYLES)
    if tag == "title":
        return "<title>%s</title>" % text(rng)
    if tag == "textarea":
        return "<textarea>%s</textarea>" % text(rng)
    inner = "".join(gen_node(rng, depth + 1) for _ in range(rng.randint(0, 3)))
    if rng.random() < 0.1:  # 閉じ忘れ（エラー注入）
        return "<%s%s>%s" % (tag, attrs(rng, tag), inner)
    if rng.random() < 0.05:  # 自己終了
        return "<%s%s/>" % (tag, attrs(rng, tag))
    return "<%s%s>%s</%s>" % (tag, attrs(rng, tag), inner, tag)

def gen_html(rng):
    parts = [rng.choice(DOCTYPES)] if rng.random() < 0.8 else []
    if rng.random() < 0.7:
        parts.append("<html>")
        if rng.random() < 0.7:
            head = ""
            if rng.random() < 0.5:
                head += "<title>%s</title>" % text(rng)
            if rng.random() < 0.5:
                head += "<style>%s</style>" % "\n".join(
                    rng.choice(STYLES) for _ in range(rng.randint(0, 3)))
            if rng.random() < 0.3:
                head += "<script>%s</script>" % rng.choice(SCRIPTS)
            parts.append("<head>%s</head>" % head)
        body = "".join(gen_node(rng, 0) for _ in range(rng.randint(1, 7)))
        parts.append("<body>%s</body>" % body)
        if rng.random() < 0.8:
            parts.append("</html>")
    else:  # 断片・裸の断片
        for _ in range(rng.randint(1, 6)):
            parts.append(gen_node(rng, 0))
        if rng.random() < 0.5:
            parts.insert(0, body_frag(rng))
    if rng.random() < 0.15:
        parts.insert(rng.randrange(0, len(parts) + 1), "<!-- comment %s -->" % text(rng))
    if rng.random() < 0.05:
        parts.insert(rng.randrange(0, len(parts) + 1), "<?pi-target data?>")
    return "".join(parts)

def body_frag(rng):
    return rng.choice(["<p>", "<table>", "<div>", ""])

MD_BLOCKS = [
    "## 見出し {n}", "### サブ {n}", "# 大見出し",
    "段落テキスト。**太字** と *斜体* と `code` を混在。日本語も入れる。",
    "| a | b | c |\n|---|---|---|\n| 1 | 2 | 3 |\n| d | e | f |",
    "```c\nint main(void) { return 0; }\n```",
    "- 項目a\n- 項目b\n  - ネスト", "1. 第一\n2. 第二",
    "> 引用文。\n> 二行目。",
    "[リンク](https://example.com/) と ![画像](img.png)。",
    "脚注参照[^f] がある。\n\n[^f]: 脚注本文。",
    "長い段落。" + "あ" * 60,
    "ハードブレーク  \n次の行",
]

def gen_md(rng):
    return "\n\n".join(rng.choice(MD_BLOCKS) for _ in range(rng.randint(1, 10)))

def maybe_sjis(rng, data: bytes) -> bytes:
    """日本語を含む UTF-8 文書を SJIS/EUC-JP 化（meta charset 付きにする）。"""
    r = rng.random()
    if r < 0.06:
        body = data.decode("utf-8", "ignore")
        for enc, label in (("cp932", "shift_jis"), ("euc-jp", "euc-jp")):
            if r < 0.03 and enc != "cp932":
                continue
            head = '<meta http-equiv="content-type" content="text/html; charset=%s">' % label
            try:
                return (head + body).encode(enc, "ignore")
            except Exception:
                return data
        try:
            return ('<meta charset="shift_jis">' + body).encode("cp932", "ignore")
        except Exception:
            return data
    return data

# ---------------- フラグ行列 ----------------

def flag_sets(rng):
    sets = [
        [], ["--no-ansi"], ["--dump-dom"], ["--dump-layout"], ["--dump-styles"],
        ["--dump-wptdom"], ["--dump-tokens"], ["--links"], ["--links", "--no-ansi"],
        ["--no-style", "--no-ansi"], ["--no-style", "--dump-dom"],
        ["--no-style", "--dump-layout"], ["--no-style", "--dump-styles"],
        ["--slim-dom"], ["--slim-dom", "--dump-dom"], ["--slim-dom", "--dump-wptdom"],
        ["--slim-dom", "--dump-layout"], ["--md"], ["--slim-dom", "--no-style"],
        ["--width", str(rng.randint(4, 250)), "--no-ansi"],
        ["--width", str(rng.randint(4, 250)), "--dump-layout"],
        ["--fragment", "body", "--dump-dom"],
        ["--fragment", rng.choice(["div", "title", "svg", "svg path", "select", "table"]),
         rng.choice(["--dump-dom", "--dump-wptdom"])],
        ["--stats", "--no-ansi"], ["--stats", "--md"],
        ["--dump-dom", "--no-style"],
    ]
    return rng.choice(sets)

STATS_SCRUB = re.compile(
    "([-]?\\d+\\.\\d\\dms|resid_rss_kb=[-0-9]+|前後差 [+-]\\d+|peak_rss_kb=\\d+|"
    "script_ms=[0-9.]+)".encode())

def scrub_stats(b: bytes) -> bytes:
    """--stats 出力の計測値（タイミング/RSS/arena 会計）をワイルドカード化。
    nodes/parse_errors/grid/links 等の決定的値はそのまま比較に残る。
    arena_kb/arena_used_kb は C 概念のため両辺で行ごと除去する。"""
    lines = b.split(b"\n")
    keep = []
    for ln in lines:
        if b"arena_kb" in ln or b"arena_used_kb" in ln or ln.startswith(b"  note:"):
            continue
        keep.append(STATS_SCRUB.sub(b"#", ln))
    return b"\n".join(keep)

def run(binary, args, stdin_data=None, env_extra=None, timeout=15):
    env = dict(os.environ)
    env.pop("IFUTO_MD_SLOW", None)
    if env_extra:
        env.update(env_extra)
    p = subprocess.run([binary] + args, input=stdin_data, capture_output=True,
                       timeout=timeout, env=env)
    return p.returncode, p.stdout, p.stderr

def compare(label, c, r, is_stats):
    crc, cout, cerr = c
    rrc, rout, rerr = r
    if is_stats:
        cerr, rerr = scrub_stats(cerr), scrub_stats(rerr)
    if crc != rrc or cout != rout or cerr != rerr:
        print("=== MISMATCH [%s] (rc %d vs %d)" % (label, crc, rrc))
        if cout != rout:
            print("--- C stdout (%d B):" % len(cout), cout[:400])
            print("--- R stdout (%d B):" % len(rout), rout[:400])
            for i, (a, b) in enumerate(zip(cout, rout)):
                if a != b:
                    print("    first stdout diff at byte %d: C=%r R=%r" % (i, cout[max(0,i-60):i+60], rout[max(0,i-60):i+60]))
                    break
        if cerr != rerr:
            print("--- C stderr:", cerr[:400])
            print("--- R stderr:", rerr[:400])
        return False
    return True

def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 20000
    seed = 0
    for i, a in enumerate(sys.argv):
        if a == "--seed" and i + 1 < len(sys.argv):
            seed = int(sys.argv[i + 1])
    rng = random.Random(seed if seed else 20260824)
    tmp = tempfile.mkdtemp(prefix="ifuto-difffuzz-")
    bad = 0
    stats_cases = 0
    env_base = {"IFUTO_HOME": os.path.join(tmp, "ifuto-home"),
                "XDG_DATA_HOME": "", "IF_SCRIPT": "1"}
    os.makedirs(env_base["IFUTO_HOME"], exist_ok=True)

    for case in range(n):
        is_md = rng.random() < 0.35
        doc = gen_md(rng) if is_md else gen_html(rng)
        data = doc.encode("utf-8")
        if not is_md:
            data = maybe_sjis(rng, data)
        ext = ".md" if (is_md and rng.random() < 0.6) else rng.choice(
            [".html", ".txt", ""])
        path = os.path.join(tmp, "doc%s" % ext)
        with open(path, "wb") as f:
            f.write(data)
        flags = flag_sets(rng)
        is_stats = "--stats" in flags
        stats_cases += is_stats
        use_stdin = rng.random() < 0.1
        env = dict(env_base)
        if rng.random() < 0.05:
            env["IF_SCRIPT"] = "0"
        args = flags + (["-"] if use_stdin else [path])
        try:
            c = run(C_BIN, args, data if use_stdin else None, env)
        except subprocess.TimeoutExpired:
            print("=== C TIMEOUT case %d flags=%s" % (case, flags)); bad += 1
            if bad > 10: break
            continue
        try:
            r = run(R_BIN, args, data if use_stdin else None, env)
        except subprocess.TimeoutExpired:
            print("=== R TIMEOUT case %d flags=%s doc=%r" % (case, flags, data[:200]))
            bad += 1
            if bad > 10: break
            continue
        label = "case %d flags=%s%s doc=%r" % (
            case, flags, " stdin" if use_stdin else "", data[:120])
        if not compare(label, c, r, is_stats):
            bad += 1
            keep = os.path.join(tmp, "keep-%d%s" % (case, ext.replace(".", "_") or "_none"))
            with open(keep, "wb") as f:
                f.write(data)
            print("    kept: %s" % keep)
            if bad > 25:
                break

    # エラー系・特殊系の決定的ケース
    edge = [
        (["--help"], None, {}), (["-h"], None, {}), (["--unknown-flag"], None, {}),
        (["--width"], None, {}),            # 値欠落
        (["--width", "3", "x"], None, {}),  # 幅下限
        (["--width", "100001", "x"], None, {}),
        (["--width", "abc", "x"], None, {}),
        (["--width", "50x", "x"], None, {}),  # atoi prefix
        (["/nonexistent/path.html"], None, {}),
        (["--dump-dom", "/nonexistent"], None, {}),
        (["--fragment", "body", "--no-ansi", path], None, {}),  # fragment ガード
        (["--show-paths"], None, {"IFUTO_HOME": os.path.join(tmp, "nope")}),
        (["--show-paths"], None, {"IFUTO_NO_STORE": "1"}),
        (["--imgdecode", "/nonexistent.png"], None, {}),
        (["--imgdecode", path], None, {}),
    ]
    for args, stdin, ex in edge:
        env = dict(env_base)
        env.update(ex)
        try:
            c = run(C_BIN, args, None, env)
            r = run(R_BIN, args, None, env)
        except subprocess.TimeoutExpired:
            print("=== TIMEOUT edge %r" % (args,)); bad += 1; continue
        if not compare("edge %r %s" % (args, ex), c, r, "--stats" in args):
            bad += 1

    # GUI 系は「Rust 側未移植」を**意図した偏差**（嘘をつかない: 明示メッセージ +
    # rc 2 で明白に拒否する）として件数を数えるだけにする。
    # ただし裸の --shot（値欠落）は C も Rust も usage+rc=2 のため、形状検査ではなく
    # byte 突合にする（拒否メッセージ経路に到達しないのが両実装の正しい姿）。
    for args in (["--gui"], ["--ui"], ["--shot", "/tmp/o.ppm"], ["--shot"]):
        try:
            rc, _, err = run(R_BIN, args, None, env_base)
        except subprocess.TimeoutExpired:
            print("=== R TIMEOUT gui-edge %r" % (args,)); bad += 1; continue
        if args == ["--shot"]:
            c = run(C_BIN, args, None, env_base)
            if not compare("gui-edge %r byte-diff" % (args,), c, run(R_BIN, args, None, env_base), False):
                bad += 1
            continue
        if rc != 2 or b"GUI" not in err or b"Rust" not in err:
            print("=== GUI 拒否の形が壊れている: %r rc=%d err=%r" % (args, rc, err[:80]))
            bad += 1
        # legacy --ui は誘導行を必ず出す
        if args == ["--ui"] and not err.startswith(b"ifuto: --ui(TUI)"):
            print("=== --ui 誘導行が壊れている: %r" % (err[:80],))
            bad += 1

    print("diff-fuzz cli: %d cases (%d stats) + %d edge + 4 gui-edge -> %d mismatch" %
          (n, stats_cases, len(edge), bad))
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()
