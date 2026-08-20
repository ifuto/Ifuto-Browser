//! 共通型・ハードリミット（C の `src/common.h` 相当）。
//!
//! C 実装では `u8`/`u32`/`i32` 等を `uint8_t` 等へ typedef していたが、Rust は
//! ネイティブに同幅の整数型を持つため、型エイリアスは設けず素直に `u8`/`u32`/
//! `i32` を使う（C の `u8` = Rust `u8`、C の `u32` = Rust `u32`、C の `i32` =
//! Rust `i32`。いずれも幅・符号が一致）。
//!
//! # 攻撃的入力へのハードリミット
//!
//! C 実装は「これを超える構造は壊れた文書ではなく攻撃」とみなす上限値を持ち、
//! 各所で fast-fail する。Rust 側でも同一値を維持する（上限の根拠は
//! `src/common.h` の台帳コメントを参照）。

/// 1 ページの入力バイト数上限（512MB）。
pub const IF_MAX_INPUT_BYTES: u64 = 512u64 * 1024 * 1024;
/// 単一 arena ブロック上限（1GB）。
pub const IF_MAX_ARENA_ALLOC: u64 = 1024 * 1024 * 1024;
/// DOM ノード数上限（400 万）。
pub const IF_MAX_DOM_NODES: u32 = 4u32 * 1000 * 1000;
/// ツリー構築の開いている要素スタック深さ上限。
pub const IF_MAX_STACK_DEPTH: u32 = 4096;

/// C の `if_fatal`（`_Noreturn`）相当。fail-fast 方針により、回復不能な状態
/// （OOM・オーバーフロー・異常サイズ）は即座にパニックする。
///
/// C 実装は `abort()` で即死するが、Rust ではパニックが「即死」の安全な代替に
/// なる（テストでは `#[should_panic]` で検証可能、かつスタックトレースが出る）。
#[inline]
pub fn fatal(msg: &str) -> ! {
    panic!("ifuto: fatal: {msg}");
}
