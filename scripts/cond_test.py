#!/usr/bin/env python3
"""
scripts/cond_test.py — mutex + condition-variable regression test (v38.27).

Boots mectov.iso on 4 SMP cores, logs in, launches the Terminal, runs the
Ring 3 `conddemo` app (`run /apps/conddemo.mct`) and verifies from the
serial log that the futex-based pthread-style primitives hold up under
real parallel load:

  * MUTEX STRESS — 4 threads bump a shared counter LOCK_ITERS times under
    a futex mutex; the final value must be EXACTLY 4 * LOCK_ITERS. On a
    broken lock (lost update on 4 cores) it would land strictly below.
  * PRODUCER/CONSUMER — a bounded buffer (8 slots) with 2 producers and
    2 consumers using condvar wait/signal; every one of the 3000 items
    must be consumed exactly once (no loss, no duplicates, no corruption),
    which exercises both the not-full and not-empty wait paths heavily.

The kernel must never panic during the whole run.

Usage:
    python3 scripts/cond_test.py [--timeout 300]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

import terminal_launch  # corner-reset + screendump-verified icon double-click

SERIAL_LOG = "/tmp/mectov_cond_serial.log"
MON_SOCK = "/tmp/mectov_cond_monitor.sock"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

RUN_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s", "slash",
            "c", "o", "n", "d", "d", "e", "m", "o", "dot", "m", "c", "t", "ret"]


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


def mon_cmd(cmd, read=False):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(3)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(0.15)
        out = b""
        if read:
            try:
                while True:
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    out += chunk
            except socket.timeout:
                pass
        s.close()
        return out.decode(errors="replace")
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")
        return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--kvm", action="store_true",
                    help="run with -enable-kvm (real timing; the CI step stays TCG)")
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
        "-smp", os.environ.get("MCTOV_SMP", "4"),
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        "-net", "none",
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    if args.kvm:
        qemu_cmd.append("-enable-kvm")
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
                mon_cmd, SERIAL_LOG, "/tmp/mectov_cond_cursor.ppm"):
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

        # Run the mutex/condvar demo (retry a few times for keystroke safety).
        for _ in range(3):
            for _ in range(24):
                mon_cmd("sendkey backspace")
            for k in RUN_KEYS:
                mon_cmd("sendkey " + k)
                time.sleep(0.12)
            mon_cmd("sendkey ret")
            if wait_for_in_file(SERIAL_LOG, "[CONDDEMO] ALL PASS", 90):
                break
            time.sleep(1.0)

        if "[CONDDEMO] ALL PASS" not in open(SERIAL_LOG, "r", errors="replace").read():
            print("=== conddemo did NOT finish — dumping all vCPUs ===")
            for c in range(4):
                print(mon_cmd(f"info registers -c {c}", read=True).strip())
            print(mon_cmd("info cpus", read=True).strip())

        with open(SERIAL_LOG, "r", errors="replace") as f:
            log_text = f.read()
        if "[CONDDEMO] FAIL" in log_text:
            print("[FAIL] conddemo reported a failed assertion")
            return 1
        if "[CONDDEMO] ALL PASS" not in log_text:
            print("[FAIL] conddemo never reached [CONDDEMO] ALL PASS")
            return 1
        print("[OK] conddemo completed ([CONDDEMO] ALL PASS)")

        if "[PANIC]" in log_text:
            print("[FAIL] kernel panicked during the condvar run")
            return 1
        print("[OK] no kernel panic in the whole condvar run")

        time.sleep(5)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after the condvar demo")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
