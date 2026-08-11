#!/usr/bin/env python3
"""
scripts/lseekfile_test.py — functional test for lseek/fstat/O_APPEND (v38.12).

Boots mectov.iso in QEMU, logs in, launches the Terminal desktop icon, types
`run /apps/lseekfiledemo.mct`, and verifies from the serial log that the app
exercised the new POSIX file APIs and exited 0:

  1. the demo ran its assertions   ("lseekfiledemo: ALL TESTS PASSED")
  2. the app exited 0              ("SYS_EXIT code=0x00000000")
  3. the OS stayed alive afterwards

Usage:
    python3 scripts/lseekfile_test.py [--timeout 240]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

SERIAL_LOG = "/tmp/mectov_lseek_serial.log"
MON_SOCK = "/tmp/mectov_lseek_monitor.sock"

# The Windows-style lock screen eats the first keypress to dismiss it, so a
# leading space is sent before the password keys.
LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
RUN_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s",
            "slash", "l", "s", "e", "e", "k", "f", "i", "l", "e", "d", "e", "m", "o",
            "dot", "m", "c", "t", "ret"]


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
        for dx, dy in [(-100, -80), (-100, -80), (-100, -76), (-40, 0)]:
            mon_cmd(f"mouse_move {dx} {dy}")
            time.sleep(0.1)
        time.sleep(0.5)
        for _ in range(4):
            mon_cmd("mouse_button 1")
            time.sleep(0.05)
            mon_cmd("mouse_button 0")
            time.sleep(0.05)
        time.sleep(0.3)

        if not wait_for_in_file(SERIAL_LOG, "[LOADER] start", 20):
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
            for _ in range(28):
                mon_cmd("sendkey backspace")
            for k in RUN_KEYS:
                mon_cmd("sendkey " + k)
                time.sleep(0.12)
            mon_cmd("sendkey ret")
            if wait_for_in_file(SERIAL_LOG, "lseekfiledemo: ", 25):
                ok_run = True
                break
            time.sleep(1.0)
        if not ok_run:
            print("[FAIL] lseekfiledemo never started")
            return 1
        print("[OK] demo app launched")

        if not wait_for_in_file(SERIAL_LOG, "lseekfiledemo: ALL TESTS PASSED", 30):
            print("[FAIL] an assertion failed (no ALL TESTS PASSED)")
            return 1
        print("[OK] all lseek/fstat/O_APPEND assertions passed")

        if not wait_for_in_file(SERIAL_LOG, "SYS_EXIT code=0x00000000", 20):
            print("[FAIL] lseekfiledemo did not exit 0")
            return 1
        print("[OK] demo exited 0")

        time.sleep(5)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after lseek/fstat test")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
