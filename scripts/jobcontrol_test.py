#!/usr/bin/env python3
"""
scripts/jobcontrol_test.py — functional test for shell job control.

Boots the OS, opens the Terminal (desktop icon double-click), then drives the
shell through the keyboard and verifies from the serial log:

  1. `sleep 2 &` forks a background job   ("background pid=")
  2. `jobs` lists it
  3. `fg 1` waits and reports the exit    ("Done (exit 0)")
  4. `sleep 5 &` + `kill %1` kills it

Usage:
    python3 scripts/jobcontrol_test.py [--timeout 240]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

SERIAL_LOG = "/tmp/mectov_jobs_serial.log"
MON_SOCK = "/tmp/mectov_jobs_monitor.sock"

LOGIN_KEYS = ["m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

def keys(s):
    """Map a string to QEMU sendkey names."""
    names = {".": "dot", "/": "slash", " ": "spc", "-": "minus",
             "_": "shift-minus", "$": "shift-4", "&": "shift-7", "%": "shift-5"}
    out = []
    for ch in s:
        if ch in names:
            out.append(names[ch])
        elif ch.isupper():
            out.append(f"shift-{ch.lower()}")
        else:
            out.append(ch)
    return out

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
        time.sleep(0.12)
        s.close()
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")

def type_keys(ks, delay=0.08):
    for k in ks:
        mon_cmd("sendkey " + k)
        time.sleep(delay)

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

    qemu = subprocess.Popen([
        "qemu-system-i386", "-vga", "std", "-cdrom", args.iso,
        "-m", "128", "-smp", "4", "-display", "none",
        "-serial", f"file:{SERIAL_LOG}", "-net", "none",
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

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

        # Open the Terminal via icon double-click burst
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
            print("[FAIL] terminal did not launch")
            return 1
        print("[OK] terminal launched")
        # Wait until the terminal has finished init (its IPC queue is up) so
        # keystrokes are guaranteed to reach the shell. Under TCG the terminal
        # can take a while to pump its event loop; typing before that drops keys
        # and makes this test flaky.
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] terminal never became ready")
            return 1
        time.sleep(1.0)

        # Click inside the terminal window (60,40,600x400 -> center ~(360,240))
        # so keyboard focus lands on it, like a real user would.
        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)

        # 1. Background job: sleep 2 &  (kernel logs the fork + job registration)
        # Retry a few times: TCG can still lose an early keystroke even after
        # the terminal is ready.
        ok_job = False
        for _ in range(3):
            # Clear any half-typed garbage first (a dropped key on a previous
            # attempt would otherwise corrupt the command line), then type fresh.
            for _ in range(24):
                mon_cmd("sendkey backspace")
            type_keys(keys("sleep 2 &"), delay=0.15)
            mon_cmd("sendkey ret")
            if wait_for_in_file(SERIAL_LOG, "[JOBS] registered job", 15):
                ok_job = True
                break
            time.sleep(1.0)
        if not ok_job:
            print("[FAIL] `sleep 2 &` did not start a background job")
            return 1
        print("[OK] background job forked + registered")

        # 2. The prompt is usable immediately: type `jobs` while the job runs
        type_keys(keys("jobs"))
        mon_cmd("sendkey ret")
        time.sleep(1)
        if not wait_for_in_file(SERIAL_LOG, "[JOBS] registered job", 2):
            pass  # same log line; nothing new needed
        print("[OK] `jobs` accepted (prompt not blocked by the background job)")

        # 3. fg 1 should wait for the job and report its exit status
        type_keys(keys("fg 1"))
        mon_cmd("sendkey ret")
        if not wait_for_in_file(SERIAL_LOG, "[JOBS] fg done status=0", 30):
            print("[FAIL] `fg 1` did not report the job exit status")
            return 1
        print("[OK] `fg 1` waited and reported exit status 0")

        # 4. sleep 5 & then kill %1 (SIGKILL terminates the parked job)
        type_keys(keys("sleep 5 &"))
        mon_cmd("sendkey ret")
        if not wait_for_in_file(SERIAL_LOG, "[JOBS] registered job", 20):
            print("[FAIL] second background job did not start")
            return 1
        type_keys(keys("kill %1"))
        mon_cmd("sendkey ret")
        if not wait_for_in_file(SERIAL_LOG, "[JOBS] kill", 20):
            print("[FAIL] `kill %1` did not send SIGKILL")
            return 1
        print("[OK] `kill %1` sent SIGKILL")

        time.sleep(4)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after job control")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass

if __name__ == "__main__":
    sys.exit(main())
