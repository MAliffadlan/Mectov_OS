#!/usr/bin/env python3
"""
scripts/lock_test.py — CI test for the lock screen feature (v38.34).

Boots mectov.iso in QEMU, logs in, launches the Terminal, then verifies:

  1. typing `lock` in the shell locks the desktop — the login gate appears
     (no taskbar, bottom-left clock) while the session stays alive.
  2. unlocking with the password restores the desktop AND the terminal
     window is still there (session was NOT reset — pixel-compared to the
     pre-lock dump).
  3. the Ctrl+Alt+L shortcut locks the desktop the same way.
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

SERIAL_LOG = "/tmp/mectov_lock_serial.log"
MON_SOCK = "/tmp/mectov_lock_monitor.sock"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]


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
    """The desktop has a pure-black taskbar strip at the bottom (v38.37); the
    login gate does not (that spot is wallpaper). Sample a point that is on
    the bar when it exists, and require near-black."""
    r, g, b = px_at(px, w, 400, h - 14)
    return r < 12 and g < 12 and b < 12


def clock_amber(px, w, h):
    """Bottom-left digital clock (HH:MM:SS amber glyphs) on the lock screen."""
    for y in range(h - 165, h - 105, 4):
        for x in range(50, 300, 4):
            r, g, b = px_at(px, w, x, y)
            if r > 160 and g > 120 and b < 130:
                return True
    return False


def is_locked(px, w, h):
    return (not is_taskbar(px, w, h)) and clock_amber(px, w, h)


def region_diff(px1, w1, px2, w2, x0, y0, x1, y1):
    total = n = 0
    for y in range(y0, y1, 4):
        for x in range(x0, x1, 4):
            a, b = px1[y * w1 + x], px2[y * w2 + x]
            total += abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])
            n += 1
    return total / max(n, 1)


def wait_screen(pred, dump_path, tries, settle=0.4):
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


def send_login_keys():
    for k in LOGIN_KEYS:
        mon_cmd("sendkey " + k)
        time.sleep(0.12)


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
                mon_cmd, SERIAL_LOG, "/tmp/mectov_lock_cursor.ppm"):
            print("[FAIL] terminal did not launch (icon double-click missed?)")
            return 1
        print("[OK] terminal launched")
        wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30)
        time.sleep(1.0)

        # Focus the terminal (click inside its window), then snapshot the
        # desktop WITH the terminal visible — this is the "session alive"
        # reference for the post-unlock comparison.
        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)
        screendump("/tmp/mectov_lock_desktop.ppm")
        w0, h0, px0 = load_ppm("/tmp/mectov_lock_desktop.ppm")
        if not is_taskbar(px0, w0, h0):
            print("[FAIL] desktop dump shows no taskbar (not on the desktop?)")
            return 1
        print("[OK] desktop reference captured (taskbar present)")

        # ---- 1. `lock` shell command locks the desktop ----
        for k in ["l", "o", "c", "k", "ret"]:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)
        locked = wait_screen(
            lambda w, h, px: is_locked(px, w, h),
            "/tmp/mectov_lock_locked.ppm", 25)
        if locked is None:
            print("[FAIL] `lock` command did not show the login gate")
            return 1
        print("[OK] `lock` command locked the desktop")

        # ---- 2. Unlock restores the desktop AND the session survived ----
        send_login_keys()
        desk = wait_screen(
            lambda w, h, px: is_taskbar(px, w, h),
            "/tmp/mectov_lock_unlocked.ppm", 25)
        if desk is None:
            print("[FAIL] unlock did not restore the desktop")
            return 1
        print("[OK] unlock restored the desktop")
        _, _, px2 = desk
        w2 = 1024
        # The terminal window content must match the pre-lock dump: the
        # session was NOT reset, so the window (and its state) survived.
        d = region_diff(px0, w0, px2, w2, 150, 150, 550, 380)
        print(f"[i] terminal region diff pre-lock vs post-unlock: {d:.1f}")
        if d >= 30:
            print("[FAIL] terminal window changed a lot — session was reset?")
            return 1
        print("[OK] terminal survived the lock (session preserved)")

        # ---- 3. Ctrl+Alt+L shortcut locks too ----
        mon_cmd("sendkey ctrl-alt-l")
        locked2 = wait_screen(
            lambda w, h, px: is_locked(px, w, h),
            "/tmp/mectov_lock_shortcut.ppm", 25)
        if locked2 is None:
            print("[FAIL] Ctrl+Alt+L did not lock the desktop")
            return 1
        print("[OK] Ctrl+Alt+L locked the desktop")

        send_login_keys()
        desk2 = wait_screen(
            lambda w, h, px: is_taskbar(px, w, h),
            "/tmp/mectov_lock_final.ppm", 25)
        if desk2 is None:
            print("[FAIL] unlock after shortcut did not restore the desktop")
            return 1
        print("[OK] desktop restored after shortcut unlock")

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
