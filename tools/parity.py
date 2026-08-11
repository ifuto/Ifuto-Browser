#!/usr/bin/env python3
"""AKL vs V8 (node) 差分テスト: スニペットを両方で実行し出力を比較する。"""
import subprocess, sys, os, json

AKL = os.environ.get('AKL_BIN', '/home/user/Ifuto-Browser/build/akl')
NODE = 'node'

SNIPPETS = [
    # --- ホイスティング ---
    "f(); function f() { return 42; }",
    "var x; x",
    "x; var x = 1;",
    # --- スコープ ---
    "{ let a = 1; } a;",
    "{ const a = 1; } a;",
    "let a = 1; { let a = 2; } a;",
    # --- Date ---
    "Date.now() > 0",
    "typeof Date",
    "new Date(0).getTime()",
    "new Date(86400000).toISOString()",
    # --- Error ---
    "typeof Error",
    "try { throw new Error('boom'); } catch (e) { e.name + ':' + e.message }",
    "try { JSON.parse('{') } catch (e) { e.name }",
    # --- call/apply/bind ---
    "function f(a, b) { return this.v + a + b; } f.call({v: 1}, 2, 3)",
    "function f(a, b) { return this.v + a + b; } f.apply({v: 1}, [2, 3])",
    "function f(a) { return this.v + a; } var g = f.bind({v: 10}, 5); g()",
    # --- 数値/文字列/配列メソッド ---
    "Number.parseInt('42')",
    "Number.isInteger(4.5)",
    "String.fromCharCode(65, 66)",
    "'abc'.repeat(3)",
    "' a b '.trim()",
    "'a,b,c'.split(',').join('-')",
    "'abc'.at(-1)",
    "[1, 2, 3].at(-1)",
    "[1, 2, 3].includes(2)",
    "[3, 1, 2].sort().join(',')",
    "[1, 2, 3].reduce((a, b) => a + b)",
    "Array.of(1, 2, 3).length",
    # --- globalThis / eval ---
    "typeof globalThis",
    "eval('1 + 2')",
    # --- typeof / 演算子 ---
    "typeof Symbol",
    "typeof Proxy",
    "0.1 + 0.2",
    "'5' - 2",
    "[] + []",
    "{} + []",
    # --- Promise マイクロタスク ---
    "var x = 0; Promise.resolve().then(function() { x = 1; }); x",
    # --- ラッパー / プロトタイプ ---
    "new String('abc').length",
    "Object.getPrototypeOf({}) === Object.prototype",
    "'abc'.toString()",
    # --- 分割代入/スプレッド ---
    "var [a, ...rest] = [1, 2, 3]; rest.join(',')",
    "var o = {x: 1, y: 2}; var {x, y} = o; x + y",
    # --- クラス ---
    "class A { constructor() { this.x = 1; } } class B extends A {} new B().x",
    "class C { static s() { return 7; } } C.s()",
    # --- テンプレート ---
    "var n = 3; `v${n + 1}`",
    # --- 型変換 ---
    "typeof null",
    "typeof undefined",
    "typeof NaN",
    "typeof function(){}",
    "[1, 2] + ''",
    "(1).toString()",
    # --- 演算子のエッジ ---
    "1 === '1'",
    "null == undefined",
    "NaN === NaN",
    "0 === -0",
    "1 / 0",
    "'b' + 'a' + +'a' + 'a'",
    # --- ループ/制御 ---
    "for (var i = 0; i < 3; i++) { if (i === 1) continue; } i",
    "var s = ''; for (var k in {a: 1, b: 2}) s += k; s",
    "var s = ''; for (var v of [1, 2, 3]) s += v; s",
    # --- 例外 ---
    "try { throw 'x'; } catch (e) { e }",
    "try { throw new RangeError('r'); } catch (e) { e.name }",
    # --- その他組込 ---
    "Math.max(1, 2, 3)",
    "Math.floor(-1.5)",
    "JSON.stringify({a: 1, b: [1, 2]})",
    "'abc'.charCodeAt(1)",
    "'abc'.indexOf('b')",
    "'a-b-c'.replace('b', 'X')",
    "'a1b2c3'.replace(/[0-9]/g, '#')",
]

def run(cmd, src):
    try:
        r = subprocess.run(cmd, input=src, capture_output=True, text=True, timeout=20)
        return r.returncode, r.stdout.strip(), r.stderr.strip()
    except subprocess.TimeoutExpired:
        return 'TIMEOUT', '', ''

def run_akl(src):
    import tempfile
    with tempfile.NamedTemporaryFile('w', suffix='.js', delete=False) as f:
        f.write(src + '\n')
        path = f.name
    try:
        r = subprocess.run([AKL, '--no-sandbox', path], capture_output=True, text=True, timeout=20)
        out = r.stdout.strip()
        # EVAL_MS / maxrss 行は除去
        out = '\n'.join(l for l in out.split('\n') if not l.startswith('EVAL_MS') and not l.startswith('maxrss'))
        return r.returncode, out, r.stderr.strip()
    except subprocess.TimeoutExpired:
        return 'TIMEOUT', '', ''
    finally:
        os.unlink(path)

def main():
    if not os.path.exists(AKL):
        print(f"akl binary not found: {AKL} (run: make build/akl)")
        return 2
    mism = []
    for src in SNIPPETS:
        rc1, out1, err1 = run_akl(src)
        # AKL は最終式の値を stdout に出す。node は console.log(eval(...)) で包む
        nsrc = 'console.log(eval(' + json.dumps(src) + '))'
        rc2, out2, err2 = run([NODE, '-e', nsrc], '')
        ok = (rc1 == rc2) and (out1 == out2)
        if not ok:
            mism.append((src, rc1, out1, rc2, out2))
    print(f"parity: {len(SNIPPETS) - len(mism)}/{len(SNIPPETS)} match")
    for src, rc1, out1, rc2, out2 in mism:
        print(f"  MISMATCH: {src!r}")
        print(f"    akl:  rc={rc1} out={out1!r}")
        print(f"    node: rc={rc2} out={out2!r}")
    return 1 if mism else 0

if __name__ == '__main__':
    sys.exit(main())
