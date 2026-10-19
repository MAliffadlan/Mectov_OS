#!/usr/bin/env python3
"""scripts/setpgid_test.py — process-group authorization gate (audit H9).

`SYS_SETPGID` used to accept any pgid. Since a task's pgrp is the key that
`SYS_FB_MAP` (and terminal input) authorize on, a plain Ring 3 app could join
the foreground group and take the scanout. The fix allows an app to create its
OWN group (pgid == target) but refuses to join an EXISTING one unless the
caller is the trusted shell (or root/kernel).

This suite drives the shipped `apps/bgread.mct` as UID 1000. Its child moves
to its own background group, then attempts:

  * setpgid(0, tcgetpgrp())  -> must be refused (-1)
  * sys_fb_map()             -> must be refused (-1), because the child is
                                still in its own background group

The existing SIGTTIN path is kept as the control: the child must still stop
on the first terminal read and resume after the parent's SIGCONT. A kernel
panic or a successful join/fb_map fails the gate.

Usage: python3 scripts/setpgid_test.py [--timeout 240]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

import terminal_launch

SERIAL_LOG = "/tmp/mectov_setpgid_serial.log"
MON_SOCK = "/tmp/mectov_setpgid_monitor.sock"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
RUN_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s",
            "slash", "b", "g", "r", "e", "a", "d", "dot", "m", "c", "t", "ret"]


def wait_for_in_file(path, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, "r", errors="replace") as f:
                if needle in f.read():
                    return True
        except (FileNotFoundError, OSError):
            pass
        time.sleep(0.5)
    return False


def read_log(path):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read()
    except (FileNotFoundError, OSError):
        return ""


def mon_cmd(cmd, wait=0.15):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(wait)
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
        "-snapshot",
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached the login screen")
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
                mon_cmd, SERIAL_LOG, "/tmp/mectov_setpgid_cursor.ppm"):
            print("[FAIL] the Terminal never became ready")
            return 1
        print("[OK] terminal launched")

        # Focus the terminal and run the demo. Retry the command once in case
        # the first Enter lands before the shell prompt is ready.
        mon_cmd("mouse_move 300 176")
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)
        launched = False
        for _ in range(2):
            for _ in range(24):
                mon_cmd("sendkey backspace")
            for k in RUN_KEYS:
                mon_cmd("sendkey " + k)
                time.sleep(0.12)
            mon_cmd("sendkey ret")
            if wait_for_in_file(SERIAL_LOG, "[BGREAD] starting", 20):
                launched = True
                break
            time.sleep(1.0)
        if not launched:
            print("[FAIL] bgread never started")
            return 1
        print("[OK] bgread launched as UID 1000")

        if not wait_for_in_file(SERIAL_LOG, "[BGREAD] child join_ok=0", 20):
            out = read_log(SERIAL_LOG)
            print("[FAIL] setpgid(0, tcgetpgrp()) was not refused")
            for ln in out.splitlines():
                if "join_ok" in ln:
                    print("       " + ln.strip())
            return 1
        print("[OK] joining the foreground group was refused")

        if not wait_for_in_file(SERIAL_LOG, "[BGREAD] child fb_ok=0", 20):
            out = read_log(SERIAL_LOG)
            print("[FAIL] SYS_FB_MAP was not refused for the background child")
            for ln in out.splitlines():
                if "fb_ok" in ln:
                    print("       " + ln.strip())
            return 1
        print("[OK] SYS_FB_MAP was refused")

        # Control: the refusal must not have broken the existing group logic.
        if not wait_for_in_file(SERIAL_LOG, "[BGREAD] child resumed after SIGTTIN", 30):
            print("[FAIL] the child did not stop and resume via SIGTTIN/SIGCONT")
            return 1
        if not wait_for_in_file(SERIAL_LOG, "[BGREAD] DONE", 30):
            print("[FAIL] bgread never completed")
            return 1
        print("[OK] SIGTTIN/SIGCONT control still passes")

        if "[PANIC]" in read_log(SERIAL_LOG):
            print("[FAIL] kernel panic on the run")
            return 1
        print("[OK] no kernel panic")
        time.sleep(3)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after the setpgid test")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
