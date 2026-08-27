//! raster fill カーネル（C の `src/raster.c` 相当）。
//!
//! | C (raster.h / raster.c) | Rust |
//! |---|---|
//! | `if_fill32` / `if_fill32_kernel` | [`fill32`] |
//!
//! # 実装済み
//!
//! 32bpp fill（`dst[0..n) = v`）。C は 4 候補カーネル（scalar / u64x2 / u64x8 /
//! smart(memset)）をマイクロベンチで自動選択するが、**全候補は bit-exact に同一**
//! （`tests/test_raster.c` が任意オフセット・任意長・任意色で相互証明する）。よって
//! 本移植では参照実装であるスカラ fill 一本に縮約する（観測不変・選択は速度のみに効く）。
//!
//! # 未移植（性能最適化・観測不変）
//!
//! - 候補カーネル（u64x2 / u64x8 / smart）: 全てスカラと bit-exact 同値。
//! - マイクロベンチ自動選択（`if_raster_autodetect`）: `clock_gettime` + `aligned_alloc`
//!   + `/dev/dri` の stat に依存する機種依存の計測。選択結果は速度にのみ効き、
//!   出力ピクセル列には影響しない。呼び出し側（描画層統合時）で必要なら Rust の
//!   `std::time::Instant` で再実装する。

/// 32bpp fill（`dst[0..n) = v`）。C の `if_fill32` 相当（スカラ参照実装）。
/// 全候補カーネルと bit-exact に同値。
pub fn fill32(dst: &mut [u32], v: u32) {
    for d in dst {
        *d = v;
    }
}

/// 候補カーネル名（C の `KNAME`。診断・表示用）。
pub const KERNEL_NAMES: [&str; 4] = ["u32x1(scalar)", "u64x2(8B)", "u64x8(64B)", "smart(memset)"];

#[cfg(test)]
mod tests {
    use super::*;

    fn ref_fill(dst: &mut [u32], v: u32) {
        for d in dst {
            *d = v;
        }
    }

    #[test]
    fn fill_matches_scalar() {
        // C の test_raster.c の任意オフセット・任意長・任意色で一致を再現
        let vals = [
            0u32,
            0xFFFF_FFFF,
            0xFF_FFFF,
            0x11_1111,
            0x3D5AF1,
            0xF5F5F0,
            0x00FF00,
            0x8000_0001,
        ];
        let ns = [0usize, 1, 2, 3, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 65];
        for &v in &vals {
            for &n in &ns {
                for off in 0..9 {
                    let mut buf = [0xA5A5_A5A5u32; 128];
                    let mut want = [0xA5A5_A5A5u32; 128];
                    fill32(&mut buf[off..off + n], v);
                    ref_fill(&mut want[off..off + n], v);
                    assert_eq!(buf, want, "off={off} n={n} v={v:#x}");
                }
            }
        }
    }

    #[test]
    fn kernel_names_len() {
        assert_eq!(KERNEL_NAMES.len(), 4);
        assert_eq!(KERNEL_NAMES[0], "u32x1(scalar)");
    }
}
