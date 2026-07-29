# Ifuto Browser — build
# 設計上の固定費を避けるため、依存ライブラリはゼロ（libc のみ）。

CC      ?= cc
SRC     := $(wildcard src/*.c)
ENGINE  := $(filter-out src/main.c,$(SRC))
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
$(BUILD)/run_tests: $(TESTSRC) $(ENGINE) | $(BUILD)
	$(CC) $(BASE) $(SAN) -Itests -o $@ $(TESTSRC) $(ENGINE) -lm

$(BUILD)/fuzz_html: fuzz/fuzz_driver.c $(ENGINE) | $(BUILD)
	$(CC) $(BASE) $(SAN) -I src -o $@ fuzz/fuzz_driver.c $(ENGINE) -lm

.PHONY: test uitest golden fuzz bench tuibench clean size conformance

test: $(BUILD)/run_tests
	./$(BUILD)/run_tests

# TUI を疑似端末(PTY)越しに駆動する e2e スモーク（tabstrip/scroll/quit 等）
uitest: $(BUILD)/ifuto
	python3 tests/tui_smoke.py ./$(BUILD)/ifuto

golden: $(BUILD)/ifuto-asan
	tests/run_golden.sh ./$(BUILD)/ifuto-asan

fuzz: $(BUILD)/fuzz_html
	fuzz/run_fuzz.sh ./$(BUILD)/fuzz_html 500

bench: $(BUILD)/ifuto
	bench/bench.sh ./$(BUILD)/ifuto

# v-chrome 天井の実測検証（PTY 冷間開始/空タブ RSS/idle CPU + 50 タブメタ + セッション復元）
$(BUILD)/bench_tabmeta: bench/bench_tabmeta.c $(ENGINE) | $(BUILD)
	$(CC) $(BASE) $(REL) -o $@ bench/bench_tabmeta.c $(ENGINE) $(LDFLAGS_REL) -lm

$(BUILD)/bench_session: bench/bench_session.c $(ENGINE) | $(BUILD)
	$(CC) $(BASE) $(REL) -o $@ bench/bench_session.c $(ENGINE) $(LDFLAGS_REL) -lm

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
