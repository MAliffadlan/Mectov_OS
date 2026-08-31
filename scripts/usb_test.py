#!/usr/bin/env python3
"""
scripts/usb_test.py — regression for the xHCI/USB 3.0 driver (v38.56).

Boots mectov.iso in QEMU with a qemu-xhci controller and a FAT32 disk
behind usb-storage attached to the controller's SuperSpeed bus (drive 8),
then verifies a full READ+WRITE round trip through the new driver, end to
end, using the runtime mount table:

  1. the controller came up                    ("[XHCI] controller @"
     + "port ... -> drive 8" + "[XHCI] ready")
  2. the stick really is USB 3.0               ("USB3 -> drive 0x00000008")
  3. `mount /usb fat32 8` mounts it            ("[MOUNT] mounted /usb") —
     reading the FAT32 BPB through xHCI/BOT/SCSI is the read-path proof
  4. `cp /usb/HELLO.TXT /usb/COPY.TXT` — data read + dirent/data write
  5. the HOST reads COPY.TXT back with mtools and byte-compares it — the
     write-path proof (data actually landed on the USB disk)
  6. the OS stayed alive afterwards

Usage:
    python3 scripts/usb_test.py [--timeout 240]
"""
import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time

import terminal_launch

SERIAL_LOG = "/tmp/mectov_usb_serial.log"
MON_SOCK = "/tmp/mectov_usb_monitor.sock"
CONTENT = b"Hello from a USB 3.0 stick via xHCI!\n"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

KEYS_MOUNT = list("mount /usb fat32 8") + ["ret"]
KEYS_CP = list("cp /usb/HELLO.TXT /usb/COPY.TXT") + ["ret"]


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


def type_line(keys):
    # sendkey wants scancode NAMES, not raw characters, and names are
    # case-sensitive: an uppercase key must be sent as shift-<lowercase>
    # (sendkey H alone is an invalid name and is silently dropped).
    names = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "_": "shiftd minus"}
    for k in keys:
        if k.isupper():
            mon_cmd("sendkey shift-" + k.lower())
        else:
            mon_cmd("sendkey " + names.get(k, k))
        time.sleep(0.12)


def run_shell_cmd(keys, needle, what, wait=25):
    for _ in range(3):
        for _ in range(48):
            mon_cmd("sendkey backspace")
        type_line(keys)
        if wait_for_in_file(SERIAL_LOG, needle, wait):
            print(f"[OK] {what}")
            return True
        time.sleep(1.0)
    print(f"[FAIL] {what} (missing '{needle}')")
    return False


def ensure_usb_image(path):
    hello = os.path.join(tempfile.gettempdir(), "usb_hello.txt")
    with open(hello, "wb") as f:
        f.write(CONTENT)
    steps = [
        ["dd", "if=/dev/zero", f"of={path}", "bs=1M", "count=16", "status=none"],
        ["mkfs.fat", "-F", "32", "-S", "512", path],
        ["mcopy", "-i", path, hello, "::HELLO.TXT"],
    ]
    for s in steps:
        r = subprocess.run(s, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode != 0:
            print(f"[FAIL] image step failed: {' '.join(s)}")
            return 1
    return 0


def ensure_boot_images(disk, ext2):
    # Mirror CI's "Create disk images" step. The runtime mount point /usb
    # (plus its populated children) is persisted into the root fs (ext2.img)
    # by vfs_save, so repeated local runs would otherwise hit "mount point
    # must be an empty directory" — regenerate the boot images for a clean
    # slate every run.
    steps = [
        ["dd", "if=/dev/zero", f"of={disk}", "bs=512", "count=2048", "status=none"],
        ["dd", "if=/dev/zero", f"of={ext2}", "bs=1M", "count=2", "status=none"],
        ["mkfs.ext2", "-F", ext2],
    ]
    for s in steps:
        r = subprocess.run(s, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode != 0:
            print(f"[FAIL] boot image step failed: {' '.join(s)}")
            return 1
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--usb", default="usb.img")
    args = ap.parse_args()

    if ensure_boot_images(args.disk, args.ext2) != 0:
        return 1
    if ensure_usb_image(args.usb) != 0:
        return 1
    print("[OK] USB FAT32 image ready")

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
        # IDE fleet (drives 0-3) + one USB3 stick behind qemu-xhci (drive 8):
        # bus=xhci0.0 is the SuperSpeed bus, so the device enumerates at
        # speed 5000 (PORTSC speed = 4) — a genuine USB 3.0 round trip.
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-device", "qemu-xhci,id=xhci0",
        "-drive", f"file={args.usb},format=raw,if=none,id=usbd0",
        "-device", "usb-storage,drive=usbd0,bus=xhci0.0",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached login screen")
            return 1
        print("[OK] booted to login screen")

        # Controller bring-up must have completed during init.
        if not wait_for_in_file(SERIAL_LOG, "[XHCI] controller @", 15):
            print("[FAIL] xHCI controller not detected")
            return 1
        if not wait_for_in_file(SERIAL_LOG, "USB3 -> drive 0x00000008", 15) or \
           not wait_for_in_file(SERIAL_LOG, "[XHCI] ready", 15):
            print("[FAIL] USB stick did not register as SuperSpeed drive 8")
            return 1
        print("[OK] xHCI controller + USB 3.0 stick as drive 8")

        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)

        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] logged in, desktop running")

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(
                mon_cmd, SERIAL_LOG, "/tmp/mectov_usb_cursor.ppm"):
            print("[FAIL] terminal did not launch (icon double-click missed?)")
            return 1
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] terminal never became ready")
            return 1
        time.sleep(1.0)

        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)

        # Mount the USB volume — fat32_init reads the BPB THROUGH xHCI.
        if not run_shell_cmd(KEYS_MOUNT, "[MOUNT] mounted /usb",
                             "mount /usb fat32 8 (BPB read via xHCI)"):
            return 1
        # Copy a file INSIDE the volume: read HELLO.TXT + write COPY.TXT,
        # both through the USB driver. (cp prints to the GUI terminal, not
        # serial, so there is no in-session marker — the host-side readback
        # below is the proof that the copy actually executed AND landed.)
        for _ in range(48):
            mon_cmd("sendkey backspace")
        type_line(KEYS_CP)
        time.sleep(4)

        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after the USB round trip")
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass

    # Host-side proof: the bytes the guest wrote must be on the USB image.
    out = os.path.join(tempfile.gettempdir(), "usb_copy_out.txt")
    try:
        os.unlink(out)
    except FileNotFoundError:
        pass
    mc = subprocess.run(["mcopy", "-i", args.usb, "::COPY.TXT", out],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if mc.returncode != 0:
        print("[FAIL] mtools cannot read COPY.TXT off the USB image")
        return 1
    with open(out, "rb") as f:
        data = f.read()
    if data != CONTENT:
        print(f"[FAIL] COPY.TXT content mismatch: {data!r}")
        return 1
    print("[OK] host reads back the exact bytes the guest wrote over USB 3.0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
