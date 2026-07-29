#!/usr/bin/env python3
"""ifuto --ui (空タブ) の実測ベンチ v-chrome 天井検証用。

全値は「このマシン・この測定法」の実測であり、他環境との比較はしない。
  - cold start: subprocess.Popen 直前の monotonic から PTY 上の最初の出力
    バイトまで (fork/exec/動的リンク/初期描画を全て含む上限値: 真の UI 常駐
    時間はこの値以下である)
  - blank-tab RSS: 初回描画直後の /proc/PID/status VmHWM
  - idle CPU: 無入力 1.2 s 間の utime+stime 増分から算出 (INV-5: 0% であること)
  - idle output bytes: idle 中の描画出力 (INV-5 厳密形: 0 B であること)
"""
import fcntl
import os
import select
import struct
import subprocess
import sys
import termios


def read_vhwm_kb(pid):
    with open("/proc/%d/status" % pid) as f:
        for line in f:
            if line.startswith("VmHWM:"):
                return int(line.split()[1])
    return -1


def read_cpu_ticks(pid):
    with open("/proc/%d/stat" % pid) as f:
        # comm は ')' まで読み飛ばす。stat(5): state=field3 → parts[0],
        # utime=field14 → parts[11], stime=field15 → parts[12]
        parts = f.read().rsplit(")", 1)[1].split()
    return int(parts[11]) + int(parts[12])


def run_once(binpath, cols=100, rows=30):
    m, s = os.openpty()
    fcntl.ioctl(s, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    t0 = time_monotonic_ns()
    p = subprocess.Popen([binpath, "--ui"], stdin=s, stdout=s, stderr=s,
                         close_fds=True, start_new_session=True)
    os.close(s)
    os.set_blocking(m, False)

    # 初回描画 (最初の出力バイト) を待つ
    t_first = None
    deadline = time_monotonic_ns() + 5_000_000_000
    while time_monotonic_ns() < deadline and t_first is None:
        r, _, _ = select.select([m], [], [], 0.05)
        if r:
            if os.read(m, 65536):
                t_first = time_monotonic_ns()
    if t_first is None:
        p.kill()
        p.wait()
        raise SystemExit("FAIL: no first paint within 5 s")
    cold_ms = (t_first - t0) / 1e6

    # 残りの描画を吸い切る
    import time as _t
    _t.sleep(0.1)
    while True:
        r, _, _ = select.select([m], [], [], 0.02)
        if not r:
            break
        try:
            os.read(m, 65536)
        except OSError:
            break

    vhwm_kb = read_vhwm_kb(p.pid)

    # idle CPU / idle 出力
    hz = os.sysconf("SC_CLK_TCK")
    c0 = read_cpu_ticks(p.pid)
    w0 = _t.monotonic()
    _t.sleep(1.2)
    idle_bytes = 0
    while True:
        r, _, _ = select.select([m], [], [], 0.0)
        if not r:
            break
        try:
            idle_bytes += len(os.read(m, 65536))
        except OSError:
            break
    w1 = _t.monotonic()
    c1 = read_cpu_ticks(p.pid)
    idle_cpu = (c1 - c0) / hz / (w1 - w0) * 100.0

    # 空タブ開始時はオムニボックス入力モード (INV-1) なので、先に ESC で
    # NORMAL へ戻してから q,q で終了する (ESC 単独確定は 25 ms 待機込み)
    os.write(m, b"\x1b")
    _t.sleep(0.25)
    os.write(m, b"q")
    _t.sleep(0.15)
    os.write(m, b"q")
    try:
        rc = p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        raise SystemExit("FAIL: quit hang")
    os.close(m)
    return cold_ms, vhwm_kb, idle_cpu, rc, idle_bytes


def time_monotonic_ns():
    import time
    return time.monotonic_ns()


def main():
    binpath = sys.argv[1] if len(sys.argv) > 1 else "./build/ifuto"
    reps = 7
    colds = []
    for i in range(reps):
        cold, vhwm, cpu, rc, idle_bytes = run_once(binpath)
        colds.append(cold)
        if i == reps // 2:
            print("blank_tab_VmHWM_kB: %d" % vhwm)
            print("idle_cpu_percent: %.2f" % cpu)
            print("idle_output_bytes: %d" % idle_bytes)
            print("quit_rc: %d" % rc)
    colds.sort()
    print("cold_start_to_first_byte_ms: n=%d median=%.2f min=%.2f max=%.2f"
          % (reps, colds[reps // 2], colds[0], colds[-1]))


if __name__ == "__main__":
    main()
