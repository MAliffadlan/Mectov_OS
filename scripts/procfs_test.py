#!/usr/bin/env python3
"""
scripts/procfs_test.py — functional test for v38.85 /proc/<pid> + symlinks.

Boots mectov.iso in QEMU, logs in, launches the Terminal (screendump-verified
double-click via terminal_launch), runs `run /apps/procfsdemo.mct`, and
asserts from the serial log that the Ring 3 demo:

  1. read /proc/self/status with real per-process content (self-status-ok)
  2. read /proc/tasks listing                    (kernel-status-ok)
  3. created a symlink /bin/term -> /apps/terminal.mct (symlink-created)
  4. opened THROUGH the symlink and got the target's MCT1 magic
     (symlink-open-ok — the walker followed the link transparently)
  5. read the link target back via readlink       (readlink-ok)

The app prints [PROCFSDemo] FAIL ... markers on any sub-assertion failure;
the test fails if any FAIL marker appears at all, so partial regressions in
the demo's internal checks are caught, not just its completion.

Usage:
    python3 scripts/procfs_test.py [--timeout 240]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

import terminal_launch  # corner-reset + screendump-verified icon double-click

SERIAL_LOG = "/tmp/mectov_procfs_serial.log"
MON_SOCK = "/tmp/mectov_procfs_monitor.sock"

# The Windows-style lock screen eats the first keypress to dismiss it, so a
# leading space is sent before the password keys.
LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
# `run /apps/procfsdemo.mct` + Enter
RUN_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s",
            "slash", "p", "r", "o", "c", "f", "s", "d", "e", "m", "o",
            "dot", "m", "c", "t", "ret"]

OK_MARKERS = [
    "self-status-ok",
    "kernel-status-ok",
    "symlink-created",
    "symlink-open-ok",
    "readlink-ok",
    # v38.86 hard-link + stat section (all mandatory)
    "hardlink-created",
    "hardlink-nlink-ok",
    "hardlink-shared-ds",
    "hardlink-write-through",
    "symlink-stat-ok",
    "hardlink-delete-ok",
    "links-done",
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
                mon_cmd, SERIAL_LOG, "/tmp/mectov_procfs_cursor.ppm"):
            print("[FAIL] the Terminal never became ready — see the [launch] report above")
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
            if wait_for_in_file(SERIAL_LOG, "[PROCFSDemo] done", 40):
                ok_run = True
                break
            time.sleep(1.0)
        if not ok_run:
            print("[FAIL] procfsdemo never completed")
            serial = read_file(SERIAL_LOG)
            for line in serial.splitlines()[-25:]:
                print(line[:130])
            return 1
        print("[OK] procfsdemo ran to completion")

        serial = read_file(SERIAL_LOG)
        # symlink-created vs symlink-exists are EITHER-OR: a persistent disk
        # may carry a root-owned seed the app cannot delete, in which case
        # only the "exists" marker fires. Everything else is mandatory.
        mandatory = [m for m in OK_MARKERS if m != "symlink-created"]
        missing = [m for m in mandatory if f"[PROCFSDemo] {m}" not in serial]
        if missing:
            print(f"[FAIL] missing markers: {missing}")
            return 1
        if "symlink-created" not in serial and "symlink-exists (ok)" not in serial:
            print("[FAIL] neither symlink-created nor symlink-exists seen")
            return 1
        print("[OK] all /proc + symlink markers present")

        app_fails = [l for l in serial.splitlines() if "[PROCFSDemo] FAIL" in l]
        if app_fails:
            # An app-internal FAIL marker means an internal assertion failed
            # even though the app reached `done`.
            print(f"[FAIL] app-internal failures: {app_fails[:5]}")
            return 1
        print("[OK] no app-internal failures")

        # Let the desktop spin briefly to catch post-demo crashes.
        time.sleep(4)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after /proc + symlink exercise")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
