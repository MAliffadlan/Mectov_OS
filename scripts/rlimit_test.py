#!/usr/bin/env python3
"""
scripts/rlimit_test.py — resource-limit (RLIMIT_*) regression test (v38.28).

Boots mectov.iso once, logs in, launches the Terminal, runs the Ring 3
rlimittest app (`run /apps/rlimittest.mct`) and verifies from the serial
log that every RLIMIT assertion passed:

  * getrlimit defaults are sane (NPROC 64/64, AS 256 MB, NOFILE 16/16)
  * setrlimit NPROC cur=1 is allowed; fork() is then refused because the
    caller's uid is shared with other live tasks
  * raising cur back to 64 is allowed and fork() succeeds (child reaped)
  * a non-root caller may never raise the hard limit (EPERM)
  * RLIMIT_NOFILE stops fd allocation at the soft limit, and closing a
    descriptor frees a slot again
  * RLIMIT_AS stops mmap reservations at the soft limit (2 MB under a
    1 MB limit refused, 256 KB allowed), and raising it re-allows 2 MB

The kernel must never panic during the whole run.

Usage:
    python3 scripts/rlimit_test.py [--timeout 300]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

import terminal_launch  # corner-reset + screendump-verified icon double-click

SERIAL_LOG = "/tmp/mectov_rlimit_serial.log"
MON_SOCK = "/tmp/mectov_rlimit_monitor.sock"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

RUN_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s", "slash",
            "r", "l", "i", "m", "i", "t", "t", "e", "s", "t", "dot", "m", "c", "t", "ret"]


def wait_for_in_file(path, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, "r", errors="replace") as f:
                if needle in f.read():
                    return True
        except (FileNotFoundError, OSError):
            pass
        time.sleep(1)
    return False


def mon_cmd(cmd):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(0.15)
        s.close()
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    qemu_cmd = [
        "qemu-system-i386",
        "-vga", "std",
        "-cdrom", args.iso,
        "-m", "128",
        "-smp", "4",
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        "-net", "none",
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached login screen")
            return 1
        print("[OK] booted to login screen")

        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)

        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] logged in, desktop running")

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(
                mon_cmd, SERIAL_LOG, "/tmp/mectov_rlimit_cursor.ppm"):
            print("[FAIL] terminal did not launch (icon double-click missed?)")
            return 1
        print("[OK] terminal launched")
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] terminal never became ready")
            return 1
        time.sleep(1.0)

        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)

        # Run the resource-limit app (retry a few times for keystroke safety).
        for _ in range(3):
            for _ in range(28):
                mon_cmd("sendkey backspace")
            for k in RUN_KEYS:
                mon_cmd("sendkey " + k)
                time.sleep(0.12)
            mon_cmd("sendkey ret")
            if wait_for_in_file(SERIAL_LOG, "[RLIM] ALL PASS", 90):
                break
            time.sleep(1.0)

        with open(SERIAL_LOG, "r", errors="replace") as f:
            log_text = f.read()
        if "[RLIM] FAIL" in log_text:
            print("[FAIL] rlimittest reported a failed assertion")
            return 1
        if "[RLIM] ALL PASS" not in log_text:
            print("[FAIL] rlimittest never reached [RLIM] ALL PASS")
            return 1
        print("[OK] rlimittest completed ([RLIM] ALL PASS)")

        if "[PANIC]" in log_text:
            print("[FAIL] kernel panicked during the rlimit run")
            return 1
        print("[OK] no kernel panic in the whole rlimit run")

        time.sleep(5)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after the rlimit test")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
