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
use crate::obj::Obj;
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
    rt.register_global_native("String", string_ctor)?;

    // String.prototype メソッド（プレーンオブジェクトに native を載せる）
    install_string_methods(rt)?;
    // Array.prototype メソッド
    install_array_methods(rt)?;
    // Object 静的メソッド（keys/values/assign）
    install_object_methods(rt)?;
    // JSON.stringify
    install_json(rt)?;
    // Map / Set / Promise
    install_map_set(rt)?;

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

/// String.prototype メソッドを登録（C の `str_meth_vals` 相当。文字列リテラルの
/// メソッド解決用に `rt.str_methods` へ登録）。`length` はプロパティなので除外。
fn install_string_methods(rt: &mut Runtime) -> Result<(), VmError> {
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
    ] {
        let v = rt.register_native(f)?;
        let nid = rt.intern(name).ok_or(VmError::Oom)?;
        rt.str_methods.push((nid, v));
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

/// Array.prototype メソッドを登録（C の `arr_meth_vals` 相当）。`length` はプロパティなので除外。
fn install_array_methods(rt: &mut Runtime) -> Result<(), VmError> {
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
    }
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

fn arr_sort(rt: &mut Runtime, this: AklVal, _a: &[AklVal]) -> Result<AklVal, VmError> {
    let id = if this.is_obj() { this.get_obj() } else { return Ok(AklVal::UNDEF) };
    let mut items = this_arr(rt, this);
    // デフォルトは文字列化比較（JS の sort 既定）。借用回避のため文字列キーを先に作る。
    let mut keyed: Vec<(String, AklVal)> = items
        .iter()
        .map(|v| (to_js_string(rt, *v), *v))
        .collect();
    keyed.sort_by(|x, y| x.0.cmp(&y.0));
    items = keyed.into_iter().map(|(_, v)| v).collect();
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
    // Promise.resolve
    rt.register_global_native("Promise", promise_ctor)?;
    Ok(())
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
    // Promise.resolve(value) 相当（executor は未対応。解決済み Promise を返す近似）
    let v = a.first().copied().unwrap_or(AklVal::UNDEF);
    let id = rt
        .heap
        .alloc(Obj::Promise { state: 1, value: v })
        .map_err(|_| VmError::Oom)?;
    Ok(AklVal::mk_obj(id))
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

/// Object 静的メソッド（keys/values/assign）を登録。
fn install_object_methods(rt: &mut Runtime) -> Result<(), VmError> {
    let obj_id = rt.intern("Object").ok_or(VmError::Oom)?;
    let obj = rt.heap.alloc(Obj::Obj(Vec::new())).map_err(|_| VmError::Oom)?;
    for (name, f) in [
        ("keys", obj_keys as crate::bytecode::NativeFn),
        ("values", obj_values),
        ("assign", obj_assign),
    ] {
        let v = rt.register_native(f)?;
        let nid = rt.intern(name).ok_or(VmError::Oom)?;
        rt.heap.prop_set(obj, nid, v).map_err(|_| VmError::Oom)?;
    }
    rt.global_set(obj_id, AklVal::mk_obj(obj));
    Ok(())
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

/// JSON.stringify を登録（簡易版）。
fn install_json(rt: &mut Runtime) -> Result<(), VmError> {
    let json_id = rt.intern("JSON").ok_or(VmError::Oom)?;
    let json = rt.heap.alloc(Obj::Obj(Vec::new())).map_err(|_| VmError::Oom)?;
    let f = rt.register_native(json_stringify)?;
    let nid = rt.intern("stringify").ok_or(VmError::Oom)?;
    rt.heap.prop_set(json, nid, f).map_err(|_| VmError::Oom)?;
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
    if let Some(Obj::Func { fidx, env: _ }) = rt.heap.get(id) {
        let fidx = *fidx;
        return rt.run(fidx, args);
    }
    Ok(AklVal::UNDEF)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::codegen::compile;

    fn run_src(src: &str) -> Result<(AklVal, Vec<String>), String> {
        let program = crate::parser::Parser::new(src).parse_program().map_err(|e| e.0)?;
        let mut rt = Runtime::new();
        install_builtins(&mut rt).map_err(|e| format!("{e:?}"))?;
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
}
