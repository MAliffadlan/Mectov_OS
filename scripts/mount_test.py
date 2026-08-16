#!/usr/bin/env python3
"""
scripts/mount_test.py — regression test for the runtime mount table (v38.42).

Boots mectov.iso with the FAT32 drive attached, logs in, opens the Terminal
and drives the umount/mount roundtrip through the serial log:

  1. boot mount of /fat32 succeeded           ("[FAT32] ok")
  2. `umount /fat32` unregisters the mount    ("unmounted /fat32")
  3. `umount /apps` is refused                ("not a mount point")
  4. `mount /fat32 fat32 3` re-mounts it      (`mount` listing shows
     "/fat32 type=fat32")
  5. fat32demo passes on the REMOUNTED fs     ("ALL TESTS PASSED") — proves
     the runtime mount serves real data end to end
  6. the OS stayed alive afterwards

Usage:
    python3 scripts/mount_test.py [--timeout 240]
"""
import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time

import terminal_launch

SERIAL_LOG = "/tmp/mectov_mount_serial.log"
MON_SOCK = "/tmp/mectov_mount_monitor.sock"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

UMOUNT_KEYS = ["u", "m", "o", "u", "n", "t", "spc", "slash", "f", "a", "t", "3", "2", "ret"]
UMOUNT_NEG_KEYS = ["u", "m", "o", "u", "n", "t", "spc", "slash", "a", "p", "p", "s", "ret"]
MOUNT_KEYS = ["m", "o", "u", "n", "t", "spc", "slash", "f", "a", "t", "3", "2",
              "spc", "f", "a", "t", "3", "2", "spc", "3", "ret"]
MOUNT_LIST_KEYS = ["m", "o", "u", "n", "t", "ret"]
RUN_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s",
            "slash", "f", "a", "t", "3", "2", "d", "e", "m", "o", "dot", "m", "c", "t", "ret"]


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


def type_keys(keys):
    for k in keys:
        mon_cmd("sendkey " + k)
        time.sleep(0.12)


def ensure_fat32_image(path):
    # Byte-identical to the FAT32 regression's image: fat32demo asserts the
    # EXACT content of every file it reads, so the content and file set
    # (HELLO.TXT, hello2.txt, docs/note.txt, both LFN files) must match
    # scripts/fat32_test.py.
    hello = os.path.join(tempfile.gettempdir(), "mounttest_hello.txt")
    with open(hello, "wb") as f:
        f.write(b"Hello from FAT32 disk!\nline2\n")   # 29 bytes
    steps = [
        ["dd", "if=/dev/zero", f"of={path}", "bs=1M", "count=16", "status=none"],
        ["mkfs.fat", "-F", "32", "-S", "512", path],
        ["mcopy", "-i", path, hello, "::HELLO.TXT"],
        ["mcopy", "-i", path, hello, "::hello2.txt"],
        ["mmd", "-i", path, "::docs"],
        ["mcopy", "-i", path, hello, "::docs/note.txt"],
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


def run_shell_cmd(keys, needle, what, wait=25):
    for _ in range(3):
        for _ in range(32):
            mon_cmd("sendkey backspace")
        type_keys(keys)
        if wait_for_in_file(SERIAL_LOG, needle, wait):
            print(f"[OK] {what}")
            return True
        time.sleep(1.0)
    print(f"[FAIL] {what} (missing '{needle}')")
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--fat32", default="fat32.img")
    args = ap.parse_args()

    if ensure_fat32_image(args.fat32) != 0:
        return 1
    print("[OK] FAT32 image ready")

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
        "-drive", f"file={args.fat32},format=raw,index=3,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached login screen")
            return 1
        print("[OK] booted to login screen")

        if not wait_for_in_file(SERIAL_LOG, "[FAT32] ok", 10):
            print("[FAIL] FAT32 did not mount at boot")
            return 1
        print("[OK] FAT32 boot mount registered")

        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)

        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] logged in, desktop running")

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(
                mon_cmd, SERIAL_LOG, "/tmp/mectov_mount_cursor.ppm"):
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

        # umount the boot mount, expect refusal on a non-mount path, then
        # remount through the runtime path.
        if not run_shell_cmd(UMOUNT_KEYS, "unmounted /fat32",
                             "umount /fat32 unregistered the mount"):
            return 1
        if not run_shell_cmd(UMOUNT_NEG_KEYS, "not a mount point",
                             "umount /apps refused"):
            return 1
        if not run_shell_cmd(MOUNT_KEYS, "[MOUNT] mounted ",
                             "mount /fat32 fat32 3 re-mounted"):
            return 1
        # The `mount` listing proves the runtime registration (distinct from
        # the "unmounted" line above, which also contains "mounted /fat32").
        if not run_shell_cmd(MOUNT_LIST_KEYS, "[MOUNT] /fat32 type=fat32",
                             "mount listing shows the runtime mount"):
            return 1
        if not run_shell_cmd(RUN_KEYS, "fat32demo: ALL TESTS PASSED",
                             "fat32demo passed on the remounted fs"):
            return 1

        time.sleep(5)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after the mount roundtrip")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
