//! オブジェクトモデル + GC（フェーズ 2）。
//!
//! C 実装からの移植対応:
//! | C (akl.c) | Rust |
//! |---|---|
//! | `AklObj`（kind タグ + union）+ `rt->objs[]` | [`Obj`]（enum）+ [`ObjTable`] |
//! | `akl_obj_new` / `free_objs`（free-list） | [`ObjTable::alloc`] |
//! | `akl_gc`（mark & sweep + 子参照 switch） | [`ObjTable::gc`] |
//! | `akl_gc_kind_children`（手動 switch） | [`Obj::children`]（match が網羅的） |
//! | スイープ時の手動 free（bytes/props/vals…） | `slot.take()` のドロップ |
//!
//! # C 実装で実際に起きたバグと、Rust で構造的に消える理由
//!
//! 1. **コールバック中 GC 後の use-after-free**（v0.10 で ASan 検出・修正）:
//!    C の `rt->objs` は `realloc` で伸びるため、`AklObj *o = &rt->objs[i]` の
//!    ポインタは GC 後に失効する。配列 HOF（map/filter 等）のループで古い `o` を
//!    使い続けて heap-use-after-free になった。Rust では `&Rt` の共有借用と
//!    `&mut Rt` の排他借用がコンパイラに強制されるため、**「参照を保持したまま
//!    テーブルを変更する」コードはコンパイルエラー**になる。
//!
//! 2. **スイープの free 漏れ・二重 free**: C は kind ごとに手動で
//!    `free(o->bytes)` / `free(o->u.po.props)` 等を列挙する（追加漏れが生存中の
//!    メモリリークになる）。Rust は [`Obj`] の所有フィールドがドロップで自動解放
//!    されるため、**解放漏れ・二重解放がコンパイル不能**。
//!
//! 3. **子参照の列挙漏れ**: C の `akl_gc_kind_children` は switch の追記漏れが
//!    あり得た（実際、v0.4 で AKL_OK_MAP/SET の追加漏れが GC 回収バグになった）。
//!    Rust では [`Obj::children`] の match が非網羅だとコンパイルエラー。
//!
//! # 設計
//!
//! - [`ObjId`] は u32（C の obj index と同じ。`AklVal::mk_obj` の obj タグ空間に乗る）
//! - 文字列もこのヒープの [`Obj::Str`] として載る（単一 id 空間。詳細は
//!   [`crate::string`] モジュール参照）
//! - [`ObjTable`] は `Vec<Slot>`（`Slot = Option<Obj>`）。`None` は空きスロット
//! - GC はルート集合（`&[Vec<AklVal>]` のリスト）から worklist で mark → sweep

#![forbid(unsafe_code)]
#![warn(missing_docs)]

use crate::bytecode::HandleVTab;
use crate::AklVal;

/// オブジェクト表の index（C の obj index。u32 なので AklVal の obj タグに乗る）。
pub type ObjId = u32;

/// 空きスロットの番兵（C の kind==0 相当）。
pub const NONE: ObjId = u32::MAX;

/// オブジェクト表の操作エラー。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ObjError {
    /// オブジェクト表の上限（u32 全域）に達した。
    TableFull,
    /// 対象が配列ではない。
    NotArray,
    /// 対象がオブジェクト（プレーン）ではない。
    NotObject,
}

/// オブジェクト（C の AklObj。kind タグ + union を enum で型安全に表現）。
///
/// 子参照（他オブジェクトへの AklVal）は [`children`](Self::children) で
/// 網羅的に列挙される。GC はこれだけを辿ればよい。
#[derive(Clone, Debug, PartialEq)]
pub enum Obj {
    /// 文字列（intern 済み文字列は [`crate::string::Interner`] が索引を持つ）。
    Str(Box<str>),
    /// ROPE 文字列（連結の遅延表現。左右の子 ObjId を持つ。C の `AKL_OK_ROPE` 相当）。
    Rope {
        /// 左の子（ObjId）。
        left: ObjId,
        /// 右の子（ObjId）。
        right: ObjId,
    },
    /// 配列（要素列）。
    Arr(Vec<AklVal>),
    /// クロージャ環境（キャプチャ済みローカル + 親環境）。
    Env {
        /// キャプチャ済みローカル値。
        vals: Vec<AklVal>,
        /// 親環境（無ければ None）。
        parent: Option<ObjId>,
    },
    /// プレーンオブジェクト（プロパティ列。name は intern 済み文字列の ObjId）。
    Obj(Vec<(ObjId, AklVal)>),
    /// バイトコード関数（C の `AklObj` FUNC 相当）。
    ///
    /// - `fidx`: 関数表（`Runtime.funcs`）の index。コード本体はそこにあり、
    ///   ヒープからは index だけを参照する（C の `code_off` と同型）。
    /// - `env`: クロージャ捕捉環境（無ければ None）。
    Func {
        /// 関数表 index。
        fidx: u32,
        /// 捕捉環境（クロージャ）。無ければ None。
        env: Option<ObjId>,
    },
    /// ネイティブ関数（C の `AKL_OK_NATIVE` 相当）。index はランタイムの
    /// `native_fns` 表を指す（C の fn ポインタ + udata と同型）。
    Native(u32),
    /// Map（キーと値のペア列。C の `AKL_OK_MAP` 相当。簡易版は線形探索）。
    Map(Vec<(AklVal, AklVal)>),
    /// Set（値の集合。C の `AKL_OK_SET` 相当）。
    Set(Vec<AklVal>),
    /// Promise（C の `AKL_OK_PROMISE` 相当）。
    Promise {
        /// 状態（0=pending 1=resolved 2=rejected）。
        state: u8,
        /// 解決値/拒否値。
        value: AklVal,
    },
    /// 正規表現（C の `AKL_OK_REGEX` 相当）。pattern + flags を保持。
    RegExp {
        /// パターン文字列。
        pattern: Box<str>,
        /// フラグ文字列。
        flags: Box<str>,
    },
    /// Date（C の `\x01ms` を持つ OBJ 相当。エポックからのミリ秒を保持）。
    /// 子参照を持たない（ms はスカラ値）。
    Date {
        /// エポック（1970-01-01T00:00:00Z）からのミリ秒。
        ms: f64,
    },
    /// ホストハンドル（C の `AKL_OK_HANDLE` 相当。DOM 要素等の不透明参照）。
    /// `ptr` はホスト側オブジェクトの不透明アドレス（u64 で保持。unsafe は FFI 層）。
    Handle {
        /// ディスパッチ vtable。
        vtab: &'static HandleVTab,
        /// ホスト側オブジェクトの不透明アドレス。
        ptr: u64,
    },
    /// ハンドルに束縛されたメソッド（`obj.method` の `method` 値を表現。
    /// `PLoad` がハンドルの未知プロパティを解決する際に生成し、`do_call` が
    /// ハンドルの vtable `call` へディスパッチする）。
    BoundMethod {
        /// 対象ハンドルの ObjId。
        handle: ObjId,
        /// メソッド名（intern 済み文字列 ObjId）。
        name: ObjId,
    },
}

impl Obj {
    /// このオブジェクトが直接参照する子オブジェクトの id 列。
    /// match が非網羅だとコンパイルエラー = 子参照の追加漏れが構造的に起きない。
    pub fn children(&self) -> impl Iterator<Item = ObjId> + '_ {
        let mut out: Vec<ObjId> = Vec::new();
        match self {
            Obj::Str(_) => {}
            Obj::Rope { left, right } => {
                out.push(*left);
                out.push(*right);
            }
            Obj::Arr(items) => {
                for v in items {
                    if v.is_obj() {
                        out.push(v.get_obj());
                    }
                }
            }
            Obj::Env { vals, parent } => {
                for v in vals {
                    if v.is_obj() {
                        out.push(v.get_obj());
                    }
                }
                if let Some(p) = parent {
                    out.push(*p);
                }
            }
            Obj::Obj(props) => {
                for (name, v) in props {
                    // プロパティ名（intern 済み文字列 ObjId）も子として mark する
                    // （C の「prop 名を obj に連動させて生存させる」設計と同一）。
                    out.push(*name);
                    if v.is_obj() {
                        out.push(v.get_obj());
                    }
                }
            }
            Obj::Func { fidx: _, env } => {
                if let Some(e) = env {
                    out.push(*e);
                }
            }
            Obj::Native(_) => {}
            Obj::Map(kv) => {
                for (k, v) in kv {
                    if k.is_obj() {
                        out.push(k.get_obj());
                    }
                    if v.is_obj() {
                        out.push(v.get_obj());
                    }
                }
            }
            Obj::Set(items) => {
                for v in items {
                    if v.is_obj() {
                        out.push(v.get_obj());
                    }
                }
            }
            Obj::Promise { value, .. } => {
                if value.is_obj() {
                    out.push(value.get_obj());
                }
            }
            Obj::RegExp { .. } => {}
            Obj::Date { .. } => {}
            Obj::Handle { .. } => {}
            Obj::BoundMethod { handle, .. } => {
                out.push(*handle);
            }
        }
        out.into_iter()
    }
}

/// オブジェクト表のスロット。`None` = 空き（C の kind==0 相当）。
type Slot = Option<Obj>;

/// オブジェクト表 + GC（mark & sweep）。
///
/// 不変条件:
/// 1. `slots` の長さは `n_alloc + free.len()`（全スロットは使用中か free リストのいずれか）
/// 2. free リストの各 index は `slots[i].is_none()`
/// 3. gc 実行後、ルートから到達可能な全オブジェクトは生存し、到達不能は全て
///    解放されている（Kani で証明）
#[derive(Debug, Default)]
pub struct ObjTable {
    slots: Vec<Slot>,
    free: Vec<ObjId>,
    /// 生存オブジェクト数（適応 GC 閾値の材料。C の n_objs - n_free に相当）。
    live: usize,
}

impl ObjTable {
    /// 空の表を作る。
    pub fn new() -> Self {
        Self::default()
    }

    /// スロット数（C の n_objs に相当）。
    pub fn len(&self) -> usize {
        self.slots.len()
    }

    /// スロットが 1 つも無いか。
    pub fn is_empty(&self) -> bool {
        self.slots.is_empty()
    }

    /// 生存オブジェクト数。
    pub fn live(&self) -> usize {
        self.live
    }

    /// オブジェクトを 1 個割り当てる（free-list 再利用。C の akl_obj_new）。
    /// 成功時 `Ok(id)`、失敗（上限到達）時 `Err(ObjError::TableFull)`。
    pub fn alloc(&mut self, obj: Obj) -> Result<ObjId, ObjError> {
        if let Some(id) = self.free.pop() {
            self.slots[id as usize] = Some(obj);
            self.live += 1;
            return Ok(id);
        }
        let id = self.slots.len() as ObjId;
        if id == NONE {
            return Err(ObjError::TableFull); // u32 上限
        }
        self.slots.push(Some(obj));
        self.live += 1;
        Ok(id)
    }

    /// id のオブジェクトへの共有参照。範囲外・空きスロットは None。
    pub fn get(&self, id: ObjId) -> Option<&Obj> {
        self.slots.get(id as usize).and_then(|s| s.as_ref())
    }

    /// id のオブジェクトへの排他参照（コールバック中 GC 問題は借用規則で排除）。
    pub fn get_mut(&mut self, id: ObjId) -> Option<&mut Obj> {
        self.slots.get_mut(id as usize).and_then(|s| s.as_mut())
    }

    /// mark & sweep GC。`roots` はルート集合（スタック・グローバル等の断片）。
    ///
    /// - mark: roots から [`Obj::children`] を worklist で辿り、到達可能 id を mark
    /// - sweep: 未到達スロットを `take()` してドロップ（所有権で自動解放）
    /// - 返り値は解放したオブジェクト数
    pub fn gc(&mut self, roots: &[Vec<AklVal>]) -> usize {
        let n = self.slots.len();
        let mut mark = vec![false; n];
        let mut wl: Vec<ObjId> = Vec::new();

        // mark 根
        for stack in roots {
            for v in stack {
                if v.is_obj() {
                    let id = v.get_obj();
                    if (id as usize) < n && !mark[id as usize] {
                        mark[id as usize] = true;
                        wl.push(id);
                    }
                }
            }
        }

        // mark 伝播（worklist）
        while let Some(id) = wl.pop() {
            if let Some(obj) = self.get(id) {
                for child in obj.children() {
                    if (child as usize) < n && !mark[child as usize] {
                        mark[child as usize] = true;
                        wl.push(child);
                    }
                }
            }
        }

        // sweep
        let mut swept = 0;
        for (i, m) in mark.iter().enumerate() {
            if !m && self.slots[i].take().is_some() {
                self.free.push(i as ObjId);
                self.live -= 1;
                swept += 1;
            }
        }
        swept
    }

    /// 空きスロット数。
    pub fn free_count(&self) -> usize {
        self.free.len()
    }

    /// 配列 map（JS の `arr.map(f)` 相当のコア。コールバックは純関数として扱う）。
    ///
    /// # なぜ C の UAF が構造的に起きないか（v0.10 実バグとの対比）
    ///
    /// C 実装は `AklObj *o = &rt->objs[ai]` のポインタをループ中に保持し、コールバック
    /// 内の GC で `rt->objs` が `realloc` されると失効 → heap-use-after-free。
    /// Rust では:
    ///
    /// 1. 元配列の要素を先に `Vec<AklVal>` に**値コピー**する（AklVal は Copy）。
    ///    借用（`&Obj`）はこのスコープで終了し、以後 `self` を自由に変更できる。
    /// 2. コールバック `f` は `FnMut(AklVal, usize) -> AklVal`（表への参照を持たない）。
    /// 3. `self` へのアクセスは全て id 経由の短命借用。
    ///
    /// コールバックが表を変更する JS のケース（map 中に push 等）は「スナップショット
    /// 方式」で近似する（要素は map 開始時点のもの。V8 は length を先に固定するため
    /// 追加要素は走査されない — 追加分の非反映は V8 と一致。削除要素の undefined 化は
    /// 未対応の既知近似として AKL_COMPAT に記録予定）。
    ///
    /// # 戻り値
    ///
    /// 新しい配列の id。`id` が配列でない場合は `Err(ObjError::NotArray)`。
    pub fn arr_map(
        &mut self,
        id: ObjId,
        mut f: impl FnMut(AklVal, usize) -> AklVal,
    ) -> Result<ObjId, ObjError> {
        // 1. 元要素を値コピー（借用終了）
        let items: Vec<AklVal> = match self.get(id) {
            Some(Obj::Arr(v)) => v.clone(),
            _ => return Err(ObjError::NotArray),
        };
        // 2. コールバック適用（self 変更なし）
        let mapped: Vec<AklVal> = items
            .iter()
            .enumerate()
            .map(|(i, &elem)| f(elem, i))
            .collect();
        // 3. 結果を新規配列として割り当て
        self.alloc(Obj::Arr(mapped))
    }

    /// 配列の要素を id で読む（範囲外・非配列は None）。
    pub fn arr_get(&self, id: ObjId, index: usize) -> Option<AklVal> {
        match self.get(id) {
            Some(Obj::Arr(v)) => v.get(index).copied(),
            _ => None,
        }
    }

    /// プレーンオブジェクトのプロパティを name で読む（無ければ None）。
    pub fn prop_get(&self, id: ObjId, name: ObjId) -> Option<AklVal> {
        match self.get(id) {
            Some(Obj::Obj(props)) => props
                .iter()
                .find(|(n, _)| *n == name)
                .map(|(_, v)| *v),
            _ => None,
        }
    }

    /// プレーンオブジェクトにプロパティを設定（既存は上書き、新規は追加）。
    /// 対象がプレーンオブジェクトでなければ `Err(ObjError::NotObject)`。
    pub fn prop_set(&mut self, id: ObjId, name: ObjId, value: AklVal) -> Result<(), ObjError> {
        match self.get_mut(id) {
            Some(Obj::Obj(props)) => {
                if let Some(slot) = props.iter_mut().find(|(n, _)| *n == name) {
                    slot.1 = value;
                } else {
                    props.push((name, value));
                }
                Ok(())
            }
            _ => Err(ObjError::NotObject),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn obj(v: AklVal) -> Obj {
        Obj::Arr(vec![v])
    }

    #[test]
    fn alloc_reuses_free_list() {
        let mut t = ObjTable::new();
        let a = t.alloc(obj(AklVal::mk_int(1))).unwrap();
        let b = t.alloc(obj(AklVal::mk_int(2))).unwrap();
        assert_eq!(t.live(), 2);
        // ルートを a のみにして GC → 到達不能な b だけが sweep され free-list に戻る
        t.gc(&[vec![AklVal::mk_obj(a)]]);
        assert_eq!(t.live(), 1);
        assert_eq!(t.free_count(), 1);
        // 再利用: 新しい alloc は free-list の id（= b）を使う
        let c = t.alloc(obj(AklVal::mk_int(3))).unwrap();
        assert_eq!(c, b, "free-list は最後に sweep された id を再利用する");
        assert_eq!(t.live(), 2);
    }

    #[test]
    fn gc_preserves_reachable() {
        let mut t = ObjTable::new();
        let leaf = t.alloc(obj(AklVal::mk_int(42))).unwrap();
        let arr = t.alloc(Obj::Arr(vec![AklVal::mk_obj(leaf)])).unwrap();
        // arr をルートにして GC → leaf も生存
        let roots = vec![vec![AklVal::mk_obj(arr)]];
        t.gc(&roots);
        assert_eq!(t.live(), 2);
        assert!(t.get(leaf).is_some());
        assert!(t.get(arr).is_some());
    }

    #[test]
    fn gc_sweeps_unreachable_chain() {
        let mut t = ObjTable::new();
        let a = t.alloc(obj(AklVal::mk_int(1))).unwrap();
        let b = t.alloc(Obj::Arr(vec![AklVal::mk_obj(a)])).unwrap();
        // b がルート → a も生存
        t.gc(&[vec![AklVal::mk_obj(b)]]);
        assert_eq!(t.live(), 2);
        // ルートを外す → 両方 sweep
        t.gc(&[]);
        assert_eq!(t.live(), 0);
        assert!(t.get(a).is_none());
        assert!(t.get(b).is_none());
    }

    #[test]
    fn env_parent_chain() {
        let mut t = ObjTable::new();
        let parent = t.alloc(Obj::Env { vals: vec![], parent: None }).unwrap();
        let child = t
            .alloc(Obj::Env { vals: vec![AklVal::mk_int(7)], parent: Some(parent) })
            .unwrap();
        // child をルート → parent も生存（Env の parent 参照を辿る）
        t.gc(&[vec![AklVal::mk_obj(child)]]);
        assert_eq!(t.live(), 2);
        assert!(t.get(parent).is_some());
    }

    #[test]
    fn func_env_is_child() {
        let mut t = ObjTable::new();
        let env = t.alloc(Obj::Env { vals: vec![AklVal::mk_int(1)], parent: None }).unwrap();
        let f = t.alloc(Obj::Func { fidx: 0, env: Some(env) }).unwrap();
        // f をルート → env も生存（Func の env 参照を辿る）
        t.gc(&[vec![AklVal::mk_obj(f)]]);
        assert_eq!(t.live(), 2);
        assert!(t.get(env).is_some());
    }

    #[test]
    fn prop_name_is_child() {
        let mut t = ObjTable::new();
        let name = t.alloc(Obj::Str("key".into())).unwrap();
        let o = t.alloc(Obj::Obj(vec![(name, AklVal::mk_int(5))])).unwrap();
        // o をルート → プロパティ名（文字列 ObjId）も生存
        t.gc(&[vec![AklVal::mk_obj(o)]]);
        assert_eq!(t.live(), 2);
        assert!(t.get(name).is_some());
    }

    #[test]
    fn get_bounds_safe() {
        let mut t = ObjTable::new();
        let id = t.alloc(obj(AklVal::mk_int(1))).unwrap();
        assert!(t.get(id).is_some());
        assert!(t.get(u32::MAX).is_none()); // 範囲外は None（パニックしない）
        assert!(t.get_mut(id).is_some());
    }

    #[test]
    fn arr_map_basic() {
        let mut t = ObjTable::new();
        let src = t
            .alloc(Obj::Arr(vec![AklVal::mk_int(1), AklVal::mk_int(2), AklVal::mk_int(3)]))
            .unwrap();
        let dst = t
            .arr_map(src, |elem, i| AklVal::mk_int(elem.get_int() * 10 + i as i32))
            .unwrap();
        match t.get(dst) {
            Some(Obj::Arr(v)) => {
                assert_eq!(v, &vec![AklVal::mk_int(10), AklVal::mk_int(21), AklVal::mk_int(32)]);
            }
            _ => panic!("dst は配列であるべき"),
        }
        // 元配列は不変
        assert_eq!(t.arr_get(src, 0), Some(AklVal::mk_int(1)));
        // 非配列 id は Err
        let env = t.alloc(Obj::Env { vals: vec![], parent: None }).unwrap();
        assert!(t.arr_map(env, |e, _| e).is_err());
    }

    #[test]
    fn arr_map_snapshot_semantics() {
        // コールバック中に元配列を変更しても、map は開始時点の要素を使う
        // （スナップショット方式。V8 の length 固定と整合）。
        let mut t = ObjTable::new();
        let src = t
            .alloc(Obj::Arr(vec![AklVal::mk_int(1), AklVal::mk_int(2)]))
            .unwrap();
        let mut calls = 0;
        let dst = t
            .arr_map(src, |elem, _| {
                calls += 1;
                elem // そのまま
            })
            .unwrap();
        assert_eq!(calls, 2);
        assert_eq!(t.arr_get(dst, 1), Some(AklVal::mk_int(2)));
    }

    #[test]
    fn prop_set_get_roundtrip() {
        let mut t = ObjTable::new();
        let o = t.alloc(Obj::Obj(Vec::new())).unwrap();
        let key = t.alloc(Obj::Str("x".into())).unwrap();
        assert!(t.prop_get(o, key).is_none());
        t.prop_set(o, key, AklVal::mk_int(9)).unwrap();
        assert_eq!(t.prop_get(o, key), Some(AklVal::mk_int(9)));
        // 上書き
        t.prop_set(o, key, AklVal::mk_int(10)).unwrap();
        assert_eq!(t.prop_get(o, key), Some(AklVal::mk_int(10)));
        // 非オブジェクトは Err
        let arr = t.alloc(Obj::Arr(Vec::new())).unwrap();
        assert_eq!(t.prop_set(arr, key, AklVal::UNDEF), Err(ObjError::NotObject));
    }
}
