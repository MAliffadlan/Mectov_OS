#!/usr/bin/env python3
"""
scripts/cpuid_test.py — the feature-probe path (v38.164).

This is deliberately NOT a "CR4.SMEP is set" test. That assertion could never
pass here: no QEMU i386 model advertises CPUID.(7,0):EBX[7] (SMEP) or [14]
(SMAP) — measured across qemu32 / qemu64 / Nehalem / core2duo — and SMAP
additionally needs 4-level paging, which this PAE 3-level kernel does not have.
A test asserting SMEP is on would be a test that can only ever assert false.

What IS testable is that the probe reads the leaves it claims to read. Before
v38.164, paging_enable_smep() issued `cpuid` with EAX=7 and never asked CPUID.0
what the maximum leaf was. Intel SDM Vol 2A makes an out-of-range leaf's
EBX/ECX/EDX undefined, and since CPUID is usually a comparison chain, an
out-of-range leaf falls through to the next branch and returns a real-looking
register. Measured on this exact default CPU: CPUID.0:EAX = 4, yet a bare
`cpuid` with EAX=7 returns EBX = 0x3F. The old code then tested bit 7 of that
garbage. paging_enable_nxe() had the same shape against extended leaf
0x80000001.

The gates here:

  1. the paging summary reports NX active — the one feature bit that IS
     reachable under QEMU i386, so the probe path is genuinely exercised
  2. SMEP is reported as a DEFINITE state, never 'unprobed'. 'unprobed' would
     mean paging_enable_smep() never ran, which is the silent-skip failure
     this suite exists to catch.
  3. the state is one of the three defined ones (not an unrecognised string)
  4. the state agrees with the CPU: max leaf < 7 must report 'cpuid-leaf7-
     absent'. A 'cpu-absent' there would mean the kernel claims to have read a
     leaf that does not exist — the exact bug, still present.

Usage:
    python3 scripts/cpuid_test.py [--timeout 240]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

SERIAL_LOG = "/tmp/mectov_cpuid_serial.log"
MON_SOCK = "/tmp/mectov_cpuid_monitor.sock"

# The three states paging_enable_smep() can land in besides being enabled.
SMEP_STATES = ("enabled", "cpu-absent", "cpuid-leaf7-absent")

# Max basic leaf per CPU model, measured with a standalone CPUID probe (the
# kernel does not print it). Used only to cross-check the reported SMEP state.
MAX_BASIC_LEAF = {
    "qemu32": 0x04,
    "pentium3": 0x03,
    "core2duo": 0x0A,
    "Nehalem": 0x0B,
    "qemu64": 0x0D,
}


def wait_for_in_file(path, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, "r", errors="replace") as f:
                if needle in f.read():
                    return True
        except (FileNotFoundError, OSError):
            pass
        time.sleep(0.5)
    return False


def read_all(path):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read()
    except OSError:
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


def paging_line():
    """The single '[MEM] PAE paging on (...)' line, or None."""
    for line in read_all(SERIAL_LOG).splitlines():
        if "[MEM] PAE paging on" in line:
            return line.strip()
    return None


def check(log, max_leaf):
    """Pure predicate over the boot log. Returns a list of failure strings."""
    failures = []

    line = paging_line()
    if not line:
        return ["no '[MEM] PAE paging on' line in the serial log"]
    print(f"  paging: {line}")

    # 1. NX must be active — the reachable feature bit.
    if "NX enabled" not in line:
        failures.append(
            "NX is not active, so the CPUID/MSR feature-probe path is not "
            "being exercised at all"
        )

    # 2/3. SMEP must be reported as a definite, known state.
    smep = None
    if "SMEP " not in line:
        failures.append("the paging summary reports no SMEP state at all")
    else:
        smep = line.split("SMEP ", 1)[1].rstrip(")").strip()
        if smep == "unprobed":
            failures.append(
                "SMEP state is 'unprobed': paging_enable_smep() never ran, "
                "so the feature probe is skipped silently"
            )
        elif smep not in SMEP_STATES:
            failures.append(
                f"SMEP state is unrecognised: {smep!r} "
                f"(expected one of {SMEP_STATES})"
            )

    # 4. The reported state must agree with what the CPU actually exposes.
    if smep is not None and max_leaf is not None:
        if max_leaf < 7:
            if smep == "cpu-absent":
                failures.append(
                    f"CPUID max leaf is {max_leaf} (< 7) yet SMEP was reported "
                    "as 'cpu-absent' — that state claims to have read a leaf "
                    "that does not exist; the max-leaf check is missing"
                )
            else:
                print(f"  max leaf {max_leaf} < 7, SMEP state '{smep}' is consistent")
        elif smep == "cpuid-leaf7-absent":
            failures.append(
                f"CPUID max leaf is {max_leaf} (>= 7) yet SMEP was reported as "
                "'cpuid-leaf7-absent' — the max-leaf check is inverted"
            )
        else:
            print(f"  max leaf {max_leaf} >= 7, SMEP state '{smep}' is plausible")

    log_all = read_all(log)
    if "[PANIC]" in log_all:
        failures.append("guest panicked")

    return failures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--cpu", default="qemu32,+nx")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    # The CPU model decides what the max leaf is; ask for the model's real name.
    model = args.cpu.split(",")[0]
    max_leaf = MAX_BASIC_LEAF.get(model)

    qemu_cmd = [
        "qemu-system-i386",
        "-cpu", args.cpu,
        "-vga", "std",
        "-cdrom", args.iso,
        "-m", "128",
        "-smp", "1",
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        "-net", "none",
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
        "-no-reboot",
    ]
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)

    try:
        # The paging summary is printed during paging_init, well before the
        # login gate, so it is already in the log by the time anything is typed.
        if not wait_for_in_file(SERIAL_LOG, "[MEM] PAE paging on", args.timeout):
            print("[FAIL] kernel never printed its paging summary")
            return 1
        print("[OK] kernel reached the paging summary")

        failures = check(SERIAL_LOG, max_leaf)
        if failures:
            print("\n[FAIL] cpuid_test")
            for f in failures:
                print(f"  - {f}")
            return 1

        print("[OK] NX active; SMEP state definite and consistent with the CPU")
        return 0
    finally:
        try:
            qemu.kill()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())
