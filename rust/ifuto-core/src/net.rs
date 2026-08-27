//! HTTP/1.1 取得の純粋関数（C の `src/net.c` の非ソケット部分相当）。
//!
//! | C (net.h / net.c) | Rust |
//! |---|---|
//! | `if_http_parse_url` | [`parse_url`] |
//! | `if_http_resolve_url` | [`resolve_url`] |
//! | `if_http_head_parse` | [`head_parse`] |
//! | `if_http_dechunk` | [`dechunk`] |
//! | `if_addr_is_private` | [`addr_is_private`] |
//!
//! # 実装済み
//!
//! URL 分解（http/https、fragment 除去、`:port` 検査、userinfo/IPv6 拒否）、URL 解決
//! （RFC3986 の最小形、リダイレクト用）、応答ヘッダ解析（状態行 + Content-Length /
//! Transfer-Encoding / Location / Content-Type）、chunked ボディ復号（chunk-ext・
//! trailer 消費）、private/loopback/link-local/CGNAT アドレス判定。
//!
//! # 未移植（ソケット I/O・最終統合）
//!
//! - `connect_one` / `send_all` / `fetch_once` / `if_http_get(_ex)`: ソケット + BearSSL
//!   TLS を使うネットワーク I/O。非決定的で純粋関数化できず、最終統合（chrome 移植時）
//!   に Rust の `std::net` + TLS で再実装する。
//! - `tls.c`（BearSSL ラッパ）: 同上。

/// 応答全体の上限（C の `IF_HTTP_MAX_BYTES`）。
pub const HTTP_MAX_BYTES: u64 = 32 * 1024 * 1024;

/// 解析済み URL（C の `IfHttpUrl` 相当）。
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HttpUrl {
    /// ホスト（ポート除く）。
    pub host: String,
    /// ポート（省略時 80 / https 443）。
    pub port: u16,
    /// URL に明示 `:port` があったか。
    pub has_port: bool,
    /// `https://` か。
    pub tls: bool,
    /// 常に `/` 始まり（`?query` 含む、`#fragment` は除去済み）。
    pub path: String,
}

/// http(s)://host[:port]/path[?query] を分解。C の `if_http_parse_url` 相当。
pub fn parse_url(url: &str) -> Option<HttpUrl> {
    let (tls, p) = match url {
        u if u.starts_with("http://") => (false, &u[7..]),
        u if u.starts_with("https://") => (true, &u[8..]),
        _ => return None,
    };

    // fragment はここで切る
    let rem = match p.find('#') {
        Some(i) => &p[..i],
        None => p,
    };
    if rem.is_empty() {
        return None;
    }

    // host 部: '/', '?' の手前まで
    let hl = rem.find(['/', '?']).unwrap_or(rem.len());
    let hostpart = &rem[..hl];

    // host[:port] 分離（':' は最初のもののみ意味を持つ）
    let colon = hostpart.find(':');
    let hostlen = colon.unwrap_or(hostpart.len());
    if hostlen == 0 || hostlen >= 256 {
        return None;
    }
    let host = &hostpart[..hostlen];
    for &c in host.as_bytes() {
        if c <= 0x20 || c == b'/' || c == b'?' || c == b'#' || c == b'@' || c == b':' {
            return None;
        }
    }

    let mut port = if tls { 443 } else { 80 };
    let mut has_port = false;
    if let Some(c) = colon {
        let d0 = c + 1;
        let nd = hl - d0;
        if nd == 0 || nd > 5 {
            return None;
        }
        let mut v = 0u32;
        for &ch in &hostpart.as_bytes()[d0..] {
            if !ch.is_ascii_digit() {
                return None;
            }
            v = v * 10 + (ch - b'0') as u32;
        }
        if v == 0 || v > 65535 {
            return None;
        }
        port = v as u16;
        has_port = true;
    }

    // path
    let q = &rem[hl..];
    let path = if q.is_empty() {
        "/".to_string()
    } else if q.starts_with('?') {
        if 1 + q.len() >= 768 {
            return None;
        }
        let mut s = String::with_capacity(q.len() + 1);
        s.push('/');
        s.push_str(q);
        s
    } else if q.starts_with('/') {
        if q.len() >= 768 {
            return None;
        }
        q.to_string()
    } else {
        return None;
    };

    Some(HttpUrl {
        host: host.to_string(),
        port,
        has_port,
        tls,
        path,
    })
}

/// base の scheme+authority 長。C の `base_origin_len` 相当。
fn base_origin_len(base: &str) -> usize {
    base[7..]
        .find(['/', '?', '#'])
        .map_or(base.len(), |i| i + 7)
}

/// redirect 用の最小 RFC3986 解決。C の `if_http_resolve_url` 相当。
pub fn resolve_url(base: &str, loc: &str, out: &mut String) -> bool {
    let loc = loc.trim_matches([' ', '\t', '\r', '\n']);
    if loc.is_empty() {
        return false;
    }
    let tls = base.starts_with("https://");
    let sch = if tls { "https://" } else { "http://" };
    if !base.starts_with(sch) {
        return false;
    }

    let resolved: String;
    if loc.starts_with(sch) {
        resolved = loc.to_string();
    } else if loc.starts_with(if tls { "http://" } else { "https://" }) {
        // scheme 変更（http<->https）は追わない
        return false;
    } else if let Some(scheme_relative) = loc.strip_prefix("//") {
        resolved = format!("{sch}{scheme_relative}");
    } else if loc.starts_with('/') {
        let o = base_origin_len(base);
        resolved = format!("{}{}", &base[..o], loc);
    } else if loc.starts_with('?') {
        let o = base_origin_len(base);
        let i = base[o..].find(['?', '#']).map_or(base.len(), |x| x + o);
        resolved = if i == o {
            format!("{}/{}", &base[..o], loc)
        } else {
            format!("{}{}", &base[..i], loc)
        };
    } else {
        // 先頭セグメント内の ':' = scheme 付き absolute-URI
        for &c in loc.as_bytes() {
            if c == b'/' || c == b'?' {
                break;
            }
            if c == b':' {
                return false;
            }
        }
        let o = base_origin_len(base);
        let i = base[o..].find(['?', '#']).map_or(base.len(), |x| x + o);
        resolved = if i == o {
            format!("{}/{}", &base[..o], loc)
        } else {
            let mut sl = i;
            while sl > o && base.as_bytes()[sl - 1] != b'/' {
                sl -= 1;
            }
            format!("{}{}", &base[..sl], loc)
        };
    }

    if resolved.len() >= 1024 {
        return false;
    }
    if tls {
        if !resolved.starts_with("https://") || resolved.len() <= 8 {
            return false;
        }
    } else if !resolved.starts_with("http://") || resolved.len() <= 7 {
        return false;
    }
    out.clear();
    out.push_str(&resolved);
    true
}

// ================= 応答ヘッダ解析 =================

/// 応答ヘッダ解析結果（C の `IfHttpHead` 相当）。
#[derive(Clone, Debug, PartialEq, Eq, Default)]
pub struct HttpHead {
    /// 状態コード。
    pub status: u32,
    /// バッファ内の body 開始オフセット。
    pub body_off: u64,
    /// Content-Length（`None` = 未指定）。
    pub content_length: Option<u64>,
    /// Transfer-Encoding: chunked。
    pub chunked: bool,
    /// Location 値。
    pub location: Option<Vec<u8>>,
    /// Content-Type 値。
    pub content_type: Option<Vec<u8>>,
}

fn ci_eq(p: &[u8], name: &[u8]) -> bool {
    p.len() == name.len() && p.eq_ignore_ascii_case(name)
}

fn ci_mem(p: &[u8], pat: &[u8]) -> bool {
    if p.len() < pat.len() {
        return false;
    }
    (0..=p.len() - pat.len()).any(|i| ci_eq(&p[i..i + pat.len()], pat))
}

/// `\r\n\r\n`（宽容 `\n\n`）までをヘッダとして解析。C の `if_http_head_parse` 相当。
pub fn head_parse(buf: &[u8]) -> Option<HttpHead> {
    if buf.len() < 14 {
        return None;
    }
    let mut out = HttpHead {
        content_length: None,
        ..Default::default()
    };
    // 状態行: "HTTP/1." digit SP 3digit
    if &buf[..7] != b"HTTP/1." {
        return None;
    }
    let mut i = 7usize;
    if !buf[i].is_ascii_digit() {
        return None;
    }
    i += 1;
    while i < buf.len() && buf[i] != b' ' && buf[i] != b'\r' && buf[i] != b'\n' {
        i += 1;
    }
    if i >= buf.len() || buf[i] != b' ' {
        return None;
    }
    i += 1;
    if i + 3 > buf.len() {
        return None;
    }
    let mut st = 0u32;
    for k in 0..3 {
        let c = buf[i + k];
        if !c.is_ascii_digit() {
            return None;
        }
        st = st * 10 + (c - b'0') as u32;
    }
    out.status = st;

    // 状態行の残りを捨てる
    let cur = buf[i..]
        .iter()
        .position(|&c| c == b'\n')
        .map(|p| i + p + 1)?;
    let mut cur = cur;
    while cur < buf.len() {
        let lf = match buf[cur..].iter().position(|&c| c == b'\n') {
            Some(p) => cur + p,
            None => buf.len(),
        };
        let le = lf;
        // 空行 = ヘッダ終端
        if le == cur || (le == cur + 1 && buf[cur] == b'\r') {
            out.body_off = if lf < buf.len() {
                (lf + 1) as u64
            } else {
                buf.len() as u64
            };
            return Some(out);
        }
        if le == buf.len() {
            return None; // 空行無しに尽きた
        }
        // コロン探索
        if let Some(colon_rel) = buf[cur..le].iter().position(|&c| c == b':') {
            let colon = cur + colon_rel;
            if colon > cur {
                let mut v = colon + 1;
                while v < le && (buf[v] == b' ' || buf[v] == b'\t') {
                    v += 1;
                }
                let mut ve = le;
                while ve > v
                    && (buf[ve - 1] == b'\r' || buf[ve - 1] == b' ' || buf[ve - 1] == b'\t')
                {
                    ve -= 1;
                }
                let name = &buf[cur..colon];
                match name.len() {
                    14 if ci_eq(name, b"content-length") => {
                        if out.content_length.is_none() {
                            // 先勝ち
                            let d = &buf[v..ve];
                            if d.is_empty() {
                                return None;
                            }
                            let mut cl = 0u64;
                            for &ch in d {
                                if !ch.is_ascii_digit() {
                                    return None;
                                }
                                if cl > (u64::MAX - 9) / 10 {
                                    return None;
                                }
                                cl = cl * 10 + (ch - b'0') as u64;
                            }
                            out.content_length = Some(cl);
                        }
                    }
                    17 if ci_eq(name, b"transfer-encoding") => {
                        if ci_mem(&buf[v..ve], b"chunked") {
                            out.chunked = true;
                        }
                    }
                    8 if ci_eq(name, b"location") => {
                        out.location = Some(buf[v..ve].to_vec());
                    }
                    12 if ci_eq(name, b"content-type") => {
                        out.content_type = Some(buf[v..ve].to_vec());
                    }
                    _ => {}
                }
            }
        }
        cur = lf + 1;
    }
    None // 空行に到達せず
}

// ================= chunked 復号 =================

/// chunked ボディのデコード（純粋関数）。C の `if_http_dechunk` 相当。
pub fn dechunk(p: &[u8]) -> Option<Vec<u8>> {
    let n = p.len();
    let mut len = 0usize;
    let mut i = 0usize;
    let mut buf: Vec<u8> = Vec::new();
    loop {
        // サイズ行: hex[;ext] まで
        let mut size = 0u64;
        let mut ndig = 0u32;
        while i < n && p[i].is_ascii_hexdigit() {
            ndig += 1;
            if ndig > 8 {
                return None;
            }
            let c = p[i];
            let v = match c {
                b'0'..=b'9' => (c - b'0') as u64,
                b'a'..=b'f' => (c - b'a' + 10) as u64,
                _ => (c - b'A' + 10) as u64,
            };
            size = size * 16 + v;
            i += 1;
        }
        if ndig == 0 {
            return None;
        }
        // chunk-ext を捨てて行末へ
        while i < n && p[i] != b'\n' {
            i += 1;
        }
        if i >= n {
            return None;
        }
        i += 1; // \n
        if size == 0 {
            // trailer section を捨てる: 空行まで
            loop {
                if i >= n {
                    break;
                }
                let ls = i;
                while i < n && p[i] != b'\n' {
                    i += 1;
                }
                let empty = (i == ls) || (i == ls + 1 && p[ls] == b'\r');
                if i < n {
                    i += 1;
                }
                if empty {
                    break;
                }
            }
            return Some(buf);
        }
        if size > HTTP_MAX_BYTES || len as u64 + size > HTTP_MAX_BYTES {
            return None;
        }
        if i as u64 + size > n as u64 {
            return None;
        }
        buf.extend_from_slice(&p[i..i + size as usize]);
        len += size as usize;
        i += size as usize;
        if i >= n {
            return None;
        }
        if p[i] == b'\r' {
            i += 1;
        }
        if i >= n || p[i] != b'\n' {
            return None;
        }
        i += 1;
    }
}

/// アドレスが private/loopback/link-local/CGNAT か。C の `if_addr_is_private` 相当。
/// 引数はホストバイトオーダーの IPv4 アドレス（`u32`）。
pub fn addr_is_private(a: u32) -> bool {
    if (a >> 24) == 127 {
        return true; // 127.0.0.0/8
    }
    if (a >> 24) == 10 {
        return true; // 10.0.0.0/8
    }
    if (a >> 24) == 169 && (a >> 16) == 0xA9FE {
        return true; // 169.254.0.0/16
    }
    if (a >> 24) == 172 {
        let o2 = (a >> 16) & 0xFF;
        if (16..=31).contains(&o2) {
            return true; // 172.16.0.0/12
        }
    }
    if (a >> 24) == 192 && (a >> 16) == 0xC0A8 {
        return true; // 192.168.0.0/16
    }
    if (a >> 24) == 100 {
        let o2 = (a >> 16) & 0xFF;
        if (64..=127).contains(&o2) {
            return true; // 100.64.0.0/10 CGNAT
        }
    }
    if a == 0 {
        return true; // 0.0.0.0/8
    }
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_url() {
        let u = parse_url("http://example.com/").unwrap();
        assert_eq!(u.host, "example.com");
        assert_eq!(u.port, 80);
        assert!(!u.has_port);
        assert_eq!(u.path, "/");
        assert!(!u.tls);

        let u = parse_url("http://example.com").unwrap();
        assert_eq!(u.path, "/");

        let u = parse_url("http://127.0.0.1:8080/a/b?x=1#frag").unwrap();
        assert_eq!(u.host, "127.0.0.1");
        assert_eq!(u.port, 8080);
        assert!(u.has_port);
        assert_eq!(u.path, "/a/b?x=1");

        let u = parse_url("http://example.com?q=1").unwrap();
        assert_eq!(u.path, "/?q=1");

        let u = parse_url("https://example.com/").unwrap();
        assert!(u.tls);
        assert_eq!(u.port, 443);

        let u = parse_url("https://example.com:8443/a").unwrap();
        assert!(u.tls && u.port == 8443 && u.has_port);

        assert!(parse_url("ftp://example.com/").is_none());
        assert!(parse_url("http://user@example.com/").is_none());
        assert!(parse_url("http://[::1]:8080/").is_none());
        assert!(parse_url("http://example.com:0/").is_none());
        assert!(parse_url("http://example.com:65536/").is_none());
        assert!(parse_url("http://example.com:12a4/").is_none());
        assert!(parse_url("http://").is_none());
        assert!(parse_url("http:///path").is_none());
        assert!(parse_url("").is_none());
    }

    #[test]
    fn parse_url_long() {
        // host 長溢れ（256 超）
        let mut u = "http://".to_string();
        u.push_str(&"a".repeat(300));
        u.push('/');
        assert!(parse_url(&u).is_none());
    }

    #[test]
    fn test_resolve_url() {
        let mut out = String::new();
        assert!(resolve_url("http://h/dir/page", "http://x/y", &mut out));
        assert_eq!(out, "http://x/y");
        assert!(resolve_url("http://h/dir/page", "//x/y", &mut out));
        assert_eq!(out, "http://x/y");
        assert!(resolve_url("http://h:8080/dir/page", "/abs", &mut out));
        assert_eq!(out, "http://h:8080/abs");
        assert!(resolve_url("http://h/dir/page", "rel", &mut out));
        assert_eq!(out, "http://h/dir/rel");
        assert!(resolve_url("http://h", "rel2", &mut out));
        assert_eq!(out, "http://h/rel2");
        assert!(resolve_url("http://h/", "rel3", &mut out));
        assert_eq!(out, "http://h/rel3");
        assert!(resolve_url("http://h/dir/page?q=1#f", "next", &mut out));
        assert_eq!(out, "http://h/dir/next");
        assert!(resolve_url("http://h/dir/page", "?q=2", &mut out));
        assert_eq!(out, "http://h/dir/page?q=2");
        assert!(resolve_url("http://h/dir/", "next2", &mut out));
        assert_eq!(out, "http://h/dir/next2");
        assert!(resolve_url("http://h/d", "  /sp  ", &mut out));
        assert_eq!(out, "http://h/sp");
        assert!(!resolve_url("http://h/d", "https://x/", &mut out));
        assert!(!resolve_url("http://h/d", "javascript:void(0)", &mut out));
        assert!(!resolve_url("http://h/d", "", &mut out));
    }

    #[test]
    fn test_head_parse() {
        let h = head_parse(b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello").unwrap();
        assert_eq!(h.status, 200);
        assert_eq!(h.content_length, Some(5));
        assert!(!h.chunked);
        assert_eq!(h.body_off, 38);

        let h = head_parse(b"HTTP/1.0 404 Not Found\r\ncONTENT-lENGTH:   3 \r\n\r\nabc").unwrap();
        assert_eq!(h.status, 404);
        assert_eq!(h.content_length, Some(3));

        let h = head_parse(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, Chunked\r\n\r\n0\r\n\r\n")
            .unwrap();
        assert!(h.chunked);
        assert_eq!(h.content_length, None);

        let h = head_parse(b"HTTP/1.1 301 Moved\r\nLocation: /new\r\nContent-Length: 0\r\n\r\n")
            .unwrap();
        assert_eq!(h.status, 301);
        assert_eq!(h.location.as_deref(), Some(&b"/new"[..]));

        let h = head_parse(b"HTTP/1.1 204 X\nServer: s\n\n").unwrap();
        assert_eq!(h.status, 204);

        let h = head_parse(b"HTTP/1.1 200 OK\r\nContent-Length-X: 9\r\n\r\nb").unwrap();
        assert_eq!(h.content_length, None);

        // 複数 CL は先勝ち
        let h = head_parse(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 9\r\n\r\nab")
            .unwrap();
        assert_eq!(h.content_length, Some(2));

        assert!(head_parse(b"garbage\r\n\r\n").is_none());
        assert!(head_parse(b"HTTP/1.1 200 OK\r\nServer: s\r\n").is_none());
        assert!(head_parse(b"HTTP/1.1 ABC X\r\n\r\n").is_none());
    }

    #[test]
    fn test_dechunk() {
        let o = dechunk(b"4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n").unwrap();
        assert_eq!(o, b"Wikipedia");

        let o = dechunk(b"4;x=y\r\nWiki\r\n0\r\nX-Trailer: v\r\n\r\n").unwrap();
        assert_eq!(o, b"Wiki");

        let o = dechunk(b"4\nWiki\n5\npedia\n0\n\n").unwrap();
        assert_eq!(o.len(), 9);

        let o = dechunk(b"A\r\n0123456789\r\n0\r\n").unwrap();
        assert_eq!(o.len(), 10);
        assert_eq!(o[9], b'9');

        let o = dechunk(b"0\r\n\r\n").unwrap();
        assert!(o.is_empty());

        assert!(dechunk(b"5\r\nWik\r\n").is_none());
        assert!(dechunk(b"Z\r\nxx\r\n").is_none());
        assert!(dechunk(b"4\r\nWiki").is_none());
        assert!(dechunk(b"1\r\nA").is_none());
    }

    #[test]
    fn test_private_addr() {
        for a in [
            0x7F000001u32,
            0x7F0000FF,
            0x0A000001,
            0x0AFFFFFF,
            0xAC100001,
            0xAC1F0001,
            0xC0A80001,
            0xC0A8FFFE,
            0xA9FE0001,
            0x64400001,
            0x647FFFFE,
            0x00000000,
            0x0A0A0A0A,
            0xAC100000,
        ] {
            assert!(addr_is_private(a), "{a:#x}");
        }
        for a in [0x08080808u32, 0xC0000201, 0xAC0F0001, 0xAC200001] {
            assert!(!addr_is_private(a), "{a:#x}");
        }
    }
}
