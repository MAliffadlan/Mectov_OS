#!/usr/bin/env python3
"""
scripts/aslr_test.py — ASLR regression: PIE ELF load base is randomized.

Boots the ISO TWICE in a row (two consecutive boots, fresh entropy each
time), logs in, launches the Terminal and runs the ET_DYN PIE app
`syncdemo.elf` (semaphore + futex workers — globals shared across threads,
so a wrong relocation or a non-PC-relative reference would crash it) twice
per boot. Verifies from the serial log:

  * every run prints "[ASLR] PIE base=0x........" — the loader randomized it
  * syncdemo runs to "[SYNC] syncdemo done" with no [CRASH]/[PANIC]/fault —
    the app works at a non-default base
  * the first-run base of boot 1 differs from boot 2 (cross-boot entropy;
    the CSPRNG is re-seeded every boot) AND the two runs within one boot
    differ (per-exec randomization, not a boot-global constant)

Usage:
    python3 scripts/aslr_test.py [--timeout 300]
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import time

import terminal_launch  # corner-reset + screendump-verified icon double-click

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

RUN_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s", "slash",
            "s", "y", "n", "c", "d", "e", "m", "o", "dot", "e", "l", "f", "ret"]

BASE_RE = re.compile(r"\[ASLR\] PIE base=0x([0-9A-F]{8})")


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


def parse_bases(path):
    try:
        with open(path, "r", errors="replace") as f:
            return [int(m, 16) for m in BASE_RE.findall(f.read())]
    except (FileNotFoundError, OSError):
        return []


def boot_once(tag, args, serial_log, mon_sock):
    """One full boot: login, launch terminal, run syncdemo.elf twice.
    Returns (bases, ok, reason)."""
    for p in (serial_log, mon_sock):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    qemu_cmd = [
        "qemu-system-i386", "-cpu", "qemu32,+nx", "-vga", "std",
        "-cdrom", args.iso, "-m", "128", "-smp", "4",
        "-display", "none", "-serial", f"file:{serial_log}",
        "-net", "none",
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-monitor", f"unix:{mon_sock},server,nowait",
    ]
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)

    def mon(cmd):
        try:
            s = socket.socket(socket.AF_UNIX)
            s.connect(mon_sock)
            s.sendall((cmd + "\n").encode())
            time.sleep(0.15)
            s.close()
        except OSError:
            pass

    try:
        if not wait_for_in_file(serial_log, "[K] login", args.timeout):
            return [], False, "kernel never reached login"
        for k in LOGIN_KEYS:
            mon("sendkey " + k)
            time.sleep(0.15)
        if not wait_for_in_file(serial_log, "BOOTED KERNEL LOOP", 90):
            return [], False, "login did not complete"
        time.sleep(1.5)
        if not terminal_launch.launch_terminal(
                mon, serial_log, f"/tmp/mectov_aslr_{tag}_cursor.ppm"):
            return [], False, "terminal did not launch"
        if not wait_for_in_file(serial_log, "ipc_create key=0x0000DEAD", 30):
            return [], False, "terminal never became ready"
        time.sleep(1.0)

        mon("mouse_move 300 176")
        time.sleep(0.1)
        mon("mouse_button 1"); time.sleep(0.1); mon("mouse_button 0")
        time.sleep(0.5)

        done = 0
        for _ in range(2):
            for _ in range(28):
                mon("sendkey backspace")
            for k in RUN_KEYS:
                mon("sendkey " + k)
                time.sleep(0.12)
            mon("sendkey ret")
            if wait_for_in_file(serial_log, "[SYNC] syncdemo done", 90):
                done += 1
                time.sleep(2.0)  # let the task fully exit + prompt return
            else:
                break

        bases = parse_bases(serial_log)
        time.sleep(3)
        if qemu.poll() is not None:
            return bases, False, f"QEMU exited early ({qemu.returncode})"
        if done < 2:
            return bases, False, f"syncdemo.elf completed {done}/2 runs"
        with open(serial_log, "r", errors="replace") as f:
            log = f.read()
        for bad in ("[PANIC]", "[CRASH]", "[EXCEPTION]", "SIGSEGV"):
            if bad in log:
                return bases, False, f"log contains {bad}"
        if len(bases) < 2:
            return bases, False, f"expected >=2 [ASLR] lines, saw {len(bases)}"
        return bases, True, "syncdemo ran twice at randomized bases"
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    args = ap.parse_args()

    b1, ok1, why1 = boot_once("boot1", args, "/tmp/mectov_aslr_1.log",
                              "/tmp/mectov_aslr_1.sock")
    print(f"[boot1] {'OK' if ok1 else 'FAIL'}: {why1} "
          f"bases={[hex(b) for b in b1]}")
    if not ok1:
        return 1

    b2, ok2, why2 = boot_once("boot2", args, "/tmp/mectov_aslr_2.log",
                              "/tmp/mectov_aslr_2.sock")
    print(f"[boot2] {'OK' if ok2 else 'FAIL'}: {why2} "
          f"bases={[hex(b) for b in b2]}")
    if not ok2:
        return 1

    if b1[0] == b2[0]:
        print(f"[FAIL] identical first base across two boots: 0x{b1[0]:08X} "
              f"(ASLR not randomized per boot?)")
        return 1
    print(f"[OK] boot1 base 0x{b1[0]:08X} != boot2 base 0x{b2[0]:08X} "
          f"(cross-boot randomization)")

    if len(b1) >= 2 and b1[0] == b1[1]:
        print(f"[FAIL] both in-boot runs landed at 0x{b1[0]:08X} "
              f"(ASLR constant within a boot?)")
        return 1
    if len(b1) >= 2:
        print(f"[OK] in-boot runs differ: 0x{b1[0]:08X} vs 0x{b1[1]:08X} "
              f"(per-exec randomization)")
    else:
        print("[!] only one [ASLR] line in boot1 — cross-boot check still valid")

    print("[OK] ASLR verified: PIE ELF apps run at randomized bases "
          "(syncdemo threads + globals correct)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
