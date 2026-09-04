#!/usr/bin/env python3
"""
scripts/watchdog_bsp_test.py — BSP hard-lockup detection by the APs (v38.67).

The v38.64 watchdog hung an AP and had the BSP detect it. That left one
blind spot: if the BSP itself dies with IF=0 (cli spin / lock deadlock in
BSP-only code), its own detector dies with it and nobody fired. v38.67 makes
the watchdog a per-core mesh — EVERY core's timer IRQ runs watchdog_check()
and watches every peer, BSP included — so a BSP hang is caught by the APs.

This test boots with `wd_self_test_bsp panic=reboot`: once the system is up,
the BSP drops into a cli+spin from its own main loop. Its detector is dead.
The AP mesh detectors must declare it HUNG ~3 s later, and an AP (CPU 1..3)
must drive the multi-core NMI-IPI dump + reboot. Verifies:

  1. the synthetic BSP hang fired       ("[WATCHDOG] SELF TEST — hanging CPU 0")
  2. CPU 0 was declared HUNG            ("[WATCHDOG] CPU 0 HUNG")
  3. the DETECTOR was an AP, not the
     BSP                                ("detected by CPU [123]")
  4. every core dumped registers        ("[PANIC] CPU 0..3 EIP=")
  5. the hung BSP's EIP is a real
     kernel .text address               (its NMI snapshot shows the spin)
  6. panic=reboot reset the system      ("[PANIC] panic=reboot — resetting")
  7. QEMU exited cleanly                (guest reset + -no-reboot → exit)

Without the per-core mesh this test fails at step 2/3: the BSP hangs forever
and no log line ever appears, because only the BSP used to run the detector.

Usage:
    python3 scripts/watchdog_bsp_test.py [--timeout 240]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

SERIAL_LOG = "/tmp/mectov_watchdog_bsp_serial.log"


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
    """Build a boot ISO whose GRUB entry passes the wd_self_test_bsp args."""
    tmp = tempfile.mkdtemp(prefix="mectov_wd_bsp_iso_")
    try:
        os.makedirs(os.path.join(tmp, "boot", "grub"), exist_ok=True)
        shutil.copy(bin_path, os.path.join(tmp, "boot", "myos.bin"))
        with open(os.path.join(tmp, "boot", "grub", "grub.cfg"), "w") as f:
            f.write("set timeout=0\n"
                    "set default=0\n"
                    "menuentry \"Mectov OS (BSP watchdog self-test)\" {\n"
                    "    multiboot /boot/myos.bin panic=reboot wd_self_test_bsp\n"
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

    iso = args.keep_iso or "/tmp/mectov_wd_bsp.iso"
    if not build_iso(args.bin, iso):
        print("[FAIL] grub-mkrescue failed to build the BSP-test ISO")
        return 1
    print("[OK] built wd_self_test_bsp ISO (panic=reboot)")

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
        if not wait_for_in_file(SERIAL_LOG,
                                "[WATCHDOG] SELF TEST — hanging CPU 0 (BSP)",
                                args.timeout):
            print("[FAIL] the synthetic BSP hang never fired (kernel hung "
                  "before the self-test point?)")
            return 1
        print("[OK] synthetic BSP hang fired (wd_self_test_bsp)")

        if not wait_for_in_file(SERIAL_LOG, "[WATCHDOG] CPU 0 HUNG", 60):
            print("[FAIL] no core declared CPU 0 HUNG — the per-core mesh is "
                  "not detecting a BSP stall (watchdog_check not on APs?)")
            return 1
        print("[OK] CPU 0 declared HUNG by a peer")

        # The whole point of the mesh: an AP must be the detector. The BSP is
        # inside a cli spin and cannot have printed the marker itself.
        with open(SERIAL_LOG, "r", errors="replace") as f:
            log = f.read()
        m = re.search(r"\[WATCHDOG\] CPU 0 HUNG[^\n]*detected by CPU ([0-9]+)",
                      log)
        if not m:
            print("[FAIL] could not parse which core detected the BSP hang")
            return 1
        detector = int(m.group(1))
        if detector == 0:
            print("[FAIL] detector is CPU 0 (the BSP) — but it is inside its "
                  "own cli spin and cannot have fired; marker is bogus")
            return 1
        print(f"[OK] BSP hang detected by CPU {detector} (an AP — mesh works)")

        # Every core must have dumped a register line. CPU 0 is the hung BSP;
        # its NMI snapshot must show where it spun.
        for cpu in range(4):
            marker = f"[PANIC] CPU {cpu} EIP="
            if not wait_for_in_file(SERIAL_LOG, marker, 30):
                print(f"[FAIL] CPU {cpu} never dumped its registers")
                return 1
            print(f"[OK] CPU {cpu} register dump present")

        with open(SERIAL_LOG, "r", errors="replace") as f:
            log = f.read()
        m = re.search(r"\[PANIC\] CPU 0 EIP=(0x[0-9A-F]+)", log)
        if not m:
            print("[FAIL] could not parse the hung BSP's EIP from the dump")
            return 1
        eip = int(m.group(1), 16)
        if not (0x100000 <= eip < 0x400000):
            print(f"[FAIL] hung BSP EIP {m.group(1)} is not a kernel .text "
                  f"address (dump captured the wrong context?)")
            return 1
        print(f"[OK] hung BSP EIP {m.group(1)} is a kernel .text address")

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
        print("[OK] BSP hard-lockup detected + dumped by AP mesh detectors")
        return 0
    finally:
        qemu.kill()


if __name__ == "__main__":
    sys.exit(main())
