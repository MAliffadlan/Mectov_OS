#!/usr/bin/env python3
"""
scripts/canary_test.py — stack-protector self-test.

Builds a boot ISO whose GRUB entry passes `canary_self_test` to the kernel.
The kernel then deliberately overflows a 16-byte stack buffer with 20 bytes
inside a noinline function compiled with -fstack-protector-strong:

  1. the probe ran                        ("[CANARY] self-test")
  2. the canary fired                     ("[KERNEL PANIC] STACK SMASHING DETECTED")
  3. every core dumped registers          ("[PANIC] CPU 0".."[PANIC] CPU 3")
  4. the panic tail halted                ("[PANIC] halted")

Without the stack protector this path either survives (no panic) or panics
somewhere else; both fail this gate.

Usage:
    python3 scripts/canary_test.py [--timeout 180]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

SERIAL_LOG = "/tmp/mectov_canary_serial.log"


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


def build_iso(bin_path, iso_path):
    """Build a boot ISO whose GRUB entry passes the canary boot arg."""
    tmp = tempfile.mkdtemp(prefix="mectov_canary_iso_")
    try:
        os.makedirs(os.path.join(tmp, "boot", "grub"), exist_ok=True)
        shutil.copy(bin_path, os.path.join(tmp, "boot", "myos.bin"))
        with open(os.path.join(tmp, "boot", "grub", "grub.cfg"), "w") as f:
            f.write("set timeout=0\n"
                    "set default=0\n"
                    "menuentry \"Mectov OS (canary self-test)\" {\n"
                    "    multiboot /boot/myos.bin canary_self_test\n"
                    "    boot\n"
                    "}\n")
        r = subprocess.run(["grub-mkrescue", "-o", iso_path, tmp],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return r.returncode == 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--bin", default="myos.bin")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--keep-iso", default=None,
                    help="write the test ISO here instead of a temp file")
    args = ap.parse_args()

    try:
        os.unlink(SERIAL_LOG)
    except FileNotFoundError:
        pass

    iso = args.keep_iso or "/tmp/mectov_canary.iso"
    if not build_iso(args.bin, iso):
        print("[FAIL] grub-mkrescue failed to build the canary-test ISO")
        return 1
    print("[OK] built canary-test ISO (canary_self_test)")

    qemu_cmd = [
        "qemu-system-i386",
        "-cpu", "qemu32,+nx",
        "-vga", "std",
        "-cdrom", iso,
        "-m", "128",
        "-smp", "4",
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        "-net", "none",
        "-snapshot",
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
    ]
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[CANARY] self-test", args.timeout):
            print("[FAIL] canary self-test probe never ran")
            return 1
        print("[OK] canary self-test probe ran")

        if not wait_for_in_file(SERIAL_LOG, "[KERNEL PANIC] STACK SMASHING DETECTED", 30):
            print("[FAIL] stack overflow was not detected (canary missing?)")
            return 1
        print("[OK] stack canary fired")

        # panic_finish() dumps every core; CPU 0 is the BSP that panicked.
        for cpu in range(4):
            marker = f"[PANIC] CPU {cpu} EIP="
            if not wait_for_in_file(SERIAL_LOG, marker, 30):
                print(f"[FAIL] CPU {cpu} never dumped its registers")
                return 1
            print(f"[OK] CPU {cpu} register dump present")

        if not wait_for_in_file(SERIAL_LOG, "[PANIC] halted", 20):
            print("[FAIL] panic tail never halted")
            return 1
        print("[OK] panic tail halted")

        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
