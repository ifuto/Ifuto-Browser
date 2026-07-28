#!/bin/sh
# ゴールデンテスト: tests/golden/*.html の出力を *.expected と厳密比較する。
# 使い方: run_golden.sh <ifuto バイナリ>
set -u
BIN="${1:?usage: run_golden.sh BIN}"
fail=0; total=0
for html in tests/golden/*.html; do
    base="${html%.html}"
    exp="$base.expected"
    [ -f "$exp" ] || continue
    total=$((total+1))
    if "$BIN" --no-ansi --width 40 "$html" | diff -u "$exp" - > /tmp/ifuto_golden_diff.$$ 2>&1; then
        echo "PASS $base"
    else
        echo "FAIL $base"; cat /tmp/ifuto_golden_diff.$$
        fail=$((fail+1))
    fi
    rm -f /tmp/ifuto_golden_diff.$$
done
echo "golden: $total run, $fail failed"
[ "$fail" -eq 0 ]
