# Ifuto Browser — build
# 設計上の固定費を避けるため、依存ライブラリはゼロ（libc のみ）。

CC      ?= cc
SRC     := $(wildcard src/*.c)
ENGINE  := $(filter-out src/main.c,$(SRC))
# V8x（自作 JS エンジン, C11/JIT なし）: v0.0 は DOM 未接続のため本体 ifuto には
# リンクしない（200KB 天井維持。v0.4 統合時に実測で天井を再設定する — BENCH.md 台帳）。
V8XSRC  := $(wildcard src/v8x/*.c)
WFLAGS  := -std=c11 -Wall -Wextra -Wshadow -Wstrict-prototypes -Wwrite-strings
BASE    := $(WFLAGS) -fno-strict-aliasing -fstack-protector-strong -D_FORTIFY_SOURCE=2
REL     := -O2 -DNDEBUG -flto -ffunction-sections -fdata-sections
LDFLAGS_REL := -flto -Wl,--gc-sections -s
SAN     := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer

BUILD   := build

all: $(BUILD)/ifuto

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/ifuto: $(SRC) | $(BUILD)
	$(CC) $(BASE) $(REL) -o $@ $(SRC) $(LDFLAGS_REL) -lm

# 開発用: sanitizer 付きバイナリ（テスト・手動検証は常にこちらで）
$(BUILD)/ifuto-asan: $(SRC) | $(BUILD)
	$(CC) $(BASE) $(SAN) -o $@ $(SRC) -lm

TESTSRC := $(wildcard tests/*.c)
$(BUILD)/run_tests: $(TESTSRC) $(ENGINE) $(V8XSRC) | $(BUILD)
	$(CC) $(BASE) $(SAN) -Itests -o $@ $(TESTSRC) $(ENGINE) $(V8XSRC) -lm

# v8x の switch dispatch 側も丸ごと走査する双子バイナリ（片側だけの不具合を封殺）
$(BUILD)/run_tests_switch: $(TESTSRC) $(ENGINE) $(V8XSRC) | $(BUILD)
	$(CC) $(BASE) $(SAN) -DV8X_TEST_SWITCH_DISPATCH -Itests -o $@ $(TESTSRC) $(ENGINE) $(V8XSRC) -lm

$(BUILD)/fuzz_html: fuzz/fuzz_driver.c $(ENGINE) | $(BUILD)
	$(CC) $(BASE) $(SAN) -I src -o $@ fuzz/fuzz_driver.c $(ENGINE) -lm

$(BUILD)/fuzz_v8x: fuzz/fuzz_v8x.c $(V8XSRC) | $(BUILD)
	$(CC) $(BASE) $(SAN) -I src -o $@ fuzz/fuzz_v8x.c $(V8XSRC) -lm

.PHONY: test uitest golden fuzz bench tuibench clean size conformance guard vsx v8xbench

test: $(BUILD)/run_tests $(BUILD)/run_tests_switch
	./$(BUILD)/run_tests
	./$(BUILD)/run_tests_switch

# TUI を疑似端末(PTY)越しに駆動する e2e スモーク（tabstrip/scroll/quit 等）
uitest: $(BUILD)/ifuto
	python3 tests/tui_smoke.py ./$(BUILD)/ifuto

golden: $(BUILD)/ifuto-asan
	tests/run_golden.sh ./$(BUILD)/ifuto-asan

fuzz: $(BUILD)/fuzz_html $(BUILD)/fuzz_v8x
	fuzz/run_fuzz.sh ./$(BUILD)/fuzz_html 500
	fuzz/run_fuzz.sh ./$(BUILD)/fuzz_v8x 500

bench: $(BUILD)/ifuto
	bench/bench.sh ./$(BUILD)/ifuto

# v-chrome 天井の実測検証（PTY 冷間開始/空タブ RSS/idle CPU + 50 タブメタ + セッション復元）
$(BUILD)/bench_tabmeta: bench/bench_tabmeta.c $(ENGINE) | $(BUILD)
	$(CC) $(BASE) $(REL) -o $@ bench/bench_tabmeta.c $(ENGINE) $(LDFLAGS_REL) -lm

$(BUILD)/bench_session: bench/bench_session.c $(ENGINE) | $(BUILD)
	$(CC) $(BASE) $(REL) -o $@ bench/bench_session.c $(ENGINE) $(LDFLAGS_REL) -lm

# V8x dispatch 決定の根拠データ（結果は BENCH.md に中央値で公開）
$(BUILD)/bench_v8x: bench/bench_v8x.c $(V8XSRC) | $(BUILD)
	$(CC) $(BASE) $(REL) -o $@ bench/bench_v8x.c $(V8XSRC) $(LDFLAGS_REL) -lm

$(BUILD)/bench_v8x_switch: bench/bench_v8x.c $(V8XSRC) | $(BUILD)
	$(CC) $(BASE) $(REL) -DV8X_TEST_SWITCH_DISPATCH -o $@ bench/bench_v8x.c $(V8XSRC) $(LDFLAGS_REL) -lm

# V8x 単体 CLI（比較ベンチ / make guard の被測定バイナリ。本体には不加入）
$(BUILD)/v8x_cli: bench/v8x_cli.c $(V8XSRC) | $(BUILD)
	$(CC) $(BASE) $(REL) -o $@ bench/v8x_cli.c $(V8XSRC) $(LDFLAGS_REL) -lm

# RSS 測定ラッパ（python 直測は fork/exec 間の python ページ混入で ~10MB 誤計るため C 製）
$(BUILD)/rssrun: bench/rssrun.c | $(BUILD)
	$(CC) -std=c11 -O2 -o $@ bench/rssrun.c

# 常時監視アラーム: 閾値（bench/v8x_guards.json）から 1 件でも逸脱したら exit 1。
# 絶対閾値は参照エンジンなしで常時有効。相対閾値は QJS=/path/to/qjs と node 検出で有効化。
guard: $(BUILD)/v8x_cli $(BUILD)/rssrun
	QJS=$${QJS:-/home/user/ref/quickjs/qjs} python3 bench/v8x_compare.py --rounds 3 --rss --guard bench/v8x_guards.json

v8xbench: $(BUILD)/bench_v8x $(BUILD)/bench_v8x_switch
	./$(BUILD)/bench_v8x
	./$(BUILD)/bench_v8x_switch

tuibench: $(BUILD)/ifuto $(BUILD)/bench_tabmeta $(BUILD)/bench_session
	python3 bench/bench_tui.py ./$(BUILD)/ifuto
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

# 兄弟セッションの harness（環境変数 V8X/QJS を参照。見つからない参照は harness 自身が報告）
vsx: $(BUILD)/v8x_cli
	python3 bench/vsx.py
