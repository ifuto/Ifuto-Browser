# Ifuto Browser — build
# 設計上の固定費を避けるため、依存ライブラリはゼロ（libc のみ）。

CC      ?= cc
SRC     := $(wildcard src/*.c)
# 単一翻訳・LTO 一括ビルドのためヘッダは全て成員に列挙する
# （ヘッダ変更で再ビルドが走らず古いバイナリを信用する事故の防除）
ENGINE  := $(filter-out src/main.c,$(SRC))
# Akl（自作 JS エンジン, C11/JIT なし）: v0.0 は DOM 未接続のため本体 ifuto には
# リンクしない（200KB 天井維持。v0.4 統合時に実測で天井を再設定する — BENCH.md 台帳）。
AKLSRC  := $(wildcard src/akl/*.c)
WFLAGS  := -std=c11 -Wall -Wextra -Wshadow -Wstrict-prototypes -Wwrite-strings
BASE    := $(WFLAGS) -fno-strict-aliasing -fstack-protector-strong -D_FORTIFY_SOURCE=2
REL     := -O2 -DNDEBUG -flto -ffunction-sections -fdata-sections
LDFLAGS_REL := -flto -Wl,--gc-sections -s
SAN     := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer

BUILD   := build

all: $(BUILD)/ifuto

$(BUILD):
	mkdir -p $(BUILD)

# GUI は TUI 廃止（2026-08-01）で単一 UI。ifuto 本体に統合（--gui / --shot）。
GUISRC := $(wildcard src/gui/*.c)
HDRS    := $(wildcard src/*.h src/gui/*.h src/akl/*.h)
$(BUILD)/ifuto: $(SRC) $(GUISRC) $(HDRS) | $(BUILD)
	$(CC) $(BASE) $(REL) -o $@ $(SRC) $(GUISRC) $(LDFLAGS_REL) -lm

# 開発用: sanitizer 付きバイナリ（テスト・手動検証は常にこちらで）
$(BUILD)/ifuto-asan: $(SRC) $(GUISRC) $(HDRS) | $(BUILD)
	$(CC) $(BASE) $(SAN) -o $@ $(SRC) $(GUISRC) -lm

gui: $(BUILD)/ifuto

# ヘッドレス GUI 検証（X 不要: --shot のラスタパイプライン + 画素検査）
guismoke: $(BUILD)/ifuto
	python3 tests/gui_smoke.py ./$(BUILD)/ifuto

TESTSRC := $(wildcard tests/*.c)
$(BUILD)/run_tests: $(TESTSRC) $(ENGINE) $(AKLSRC) $(HDRS) | $(BUILD)
	$(CC) $(BASE) $(SAN) -Itests -o $@ $(TESTSRC) $(ENGINE) $(AKLSRC) -lm

# akl の switch dispatch 側も丸ごと走査する双子バイナリ（片側だけの不具合を封殺）
$(BUILD)/run_tests_switch: $(TESTSRC) $(ENGINE) $(AKLSRC) $(HDRS) | $(BUILD)
	$(CC) $(BASE) $(SAN) -DAKL_TEST_SWITCH_DISPATCH -Itests -o $@ $(TESTSRC) $(ENGINE) $(AKLSRC) -lm

$(BUILD)/fuzz_html: fuzz/fuzz_driver.c $(ENGINE) | $(BUILD)
	$(CC) $(BASE) $(SAN) -I src -o $@ fuzz/fuzz_driver.c $(ENGINE) -lm

$(BUILD)/fuzz_akl: fuzz/fuzz_akl.c $(AKLSRC) | $(BUILD)
	$(CC) $(BASE) $(SAN) -I src -o $@ fuzz/fuzz_akl.c $(AKLSRC) -lm

# リモート入力面（net.c の URL/resolve/head/dechunk）も同じ穴倉で打つ
$(BUILD)/fuzz_net: fuzz/fuzz_net.c src/net.c src/net.h src/arena.c src/common.c | $(BUILD)
	$(CC) $(BASE) $(SAN) -I src -o $@ fuzz/fuzz_net.c src/net.c src/arena.c src/common.c -lm

.PHONY: test golden fuzz bench clean size conformance guard vsx aklbench gui guismoke akltest

test: $(BUILD)/run_tests $(BUILD)/run_tests_switch cxxtest
	./$(BUILD)/run_tests
	./$(BUILD)/run_tests_switch

# C++ V8 API ファサード（src/akl/v8.h）の実動証明。
# 製品法則の検査を兼ねる: このテストバイナリは libstdc++ を動的リンクしないこと。
CXX ?= g++
$(BUILD)/v8_compat_smoke.o: tests/cpp/v8_compat_smoke.cc src/akl/v8.h src/akl/akl.h | $(BUILD)
	$(CXX) -std=c++11 -fno-exceptions -fno-rtti -O2 -Wall -Wextra -I src -o $@ -c tests/cpp/v8_compat_smoke.cc
$(BUILD)/test_v8compat: $(BUILD)/v8_compat_smoke.o $(AKLSRC) | $(BUILD)
	$(CC) $(BASE) $(REL) -o $@ $(BUILD)/v8_compat_smoke.o $(AKLSRC) $(LDFLAGS_REL) -lm
# akl 単体ランナーの e2e（budget kill・既定 budget 収束・--no-cojit・seccomp 回帰）
akltest: $(BUILD)/akl
	python3 tests/akl_cli_smoke.py ./$(BUILD)/akl

cxxtest: $(BUILD)/test_v8compat
	@if ldd $(BUILD)/test_v8compat | grep -q 'libstdc++'; then \
		echo 'FATAL: v8compat links libstdc++ (製品法則違反)' >&2; exit 1; fi
	./$(BUILD)/test_v8compat

golden: $(BUILD)/ifuto-asan
	tests/run_golden.sh ./$(BUILD)/ifuto-asan

fuzz: $(BUILD)/fuzz_html $(BUILD)/fuzz_akl $(BUILD)/fuzz_net
	fuzz/run_fuzz.sh ./$(BUILD)/fuzz_html 500
	fuzz/run_fuzz.sh ./$(BUILD)/fuzz_akl 500
	IFUZZ_SEEDS=tests/fuzzseeds/net fuzz/run_fuzz.sh ./$(BUILD)/fuzz_net 500

bench: $(BUILD)/ifuto
	bench/bench.sh ./$(BUILD)/ifuto

# v-chrome 天井の実測検証（PTY 冷間開始/空タブ RSS/idle CPU + 50 タブメタ + セッション復元）
$(BUILD)/bench_tabmeta: bench/bench_tabmeta.c $(ENGINE) | $(BUILD)
	$(CC) $(BASE) $(REL) -o $@ bench/bench_tabmeta.c $(ENGINE) $(LDFLAGS_REL) -lm

$(BUILD)/bench_session: bench/bench_session.c $(ENGINE) | $(BUILD)
	$(CC) $(BASE) $(REL) -o $@ bench/bench_session.c $(ENGINE) $(LDFLAGS_REL) -lm

# Akl dispatch 決定の根拠データ（結果は BENCH.md に中央値で公開）
$(BUILD)/bench_akl: bench/bench_akl.c $(AKLSRC) | $(BUILD)
	$(CC) $(BASE) $(REL) -o $@ bench/bench_akl.c $(AKLSRC) $(LDFLAGS_REL) -lm

$(BUILD)/bench_akl_switch: bench/bench_akl.c $(AKLSRC) | $(BUILD)
	$(CC) $(BASE) $(REL) -DAKL_TEST_SWITCH_DISPATCH -o $@ bench/bench_akl.c $(AKLSRC) $(LDFLAGS_REL) -lm

# CSS RuleSet 索引の効果（naive 全走査との同一バイナリ比較。結果は BENCH.md へ実測公開）
$(BUILD)/bench_css: bench/bench_css.c $(ENGINE) | $(BUILD)
	$(CC) $(BASE) $(REL) -o $@ bench/bench_css.c $(ENGINE) $(LDFLAGS_REL) -lm

cssbench: $(BUILD)/bench_css
	$(BUILD)/bench_css

# Akl 単体 CLI（比較ベンチ / make guard の被測定バイナリ。本体には不加入）
$(BUILD)/akl: bench/akl_cli.c $(AKLSRC) src/sandbox.c | $(BUILD)
	$(CC) $(BASE) $(REL) -o $@ bench/akl_cli.c $(AKLSRC) src/sandbox.c $(LDFLAGS_REL) -lm

# RSS 測定ラッパ（python 直測は fork/exec 間の python ページ混入で ~10MB 誤計るため C 製）
$(BUILD)/rssrun: bench/rssrun.c | $(BUILD)
	$(CC) -std=c11 -O2 -o $@ bench/rssrun.c

# 常時監視アラーム: 閾値（bench/akl_guards.json）から 1 件でも逸脱したら exit 1。
# 絶対閾値は参照エンジンなしで常時有効。相対閾値は QJS=/path/to/qjs と node 検出で有効化。
guard: $(BUILD)/akl $(BUILD)/rssrun
	QJS=$${QJS:-/home/user/ref/quickjs/qjs} python3 bench/akl_compare.py --rounds 3 --rss --guard bench/akl_guards.json

aklbench: $(BUILD)/bench_akl $(BUILD)/bench_akl_switch
	./$(BUILD)/bench_akl
	./$(BUILD)/bench_akl_switch

tuibench: $(BUILD)/ifuto $(BUILD)/bench_tabmeta $(BUILD)/bench_session
	./$(BUILD)/bench_tabmeta
	./$(BUILD)/bench_session

# tree-construction 適合採点（WPT resources/*.dat, PINNED.sha でピン留め）
conformance: $(BUILD)/ifuto
	python3 tests/run_html5lib.py ./$(BUILD)/ifuto tests/wpt-tree-construction

size: $(BUILD)/ifuto
	@ls -l $(BUILD)/ifuto | awk '{printf "binary bytes: %d\n", $$5}'
	@size $(BUILD)/ifuto 2>/dev/null || true

clean:
	rm -rf $(BUILD)

# 兄弟セッションの harness（環境変数 AKL/QJS を参照。見つからない参照は harness 自身が報告）
vsx: $(BUILD)/akl
	python3 bench/vsx.py
