//! TLS クライアントの純粋関数（C の `src/tls.c` の base64 デコード相当）。
//!
//! | C (tls.c) | Rust |
//! |---|---|
//! | `b64_decode` | [`b64_decode`] |
//!
//! # 実装済み
//!
//! PEM 標準表の base64 デコード（空白類は無視、`=` padding 対応、`\0` で打ち切り）。
//! CA バンドル（PEM）の証明書抽出（`ca_load_pem`）が使う。
//!
//! # 未移植（ソケット I/O・最終統合）
//!
//! - `ta_add` / `ca_load_pem` / `ca_load` / `if_tls_client` / `if_tls_send_all` /
//!   `if_tls_recv` / `if_tls_close`: BearSSL の静的リンク + socket I/O。非決定的で
//!   純粋関数化不能。最終統合（chrome 移植時）に Rust の TLS（`rustls` 等）で再実装する。

/// PEM 標準表の base64 デコード。C の `b64_decode` 相当。
///
/// 空白類（space/tab/lf/cr）は無視、`=`（padding）は消費、`\0` で打ち切り。
/// padding 後に非 padding 文字が来たら失敗。戻り値はデコード済みバイト列。
pub fn b64_decode(input: &[u8]) -> Option<Vec<u8>> {
    // base64 デコード表（-1 = 不正）
    const T: [i8; 128] = {
        let mut t = [-1i8; 128];
        let mut i = 0;
        while i < 128 {
            t[i] = -1;
            i += 1;
        }
        // 標準 base64 表
        // '+' = 62, '/' = 63
        t[43] = 62;
        t[47] = 63;
        // '0'-'9' = 52-61
        let mut d = 52i8;
        let mut c = b'0' as usize;
        while c <= b'9' as usize {
            t[c] = d;
            d += 1;
            c += 1;
        }
        // 'A'-'Z' = 0-25
        let mut d = 0i8;
        let mut c = b'A' as usize;
        while c <= b'Z' as usize {
            t[c] = d;
            d += 1;
            c += 1;
        }
        // 'a'-'z' = 26-51
        let mut d = 26i8;
        let mut c = b'a' as usize;
        while c <= b'z' as usize {
            t[c] = d;
            d += 1;
            c += 1;
        }
        t
    };

    let mut acc: u64 = 0;
    let mut nbits: i32 = 0;
    let mut out = Vec::new();
    let mut seen_nonpad = false;
    for &c in input {
        if c == b' ' || c == b'\t' || c == b'\n' || c == b'\r' {
            continue;
        }
        if c == b'=' {
            seen_nonpad = true;
            continue;
        }
        if c == 0 {
            break;
        }
        if seen_nonpad {
            return None; // padding 後の非 padding
        }
        let v = if (c as usize) < 128 { T[c as usize] } else { -1 };
        if v < 0 {
            return None;
        }
        acc = (acc << 6) | v as u64;
        nbits += 6;
        if nbits >= 8 {
            nbits -= 8;
            out.push((acc >> nbits) as u8);
            acc &= (1u64 << nbits) - 1;
        }
    }
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn basic() {
        // "Wiki" = base64 "V2lraQ=="
        assert_eq!(b64_decode(b"V2lraQ=="), Some(b"Wiki".to_vec()));
        // "Wikipedia" の一部など
        assert_eq!(b64_decode(b"aGVsbG8="), Some(b"hello".to_vec()));
        // 空
        assert_eq!(b64_decode(b""), Some(Vec::new()));
    }

    #[test]
    fn whitespace_ignored() {
        assert_eq!(b64_decode(b"aGVs\nbG8="), Some(b"hello".to_vec()));
        assert_eq!(b64_decode(b"aGVs\tbG8="), Some(b"hello".to_vec()));
    }

    #[test]
    fn nul_terminates() {
        assert_eq!(b64_decode(b"aGVsbG8=\0extra"), Some(b"hello".to_vec()));
    }

    #[test]
    fn rejects_invalid() {
        assert_eq!(b64_decode(b"aGVsbG8"), Some(b"hello".to_vec())); // padding なしも OK
        assert_eq!(b64_decode(b"!!!!"), None); // 不正文字
        assert_eq!(b64_decode(b"a=bc"), None); // padding 後の非 padding
    }
}
