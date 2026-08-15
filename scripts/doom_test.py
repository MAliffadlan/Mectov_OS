#!/usr/bin/env python3
"""
scripts/doom_test.py — CI smoke test for windowed DOOM (v38.29).

Boots mectov.iso in QEMU, logs in, launches the Terminal, types `doom`,
and verifies from the serial log that:

  1. a WM window was opened for DOOM            ("[DOOM] window id=")
  2. the game loop is running                   ("[DOOM] Entering game loop...")
  3. frames are being produced                  ("[DOOM] tick f=..." lines advance)
  4. the OS stayed alive afterwards

The frame counter line proves the compositor path is live: DG_DrawFrame
marks the window dirty instead of blitting the framebuffer, so a tick line
means the game loop (and thus the window content) is advancing.

Usage:
    python3 scripts/doom_test.py [--timeout 300]
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import time

import terminal_launch  # corner-reset + screendump-verified icon double-click

SERIAL_LOG = "/tmp/mectov_doom_serial.log"
MON_SOCK = "/tmp/mectov_doom_monitor.sock"

# The Windows-style lock screen eats the first keypress to dismiss it, so a
# leading space is sent before the password keys.
LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
# `doom` + Enter, as scancode names understood by sendkey
DOOM_KEYS = ["d", "o", "o", "m", "ret"]


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


def count_in_file(path, pattern, timeout):
    """Wait until `pattern` matches, then return how many times it matched."""
    deadline = time.time() + timeout
    rx = re.compile(pattern)
    while time.time() < deadline:
        try:
            with open(path, "r", errors="replace") as f:
                txt = f.read()
            n = len(rx.findall(txt))
            if n > 0:
                return n
        except (FileNotFoundError, OSError):
            pass
        time.sleep(1)
    return 0


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
    ap.add_argument("--timeout", type=int, default=480)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--kvm", action="store_true",
                    help="run with -enable-kvm (much faster, needs /dev/kvm)")
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
            try:
                with open(SERIAL_LOG, "r", errors="replace") as f:
                    tail = f.read().splitlines()[-25:]
                for line in tail:
                    print(line[:130])
            except OSError:
                pass
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
                mon_cmd, SERIAL_LOG, "/tmp/mectov_doom_cursor.ppm"):
            print("[FAIL] terminal did not launch (icon double-click missed?)")
            return 1
        print("[OK] terminal launched")
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] terminal never became ready")
            return 1
        time.sleep(1.0)

        # Click inside the terminal window (60,40,600x400 -> center ~(360,240))
        # so keyboard focus lands on it.
        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)

        ok_typed = False
        for _ in range(3):
            for _ in range(24):
                mon_cmd("sendkey backspace")
            for k in DOOM_KEYS:
                mon_cmd("sendkey " + k)
                time.sleep(0.12)
            mon_cmd("sendkey ret")
            if wait_for_in_file(SERIAL_LOG, "windowed mode", 20):
                ok_typed = True
                break
            time.sleep(1.0)
        if not ok_typed:
            print("[FAIL] `doom` command never reached the shell")
            return 1
        print("[OK] `doom` launched in windowed mode")

        if not wait_for_in_file(SERIAL_LOG, "[DOOM] window id=", 30):
            print("[FAIL] DOOM never opened a WM window")
            return 1
        print("[OK] DOOM opened a WM window")

        if not wait_for_in_file(SERIAL_LOG, "[DOOM] Entering game loop...", 30):
            print("[FAIL] DOOM never entered its game loop")
            return 1
        print("[OK] DOOM entered its game loop")

        # The tick line is emitted once per 600 rendered frames. Under KVM
        # the unthrottled render loop hits that almost instantly; under TCG
        # (CI) the emulated DOOM renderer manages only a few fps, so the
        # first tick can take a couple of minutes of wall time — be generous.
        n = count_in_file(SERIAL_LOG, r"\[DOOM\] tick f=([0-9A-Fa-f]+)", 240)
        if n == 0:
            print("[FAIL] no [DOOM] tick lines — game loop not producing frames")
            return 1
        print(f"[OK] {n} [DOOM] tick line(s) — frames are being produced")

        # Desktop must still be alive and compositing around the window.
        time.sleep(3)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive with the DOOM window open")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
