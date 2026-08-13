#!/usr/bin/env python3
"""
scripts/panic_test.py — multi-core panic dump + `panic=reboot` self-test.

Builds a boot ISO whose GRUB entry passes `panic=reboot panic_self_test` to
the kernel, boots it in QEMU with `-no-reboot`, and verifies the deliberate
kernel panic (fired by the BSP once the desktop loop is running, so all 4
APs are up):

  1. the panic fired                ("[PANIC] SELF TEST")
  2. EVERY core dumped registers    ("[PANIC] CPU 0".."[PANIC] CPU 3")
  3. panic=reboot reset the system  ("[PANIC] panic=reboot — resetting")
  4. QEMU exited cleanly            (guest reset + -no-reboot → exit)

Without a working multi-core dump this test fails on step 2 — a deadlock or
a 4-core race panic now leaves a full register line per core in the log
instead of a single EIP from the faulting core.

Usage:
    python3 scripts/panic_test.py [--timeout 240]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

SERIAL_LOG = "/tmp/mectov_panic_serial.log"


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
    """Build a boot ISO whose GRUB entry passes the panic boot args."""
    tmp = tempfile.mkdtemp(prefix="mectov_panic_iso_")
    try:
        os.makedirs(os.path.join(tmp, "boot", "grub"), exist_ok=True)
        shutil.copy(bin_path, os.path.join(tmp, "boot", "myos.bin"))
        with open(os.path.join(tmp, "boot", "grub", "grub.cfg"), "w") as f:
            f.write("set timeout=0\n"
                    "set default=0\n"
                    "menuentry \"Mectov OS (panic self-test)\" {\n"
                    "    multiboot /boot/myos.bin panic=reboot panic_self_test\n"
                    "    boot\n"
                    "}\n")
        r = subprocess.run(["grub-mkrescue", "-o", iso_path, tmp],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return r.returncode == 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=240)
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

    iso = args.keep_iso or "/tmp/mectov_panic.iso"
    if not build_iso(args.bin, iso):
        print("[FAIL] grub-mkrescue failed to build the panic-test ISO")
        return 1
    print("[OK] built panic-test ISO (panic=reboot panic_self_test)")

    qemu_cmd = [
        "qemu-system-i386",
        "-vga", "std",
        "-cdrom", iso,
        "-m", "128",
        "-smp", "4",
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        "-net", "none",
        "-no-reboot",          # guest reset (panic=reboot) → QEMU exits
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
    ]
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[PANIC] SELF TEST", args.timeout):
            print("[FAIL] deliberate panic never fired (kernel hung before the main loop?)")
            return 1
        print("[OK] panic self-test fired")

        # Every core must have dumped a register line. The panic fires on the
        # BSP; CPU 0 is the BSP (self snapshot), 1-3 answer the NMI-IPI.
        for cpu in range(4):
            marker = f"[PANIC] CPU {cpu} EIP="
            if not wait_for_in_file(SERIAL_LOG, marker, 30):
                print(f"[FAIL] CPU {cpu} never dumped its registers")
                return 1
            print(f"[OK] CPU {cpu} register dump present")
        time.sleep(1)

        if not wait_for_in_file(SERIAL_LOG, "[PANIC] panic=reboot", 20):
            print("[FAIL] panic=reboot did not reset the system")
            return 1
        print("[OK] panic=reboot reset the system")

        # With -no-reboot the guest reset exits QEMU. It should NOT still be
        # running after the panic fired.
        try:
            qemu.wait(timeout=20)
            print(f"[OK] QEMU exited after the reset (code {qemu.returncode})")
        except subprocess.TimeoutExpired:
            print("[FAIL] QEMU still running after panic=reboot + -no-reboot")
            return 1

        # Sanity: no double fault / triple fault shadowing the real dump.
        with open(SERIAL_LOG, "r", errors="replace") as f:
            log = f.read()
        if "[PANIC] DOUBLE FAULT" in log and "[PANIC] CPU 3" not in log:
            print("[FAIL] double fault before the multi-core dump completed")
            return 1
        print("[OK] multi-core panic dump + reboot path verified")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
