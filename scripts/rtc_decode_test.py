#!/usr/bin/env python3
"""
scripts/rtc_decode_test.py — deterministic RTC decode + straddle gate (v38.165).

The guest gate (scripts/rtc_test.py) proves the LOCK: four threads on four
cores tear the 0x70/0x71 pair 32k-37k times per 48k samples without it. This
one proves the other two thirds of the fix, which a real chip cannot be asked
to demonstrate on demand:

  * Status Register B is honoured — an RTC in BINARY mode (DM bit) or in
    12-hour mode (hour bit 7 = PM) previously decoded to nonsense, because
    `bcd2bin()` was applied unconditionally. QEMU/SeaBIOS is BCD + 24-hour, so
    no guest suite could ever see it;
  * the update cycle cannot land inside a burst — the previous driver waited
    for UIP once, before reading, and nothing checked whether an update began
    while the burst was running.

Method: compile the SHIPPED src/drivers/rtc.c for the host with only its
`#include "../include/..."` lines rewritten to scripts/rtc_host/ (a fake CMOS
chip whose register file a case can swap mid-burst, and a no-op lock), then
drive the driver's public entry point `rtc_read_time()` and assert the decoded
time field by field. scripts/rtc_host/main.c holds the cases.

Both this harness and the guest one hit `rtc_read_time()`, so the same
assertions are RED on the v38.164 driver and GREEN on the fixed one:

    # RED, against the driver as it was:
    git show HEAD:src/drivers/rtc.c > /tmp/rtc_old.c
    python3 scripts/rtc_decode_test.py --driver /tmp/rtc_old.c
    # -> binary/12h/12-AM/straddle FAIL, control PASS

    python3 scripts/rtc_decode_test.py            # GREEN on the fixed driver

Usage:
    python3 scripts/rtc_decode_test.py [--driver src/drivers/rtc.c] [--keep]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

INCLUDE_SRC = 'src/include'
SHIM_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rtc_host')

# 1 control + 1 binary + 1 twelve-hour PM + 1 twelve-hour 12 AM + 4 straddle.
EXPECTED_CHECKS = 8


def stage_driver(driver_path, dest_dir):
    """Copy the driver, rewriting its kernel includes to the host shims.

    Only the include lines change: the decode rules and the port sequence are
    the shipped ones, which is the whole point of testing this file.
    """
    with open(driver_path, 'r') as f:
        src = f.read()

    rewritten, n = re.subn(r'#include\s+"\.\./include/([A-Za-z0-9_]+\.h)"',
                           r'#include "\1"', src)
    if n == 0:
        print(f"[FAIL] {driver_path} has no '../include/...' lines to rewrite — "
              f"is that really the RTC driver?")
        return None

    dest = os.path.join(dest_dir, 'rtc_under_test.c')
    with open(dest, 'w') as f:
        f.write(rewritten)
    return dest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--driver", default=os.path.join("src", "drivers", "rtc.c"),
                    help="driver source to test (default: the shipped one)")
    ap.add_argument("--keep", action="store_true",
                    help="keep the build directory for inspection")
    # Accepted and ignored: scripts/check.py drives every suite with
    # `--timeout <seconds>`. This gate compiles in a second and never boots a
    # VM, so there is nothing to bound — the flag exists so it can be a suite.
    ap.add_argument("--timeout", type=int, default=0, help=argparse.SUPPRESS)
    args = ap.parse_args()

    if not os.path.exists(args.driver):
        print(f"[FAIL] {args.driver} not found")
        return 2
    if not os.path.exists(SHIM_DIR):
        print(f"[FAIL] {SHIM_DIR} not found")
        return 2

    tmpdir = tempfile.mkdtemp(prefix="mectov_rtcdecode_")
    driver = stage_driver(args.driver, tmpdir)
    if driver is None:
        return 1

    binary = os.path.join(tmpdir, "rtc_decode")
    cmd = [
        "gcc", "-O1", "-Wall", "-Wextra", "-g",
        "-I", SHIM_DIR,
        "-I", INCLUDE_SRC,
        os.path.join(SHIM_DIR, "rtc_host.c"),
        os.path.join(SHIM_DIR, "main.c"),
        driver,
        "-o", binary,
    ]
    build = subprocess.run(cmd, capture_output=True, text=True)
    if build.returncode != 0:
        print("[FAIL] the host build of the driver failed:")
        print((build.stdout + build.stderr).strip()[:4000])
        return 1
    warnings = [ln for ln in (build.stdout + build.stderr).splitlines()
                if "warning:" in ln]
    for ln in warnings:
        print("  [cc] " + ln.strip())
    if warnings:
        print(f"[FAIL] the host build produced {len(warnings)} warning(s)")
        return 1

    run = subprocess.run([binary], capture_output=True, text=True)
    out = run.stdout
    for ln in out.splitlines():
        print("  " + ln)

    if "[FAIL]" in out:
        print(f"[FAIL] rtc_decode: at least one case failed against {args.driver}")
        if not args.keep:
            shutil.rmtree(tmpdir, ignore_errors=True)
        return 1

    m = re.search(r"rtc decode checks=(\d+) failures=(\d+)", out)
    if not m:
        print("[FAIL] the harness printed no verdict line")
        return 1
    checks, failures = int(m.group(1)), int(m.group(2))
    if failures != 0:
        print(f"[FAIL] rtc_decode: {failures} failures")
        return 1
    if checks < EXPECTED_CHECKS:
        print(f"[FAIL] rtc_decode: only {checks} checks ran, expected "
              f"{EXPECTED_CHECKS} — a case was lost")
        return 1
    if run.returncode != 0:
        print(f"[FAIL] rtc_decode exited {run.returncode} while reporting "
              f"{failures} failures")
        return 1

    if args.keep:
        print(f"[keep] build dir {tmpdir}")
    else:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print(f"[OK] rtc_decode: driver={args.driver} checks={checks} failures=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
