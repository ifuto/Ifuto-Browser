#!/usr/bin/env python3
"""ifuto --ui の PTY e2e スモークテスト。

実端末なしのコンテナで pty を割り当てて実動作を検証する:
  シナリオ1 (基本動作):
    1. 初期描画にタブ帯(tタイトル頭文字 + 番号)が出る
    2. 'G' で末尾スクロール → ステータス行が 100% になる
    3. 't' で新タブ → オムニボックスがフォーカスされる → パス入力で開く
    4. 描画が指定ファイルのタイトルに変わる
    5. 'q''q' で終了（exit 0）
  シナリオ2 (slice-2: ストア):
    6. セッション復元: 再起動でタブ列が復活する（初回引数なし起動）
    7. ブックマーク: 'b' でトグル → bookmarks.tsv に URL が残る
    8. --show-paths: 3 ストアのパスを列挙する（副作用ゼロの検査）
  全プロセスに IFUTO_HOME=<tmp> を渡す（実 $HOME を一切汚さない）。
"""
import fcntl
import os
import re
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/ifuto"
PAGE = "tests/pages/hello.html"


def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def ansi_strip(b):
    s = re.sub(rb"\x1b\[[0-9;?]*[a-zA-Z]", b"", b)
    s = re.sub(rb"\x1b\][^\x07]*\x07", b"", s)
    return s.replace(b"\x1b[0m", b"")


class TuiProc:
    """PTY 子プロセス + 常時ドレイナスレッド。

    TUI はイベント毎に 1 フレーム(10KB 級)を一括 write する。PTY バッファが
    満杯になると app が write ブロックで凍り、後続入力が読めなくなる
    （一見デコーダがバイトを落とす症状になる）。これを防ぐため、受信は常駐
    スレッドが連続で吸い切り、アサートは蓄積バッファを検査するのみとする。
    """

    def __init__(self, argv, env):
        self.master, slave = os.openpty()
        set_winsize(slave, 30, 100)
        self.proc = subprocess.Popen(argv, stdin=slave, stdout=slave,
                                     stderr=subprocess.DEVNULL, close_fds=True,
                                     env=env)
        os.close(slave)
        self.acc = b""
        self._alive = True
        self._thr = threading.Thread(target=self._drain, daemon=True)
        self._thr.start()
        # 初回フレームが流れ切るまで待つ（ここで待たないと以後全ての判定が偽陰性）
        self.read_until("tabs", 5)

    def _drain(self):
        while self._alive:
            try:
                r, _, _ = select.select([self.master], [], [], 0.05)
            except (ValueError, OSError):
                break
            if not r:
                continue
            try:
                chunk = os.read(self.master, 65536)
            except OSError:
                break
            if not chunk:
                break
            self.acc += chunk

    def read_until(self, needle, timeout=5.0):
        end = time.time() + timeout
        n = needle.encode()
        while time.time() < end:
            if n in ansi_strip(self.acc):
                return True
            time.sleep(0.05)
        return False

    def send(self, s, settle=0.15):
        os.write(self.master, s.encode())
        time.sleep(settle)

    def quit_qq(self, timeout=5.0):
        self.send("q", 0.2)
        self.send("q", 0.4)
        return self.proc.wait(timeout=timeout)

    def kill(self):
        self._alive = False
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        self._thr.join(timeout=1)
        os.close(self.master)


def main():
    fails = 0

    def check(ok, label):
        nonlocal fails
        print(("ok  " if ok else "FAIL"), label)
        if not ok:
            fails += 1

    sandbox = tempfile.mkdtemp(prefix="ifuto-smoke-")
    env = dict(os.environ)
    env["IFUTO_HOME"] = sandbox  # ストアの実験場を完全に封じ込める

    # ---- シナリオ1: 基本動作 ----
    p = TuiProc([BIN, "--ui", PAGE], env)
    try:
        check(p.read_until("I1", 5), "initial paint: tab strip shows active tab")
        p.send("G")
        time.sleep(0.2)
        p.read_until("mem", 2)
        check(re.search(rb"\d+%", ansi_strip(p.acc)) is not None,
              "status line shows scroll percentage")
        check(b"100%" in ansi_strip(p.acc), "bottom shows 100% after G")
        p.send("t")
        time.sleep(0.2)
        p.send("tests/golden/doc.html\r", 0.5)
        time.sleep(0.5)
        plain = ansi_strip(p.acc)
        check(b"tabs 2" in plain, "two tabs after t + open")
        check(b"Golden" in plain or b"golden" in plain or b"doc.html" in plain,
              "second tab opened doc.html (title or url visible)")
        check(re.search(rb"tabs [12]", ansi_strip(p.acc)) is not None,
              "status shows tab count")
        rc = p.quit_qq()
        check(rc == 0, f"quit via q,q (rc={rc})")
    finally:
        p.kill()

    # ---- シナリオ2: ストア（restore / bookmark / show-paths / 検索 / グループ） ----
    p2 = TuiProc([BIN, "--ui"], env)  # ファイル引数なし起動 → 前回セッション復元
    try:
        ok = p2.read_until("tabs 2", 5)
        check(ok, "session restore: two tabs revived on relaunch")
        plain = ansi_strip(p2.acc)
        check(b"doc.html" in plain or b"Golden" in plain or b"hello" in plain,
              "restored tab shows a known title")
        # グループ割当: o → @smoke → Enter
        p2.send("o")
        time.sleep(0.2)
        p2.send("@smoke\r", 0.4)
        plain = ansi_strip(p2.acc)
        check(b"smoke]" in plain, "group prefix [smoke] appears in tabstrip")
        # タブ検索: o → ?gold → Enter → (ヒット数に応じ選択相) 1 → ... 直接比較
        p2.send("o")
        time.sleep(0.2)
        p2.send("?gold\r", 0.5)
        plain = ansi_strip(p2.acc)
        check(b"tab search" in plain, "tab search overlay shown after ?query+Enter")
        p2.send("\x1b", 0.4)  # Esc で戻る
        # ブックマーク: 現在タブをトグル
        p2.send("b", 0.3)
        plain = ansi_strip(p2.acc)
        check(b"bookmarked" in plain, "bookmark toggle shows toast")
        rc = p2.quit_qq()
        check(rc == 0, f"quit via q,q in restored session (rc={rc})")
    finally:
        p2.kill()

    # ---- ファイル側検査: bookmarks.tsv に URL が残っている ----
    bmrk = os.path.join(sandbox, "bookmarks.tsv")
    ok = False
    if os.path.exists(bmrk):
        with open(bmrk, "rb") as f:
            body = f.read()
        ok = b"doc.html" in body or b"hello.html" in body
    check(ok, "bookmarks.tsv persisted the toggled URL")

    # ---- INV-9: --show-paths ----
    sp = subprocess.run([BIN, "--show-paths"], env=env,
                        capture_output=True, timeout=10)
    out = sp.stdout
    check(sp.returncode == 0 and b"session.txt" in out and b"history.tsv" in out
          and b"bookmarks.tsv" in out and sandbox.encode() in out,
          "--show-paths lists the 3 stores under IFUTO_HOME")

    shutil.rmtree(sandbox, ignore_errors=True)
    print("----")
    print("tui_smoke:", "PASS" if fails == 0 else f"{fails} FAILURES")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
