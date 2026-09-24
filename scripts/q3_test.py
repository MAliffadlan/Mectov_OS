#!/usr/bin/env python3
"""
scripts/q3_test.py — end-to-end test for the engine-core port (v38.98 phase 1,
now running on id Software's official source: third_party/q3a).

Boots mectov.iso (512MB RAM — the Q3 hunk/zone budget needs the enlarged
kernel heap), logs in, launches the Terminal, then types `q3` and asserts
the engine's liveness via serial markers:

  [Q3] Starting Quake III Arena (id Software source) on Mectov OS...
  Q3 1.32b ... linux-i386 <date>        (Com_Init banner via Sys_Print; the
                                         1.32b string is id's own Q3_VERSION
                                         from code/game/q_shared.h)
  [Q3] Com_Init returned — ticking Com_Frame x10
  [Q3] 10 Com_Frame ticks done — engine core alive

Usage:
    python3 scripts/q3_test.py [--timeout 240]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

import terminal_launch

SERIAL_LOG = "/tmp/mectov_q3_serial.log"
MON_SOCK = "/tmp/mectov_q3_monitor.sock"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
Q3_KEYS = ["q", "3", "ret"]

OK_MARKERS = [
    "[Q3] Starting Quake III Arena",
    "Q3 1.32b",
    "[Q3] Com_Init returned",
    "[Q3] 10 Com_Frame ticks done — engine core alive",
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


def type_line(keys, retries=3, ready_marker=None, timeout=60):
    for _ in range(retries):
        for _ in range(24):
            mon_cmd("sendkey backspace")
        for k in keys:
            mon_cmd("sendkey " + k)
            time.sleep(0.12)
        mon_cmd("sendkey ret")
        if wait_for_in_file(SERIAL_LOG, ready_marker, timeout):
            return True
        time.sleep(1.0)
    return False


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
        "-m", "512",
        "-smp", "2",
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
                mon_cmd, SERIAL_LOG, "/tmp/mectov_q3_cursor.ppm"):
            print("[FAIL] the Terminal never became ready — see the [launch] report above")
            return 1
        print("[OK] terminal launched")
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] terminal never became ready")
            return 1
        time.sleep(1.0)

        # Focus the terminal window center, then run `q3`.
        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)

        if not type_line(Q3_KEYS, retries=2, ready_marker="engine core alive", timeout=60):
            print("[FAIL] q3 engine core never became alive")
            serial = read_file(SERIAL_LOG)
            for line in serial.splitlines()[-30:]:
                print(line[:130])
            return 1
        print("[OK] q3 engine core alive")

        serial = read_file(SERIAL_LOG)
        missing = [m for m in OK_MARKERS if m not in serial]
        if missing:
            print(f"[FAIL] missing markers: {missing}")
            return 1
        print("[OK] all q3 markers present")

        # Let the engine tick a while longer to catch post-init crashes.
        time.sleep(5)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive with the Q3 engine core running")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
