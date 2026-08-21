//! BearSSL（`vendor/bearssl`、MIT）の unsafe FFI 境界。C の `src/tls.c` の
//! `if_tls_client` / `if_tls_send_all` / `if_tls_recv` / `if_tls_close` 相当を、
//! `std::net::TcpStream` 駆動で再実装する。
//!
//! # 設計
//!
//! BearSSL の SSL エンジンは fd 非依存（`sendrec_buf` / `recvrec_buf` が生のレコード
//! バイト列を渡し、アプリ側が実際の I/O を行う）。そこで:
//! - **ソケット I/O は Rust 側**（`TcpStream` の `read` / `write_all`）が担い、
//! - **レコードの組み立て・暗号・証明書検証** を BearSSL エンジンが担う。
//!
//! C の `tls.c` が `int fd` へ raw `send`/`recv` するのに対し、本実装は `TcpStream` を
//! 所有して安全に I/O する（`std::net` への移行）。CA ロード（`ca_load`）と証明書解析
//! （`ca_load_pem` / `ta_add`）は `ifuto_core::tls` の純 Rust 実装（差分 fuzz 検証済み）
//! を再利用する。
//!
//! # unsafe の隔離
//!
//! 本モジュールが唯一の unsafe（`extern "C"` 宣言・生ポインタの参照解除・`repr(C)`
//! union への書き込み）を持つ。`br_ssl_client_context` / `br_x509_minimal_context` は
//! 巨大な内部構造体のため、**レイアウトを再現せず**、`build.rs` が `sizeof` から生成
//! するサイズ（[`SIZEOF_BR_SSL_CLIENT`] 等）で確保した 8 バイトアライン済みバッファを
//! 不透明ポインタとして渡す。各 unsafe 操作には `// SAFETY:` コメントを付す。

#![allow(unsafe_code)]

use ifuto_core::tls::{ca_load_pem_anchors, Pkey, TrustAnchor};
use std::ffi::CString;
use std::io::{ErrorKind, Read, Write};
use std::net::TcpStream;
use std::os::raw::{c_char, c_int, c_uint, c_void};

// build.rs が生成する ABI 依存定数（sizeof + バッファサイズ）。
include!(concat!(env!("OUT_DIR"), "/bearssl_sizes.rs"));

// ---- BearSSL 定数（bearssl_ssl.h / bearssl_x509.h） ----
const BR_TLS12: c_int = 0x0303;
const BR_ERR_OK: c_int = 0;
const BR_SSL_CLOSED: c_uint = 0x0001;
const BR_SSL_SENDREC: c_uint = 0x0002;
const BR_SSL_RECVREC: c_uint = 0x0004;
const BR_SSL_SENDAPP: c_uint = 0x0008;
const BR_SSL_RECVAPP: c_uint = 0x0010;
const BR_KEYTYPE_RSA: u8 = 1;
const BR_KEYTYPE_EC: u8 = 2;
const BR_X509_TA_CA: u32 = 0x0001;

// ---- BearSSL 構造体（`br_x509_trust_anchor` 等。レイアウトは C と一致させる） ----

/// `br_x500_name`（DER エンコード DN）。
#[repr(C)]
#[derive(Clone, Copy)]
struct BrX500Name {
    data: *mut u8,
    len: usize,
}

/// `br_rsa_public_key`。
#[repr(C)]
#[derive(Clone, Copy)]
struct BrRsaPublicKey {
    n: *mut u8,
    nlen: usize,
    e: *mut u8,
    elen: usize,
}

/// `br_ec_public_key`。
#[repr(C)]
#[derive(Clone, Copy)]
struct BrEcPublicKey {
    curve: c_int,
    q: *mut u8,
    qlen: usize,
}

/// `br_x509_pkey` の key union。
#[repr(C)]
#[derive(Clone, Copy)]
union BrPkey {
    rsa: BrRsaPublicKey,
    ec: BrEcPublicKey,
}

/// `br_x509_pkey`。
#[repr(C)]
#[derive(Clone, Copy)]
struct BrX509Pkey {
    key_type: u8,
    key: BrPkey,
}

/// `br_x509_trust_anchor`。
#[repr(C)]
#[derive(Clone, Copy)]
struct BrX509TrustAnchor {
    dn: BrX500Name,
    flags: u32,
    pkey: BrX509Pkey,
}

// レイアウトのコンパイル時検証（C の sizeof と一致することを build 時に保証）。
const _: () = {
    assert!(core::mem::size_of::<BrX509TrustAnchor>() == SIZEOF_BR_X509_TRUST_ANCHOR);
    assert!(core::mem::align_of::<BrX509TrustAnchor>() == core::mem::align_of::<usize>());
};

// ---- BearSSL 公開関数（extern "C"） ----

extern "C" {
    fn br_ssl_client_init_full(
        cc: *mut c_void,
        xc: *mut c_void,
        trust_anchors: *const BrX509TrustAnchor,
        trust_anchors_num: usize,
    );
    fn br_ssl_engine_set_buffer(cc: *mut c_void, buf: *mut u8, buf_len: usize, bidi: c_int);
    fn br_ssl_client_reset(cc: *mut c_void, server_name: *const c_char, resume: c_int) -> c_int;
    fn br_ssl_engine_current_state(cc: *const c_void) -> c_uint;
    fn br_ssl_engine_sendapp_buf(cc: *const c_void, len: *mut usize) -> *mut u8;
    fn br_ssl_engine_sendapp_ack(cc: *mut c_void, len: usize);
    fn br_ssl_engine_recvapp_buf(cc: *const c_void, len: *mut usize) -> *mut u8;
    fn br_ssl_engine_recvapp_ack(cc: *mut c_void, len: usize);
    fn br_ssl_engine_sendrec_buf(cc: *const c_void, len: *mut usize) -> *mut u8;
    fn br_ssl_engine_sendrec_ack(cc: *mut c_void, len: usize);
    fn br_ssl_engine_recvrec_buf(cc: *const c_void, len: *mut usize) -> *mut u8;
    fn br_ssl_engine_recvrec_ack(cc: *mut c_void, len: usize);
    fn br_ssl_engine_flush(cc: *mut c_void, force: c_int);
    fn br_ssl_engine_close(cc: *mut c_void);

    // bearssl_shim.c が提供する static inline API の非インライン版。
    fn ifuto_br_ssl_engine_set_versions(cc: *mut c_void, min_version: c_uint, max_version: c_uint);
    fn ifuto_br_ssl_engine_last_error(cc: *const c_void) -> c_int;
}

// ---- CA ロード（C の `tls.c` `ca_load` 相当） ----

const CA_PATHS: &[&str] = &[
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/ssl/cert.pem",
    "/etc/pki/tls/certs/ca-bundle.crt",
    "/etc/ssl/ca-bundle.pem",
];

/// システム CA バンドルからトラストアンカー列をロード（C の `ca_load` 相当）。
/// `IFUTO_CA_BUNDLE` があればそれのみ、無ければ既定パスを順に試す。1 つでも読めて
/// アンカーが得られたらそれを返す。
pub fn ca_load() -> Vec<TrustAnchor> {
    let paths: Vec<String> = match std::env::var("IFUTO_CA_BUNDLE") {
        Ok(p) if !p.is_empty() => vec![p],
        _ => CA_PATHS.iter().map(|s| s.to_string()).collect(),
    };
    for p in paths {
        if p.is_empty() {
            continue;
        }
        if let Ok(bytes) = std::fs::read(&p) {
            let anchors = ca_load_pem_anchors(&bytes);
            if !anchors.is_empty() {
                return anchors;
            }
        }
    }
    Vec::new()
}

/// `run_until` の結果（C の `tls_run_until` 戻り値 0 / 1 / -1 相当）。
enum RunResult {
    /// 目標状態に到達。
    Ok,
    /// 正常クローズ（close_notify / TCP FIN）。
    Eof,
    /// エラー（分類文字列付き）。
    Err(&'static str),
}

/// 8 バイトアライン済みのゼロ初期化バッファを確保する（C の `calloc` 相当）。
fn alloc_zeroed(size: usize) -> Box<[u64]> {
    vec![0u64; size.div_ceil(8)].into_boxed_slice()
}

/// トラストアンカー列を BearSSL の `br_x509_trust_anchor` 配列へ変換する。
///
/// DN・公開鍵バイトは単一の安定ストレージ（`Box<[u8]>`）に複製し、配列要素はそれを
/// 指す（C の `g_der` 連結と同型。アンカーは接続中ポインタ参照のみ）。
fn build_anchors(tas: &[TrustAnchor]) -> (Box<[u8]>, Vec<BrX509TrustAnchor>) {
    let mut total = 0usize;
    for ta in tas {
        total += ta.dn.len();
        match &ta.pkey {
            Pkey::Rsa { n, e } => total += n.len() + e.len(),
            Pkey::Ec { q, .. } => total += q.len(),
        }
    }
    let mut storage = vec![0u8; total].into_boxed_slice();
    let base = storage.as_mut_ptr();
    let mut off = 0usize;
    let mut anchors = Vec::with_capacity(tas.len());
    for ta in tas {
        // DN
        let dn_ptr = unsafe { base.add(off) };
        storage[off..off + ta.dn.len()].copy_from_slice(&ta.dn);
        let dn_len = ta.dn.len();
        off += ta.dn.len();
        let pkey = match &ta.pkey {
            Pkey::Rsa { n, e } => {
                let n_ptr = unsafe { base.add(off) };
                storage[off..off + n.len()].copy_from_slice(n);
                off += n.len();
                let e_ptr = unsafe { base.add(off) };
                storage[off..off + e.len()].copy_from_slice(e);
                off += e.len();
                BrX509Pkey {
                    key_type: BR_KEYTYPE_RSA,
                    // union 初期化（key_type と対応して正当）。
                    key: BrPkey {
                        rsa: BrRsaPublicKey {
                            n: n_ptr,
                            nlen: n.len(),
                            e: e_ptr,
                            elen: e.len(),
                        },
                    },
                }
            }
            Pkey::Ec { curve, q } => {
                let q_ptr = unsafe { base.add(off) };
                storage[off..off + q.len()].copy_from_slice(q);
                off += q.len();
                BrX509Pkey {
                    key_type: BR_KEYTYPE_EC,
                    // union 初期化（key_type と対応して正当）。
                    key: BrPkey {
                        ec: BrEcPublicKey {
                            curve: *curve,
                            q: q_ptr,
                            qlen: q.len(),
                        },
                    },
                }
            }
        };
        anchors.push(BrX509TrustAnchor {
            dn: BrX500Name {
                data: dn_ptr,
                len: dn_len,
            },
            flags: BR_X509_TA_CA,
            pkey,
        });
    }
    (storage, anchors)
}

/// TLS クライアント接続（C の `IfTls` + `if_tls_client` 相当）。`TcpStream` を所有し、
/// ハンドシェイク・送受信・クローズを行う。
pub struct TlsClient {
    /// `br_ssl_client_context`（先頭フィールドが `br_ssl_engine_context eng` なので、
    /// エンジン関数へはこのバッファ先頭ポインタを渡す）。
    sc: Box<[u64]>,
    /// `br_x509_minimal_context`。
    xc: Box<[u64]>,
    /// 全二重 I/O バッファ（`BR_SSL_BUFSIZE_BIDI`）。
    iobuf: Box<[u64]>,
    /// アンカーの安定ストレージ（DN・鍵バイト。アンカー配列が参照）。
    _anchors: Box<[u8]>,
    /// アンカー配列（`br_ssl_client_init_full` が参照。接続中生存）。
    _anchor_vec: Vec<BrX509TrustAnchor>,
    stream: TcpStream,
}

impl TlsClient {
    /// TLS ハンドシェイク込みで接続（C の `if_tls_client` 相当）。失敗で分類文字列
    /// （`"ca"` / `"tls"` / `"cert"` / `"send"` / `"recv"`）を返す。
    pub fn connect(stream: TcpStream, host: &str, anchors: &[TrustAnchor]) -> Result<Self, &'static str> {
        if anchors.is_empty() {
            return Err("ca");
        }
        let (storage, anchor_vec) = build_anchors(anchors);
        let mut tls = TlsClient {
            sc: alloc_zeroed(SIZEOF_BR_SSL_CLIENT),
            xc: alloc_zeroed(SIZEOF_BR_X509_MINIMAL),
            iobuf: alloc_zeroed(BR_SSL_BUFSIZE_BIDI),
            _anchors: storage,
            _anchor_vec: anchor_vec,
            stream,
        };
        let host_c = CString::new(host).map_err(|_| "bad url")?;
        // SAFETY: sc/xc/iobuf は 8 バイトアライン済み・十分な長さのゼロ初期化バッファ。
        // anchor_vec は init_full 呼び出し後も TlsClient の生存中不変。
        unsafe {
            let scp = tls.sc.as_mut_ptr() as *mut c_void;
            let xcp = tls.xc.as_mut_ptr() as *mut c_void;
            let iobufp = tls.iobuf.as_mut_ptr() as *mut u8;
            br_ssl_client_init_full(
                scp,
                xcp,
                tls._anchor_vec.as_ptr(),
                tls._anchor_vec.len(),
            );
            ifuto_br_ssl_engine_set_versions(scp, BR_TLS12 as c_uint, BR_TLS12 as c_uint);
            br_ssl_engine_set_buffer(scp, iobufp, BR_SSL_BUFSIZE_BIDI, 1);
            if br_ssl_client_reset(scp, host_c.as_ptr(), 0) == 0 {
                return Err("tls"); // 初期エントロピー取得失敗等
            }
        }
        match tls.run_until(BR_SSL_SENDAPP) {
            RunResult::Ok => {}
            RunResult::Eof => return Err("tls"), // ハンドシェイク中のクローズ
            RunResult::Err(e) => return Err(e),
        }
        // 検証失敗はエンジンが CLOSED にしているはずだが、二重確認（C と同型）
        let scp = tls.sc.as_mut_ptr() as *mut c_void;
        // SAFETY: scp は TlsClient が所有する有効な br_ssl_client_context 先頭。
        if unsafe { ifuto_br_ssl_engine_last_error(scp) } != BR_ERR_OK {
            return Err("cert");
        }
        Ok(tls)
    }

    fn sc_ptr(&mut self) -> *mut c_void {
        self.sc.as_mut_ptr() as *mut c_void
    }

    /// エンジン状態を目標状態まで進める（C の `tls_run_until` 相当）。
    fn run_until(&mut self, target: c_uint) -> RunResult {
        let scp = self.sc_ptr();
        loop {
            // SAFETY: scp は TlsClient が所有する有効な br_ssl_client_context 先頭。
            let state = unsafe { br_ssl_engine_current_state(scp) };
            if state & BR_SSL_CLOSED != 0 {
                // SAFETY: scp は有効。
                let e = unsafe { ifuto_br_ssl_engine_last_error(scp) };
                if e == BR_ERR_OK {
                    return RunResult::Eof; // close_notify 正常
                }
                return RunResult::Err(if (33..=63).contains(&e) { "cert" } else { "tls" });
            }
            if state & target != 0 {
                return RunResult::Ok;
            }
            if state & BR_SSL_SENDREC != 0 {
                let mut len = 0usize;
                // SAFETY: scp は有効。len は送信すべきレコード長で初期化される。
                let buf = unsafe { br_ssl_engine_sendrec_buf(scp, &mut len) };
                if len == 0 {
                    return RunResult::Err("tls");
                }
                // SAFETY: buf は len バイトの有効な送信レコード。
                let bytes = unsafe { std::slice::from_raw_parts(buf, len) };
                if self.stream.write_all(bytes).is_err() {
                    return RunResult::Err("send");
                }
                // SAFETY: len バイト送信済み。
                unsafe { br_ssl_engine_sendrec_ack(scp, len) };
            } else if state & BR_SSL_RECVREC != 0 {
                let mut len = 0usize;
                // SAFETY: scp は有効。len は受信可能なレコード長で初期化される。
                let buf = unsafe { br_ssl_engine_recvrec_buf(scp, &mut len) };
                if len == 0 {
                    continue;
                }
                // SAFETY: buf は len バイトの受信バッファ。
                let dst = unsafe { std::slice::from_raw_parts_mut(buf, len) };
                match self.stream.read(dst) {
                    Ok(0) => {
                        // TCP FIN: close_notify なし切断（Connection: close では一般的）
                        // SAFETY: 0 バイト受信（EOF）をエンジンへ伝達。
                        unsafe { br_ssl_engine_recvrec_ack(scp, 0) };
                        return RunResult::Eof;
                    }
                    Ok(r) => {
                        // SAFETY: r バイト受信済み。
                        unsafe { br_ssl_engine_recvrec_ack(scp, r) };
                    }
                    Err(ref e) if e.kind() == ErrorKind::Interrupted => continue,
                    Err(_) => return RunResult::Err("recv"),
                }
            } else {
                // 待機すべき状態が無い（理論上到達しない）
                return RunResult::Err("tls");
            }
        }
    }

    /// 全データを送信（C の `if_tls_send_all` 相当）。
    pub fn send_all(&mut self, p: &[u8]) -> Result<(), &'static str> {
        let scp = self.sc_ptr();
        let mut n = p.len();
        let mut off = 0usize;
        while n > 0 {
            let mut len = 0usize;
            // SAFETY: scp は有効。len は送信アプリデータバッファ長で初期化される。
            let buf = unsafe { br_ssl_engine_sendapp_buf(scp, &mut len) };
            if len == 0 {
                match self.run_until(BR_SSL_SENDAPP) {
                    RunResult::Ok => continue,
                    RunResult::Eof | RunResult::Err(_) => return Err("tls"),
                }
            }
            let c = n.min(len);
            // SAFETY: buf は len バイト、p[off..] は n バイト有効。c <= min(n, len)。
            unsafe { std::ptr::copy_nonoverlapping(p.as_ptr().add(off), buf, c) };
            // SAFETY: c バイトを送信アプリバッファへ書いた。
            unsafe { br_ssl_engine_sendapp_ack(scp, c) };
            off += c;
            n -= c;
        }
        // フラッシュ: レコード化を強制（ack だけでは SENDREC が立たない）。
        // SAFETY: scp は有効。
        unsafe { br_ssl_engine_flush(scp, 1) };
        loop {
            // SAFETY: scp は有効。
            let state = unsafe { br_ssl_engine_current_state(scp) };
            if state & BR_SSL_CLOSED != 0 {
                // SAFETY: scp は有効。
                let e = unsafe { ifuto_br_ssl_engine_last_error(scp) };
                if e != BR_ERR_OK {
                    return Err("tls");
                }
                return Ok(()); // close_notify 受信 = 相手は閉じる。送信完了扱い
            }
            if state & BR_SSL_SENDREC != 0 {
                let mut len = 0usize;
                // SAFETY: scp は有効。
                let buf = unsafe { br_ssl_engine_sendrec_buf(scp, &mut len) };
                if len == 0 {
                    return Ok(());
                }
                // SAFETY: buf は len バイト有効。
                let bytes = unsafe { std::slice::from_raw_parts(buf, len) };
                if self.stream.write_all(bytes).is_err() {
                    return Err("send");
                }
                // SAFETY: len バイト送信済み。
                unsafe { br_ssl_engine_sendrec_ack(scp, len) };
                continue;
            }
            break; // 送信すべきデータなし = フラッシュ完了
        }
        Ok(())
    }

    /// 受信（C の `if_tls_recv` 相当）。0 = EOF、`Err` = エラー、`Ok(n)` = n バイト。
    pub fn recv(&mut self, out: &mut [u8]) -> Result<usize, &'static str> {
        let scp = self.sc_ptr();
        loop {
            match self.run_until(BR_SSL_RECVAPP) {
                RunResult::Ok => {}
                RunResult::Eof => return Ok(0),
                RunResult::Err(e) => return Err(e),
            }
            let mut len = 0usize;
            // SAFETY: scp は有効。len は受信アプリデータ長で初期化される。
            let buf = unsafe { br_ssl_engine_recvapp_buf(scp, &mut len) };
            if len == 0 {
                continue; // レコード境界（再ループ）
            }
            let c = out.len().min(len);
            // SAFETY: buf は len バイト、out は out.len() バイト有効。c <= min。
            unsafe { std::ptr::copy_nonoverlapping(buf, out.as_mut_ptr(), c) };
            // SAFETY: c バイト消費済み。
            unsafe { br_ssl_engine_recvapp_ack(scp, c) };
            return Ok(c);
        }
    }

    /// クローズ（C の `if_tls_close` 相当。close_notify 送信はベストエフォート）。
    pub fn close(&mut self) {
        // SAFETY: scp は有効。
        unsafe { br_ssl_engine_close(self.sc_ptr()) };
        let _ = self.run_until(BR_SSL_CLOSED);
    }
}
