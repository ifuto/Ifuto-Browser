# 実行結果 (2026-08-14T13:35:13Z)

source: trigger/trigger.md @ 14e62f3ed2568e88c383c3dc7cadf6312b16f3b1

- [x] [CMD_RESULT 1] `cd rust && cargo build --workspace` — exit 0
- [ ] [CMD_RESULT 2] `cd rust && cargo test --workspace` — **exit 101**
- [ ] [CMD_RESULT 3] `cd rust && cargo clippy --workspace -- -D warnings` — **exit 101**
- [ ] [CMD_RESULT 4] `cd rust && cargo +nightly miri test --workspace` — **exit 1**
- [ ] [CMD_RESULT 5] `cd rust && cargo kani --workspace` — **exit 101**
- [x] [CMD_RESULT 6] `cd rust && cargo geiger --workspace 2>/dev/null || true` — exit 0
