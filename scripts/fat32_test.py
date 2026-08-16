#!/usr/bin/env python3
"""
scripts/fat32_test.py — functional test for the FAT32 driver (v38.15).

Boots mectov.iso in QEMU with a FAT32 drive attached (secondary slave,
index=3 — index 2 is the CD-ROM), logs in, launches the Terminal desktop
icon, types `run /apps/fat32demo.mct`, and verifies from the serial log
that the Ring 3 app exercised the FAT32 read/write path:

  1. the demo passed every FAT32 assertion ("fat32demo: ALL TESTS PASSED")
  2. the app exited 0                  ("SYS_EXIT code=0x00000000")
  3. the OS stayed alive afterwards

If the FAT32 image does not exist (or lacks the marker files), it is built
with mkfs.fat + mtools (mcopy/mmd), exactly like the CI workflow does.

Usage:
    python3 scripts/fat32_test.py [--timeout 240]
"""
import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time

import terminal_launch  # corner-reset + screendump-verified icon double-click

SERIAL_LOG = "/tmp/mectov_fat32_serial.log"
MON_SOCK = "/tmp/mectov_fat32_monitor.sock"

# The Windows-style lock screen eats the first keypress to dismiss it, so a
# leading space is sent before the password keys.
LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
RUN_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s",
            "slash", "f", "a", "t", "3", "2", "d", "e", "m", "o",
            "dot", "m", "c", "t", "ret"]

HELLO = b"Hello from FAT32 disk!\nline2\n"  # 29 bytes


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


def ensure_fat32_image(path):
    """Create a FAT32 image with marker files if it doesn't already have them."""
    if os.path.exists(path):
        # Recreate when any marker file is missing (fresh image needed). The
        # LFN marker is the UTF-16LE bytes of "The quick bro" inside the
        # long-name entry of "The quick brown fox.txt".
        with open(path, "rb") as f:
            f.seek(536 * 512)  # root dir cluster (first_data=536, root=2)
            root = f.read(1024)
        if b"HELLO" in root and b"T\x00h\x00e\x00 \x00q\x00" in root:
            return 0
    with tempfile.TemporaryDirectory() as td:
        hello = os.path.join(td, "hello.txt")
        with open(hello, "wb") as f:
            f.write(HELLO)
        steps = [
            ["dd", "if=/dev/zero", f"of={path}", "bs=1M", "count=16", "status=none"],
            ["mkfs.fat", "-F", "32", "-S", "512", path],
            ["mcopy", "-i", path, hello, "::HELLO.TXT"],
            ["mcopy", "-i", path, hello, "::hello2.txt"],
            ["mmd", "-i", path, "::docs"],
            ["mcopy", "-i", path, hello, "::docs/note.txt"],
            # Long file names (LFN entries): must round-trip through the driver.
            ["mcopy", "-i", path, hello, "::The quick brown fox.txt"],
            ["mmd", "-i", path, "::My Vacation Photos"],
            ["mcopy", "-i", path, hello, "::My Vacation Photos/summer2026 beach.txt"],
        ]
        for s in steps:
            r = subprocess.run(s, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if r.returncode != 0:
                print(f"[FAIL] image step failed: {' '.join(s)}")
                return 1
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--fat32", default="fat32.img")
    args = ap.parse_args()

    if ensure_fat32_image(args.fat32) != 0:
        print("[FAIL] could not prepare the FAT32 image")
        return 1
    print("[OK] FAT32 image ready")

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
        "-drive", f"file={args.fat32},format=raw,index=3,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached login screen")
            return 1
        print("[OK] booted to login screen")

        # The FAT32 mount must have succeeded during init.
        if not wait_for_in_file(SERIAL_LOG, "[FAT32] ok", 10):
            print("[FAIL] FAT32 did not mount at boot")
            return 1
        print("[OK] FAT32 mounted (drive 3)")

        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)

        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] logged in, desktop running")

        # Open the Terminal via icon double-click (corner-reset + screendump
        # verification — see scripts/terminal_launch.py).
        time.sleep(1.5)
        if not terminal_launch.launch_terminal(
                mon_cmd, SERIAL_LOG, "/tmp/mectov_fat32_cursor.ppm"):
            print("[FAIL] terminal did not launch (icon double-click missed?)")
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
            for _ in range(24):
                mon_cmd("sendkey backspace")
            for k in RUN_KEYS:
                mon_cmd("sendkey " + k)
                time.sleep(0.12)
            mon_cmd("sendkey ret")
            if wait_for_in_file(SERIAL_LOG, "fat32demo: ", 25):
                ok_run = True
                break
            time.sleep(1.0)
        if not ok_run:
            print("[FAIL] fat32demo never started")
            return 1
        print("[OK] demo app launched")

        if not wait_for_in_file(SERIAL_LOG, "fat32demo: ALL TESTS PASSED", 30):
            print("[FAIL] an assertion failed (no ALL TESTS PASSED)")
            return 1
        print("[OK] all FAT32 assertions passed")

        if not wait_for_in_file(SERIAL_LOG, "SYS_EXIT code=0x00000000", 20):
            print("[FAIL] fat32demo did not exit 0")
            return 1
        print("[OK] demo exited 0")

        # Interop: the OS created "/demo/long file name test.txt" with its own
        # LFN writer — host mtools must see it by its full name and read the
        # exact bytes back.
        time.sleep(2)
        lfn_dst = os.path.join(tempfile.gettempdir(), "lfn_out.txt")
        try:
            os.unlink(lfn_dst)
        except FileNotFoundError:
            pass
        mc = subprocess.run(
            ["mcopy", "-i", args.fat32, "::/demo/long file name test.txt", lfn_dst],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if mc.returncode != 0:
            print("[FAIL] mtools cannot read the OS-written LFN file")
            return 1
        with open(lfn_dst, "rb") as f:
            data = f.read()
        if data != b"LONG NAME WRITE OK\n":
            print(f"[FAIL] LFN content mismatch: {data!r}")
            return 1
        print("[OK] mtools reads the OS-written LFN file by its long name")

        time.sleep(5)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after FAT32 test")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
