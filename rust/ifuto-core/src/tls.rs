//! TLS クライアントの純粋関数（C の `src/tls.c` の base64 デコード + CA ロード相当）。
//!
//! | C (tls.c) | Rust |
//! |---|---|
//! | `b64_decode` | [`b64_decode`] |
//! | `ca_load_pem`（PEM → 証明書） | [`ca_load_pem`] |
//! | `ta_add`（DER → トラストアンカー） | [`ta_add`] |
//!
//! # 実装済み
//!
//! PEM 標準表の base64 デコード（空白類は無視、`=` padding 対応、`\0` で打ち切り）。
//! CA バンドル（PEM）の証明書抽出（`ca_load_pem`）と、DER 証明書からのトラストアンカー
//! 抽出（`ta_add`。X.509 の subject DN + SPKI 公開鍵を BearSSL の `br_x509_decoder` と
//! 同一の規則で抜き出す）。
//!
//! DN + 公開鍵の抽出は TLS バックエンド非依存（rustls の `TrustAnchor` も DN + SPKI を
//! 要求する）ため、ここでは自己完結の [`TrustAnchor`] / [`Pkey`] に正規化する。
//!
//! # 未移植（ソケット I/O・最終統合）
//!
//! - `ca_load`（ファイル I/O）/ `if_tls_client` / `if_tls_send_all` / `if_tls_recv` /
//!   `if_tls_close`: BearSSL の静的リンク + socket I/O。非決定的で純粋関数化不能。
//!   最終統合（chrome 移植時）に Rust の TLS（`rustls` 等）で再実装する。

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

/// トラストアンカーの公開鍵（C の `br_x509_pkey` 相当。RSA / EC のみ）。
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Pkey {
    /// RSA。`n` = モジュラス（先頭 0 を剥いだ INTEGER 値）、`e` = 公開指数。
    Rsa {
        /// モジュラス（先頭 0 を剥いだ INTEGER 値）。
        n: Vec<u8>,
        /// 公開指数。
        e: Vec<u8>,
    },
    /// 楕円曲線。`curve` = BearSSL の曲線 ID（23=P-256 / 24=P-384 / 25=P-521）。
    /// `q` = 公開点（BIT STRING 内容の生バイト列）。
    Ec {
        /// BearSSL の曲線 ID（23=P-256 / 24=P-384 / 25=P-521）。
        curve: i32,
        /// 公開点（BIT STRING 内容の生バイト列）。
        q: Vec<u8>,
    },
}

/// トラストアンカー（C の `br_x509_trust_anchor` 相当）。
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TrustAnchor {
    /// subject Name の DER エンコード（`0x30` SEQUENCE のタグ・長さ・内容全体）。
    pub dn: Vec<u8>,
    /// 公開鍵。
    pub pkey: Pkey,
}

// OID 内容バイト列（tag/length を除いた値部）。
const OID_RSA_ENCRYPTION: &[u8] = &[0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01];
const OID_EC_PUBLIC_KEY: &[u8] = &[0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01];
const OID_BASIC_CONSTRAINTS: &[u8] = &[0x55, 0x1D, 0x13]; // 2.5.29.19
const OID_P256: &[u8] = &[0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07];
const OID_P384: &[u8] = &[0x2B, 0x81, 0x04, 0x00, 0x22];
const OID_P521: &[u8] = &[0x2B, 0x81, 0x04, 0x00, 0x23];

/// 最小 DER リーダ（BearSSL の `br_x509_decoder` が受理する DER 部分集合に限定）。
///
/// BearSSL の `lim`（現在の最内構造の終端）を [`Der::lim`] で追跡し、入れ子の長さが
/// 親構造をはみ出す（`open-elt` の `lim < length`）読みを拒否する。
struct Der<'a> {
    b: &'a [u8],
    p: usize,
    /// 現在の最内構造の終端オフセット（これより先は読まない）。初期は全バッファ。
    lim: usize,
}

impl<'a> Der<'a> {
    fn new(b: &'a [u8]) -> Self {
        Der { b, p: 0, lim: b.len() }
    }

    fn byte(&mut self) -> Option<u8> {
        if self.p >= self.lim {
            return None;
        }
        let c = *self.b.get(self.p)?;
        self.p += 1;
        Some(c)
    }

    /// タグを読む（クラス検証・拡張タグ拒否）。`(constructed, value)`。
    /// BearSSL の `read-tag`: クラスは universal(00)/context(10) のみ、拡張タグ(31)不可。
    /// `value` はクラスオフセット込み（context は +32）で、SEQUENCE=0x10、INTEGER=0x02、
    /// context[0]=0x20、context[3]=0x23 等（BearSSL の `read-tag` と同一）。
    fn tag(&mut self) -> Option<(bool, u8)> {
        let b = self.byte()?;
        if b & 0x40 != 0 {
            return None; // クラス 01/11（application/private）は不可
        }
        let t = b & 0x1F;
        if t == 0x1F {
            return None; // 拡張タグは非対応
        }
        let class_off = if b & 0x80 != 0 { 32 } else { 0 };
        Some((b & 0x20 != 0, class_off + t))
    }

    /// 長さを読む（不定長 0x80 は拒否。0x7FFFFF 超も拒否）。
    fn len(&mut self) -> Option<usize> {
        let b = self.byte()?;
        if b < 0x80 {
            return Some(b as usize);
        }
        if b == 0x80 {
            return None; // 不定長
        }
        let n = (b & 0x7F) as usize;
        if n == 0 || n > 3 {
            return None;
        }
        let mut v = 0usize;
        for _ in 0..n {
            let c = self.byte()?;
            if v > 0x7F_FFFF {
                return None;
            }
            v = (v << 8) | c as usize;
        }
        Some(v)
    }

    /// 構造要素を開く（長さを読み、内容が現在の `lim` 内に収まることを確認して
    /// `lim` を内容末尾へ更新）。戻り値は `(内容長, 開く前の lim)`。
    fn open(&mut self) -> Option<(usize, usize)> {
        let l = self.len()?;
        let end = self.p.checked_add(l)?;
        if end > self.lim {
            return None; // open-elt: 親構造をはみ出す
        }
        let saved = self.lim;
        self.lim = end;
        Some((l, saved))
    }

    /// 構造要素を閉じる（内容を完全に消費したことを確認して `lim` を復元）。
    /// BearSSL の `close-elt`（余分な要素が残っていれば失敗）。
    fn close_elt(&mut self, saved: usize) -> Option<()> {
        if self.p != self.lim {
            return None; // ERR_X509_EXTRA_ELEMENT
        }
        self.lim = saved;
        Some(())
    }

    /// SEQUENCE（0x30）を開く。
    fn seq(&mut self) -> Option<(usize, usize)> {
        let (c, v) = self.tag()?;
        if !c || v != tag_val::SEQ {
            return None;
        }
        self.open()
    }

    /// INTEGER（0x02）を読んで内容長を返す（葉要素。内容は現在の `lim` で制限）。
    fn integer(&mut self) -> Option<usize> {
        let (c, v) = self.tag()?;
        if c || v != tag_val::INT {
            return None;
        }
        self.len()
    }

    /// OID（0x06）のタグと長さを読み、内容長を返す（内容は進めない）。C の
    /// `read-OID` の `read-small-value` が 255 バイト超を false 扱いするため、
    /// 呼び出し側で 255 との比較に使う。
    fn oid_len(&mut self) -> Option<usize> {
        let (c, v) = self.tag()?;
        if c || v != tag_val::OID {
            return None;
        }
        self.len()
    }

    /// BIT STRING（0x03）を開く。
    fn bitstring(&mut self) -> Option<(usize, usize)> {
        let (c, v) = self.tag()?;
        if c || v != tag_val::BITSTR {
            return None;
        }
        self.open()
    }

    /// context タグ（構築済み）を開く。`expected` は `tag_val::CTX*`。
    fn ctx_open(&mut self, expected: u8) -> Option<(usize, usize)> {
        let (c, v) = self.tag()?;
        if !c || v != expected {
            return None;
        }
        self.open()
    }

    fn skip(&mut self, n: usize) -> Option<()> {
        if self.p + n > self.lim {
            return None;
        }
        self.p += n;
        Some(())
    }

    /// 現在の構造の残り全バイトを飛ばす（BearSSL の `skip-remaining`）。
    fn skip_remaining(&mut self) {
        self.p = self.lim;
    }

    fn take(&mut self, n: usize) -> Option<&'a [u8]> {
        if self.p + n > self.lim {
            return None;
        }
        let s = &self.b[self.p..self.p + n];
        self.p += n;
        Some(s)
    }

    /// 先頭 `n` バイトをその場で借用（消費しない。version タグの覗き見用）。
    fn peek(&self, n: usize) -> Option<&'a [u8]> {
        if self.p + n > self.lim {
            return None;
        }
        Some(&self.b[self.p..self.p + n])
    }
}

/// RSA の INTEGER（n / e）値を読む。C の `read-integer` 相当:
/// 先頭バイトは 0x80 未満（符号ビット 0）、先頭の 0x00 を剥ぐ。
fn read_rsa_integer(r: &mut Der<'_>) -> Option<Vec<u8>> {
    let l = r.integer()?;
    if l == 0 {
        return None; // 空 INTEGER（read8 が -1 = 打ち切り）
    }
    let content = r.take(l)?;
    if content[0] >= 0x80 {
        return None; // 符号ビット 1（負数）
    }
    // 先頭の 0x00 を剥ぐ
    let mut out = Vec::with_capacity(content.len());
    let mut started = false;
    for &b in content {
        if !started && b == 0 {
            continue;
        }
        started = true;
        out.push(b);
    }
    Some(out)
}

/// DER タグ値（BearSSL の `read-tag` の value と同一）。
mod tag_val {
    pub const SEQ: u8 = 0x10; // SEQUENCE
    pub const INT: u8 = 0x02; // INTEGER
    pub const BITSTR: u8 = 0x03; // BIT STRING
    pub const OCTSTR: u8 = 0x04; // OCTET STRING
    pub const OID: u8 = 0x06; // OBJECT IDENTIFIER
    pub const BOOL: u8 = 0x01; // BOOLEAN
    pub const UTC: u8 = 0x17; // UTCTime
    pub const GENT: u8 = 0x18; // GeneralizedTime
    pub const CTX0: u8 = 0x20; // [0]（version）
    pub const CTX1: u8 = 0x21; // [1]（issuerUniqueID）
    pub const CTX2: u8 = 0x22; // [2]（subjectUniqueID）
    pub const CTX3: u8 = 0x23; // [3]（extensions）
}

/// 10 進 2 桁を読む（ASCII 数字以外は `None`）。C の `read-dec2` 相当。
fn dec2(content: &[u8], p: &mut usize) -> Option<u32> {
    let a = content.get(*p).filter(|c| c.is_ascii_digit())?;
    *p += 1;
    let b = content.get(*p).filter(|c| c.is_ascii_digit())?;
    *p += 1;
    Some((a - b'0') as u32 * 10 + (b - b'0') as u32)
}

/// 日付（UTCTime / GeneralizedTime）を検証する。C の `read-date` 相当（日数計算は
/// 不要なので書式・範囲検査のみ）。失敗で `None`。
fn read_date(r: &mut Der<'_>) -> Option<()> {
    let (c, v) = r.tag()?;
    if c || (v != tag_val::UTC && v != tag_val::GENT) {
        return None;
    }
    let y4 = v == tag_val::GENT;
    let l = r.len()?;
    let content = r.take(l)?;
    let mut p = 0usize;

    // 年（UTCTime は 2 桁、GeneralizedTime は 4 桁）
    let year = if y4 {
        let a = dec2(content, &mut p)?;
        let b = dec2(content, &mut p)?;
        a * 100 + b
    } else {
        let yy = dec2(content, &mut p)?;
        if yy < 50 {
            yy + 2000
        } else {
            yy + 1900
        }
    };

    // 月 1..12
    let month = dec2(content, &mut p)?;
    if !(1..=12).contains(&month) {
        return None;
    }

    // 日（うるう年は 2 月 29 日まで）
    let leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    let max_day = match month {
        2 if leap => 29,
        2 => 28,
        4 | 6 | 9 | 11 => 30,
        _ => 31,
    };
    let day = dec2(content, &mut p)?;
    if day < 1 || day > max_day {
        return None;
    }

    // 時 0..23 / 分 0..59 / 秒 0..60（うるう秒）
    if dec2(content, &mut p)? > 23 {
        return None;
    }
    if dec2(content, &mut p)? > 59 {
        return None;
    }
    if dec2(content, &mut p)? > 60 {
        return None;
    }

    // 小数秒（任意。'.' の後に数字 0 個以上）
    if p < content.len() && content[p] == b'.' {
        p += 1;
        while p < content.len() && content[p].is_ascii_digit() {
            p += 1;
        }
    }

    // タイムゾーン 'Z' で終端（余分なバイトは不可）
    if content.get(p) != Some(&b'Z') {
        return None;
    }
    p += 1;
    if p != content.len() {
        return None;
    }
    Some(())
}

/// DER 証明書 → トラストアンカー（C の `ta_add` 相当）。失敗で `None`。
///
/// `br_x509_decoder` の全構文規則（version 0..2 / validity 日付検証 / extensions /
/// basicConstraints / signature / 末尾余分バイト拒否 / RSA・EC 公開鍵のみ）を忠実に
/// 再現する。DN は subject Name の DER 全体（SEQUENCE のタグ+長さ+内容）。
pub fn ta_add(der: &[u8]) -> Option<TrustAnchor> {
    let mut r = Der::new(der);

    // Certificate ::= SEQUENCE { tbsCertificate, signatureAlgorithm, signatureValue }
    let (_cert_len, cert_saved) = r.seq()?;

    // tbsCertificate ::= SEQUENCE
    let (_tbs_len, tbs_saved) = r.seq()?;

    // [0] EXPLICIT version OPTIONAL（0xA0 = context[0] constructed）
    if r.peek(1) == Some(&[0xA0]) {
        let (_vl, vsaved) = r.ctx_open(tag_val::CTX0)?;
        // version: read-small-int-value（先頭バイト < 0x80、値 <= 2）
        let il = r.integer()?;
        if il == 0 {
            return None;
        }
        let first = r.byte()?;
        if first >= 0x80 {
            return None;
        }
        let mut iv = first as u32;
        for _ in 1..il {
            if iv >= 0xFF_FFFF {
                return None;
            }
            iv = (iv << 8) | r.byte()? as u32;
        }
        if iv > 2 {
            return None; // version 3+ は非対応
        }
        r.close_elt(vsaved)?;
    }

    // serialNumber: INTEGER（skip）
    let sn = r.integer()?;
    r.skip(sn)?;

    // signature: SEQUENCE（skip）
    let (sig, sig_saved) = r.seq()?;
    r.skip(sig)?;
    r.close_elt(sig_saved)?;

    // issuer: Name（SEQUENCE。skip）
    let (iss, iss_saved) = r.seq()?;
    r.skip(iss)?;
    r.close_elt(iss_saved)?;

    // validity: SEQUENCE { notBefore, notAfter }（日付を検証）
    let (_val, val_saved) = r.seq()?;
    read_date(&mut r)?;
    read_date(&mut r)?;
    r.close_elt(val_saved)?;

    // subject: Name（SEQUENCE。DER 全体を捕捉）
    let subj_start = r.p;
    let (subj, subj_saved) = r.seq()?;
    r.skip(subj)?;
    let dn = der[subj_start..r.p].to_vec();
    if dn.is_empty() {
        return None;
    }
    r.close_elt(subj_saved)?;

    // subjectPublicKeyInfo ::= SEQUENCE { algorithm, subjectPublicKey }
    let (_spki, spki_saved) = r.seq()?;

    // algorithm ::= SEQUENCE { OID, ... }
    let (_alg, alg_saved) = r.seq()?;
    // OID（>255 バイトは BearSSL の read-OID が fail 扱い）
    let oid_len = r.oid_len()?;
    if oid_len > 255 {
        return None;
    }
    let oid = r.take(oid_len)?;

    let pkey = if oid == OID_RSA_ENCRYPTION {
        // 残りの algorithm 内容（NULL 等）を飛ばす
        r.skip_remaining();
        r.close_elt(alg_saved)?;
        // subjectPublicKey: BIT STRING → RSAPublicKey ::= SEQUENCE { n, e }
        let (bsl, bs_saved) = r.bitstring()?;
        if bsl == 0 {
            return None;
        }
        let unused = r.byte()?;
        if unused != 0 {
            return None; // 部分バイトあり（BIT STRING の未使用ビット）
        }
        let (_rsa, rsa_saved) = r.seq()?;
        let n = read_rsa_integer(&mut r)?;
        let e = read_rsa_integer(&mut r)?;
        r.close_elt(rsa_saved)?;
        r.close_elt(bs_saved)?;
        Pkey::Rsa { n, e }
    } else if oid == OID_EC_PUBLIC_KEY {
        // parameters: curve OID（read-curve-ID）
        let curve_len = r.oid_len()?;
        if curve_len > 255 {
            return None;
        }
        let curve_oid = r.take(curve_len)?;
        let curve = if curve_oid == OID_P256 {
            23
        } else if curve_oid == OID_P384 {
            24
        } else if curve_oid == OID_P521 {
            25
        } else {
            return None;
        };
        r.close_elt(alg_saved)?; // algorithm は OID 2 個で終端（NULL 等なし）
        let (bsl, bs_saved) = r.bitstring()?;
        if bsl == 0 {
            return None;
        }
        let unused = r.byte()?;
        if unused != 0 {
            return None;
        }
        let q = r.take(bsl - 1)?.to_vec();
        r.close_elt(bs_saved)?;
        Pkey::Ec { curve, q }
    } else {
        return None; // 非対応の公開鍵
    };
    r.close_elt(spki_saved)?;

    // [1]/[2]/[3] 任意（issuerUniqueID / subjectUniqueID / extensions）
    loop {
        if r.p >= r.lim {
            break; // read-tag-or-end: tbs 終端
        }
        let b = r.peek(1)?[0];
        if b == 0x81 || b == 0x82 {
            // [1] issuerUniqueID / [2] subjectUniqueID: skip
            let (_l, saved) =
                r.ctx_open(if b == 0x81 { tag_val::CTX1 } else { tag_val::CTX2 })?;
            r.skip_remaining();
            r.close_elt(saved)?;
        } else if b == 0xA3 {
            // [3] extensions（EXPLICIT）: 構造を検証しつつ走査
            let (_l, ext_saved) = r.ctx_open(tag_val::CTX3)?;
            let (_seq, seq_saved) = r.seq()?;
            while r.p < r.lim {
                let (_e, e_saved) = r.seq()?;
                // extension OID
                let ol = r.oid_len()?;
                let oid = r.take(ol)?;
                // 任意の critical BOOLEAN（0x01）
                if r.peek(1) == Some(&[0x01]) {
                    let (bc, bv) = r.tag()?;
                    if bc || bv != tag_val::BOOL {
                        return None;
                    }
                    let bl = r.len()?;
                    if bl != 1 {
                        return None; // read-boolean: 長さ 1
                    }
                    r.byte()?; // 値（無視）
                }
                // extnValue: OCTET STRING
                let (oc, ov) = r.tag()?;
                if oc || ov != tag_val::OCTSTR {
                    return None;
                }
                let (_xl, x_saved) = r.open()?;
                if oid == OID_BASIC_CONSTRAINTS {
                    // process-basicConstraints: SEQUENCE { BOOLEAN OPTIONAL, ... }
                    let (_bc, bc_saved) = r.seq()?;
                    if r.p < r.lim {
                        let (c, v) = r.tag()?;
                        if !c && v == tag_val::BOOL {
                            let bl = r.len()?;
                            if bl != 1 {
                                return None;
                            }
                            r.byte()?;
                        }
                    }
                    r.skip_remaining();
                    r.close_elt(bc_saved)?;
                } else {
                    r.skip_remaining();
                }
                r.close_elt(x_saved)?;
                r.close_elt(e_saved)?;
            }
            r.close_elt(seq_saved)?;
            r.close_elt(ext_saved)?;
        } else {
            return None; // ERR_X509_UNEXPECTED
        }
    }
    r.close_elt(tbs_saved)?;

    // signatureAlgorithm ::= SEQUENCE { OID, ... }（OID は 255 超なら読み捨て）
    let (_sigalg, sigalg_saved) = r.seq()?;
    let ol = r.oid_len()?;
    r.skip(ol)?;
    r.skip_remaining();
    r.close_elt(sigalg_saved)?;

    // signatureValue: BIT STRING
    let (sv, sv_saved) = r.bitstring()?;
    if sv == 0 {
        return None;
    }
    let unused = r.byte()?;
    if unused != 0 {
        return None;
    }
    r.skip_remaining();
    r.close_elt(sv_saved)?;

    // close-elt: Certificate + 末尾余分バイト拒否
    r.close_elt(cert_saved)?;
    if r.p != der.len() {
        return None; // ERR_X509_EXTRA_ELEMENT（末尾バイト）
    }

    Some(TrustAnchor { dn, pkey })
}

/// PEM バンドルから証明書（DER）を抽出する。C の `ca_load_pem` の抽出部分相当。
///
/// `-----BEGIN CERTIFICATE-----` / `-----END CERTIFICATE-----` の組を走査し、間の
/// base64 を [`b64_decode`] で復号する。復号に失敗した証明書は無視（C と同じく
/// 1 枚でも成功すれば前進する）。戻り値は復号に成功した DER 証明書の列。
pub fn ca_load_pem(pem: &[u8]) -> Vec<Vec<u8>> {
    const BEGIN: &[u8] = b"-----BEGIN CERTIFICATE-----";
    const END: &[u8] = b"-----END CERTIFICATE-----";
    let mut certs = Vec::new();
    let mut p = 0usize;
    while p + BEGIN.len() <= pem.len() {
        if &pem[p..p + BEGIN.len()] != BEGIN {
            p += 1;
            continue;
        }
        let b64 = p + BEGIN.len();
        let mut e = b64;
        while e + END.len() <= pem.len() && &pem[e..e + END.len()] != END {
            e += 1;
        }
        if e + END.len() > pem.len() {
            break;
        }
        if let Some(der) = b64_decode(&pem[b64..e]) {
            if !der.is_empty() {
                certs.push(der);
            }
        }
        p = e + END.len();
    }
    certs
}

/// PEM バンドルからトラストアンカー列へ（C の `ca_load_pem` 全体相当）。復号・
/// [`ta_add`] に成功したアンカーだけを返す。
pub fn ca_load_pem_anchors(pem: &[u8]) -> Vec<TrustAnchor> {
    ca_load_pem(pem).into_iter().filter_map(|der| ta_add(&der)).collect()
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

    #[test]
    fn ca_load_pem_extracts() {
        // BEGIN/END の間の base64 を復号して DER 列へ
        let pem = b"garbage\n-----BEGIN CERTIFICATE-----\naGVsbG8=\n-----END CERTIFICATE-----\ntrailing\n\
                    -----BEGIN CERTIFICATE-----\nd29ybGQ=\n-----END CERTIFICATE-----\n";
        assert_eq!(ca_load_pem(pem), vec![b"hello".to_vec(), b"world".to_vec()]);
        // 破損 base64 は無視（次の証明書へ進む）
        let pem2 = b"-----BEGIN CERTIFICATE-----\n!!!!\n-----END CERTIFICATE-----\n\
                     -----BEGIN CERTIFICATE-----\naGk=\n-----END CERTIFICATE-----\n";
        assert_eq!(ca_load_pem(pem2), vec![b"hi".to_vec()]);
        // 空入力
        assert_eq!(ca_load_pem(b""), Vec::<Vec<u8>>::new());
    }

    #[test]
    fn ta_add_rejects_garbage() {
        assert_eq!(ta_add(b""), None);
        assert_eq!(ta_add(b"\x30\x00"), None); // 空 Certificate
        assert_eq!(ta_add(b"\x30\x03\x02\x01\x01"), None); // tbsCertificate なし
        assert_eq!(ta_add(b"not a cert at all"), None);
    }

    /// 最小 RSA 証明書（tbs: version なし・serial 1・signature/issuer/validity/subject
    /// を空 SEQUENCE にした合成入力）は validity 日付が無いため拒否される。
    #[test]
    fn ta_add_requires_dates() {
        // Certificate { tbs { serial=1, sig={}, issuer={}, validity={}, subject={}, spki... } }
        // validity が空 SEQUENCE なので read_date が失敗する
        let der = b"\x30\x2a\x30\x28\x02\x01\x01\x30\x00\x30\x00\x30\x00\x30\x00\
                    \x30\x0c\x30\x0a\x06\x08\x2a\x86\x48\x86\xf7\x0d\x01\x01\x01\x05\x00";
        assert_eq!(ta_add(der), None);
    }
}
