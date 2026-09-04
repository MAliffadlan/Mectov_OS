#!/usr/bin/env python3
"""
scripts/bigread_test.py — DMA + multi-sector disk read regression test.

Boots mectov.iso once, logs in, launches the Terminal, runs the Ring 3
`bigread` app (`run /apps/bigread.mct`) and verifies from the serial log:

  * the bus-mastering IDE DMA controller was detected and engaged
    ("[ATA] BMIDE DMA ready") and no DMA transfer ever failed
  * /bench.big (160 KB, > PCACHE_MAX_FILE) is read 3 times — every read is
    a real disk read through the batched multi-sector path (DMA when
    available, multi-sector PIO otherwise)
  * the byte pattern (index mod 256) is intact on every read, so a batch
    offset or DMA-address bug cannot pass silently
  * the kernel never panicked during the run

Usage:
    python3 scripts/bigread_test.py [--timeout 300]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

import terminal_launch  # corner-reset + screendump-verified icon double-click

SERIAL_LOG = "/tmp/mectov_bigread_serial.log"
MON_SOCK = "/tmp/mectov_bigread_monitor.sock"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

RUN_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s", "slash",
            "b", "i", "g", "r", "e", "a", "d", "dot", "m", "c", "t", "ret"]


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--kvm", action="store_true",
                    help="run with -enable-kvm (real timing; the CI step stays TCG)")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    qemu_cmd = [
        "qemu-system-i386",
        "-cpu", "qemu32,+nx",
        "-vga", "std",
    ]
    if args.kvm:
        qemu_cmd.append("-enable-kvm")
    qemu_cmd += [
        "-cdrom", args.iso,
        "-m", "128",
        "-smp", "4",
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        "-net", "none",
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached login screen")
            return 1
        print("[OK] booted to login screen")

        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)

        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] logged in, desktop running")

        # The first boot with an old disk seeds /bench.big; wait for it.
        wait_for_in_file(SERIAL_LOG, "seeded /bench.big", 60)

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(
                mon_cmd, SERIAL_LOG, "/tmp/mectov_bigread_cursor.ppm"):
            print("[FAIL] terminal did not launch (icon double-click missed?)")
            return 1
        print("[OK] terminal launched")
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] terminal never became ready")
            return 1
        time.sleep(1.0)

        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)

        for _ in range(3):
            for _ in range(28):
                mon_cmd("sendkey backspace")
            for k in RUN_KEYS:
                mon_cmd("sendkey " + k)
                time.sleep(0.12)
            mon_cmd("sendkey ret")
            if wait_for_in_file(SERIAL_LOG, "[BIGREAD] ALL PASS", 120):
                break
            time.sleep(1.0)

        with open(SERIAL_LOG, "r", errors="replace") as f:
            log_text = f.read()
        if "[BIGREAD] FAIL" in log_text:
            print("[FAIL] bigread reported a failed assertion")
            return 1
        if "[BIGREAD] ALL PASS" not in log_text:
            print("[FAIL] bigread never reached [BIGREAD] ALL PASS")
            return 1
        print("[OK] bigread completed ([BIGREAD] ALL PASS)")

        # v38.62 block-cache verdict: read0 is the cold disk read; read1/read2
        # must come back several times faster (served from the sector cache).
        import re
        reads = {}
        for m in re.finditer(r"\[BIGREAD\] read(\d+)_us=(-?\d+)", log_text):
            reads[int(m.group(1))] = int(m.group(2))
        if 0 not in reads or len(reads) < 3:
            print("[FAIL] could not parse bigread timing lines")
            return 1
        r0 = reads[0]
        if r0 == 0:
            print("[FAIL] read0_us is 0 (timing broken)")
            return 1
        hot_min = min(v for k, v in reads.items() if k > 0 and v >= 0)
        if hot_min * 4 >= r0:
            print(f"[FAIL] block cache ineffective: cold={r0}us hot={hot_min}us "
                  f"(expected >=4x faster)")
            return 1
        print(f"[OK] block cache serving hot reads "
              f"(cold={r0}us best_hot={hot_min}us)")

        if "[PANIC]" in log_text:
            print("[FAIL] kernel panicked during the big-read run")
            return 1
        print("[OK] no kernel panic in the whole big-read run")

        if "[ATA] BMIDE DMA ready" not in log_text:
            print("[FAIL] bus-mastering IDE DMA was not engaged")
            return 1
        print("[OK] IDE bus-mastering DMA engaged")
        if "[ATA] DMA read FAIL" in log_text or "[ATA] DMA xfer fail" in log_text:
            print("[FAIL] a DMA transfer failed and fell back to PIO")
            return 1
        print("[OK] no DMA transfer failed (all reads used the DMA path)")

        time.sleep(5)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after the big-read benchmark")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
