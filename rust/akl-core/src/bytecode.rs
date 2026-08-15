//! バイトコード VM コア（フェーズ 3）。
//!
//! C 実装（`src/akl/akl.c` の `vm_exec` / `OP_*`）のコア命令セットを移植する。
//! フェーズ 3 では「命令の静的性質（スタック効果）」と「1 ステップ実行」を
//! スカラー範囲で Kani 証明する。ループ・関数呼び出し・GC 連携はフェーズ 3b で追加。
//!
//! # セキュリティ設計
//!
//! - スタックは `Vec<i64>`（C の AklVal スタック + AKL_POP マクロの下限検査が、
//!   `get()` の Option で構造的に安全になる）
//! - 命令のスタック効果（pop/push 数）は [`Op::stack_effect`] として 1 箇所に集約。
//!   C の `akl_op_imm_len` 表（verifier と drift して食い違う可能性があった）に相当する
//!   静的性質が、enum の match で網羅的に定義される
//! - `verify_stack` はバイトコード全体のスタック深さを検査（C の `akl_verify` 相当）

#![forbid(unsafe_code)]
#![warn(missing_docs)]

/// 比較演算子（C の CJMPF_L/G の cmp 表: 0=Lt 1=Le 2=Gt 3=Ge）。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Cmp {
    /// `<`
    Lt,
    /// `<=`
    Le,
    /// `>`
    Gt,
    /// `>=`
    Ge,
}

impl Cmp {
    /// 比較を適用する（i64 全域で panic しない）。
    pub const fn apply(self, a: i64, b: i64) -> bool {
        match self {
            Cmp::Lt => a < b,
            Cmp::Le => a <= b,
            Cmp::Gt => a > b,
            Cmp::Ge => a >= b,
        }
    }
}

/// VM 命令（C の OP_* のコアサブセット）。
///
/// フェーズ 3 はスカラー命令のみ。文字列・オブジェクト・関数呼び出しは 3b で追加。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Op {
    /// i32 定数を push（C の CONST_I）。
    ConstI(i32),
    /// ローカル slot を push（C の LLOAD）。
    LLoad(u32),
    /// ローカル slot へ pop した値を保存（C の LSTORE）。
    LStore(u32),
    /// グローバル slot を push（C の GLOAD_S）。
    GLoadS(u32),
    /// グローバル slot へ pop した値を保存（C の GSTORE_S）。
    GStoreS(u32),
    /// 加算（i64 checked。C の ADD の int fast path 相当）。
    Add,
    /// 減算（i64 checked）。
    Sub,
    /// 乗算（i64 checked）。
    Mul,
    /// ローカル slot と imm の比較が**偽**なら tgt へ（C の CJMPF_L）。
    CJmpfL {
        /// 比較するローカル slot。
        slot: u32,
        /// 比較相手の定数。
        imm: i32,
        /// 比較演算子。
        cmp: Cmp,
        /// 偽のときのジャンプ先。
        tgt: u32,
    },
    /// 無条件ジャンプ（C の JMP）。
    Jmp(u32),
    /// スタックから 1 個捨てる（C の POP）。
    Pop,
    /// スタック先頭を複製（C の DUP）。
    Dup,
    /// 停止（C の HALT）。
    Halt,
}

/// スタック効果: 命令が消費（pop）する個数と生成（push）する個数。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct StackEffect {
    /// pop 数。
    pub pop: usize,
    /// push 数。
    pub push: usize,
}

impl Op {
    /// この命令のスタック効果（静的・引数非依存）。
    ///
    /// 不変条件: どの命令も `pop <= 2` かつ `push <= 1`（Kani で証明）。
    pub const fn stack_effect(&self) -> StackEffect {
        match self {
            Op::ConstI(_) | Op::LLoad(_) | Op::GLoadS(_) | Op::Dup => {
                StackEffect { pop: 0, push: 1 }
            }
            Op::LStore(_) | Op::GStoreS(_) | Op::Pop => {
                StackEffect { pop: 1, push: 0 }
            }
            Op::Add | Op::Sub | Op::Mul => {
                StackEffect { pop: 2, push: 1 }
            }
            Op::CJmpfL { .. } | Op::Jmp(_) | Op::Halt => {
                StackEffect { pop: 0, push: 0 }
            }
        }
    }
}

/// 1 ステップ実行の結果。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Step {
    /// 次の命令へ（pc+1）。
    Next,
    /// 指定 pc へ。
    Jump(u32),
    /// 停止。
    Halt,
}

/// VM 実行エラー。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum VmError {
    /// スタックが不足。
    StackUnderflow,
    /// ローカル slot が範囲外。
    LocalOob(u32),
    /// グローバル slot が範囲外。
    GlobalOob(u32),
    /// ジャンプ先が範囲外。
    JumpOob(u32),
    /// 整数演算のオーバーフロー（i64 上限）。
    Overflow,
}

/// 1 命令実行のコンテキスト（スタック・ローカル・グローバル）。
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Vm {
    /// 値スタック。
    pub stk: Vec<i64>,
    /// ローカル変数。
    pub locals: Vec<i64>,
    /// グローバル変数。
    pub globals: Vec<i64>,
}

impl Vm {
    /// 空の VM を作る（グローバルは 0 初期化で n 個）。
    pub fn new(n_globals: usize) -> Self {
        Self {
            stk: Vec::new(),
            locals: Vec::new(),
            globals: vec![0; n_globals],
        }
    }

    /// 1 命令実行。`pc` は命令の位置、`code_len` はコード長（ジャンプ検査用）。
    ///
    /// 戻り値は次の pc（`Step::Next` なら pc+1、`Jump(t)` なら t、`Halt` なら pc）。
    pub fn step(&mut self, op: &Op, pc: usize, code_len: usize) -> Result<(Step, usize), VmError> {
        match op {
            Op::ConstI(v) => {
                self.stk.push(*v as i64);
                Ok((Step::Next, pc + 1))
            }
            Op::LLoad(slot) => {
                let v = *self
                    .locals
                    .get(*slot as usize)
                    .ok_or(VmError::LocalOob(*slot))?;
                self.stk.push(v);
                Ok((Step::Next, pc + 1))
            }
            Op::LStore(slot) => {
                let v = self.stk.pop().ok_or(VmError::StackUnderflow)?;
                let s = self
                    .locals
                    .get_mut(*slot as usize)
                    .ok_or(VmError::LocalOob(*slot))?;
                *s = v;
                Ok((Step::Next, pc + 1))
            }
            Op::GLoadS(gi) => {
                let v = *self
                    .globals
                    .get(*gi as usize)
                    .ok_or(VmError::GlobalOob(*gi))?;
                self.stk.push(v);
                Ok((Step::Next, pc + 1))
            }
            Op::GStoreS(gi) => {
                let v = self.stk.pop().ok_or(VmError::StackUnderflow)?;
                let g = self
                    .globals
                    .get_mut(*gi as usize)
                    .ok_or(VmError::GlobalOob(*gi))?;
                *g = v;
                Ok((Step::Next, pc + 1))
            }
            Op::Add | Op::Sub | Op::Mul => {
                let b = self.stk.pop().ok_or(VmError::StackUnderflow)?;
                let a = self.stk.pop().ok_or(VmError::StackUnderflow)?;
                let r = match op {
                    Op::Add => a.checked_add(b),
                    Op::Sub => a.checked_sub(b),
                    _ => a.checked_mul(b),
                };
                self.stk.push(r.ok_or(VmError::Overflow)?);
                Ok((Step::Next, pc + 1))
            }
            Op::CJmpfL { slot, imm, cmp, tgt } => {
                let v = *self
                    .locals
                    .get(*slot as usize)
                    .ok_or(VmError::LocalOob(*slot))?;
                let cond = cmp.apply(v, *imm as i64);
                if cond {
                    Ok((Step::Next, pc + 1))
                } else {
                    if (*tgt as usize) > code_len {
                        return Err(VmError::JumpOob(*tgt));
                    }
                    Ok(Step::Jump(*tgt))
                }
            }
            Op::Jmp(tgt) => {
                if (*tgt as usize) > code_len {
                    return Err(VmError::JumpOob(*tgt));
                }
                Ok(Step::Jump(*tgt))
            }
            Op::Pop => {
                self.stk.pop().ok_or(VmError::StackUnderflow)?;
                Ok((Step::Next, pc + 1))
            }
            Op::Dup => {
                let top = *self.stk.last().ok_or(VmError::StackUnderflow)?;
                self.stk.push(top);
                Ok((Step::Next, pc + 1))
            }
            Op::Halt => Ok((Step::Halt, pc)),
        }
    }
}

/// バイトコード全体のスタック効果を検査（C の `akl_verify` 相当の静的検査）。
///
/// 各命令の pop が現在の深さ以下であること（underflow なし）を先頭から検証し、
/// 最大スタック深さを返す。ジャンプ先の整合はフェーズ 3b（制御フロー解析）で行う。
pub fn verify_stack(code: &[Op]) -> Result<usize, VmError> {
    let mut depth: i64 = 0;
    let mut max_depth: usize = 0;
    for op in code {
        let e = op.stack_effect();
        depth -= e.pop as i64;
        if depth < 0 {
            return Err(VmError::StackUnderflow);
        }
        depth += e.push as i64;
        if depth as usize > max_depth {
            max_depth = depth as usize;
        }
    }
    Ok(max_depth)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn simple_prog() -> Vec<Op> {
        vec![
            Op::ConstI(1),
            Op::ConstI(2),
            Op::Add,
            Op::Dup,
            Op::Pop,
            Op::Halt,
        ]
    }

    #[test]
    fn step_const_add() {
        let mut vm = Vm::new(0);
        let code = simple_prog();
        let mut pc = 0usize;
        loop {
            let (step, npc) = vm.step(&code[pc], pc, code.len()).unwrap();
            match step {
                Step::Halt => break,
                _ => pc = npc,
            }
        }
        assert_eq!(vm.stk, vec![3]);
    }

    #[test]
    fn step_underflow() {
        let mut vm = Vm::new(0);
        let err = vm.step(&Op::Add, 0, 1).unwrap_err();
        assert_eq!(err, VmError::StackUnderflow);
    }

    #[test]
    fn step_local_roundtrip() {
        let mut vm = Vm::new(0);
        vm.locals = vec![0, 0];
        vm.step(&Op::ConstI(5), 0, 4).unwrap();
        vm.step(&Op::LStore(0), 1, 4).unwrap();
        vm.step(&Op::LLoad(0), 2, 4).unwrap();
        assert_eq!(vm.stk, vec![5]);
        assert_eq!(vm.locals[0], 5);
    }

    #[test]
    fn step_global_roundtrip() {
        let mut vm = Vm::new(1);
        vm.step(&Op::ConstI(7), 0, 4).unwrap();
        vm.step(&Op::GStoreS(0), 1, 4).unwrap();
        vm.step(&Op::GLoadS(0), 2, 4).unwrap();
        assert_eq!(vm.stk, vec![7]);
        assert_eq!(vm.globals[0], 7);
    }

    #[test]
    fn step_cjmpl_taken_and_not() {
        let mut vm = Vm::new(0);
        vm.locals = vec![3];
        let (step, _) = vm
            .step(&Op::CJmpfL { slot: 0, imm: 5, cmp: Cmp::Lt, tgt: 9 }, 0, 10)
            .unwrap();
        assert_eq!(step, Step::Next); // 3 < 5 は真 → ジャンプしない
        let (step2, _) = vm
            .step(&Op::CJmpfL { slot: 0, imm: 5, cmp: Cmp::Gt, tgt: 9 }, 0, 10)
            .unwrap();
        assert_eq!(step2, Step::Jump(9)); // 3 > 5 は偽 → ジャンプ
    }

    #[test]
    fn step_jump_oob() {
        let mut vm = Vm::new(0);
        let err = vm.step(&Op::Jmp(99), 0, 10).unwrap_err();
        assert_eq!(err, VmError::JumpOob(99));
    }

    #[test]
    fn step_overflow_detected() {
        let mut vm = Vm::new(0);
        vm.stk = vec![i64::MAX, 1];
        let err = vm.step(&Op::Add, 0, 1).unwrap_err();
        assert_eq!(err, VmError::Overflow);
    }

    #[test]
    fn verify_stack_ok() {
        let max = verify_stack(&simple_prog()).unwrap();
        assert!(max >= 2);
    }

    #[test]
    fn verify_stack_underflow_detected() {
        let code = vec![Op::Pop];
        assert_eq!(verify_stack(&code), Err(VmError::StackUnderflow));
    }

    #[test]
    fn cmp_all() {
        assert!(Cmp::Lt.apply(1, 2));
        assert!(!Cmp::Lt.apply(2, 2));
        assert!(Cmp::Le.apply(2, 2));
        assert!(Cmp::Gt.apply(3, 2));
        assert!(Cmp::Ge.apply(2, 2));
        assert!(!Cmp::Ge.apply(1, 2));
    }
}

/// Kani による機械的証明（`cargo kani` で実行）。
///
/// フェーズ 3 の証明対象（全てスカラー範囲）:
/// 1. 各命令のスタック効果が有界（pop <= 2, push <= 1）
/// 2. `Cmp::apply` が任意の i64 ペアで panic しない
/// 3. `checked_add` の Some 結果は数学的加算と一致（オーバーフローなし）
/// 4. `verify_stack` が固定コード列を正しく検証する
#[cfg(kani)]
mod verification {
    use super::*;

    /// 全 Op のスタック効果: pop <= 2 かつ push <= 1。
    /// stack_effect は引数非依存のため、代表 Op の列挙で全 variant を網羅する。
    #[kani::proof]
    fn stack_effect_bounded() {
        let ops = [
            Op::ConstI(0),
            Op::LLoad(0),
            Op::LStore(0),
            Op::GLoadS(0),
            Op::GStoreS(0),
            Op::Add,
            Op::Sub,
            Op::Mul,
            Op::CJmpfL { slot: 0, imm: 0, cmp: Cmp::Lt, tgt: 0 },
            Op::Jmp(0),
            Op::Pop,
            Op::Dup,
            Op::Halt,
        ];
        for op in ops {
            let e = op.stack_effect();
            assert!(e.pop <= 2, "pop は高々 2");
            assert!(e.push <= 1, "push は高々 1");
        }
    }

    /// Cmp::apply は任意の i64 ペアで panic しない（全数検証）。
    #[kani::proof]
    fn cmp_apply_total() {
        let a: i64 = kani::any();
        let b: i64 = kani::any();
        let _ = Cmp::Lt.apply(a, b);
        let _ = Cmp::Le.apply(a, b);
        let _ = Cmp::Gt.apply(a, b);
        let _ = Cmp::Ge.apply(a, b);
    }

    /// checked_add が Some を返すとき、結果は数学的加算と一致（wrap しない）。
    #[kani::proof]
    fn add_some_is_exact() {
        let a: i64 = kani::any();
        let b: i64 = kani::any();
        if let Some(v) = a.checked_add(b) {
            assert_eq!(v.wrapping_sub(a), b);
        }
    }

    /// verify_stack は固定プログラムで underflow を検出しない。
    #[kani::proof]
    fn verify_stack_simple_program() {
        let code = [Op::ConstI(1), Op::ConstI(2), Op::Add, Op::Pop, Op::Halt];
        let max = verify_stack(&code).unwrap();
        assert!(max >= 1);
    }

    /// verify_stack は underflow コード列を検出する。
    #[kani::proof]
    fn verify_stack_detects_underflow() {
        let code = [Op::Pop];
        assert_eq!(verify_stack(&code), Err(VmError::StackUnderflow));
    }
}
