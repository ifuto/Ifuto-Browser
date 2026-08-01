/* Ifuto — プロセスサンドボックス（seccomp-BPF 自作。libseccomp 不使用）
 *
 * 脅威モデル（docs/SANDBOX.md が唯一の正）:
 *   リモート由来コンテンツの解釈中のバグ（UAF/overflow）を踏んだ攻撃者が
 *   「何ができるか」をカーネル強制で削る。ファイルを開く・ソケットを作る・
 *   子プロセスを作る・ptrace・execve 等の post-exploit 動線を **構造的に遮断**
 *   する（未知の攻撃にも効く = 予測不能な副作用を持たない allowlist 方式）。
 *
 * 設計規律:
 *   - allowlist に無い syscall は SECCOMP_RET_KILL_PROCESS（fail-stop。
 *     errno で騙さない。エンジンの budget 打ち切りと同じ安全側原理）
 *   - アーキテクチャ固定（AUDIT_ARCH_X86_64 のみ受理。跨ぎ syscall 表の誤適用を防ぐ）
 *   - 適用は不可逆（no_new_privs + filter は解除不能。逃逸経路を持たせない）
 *   - 依存は libc のみ（libseccomp 不使用。製品 ldd 不変条件を侵さない）
 *
 * v0.1 の境界（正直な台帳）:
 *   - プロセス単位のフィルタ。タブ単位の「住所の壁」（プロセス分離）は
 *     docs/SANDBOX.md の v0.4 計画欄。v0.1 は「タブ処理を行うプロセス全体」を
 *     強く閉じ込める形
 *   - mmap/mprotect は実行権限の昇格検査までは行わない（JIT を持たない構造上、
 *     RWX が新規生成される経路が無い。W^X は OS が担保）
 */
#ifndef IFUTO_SANDBOX_H
#define IFUTO_SANDBOX_H

typedef enum {
    IF_SB_AKL = 0,   /* akl 単体ランナー: ファイル/ソケット/proc 操作は全面禁止 */
    IF_SB_CHROME     /* ブラウザ chrome (TUI/GUI): tty/X11/SHM の最小限を追加許可 */
} IfSandboxProfile;

/* 不可逆フィルタをインストールする。
 * 戻り値: 0 = 成功（以後 profile 許可外の syscall はプロセス即死）、
 *         1 = カーネル非対応 / arch 非対応（呼出し側の方針判断に委ねる。
 *             akl CLI はこれを致命的として終了する = 安全側） */
int if_sandbox_apply(IfSandboxProfile profile);

/* profile 名（観測・診断用） */
const char *if_sandbox_profile_name(IfSandboxProfile profile);

#endif
