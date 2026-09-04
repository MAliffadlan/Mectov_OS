#!/usr/bin/env python3
"""
scripts/watchdog_test.py — multi-core hard-lockup watchdog self-test.

Boots with `wd_self_test panic=reboot`: once the system is up the kernel
hangs the first AP with a directed fixed IPI (the AP cli-spins forever —
the exact hard-lockup state no timer interrupt can reach), and the BSP's
watchdog must detect the stall ~3 s later and fall into the multi-core
NMI-IPI register dump. Verifies:

  1. the synthetic hang fired             ("[WATCHDOG] SELF TEST")
  2. the watchdog detected the stalled AP ("[WATCHDOG] CPU n HUNG")
  3. EVERY core dumped registers          ("[PANIC] CPU 0".."[PANIC] CPU 3")
     — including the hung AP, whose NMI snapshot shows where it spun
  4. the hung core's EIP is a real kernel .text address (not 0/garbage)
  5. panic=reboot reset the system        ("[PANIC] panic=reboot — resetting")
  6. QEMU exited cleanly                  (guest reset + -no-reboot → exit)

Without a working watchdog this test fails on step 2: the AP hangs forever,
no log line ever appears, and QEMU runs until the workflow timeout.

Usage:
    python3 scripts/watchdog_test.py [--timeout 240]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

SERIAL_LOG = "/tmp/mectov_watchdog_serial.log"


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
    """Build a boot ISO whose GRUB entry passes the watchdog boot args."""
    tmp = tempfile.mkdtemp(prefix="mectov_watchdog_iso_")
    try:
        os.makedirs(os.path.join(tmp, "boot", "grub"), exist_ok=True)
        shutil.copy(bin_path, os.path.join(tmp, "boot", "myos.bin"))
        with open(os.path.join(tmp, "boot", "grub", "grub.cfg"), "w") as f:
            f.write("set timeout=0\n"
                    "set default=0\n"
                    "menuentry \"Mectov OS (watchdog self-test)\" {\n"
                    "    multiboot /boot/myos.bin panic=reboot wd_self_test\n"
                    "    boot\n"
                    "}\n")
        r = subprocess.run(["grub-mkrescue", "-o", iso_path, tmp],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return r.returncode == 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def ensure_images(disk, ext2):
    """The kernel expects both drives; create blank ones if absent so a
    standalone run (outside check.py, which makes them fresh) still boots."""
    for path, mkfs in ((disk, "mkfs.ext2"), (ext2, "mkfs.ext2")):
        if not os.path.exists(path):
            with open(path, "wb") as f:
                f.truncate(32 * 1024 * 1024)
            subprocess.run([mkfs, "-q", path],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


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

    ensure_images(args.disk, args.ext2)

    iso = args.keep_iso or "/tmp/mectov_watchdog.iso"
    if not build_iso(args.bin, iso):
        print("[FAIL] grub-mkrescue failed to build the watchdog-test ISO")
        return 1
    print("[OK] built watchdog-test ISO (panic=reboot wd_self_test)")

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
        "-no-reboot",          # guest reset (panic=reboot) → QEMU exits
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
    ]
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[WATCHDOG] SELF TEST", args.timeout):
            print("[FAIL] synthetic AP hang never fired (kernel hung before it? "
                  "watchdog IPI handler missing?)")
            return 1
        print("[OK] synthetic hang fired (wd_self_test)")

        if not wait_for_in_file(SERIAL_LOG, "[WATCHDOG] CPU 1 HUNG", 60):
            print("[FAIL] watchdog never detected the stalled AP "
                  "(false negative — heartbeat kept advancing?)")
            return 1
        print("[OK] watchdog detected the hung AP")

        # Every core must have dumped a register line (the multi-core NMI-IPI
        # dump). CPU 0 is the BSP self-snapshot; 1 is the hung AP — its NMI
        # must have reached it from inside its cli spin.
        for cpu in range(4):
            marker = f"[PANIC] CPU {cpu} EIP="
            if not wait_for_in_file(SERIAL_LOG, marker, 30):
                print(f"[FAIL] CPU {cpu} never dumped its registers")
                return 1
            print(f"[OK] CPU {cpu} register dump present")
        time.sleep(1)

        # Sanity: the hung AP's EIP is a real kernel address, not 0 or the
        # NMI handler itself (proof the dump caught the spin, not a stub).
        with open(SERIAL_LOG, "r", errors="replace") as f:
            log = f.read()
        m = re.search(r"\[PANIC\] CPU 1 EIP=(0x[0-9A-F]+)", log)
        if not m:
            print("[FAIL] could not parse the hung AP's EIP from the dump")
            return 1
        eip = int(m.group(1), 16)
        if not (0x100000 <= eip < 0x400000):
            print(f"[FAIL] hung AP EIP {m.group(1)} is not a kernel .text "
                  f"address (dump captured the wrong context?)")
            return 1
        print(f"[OK] hung AP EIP {m.group(1)} is a kernel .text address")

        if not wait_for_in_file(SERIAL_LOG, "[PANIC] panic=reboot", 20):
            print("[FAIL] panic=reboot did not reset the system")
            return 1
        print("[OK] panic=reboot reset the system")

        try:
            qemu.wait(timeout=20)
            print(f"[OK] QEMU exited after the reset (code {qemu.returncode})")
        except subprocess.TimeoutExpired:
            print("[FAIL] QEMU still running after panic=reboot + -no-reboot")
            return 1

        # No double/triple fault shadowing the real dump.
        if "[PANIC] DOUBLE FAULT" in log and "[PANIC] CPU 3" not in log:
            print("[FAIL] double fault before the multi-core dump completed")
            return 1
        print("[OK] watchdog hard-lockup detection + multi-core dump verified")
        return 0
    finally:
        qemu.kill()


if __name__ == "__main__":
    sys.exit(main())
