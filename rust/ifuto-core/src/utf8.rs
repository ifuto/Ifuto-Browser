//! UTF-8 デコーダ / エンコーダ / 端末セル幅（C の `src/utf8.c` 相当）。
//!
//! C 実装からの移植対応:
//! | C (utf8.c) | Rust |
//! |---|---|
//! | `if_utf8_decode(const u8*, u32 n, u32 *pos)` | [`decode`]（`&[u8]` + `&mut usize`） |
//! | `if_utf8_encode(u32 cp, u8 out[4])` | [`encode`]（`&mut [u8; 4]`） |
//! | `if_glyph_width(u32 cp)` | [`glyph_width`] |
//! | `if_utf8_band_w2(b0, b1, b2)` | [`band_w2`] |
//! | `IF_CP_REPLACEMENT` | [`REPLACEMENT`] |
//!
//! # 不変条件（C 実装と同一）
//!
//! - どんなバイト列でも停止せず、不正列は U+FFFD に置換して前進する。
//! - サロゲート・overlong・U+10FFFF 超過はすべて不正。
//! - `pos` は常に単調増加し、`n` を超えない（無限ループ不能）。
//!
//! # C 実装で実際に起き得たバグと、Rust で構造的に消える理由
//!
//! C では `s[i + j]` の境界を `i + j >= n` の手動検査で守るため、検査漏れが
//! そのまま OOB 読みになる。Rust では `&[u8]` スライスの `get`/添字が境界検査を
//! 保証するため、`decode` 内のすべての読み出しが構造的に安全。

/// 不正バイト列の置換文字（U+FFFD）。
pub const REPLACEMENT: u32 = 0xFFFD;

/// `s[pos]` から 1 コードポイントをデコードし、`pos` を進める。`pos >= len` なら 0。
///
/// WHATWG 流儀のエラー回復: 不正列は U+FFFD を返して可能な限り前進する。
/// overlong（C0/C1）、サロゲート（ED A0..BF）、U+10FFFF 超過（F5..）はすべて不正。
pub fn decode(s: &[u8], pos: &mut usize) -> u32 {
    let i = *pos;
    let n = s.len();
    if i >= n {
        return 0;
    }
    let b0 = s[i];

    if b0 < 0x80 {
        *pos = i + 1;
        return b0 as u32;
    }

    let (need, mut cp, lo, hi): (usize, u32, u8, u8) = if (0xC2..=0xDF).contains(&b0) {
        (1, (b0 & 0x1F) as u32, 0x80, 0xBF)
    } else if (0xE0..=0xEF).contains(&b0) {
        if b0 == 0xE0 {
            (2, (b0 & 0x0F) as u32, 0xA0, 0xBF) // overlong 排除
        } else if b0 == 0xED {
            (2, (b0 & 0x0F) as u32, 0x80, 0x9F) // サロゲート排除
        } else {
            (2, (b0 & 0x0F) as u32, 0x80, 0xBF)
        }
    } else if (0xF0..=0xF4).contains(&b0) {
        if b0 == 0xF0 {
            (3, (b0 & 0x07) as u32, 0x90, 0xBF) // overlong 排除
        } else if b0 == 0xF4 {
            (3, (b0 & 0x07) as u32, 0x80, 0x8F) // U+10FFFF 上限
        } else {
            (3, (b0 & 0x07) as u32, 0x80, 0xBF)
        }
    } else {
        // 孤立継続バイト / C0,C1,F5..FF
        *pos = i + 1;
        return REPLACEMENT;
    };

    for j in 1..=need {
        if i + j >= n {
            // 末尾切断: 残りを全部消費
            *pos = n;
            return REPLACEMENT;
        }
        let bj = s[i + j];
        let l = if j == 1 { lo } else { 0x80 };
        let h = if j == 1 { hi } else { 0xBF };
        if bj < l || bj > h {
            // 不正バイトは次回再解釈
            *pos = i + j;
            return REPLACEMENT;
        }
        cp = (cp << 6) | (bj & 0x3F) as u32;
    }
    *pos = i + need + 1;
    cp
}

/// `cp` を UTF-8 にエンコード。戻り値はバイト数（1..4）。`out` は 4B 必要。
pub fn encode(cp: u32, out: &mut [u8; 4]) -> usize {
    let mut cp = cp;
    if cp < 0x80 {
        out[0] = cp as u8;
        1
    } else if cp < 0x800 {
        out[0] = (0xC0 | (cp >> 6)) as u8;
        out[1] = (0x80 | (cp & 0x3F)) as u8;
        2
    } else if cp < 0x10000 {
        if (0xD800..=0xDFFF).contains(&cp) {
            cp = REPLACEMENT; // 念のため
        }
        out[0] = (0xE0 | (cp >> 12)) as u8;
        out[1] = (0x80 | ((cp >> 6) & 0x3F)) as u8;
        out[2] = (0x80 | (cp & 0x3F)) as u8;
        3
    } else {
        if cp > 0x10FFFF {
            cp = REPLACEMENT;
        }
        out[0] = (0xF0 | (cp >> 18)) as u8;
        out[1] = (0x80 | ((cp >> 12) & 0x3F)) as u8;
        out[2] = (0x80 | ((cp >> 6) & 0x3F)) as u8;
        out[3] = (0x80 | (cp & 0x3F)) as u8;
        4
    }
}

/// 端末セル幅: 0 = 制御/結合, 1 = 通常, 2 = 全角。
pub fn glyph_width(cp: u32) -> i32 {
    if cp < 0x20 {
        return 0;
    }
    if (0x7F..=0x9F).contains(&cp) {
        return 0;
    }
    // 結合文字（表示幅 0）
    if (0x0300..=0x036F).contains(&cp)
        || (0x1AB0..=0x1AFF).contains(&cp)
        || (0x1DC0..=0x1DFF).contains(&cp)
        || (0x20D0..=0x20FF).contains(&cp)
        || (0xFE20..=0xFE2F).contains(&cp)
    {
        return 0;
    }
    // East Asian Wide/Fullwidth の主要レンジ（省略版・表示不能時も安全側に倒れるだけ）
    if (0x1100..=0x115F).contains(&cp) // Hangul Jamo
        || (0x2E80..=0x303E).contains(&cp) // CJK Radicals..CJK Symbols
        || (0x3041..=0x33FF).contains(&cp) // ひらがな・カタカナ・CJK 記号
        || (0x3400..=0x4DBF).contains(&cp) // CJK Ext A
        || (0x4E00..=0x9FFF).contains(&cp) // CJK 統合漢字
        || (0xA000..=0xA4CF).contains(&cp) // Yi
        || (0xAC00..=0xD7A3).contains(&cp) // Hangul Syllables
        || (0xF900..=0xFAFF).contains(&cp) // CJK 互換漢字
        || (0xFE30..=0xFE6F).contains(&cp) // CJK 互換形・小字型
        || (0xFF00..=0xFF60).contains(&cp) // 全角 ASCII・半角カナ境界
        || (0xFFE0..=0xFFE6).contains(&cp) // 全角記号
        || (0x1F300..=0x1F64F).contains(&cp) // Emoji 主要
        || (0x20000..=0x3FFFD).contains(&cp)
    // CJK Ext B+
    {
        return 2;
    }
    1
}

/// 「妥当 3 バイト列 ∧ [`glyph_width`]==2」を lead/継続バイトだけで確定できる帯
/// （高速述語）。真 ⟹ decode 成功 ∧ 幅 2（逆は言えない: 幅 2 の帯はこれより広い。
/// 縮小側は安全）。
///
/// 帯の構成（`glyph_width` の幅 2 レンジの byte 空間写像）:
///
/// - 0x3000-0x33FF: `E3` `b1∈[0x80,0x8F]` ただし U+303F(`E3 80 BF`, 幅 1) と
///   U+3040(`E3 81 80`, 幅 1) は除外
/// - 0x4E00-0x9FFF: `E4` `b1∈[0xB8,0xBF]`（=[0x4E00,0x4FFF]）| `E5..E9`（=[0x5000,0x9FFF]）
#[inline]
pub fn band_w2(b0: u8, b1: u8, b2: u8) -> bool {
    if (b2 & 0xC0) != 0x80 {
        return false;
    }
    // 0x3000-0x303E（U+303F=幅 1 を除外）、0x3041-0x33FF（0x3040=幅 1 を除外）
    if b0 == 0xE3 {
        return (0x80..=0x8F).contains(&b1)
            && !(b1 == 0x80 && b2 == 0xBF)
            && !(b1 == 0x81 && b2 == 0x80);
    }
    if b0 == 0xE4 {
        return (0xB8..=0xBF).contains(&b1);
    }
    (0xE5..=0xE9).contains(&b0) && (b1 & 0xC0) == 0x80
}

#[cfg(test)]
mod tests {
    use super::*;

    /// C の `tests/test_utf8.c` の `dec1` 相当。
    fn dec1(s: &[u8], pos: &mut usize) -> u32 {
        decode(s, pos)
    }

    /// C の `tests/test_utf8.c` を 1:1 で再現したオラクル。
    #[test]
    fn oracle_mirrors_c() {
        let mut pos;

        // ASCII
        pos = 0;
        assert!(dec1(b"ABC", &mut pos) == b'A' as u32 && pos == 1);

        // 2 バイト: é (U+00E9 = C3 A9)
        pos = 0;
        assert!(dec1(b"\xC3\xA9!", &mut pos) == 0xE9 && pos == 2);

        // 3 バイト: U+3042 (E3 81 82)
        pos = 0;
        assert!(dec1(b"\xE3\x81\x82", &mut pos) == 0x3042 && pos == 3);

        // 4 バイト: U+1F600 (F0 9F 98 80)
        pos = 0;
        assert!(dec1(b"\xF0\x9F\x98\x80", &mut pos) == 0x1F600 && pos == 4);

        // 不正: 孤立継続バイト → FFFD, 1 バイト前進
        {
            let b = [0x80u8, 0x80, 0x41];
            pos = 0;
            assert!(decode(&b, &mut pos) == REPLACEMENT && pos == 1);
            assert!(decode(&b, &mut pos) == REPLACEMENT && pos == 2);
            assert!(decode(&b, &mut pos) == 0x41 && pos == 3);
        }

        // overlong 排除: C0 81 は不正
        pos = 0;
        assert!(dec1(b"\xC0\x81", &mut pos) == REPLACEMENT && pos == 1);

        // 2 バイト頭 + 不正継続 → FFFD、不正バイトから再解釈
        {
            let b = [0xC3u8, 0x28];
            pos = 0;
            assert!(decode(&b, &mut pos) == REPLACEMENT && pos == 1);
            assert!(decode(&b, &mut pos) == 0x28 && pos == 2);
        }

        // 末尾切断: 3 バイト列の 2 バイト目で終端 → 残り消費
        {
            let b = [0xE3u8, 0x81];
            pos = 0;
            assert!(decode(&b, &mut pos) == REPLACEMENT && pos == 2);
        }

        // サロゲート ED A0 80 → 不正
        pos = 0;
        assert!(dec1(b"\xED\xA0\x80", &mut pos) == REPLACEMENT);

        // U+10FFFF 上限: F4 8F BF BF は合法、F5 以降は不正
        {
            let max_ok = [0xF4u8, 0x8F, 0xBF, 0xBF];
            pos = 0;
            assert!(decode(&max_ok, &mut pos) == 0x10FFFF && pos == 4);
        }
        pos = 0;
        assert!(dec1(b"\xF5\x80\x80\x80", &mut pos) == REPLACEMENT && pos == 1);

        // encoder round-trip
        {
            let cps = [0x41u32, 0xE9, 0x3042, 0x1F600, 0xFFFD];
            for &cp in &cps {
                let mut buf = [0u8; 4];
                let len = encode(cp, &mut buf);
                let mut p = 0;
                assert_eq!(decode(&buf[..len], &mut p), cp);
                assert_eq!(p, len);
            }
        }

        // セル幅
        assert_eq!(glyph_width(b'A' as u32), 1);
        assert_eq!(glyph_width(0x3042), 2); // あ
        assert_eq!(glyph_width(0xFF21), 2); // Ａ 全角
        assert_eq!(glyph_width(0x0301), 0); // 結合アクセント
        assert_eq!(glyph_width(0x07), 0); // BEL
    }

    /// エンコーダの全コードポイント往復（サロゲート・範囲外は REPLACEMENT へ）。
    #[test]
    fn encode_roundtrip_all_codepoints() {
        let mut buf = [0u8; 4];
        let check = |buf: &mut [u8; 4], cp: u32| {
            if (0xD800..=0xDFFF).contains(&cp) {
                return; // サロゲートはエンコード対象外
            }
            let len = encode(cp, buf);
            let mut p = 0;
            assert_eq!(decode(&buf[..len], &mut p), cp, "cp=U+{cp:04X}");
            assert_eq!(p, len);
        };
        // Miri は UB 検出が目的（掃引網羅は通常 test / fuzz の責務）。Miri 下では
        // 粗 step + 分岐境界の明示集合に縮小する（全長掃引は 600s ゲートに載らない）。
        if cfg!(miri) {
            for cp in (0u32..=0x10FFFF).step_by(4093) {
                check(&mut buf, cp);
            }
            for &cp in &[
                0x7Fu32, 0x80, 0x7FF, 0x800, 0xFFF, 0x1000, 0xCFFF, 0xD000, 0xD7FF, 0xE000, 0xFFFF,
                0x10000, 0xF_FFFF, 0x10_0000, 0x10FFF0, 0x10FFFF,
            ] {
                check(&mut buf, cp);
            }
        } else {
            for cp in 0u32..=0x10FFFF {
                check(&mut buf, cp);
            }
        }
        // 範囲外（u32 上限を超えない最大）とサロゲートは REPLACEMENT に置換
        for &cp in &[0x110000u32, 0xFFFF_FFFF, 0xD800, 0xDFFF] {
            let len = encode(cp, &mut buf);
            let mut p = 0;
            assert_eq!(decode(&buf[..len], &mut p), REPLACEMENT);
        }
    }

    /// `band_w2` の健全性を 3 バイト全域で機械証明:
    /// `band_w2(b0,b1,b2)` ⟹ decode 成功 ∧ 幅 2。
    #[test]
    fn band_w2_implies_valid_wide() {
        // Miri 下では粗 step に縮小（同上。段判定の機械性質は step 問わず同一）。
        const STEP: usize = if cfg!(miri) { 17 } else { 1 };
        for b0 in 0xE0u8..=0xEF {
            for b1 in (0u8..=0xFF).step_by(STEP) {
                for b2 in (0u8..=0xFF).step_by(STEP) {
                    if !band_w2(b0, b1, b2) {
                        continue;
                    }
                    let s = [b0, b1, b2];
                    let mut p = 0;
                    let cp = decode(&s, &mut p);
                    // 妥当（3 バイト消費）かつ幅 2
                    assert_eq!(p, 3, "band_w2 hit だが不正: {b0:02X} {b1:02X} {b2:02X}");
                    assert_eq!(
                        glyph_width(cp),
                        2,
                        "band_w2 hit だが幅 2 でない: {b0:02X} {b1:02X} {b2:02X} -> U+{cp:04X}"
                    );
                }
            }
        }
    }

    /// `band_w2` の縮小側安全（偽陰性は許す）: 2 バイト目が継続でない列を
    /// 誤って真にしない。
    #[test]
    fn band_w2_rejects_bad_continuation() {
        // b2 の上位 2 ビットが 10 でないものは全て偽
        for b2 in 0u8..=0xFF {
            if (b2 & 0xC0) != 0x80 {
                assert!(!band_w2(0xE5, 0x80, b2), "b2={b2:02X} を真にした");
            }
        }
    }
}
