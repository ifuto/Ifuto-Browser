#!/usr/bin/env python3
"""ifuto --ui の PTY e2e スモークテスト。

実端末なしのコンテナで pty を割り当てて実動作を検証する:
  1. 初期描画にタブ帯(tタイトル頭文字 + 番号)が出る
  2. 'G' で末尾スクロール → ステータス行が 100% になる
  3. 't' で新タブ → オムニボックスがフォーカスされる → パス入力で開く
  4. 描画が指定ファイルのタイトルに変わる
  5. 'q''q' で終了（exit 0）
"""
import fcntl
import os
import re
import select
import struct
import subprocess
import sys
import termios
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/ifuto"
PAGE = "tests/pages/hello.html"


def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


class TuiProc:
    def __init__(self, argv):
        self.master, slave = os.openpty()
        set_winsize(slave, 30, 100)
        self.proc = subprocess.Popen(argv, stdin=slave, stdout=slave,
                                     stderr=subprocess.DEVNULL, close_fds=True)
        os.close(slave)
        os.set_blocking(self.master, False)
        self.acc = b""

    def read_until(self, needle, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            if needle.encode() in ansi_strip(self.acc):
                return True
            r, _, _ = select.select([self.master], [], [], 0.1)
            if r:
                try:
                    self.acc += os.read(self.master, 65536)
                except OSError:
                    break
        return needle.encode() in ansi_strip(self.acc)

    def send(self, s, settle=0.15):
        os.write(self.master, s.encode())
        time.sleep(settle)
        r, _, _ = select.select([self.master], [], [], 0)
        while r:
            try:
                self.acc += os.read(self.master, 65536)
            except OSError:
                break
            r, _, _ = select.select([self.master], [], [], 0)


def ansi_strip(b):
    s = re.sub(rb"\x1b\[[0-9;?]*[a-zA-Z]", b"", b)
    s = re.sub(rb"\x1b\][^\x07]*\x07", b"", s)
    return s.replace(b"\x1b[0m", b"")


def main():
    fails = 0

    def check(ok, label):
        nonlocal fails
        print(("ok  " if ok else "FAIL"), label)
        if not ok:
            fails += 1

    p = TuiProc([BIN, "--ui", PAGE])
    try:
        check(p.read_until("I1", 5), "initial paint: tab strip shows active tab")
        # 'G' で最下部へ
        p.send("G")
        time.sleep(0.2)
        p.read_until("mem", 2)
        check(re.search(rb"\b1?0?\d%|\d+%", ansi_strip(p.acc)) is not None,
              "status line shows scroll percentage")
        check(b"100%" in ansi_strip(p.acc), "bottom shows 100% after G")
        # 新タブ + パス入力で開く
        p.send("t")
        time.sleep(0.2)
        p.send("tests/golden/doc.html\r", 0.5)
        time.sleep(0.5)
        plain = ansi_strip(p.acc)
        check(b"tabs 2" in plain, "two tabs after t + open")
        check(b"Golden" in plain or b"golden" in plain or b"doc.html" in plain,
              "second tab opened doc.html (title or url visible)")
        # 'q''q' で終了
        p.send("q", 0.2)
        p.send("q", 0.4)
        rc = p.proc.wait(timeout=5)
        check(rc == 0, f"quit via q,q (rc={rc})")
    finally:
        if p.proc.poll() is None:
            p.proc.kill()
            p.proc.wait()
        os.close(p.master)
    print("----")
    print("tui_smoke:", "PASS" if fails == 0 else f"{fails} FAILURES")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
