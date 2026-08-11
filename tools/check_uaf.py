#!/usr/bin/env python3
"""AKL の UAF（use-after-free）機械検出ツール。

対象: src/akl/akl.c
検出: `AklObj *NAME = &rt->objs[...]` で取得した一時ポインタを保持したまま、
obj 配列を realloc し得る関数（akl_obj_new / akl_mkstr / akl_intern /
akl_to_string / akl_mkarray / akl_mkobject / akl_mkstring / akl_promise_make /
akl_map_make / akl_set_make / akl_gc / akl_vm_frame_hidden / akl_mkhandle）を
呼び、その後に NAME-> を使用している箇所を警告する。

再取得（`NAME = &rt->objs[...]`）が realloc 呼び出しと使用の間にある場合は安全と
判定する（使用直前再取得が修正規約 — docs/SECURITY.md）。

使い方:
  python3 tools/check_uaf.py [src/akl/akl.c]
  警告があれば exit 1（make ゲート用）。

偽陽性の扱い: 関数呼び出しをまたぐ解析はしない（単純保守）。警告が出たら
「本当に realloc を挟んでいないか / 再取得を足すべきか」を人間が確認する。
"""
import re
import sys

REALLOC_FNS = [
    "akl_obj_new", "akl_mkstr", "akl_intern", "akl_to_string",
    "akl_mkarray", "akl_mkobject", "akl_mkstring", "akl_promise_make",
    "akl_map_make", "akl_set_make", "akl_gc", "akl_vm_frame_hidden",
    "akl_mkhandle",
]


def strip_comments(src: str) -> str:
    """C のコメントと文字列リテラルを空白に置換（位置は保持）。"""
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            j = src.find('*/', i + 2)
            if j < 0:
                j = n
            # 改行は保持（行番号の整合）。それ以外は空白
            seg = src[i:j + 2]
            out.append(''.join(ch if ch == '\n' else ' ' for ch in seg))
            i = j + 2
        elif c == '/' and i + 1 < n and src[i + 1] == '/':
            j = src.find('\n', i + 2)
            if j < 0:
                j = n
            out.append(' ' * (j - i))
            i = j
        elif c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                if src[j] == '\\':
                    j += 2
                else:
                    j += 1
            if j >= n:
                j = n - 1
            seg = src[i:j + 1]
            out.append(''.join(ch if ch == '\n' else ' ' for ch in seg))
            i = j + 1
        elif c == "'":
            j = i + 1
            while j < n and src[j] != "'":
                if src[j] == '\\':
                    j += 2
                else:
                    j += 1
            if j >= n:
                j = n - 1
            seg = src[i:j + 1]
            out.append(''.join(ch if ch == '\n' else ' ' for ch in seg))
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def find_funcs(src: str):
    """中括弧バランスでトップレベル関数を分割。[(名前, 開始行, コード)]"""
    funcs = []
    i = 0
    n = len(src)
    # トップレベルの関数定義開始: 行頭で "static ... (" または "AklVal akl_..." 等
    # 行頭の型名で始まり、対応する { までに '(' を含む行
    lines = src.split('\n')
    starts = []
    for idx, ln in enumerate(lines):
        s = ln.strip()
        if not s or s.endswith(';'):
            continue  # プロトタイプ・変数宣言は除外
        # 型名 関数名( ... で始まる関数定義（'(' を含み ';' で終わらない）
        if re.match(r'^(static |AKL_COLDFN )?(bool|void|u32|i32|u64|double|AklVal|const u8 \*|char \*|AklModule \*)\s+(\w+)\s*\(', s):
            starts.append(idx)
    for k, sline in enumerate(starts):
        # その行から '{' を見つけ、対応する '}' まで
        depth = 0
        start_idx = None
        for i2 in range(sline, len(lines)):
            seg = lines[i2]
            for ch in seg:
                if ch == '{':
                    if depth == 0:
                        start_idx = i2
                    depth += 1
                elif ch == '}':
                    depth -= 1
                    if depth == 0 and start_idx is not None:
                        end_idx = i2
                        break
            if depth == 0 and start_idx is not None:
                break
        if start_idx is None:
            continue
        code = '\n'.join(lines[sline:end_idx + 1])
        funcs.append((lines[sline].strip()[:70], sline, code))
    return funcs


def analyze_code(name: str, code: str, base_line: int, warnings: list, depth: int):
    """1 関数内を解析。vm_exec はハンドラ単位で再帰分割する。"""
    # vm_exec のハンドラ分割: AKL_L(NAME): { ... 次の AKL_L( まで
    if depth < 2 and 'AKL_L(' in code:
        # ハンドラ開始位置
        for m in re.finditer(r'AKL_L\((\w+)\):\s*\{', code):
            seg_start = m.start()
            seg_end = code.find('\n    AKL_L(', seg_start + 10)
            if seg_end < 0:
                seg_end = len(code)
            hcode = code[seg_start:seg_end]
            hline = base_line + code[:seg_start].count('\n')
            analyze_code(f"{name} / {m.group(1)}", hcode, hline, warnings, depth + 1)
        return

    # ポインタ宣言
    for m in re.finditer(r'AklObj \*(\w+) = &rt->objs\[', code):
        var = m.group(1)
        decl_end = m.end()
        # 宣言より後の realloc 呼び出し
        for r in re.finditer(r'\b(' + '|'.join(REALLOC_FNS) + r')\s*\(', code[decl_end:]):
            rpos = decl_end + r.start()
            rfn = r.group(1)
            # return 文内の realloc はスキップ（return で関数が終了し、後続の var-> は
            # 到達不能 or 別ブランチ。引数内の var-> は realloc より前に評価される）
            # 判定は「直前の ';' または '{' から rpos までの間に return があるか」
            seg_start = max(code.rfind(';', 0, rpos), code.rfind('{', 0, rpos))
            if 'return' in code[seg_start:rpos]:
                continue
            # 到達不能判定: realloc の後、最初の AKL_NEXT()（VM ハンドラの無条件終了）までに
            # var-> が無ければ、その後の var-> は到達不能 → スキップ。
            # return は term にしない（条件付き return の後も到達し得るため — 保守側）。
            term = code.find('AKL_NEXT()', rpos)
            if term >= 0 and not re.search(r'\b' + re.escape(var) + r'->', code[rpos:term]):
                continue  # AKL_NEXT までに使用なし → 後続は到達不能
            # 引数内の使用（realloc より前に評価される）はスキップ:
            # rpos の関数呼び出しの対応する ')' までにある var-> は引数。
            # ただしループ本体ではスキップしない: 2 回目以降の反復では前回の realloc が
            # 済んでいるため引数内の var-> も危険（join 型 UAF — 実バグで検証済み）。
            paren_end = rpos
            dp = 0
            i = rpos
            while i < len(code):
                if code[i] == '(':
                    dp += 1
                elif code[i] == ')':
                    dp -= 1
                    if dp == 0:
                        paren_end = i
                        break
                i += 1
            open_br = code.rfind('{', 0, rpos)
            prev_txt = code[max(0, open_br - 60):open_br] if open_br >= 0 else ''
            in_loop = bool(re.search(r'\b(for|while)\s*\([^)]*\)\s*$', prev_txt) or
                        prev_txt.rstrip().endswith('do'))
            # その realloc より後の var-> 使用
            for u in re.finditer(r'\b' + re.escape(var) + r'->', code[rpos:]):
                upos = rpos + u.start()
                if upos <= paren_end and not in_loop:
                    continue  # 引数内（呼び出し前に評価。安全）
                between = code[rpos:upos]
                if re.search(r'\b' + re.escape(var) + r'\s*=\s*&rt->objs\[', between):
                    continue  # 使用直前再取得（安全）
                line_no = base_line + code[:rpos].count('\n') + 1
                warnings.append((line_no, name, var, rfn))
                break  # この realloc は警告済み


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'src/akl/akl.c'
    src = open(path, 'r', encoding='utf-8').read()
    stripped = strip_comments(src)
    funcs = find_funcs(stripped)
    warnings = []
    for name, sline, code in funcs:
        analyze_code(name, code, sline, warnings, 0)
    if warnings:
        seen = set()
        print(f"UAF candidates in {path}: {len(warnings)}")
        for line_no, fn, var, rfn in sorted(warnings):
            key = (line_no, var, rfn)
            if key in seen:
                continue
            seen.add(key)
            print(f"  {path}:{line_no}: {fn}: `{var}` used after {rfn}() without re-acquire")
        return 1
    print(f"check_uaf: no UAF candidates in {path}")
    return 0


SELF_TEST = r"""
// 検出されるべき（危険）: ループ内で realloc 後に o-> 使用
static void bad_loop(AklRT *rt, u32 ai) {
    AklObj *o = &rt->objs[ai];
    for (u32 i = 0; i < 10; i++) {
        u32 si = akl_to_string(rt, o->u.arr.v[i]);  // UAF: 前の反復で realloc
        (void)si;
    }
}
// 検出されるべき（危険）: realloc 後に別文で o-> 使用
static void bad_seq(AklRT *rt, u32 ai) {
    AklObj *o = &rt->objs[ai];
    u32 si = akl_to_string(rt, o->u.arr.v[0]);
    (void)si;
    (void)o->u.arr.n;  // UAF
}
// 許容: 使用直前再取得
static void ok_reacquire(AklRT *rt, u32 ai) {
    u32 ai0 = ai;
    AklObj *o = &rt->objs[ai0];
    for (u32 i = 0; i < 10; i++) {
        u32 si = akl_to_string(rt, rt->objs[ai0].u.arr.v[i]);
        (void)si;
    }
}
// 許容: return 文内 realloc
static u32 ok_return(AklRT *rt, u32 ai) {
    AklObj *o = &rt->objs[ai];
    if (o->kind == 1) return akl_mkstr(rt, (const u8 *)"x", 1);
    return akl_mkstr(rt, (const u8 *)"y", 1);
}
// 許容: AKL_NEXT で分岐終了（VM ハンドラ）
static void ok_next(AklRT *rt, u32 ai) {
    AklObj *o = &rt->objs[ai];
    if (o->kind == 1) {
        u32 si = akl_to_string(rt, rt->objs[ai].u.arr.v[0]);
        (void)si;
        AKL_NEXT();
    }
    if (o->kind == 2) { (void)o->u.arr.n; }
}
// 許容: 引数内使用（呼び出し前に評価）
static void ok_arg(AklRT *rt, u32 ai) {
    AklObj *o = &rt->objs[ai];
    (void)akl_vm_frame_hidden(rt, NULL, 0, 0, NULL, o->thisv, o->env);
}
"""


def self_test():
    import tempfile
    with tempfile.NamedTemporaryFile('w', suffix='.c', delete=False) as f:
        f.write(SELF_TEST)
        path = f.name
    import subprocess
    r = subprocess.run([sys.executable, __file__, path], capture_output=True, text=True)
    out = r.stdout + r.stderr
    ok = 'bad_loop' in out and 'bad_seq' in out
    ok = ok and 'ok_reacquire' not in out and 'ok_return' not in out
    ok = ok and 'ok_next' not in out and 'ok_arg' not in out
    import os
    os.unlink(path)
    if not ok:
        print("self-test FAILED:")
        print(out)
        return 1
    print("check_uaf self-test: PASS (2 detected, 4 allowed)")
    return 0


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--self-test':
        sys.exit(self_test())
    sys.exit(main())
