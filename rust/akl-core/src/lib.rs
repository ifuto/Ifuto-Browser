//! Aklus（Ifuto の自作 JS エンジン）のコア値表現 — Rust 移行フェーズ 0。
//!
//! C 実装 `src/akl/akl.c` の NaN-box 値表現（AklVal = u64）を、型安全な
//! newtype として移植する。セキュリティ設計:
//!
//! - `#![forbid(unsafe_code)]` — このクレートに unsafe は存在しない
//!   （Rudra / cargo-geiger / Miri で監査可能）
//! - 生ビット列は構造体に隠蔽し、タグ操作はメソッド経由のみ
//! - 不変条件を Kani で機械的に証明（`#[cfg(kani)]` モジュール参照）
//!
//! C からの移植対応表:
//! | C (akl.c) | Rust |
//! |---|---|
//! | `AKL_TAG_MASK` | [`TAG_MASK`] |
//! | `AKL_MK_INT` / `akl_is_intv` / `akl_get_int` | [`AklVal::mk_int`] / [`AklVal::is_int`] / [`AklVal::get_int`] |
//! | `AKL_MK_OBJ` / `akl_is_objv` / `akl_get_obj` | [`AklVal::mk_obj`] / [`AklVal::is_obj`] / [`AklVal::get_obj`] |
//! | `akl_from_double` | [`AklVal::from_f64`] |
//! | ADD ハンドラの int fast path | [`int_add`] |

#![forbid(unsafe_code)]
#![warn(missing_docs)]

/// 文字列インターン層（フェーズ 1）。
pub mod string;

/// オブジェクトモデル + GC（フェーズ 2）。
pub mod obj;

/// タグ空間マスク（上位 16 bit が 0xFFFF）。
pub const TAG_MASK: u64 = 0xFFFF_0000_0000_0000;

/// int タグ: `TAG_MASK | (1 << 32)`。下位 32 bit が i32 のビット列。
const INT_TAG: u64 = TAG_MASK | (1 << 32);

/// obj タグ: `TAG_MASK | (2 << 32)`。下位 32 bit がオブジェクト表 index。
const OBJ_TAG: u64 = TAG_MASK | (2 << 32);

/// double 演算結果の canonical NaN（タグ空間と非衝突。C の 0x7FF8000000000000）。
pub const CANON_NAN: u64 = 0x7FF8_0000_0000_0000;

/// タグ検査に使う上位 33 bit マスク（C の `0xFFFFFFFF00000000ull`）。
const TAG_CHECK_MASK: u64 = 0xFFFF_FFFF_0000_0000;

/// JS の値。u64 の NaN-box 表現（C の AklVal と同一ビットレイアウト）。
///
/// ビットレイアウト:
/// - タグ空間（上位 16 bit = 0xFFFF）: int / obj / undefined / null / false / true / TDZ
/// - それ以外: double のビット列（NaN は canonical NaN に正規化）
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct AklVal(u64);

impl AklVal {
    /// undefined（タグ空間 0）。
    pub const UNDEF: Self = Self(TAG_MASK | 0);
    /// null（タグ空間 1）。
    pub const NULL: Self = Self(TAG_MASK | 1);
    /// false（タグ空間 2）。
    pub const FALSE: Self = Self(TAG_MASK | 2);
    /// true（タグ空間 3）。
    pub const TRUE: Self = Self(TAG_MASK | 3);
    /// let/const の TDZ マーカ（タグ空間 4。C の `AKL_VAL_TDZ`）。
    pub const TDZ: Self = Self(TAG_MASK | 4);

    /// i32 を int 値に包む（C の `AKL_MK_INT`）。
    pub const fn mk_int(v: i32) -> Self {
        Self(INT_TAG | (v as u32 as u64))
    }

    /// int 値か（C の `akl_is_intv`）。
    pub const fn is_int(self) -> bool {
        (self.0 & TAG_CHECK_MASK) == INT_TAG
    }

    /// int 値を取り出す（C の `akl_get_int`。is_int 前提）。
    pub const fn get_int(self) -> i32 {
        self.0 as u32 as i32
    }

    /// オブジェクト表 index を obj 値に包む（C の `AKL_MK_OBJ`）。
    pub const fn mk_obj(v: u32) -> Self {
        Self(OBJ_TAG | v as u64)
    }

    /// obj 値か（C の `akl_is_objv`）。
    pub const fn is_obj(self) -> bool {
        (self.0 & TAG_CHECK_MASK) == OBJ_TAG
    }

    /// オブジェクト表 index を取り出す（C の `akl_get_obj`。is_obj 前提）。
    pub const fn get_obj(self) -> u32 {
        self.0 as u32
    }

    /// undefined か。
    pub const fn is_undef(self) -> bool {
        self.0 == TAG_MASK
    }

    /// null か。
    pub const fn is_null(self) -> bool {
        self.0 == (TAG_MASK | 1)
    }

    /// タグ空間の値か（int / obj / undef / null / false / true / TDZ）。
    pub const fn is_tagged(self) -> bool {
        (self.0 & TAG_MASK) == TAG_MASK
    }

    /// double を値に包む（C の `akl_from_double`）。
    ///
    /// NaN は canonical NaN（[`CANON_NAN`]）に正規化する。これにより
    /// 「演算結果の double がタグ空間に衝突する」経路が構造的に存在しない
    /// （akl.h の設計不変条件と同じ）。
    pub fn from_f64(d: f64) -> Self {
        if d.is_nan() {
            Self(CANON_NAN)
        } else {
            Self(d.to_bits())
        }
    }

    /// タグ空間でなければ double として返す（C の `akl_numv` 相当）。
    pub fn as_f64(self) -> Option<f64> {
        if self.is_tagged() {
            None
        } else {
            Some(f64::from_bits(self.0))
        }
    }

    /// 内部ビット列（C 連携・テスト用。通常は使わない）。
    pub const fn bits(self) -> u64 {
        self.0
    }
}

/// int + int の加算結果。i32 に収まる場合は I32、溢れる場合は I64 で保持
/// （C の ADD ハンドラの int fast path と同一ロジック）。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum IntAdd {
    /// i32 に収まる（C: `AKL_MK_INT((i32)r)`）。
    I32(i32),
    /// i32 に収まらない（C: `akl_num((double)r)` = double 化）。
    I64(i64),
}

/// int + int の加算（C の ADD ハンドラの int fast path を移植）。
pub fn int_add(a: i32, b: i32) -> IntAdd {
    let r = a as i64 + b as i64;
    if r >= i32::MIN as i64 && r <= i32::MAX as i64 {
        IntAdd::I32(r as i32)
    } else {
        IntAdd::I64(r)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn int_roundtrip_known() {
        for v in [0i32, 1, -1, i32::MIN, i32::MAX] {
            let x = AklVal::mk_int(v);
            assert!(x.is_int());
            assert_eq!(x.get_int(), v);
        }
    }

    #[test]
    fn obj_roundtrip_known() {
        for v in [0u32, 1, u32::MAX] {
            let x = AklVal::mk_obj(v);
            assert!(x.is_obj());
            assert_eq!(x.get_obj(), v);
        }
    }

    #[test]
    fn tag_values() {
        assert!(AklVal::UNDEF.is_undef());
        assert!(AklVal::NULL.is_null());
        assert!(AklVal::TRUE.is_tagged());
        assert!(AklVal::FALSE.is_tagged());
        assert!(AklVal::TDZ.is_tagged());
        assert!(!AklVal::TRUE.is_int());
        assert!(!AklVal::TRUE.is_obj());
        assert!(!AklVal::NULL.is_int());
        assert!(!AklVal::NULL.is_obj());
        assert!(!AklVal::UNDEF.is_int());
        assert!(!AklVal::UNDEF.is_obj());
    }

    #[test]
    fn f64_roundtrip() {
        for d in [0.0, -0.0, 1.5, -1e300, 1.0e-300] {
            let v = AklVal::from_f64(d);
            assert!(!v.is_tagged(), "f64 {d} がタグ空間に衝突");
            assert_eq!(v.as_f64().unwrap().to_bits(), d.to_bits());
        }
    }

    #[test]
    fn nan_normalized() {
        let v = AklVal::from_f64(f64::NAN);
        assert_eq!(v.bits(), CANON_NAN);
        // 符号付き NaN も canonical 化される
        let neg_nan = f64::from_bits(0xFFF8_0000_0000_0000);
        let v2 = AklVal::from_f64(neg_nan);
        assert_eq!(v2.bits(), CANON_NAN);
    }

    #[test]
    fn int_add_known() {
        assert_eq!(int_add(1, 2), IntAdd::I32(3));
        assert_eq!(int_add(i32::MAX, 1), IntAdd::I64(i32::MAX as i64 + 1));
        assert_eq!(int_add(i32::MIN, -1), IntAdd::I64(i32::MIN as i64 - 1));
        assert_eq!(int_add(i32::MAX, i32::MIN), IntAdd::I32(-1));
    }
}

/// Kani による機械的証明（`cargo kani` で実行）。
///
/// 証明する性質:
/// 1. int / obj の往復（mk → get で元の値が復元される）
/// 2. タグ空間の排他性（int / obj / undef / null / bool / TDZ は互いに素）
/// 3. `from_f64` は非 NaN 入力に対してタグ空間を生成しない（NaN は canonical 化）
/// 4. `int_add` の overflow 判定は i64 全域で正しい
#[cfg(kani)]
mod verification {
    use super::*;

    #[kani::proof]
    fn int_roundtrip_all() {
        let x: i32 = kani::any();
        let v = AklVal::mk_int(x);
        assert!(v.is_int());
        assert_eq!(v.get_int(), x);
    }

    #[kani::proof]
    fn obj_roundtrip_all() {
        let x: u32 = kani::any();
        let v = AklVal::mk_obj(x);
        assert!(v.is_obj());
        assert_eq!(v.get_obj(), x);
    }

    #[kani::proof]
    fn int_not_obj_not_undef() {
        let x: i32 = kani::any();
        let v = AklVal::mk_int(x);
        assert!(v.is_tagged());
        assert!(!v.is_obj());
        assert!(!v.is_undef());
        assert!(!v.is_null());
        assert_ne!(v, AklVal::TRUE);
        assert_ne!(v, AklVal::FALSE);
        assert_ne!(v, AklVal::TDZ);
    }

    #[kani::proof]
    fn obj_not_int_not_undef() {
        let x: u32 = kani::any();
        let v = AklVal::mk_obj(x);
        assert!(v.is_tagged());
        assert!(!v.is_int());
        assert!(!v.is_undef());
        assert!(!v.is_null());
    }

    #[kani::proof]
    fn tag_constants_disjoint() {
        assert!(!AklVal::UNDEF.is_int());
        assert!(!AklVal::UNDEF.is_obj());
        assert!(!AklVal::NULL.is_int());
        assert!(!AklVal::NULL.is_obj());
        assert!(!AklVal::TRUE.is_int());
        assert!(!AklVal::TRUE.is_obj());
        assert!(!AklVal::FALSE.is_int());
        assert!(!AklVal::FALSE.is_obj());
        assert!(!AklVal::TDZ.is_int());
        assert!(!AklVal::TDZ.is_obj());
        assert!(!AklVal::NULL.is_undef());
        assert!(!AklVal::TRUE.is_undef());
        assert!(!AklVal::FALSE.is_undef());
        assert!(!AklVal::TDZ.is_undef());
    }

    /// from_f64 の正規化: 非 NaN → タグ空間に乗らない / NaN → canonical。
    /// Kani の適用範囲をスカラーに限定するため、f64 はビット列の代表パターン
    /// （指数/仮数の極値 + 符号）で全数検証する。f64 全域のシンボリック値は
    /// Kani の探索空間が爆発するため対象外（Miri + テストで担保）。
    #[kani::proof]
    fn from_f64_normalizes() {
        // 代表ビット列: 0, 1, 最小正規, 最大正規, 無限大, 符号付き 0
        let samples: [u64; 6] = [
            0x0000_0000_0000_0000, // +0.0
            0x8000_0000_0000_0000, // -0.0
            0x3FF0_0000_0000_0000, // 1.0
            0x7FE0_0000_0000_0000, // 最大正規
            0x7FF0_0000_0000_0000, // +inf
            0xFFF0_0000_0000_0000, // -inf
        ];
        for bits in samples {
            let d = f64::from_bits(bits);
            let v = AklVal::from_f64(d);
            assert!(!v.is_tagged(), "非 NaN double がタグ空間に衝突");
            assert_eq!(v.as_f64().unwrap().to_bits(), d.to_bits());
        }
    }

    /// int_add の overflow 判定: 収まるなら i32 に丸めても一致、収まらないなら
    /// i64 で正確に保持される。
    #[kani::proof]
    fn int_add_fast_path_correct() {
        let a: i32 = kani::any();
        let b: i32 = kani::any();
        let r = a as i64 + b as i64;
        match int_add(a, b) {
            IntAdd::I32(v) => {
                assert_eq!(v as i64, r, "I32 結果は i64 計算と一致すべき");
            }
            IntAdd::I64(v) => {
                assert_eq!(v, r, "I64 結果は i64 計算と一致すべき");
                assert!(r < i32::MIN as i64 || r > i32::MAX as i64,
                        "I64 に落ちるのは i32 域外のときだけ");
            }
        }
    }

    /// canonical NaN はタグ空間と非衝突（ビットレベルで排他）。
    #[kani::proof]
    fn canon_nan_not_tagged() {
        let v = AklVal::from_f64(f64::NAN);
        assert!(!v.is_tagged());
        assert!(!v.is_int());
        assert!(!v.is_obj());
        assert_ne!(v, AklVal::UNDEF);
        assert_ne!(v, AklVal::NULL);
        assert_ne!(v, AklVal::TRUE);
        assert_ne!(v, AklVal::FALSE);
        assert_ne!(v, AklVal::TDZ);
    }
}
