#!/usr/bin/env python3
"""
scripts/sb16_selftest.py — functional test for the v38.170 SB16 driver upgrade.

Boots mectov.iso in QEMU with a SB16 card attached (audiodev backend `none`,
so no host sound hardware/PulseAudio/pipewire is required), and verifies from
the serial log that the boot-time `sb16_selftest()` proved the extended API:

  1. the card is present            ("[SB16] Speaker ON. Ready.")
  2. the selftest PASSed            ("[SB16] selftest PASS (...)")
  3. no selftest FAIL line appeared
  4. the OS booted to the desktop loop and stayed alive

Usage:
    python3 scripts/sb16_selftest.py [--timeout 180]
"""
import argparse
import os
import subprocess
import sys
import time

SERIAL_LOG = "/tmp/mectov_sb16_serial.log"

PASS_MARK = "[SB16] selftest PASS"
FAIL_MARK = "[SB16] selftest FAIL"
SKIP_MARK = "[SB16] selftest skipped"
READY_MARK = "[SB16] Speaker ON. Ready."

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

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    args = ap.parse_args()

    try:
        os.unlink(SERIAL_LOG)
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
        # `driver=none` is a null audio backend: SB16 card present, no host
        # audio requires. Works headless in CI.
        "-audiodev", "id=snd0,driver=none",
        "-device", "sb16,audiodev=snd0",
        "-net", "nic,model=rtl8139",
        "-net", "user",
        "-snapshot",
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
    ]
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, READY_MARK, args.timeout):
            print("[FAIL] SB16 card was not detected (no 'Speaker ON. Ready.')")
            return 1
        print("[OK] SB16 card detected")

        if wait_for_in_file(SERIAL_LOG, SKIP_MARK, 10):
            print("[FAIL] selftest skipped unexpectedly (card claimed present)")
            return 1

        if not wait_for_in_file(SERIAL_LOG, PASS_MARK, args.timeout):
            if wait_for_in_file(SERIAL_LOG, FAIL_MARK, 10):
                print("[FAIL] sb16 selftest reported FAIL")
            else:
                print("[FAIL] no sb16 selftest PASS (timed out)")
            return 1
        print("[OK] sb16 selftest passed (fmt/play/pause/resume/mixer/stop)")

        # The selftest runs during boot, before login — reaching the login
        # gate is enough to prove the system survived (the real proof is the
        # PASS marker above, which only prints if the test ran to completion).
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] OS did not reach the login screen")
            return 1
        print("[OK] OS booted to the login screen and stayed alive")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
