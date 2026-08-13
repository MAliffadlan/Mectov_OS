#!/usr/bin/env python3
"""
scripts/app_smoke_test.py — smoke tests for the remaining demo apps.

Boots mectov.iso once, logs in, launches the Terminal, then runs three apps
back-to-back in the same terminal and verifies each one from the serial log:

  1. smpstress  — 8 children across the 4-core SMP scheduler: CPU burns,
                  sleep slices, self-signals; parent reaps all with the
                  right codes  ->  "[SMPSTRESS] ALL PASS"
  2. sigdemo    — sigprocmask hold/unblock, sa_mask deferral, SA_NODEFER,
                  SA_RESTART     ->  "[SIGDEMO] ALL TESTS PASSED"
  3. crashme    — deliberately executes `ud2`; the kernel must log
                  "[EXCEPTION] int_no=6", kill the task, and keep running
                  (no exit marker — the app is killed on purpose)

After all three, asserts the OS stayed alive and no "[PANIC]" appeared.

Usage:
    python3 scripts/app_smoke_test.py [--timeout 300]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

import terminal_launch  # corner-reset + screendump-verified icon double-click

SERIAL_LOG = "/tmp/mectov_appsmoke_serial.log"
MON_SOCK = "/tmp/mectov_appsmoke_monitor.sock"

# The Windows-style lock screen eats the first keypress to dismiss it, so a
# leading space is sent before the password keys.
LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

# Each entry: (app name, marker that proves it finished, run keys, wait).
# smpstress needs the longest wait: 8 children × CPU burn on TCG.
APPS = [
    ("smpstress",
     "[SMPSTRESS] ALL PASS",
     ["r", "u", "n", "spc", "slash", "a", "p", "p", "s", "slash",
      "s", "m", "p", "s", "t", "r", "e", "s", "s", "dot", "m", "c", "t", "ret"],
     180),
    ("sigdemo",
     "[SIGDEMO] ALL TESTS PASSED",
     ["r", "u", "n", "spc", "slash", "a", "p", "p", "s", "slash",
      "s", "i", "g", "d", "e", "m", "o", "dot", "m", "c", "t", "ret"],
     90),
    ("crashme",
     "[EXCEPTION] int_no=0x00000006",
     ["r", "u", "n", "spc", "slash", "a", "p", "p", "s", "slash",
      "c", "r", "a", "s", "h", "m", "e", "dot", "m", "c", "t", "ret"],
     90),
]


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


def run_app(mon, serial_log, name, marker, run_keys, wait):
    """Type `run /apps/<name>.mct`, retrying until the marker appears."""
    for _ in range(3):
        for _ in range(28):
            mon("sendkey backspace")
        for k in run_keys:
            mon("sendkey " + k)
            time.sleep(0.12)
        mon("sendkey ret")
        if wait_for_in_file(serial_log, marker, wait):
            return True
        time.sleep(1.0)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--apps", default=None,
                    help="comma list to run a subset (default: all three)")
    args = ap.parse_args()

    apps = APPS
    if args.apps:
        want = [a.strip() for a in args.apps.split(",")]
        apps = [a for a in APPS if a[0] in want]

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
                mon_cmd, SERIAL_LOG, "/tmp/mectov_appsmoke_cursor.ppm"):
            print("[FAIL] terminal did not launch (icon double-click missed?)")
            return 1
        print("[OK] terminal launched")
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] terminal never became ready")
            return 1
        time.sleep(1.0)

        # Click inside the terminal window so keyboard focus lands on it.
        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)

        for name, marker, run_keys, wait in apps:
            if not run_app(mon_cmd, SERIAL_LOG, name, marker, run_keys, wait):
                print(f"[FAIL] {name} never reached its marker ({marker})")
                return 1
            print(f"[OK] {name} completed ({marker})")
            # Give the terminal a moment to return to the prompt before the
            # next `run` (the previous app's output is still draining).
            time.sleep(1.5)

        # crashme dies on purpose, so an SYS_EXIT check would be wrong for it;
        # the whole-run safety net below (no PANIC + OS alive) is the verdict.
        # A [PANIC] line anywhere in the log fails the whole run.
        with open(SERIAL_LOG, "r", errors="replace") as f:
            log_text = f.read()
        if "[PANIC]" in log_text:
            print("[FAIL] kernel panicked during the app smoke run")
            return 1
        print("[OK] no kernel panic in the whole app smoke run")

        time.sleep(5)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after all demo apps (incl. crashme)")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
