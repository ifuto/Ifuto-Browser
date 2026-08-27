//! BearSSL（`vendor/bearssl`、MIT）をシステム `cc` で静的ライブラリ化してリンクする。
//!
//! オフライン環境のため `cc` クレートに依存せず、`std::process::Command` で直接 `cc` /
//! `ar` を呼ぶ。コンパイル対象は Makefile の `BEARSRC` と同一（`ec_prime_i31_secp{256,384,
//! 521}r1.c` は `inner.h` で `#if 0` の旧残骸で独立コンパイル不能のため除外）。
//!
//! 製品法則「ldd = vdso/libm/libc/ld」は静的リンクで維持される（C 本体と同じ方針）。

use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn collect_sources(dir: &Path, out: &mut Vec<PathBuf>) {
    let mut entries: Vec<_> = std::fs::read_dir(dir)
        .unwrap()
        .map(|e| e.unwrap().path())
        .collect();
    entries.sort();
    for p in entries {
        if p.is_dir() {
            collect_sources(&p, out);
        } else if p.extension().map(|e| e == "c").unwrap_or(false) {
            out.push(p);
        }
    }
}

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    // rust/ifuto-ffi -> リポジトリルート
    let repo_root = manifest_dir
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .to_path_buf();
    let bearssl = repo_root.join("vendor/bearssl");
    let src_dir = bearssl.join("src");
    let inc_dir = bearssl.join("inc");

    let mut sources = Vec::new();
    collect_sources(&src_dir, &mut sources);
    sources.retain(|p| {
        let name = p.file_name().unwrap().to_string_lossy().into_owned();
        !matches!(
            name.as_str(),
            "ec_prime_i31_secp256r1.c" | "ec_prime_i31_secp384r1.c" | "ec_prime_i31_secp521r1.c"
        )
    });

    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    let obj_dir = out_dir.join("obj");
    std::fs::create_dir_all(&obj_dir).unwrap();

    let warn = [
        "-Wno-shadow",
        "-Wno-unused-parameter",
        "-Wno-sign-compare",
        "-Wno-missing-field-initializers",
        "-Wno-unused-but-set-variable",
        "-Wno-unused-variable",
    ];

    let mut objs: Vec<PathBuf> = Vec::new();
    for src in &sources {
        let rel = src.strip_prefix(&src_dir).unwrap();
        let obj_name = format!("{}.o", rel.to_string_lossy().replace('/', "__"));
        let obj = obj_dir.join(obj_name);
        let status = Command::new("cc")
            .arg("-c")
            .arg("-std=c11")
            .arg("-O2")
            .arg("-fno-strict-aliasing")
            .arg("-I")
            .arg(&inc_dir)
            .arg("-I")
            .arg(&src_dir)
            .args(warn.iter())
            .arg("-o")
            .arg(&obj)
            .arg(src)
            .status()
            .expect("failed to spawn cc");
        if !status.success() {
            panic!("BearSSL compile failed: {}", src.display());
        }
        objs.push(obj);
    }

    // static inline API のシム（bearssl_shim.c）をコンパイルして同梱。
    let shim = manifest_dir.join("bearssl_shim.c");
    let shim_obj = obj_dir.join("bearssl_shim.c.o");
    let status = Command::new("cc")
        .arg("-c")
        .arg("-std=c11")
        .arg("-O2")
        .arg("-fno-strict-aliasing")
        .arg("-I")
        .arg(&inc_dir)
        .arg("-I")
        .arg(&src_dir)
        .args(warn.iter())
        .arg("-o")
        .arg(&shim_obj)
        .arg(&shim)
        .status()
        .expect("failed to spawn cc (shim)");
    assert!(status.success(), "bearssl_shim.c compile failed");
    objs.push(shim_obj);
    println!("cargo:rerun-if-changed={}", shim.display());

    let lib = out_dir.join("libbearssl.a");
    let _ = std::fs::remove_file(&lib);
    let mut ar = Command::new("ar");
    ar.arg("rcs").arg(&lib);
    for o in &objs {
        ar.arg(o);
    }
    let status = ar.status().expect("failed to spawn ar");
    assert!(status.success(), "ar failed");

    // ABI 依存の構造体サイズ（sizeof）を C プログラムの実行結果から取得し、
    // Rust 側へ生成ファイルとして渡す（x86-64 / ARM 等の幅差をビルド時に吸収）。
    let sz_c = out_dir.join("bearssl_sizes.c");
    let sz_bin = out_dir.join("bearssl_sizes");
    std::fs::write(
        &sz_c,
        r#"
#include <stdio.h>
#include "bearssl.h"
int main(void) {
    printf("pub const SIZEOF_BR_SSL_CLIENT: usize = %zu;\n", sizeof(br_ssl_client_context));
    printf("pub const SIZEOF_BR_X509_MINIMAL: usize = %zu;\n", sizeof(br_x509_minimal_context));
    printf("pub const BR_SSL_BUFSIZE_BIDI: usize = %d;\n", BR_SSL_BUFSIZE_BIDI);
    printf("pub const SIZEOF_BR_X509_TRUST_ANCHOR: usize = %zu;\n", sizeof(br_x509_trust_anchor));
    return 0;
}
"#,
    )
    .unwrap();
    let status = Command::new("cc")
        .arg("-I")
        .arg(&inc_dir)
        .arg("-I")
        .arg(&src_dir)
        .arg("-o")
        .arg(&sz_bin)
        .arg(&sz_c)
        .status()
        .expect("failed to spawn cc (sizes)");
    assert!(status.success(), "sizes.c compile failed");
    let sizes_out = Command::new(&sz_bin)
        .output()
        .expect("failed to run sizes probe");
    assert!(sizes_out.status.success(), "sizes probe failed");
    std::fs::write(out_dir.join("bearssl_sizes.rs"), sizes_out.stdout).unwrap();

    println!("cargo:rustc-link-search=native={}", out_dir.display());
    println!("cargo:rustc-link-lib=static=bearssl");
    for src in &sources {
        println!("cargo:rerun-if-changed={}", src.display());
    }
    println!("cargo:rerun-if-changed={}", inc_dir.display());
}
