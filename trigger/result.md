# 実行結果 (2026-08-14T13:35:13Z)

source: trigger/trigger.md @ 14e62f3ed2568e88c383c3dc7cadf6312b16f3b1

- [x] [CMD_RESULT 1] `cd rust && cargo build --workspace` — exit 0
- [ ] [CMD_RESULT 2] `cd rust && cargo test --workspace` — **exit 101**
- [ ] [CMD_RESULT 3] `cd rust && cargo clippy --workspace -- -D warnings` — **exit 101**
- [ ] [CMD_RESULT 4] `cd rust && cargo +nightly miri test --workspace` — **exit 1**
- [ ] [CMD_RESULT 5] `cd rust && cargo kani --workspace` — **exit 101**
- [x] [CMD_RESULT 6] `cd rust && cargo geiger --workspace 2>/dev/null || true` — exit 0
# 実行結果 (2026-08-15T00:34:34Z)

source: trigger/trigger.md @ c229a2e5009fc61f4a9677f98e2d64514fdd3b2c

- [x] [CMD_RESULT 1] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; rustup toolchain install nightly --profile minimal && rustup component add clippy rustfmt && rustup component add miri --toolchain nightly && rustc --version && cargo --version` — exit 0
- [x] [CMD_RESULT 2] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo build --workspace` — exit 0
- [x] [CMD_RESULT 3] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo test --workspace` — exit 0
- [ ] [CMD_RESULT 4] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo clippy --workspace -- -D warnings` — **exit 101**
- [x] [CMD_RESULT 5] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 600 cargo +nightly miri test --workspace` — exit 0
- [ ] [CMD_RESULT 6] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 900 cargo kani --workspace` — **exit 101**
- [x] [CMD_RESULT 7] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; timeout 900 bash -c 'cargo install --locked prusti --version 1.0.0 2>&1 | tail -3 || echo "prusti install failed (version pin)"; cargo prusti --version 2>/dev/null || true'` — exit 0
- [x] [CMD_RESULT 8] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && (timeout 300 cargo geiger --workspace 2>/dev/null || true)` — exit 0
# 実行結果 (2026-08-15T00:37:23Z)

source: trigger/trigger.md @ 11ae9eba9f3110cb484992f363624663bb709cd7

- [x] [CMD_RESULT 1] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; rustup toolchain install nightly --profile minimal && rustup component add clippy rustfmt && rustup component add miri --toolchain nightly && rustc --version && cargo --version` — exit 0
- [x] [CMD_RESULT 2] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; timeout 600 bash -c 'cargo install --locked kani-verifier 2>&1 | tail -5; cargo kani --version || echo "kani unavailable"'` — exit 0
- [x] [CMD_RESULT 3] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo build --workspace` — exit 0
- [x] [CMD_RESULT 4] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo test --workspace` — exit 0
- [ ] [CMD_RESULT 5] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo clippy --workspace -- -D warnings 2>&1 | tail -30; test ${PIPESTATUS[0]} -eq 0` — **exit 1**
- [x] [CMD_RESULT 6] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 600 cargo +nightly miri test --workspace` — exit 0
- [x] [CMD_RESULT 7] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 900 cargo kani --workspace` — exit 0
- [x] [CMD_RESULT 8] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; timeout 900 bash -c 'cargo install --locked prusti --version 1.0.0 2>&1 | tail -3 || echo "prusti install failed (version pin)"; cargo prusti --version 2>/dev/null || true'` — exit 0
- [x] [CMD_RESULT 9] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && (timeout 300 cargo geiger --workspace 2>/dev/null || true)` — exit 0

### clippy 実行 (2026-08-15T00:39:44Z)
```
    = help: to override `-D warnings` add `#[allow(clippy::collapsible_if)]`
help: collapse nested if block
    |
200 ~             if !mark[i]
201 ~                 && self.slots[i].take().is_some() {
202 |                     self.free.push(i as ObjId);
203 |                     self.live -= 1;
204 |                     swept += 1;
205 ~                 }
    |

error: this returns a `Result<_, ()>`
   --> akl-core/src/obj.rs:238:5
    |
238 | /     pub fn arr_map(
239 | |         &mut self,
240 | |         id: ObjId,
241 | |         mut f: impl FnMut(AklVal, usize) -> AklVal,
242 | |     ) -> Result<ObjId, ()> {
    | |__________________________^
    |
    = help: use a custom `Error` type instead
    = help: for further information visit https://rust-lang.github.io/rust-clippy/rust-1.97.0/index.html#result_unit_err

error: this operation has no effect
  --> akl-core/src/lib.rs:55:34
   |
55 |     pub const UNDEF: Self = Self(TAG_MASK | 0);
   |                                  ^^^^^^^^^^^^ help: consider reducing it to: `TAG_MASK`
   |
   = help: for further information visit https://rust-lang.github.io/rust-clippy/rust-1.97.0/index.html#identity_op
   = note: `-D clippy::identity-op` implied by `-D warnings`
   = help: to override `-D warnings` add `#[allow(clippy::identity_op)]`

error: missing documentation for a struct field
  --> akl-core/src/obj.rs:60:11
   |
60 |     Env { vals: Vec<AklVal>, parent: Option<ObjId> },
   |           ^^^^^^^^^^^^^^^^^
   |
   = note: `-D missing-docs` implied by `-D warnings`
   = help: to override `-D warnings` add `#[allow(missing_docs)]`

error: missing documentation for a struct field
  --> akl-core/src/obj.rs:60:30
   |
60 |     Env { vals: Vec<AklVal>, parent: Option<ObjId> },
   |                              ^^^^^^^^^^^^^^^^^^^^^

error: could not compile `akl-core` (lib) due to 9 previous errors
```
# 実行結果 (2026-08-15T00:40:04Z)

source: trigger/trigger.md @ ad54f1c670ee1321139dc7b2155479609a48262f

- [x] [CMD_RESULT 1] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; rustup toolchain install nightly --profile minimal && rustup component add clippy rustfmt && rustup component add miri --toolchain nightly && rustc --version && cargo --version` — exit 0
- [x] [CMD_RESULT 2] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; timeout 600 bash -c 'cargo install --locked kani-verifier 2>&1 | tail -5; cargo kani --version || echo "kani unavailable"'` — exit 0
- [x] [CMD_RESULT 3] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo build --workspace` — exit 0
- [x] [CMD_RESULT 4] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo test --workspace` — exit 0
- [ ] [CMD_RESULT 5] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo clippy --workspace -- -D warnings > /tmp/clippy.log 2>&1; rc=$?; { echo ""; echo "### clippy 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -50 /tmp/clippy.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — **exit 1**
- [x] [CMD_RESULT 6] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 600 cargo +nightly miri test --workspace` — exit 0
- [x] [CMD_RESULT 7] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 900 cargo kani --workspace` — exit 0
- [x] [CMD_RESULT 8] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; timeout 900 bash -c 'cargo install --locked prusti --version 1.0.0 2>&1 | tail -3 || echo "prusti install failed (version pin)"; cargo prusti --version 2>/dev/null || true'` — exit 0
- [x] [CMD_RESULT 9] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && (timeout 300 cargo geiger --workspace 2>/dev/null || true)` — exit 0

### clippy 実行 (2026-08-15T00:42:14Z)
```
    Checking akl-core v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/akl-core)
error: unexpected `cfg` condition name: `kani`
   --> akl-core/src/lib.rs:230:7
    |
230 | #[cfg(kani)]
    |       ^^^^
    |
    = help: expected names are: `docsrs`, `feature`, and `test` and 32 more
    = help: consider using a Cargo feature instead
    = help: or consider adding in `Cargo.toml` the `check-cfg` lint config for the lint:
             [lints.rust]
             unexpected_cfgs = { level = "warn", check-cfg = ['cfg(kani)'] }
    = help: or consider adding `println!("cargo::rustc-check-cfg=cfg(kani)");` to the top of the `build.rs`
    = note: see <https://doc.rust-lang.org/nightly/rustc/check-cfg/cargo-specifics.html> for more information about checking conditional configuration
    = note: `-D unexpected-cfgs` implied by `-D warnings`
    = help: to override `-D warnings` add `#[allow(unexpected_cfgs)]`

error: struct `ObjTable` has a public `len` method, but no `is_empty` method
   --> akl-core/src/obj.rs:140:5
    |
140 |     pub fn len(&self) -> usize {
    |     ^^^^^^^^^^^^^^^^^^^^^^^^^^
    |
    = help: for further information visit https://rust-lang.github.io/rust-clippy/rust-1.97.0/index.html#len_without_is_empty
    = note: `-D clippy::len-without-is-empty` implied by `-D warnings`
    = help: to override `-D warnings` add `#[allow(clippy::len_without_is_empty)]`

error: the loop variable `i` is used to index `mark`
   --> akl-core/src/obj.rs:213:18
    |
213 |         for i in 0..n {
    |                  ^^^^
    |
    = help: for further information visit https://rust-lang.github.io/rust-clippy/rust-1.97.0/index.html#needless_range_loop
    = note: `-D clippy::needless-range-loop` implied by `-D warnings`
    = help: to override `-D warnings` add `#[allow(clippy::needless_range_loop)]`
help: consider using an iterator and enumerate()
    |
213 -         for i in 0..n {
213 +         for (i, <item>) in mark.iter().enumerate().take(n) {
    |

error: could not compile `akl-core` (lib) due to 3 previous errors
```
# 実行結果 (2026-08-15T00:42:33Z)

source: trigger/trigger.md @ e33ea3bf57046ec8ae8b2a3164ff3b44f49770e1

- [x] [CMD_RESULT 1] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; rustup toolchain install nightly --profile minimal && rustup component add clippy rustfmt && rustup component add miri --toolchain nightly && rustc --version && cargo --version` — exit 0
- [x] [CMD_RESULT 2] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; timeout 600 bash -c 'cargo install --locked kani-verifier 2>&1 | tail -5; cargo kani --version || echo "kani unavailable"'` — exit 0
- [x] [CMD_RESULT 3] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo build --workspace` — exit 0
- [x] [CMD_RESULT 4] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo test --workspace` — exit 0
- [ ] [CMD_RESULT 5] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo clippy --workspace -- -D warnings > /tmp/clippy.log 2>&1; rc=$?; { echo ""; echo "### clippy 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -50 /tmp/clippy.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — **exit 1**
- [x] [CMD_RESULT 6] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 600 cargo +nightly miri test --workspace` — exit 0
- [x] [CMD_RESULT 7] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 900 cargo kani --workspace` — exit 0
- [x] [CMD_RESULT 8] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; timeout 900 bash -c 'cargo install --locked prusti --version 1.0.0 2>&1 | tail -3 || echo "prusti install failed (version pin)"; cargo prusti --version 2>/dev/null || true'` — exit 0
- [x] [CMD_RESULT 9] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && (timeout 300 cargo geiger --workspace 2>/dev/null || true)` — exit 0

### clippy 実行 (2026-08-15T00:43:55Z)
```
error: failed to parse manifest at `/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/Cargo.toml`

Caused by:
  this virtual manifest specifies a `lints` section, which is not allowed
```
# 実行結果 (2026-08-15T00:44:14Z)

source: trigger/trigger.md @ 35841747bd0b12431d20d173ce010a0afa2bb72c

- [x] [CMD_RESULT 1] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; rustup toolchain install nightly --profile minimal && rustup component add clippy rustfmt && rustup component add miri --toolchain nightly && rustc --version && cargo --version` — exit 0
- [x] [CMD_RESULT 2] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; timeout 600 bash -c 'cargo install --locked kani-verifier 2>&1 | tail -5; cargo kani --version || echo "kani unavailable"'` — exit 0
- [ ] [CMD_RESULT 3] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo build --workspace` — **exit 101**
- [ ] [CMD_RESULT 4] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo test --workspace` — **exit 101**
- [ ] [CMD_RESULT 5] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo clippy --workspace -- -D warnings > /tmp/clippy.log 2>&1; rc=$?; { echo ""; echo "### clippy 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -50 /tmp/clippy.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — **exit 1**
- [ ] [CMD_RESULT 6] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 600 cargo +nightly miri test --workspace` — **exit 1**
- [ ] [CMD_RESULT 7] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 900 cargo kani --workspace` — **exit 1**
- [x] [CMD_RESULT 8] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; timeout 900 bash -c 'cargo install --locked prusti --version 1.0.0 2>&1 | tail -3 || echo "prusti install failed (version pin)"; cargo prusti --version 2>/dev/null || true'` — exit 0
- [x] [CMD_RESULT 9] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && (timeout 300 cargo geiger --workspace 2>/dev/null || true)` — exit 0
