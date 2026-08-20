//! 拡張 manifest パーサ（C の `src/ext_manifest.c` 相当。E1、docs/EXTENSIONS.md §2 が唯一の正）。
//!
//! `manifest.txt`: 行ベース `"key: value"`（JSON パーサ不在 = 新 fuzz 面を背負わない）。
//! 本モジュールは純粋関数のみ（FS/グローバル不触）。
//!
//! # 文法（E1 凍結）
//!
//! - 行: 空行 / `#` 始まりコメントはスキップ。それ以外は `key ':' value`
//! - トリム: 前後の `' ' '\t'`。行末の単一 `'\r'` は除去（CRLF 救済）
//! - key: `name` | `version` | `entry` | `permissions`（完全一致・重複は失敗）
//! - name: `[A-Za-z0-9_.-]{1,63}` / version: 同 charset {1,23} / entry: basename のみ
//! - permissions: `,` 区切り・各 token は `{"status","log"}`。E1 は単一効果規則
//!   （2 つ以上の宣言は失敗）
//! - 必須: name / version / entry（permissions は省略可）
//! - サイズ: src ≤ 64KB
//!
//! # C との違い（所有権による構造的な改善）
//!
//! C は `char name[64]` 固定配列 + `char *err` の書き込み先を呼び出し側が用意する。
//! Rust では [`Manifest`] の `String` フィールドと `Result<_, String>` で表現し、
//! バッファ長の手動管理・NUL 終端・err バッファの境界検査が構造的に消える。

/// manifest ソースの上限（64KB）。
pub const MANIFEST_CAP: usize = 65536;
/// name の最大長（これ未満）。
pub const NAME_CAP: usize = 64;
/// version の最大長（これ未満）。
pub const VER_CAP: usize = 24;
/// entry の最大長（これ未満）。
pub const ENTRY_CAP: usize = 128;

/// perm 値（E1 単一効果）。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Perm {
    /// 効果なし。
    None,
    /// `status`（toast 表面化）。
    Status,
    /// `log`（console 常設面）。
    Log,
}

/// 解析済み manifest。
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Manifest {
    /// 拡張名（`[A-Za-z0-9_.-]{1,63}`）。
    pub name: String,
    /// バージョン（同 charset {1,23}）。
    pub version: String,
    /// エントリ basename（≤120、`/`・`\`・先頭 `.` を拒否）。
    pub entry: String,
    /// 権限。
    pub perm: Perm,
}

/// `[A-Za-z0-9_.-]` のみ（表示・パス両安全面の構造排除）。空は false。
fn is_charset(s: &[u8]) -> bool {
    !s.is_empty()
        && s.iter().all(|&c| {
            c.is_ascii_alphanumeric() || c == b'_' || c == b'.' || c == b'-'
        })
}

/// 前後の `' ' '\t'` と行末の単一 `'\r'` を除く。
fn trim(s: &[u8]) -> &[u8] {
    let mut a = 0;
    let mut b = s.len();
    while a < b && (s[a] == b' ' || s[a] == b'\t') {
        a += 1;
    }
    while b > a && (s[b - 1] == b' ' || s[b - 1] == b'\t' || s[b - 1] == b'\r') {
        b -= 1;
    }
    &s[a..b]
}

/// 表示用の権限名。
pub fn perm_name(perm: Perm) -> &'static str {
    match perm {
        Perm::Status => "status",
        Perm::Log => "log",
        Perm::None => "none",
    }
}

/// `src`（`manifest.txt` の内容）を解析する。失敗は 1 行理由（`\n` を含まない）。
pub fn parse(src: &[u8]) -> Result<Manifest, String> {
    if src.len() > MANIFEST_CAP {
        return Err(format!("manifest: too large ({} bytes)", src.len()));
    }
    let mut out = Manifest {
        name: String::new(),
        version: String::new(),
        entry: String::new(),
        perm: Perm::None,
    };
    let (mut seen_name, mut seen_ver, mut seen_entry, mut seen_perm) = (false, false, false, false);

    let mut lineno: u32 = 0;
    let mut p = src;
    while !p.is_empty() {
        // 1 行を取り出す（\n 区切り。末尾は \n が無くても 1 行）
        let (line, rest) = match p.iter().position(|&c| c == b'\n') {
            Some(i) => (&p[..i], &p[i + 1..]),
            None => (p, &[][..]),
        };
        p = rest;
        lineno += 1;

        let t = trim(line);
        if t.is_empty() || t[0] == b'#' {
            continue;
        }
        let colon = match t.iter().position(|&c| c == b':') {
            Some(i) => i,
            None => return Err(format!("manifest: line {lineno}: missing ':'")),
        };
        let key = trim(&t[..colon]);
        let val = trim(&t[colon + 1..]);

        if key == b"name" {
            if seen_name {
                return Err(format!("manifest: line {lineno}: duplicate key \"name\""));
            }
            seen_name = true;
            if !is_charset(val) || val.len() >= NAME_CAP {
                return Err(format!("manifest: line {lineno}: bad name"));
            }
            out.name = String::from_utf8_lossy(val).into_owned();
        } else if key == b"version" {
            if seen_ver {
                return Err(format!("manifest: line {lineno}: duplicate key \"version\""));
            }
            seen_ver = true;
            if !is_charset(val) || val.len() >= VER_CAP {
                return Err(format!("manifest: line {lineno}: bad version"));
            }
            out.version = String::from_utf8_lossy(val).into_owned();
        } else if key == b"entry" {
            if seen_entry {
                return Err(format!("manifest: line {lineno}: duplicate key \"entry\""));
            }
            seen_entry = true;
            if !is_charset(val) || val.len() >= ENTRY_CAP || val.first() == Some(&b'.') {
                return Err(format!("manifest: line {lineno}: bad entry (basename only)"));
            }
            // charset で '/' '\' は既に排除済み（二重防御の明記）
            out.entry = String::from_utf8_lossy(val).into_owned();
        } else if key == b"permissions" {
            if seen_perm {
                return Err(format!("manifest: line {lineno}: duplicate key \"permissions\""));
            }
            seen_perm = true;
            // ',' 区切りトークン走査。E1: ≤1 つの有効ケイパビリティ
            let mut perm = Perm::None;
            let mut n_tok: u32 = 0;
            let mut rest = val;
            while !rest.is_empty() {
                let (tok, next) = match rest.iter().position(|&c| c == b',') {
                    Some(i) => (trim(&rest[..i]), &rest[i + 1..]),
                    None => (trim(rest), &[][..]),
                };
                rest = next;
                if tok.is_empty() {
                    continue; // "a,,b" の空片は寛容（無害）
                }
                n_tok += 1;
                if tok == b"status" {
                    perm = Perm::Status;
                } else if tok == b"log" {
                    perm = Perm::Log;
                } else {
                    let shown = String::from_utf8_lossy(&tok[..tok.len().min(24)]).into_owned();
                    return Err(format!(
                        "manifest: line {lineno}: unknown permission \"{shown}\""
                    ));
                }
            }
            if n_tok > 1 {
                return Err(format!("manifest: line {lineno}: E1: at most one permission"));
            }
            out.perm = perm;
        } else {
            let shown = String::from_utf8_lossy(&key[..key.len().min(24)]).into_owned();
            return Err(format!("manifest: line {lineno}: unknown key \"{shown}\""));
        }
    }

    if !seen_name || !seen_ver || !seen_entry {
        return Err(format!(
            "manifest: required key missing ({}{}{})",
            if seen_name { "" } else { "name " },
            if seen_ver { "" } else { "version " },
            if seen_entry { "" } else { "entry" },
        ));
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn valid_basic() {
        let m = parse(b"name: hello\nversion: 0.1\nentry: main.js\npermissions: status\n").unwrap();
        assert_eq!(m.name, "hello");
        assert_eq!(m.version, "0.1");
        assert_eq!(m.entry, "main.js");
        assert_eq!(m.perm, Perm::Status);
    }

    #[test]
    fn valid_no_permissions() {
        let m = parse(b"name: chatty\nversion: 2\nentry: main.js\n").unwrap();
        assert_eq!(m.perm, Perm::None);
    }

    #[test]
    fn unknown_permission() {
        let e = parse(b"name: badcap\nversion: 0.1\nentry: main.js\npermissions: net\n").unwrap_err();
        assert_eq!(e, "manifest: line 4: unknown permission \"net\"");
    }

    #[test]
    fn missing_version() {
        let e = parse(b"name: broken\nentry: main.js\n").unwrap_err();
        assert_eq!(e, "manifest: required key missing (version )");
    }

    #[test]
    fn missing_all() {
        let e = parse(b"permissions: log\n").unwrap_err();
        assert_eq!(e, "manifest: required key missing (name version entry)");
    }

    #[test]
    fn duplicate_key() {
        let e = parse(b"name: a\nname: b\nversion: 1\nentry: x\n").unwrap_err();
        assert_eq!(e, "manifest: line 2: duplicate key \"name\"");
    }

    #[test]
    fn missing_colon() {
        let e = parse(b"name\n").unwrap_err();
        assert_eq!(e, "manifest: line 1: missing ':'");
    }

    #[test]
    fn bad_entry_dot() {
        let e = parse(b"name: a\nversion: 1\nentry: .hidden\n").unwrap_err();
        assert_eq!(e, "manifest: line 3: bad entry (basename only)");
    }

    #[test]
    fn bad_name_charset() {
        let e = parse(b"name: bad name\nversion: 1\nentry: x\n").unwrap_err();
        assert_eq!(e, "manifest: line 1: bad name");
    }

    #[test]
    fn unknown_key() {
        let e = parse(b"name: a\nversion: 1\nentry: x\nfoo: bar\n").unwrap_err();
        assert_eq!(e, "manifest: line 4: unknown key \"foo\"");
    }

    #[test]
    fn too_large() {
        let src = vec![b'x'; MANIFEST_CAP + 1];
        let e = parse(&src).unwrap_err();
        assert_eq!(e, format!("manifest: too large ({} bytes)", src.len()));
    }

    #[test]
    fn comments_and_blank() {
        let m = parse(b"# comment\n\nname: hello\nversion: 0.1\nentry: main.js\n").unwrap();
        assert_eq!(m.name, "hello");
    }

    #[test]
    fn crlf_and_trim() {
        let m = parse(b"name: hello\r\nversion: 0.1\r\nentry: main.js\r\n").unwrap();
        assert_eq!(m.entry, "main.js");
    }

    #[test]
    fn multiple_permissions_rejected() {
        let e = parse(b"name: a\nversion: 1\nentry: x\npermissions: status, log\n").unwrap_err();
        assert_eq!(e, "manifest: line 4: E1: at most one permission");
    }

    #[test]
    fn empty_perm_token_tolerated() {
        // "status,," は空片を寛容（無害）→ 有効トークン 1 個で成功
        let m = parse(b"name: a\nversion: 1\nentry: x\npermissions: status,\n").unwrap();
        assert_eq!(m.perm, Perm::Status);
    }

    #[test]
    fn perm_name_matches() {
        assert_eq!(perm_name(Perm::None), "none");
        assert_eq!(perm_name(Perm::Status), "status");
        assert_eq!(perm_name(Perm::Log), "log");
    }

    /// fuzz_ext.c の機械不変条件を再現:
    /// - 成功 ⇒ name/version/entry 非空・cap 内、perm ∈ {0,1,2}
    /// - 失敗 ⇒ 理由非空（理由なし失敗は契約違反）
    /// - 決定性（同一入力 2 回で完全一致）
    #[test]
    fn fuzz_invariants_and_determinism() {
        let cases: &[&[u8]] = &[
            b"name: a\nversion: 1\nentry: x\n",
            b"name: a\nversion: 1\nentry: x\npermissions: status\n",
            b"name: a\nversion: 1\nentry: x\npermissions: log\n",
            b"",
            b"garbage without colon\n",
            b"name: a\nversion: 1\nentry: x\npermissions: bogus\n",
        ];
        for &src in cases {
            let r1 = parse(src);
            let r2 = parse(src);
            assert_eq!(r1, r2, "非決定的: {src:?}");
            match r1 {
                Ok(m) => {
                    assert!(!m.name.is_empty() && m.name.len() < NAME_CAP);
                    assert!(!m.version.is_empty() && m.version.len() < VER_CAP);
                    assert!(!m.entry.is_empty() && m.entry.len() < ENTRY_CAP);
                }
                Err(e) => assert!(!e.is_empty(), "理由なし失敗: {src:?}"),
            }
        }
    }
}
