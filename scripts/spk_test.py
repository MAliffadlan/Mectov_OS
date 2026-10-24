#!/usr/bin/env python3
"""
scripts/spk_test.py — SYS_PLAY_SOUND ABI test, end to end from Ring 3.

Boots mectov.iso in QEMU, logs in, launches the Terminal desktop icon, types
`run /apps/spk_test.mct`, and verifies from the serial log that the syscall
answers its callers at all:

  1. every in-range call returns 0, including both edges of the contract
     (20 Hz / 20000 Hz, 0 ms / 5000 ms);
  2. every out-of-range call returns -1 — one step outside each edge, plus a
     negative frequency and a negative duration;
  3. a 3000 ms note returns in well under 500 ms, so a game loop firing an
     effect is not frozen for the length of the note;
  4. the count line reports failures=0 and was not short of checks;
  5. the OS stayed alive afterwards.

Why a guest suite when scripts/spk_deadline_test.py already covers the driver:
the defect this pins was in the WIRING. SYS_PLAY_SOUND was a branch that set
eax=0 and returned — no speaker code was ever reached — and a gate that tests
speaker.c directly cannot see a syscall that never calls it. Only a Ring 3
caller can prove the branch still reaches the driver and still hands back its
verdict. The host gate covers what this one cannot: that the frequency really
reaches the PIT (Ring 3 cannot read port 0x61).

Usage:
    python3 scripts/spk_test.py [--timeout 240]
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import time

import terminal_launch  # corner-reset + screendump-verified icon double-click

SERIAL_LOG = "/tmp/mectov_spk_serial.log"
MON_SOCK = "/tmp/mectov_spk_monitor.sock"

# 9 argument cases + the 3 s non-blocking note + the closing silence.
EXPECTED_CHECKS = 11

# The Windows-style lock screen eats the first keypress to dismiss it, so a
# leading space is sent before the password keys.
LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
# "run /apps/spk_test.mct" — '_' has no QEMU key name, so it is shift-minus
# (the same convention hardening_test.py and pty_test.py use).
RUN_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s", "slash",
            "s", "p", "k", "shift-minus", "t", "e", "s", "t", "dot", "m",
            "c", "t", "ret"]


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


def read_log():
    try:
        with open(SERIAL_LOG, "r", errors="replace") as f:
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
        "-net", "nic,model=rtl8139",
        "-net", "user",
        "-snapshot",
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
        if not terminal_launch.launch_terminal(
                mon_cmd, SERIAL_LOG, "/tmp/mectov_spk_cursor.ppm"):
            print("[FAIL] the Terminal never became ready — see the [launch] report above")
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
            if wait_for_in_file(SERIAL_LOG, "[SPK] SYS_PLAY_SOUND ABI test", 25):
                ok_run = True
                break
            time.sleep(1.0)
        if not ok_run:
            print("[FAIL] spk_test never started")
            return 1
        print("[OK] spk_test launched")

        if not wait_for_in_file(SERIAL_LOG, "[SPK] checks=", 30):
            print("[FAIL] spk_test printed no verdict line (it died early?)")
            return 1

        log = read_log()
        verdict = re.search(r"\[SPK\] checks=(\d+) failures=(\d+)", log)
        if not verdict:
            print("[FAIL] the verdict line did not parse")
            return 1
        checks, failures = int(verdict.group(1)), int(verdict.group(2))

        for line in log.splitlines():
            if line.startswith("[SPK]"):
                print("  " + line.strip())

        if "WRONG" in log:
            print("[FAIL] at least one call got the wrong verdict — see the WRONG "
                  "lines above")
            return 1
        if failures != 0:
            print(f"[FAIL] spk_test reported {failures} failures")
            return 1
        if checks < EXPECTED_CHECKS:
            print(f"[FAIL] only {checks} checks ran, expected {EXPECTED_CHECKS} — "
                  f"a case was lost")
            return 1
        if "[SPK] ALL TESTS PASSED" not in log:
            print("[FAIL] no ALL TESTS PASSED line")
            return 1
        print(f"[OK] {checks} checks, 0 failures")

        time.sleep(5)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        if "[PANIC]" in read_log():
            print("[FAIL] the kernel panicked during the run")
            return 1
        print("[OK] OS stayed alive after the SYS_PLAY_SOUND test")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
