#!/usr/bin/env python3
"""
scripts/autolock_test.py — CI test for the idle auto-lock (v38.51).

Boots mectov.iso in QEMU, logs in, launches the Terminal, then verifies:

  1. `locktimeout 20` arms the idle lock; with no keyboard/mouse input the
     login gate appears on its own (no taskbar, bottom-left amber clock).
  2. unlocking with the password restores the desktop AND the session — and,
     crucially, the freshly-unlocked desktop does NOT re-lock immediately
     (the countdown is re-anchored after unlock).
  3. `locktimeout 0` disables the timer: another quiet window of the same
     length passes with no lock.
  4. the OS stays alive through it all.

Exit code 0 = pass, 1 = fail.
"""
import argparse
import os
import socket
import subprocess
import sys
import time

import terminal_launch  # corner-reset + screendump-verified icon double-click

SERIAL_LOG = "/tmp/mectov_autolock_serial.log"
MON_SOCK = "/tmp/mectov_autolock_monitor.sock"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

# 20 s of quiet. TCG makes the PIT drift (ticks_per_sec ~550-1400), so the
# real wall time is somewhere in 14-36 s — the poll budget below covers it.
ARM_SECS = 20
ARM_KEYS = list("locktimeout %d" % ARM_SECS) + ["ret"]
OFF_KEYS = list("locktimeout 0") + ["ret"]


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


def mon_cmd(cmd, wait=0.15):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(wait)
        s.close()
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")


def screendump(path):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    mon_cmd(f"screendump {path}")
    deadline = time.time() + 5
    while time.time() < deadline:
        if os.path.exists(path):
            return True
        time.sleep(0.2)
    return False


def load_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = map(int, line.split())
        f.readline()
        data = f.read()
    px = []
    for i in range(0, len(data), 3):
        px.append((data[i], data[i + 1], data[i + 2]))
    return w, h, px


def px_at(px, w, x, y):
    return px[y * w + x]


def is_taskbar(px, w, h):
    """Desktop = pure-black taskbar strip at the bottom; the login gate has
    wallpaper there instead."""
    r, g, b = px_at(px, w, 400, h - 14)
    return r < 12 and g < 12 and b < 12


def clock_amber(px, w, h):
    """Bottom-left digital clock (amber glyphs) on the lock/login gate."""
    for y in range(h - 165, h - 105, 4):
        for x in range(50, 300, 4):
            r, g, b = px_at(px, w, x, y)
            if r > 160 and g > 120 and b < 130:
                return True
    return False


def is_locked(px, w, h):
    return (not is_taskbar(px, w, h)) and clock_amber(px, w, h)


def wait_screen(pred, dump_path, tries, settle=0.5):
    """Screendump repeatedly until `pred(w, h, px)` is true; returns the last
    (w, h, px) or None."""
    for _ in range(tries):
        time.sleep(settle)
        if not screendump(dump_path):
            continue
        w, h, px = load_ppm(dump_path)
        if pred(w, h, px):
            return (w, h, px)
    return None


def type_line(keys):
    # sendkey wants scancode NAMES: ' ' must become 'spc' (a raw space is an
    # invalid key name and is silently dropped, joining the words together).
    names = {" ": "spc"}
    for k in keys:
        mon_cmd("sendkey " + names.get(k, k))
        time.sleep(0.12)


def send_login_keys():
    type_line(LOGIN_KEYS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=480)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--kvm", action="store_true",
                    help="run with -enable-kvm (much faster, needs /dev/kvm)")
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
        "-snapshot",   # never write the drive images (a run.sh may hold them)
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    if args.kvm:
        qemu_cmd.insert(1, "-enable-kvm")
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached login screen")
            return 1
        print("[OK] booted to login screen")

        send_login_keys()
        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] logged in, desktop running")

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(
                mon_cmd, SERIAL_LOG, "/tmp/mectov_autolock_cursor.ppm"):
            print("[FAIL] terminal did not launch (icon double-click missed?)")
            return 1
        print("[OK] terminal launched")
        wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30)
        time.sleep(1.0)

        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)
        screendump("/tmp/mectov_autolock_desktop.ppm")
        w0, h0, px0 = load_ppm("/tmp/mectov_autolock_desktop.ppm")
        if not is_taskbar(px0, w0, h0):
            print("[FAIL] desktop dump shows no taskbar (not on the desktop?)")
            return 1
        print("[OK] desktop reference captured (taskbar present)")

        # ---- 1. arm the idle auto-lock, then go completely quiet ----
        type_line(ARM_KEYS)
        print(f"[i] armed `locktimeout {ARM_SECS}`, going quiet...")
        locked = wait_screen(
            lambda w, h, px: is_locked(px, w, h),
            "/tmp/mectov_autolock_locked.ppm", 200)   # up to ~100 s of quiet
        if locked is None:
            print("[FAIL] auto-lock never fired after the idle timeout")
            return 1
        print(f"[OK] desktop auto-locked after ~{ARM_SECS} s of no input")

        # ---- 2. unlock restores the desktop and does NOT re-lock ----
        send_login_keys()
        desk = wait_screen(
            lambda w, h, px: is_taskbar(px, w, h),
            "/tmp/mectov_autolock_unlocked.ppm", 40)
        if desk is None:
            print("[FAIL] unlock did not restore the desktop")
            return 1
        print("[OK] unlock restored the desktop")
        # The armed timer is still running from unlock; disable it now (fast:
        # the countdown was re-anchored after unlock, so we have plenty of
        # time before another 20 s could elapse).
        type_line(OFF_KEYS)
        time.sleep(2)

        # ---- 3. `locktimeout 0` disabled: a quiet window longer than the
        # armed timeout must pass with NO lock ----
        no_lock = wait_screen(
            lambda w, h, px: is_locked(px, w, h),
            "/tmp/mectov_autolock_rearmed.ppm", 10)
        if no_lock is not None:
            print("[FAIL] desktop re-locked right after disabling — timer stuck?")
            return 1
        still = wait_screen(
            lambda w, h, px: is_taskbar(px, w, h),
            "/tmp/mectov_autolock_still.ppm", 60)     # ~30 s more of quiet
        if still is None:
            print("[FAIL] desktop gone while idle after `locktimeout 0`")
            return 1
        print("[OK] `locktimeout 0` disabled the timer (desktop stays)")

        time.sleep(2)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
