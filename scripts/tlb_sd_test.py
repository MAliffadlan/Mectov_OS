#!/usr/bin/env python3
"""
scripts/tlb_sd_test.py — TLB shootdown IPI machinery self-test (v38.66).

Boots with `tlb_self_test panic=reboot`: once the system is up (all APs
running), the BSP parks the first AP inside the TLB_TEST_VECTOR handler
(IF=1, its idle page directory active) and fires two shootdowns at it:

  1. MATCH     vmm_tlb_shootdown(boot_cr3)   — the AP IS running that space,
               so its handler must ack AND reload CR3.
  2. NO-MATCH  vmm_tlb_shootdown(0x0BADF000) — no core runs it, so the
               handler must ack but SKIP the reload.

The kernel prints "[TLB_SD] SELF TEST PASS - recv=2 reload=1" when both
behaved (recv counts handled shootdown IPIs, reload counts actual CR3
reloads), then panic_finish() resets the machine. This driver asserts:

  1. the self-test fired               ("[TLB_SD] SELF TEST on CPU")
  2. the verdict line exists           ("[TLB_SD] SELF TEST PASS")
  3. panic=reboot reset the system     ("[PANIC] panic=reboot — resetting")
  4. QEMU exited cleanly               (guest reset + -no-reboot → exit)

Counter-based verdicts make the test deterministic — immune to TCG timing
(no data observation races a 1 kHz tick).

Usage:
    python3 scripts/tlb_sd_test.py [--timeout 240]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

SERIAL_LOG = "/tmp/mectov_tlb_sd_serial.log"


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
    """Build a boot ISO whose GRUB entry passes the tlb_self_test boot args."""
    tmp = tempfile.mkdtemp(prefix="mectov_tlbsd_iso_")
    try:
        os.makedirs(os.path.join(tmp, "boot", "grub"), exist_ok=True)
        shutil.copy(bin_path, os.path.join(tmp, "boot", "myos.bin"))
        with open(os.path.join(tmp, "boot", "grub", "grub.cfg"), "w") as f:
            f.write("set timeout=0\n"
                    "set default=0\n"
                    "menuentry \"Mectov OS (TLB shootdown self-test)\" {\n"
                    "    multiboot /boot/myos.bin panic=reboot tlb_self_test\n"
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

    iso = args.keep_iso or "/tmp/mectov_tlb_sd.iso"
    if not build_iso(args.bin, iso):
        print("[FAIL] grub-mkrescue failed to build the tlb_self_test ISO")
        return 1
    print("[OK] built tlb_self_test ISO (panic=reboot tlb_self_test)")

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
        if not wait_for_in_file(SERIAL_LOG, "[TLB_SD] SELF TEST on CPU", args.timeout):
            print("[FAIL] tlb_self_test never fired (kernel hung before the "
                  "self-test point? park IPI missing?)")
            return 1
        print("[OK] tlb_self_test fired (AP parked)")

        if not wait_for_in_file(SERIAL_LOG, "[TLB_SD] SELF TEST PASS", 60):
            # Surface the kernel's own FAIL line (recv/reload counters) so the
            # CI log shows exactly which assertion broke.
            try:
                with open(SERIAL_LOG, "r", errors="replace") as f:
                    log = f.read()
                for line in log.splitlines():
                    if "[TLB_SD]" in line:
                        print(f"[INFO] {line}")
            except (FileNotFoundError, OSError):
                pass
            print("[FAIL] shootdown verdict did not PASS "
                  "(conditional reload wrong? ack protocol broke?)")
            return 1
        print("[OK] shootdown verdict PASS (matching pd reloaded, "
              "non-matching skipped)")

        if not wait_for_in_file(SERIAL_LOG, "[PANIC] panic=reboot", 30):
            print("[FAIL] panic=reboot did not reset the system")
            return 1
        print("[OK] panic=reboot reset the system")

        try:
            qemu.wait(timeout=20)
            print(f"[OK] QEMU exited after the reset (code {qemu.returncode})")
        except subprocess.TimeoutExpired:
            print("[FAIL] QEMU still running after panic=reboot + -no-reboot")
            return 1
        print("[OK] TLB shootdown IPI + conditional flush + ack verified")
        return 0
    finally:
        qemu.kill()


if __name__ == "__main__":
    sys.exit(main())
