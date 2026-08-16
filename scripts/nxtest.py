#!/usr/bin/env python3
"""
scripts/nxtest.py — W^X regression for PAE + NX (v38.49).

Boots mectov.iso, logs in, runs `run /apps/nxtest.mct` and verifies that
EXECUTING CODE ON THE USER STACK is killed by SIGSEGV:

  1. the kernel booted with NX active       ("[MEM] PAE paging on (NX enabled)")
  2. the app placed code on its stack and called it — and DIED
     ("[CRASH] Ring 3 fault" + "SYS_EXIT code=0x0000008B" = 128+SIGSEGV)
  3. the FAIL line never appears (executing data would mean NX is off)
  4. the OS stayed alive afterwards

Usage:
    python3 scripts/nxtest.py [--timeout 240]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

import terminal_launch

SERIAL_LOG = "/tmp/mectov_nx_serial.log"
MON_SOCK = "/tmp/mectov_nx_monitor.sock"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
RUN_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s",
            "slash", "n", "x", "t", "e", "s", "t", "dot", "m", "c", "t", "ret"]


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
            return 1
        print("[OK] booted to login screen")

        if not wait_for_in_file(SERIAL_LOG, "[MEM] PAE paging on (NX enabled)", 15):
            print("[FAIL] PAE boot banner missing NX enabled")
            return 1
        print("[OK] PAE active with NX enabled")

        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)

        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] logged in, desktop running")

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(
                mon_cmd, SERIAL_LOG, "/tmp/mectov_nx_cursor.ppm"):
            print("[FAIL] terminal did not launch (icon double-click missed?)")
            return 1
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
            for _ in range(32):
                mon_cmd("sendkey backspace")
            for k in RUN_KEYS:
                mon_cmd("sendkey " + k)
                time.sleep(0.12)
            if wait_for_in_file(SERIAL_LOG, "NXTEST start", 25):
                ok_run = True
                break
            time.sleep(1.0)
        if not ok_run:
            print("[FAIL] nxtest never started")
            return 1
        print("[OK] nxtest running")

        # THE assertion: the stack-exec fetch fault must be treated as a W^X
        # violation and kill the task (no demand-mapping, no NXTEST FAIL).
        if not wait_for_in_file(SERIAL_LOG, "[W^X] execute fault", 30):
            print("[FAIL] no execute fault - stack code was NOT blocked")
            return 1
        print("[OK] execute fault recognized as W^X violation")
        if not wait_for_in_file(SERIAL_LOG, "[CRASH] Ring 3 fault", 30):
            print("[FAIL] SIGSEGV was not delivered to the task")
            return 1
        print("[OK] task killed by SIGSEGV")

        # And the FAIL line must never appear.
        try:
            with open(SERIAL_LOG, "r", errors="replace") as f:
                if "NXTEST FAIL" in f.read():
                    print("[FAIL] stack code executed - NX is not active")
                    return 1
        except OSError:
            pass
        print("[OK] no NXTEST FAIL line")

        time.sleep(5)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after the NX kill")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
