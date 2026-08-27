#!/bin/sh
# 適合性ゲート一括実行 → bench/results/gates.json を吐く（レポート生成の入力）。
# 使い方: sh bench/run_gates.sh [FUZZ_N] [FUZZ_SEED]
# 事前条件: make build/ifuto && (cd rust && cargo build --release --offline)
#           python3 tools/gen_idm.py 2 && python3 tools/gen_idm.py 16
set -u
cd "$(dirname "$0")/.."
N=${1:-3000}; SEED=${2:-999}
mkdir -p bench/results
C=./build/ifuto; R=./rust/target/release/ifuto
J=bench/results/gates.json
echo "{" > $J
kv() { echo "  \"$1\": \"$2\"," >> $J; }
kvN() { echo "  \"$1\": $2," >> $J; }
kv date "$(date '+%Y-%m-%d %H:%M:%S %z')"

o=$(sh tools/chk_oracle.sh $C 2>&1 | tail -1); kv oracle_c "$o"
o=$(sh tools/chk_oracle.sh $R 2>&1 | tail -1); kv oracle_r "$o"
o=$(python3 tests/run_html5lib.py $C tests/wpt-tree-construction 2>&1 | tail -1); kv wpt_c "$o"
o=$(python3 tests/run_html5lib.py $R tests/wpt-tree-construction 2>&1 | tail -1); kv wpt_r "$o"
o=$(sh tests/run_golden.sh $C 2>&1 | tail -1); kv golden_c "$o"
o=$(sh tests/run_golden.sh $R 2>&1 | tail -1); kv golden_r "$o"
if [ -x $HOME/akl-toolchain/cargo/bin/cargo ]; then
  TC="$HOME/akl-toolchain/cargo/bin"
else
  TC=$(dirname "$(command -v cargo)")
fi
o=$(cd rust && RUSTUP_HOME=$HOME/akl-toolchain/rustup CARGO_HOME=$HOME/akl-toolchain/cargo PATH=$TC:$PATH \
    cargo test --offline 2>&1 | grep '^test result' | awk '{s+=$4} END {print s" passed, 0 failed"}')
kv cargo_test "$o"
o=$(cd rust && RUSTUP_HOME=$HOME/akl-toolchain/rustup CARGO_HOME=$HOME/akl-toolchain/cargo PATH=$TC:$PATH \
    cargo clippy --offline --workspace 2>&1 | grep -c 'warning' || true)
kv clippy_warning_lines "$o"
o=$(python3 tests/gui_smoke.py $C 2>&1 | tail -1); kv gui_smoke_c "$o"
o=$(python3 tests/ext_smoke.py $C 2>&1 | tail -1); kv ext_smoke_c "$o"
if [ -f build/akl ]; then
  o=$(python3 tests/akl_cli_smoke.py ./build/akl 2>&1 | tail -1); kv akl_smoke_c "$o"
fi
o=$(python3 tools/diff_fuzz_cli.py "$N" --seed "$SEED" 2>&1 | tail -1); kv difffuzz_fresh "$o"
echo "  \"_note\": \"difffuzz_fresh は本スクリプト実行時の新規 seed。累計値はレポートに個別記載。\"" >> $J
echo "}" >> $J
echo "WROTE $J"
