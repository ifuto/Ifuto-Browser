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

### clippy 実行 (2026-08-15T00:45:27Z)
```
    Checking akl-core v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/akl-core)
    Finished `dev` profile [unoptimized + debuginfo] target(s) in 2.38s
```
# 実行結果 (2026-08-15T00:45:50Z)

source: trigger/trigger.md @ 8bb60ff2095f38ee538b7c3701974a671dd4e329

- [x] [CMD_RESULT 1] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; rustup toolchain install nightly --profile minimal && rustup component add clippy rustfmt && rustup component add miri --toolchain nightly && rustc --version && cargo --version` — exit 0
- [x] [CMD_RESULT 2] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; timeout 600 bash -c 'cargo install --locked kani-verifier 2>&1 | tail -5; cargo kani --version || echo "kani unavailable"'` — exit 0
- [x] [CMD_RESULT 3] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo build --workspace` — exit 0
- [x] [CMD_RESULT 4] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo test --workspace` — exit 0
- [x] [CMD_RESULT 5] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 300 cargo clippy --workspace -- -D warnings > /tmp/clippy.log 2>&1; rc=$?; { echo ""; echo "### clippy 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -50 /tmp/clippy.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — exit 0
- [x] [CMD_RESULT 6] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 600 cargo +nightly miri test --workspace` — exit 0
- [x] [CMD_RESULT 7] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && timeout 900 cargo kani --workspace` — exit 0
- [x] [CMD_RESULT 8] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; timeout 900 bash -c 'cargo install --locked prusti --version 1.0.0 2>&1 | tail -3 || echo "prusti install failed (version pin)"; cargo prusti --version 2>/dev/null || true'` — exit 0
- [x] [CMD_RESULT 9] `export PATH="$HOME/.cargo/bin:/usr/local/cargo/bin:$PATH"; cd rust && (timeout 300 cargo geiger --workspace 2>/dev/null || true)` — exit 0

### clippy 実行 (2026-08-15T01:21:54Z)
```
bash: line 1: trigger/tc: No such file or directory
```
# 実行結果 (2026-08-15T01:21:54Z)

source: trigger/trigger.md @ 8e49918b74cba54b34e4de34bbb3dddbd3078f28

- [x] [CMD_RESULT 1] `bash trigger/toolchain.sh` — exit 0
- [ ] [CMD_RESULT 2] `cd rust && trigger/tc timeout 300 cargo build --workspace` — **exit 127**
- [ ] [CMD_RESULT 3] `cd rust && trigger/tc timeout 300 cargo test --workspace` — **exit 127**
- [ ] [CMD_RESULT 4] `cd rust && trigger/tc timeout 300 cargo clippy --workspace -- -D warnings > /tmp/clippy.log 2>&1; rc=$?; { echo ""; echo "### clippy 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -50 /tmp/clippy.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — **exit 1**
- [ ] [CMD_RESULT 5] `cd rust && trigger/tc timeout 600 cargo +nightly miri test --workspace` — **exit 127**
- [ ] [CMD_RESULT 6] `cd rust && trigger/tc timeout 900 cargo kani --workspace` — **exit 127**
- [x] [CMD_RESULT 7] `cd rust && (trigger/tc timeout 300 cargo geiger --workspace 2>/dev/null || true)` — exit 0
- [x] [CMD_RESULT 8] `cd rust && (trigger/tc timeout 300 cargo tarpaulin --workspace 2>&1 | tail -12 || true)` — exit 0
- [x] [CMD_RESULT 9] `trigger/tc cargo fuzz --version 2>/dev/null || echo "cargo-fuzz not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 10] `trigger/tc flux --version 2>/dev/null || echo "flux not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 11] `trigger/tc mirai --version 2>/dev/null || echo "mirai not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 12] `trigger/tc cargo prusti --version 2>/dev/null || trigger/tc prusti-rustc --version 2>/dev/null || echo "prusti not installed (skipped)"` — exit 0

### toolchain 実行 (2026-08-15T01:32:54Z) rc=0
```
==> Release toolchain-v1 が無いためビルドします（初回のみ・数十分）
info: downloading installer
warn: It looks like you have an existing rustup settings file at:
warn: /home/runner/akl-toolchain/rustup/settings.toml
warn: Rustup will install the default toolchain as specified in the settings file,
warn: instead of the one inferred from the default host triple.
info: profile set to minimal
info: default host triple is x86_64-unknown-linux-gnu
info: syncing channel updates for stable-x86_64-unknown-linux-gnu
info: latest update on 2026-07-16 for version 1.97.1 (8bab26f4f 2026-07-14)
info: downloading 3 components
info: default toolchain set to stable-x86_64-unknown-linux-gnu

  stable-x86_64-unknown-linux-gnu installed - rustc 1.97.1 (8bab26f4f 2026-07-14)


Rust is installed now. Great!

To get started you may need to restart your current shell.
This would reload your PATH environment variable to include
Cargo's bin directory (/home/runner/akl-toolchain/cargo/bin).

To configure your current shell, you need to source
the corresponding env file under /home/runner/akl-toolchain/cargo.

This is usually done by running one of the following (note the leading DOT):
. "/home/runner/akl-toolchain/cargo/env"            # For 
sh/bash/zsh/ash/dash/pdksh
source "/home/runner/akl-toolchain/cargo/env.fish"  # For fish
source "/home/runner/akl-toolchain/cargo/env.nu"  # For nushell
source "/home/runner/akl-toolchain/cargo/env.tcsh"  # For tcsh
. "/home/runner/akl-toolchain/cargo/env.ps1"        # For pwsh
source "/home/runner/akl-toolchain/cargo/env.xsh"   # For xonsh
info: syncing channel updates for nightly-x86_64-unknown-linux-gnu
info: latest update on 2026-08-14 for version 1.99.0-nightly (ba28ff76f 2026-08-13)
info: downloading 3 components

  nightly-x86_64-unknown-linux-gnu installed - rustc 1.99.0-nightly (ba28ff76f 2026-08-13)

info: downloading component clippy
info: downloading component rustfmt
info: downloading component miri
rustc 1.97.1 (8bab26f4f 2026-07-14)
cargo 1.97.1 (c980f4866 2026-06-30)
  Installing /home/runner/akl-toolchain/cargo/bin/cargo-kani
  Installing /home/runner/akl-toolchain/cargo/bin/kani
   Installed package `kani-verifier v0.67.0` (executables `cargo-kani`, `kani`)
[0/5] Running Kani first-time setup...
[1/5] Ensuring the existence of: /home/runner/.kani/kani-0.67.0
[2/5] Downloading Kani release bundle: kani-0.67.0-x86_64-unknown-linux-gnu.tar.gz
[3/5] Installing rust toolchain version: nightly-2025-11-21-x86_64-unknown-linux-gnu
info: syncing channel updates for nightly-2025-11-21-x86_64-unknown-linux-gnu
info: latest update on 2025-11-21 for version 1.93.0-nightly (53732d5e0 2025-11-20)
info: downloading 3 components

  nightly-2025-11-21-x86_64-unknown-linux-gnu installed - rustc 1.93.0-nightly (53732d5e0 2025-11-20)

[5/5] Successfully completed Kani first-time setup.
cargo-kani 0.67.0
  Installing /home/runner/akl-toolchain/cargo/bin/cargo-tarpaulin
   Installed package `cargo-tarpaulin v0.37.1` (executable `cargo-tarpaulin`)
  Installing /home/runner/akl-toolchain/cargo/bin/cargo-geiger
   Installed package `cargo-geiger v0.13.0` (executable `cargo-geiger`)
  Installing /home/runner/akl-toolchain/cargo/bin/cargo-fuzz
   Installed package `cargo-fuzz v0.13.2` (executable `cargo-fuzz`)
    Updating git repository `https://github.com/flux-rs/flux`
error: could not find `flux` in https://github.com/flux-rs/flux with version `*`
flux: skip
`cargo install` is only for installing programs, and can't be used with libraries.
To use a library crate, add it as a dependency to a Cargo project with `cargo add`.
mirai: skip
    Updating crates.io index
error: could not find `prusti` in registry `crates-io` with version `=1.0.0`
prusti: skip
==> アーカイブ作成
-rw-r--r-- 1 runner runner 918M Aug 15 01:32 /home/runner/akl-toolchain/akl-toolchain.tar.gz
gh: To use GitHub CLI in a GitHub Actions workflow, set the GH_TOKEN environment variable. Example:
  env:
    GH_TOKEN: ${{ github.token }}
==> Release toolchain-v1 作成完了
```

### clippy 実行 (2026-08-15T01:32:55Z)
```
    |                    ^                ^
    |
help: remove these parentheses
    |
237 -                 Ok((Step::Jump(*tgt)))
237 +                 Ok(Step::Jump(*tgt))
    |

error[E0308]: mismatched types
   --> akl-core/src/bytecode.rs:230:24
    |
230 |                     Ok((Step::Jump(*tgt)))
    |                     -- ^^^^^^^^^^^^^^^^^^ expected `(Step, usize)`, found `Step`
    |                     |
    |                     arguments to this enum variant are incorrect
    |
    = note: expected tuple `(bytecode::Step, usize)`
                found enum `bytecode::Step`
help: the type constructed contains `bytecode::Step` due to the type of the argument passed
   --> akl-core/src/bytecode.rs:230:21
    |
230 |                     Ok((Step::Jump(*tgt)))
    |                     ^^^------------------^
    |                        |
    |                        this argument influences the type of `Ok`
note: tuple variant defined here
   --> /rustc/8bab26f4f68e0e26f0bb7960be334d5b520ea452/library/core/src/result.rs:561:4

error[E0308]: mismatched types
   --> akl-core/src/bytecode.rs:237:20
    |
237 |                 Ok((Step::Jump(*tgt)))
    |                 -- ^^^^^^^^^^^^^^^^^^ expected `(Step, usize)`, found `Step`
    |                 |
    |                 arguments to this enum variant are incorrect
    |
    = note: expected tuple `(bytecode::Step, usize)`
                found enum `bytecode::Step`
help: the type constructed contains `bytecode::Step` due to the type of the argument passed
   --> akl-core/src/bytecode.rs:237:17
    |
237 |                 Ok((Step::Jump(*tgt)))
    |                 ^^^------------------^
    |                    |
    |                    this argument influences the type of `Ok`
note: tuple variant defined here
   --> /rustc/8bab26f4f68e0e26f0bb7960be334d5b520ea452/library/core/src/result.rs:561:4

For more information about this error, try `rustc --explain E0308`.
error: could not compile `akl-core` (lib) due to 6 previous errors
```
# 実行結果 (2026-08-15T01:33:13Z)

source: trigger/trigger.md @ 450ae0e68c83dfff6105fdb0d3d33eaf25cc57d6

- [x] [CMD_RESULT 1] `bash trigger/toolchain.sh > /tmp/tc.log 2>&1; rc=$?; { echo ""; echo "### toolchain 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ)) rc=$rc"; echo '```'; tail -80 /tmp/tc.log; echo '```'; } >> trigger/result.md; test $rc -eq 0` — exit 0
- [ ] [CMD_RESULT 2] `cd rust && ../trigger/tc timeout 300 cargo build --workspace` — **exit 101**
- [ ] [CMD_RESULT 3] `cd rust && ../trigger/tc timeout 300 cargo test --workspace` — **exit 101**
- [ ] [CMD_RESULT 4] `cd rust && ../trigger/tc timeout 300 cargo clippy --workspace -- -D warnings > /tmp/clippy.log 2>&1; rc=$?; { echo ""; echo "### clippy 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -50 /tmp/clippy.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — **exit 1**
- [ ] [CMD_RESULT 5] `cd rust && ../trigger/tc timeout 600 cargo +nightly miri test --workspace` — **exit 101**
- [ ] [CMD_RESULT 6] `cd rust && ../trigger/tc timeout 900 cargo kani --workspace` — **exit 1**
- [x] [CMD_RESULT 7] `cd rust && (trigger/tc timeout 300 cargo geiger --workspace 2>/dev/null || true)` — exit 0
- [x] [CMD_RESULT 8] `cd rust && (trigger/tc timeout 300 cargo tarpaulin --workspace 2>&1 | tail -12 || true)` — exit 0
- [x] [CMD_RESULT 9] `../trigger/tc cargo fuzz --version 2>/dev/null || echo "cargo-fuzz not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 10] `../trigger/tc flux --version 2>/dev/null || echo "flux not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 11] `../trigger/tc mirai --version 2>/dev/null || echo "mirai not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 12] `../trigger/tc cargo prusti --version 2>/dev/null || ../trigger/tc prusti-rustc --version 2>/dev/null || echo "prusti not installed (skipped)"` — exit 0

### toolchain 実行 (2026-08-15T01:44:23Z) rc=1
```
==> Release toolchain-v1 が無いためビルドします（初回のみ・数十分）
info: downloading installer
warn: It looks like you have an existing rustup settings file at:
warn: /home/runner/akl-toolchain/rustup/settings.toml
warn: Rustup will install the default toolchain as specified in the settings file,
warn: instead of the one inferred from the default host triple.
info: profile set to minimal
info: default host triple is x86_64-unknown-linux-gnu
info: syncing channel updates for stable-x86_64-unknown-linux-gnu
info: latest update on 2026-07-16 for version 1.97.1 (8bab26f4f 2026-07-14)
info: downloading 3 components
info: default toolchain set to stable-x86_64-unknown-linux-gnu

  stable-x86_64-unknown-linux-gnu installed - rustc 1.97.1 (8bab26f4f 2026-07-14)


Rust is installed now. Great!

To get started you may need to restart your current shell.
This would reload your PATH environment variable to include
Cargo's bin directory (/home/runner/akl-toolchain/cargo/bin).

To configure your current shell, you need to source
the corresponding env file under /home/runner/akl-toolchain/cargo.

This is usually done by running one of the following (note the leading DOT):
. "/home/runner/akl-toolchain/cargo/env"            # For 
sh/bash/zsh/ash/dash/pdksh
source "/home/runner/akl-toolchain/cargo/env.fish"  # For fish
source "/home/runner/akl-toolchain/cargo/env.nu"  # For nushell
source "/home/runner/akl-toolchain/cargo/env.tcsh"  # For tcsh
. "/home/runner/akl-toolchain/cargo/env.ps1"        # For pwsh
source "/home/runner/akl-toolchain/cargo/env.xsh"   # For xonsh
info: syncing channel updates for nightly-x86_64-unknown-linux-gnu
info: latest update on 2026-08-14 for version 1.99.0-nightly (ba28ff76f 2026-08-13)
info: downloading 3 components

  nightly-x86_64-unknown-linux-gnu installed - rustc 1.99.0-nightly (ba28ff76f 2026-08-13)

info: downloading component clippy
info: downloading component rustfmt
info: downloading component miri
rustc 1.97.1 (8bab26f4f 2026-07-14)
cargo 1.97.1 (c980f4866 2026-06-30)
  Installing /home/runner/akl-toolchain/cargo/bin/cargo-kani
  Installing /home/runner/akl-toolchain/cargo/bin/kani
   Installed package `kani-verifier v0.67.0` (executables `cargo-kani`, `kani`)
[0/5] Running Kani first-time setup...
[1/5] Ensuring the existence of: /home/runner/.kani/kani-0.67.0
[2/5] Downloading Kani release bundle: kani-0.67.0-x86_64-unknown-linux-gnu.tar.gz
[3/5] Installing rust toolchain version: nightly-2025-11-21-x86_64-unknown-linux-gnu
info: syncing channel updates for nightly-2025-11-21-x86_64-unknown-linux-gnu
info: latest update on 2025-11-21 for version 1.93.0-nightly (53732d5e0 2025-11-20)
info: downloading 3 components

  nightly-2025-11-21-x86_64-unknown-linux-gnu installed - rustc 1.93.0-nightly (53732d5e0 2025-11-20)

[5/5] Successfully completed Kani first-time setup.
cargo-kani 0.67.0
  Installing /home/runner/akl-toolchain/cargo/bin/cargo-tarpaulin
   Installed package `cargo-tarpaulin v0.37.1` (executable `cargo-tarpaulin`)
  Installing /home/runner/akl-toolchain/cargo/bin/cargo-geiger
   Installed package `cargo-geiger v0.13.0` (executable `cargo-geiger`)
  Installing /home/runner/akl-toolchain/cargo/bin/cargo-fuzz
   Installed package `cargo-fuzz v0.13.2` (executable `cargo-fuzz`)
    Updating git repository `https://github.com/flux-rs/flux`
error: could not find `flux` in https://github.com/flux-rs/flux with version `*`
flux: skip
`cargo install` is only for installing programs, and can't be used with libraries.
To use a library crate, add it as a dependency to a Cargo project with `cargo add`.
mirai: skip
    Updating crates.io index
error: could not find `prusti` in registry `crates-io` with version `=1.0.0`
prusti: skip
==> アーカイブ作成
-rw-r--r-- 1 runner runner 760M Aug 15 01:44 /home/runner/akl-toolchain/akl-toolchain.tar.gz
FATAL: GH_TOKEN がありません（gh release create 不可）
```

### clippy 実行 (2026-08-15T01:44:23Z)
```
    Checking akl-core v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/akl-core)
error[E0308]: mismatched types
   --> akl-core/src/bytecode.rs:230:24
    |
230 |                     Ok(Step::Jump(*tgt))
    |                     -- ^^^^^^^^^^^^^^^^ expected `(Step, usize)`, found `Step`
    |                     |
    |                     arguments to this enum variant are incorrect
    |
    = note: expected tuple `(bytecode::Step, usize)`
                found enum `bytecode::Step`
help: the type constructed contains `bytecode::Step` due to the type of the argument passed
   --> akl-core/src/bytecode.rs:230:21
    |
230 |                     Ok(Step::Jump(*tgt))
    |                     ^^^----------------^
    |                        |
    |                        this argument influences the type of `Ok`
note: tuple variant defined here
   --> /rustc/8bab26f4f68e0e26f0bb7960be334d5b520ea452/library/core/src/result.rs:561:4

error[E0308]: mismatched types
   --> akl-core/src/bytecode.rs:237:20
    |
237 |                 Ok(Step::Jump(*tgt))
    |                 -- ^^^^^^^^^^^^^^^^ expected `(Step, usize)`, found `Step`
    |                 |
    |                 arguments to this enum variant are incorrect
    |
    = note: expected tuple `(bytecode::Step, usize)`
                found enum `bytecode::Step`
help: the type constructed contains `bytecode::Step` due to the type of the argument passed
   --> akl-core/src/bytecode.rs:237:17
    |
237 |                 Ok(Step::Jump(*tgt))
    |                 ^^^----------------^
    |                    |
    |                    this argument influences the type of `Ok`
note: tuple variant defined here
   --> /rustc/8bab26f4f68e0e26f0bb7960be334d5b520ea452/library/core/src/result.rs:561:4

For more information about this error, try `rustc --explain E0308`.
error: could not compile `akl-core` (lib) due to 2 previous errors
```
# 実行結果 (2026-08-15T01:44:44Z)

source: trigger/trigger.md @ 9864920993bd088dd92b45c23a13904193b5dd66

- [ ] [CMD_RESULT 1] `bash trigger/toolchain.sh > /tmp/tc.log 2>&1; rc=$?; { echo ""; echo "### toolchain 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ)) rc=$rc"; echo '```'; tail -80 /tmp/tc.log; echo '```'; } >> trigger/result.md; test $rc -eq 0` — **exit 1**
- [ ] [CMD_RESULT 2] `cd rust && ../trigger/tc timeout 300 cargo build --workspace` — **exit 101**
- [ ] [CMD_RESULT 3] `cd rust && ../trigger/tc timeout 300 cargo test --workspace` — **exit 101**
- [ ] [CMD_RESULT 4] `cd rust && ../trigger/tc timeout 300 cargo clippy --workspace -- -D warnings > /tmp/clippy.log 2>&1; rc=$?; { echo ""; echo "### clippy 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -50 /tmp/clippy.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — **exit 1**
- [ ] [CMD_RESULT 5] `cd rust && ../trigger/tc timeout 600 cargo +nightly miri test --workspace` — **exit 101**
- [ ] [CMD_RESULT 6] `cd rust && ../trigger/tc timeout 900 cargo kani --workspace` — **exit 1**
- [x] [CMD_RESULT 7] `cd rust && (../trigger/tc timeout 300 cargo geiger --workspace 2>/dev/null || true)` — exit 0
- [x] [CMD_RESULT 8] `cd rust && (../trigger/tc timeout 300 cargo tarpaulin --workspace 2>&1 | tail -12 || true)` — exit 0
- [x] [CMD_RESULT 9] `../trigger/tc cargo fuzz --version 2>/dev/null || echo "cargo-fuzz not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 10] `../trigger/tc flux --version 2>/dev/null || echo "flux not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 11] `../trigger/tc mirai --version 2>/dev/null || echo "mirai not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 12] `../trigger/tc cargo prusti --version 2>/dev/null || ../trigger/tc prusti-rustc --version 2>/dev/null || echo "prusti not installed (skipped)"` — exit 0

### toolchain 実行 (2026-08-15T01:57:28Z) rc=0
```
sh/bash/zsh/ash/dash/pdksh
source "/home/runner/akl-toolchain/cargo/env.fish"  # For fish
source "/home/runner/akl-toolchain/cargo/env.nu"  # For nushell
source "/home/runner/akl-toolchain/cargo/env.tcsh"  # For tcsh
. "/home/runner/akl-toolchain/cargo/env.ps1"        # For pwsh
source "/home/runner/akl-toolchain/cargo/env.xsh"   # For xonsh
info: syncing channel updates for nightly-x86_64-unknown-linux-gnu
info: latest update on 2026-08-15 for version 1.99.0-nightly (d453bdd8f 2026-08-14)
info: downloading 3 components

  nightly-x86_64-unknown-linux-gnu installed - rustc 1.99.0-nightly (d453bdd8f 2026-08-14)

info: downloading component clippy
info: downloading component rustfmt
info: downloading component miri
rustc 1.97.1 (8bab26f4f 2026-07-14)
cargo 1.97.1 (c980f4866 2026-06-30)
  Installing /home/runner/akl-toolchain/cargo/bin/cargo-kani
  Installing /home/runner/akl-toolchain/cargo/bin/kani
   Installed package `kani-verifier v0.67.0` (executables `cargo-kani`, `kani`)
[0/5] Running Kani first-time setup...
[1/5] Ensuring the existence of: /home/runner/.kani/kani-0.67.0
[2/5] Downloading Kani release bundle: kani-0.67.0-x86_64-unknown-linux-gnu.tar.gz
[3/5] Installing rust toolchain version: nightly-2025-11-21-x86_64-unknown-linux-gnu
info: syncing channel updates for nightly-2025-11-21-x86_64-unknown-linux-gnu
info: latest update on 2025-11-21 for version 1.93.0-nightly (53732d5e0 2025-11-20)
info: downloading 3 components

  nightly-2025-11-21-x86_64-unknown-linux-gnu installed - rustc 1.93.0-nightly (53732d5e0 2025-11-20)

[5/5] Successfully completed Kani first-time setup.
cargo-kani 0.67.0
  Installing /home/runner/akl-toolchain/cargo/bin/cargo-tarpaulin
   Installed package `cargo-tarpaulin v0.37.1` (executable `cargo-tarpaulin`)
  Installing /home/runner/akl-toolchain/cargo/bin/cargo-geiger
   Installed package `cargo-geiger v0.13.0` (executable `cargo-geiger`)
  Installing /home/runner/akl-toolchain/cargo/bin/cargo-fuzz
   Installed package `cargo-fuzz v0.13.2` (executable `cargo-fuzz`)
    Updating git repository `https://github.com/flux-rs/flux`
error: could not find `flux` in https://github.com/flux-rs/flux with version `*`
flux: skip
`cargo install` is only for installing programs, and can't be used with libraries.
To use a library crate, add it as a dependency to a Cargo project with `cargo add`.
mirai: skip
    Updating crates.io index
error: could not find `prusti` in registry `crates-io` with version `=1.0.0`
prusti: skip
==> アーカイブ作成
-rw-r--r-- 1 runner runner 760M Aug 15 01:55 /home/runner/akl-toolchain/akl-toolchain.tar.gz
==> 分割: 9 個
Preparing worktree (new branch 'toolchain-bin')
HEAD is now at 87b5e11 trigger: git ブランチ方式 + bytecode 型修正の検証を実行
[toolchain-bin 7ba4ffc] toolchain toolchain-v1 parts [skip ci]
 9 files changed, 0 insertions(+), 0 deletions(-)
 create mode 100644 part_00
 create mode 100644 part_01
 create mode 100644 part_02
 create mode 100644 part_03
 create mode 100644 part_04
 create mode 100644 part_05
 create mode 100644 part_06
 create mode 100644 part_07
 create mode 100644 part_08
remote: warning: See https://gh.io/lfs for more information.        
remote: warning: File part_02 is 90.00 MB; this is larger than GitHub's recommended maximum file size of 50.00 MB        
remote: warning: File part_03 is 90.00 MB; this is larger than GitHub's recommended maximum file size of 50.00 MB        
remote: warning: File part_04 is 90.00 MB; this is larger than GitHub's recommended maximum file size of 50.00 MB        
remote: warning: File part_05 is 90.00 MB; this is larger than GitHub's recommended maximum file size of 50.00 MB        
remote: warning: File part_06 is 90.00 MB; this is larger than GitHub's recommended maximum file size of 50.00 MB        
remote: warning: File part_07 is 90.00 MB; this is larger than GitHub's recommended maximum file size of 50.00 MB        
remote: warning: File part_00 is 90.00 MB; this is larger than GitHub's recommended maximum file size of 50.00 MB        
remote: warning: File part_01 is 90.00 MB; this is larger than GitHub's recommended maximum file size of 50.00 MB        
remote: warning: GH001: Large files detected. You may want to try Git Large File Storage - https://git-lfs.github.com.        
remote: 
remote: Create a pull request for 'toolchain-bin' on GitHub by visiting:        
remote:      https://github.com/ifuto/Ifuto-Browser/pull/new/toolchain-bin        
remote: 
To https://github.com/ifuto/Ifuto-Browser
 * [new branch]      toolchain-bin -> toolchain-bin
==> ブランチ toolchain-bin へのアップロード完了
```

### clippy 実行 (2026-08-15T01:57:29Z)
```
    Checking akl-core v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/akl-core)
    Finished `dev` profile [unoptimized + debuginfo] target(s) in 0.12s
```
# 実行結果 (2026-08-15T01:57:54Z)

source: trigger/trigger.md @ 87b5e11b877c6fe3fb9fb23bbc94e7cf0c12f8d1

- [x] [CMD_RESULT 1] `bash trigger/toolchain.sh > /tmp/tc.log 2>&1; rc=$?; { echo ""; echo "### toolchain 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ)) rc=$rc"; echo '```'; tail -80 /tmp/tc.log; echo '```'; } >> trigger/result.md; test $rc -eq 0` — exit 0
- [x] [CMD_RESULT 2] `cd rust && ../trigger/tc timeout 300 cargo build --workspace` — exit 0
- [x] [CMD_RESULT 3] `cd rust && ../trigger/tc timeout 300 cargo test --workspace` — exit 0
- [x] [CMD_RESULT 4] `cd rust && ../trigger/tc timeout 300 cargo clippy --workspace -- -D warnings > /tmp/clippy.log 2>&1; rc=$?; { echo ""; echo "### clippy 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -50 /tmp/clippy.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — exit 0
- [x] [CMD_RESULT 5] `cd rust && ../trigger/tc timeout 600 cargo +nightly miri test --workspace` — exit 0
- [x] [CMD_RESULT 6] `cd rust && ../trigger/tc timeout 900 cargo kani --workspace` — exit 0
- [x] [CMD_RESULT 7] `cd rust && (../trigger/tc timeout 300 cargo geiger --workspace 2>/dev/null || true)` — exit 0
- [x] [CMD_RESULT 8] `cd rust && (../trigger/tc timeout 300 cargo tarpaulin --workspace 2>&1 | tail -12 || true)` — exit 0
- [x] [CMD_RESULT 9] `../trigger/tc cargo fuzz --version 2>/dev/null || echo "cargo-fuzz not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 10] `../trigger/tc flux --version 2>/dev/null || echo "flux not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 11] `../trigger/tc mirai --version 2>/dev/null || echo "mirai not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 12] `../trigger/tc cargo prusti --version 2>/dev/null || ../trigger/tc prusti-rustc --version 2>/dev/null || echo "prusti not installed (skipped)"` — exit 0

### toolchain 実行 (2026-08-15T02:06:36Z) rc=0
```
==> ブランチ toolchain-bin にアーカイブ（9 分割）があります。取得して展開します
==> 展開完了
rustc 1.97.1 (8bab26f4f 2026-07-14)
==> rust-src を追加してアーカイブを更新します
info: downloading component rust-src
branch 'toolchain-bin' set up to track 'origin/toolchain-bin'.
HEAD is now at 7ba4ffc toolchain toolchain-v1 parts [skip ci]
[toolchain-bin c9debb6] toolchain toolchain-v1 parts + rust-src [skip ci]
 9 files changed, 0 insertions(+), 0 deletions(-)
remote: warning: See https://gh.io/lfs for more information.        
remote: warning: File part_00 is 90.00 MB; this is larger than GitHub's recommended maximum file size of 50.00 MB        
remote: warning: File part_01 is 90.00 MB; this is larger than GitHub's recommended maximum file size of 50.00 MB        
remote: warning: File part_02 is 90.00 MB; this is larger than GitHub's recommended maximum file size of 50.00 MB        
remote: warning: File part_03 is 90.00 MB; this is larger than GitHub's recommended maximum file size of 50.00 MB        
remote: warning: File part_04 is 90.00 MB; this is larger than GitHub's recommended maximum file size of 50.00 MB        
remote: warning: File part_05 is 90.00 MB; this is larger than GitHub's recommended maximum file size of 50.00 MB        
remote: warning: File part_06 is 90.00 MB; this is larger than GitHub's recommended maximum file size of 50.00 MB        
remote: warning: File part_07 is 90.00 MB; this is larger than GitHub's recommended maximum file size of 50.00 MB        
remote: warning: GH001: Large files detected. You may want to try Git Large File Storage - https://git-lfs.github.com.        
To https://github.com/ifuto/Ifuto-Browser
   7ba4ffc..c9debb6  toolchain-bin -> toolchain-bin
==> ブランチ更新完了（rust-src 込み）
```

### clippy 実行 (2026-08-15T02:06:37Z)
```
    Checking akl-core v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/akl-core)
    Finished `dev` profile [unoptimized + debuginfo] target(s) in 0.15s
```
# 実行結果 (2026-08-15T02:07:04Z)

source: trigger/trigger.md @ 2cc4d6d26673541a43f93cc6dba06050f527ec0b

- [x] [CMD_RESULT 1] `bash trigger/toolchain.sh > /tmp/tc.log 2>&1; rc=$?; { echo ""; echo "### toolchain 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ)) rc=$rc"; echo '```'; tail -80 /tmp/tc.log; echo '```'; } >> trigger/result.md; test $rc -eq 0` — exit 0
- [x] [CMD_RESULT 2] `cd rust && ../trigger/tc timeout 300 cargo build --workspace` — exit 0
- [x] [CMD_RESULT 3] `cd rust && ../trigger/tc timeout 300 cargo test --workspace` — exit 0
- [x] [CMD_RESULT 4] `cd rust && ../trigger/tc timeout 300 cargo clippy --workspace -- -D warnings > /tmp/clippy.log 2>&1; rc=$?; { echo ""; echo "### clippy 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -50 /tmp/clippy.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — exit 0
- [x] [CMD_RESULT 5] `cd rust && ../trigger/tc timeout 600 cargo +nightly miri test --workspace` — exit 0
- [x] [CMD_RESULT 6] `cd rust && ../trigger/tc timeout 900 cargo kani --workspace` — exit 0
- [x] [CMD_RESULT 7] `cd rust && (../trigger/tc timeout 300 cargo geiger --workspace 2>/dev/null || true)` — exit 0
- [x] [CMD_RESULT 8] `cd rust && (../trigger/tc timeout 300 cargo tarpaulin --workspace 2>&1 | tail -12 || true)` — exit 0
- [x] [CMD_RESULT 9] `../trigger/tc cargo fuzz --version 2>/dev/null || echo "cargo-fuzz not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 10] `../trigger/tc flux --version 2>/dev/null || echo "flux not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 11] `../trigger/tc mirai --version 2>/dev/null || echo "mirai not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 12] `../trigger/tc cargo prusti --version 2>/dev/null || ../trigger/tc prusti-rustc --version 2>/dev/null || echo "prusti not installed (skipped)"` — exit 0

### toolchain 実行 (2026-08-27T22:58:18Z) rc=0
```
==> ブランチ toolchain-bin にアーカイブ（9 分割）があります。取得して展開します
==> 展開完了
rustc 1.98.0 (88d9e12ae 2026-08-18)
```

### clippy 実行 (2026-08-27T22:58:42Z)
```
   Compiling ifuto-ffi v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-ffi)
    Checking akl-core v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/akl-core)
    Checking ifuto-core v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-core)
    Checking akl-ffi v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/akl-ffi)
    Checking ifuto-cli v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-cli)
    Finished `dev` profile [unoptimized + debuginfo] target(s) in 9.68s
```
# 実行結果 (2026-08-27T23:07:08Z)

source: trigger/trigger.md @ 59de005816d1fa50c0f16784dd68a7c8cb62afae

- [x] [CMD_RESULT 1] `bash trigger/toolchain.sh > /tmp/tc.log 2>&1; rc=$?; { echo ""; echo "### toolchain 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ)) rc=$rc"; echo '```'; tail -80 /tmp/tc.log; echo '```'; } >> trigger/result.md; test $rc -eq 0` — exit 0
- [x] [CMD_RESULT 2] `cd rust && ../trigger/tc timeout 300 cargo build --workspace` — exit 0
- [x] [CMD_RESULT 3] `cd rust && ../trigger/tc timeout 300 cargo test --workspace` — exit 0
- [x] [CMD_RESULT 4] `cd rust && ../trigger/tc timeout 300 cargo clippy --workspace -- -D warnings > /tmp/clippy.log 2>&1; rc=$?; { echo ""; echo "### clippy 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -50 /tmp/clippy.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — exit 0
- [ ] [CMD_RESULT 5] `cd rust && ../trigger/tc timeout 600 cargo +nightly miri test --workspace` — **exit 101**
- [x] [CMD_RESULT 6] `cd rust && ../trigger/tc timeout 900 cargo kani --workspace` — exit 0
- [x] [CMD_RESULT 7] `cd rust && (../trigger/tc timeout 300 cargo geiger --workspace 2>/dev/null || true)` — exit 0
- [x] [CMD_RESULT 8] `cd rust && (../trigger/tc timeout 300 cargo tarpaulin --workspace 2>&1 | tail -12 || true)` — exit 0
- [x] [CMD_RESULT 9] `../trigger/tc cargo fuzz --version 2>/dev/null || echo "cargo-fuzz not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 10] `../trigger/tc flux --version 2>/dev/null || echo "flux not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 11] `../trigger/tc mirai --version 2>/dev/null || echo "mirai not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 12] `../trigger/tc cargo prusti --version 2>/dev/null || ../trigger/tc prusti-rustc --version 2>/dev/null || echo "prusti not installed (skipped)"` — exit 0

### toolchain 実行 (2026-08-28T05:29:11Z) rc=0
```
==> ブランチ toolchain-bin にアーカイブ（9 分割）があります。取得して展開します
==> 展開完了
rustc 1.98.0 (88d9e12ae 2026-08-18)
```

### clippy 実行 (2026-08-28T05:29:34Z)
```
    Checking akl-core v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/akl-core)
   Compiling ifuto-ffi v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-ffi)
    Checking ifuto-core v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-core)
    Checking akl-ffi v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/akl-ffi)
    Checking ifuto-cli v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-cli)
    Finished `dev` profile [unoptimized + debuginfo] target(s) in 8.49s
```

### miri 実行 (2026-08-28T05:37:38Z)
```
test lexer::tests::numbers_decimal ... FAILED
test lexer::tests::numbers_radix ... ok
test lexer::tests::punctuators_longest_match ... ok
test lexer::tests::strings_and_escapes ... ok
test lexer::tests::template_literals ... ok
test lexer::tests::unterminated_string_errors ... ok
test obj::tests::alloc_reuses_free_list ... ok
test obj::tests::arr_map_basic ... ok
test obj::tests::arr_map_snapshot_semantics ... ok
test obj::tests::env_parent_chain ... ok
test obj::tests::func_env_is_child ... ok
test obj::tests::gc_preserves_reachable ... ok
test obj::tests::gc_sweeps_unreachable_chain ... ok
test obj::tests::get_bounds_safe ... ok
test obj::tests::prop_name_is_child ... ok
test obj::tests::prop_set_get_roundtrip ... ok
test parser::tests::array_literal ... ok
test parser::tests::binary_precedence ... ok
test parser::tests::compound_assign_and_incdec ... ok
test parser::tests::equality_and_relational ... ok
test parser::tests::for_do_while_break_continue ... ok
test parser::tests::func_decl_and_call ... ok
test parser::tests::func_expr_and_switch ... ok
test parser::tests::generator_and_async_decl ... ok
test parser::tests::if_while_block ... ok
test parser::tests::import_export ... ok
test parser::tests::index_and_member_assign ... ok
test parser::tests::literals ... ok
test parser::tests::member_and_index_access ... ok
test parser::tests::object_literal ... ok
test parser::tests::regex_literal ... ok
test parser::tests::ternary ... ok
test parser::tests::this_and_arrow ... ok
test parser::tests::unary ... ok
test parser::tests::unsupported_errors ... ok
test parser::tests::var_decl_and_assign ... ok
test regex::tests::alternation_and_group ... ok
test regex::tests::anchors ... ok
test regex::tests::char_class ... ok
test regex::tests::dot_star ... ok
test regex::tests::ignore_case ... ok
test regex::tests::literal_match ... ok
test string::tests::intern_distinct ... ok
test string::tests::intern_identity ... ok
test string::tests::interned_is_heap_str ... ok
test string::tests::len_tracks ... ok
test string::tests::lookup_works ... ok
test tests::f64_roundtrip ... ok
test tests::int_add_known ... ok
test tests::int_roundtrip_known ... ok
test tests::nan_normalized ... ok
test tests::obj_roundtrip_known ... ok
test tests::tag_values ... ok

failures:

---- builtins::tests::math stdout ----

thread 'builtins::tests::math' (1023) panicked at akl-core/src/builtins.rs:3486:9:
assertion `left == right` failed
  left: AklVal(4652218415073722362)
 right: AklVal(4652218415073722368)
note: run with `RUST_BACKTRACE=1` environment variable to display a backtrace
note: in Miri, you may have to set `MIRIFLAGS=-Zmiri-env-forward=RUST_BACKTRACE` for the environment variable to have an effect

---- lexer::tests::numbers_decimal stdout ----

thread 'lexer::tests::numbers_decimal' (1090) panicked at akl-core/src/lexer.rs:936:9:
assertion `left == right` failed
  left: [Num(Float(0.015000000000000003)), Eof]
 right: [Num(Float(0.015)), Eof]


failures:
    builtins::tests::math
    lexer::tests::numbers_decimal

test result: FAILED. 140 passed; 2 failed; 0 ignored; 0 measured; 0 filtered out; finished in 638.03s

error: test failed, to rerun pass `-p akl-core --lib`
```
# 実行結果 (2026-08-28T05:38:24Z)

source: trigger/trigger.md @ d168584489cd710cb9367e6719e429d800c140ae

- [x] [CMD_RESULT 1] `bash trigger/toolchain.sh > /tmp/tc.log 2>&1; rc=$?; { echo ""; echo "### toolchain 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ)) rc=$rc"; echo '```'; tail -80 /tmp/tc.log; echo '```'; } >> trigger/result.md; test $rc -eq 0` — exit 0
- [x] [CMD_RESULT 2] `cd rust && ../trigger/tc timeout 300 cargo build --workspace` — exit 0
- [x] [CMD_RESULT 3] `cd rust && ../trigger/tc timeout 300 cargo test --workspace` — exit 0
- [x] [CMD_RESULT 4] `cd rust && ../trigger/tc timeout 300 cargo clippy --workspace -- -D warnings > /tmp/clippy.log 2>&1; rc=$?; { echo ""; echo "### clippy 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -50 /tmp/clippy.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — exit 0
- [ ] [CMD_RESULT 5] `cd rust && ../trigger/tc timeout 600 cargo +nightly miri test --workspace > /tmp/miri.log 2>&1; rc=$?; { echo ""; echo "### miri 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -80 /tmp/miri.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — **exit 1**
- [x] [CMD_RESULT 6] `cd rust && ../trigger/tc timeout 900 cargo kani --workspace` — exit 0
- [x] [CMD_RESULT 7] `cd rust && (../trigger/tc timeout 300 cargo geiger --workspace 2>/dev/null || true)` — exit 0
- [x] [CMD_RESULT 8] `cd rust && (../trigger/tc timeout 300 cargo tarpaulin --workspace 2>&1 | tail -12 || true)` — exit 0
- [x] [CMD_RESULT 9] `../trigger/tc cargo fuzz --version 2>/dev/null || echo "cargo-fuzz not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 10] `../trigger/tc flux --version 2>/dev/null || echo "flux not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 11] `../trigger/tc mirai --version 2>/dev/null || echo "mirai not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 12] `../trigger/tc cargo prusti --version 2>/dev/null || ../trigger/tc prusti-rustc --version 2>/dev/null || echo "prusti not installed (skipped)"` — exit 0

### toolchain 実行 (2026-08-28T06:15:53Z) rc=0
```
==> ブランチ toolchain-bin にアーカイブ（9 分割）があります。取得して展開します
==> 展開完了
rustc 1.98.0 (88d9e12ae 2026-08-18)
```

### clippy 実行 (2026-08-28T06:16:17Z)
```
    Checking akl-core v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/akl-core)
   Compiling ifuto-ffi v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-ffi)
    Checking ifuto-core v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-core)
    Checking akl-ffi v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/akl-ffi)
    Checking ifuto-cli v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-cli)
    Finished `dev` profile [unoptimized + debuginfo] target(s) in 8.31s
```

### miri 実行 (2026-08-28T06:22:50Z)
```
1221 |             unsafe { akl_mkhandle(rt, vt as *const CAklHandleVTab, 0xdead as *mut c_void) };
     |                                                                    ^^^^^^^^^^^^^^^^^^^^^ integer-to-pointer cast
     |
     = help: this program is using integer-to-pointer casts or (equivalently) `ptr::with_exposed_provenance`, which means that Miri might miss pointer bugs in this program
     = help: see https://doc.rust-lang.org/nightly/std/ptr/fn.with_exposed_provenance.html for more details on that operation
     = help: to ensure that Miri does not miss bugs in your program, use Strict Provenance APIs (https://doc.rust-lang.org/nightly/std/ptr/index.html#strict-provenance, https://crates.io/crates/sptr) instead
     = help: you can then set `MIRIFLAGS=-Zmiri-strict-provenance` to ensure you are not relying on `with_exposed_provenance` semantics
     = help: alternatively, `MIRIFLAGS=-Zmiri-permissive-provenance` disables this warning
     = note: this is on thread `tests::handle_r`
     = note: stack backtrace:
             0: tests::handle_roundtrip
                 at akl-ffi/src/lib.rs:1221:68: 1221:89
             1: tests::handle_roundtrip::{closure#0}
                 at akl-ffi/src/lib.rs:1191:26: 1191:26

warning: integer-to-pointer cast
   --> akl-ffi/src/lib.rs:201:19
    |
201 |     let wrapper = rt.host_ctx as *mut AklRT;
    |                   ^^^^^^^^^^^^^^^^^^^^^^^^^ integer-to-pointer cast
    |
    = note: this is on thread `tests::handle_r`
    = note: stack backtrace:
            0: handle_get_adapter
                at akl-ffi/src/lib.rs:201:19: 201:44
            1: akl_core::bytecode::Runtime::run_loop
                at akl-core/src/bytecode.rs:1625:33: 1625:71
            2: akl_core::bytecode::Runtime::run_with_this_env
                at akl-core/src/bytecode.rs:1014:15: 1014:53
            3: akl_core::bytecode::Runtime::run_with_this
                at akl-core/src/bytecode.rs:969:9: 969:65
            4: akl_core::bytecode::Runtime::run
                at akl-core/src/bytecode.rs:959:9: 959:58
            5: akl_eval
                at akl-ffi/src/lib.rs:404:11: 404:31
            6: tests::eval
                at akl-ffi/src/lib.rs:1028:27: 1028:61
            7: tests::handle_roundtrip
                at akl-ffi/src/lib.rs:1227:23: 1227:50
            8: tests::handle_roundtrip::{closure#0}
                at akl-ffi/src/lib.rs:1191:26: 1191:26

error: Undefined Behavior: not granting access to tag <wildcard> because that would remove [Unique for <2595476>] which is strongly protected
   --> akl-ffi/src/lib.rs:203:22
    |
203 |     let w = unsafe { &*wrapper };
    |                      ^^^^^^^^^ Undefined Behavior occurred here
    |
    = help: this indicates a potential bug in the program: it performed an invalid operation, but the Stacked Borrows rules it violated are still experimental
    = help: see https://github.com/rust-lang/unsafe-code-guidelines/blob/master/wip/stacked-borrows.md for further information
    = note: this is on thread `tests::handle_r`
    = note: stack backtrace:
            0: handle_get_adapter
                at akl-ffi/src/lib.rs:203:22: 203:31
            1: akl_core::bytecode::Runtime::run_loop
                at akl-core/src/bytecode.rs:1625:33: 1625:71
            2: akl_core::bytecode::Runtime::run_with_this_env
                at akl-core/src/bytecode.rs:1014:15: 1014:53
            3: akl_core::bytecode::Runtime::run_with_this
                at akl-core/src/bytecode.rs:969:9: 969:65
            4: akl_core::bytecode::Runtime::run
                at akl-core/src/bytecode.rs:959:9: 959:58
            5: akl_eval
                at akl-ffi/src/lib.rs:404:11: 404:31
            6: tests::eval
                at akl-ffi/src/lib.rs:1028:27: 1028:61
            7: tests::handle_roundtrip
                at akl-ffi/src/lib.rs:1227:23: 1227:50
            8: tests::handle_roundtrip::{closure#0}
                at akl-ffi/src/lib.rs:1191:26: 1191:26

note: some details are omitted, run with `MIRIFLAGS=-Zmiri-backtrace=full` for a verbose backtrace

error: aborting due to 1 previous error; 2 warnings emitted

error: test failed, to rerun pass `-p akl-ffi --lib`

Caused by:
  process didn't exit successfully: `/home/runner/akl-toolchain/rustup/toolchains/nightly-x86_64-unknown-linux-gnu/bin/cargo-miri runner /home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/target/miri/x86_64-unknown-linux-gnu/debug/build/akl-ffi/d1a0b609b8c5069e/out/akl_ffi-d1a0b609b8c5069e` (exit status: 1)
note: test exited abnormally; to see the full output pass --no-capture to the harness.
```
# 実行結果 (2026-08-28T06:23:29Z)

source: trigger/trigger.md @ d355a17fa1ff3489d47874dbebb739a99b76b348

- [x] [CMD_RESULT 1] `bash trigger/toolchain.sh > /tmp/tc.log 2>&1; rc=$?; { echo ""; echo "### toolchain 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ)) rc=$rc"; echo '```'; tail -80 /tmp/tc.log; echo '```'; } >> trigger/result.md; test $rc -eq 0` — exit 0
- [x] [CMD_RESULT 2] `cd rust && ../trigger/tc timeout 300 cargo build --workspace` — exit 0
- [x] [CMD_RESULT 3] `cd rust && ../trigger/tc timeout 300 cargo test --workspace` — exit 0
- [x] [CMD_RESULT 4] `cd rust && ../trigger/tc timeout 300 cargo clippy --workspace -- -D warnings > /tmp/clippy.log 2>&1; rc=$?; { echo ""; echo "### clippy 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -50 /tmp/clippy.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — exit 0
- [ ] [CMD_RESULT 5] `cd rust && ../trigger/tc timeout 600 cargo +nightly miri test --workspace > /tmp/miri.log 2>&1; rc=$?; { echo ""; echo "### miri 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -80 /tmp/miri.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — **exit 1**
- [x] [CMD_RESULT 6] `cd rust && ../trigger/tc timeout 900 cargo kani --workspace` — exit 0
- [x] [CMD_RESULT 7] `cd rust && (../trigger/tc timeout 300 cargo geiger --workspace 2>/dev/null || true)` — exit 0
- [x] [CMD_RESULT 8] `cd rust && (../trigger/tc timeout 300 cargo tarpaulin --workspace 2>&1 | tail -12 || true)` — exit 0
- [x] [CMD_RESULT 9] `../trigger/tc cargo fuzz --version 2>/dev/null || echo "cargo-fuzz not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 10] `../trigger/tc flux --version 2>/dev/null || echo "flux not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 11] `../trigger/tc mirai --version 2>/dev/null || echo "mirai not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 12] `../trigger/tc cargo prusti --version 2>/dev/null || ../trigger/tc prusti-rustc --version 2>/dev/null || echo "prusti not installed (skipped)"` — exit 0

### toolchain 実行 (2026-08-28T13:31:31Z) rc=0
```
==> ブランチ toolchain-bin にアーカイブ（9 分割）があります。取得して展開します
==> 展開完了
rustc 1.98.0 (88d9e12ae 2026-08-18)
```

### clippy 実行 (2026-08-28T13:31:54Z)
```
   Compiling ifuto-ffi v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-ffi)
    Checking akl-core v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/akl-core)
    Checking ifuto-core v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-core)
    Checking akl-ffi v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/akl-ffi)
    Checking ifuto-cli v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-cli)
    Finished `dev` profile [unoptimized + debuginfo] target(s) in 8.34s
```

### miri 実行 (2026-08-28T13:39:27Z)
```
1221 |             unsafe { akl_mkhandle(rt, vt as *const CAklHandleVTab, 0xdead as *mut c_void) };
     |                                                                    ^^^^^^^^^^^^^^^^^^^^^ integer-to-pointer cast
     |
     = help: this program is using integer-to-pointer casts or (equivalently) `ptr::with_exposed_provenance`, which means that Miri might miss pointer bugs in this program
     = help: see https://doc.rust-lang.org/nightly/std/ptr/fn.with_exposed_provenance.html for more details on that operation
     = help: to ensure that Miri does not miss bugs in your program, use Strict Provenance APIs (https://doc.rust-lang.org/nightly/std/ptr/index.html#strict-provenance, https://crates.io/crates/sptr) instead
     = help: you can then set `MIRIFLAGS=-Zmiri-strict-provenance` to ensure you are not relying on `with_exposed_provenance` semantics
     = help: alternatively, `MIRIFLAGS=-Zmiri-permissive-provenance` disables this warning
     = note: this is on thread `tests::handle_r`
     = note: stack backtrace:
             0: tests::handle_roundtrip
                 at akl-ffi/src/lib.rs:1221:68: 1221:89
             1: tests::handle_roundtrip::{closure#0}
                 at akl-ffi/src/lib.rs:1191:26: 1191:26

warning: integer-to-pointer cast
   --> akl-ffi/src/lib.rs:201:19
    |
201 |     let wrapper = rt.host_ctx as *mut AklRT;
    |                   ^^^^^^^^^^^^^^^^^^^^^^^^^ integer-to-pointer cast
    |
    = note: this is on thread `tests::handle_r`
    = note: stack backtrace:
            0: handle_get_adapter
                at akl-ffi/src/lib.rs:201:19: 201:44
            1: akl_core::bytecode::Runtime::run_loop
                at akl-core/src/bytecode.rs:1625:33: 1625:71
            2: akl_core::bytecode::Runtime::run_with_this_env
                at akl-core/src/bytecode.rs:1014:15: 1014:53
            3: akl_core::bytecode::Runtime::run_with_this
                at akl-core/src/bytecode.rs:969:9: 969:65
            4: akl_core::bytecode::Runtime::run
                at akl-core/src/bytecode.rs:959:9: 959:58
            5: akl_eval
                at akl-ffi/src/lib.rs:404:11: 404:31
            6: tests::eval
                at akl-ffi/src/lib.rs:1028:27: 1028:61
            7: tests::handle_roundtrip
                at akl-ffi/src/lib.rs:1227:23: 1227:50
            8: tests::handle_roundtrip::{closure#0}
                at akl-ffi/src/lib.rs:1191:26: 1191:26

error: Undefined Behavior: not granting access to tag <wildcard> because that would remove [Unique for <2595476>] which is strongly protected
   --> akl-ffi/src/lib.rs:203:22
    |
203 |     let w = unsafe { &*wrapper };
    |                      ^^^^^^^^^ Undefined Behavior occurred here
    |
    = help: this indicates a potential bug in the program: it performed an invalid operation, but the Stacked Borrows rules it violated are still experimental
    = help: see https://github.com/rust-lang/unsafe-code-guidelines/blob/master/wip/stacked-borrows.md for further information
    = note: this is on thread `tests::handle_r`
    = note: stack backtrace:
            0: handle_get_adapter
                at akl-ffi/src/lib.rs:203:22: 203:31
            1: akl_core::bytecode::Runtime::run_loop
                at akl-core/src/bytecode.rs:1625:33: 1625:71
            2: akl_core::bytecode::Runtime::run_with_this_env
                at akl-core/src/bytecode.rs:1014:15: 1014:53
            3: akl_core::bytecode::Runtime::run_with_this
                at akl-core/src/bytecode.rs:969:9: 969:65
            4: akl_core::bytecode::Runtime::run
                at akl-core/src/bytecode.rs:959:9: 959:58
            5: akl_eval
                at akl-ffi/src/lib.rs:404:11: 404:31
            6: tests::eval
                at akl-ffi/src/lib.rs:1028:27: 1028:61
            7: tests::handle_roundtrip
                at akl-ffi/src/lib.rs:1227:23: 1227:50
            8: tests::handle_roundtrip::{closure#0}
                at akl-ffi/src/lib.rs:1191:26: 1191:26

note: some details are omitted, run with `MIRIFLAGS=-Zmiri-backtrace=full` for a verbose backtrace

error: aborting due to 1 previous error; 2 warnings emitted

error: test failed, to rerun pass `-p akl-ffi --lib`

Caused by:
  process didn't exit successfully: `/home/runner/akl-toolchain/rustup/toolchains/nightly-x86_64-unknown-linux-gnu/bin/cargo-miri runner /home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/target/miri/x86_64-unknown-linux-gnu/debug/build/akl-ffi/d1a0b609b8c5069e/out/akl_ffi-d1a0b609b8c5069e` (exit status: 1)
note: test exited abnormally; to see the full output pass --no-capture to the harness.
```
# 実行結果 (2026-08-28T13:40:16Z)

source: trigger/trigger.md @ b87904525172bda92a4b974d142f413bb7ddb68c

- [x] [CMD_RESULT 1] `bash trigger/toolchain.sh > /tmp/tc.log 2>&1; rc=$?; { echo ""; echo "### toolchain 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ)) rc=$rc"; echo '```'; tail -80 /tmp/tc.log; echo '```'; } >> trigger/result.md; test $rc -eq 0` — exit 0
- [x] [CMD_RESULT 2] `cd rust && ../trigger/tc timeout 300 cargo build --workspace` — exit 0
- [x] [CMD_RESULT 3] `cd rust && ../trigger/tc timeout 300 cargo test --workspace` — exit 0
- [x] [CMD_RESULT 4] `cd rust && ../trigger/tc timeout 300 cargo clippy --workspace -- -D warnings > /tmp/clippy.log 2>&1; rc=$?; { echo ""; echo "### clippy 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -50 /tmp/clippy.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — exit 0
- [ ] [CMD_RESULT 5] `cd rust && ../trigger/tc timeout 600 cargo +nightly miri test --workspace > /tmp/miri.log 2>&1; rc=$?; { echo ""; echo "### miri 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -80 /tmp/miri.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — **exit 1**
- [x] [CMD_RESULT 6] `cd rust && ../trigger/tc timeout 900 cargo kani --workspace` — exit 0
- [x] [CMD_RESULT 7] `cd rust && (../trigger/tc timeout 300 cargo geiger --workspace 2>/dev/null || true)` — exit 0
- [x] [CMD_RESULT 8] `cd rust && (../trigger/tc timeout 300 cargo tarpaulin --workspace 2>&1 | tail -12 || true)` — exit 0
- [x] [CMD_RESULT 9] `../trigger/tc cargo fuzz --version 2>/dev/null || echo "cargo-fuzz not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 10] `../trigger/tc flux --version 2>/dev/null || echo "flux not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 11] `../trigger/tc mirai --version 2>/dev/null || echo "mirai not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 12] `../trigger/tc cargo prusti --version 2>/dev/null || ../trigger/tc prusti-rustc --version 2>/dev/null || echo "prusti not installed (skipped)"` — exit 0

### toolchain 実行 (2026-08-28T14:00:48Z) rc=0
```
==> ブランチ toolchain-bin にアーカイブ（9 分割）があります。取得して展開します
==> 展開完了
rustc 1.98.0 (88d9e12ae 2026-08-18)
```

### clippy 実行 (2026-08-28T14:01:22Z)
```
   Compiling ifuto-ffi v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-ffi)
    Checking akl-core v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/akl-core)
    Checking ifuto-core v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-core)
    Checking akl-ffi v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/akl-ffi)
    Checking ifuto-cli v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-cli)
    Finished `dev` profile [unoptimized + debuginfo] target(s) in 10.33s
```

### miri 実行 (2026-08-28T14:09:07Z)
```
            4: akl_core::bytecode::Runtime::run
                at akl-core/src/bytecode.rs:959:9: 959:58
            5: akl_eval
                at akl-ffi/src/lib.rs:446:11: 446:31
            6: tests::eval
                at akl-ffi/src/lib.rs:1081:27: 1081:61
            7: tests::handle_roundtrip
                at akl-ffi/src/lib.rs:1280:23: 1280:50
            8: tests::handle_roundtrip::{closure#0}
                at akl-ffi/src/lib.rs:1244:26: 1244:26

warning: integer-to-pointer cast
   --> akl-ffi/src/lib.rs:254:13
    |
254 |             ptr as *mut c_void,
    |             ^^^^^^^^^^^^^^^^^^ integer-to-pointer cast
    |
    = note: this is on thread `tests::handle_r`
    = note: stack backtrace:
            0: handle_get_adapter
                at akl-ffi/src/lib.rs:254:13: 254:31
            1: akl_core::bytecode::Runtime::run_loop
                at akl-core/src/bytecode.rs:1625:33: 1625:71
            2: akl_core::bytecode::Runtime::run_with_this_env
                at akl-core/src/bytecode.rs:1014:15: 1014:53
            3: akl_core::bytecode::Runtime::run_with_this
                at akl-core/src/bytecode.rs:969:9: 969:65
            4: akl_core::bytecode::Runtime::run
                at akl-core/src/bytecode.rs:959:9: 959:58
            5: akl_eval
                at akl-ffi/src/lib.rs:446:11: 446:31
            6: tests::eval
                at akl-ffi/src/lib.rs:1081:27: 1081:61
            7: tests::handle_roundtrip
                at akl-ffi/src/lib.rs:1280:23: 1280:50
            8: tests::handle_roundtrip::{closure#0}
                at akl-ffi/src/lib.rs:1244:26: 1244:26

ok
test tests::native_register_and_call ... error: Undefined Behavior: attempting a read access using <3010383> at alloc1042172[0x6], but that tag does not exist in the borrow stack for this location
   --> /home/runner/akl-toolchain/rustup/toolchains/nightly-x86_64-unknown-linux-gnu/lib/rustlib/src/rust/library/core/src/ffi/c_str.rs:772:22
    |
772 |             unsafe { strlen(s) }
    |                      ^^^^^^^^^ this error occurs as part of an access at alloc1042172[0x6..0x7]
    |
    = help: this indicates a potential bug in the program: it performed an invalid operation, but the Stacked Borrows rules it violated are still experimental
    = help: see https://github.com/rust-lang/unsafe-code-guidelines/blob/master/wip/stacked-borrows.md for further information
help: <3010383> was created by a SharedReadOnly retag at offsets [0x0..0x6]
   --> akl-ffi/src/lib.rs:915:33
    |
915 |     unsafe { akl_global_set(rt, name.as_ptr() as *const c_char, v) }
    |                                 ^^^^^^^^^^^^^
    = note: this is on thread `tests::native_r`
    = note: stack backtrace:
            0: core::ffi::c_str::strlen::runtime
                at /home/runner/akl-toolchain/rustup/toolchains/nightly-x86_64-unknown-linux-gnu/lib/rustlib/src/rust/library/core/src/ffi/c_str.rs:772:22: 772:31
            1: core::ffi::c_str::strlen
                at /home/runner/akl-toolchain/rustup/toolchains/nightly-x86_64-unknown-linux-gnu/lib/rustlib/src/rust/library/core/src/intrinsics/mod.rs:2636:9: 2636:61
            2: std::ffi::CStr::from_ptr::<'_>
                at /home/runner/akl-toolchain/rustup/toolchains/nightly-x86_64-unknown-linux-gnu/lib/rustlib/src/rust/library/core/src/ffi/c_str.rs:257:28: 257:39
            3: cstr::<'_>
                at akl-ffi/src/lib.rs:113:22: 113:49
            4: akl_global_set
                at akl-ffi/src/lib.rs:856:25: 856:35
            5: akl_native_register
                at akl-ffi/src/lib.rs:915:14: 915:67
            6: tests::native_register_and_call
                at akl-ffi/src/lib.rs:1223:13: 1223:88
            7: tests::native_register_and_call::{closure#0}
                at akl-ffi/src/lib.rs:1219:34: 1219:34

note: some details are omitted, run with `MIRIFLAGS=-Zmiri-backtrace=full` for a verbose backtrace

error: aborting due to 1 previous error; 3 warnings emitted

error: test failed, to rerun pass `-p akl-ffi --lib`

Caused by:
  process didn't exit successfully: `/home/runner/akl-toolchain/rustup/toolchains/nightly-x86_64-unknown-linux-gnu/bin/cargo-miri runner /home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/target/miri/x86_64-unknown-linux-gnu/debug/build/akl-ffi/d1a0b609b8c5069e/out/akl_ffi-d1a0b609b8c5069e` (exit status: 1)
note: test exited abnormally; to see the full output pass --no-capture to the harness.
```
# 実行結果 (2026-08-28T14:09:56Z)

source: trigger/trigger.md @ 588cd7bfe4d5d89ec15128bef9dec83fc94828db

- [x] [CMD_RESULT 1] `bash trigger/toolchain.sh > /tmp/tc.log 2>&1; rc=$?; { echo ""; echo "### toolchain 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ)) rc=$rc"; echo '```'; tail -80 /tmp/tc.log; echo '```'; } >> trigger/result.md; test $rc -eq 0` — exit 0
- [x] [CMD_RESULT 2] `cd rust && ../trigger/tc timeout 300 cargo build --workspace` — exit 0
- [x] [CMD_RESULT 3] `cd rust && ../trigger/tc timeout 300 cargo test --workspace` — exit 0
- [x] [CMD_RESULT 4] `cd rust && ../trigger/tc timeout 300 cargo clippy --workspace -- -D warnings > /tmp/clippy.log 2>&1; rc=$?; { echo ""; echo "### clippy 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -50 /tmp/clippy.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — exit 0
- [ ] [CMD_RESULT 5] `cd rust && ../trigger/tc timeout 600 cargo +nightly miri test --workspace > /tmp/miri.log 2>&1; rc=$?; { echo ""; echo "### miri 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -80 /tmp/miri.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — **exit 1**
- [x] [CMD_RESULT 6] `cd rust && ../trigger/tc timeout 900 cargo kani --workspace` — exit 0
- [x] [CMD_RESULT 7] `cd rust && (../trigger/tc timeout 300 cargo geiger --workspace 2>/dev/null || true)` — exit 0
- [x] [CMD_RESULT 8] `cd rust && (../trigger/tc timeout 300 cargo tarpaulin --workspace 2>&1 | tail -12 || true)` — exit 0
- [x] [CMD_RESULT 9] `../trigger/tc cargo fuzz --version 2>/dev/null || echo "cargo-fuzz not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 10] `../trigger/tc flux --version 2>/dev/null || echo "flux not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 11] `../trigger/tc mirai --version 2>/dev/null || echo "mirai not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 12] `../trigger/tc cargo prusti --version 2>/dev/null || ../trigger/tc prusti-rustc --version 2>/dev/null || echo "prusti not installed (skipped)"` — exit 0

### toolchain 実行 (2026-08-28T14:14:03Z) rc=0
```
==> ブランチ toolchain-bin にアーカイブ（9 分割）があります。取得して展開します
==> 展開完了
rustc 1.98.0 (88d9e12ae 2026-08-18)
```

### clippy 実行 (2026-08-28T14:14:28Z)
```
    Checking akl-core v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/akl-core)
   Compiling ifuto-ffi v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-ffi)
    Checking ifuto-core v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-core)
    Checking akl-ffi v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/akl-ffi)
    Checking ifuto-cli v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-cli)
    Finished `dev` profile [unoptimized + debuginfo] target(s) in 8.43s
```

### miri 実行 (2026-08-28T14:22:02Z)
```
            1: akl_core::bytecode::Runtime::run_loop
                at akl-core/src/bytecode.rs:1625:33: 1625:71
            2: akl_core::bytecode::Runtime::run_with_this_env
                at akl-core/src/bytecode.rs:1014:15: 1014:53
            3: akl_core::bytecode::Runtime::run_with_this
                at akl-core/src/bytecode.rs:969:9: 969:65
            4: akl_core::bytecode::Runtime::run
                at akl-core/src/bytecode.rs:959:9: 959:58
            5: akl_eval
                at akl-ffi/src/lib.rs:446:11: 446:31
            6: tests::eval
                at akl-ffi/src/lib.rs:1085:27: 1085:61
            7: tests::handle_roundtrip
                at akl-ffi/src/lib.rs:1284:23: 1284:50
            8: tests::handle_roundtrip::{closure#0}
                at akl-ffi/src/lib.rs:1248:26: 1248:26

ok
test tests::native_register_and_call ... warning: integer-to-pointer cast
   --> akl-ffi/src/lib.rs:199:19
    |
199 |     let wrapper = rt.host_ctx as *mut AklRT;
    |                   ^^^^^^^^^^^^^^^^^^^^^^^^^ integer-to-pointer cast
    |
    = note: this is on thread `tests::native_r`
    = note: stack backtrace:
            0: foreign_adapter
                at akl-ffi/src/lib.rs:199:19: 199:44
            1: akl_core::bytecode::Runtime::do_call
                at akl-core/src/bytecode.rs:2122:21: 2122:48
            2: akl_core::bytecode::Runtime::run_loop
                at akl-core/src/bytecode.rs:1239:27: 1239:91
            3: akl_core::bytecode::Runtime::run_with_this_env
                at akl-core/src/bytecode.rs:1014:15: 1014:53
            4: akl_core::bytecode::Runtime::run_with_this
                at akl-core/src/bytecode.rs:969:9: 969:65
            5: akl_core::bytecode::Runtime::run
                at akl-core/src/bytecode.rs:959:9: 959:58
            6: akl_eval
                at akl-ffi/src/lib.rs:446:11: 446:31
            7: tests::eval
                at akl-ffi/src/lib.rs:1085:27: 1085:61
            8: tests::native_register_and_call
                at akl-ffi/src/lib.rs:1231:23: 1231:53
            9: tests::native_register_and_call::{closure#0}
                at akl-ffi/src/lib.rs:1223:34: 1223:34

ok

test result: ok. 6 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out; finished in 27.86s

error: memory leaked: alloc749089 (Rust heap, size: 32, align: 8), allocated here:
    --> akl-ffi/src/lib.rs:1268:28
     |
1268 |           let vt = Box::leak(Box::new(CAklHandleVTab {
     |  ____________________________^
1269 | |             tag: c"TestElem".as_ptr(),
1270 | |             get: Some(t_get),
1271 | |             set: None,
1272 | |             call: None,
1273 | |         }));
     | |__________^
     |
     = note: stack backtrace:
             0: tests::handle_roundtrip
                 at akl-ffi/src/lib.rs:1268:28: 1273:11
             1: tests::handle_roundtrip::{closure#0}
                 at akl-ffi/src/lib.rs:1248:26: 1248:26

note: some details are omitted, run with `MIRIFLAGS=-Zmiri-backtrace=full` for a verbose backtrace

note: set `MIRIFLAGS=-Zmiri-ignore-leaks` to disable this check

error: aborting due to 1 previous error; 4 warnings emitted

error: test failed, to rerun pass `-p akl-ffi --lib`

Caused by:
  process didn't exit successfully: `/home/runner/akl-toolchain/rustup/toolchains/nightly-x86_64-unknown-linux-gnu/bin/cargo-miri runner /home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/target/miri/x86_64-unknown-linux-gnu/debug/build/akl-ffi/d1a0b609b8c5069e/out/akl_ffi-d1a0b609b8c5069e` (exit status: 1)
note: test exited abnormally; to see the full output pass --no-capture to the harness.
```
# 実行結果 (2026-08-28T14:22:49Z)

source: trigger/trigger.md @ 59189648946d7f27382adf67c9579f36db5100be

- [x] [CMD_RESULT 1] `bash trigger/toolchain.sh > /tmp/tc.log 2>&1; rc=$?; { echo ""; echo "### toolchain 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ)) rc=$rc"; echo '```'; tail -80 /tmp/tc.log; echo '```'; } >> trigger/result.md; test $rc -eq 0` — exit 0
- [x] [CMD_RESULT 2] `cd rust && ../trigger/tc timeout 300 cargo build --workspace` — exit 0
- [x] [CMD_RESULT 3] `cd rust && ../trigger/tc timeout 300 cargo test --workspace` — exit 0
- [x] [CMD_RESULT 4] `cd rust && ../trigger/tc timeout 300 cargo clippy --workspace -- -D warnings > /tmp/clippy.log 2>&1; rc=$?; { echo ""; echo "### clippy 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -50 /tmp/clippy.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — exit 0
- [ ] [CMD_RESULT 5] `cd rust && ../trigger/tc timeout 600 cargo +nightly miri test --workspace > /tmp/miri.log 2>&1; rc=$?; { echo ""; echo "### miri 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -80 /tmp/miri.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — **exit 1**
- [x] [CMD_RESULT 6] `cd rust && ../trigger/tc timeout 900 cargo kani --workspace` — exit 0
- [x] [CMD_RESULT 7] `cd rust && (../trigger/tc timeout 300 cargo geiger --workspace 2>/dev/null || true)` — exit 0
- [x] [CMD_RESULT 8] `cd rust && (../trigger/tc timeout 300 cargo tarpaulin --workspace 2>&1 | tail -12 || true)` — exit 0
- [x] [CMD_RESULT 9] `../trigger/tc cargo fuzz --version 2>/dev/null || echo "cargo-fuzz not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 10] `../trigger/tc flux --version 2>/dev/null || echo "flux not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 11] `../trigger/tc mirai --version 2>/dev/null || echo "mirai not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 12] `../trigger/tc cargo prusti --version 2>/dev/null || ../trigger/tc prusti-rustc --version 2>/dev/null || echo "prusti not installed (skipped)"` — exit 0

### toolchain 実行 (2026-08-28T14:27:02Z) rc=0
```
==> ブランチ toolchain-bin にアーカイブ（9 分割）があります。取得して展開します
==> 展開完了
rustc 1.98.0 (88d9e12ae 2026-08-18)
```

### clippy 実行 (2026-08-28T14:27:32Z)
```
    Checking akl-core v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/akl-core)
   Compiling ifuto-ffi v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-ffi)
    Checking ifuto-core v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-core)
    Checking akl-ffi v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/akl-ffi)
    Checking ifuto-cli v0.1.0 (/home/runner/work/Ifuto-Browser/Ifuto-Browser/rust/ifuto-cli)
    Finished `dev` profile [unoptimized + debuginfo] target(s) in 9.63s
```

### miri 実行 (2026-08-28T14:37:32Z)
```
                at akl-ffi/src/lib.rs:1248:26: 1248:26

warning: integer-to-pointer cast
   --> akl-ffi/src/lib.rs:254:13
    |
254 |             ptr as *mut c_void,
    |             ^^^^^^^^^^^^^^^^^^ integer-to-pointer cast
    |
    = note: this is on thread `tests::handle_r`
    = note: stack backtrace:
            0: handle_get_adapter
                at akl-ffi/src/lib.rs:254:13: 254:31
            1: akl_core::bytecode::Runtime::run_loop
                at akl-core/src/bytecode.rs:1625:33: 1625:71
            2: akl_core::bytecode::Runtime::run_with_this_env
                at akl-core/src/bytecode.rs:1014:15: 1014:53
            3: akl_core::bytecode::Runtime::run_with_this
                at akl-core/src/bytecode.rs:969:9: 969:65
            4: akl_core::bytecode::Runtime::run
                at akl-core/src/bytecode.rs:959:9: 959:58
            5: akl_eval
                at akl-ffi/src/lib.rs:446:11: 446:31
            6: tests::eval
                at akl-ffi/src/lib.rs:1085:27: 1085:61
            7: tests::handle_roundtrip
                at akl-ffi/src/lib.rs:1286:23: 1286:50
            8: tests::handle_roundtrip::{closure#0}
                at akl-ffi/src/lib.rs:1248:26: 1248:26

ok
test tests::native_register_and_call ... warning: integer-to-pointer cast
   --> akl-ffi/src/lib.rs:199:19
    |
199 |     let wrapper = rt.host_ctx as *mut AklRT;
    |                   ^^^^^^^^^^^^^^^^^^^^^^^^^ integer-to-pointer cast
    |
    = note: this is on thread `tests::native_r`
    = note: stack backtrace:
            0: foreign_adapter
                at akl-ffi/src/lib.rs:199:19: 199:44
            1: akl_core::bytecode::Runtime::do_call
                at akl-core/src/bytecode.rs:2122:21: 2122:48
            2: akl_core::bytecode::Runtime::run_loop
                at akl-core/src/bytecode.rs:1239:27: 1239:91
            3: akl_core::bytecode::Runtime::run_with_this_env
                at akl-core/src/bytecode.rs:1014:15: 1014:53
            4: akl_core::bytecode::Runtime::run_with_this
                at akl-core/src/bytecode.rs:969:9: 969:65
            5: akl_core::bytecode::Runtime::run
                at akl-core/src/bytecode.rs:959:9: 959:58
            6: akl_eval
                at akl-ffi/src/lib.rs:446:11: 446:31
            7: tests::eval
                at akl-ffi/src/lib.rs:1085:27: 1085:61
            8: tests::native_register_and_call
                at akl-ffi/src/lib.rs:1231:23: 1231:53
            9: tests::native_register_and_call::{closure#0}
                at akl-ffi/src/lib.rs:1223:34: 1223:34

ok

test result: ok. 6 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out; finished in 27.96s

warning: 4 warnings emitted

     Running unittests src/main.rs (target/miri/x86_64-unknown-linux-gnu/debug/build/ifuto-cli/adbc917ba816c3f2/out/ifuto-adbc917ba816c3f2)

running 0 tests

test result: ok. 0 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out; finished in 0.03s

     Running unittests src/lib.rs (target/miri/x86_64-unknown-linux-gnu/debug/build/ifuto-core/63b5096aea252227/out/ifuto_core-63b5096aea252227)

running 183 tests
test charset::tests::oracle_bom_strip_rule ... ok
test charset::tests::oracle_decode_euc ... ok
test charset::tests::oracle_decode_sjis ... ok
test charset::tests::oracle_label ... ok
test charset::tests::oracle_sniff ... ok
test charset::tests::oracle_sweep ... ```
# 実行結果 (2026-08-28T14:38:19Z)

source: trigger/trigger.md @ 75e7dbba1dd26269eb8d8fbdc08bfc8c5b5e9c70

- [x] [CMD_RESULT 1] `bash trigger/toolchain.sh > /tmp/tc.log 2>&1; rc=$?; { echo ""; echo "### toolchain 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ)) rc=$rc"; echo '```'; tail -80 /tmp/tc.log; echo '```'; } >> trigger/result.md; test $rc -eq 0` — exit 0
- [x] [CMD_RESULT 2] `cd rust && ../trigger/tc timeout 300 cargo build --workspace` — exit 0
- [x] [CMD_RESULT 3] `cd rust && ../trigger/tc timeout 300 cargo test --workspace` — exit 0
- [x] [CMD_RESULT 4] `cd rust && ../trigger/tc timeout 300 cargo clippy --workspace -- -D warnings > /tmp/clippy.log 2>&1; rc=$?; { echo ""; echo "### clippy 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -50 /tmp/clippy.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — exit 0
- [ ] [CMD_RESULT 5] `cd rust && ../trigger/tc timeout 600 cargo +nightly miri test --workspace > /tmp/miri.log 2>&1; rc=$?; { echo ""; echo "### miri 実行 ($(date -u +%Y-%m-%dT%H:%M:%SZ))"; echo '```'; tail -80 /tmp/miri.log; echo '```'; } >> ../trigger/result.md; test $rc -eq 0` — **exit 1**
- [x] [CMD_RESULT 6] `cd rust && ../trigger/tc timeout 900 cargo kani --workspace` — exit 0
- [x] [CMD_RESULT 7] `cd rust && (../trigger/tc timeout 300 cargo geiger --workspace 2>/dev/null || true)` — exit 0
- [x] [CMD_RESULT 8] `cd rust && (../trigger/tc timeout 300 cargo tarpaulin --workspace 2>&1 | tail -12 || true)` — exit 0
- [x] [CMD_RESULT 9] `../trigger/tc cargo fuzz --version 2>/dev/null || echo "cargo-fuzz not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 10] `../trigger/tc flux --version 2>/dev/null || echo "flux not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 11] `../trigger/tc mirai --version 2>/dev/null || echo "mirai not installed (skipped)"` — exit 0
- [x] [CMD_RESULT 12] `../trigger/tc cargo prusti --version 2>/dev/null || ../trigger/tc prusti-rustc --version 2>/dev/null || echo "prusti not installed (skipped)"` — exit 0
