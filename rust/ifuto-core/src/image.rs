//! 軽量画像デコード（BMP / PNG。C の `src/image.c` 相当）。
//!
//! | C (image.h / image.c) | Rust |
//! |---|---|
//! | `IfImage` | [`Image`] |
//! | `if_img_decode` | [`decode`] |
//! | `if_img_free` | （所有権により不要） |
//!
//! # 実装済み
//!
//! - **PNG**: チャンク走査 → IDAT 連結 → zlib inflate（RFC1950/1951）→ フィルタ解除
//!   （None/Sub/Up/Average/Paeth）→ 色変換（グレー/RGB/グレー+α/RGBA）。8bit 深度のみ、
//!   パレット・インターレースは拒否。zlib inflate はストアド/固定/動的ハフマンを内蔵。
//! - **BMP**: 無圧縮 24/32bpp（BITMAPINFOHEADER のみ。ボトムアップ対応）。RLE・16bpp・
//!   パレットは拒否。
//! - メモリ上限: 1 画像 64MB（`IMG_MAX_BYTES`）、次元 16384 まで。
//!
//! # C との違い（所有権による構造的な改善）
//!
//! C は `malloc`/`realloc`/`free` を手動管理し、出力を `u8 *px`（`w*h*4`）で返す。
//! Rust では `Vec<u8>` に置き換え、`Option<Image>` + エラー文字列で明白に失敗する。
//! 二重 free・free 漏れ・バッファ境界検査漏れを構造的に排除する。
//!
//! 出力ピクセル列・エラー文言は C と byte 一致（差分 fuzz で実証）。

/// 1 画像のメモリ上限（C の `IMG_MAX_BYTES`）。
const IMG_MAX_BYTES: u64 = 64 << 20;
/// 次元上限（C の `IMG_MAX_DIM`）。
const IMG_MAX_DIM: u32 = 16384;

/// デコード済み画像（C の `IfImage` 相当）。
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Image {
    /// 幅。
    pub w: u32,
    /// 高さ。
    pub h: u32,
    /// `w*h*4` の RGBA8888 ピクセル列。
    pub px: Vec<u8>,
}

// ================= ビットリーダー =================

struct BitR<'a> {
    p: &'a [u8],
    pos: usize,
    acc: u32,
    acc_n: u32,
    err: bool,
}

impl<'a> BitR<'a> {
    fn new(p: &'a [u8]) -> Self {
        BitR {
            p,
            pos: 0,
            acc: 0,
            acc_n: 0,
            err: false,
        }
    }

    fn byte(&mut self) -> u32 {
        if self.pos >= self.p.len() {
            self.err = true;
            return 0;
        }
        let v = self.p[self.pos] as u32;
        self.pos += 1;
        v
    }

    /// n ビット（LSB first。deflate は LSB 順）。
    fn bits(&mut self, n: u32) -> u32 {
        while self.acc_n < n {
            self.acc |= self.byte() << self.acc_n;
            self.acc_n += 8;
            if self.err {
                return 0;
            }
        }
        let v = self.acc & ((1u32 << n) - 1);
        self.acc >>= n;
        self.acc_n -= n;
        v
    }

    fn align_byte(&mut self) {
        self.acc = 0;
        self.acc_n = 0;
    }
}

// ================= deflate =================

const HLIT_MAX: usize = 288;
const HDIST_MAX: usize = 32;
const SYM_CAP: usize = HLIT_MAX + HDIST_MAX; // 320

/// 正準ハフマン。C の `Huff` 相当。
struct Huff {
    count: [u16; 16],
    first: [u16; 16],
    sym: [[u16; SYM_CAP]; 16],
    n: u16,
}

impl Huff {
    fn new() -> Self {
        Huff {
            count: [0; 16],
            first: [0; 16],
            sym: [[0; SYM_CAP]; 16],
            n: 0,
        }
    }

    /// length 配列から構成。C の `huff_build` 相当。
    fn build(&mut self, lens: &[u8], n: u16) -> bool {
        self.count = [0; 16];
        self.first = [0; 16];
        self.n = n;
        for &l in lens.iter().take(n as usize) {
            if l > 15 {
                return false;
            }
            if l != 0 {
                self.count[l as usize] += 1;
            }
        }
        let mut code = 0u32;
        for bits in 1..=15usize {
            code = (code + self.count[bits - 1] as u32) << 1;
            self.first[bits] = code as u16;
        }
        let mut fill = [0u16; 16];
        for (i, &l) in lens.iter().take(n as usize).enumerate() {
            if l != 0 {
                let l = l as usize;
                let c = self.first[l] + fill[l];
                fill[l] += 1;
                if c as u32 >= (1u32 << l) {
                    return false; // 過密（壊れ）
                }
                self.sym[l][(c - self.first[l]) as usize] = i as u16;
            }
        }
        true
    }

    /// 1 シンボル復号。C の `huff_sym` 相当。エラーは `None`。
    fn sym(&self, b: &mut BitR) -> Option<u16> {
        let mut code = 0u32;
        for l in 1..=15usize {
            code = (code << 1) | b.bits(1);
            if b.err {
                return None;
            }
            if code >= self.first[l] as u32 {
                let idx = (code - self.first[l] as u32) as usize;
                if idx < self.count[l] as usize {
                    return Some(self.sym[l][idx]);
                }
            }
        }
        None
    }
}

const LEN_EXTRA: [u8; 29] = [0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0];
const LEN_BASE: [u32; 29] = [3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258];
const DIST_EXTRA: [u8; 30] = [0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13];
const DIST_BASE: [u32; 30] = [1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577];

fn huff_fixed_lit(h: &mut Huff) {
    let mut lens = [0u8; 288];
    for l in lens.iter_mut().take(144) {
        *l = 8;
    }
    for l in lens.iter_mut().take(256).skip(144) {
        *l = 9;
    }
    for l in lens.iter_mut().take(280).skip(256) {
        *l = 7;
    }
    for l in lens.iter_mut().take(288).skip(280) {
        *l = 8;
    }
    h.build(&lens, 288);
}

fn huff_fixed_dist(h: &mut Huff) {
    let lens = [5u8; 32];
    h.build(&lens, 32);
}

/// deflate 全体を展開。C の `inflate` 相当。
fn inflate(src: &[u8], out_cap: u32) -> Option<Vec<u8>> {
    let mut b = BitR::new(src);
    let mut out = Vec::new();
    let mut final_ = false;
    let mut lit = Huff::new();
    let mut dist = Huff::new();
    let mut clen = Huff::new();
    while !final_ && !b.err {
        final_ = b.bits(1) != 0;
        let type_ = b.bits(2);
        if type_ == 0 {
            // ストアド
            b.align_byte();
            let len = b.byte() | (b.byte() << 8);
            let nlen = b.byte() | (b.byte() << 8);
            if (len ^ 0xFFFF) != nlen {
                b.err = true;
                break;
            }
            if out.len() as u32 + len > out_cap {
                b.err = true;
                break;
            }
            for _ in 0..len {
                out.push(b.byte() as u8);
            }
        } else if type_ == 1 {
            huff_fixed_lit(&mut lit);
            huff_fixed_dist(&mut dist);
        } else if type_ == 2 {
            let hlit = b.bits(5) + 257;
            let hdist = b.bits(5) + 1;
            let hclen = b.bits(4) + 4;
            const CL_ORDER: [u8; 19] = [16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15];
            let mut cl_lens = [0u8; 19];
            for &i in CL_ORDER.iter().take(hclen as usize) {
                cl_lens[i as usize] = b.bits(3) as u8;
            }
            if !clen.build(&cl_lens, 19) {
                b.err = true;
                break;
            }
            let mut lens = [0u8; SYM_CAP];
            let mut lidx = 0usize;
            while lidx < (hlit + hdist) as usize {
                let sym = match clen.sym(&mut b) {
                    Some(s) => s,
                    None => {
                        b.err = true;
                        break;
                    }
                };
                if sym < 16 {
                    lens[lidx] = sym as u8;
                    lidx += 1;
                } else if sym == 16 {
                    let rep = b.bits(2) + 3;
                    if lidx == 0 {
                        b.err = true;
                        break;
                    }
                    let prev = lens[lidx - 1];
                    for _ in 0..rep {
                        if lidx >= (hlit + hdist) as usize {
                            b.err = true;
                            break;
                        }
                        lens[lidx] = prev;
                        lidx += 1;
                    }
                } else if sym == 17 {
                    let rep = b.bits(3) + 3;
                    for _ in 0..rep {
                        if lidx >= (hlit + hdist) as usize {
                            b.err = true;
                            break;
                        }
                        lens[lidx] = 0;
                        lidx += 1;
                    }
                } else {
                    let rep = b.bits(7) + 11;
                    for _ in 0..rep {
                        if lidx >= (hlit + hdist) as usize {
                            b.err = true;
                            break;
                        }
                        lens[lidx] = 0;
                        lidx += 1;
                    }
                }
            }
            if b.err {
                break;
            }
            if !lit.build(&lens, hlit as u16) {
                b.err = true;
                break;
            }
            if !dist.build(&lens[hlit as usize..], hdist as u16) {
                b.err = true;
                break;
            }
        } else {
            b.err = true;
            break;
        }
        // 共通: シンボルループ（type 0 は continue 相当でここに来ない）
        if type_ != 0 {
            loop {
                let sym = match lit.sym(&mut b) {
                    Some(s) => s,
                    None => {
                        b.err = true;
                        break;
                    }
                };
                if sym < 256 {
                    if out.len() >= out_cap as usize {
                        b.err = true;
                        break;
                    }
                    out.push(sym as u8);
                } else if sym == 256 {
                    break;
                } else {
                    let li = (sym - 257) as usize;
                    if li >= 29 {
                        b.err = true;
                        break;
                    }
                    let len = LEN_BASE[li] + b.bits(LEN_EXTRA[li] as u32);
                    let dsym = match dist.sym(&mut b) {
                        Some(s) => s,
                        None => {
                            b.err = true;
                            break;
                        }
                    };
                    if dsym >= 30 {
                        b.err = true;
                        break;
                    }
                    let d = DIST_BASE[dsym as usize] + b.bits(DIST_EXTRA[dsym as usize] as u32);
                    if d as usize > out.len() {
                        b.err = true;
                        break;
                    }
                    if out.len() as u32 + len > out_cap {
                        b.err = true;
                        break;
                    }
                    let src_i = out.len() - d as usize;
                    for i in 0..len as usize {
                        let v = out[src_i + i];
                        out.push(v);
                    }
                }
            }
        }
    }
    if b.err {
        None
    } else {
        Some(out)
    }
}

// ================= PNG =================

const CRC_TAB: [u32; 256] = [
    0x00000000,0x77073096,0xEE0E612C,0x990951BA,0x076DC419,0x706AF48F,0xE963A535,0x9E6495A3,
    0x0EDB8832,0x79DCB8A4,0xE0D5E91E,0x97D2D988,0x09B64C2B,0x7EB17CBD,0xE7B82D07,0x90BF1D91,
    0x1DB71064,0x6AB020F2,0xF3B97148,0x84BE41DE,0x1ADAD47D,0x6DDDE4EB,0xF4D4B551,0x83D385C7,
    0x136C9856,0x646BA8C0,0xFD62F97A,0x8A65C9EC,0x14015C4F,0x63066CD9,0xFA0F3D63,0x8D080DF5,
    0x3B6E20C8,0x4C69105E,0xD56041E4,0xA2677172,0x3C03E4D1,0x4B04D447,0xD20D85FD,0xA50AB56B,
    0x35B5A8FA,0x42B2986C,0xDBBBC9D6,0xACBCF940,0x32D86CE3,0x45DF5C75,0xDCD60DCF,0xABD13D59,
    0x26D930AC,0x51DE003A,0xC8D75180,0xBFD06116,0x21B4F4B5,0x56B3C423,0xCFBA9599,0xB8BDA50F,
    0x2802B89E,0x5F058808,0xC60CD9B2,0xB10BE924,0x2F6F7C87,0x58684C11,0xC1611DAB,0xB6662D3D,
    0x76DC4190,0x01DB7106,0x98D220BC,0xEFD5102A,0x71B18589,0x06B6B51F,0x9FBFE4A5,0xE8B8D433,
    0x7807C9A2,0x0F00F934,0x9609A88E,0xE10E9818,0x7F6A0DBB,0x086D3D2D,0x91646C97,0xE6635C01,
    0x6B6B51F4,0x1C6C6162,0x856530D8,0xF262004E,0x6C0695ED,0x1B01A57B,0x8208F4C1,0xF50FC457,
    0x65B0D9C6,0x12B7E950,0x8BBEB8EA,0xFCB9887C,0x62DD1DDF,0x15DA2D49,0x8CD37CF3,0xFBD44C65,
    0x4DB26158,0x3AB551CE,0xA3BC0074,0xD4BB30E2,0x4ADFA541,0x3DD895D7,0xA4D1C46D,0xD3D6F4FB,
    0x4369E96A,0x346ED9FC,0xAD678846,0xDA60B8D0,0x44042D73,0x33031DE5,0xAA0A4C5F,0xDD0D7CC9,
    0x5005713C,0x270241AA,0xBE0B1010,0xC90C2086,0x5768B525,0x206F85B3,0xB966D409,0xCE61E49F,
    0x5EDEF90E,0x29D9C998,0xB0D09822,0xC7D7A8B4,0x59B33D17,0x2EB40D81,0xB7BD5C3B,0xC0BA6CAD,
    0xEDB88320,0x9ABFB3B6,0x03B6E20C,0x74B1D29A,0xEAD54739,0x9DD277AF,0x04DB2615,0x73DC1683,
    0xE3630B12,0x94643B84,0x0D6D6A3E,0x7A6A5AA8,0xE40ECF0B,0x9309FF9D,0x0A00AE27,0x7D079EB1,
    0xF00F9344,0x8708A3D2,0x1E01F268,0x6906C2FE,0xF762575D,0x806567CB,0x196C3671,0x6E6B06E7,
    0xFED41B76,0x89D32BE0,0x10DA7A5A,0x67DD4ACC,0xF9B9DF6F,0x8EBEEFF9,0x17B7BE43,0x60B08ED5,
    0xD6D6A3E8,0xA1D1937E,0x38D8C2C4,0x4FDFF252,0xD1BB67F1,0xA6BC5767,0x3FB506DD,0x48B2364B,
    0xD80D2BDA,0xAF0A1B4C,0x36034AF6,0x41047A60,0xDF60EFC3,0xA867DF55,0x316E8EEF,0x4669BE79,
    0xCB61B38C,0xBC66831A,0x256FD2A0,0x5268E236,0xCC0C7795,0xBB0B4703,0x220216B9,0x5505262F,
    0xC5BA3BBE,0xB2BD0B28,0x2BB45A92,0x5CB36A04,0xC2D7FFA7,0xB5D0CF31,0x2CD99E8B,0x5BDEAE1D,
    0x9B64C2B0,0xEC63F226,0x756AA39C,0x026D930A,0x9C0906A9,0xEB0E363F,0x72076785,0x05005713,
    0x95BF4A82,0xE2B87A14,0x7BB12BAE,0x0CB61B38,0x92D28E9B,0xE5D5BE0D,0x7CDCEFB7,0x0BDBDF21,
    0x86D3D2D4,0xF1D4E242,0x68DDB3F8,0x1FDA836E,0x81BE16CD,0xF6B9265B,0x6FB077E1,0x18B74777,
    0x88085AE6,0xFF0F6A70,0x66063BCA,0x11010B5C,0x8F659EFF,0xF862AE69,0x616BFFD3,0x166CCF45,
    0xA00AE278,0xD70DD2EE,0x4E048354,0x3903B3C2,0xA7672661,0xD06016F7,0x4969474D,0x3E6E77DB,
    0xAED16A4A,0xD9D65ADC,0x40DF0B66,0x37D83BF0,0xA9BCAE53,0xDEBB9EC5,0x47B2CF7F,0x30B5FFE9,
    0xBDBDF21C,0xCABAC28A,0x53B39330,0x24B4A3A6,0xBAD03605,0xCDD70693,0x54DE5729,0x23D967BF,
    0xB3667A2E,0xC4614AB8,0x5D681B02,0x2A6F2B94,0xB40BBE37,0xC30C8EA1,0x5A05DF1B,0x2D02EF8D,
];

fn png_crc_raw(mut crc: u32, p: &[u8]) -> u32 {
    for &b in p {
        crc = (crc >> 8) ^ CRC_TAB[((crc ^ b as u32) & 0xFF) as usize];
    }
    crc
}

/// PNG チャンク CRC: tag(4) + data(n)。C の `png_crc` 相当。
fn png_crc(tag: &[u8], data: &[u8]) -> u32 {
    let mut crc = png_crc_raw(0xFFFF_FFFF, tag);
    if !data.is_empty() {
        crc = png_crc_raw(crc, data);
    }
    crc ^ 0xFFFF_FFFF
}

fn paeth(a: u8, b: u8, c: u8) -> u8 {
    let p = a as i32 + b as i32 - c as i32;
    let pa = (p - a as i32).abs();
    let pb = (p - b as i32).abs();
    let pc = (p - c as i32).abs();
    if pa <= pb && pa <= pc {
        a
    } else if pb <= pc {
        b
    } else {
        c
    }
}

/// PNG デコード本体。C の `png_decode` 相当。
fn png_decode(data: &[u8], w: u32, h: u32, ct: u8) -> Result<Vec<u8>, String> {
    let bpp = match ct {
        0 => 1, // グレー
        2 => 3, // RGB
        4 => 2, // グレー+α
        6 => 4, // RGBA
        other => return Err(format!("png: unsupported color type {other}")),
    };
    let stride = w * bpp;
    let raw_len = (stride as u64 + 1) * h as u64;
    if raw_len > IMG_MAX_BYTES {
        return Err("png: image too large".to_string());
    }

    // IDAT 連結
    let mut pos = 8usize; // シグネチャ後
    let mut idat: Vec<u8> = Vec::new();
    let mut got_iend = false;
    while pos + 8 <= data.len() {
        let clen = read_be32(data, &mut pos) as usize;
        let tag = [data[pos], data[pos + 1], data[pos + 2], data[pos + 3]];
        pos += 4;
        if pos + clen > data.len() {
            break;
        }
        let data_off = pos;
        let crc_off = data_off + clen;
        let mut crc_pos = crc_off;
        let crc = read_be32(data, &mut crc_pos);
        // CRC 検証（タグ + データ）
        let ccrc = png_crc(&tag, &data[data_off..data_off + clen]);
        if ccrc != crc {
            break;
        }
        if &tag == b"IDAT" {
            if idat.len() + clen > IMG_MAX_BYTES as usize {
                break;
            }
            idat.extend_from_slice(&data[data_off..data_off + clen]);
        } else if &tag == b"IEND" {
            got_iend = true;
            break;
        }
        pos = crc_off + 4;
    }
    if !got_iend || idat.len() < 2 {
        return Err("png: missing IEND".to_string());
    }

    // zlib: 2 バイトヘッダ（CMF/FLG）→ deflate
    let raw = match inflate(&idat[2..], raw_len as u32) {
        Some(r) => r,
        None => return Err("png: inflate failed".to_string()),
    };
    if raw.len() as u64 != raw_len {
        return Err("png: inflate failed".to_string());
    }

    // フィルタ解除 → RGBA（C は raw をその場で書き換え、prev は解除済み前行を指す）
    let mut raw = raw;
    let mut out = vec![0u8; (w as u64 * h as u64 * 4) as usize];
    for y in 0..h as usize {
        let f = raw[y * (stride as usize + 1)];
        let row_off = y * (stride as usize + 1) + 1;
        let prev_off = if y > 0 { (y - 1) * (stride as usize + 1) + 1 } else { 0 };
        match f {
            0 => {}
            1 => {
                // Sub
                for i in bpp as usize..stride as usize {
                    raw[row_off + i] = raw[row_off + i].wrapping_add(raw[row_off + i - bpp as usize]);
                }
            }
            2 => {
                // Up
                if y > 0 {
                    for i in 0..stride as usize {
                        raw[row_off + i] = raw[row_off + i].wrapping_add(raw[prev_off + i]);
                    }
                }
            }
            3 => {
                // Average
                for i in 0..stride as usize {
                    let a = if i >= bpp as usize { raw[row_off + i - bpp as usize] } else { 0 };
                    let b = if y > 0 { raw[prev_off + i] } else { 0 };
                    raw[row_off + i] = raw[row_off + i].wrapping_add(((a as u16 + b as u16) >> 1) as u8);
                }
            }
            4 => {
                // Paeth
                for i in 0..stride as usize {
                    let a = if i >= bpp as usize { raw[row_off + i - bpp as usize] } else { 0 };
                    let b = if y > 0 { raw[prev_off + i] } else { 0 };
                    let c = if i >= bpp as usize && y > 0 { raw[prev_off + i - bpp as usize] } else { 0 };
                    raw[row_off + i] = raw[row_off + i].wrapping_add(paeth(a, b, c));
                }
            }
            other => return Err(format!("png: bad filter {other}")),
        }
        // RGBA へ
        let row = &raw[row_off..row_off + stride as usize];
        let px = &mut out[y * w as usize * 4..(y + 1) * w as usize * 4];
        for x in 0..w as usize {
            let o = x * 4;
            match ct {
                0 => {
                    px[o] = row[x];
                    px[o + 1] = row[x];
                    px[o + 2] = row[x];
                    px[o + 3] = 255;
                }
                2 => {
                    px[o] = row[x * 3];
                    px[o + 1] = row[x * 3 + 1];
                    px[o + 2] = row[x * 3 + 2];
                    px[o + 3] = 255;
                }
                4 => {
                    px[o] = row[x * 2];
                    px[o + 1] = row[x * 2];
                    px[o + 2] = row[x * 2];
                    px[o + 3] = row[x * 2 + 1];
                }
                6 => {
                    px[o] = row[x * 4];
                    px[o + 1] = row[x * 4 + 1];
                    px[o + 2] = row[x * 4 + 2];
                    px[o + 3] = row[x * 4 + 3];
                }
                _ => unreachable!(),
            }
        }
    }
    Ok(out)
}

fn read_be32(data: &[u8], pos: &mut usize) -> u32 {
    if *pos + 4 > data.len() {
        *pos = data.len() + 1;
        return 0;
    }
    let v = ((data[*pos] as u32) << 24)
        | ((data[*pos + 1] as u32) << 16)
        | ((data[*pos + 2] as u32) << 8)
        | (data[*pos + 3] as u32);
    *pos += 4;
    v
}

/// C の `png_read_byte` 相当（範囲外は 0 を返し pos を n+1 へ）。
fn read_byte(data: &[u8], pos: &mut usize) -> u8 {
    if *pos >= data.len() {
        *pos = data.len() + 1;
        return 0;
    }
    let v = data[*pos];
    *pos += 1;
    v
}

// ================= BMP =================

fn bmp_decode(data: &[u8]) -> Result<(u32, u32, Vec<u8>), String> {
    if data.len() < 54 || data[0] != b'B' || data[1] != b'M' {
        return Err("bmp: not a BMP file".to_string());
    }
    let data_off = u32::from_le_bytes([data[10], data[11], data[12], data[13]]);
    let bw = u32::from_le_bytes([data[18], data[19], data[20], data[21]]);
    let h_raw = i32::from_le_bytes([data[22], data[23], data[24], data[25]]);
    let bpp = u16::from_le_bytes([data[28], data[29]]);
    let comp = u32::from_le_bytes([data[30], data[31], data[32], data[33]]);
    if bw == 0 || bw > IMG_MAX_DIM || h_raw == 0 || h_raw > IMG_MAX_DIM as i32 {
        return Err("bmp: bad dimensions".to_string());
    }
    if comp != 0 {
        return Err("bmp: compressed BMP not supported".to_string());
    }
    let flip = h_raw > 0; // 正の高さ = ボトムアップ
    let bh = h_raw.unsigned_abs();
    let bytespp = (bpp / 8) as u32;
    if bpp != 24 && bpp != 32 {
        return Err("bmp: only 24/32bpp supported".to_string());
    }
    if bw as u64 * bh as u64 * 4 > IMG_MAX_BYTES {
        return Err("bmp: too large".to_string());
    }
    let stride = (bw * bytespp).div_ceil(4) * 4;
    if data_off as u64 + stride as u64 * bh as u64 > data.len() as u64 {
        return Err("bmp: truncated".to_string());
    }
    let mut out = vec![0u8; (bw as u64 * bh as u64 * 4) as usize];
    for y in 0..bh {
        let src_y = if flip { bh - 1 - y } else { y };
        let row = &data[(data_off as usize + src_y as usize * stride as usize)..];
        for x in 0..bw as usize {
            let o = x * bytespp as usize;
            out[(y as usize * bw as usize + x) * 4] = row[o + 2]; // R
            out[(y as usize * bw as usize + x) * 4 + 1] = row[o + 1]; // G
            out[(y as usize * bw as usize + x) * 4 + 2] = row[o]; // B
            out[(y as usize * bw as usize + x) * 4 + 3] = if bpp == 32 { row[o + 3] } else { 255 };
        }
    }
    Ok((bw, bh, out))
}

// ================= 公開 API =================

/// PNG / BMP をデコード。失敗は `Err(理由)`。C の `if_img_decode` 相当。
pub fn decode(data: &[u8]) -> Result<Image, String> {
    if data.len() < 8 {
        return Err("image: too short".to_string());
    }
    // PNG シグネチャ: 89 50 4E 47 0D 0A 1A 0A
    if data[0] == 0x89 && data[1] == b'P' && data[2] == b'N' && data[3] == b'G' {
        let mut pos = 8usize;
        let clen = read_be32(data, &mut pos);
        let mut tag = [0u8; 4];
        for t in &mut tag {
            *t = read_byte(data, &mut pos);
        }
        if clen != 13 || &tag != b"IHDR" || pos + 13 > data.len() {
            return Err("png: bad IHDR".to_string());
        }
        let w = read_be32(data, &mut pos);
        let h = read_be32(data, &mut pos);
        let depth = read_byte(data, &mut pos);
        let ct = read_byte(data, &mut pos);
        let comp = read_byte(data, &mut pos);
        let filt = read_byte(data, &mut pos);
        let inter = read_byte(data, &mut pos);
        if w == 0 || h == 0 || w > IMG_MAX_DIM || h > IMG_MAX_DIM {
            return Err("png: bad dimensions".to_string());
        }
        if depth != 8 {
            return Err("png: only 8-bit depth supported".to_string());
        }
        if comp != 0 || filt != 0 || inter != 0 {
            return Err("png: compression/filter/interlace not supported".to_string());
        }
        let total = w as u64 * h as u64 * 4;
        if total > IMG_MAX_BYTES {
            return Err("png: image too large".to_string());
        }
        let px = png_decode(data, w, h, ct)?;
        Ok(Image { w, h, px })
    } else {
        let (w, h, px) = bmp_decode(data)?;
        Ok(Image { w, h, px })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// PNG を構築するヘルパ（テスト専用）。
    fn build_png(w: u32, h: u32, ct: u8, raw: &[u8]) -> Vec<u8> {
        // シグネチャ + IHDR + IDAT(zlib) + IEND
        let mut out = vec![0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A];
        // IHDR
        let mut ihdr = Vec::new();
        ihdr.extend_from_slice(&w.to_be_bytes());
        ihdr.extend_from_slice(&h.to_be_bytes());
        ihdr.extend_from_slice(&[8, ct, 0, 0, 0]); // depth/ct/comp/filt/inter
        let mut chunk = Vec::new();
        chunk.extend_from_slice(&13u32.to_be_bytes());
        chunk.extend_from_slice(b"IHDR");
        chunk.extend_from_slice(&ihdr);
        chunk.extend_from_slice(&png_crc(b"IHDR", &ihdr).to_be_bytes());
        out.extend_from_slice(&chunk);
        // IDAT: zlib ヘッダ + deflate(ストアド) + adler32（簡略: zlib ヘッダ + 生 + 検証なし）
        let mut idat = Vec::new();
        idat.extend_from_slice(&[0x78, 0x01]); // CMF/FLG
        // deflate ストアドブロック
        let n = raw.len();
        idat.push(0x01); // BFINAL=1, BTYPE=00
        idat.extend_from_slice(&(n as u16).to_le_bytes());
        idat.extend_from_slice(&(!(n as u16)).to_le_bytes());
        idat.extend_from_slice(raw);
        // adler32（zlib は adler32 を要求するが、C 実装は検証しない）
        idat.extend_from_slice(&[0, 0, 0, 0]);
        let mut chunk = Vec::new();
        chunk.extend_from_slice(&(idat.len() as u32).to_be_bytes());
        chunk.extend_from_slice(b"IDAT");
        chunk.extend_from_slice(&idat);
        chunk.extend_from_slice(&png_crc(b"IDAT", &idat).to_be_bytes());
        out.extend_from_slice(&chunk);
        // IEND
        let mut chunk = Vec::new();
        chunk.extend_from_slice(&0u32.to_be_bytes());
        chunk.extend_from_slice(b"IEND");
        chunk.extend_from_slice(&png_crc(b"IEND", &[]).to_be_bytes());
        out.extend_from_slice(&chunk);
        out
    }

    #[test]
    fn png_rgba() {
        // 2x2 RGBA（赤/緑/青/白）、フィルタ 0（None）
        let stride = 2 * 4;
        let mut raw = Vec::new();
        for row in [
            [255u8, 0, 0, 255, 0, 255, 0, 255],
            [0, 0, 255, 255, 255, 255, 255, 255],
        ] {
            raw.push(0); // filter
            raw.extend_from_slice(&row);
        }
        let data = build_png(2, 2, 6, &raw);
        let img = decode(&data).unwrap();
        assert_eq!((img.w, img.h), (2, 2));
        assert_eq!(
            img.px,
            vec![255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255]
        );
    }

    #[test]
    fn png_gray() {
        // 1x1 グレー
        let raw = [0u8, 128];
        let data = build_png(1, 1, 0, &raw);
        let img = decode(&data).unwrap();
        assert_eq!(img.px, vec![128, 128, 128, 255]);
    }

    #[test]
    fn bmp_24() {
        // 1x1 24bpp BMP（BGR 入力 → RGB 出力）
        let mut data = vec![0u8; 54];
        data[0] = b'B';
        data[1] = b'M';
        data[10] = 54; // data offset
        data[18] = 1; // width
        data[22] = 1; // height (positive = bottom-up)
        data[28] = 24; // bpp
        // ピクセル行（BGR）+ パディング
        data.extend_from_slice(&[0, 0, 255, 0]); // B=0, G=0, R=255, pad
        let img = decode(&data).unwrap();
        assert_eq!((img.w, img.h), (1, 1));
        assert_eq!(img.px, vec![255, 0, 0, 255]);
    }

    #[test]
    fn rejects() {
        assert!(decode(b"short").is_err()); // len < 8
        assert_eq!(decode(b"notanimage").unwrap_err(), "bmp: not a BMP file");
        assert!(decode(b"\x89PNG\r\n\x1a\nXXXX").is_err());
    }
}
