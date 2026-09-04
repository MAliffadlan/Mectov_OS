#!/usr/bin/env python3
"""
scripts/boot_test.py — CI boot smoke test for Mectov OS.

Boots mectov.iso in QEMU without KVM (works on GitHub Actions runners),
logs in through the QEMU monitor, and verifies the kernel reaches
"BOOTED KERNEL LOOP" in the serial log.

Exit code 0 = boot + login OK, 1 = failure.

The QEMU configuration (CPU model / RAM / SMP count) is parameterizable for
CI's config matrix — combo-only bugs (NX-less CPUs, low RAM, 1-core vs
multi-core) are exactly what the matrix hunts. Defaults match the historical
single-config run.

Usage:
    python3 scripts/boot_test.py [--timeout 180] [--cpu qemu32,+nx]
                                 [--mem-mb 128] [--smp 4]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

SERIAL_LOG = "/tmp/mectov_ci_serial.log"
MON_SOCK = "/tmp/mectov_ci_monitor.sock"

# The Windows-style lock screen eats the first keypress to dismiss it, so a
# leading space is sent before the password keys.
LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]


def wait_for_in_file(path, needle, timeout):
    """Poll `path` until it contains `needle` (or timeout). Returns bool."""
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
    """Send one command to the QEMU monitor over a unix socket."""
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(0.2)
        s.close()
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=240,
                    help="overall wall-clock budget in seconds (TCG is slow)")
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--cpu", default="qemu32,+nx",
                    help="QEMU -cpu model (config matrix: qemu32 / Nehalem)")
    ap.add_argument("--mem-mb", type=int, default=128,
                    help="guest RAM in MB (config matrix: 64 / 256)")
    ap.add_argument("--smp", type=int, default=4,
                    help="guest CPU count (config matrix: 1 / 2 / 4)")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    qemu = subprocess.Popen([
        "qemu-system-i386",
        "-cpu", args.cpu,
        "-vga", "std",
        "-cdrom", args.iso,
        "-m", str(args.mem_mb),
        "-smp", str(args.smp),
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        "-net", "none",
        "-snapshot",   # never write the drive images (a run.sh instance may hold them)
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        # Stage 1: kernel init should reach the login screen.
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached login screen")
            tail = ""
            try:
                with open(SERIAL_LOG, "r", errors="replace") as f:
                    tail = f.read()[-2000:]
            except OSError:
                pass
            print("---- serial tail ----")
            print(tail)
            return 1
        print("[OK] booted to login screen")

        # Stage 2: log in via monitor keyboard injection.
        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)

        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete (no BOOTED KERNEL LOOP)")
            return 1
        print("[OK] login succeeded, desktop loop running")

        # Stage 3: let it spin a moment to catch late crashes/hangs.
        time.sleep(5)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive for the smoke window")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
