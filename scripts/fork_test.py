#!/usr/bin/env python3
"""
scripts/fork_test.py — functional test for the fork/waitpid/signal process model.

Boots mectov.iso in QEMU, logs in, double-clicks the Terminal desktop icon
(which starts at a known position since the OS cursor begins at 400,300 and
mouse_move is relative), types `run /apps/forkdemo.mct`, and verifies from the
serial log that:

  1. a child task was forked            ("fork: child")
  2. the child exited with status 42    ("SYS_EXIT code=2A")
  3. the OS stayed alive afterwards

Usage:
    python3 scripts/fork_test.py [--timeout 240]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

SERIAL_LOG = "/tmp/mectov_fork_serial.log"
MON_SOCK = "/tmp/mectov_fork_monitor.sock"

# The Windows-style lock screen eats the first keypress to dismiss it, so a
# leading space is sent before the password keys.
LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
# `run /apps/forkdemo.mct` + Enter, as scancode names understood by sendkey
RUN_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s",
            "slash", "f", "o", "r", "k", "d", "e", "m", "o", "dot", "m", "c", "t", "ret"]


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
                    help="run with -enable-kvm (real timing, mandatory SMP "
                         "regression: the keyboard single-consumer race only "
                         "reproduces under KVM 4-core, not TCG)")
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
            # Diagnostics: dump the serial tail so a QEMU startup failure
            # (e.g. KVM unavailable) vs. a mid-boot hang is distinguishable.
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

        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)

        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] logged in, desktop running")

        # Let the desktop finish its first frame so icons are initialised and
        # the main loop is stable, then double-click the Terminal desktop icon.
        # The OS cursor starts at (400,300); drive it to the top-left corner
        # (0,0) with small -127 moves (the kernel clamps at the corner, so
        # repeated negative moves are idempotent there), then one packet to the
        # icon center (60,64). Retry from the corner: a dropped PS/2 packet
        # under slow TCG would otherwise leave the cursor at an unknown spot.
        # Moves stay one packet each with gaps so the 16-byte PS/2 buffer never
        # overflows (a burst of huge deltas floods it and corrupts alignment).
        time.sleep(1.5)
        launched = False
        for _ in range(4):
            for _ in range(4):
                mon_cmd("mouse_move -127 -127")
                time.sleep(0.25)
            time.sleep(0.3)
            mon_cmd("mouse_move 60 64")
            time.sleep(0.3)
            # Burst of clicks: the desktop launches on any two clicks within 800
            # ticks of each other, and TCG's virtual clock can jump between
            # clicks, so several quick pairs make it virtually certain one pair
            # lands in the window. (Extra clicks after launch are harmless.)
            for _ in range(4):
                mon_cmd("mouse_button 1")
                time.sleep(0.05)
                mon_cmd("mouse_button 0")
                time.sleep(0.05)
            if wait_for_in_file(SERIAL_LOG, "[LOADER] start", 6):
                launched = True
                break
        if not launched:
            print("[FAIL] terminal did not launch (icon double-click missed?)")
            return 1
        print("[OK] terminal launched")
        # Wait for the terminal's IPC queue (keyboard routing is live only then),
        # like a real user would wait for the window to appear.
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] terminal never became ready")
            return 1
        time.sleep(1.0)

        # Click inside the terminal window (60,40,600x400 -> center ~(360,240))
        # so keyboard focus lands on it, like a real user would.
        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)

        ok_run = False
        for _ in range(3):
            # Clear any half-typed garbage first, then type fresh.
            for _ in range(24):
                mon_cmd("sendkey backspace")
            for k in RUN_KEYS:
                mon_cmd("sendkey " + k)
                time.sleep(0.12)
            mon_cmd("sendkey ret")
            if wait_for_in_file(SERIAL_LOG, "fork: child", 25):
                ok_run = True
                break
            time.sleep(1.0)
        if not ok_run:
            print("[FAIL] fork() never happened")
            return 1
        print("[OK] fork() created a child task")

        if not wait_for_in_file(SERIAL_LOG, "SYS_EXIT code=0x0000002A", 30):
            print("[FAIL] child did not exit with status 42")
            return 1
        print("[OK] child exited with status 42 (0x2A)")

        # waitpid should have reaped it; let the desktop spin to catch crashes
        time.sleep(5)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after fork/waitpid/signal")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
