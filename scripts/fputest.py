#!/usr/bin/env python3
"""
scripts/fputest.py — regression test for eager FPU/SSE context switching (v38.41).

Boots mectov.iso in QEMU, logs in, opens the Terminal, types
`run /apps/fputest.mct` and verifies from the serial log that:

  1. the kernel enabled fxsave switching on the BSP ("[FPU] eager fxsave")
  2. two processes accumulating in live x87/SSE registers across
     preemptions and fork() both computed exact sums ("FPUTEST CHILD OK"
     and "FPUTEST PARENT OK")
  3. the app reported overall success ("FPUTEST PASS")
  4. the OS stayed alive afterwards

Before v38.41 this exact scenario corrupted the second FPU user's state.

Usage:
    python3 scripts/fputest.py [--timeout 240] [--kvm]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

import terminal_launch  # corner-reset + screendump-verified icon double-click

SERIAL_LOG = "/tmp/mectov_fputest_serial.log"
MON_SOCK = "/tmp/mectov_fputest_monitor.sock"

# The Windows-style lock screen eats the first keypress to dismiss it, so a
# leading space is sent before the password keys.
LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
# `run /apps/fputest.mct` + Enter, as scancode names understood by sendkey
RUN_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s",
            "slash", "f", "p", "u", "t", "e", "s", "t", "dot", "m", "c", "t", "ret"]


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
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--kvm", action="store_true",
                    help="run with -enable-kvm (real 4-core timing)")
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
            try:
                with open(SERIAL_LOG, "r", errors="replace") as f:
                    tail = f.read().splitlines()[-25:]
                print(f"--- serial log tail ({len(tail)} lines) ---")
                for line in tail:
                    print(line[:130])
            except OSError:
                print("(serial log missing or empty — QEMU likely failed to start)")
            return 1
        print("[OK] booted to login screen")

        if not wait_for_in_file(SERIAL_LOG, "[FPU] eager fxsave", 30):
            print("[FAIL] eager FPU switching was not enabled at boot")
            return 1
        print("[OK] eager fxsave switching enabled")

        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)

        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] logged in, desktop running")

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(
                mon_cmd, SERIAL_LOG, "/tmp/mectov_fputest_cursor.ppm"):
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

        ok_run = False
        for _ in range(3):
            for _ in range(24):
                mon_cmd("sendkey backspace")
            for k in RUN_KEYS:
                mon_cmd("sendkey " + k)
                time.sleep(0.12)
            mon_cmd("sendkey ret")
            if wait_for_in_file(SERIAL_LOG, "FPUTEST start", 25):
                ok_run = True
                break
            time.sleep(1.0)
        if not ok_run:
            print("[FAIL] fputest never started")
            return 1
        print("[OK] fputest running")

        for needle, what in (("FPUTEST CHILD OK", "child sums exact"),
                             ("FPUTEST PARENT OK", "parent sums exact"),
                             ("FPUTEST PASS", "overall PASS")):
            if not wait_for_in_file(SERIAL_LOG, needle, 60):
                print(f"[FAIL] {what} not confirmed (missing '{needle}')")
                return 1
            print(f"[OK] {what}")

        time.sleep(5)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after the FPU stress test")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
