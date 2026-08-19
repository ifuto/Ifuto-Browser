//! 組み込み（フェーズ 5）。C 実装 `src/akl/akl.c` の `akl_builtins_install` /
//! `AKL_STR_METHODS` / `AKL_ARR_METHODS` / Math / console を移植する。
//!
//! C 実装からの移植対応:
//! | C (akl.c) | Rust |
//! |---|---|
//! | `akl_builtins_install` | [`install_builtins`] |
//! | `AKL_STR_METHODS`（24 メソッド） | [`string_methods`]（部分） |
//! | `AKL_ARR_METHODS`（24 メソッド） | [`array_methods`]（部分） |
//! | `console.log` | [`console_log`] |
//! | `Math.*` | [`math_*`] |
//! | `parseInt` / `parseFloat` / `isNaN` / `Number` / `String` / `JSON.stringify` | 同左 |
//!
//! # 既知の近似（今後のフェーズ）
//!
//! - 正規表現系（`match`/`replace`/`search`/`split` with regex）は未対応
//! - `Date` は未対応
//! - `JSON.parse` は未対応（`JSON.stringify` のみ）

#![forbid(unsafe_code)]
#![warn(missing_docs)]

use crate::bytecode::{Runtime, VmError};
use crate::obj::{Obj, ObjId};
use crate::AklVal;

/// 全組み込みをランタイムに登録する（C の `akl_builtins_install` 相当）。
pub fn install_builtins(rt: &mut Runtime) -> Result<(), VmError> {
    // `length` プロパティ名を設定（文字列/配列の length 用）
    rt.length_id = rt.intern("length").ok_or(VmError::Oom)?;

    // console.log
    let console_id = rt.intern("console").ok_or(VmError::Oom)?;
    let console = rt.heap.alloc(Obj::Obj(Vec::new())).map_err(|_| VmError::Oom)?;
    let log = rt.register_native(console_log)?;
    let log_name = rt.intern("log").ok_or(VmError::Oom)?;
    rt.heap.prop_set(console, log_name, log).map_err(|_| VmError::Oom)?;
    rt.global_set(console_id, AklVal::mk_obj(console));

    // Math オブジェクト
    let math_id = rt.intern("Math").ok_or(VmError::Oom)?;
    let math = rt.heap.alloc(Obj::Obj(Vec::new())).map_err(|_| VmError::Oom)?;
    for (name, f) in [
        ("abs", math_abs as crate::bytecode::NativeFn),
        ("floor", math_floor),
        ("ceil", math_ceil),
        ("round", math_round),
        ("sqrt", math_sqrt),
        ("pow", math_pow),
        ("max", math_max),
        ("min", math_min),
        ("random", math_random),
        ("trunc", math_trunc),
    ] {
        let v = rt.register_native(f)?;
        let nid = rt.intern(name).ok_or(VmError::Oom)?;
        rt.heap.prop_set(math, nid, v).map_err(|_| VmError::Oom)?;
    }
    rt.global_set(math_id, AklVal::mk_obj(math));

    // グローバル関数
    rt.register_global_native("parseInt", parse_int)?;
    rt.register_global_native("parseFloat", parse_float)?;
    rt.register_global_native("isNaN", is_nan)?;
    rt.register_global_native("Number", number_ctor)?;
    // String はコンストラクタオブジェクト（constructor + prototype）として登録
    // （lodash が `String.prototype.replace` 等を使うため）。install_string_methods で
    // prototype を構築する。
    {
        let s_id = rt.intern("String").ok_or(VmError::Oom)?;
        let s_obj = rt.heap.alloc(Obj::Obj(Vec::new())).map_err(|_| VmError::Oom)?;
        let ctor = rt.register_native(string_ctor)?;
        rt.heap.prop_set(s_obj, rt.ctor_name, ctor).map_err(|_| VmError::Oom)?;
        rt.global_set(s_id, AklVal::mk_obj(s_obj));
    }

    // globalThis ハンドル + 基本コンストラクタ（lodash の root 検出・環境判定用）
    install_global_this_and_ctors(rt)?;

    // String.prototype メソッド（プレーンオブジェクトに native を載せる）
    install_string_methods(rt)?;
    // Array.prototype メソッド
    install_array_methods(rt)?;
    // Object 静的メソッド（keys/values/assign）
    install_object_methods(rt)?;
    // JSON.stringify / JSON.parse
    install_json(rt)?;
    // Map / Set / Promise
    install_map_set(rt)?;
    // Function.prototype.call / apply
    install_func_methods(rt)?;
    // ジェネレータメソッド（next）
    install_gen_methods(rt)?;
    // 正規表現メソッド（exec / test）
    install_regex_methods(rt)?;
    // Date（同期近似）
    install_date(rt)?;

    Ok(())
}

/// 文字列値の内容を取り出す（文字列でなければ空）。
fn str_of(rt: &Runtime, v: AklVal) -> String {
    if v.is_obj() {
        if let Some(Obj::Str(s)) = rt.heap.get(v.get_obj()) {
            return s.to_string();
        }
    }
    String::new()
}

/// 値を JS 文字列化（C の akl_to_string 簡易版）。
fn to_js_string(rt: &mut Runtime, v: AklVal) -> String {
    if v.is_int() {
        v.get_int().to_string()
    } else if let Some(d) = v.as_f64() {
        crate::bytecode::fmt_num_pub(d)
    } else if v.is_undef() {
        "undefined".into()
    } else if v.is_null() {
        "null".into()
    } else if v == AklVal::TRUE {
        "true".into()
    } else if v == AklVal::FALSE {
        "false".into()
    } else if v.is_obj() {
        str_of(rt, v)
    } else {
        "undefined".into()
    }
}

/// 値を数値化（C の akl_to_number 簡易版）。
fn to_number(rt: &Runtime, v: AklVal) -> f64 {
    if v.is_int() {
        v.get_int() as f64
    } else if let Some(d) = v.as_f64() {
        d
    } else if v.is_undef() {
        f64::NAN
    } else if v.is_null() {
        0.0
    } else if v == AklVal::TRUE {
        1.0
    } else if v == AklVal::FALSE {
        0.0
    } else if v.is_obj() {
        str_of(rt, v).parse::<f64>().unwrap_or(f64::NAN)
    } else {
        f64::NAN
    }
}

/// 値を truthy 判定（filter 用）。
fn truthy_builtin(rt: &Runtime, v: AklVal) -> bool {
    if v.is_undef() || v.is_null() || v == AklVal::FALSE {
        return false;
    }
    if v == AklVal::TRUE {
        return true;
    }
    if v.is_int() {
        return v.get_int() != 0;
    }
    if let Some(d) = v.as_f64() {
        return d != 0.0 && !d.is_nan();
    }
    if v.is_obj() {
        return !matches!(rt.heap.get(v.get_obj()), Some(Obj::Str(s)) if s.is_empty());
    }
    true
}

/// console.log(...)。引数を空白区切りで console_out に追記。
fn console_log(rt: &mut Runtime, _this: AklVal, args: &[AklVal]) -> Result<AklVal, VmError> {
    let parts: Vec<String> = args.iter().map(|a| to_js_string(rt, *a)).collect();
    rt.console_out.push(parts.join(" "));
    Ok(AklVal::UNDEF)
}

/// 1 引数の数値 native ヘルパ。
fn unary_math(rt: &mut Runtime, args: &[AklVal], f: impl Fn(f64) -> f64) -> Result<AklVal, VmError> {
    let v = args.first().copied().unwrap_or(AklVal::UNDEF);
    let d = to_number(rt, v);
    Ok(AklVal::from_f64(f(d)))
}

fn math_abs(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    unary_math(rt, a, |d| d.abs())
}
fn math_floor(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    unary_math(rt, a, f64::floor)
}
fn math_ceil(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    unary_math(rt, a, f64::ceil)
}
fn math_round(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    unary_math(rt, a, f64::round)
}
fn math_sqrt(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    unary_math(rt, a, f64::sqrt)
}
fn math_trunc(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    unary_math(rt, a, f64::trunc)
}
fn math_pow(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let x = to_number(rt, a.first().copied().unwrap_or(AklVal::UNDEF));
    let y = to_number(rt, a.get(1).copied().unwrap_or(AklVal::UNDEF));
    Ok(AklVal::from_f64(x.powf(y)))
}
fn math_max(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let mut m = f64::NEG_INFINITY;
    for v in a {
        let d = to_number(rt, *v);
        if d.is_nan() {
            return Ok(AklVal::from_f64(f64::NAN));
        }
        if d > m {
            m = d;
        }
    }
    Ok(AklVal::from_f64(m))
}
fn math_min(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let mut m = f64::INFINITY;
    for v in a {
        let d = to_number(rt, *v);
        if d.is_nan() {
            return Ok(AklVal::from_f64(f64::NAN));
        }
        if d < m {
            m = d;
        }
    }
    Ok(AklVal::from_f64(m))
}
fn math_random(_rt: &mut Runtime, _t: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    // 決定的（テスト用）: 疑似乱数。C の Math.random は clock_gettime ベース。
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.subsec_nanos())
        .unwrap_or(0);
    let x = (nanos % 1000) as f64 / 1000.0;
    Ok(AklVal::from_f64(x))
}

fn parse_int(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = to_js_string(rt, a.first().copied().unwrap_or(AklVal::UNDEF));
    let s = s.trim();
    // 先頭の符号
    let (neg, rest) = if let Some(r) = s.strip_prefix('-') {
        (true, r)
    } else if let Some(r) = s.strip_prefix('+') {
        (false, r)
    } else {
        (false, s)
    };
    let _ = neg;
    let radix = a
        .get(1)
        .map(|r| to_number(rt, *r) as u32)
        .unwrap_or(10);
    let digits: String = rest.chars().take_while(|c| c.is_digit(radix)).collect();
    if digits.is_empty() {
        return Ok(AklVal::from_f64(f64::NAN));
    }
    let v = u64::from_str_radix(&digits, radix).unwrap_or(0) as f64;
    Ok(AklVal::from_f64(if neg { -v } else { v }))
}

fn parse_float(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = to_js_string(rt, a.first().copied().unwrap_or(AklVal::UNDEF));
    let s = s.trim();
    // 先頭から float をパース
    let end = s
        .char_indices()
        .find_map(|(_i, c)| {
            let is_float_char =
                c.is_ascii_digit() || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+';
            if is_float_char {
                None
            } else {
                Some(_i)
            }
        })
        .unwrap_or(s.len());
    let head = &s[..end];
    let v = head.parse::<f64>().unwrap_or(f64::NAN);
    Ok(AklVal::from_f64(v))
}

fn is_nan(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let d = to_number(rt, a.first().copied().unwrap_or(AklVal::UNDEF));
    Ok(AklVal::from_bool(d.is_nan()))
}

fn number_ctor(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let v = a.first().copied().unwrap_or(AklVal::UNDEF);
    Ok(AklVal::from_f64(to_number(rt, v)))
}

fn string_ctor(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let v = a.first().copied().unwrap_or(AklVal::UNDEF);
    let s = to_js_string(rt, v);
    let id = rt.intern(&s).ok_or(VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

/// `Function('return this')()` が返す native（`this` が undefined なら globalThis）。
fn fn_return_this(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    if !this.is_undef() {
        return Ok(this);
    }
    let name = rt.intern("globalThis").ok_or(VmError::Oom)?;
    Ok(rt.global_get(name).unwrap_or(AklVal::UNDEF))
}

/// `Function(body)`：'return this' のみ対応（lodash の root 検出）。C 実装と同型。
fn function_ctor(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let body = a.last().copied().unwrap_or(AklVal::UNDEF);
    if rt.flatten_str(body).trim() == "return this" {
        return rt.register_native(fn_return_this);
    }
    let msg = rt
        .intern("TypeError: Function constructor is not supported")
        .unwrap_or(0);
    Err(VmError::Thrown(AklVal::mk_obj(msg)))
}

/// `Array(...)` コンストラクタ（`Array(n)` / `Array.isArray` 用）。
fn array_ctor(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    // Array(len) → 空配列。Array(v) → [v]。簡易近似。
    let items: Vec<AklVal> = if a.len() == 1 && !a[0].is_obj() {
        Vec::new()
    } else {
        a.to_vec()
    };
    let id = rt.heap.alloc(Obj::Arr(items)).map_err(|_| VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

/// `Array.isArray(x)` 用の静的メソッド。
fn array_is_array(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let v = a.first().copied().unwrap_or(AklVal::UNDEF);
    let is = v.is_obj() && matches!(rt.heap.get(v.get_obj()), Some(Obj::Arr(_)));
    Ok(AklVal::from_bool(is))
}

/// `Boolean(v)` コンストラクタ。
fn boolean_ctor(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let v = a.first().copied().unwrap_or(AklVal::UNDEF);
    Ok(AklVal::from_bool(truthy_builtin(rt, v)))
}

/// `RegExp(pattern[, flags])` コンストラクタ。
fn regexp_ctor(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let pattern = rt.flatten_str(a.first().copied().unwrap_or(AklVal::UNDEF));
    let flags = rt.flatten_str(a.get(1).copied().unwrap_or(AklVal::UNDEF));
    let id = rt
        .heap
        .alloc(Obj::RegExp { pattern: pattern.into_boxed_str(), flags: flags.into_boxed_str() })
        .map_err(|_| VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

/// `Symbol([desc])` コンストラクタ（一意値を返す簡易近似。well-known symbol は未対応）。
fn symbol_ctor(rt: &mut Runtime, _t: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    // 一意な文字列で近似（`typeof` が "symbol" にはならないが、lodash は `typeof Symbol`
    // を環境判定に使うだけなので関数であれば十分）。
    let id = rt.heap.alloc(Obj::Obj(Vec::new())).map_err(|_| VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

/// `isFinite(v)`。
fn is_finite(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let d = to_number(rt, a.first().copied().unwrap_or(AklVal::UNDEF));
    Ok(AklVal::from_bool(d.is_finite()))
}

/// `Function.prototype.toString.call(fn)` → 関数ソース近似（lodash の native 検出用）。
fn function_to_string(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = if this.is_obj()
        && matches!(rt.heap.get(this.get_obj()), Some(Obj::Func { .. }) | Some(Obj::Native(_)))
    {
        "function () { [native code] }"
    } else {
        "function () {}"
    };
    let id = rt.intern(s).ok_or(VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

/// `Function.prototype.bind(thisArg, ...)`：束縛関数を返す簡易近似（thisArg を無視し、
/// 元関数をそのまま返す。lodash は bind を直接は使わないが API を揃える）。
fn function_bind(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    let _ = rt;
    Ok(this)
}

/// globalThis ハンドル + `Function` / `Array` / `Boolean` コンストラクタを登録。
fn install_global_this_and_ctors(rt: &mut Runtime) -> Result<(), VmError> {
    // globalThis（プロパティアクセスをグローバル解決に写像するハンドル）
    let gt = rt
        .heap
        .alloc(Obj::Handle { vtab: &crate::bytecode::GLOBAL_THIS_VT, data: 0, ptr: 0 })
        .map_err(|_| VmError::Oom)?;
    let gt_name = rt.intern("globalThis").ok_or(VmError::Oom)?;
    rt.global_set(gt_name, AklVal::mk_obj(gt));

    // Function: constructor + prototype（call/apply/bind/toString）。lodash の
    // `funcProto = Function.prototype; funcProto.toString.call(...)` に必要。
    let fn_id = rt.intern("Function").ok_or(VmError::Oom)?;
    let fn_obj = rt.heap.alloc(Obj::Obj(Vec::new())).map_err(|_| VmError::Oom)?;
    let ctor = rt.register_native(function_ctor)?;
    rt.heap.prop_set(fn_obj, rt.ctor_name, ctor).map_err(|_| VmError::Oom)?;
    let fn_proto = rt.heap.alloc(Obj::Obj(Vec::new())).map_err(|_| VmError::Oom)?;
    for (name, f) in [
        ("toString", function_to_string as crate::bytecode::NativeFn),
        ("call", func_call),
        ("apply", func_apply),
        ("bind", function_bind),
    ] {
        let v = rt.register_native(f)?;
        let nid = rt.intern(name).ok_or(VmError::Oom)?;
        rt.heap.prop_set(fn_proto, nid, v).map_err(|_| VmError::Oom)?;
    }
    let proto_name = rt.intern("prototype").ok_or(VmError::Oom)?;
    rt.heap.prop_set(fn_obj, proto_name, AklVal::mk_obj(fn_proto)).map_err(|_| VmError::Oom)?;
    rt.global_set(fn_id, AklVal::mk_obj(fn_obj));

    // Boolean
    rt.register_global_native("Boolean", boolean_ctor)?;
    // RegExp / Symbol / isFinite（lodash の環境判定・正規表現生成用）
    rt.register_global_native("RegExp", regexp_ctor)?;
    rt.register_global_native("Symbol", symbol_ctor)?;
    rt.register_global_native("isFinite", is_finite)?;

    // Array（constructor + isArray）
    let arr_id = rt.intern("Array").ok_or(VmError::Oom)?;
    let arr = rt.heap.alloc(Obj::Obj(Vec::new())).map_err(|_| VmError::Oom)?;
    let ctor = rt.register_native(array_ctor)?;
    rt.heap
        .prop_set(arr, rt.ctor_name, ctor)
        .map_err(|_| VmError::Oom)?;
    let is_arr = rt.register_native(array_is_array)?;
    let is_arr_name = rt.intern("isArray").ok_or(VmError::Oom)?;
    rt.heap
        .prop_set(arr, is_arr_name, is_arr)
        .map_err(|_| VmError::Oom)?;
    rt.global_set(arr_id, AklVal::mk_obj(arr));

    Ok(())
}

/// String.prototype メソッドを登録（C の `str_meth_vals` 相当。文字列リテラルの
/// メソッド解決用に `rt.str_methods` へ登録）。`length` はプロパティなので除外。
fn install_string_methods(rt: &mut Runtime) -> Result<(), VmError> {
    let mut prototype = Vec::new();
    for (name, f) in [
        ("toUpperCase", str_to_upper as crate::bytecode::NativeFn),
        ("toLowerCase", str_to_lower),
        ("trim", str_trim),
        ("indexOf", str_index_of),
        ("slice", str_slice),
        ("includes", str_includes),
        ("startsWith", str_starts_with),
        ("endsWith", str_ends_with),
        ("repeat", str_repeat),
        ("match", str_match),
        ("replace", str_replace),
        ("search", str_search),
        ("split", str_split),
        ("padStart", str_pad_start),
        ("padEnd", str_pad_end),
    ] {
        let v = rt.register_native(f)?;
        let nid = rt.intern(name).ok_or(VmError::Oom)?;
        rt.str_methods.push((nid, v));
        prototype.push((nid, v));
    }
    // String.prototype を構築して String コンストラクタに設定
    let s_id = rt.intern("String").ok_or(VmError::Oom)?;
    if let Some(sv) = rt.global_get(s_id) {
        if sv.is_obj() {
            attach_prototype(rt, sv.get_obj(), &prototype)?;
        }
    }
    Ok(())
}

/// `this` を文字列として取得するヘルパ（String メソッド用）。
fn this_str(rt: &Runtime, this: AklVal) -> String {
    str_of(rt, this)
}

fn str_to_upper(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = this_str(rt, this).to_uppercase();
    let id = rt.intern(&s).ok_or(VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}
fn str_to_lower(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = this_str(rt, this).to_lowercase();
    let id = rt.intern(&s).ok_or(VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}
fn str_trim(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = this_str(rt, this).trim().to_string();
    let id = rt.intern(&s).ok_or(VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}
fn str_index_of(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = this_str(rt, this);
    let needle = to_js_string(rt, a.first().copied().unwrap_or(AklVal::UNDEF));
    match s.find(&needle) {
        Some(i) => Ok(AklVal::mk_int(s[..i].chars().count() as i32)),
        None => Ok(AklVal::mk_int(-1)),
    }
}
fn str_slice(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = this_str(rt, this);
    let chars: Vec<char> = s.chars().collect();
    let start = a.first().map(|v| to_number(rt, *v) as i64).unwrap_or(0);
    let end = a.get(1).map(|v| to_number(rt, *v) as i64);
    let n = chars.len() as i64;
    let start = if start < 0 { (n + start).max(0) } else { start.min(n) } as usize;
    let end = end.map(|e| if e < 0 { (n + e).max(0) } else { e.min(n) } as usize).unwrap_or(chars.len());
    let out: String = chars[start..end.max(start)].iter().collect();
    let id = rt.intern(&out).ok_or(VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}
fn str_includes(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = this_str(rt, this);
    let needle = to_js_string(rt, a.first().copied().unwrap_or(AklVal::UNDEF));
    Ok(AklVal::from_bool(s.contains(&needle)))
}
fn str_starts_with(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = this_str(rt, this);
    let needle = to_js_string(rt, a.first().copied().unwrap_or(AklVal::UNDEF));
    Ok(AklVal::from_bool(s.starts_with(&needle)))
}
fn str_ends_with(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = this_str(rt, this);
    let needle = to_js_string(rt, a.first().copied().unwrap_or(AklVal::UNDEF));
    Ok(AklVal::from_bool(s.ends_with(&needle)))
}
fn str_repeat(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = this_str(rt, this);
    let n = a.first().map(|v| to_number(rt, *v) as i64).unwrap_or(0).max(0) as usize;
    let out = s.repeat(n);
    let id = rt.intern(&out).ok_or(VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

/// `padStart` / `padEnd` の共通実装。`from_end` が true なら padEnd。
fn str_pad(rt: &mut Runtime, this: AklVal, a: &[AklVal], from_end: bool) -> Result<AklVal, VmError> {
    let s = this_str(rt, this);
    let target = a.first().map(|v| to_number(rt, *v) as i64).unwrap_or(0).max(0) as usize;
    let pad = a.get(1).map(|v| to_js_string(rt, *v)).unwrap_or_else(|| " ".to_string());
    let chars: Vec<char> = s.chars().collect();
    if chars.len() >= target {
        let id = rt.intern(&s).ok_or(VmError::Oom)?;
        return Ok(AklVal::mk_obj(id));
    }
    let need = target - chars.len();
    let pad_chars: Vec<char> = pad.chars().collect();
    let mut out = String::new();
    if from_end {
        out.extend(&chars);
        for i in 0..need {
            if pad_chars.is_empty() {
                out.push(' ');
            } else {
                out.push(pad_chars[i % pad_chars.len()]);
            }
        }
    } else {
        for i in 0..need {
            if pad_chars.is_empty() {
                out.push(' ');
            } else {
                out.push(pad_chars[i % pad_chars.len()]);
            }
        }
        out.extend(&chars);
    }
    let id = rt.intern(&out).ok_or(VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

fn str_pad_start(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    str_pad(rt, this, a, false)
}

fn str_pad_end(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    str_pad(rt, this, a, true)
}

/// 正規表現オブジェクトを取得（引数が Obj::RegExp ならその pattern/flags）。
fn regex_of(rt: &Runtime, v: AklVal) -> Option<(String, String)> {
    if v.is_obj() {
        if let Some(Obj::RegExp { pattern, flags }) = rt.heap.get(v.get_obj()) {
            return Some((pattern.to_string(), flags.to_string()));
        }
    }
    None
}

fn str_match(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = this_str(rt, this);
    let rx_v = a.first().copied().unwrap_or(AklVal::UNDEF);
    if let Some((pattern, flags)) = regex_of(rt, rx_v) {
        let flag_num = flags_to_num(&flags);
        let rx = crate::regex::Regex::compile(&pattern, flag_num).map_err(|e| {
        let msg = rt.intern(&format!("SyntaxError: invalid regexp: {e}")).unwrap_or(0);
        VmError::Thrown(AklVal::mk_obj(msg))
    })?;
        if flags.contains('g') {
            // g フラグ: 全マッチの配列
            let matches = rx.find_all(&s);
            let items: Vec<AklVal> = matches
                .iter()
                .map(|m| rt.intern(m).map(AklVal::mk_obj).unwrap_or(AklVal::UNDEF))
                .collect();
            let id = rt.heap.alloc(Obj::Arr(items)).map_err(|_| VmError::Oom)?;
            Ok(AklVal::mk_obj(id))
        } else {
            match rx.find(&s) {
                Some(caps) => {
                    // マッチ全体 + 捕捉グループの配列
                    let items: Vec<AklVal> = caps
                        .iter()
                        .map(|c| rt.intern(c).map(AklVal::mk_obj).unwrap_or(AklVal::UNDEF))
                        .collect();
                    let id = rt.heap.alloc(Obj::Arr(items)).map_err(|_| VmError::Oom)?;
                    Ok(AklVal::mk_obj(id))
                }
                None => Ok(AklVal::NULL),
            }
        }
    } else {
        Ok(AklVal::NULL)
    }
}

/// `repl` が callable（Func/Native）か。
fn is_callable(rt: &Runtime, v: AklVal) -> bool {
    v.is_obj()
        && matches!(
            rt.heap.get(v.get_obj()),
            Some(Obj::Func { .. })
                | Some(Obj::Native(_))
                | Some(Obj::ForeignNative { .. })
                | Some(Obj::BoundMethod { .. })
        )
}

fn str_replace(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = this_str(rt, this);
    let rx_v = a.first().copied().unwrap_or(AklVal::UNDEF);
    let repl_v = a.get(1).copied().unwrap_or(AklVal::UNDEF);
    let callable = is_callable(rt, repl_v);
    let out = if let Some((pattern, flags)) = regex_of(rt, rx_v) {
        let flag_num = flags_to_num(&flags);
        let rx = crate::regex::Regex::compile(&pattern, flag_num).map_err(|e| {
        let msg = rt.intern(&format!("SyntaxError: invalid regexp: {e}")).unwrap_or(0);
        VmError::Thrown(AklVal::mk_obj(msg))
    })?;
        let global = flags.contains('g');
        if callable {
            replace_regex_fn(rt, &s, &rx, repl_v, global)?
        } else {
            let repl = to_js_string(rt, repl_v);
            if global {
                crate::regex::replace_all(&s, &rx, &repl)
            } else {
                crate::regex::replace_first(&s, &rx, &repl)
            }
        }
    } else {
        let needle = to_js_string(rt, rx_v);
        if callable {
            // 文字列 needle + 関数: 最初の 1 箇所を cb(needle) で置換
            match s.find(&needle) {
                Some(start) => {
                    let mut result = String::new();
                    result.push_str(&s[..start]);
                    let arg = rt.intern(&needle).ok_or(VmError::Oom)?;
                    let r = call_native(rt, repl_v, AklVal::UNDEF, &[AklVal::mk_obj(arg)])?;
                    result.push_str(&to_js_string(rt, r));
                    result.push_str(&s[start + needle.len()..]);
                    result
                }
                None => s.clone(),
            }
        } else {
            let repl = to_js_string(rt, repl_v);
            s.replacen(&needle, &repl, 1)
        }
    };
    let id = rt.intern(&out).ok_or(VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

/// RegExp + 関数コールバックの置換（`s.replace(/re/g, cb)`）。cb は (match, ...caps) を
/// 受け、戻り値を置換文字列とする。`g` が無ければ最初の 1 箇所のみ。
fn replace_regex_fn(
    rt: &mut Runtime,
    s: &str,
    rx: &crate::regex::Regex,
    cb: AklVal,
    global: bool,
) -> Result<String, VmError> {
    let mut result = String::new();
    let mut rest = s;
    let mut guard = 0;
    loop {
        if guard > 10000 {
            break;
        }
        guard += 1;
        match rx.find(rest) {
            Some(caps) => {
                let full = caps[0].clone();
                if full.is_empty() {
                    result.push_str(rest);
                    break;
                }
                let start = rest.find(&full).unwrap_or(0);
                result.push_str(&rest[..start]);
                // コールバック引数: マッチ全体 + 捕捉グループ
                let mut args = Vec::new();
                for c in &caps {
                    let arg = rt.intern(c).ok_or(VmError::Oom)?;
                    args.push(AklVal::mk_obj(arg));
                }
                let r = call_native(rt, cb, AklVal::UNDEF, &args)?;
                result.push_str(&to_js_string(rt, r));
                rest = &rest[start + full.len()..];
                if !global {
                    result.push_str(rest);
                    break;
                }
            }
            None => {
                result.push_str(rest);
                break;
            }
        }
    }
    Ok(result)
}

/// `RegExp.prototype.exec`：マッチ全体 + 捕捉グループの配列（無ければ null）。
fn regex_exec(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = to_js_string(rt, a.first().copied().unwrap_or(AklVal::UNDEF));
    let Some((pattern, flags)) = regex_of(rt, this) else {
        return Ok(AklVal::NULL);
    };
    let flag_num = flags_to_num(&flags);
    let rx = crate::regex::Regex::compile(&pattern, flag_num).map_err(|e| {
        let msg = rt.intern(&format!("SyntaxError: invalid regexp: {e}")).unwrap_or(0);
        VmError::Thrown(AklVal::mk_obj(msg))
    })?;
    match rx.find(&s) {
        Some(caps) => {
            let items: Vec<AklVal> = caps
                .iter()
                .map(|c| rt.intern(c).map(AklVal::mk_obj).unwrap_or(AklVal::UNDEF))
                .collect();
            let id = rt.heap.alloc(Obj::Arr(items)).map_err(|_| VmError::Oom)?;
            Ok(AklVal::mk_obj(id))
        }
        None => Ok(AklVal::NULL),
    }
}

/// `RegExp.prototype.test`：マッチすれば true。
fn regex_test(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = to_js_string(rt, a.first().copied().unwrap_or(AklVal::UNDEF));
    let Some((pattern, flags)) = regex_of(rt, this) else {
        return Ok(AklVal::FALSE);
    };
    let flag_num = flags_to_num(&flags);
    let rx = crate::regex::Regex::compile(&pattern, flag_num).map_err(|e| {
        let msg = rt.intern(&format!("SyntaxError: invalid regexp: {e}")).unwrap_or(0);
        VmError::Thrown(AklVal::mk_obj(msg))
    })?;
    Ok(AklVal::from_bool(rx.find(&s).is_some()))
}

fn str_search(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = this_str(rt, this);
    let rx_v = a.first().copied().unwrap_or(AklVal::UNDEF);
    if let Some((pattern, flags)) = regex_of(rt, rx_v) {
        let flag_num = flags_to_num(&flags);
        let rx = crate::regex::Regex::compile(&pattern, flag_num).map_err(|e| {
        let msg = rt.intern(&format!("SyntaxError: invalid regexp: {e}")).unwrap_or(0);
        VmError::Thrown(AklVal::mk_obj(msg))
    })?;
        match rx.find(&s) {
            Some(caps) => {
                let idx = s.find(&caps[0]).unwrap_or(0);
                Ok(AklVal::mk_int(idx as i32))
            }
            None => Ok(AklVal::mk_int(-1)),
        }
    } else {
        Ok(AklVal::mk_int(-1))
    }
}

fn str_split(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = this_str(rt, this);
    let sep = a.first().copied().unwrap_or(AklVal::UNDEF);
    let parts: Vec<String> = if let Some((pattern, flags)) = regex_of(rt, sep) {
        let flag_num = flags_to_num(&flags);
        let rx = crate::regex::Regex::compile(&pattern, flag_num).map_err(|e| {
        let msg = rt.intern(&format!("SyntaxError: invalid regexp: {e}")).unwrap_or(0);
        VmError::Thrown(AklVal::mk_obj(msg))
    })?;
        crate::regex::split(&s, &rx)
    } else {
        let needle = to_js_string(rt, sep);
        if needle.is_empty() {
            s.chars().map(|c| c.to_string()).collect()
        } else {
            s.split(&needle).map(|p| p.to_string()).collect()
        }
    };
    let items: Vec<AklVal> = parts
        .iter()
        .map(|p| rt.intern(p).map(AklVal::mk_obj).unwrap_or(AklVal::UNDEF))
        .collect();
    let id = rt.heap.alloc(Obj::Arr(items)).map_err(|_| VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

/// フラグ文字列 → 数値（i=1）。
fn flags_to_num(flags: &str) -> u32 {
    let mut n = 0u32;
    if flags.contains('i') {
        n |= 1;
    }
    n
}

/// Array.prototype メソッドを登録（C の `arr_meth_vals` 相当）。`length` はプロパティなので除外。
fn install_array_methods(rt: &mut Runtime) -> Result<(), VmError> {
    let mut prototype = Vec::new();
    for (name, f) in [
        ("push", arr_push as crate::bytecode::NativeFn),
        ("pop", arr_pop),
        ("shift", arr_shift),
        ("unshift", arr_unshift),
        ("join", arr_join),
        ("concat", arr_concat),
        ("indexOf", arr_index_of),
        ("includes", arr_includes),
        ("reverse", arr_reverse),
        ("slice", arr_slice),
        ("map", arr_map),
        ("filter", arr_filter),
        ("forEach", arr_for_each),
        ("reduce", arr_reduce),
        ("find", arr_find),
        ("findIndex", arr_find_index),
        ("some", arr_some),
        ("every", arr_every),
        ("sort", arr_sort),
        ("splice", arr_splice),
        ("flat", arr_flat),
        ("at", arr_at),
    ] {
        let v = rt.register_native(f)?;
        let nid = rt.intern(name).ok_or(VmError::Oom)?;
        rt.arr_methods.push((nid, v));
        prototype.push((nid, v));
    }
    // Array.prototype を構築して Array コンストラクタに設定
    let a_id = rt.intern("Array").ok_or(VmError::Oom)?;
    if let Some(av) = rt.global_get(a_id) {
        if av.is_obj() {
            attach_prototype(rt, av.get_obj(), &prototype)?;
        }
    }
    Ok(())
}

/// メソッド表（name → native）から prototype オブジェクトを構築し、コンストラクタ
/// オブジェクト `ctor` の `prototype` プロパティに設定する（`Array.prototype.slice`
/// 等の `.prototype.X` 参照用。インスタンスメソッド解決はメソッド表経由で別に行う）。
fn attach_prototype(rt: &mut Runtime, ctor: ObjId, methods: &[(ObjId, AklVal)]) -> Result<(), VmError> {
    let proto = rt.heap.alloc(Obj::Obj(Vec::new())).map_err(|_| VmError::Oom)?;
    for (n, v) in methods {
        rt.heap.prop_set(proto, *n, *v).map_err(|_| VmError::Oom)?;
    }
    let proto_name = rt.intern("prototype").ok_or(VmError::Oom)?;
    rt.heap.prop_set(ctor, proto_name, AklVal::mk_obj(proto)).map_err(|_| VmError::Oom)?;
    Ok(())
}

fn this_arr(rt: &Runtime, this: AklVal) -> Vec<AklVal> {
    if this.is_obj() {
        if let Some(Obj::Arr(items)) = rt.heap.get(this.get_obj()) {
            return items.clone();
        }
    }
    Vec::new()
}

fn arr_push(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let id = if this.is_obj() { this.get_obj() } else { return Ok(AklVal::mk_int(0)) };
    if let Some(Obj::Arr(v)) = rt.heap.get_mut(id) {
        v.extend_from_slice(a);
        let n = v.len();
        Ok(AklVal::mk_int(n as i32))
    } else {
        Ok(AklVal::mk_int(0))
    }
}
fn arr_pop(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    let id = if this.is_obj() { this.get_obj() } else { return Ok(AklVal::UNDEF) };
    match rt.heap.get_mut(id) {
        Some(Obj::Arr(v)) => Ok(v.pop().unwrap_or(AklVal::UNDEF)),
        _ => Ok(AklVal::UNDEF),
    }
}
fn arr_join(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let items = this_arr(rt, this);
    let sep = a.first().map(|v| str_of(rt, *v)).unwrap_or_default();
    let parts: Vec<String> = items.iter().map(|v| to_js_string(rt, *v)).collect();
    let s = parts.join(&sep);
    let id = rt.intern(&s).ok_or(VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}
fn arr_index_of(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let items = this_arr(rt, this);
    let needle = a.first().copied().unwrap_or(AklVal::UNDEF);
    for (i, v) in items.iter().enumerate() {
        if *v == needle {
            return Ok(AklVal::mk_int(i as i32));
        }
    }
    Ok(AklVal::mk_int(-1))
}
fn arr_slice(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let items = this_arr(rt, this);
    let n = items.len() as i64;
    let start = a.first().map(|v| to_number(rt, *v) as i64).unwrap_or(0);
    let start = if start < 0 { (n + start).max(0) } else { start.min(n) } as usize;
    let end = a.get(1).map(|v| to_number(rt, *v) as i64).map(|e| if e < 0 { (n + e).max(0) } else { e.min(n) } as usize).unwrap_or(items.len());
    let out: Vec<AklVal> = items[start..end.max(start)].to_vec();
    let id = rt.heap.alloc(Obj::Arr(out)).map_err(|_| VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}
fn arr_map(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let items = this_arr(rt, this);
    let cb = a.first().copied().unwrap_or(AklVal::UNDEF);
    let mut out = Vec::new();
    for (i, v) in items.iter().enumerate() {
        // コールバックを呼ぶ: cb(v, i)
        let args = vec![*v, AklVal::mk_int(i as i32)];
        let r = call_native(rt, cb, AklVal::UNDEF, &args)?;
        out.push(r);
    }
    let id = rt.heap.alloc(Obj::Arr(out)).map_err(|_| VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

fn arr_filter(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let items = this_arr(rt, this);
    let cb = a.first().copied().unwrap_or(AklVal::UNDEF);
    let mut out = Vec::new();
    for (i, v) in items.iter().enumerate() {
        let args = vec![*v, AklVal::mk_int(i as i32)];
        let r = call_native(rt, cb, AklVal::UNDEF, &args)?;
        if truthy_builtin(rt, r) {
            out.push(*v);
        }
    }
    let id = rt.heap.alloc(Obj::Arr(out)).map_err(|_| VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

fn arr_for_each(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let items = this_arr(rt, this);
    let cb = a.first().copied().unwrap_or(AklVal::UNDEF);
    for (i, v) in items.iter().enumerate() {
        let args = vec![*v, AklVal::mk_int(i as i32)];
        call_native(rt, cb, AklVal::UNDEF, &args)?;
    }
    Ok(AklVal::UNDEF)
}

fn arr_reduce(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let items = this_arr(rt, this);
    let cb = a.first().copied().unwrap_or(AklVal::UNDEF);
    let mut acc = a.get(1).copied().unwrap_or(AklVal::UNDEF);
    let mut start = 0;
    if a.len() < 2 {
        // 初期値なし: 先頭要素を初期値に
        if items.is_empty() {
            return Ok(AklVal::UNDEF);
        }
        acc = items[0];
        start = 1;
    }
    for (i, v) in items.iter().enumerate().skip(start) {
        let args = vec![acc, *v, AklVal::mk_int(i as i32)];
        acc = call_native(rt, cb, AklVal::UNDEF, &args)?;
    }
    Ok(acc)
}

fn arr_shift(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    let id = if this.is_obj() { this.get_obj() } else { return Ok(AklVal::UNDEF) };
    match rt.heap.get_mut(id) {
        Some(Obj::Arr(v)) => {
            if v.is_empty() {
                Ok(AklVal::UNDEF)
            } else {
                Ok(v.remove(0))
            }
        }
        _ => Ok(AklVal::UNDEF),
    }
}

fn arr_unshift(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let id = if this.is_obj() { this.get_obj() } else { return Ok(AklVal::mk_int(0)) };
    match rt.heap.get_mut(id) {
        Some(Obj::Arr(v)) => {
            let mut new_items = a.to_vec();
            new_items.append(v);
            *v = new_items;
            Ok(AklVal::mk_int(v.len() as i32))
        }
        _ => Ok(AklVal::mk_int(0)),
    }
}

fn arr_concat(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let mut out = this_arr(rt, this);
    for v in a {
        if v.is_obj() {
            if let Some(Obj::Arr(items)) = rt.heap.get(v.get_obj()) {
                out.extend_from_slice(items);
                continue;
            }
        }
        out.push(*v);
    }
    let id = rt.heap.alloc(Obj::Arr(out)).map_err(|_| VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

fn arr_includes(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let items = this_arr(rt, this);
    let needle = a.first().copied().unwrap_or(AklVal::UNDEF);
    Ok(AklVal::from_bool(items.contains(&needle)))
}

fn arr_reverse(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    let id = if this.is_obj() { this.get_obj() } else { return Ok(AklVal::UNDEF) };
    match rt.heap.get_mut(id) {
        Some(Obj::Arr(v)) => {
            v.reverse();
            Ok(this)
        }
        _ => Ok(AklVal::UNDEF),
    }
}

fn arr_find(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let items = this_arr(rt, this);
    let cb = a.first().copied().unwrap_or(AklVal::UNDEF);
    for (i, v) in items.iter().enumerate() {
        let args = vec![*v, AklVal::mk_int(i as i32)];
        let r = call_native(rt, cb, AklVal::UNDEF, &args)?;
        if truthy_builtin(rt, r) {
            return Ok(*v);
        }
    }
    Ok(AklVal::UNDEF)
}

fn arr_find_index(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let items = this_arr(rt, this);
    let cb = a.first().copied().unwrap_or(AklVal::UNDEF);
    for (i, v) in items.iter().enumerate() {
        let args = vec![*v, AklVal::mk_int(i as i32)];
        let r = call_native(rt, cb, AklVal::UNDEF, &args)?;
        if truthy_builtin(rt, r) {
            return Ok(AklVal::mk_int(i as i32));
        }
    }
    Ok(AklVal::mk_int(-1))
}

fn arr_some(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let items = this_arr(rt, this);
    let cb = a.first().copied().unwrap_or(AklVal::UNDEF);
    for (i, v) in items.iter().enumerate() {
        let args = vec![*v, AklVal::mk_int(i as i32)];
        let r = call_native(rt, cb, AklVal::UNDEF, &args)?;
        if truthy_builtin(rt, r) {
            return Ok(AklVal::TRUE);
        }
    }
    Ok(AklVal::FALSE)
}

fn arr_every(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let items = this_arr(rt, this);
    let cb = a.first().copied().unwrap_or(AklVal::UNDEF);
    for (i, v) in items.iter().enumerate() {
        let args = vec![*v, AklVal::mk_int(i as i32)];
        let r = call_native(rt, cb, AklVal::UNDEF, &args)?;
        if !truthy_builtin(rt, r) {
            return Ok(AklVal::FALSE);
        }
    }
    Ok(AklVal::TRUE)
}

fn arr_sort(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let id = if this.is_obj() { this.get_obj() } else { return Ok(AklVal::UNDEF) };
    let mut items = this_arr(rt, this);
    let cmp = a.first().copied().unwrap_or(AklVal::UNDEF);
    let has_cmp = cmp.is_obj()
        && matches!(
            rt.heap.get(cmp.get_obj()),
            Some(Obj::Native(_)) | Some(Obj::Func { .. })
        );
    if has_cmp {
        // 比較関数付き: 挿入ソート（借用回避のため比較関数呼び出しをループ内で行う）。
        for i in 1..items.len() {
            let mut j = i;
            while j > 0 {
                let args = vec![items[j], items[j - 1]];
                let r = call_native(rt, cmp, AklVal::UNDEF, &args)?;
                if to_number(rt, r) < 0.0 {
                    items.swap(j, j - 1);
                    j -= 1;
                } else {
                    break;
                }
            }
        }
    } else {
        // デフォルトは文字列化比較（JS の sort 既定）。借用回避のため文字列キーを先に作る。
        let mut keyed: Vec<(String, AklVal)> = items
            .iter()
            .map(|v| (to_js_string(rt, *v), *v))
            .collect();
        keyed.sort_by(|x, y| x.0.cmp(&y.0));
        items = keyed.into_iter().map(|(_, v)| v).collect();
    }
    if let Some(Obj::Arr(v)) = rt.heap.get_mut(id) {
        *v = items;
    }
    Ok(this)
}

fn arr_splice(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let id = if this.is_obj() { this.get_obj() } else { return Ok(AklVal::UNDEF) };
    let mut items = this_arr(rt, this);
    let n = items.len() as i64;
    let start = a.first().map(|v| to_number(rt, *v) as i64).unwrap_or(0);
    let start = if start < 0 { (n + start).max(0) } else { start.min(n) } as usize;
    let delete_count = a
        .get(1)
        .map(|v| to_number(rt, *v) as i64)
        .unwrap_or(n - start as i64)
        .max(0) as usize;
    let removed: Vec<AklVal> = items.splice(start..(start + delete_count).min(items.len()), a[2..].to_vec()).collect();
    if let Some(Obj::Arr(v)) = rt.heap.get_mut(id) {
        *v = items;
    }
    let rid = rt.heap.alloc(Obj::Arr(removed)).map_err(|_| VmError::Oom)?;
    Ok(AklVal::mk_obj(rid))
}

fn arr_flat(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let items = this_arr(rt, this);
    let depth = a.first().map(|v| to_number(rt, *v) as i64).unwrap_or(1).max(0) as usize;
    let mut out = Vec::new();
    fn flatten(rt: &Runtime, items: &[AklVal], depth: usize, out: &mut Vec<AklVal>) {
        for v in items {
            if depth > 0 && v.is_obj() {
                if let Some(Obj::Arr(inner)) = rt.heap.get(v.get_obj()) {
                    flatten(rt, inner, depth - 1, out);
                    continue;
                }
            }
            out.push(*v);
        }
    }
    flatten(rt, &items, depth, &mut out);
    let id = rt.heap.alloc(Obj::Arr(out)).map_err(|_| VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

fn arr_at(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let items = this_arr(rt, this);
    let n = items.len() as i64;
    let idx = a.first().map(|v| to_number(rt, *v) as i64).unwrap_or(0);
    let idx = if idx < 0 { n + idx } else { idx };
    if idx < 0 || idx >= n {
        Ok(AklVal::UNDEF)
    } else {
        Ok(items[idx as usize])
    }
}

/// 正規表現メソッド（exec / test）を登録（`rt.regex_methods` 経由で `PLoad` が解決）。
fn install_regex_methods(rt: &mut Runtime) -> Result<(), VmError> {
    for (name, f) in [
        ("exec", regex_exec as crate::bytecode::NativeFn),
        ("test", regex_test),
    ] {
        let v = rt.register_native(f)?;
        let nid = rt.intern(name).ok_or(VmError::Oom)?;
        rt.regex_methods.push((nid, v));
    }
    Ok(())
}

/// Map / Set / Promise を登録。
fn install_map_set(rt: &mut Runtime) -> Result<(), VmError> {
    // Map コンストラクタ
    rt.register_global_native("Map", map_ctor)?;
    for (name, f) in [
        ("set", map_set as crate::bytecode::NativeFn),
        ("get", map_get),
        ("has", map_has),
    ] {
        let v = rt.register_native(f)?;
        let nid = rt.intern(name).ok_or(VmError::Oom)?;
        rt.map_methods.push((nid, v));
    }
    // Set コンストラクタ
    rt.register_global_native("Set", set_ctor)?;
    for (name, f) in [
        ("add", set_add as crate::bytecode::NativeFn),
        ("has", set_has),
    ] {
        let v = rt.register_native(f)?;
        let nid = rt.intern(name).ok_or(VmError::Oom)?;
        rt.set_methods.push((nid, v));
    }
    // Promise: グローバルは `constructor` + `resolve` を持つプレーンオブジェクト
    // （`new Promise(...)` と `Promise.resolve(...)` の両方に対応）。インスタンス
    // メソッド（then/catch）は `rt.promise_methods` 経由。
    let promise_id = rt.intern("Promise").ok_or(VmError::Oom)?;
    let promise = rt.heap.alloc(Obj::Obj(Vec::new())).map_err(|_| VmError::Oom)?;
    let ctor = rt.register_native(promise_ctor)?;
    rt.heap
        .prop_set(promise, rt.ctor_name, ctor)
        .map_err(|_| VmError::Oom)?;
    let resolve = rt.register_native(promise_resolve)?;
    let resolve_name = rt.intern("resolve").ok_or(VmError::Oom)?;
    rt.heap
        .prop_set(promise, resolve_name, resolve)
        .map_err(|_| VmError::Oom)?;
    rt.global_set(promise_id, AklVal::mk_obj(promise));
    // インスタンスメソッド then / catch
    for (name, f) in [
        ("then", promise_then as crate::bytecode::NativeFn),
        ("catch", promise_catch),
    ] {
        let v = rt.register_native(f)?;
        let nid = rt.intern(name).ok_or(VmError::Oom)?;
        rt.promise_methods.push((nid, v));
    }
    Ok(())
}

/// `Function.prototype.call` / `apply` を登録（lodash 等が `fn.call(thisArg, ...)` を使う）。
fn install_func_methods(rt: &mut Runtime) -> Result<(), VmError> {
    for (name, f) in [
        ("call", func_call as crate::bytecode::NativeFn),
        ("apply", func_apply),
    ] {
        let v = rt.register_native(f)?;
        let nid = rt.intern(name).ok_or(VmError::Oom)?;
        rt.func_methods.push((nid, v));
    }
    Ok(())
}

/// `fn.call(thisArg, ...args)`：`this`（＝呼ばれる関数）を thisArg で呼ぶ。
fn func_call(rt: &mut Runtime, this: AklVal, args: &[AklVal]) -> Result<AklVal, VmError> {
    let this_arg = args.first().copied().unwrap_or(AklVal::UNDEF);
    rt.call_value(this, this_arg, &args[1..])
}

/// `fn.apply(thisArg, [args])`：`this`（＝呼ばれる関数）を thisArg で呼ぶ。
fn func_apply(rt: &mut Runtime, this: AklVal, args: &[AklVal]) -> Result<AklVal, VmError> {
    let this_arg = args.first().copied().unwrap_or(AklVal::UNDEF);
    let arg_arr = args.get(1).copied().unwrap_or(AklVal::UNDEF);
    let rest: Vec<AklVal> = if arg_arr.is_obj() {
        match rt.heap.get(arg_arr.get_obj()) {
            Some(Obj::Arr(items)) => items.clone(),
            _ => Vec::new(),
        }
    } else {
        Vec::new()
    };
    rt.call_value(this, this_arg, &rest)
}

/// ジェネレータメソッド（`next`）を登録。
fn install_gen_methods(rt: &mut Runtime) -> Result<(), VmError> {
    let v = rt.register_native(gen_next)?;
    let nid = rt.intern("next").ok_or(VmError::Oom)?;
    rt.gen_methods.push((nid, v));
    Ok(())
}

/// ジェネレータを 1 段進める（`gen.next()`）。`{ value, done }` を返す。
fn gen_next(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    if this.is_obj() {
        return rt.gen_resume(this.get_obj());
    }
    Ok(AklVal::UNDEF)
}

fn map_ctor(rt: &mut Runtime, _t: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    let id = rt.heap.alloc(Obj::Map(Vec::new())).map_err(|_| VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}
fn set_ctor(rt: &mut Runtime, _t: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    let id = rt.heap.alloc(Obj::Set(Vec::new())).map_err(|_| VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}
fn promise_ctor(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    // `new Promise(executor)`。executor があれば resolve コールバック付きで同期的に
    // 呼ぶ近似（マイクロタスクは実行しない。then コールバックは後で消化されない）。
    let id = rt
        .heap
        .alloc(Obj::Promise { state: 0, value: AklVal::UNDEF })
        .map_err(|_| VmError::Oom)?;
    let exec = a.first().copied().unwrap_or(AklVal::UNDEF);
    if is_callable(rt, exec) {
        let resolve = rt.register_native(promise_resolve)?;
        let _ = call_native(rt, exec, AklVal::UNDEF, &[resolve]);
    }
    Ok(AklVal::mk_obj(id))
}

/// `Promise.resolve(value)` → 解決済み Promise。
fn promise_resolve(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let v = a.first().copied().unwrap_or(AklVal::UNDEF);
    let id = rt
        .heap
        .alloc(Obj::Promise { state: 1, value: v })
        .map_err(|_| VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

/// `promise.then(onFulfilled)`。マイクロタスク近似: コールバックは登録するが実行
/// しない（スクリプト本体内で読む値は 0 のまま = V8 準拠のオラクルと一致）。
fn promise_then(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    let _ = rt;
    Ok(this)
}

/// `promise.catch(onRejected)`。then と同様に no-op 近似。
fn promise_catch(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    let _ = rt;
    Ok(this)
}

fn map_set(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let id = if this.is_obj() { this.get_obj() } else { return Ok(AklVal::UNDEF) };
    let key = a.first().copied().unwrap_or(AklVal::UNDEF);
    let val = a.get(1).copied().unwrap_or(AklVal::UNDEF);
    if let Some(Obj::Map(kv)) = rt.heap.get_mut(id) {
        if let Some(slot) = kv.iter_mut().find(|(k, _)| *k == key) {
            slot.1 = val;
        } else {
            kv.push((key, val));
        }
        Ok(this)
    } else {
        Ok(AklVal::UNDEF)
    }
}
fn map_get(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let id = if this.is_obj() { this.get_obj() } else { return Ok(AklVal::UNDEF) };
    let key = a.first().copied().unwrap_or(AklVal::UNDEF);
    if let Some(Obj::Map(kv)) = rt.heap.get(id) {
        Ok(kv.iter().find(|(k, _)| *k == key).map(|(_, v)| *v).unwrap_or(AklVal::UNDEF))
    } else {
        Ok(AklVal::UNDEF)
    }
}
fn map_has(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let id = if this.is_obj() { this.get_obj() } else { return Ok(AklVal::FALSE) };
    let key = a.first().copied().unwrap_or(AklVal::UNDEF);
    if let Some(Obj::Map(kv)) = rt.heap.get(id) {
        Ok(AklVal::from_bool(kv.iter().any(|(k, _)| *k == key)))
    } else {
        Ok(AklVal::FALSE)
    }
}
fn set_add(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let id = if this.is_obj() { this.get_obj() } else { return Ok(AklVal::UNDEF) };
    let val = a.first().copied().unwrap_or(AklVal::UNDEF);
    if let Some(Obj::Set(items)) = rt.heap.get_mut(id) {
        if !items.contains(&val) {
            items.push(val);
        }
        Ok(this)
    } else {
        Ok(AklVal::UNDEF)
    }
}
fn set_has(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let id = if this.is_obj() { this.get_obj() } else { return Ok(AklVal::FALSE) };
    let val = a.first().copied().unwrap_or(AklVal::UNDEF);
    if let Some(Obj::Set(items)) = rt.heap.get(id) {
        Ok(AklVal::from_bool(items.contains(&val)))
    } else {
        Ok(AklVal::FALSE)
    }
}

/// Object 静的メソッド（keys/values/assign）+ Object.prototype を登録。
fn install_object_methods(rt: &mut Runtime) -> Result<(), VmError> {
    let obj_id = rt.intern("Object").ok_or(VmError::Oom)?;
    let obj = rt.heap.alloc(Obj::Obj(Vec::new())).map_err(|_| VmError::Oom)?;
    for (name, f) in [
        ("keys", obj_keys as crate::bytecode::NativeFn),
        ("values", obj_values),
        ("assign", obj_assign),
        ("entries", obj_entries),
        ("fromEntries", obj_from_entries),
    ] {
        let v = rt.register_native(f)?;
        let nid = rt.intern(name).ok_or(VmError::Oom)?;
        rt.heap.prop_set(obj, nid, v).map_err(|_| VmError::Oom)?;
    }
    // Object 静的メソッド（lodash の環境判定・baseCreate 用）
    for (name, f) in [
        ("create", obj_create as crate::bytecode::NativeFn),
        ("getPrototypeOf", obj_get_prototype_of),
        ("getOwnPropertySymbols", obj_get_own_property_symbols),
        ("getOwnPropertyNames", obj_keys),
        ("defineProperty", obj_define_property),
        ("setPrototypeOf", obj_set_prototype_of),
    ] {
        let v = rt.register_native(f)?;
        let nid = rt.intern(name).ok_or(VmError::Oom)?;
        rt.heap.prop_set(obj, nid, v).map_err(|_| VmError::Oom)?;
    }
    // Object.prototype（toString / hasOwnProperty / propertyIsEnumerable。
    // lodash の getTag / hasOwnProperty.call 用）
    let proto = rt.heap.alloc(Obj::Obj(Vec::new())).map_err(|_| VmError::Oom)?;
    for (name, f) in [
        ("toString", object_proto_to_string as crate::bytecode::NativeFn),
        ("hasOwnProperty", object_proto_has_own_property),
        ("propertyIsEnumerable", object_proto_property_is_enumerable),
    ] {
        let v = rt.register_native(f)?;
        let nid = rt.intern(name).ok_or(VmError::Oom)?;
        rt.heap.prop_set(proto, nid, v).map_err(|_| VmError::Oom)?;
    }
    let proto_name = rt.intern("prototype").ok_or(VmError::Oom)?;
    rt.heap
        .prop_set(obj, proto_name, AklVal::mk_obj(proto))
        .map_err(|_| VmError::Oom)?;
    rt.global_set(obj_id, AklVal::mk_obj(obj));
    Ok(())
}

/// 値の `Object.prototype.toString` タグ（`[object X]` の X）。
fn object_tag(rt: &Runtime, v: AklVal) -> &'static str {
    if v.is_int() || !v.is_tagged() {
        return "Number";
    }
    if v.is_undef() {
        return "Undefined";
    }
    if v.is_null() {
        return "Null";
    }
    if v == AklVal::TRUE || v == AklVal::FALSE {
        return "Boolean";
    }
    if v.is_obj() {
        return match rt.heap.get(v.get_obj()) {
            Some(Obj::Str(_)) | Some(Obj::Rope { .. }) => "String",
            Some(Obj::Arr(_)) => "Array",
            Some(Obj::Func { .. })
            | Some(Obj::Native(_))
            | Some(Obj::ForeignNative { .. })
            | Some(Obj::BoundMethod { .. }) => "Function",
            Some(Obj::Map(_)) => "Map",
            Some(Obj::Set(_)) => "Set",
            Some(Obj::Promise { .. }) => "Promise",
            Some(Obj::RegExp { .. }) => "RegExp",
            Some(Obj::Date { .. }) => "Date",
            Some(Obj::BigInt(_)) => "BigInt",
            Some(Obj::Handle { vtab, .. }) => vtab.tag,
            _ => "Object",
        };
    }
    "Object"
}

/// `Object.prototype.toString.call(v)` → `[object X]`。
fn object_proto_to_string(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    let tag = object_tag(rt, this);
    let id = rt.intern(&format!("[object {tag}]")).ok_or(VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

/// `Object.create(proto)` → 新オブジェクト（`[[Prototype]]` = proto）。
fn obj_create(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let proto = a.first().copied().unwrap_or(AklVal::NULL);
    let id = rt.heap.alloc(Obj::Obj(Vec::new())).map_err(|_| VmError::Oom)?;
    if proto.is_obj() {
        rt.obj_set_proto(id, proto.get_obj())?;
    }
    Ok(AklVal::mk_obj(id))
}

/// `Object.getPrototypeOf(obj)` → `[[Prototype]]`（無ければ null）。
fn obj_get_prototype_of(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let obj = a.first().copied().unwrap_or(AklVal::UNDEF);
    if obj.is_obj() {
        if let Some(Obj::Obj(props)) = rt.heap.get(obj.get_obj()) {
            if let Some((_, v)) = props.iter().find(|(n, _)| *n == rt.proto_name) {
                if v.is_obj() {
                    return Ok(*v);
                }
            }
        }
    }
    Ok(AklVal::NULL)
}

/// `Object.getOwnPropertySymbols(obj)` → 空配列（Symbol 未実装の近似）。
fn obj_get_own_property_symbols(rt: &mut Runtime, _t: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    let id = rt.heap.alloc(Obj::Arr(Vec::new())).map_err(|_| VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

/// `Object.defineProperty(obj, key, desc)` → プロパティ設定（value 記述子のみ）。
fn obj_define_property(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let obj = a.first().copied().unwrap_or(AklVal::UNDEF);
    let key = rt.flatten_str(a.get(1).copied().unwrap_or(AklVal::UNDEF));
    let desc = a.get(2).copied().unwrap_or(AklVal::UNDEF);
    let vname = rt.intern("value").ok_or(VmError::Oom)?;
    let val = if desc.is_obj() {
        if let Some(Obj::Obj(props)) = rt.heap.get(desc.get_obj()) {
            props.iter().find(|(n, _)| *n == vname).map(|(_, v)| *v).unwrap_or(AklVal::UNDEF)
        } else {
            AklVal::UNDEF
        }
    } else {
        AklVal::UNDEF
    };
    if obj.is_obj() {
        let key_id = rt.intern(&key).ok_or(VmError::Oom)?;
        rt.heap.prop_set(obj.get_obj(), key_id, val).map_err(|_| VmError::Oom)?;
    }
    Ok(obj)
}

/// `Object.setPrototypeOf(obj, proto)`。
fn obj_set_prototype_of(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let obj = a.first().copied().unwrap_or(AklVal::UNDEF);
    let proto = a.get(1).copied().unwrap_or(AklVal::UNDEF);
    if obj.is_obj() && proto.is_obj() {
        rt.obj_set_proto(obj.get_obj(), proto.get_obj())?;
    }
    Ok(obj)
}

/// `Object.prototype.propertyIsEnumerable.call(obj, key)` → own プロパティなら true。
fn object_proto_property_is_enumerable(
    rt: &mut Runtime,
    this: AklVal,
    a: &[AklVal],
) -> Result<AklVal, VmError> {
    let key = rt.flatten_str(a.first().copied().unwrap_or(AklVal::UNDEF));
    let key_id = rt.intern(&key).ok_or(VmError::Oom)?;
    let has = this.is_obj()
        && matches!(rt.heap.get(this.get_obj()), Some(Obj::Obj(props)) if props.iter().any(|(n, _)| *n == key_id && *n != rt.proto_name));
    Ok(AklVal::from_bool(has))
}

/// `Object.prototype.hasOwnProperty.call(obj, key)`。
fn object_proto_has_own_property(
    rt: &mut Runtime,
    this: AklVal,
    a: &[AklVal],
) -> Result<AklVal, VmError> {
    let key = rt.flatten_str(a.first().copied().unwrap_or(AklVal::UNDEF));
    let key_id = rt.intern(&key).ok_or(VmError::Oom)?;
    let has = this.is_obj()
        && matches!(rt.heap.get(this.get_obj()), Some(Obj::Obj(props)) if props.iter().any(|(n, _)| *n == key_id));
    Ok(AklVal::from_bool(has))
}

/// プレーンオブジェクトのプロパティ列を取得。
fn obj_props(rt: &Runtime, v: AklVal) -> Vec<(AklVal, AklVal)> {
    if v.is_obj() {
        if let Some(Obj::Obj(props)) = rt.heap.get(v.get_obj()) {
            return props
                .iter()
                .map(|(n, val)| (AklVal::mk_obj(*n), *val))
                .collect();
        }
    }
    Vec::new()
}

fn obj_keys(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let v = a.first().copied().unwrap_or(AklVal::UNDEF);
    let keys: Vec<AklVal> = obj_props(rt, v).into_iter().map(|(k, _)| k).collect();
    let id = rt.heap.alloc(Obj::Arr(keys)).map_err(|_| VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

fn obj_values(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let v = a.first().copied().unwrap_or(AklVal::UNDEF);
    let vals: Vec<AklVal> = obj_props(rt, v).into_iter().map(|(_, val)| val).collect();
    let id = rt.heap.alloc(Obj::Arr(vals)).map_err(|_| VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

fn obj_assign(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let target = a.first().copied().unwrap_or(AklVal::UNDEF);
    for src in a.iter().skip(1) {
        for (k, val) in obj_props(rt, *src) {
            if k.is_obj() && target.is_obj() {
                let _ = rt.heap.prop_set(target.get_obj(), k.get_obj(), val);
            }
        }
    }
    Ok(target)
}

/// `Object.entries(obj)` → `[[key, value], ...]` の配列。
fn obj_entries(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let v = a.first().copied().unwrap_or(AklVal::UNDEF);
    let mut out = Vec::new();
    for (k, val) in obj_props(rt, v) {
        let pair = vec![k, val];
        let pid = rt.heap.alloc(Obj::Arr(pair)).map_err(|_| VmError::Oom)?;
        out.push(AklVal::mk_obj(pid));
    }
    let id = rt.heap.alloc(Obj::Arr(out)).map_err(|_| VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

/// `Object.fromEntries([[key, value], ...])` → オブジェクト。
fn obj_from_entries(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let v = a.first().copied().unwrap_or(AklVal::UNDEF);
    let obj_id = rt.heap.alloc(Obj::Obj(Vec::new())).map_err(|_| VmError::Oom)?;
    // 借用回避: エントリ列を値コピーしてから再構築
    let entries: Vec<AklVal> = if v.is_obj() {
        match rt.heap.get(v.get_obj()) {
            Some(Obj::Arr(e)) => e.clone(),
            _ => Vec::new(),
        }
    } else {
        Vec::new()
    };
    for entry in entries {
        let pair: Vec<AklVal> = if entry.is_obj() {
            match rt.heap.get(entry.get_obj()) {
                Some(Obj::Arr(p)) => p.clone(),
                _ => Vec::new(),
            }
        } else {
            Vec::new()
        };
        if pair.is_empty() {
            continue;
        }
        let k = pair.first().copied().unwrap_or(AklVal::UNDEF);
        let val = pair.get(1).copied().unwrap_or(AklVal::UNDEF);
        // キーを文字列化して intern
        let ks = to_js_string(rt, k);
        let kid = rt.intern(&ks).ok_or(VmError::Oom)?;
        rt.heap.prop_set(obj_id, kid, val).map_err(|_| VmError::Oom)?;
    }
    Ok(AklVal::mk_obj(obj_id))
}

/// JSON.stringify / JSON.parse を登録（簡易版）。
fn install_json(rt: &mut Runtime) -> Result<(), VmError> {
    let json_id = rt.intern("JSON").ok_or(VmError::Oom)?;
    let json = rt.heap.alloc(Obj::Obj(Vec::new())).map_err(|_| VmError::Oom)?;
    let f = rt.register_native(json_stringify)?;
    let nid = rt.intern("stringify").ok_or(VmError::Oom)?;
    rt.heap.prop_set(json, nid, f).map_err(|_| VmError::Oom)?;
    let p = rt.register_native(json_parse)?;
    let pid = rt.intern("parse").ok_or(VmError::Oom)?;
    rt.heap.prop_set(json, pid, p).map_err(|_| VmError::Oom)?;
    rt.global_set(json_id, AklVal::mk_obj(json));
    Ok(())
}

/// JSON 文字列化（再帰。文字列・数値・真偽値・null・配列・プレーンオブジェクト）。
fn json_stringify(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let v = a.first().copied().unwrap_or(AklVal::UNDEF);
    let s = json_stringify_val(rt, v, 0)?;
    let id = rt.intern(&s).ok_or(VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

fn json_stringify_val(rt: &mut Runtime, v: AklVal, depth: u32) -> Result<String, VmError> {
    if depth > 64 {
        return Ok("\"[circular]\"".to_string());
    }
    if v.is_undef() {
        return Ok("undefined".to_string());
    }
    if v.is_null() {
        return Ok("null".to_string());
    }
    if v == AklVal::TRUE {
        return Ok("true".to_string());
    }
    if v == AklVal::FALSE {
        return Ok("false".to_string());
    }
    if v.is_int() {
        return Ok(v.get_int().to_string());
    }
    if let Some(d) = v.as_f64() {
        if d.is_nan() || d.is_infinite() {
            return Ok("null".to_string());
        }
        return Ok(crate::bytecode::fmt_num_pub(d));
    }
    if v.is_obj() {
        let id = v.get_obj();
        // 借用回避: データをクローンしてから再帰
        let cloned = match rt.heap.get(id) {
            Some(Obj::Str(s)) => Some(JsonRepr::Str(s.clone())),
            Some(Obj::Arr(items)) => Some(JsonRepr::Arr(items.clone())),
            Some(Obj::Obj(props)) => Some(JsonRepr::Obj(
                props
                    .iter()
                    .filter_map(|(n, val)| match rt.heap.get(*n) {
                        Some(Obj::Str(s)) => Some((s.clone(), *val)),
                        _ => None,
                    })
                    .collect(),
            )),
            _ => None,
        };
        return match cloned {
            Some(JsonRepr::Str(s)) => Ok(json_quote(&s)),
            Some(JsonRepr::Arr(items)) => {
                let parts: Vec<String> = items
                    .iter()
                    .map(|x| json_stringify_val(rt, *x, depth + 1))
                    .collect::<Result<_, _>>()?;
                Ok(format!("[{}]", parts.join(",")))
            }
            Some(JsonRepr::Obj(props)) => {
                let mut parts = Vec::new();
                for (s, val) in props {
                    let val_s = json_stringify_val(rt, val, depth + 1)?;
                    if val_s != "undefined" {
                        parts.push(format!("{}:{}", json_quote(&s), val_s));
                    }
                }
                Ok(format!("{{{}}}", parts.join(",")))
            }
            None => Ok("null".to_string()),
        };
    }
    Ok("undefined".to_string())
}

/// JSON 再帰のためのデータ表現（借用回避）。
enum JsonRepr {
    /// 文字列。
    Str(Box<str>),
    /// 配列。
    Arr(Vec<AklVal>),
    /// オブジェクト（キーは文字列）。
    Obj(Vec<(Box<str>, AklVal)>),
}

/// JSON 文字列のクォート（エスケープ）。
fn json_quote(s: &str) -> String {
    let mut out = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// `JSON.parse`（再帰下降。文字列 → AklVal）。
fn json_parse(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = str_of(rt, a.first().copied().unwrap_or(AklVal::UNDEF));
    let mut p = JsonParser {
        s: s.as_bytes(),
        pos: 0,
    };
    let v = p.value(rt, 0).map_err(|_| json_syntax_error(rt))?;
    p.ws();
    if p.pos != p.s.len() {
        return Err(json_syntax_error(rt));
    }
    Ok(v)
}

/// JSON 構文エラーを `Thrown` として返す。
fn json_syntax_error(rt: &mut Runtime) -> VmError {
    let msg = rt.intern("SyntaxError: invalid JSON").unwrap_or(0);
    VmError::Thrown(AklVal::mk_obj(msg))
}

/// JSON 再帰下降パーサ（バイト列をスキャンして AklVal を構築）。
struct JsonParser<'a> {
    /// 入力バイト列。
    s: &'a [u8],
    /// 現在位置。
    pos: usize,
}

impl<'a> JsonParser<'a> {
    /// 空白を読み飛ばす。
    fn ws(&mut self) {
        while self.pos < self.s.len() {
            let c = self.s[self.pos];
            if c == b' ' || c == b'\t' || c == b'\n' || c == b'\r' {
                self.pos += 1;
            } else {
                break;
            }
        }
    }

    /// リテラル（true/false/null）を照合。
    fn lit(&mut self, lit: &[u8]) -> bool {
        if self.pos + lit.len() > self.s.len() {
            return false;
        }
        if &self.s[self.pos..self.pos + lit.len()] == lit {
            self.pos += lit.len();
            true
        } else {
            false
        }
    }

    /// 4 桁の 16 進を読む。
    fn hex4(&mut self) -> Option<u32> {
        if self.pos + 4 > self.s.len() {
            return None;
        }
        let mut v = 0u32;
        for i in 0..4 {
            let c = self.s[self.pos + i];
            let d = match c {
                b'0'..=b'9' => (c - b'0') as u32,
                b'a'..=b'f' => (c - b'a' + 10) as u32,
                b'A'..=b'F' => (c - b'A' + 10) as u32,
                _ => return None,
            };
            v = v * 16 + d;
        }
        self.pos += 4;
        Some(v)
    }

    /// 文字列（エスケープ込み。UTF-16 サロゲートペア対応）。
    fn string(&mut self) -> Option<String> {
        if self.pos >= self.s.len() || self.s[self.pos] != b'"' {
            return None;
        }
        self.pos += 1;
        let mut out = String::new();
        while self.pos < self.s.len() {
            let c = self.s[self.pos];
            self.pos += 1;
            match c {
                b'"' => return Some(out),
                b'\\' => {
                    if self.pos >= self.s.len() {
                        return None;
                    }
                    let e = self.s[self.pos];
                    self.pos += 1;
                    match e {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\u{0008}'),
                        b'f' => out.push('\u{000c}'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            let mut cp = self.hex4()?;
                            // サロゲートペア
                            if (0xD800..=0xDBFF).contains(&cp)
                                && self.pos + 2 <= self.s.len()
                                && self.s[self.pos] == b'\\'
                                && self.s[self.pos + 1] == b'u'
                            {
                                let save = self.pos;
                                self.pos += 2;
                                if let Some(lo) = self.hex4() {
                                    if (0xDC00..=0xDFFF).contains(&lo) {
                                        cp = 0x10000
                                            + ((cp - 0xD800) << 10)
                                            + (lo - 0xDC00);
                                    } else {
                                        self.pos = save;
                                    }
                                } else {
                                    self.pos = save;
                                }
                            }
                            out.push(char::from_u32(cp)?);
                        }
                        _ => return None,
                    }
                }
                c if c < 0x20 => return None,
                c => {
                    // マルチバイト UTF-8 は生バイト列をそのまま複製
                    let seq = if c < 0x80 {
                        1
                    } else if (c & 0xE0) == 0xC0 {
                        2
                    } else if (c & 0xF0) == 0xE0 {
                        3
                    } else if (c & 0xF8) == 0xF0 {
                        4
                    } else {
                        1
                    };
                    let start = self.pos - 1;
                    if start + seq > self.s.len() {
                        return None;
                    }
                    let bytes = &self.s[start..start + seq];
                    out.push_str(std::str::from_utf8(bytes).ok()?);
                    self.pos += seq - 1;
                }
            }
        }
        None
    }

    /// 数値（先頭の `0` のみの整数は JSON では不正）。
    fn number(&mut self) -> Option<AklVal> {
        let st = self.pos;
        let mut is_int = true;
        if self.pos < self.s.len() && self.s[self.pos] == b'-' {
            self.pos += 1;
        }
        let intst = self.pos;
        let mut any = false;
        while self.pos < self.s.len() && self.s[self.pos].is_ascii_digit() {
            self.pos += 1;
            any = true;
        }
        if !any {
            self.pos = st;
            return None;
        }
        // 先頭 0 の複数桁は不正（-01 等）
        if self.pos - intst > 1 && self.s[intst] == b'0' {
            self.pos = st;
            return None;
        }
        if self.pos < self.s.len() && self.s[self.pos] == b'.' {
            is_int = false;
            self.pos += 1;
            while self.pos < self.s.len() && self.s[self.pos].is_ascii_digit() {
                self.pos += 1;
            }
        }
        if self.pos < self.s.len() && (self.s[self.pos] == b'e' || self.s[self.pos] == b'E') {
            is_int = false;
            self.pos += 1;
            if self.pos < self.s.len()
                && (self.s[self.pos] == b'+' || self.s[self.pos] == b'-')
            {
                self.pos += 1;
            }
            while self.pos < self.s.len() && self.s[self.pos].is_ascii_digit() {
                self.pos += 1;
            }
        }
        let text = std::str::from_utf8(&self.s[st..self.pos]).ok()?;
        if is_int {
            if let Ok(i) = text.parse::<i64>() {
                if i >= i32::MIN as i64 && i <= i32::MAX as i64 {
                    return Some(AklVal::mk_int(i as i32));
                }
                return Some(AklVal::from_f64(i as f64));
            }
            return Some(AklVal::from_f64(text.parse::<f64>().unwrap_or(f64::NAN)));
        }
        Some(AklVal::from_f64(text.parse::<f64>().unwrap_or(f64::NAN)))
    }

    /// 値（object / array / string / number / true / false / null）。
    fn value(&mut self, rt: &mut Runtime, depth: u32) -> Result<AklVal, ()> {
        if depth > 256 {
            return Err(());
        }
        self.ws();
        if self.pos >= self.s.len() {
            return Err(());
        }
        let c = self.s[self.pos];
        if c == b'{' {
            self.pos += 1;
            let obj_id = rt.heap.alloc(Obj::Obj(Vec::new())).map_err(|_| ())?;
            self.ws();
            if self.pos < self.s.len() && self.s[self.pos] == b'}' {
                self.pos += 1;
                return Ok(AklVal::mk_obj(obj_id));
            }
            loop {
                self.ws();
                let key = self.string().ok_or(())?;
                let kid = rt.intern(&key).ok_or(())?;
                self.ws();
                if self.pos >= self.s.len() || self.s[self.pos] != b':' {
                    return Err(());
                }
                self.pos += 1;
                let v = self.value(rt, depth + 1)?;
                rt.heap.prop_set(obj_id, kid, v).map_err(|_| ())?;
                self.ws();
                if self.pos < self.s.len() && self.s[self.pos] == b',' {
                    self.pos += 1;
                    continue;
                }
                if self.pos < self.s.len() && self.s[self.pos] == b'}' {
                    self.pos += 1;
                    return Ok(AklVal::mk_obj(obj_id));
                }
                return Err(());
            }
        }
        if c == b'[' {
            self.pos += 1;
            let mut items = Vec::new();
            self.ws();
            if self.pos < self.s.len() && self.s[self.pos] == b']' {
                self.pos += 1;
                let id = rt.heap.alloc(Obj::Arr(items)).map_err(|_| ())?;
                return Ok(AklVal::mk_obj(id));
            }
            loop {
                self.ws();
                let v = self.value(rt, depth + 1)?;
                items.push(v);
                self.ws();
                if self.pos < self.s.len() && self.s[self.pos] == b',' {
                    self.pos += 1;
                    continue;
                }
                if self.pos < self.s.len() && self.s[self.pos] == b']' {
                    self.pos += 1;
                    let id = rt.heap.alloc(Obj::Arr(items)).map_err(|_| ())?;
                    return Ok(AklVal::mk_obj(id));
                }
                return Err(());
            }
        }
        if c == b'"' {
            let s = self.string().ok_or(())?;
            let id = rt.intern(&s).ok_or(())?;
            return Ok(AklVal::mk_obj(id));
        }
        if c == b't' && self.lit(b"true") {
            return Ok(AklVal::TRUE);
        }
        if c == b'f' && self.lit(b"false") {
            return Ok(AklVal::FALSE);
        }
        if c == b'n' && self.lit(b"null") {
            return Ok(AklVal::NULL);
        }
        if let Some(n) = self.number() {
            return Ok(n);
        }
        Err(())
    }
}

/// 値（関数）を呼ぶ（native またはバイトコード関数）。簡易実装。
fn call_native(rt: &mut Runtime, f: AklVal, this: AklVal, args: &[AklVal]) -> Result<AklVal, VmError> {
    if !f.is_obj() {
        return Ok(AklVal::UNDEF);
    }
    let id = f.get_obj();
    if let Some(Obj::Native(nidx)) = rt.heap.get(id) {
        let nidx = *nidx;
        let nf = rt.native_fns.get(nidx as usize).ok_or(VmError::NotCallable)?;
        let nf = *nf;
        return nf(rt, this, args);
    }
    if let Some(Obj::Func { fidx, .. }) = rt.heap.get(id) {
        let fidx = *fidx;
        return rt.run(fidx, args);
    }
    Ok(AklVal::UNDEF)
}

/// 現在時刻（エポック ms。f64）。
fn now_ms() -> f64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs_f64() * 1000.0)
        .unwrap_or(0.0)
}

/// Date 値を ms として読む（非 Date は None）。
fn date_ms(rt: &Runtime, this: AklVal) -> Option<f64> {
    if this.is_obj() {
        if let Some(Obj::Date { ms }) = rt.heap.get(this.get_obj()) {
            return Some(*ms);
        }
    }
    None
}

/// ISO 8601 簡易パース（`YYYY-MM-DDTHH:MM:SS.sssZ` / 日付のみ / 年月のみ / 年のみ）。
fn parse_iso_date(s: &str) -> f64 {
    let s = s.trim();
    let b = s.as_bytes();
    let n = b.len();
    let mut i = 0usize;
    let mut neg = false;
    if i < n && (b[i] == b'-' || b[i] == b'+') {
        neg = b[i] == b'-';
        i += 1;
    }
    let read_uint = |i: &mut usize| -> Option<i64> {
        let st = *i;
        let mut v = 0i64;
        while *i < n && b[*i].is_ascii_digit() {
            v = v * 10 + (b[*i] - b'0') as i64;
            *i += 1;
        }
        if *i == st {
            None
        } else {
            Some(v)
        }
    };
    let year = match read_uint(&mut i) {
        Some(v) => if neg { -v } else { v },
        None => return f64::NAN,
    };
    let mut month = 1i64;
    let mut day = 1i64;
    let mut hour = 0i64;
    let mut minute = 0i64;
    let mut second = 0i64;
    let mut ms = 0i64;
    if i < n && b[i] == b'-' {
        i += 1;
        month = match read_uint(&mut i) {
            Some(v) => v,
            None => return f64::NAN,
        };
        if i < n && b[i] == b'-' {
            i += 1;
            day = match read_uint(&mut i) {
                Some(v) => v,
                None => return f64::NAN,
            };
        }
    }
    if i < n && (b[i] == b'T' || b[i] == b't' || b[i] == b' ') {
        i += 1;
        hour = match read_uint(&mut i) {
            Some(v) => v,
            None => return f64::NAN,
        };
        if i < n && b[i] == b':' {
            i += 1;
            minute = match read_uint(&mut i) {
                Some(v) => v,
                None => return f64::NAN,
            };
        }
        if i < n && b[i] == b':' {
            i += 1;
            second = match read_uint(&mut i) {
                Some(v) => v,
                None => return f64::NAN,
            };
        }
        if i < n && b[i] == b'.' {
            i += 1;
            // 小数秒（ms 桁まで）
            let mut scale = 100;
            while i < n && b[i].is_ascii_digit() && scale > 0 {
                ms += (b[i] - b'0') as i64 * scale;
                scale /= 10;
                i += 1;
            }
            while i < n && b[i].is_ascii_digit() {
                i += 1;
            }
        }
    }
    // 末尾 Z 等は無視（UTC 近似）
    let days = crate::bytecode::days_from_civil(year as i32, month as u32, day as u32);
    days as f64 * 86_400_000.0
        + ((hour * 3600 + minute * 60 + second) as f64) * 1000.0
        + ms as f64
}

/// `Date` を登録。Date グローバルは `constructor` + `now`/`parse`/`UTC` を持つ
/// `Obj::Obj`。インスタンスは `Obj::Date`、メソッドは `rt.date_methods` 経由。
fn install_date(rt: &mut Runtime) -> Result<(), VmError> {
    let date_id = rt.intern("Date").ok_or(VmError::Oom)?;
    let obj = rt.heap.alloc(Obj::Obj(Vec::new())).map_err(|_| VmError::Oom)?;
    // constructor（new Date() / Date() 用のフォールバック解決対象）
    let ctor = rt.register_native(date_ctor)?;
    rt.heap
        .prop_set(obj, rt.ctor_name, ctor)
        .map_err(|_| VmError::Oom)?;
    // 静的メソッド
    for (name, f) in [
        ("now", date_now as crate::bytecode::NativeFn),
        ("parse", date_parse),
        ("UTC", date_utc),
    ] {
        let v = rt.register_native(f)?;
        let nid = rt.intern(name).ok_or(VmError::Oom)?;
        rt.heap.prop_set(obj, nid, v).map_err(|_| VmError::Oom)?;
    }
    rt.global_set(date_id, AklVal::mk_obj(obj));
    // インスタンスメソッド
    for (name, f) in [
        ("getTime", date_get_time as crate::bytecode::NativeFn),
        ("valueOf", date_get_time),
        ("getFullYear", date_get_full_year),
        ("getMonth", date_get_month),
        ("getDate", date_get_date),
        ("getDay", date_get_day),
        ("getHours", date_get_hours),
        ("getMinutes", date_get_minutes),
        ("getSeconds", date_get_seconds),
        ("getMilliseconds", date_get_milliseconds),
        ("getUTCFullYear", date_get_full_year),
        ("getUTCMonth", date_get_month),
        ("getUTCDate", date_get_date),
        ("getUTCDay", date_get_day),
        ("getUTCHours", date_get_hours),
        ("getUTCMinutes", date_get_minutes),
        ("getUTCSeconds", date_get_seconds),
        ("getUTCMilliseconds", date_get_milliseconds),
        ("toISOString", date_to_iso),
        ("toString", date_to_str),
        ("setTime", date_set_time),
    ] {
        let v = rt.register_native(f)?;
        let nid = rt.intern(name).ok_or(VmError::Oom)?;
        rt.date_methods.push((nid, v));
    }
    Ok(())
}

/// `new Date()` / `Date(...)` のコンストラクタ。
fn date_ctor(rt: &mut Runtime, _this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let ms = if a.is_empty() {
        now_ms()
    } else if a.len() == 1 {
        let v = a[0];
        if v.is_int() || v.as_f64().is_some() {
            to_number(rt, v)
        } else {
            parse_iso_date(&to_js_string(rt, v))
        }
    } else {
        // new Date(y, m, d, h, mi, s, ms): UTC として解釈（TZ 非依存近似）
        let y = to_number(rt, a[0]) as i32;
        let m = to_number(rt, a.get(1).copied().unwrap_or(AklVal::UNDEF)) as i32;
        let d = to_number(rt, a.get(2).copied().unwrap_or(AklVal::mk_int(1))) as i32;
        let h = to_number(rt, a.get(3).copied().unwrap_or(AklVal::mk_int(0))) as i64;
        let mi = to_number(rt, a.get(4).copied().unwrap_or(AklVal::mk_int(0))) as i64;
        let s = to_number(rt, a.get(5).copied().unwrap_or(AklVal::mk_int(0))) as i64;
        let msd = to_number(rt, a.get(6).copied().unwrap_or(AklVal::mk_int(0)));
        let days = crate::bytecode::days_from_civil(y, (m + 1) as u32, d as u32);
        days as f64 * 86_400_000.0 + ((h * 3600 + mi * 60 + s) as f64) * 1000.0 + msd
    };
    let id = rt.heap.alloc(Obj::Date { ms }).map_err(|_| VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

fn date_now(_rt: &mut Runtime, _t: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    Ok(AklVal::from_f64(now_ms()))
}

fn date_parse(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = str_of(rt, a.first().copied().unwrap_or(AklVal::UNDEF));
    Ok(AklVal::from_f64(parse_iso_date(&s)))
}

fn date_utc(rt: &mut Runtime, _t: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let y = to_number(rt, a.first().copied().unwrap_or(AklVal::UNDEF)) as i32;
    let m = to_number(rt, a.get(1).copied().unwrap_or(AklVal::UNDEF)) as i32;
    let d = to_number(rt, a.get(2).copied().unwrap_or(AklVal::mk_int(1))) as i32;
    let h = to_number(rt, a.get(3).copied().unwrap_or(AklVal::mk_int(0))) as i64;
    let mi = to_number(rt, a.get(4).copied().unwrap_or(AklVal::mk_int(0))) as i64;
    let s = to_number(rt, a.get(5).copied().unwrap_or(AklVal::mk_int(0))) as i64;
    let msd = to_number(rt, a.get(6).copied().unwrap_or(AklVal::mk_int(0)));
    let days = crate::bytecode::days_from_civil(y, (m + 1) as u32, d as u32);
    Ok(AklVal::from_f64(
        days as f64 * 86_400_000.0 + ((h * 3600 + mi * 60 + s) as f64) * 1000.0 + msd,
    ))
}

fn date_get_time(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    match date_ms(rt, this) {
        Some(ms) => Ok(AklVal::from_f64(ms)),
        None => Ok(AklVal::from_f64(f64::NAN)),
    }
}

fn date_field(rt: &Runtime, this: AklVal, f: impl Fn(crate::bytecode::DateFields) -> i32) -> AklVal {
    match date_ms(rt, this) {
        Some(ms) => {
            let fields = crate::bytecode::date_utc_fields(ms);
            AklVal::mk_int(f(fields))
        }
        None => AklVal::from_f64(f64::NAN),
    }
}

fn date_get_full_year(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    Ok(date_field(rt, this, |f| f.year))
}
fn date_get_month(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    Ok(date_field(rt, this, |f| f.month as i32 - 1))
}
fn date_get_date(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    Ok(date_field(rt, this, |f| f.day as i32))
}
fn date_get_day(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    Ok(date_field(rt, this, |f| f.weekday as i32))
}
fn date_get_hours(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    Ok(date_field(rt, this, |f| f.hour as i32))
}
fn date_get_minutes(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    Ok(date_field(rt, this, |f| f.minute as i32))
}
fn date_get_seconds(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    Ok(date_field(rt, this, |f| f.second as i32))
}
fn date_get_milliseconds(
    rt: &mut Runtime,
    this: AklVal,
    _a: &[AklVal],
) -> Result<AklVal, VmError> {
    Ok(date_field(rt, this, |f| f.millisecond as i32))
}

fn date_to_iso(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = match date_ms(rt, this) {
        Some(ms) => crate::bytecode::date_to_iso_string(ms),
        None => "Invalid Date".to_string(),
    };
    let id = rt.intern(&s).ok_or(VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

fn date_to_str(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    let s = match date_ms(rt, this) {
        Some(ms) => crate::bytecode::date_to_string(ms),
        None => "Invalid Date".to_string(),
    };
    let id = rt.intern(&s).ok_or(VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
}

fn date_set_time(rt: &mut Runtime, this: AklVal, a: &[AklVal]) -> Result<AklVal, VmError> {
    let ms = to_number(rt, a.first().copied().unwrap_or(AklVal::UNDEF));
    if this.is_obj() {
        if let Some(Obj::Date { ms: slot }) = rt.heap.get_mut(this.get_obj()) {
            *slot = ms;
        }
    }
    Ok(AklVal::from_f64(ms))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bytecode::HandleVTab;
    use crate::codegen::compile;

    // ---- ハンドル（DOM 等）ディスパッチのテスト用 vtable ----
    fn h_get(rt: &mut Runtime, _data: u64, _ptr: u64, name: &str) -> Option<AklVal> {
        if name == "title" {
            let id = rt.intern("Hello")?;
            Some(AklVal::mk_obj(id))
        } else {
            None
        }
    }
    fn h_set(rt: &mut Runtime, _data: u64, _ptr: u64, name: &str, v: AklVal) -> bool {
        let s = rt.flatten_str(v);
        rt.console_out.push(format!("set {name} = {s}"));
        true
    }
    fn h_call(rt: &mut Runtime, _data: u64, _ptr: u64, name: &str, args: &[AklVal]) -> Option<AklVal> {
        if name == "getElementById" {
            let arg = args.first().map(|v| rt.flatten_str(*v)).unwrap_or_default();
            let id = rt.intern(&format!("found:{arg}"))?;
            Some(AklVal::mk_obj(id))
        } else {
            None
        }
    }
    static H_VT: HandleVTab = HandleVTab {
        tag: "TestDoc",
        get: h_get,
        set: h_set,
        call: h_call,
    };

    fn run_src(src: &str) -> Result<(AklVal, Vec<String>), String> {
        let program = crate::parser::Parser::new(src).parse_program().map_err(|e| e.0)?;
        let mut rt = Runtime::new();
        install_builtins(&mut rt).map_err(|e| format!("{e:?}"))?;
        // テスト用に document ハンドルを束縛
        let handle = rt
            .heap
            .alloc(Obj::Handle { vtab: &H_VT, data: 0, ptr: 0x1234 })
            .map_err(|e| format!("{e:?}"))?;
        let doc_name = rt.intern("document").unwrap();
        rt.global_set(doc_name, AklVal::mk_obj(handle));
        let fidx = compile(&mut rt, &program).map_err(|e| e.0)?;
        let v = rt.run(fidx, &[]).map_err(|e| format!("{e:?}"))?;
        Ok((v, rt.console_out.clone()))
    }

    #[test]
    fn console_log() {
        let (_, out) = run_src("console.log(\"hello\", 42);").unwrap();
        assert_eq!(out, vec!["hello 42".to_string()]);
    }

    #[test]
    fn math() {
        assert_eq!(run_src("Math.abs(-5);").unwrap().0, AklVal::from_f64(5.0));
        assert_eq!(run_src("Math.floor(3.7);").unwrap().0, AklVal::from_f64(3.0));
        assert_eq!(run_src("Math.ceil(3.2);").unwrap().0, AklVal::from_f64(4.0));
        assert_eq!(run_src("Math.max(1, 5, 3);").unwrap().0, AklVal::from_f64(5.0));
        assert_eq!(run_src("Math.min(1, 5, 3);").unwrap().0, AklVal::from_f64(1.0));
        assert_eq!(run_src("Math.sqrt(9);").unwrap().0, AklVal::from_f64(3.0));
        assert_eq!(run_src("Math.pow(2, 10);").unwrap().0, AklVal::from_f64(1024.0));
    }

    #[test]
    fn global_funcs() {
        assert_eq!(run_src("parseInt(\"42\");").unwrap().0, AklVal::from_f64(42.0));
        assert_eq!(run_src("parseInt(\"-7\");").unwrap().0, AklVal::from_f64(-7.0));
        assert_eq!(run_src("isNaN(\"abc\");").unwrap().0, AklVal::TRUE);
        assert_eq!(run_src("Number(\"3.5\");").unwrap().0, AklVal::from_f64(3.5));
    }

    #[test]
    fn string_methods() {
        // "hello".toUpperCase() → "HELLO"（内容で確認）
        let (v, _) = run_src("\"hello\".toUpperCase();").unwrap();
        assert!(v.is_obj());
        // 内容を Runtime から取得できないので、intern 経由で確認するのは
        // テストヘルパが無いため、別の方法: "HELLO" との比較式で確認
        assert_eq!(run_src("\"hello\".toUpperCase() === \"HELLO\";").unwrap().0, AklVal::TRUE);
        assert_eq!(run_src("\"abc\".length;").unwrap().0, AklVal::mk_int(3));
        assert_eq!(run_src("\"hello\".indexOf(\"l\");").unwrap().0, AklVal::mk_int(2));
        assert_eq!(run_src("\"hello\".includes(\"ell\");").unwrap().0, AklVal::TRUE);
    }

    #[test]
    fn array_methods_higher_order() {
        // map
        assert_eq!(
            run_src("var a = [1, 2, 3]; a.map(function(x) { return x * 2; })[1];").unwrap().0,
            AklVal::mk_int(4)
        );
        // filter
        assert_eq!(
            run_src("var a = [1, 2, 3, 4]; a.filter(function(x) { return x % 2 === 0; }).length;").unwrap().0,
            AklVal::mk_int(2)
        );
        // reduce
        assert_eq!(
            run_src("var a = [1, 2, 3, 4]; a.reduce(function(acc, x) { return acc + x; }, 0);").unwrap().0,
            AklVal::mk_int(10)
        );
    }

    #[test]
    fn array_methods_basic() {
        assert_eq!(run_src("[1, 2, 3].push(4);").unwrap().0, AklVal::mk_int(4));
        assert_eq!(run_src("[1, 2, 3].shift();").unwrap().0, AklVal::mk_int(1));
        assert_eq!(run_src("[2, 3].unshift(1);").unwrap().0, AklVal::mk_int(3));
        assert_eq!(run_src("[1, 2, 3].includes(2);").unwrap().0, AklVal::TRUE);
        assert_eq!(run_src("[1, 2, 3].concat([4, 5]).length;").unwrap().0, AklVal::mk_int(5));
        assert_eq!(run_src("[1, 2, 3].join(\"-\") === \"1-2-3\";").unwrap().0, AklVal::TRUE);
        assert_eq!(run_src("[1, 2, 3].at(1);").unwrap().0, AklVal::mk_int(2));
        assert_eq!(run_src("[1, 2, 3].at(-1);").unwrap().0, AklVal::mk_int(3));
    }

    #[test]
    fn array_methods_find_some_every() {
        assert_eq!(
            run_src("[1, 2, 3, 4].find(function(x) { return x > 2; });").unwrap().0,
            AklVal::mk_int(3)
        );
        assert_eq!(
            run_src("[1, 2, 3].findIndex(function(x) { return x === 2; });").unwrap().0,
            AklVal::mk_int(1)
        );
        assert_eq!(
            run_src("[1, 2, 3].some(function(x) { return x > 2; });").unwrap().0,
            AklVal::TRUE
        );
        assert_eq!(
            run_src("[1, 2, 3].every(function(x) { return x > 0; });").unwrap().0,
            AklVal::TRUE
        );
    }

    #[test]
    fn array_sort_splice_flat() {
        assert_eq!(
            run_src("var a = [3, 1, 2]; a.sort(); a[0];").unwrap().0,
            AklVal::mk_int(1)
        );
        assert_eq!(
            run_src("var a = [1, 2, 3, 4]; a.splice(1, 2); a.length;").unwrap().0,
            AklVal::mk_int(2)
        );
        assert_eq!(
            run_src("[[1, 2], [3, 4]].flat().length;").unwrap().0,
            AklVal::mk_int(4)
        );
    }

    #[test]
    fn object_methods() {
        assert_eq!(
            run_src("Object.keys({a: 1, b: 2}).length;").unwrap().0,
            AklVal::mk_int(2)
        );
        assert_eq!(
            run_src("Object.values({a: 1, b: 2}).length;").unwrap().0,
            AklVal::mk_int(2)
        );
        assert_eq!(
            run_src("var o = {a: 1}; Object.assign(o, {b: 2}); o.b;").unwrap().0,
            AklVal::mk_int(2)
        );
    }

    #[test]
    fn regex_methods() {
        // マッチ
        assert_eq!(
            run_src("\"abc123\".match(/[0-9]+/)[0] === \"123\";").unwrap().0,
            AklVal::TRUE
        );
        // search
        assert_eq!(
            run_src("\"abc123\".search(/[0-9]+/);").unwrap().0,
            AklVal::mk_int(3)
        );
        // replace
        assert_eq!(
            run_src("\"hello world\".replace(/world/, \"there\") === \"hello there\";").unwrap().0,
            AklVal::TRUE
        );
        // split
        assert_eq!(
            run_src("\"a,b,c\".split(/,/).length;").unwrap().0,
            AklVal::mk_int(3)
        );
    }

    #[test]
    fn map_set() {
        assert_eq!(
            run_src("var m = new Map(); m.set(\"a\", 1); m.get(\"a\");").unwrap().0,
            AklVal::mk_int(1)
        );
        assert_eq!(
            run_src("var m = new Map(); m.set(\"a\", 1); m.has(\"a\");").unwrap().0,
            AklVal::TRUE
        );
        assert_eq!(
            run_src("var s = new Set(); s.add(1); s.has(1);").unwrap().0,
            AklVal::TRUE
        );
        assert_eq!(
            run_src("var s = new Set(); s.add(1); s.has(2);").unwrap().0,
            AklVal::FALSE
        );
    }

    #[test]
    fn json_stringify() {
        assert_eq!(
            run_src("JSON.stringify(42) === \"42\";").unwrap().0,
            AklVal::TRUE
        );
        assert_eq!(
            run_src("JSON.stringify(\"hi\") === \"\\\"hi\\\"\";").unwrap().0,
            AklVal::TRUE
        );
        assert_eq!(
            run_src("JSON.stringify([1, 2, 3]) === \"[1,2,3]\";").unwrap().0,
            AklVal::TRUE
        );
        assert_eq!(
            run_src("JSON.stringify({a: 1}) === \"{\\\"a\\\":1}\";").unwrap().0,
            AklVal::TRUE
        );
    }

    #[test]
    fn json_parse() {
        assert_eq!(run_src("JSON.parse(\"42\");").unwrap().0, AklVal::mk_int(42));
        assert_eq!(
            run_src("JSON.parse(\"1.5\") === 1.5;").unwrap().0,
            AklVal::TRUE
        );
        assert_eq!(run_src("JSON.parse(\"true\");").unwrap().0, AklVal::TRUE);
        assert_eq!(run_src("JSON.parse(\"null\");").unwrap().0, AklVal::NULL);
        assert_eq!(
            run_src("JSON.parse(\"\\\"hi\\\"\") === \"hi\";").unwrap().0,
            AklVal::TRUE
        );
        assert_eq!(
            run_src("JSON.parse(\"[1, 2, 3]\")[1];").unwrap().0,
            AklVal::mk_int(2)
        );
        assert_eq!(
            run_src("JSON.parse(\"{\\\"a\\\": 1}\").a;").unwrap().0,
            AklVal::mk_int(1)
        );
        // ネスト
        assert_eq!(
            run_src("JSON.parse(\"{\\\"a\\\": [1, {\\\"b\\\": 2}]}\").a[1].b;").unwrap().0,
            AklVal::mk_int(2)
        );
        // エスケープ
        assert_eq!(
            run_src("JSON.parse(\"\\\"a\\\\nb\\\"\") === \"a\\nb\";").unwrap().0,
            AklVal::TRUE
        );
    }

    #[test]
    fn json_parse_invalid_throws() {
        // 不正 JSON は SyntaxError を throw（catch で捕捉できる）
        assert_eq!(
            run_src("var r = 0; try { JSON.parse(\"{bad}\"); } catch (e) { r = 1; } r;")
                .unwrap()
                .0,
            AklVal::mk_int(1)
        );
    }

    #[test]
    fn array_sort_comparator() {
        assert_eq!(
            run_src("var a = [3, 1, 2]; a.sort(function(x, y) { return x - y; }); a[0];")
                .unwrap()
                .0,
            AklVal::mk_int(1)
        );
        assert_eq!(
            run_src("var a = [1, 2, 3]; a.sort(function(x, y) { return y - x; }); a[0];")
                .unwrap()
                .0,
            AklVal::mk_int(3)
        );
    }

    #[test]
    fn object_entries_from_entries() {
        assert_eq!(
            run_src("Object.entries({a: 1, b: 2}).length;").unwrap().0,
            AklVal::mk_int(2)
        );
        assert_eq!(
            run_src("Object.fromEntries([[\"a\", 1], [\"b\", 2]]).b;").unwrap().0,
            AklVal::mk_int(2)
        );
    }

    #[test]
    fn handle_dispatch() {
        // プロパティ取得 → vtable.get
        assert_eq!(
            run_src("document.title === \"Hello\";").unwrap().0,
            AklVal::TRUE
        );
        // メソッド呼び出し → vtable.call
        assert_eq!(
            run_src("document.getElementById(\"a\") === \"found:a\";").unwrap().0,
            AklVal::TRUE
        );
        // ブラケットアクセス → vtable.get（文字列キー）
        assert_eq!(
            run_src("document[\"title\"] === \"Hello\";").unwrap().0,
            AklVal::TRUE
        );
        // プロパティ設定 → vtable.set（console_out で観測）
        let (_, out) = run_src("document.title = \"Bye\";").unwrap();
        assert_eq!(out, vec!["set title = Bye".to_string()]);
        // typeof handle = object / 未知メソッドは throw
        assert_eq!(
            run_src("typeof document === \"object\";").unwrap().0,
            AklVal::TRUE
        );
        assert_eq!(
            run_src("var r = 0; try { document.noSuchMethod(); } catch (e) { r = 1; } r;")
                .unwrap()
                .0,
            AklVal::mk_int(1)
        );
    }

    #[test]
    fn logical_assignment() {
        assert_eq!(
            run_src("var x = 0; x ||= 5; x;").unwrap().0,
            AklVal::mk_int(5)
        );
        assert_eq!(
            run_src("var x = 7; x ||= 5; x;").unwrap().0,
            AklVal::mk_int(7)
        );
        assert_eq!(
            run_src("var x; x ??= 3; x;").unwrap().0,
            AklVal::mk_int(3)
        );
        assert_eq!(
            run_src("var x = 0; x ??= 3; x;").unwrap().0,
            AklVal::mk_int(0)
        );
        assert_eq!(
            run_src("var x = 1; x &&= 9; x;").unwrap().0,
            AklVal::mk_int(9)
        );
    }

    #[test]
    fn class_fields() {
        assert_eq!(
            run_src("class Item { name = 'unnamed'; count = 0; } var it = new Item(); it.count;")
                .unwrap()
                .0,
            AklVal::mk_int(0)
        );
        assert_eq!(
            run_src("class Item { name = 'unnamed'; } var it = new Item(); it.name === 'unnamed';")
                .unwrap()
                .0,
            AklVal::TRUE
        );
    }

    #[test]
    fn getter_setter_objlit() {
        // `x / 2` は double を返すため、数値比較は `===`（int/double 統一）で確認する
        assert_eq!(
            run_src("var o = { v: 10, get d() { return this.v * 2; }, set d(x) { this.v = x / 2; } }; o.d = 20; o.d === 20;")
                .unwrap()
                .0,
            AklVal::TRUE
        );
        assert_eq!(
            run_src("var o = { v: 10, get d() { return this.v * 2; }, set d(x) { this.v = x / 2; } }; o.d = 20; o.v === 10;")
                .unwrap()
                .0,
            AklVal::TRUE
        );
    }

    #[test]
    fn object_spread() {
        assert_eq!(
            run_src("var b = {w: 100, h: 50}; var o = {w: 0, h: 60, z: 1}; o;").unwrap().0.is_obj(),
            true
        );
        // {...b, h: 60} で h が上書きされる
        assert_eq!(
            run_src("var b = {w: 100, h: 50}; var o = {...b, h: 60}; o.h;").unwrap().0,
            AklVal::mk_int(60)
        );
        assert_eq!(
            run_src("var b = {w: 100, h: 50}; var o = {...b, h: 60}; o.w;").unwrap().0,
            AklVal::mk_int(100)
        );
    }

    #[test]
    fn call_spread() {
        assert_eq!(
            run_src("function f(a, b, c) { return a + b + c; } var xs = [1, 2]; f(...xs, 3);")
                .unwrap()
                .0,
            AklVal::mk_int(6)
        );
    }

    #[test]
    fn destructure_assign() {
        assert_eq!(
            run_src("var r; [r] = [9, 8, 7]; r;").unwrap().0,
            AklVal::mk_int(9)
        );
        assert_eq!(
            run_src("var f; var r2; var o = {first: 1, second: 2, third: 3}; var {first: f, ...r2} = o; r2.second + r2.third;")
                .unwrap()
                .0,
            AklVal::mk_int(5)
        );
    }

    #[test]
    fn date_basic() {
        // Date.UTC(1970, 0, 1) = 0
        assert_eq!(
            run_src("Date.UTC(1970, 0, 1) === 0;").unwrap().0,
            AklVal::TRUE
        );
        // Date.parse("1970-01-01") = 0
        assert_eq!(
            run_src("Date.parse(\"1970-01-01\") === 0;").unwrap().0,
            AklVal::TRUE
        );
        // getTime / getUTCFullYear / getUTCDate / getUTCSeconds
        assert_eq!(
            run_src("var d = new Date(0); d.getTime() === 0;").unwrap().0,
            AklVal::TRUE
        );
        assert_eq!(
            run_src("var d = new Date(0); d.getUTCFullYear();").unwrap().0,
            AklVal::mk_int(1970)
        );
        assert_eq!(
            run_src("var d = new Date(0); d.getUTCDate();").unwrap().0,
            AklVal::mk_int(1)
        );
        // toISOString
        assert_eq!(
            run_src("var d = new Date(0); d.toISOString() === \"1970-01-01T00:00:00.000Z\";")
                .unwrap()
                .0,
            AklVal::TRUE
        );
        // valueOf による数値化（+new Date(0)）
        assert_eq!(
            run_src("var d = new Date(0); +d === 0;").unwrap().0,
            AklVal::TRUE
        );
        // 時刻フィールド（3661 秒 = 01:01:01）
        assert_eq!(
            run_src("var d = new Date(3661000); d.getUTCSeconds();").unwrap().0,
            AklVal::mk_int(1)
        );
        assert_eq!(
            run_src("var d = new Date(3661000); d.getUTCMinutes();").unwrap().0,
            AklVal::mk_int(1)
        );
        assert_eq!(
            run_src("var d = new Date(3661000); d.getUTCHours();").unwrap().0,
            AklVal::mk_int(1)
        );
    }

    #[test]
    fn bigint_arithmetic() {
        // 加算・乗算（BigInt === BigInt は値比較）・typeof・==（数値との比較）
        assert_eq!(
            run_src("9007199254740993n + 1n === 9007199254740994n;").unwrap().0,
            AklVal::TRUE
        );
        assert_eq!(run_src("10n * 3n === 30n;").unwrap().0, AklVal::TRUE);
        assert_eq!(run_src("typeof 10n === 'bigint';").unwrap().0, AklVal::TRUE);
        assert_eq!(
            run_src("((10n == 10) ? 'yes' : 'no') === 'yes';").unwrap().0,
            AklVal::TRUE
        );
        assert_eq!(run_src("10n === 10n;").unwrap().0, AklVal::TRUE);
    }

    #[test]
    fn generator_yield() {
        let src = "function* nums() { yield 3; yield 1; yield 4; }
                   var it = nums();
                   var a = it.next(); var b = it.next(); var c = it.next();
                   a.value + b.value + c.value;";
        assert_eq!(run_src(src).unwrap().0, AklVal::mk_int(8));
        // done フラグ（3 回 yield 後、4 回目の next は done）
        let src2 = "function* g() { yield 1; }
                    var it = g(); it.next(); it.next().done;";
        assert_eq!(run_src(src2).unwrap().0, AklVal::TRUE);
    }

    #[test]
    fn logic_short_circuit() {
        // && / || は値返し短絡（lodash の環境検出 `typeof x == 'object' && x` が依存）
        assert_eq!(run_src("1 && 2;").unwrap().0, AklVal::mk_int(2));
        assert_eq!(run_src("0 && 2;").unwrap().0, AklVal::mk_int(0));
        assert_eq!(run_src("1 || 2;").unwrap().0, AklVal::mk_int(1));
        assert_eq!(run_src("0 || 2;").unwrap().0, AklVal::mk_int(2));
        // 短絡で未宣言グローバルも評価されない（typeof と組み合わせた環境検出）
        assert_eq!(
            run_src("(typeof noSuchGlobal == 'object' && noSuchGlobal) ? 1 : 2;").unwrap().0,
            AklVal::mk_int(2)
        );
    }

    #[test]
    fn object_proto_to_string() {
        assert_eq!(
            run_src("Object.prototype.toString.call([]) === '[object Array]';").unwrap().0,
            AklVal::TRUE
        );
        assert_eq!(
            run_src("Object.prototype.toString.call({}) === '[object Object]';").unwrap().0,
            AklVal::TRUE
        );
        assert_eq!(
            run_src("Object.prototype.hasOwnProperty.call({a: 1}, 'a');").unwrap().0,
            AklVal::TRUE
        );
    }

    #[test]
    fn func_call_apply() {
        // Function.prototype.call / apply（lodash 等が `fn.call(thisArg, ...)` を使う）
        assert_eq!(
            run_src("function f(a, b) { return this.v + a + b; } f.call({v: 1}, 2, 3);")
                .unwrap()
                .0,
            AklVal::mk_int(6)
        );
        assert_eq!(
            run_src("function f(a, b) { return this.v + a + b; } f.apply({v: 1}, [2, 3]);")
                .unwrap()
                .0,
            AklVal::mk_int(6)
        );
    }

    #[test]
    fn promise_and_async() {
        // Promise.then は no-op（マイクロタスク近似）。total は 0 のまま。
        let src = "var total = 0;
                   var p = new Promise(function(res) { res(5); });
                   p.then(function(v) { total = v * 10; });
                   total;";
        assert_eq!(run_src(src).unwrap().0, AklVal::mk_int(0));
        // async/await（await は解決済み Promise を unwrap、async 関数は Promise を返す）
        let src2 = "var r = 0;
                    async function compute() { return await Promise.resolve(7) * 3; }
                    compute().then(function(v) { r = v; });
                    r;";
        assert_eq!(run_src(src2).unwrap().0, AklVal::mk_int(0));
    }
}
