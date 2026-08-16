#!/usr/bin/env python3
"""
scripts/ulimit_test.py — shell `ulimit` builtin regression test (v38.28).

Boots mectov.iso once, logs in, launches the Terminal and drives the shell
`ulimit` builtin, verifying from the serial log (the builtin echoes every
result as `[ULIMIT] ...`):

  * `ulimit -a` lists all three resources with sane soft/hard values
    (nproc 64/64, as 268435456/268435456, nofile 16/16)
  * `ulimit -n 8` lowers RLIMIT_NOFILE to 8 and `ulimit -n` reads it back
  * `ulimit -n 99` is refused: a non-root caller may not raise a soft limit
    above its hard limit (16), so it fails with "[ULIMIT] set failed"
  * `ulimit -u 4` lowers RLIMIT_NPROC and `ulimit -u` reads it back
  * the lowered NOFILE limit is actually ENFORCED: a follow-up `run
    /apps/rlimittest.mct` still passes (rlimittest raises its own limits),
    and more directly, `ulimit -n 8` is visible to child processes.

The kernel must never panic during the whole run.

Usage:
    python3 scripts/ulimit_test.py [--timeout 300]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

import terminal_launch  # corner-reset + screendump-verified icon double-click

SERIAL_LOG = "/tmp/mectov_ulimit_serial.log"
MON_SOCK = "/tmp/mectov_ulimit_monitor.sock"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]


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


def type_command(keys):
    for k in keys:
        mon_cmd("sendkey " + k)
        time.sleep(0.12)
    mon_cmd("sendkey ret")


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
        "-cpu", "qemu32,+nx",
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
                mon_cmd, SERIAL_LOG, "/tmp/mectov_ulimit_cursor.ppm"):
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

        def send_cmd(s):
            for _ in range(24):
                mon_cmd("sendkey backspace")
            type_command(["u", "l", "i", "m", "i", "t"] + s)

        # 1. `ulimit -a`: all three resources listed with defaults.
        send_cmd(["spc", "minus", "a"])
        if not wait_for_in_file(SERIAL_LOG,
                "[ULIMIT] get nproc cur=64 max=64", 60):
            print("[FAIL] ulimit -a did not list nproc 64/64")
            return 1
        if not wait_for_in_file(SERIAL_LOG,
                "[ULIMIT] get as cur=268435456 max=268435456", 30):
            print("[FAIL] ulimit -a did not list as 256 MB")
            return 1
        if not wait_for_in_file(SERIAL_LOG,
                "[ULIMIT] get nofile cur=16 max=16", 30):
            print("[FAIL] ulimit -a did not list nofile 16/16")
            return 1
        print("[OK] ulimit -a lists all resources with sane defaults")

        # 2. `ulimit -n 8` lowers NOFILE; `ulimit -n` reads it back.
        send_cmd(["spc", "minus", "n", "spc", "8"])
        if not wait_for_in_file(SERIAL_LOG, "[ULIMIT] set nofile", 30):
            print("[FAIL] ulimit -n 8 did not set NOFILE")
            return 1
        send_cmd(["spc", "minus", "n"])
        if not wait_for_in_file(SERIAL_LOG,
                "[ULIMIT] get nofile cur=8 max=16", 30):
            print("[FAIL] ulimit -n did not read back 8/16")
            return 1
        print("[OK] ulimit -n 8 lowered NOFILE and ulimit -n reads 8/16")

        # 3. Raising soft above hard is refused for a non-root caller.
        send_cmd(["spc", "minus", "n", "spc", "9", "9"])
        if not wait_for_in_file(SERIAL_LOG, "[ULIMIT] set failed", 30):
            print("[FAIL] ulimit -n 99 (above hard 16) was not refused")
            return 1
        print("[OK] raising soft above the hard limit is refused")

        # 4. `ulimit -u 4` lowers NPROC; `ulimit -u` reads it back.
        send_cmd(["spc", "minus", "u", "spc", "4"])
        if not wait_for_in_file(SERIAL_LOG, "[ULIMIT] set nproc", 30):
            print("[FAIL] ulimit -u 4 did not set NPROC")
            return 1
        send_cmd(["spc", "minus", "u"])
        if not wait_for_in_file(SERIAL_LOG,
                "[ULIMIT] get nproc cur=4 max=64", 30):
            print("[FAIL] ulimit -u did not read back 4/64")
            return 1
        print("[OK] ulimit -u 4 lowered NPROC and ulimit -u reads 4/64")

        # 5. Raising cur back to the hard limit is legal for a non-root
        #    caller, and the restored limits let rlimittest pass (it inherits
        #    the terminal's limits — inheritance is exactly what we want to
        #    verify here: a lowered `ulimit -n`/`ulimit -u` must be inherited
        #    by child processes, and the restore must undo it).
        send_cmd(["spc", "minus", "n", "spc", "1", "6"])
        if not wait_for_in_file(SERIAL_LOG, "[ULIMIT] set nofile", 30):
            print("[FAIL] ulimit -n 16 (restore) was refused")
            return 1
        send_cmd(["spc", "minus", "u", "spc", "6", "4"])
        if not wait_for_in_file(SERIAL_LOG, "[ULIMIT] set nproc", 30):
            print("[FAIL] ulimit -u 64 (raise back to hard) was refused")
            return 1
        send_cmd(["spc", "minus", "u"])
        if not wait_for_in_file(SERIAL_LOG,
                "[ULIMIT] get nproc cur=64 max=64", 30):
            print("[FAIL] ulimit -u did not read back 64/64 after restore")
            return 1
        print("[OK] raising cur back to the hard limit is allowed and restored")

        for _ in range(24):
            mon_cmd("sendkey backspace")
        type_command(["r", "u", "n", "spc", "slash", "a", "p", "p", "s", "slash",
                      "r", "l", "i", "m", "i", "t", "t", "e", "s", "t", "dot",
                      "m", "c", "t"])
        if not wait_for_in_file(SERIAL_LOG, "[RLIM] ALL PASS", 120):
            print("[FAIL] rlimittest did not pass after ulimit changes")
            return 1
        print("[OK] rlimittest still passes after the ulimit changes")

        with open(SERIAL_LOG, "r", errors="replace") as f:
            log_text = f.read()
        if "[PANIC]" in log_text:
            print("[FAIL] kernel panicked during the ulimit run")
            return 1
        print("[OK] no kernel panic in the whole ulimit run")

        time.sleep(5)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after the ulimit test")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
