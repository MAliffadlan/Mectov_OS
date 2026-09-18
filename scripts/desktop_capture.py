#!/usr/bin/env python3
"""Boot Mectov OS in QEMU, log in, and grab a desktop screendump.

Shared by scripts/icon_shot.py (manual visual check) and
scripts/icon_render_test.py (assertion-based regression), so the boot/login
choreography lives in exactly one place.

The login sequence mirrors boot_test.py: the lock screen swallows the first
keypress, so a leading space is sent before the password.
"""
import os
import signal
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# name-to-scancode map for sendkey (subset used to type "root\n")
SCAN = {}
for i, row in enumerate([
    "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"
]):
    for j, ch in enumerate(row):
        SCAN[ch] = f"{i+2}-{hex(0x02 + j)[2:]}" if False else None
# Simpler: use known sendkey names for login (matches boot_test.py LOGIN_KEYS style)
LOGIN_SEQ = ["spc", "r", "o", "o", "t", "ret"] if False else None


def mon(sock_path, cmd, settle=0.2):
    """Send one command to the QEMU monitor over a unix socket."""
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(sock_path)
        s.sendall((cmd + "\n").encode())
        time.sleep(settle)
        s.close()
        return True
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")
        return False


def capture(out_ppm, iso=None, disk=None, ext2=None, smp=None,
            serial_log="/tmp/mectov_desktop_serial.log",
            mon_sock="/tmp/mectov_desktop_monitor.sock",
            boot_timeout=180, login_timeout=120, settle=8, password="mectov123"):
    """Boot, log in and screendump the desktop. True on success."""
    iso = iso or os.path.join(ROOT, "mectov.iso")
    disk = disk or os.path.join(ROOT, "disk.img")
    ext2 = ext2 or os.path.join(ROOT, "ext2.img")
    smp = smp or os.environ.get("MCTOV_SMP", "2")

    for p in (serial_log, mon_sock, out_ppm):
        try:
            os.unlink(p)
        except OSError:
            pass
    open(serial_log, "w").close()

    qemu = subprocess.Popen([
        "qemu-system-i386",
        "-cpu", "qemu32,+nx",
        "-vga", "std",
        "-cdrom", iso,
        "-m", "128",
        "-smp", str(smp),
        "-display", "none",
        "-serial", f"file:{serial_log}",
        "-net", "none",
        "-snapshot",
        "-drive", f"file={disk},format=raw,index=0,media=disk",
        "-drive", f"file={ext2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{mon_sock},server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        deadline = time.time() + boot_timeout
        while time.time() < deadline:
            if "[K] login" in open(serial_log, errors="replace").read():
                break
            if qemu.poll() is not None:
                print(f"[FAIL] qemu exited early: {qemu.returncode}")
                return False
            time.sleep(0.5)
        else:
            print("[FAIL] no login screen")
            return False
        print("[OK] login screen")

        for k in ["spc"] + list(password) + ["ret"]:
            mon(mon_sock, f"sendkey {k}", settle=0.15)

        deadline = time.time() + login_timeout
        while time.time() < deadline:
            if "BOOTED KERNEL LOOP" in open(serial_log, errors="replace").read():
                break
            if qemu.poll() is not None:
                print(f"[FAIL] qemu exited early: {qemu.returncode}")
                return False
            time.sleep(0.5)
        else:
            print("[FAIL] login incomplete")
            return False
        print("[OK] desktop up")

        time.sleep(settle)          # let icons/taskbar/wallpaper settle
        mon(mon_sock, f"screendump {out_ppm}", settle=1.0)
        if os.path.exists(out_ppm) and os.path.getsize(out_ppm) > 1000:
            print(f"[OK] screendump -> {out_ppm} ({os.path.getsize(out_ppm)} bytes)")
            return True
        print("[FAIL] no screendump")
        return False
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            qemu.send_signal(signal.SIGKILL)
