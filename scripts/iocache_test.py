#!/usr/bin/env python3
"""
scripts/iocache_test.py — page-cache benchmark test.

Boots mectov.iso in QEMU, logs in, double-clicks the Terminal desktop icon,
types `run /apps/iobench.mct`, and verifies from the serial log that:

  1. the benchmark ran cold (disk) and cached read timings  ("cold_us=",
     "hot_us=" — first read of a seeded file is a cache miss, the next 29
     are served from RAM)
  2. the verdict passed                                     ("verdict OK")
  3. the kernel never panicked                              (no "[PANIC]")
  4. the OS stayed alive afterwards

The benchmark's own verdict requires cached reads to be at least 4x faster
than the cold read, so this test fails if the whole-file page cache
(src/sys/pcache.c) stops serving hits.

Usage:
    python3 scripts/iocache_test.py [--timeout 300]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

import terminal_launch  # corner-reset + screendump-verified icon double-click

SERIAL_LOG = "/tmp/mectov_iocache_serial.log"
MON_SOCK = "/tmp/mectov_iocache_monitor.sock"

# The Windows-style lock screen eats the first keypress to dismiss it, so a
# leading space is sent before the password keys.
LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
# `run /apps/iobench.mct` + Enter, as scancode names understood by sendkey
RUN_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s",
            "slash", "i", "o", "b", "e", "n", "c", "h", "dot", "m", "c", "t", "ret"]

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
                    help="run with -enable-kvm (real timing — the cold/cached "
                         "read ratio is still several fold under KVM)")
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
                mon_cmd, SERIAL_LOG, "/tmp/mectov_iocache_cursor.ppm"):
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
            if wait_for_in_file(SERIAL_LOG, "[IOBENCH] start", 25):
                ok_run = True
                break
            time.sleep(1.0)
        if not ok_run:
            print("[FAIL] benchmark never started")
            return 1
        print("[OK] benchmark started")

        if not wait_for_in_file(SERIAL_LOG, "[IOBENCH] DONE", 90):
            print("[FAIL] benchmark did not complete")
            for line in read_log(SERIAL_LOG).splitlines()[-30:]:
                print(line[:130])
            return 1
        print("[OK] benchmark completed")

        # The two timings and the verdict must be present.
        log = read_log(SERIAL_LOG)
        for needle in ("[IOBENCH] cold_us=", "[IOBENCH] hot_us=",
                       "[IOBENCH] verdict OK"):
            if needle not in log:
                print(f"[FAIL] marker '{needle}' missing")
                return 1
        # Surface the measured numbers for the log.
        for line in log.splitlines():
            if "[IOBENCH]" in line and "us=" in line:
                print(f"    {line[:90]}")
        print("[OK] cold read (disk) vs cached read timing measured")

        for m in PANIC_MARKERS:
            if m in log:
                print(f"[FAIL] kernel PANIC marker found: {m}")
                return 1
        print("[OK] no kernel panic during the benchmark")

        time.sleep(5)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after the benchmark")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
