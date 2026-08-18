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
        ("join", arr_join),
        ("indexOf", arr_index_of),
        ("slice", arr_slice),
        ("map", arr_map),
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
}
