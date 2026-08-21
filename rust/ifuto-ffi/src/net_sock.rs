//! `net.c` のソケット層（`connect_one` / `send_all` / `fetch_once` / `if_http_get_ex`）の
//! `std::net` 移植。http（平文）と https（TLS 1.2、BearSSL）の両経路をカバーする。
//!
//! | C (net.c) | Rust |
//! |---|---|
//! | `connect_one` | [`connect_one`] |
//! | `send_all` | [`send_all`] |
//! | `fetch_once` | [`fetch_once`] |
//! | `if_http_get` / `if_http_get_ex` | [`http_get`] / [`http_get_ex`] |
//!
//! URL 分解・解決・ヘッダ解析・chunked 復号・private 判定は `ifuto_core::net` の純粋関数を
//! 再利用する。https は [`crate::bearssl::TlsClient`]（BearSSL、TLS 1.2）でハンドシェイク
//! し、`std::net::TcpStream` で I/O する。C の arena + raw fd を、所有 `TcpStream` +
//! `Vec<u8>` に置換する。

use crate::bearssl::{ca_load, TlsClient};
use ifuto_core::net::{
    addr_is_private, dechunk, head_parse, parse_url, resolve_url, HttpHead, HttpUrl,
    HTTP_MAX_BYTES,
};
use ifuto_core::tls::TrustAnchor;
use std::io::{ErrorKind, Read, Write};
use std::net::{Ipv4Addr, SocketAddr, TcpStream, ToSocketAddrs};
use std::sync::OnceLock;
use std::time::Duration;

/// C の `IF_HTTP_MAX_REDIRECTS`。
const MAX_REDIRECTS: u32 = 5;

// C の err 分類文字列（net.c の E_* 定数と同一）。
const E_URL: &str = "bad url";
const E_DNS: &str = "dns";
const E_CONN: &str = "connect";
const E_SEND: &str = "send";
const E_RECV: &str = "recv";
const E_BIG: &str = "too large";
const E_RESP: &str = "bad response";
const E_TRUNC: &str = "truncated";
const E_LOOP: &str = "redirect loop";
const E_PRIV: &str = "private redirect blocked";
const E_DOWNGRADE: &str = "https downgrade blocked";

/// 接続失敗の種別（C の `connect_one` 戻り値 -2 / -1 相当）。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConnectError {
    /// DNS 解決失敗（getaddrinfo 相当が失敗）。
    Dns,
    /// 接続失敗（全候補アドレスが到達不能 or private スキップ）。
    Connect,
}

/// IPv4 アドレスの private/loopback/link-local/CGNAT 判定。C の `if_addr_is_private`
/// の `ntohl` 後の値（第 1 オクテットが MSB）に `Ipv4Addr` を写像して再利用。
fn ip4_is_private(ip: Ipv4Addr) -> bool {
    addr_is_private(u32::from(ip))
}

/// C の `connect_one` 相当。IPv4 のみ・connect 8s 上限・送受信 SO_*TIMEO 10s。
///
/// `allow_private` が false のとき private アドレスは試さない（DNS rebinding / SSRF
/// 対策）。成功で `(stream, private)`（接続先が private か）。
pub fn connect_one(
    host: &str,
    port: u16,
    allow_private: bool,
) -> Result<(TcpStream, bool), ConnectError> {
    let addrs: Vec<SocketAddr> = match (host, port).to_socket_addrs() {
        Ok(it) => it.filter(|a| a.is_ipv4()).collect(),
        Err(_) => return Err(ConnectError::Dns),
    };
    if addrs.is_empty() {
        // C は hints.ai_family = AF_INET で getaddrinfo するため、AAAA しか無い
        // ホストは解決失敗（-2）。Rust は全ファミリを引いてから v4 で絞るので、
        // v4 が 0 件なら DNS 失敗とみなす。
        return Err(ConnectError::Dns);
    }
    for addr in addrs {
        let private = match addr.ip() {
            std::net::IpAddr::V4(v4) => ip4_is_private(v4),
            _ => continue,
        };
        if !allow_private && private {
            continue;
        }
        match TcpStream::connect_timeout(&addr, Duration::from_millis(8000)) {
            Ok(s) => {
                let _ = s.set_read_timeout(Some(Duration::from_secs(10)));
                let _ = s.set_write_timeout(Some(Duration::from_secs(10)));
                return Ok((s, private));
            }
            Err(_) => continue,
        }
    }
    Err(ConnectError::Connect)
}

/// C の `send_all` 相当（EINTR 再試行・失敗で `false`）。
pub fn send_all(s: &mut TcpStream, p: &[u8]) -> bool {
    s.write_all(p).is_ok()
}

/// `fetch_once` の結果（C の出力引数群を所有型に正規化）。
#[derive(Debug, Clone)]
pub struct FetchResult {
    /// デコード済みボディ。
    pub body: Vec<u8>,
    /// 状態コード。
    pub status: u32,
    /// Location 値（無ければ `None`）。
    pub location: Option<Vec<u8>>,
    /// Content-Type 値（無ければ `None`）。
    pub content_type: Option<Vec<u8>>,
    /// 接続先が private だったか（リダイレクトチェーン追跡用）。
    pub conn_private: bool,
}

/// プロセスで 1 回だけ CA をロード（C の `tls.c` のプロセス静的 `g_ta` と同型）。
/// ロード失敗（どのパスも読めない / アンカー 0 件）は空列。
fn trust_anchors() -> &'static [TrustAnchor] {
    static ANCHORS: OnceLock<Vec<TrustAnchor>> = OnceLock::new();
    ANCHORS.get_or_init(ca_load).as_slice()
}

/// TLS 送信路（`send_all`）の抽象（平文 / TLS）。
enum Tx {
    Plain(TcpStream),
    Tls(TlsClient),
}

/// C の `fetch_once` 相当（http + https）。リダイレクトは追わない。
pub fn fetch_once(u: &HttpUrl, allow_private: bool) -> Result<FetchResult, &'static str> {
    let (stream, conn_private) = match connect_one(&u.host, u.port, allow_private) {
        Ok(x) => x,
        Err(ConnectError::Dns) => return Err(E_DNS),
        Err(ConnectError::Connect) => return Err(if allow_private { E_CONN } else { E_PRIV }),
    };

    let mut tx = if u.tls {
        // https: TLS 1.2 ハンドシェイク（CA 検証 + サーバ名照合は BearSSL が実施）。
        // 失敗分類（"tls" / "cert" / "ca" / "send" / "recv"）はそのまま返す。
        Tx::Tls(TlsClient::connect(stream, &u.host, trust_anchors())?)
    } else {
        Tx::Plain(stream)
    };

    // リクエスト構築（Host は明示 :port のときだけ付ける）
    let host_header = if u.has_port {
        format!("{}:{}", u.host, u.port)
    } else {
        u.host.clone()
    };
    let req = format!(
        "GET {} HTTP/1.1\r\nHost: {}\r\nUser-Agent: Ifuto/0.3\r\nAccept: */*\r\nConnection: close\r\n\r\n",
        u.path, host_header
    );
    match &mut tx {
        Tx::Plain(s) => {
            if !send_all(s, req.as_bytes()) {
                return Err(E_SEND);
            }
        }
        Tx::Tls(t) => {
            if t.send_all(req.as_bytes()).is_err() {
                return Err(E_SEND);
            }
        }
    }

    // EOF まで読む（Content-Length が確定したらその分だけ読んで早期完了）
    let mut buf: Vec<u8> = Vec::new();
    let mut body_need: Option<u64> = None;
    let mut tmp = [0u8; 16384];
    loop {
        if buf.len() as u64 >= HTTP_MAX_BYTES {
            return Err(E_BIG);
        }
        let r = match &mut tx {
            Tx::Plain(s) => match s.read(&mut tmp) {
                Ok(0) => break, // EOF
                Ok(r) => r,
                Err(ref e) if e.kind() == ErrorKind::Interrupted => continue,
                Err(_) => return Err(E_RECV),
            },
            Tx::Tls(t) => match t.recv(&mut tmp) {
                Ok(0) => break, // close_notify / FIN = EOF
                Ok(r) => r,
                Err(_) => return Err(E_RECV),
            },
        };
        buf.extend_from_slice(&tmp[..r]);
        if body_need.is_none() {
            if let Some(h) = head_parse(&buf) {
                if let Some(cl) = h.content_length {
                    body_need = Some(h.body_off + cl);
                }
            }
        }
        if let Some(bn) = body_need {
            if buf.len() as u64 >= bn {
                break;
            }
        }
    }
    if let Tx::Tls(t) = &mut tx {
        t.close();
    }

    let h: HttpHead = head_parse(&buf).ok_or(E_RESP)?;
    let body_off = h.body_off as usize;
    let bp = &buf[body_off..];

    let body = if h.chunked {
        dechunk(bp).ok_or(E_RESP)?
    } else if let Some(cl) = h.content_length {
        if cl > HTTP_MAX_BYTES {
            return Err(E_BIG);
        }
        if (bp.len() as u64) < cl {
            return Err(E_TRUNC);
        }
        bp[..cl as usize].to_vec()
    } else {
        bp.to_vec() // close までがボディ
    };

    Ok(FetchResult {
        body,
        status: h.status,
        location: h.location.clone(),
        content_type: h.content_type.clone(),
        conn_private,
    })
}

/// `if_http_get_ex` の戻り値（ボディ, 最終状態コード, Content-Type）。
pub type GetResult = Result<(Vec<u8>, u32, Option<Vec<u8>>), &'static str>;

/// C の `if_http_get_ex` 相当（リダイレクト追跡 + https→http 降格防止）。
///
/// 戻り値は `(ボディ, 最終状態コード, Content-Type)`。リダイレクトは 5 回まで・
/// private 拒否・降格拒否を C と同一に。
pub fn http_get_ex(url: &str) -> GetResult {
    if url.len() >= 1024 {
        return Err(E_URL);
    }
    let mut cur = url.to_string();
    let chain_tls = cur.starts_with("https://");
    let mut chain_private = false;
    for depth in 0u32.. {
        let u = parse_url(&cur).ok_or(E_URL)?;
        let allow_priv = depth == 0 || chain_private;
        let fr = fetch_once(&u, allow_priv)?;
        if depth == 0 && fr.conn_private {
            chain_private = true;
        }
        let redir = matches!(fr.status, 301 | 302 | 303 | 307 | 308) && fr.location.is_some();
        if !redir {
            return Ok((fr.body, fr.status, fr.content_type));
        }
        if depth >= MAX_REDIRECTS {
            return Err(E_LOOP);
        }
        let loc = fr.location.unwrap();
        if loc.len() >= 1024 {
            return Ok((fr.body, fr.status, fr.content_type)); // 異常に長い Location は追わない
        }
        let loc_str = String::from_utf8_lossy(&loc).into_owned();
        let mut nxt = String::new();
        if !resolve_url(&cur, &loc_str, &mut nxt) {
            return Ok((fr.body, fr.status, fr.content_type)); // 解決不能は最後の応答
        }
        if chain_tls && nxt.starts_with("http://") && !nxt.starts_with("https://") {
            return Err(E_DOWNGRADE);
        }
        cur = nxt;
    }
    unreachable!()
}

/// C の `if_http_get` 相当（Content-Type なし）。
pub fn http_get(url: &str) -> Result<(Vec<u8>, u32), &'static str> {
    http_get_ex(url).map(|(b, s, _)| (b, s))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn private_detection() {
        assert!(ip4_is_private(Ipv4Addr::new(127, 0, 0, 1)));
        assert!(ip4_is_private(Ipv4Addr::new(10, 1, 2, 3)));
        assert!(ip4_is_private(Ipv4Addr::new(192, 168, 0, 1)));
        assert!(ip4_is_private(Ipv4Addr::new(172, 16, 0, 1)));
        assert!(ip4_is_private(Ipv4Addr::new(172, 31, 255, 255)));
        assert!(ip4_is_private(Ipv4Addr::new(169, 254, 1, 1)));
        assert!(ip4_is_private(Ipv4Addr::new(100, 64, 0, 1)));
        assert!(!ip4_is_private(Ipv4Addr::new(8, 8, 8, 8)));
        assert!(!ip4_is_private(Ipv4Addr::new(172, 32, 0, 1)));
        assert!(!ip4_is_private(Ipv4Addr::new(11, 0, 0, 1)));
    }

    #[test]
    fn url_too_long() {
        let long = format!("http://x/{}", "a".repeat(2000));
        assert_eq!(http_get(&long), Err(E_URL));
    }

    #[test]
    fn bad_url() {
        assert_eq!(http_get("not a url"), Err(E_URL));
        assert_eq!(http_get("ftp://x/"), Err(E_URL));
    }
}
