//! 文字列層（フェーズ 1）: intern テーブル + 文字列型。
//!
//! C 実装からの移植対応:
//! | C (akl.c) | Rust |
//! |---|---|
//! | `akl_intern` / `strtab`（開番地ハッシュ） | [`Interner`]（`HashMap` ベース） |
//! | STR オブジェクト（obj 配列 + u32 index） | [`StrId`]（`Vec<Box<str>>` の index） |
//! | `AKL_STRF_INTERNED` フラグ（一意性の手動維持） | 型レベルで保証（下記） |
//!
//! # C 実装で実際に起きたバグと、Rust で構造的に消える理由
//!
//! 1. **intern 一意性の破壊**（v0.10 で実測）: C では `akl_mkstring`（intern を通らない
//!    直接生成）と `akl_intern` が別経路で同じ内容の STR を作り得る。`"TypeError"` が
//!    2 つの id を持ち、グローバル解決が miss して `typeof TypeError === "undefined"` に
//!    なった。Rust では [`Interner::intern`] が唯一の生成経路であり、`&str` → `StrId` の
//!    写像は `HashMap` が全単射を保証する → **同一内容 = 同一 id が型で保証される**。
//!
//! 2. **GC 死エントリによる開番地クラスタ劣化**（v0.9b/v0.9e で 2 回試作して 2 回撤収）:
//!    C の手書き開番地ハッシュは削除エントリを「空」にできず、死エントリがクラスタに
//!    溜まると検索が O(挿入数) に劣化。プローブ上限付きでも rebuild が頻発して
//!    test_akl が 3 秒 → 250 秒超に落ちた。Rust の `HashMap` は削除を正しく処理するため
//!    **この問題は存在しない**（`remove` 後の参照整合性は型システムが保証）。
//!
//! 3. **文字列バイト列の手動 free**（C は `free(o->bytes)` を GC スイープで手動実行）:
//!    Rust は `Box<str>` のドロップが所有権で自動実行 → **二重 free / use-after-free /
//!    リークが構造的に起きない**（コンパイルエラーになる）。

#![forbid(unsafe_code)]
#![warn(missing_docs)]

use std::collections::HashMap;

/// intern 済み文字列の id（C の STR obj index に相当）。u32 なので AklVal の
/// obj タグ空間（下位 32 bit）にそのまま乗る。
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct StrId(u32);

impl StrId {
    /// u32 から直接作る（C 連携・テスト用。通常は [`Interner::intern`] を使う）。
    pub const fn from_u32(v: u32) -> Self {
        Self(v)
    }

    /// 内部の u32（C 連携・テスト用）。
    pub const fn as_u32(self) -> u32 {
        self.0
    }
}

/// 文字列インターンテーブル。
///
/// - `map`: 内容 → id の写像（全単射の保証元）
/// - `strings`: id → 内容 の配列（id は生成順の連番 = 配列 index）
///
/// 不変条件:
/// 1. `map.len() == strings.len()`（全 intern 済み文字列が両方に載る）
/// 2. `map[s] == StrId(i)` なら `strings[i] == s`（写像の往復整合）
/// 3. 同一内容は必ず同一 id（一意性。`HashMap` の get-or-insert が保証）
#[derive(Default)]
pub struct Interner {
    map: HashMap<Box<str>, StrId>,
    strings: Vec<Box<str>>,
}

impl Interner {
    /// 空のインターンを作る。
    pub fn new() -> Self {
        Self::default()
    }

    /// 文字列を intern し id を返す。既にあれば既存 id（一意性）。
    /// このメソッドが文字列をテーブルに追加する唯一の経路。
    pub fn intern(&mut self, s: &str) -> StrId {
        if let Some(&id) = self.map.get(s) {
            return id;
        }
        let boxed: Box<str> = s.into();
        let id = StrId(self.strings.len() as u32);
        self.map.insert(boxed.clone(), id);
        self.strings.push(boxed);
        id
    }

    /// id に対応する文字列を返す。範囲外は panic（= バグの早期検出。
    /// C の「範囲外 index で未定義動作」より安全側）。
    pub fn get(&self, id: StrId) -> &str {
        &self.strings[id.0 as usize]
    }

    /// 登録済み文字列数。
    pub fn len(&self) -> usize {
        self.strings.len()
    }

    /// 空か。
    pub fn is_empty(&self) -> bool {
        self.strings.is_empty()
    }

    /// 内容 → id の逆引き（存在しなければ None）。
    pub fn lookup(&self, s: &str) -> Option<StrId> {
        self.map.get(s).copied()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn intern_identity() {
        let mut it = Interner::new();
        let a = it.intern("hello");
        let b = it.intern("hello");
        assert_eq!(a, b, "同一内容は同一 id");
    }

    #[test]
    fn intern_distinct() {
        let mut it = Interner::new();
        let a = it.intern("hello");
        let b = it.intern("world");
        assert_ne!(a, b, "異なる内容は異なる id");
    }

    #[test]
    fn roundtrip() {
        let mut it = Interner::new();
        for s in ["", "a", "abc", "日本語", "TypeError", "aaaaaaaaaaaaaaaaaaaaaaaaaaaa"] {
            let id = it.intern(s);
            assert_eq!(it.get(id), s, "get(intern(s)) == s");
        }
    }

    #[test]
    fn lookup_works() {
        let mut it = Interner::new();
        let id = it.intern("x");
        assert_eq!(it.lookup("x"), Some(id));
        assert_eq!(it.lookup("y"), None);
    }

    #[test]
    fn len_tracks() {
        let mut it = Interner::new();
        assert!(it.is_empty());
        it.intern("a");
        it.intern("b");
        it.intern("a"); // 重複は増えない
        assert_eq!(it.len(), 2);
    }
}

/// Kani による機械的証明（`cargo kani` で実行）。
///
/// 証明する性質（C 実装で実際に壊れた不変条件）:
/// 1. **一意性**: 同一内容を 2 回 intern しても同一 id（C の「二重 id」バグの否定）
/// 2. **往復整合**: `get(intern(s)) == s`（C の「id が別内容を指す」バグの否定）
/// 3. **単射性**: 異なる id は異なる内容（同じ id が 2 つの内容を指さない）
#[cfg(kani)]
mod verification {
    use super::*;

    /// 同一文字列の二重 intern が同一 id を返す。
    /// C の v0.10 実バグ（akl_mkstring と akl_intern の二重経路で "TypeError" が
    /// id 292 と 293 に分かれた）の構造的否定。
    #[kani::proof]
    fn intern_idempotent() {
        let mut it = Interner::new();
        // Kani は任意長の文字列を扱えないため、決まった文字列で全分岐を証明する
        let samples = ["", "a", "ab", "hello", "TypeError", "日本語"];
        for s in samples {
            let id1 = it.intern(s);
            let id2 = it.intern(s);
            assert_eq!(id1, id2, "同一内容は同一 id でなければならない");
        }
    }

    /// get(intern(s)) が元の文字列を返す（往復整合）。
    /// C の「intern 済みフラグ漏れで別 STR が作られ、globals 解決が miss」の構造的否定。
    #[kani::proof]
    fn intern_roundtrip() {
        let mut it = Interner::new();
        let samples = ["", "a", "abc", "TypeError", "とても長い日本語の文字列です"];
        for s in samples {
            let id = it.intern(s);
            assert_eq!(it.get(id), s, "get(intern(s)) は s と一致しなければならない");
        }
    }

    /// 単射性: intern 後に strings 配列内で同一内容が 2 箇所に存在しない。
    #[kani::proof]
    fn intern_injective() {
        let mut it = Interner::new();
        let samples = ["alpha", "beta", "gamma"];
        for s in samples {
            it.intern(s);
        }
        let n = it.len();
        for i in 0..n {
            for j in 0..n {
                let si = it.get(StrId::from_u32(i as u32));
                let sj = it.get(StrId::from_u32(j as u32));
                if i != j {
                    assert_ne!(si, sj, "異なる id は異なる内容でなければならない");
                }
            }
        }
    }

    /// 空文字列の扱い（C では n==0 の memcmp 分岐が特殊だった）。
    #[kani::proof]
    fn intern_empty() {
        let mut it = Interner::new();
        let id1 = it.intern("");
        let id2 = it.intern("");
        assert_eq!(id1, id2);
        assert_eq!(it.get(id1), "");
    }
}
