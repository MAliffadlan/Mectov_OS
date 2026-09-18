#!/usr/bin/env python3
"""
scripts/procsys_test.py — functional test for v38.91 /proc/sys tunables + nice.

Boots mectov.iso in QEMU, logs in, launches the Terminal (screendump-verified
double-click via terminal_launch), runs `run /apps/procsysdemo.mct`, and
asserts from the serial log that the Ring 3 demo:

  1. /proc/sys/all lists every knob with its range       (index-ok)
  2. zombie_reap_ms write -> readback -> restore          (readback-ok)
  3. out-of-range writes are rejected, knob intact        (reject-ok)
  4. garbage writes are rejected, knob intact             (garbage-ok)
  5. load_window legal change + restore                   (loadwindow-ok)
  6. futex_sweep_div legal change + restore               (sweepdiv-ok)
  7. nice round-trip + boundary clamps                    (nice-ok)
  8. /proc/self/status carries Nice: and CPU%:            (cpu-ok)

The app prints [PROCSYS] FAIL ... markers on any sub-assertion failure; the
test fails if any FAIL marker appears at all.

Usage:
    python3 scripts/procsys_test.py [--timeout 240]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

import terminal_launch  # corner-reset + screendump-verified icon double-click

SERIAL_LOG = "/tmp/mectov_procsys_serial.log"
MON_SOCK = "/tmp/mectov_procsys_monitor.sock"

# The Windows-style lock screen eats the first keypress to dismiss it, so a
# leading space is sent before the password keys.
LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
# `run /apps/procsysdemo.mct` + Enter, as scancode names understood by sendkey
RUN_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s",
            "slash", "p", "r", "o", "c", "s", "y", "s", "d", "e", "m", "o",
            "dot", "m", "c", "t", "ret"]

OK_MARKERS = [
    "index-ok",
    "readback-ok",
    "reject-ok",
    "garbage-ok",
    "loadwindow-ok",
    "sweepdiv-ok",
    "nice-ok",
    "cpu-ok",
    "done",
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


def read_file(path):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read()
    except (FileNotFoundError, OSError):
        return ""


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
            serial = read_file(SERIAL_LOG)
            for line in serial.splitlines()[-25:]:
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
                mon_cmd, SERIAL_LOG, "/tmp/mectov_procsys_cursor.ppm"):
            print("[FAIL] terminal did not launch (icon double-click missed?)")
            return 1
        print("[OK] terminal launched")
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] terminal never became ready")
            return 1
        time.sleep(1.0)

        # Focus the terminal window (60,40,600x400 -> center ~(360,240))
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
            if wait_for_in_file(SERIAL_LOG, "[PROCSYS] done", 60):
                ok_run = True
                break
            time.sleep(1.0)
        if not ok_run:
            print("[FAIL] procsysdemo never completed")
            serial = read_file(SERIAL_LOG)
            for line in serial.splitlines()[-25:]:
                print(line[:130])
            return 1
        print("[OK] procsysdemo ran to completion")

        serial = read_file(SERIAL_LOG)
        missing = [m for m in OK_MARKERS if f"[PROCSYS] {m}" not in serial]
        if missing:
            print(f"[FAIL] missing markers: {missing}")
            return 1
        print("[OK] all /proc/sys + nice markers present")

        app_fails = [l for l in serial.splitlines() if "[PROCSYS] FAIL" in l]
        if app_fails:
            print(f"[FAIL] app-internal failures: {app_fails[:5]}")
            return 1
        print("[OK] no app-internal failures")

        # Let the desktop spin briefly to catch post-demo crashes.
        time.sleep(4)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after /proc/sys exercise")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
