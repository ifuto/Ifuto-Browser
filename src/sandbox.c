/* 実装は sandbox.h のコメントを参照。seccomp-BPF の素朴な allowlist を
 * struct sock_filter の配列として手組みする（libseccomp 不使用）。
 * 数値はカーネル UAPI ヘッダ由来（<linux/seccomp.h> <linux/filter.h> <linux/audit.h>）。
 * syscall 番号は <sys/syscall.h> 経由で build ホストの libc 定義を使う
 * （x86-64 固定を arch チェックで強制し、番号表の跨ぎ適用を防ぐ）。 */
#define _GNU_SOURCE
#include "sandbox.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>

/* jump 計算: 条件一致で「配列末尾の統一出口」へ飛ばない（=次の個別処理へ落ちる）
 * 単純構造に留める。EQ(nr, allowed_i): 一致→jt=0（次命令=ALLOW ret）、不一致→jf=1（次の比較へ）
 * という 2 命令ペアの列にすると検証が自明になるが命令数が 2x。
 * ここは「EQ で飛び先 = ALLOW 位置」を固定長にする標準形で組む:
 *   各比較: BPF_JUMP(JEQ, nr, jt=fixed, jf=0) … 不一致は次の比較へ
 * ができない（jt は 8bit）ので、akl 用は規模が小さいため「比較+ALLOW」をペアにした
 * 素朴列で組み、末尾に KILL を置く（監査容易性優先。比較数は <=64）。 */

#define SB_ALLOW(nr_) do { \
    PROG[idx].code = BPF_JMP | BPF_JEQ | BPF_K; PROG[idx].jt = 0; PROG[idx].jf = 1; \
    PROG[idx].k = (uint32_t)(nr_); idx++; \
    PROG[idx].code = BPF_RET | BPF_K; PROG[idx].jt = 0; PROG[idx].jf = 0; \
    PROG[idx].k = SECCOMP_RET_ALLOW; idx++; \
} while (0)

/* 戻り値契約は sandbox.h */
int if_sandbox_apply(IfSandboxProfile profile) {
#if !defined(__x86_64__)
    (void)profile;
    return 1; /* arch 固定: 非 x86-64 は未対応を明示（跨ぎ番号表の誤適用を防ぐ） */
#else
    /* no_new_privs: 以後の execve 等で特権昇格不可にする前提条件 + filter 設置の必須フラグ */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return 1;
    /* core dump 抑止（事後の退避経路を塞ぐ）。負荷にならない定数処置 */
    struct rlimit zr; memset(&zr, 0, sizeof zr);
    setrlimit(RLIMIT_CORE, &zr);

    struct sock_filter PROG[6 + 2 * 96];
    uint32_t idx = 0;
    /* arch 検査（違うなら即 kill。表の跨ぎ適用は脆弱性化する） */
    PROG[idx].code = BPF_LD | BPF_W | BPF_ABS; PROG[idx].jt = 0; PROG[idx].jf = 0;
    PROG[idx].k = (uint32_t)offsetof(struct seccomp_data, arch); idx++;
    PROG[idx].code = BPF_JMP | BPF_JEQ | BPF_K; PROG[idx].jt = 1; PROG[idx].jf = 0;
    PROG[idx].k = (uint32_t)AUDIT_ARCH_X86_64; idx++;
    PROG[idx].code = BPF_RET | BPF_K; PROG[idx].jt = 0; PROG[idx].jf = 0;
    PROG[idx].k = SECCOMP_RET_KILL_PROCESS; idx++;
    /* nr 読込 */
    PROG[idx].code = BPF_LD | BPF_W | BPF_ABS; PROG[idx].jt = 0; PROG[idx].jf = 0;
    PROG[idx].k = (uint32_t)offsetof(struct seccomp_data, nr); idx++;

    /* ---- 共通: メモリ/シグナル/時刻/終了。実行権限昇格の無い領域 ---- */
    SB_ALLOW(__NR_read);
    SB_ALLOW(__NR_write);
    SB_ALLOW(__NR_writev);
    SB_ALLOW(__NR_brk);
    SB_ALLOW(__NR_mmap);
    SB_ALLOW(__NR_mprotect);
    SB_ALLOW(__NR_munmap);
    SB_ALLOW(__NR_mremap);
    SB_ALLOW(__NR_madvise);
    SB_ALLOW(__NR_rt_sigaction);
    SB_ALLOW(__NR_rt_sigprocmask);
    SB_ALLOW(__NR_rt_sigreturn);
    SB_ALLOW(__NR_clock_gettime);
    SB_ALLOW(__NR_getpid);
    SB_ALLOW(__NR_gettid);
    SB_ALLOW(__NR_futex);
    SB_ALLOW(__NR_close);
    SB_ALLOW(__NR_exit);
    SB_ALLOW(__NR_exit_group);
    SB_ALLOW(__NR_getrandom);
    SB_ALLOW(__NR_newfstatat);   /* stdio の buffering 判定（fstat 系） */
    SB_ALLOW(__NR_lseek);        /* stdio の seekable 判定 */
    SB_ALLOW(__NR_readv);
    SB_ALLOW(__NR_pread64);      /* 既オープン fd のみ（open は許可しない） */
    SB_ALLOW(__NR_getrusage);    /* --rss 観測（副作用なし） */

    if (profile == IF_SB_CHROME) {
        /* tty / X11 / SHM を最小追加: いずれも既接続 fd 上の操作のみ（open/connect 不許可） */
        SB_ALLOW(__NR_ioctl);        /* TIOCGWINSZ 等（v0.1。request 絞りは台帳） */
        SB_ALLOW(__NR_poll);
        SB_ALLOW(__NR_ppoll);
        SB_ALLOW(__NR_select);
        SB_ALLOW(__NR_nanosleep);
        SB_ALLOW(__NR_sendmsg);      /* X11 プロトコル（確立済ソケット） */
        SB_ALLOW(__NR_recvmsg);
        SB_ALLOW(__NR_getpeername);  /* 接続済み確認（診断用、副作用なし） */
        SB_ALLOW(__NR_shmget);       /* MIT-SHM バックバッファ */
        SB_ALLOW(__NR_shmat);
        SB_ALLOW(__NR_shmdt);
        SB_ALLOW(__NR_shmctl);
        SB_ALLOW(__NR_sched_yield);  /* 軽い待ち合わせ（busy loop を作らない規律） */
    }

    /* 既定: kill（allowlist 非一致）。errno による soft-fail は攻撃者に試行余地を与えるため採らない */
    PROG[idx].code = BPF_RET | BPF_K; PROG[idx].jt = 0; PROG[idx].jf = 0;
    PROG[idx].k = SECCOMP_RET_KILL_PROCESS; idx++;

    struct sock_fprog fp;
    fp.len = (unsigned short)idx;
    fp.filter = PROG;
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fp) != 0) return 1;
    return 0;
#endif
}

const char *if_sandbox_profile_name(IfSandboxProfile profile) {
    return profile == IF_SB_AKL ? "akl" : "chrome";
}
