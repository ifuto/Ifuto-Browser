//! Aklus JS エンジンの C ABI ファサード（`akl.h` 互換）。
//!
//! ブラウザ統合（フェーズ 6）の FFI 層。`akl-core`（`#![forbid(unsafe_code)]`）を
//! 安全なコアのまま維持し、ここで最小限の `unsafe`（`extern "C"`・生ポインタの
//! 参照解除・C コールバック呼び出し）を 1 ファイルに集約して監査する。
//!
//! C 側（`src/akl/akl.h`）の `AklVal = uint64_t` は、Rust 側 `AklVal`（NaN-box、
//! `repr(transparent)` な u64 newtype）とビット互換。相互変換は
//! [`AklVal::bits`] / [`AklVal::from_bits`] で行う。

#![deny(unsafe_op_in_unsafe_fn)]
// FFI 境界の unsafe fn は、その安全性契約が全て「ポインタは akl.h の契約どおり
// 有効（NULL 可 / NUL 終端 / 長さ分の有効バッファ）」に統一される。個々の関数に
// `# Safety` を重複記載するより、境界を 1 ファイルに集約して一括監査する方針
// （RUST_MIGRATION.md「FFI の unsafe は最小限にし境界を 1 ファイルに集約」）。
// 各 unsafe 操作には本文内の `// SAFETY:` コメントを付す。
#![allow(clippy::missing_safety_doc)]

use akl_core::builtins::install_builtins;
use akl_core::bytecode::{HandleVTab, Runtime, VmError};
use akl_core::codegen::compile;
use akl_core::obj::Obj;
use akl_core::parser::Parser;
use akl_core::AklVal;
use std::os::raw::{c_char, c_int, c_void};

// ---- C ABI 型（akl.h と一致させる） ----

/// C 側 `AklVal`（`typedef uint64_t AklVal`）。
type CAklVal = u64;

/// C 側 `AklNativeFn`。
type CAklNativeFn =
    unsafe extern "C" fn(*mut AklRT, CAklVal, c_int, *const CAklVal, *mut c_void) -> CAklVal;

/// C 側 `AklModuleLoader`。
type CAklModuleLoader = unsafe extern "C" fn(
    *mut AklRT,
    *const c_char,
    *const c_char,
    *mut c_void,
    *mut *mut c_char,
    *mut *mut c_char,
);

/// C 側 `AklHandleVTab`（`get`/`set`/`call` の 3 コールバック + tag）。
#[repr(C)]
#[derive(Clone, Copy)]
pub struct CAklHandleVTab {
    tag: *const c_char,
    get: Option<
        unsafe extern "C" fn(*mut AklRT, *mut c_void, *const c_char, u32, *mut CAklVal) -> bool,
    >,
    set: Option<unsafe extern "C" fn(*mut AklRT, *mut c_void, *const c_char, u32, CAklVal) -> bool>,
    call: Option<
        unsafe extern "C" fn(
            *mut AklRT,
            *mut c_void,
            *const c_char,
            u32,
            c_int,
            *const CAklVal,
            *mut CAklVal,
        ) -> bool,
    >,
}

/// 不透明ラッパー（C からは `AklRT*` として見える。`rt.host_ctx` がこのアドレスを保持）。
#[repr(C)]
pub struct AklRT {
    /// 本体のランタイム。
    rt: Runtime,
    /// C ネイティブ登録表（`Obj::ForeignNative.data` がこの index を指す）。
    natives: Vec<(CAklNativeFn, *mut c_void)>,
    /// C ハンドル vtable 登録表（`Obj::Handle.data` がこの index を指す）。
    vtables: Vec<*const CAklHandleVTab>,
    /// C vtable ポインタ → `HandleVTab`（tag 込みで 1 回だけ作る）のキャッシュ。
    handle_vtab_cache: Vec<(*const CAklHandleVTab, &'static HandleVTab)>,
    /// `handle_vtab_for` が `Box::into_raw` で生成した `HandleVTab` の所有ポインタ
    /// （cache の `&'static` はこれらを指す。`akl_free` で `Box::from_raw` して解放）。
    /// **Box 値として保持しない理由（Miri/Tree Borrows 適合）**: Box を `Vec<Box<T>>`
    /// で保持すると push/再配置のたびに Box 値がムーブされ、Tree Borrows は
    /// pointee を Unique retag = 先行の `&'static` 派生タグを殺す。`into_raw` で
    /// 生ポインタ化しておけば Box 値のムーブは存在せず、参照タグは from_raw による
    /// 回収まで生存する。ヒープ領域自体も動かない。
    owned_vtabs: Vec<*mut HandleVTab>,
    /// 同上。C tag 文字列のコピーの所有ポインタ（`Box<str>` の `into_raw` 由来）。
    owned_tags: Vec<*mut str>,
    /// 汎用 foreign アダプタの登録 index（遅延 1 回）。
    foreign_idx: Option<u32>,
    /// エラー文言バッファ（NUL 終端。C の `rt->err` 相当）。
    err: [u8; 256],
    /// エラー文言長（NUL 除く）。
    err_len: usize,
    /// C ネイティブが `akl_native_throw` したか。
    native_err: bool,
    /// モジュールローダ（API 互換で保持。Rust 側 import は簡易近似のため未使用）。
    _mod_loader: Option<CAklModuleLoader>,
    _mod_udata: *mut c_void,
    _mod_base: Vec<u8>,
    /// 命令バジェット（未使用だが API 互換で保持）。
    _insn_budget: u64,
}

// ---- ユーティリティ ----

/// NUL 終端 C 文字列 → `&str`（NULL ポインタなら空文字列）。
unsafe fn cstr<'a>(p: *const c_char) -> &'a str {
    if p.is_null() {
        return "";
    }
    // SAFETY: 呼び出し側が NUL 終端の有効なポインタを渡す契約（akl.h 準拠）。
    let c = unsafe { std::ffi::CStr::from_ptr(p) };
    c.to_str().unwrap_or("")
}

/// エラー文言を設定（NUL 終端バッファへコピー）。
fn set_err(rt: &mut AklRT, msg: &str) {
    let bytes = msg.as_bytes();
    let n = bytes.len().min(255);
    rt.err[..n].copy_from_slice(&bytes[..n]);
    rt.err[n] = 0;
    rt.err_len = n;
}

/// エラー文言をクリア（NUL 終端も戻す。`akl_error` が陳腐な文言を返さないため）。
fn clear_err(rt: &mut AklRT) {
    rt.err[0] = 0;
    rt.err_len = 0;
}

/// `VmError` → 文言（`akl_error` 用の簡易マップ）。
fn vm_err_string(e: &VmError) -> String {
    match e {
        VmError::StackUnderflow => "stack underflow".into(),
        VmError::LocalOob => "local out of bounds".into(),
        VmError::GlobalNotFound => "ReferenceError: identifier is not defined".into(),
        VmError::NotCallable => "TypeError: not a function".into(),
        VmError::JumpOob => "jump out of bounds".into(),
        VmError::Oom => "out of memory".into(),
        VmError::NotObject => "TypeError: not an object".into(),
        VmError::BudgetExhausted => "instruction budget exhausted".into(),
        VmError::Thrown(v) => {
            // 例外値（文字列ならそのまま、それ以外は汎用文言）
            let mut s = String::new();
            // 簡易: 値のビット列は解釈しない（C 側も値伝搬は別経路）
            s.push_str(&format!("uncaught exception: {v:?}"));
            s
        }
    }
}

/// ポインタ + 長さ → `&[CAklVal]`（NULL / 長さ 0 は空）。
unsafe fn val_slice<'a>(p: *const CAklVal, n: c_int) -> &'a [CAklVal] {
    if p.is_null() || n <= 0 {
        return &[];
    }
    // SAFETY: 呼び出し側が n 個の有効な AklVal を渡す契約（akl.h 準拠）。
    unsafe { std::slice::from_raw_parts(p, n as usize) }
}

// ---- ホスト（FFI 層）コールバックのアダプタ（安全シグネチャ・unsafe ボディ） ----
//
// 【Miri/Stacked Borrows 適合ノート】
// 本節のアダプタは `run_loop` 実行中に呼ばれる。このとき上位フレーム（akl_eval
// 系）には AklRT box への `&mut` が生存しており、そのタグは強保護されている。
// ここで `host_ctx`（整数経由・wildcard 来歴）から `&*w` / `&mut *w` のような
// **参照の再生成**を行うと、その保護タグのポップが必要になり Stacked Borrows
// 違反（実機では動くが形式的 UB）となる。一方、プレーンな生 read/write は
// スタック頂上の保護タグ自身が許可するため合法。
// よって本節では参照を一切生成せず、Vec ヘッダは `ptr::read` で **値コピー**
// （要素バッファは別割当で保護競合なし）、スカラ/配列は `addr_of[_mut]` 経由の
// 生アクセスで処理する。

/// box 内 `natives` 登録表の生読み。返す `ManuallyDrop` は Vec ヘッダの値コピー
/// （所有権の移動ではない。インデックス読み取り専用。二重 drop させないこと）。
///
/// SAFETY: `w` は有効な AklRT を指す（akl_new 由来）。
unsafe fn rd_natives(w: *mut AklRT) -> std::mem::ManuallyDrop<Vec<(CAklNativeFn, *mut c_void)>> {
    // SAFETY: 上記契約どおり。参照は生成しない。
    std::mem::ManuallyDrop::new(unsafe { std::ptr::addr_of!((*w).natives).read() })
}

/// box 内 `vtables` 登録表の生読み（`rd_natives` と同じ契約）。
///
/// SAFETY: `w` は有効な AklRT を指す（akl_new 由来）。
unsafe fn rd_vtables(w: *mut AklRT) -> std::mem::ManuallyDrop<Vec<*const CAklHandleVTab>> {
    // SAFETY: 上記契約どおり。参照は生成しない。
    std::mem::ManuallyDrop::new(unsafe { std::ptr::addr_of!((*w).vtables).read() })
}

/// C ネイティブを呼ぶ汎用アダプタ（`Obj::ForeignNative` から呼ばれる）。
fn foreign_adapter(
    rt: &mut Runtime,
    this: AklVal,
    args: &[AklVal],
    data: u64,
) -> Result<AklVal, VmError> {
    let wrapper = rt.host_ctx as *mut AklRT;
    if wrapper.is_null() {
        return Err(VmError::NotCallable);
    }
    // SAFETY: host_ctx は akl_new が自前 AklRT のアドレスで初期化する。
    let natives = unsafe { rd_natives(wrapper) };
    let (c_fn, udata) = natives[data as usize];
    let argv: Vec<CAklVal> = args.iter().map(|v| v.bits()).collect();
    // SAFETY: c_fn は登録時の AklNativeFn。argv は args のビット列を保持したローカル。
    let result = unsafe {
        c_fn(
            wrapper,
            this.bits(),
            args.len() as c_int,
            argv.as_ptr(),
            udata,
        )
    };
    // C ネイティブが akl_native_throw したか（すべて参照不生成の生アクセス）
    // SAFETY: wrapper は akl_new 由来の有効ポインタ。
    let native_err = unsafe { std::ptr::addr_of!((*wrapper).native_err).read() };
    if native_err {
        // SAFETY: 同上（生書き込み）。
        unsafe { std::ptr::addr_of_mut!((*wrapper).native_err).write(false) };
        // SAFETY: 同上（生読み込み。err_len は常に 255 以下）。
        let err_len = unsafe { std::ptr::addr_of!((*wrapper).err_len).read() };
        let mut buf = vec![0u8; err_len];
        // SAFETY: buf は err_len バイト確保済み、err[..err_len] は有効範囲。
        unsafe {
            std::ptr::copy_nonoverlapping(
                std::ptr::addr_of!((*wrapper).err).cast::<u8>(),
                buf.as_mut_ptr(),
                err_len,
            )
        };
        let msg = std::str::from_utf8(&buf).unwrap_or("native exception");
        let msg_id = rt.intern(msg).unwrap_or(0);
        return Err(VmError::Thrown(AklVal::mk_obj(msg_id)));
    }
    Ok(AklVal::from_bits(result))
}

/// C ハンドルの `get` アダプタ。
fn handle_get_adapter(rt: &mut Runtime, data: u64, ptr: u64, name: &str) -> Option<AklVal> {
    let wrapper = rt.host_ctx as *mut AklRT;
    // SAFETY: host_ctx は akl_new が初期化。data は akl_mkhandle が vtables index で設定。
    let vtables = unsafe { rd_vtables(wrapper) };
    let c_vt = vtables[data as usize];
    // SAFETY: c_vt は登録時の CAklHandleVTab ポインタ。
    let get = unsafe { (*c_vt).get }?;
    let mut out: CAklVal = 0;
    // SAFETY: get は C vtable のコールバック。name は intern 済み文字列の有効スライス。
    let ok = unsafe {
        get(
            wrapper,
            ptr as *mut c_void,
            name.as_ptr() as *const c_char,
            name.len() as u32,
            &mut out,
        )
    };
    if ok {
        Some(AklVal::from_bits(out))
    } else {
        None
    }
}

/// C ハンドルの `set` アダプタ。
fn handle_set_adapter(rt: &mut Runtime, data: u64, ptr: u64, name: &str, v: AklVal) -> bool {
    let wrapper = rt.host_ctx as *mut AklRT;
    // SAFETY: host_ctx は akl_new が初期化。data は vtables index。
    let vtables = unsafe { rd_vtables(wrapper) };
    let c_vt = vtables[data as usize];
    // SAFETY: c_vt は登録時のポインタ。
    let Some(set) = (unsafe { (*c_vt).set }) else {
        return false;
    };
    // SAFETY: set は C vtable のコールバック。
    unsafe {
        set(
            wrapper,
            ptr as *mut c_void,
            name.as_ptr() as *const c_char,
            name.len() as u32,
            v.bits(),
        )
    }
}

/// C ハンドルの `call` アダプタ。
fn handle_call_adapter(
    rt: &mut Runtime,
    data: u64,
    ptr: u64,
    name: &str,
    args: &[AklVal],
) -> Option<AklVal> {
    let wrapper = rt.host_ctx as *mut AklRT;
    // SAFETY: host_ctx は akl_new が初期化。data は vtables index。
    let vtables = unsafe { rd_vtables(wrapper) };
    let c_vt = vtables[data as usize];
    // SAFETY: c_vt は登録時のポインタ。
    let call = unsafe { (*c_vt).call }?;
    let argv: Vec<CAklVal> = args.iter().map(|v| v.bits()).collect();
    let mut out: CAklVal = 0;
    // SAFETY: call は C vtable のコールバック。argv はローカルのビット列。
    let ok = unsafe {
        call(
            wrapper,
            ptr as *mut c_void,
            name.as_ptr() as *const c_char,
            name.len() as u32,
            args.len() as c_int,
            argv.as_ptr(),
            &mut out,
        )
    };
    if ok {
        Some(AklVal::from_bits(out))
    } else {
        None
    }
}

/// C vtable ポインタ → （tag 込みの）`&'static HandleVTab`（キャッシュ・dedup）。
fn handle_vtab_for(rt: &mut AklRT, vt: *const CAklHandleVTab) -> &'static HandleVTab {
    for (cvt, hv) in &rt.handle_vtab_cache {
        if *cvt == vt {
            return hv;
        }
    }
    // tag を C 文字列からコピーしてヒープ保持（leak ではなく AklRT 所有。
    // `&'static` の見かけは FFI 境界のライフタイム延長で、実寿命は akl_free まで）。
    // SAFETY: vt は呼び出し側が有効な AklHandleVTab を渡す契約。
    let tag: &'static str = unsafe {
        if (*vt).tag.is_null() {
            "Handle"
        } else {
            let c = std::ffi::CStr::from_ptr((*vt).tag);
            let s = c.to_str().unwrap_or("Handle");
            let p: *mut str = Box::into_raw(s.to_string().into_boxed_str());
            rt.owned_tags.push(p);
            // SAFETY: p は直上の Box::into_raw 由来。into_raw 後 Box 値は一度も
            // ムーブされず（Vec には生ポインタで保持）、akl_free までアドレス・
            // タグともに安定。回収は akl_free が from_raw で 1 回だけ行う。
            &*p
        }
    };
    let hp: *mut HandleVTab = Box::into_raw(Box::new(HandleVTab {
        tag,
        get: handle_get_adapter,
        set: handle_set_adapter,
        call: handle_call_adapter,
    }));
    rt.owned_vtabs.push(hp);
    // SAFETY: 同上。into_raw 由来の安定アドレス/タグ。
    let hv: &'static HandleVTab = unsafe { &*hp };
    rt.handle_vtab_cache.push((vt, hv));
    hv
}

// ---- C ABI 公開関数 ----

/// `akl_new`: ランタイムを作る（失敗時 NULL）。
#[no_mangle]
pub unsafe extern "C" fn akl_new() -> *mut AklRT {
    let mut rt = Runtime::new();
    if install_builtins(&mut rt).is_err() {
        return std::ptr::null_mut();
    }
    let boxed = Box::new(AklRT {
        rt,
        natives: Vec::new(),
        vtables: Vec::new(),
        handle_vtab_cache: Vec::new(),
        owned_vtabs: Vec::new(),
        owned_tags: Vec::new(),
        foreign_idx: None,
        err: [0; 256],
        err_len: 0,
        native_err: false,
        _mod_loader: None,
        _mod_udata: std::ptr::null_mut(),
        _mod_base: Vec::new(),
        _insn_budget: 0,
    });
    let ptr = Box::into_raw(boxed);
    // SAFETY: ptr は直上の Box::into_raw 由来。host_ctx を自アドレスで初期化。
    unsafe {
        (*ptr).rt.host_ctx = ptr as u64;
    }
    ptr
}

/// `akl_free`: ランタイムを破棄。
#[no_mangle]
pub unsafe extern "C" fn akl_free(rt: *mut AklRT) {
    if !rt.is_null() {
        // SAFETY: rt は akl_new が返した所有ポインタ（呼び出し側の契約）。
        let mut boxed = unsafe { Box::from_raw(rt) };
        // handle_vtab_for が Box::into_raw した vtab/tag 群を from_raw で回収する。
        // akl-core にカスタム Drop は無く（`impl Drop` grep 0 件確認）、本体
        // （Runtime）の drop は heap 内 `Obj::Handle` の vtab/tag の中身を読まない
        // ため、先に回収しても安全。`&'static` 参照はこの後 1 度も使われない
        // （cache も一緒に消える）ので use-after-free は成立しない。
        let vtabs = std::mem::take(&mut boxed.owned_vtabs);
        let tags = std::mem::take(&mut boxed.owned_tags);
        for p in vtabs {
            // SAFETY: p は handle_vtab_for で into_raw した box（1 回だけ解放）。
            unsafe { drop(Box::from_raw(p)) };
        }
        for p in tags {
            // SAFETY: 同上（Box<str> の into_raw 由来。同様に 1 回だけ）。
            unsafe { drop(Box::from_raw(p)) };
        }
        drop(boxed);
    }
}

/// `akl_eval`: ソースを実行して最後の式文の値を *out へ。
#[no_mangle]
pub unsafe extern "C" fn akl_eval(rt: *mut AklRT, src: *const c_char, out: *mut CAklVal) -> bool {
    if rt.is_null() {
        return false;
    }
    // SAFETY: rt は akl_new 由来。src は NUL 終端（akl.h 契約）。
    let rt = unsafe { &mut *rt };
    let src = unsafe { cstr(src) };
    // 前回エラーをクリア
    clear_err(rt);
    rt.native_err = false;
    let program = match Parser::new(src).parse_program() {
        Ok(p) => p,
        Err(e) => {
            // C 実装と同一の「SyntaxError: <why>」書式（test_script のオラクルが依存）
            set_err(rt, &format!("SyntaxError: {}", e.0));
            return false;
        }
    };
    let fidx = match compile(&mut rt.rt, &program) {
        Ok(f) => f,
        Err(e) => {
            set_err(rt, &format!("SyntaxError: {}", e.0));
            return false;
        }
    };
    match rt.rt.run(fidx, &[]) {
        Ok(v) => {
            if !out.is_null() {
                // SAFETY: out は呼び出し側の有効ポインタ。
                unsafe { *out = v.bits() };
            }
            true
        }
        Err(e) => {
            // Thrown（例外値）は文字列なら内容を、それ以外は汎用文言を返す。
            let msg = match &e {
                VmError::Thrown(v) => {
                    if v.is_obj() {
                        let s = rt.rt.flatten_str(*v);
                        if !s.is_empty() {
                            s
                        } else {
                            vm_err_string(&e)
                        }
                    } else {
                        vm_err_string(&e)
                    }
                }
                _ => vm_err_string(&e),
            };
            set_err(rt, &msg);
            false
        }
    }
}

/// `akl_call`: 関数値を呼ぶ（ホストからの再入。FUNC/NATIVE）。
#[no_mangle]
pub unsafe extern "C" fn akl_call(
    rt: *mut AklRT,
    f: CAklVal,
    argc: c_int,
    argv: *const CAklVal,
    out: *mut CAklVal,
) -> bool {
    unsafe { akl_call_this(rt, f, AklVal::UNDEF.bits(), argc, argv, out) }
}

/// `akl_call_this`: this 指定の再入呼び出し。
#[no_mangle]
pub unsafe extern "C" fn akl_call_this(
    rt: *mut AklRT,
    f: CAklVal,
    thisv: CAklVal,
    argc: c_int,
    argv: *const CAklVal,
    out: *mut CAklVal,
) -> bool {
    if rt.is_null() {
        return false;
    }
    // SAFETY: rt は akl_new 由来。argv は argc 個の有効値。
    let rt = unsafe { &mut *rt };
    let args: Vec<AklVal> = unsafe { val_slice(argv, argc) }
        .iter()
        .map(|v| AklVal::from_bits(*v))
        .collect();
    match rt
        .rt
        .call_value(AklVal::from_bits(f), AklVal::from_bits(thisv), &args)
    {
        Ok(v) => {
            if !out.is_null() {
                // SAFETY: out は呼び出し側の有効ポインタ。
                unsafe { *out = v.bits() };
            }
            true
        }
        Err(e) => {
            set_err(rt, &vm_err_string(&e));
            false
        }
    }
}

/// `akl_error`: 直近のエラー文言。
#[no_mangle]
pub unsafe extern "C" fn akl_error(rt: *mut AklRT) -> *const c_char {
    if rt.is_null() {
        return c"".as_ptr();
    }
    // SAFETY: rt は akl_new 由来。
    let rt = unsafe { &*rt };
    // C 実装の `akl_error` は `err[0]` が NUL なら空文字を返す。err_len で同等に判定し、
    // 前回 eval の残留文言が後続 eval 成功後に見えることを防ぐ。
    if rt.err_len == 0 {
        return c"".as_ptr();
    }
    rt.err.as_ptr() as *const c_char
}

/// `akl_as_num`: 数値（int/double）なら out を満たして true。
#[no_mangle]
pub unsafe extern "C" fn akl_as_num(v: CAklVal, out: *mut f64) -> bool {
    let val = AklVal::from_bits(v);
    let d = if val.is_int() {
        val.get_int() as f64
    } else if let Some(d) = val.as_f64() {
        d
    } else {
        return false;
    };
    if !out.is_null() {
        // SAFETY: out は呼び出し側の有効ポインタ。
        unsafe { *out = d };
    }
    true
}

/// `akl_as_bool`: 真偽値なら out を満たして true。
#[no_mangle]
pub unsafe extern "C" fn akl_as_bool(v: CAklVal, out: *mut bool) -> bool {
    let val = AklVal::from_bits(v);
    let b = if val == AklVal::TRUE {
        true
    } else if val == AklVal::FALSE {
        false
    } else {
        return false;
    };
    if !out.is_null() {
        // SAFETY: out は呼び出し側の有効ポインタ。
        unsafe { *out = b };
    }
    true
}

/// `akl_is_null`。
#[no_mangle]
pub unsafe extern "C" fn akl_is_null(v: CAklVal) -> bool {
    AklVal::from_bits(v).is_null()
}

/// `akl_is_undefined`。
#[no_mangle]
pub unsafe extern "C" fn akl_is_undefined(v: CAklVal) -> bool {
    AklVal::from_bits(v).is_undef()
}

/// `akl_is_string`（STR/ROPE）。
#[no_mangle]
pub unsafe extern "C" fn akl_is_string(rt: *mut AklRT, v: CAklVal) -> bool {
    if rt.is_null() {
        return false;
    }
    let val = AklVal::from_bits(v);
    if !val.is_obj() {
        return false;
    }
    // SAFETY: rt は akl_new 由来。
    matches!(
        unsafe { (*rt).rt.heap.get(val.get_obj()) },
        Some(Obj::Str(_)) | Some(Obj::Rope { .. })
    )
}

/// `akl_as_str`: 文字列内容のポインタ（rt 所有。len はバイト長）。
///
/// C 実装と同一に「ヒープ文字列の内部ポインタ」を返す（単一スクラッチバッファでは
/// 呼び出し側が `akl_as_str(argv[0])` と `akl_as_str(argv[1])` を同時に保持できず、
/// `elem_call` の setAttribute で名前と値が相互に破壊される）。ROPE はここで
/// 平坦化して同一 ObjId の STR に置換する（Rust 側は実行中に自動 GC しないため、
/// 返したポインタは `akl_free` まで有効）。
#[no_mangle]
pub unsafe extern "C" fn akl_as_str(rt: *mut AklRT, v: CAklVal, len: *mut u32) -> *const c_char {
    if rt.is_null() {
        return std::ptr::null();
    }
    // SAFETY: rt は akl_new 由来。
    let rt = unsafe { &mut *rt };
    let val = AklVal::from_bits(v);
    if !val.is_obj() {
        return std::ptr::null();
    }
    let id = val.get_obj();
    if !matches!(
        rt.rt.heap.get(id),
        Some(Obj::Str(_)) | Some(Obj::Rope { .. })
    ) {
        return std::ptr::null();
    }
    rt.rt.flatten_rope_in_place(id);
    match rt.rt.heap.get(id) {
        Some(Obj::Str(s)) => {
            if !len.is_null() {
                // SAFETY: len は呼び出し側の有効ポインタ。
                unsafe { *len = s.len() as u32 };
            }
            // SAFETY: Box<str> のヒープデータは安定（akl_free まで不変）。
            s.as_ptr() as *const c_char
        }
        _ => std::ptr::null(),
    }
}

// ---- 値生成 ----

/// `akl_mknum`。
#[no_mangle]
pub unsafe extern "C" fn akl_mknum(d: f64) -> CAklVal {
    AklVal::from_f64(d).bits()
}

/// `akl_mkbool`。
#[no_mangle]
pub unsafe extern "C" fn akl_mkbool(b: bool) -> CAklVal {
    AklVal::from_bool(b).bits()
}

/// `akl_mknull`。
#[no_mangle]
pub unsafe extern "C" fn akl_mknull() -> CAklVal {
    AklVal::NULL.bits()
}

/// `akl_mkundefined`。
#[no_mangle]
pub unsafe extern "C" fn akl_mkundefined() -> CAklVal {
    AklVal::UNDEF.bits()
}

/// `akl_mkstring`: 文字列値を生成（intern）。
#[no_mangle]
pub unsafe extern "C" fn akl_mkstring(rt: *mut AklRT, s: *const c_char, len: u32) -> CAklVal {
    if rt.is_null() {
        return AklVal::UNDEF.bits();
    }
    // SAFETY: rt は akl_new 由来。s は len バイトの有効データ（NULL なら空）。
    let rt = unsafe { &mut *rt };
    let bytes = if s.is_null() || len == 0 {
        &[][..]
    } else {
        // SAFETY: s は len バイトの有効バッファ。
        unsafe { std::slice::from_raw_parts(s as *const u8, len as usize) }
    };
    let text = std::str::from_utf8(bytes).unwrap_or("");
    match rt.rt.intern(text) {
        Some(id) => AklVal::mk_obj(id).bits(),
        None => {
            set_err(rt, "oom: string");
            AklVal::UNDEF.bits()
        }
    }
}

/// `akl_mkarray`: ホスト側から配列を生成。
#[no_mangle]
pub unsafe extern "C" fn akl_mkarray(rt: *mut AklRT, items: *const CAklVal, n: u32) -> CAklVal {
    if rt.is_null() {
        return AklVal::UNDEF.bits();
    }
    // SAFETY: rt は akl_new 由来。
    let rt = unsafe { &mut *rt };
    let vals: Vec<AklVal> = if items.is_null() || n == 0 {
        Vec::new()
    } else {
        // SAFETY: items は n 個の有効値。
        unsafe { std::slice::from_raw_parts(items, n as usize) }
            .iter()
            .map(|v| AklVal::from_bits(*v))
            .collect()
    };
    match rt.rt.heap.alloc(Obj::Arr(vals)) {
        Ok(id) => AklVal::mk_obj(id).bits(),
        Err(_) => {
            set_err(rt, "oom: array");
            AklVal::UNDEF.bits()
        }
    }
}

/// `akl_arr_len`: 配列の要素数。
#[no_mangle]
pub unsafe extern "C" fn akl_arr_len(rt: *mut AklRT, arr: CAklVal) -> u32 {
    if rt.is_null() {
        return 0;
    }
    let val = AklVal::from_bits(arr);
    if !val.is_obj() {
        return 0;
    }
    // SAFETY: rt は akl_new 由来。
    match unsafe { (*rt).rt.heap.get(val.get_obj()) } {
        Some(Obj::Arr(items)) => items.len() as u32,
        _ => 0,
    }
}

/// `akl_mkobject`: 空のプレーンオブジェクトを生成。
#[no_mangle]
pub unsafe extern "C" fn akl_mkobject(rt: *mut AklRT) -> CAklVal {
    if rt.is_null() {
        return AklVal::UNDEF.bits();
    }
    // SAFETY: rt は akl_new 由来。
    let rt = unsafe { &mut *rt };
    match rt.rt.heap.alloc(Obj::Obj(Vec::new())) {
        Ok(id) => AklVal::mk_obj(id).bits(),
        Err(_) => {
            set_err(rt, "oom: object");
            AklVal::UNDEF.bits()
        }
    }
}

/// `akl_is_object`: プレーンオブジェクト（OBJ）か。
#[no_mangle]
pub unsafe extern "C" fn akl_is_object(rt: *mut AklRT, v: CAklVal) -> bool {
    if rt.is_null() {
        return false;
    }
    let val = AklVal::from_bits(v);
    if !val.is_obj() {
        return false;
    }
    // SAFETY: rt は akl_new 由来。
    matches!(
        unsafe { (*rt).rt.heap.get(val.get_obj()) },
        Some(Obj::Obj(_))
    )
}

/// `akl_is_handle`: ハンドルか。
#[no_mangle]
pub unsafe extern "C" fn akl_is_handle(rt: *mut AklRT, v: CAklVal) -> bool {
    if rt.is_null() {
        return false;
    }
    let val = AklVal::from_bits(v);
    if !val.is_obj() {
        return false;
    }
    // SAFETY: rt は akl_new 由来。
    matches!(
        unsafe { (*rt).rt.heap.get(val.get_obj()) },
        Some(Obj::Handle { .. })
    )
}

/// `akl_prop_set`: プレーンオブジェクトのプロパティ設定。
#[no_mangle]
pub unsafe extern "C" fn akl_prop_set(
    rt: *mut AklRT,
    obj: CAklVal,
    name: *const c_char,
    v: CAklVal,
) -> bool {
    if rt.is_null() {
        return false;
    }
    // SAFETY: rt は akl_new 由来。name は NUL 終端。
    let rt = unsafe { &mut *rt };
    let name = unsafe { cstr(name) };
    let val = AklVal::from_bits(v);
    let objval = AklVal::from_bits(obj);
    if !objval.is_obj() {
        set_err(rt, "prop_set on non-object");
        return false;
    }
    let name_id = match rt.rt.intern(name) {
        Some(id) => id,
        None => {
            set_err(rt, "oom: intern");
            return false;
        }
    };
    rt.rt.heap.prop_set(objval.get_obj(), name_id, val).is_ok()
}

/// `akl_prop_get`: プレーンオブジェクトのプロパティ取得（無ければ undefined）。
#[no_mangle]
pub unsafe extern "C" fn akl_prop_get(
    rt: *mut AklRT,
    obj: CAklVal,
    name: *const c_char,
) -> CAklVal {
    if rt.is_null() {
        return AklVal::UNDEF.bits();
    }
    // SAFETY: rt は akl_new 由来。name は NUL 終端。
    let rt = unsafe { &mut *rt };
    let name = unsafe { cstr(name) };
    let objval = AklVal::from_bits(obj);
    if !objval.is_obj() {
        return AklVal::UNDEF.bits();
    }
    let name_id = match rt.rt.intern(name) {
        Some(id) => id,
        None => return AklVal::UNDEF.bits(),
    };
    rt.rt
        .heap
        .prop_get(objval.get_obj(), name_id)
        .map(|v| v.bits())
        .unwrap_or(AklVal::UNDEF.bits())
}

/// `akl_global_set`: グローバル変数を値に束縛。
#[no_mangle]
pub unsafe extern "C" fn akl_global_set(rt: *mut AklRT, name: *const c_char, v: CAklVal) -> bool {
    if rt.is_null() {
        return false;
    }
    // SAFETY: rt は akl_new 由来。name は NUL 終端。
    let rt = unsafe { &mut *rt };
    let name = unsafe { cstr(name) };
    let name_id = match rt.rt.intern(name) {
        Some(id) => id,
        None => {
            set_err(rt, "oom: intern");
            return false;
        }
    };
    rt.rt.global_set(name_id, AklVal::from_bits(v));
    true
}

/// `akl_tostring`: JS ToString（全型で文字列値を返す）。
#[no_mangle]
pub unsafe extern "C" fn akl_tostring(rt: *mut AklRT, v: CAklVal) -> CAklVal {
    if rt.is_null() {
        return AklVal::UNDEF.bits();
    }
    // SAFETY: rt は akl_new 由来。
    let rt = unsafe { &mut *rt };
    match rt.rt.stringify(AklVal::from_bits(v)) {
        Ok(id) => AklVal::mk_obj(id).bits(),
        Err(_) => {
            set_err(rt, "oom: stringify");
            AklVal::UNDEF.bits()
        }
    }
}

// ---- ネイティブ / ハンドル登録 ----

/// 汎用 foreign アダプタの index を取得（遅延 1 回登録）。
fn foreign_idx(rt: &mut AklRT) -> Result<u32, VmError> {
    if let Some(i) = rt.foreign_idx {
        return Ok(i);
    }
    let i = rt.rt.register_foreign_native(foreign_adapter)?;
    rt.foreign_idx = Some(i);
    Ok(i)
}

/// `akl_native_register`: C ネイティブをグローバル const として束縛。
#[no_mangle]
pub unsafe extern "C" fn akl_native_register(
    rt: *mut AklRT,
    name: *const c_char,
    fn_: CAklNativeFn,
    udata: *mut c_void,
) -> bool {
    if rt.is_null() {
        return false;
    }
    // SAFETY: rt は akl_new 由来。
    let rt = unsafe { &mut *rt };
    let v = unsafe { akl_mknative(rt, fn_, udata) };
    if v == AklVal::UNDEF.bits() {
        return false;
    }
    // name は **呼び出し側から受け取った生ポインタをそのまま転送** する。
    // `cstr()` で &str に落として `as_ptr()` で戻す往復は、SharedReadOnly retag が
    // [0..len) にしか及ばず、内側の strlen が読む NUL（[len]）の来歴を殺すため
    // Stacked Borrows 違反（実機では偶発的に動く形式的 UB）になる。
    // SAFETY: name は NUL 終端（akl.h 契約）。akl_global_set 内で cstr される。
    unsafe { akl_global_set(rt, name, v) }
}

/// `akl_mknative`: C ネイティブの関数値を生成（束縛はしない）。
#[no_mangle]
pub unsafe extern "C" fn akl_mknative(
    rt: *mut AklRT,
    fn_: CAklNativeFn,
    udata: *mut c_void,
) -> CAklVal {
    if rt.is_null() {
        return AklVal::UNDEF.bits();
    }
    // SAFETY: rt は akl_new 由来。
    let rt = unsafe { &mut *rt };
    rt.natives.push((fn_, udata));
    let data = (rt.natives.len() - 1) as u64;
    let idx = match foreign_idx(rt) {
        Ok(i) => i,
        Err(_) => {
            set_err(rt, "oom: foreign native");
            return AklVal::UNDEF.bits();
        }
    };
    match rt.rt.heap.alloc(Obj::ForeignNative { idx, data }) {
        Ok(id) => AklVal::mk_obj(id).bits(),
        Err(_) => {
            set_err(rt, "oom: native");
            AklVal::UNDEF.bits()
        }
    }
}

/// `akl_mkhandle`: C ハンドル（DOM 要素等）の値を生成。
#[no_mangle]
pub unsafe extern "C" fn akl_mkhandle(
    rt: *mut AklRT,
    vt: *const CAklHandleVTab,
    ptr: *mut c_void,
) -> CAklVal {
    if rt.is_null() || vt.is_null() {
        return AklVal::UNDEF.bits();
    }
    // SAFETY: rt は akl_new 由来。
    let rt = unsafe { &mut *rt };
    rt.vtables.push(vt);
    let data = (rt.vtables.len() - 1) as u64;
    let hv = handle_vtab_for(rt, vt);
    match rt.rt.heap.alloc(Obj::Handle {
        vtab: hv,
        data,
        ptr: ptr as u64,
    }) {
        Ok(id) => AklVal::mk_obj(id).bits(),
        Err(_) => {
            set_err(rt, "oom: handle");
            AklVal::UNDEF.bits()
        }
    }
}

/// `akl_native_throw`: C ネイティブ内から例外相当を起こす。
#[no_mangle]
pub unsafe extern "C" fn akl_native_throw(rt: *mut AklRT, msg: *const c_char) {
    if rt.is_null() {
        return;
    }
    // SAFETY: msg は NUL 終端（akl.h 契約）。
    let msg = unsafe { cstr(msg) };
    // 本関数は eval 実行中のネイティブから呼ばれうる。その間は上位フレームに
    // AklRT box への `&mut` が生存（強保護）しており、`&mut *rt` の再参照生成は
    // Stacked Borrows 違反となるため、err / err_len / native_err への書き込みは
    // 参照不生成の生経路で行う（set_err と同じ収支）。
    let bytes = msg.as_bytes();
    let n = bytes.len().min(255);
    // SAFETY: rt は akl_new 由来の有効ポインタ。err は [u8; 256] で n <= 255。
    unsafe {
        let err = std::ptr::addr_of_mut!((*rt).err).cast::<u8>();
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), err, n);
        err.add(n).write(0);
        std::ptr::addr_of_mut!((*rt).err_len).write(n);
        std::ptr::addr_of_mut!((*rt).native_err).write(true);
    }
}

// ---- チューニング / モジュール（API 互換。Rust 側は近似のため一部 no-op） ----

/// `akl_set_insn_budget`: 1 回の eval で許す命令数（0 なら即枯渇。1 以上を渡すこと）。
#[no_mangle]
pub unsafe extern "C" fn akl_set_insn_budget(rt: *mut AklRT, budget: u64) {
    if rt.is_null() {
        return;
    }
    // SAFETY: rt は akl_new 由来。
    unsafe {
        (*rt)._insn_budget = budget;
        (*rt).rt.insn_budget = budget;
    }
}

/// `akl_set_cojit`（no-op。Rust 実装に CoJIT 相当は無い）。
#[no_mangle]
pub unsafe extern "C" fn akl_set_cojit(_rt: *mut AklRT, _enabled: c_int) {}

/// `akl_cojit_count`（常に 0）。
#[no_mangle]
pub unsafe extern "C" fn akl_cojit_count(_rt: *mut AklRT) -> u32 {
    0
}

/// `akl_set_module_loader`（保持のみ。Rust 側 import は簡易近似でローダ未使用）。
#[no_mangle]
pub unsafe extern "C" fn akl_set_module_loader(
    rt: *mut AklRT,
    loader: CAklModuleLoader,
    udata: *mut c_void,
) {
    if rt.is_null() {
        return;
    }
    // SAFETY: rt は akl_new 由来。
    unsafe {
        (*rt)._mod_loader = Some(loader);
        (*rt)._mod_udata = udata;
    }
}

/// `akl_set_module_base`（保持のみ）。
#[no_mangle]
pub unsafe extern "C" fn akl_set_module_base(rt: *mut AklRT, base: *const c_char) {
    if rt.is_null() {
        return;
    }
    // SAFETY: rt は akl_new 由来。base は NUL 終端。
    let rt = unsafe { &mut *rt };
    let base = unsafe { cstr(base) };
    rt._mod_base = base.as_bytes().to_vec();
}

/// `akl_eval_module`: モジュールとして評価（Rust 側は import/export をグローバル近似
/// しているため `akl_eval` と同一挙動）。
#[no_mangle]
pub unsafe extern "C" fn akl_eval_module(
    rt: *mut AklRT,
    src: *const c_char,
    _base: *const c_char,
    out: *mut CAklVal,
) -> bool {
    unsafe { akl_eval(rt, src, out) }
}

/// `akl_tune`（保持のみ。Rust VM はヒープ上限を定数で扱うため no-op）。
#[no_mangle]
pub unsafe extern "C" fn akl_tune(_rt: *mut AklRT, _insn: u64, _heap_mb: u32, _max_objs: u32) {}

// ---- テスト ----

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;

    /// NUL 終端 CString を借用して eval するヘルパ。
    fn eval(rt: *mut AklRT, src: &str) -> (bool, CAklVal) {
        let c = CString::new(src).unwrap();
        let mut out: CAklVal = 0;
        let ok = unsafe { akl_eval(rt, c.as_ptr(), &mut out) };
        (ok, out)
    }

    #[test]
    fn eval_and_values() {
        let rt = unsafe { akl_new() };
        assert!(!rt.is_null());

        // 数値・真偽値
        let (ok, v) = eval(rt, "1 + 2;");
        assert!(ok);
        assert_eq!(v, AklVal::mk_int(3).bits());

        let (ok, v) = eval(rt, "true && false;");
        assert!(ok);
        assert_eq!(v, AklVal::FALSE.bits());

        // 文字列 as_str で確認（global に置いて読む）
        let (_, _) = eval(rt, "var s = \"abc\";");
        let mut len: u32 = 0;
        let p = unsafe {
            let sv = akl_global_get_test(rt, "s");
            akl_as_str(rt, sv, &mut len)
        };
        assert!(!p.is_null());
        assert_eq!(len, 3);
        let bytes = unsafe { std::slice::from_raw_parts(p as *const u8, len as usize) };
        assert_eq!(bytes, b"abc");

        // is_string / as_num / as_bool
        let sv = unsafe { akl_global_get_test(rt, "s") };
        assert!(unsafe { akl_is_string(rt, sv) });
        let mut d = 0.0;
        assert!(unsafe { akl_as_num(AklVal::mk_int(5).bits(), &mut d) });
        assert_eq!(d, 5.0);
        let mut b = false;
        assert!(unsafe { akl_as_bool(AklVal::TRUE.bits(), &mut b) });
        assert!(b);
        assert!(unsafe { akl_is_null(AklVal::NULL.bits()) });
        assert!(unsafe { akl_is_undefined(AklVal::UNDEF.bits()) });

        unsafe { akl_free(rt) };
    }

    /// テスト用: グローバル取得（FFI には akl_global_get が無いので intern 経由）。
    unsafe fn akl_global_get_test(rt: *mut AklRT, name: &str) -> CAklVal {
        let rt = unsafe { &mut *rt };
        let id = rt.rt.intern(name).unwrap();
        rt.rt
            .global_get(id)
            .map(|v| v.bits())
            .unwrap_or(AklVal::UNDEF.bits())
    }

    #[test]
    fn error_reporting() {
        let rt = unsafe { akl_new() };
        let c = CString::new("this is { not valid").unwrap();
        let mut out: CAklVal = 0;
        let ok = unsafe { akl_eval(rt, c.as_ptr(), &mut out) };
        assert!(!ok);
        let err = unsafe { std::ffi::CStr::from_ptr(akl_error(rt)) }
            .to_string_lossy()
            .into_owned();
        assert!(!err.is_empty());
        unsafe { akl_free(rt) };
    }

    #[test]
    fn budget_kills_infinite_loop() {
        let rt = unsafe { akl_new() };
        // 小さいバジェットで while(1){} を打ち切る（ハングしない）
        unsafe { akl_set_insn_budget(rt, 1000) };
        let c = CString::new("while (1) {}").unwrap();
        let mut out: CAklVal = 0;
        let ok = unsafe { akl_eval(rt, c.as_ptr(), &mut out) };
        assert!(!ok);
        let err = unsafe { std::ffi::CStr::from_ptr(akl_error(rt)) }
            .to_string_lossy()
            .into_owned();
        assert!(err.contains("budget"), "err = {err}");
        unsafe { akl_free(rt) };
    }

    #[test]
    fn global_set_and_prop() {
        let rt = unsafe { akl_new() };
        // グローバルに数値を束縛して読み出す
        let name = CString::new("x").unwrap();
        assert!(unsafe { akl_global_set(rt, name.as_ptr(), AklVal::mk_int(42).bits()) });
        let (ok, v) = eval(rt, "x;");
        assert!(ok);
        assert_eq!(v, AklVal::mk_int(42).bits());

        // プレーンオブジェクト + prop_set/get
        let obj = unsafe { akl_mkobject(rt) };
        let pname = CString::new("k").unwrap();
        assert!(unsafe { akl_prop_set(rt, obj, pname.as_ptr(), AklVal::mk_int(7).bits()) });
        let got = unsafe { akl_prop_get(rt, obj, pname.as_ptr()) };
        assert_eq!(got, AklVal::mk_int(7).bits());

        // tostring（数値 → 文字列）
        let ts = unsafe { akl_tostring(rt, AklVal::mk_int(42).bits()) };
        let mut len: u32 = 0;
        let p = unsafe { akl_as_str(rt, ts, &mut len) };
        assert!(!p.is_null());
        let bytes = unsafe { std::slice::from_raw_parts(p as *const u8, len as usize) };
        assert_eq!(bytes, b"42");

        unsafe { akl_free(rt) };
    }

    // ---- C ネイティブ登録（foreign_adapter）と ハンドル（DOM 相当）の e2e ----

    /// C ネイティブ: double(x) = x * 2。
    unsafe extern "C" fn double_native(
        _rt: *mut AklRT,
        _self: CAklVal,
        argc: c_int,
        argv: *const CAklVal,
        _udata: *mut c_void,
    ) -> CAklVal {
        if argc < 1 {
            return AklVal::UNDEF.bits();
        }
        // SAFETY: argv は argc 個の有効値。
        let args = unsafe { val_slice(argv, argc) };
        let v = AklVal::from_bits(args[0]);
        let d = if v.is_int() {
            v.get_int() as f64 * 2.0
        } else {
            v.as_f64().unwrap_or(f64::NAN) * 2.0
        };
        AklVal::from_f64(d).bits()
    }

    #[test]
    fn native_register_and_call() {
        let rt = unsafe { akl_new() };
        let name = CString::new("double").unwrap();
        assert!(unsafe {
            akl_native_register(rt, name.as_ptr(), double_native, std::ptr::null_mut())
        });

        // double(21) = 42（double 値を返す。=== で数値統一比較）
        let (ok, v) = eval(rt, "double(21) === 42;");
        assert!(ok);
        assert_eq!(v, AklVal::TRUE.bits());

        // ホストからの再入呼び出し（akl_call で JS 関数を呼ぶ）
        let (ok, _) = eval(rt, "function add(a, b) { return a + b; }");
        assert!(ok);
        let add = unsafe { akl_global_get_test(rt, "add") };
        let argv = [AklVal::mk_int(3).bits(), AklVal::mk_int(4).bits()];
        let mut out: CAklVal = 0;
        assert!(unsafe { akl_call(rt, add, 2, argv.as_ptr(), &mut out) });
        assert_eq!(out, AklVal::mk_int(7).bits());

        unsafe { akl_free(rt) };
    }

    #[test]
    fn handle_roundtrip() {
        // ハンドルの最小 vtable（get のみ。tag あり）
        // get コールバック: "title" なら 999 を返す
        unsafe extern "C" fn t_get(
            _rt: *mut AklRT,
            _ptr: *mut c_void,
            name: *const c_char,
            len: u32,
            out: *mut CAklVal,
        ) -> bool {
            // SAFETY: name は len バイトの有効バッファ（C vtable 契約）。
            let bytes = unsafe { std::slice::from_raw_parts(name as *const u8, len as usize) };
            let name = std::str::from_utf8(bytes).unwrap_or("");
            if name == "title" {
                // SAFETY: out は有効ポインタ。
                unsafe { *out = AklVal::mk_int(999).bits() };
                return true;
            }
            false
        }
        let vt = Box::leak(Box::new(CAklHandleVTab {
            tag: c"TestElem".as_ptr(),
            get: Some(t_get),
            set: None,
            call: None,
        }));

        let rt = unsafe { akl_new() };
        // ハンドルをグローバル document に束縛
        let handle =
            unsafe { akl_mkhandle(rt, vt as *const CAklHandleVTab, 0xdead as *mut c_void) };
        assert!(unsafe { akl_is_handle(rt, handle) });
        let doc = CString::new("document").unwrap();
        assert!(unsafe { akl_global_set(rt, doc.as_ptr(), handle) });

        // document.title → 999（get コールバック経由）
        let (ok, v) = eval(rt, "document.title;");
        assert!(ok);
        assert_eq!(v, AklVal::mk_int(999).bits());

        unsafe { akl_free(rt) };
    }
}
