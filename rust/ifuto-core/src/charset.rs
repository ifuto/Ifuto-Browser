//! 文字コード層（C の `src/charset.c` 相当）。Shift_JIS 系 / EUC-JP → UTF-8 変換。
//!
//! C 実装からの移植対応:
//! | C (charset.c) | Rust |
//! |---|---|
//! | `IfEnc`（`IF_ENC_UTF8/SJIS/EUCJP`） | [`Enc`] |
//! | `if_charset_label(const char*, u32)` | [`label`]（`&[u8]`） |
//! | `if_charset_from_http(IfStr)` | [`from_http`]（`Option<&[u8]>`） |
//! | `if_charset_sniff(IfStr, IfStr, bool*)` | [`sniff`]（`(Enc, bool)` を返す） |
//! | `if_charset_decode(IfArena*, IfStr, IfEnc)` | [`decode`]（`Vec<u8>` を返す） |
//! | `charset_tables_gen.h`（生成表） | [`charset_tables`]（生成表） |
//!
//! # 設計（docs/CHARSET.md 凍結）
//!
//! - 対応ラベル: shift_jis 系（windows-31j/cp932 含む）・euc-jp 系・utf-8 系。
//!   未知ラベルは UTF-8 へ安全側フォールバック。
//! - 波ダッシュ 6 件は cp932 採用。
//! - malformed は常に U+FFFD で 1 文字前進（WHATWG 準拠の restore 規則。
//!   無限ループ不成立）。
//! - 判定順: HTTP charset > BOM(UTF-8) > meta prescan(先頭 4096B) > UTF-8。
//!
//! # C との違い（所有権による構造的な改善）
//!
//! C の `if_charset_decode` は呼び出し側の arena に出力を書く（`IfArena*` を渡す）。
//! Rust では arena を所有権ベースの [`Vec`] に置き換え、`decode` が所有する
//! `Vec<u8>` を返す。手動の容量計算（`3n+3`）とバッファ境界検査が不要になり、
//! 出力書き込みの OOB が構造的に消える。

use crate::strutil::{ascii_lower, str_eq_ci};

/// 生成表（C の `charset_tables_gen.h` 相当。`tools/gen_charset.py` が生成）。
pub use crate::charset_tables;

/// 文字コード（C の `IfEnc` 相当）。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Enc {
    /// UTF-8（既定）。
    Utf8,
    /// Shift_JIS 系（windows-31j / cp932 含む）。
    Sjis,
    /// EUC-JP 系。
    EucJp,
}

/// 不正バイト列の置換文字（U+FFFD）。
const REPLACEMENT: u32 = 0xFFFD;

/// ラベルの前後 space/tab を除く（C の `if_charset_label` 先頭の trimming）。
fn trim_label(mut p: &[u8]) -> &[u8] {
    while p.first().is_some_and(|&c| c == b' ' || c == b'\t') {
        p = &p[1..];
    }
    while p.last().is_some_and(|&c| c == b' ' || c == b'\t') {
        p = &p[..p.len() - 1];
    }
    p
}

/// charset ラベル（前後空白許容・ci）→ [`Enc`]。未知/非対応は [`Enc::Utf8`]。
pub fn label(p: &[u8]) -> Enc {
    let p = trim_label(p);
    const SJ: &[&[u8]] = &[
        b"shift_jis",
        b"shift-jis",
        b"sjis",
        b"csshiftjis",
        b"ms_kanji",
        b"windows-31j",
        b"x-sjis",
        b"cp932",
        b"ms932",
        b"x-ms-cp932",
    ];
    const EJ: &[&[u8]] = &[b"euc-jp", b"cseucpkdfmtjapanese", b"x-euc-jp"];
    const U8: &[&[u8]] = &[b"utf-8", b"utf8", b"unicode-1-1-utf-8"];
    if SJ.iter().any(|w| str_eq_ci(p, w)) {
        return Enc::Sjis;
    }
    if EJ.iter().any(|w| str_eq_ci(p, w)) {
        return Enc::EucJp;
    }
    if U8.iter().any(|w| str_eq_ci(p, w)) {
        return Enc::Utf8;
    }
    Enc::Utf8 // 未知ラベル = 安全側フォールバック
}

/// `"charset"` の直後の値を取り出す（属性形 / content 形の両方を 1 スキャナで拾う）。
/// 直前が名前構成文字（`[A-Za-z0-9_-]`）の `charset`（`data-charset` 等）は拾わない。
/// 値は引用符付き/裸形を受理。見つかればラベルスライスを返す。
fn extract_charset(p: &[u8]) -> Option<&[u8]> {
    const KW: &[u8] = b"charset";
    let n = p.len();
    let mut i = 0;
    while i + 7 <= n {
        if ascii_lower(p[i]) != b'c' {
            i += 1;
            continue;
        }
        let mut hit = true;
        for k in 0..7 {
            if ascii_lower(p[i + k]) != KW[k] {
                hit = false;
                break;
            }
        }
        if !hit {
            i += 1;
            continue;
        }
        if i > 0 {
            let prev = p[i - 1];
            let pl = ascii_lower(prev);
            if pl == b'-' || pl == b'_' || prev.is_ascii_digit() || pl.is_ascii_lowercase() {
                i += 1;
                continue; // xxxcharset / data-charset は別語
            }
        }
        let mut j = i + 7;
        while j < n && (p[j] == b' ' || p[j] == b'\t') {
            j += 1;
        }
        if j >= n || p[j] != b'=' {
            i += 1;
            continue;
        }
        j += 1;
        while j < n && (p[j] == b' ' || p[j] == b'\t') {
            j += 1;
        }
        let mut q = 0u8;
        if j < n && (p[j] == b'"' || p[j] == b'\'') {
            q = p[j];
            j += 1;
        }
        let b = j;
        while j < n {
            let c = p[j];
            if q != 0 {
                if c == q {
                    break;
                }
            } else if c == b' '
                || c == b'\t'
                || c == b'"'
                || c == b'\''
                || c == b';'
                || c == b'/'
                || c == b'>'
                || c < 0x20
            {
                break;
            }
            j += 1;
        }
        let len = j - b;
        if len == 0 || len >= 64 {
            return None;
        }
        return Some(&p[b..j]);
    }
    None
}

/// HTTP Content-Type 値から charset ラベルを抽出して [`Enc`] へ。無効・未知は [`Enc::Utf8`]。
pub fn from_http(ctype_header: Option<&[u8]>) -> Enc {
    match ctype_header {
        Some(h) if !h.is_empty() => extract_charset(h).map(label).unwrap_or(Enc::Utf8),
        _ => Enc::Utf8,
    }
}

/// meta prescan: 先頭 limit バイト内の `<meta ...>` 要素から charset 値を拾う。
/// `<meta` の後続が空白/スラッシュ/`>` のときだけタグと認める。
fn prescan_meta(p: &[u8]) -> Option<&[u8]> {
    let n = p.len();
    let limit = n.min(4096);
    let mut i = 0;
    while i + 6 <= limit {
        if p[i] != b'<' {
            i += 1;
            continue;
        }
        if ascii_lower(p[i + 1]) != b'm'
            || ascii_lower(p[i + 2]) != b'e'
            || ascii_lower(p[i + 3]) != b't'
            || ascii_lower(p[i + 4]) != b'a'
        {
            i += 1;
            continue;
        }
        let c5 = p[i + 5];
        if !(c5 == b' ' || c5 == b'\t' || c5 == b'/' || c5 == b'>' || c5 == b'\n' || c5 == b'\r') {
            i += 1;
            continue;
        }
        let mut j = i + 5;
        while j < limit && p[j] != b'>' {
            j += 1;
        }
        if j >= limit {
            return None; // タグ未完 = 以降にタグなしと同義（継続しない）
        }
        if let Some(lab) = extract_charset(&p[i + 5..j]) {
            return Some(lab);
        }
        i = j; // この meta に charset が無ければ次の < から再開
    }
    None
}

/// 判定: HTTP > BOM(UTF-8) > meta prescan(先頭 4096B) > UTF-8。
/// 戻り値は `(エンコーディング, UTF-8 BOM を検出したか)`。
pub fn sniff(ctype_header: Option<&[u8]>, bytes: &[u8]) -> (Enc, bool) {
    let bom = bytes.len() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF;
    // 1) HTTP Content-Type（対応ラベルに確定したときのみ確定）
    if let Some(h) = ctype_header.filter(|h| !h.is_empty()) {
        if let Some(lab) = extract_charset(h) {
            let e = label(lab);
            if e != Enc::Utf8 || str_eq_ci(lab, b"utf-8") || str_eq_ci(lab, b"utf8") {
                return (e, bom);
            }
        }
    }
    // 2) UTF-8 BOM
    if bom {
        return (Enc::Utf8, bom);
    }
    // 3) meta prescan
    if !bytes.is_empty() {
        if let Some(lab) = prescan_meta(bytes) {
            return (label(lab), bom);
        }
    }
    (Enc::Utf8, bom)
}

/// BMP のみ（表は BMP 保証。FFFD=0xFFFD も BMP）の UTF-8 エンコードを `out` へ追記。
fn emit_utf8(out: &mut Vec<u8>, cp: u32) {
    if cp < 0x80 {
        out.push(cp as u8);
    } else if cp < 0x800 {
        out.push((0xC0 | (cp >> 6)) as u8);
        out.push((0x80 | (cp & 63)) as u8);
    } else {
        out.push((0xE0 | (cp >> 12)) as u8);
        out.push((0x80 | ((cp >> 6) & 63)) as u8);
        out.push((0x80 | (cp & 63)) as u8);
    }
}

/// 表引き（範囲外は 0）。C の `jis_idx` 相当。
fn jis_idx(tbl: &[u16], idx: usize) -> u16 {
    tbl.get(idx).copied().unwrap_or(0)
}

/// cp932 拡張表の昇順二分探索（key = lead<<8|trail。上位 16bit が key）。C の `sjis_ext_get` 相当。
fn sjis_ext_get(key: u16) -> u16 {
    let tbl = charset_tables::SJIS_EXT;
    let mut lo = 0usize;
    let mut hi = tbl.len();
    while lo < hi {
        let mid = lo + (hi - lo) / 2;
        let k = (tbl[mid] >> 16) as u16;
        if k < key {
            lo = mid + 1;
        } else if k > key {
            hi = mid;
        } else {
            return (tbl[mid] & 0xFFFF) as u16;
        }
    }
    0
}

/// `bytes`（`enc`）→ UTF-8（所有 `Vec<u8>`）。malformed は U+FFFD。
/// `enc == Enc::Utf8` は呼ばないこと（恒等は呼び出し側で選ぶ）。
pub fn decode(input: &[u8], enc: Enc) -> Vec<u8> {
    let mut out = Vec::with_capacity(input.len() * 3 + 3);
    let s = input;
    let n = s.len();
    let mut i = 0usize;
    match enc {
        Enc::Sjis => {
            while i < n {
                let b = s[i];
                if b < 0x80 {
                    out.push(b);
                    i += 1;
                    continue;
                }
                if (0xA1..=0xDF).contains(&b) {
                    // 半角カナ U+FF61+(b-0xA1)
                    emit_utf8(&mut out, 0xFF61 + (b - 0xA1) as u32);
                    i += 1;
                    continue;
                }
                if (0x81..=0x9F).contains(&b) || (0xE0..=0xFC).contains(&b) {
                    if i + 1 >= n {
                        emit_utf8(&mut out, REPLACEMENT);
                        i += 1;
                        continue;
                    }
                    let t = s[i + 1];
                    if (0x40..=0x7E).contains(&t) || (0x80..=0xFC).contains(&t) {
                        let mut cp = sjis_ext_get(((b as u16) << 8) | t as u16);
                        if cp == 0 {
                            // tbl_jis208 を WHATWG pointer で引く
                            let trail_off: u32 = if t < 0x7F { 0x40 } else { 0x41 };
                            let lead_off: u32 = if b < 0xA0 { 0x81 } else { 0xC1 };
                            let idx = (b as u32 - lead_off) * 188 + (t as u32 - trail_off);
                            cp = jis_idx(charset_tables::JIS208, idx as usize);
                        }
                        emit_utf8(&mut out, if cp != 0 { cp as u32 } else { REPLACEMENT });
                        i += 2;
                    } else {
                        // trail 不成立 → FFFD は lead のみ消費（trail は restore）
                        emit_utf8(&mut out, REPLACEMENT);
                        i += 1;
                    }
                    continue;
                }
                // 0x80/0xA0/0xFD.. 孤立
                emit_utf8(&mut out, REPLACEMENT);
                i += 1;
            }
        }
        Enc::EucJp => {
            while i < n {
                let b = s[i];
                if b < 0x80 {
                    out.push(b);
                    i += 1;
                    continue;
                }
                if b == 0x8E {
                    // SS2: 半角カナ
                    if i + 1 >= n {
                        emit_utf8(&mut out, REPLACEMENT);
                        i += 1;
                        continue;
                    }
                    let t = s[i + 1];
                    if (0xA1..=0xDF).contains(&t) {
                        emit_utf8(&mut out, 0xFF61 + (t - 0xA1) as u32);
                        i += 2;
                    } else {
                        emit_utf8(&mut out, REPLACEMENT);
                        i += 1; // t restore
                    }
                    continue;
                }
                if b == 0x8F {
                    // SS3: JIS X 0212（trail は 0xA1..0xFE。0xFF は行越境不可）
                    if i + 1 >= n {
                        emit_utf8(&mut out, REPLACEMENT);
                        i += 1;
                        continue;
                    }
                    let t = s[i + 1];
                    if !(0xA1..=0xFE).contains(&t) {
                        emit_utf8(&mut out, REPLACEMENT);
                        i += 1; // t restore
                        continue;
                    }
                    if i + 2 >= n {
                        emit_utf8(&mut out, REPLACEMENT);
                        i += 2;
                        continue;
                    }
                    let u = s[i + 2];
                    if !(0xA1..=0xFE).contains(&u) {
                        emit_utf8(&mut out, REPLACEMENT);
                        i += 2; // u restore
                        continue;
                    }
                    let cp = jis_idx(
                        charset_tables::JIS212,
                        (t as usize - 0xA1) * 94 + (u as usize - 0xA1),
                    );
                    emit_utf8(&mut out, if cp != 0 { cp as u32 } else { REPLACEMENT });
                    i += 3;
                    continue;
                }
                if (0xA1..=0xFE).contains(&b) {
                    // JIS X 0208 面（lead/trail 共に 0xA1..0xFE）
                    if i + 1 >= n {
                        emit_utf8(&mut out, REPLACEMENT);
                        i += 1;
                        continue;
                    }
                    let t = s[i + 1];
                    if (0xA1..=0xFE).contains(&t) {
                        let cp = jis_idx(
                            charset_tables::JIS208,
                            (b as usize - 0xA1) * 94 + (t as usize - 0xA1),
                        );
                        emit_utf8(&mut out, if cp != 0 { cp as u32 } else { REPLACEMENT });
                        i += 2;
                    } else {
                        emit_utf8(&mut out, REPLACEMENT);
                        i += 1; // t restore
                    }
                    continue;
                }
                // 0x81..0x8D,0x90..0x9F
                emit_utf8(&mut out, REPLACEMENT);
                i += 1;
            }
        }
        Enc::Utf8 => {
            // 恒等（呼び出し側で選ぶべき。防御として原入力をそのまま返す）
            return input.to_vec();
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dec(input: &[u8], enc: Enc) -> Vec<u8> {
        decode(input, enc)
    }

    fn eq(s: &[u8], lit: &str) -> bool {
        s == lit.as_bytes()
    }

    #[test]
    fn oracle_label() {
        assert_eq!(label(b"shift_jis"), Enc::Sjis);
        assert_eq!(label(b"Shift_JIS"), Enc::Sjis);
        assert_eq!(label(b"shift-jis"), Enc::Sjis);
        assert_eq!(label(b"sjis"), Enc::Sjis);
        assert_eq!(label(b"windows-31j"), Enc::Sjis);
        assert_eq!(label(b"cp932"), Enc::Sjis);
        assert_eq!(label(b"x-sjis"), Enc::Sjis);
        assert_eq!(label(b"MS_Kanji"), Enc::Sjis);
        assert_eq!(label(b" shift_jis "), Enc::Sjis);
        assert_eq!(label(b"euc-jp"), Enc::EucJp);
        assert_eq!(label(b"EUC-JP"), Enc::EucJp);
        assert_eq!(label(b"x-euc-jp"), Enc::EucJp);
        assert_eq!(label(b"cseucpkdfmtjapanese"), Enc::EucJp);
        assert_eq!(label(b"utf-8"), Enc::Utf8);
        assert_eq!(label(b"utf8"), Enc::Utf8);
        // 未知ラベル安全側フォールバック
        assert_eq!(label(b"iso-8859-1"), Enc::Utf8);
        assert_eq!(label(b"windows-1252"), Enc::Utf8);
        assert_eq!(label(b""), Enc::Utf8);
    }

    #[test]
    fn oracle_sniff() {
        // HTTP 優先
        let (e, _) = sniff(
            Some(b"text/html; charset=euc-jp"),
            b"<meta charset=shift_jis><p>x",
        );
        assert_eq!(e, Enc::EucJp);
        // meta 引用符・ci・空白
        let (e, _) = sniff(None, b"<META  CHARSET = \"Shift_JIS\">");
        assert_eq!(e, Enc::Sjis);
        // http-equiv content 形
        let (e, _) = sniff(
            None,
            b"<meta http-equiv=\"Content-Type\" content=\"text/html;charset=Shift_JIS\">",
        );
        assert_eq!(e, Enc::Sjis);
        // BOM > meta
        {
            let (e, bom) = sniff(None, b"\xEF\xBB\xBF<meta charset=shift_jis>");
            assert_eq!(e, Enc::Utf8);
            assert!(bom);
        }
        // 4096 境界
        {
            let mut big = vec![b' '; 5000];
            big[4050..4050 + 21].copy_from_slice(b"<meta charset=euc-jp>");
            let (e, _) = sniff(None, &big);
            assert_eq!(e, Enc::EucJp);
            let mut big2 = vec![b' '; 4200];
            big2[4120..4120 + 21].copy_from_slice(b"<meta charset=euc-jp>");
            let (e, _) = sniff(None, &big2);
            assert_eq!(e, Enc::Utf8);
        }
        // 非 meta / data-charset 偽陽性排除 / 既定 UTF-8
        let (e, _) = sniff(None, b"<div data-charset=euc-jp>");
        assert_eq!(e, Enc::Utf8);
        let (e, bom) = sniff(None, b"<p>hello");
        assert_eq!(e, Enc::Utf8);
        assert!(!bom);
        // <metafoo は meta ではない
        let (e, _) = sniff(None, b"<metafoo charset=euc-jp>");
        assert_eq!(e, Enc::Utf8);
    }

    #[test]
    fn oracle_decode_sjis() {
        // ASCII 直通 + 0x5C はバックスラッシュのまま
        assert!(eq(&dec(b"A\\B~", Enc::Sjis), "A\\B~"));
        // こんにちは日本語
        {
            let b = [
                0x82u8, 0xb1, 0x82, 0xf1, 0x82, 0xc9, 0x82, 0xbf, 0x82, 0xcd, 0x93, 0xfa, 0x96,
                0x7b, 0x8c, 0xea,
            ];
            assert!(eq(&dec(&b, Enc::Sjis), "こんにちは日本語"));
        }
        // 半角カナ ｱｲｳ
        assert!(eq(&dec(&[0xb1, 0xb2, 0xb3], Enc::Sjis), "ｱｲｳ"));
        // 波ダッシュ cp932 採用
        assert!(eq(&dec(&[0x81, 0x60], Enc::Sjis), "～"));
        // NEC 選定 ① ／ IBM 拡張 ⅰ
        assert!(eq(&dec(&[0x87, 0x40], Enc::Sjis), "①"));
        assert!(eq(&dec(&[0xfa, 0x40], Enc::Sjis), "ⅰ"));
        // malformed: lead+EOF → FFFD(1)
        assert!(eq(&dec(&[0x93], Enc::Sjis), "\u{FFFD}"));
        // lead + 範囲外 trail(0x20) → FFFD(lead のみ)。trail restore
        assert!(eq(
            &dec(&[0x93, 0x20, b'A', b'B'], Enc::Sjis),
            "\u{FFFD} AB"
        ));
        // 有効 lead + 有効 trail: 0x93 0x41 = 鄭
        assert!(eq(&dec(&[0x93, 0x41], Enc::Sjis), "鄭"));
        // 孤立 0x80 / 0xA0 / 0xFD
        assert!(eq(
            &dec(&[0x80, 0xA0, 0xFD], Enc::Sjis),
            "\u{FFFD}\u{FFFD}\u{FFFD}"
        ));
        // lead + trail 範囲外の継続: 0x82 0x20 → FFFD + ' '
        assert!(eq(
            &dec(&[0x82, 0x20, 0x82, 0xa0], Enc::Sjis),
            "\u{FFFD} あ"
        ));
        // 有効 lead+範囲内 trail だが無字セル → FFFD（2 消費）
        assert!(eq(&dec(&[0x85, 0x40], Enc::Sjis), "\u{FFFD}"));
    }

    #[test]
    fn oracle_decode_euc() {
        assert!(eq(&dec(b"abcXYZ", Enc::EucJp), "abcXYZ"));
        // 日本
        assert!(eq(&dec(&[0xc6, 0xfc, 0xcb, 0xdc], Enc::EucJp), "日本"));
        // SS2 半角 ｱ
        assert!(eq(&dec(&[0x8e, 0xb1], Enc::EucJp), "ｱ"));
        // SS3 JIS X 0212: 8f a2 af → U+02D8
        assert!(eq(&dec(&[0x8f, 0xa2, 0xaf], Enc::EucJp), "\u{02D8}"));
        // malformed: 8F + EOF → FFFD(1)。8F a2 + EOF → FFFD(2)
        assert!(eq(&dec(&[0x8f], Enc::EucJp), "\u{FFFD}"));
        assert!(eq(&dec(&[0x8f, 0xa2], Enc::EucJp), "\u{FFFD}"));
        // 8F + 非範囲 → FFFD(1) + restore,'A' 生存
        assert!(eq(&dec(&[0x8f, b'A'], Enc::EucJp), "\u{FFFD}A"));
        // 8F a2 + 非範囲 → FFFD(2) + 'B' 生存
        assert!(eq(&dec(&[0x8f, 0xa2, b'B'], Enc::EucJp), "\u{FFFD}B"));
        // 0208 lead 単体 → FFFD(1)。lead + ASCII → FFFD(1)+restore
        assert!(eq(&dec(&[0xc6], Enc::EucJp), "\u{FFFD}"));
        assert!(eq(&dec(&[0xc6, b'!'], Enc::EucJp), "\u{FFFD}!"));
        // 0xFF trail は行越境しない
        assert!(eq(&dec(&[0xc6, 0xff], Enc::EucJp), "\u{FFFD}\u{FFFD}"));
        // 0x8E + 非範囲 → FFFD(1)+restore
        assert!(eq(&dec(&[0x8e, 0x41], Enc::EucJp), "\u{FFFD}A"));
        // 孤立 0x81/0x90/0xFF → 各 FFFD(1)
        assert!(eq(
            &dec(&[0x81, 0x90, 0xff], Enc::EucJp),
            "\u{FFFD}\u{FFFD}\u{FFFD}"
        ));
    }

    /// 全バイト対（65536 × 2）の総当たり健全性: クラッシュせず・出力長 ≤ 6・決定的。
    #[test]
    fn oracle_sweep() {
        // Miri 下では素数 step に縮小（UB 検出が目的。掃引網羅は通常 test / fuzz）。
        // 0xFFFF は必ず踏む（chain に明示）。
        const STEP: usize = if cfg!(miri) { 251 } else { 1 };
        for v in (0u32..=0xFFFF).step_by(STEP).chain([0xFFFF]) {
            let b = [(v >> 8) as u8, v as u8];
            let s1 = decode(&b, Enc::Sjis);
            let s2 = decode(&b, Enc::EucJp);
            assert!(s1.len() <= 6 && s2.len() <= 6, "overlong at {v:#06x}");
        }
    }

    /// BOM strip 規則（関門側の責務）。sniff が bom を報告し、3B を剥がす。
    #[test]
    fn oracle_bom_strip_rule() {
        let html = b"\xEF\xBB\xBF<p>x";
        let (enc, bom) = sniff(None, html);
        assert_eq!(enc, Enc::Utf8);
        assert!(bom);
        let mut s: &[u8] = html;
        if bom && s.len() >= 3 {
            s = &s[3..];
        }
        assert_eq!(s.len(), 4);
        assert_eq!(s, b"<p>x");
    }
}
