#!/bin/sh
# Ifuto ベンチ: バイナリサイズ / 起動時間 / ページ時間 / MaxRSS を測定して表にする。
# 数値は「このマシン・この測定法での実測」。他環境との比較は厳禁。
set -u
BIN="${1:?usage: bench.sh BIN}"
which time >/dev/null 2>&1 || true

echo "== binary size =="
BYTES=$(wc -c < "$BIN")
echo "binary_bytes: $BYTES"
SIZE_OUT=$(size "$BIN" 2>/dev/null | tail -1)
echo "size(text data bss): $SIZE_OUT"

echo "== cold start (empty doc, 200 runs) =="
echo -n > /tmp/ifuto_empty.html
S=$(date +%s%N)
i=0; while [ $i -lt 200 ]; do "$BIN" --no-ansi /tmp/ifuto_empty.html > /dev/null; i=$((i+1)); done
E=$(date +%s%N)
echo "startup_avg_ms: $(( (E - S) / 200000000 )).$(( ((E - S) / 2000000) % 100 ))  (process spawn 込み)"

mkdir -p bench
# 合成文書: 小（実文書相当）と大（2MB, 5000 段落 + CSS）
python3 - <<'PY'
import random
random.seed(42)
words = "the quick brown fox jumps over lazy dog 日本語のテキストも混ぜる 全力で行く".split()
with open("bench/small.html", "w") as f:
    f.write("<style>p{margin:2px} .x{color:#123456}</style><h1>Bench</h1>")
    for i in range(60):
        f.write(f"<p class=x><b>{i}</b> " + " ".join(random.choices(words, k=30)) + "</p>")
with open("bench/big.html", "w") as f:
    f.write("<style>p{margin:1px} b{color:red} li{color:blue} div{padding:1px}</style>")
    for i in range(5000):
        tag = "li" if i % 7 == 0 else "p"
        f.write(f"<div><{tag}><b>{i}</b> " + " ".join(random.choices(words, k=40)) + f"</{tag}></div>")
PY

measure() {
    DOC="$1"; NAME="$2"
    BYTES_DOC=$(wc -c < "$DOC")
    # 時間: 20 run の wall 合計
    S=$(date +%s%N)
    i=0; while [ $i -lt 20 ]; do "$BIN" --no-ansi --width 120 "$DOC" > /dev/null; i=$((i+1)); done
    E=$(date +%s%N)
    MS=$(( (E - S) / 20000000 ))
    # RSS: ifuto の --stats 自己報告（/proc/self/status VmHWM を終了直前に読むので
    # 短命プロセスでも正確。環境の壊れた ru_maxrss に依存しない）
    STATS_OUT=$("$BIN" --no-ansi --width 120 --stats "$DOC" 2>&1 > /dev/null)
    RSS=$(printf '%s\n' "$STATS_OUT" | grep -o 'peak_rss_kb=[0-9]*' | cut -d= -f2)
    ARENA_KB=$(printf '%s\n' "$STATS_OUT" | grep -o 'arena_kb:.*render=[0-9.]*' | grep -o 'render=[0-9.]*' | cut -d= -f2)
    RSS=${RSS:-unavailable}; ARENA_KB=${ARENA_KB:-unavailable}
    echo "$NAME: doc_bytes=$BYTES_DOC avg_ms=$MS peak_rss_kb=$RSS arena_kb=$ARENA_KB"
}

echo "== page bench (avg of 20, width=120) =="
measure bench/small.html small
measure bench/big.html big
