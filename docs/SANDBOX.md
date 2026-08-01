# SANDBOX.md — Ifuto サンドボックスの脅威モデルと境界（唯一の正）

## 脅威モデル

リモート由来コンテンツ（HTML/CSS/MD/JS）の解釈中のバグ（UAF・overflow・パーサ欠陥）を
踏んだ攻撃者が **「次に何ができるか」** をカーネル強制で削る。
post-exploit 動線 — ファイルを開く・ソケットを作る・子プロセスを作る・ptrace・execve —
を **構造的に遮断** する。未知の攻撃にも効く allowlist 方式（予測不能な副作用を持たない）。

## 実装（src/sandbox.c、libseccomp 不使用・依存 libc のみ）

- 自作 seccomp-BPF。`prctl(PR_SET_NO_NEW_PRIVS)` + `RLIMIT_CORE=0` の後、不可逆 filter を設置。
- arch を AUDIT_ARCH_X86_64 に固定（跨ぎでは拒否。跨ぎ番号表の誤適用は脆弱性化する）。
- allowlist 非一致は `SECCOMP_RET_KILL_PROCESS`（fail-stop。errno soft-fail は攻撃者に
  試行余地を与えるため採らない。エンジンの budget 打切りと同じ安全側原理）。
- 共通 allowlist: read/write/writev/readv/pread64/brk/mmap/mprotect/munmap/mremap/madvise、
  rt_sigaction/rt_sigprocmask/rt_sigreturn、clock_gettime/getpid/gettid/futex/close、
  exit/exit_group/getrandom/newfstatat/lseek/getrusage。
- IF_SB_AKL: 上記のみ + **ioctl は `request==TCGETS` 単独に限定許可**（glibc stdio が
  stdout/stderr が文字デバイス時に isatty 判定で投げる 1 命令のみ。副作用なし。
  それ以外の ioctl — TCSETS 等 — は従来通り kill。2026-08-01 実測: nr=16 SIGSYS を同定し
  この限定許可で解消、回帰は tests/akl_cli_smoke.py が機械監視）。
- IF_SB_CHROME: 共通 + ioctl/poll/ppoll/select/nanosleep、X11 用 sendmsg/recvmsg/getpeername、
  MIT-SHM（shmget/shmat/shmdt/shmctl）、sched_yield。**chrome への適用は v0.2 台帳**
  （現行は適用していない。primitive のみ実装済み・IF_SB_AKL での強制適用で実戦検証済）。

## 製品への配線（現行）

- `build/akl`（akl 単体ランナー）は **既定 ON で強制適用**: ファイルslurp後に適用し、
  以後の評価・実行は filter の内側。`--no-sandbox` のみ明示解除（デバッグ用）。
- 非対応カーネル/arch では **rc=2 で終了**（fail-stop。黙って素通ししない）。
- 検証: TCGETS 生存 / TCSETS 死亡（rc=159=128+SIGSYS）/ open() 死亡 を probe で実測。

## 正直な境界（v0.1、誇張しない欄）

- **プロセス単位** の filter。タブ単位の「住所の壁」（プロセス分離による GPU/タブ隔離）
  は v0.4 計画。現行は「タブ処理を行うプロセス全体」を強く閉じ込める形であり、
  タブ A のバグからタブ B のメモリは守らない（これを守ると言うつもりはない）。
- mmap/mprotect の実行権限昇格検査までは行わない（JIT を持たない構造上、RWX が新規
  生成される経路が存在しない。W^X は OS が担保）。
- ブラウザ本体（TUI/GUI プロセス）への IF_SB_CHROME 適用は未配線（v0.2 台帳）。
  tty 入力・X11・SHM を必要とするため、IF_SB_AKL より許可面が広いことは認める。
