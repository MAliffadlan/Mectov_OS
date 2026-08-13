#!/usr/bin/env python3
"""
scripts/fuzz_test.py — Ring 3 syscall fuzzer test.

Boots mectov.iso in QEMU, logs in, double-clicks the Terminal desktop icon,
types `run /apps/fuzz.mct`, and verifies from the serial log that:

  1. the fuzzer started and completed its full run        ("[FUZZ] DONE ok=")
  2. the targeted hostile probes were all rejected cleanly
     ("[FUZZ] probe ... OK" — the SYS_LIST_DIR/SYS_GET_TASKS max_count
     overflow fix, the SYS_SIGRETURN selector validation, and the SYS_KILL
     range check)
  3. the kernel never panicked                              (no "[PANIC]")
  4. the OS stayed alive afterwards

The fuzzer issues ~2000 random syscalls with garbage pointers, absurd sizes
and invalid fds/pids — every one must return an error without a kernel
fault at CPL 0.

Usage:
    python3 scripts/fuzz_test.py [--timeout 300]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

import terminal_launch  # corner-reset + screendump-verified icon double-click

SERIAL_LOG = "/tmp/mectov_fuzz_serial.log"
MON_SOCK = "/tmp/mectov_fuzz_monitor.sock"

# The Windows-style lock screen eats the first keypress to dismiss it, so a
# leading space is sent before the password keys.
LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
# `run /apps/fuzz.mct` + Enter, as scancode names understood by sendkey
RUN_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s",
            "slash", "f", "u", "z", "z", "dot", "m", "c", "t", "ret"]

# Any of these in the serial log means the kernel faulted at CPL 0.
PANIC_MARKERS = ["[KERNEL PANIC]", "[PANIC]"]


def read_log(path):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for_in_file(path, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if needle in read_log(path):
            return True
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
    ap.add_argument("--kvm", action="store_true",
                    help="run with -enable-kvm (real timing; the fuzzer's "
                         "random syscalls are best stressed on 4 cores)")
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
    if args.kvm:
        qemu_cmd.insert(1, "-enable-kvm")
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached login screen")
            tail = read_log(SERIAL_LOG).splitlines()[-25:]
            print("--- serial log tail ---")
            for line in tail:
                print(line[:130])
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
                mon_cmd, SERIAL_LOG, "/tmp/mectov_fuzz_cursor.ppm"):
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

        ok_run = False
        for _ in range(3):
            for _ in range(24):
                mon_cmd("sendkey backspace")
            for k in RUN_KEYS:
                mon_cmd("sendkey " + k)
                time.sleep(0.12)
            mon_cmd("sendkey ret")
            if wait_for_in_file(SERIAL_LOG, "[FUZZ] start", 25):
                ok_run = True
                break
            time.sleep(1.0)
        if not ok_run:
            print("[FAIL] fuzzer never started")
            return 1
        print("[OK] fuzzer started")

        if not wait_for_in_file(SERIAL_LOG, "[FUZZ] DONE ok=", 120):
            print("[FAIL] fuzzer did not complete its run "
                  "(kernel likely faulted or the task died)")
            for line in read_log(SERIAL_LOG).splitlines()[-40:]:
                print(line[:130])
            return 1
        print("[OK] fuzzer completed its full run")

        # Targeted probes must have been rejected cleanly.
        probes = ["list_dir_huge OK", "get_tasks_huge OK", "get_windows_huge OK",
                  "sigreturn_bare OK", "kill_oob OK"]
        log = read_log(SERIAL_LOG)
        for p in probes:
            if f"[FUZZ] probe {p} ret=" not in log:
                print(f"[FAIL] probe '{p}' marker missing")
                return 1
        print("[OK] all targeted probes rejected cleanly")

        # The kernel must never have panicked (CPL-0 fault = panic here).
        for m in PANIC_MARKERS:
            if m in log:
                print(f"[FAIL] kernel PANIC marker found: {m}")
                for line in log.splitlines():
                    if m in line:
                        print(line[:160])
                return 1
        print("[OK] no kernel panic in the whole fuzz run")

        # Let the desktop spin to catch late crashes, then confirm QEMU alive.
        time.sleep(5)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after the fuzz run")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
