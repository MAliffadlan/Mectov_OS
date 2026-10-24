#!/usr/bin/env python3
"""
scripts/spk_deadline_test.py — deterministic PC-speaker tone-deadline gate.

The driver has a non-blocking tone (speaker_tone) whose end is decided by the
timer tick (speaker_tick), because every SYS_PLAY_SOUND caller is a game loop
that fires an effect and carries on. Four things that can go wrong there are
invisible to the guest ABI test:

  * the tone never reaches the PIT at all (Ring 3 cannot read port 0x61);
  * the note ends early — a deadline that wrapped past 2^32 looks like it is
    already in the past, so a bare `get_ticks() >= tone_off_ms` silences it;
  * the note never ends — the same comparison with the counter wrapping
    instead of the deadline leaves it sounding for ~49 days;
  * a timer IRQ arrives after a replacement pitch is programmed but before
    its new deadline is published. The old deadline must not silence the new
    pitch. The host clock shim reproduces this interleaving deterministically.

The two wrap cases fail in opposite directions, so both are covered here.
Waiting for either on a real machine means waiting for the millisecond counter
to wrap; moving the clock by hand is the only way to ask the question at all.
The replacement case models a single-CPU timer IRQ, not an SMP stress test.

Method: compile the SHIPPED src/drivers/speaker.c for the host with only its
`#include "../include/..."` lines rewritten to scripts/spk_host/ (a fake PIT
and speaker gate, plus the definition of the clock), then drive the driver's
own entry points and assert what reached the ports.
scripts/spk_host/main.c holds the cases.

RED evidence, on demand:

    # the comparison as it was before the fix:
    sed 's/(int32_t)(get_ticks() - tone_off_ms) >= 0/get_ticks() >= tone_off_ms/' \\
        src/drivers/speaker.c > /tmp/spk_naive.c
    python3 scripts/spk_deadline_test.py --driver /tmp/spk_naive.c --no-count
    # -> wrap A FAIL (silenced one tick in) and wrap B FAIL (never silenced)

    python3 scripts/spk_deadline_test.py          # GREEN on the shipped driver

    # the lock-free replacement transaction (host-only A/B):
    #   the same driver without tone_lock/irqsave, otherwise unchanged
    # -> replacement gate FAIL: an IRQ at get_ticks() can retire the new pitch

Usage:
    python3 scripts/spk_deadline_test.py [--driver src/drivers/speaker.c] [--keep]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

INCLUDE_SRC = 'src/include'
SHIM_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'spk_host')

# 2 reset + 2 pitch + 3 deadline + 3 second pitch + 2 zero-length + 1 negative
# + 1 no-pitch + 5 wrap A + 2 wrap B + 5 interrupt-during-replacement
# + 2 legacy cancellation + 1 port discipline.
EXPECTED_CHECKS = 29


def stage_driver(driver_path, dest_dir):
    """Copy the driver, rewriting its kernel includes to the host shims.

    Only the include lines change: the deadline arithmetic and the port
    sequence are the shipped ones, which is the whole point of testing this
    file. speaker.h is deliberately NOT shimmed, so it resolves to the real
    src/include/speaker.h — a signature that drifts from the kernel's is a
    build failure here, not a silent divergence.
    """
    with open(driver_path, 'r') as f:
        src = f.read()

    rewritten, n = re.subn(r'#include\s+"\.\./include/([A-Za-z0-9_]+\.h)"',
                           r'#include "\1"', src)
    if n == 0:
        print(f"[FAIL] {driver_path} has no '../include/...' lines to rewrite — "
              f"is that really the speaker driver?")
        return None

    dest = os.path.join(dest_dir, 'speaker_under_test.c')
    with open(dest, 'w') as f:
        f.write(rewritten)
    return dest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--driver", default=os.path.join("src", "drivers", "speaker.c"),
                    help="driver source to test (default: the shipped one)")
    ap.add_argument("--keep", action="store_true",
                    help="keep the build directory for inspection")
    ap.add_argument("--no-count", action="store_true",
                    help="skip the expected-check-count guard for A/B comparison")
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

    tmpdir = tempfile.mkdtemp(prefix="mectov_spkdeadline_")
    driver = stage_driver(args.driver, tmpdir)
    if driver is None:
        return 1

    binary = os.path.join(tmpdir, "spk_deadline")
    cmd = [
        "gcc", "-O1", "-Wall", "-Wextra", "-g",
        "-I", SHIM_DIR,
        "-I", INCLUDE_SRC,
        os.path.join(SHIM_DIR, "spk_host.c"),
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
        print(f"[FAIL] spk_deadline: at least one case failed against {args.driver}")
        if not args.keep:
            shutil.rmtree(tmpdir, ignore_errors=True)
        return 1

    m = re.search(r"spk deadline checks=(\d+) failures=(\d+)", out)
    if not m:
        print("[FAIL] the harness printed no verdict line")
        return 1
    checks, failures = int(m.group(1)), int(m.group(2))
    if failures != 0:
        print(f"[FAIL] spk_deadline: {failures} failures")
        return 1
    if not args.no_count and checks < EXPECTED_CHECKS:
        print(f"[FAIL] spk_deadline: only {checks} checks ran, expected "
              f"{EXPECTED_CHECKS} — a case was lost")
        return 1
    if run.returncode != 0:
        print(f"[FAIL] spk_deadline exited {run.returncode} while reporting "
              f"{failures} failures")
        return 1

    if args.keep:
        print(f"[keep] build dir {tmpdir}")
    else:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print(f"[OK] spk_deadline: driver={args.driver} checks={checks} failures=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
