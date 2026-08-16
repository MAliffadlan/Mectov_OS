#!/usr/bin/env python3
"""
scripts/poweroff_test.py — regression test for ACPI S5 poweroff (v38.45).

Boots mectov.iso, logs in, opens the Terminal, types `shutdown` and asserts
that the QEMU PROCESS EXITS BY ITSELF: the kernel now parses the firmware
FADT (PM1a/b control blocks) + the DSDT \\_S5 package and writes the real
sleeping type + SLP_EN, which QEMU honours by ending the VM. The serial log
must show the FADT was found at boot ("[ACPI] FADT:") and the S5 path was
taken ("[ACPI] S5 poweroff").

Usage:
    python3 scripts/poweroff_test.py [--timeout 240]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

import terminal_launch

SERIAL_LOG = "/tmp/mectov_poweroff_serial.log"
MON_SOCK = "/tmp/mectov_poweroff_monitor.sock"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
SHUTDOWN_KEYS = ["s", "h", "u", "t", "d", "o", "w", "n", "ret"]


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

        # The FADT must have been located during init for S5 to be possible.
        if not wait_for_in_file(SERIAL_LOG, "[ACPI] FADT:", 10):
            print("[FAIL] FADT was not found at boot")
            return 1
        print("[OK] FADT parsed (PM1a/b control blocks known)")

        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)

        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] logged in, desktop running")

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(
                mon_cmd, SERIAL_LOG, "/tmp/mectov_poweroff_cursor.ppm"):
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

        for k in SHUTDOWN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)
        print("[OK] typed 'shutdown'")

        # THE assertion: the ACPI S5 write must make QEMU exit by itself.
        if not wait_for_in_file(SERIAL_LOG, "[ACPI] S5 poweroff", 20):
            print("[FAIL] the S5 poweroff path never ran")
            return 1
        print("[OK] S5 poweroff path taken")

        deadline = time.time() + 30
        while time.time() < deadline:
            if qemu.poll() is not None:
                print(f"[OK] QEMU exited by itself (code {qemu.returncode}) — machine powered off")
                return 0
            time.sleep(1)
        print("[FAIL] QEMU is still running 30s after shutdown — S5 did not take")
        return 1
    finally:
        if qemu.poll() is None:
            qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
