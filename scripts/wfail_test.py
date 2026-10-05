#!/usr/bin/env python3
"""
scripts/wfail_test.py — medium write-failure propagation test (v38.159, F1).

The bug this gate exists for: `ext2_write_block()` and `fat32_write_sectors()`
were `void` and dropped `ata_write_sectors_drive()`'s return code, and the VFS
file-data writer ignored it too, so a disk that refuses a write still produced
a *successful* syscall — the in-memory metadata and the page cache then served
the data back, and every self-check inside the guest passed while nothing
reached the medium.

How it forces the failure: QEMU's `blkdebug` block filter is attached to the
FAT32 image with an `inject-error` rule for `write_aio` (errno EIO). Reads work
normally, so the guest boots and mounts /fat32 as usual; every write to that
medium fails at the host level, and the ATA device reports it in its status
register.

Two boots, both with the same image copy. The reporter is apps/wfiledemo.c, a
Ring 3 app that prints exactly what the syscalls told it (`WFILE ...`); the
host-side ground truth is mtools reading the image, never the guest's word.

  * `--mode control` — no injection. Demands the full success path: the app
    prints `WFILE write ok` + `WFILE readback ok`, and host mtools reads back
    the exact 512-byte pattern the app wrote. This is the positive control:
    the gate must not simply make every write fail.
  * `--mode inject` — injection on. Demands the guest does NOT claim success:
    no `WFILE write ok`, at least one reported failure marker, and (host side)
    mtools must NOT find the file the demo creates. Before the fix this boot
    printed `WFILE write ok`, which is exactly the false success the gate is
    written to catch.

Usage:
    python3 scripts/wfail_test.py --mode control [--timeout 240]
    python3 scripts/wfail_test.py --mode inject  [--timeout 240]

The FAT32 fixture is created/seeded via fat32_test.ensure_fat32_image() when
missing, so this works from a clean tree (`make check-wfail`).
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

import fat32_test       # ensure_fat32_image: same fixture every FAT32 gate uses
import terminal_launch  # corner-reset + screendump-verified icon double-click

SERIAL_LOG = "/tmp/mectov_wfail_serial.log"
MON_SOCK = "/tmp/mectov_wfail_monitor.sock"

# The Windows-style lock screen eats the first keypress to dismiss it, so a
# leading space is sent before the password keys.
LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
RUN_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s",
            "slash", "w", "f", "i", "l", "e", "d", "e", "m", "o",
            "dot", "m", "c", "t", "ret"]

# Written by wfiledemo (see apps/wfiledemo.c): 512 bytes of 'A'+i%26.
OS_FILE = "::/wfail_out.txt"
OS_FILE_BYTES = bytes((ord('A') + (i % 26)) for i in range(512))

# apps/wfiledemo.c markers: the syscall results themselves.
M_WRITE_OK = "WFILE write ok"
M_WRITE_BAD = ("WFILE write FAILED", "WFILE write SHORT", "WFILE create FAILED")
M_READBACK_OK = "WFILE readback ok"

INJECT_CONF = """[inject-error]
event = "write_aio"
errno = "5"
"""


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


def read_serial():
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


def prepare_fat32_image(src, dst):
    """Copy the fixture so this test never mutates the repo's fat32.img.

    Uses fat32_test.ensure_fat32_image() to create and seed the image when the
    tree is clean, so this harness does not depend on a previous test having
    run."""
    if fat32_test.ensure_fat32_image(src) != 0:
        print(f"[FAIL] could not prepare {src}")
        return 1
    shutil.copyfile(src, dst)
    with open(dst, "rb") as f:
        f.seek(536 * 512)          # root dir cluster, as in fat32_test.py
        root = f.read(1024)
    if b"HELLO" not in root:
        print(f"[FAIL] {src} has no HELLO.TXT marker — recreate it (mkfs.fat + mcopy)")
        return 1
    return 0


def run_guest(args, img, mode):
    """Boot, log in, launch the Terminal, run /apps/wfiledemo.mct."""
    for p in (SERIAL_LOG, MON_SOCK):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    if mode == "inject":
        conf = os.path.join(os.path.dirname(img), "wfail_blkdebug.conf")
        with open(conf, "w") as f:
            f.write(INJECT_CONF)
        fat32_spec = f"blkdebug:{conf}:{img}"
    else:
        fat32_spec = img

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
        "-drive", f"file={fat32_spec},format=raw,index=3,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE)
    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached the login screen")
            return 1
        print("[OK] booted to login screen")

        if not wait_for_in_file(SERIAL_LOG, "[FAT32] ok", 15):
            print("[FAIL] FAT32 did not mount at boot")
            return 1
        print("[OK] FAT32 mounted")

        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)
        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] logged in")

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(
                mon_cmd, SERIAL_LOG, "/tmp/mectov_wfail_cursor.ppm"):
            print("[FAIL] the Terminal never became ready")
            return 1
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] terminal never became ready")
            return 1
        time.sleep(1.0)

        # Focus the terminal window before typing.
        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)

        started = False
        for _ in range(3):
            for _ in range(24):
                mon_cmd("sendkey backspace")
            for k in RUN_KEYS:
                mon_cmd("sendkey " + k)
                time.sleep(0.12)
            mon_cmd("sendkey ret")
            if wait_for_in_file(SERIAL_LOG, "wfiledemo: ", 25):
                started = True
                break
            time.sleep(1.0)
        if not started:
            print("[FAIL] wfiledemo never started")
            return 1
        print("[OK] wfiledemo launched")

        # Let the demo run to completion (it exits or fails fast).
        for _ in range(args.timeout):
            if "WFILE done" in read_serial() or "SYS_EXIT" in read_serial():
                break
            time.sleep(1)
        return 0
    finally:
        err = b""
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
        if qemu.stderr is not None:
            try:
                err = qemu.stderr.read() or b""
            except (OSError, ValueError):
                err = b""
        if err.strip():
            print("[qemu stderr] " + err.decode("utf-8", "replace").strip()[:400])


def mtools_reads_os_file(img):
    """Host-side ground truth: can mtools read the file the guest claims to have
    written? Returns True/False (None when mtools itself cannot run)."""
    dst = os.path.join(tempfile.gettempdir(), "wfail_lfn_out.txt")
    try:
        os.unlink(dst)
    except FileNotFoundError:
        pass
    r = subprocess.run(["mcopy", "-i", img, OS_FILE, dst],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode != 0:
        return False
    with open(dst, "rb") as f:
        return f.read() == OS_FILE_BYTES


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["control", "inject"], required=True,
                    help="control: writes must work; inject: writes must be reported as failures")
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--fat32", default="fat32.img")
    ap.add_argument("--keep-image", help="debug: use this image instead of a copy")
    args = ap.parse_args()

    tmpdir = tempfile.mkdtemp(prefix="wfail_")
    img = args.keep_image or os.path.join(tmpdir, "fat32.img")
    if prepare_fat32_image(args.fat32, img) != 0:
        return 1

    rc = run_guest(args, img, args.mode)
    if rc != 0:
        return rc

    serial = read_serial()
    claimed_ok = M_WRITE_OK in serial
    bad_markers = [m for m in M_WRITE_BAD if m in serial]
    readback_ok = M_READBACK_OK in serial
    on_media = mtools_reads_os_file(img)

    if args.mode == "control":
        print(f"[info] write ok={claimed_ok}  readback ok={readback_ok}  "
              f"file on media={on_media}  failure markers={bad_markers}")
        if not claimed_ok or not readback_ok:
            print("[FAIL] control run: a healthy medium was reported as failing")
            return 1
        if not on_media:
            print("[FAIL] control run: the guest-reported file is not on the medium")
            return 1
        print("[OK] control: write and read-back succeed, mtools reads the file")
        return 0

    # inject mode: the medium refuses every write.
    print(f"[info] write ok={claimed_ok}  readback ok={readback_ok}  "
          f"file on media={on_media}  failure markers={bad_markers}")

    bad = 0
    if claimed_ok:
        print("[FAIL] the guest reported 'WFILE write ok' while the medium refused "
              "every write (a failed write was reported as success)")
        bad += 1
    if not bad_markers:
        print("[FAIL] the guest never reported a write failure")
        bad += 1
    if on_media:
        print("[FAIL] the file reached a medium that injects write errors")
        bad += 1
    if bad:
        return 1
    print("[OK] inject: the refused write was reported to the writer "
          "(no false success, nothing on the medium)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
