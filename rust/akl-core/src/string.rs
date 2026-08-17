//! 文字列インターン層（フェーズ 1）: 内容 → ヒープ上の文字列オブジェクト id の写像。
//!
//! C 実装からの移植対応:
//! | C (akl.c) | Rust |
//! |---|---|
//! | `akl_intern` / `strtab`（開番地ハッシュ） | [`Interner`]（`HashMap` ベース） |
//! | STR オブジェクト（`rt->objs[]` 内の obj index） | [`crate::obj::Obj::Str`]（ヒープ内） |
//! | `AKL_STRF_INTERNED` フラグ（一意性の手動維持） | 型レベルで保証（下記） |
//!
//! # 設計（C との対応）
//!
//! C 実装は文字列も `rt->objs[]`（単一 obj テーブル）に載せ、`strtab` はその
//! 「内容 → obj index」の高速索引にすぎない。本クレートも同じ構成を採る:
//!
//! - 文字列の**正体**は [`crate::obj::ObjTable`] 内の [`crate::obj::Obj::Str`]。
//! - [`Interner`] は `HashMap<内容, ObjId>` の**最適化キャッシュ**であり、
//!   文字列を所有しない。`ObjId` はヒープ上の文字列オブジェクトを指す。
//!
//! これにより文字列とオブジェクトが**単一の id 空間**（`ObjId`）に統一され、
//! `AklVal::mk_obj` が文字列・配列・関数・オブジェクトのいずれも指せる
//! （C の NaN-box obj タグと同一のセマンティクス）。
//!
//! # C 実装で実際に起きたバグと、Rust で構造的に消える理由
//!
//! 1. **intern 一意性の破壊**（v0.10 で実測）: C では `akl_mkstring`（intern を通らない
//!    直接生成）と `akl_intern` が別経路で同じ内容の STR を作り得る。`"TypeError"` が
//!    2 つの id を持ち、グローバル解決が miss して `typeof TypeError === "undefined"` に
//!    なった。Rust では [`Interner::intern`] が唯一の生成経路であり、`&str` → `ObjId` の
//!    写像は `HashMap` が全単射を保証する → **同一内容 = 同一 id が型で保証される**。
//!
//! 2. **GC 死エントリによる開番地クラスタ劣化**（v0.9b/v0.9e で 2 回試作して 2 回撤収）:
//!    C の手書き開番地ハッシュは削除エントリを「空」にできず、死エントリがクラスタに
//!    溜まると検索が O(挿入数) に劣化。Rust の `HashMap` は削除を正しく処理するため
//!    **この問題は存在しない**。
//!
//! 3. **文字列バイト列の手動 free**: C は `free(o->bytes)` を GC スイープで手動実行。
//!    Rust は `Box<str>` のドロップが所有権で自動実行 → **二重 free / use-after-free /
//!    リークが構造的に起きない**。
//!
//! # 既知の近似（今後のフェーズ）
//!
//! - 現在は intern 経由で作られた文字列を**不滅**（GC 対象外）として扱う。C の
//!   `pin_mark`（コンパイル由来文字列の保護）と同型。実行時連結で生まれる文字列の
//!   回収は GC フェーズで導入する。

#![forbid(unsafe_code)]
#![warn(missing_docs)]

use std::collections::HashMap;

use crate::obj::{Obj, ObjId, ObjTable};

/// 文字列インターンキャッシュ。`HashMap<内容, ObjId>`（`ObjId` はヒープ上の
/// [`crate::obj::Obj::Str`] を指す）。文字列を所有せず、写像だけを持つ。
#[derive(Debug, Default)]
pub struct Interner {
    map: HashMap<Box<str>, ObjId>,
}

impl Interner {
    /// 空のインターンを作る。
    pub fn new() -> Self {
        Self::default()
    }

    /// 文字列を intern し、ヒープ上の文字列オブジェクト id を返す。
    /// 既にあれば既存 id（一意性）。新規は `heap` に [`Obj::Str`] を割り当てる。
    ///
    /// このメソッドが文字列をヒープに追加する唯一の経路（C の `akl_intern` 相当）。
    /// 失敗（ヒープ上限）は `None`。
    pub fn intern(&mut self, heap: &mut ObjTable, s: &str) -> Option<ObjId> {
        if let Some(&id) = self.map.get(s) {
            return Some(id);
        }
        let id = heap.alloc(Obj::Str(s.into())).ok()?;
        self.map.insert(s.into(), id);
        Some(id)
    }

    /// 内容 → id の逆引き（存在しなければ `None`）。
    pub fn lookup(&self, s: &str) -> Option<ObjId> {
        self.map.get(s).copied()
    }

    /// 登録済み文字列数。
    pub fn len(&self) -> usize {
        self.map.len()
    }

    /// 空か。
    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn intern_identity() {
        let mut heap = ObjTable::new();
        let mut it = Interner::new();
        let a = it.intern(&mut heap, "hello").unwrap();
        let b = it.intern(&mut heap, "hello").unwrap();
        assert_eq!(a, b, "同一内容は同一 id");
    }

    #[test]
    fn intern_distinct() {
        let mut heap = ObjTable::new();
        let mut it = Interner::new();
        let a = it.intern(&mut heap, "hello").unwrap();
        let b = it.intern(&mut heap, "world").unwrap();
        assert_ne!(a, b, "異なる内容は異なる id");
    }

    #[test]
    fn interned_is_heap_str() {
        let mut heap = ObjTable::new();
        let mut it = Interner::new();
        for s in ["", "a", "abc", "日本語", "TypeError"] {
            let id = it.intern(&mut heap, s).unwrap();
            match heap.get(id) {
                Some(Obj::Str(stored)) => assert_eq!(&**stored, s, "get(intern(s)) == s"),
                other => panic!("interned id は Obj::Str であるべき: {other:?}"),
            }
        }
    }

    #[test]
    fn lookup_works() {
        let mut heap = ObjTable::new();
        let mut it = Interner::new();
        let id = it.intern(&mut heap, "x").unwrap();
        assert_eq!(it.lookup("x"), Some(id));
        assert_eq!(it.lookup("y"), None);
    }

    #[test]
    fn len_tracks() {
        let mut heap = ObjTable::new();
        let mut it = Interner::new();
        assert!(it.is_empty());
        it.intern(&mut heap, "a").unwrap();
        it.intern(&mut heap, "b").unwrap();
        it.intern(&mut heap, "a").unwrap(); // 重複は増えない
        assert_eq!(it.len(), 2);
    }
}
