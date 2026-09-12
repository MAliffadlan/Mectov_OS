#!/usr/bin/env python3
"""
scripts/virtio_test.py — regression for the virtio-blk driver (v38.78).

Boots mectov.iso in QEMU with a FAT32 disk behind a TRANSITIONAL
virtio-blk-pci controller (`disable-modern=on`, legacy 1AF4:1001 interface,
drive 12), then verifies a full READ+WRITE round trip through the new
driver, end to end, using the runtime mount table:

  1. the legacy interface came up               ("[VIRTIO] blk ... io=..."
     + "-> drive 0x0000000C" + "[VIRTIO] ready")
  2. `mount /vblk fat32 12` mounts it           ("[MOUNT] mounted /vblk") —
     reading the FAT32 BPB through the virtqueue is the read-path proof
  3. `cp /vblk/HELLO.TXT /vblk/COPY.TXT` — data read + dirent/data write
  4. the HOST reads COPY.TXT back with mtools and byte-compares it — the
     write-path proof (data actually landed on the virtio disk)
  5. per-volume coexistence (the v38.78 mount auto-select): with /vblk
     mounted, `cp /vblk/HELLO.TXT /fat32/F32.TXT` writes through the BOOT
     fat32 volume (drive 3) — the backend must switch back from drive 12.
     The host reads F32.TXT off fat32.img and byte-compares it. Under the
     old single-global backend this silently hit the wrong disk.
  6. the OS stayed alive afterwards, zero [PANIC].

Usage:
    python3 scripts/virtio_test.py [--timeout 240]
"""
import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time

import terminal_launch

SERIAL_LOG = "/tmp/mectov_virtio_serial.log"
MON_SOCK = "/tmp/mectov_virtio_monitor.sock"
CONTENT = b"Hello from a virtio-blk disk!\n"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

KEYS_MOUNT = list("mount /vblk fat32 12") + ["ret"]
KEYS_CP = list("cp /vblk/HELLO.TXT /vblk/COPY.TXT") + ["ret"]
KEYS_CP_FAT32 = list("cp /vblk/HELLO.TXT /fat32/F32.TXT") + ["ret"]


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


def ensure_fat_image(path, hello_name):
    hello = os.path.join(tempfile.gettempdir(), "virtio_hello.txt")
    with open(hello, "wb") as f:
        f.write(CONTENT)
    steps = [
        ["dd", "if=/dev/zero", f"of={path}", "bs=1M", "count=16", "status=none"],
        ["mkfs.fat", "-F", "32", "-S", "512", path],
        ["mcopy", "-i", path, hello, f"::{hello_name}"],
    ]
    for s in steps:
        r = subprocess.run(s, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode != 0:
            print(f"[FAIL] image step failed: {' '.join(s)}")
            return 1
    return 0


def ensure_boot_images(disk, ext2, fat32):
    # Fresh slate every run: the runtime mount points (plus populated
    # children) are persisted by vfs_save, so repeated runs would hit
    # "mount point must be an empty directory" or read stale data.
    # fat32.img is recreated too: step 5 writes F32.TXT into it and the
    # host-side proof requires exactly one known-good copy.
    steps = [
        ["dd", "if=/dev/zero", f"of={disk}", "bs=512", "count=2048", "status=none"],
        ["dd", "if=/dev/zero", f"of={ext2}", "bs=1M", "count=2", "status=none"],
        ["mkfs.ext2", "-F", ext2],
        ["dd", "if=/dev/zero", f"of={fat32}", "bs=1M", "count=16", "status=none"],
        ["mkfs.fat", "-F", "32", "-S", "512", fat32],
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
    ap.add_argument("--fat32", default="fat32.img")
    ap.add_argument("--virtio", default="virtio.img")
    args = ap.parse_args()

    if ensure_boot_images(args.disk, args.ext2, args.fat32) != 0:
        return 1
    if ensure_fat_image(args.virtio, "HELLO.TXT") != 0:
        return 1
    print("[OK] virtio FAT32 image ready")

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
        # IDE fleet (drives 0-3) + one virtio-blk disk (drive 12).
        # disable-modern=on exposes the LEGACY 1AF4:1001 interface the
        # kernel driver targets (no modern caps, no MSI-X).
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-drive", f"file={args.fat32},format=raw,index=3,media=disk",
        "-drive", f"file={args.virtio},format=raw,if=none,id=vd0",
        "-device", "virtio-blk-pci,drive=vd0,disable-modern=on",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached login screen")
            return 1
        print("[OK] booted to login screen")

        # Driver bring-up must have completed during init.
        if not wait_for_in_file(SERIAL_LOG, "[VIRTIO] blk ", 15):
            print("[FAIL] virtio-blk device not detected on PCI")
            return 1
        if not wait_for_in_file(SERIAL_LOG, "-> drive 0x0000000C", 15) or \
           not wait_for_in_file(SERIAL_LOG, "[VIRTIO] ready", 15):
            print("[FAIL] virtio-blk disk did not register as drive 12")
            return 1
        print("[OK] virtio-blk controller + disk as drive 12")

        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)

        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] logged in, desktop running")

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(
                mon_cmd, SERIAL_LOG, "/tmp/mectov_virtio_cursor.ppm"):
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

        # Mount the virtio volume — fat32_init reads the BPB through the
        # virtqueue (read-path proof).
        if not run_shell_cmd(KEYS_MOUNT, "[MOUNT] mounted /vblk",
                             "mount /vblk fat32 12 (BPB read via virtqueue)"):
            return 1
        # Copy a file INSIDE the volume: read HELLO.TXT + write COPY.TXT,
        # both through virtio-blk. (cp prints to the GUI terminal, not
        # serial, so there is no in-session marker — the host-side readback
        # below is the proof that the copy actually executed AND landed.)
        # Retry blindly: on a slow runner the guest can lag several seconds
        # behind the keystrokes, so one 4s window may expire with the copy
        # unstarted (CI once showed this as an empty F32.TXT — created but
        # never written before shutdown). Re-issuing is idempotent
        # (overwrite) and every attempt starts with 48 backspaces, which
        # clears any partial line a delayed previous attempt left behind,
        # so the byte stream stays self-healing no matter the timing.
        for _ in range(3):
            for _ in range(48):
                mon_cmd("sendkey backspace")
            type_line(KEYS_CP)
            time.sleep(8)

        # Coexistence: write through the BOOT fat32 volume (drive 3) while
        # /vblk (drive 12) stays mounted — the backend auto-select must
        # switch back. Same deal: host-side readback is the proof.
        for _ in range(3):
            for _ in range(48):
                mon_cmd("sendkey backspace")
            type_line(KEYS_CP_FAT32)
            time.sleep(8)

        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after the virtio round trip")
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass

    # Host-side proofs: the bytes the guest wrote must be on the images.
    def check_image(img, name, what):
        out = os.path.join(tempfile.gettempdir(), "virtio_copy_out.txt")
        try:
            os.unlink(out)
        except FileNotFoundError:
            pass
        mc = subprocess.run(["mcopy", "-i", img, f"::{name}", out],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if mc.returncode != 0:
            print(f"[FAIL] mtools cannot read {name} off {what}")
            return 1
        with open(out, "rb") as f:
            data = f.read()
        if data != CONTENT:
            print(f"[FAIL] {name} content mismatch: {data!r}")
            return 1
        print(f"[OK] host reads back the exact bytes the guest wrote ({what})")
        return 0

    if check_image(args.virtio, "COPY.TXT", "virtio.img via virtqueue") != 0:
        return 1
    if check_image(args.fat32, "F32.TXT", "fat32.img via auto-select") != 0:
        return 1

    with open(SERIAL_LOG, "r", errors="replace") as f:
        if "[PANIC]" in f.read():
            print("[FAIL] kernel panicked during the virtio run")
            return 1
    print("[OK] no kernel panic in the whole virtio run")
    return 0


if __name__ == "__main__":
    sys.exit(main())
