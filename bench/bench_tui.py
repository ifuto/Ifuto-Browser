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


def run_once(binpath, cols=100, rows=30, env=None):
    """1 回の計測。PTY の受信は常駐スレッドで連続ドレインする（TUI の
    イベント単位の全画面 write が PTY バッファ満杯でブロックされるのを防ぐ。
    tui_smoke.py と同じ教訓: ブロックされる and app が入力を読まない）。"""
    import threading
    m, s = os.openpty()
    fcntl.ioctl(s, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    t0 = time_monotonic_ns()
    p = subprocess.Popen([binpath, "--ui"], stdin=s, stdout=s, stderr=s,
                         close_fds=True, start_new_session=True, env=env)
    os.close(s)

    state = {"got_first": False, "bytes": 0, "alive": True}

    def drain():
        import select as _sel
        while state["alive"]:
            r, _, _ = _sel.select([m], [], [], 0.05)
            if not r:
                continue
            try:
                chunk = os.read(m, 65536)
            except OSError:
                break
            if not chunk:
                break
            if not state["got_first"]:
                state["t_first"] = time_monotonic_ns()
                state["got_first"] = True
            state["bytes"] += len(chunk)

    thr = threading.Thread(target=drain, daemon=True)
    thr.start()

    # 初回描画 (最初の出力バイト) を待つ
    import time as _t
    deadline = time_monotonic_ns() + 5_000_000_000
    while time_monotonic_ns() < deadline and not state["got_first"]:
        _t.sleep(0.005)
    if not state["got_first"]:
        state["alive"] = False
        p.kill()
        p.wait()
        raise SystemExit("FAIL: no first paint within 5 s")
    cold_ms = (state["t_first"] - t0) / 1e6

    _t.sleep(0.15)  # 残りフレームを流す余裕
    vhwm_kb = read_vhwm_kb(p.pid)

    # idle CPU / idle 出力
    hz = os.sysconf("SC_CLK_TCK")
    b0 = state["bytes"]
    c0 = read_cpu_ticks(p.pid)
    w0 = _t.monotonic()
    _t.sleep(1.2)
    w1 = _t.monotonic()
    c1 = read_cpu_ticks(p.pid)
    idle_bytes = state["bytes"] - b0
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
        state["alive"] = False
        p.kill()
        p.wait()
        raise SystemExit("FAIL: quit hang")
    state["alive"] = False
    thr.join(timeout=1)
    os.close(m)
    return cold_ms, vhwm_kb, idle_cpu, rc, idle_bytes


def time_monotonic_ns():
    import time
    return time.monotonic_ns()


def main():
    binpath = sys.argv[1] if len(sys.argv) > 1 else "./build/ifuto"
    import shutil
    import tempfile
    # ストアの書き込みは完全に封じ込める（実 $HOME を汚さない）。
    # 空起動で毎回 session.txt が 1 度書かれる点は slice-2 以後の正しい動作。
    sandbox = tempfile.mkdtemp(prefix="ifuto-tuibench-")
    env = dict(os.environ)
    env["IFUTO_HOME"] = sandbox
    reps = 7
    colds = []
    try:
        for i in range(reps):
            cold, vhwm, cpu, rc, idle_bytes = run_once(binpath, env=env)
            colds.append(cold)
            if i == reps // 2:
                print("blank_tab_VmHWM_kB: %d" % vhwm)
                print("idle_cpu_percent: %.2f" % cpu)
                print("idle_output_bytes: %d" % idle_bytes)
                print("quit_rc: %d" % rc)
    finally:
        shutil.rmtree(sandbox, ignore_errors=True)
    colds.sort()
    print("cold_start_to_first_byte_ms: n=%d median=%.2f min=%.2f max=%.2f"
          % (reps, colds[reps // 2], colds[0], colds[-1]))


if __name__ == "__main__":
    main()
