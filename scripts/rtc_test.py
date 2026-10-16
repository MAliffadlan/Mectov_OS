#!/usr/bin/env python3
"""
scripts/rtc_test.py — concurrent CMOS RTC read-consistency gate (v38.165).

The bug this gate exists for: a CMOS register read is a TWO-PORT transaction —
write the register index to 0x70, then read the value from 0x71 — and
`cmos_read()` in src/drivers/rtc.c did nothing to keep the pair together. Four
real readers exist: SYS_GET_TIME from any task on any core, the taskbar clock,
the timer's once-per-second rate calibration, and the login lockout. With more
than one CPU, one core's `inb(0x71)` can return the register ANOTHER core just
selected, so `rtc_read_time()` hands back a time assembled from mixed fields —
a "second" that is really the minute register, a day/month pair from different
instants, and so on.

The detector is apps/rtc_stress.mct, which runs four threads on four cores and
calls sys_get_time() back to back. It relies on the RTC updating once per
second: two reads microseconds apart must observe the SAME second, unless the
second boundary fell between them, in which case a third read breaks the tie.
Every stamp is also range-checked (a crossed read decodes to out-of-range
garbage far more often than a healthy one does).

  RED  (the shape this gate was written against): the app prints
       `[RTC_STRESS] FAIL torn=N of M` plus one `example id=.. a=.. b=..` line
       per reader that saw one. That is a real tear, and the examples are the
       post-mortem.
  GREEN: `[RTC_STRESS] PASS checked=N` with N > 0 and no FAIL line anywhere.

This gate asserts the guest's OWN numbers, so a stack that answers two
assertions and then dies cannot pass: it demands the PASS line, a non-zero
checked count, the absence of any FAIL line, and no [PANIC] on the whole run.

Measured sensitivity, stated rather than implied: a tear that swaps two
registers holding the SAME value (e.g. a minute whose BCD pair is unchanged)
leaves the stamp self-consistent and is NOT detected. The gate is a detector
for observable inconsistency, not a proof that the port pair is serialized —
the lock is justified by the mechanism, and the deterministic decode/straddle
gate is scripts/rtc_decode_test.py on the host.

Usage:
    python3 scripts/rtc_test.py [--repeat 3] [--smp 4] [--timeout 240]
    make check CHECK_ARGS=--only=rtc
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

import terminal_launch  # corner-reset + screendump-verified icon double-click

SERIAL_LOG = "/tmp/mectov_rtc_serial.log"
MON_SOCK = "/tmp/mectov_rtc_monitor.sock"

# The lock screen eats the first keypress to dismiss it, so a leading space
# comes before the password.
LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

M_START = "[RTC_STRESS] start readers="
M_PASS = "[RTC_STRESS] PASS checked="
M_FAIL = "[RTC_STRESS] FAIL"


def wait_for_in_file(path, needle, timeout, after=0):
    """True once `needle` appears in `path` at or after byte offset `after`.

    The offset is what makes `--repeat` honest: each round only matches bytes
    appended after that round started, so a previous round's PASS can never
    satisfy a later round (the same rule scripts/cond_test.py uses).
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, "r", errors="replace") as f:
                data = f.read()
            if needle in data[after:]:
                return True
        except (FileNotFoundError, OSError):
            pass
        time.sleep(0.5)
    return False


def read_serial():
    try:
        with open(SERIAL_LOG, "r", errors="replace") as f:
            return f.read()
    except (FileNotFoundError, OSError):
        return ""


def serial_len():
    try:
        return os.path.getsize(SERIAL_LOG)
    except OSError:
        return 0


def mon_cmd(cmd, wait=0.15):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(wait)
        s.close()
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")


def send_keys(keys):
    for k in keys:
        mon_cmd("sendkey " + k)
        time.sleep(0.12)


def type_line(keys):
    # QEMU sendkey wants scancode NAMES: ' ' -> spc, '/' -> slash, '.' -> dot,
    # '_' -> shift-minus (a raw '_' is an invalid name and is silently dropped).
    names = {" ": "spc", "/": "slash", ".": "dot", "_": "shift-minus"}
    for k in keys:
        mon_cmd("sendkey " + names.get(k, k))
        time.sleep(0.12)


def run_app_cmd(name, marker, wait):
    """Type `run /apps/<name>.mct`, clearing the line and retrying until the
    marker appears in the serial log."""
    for _ in range(3):
        for _ in range(40):
            mon_cmd("sendkey backspace")
        type_line(["r", "u", "n", " ", "/", "a", "p", "p", "s", "/"] +
                  list(name) + [".", "m", "c", "t", "ret"])
        if wait_for_in_file(SERIAL_LOG, marker, wait):
            return True
        time.sleep(1.0)
    return False


def dump_failure_evidence():
    """Surface the app's own verdict lines — the actionable post-mortem."""
    try:
        with open(SERIAL_LOG, errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return
    for ln in lines:
        if ("[RTC_STRESS]" in ln or "[FAIL]" in ln or "[PANIC]" in ln
                or "[RTC]" in ln):
            print("  " + ln[:200])


def parse_total(serial, marker):
    """Pull the integer that follows `marker` (e.g. '... PASS checked=48012')."""
    i = serial.rfind(marker)
    if i < 0:
        return None
    j = i + len(marker)
    digits = ""
    while j < len(serial) and serial[j].isdigit():
        digits += serial[j]
        j += 1
    return int(digits) if digits else None


def run_guest(args):
    """Boot, log in, launch the Terminal, then run the detector --repeat times
    in ONE boot (a torn stamp is a race, so more rounds is more samples; each
    round is also cheap because the login is paid once)."""
    for p in (SERIAL_LOG, MON_SOCK):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    tmpdir = tempfile.mkdtemp(prefix="mectov_rtc_")
    tdisk = os.path.join(tmpdir, "disk.img")
    text2 = os.path.join(tmpdir, "ext2.img")
    shutil.copy(args.disk, tdisk)
    shutil.copy(args.ext2, text2)

    qemu_cmd = [
        "qemu-system-i386",
        "-cpu", "qemu32,+nx",
        "-vga", "std",
        "-cdrom", args.iso,
        "-m", "512",
        "-smp", str(args.smp),
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        "-net", "none",
        "-drive", f"file={tdisk},format=raw,index=0,media=disk",
        "-drive", f"file={text2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    qemu = subprocess.Popen(qemu_cmd)
    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached the login screen")
            return 1
        send_keys(LOGIN_KEYS)
        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] booted and logged in")

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(
                mon_cmd, SERIAL_LOG, "/tmp/mectov_rtc_cursor.ppm"):
            print("[FAIL] the Terminal never became ready")
            return 1
        wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30)
        time.sleep(1.0)

        # Focus the terminal window before typing.
        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)

        checked_total = 0
        for rnd in range(1, args.repeat + 1):
            start = serial_len()
            if not run_app_cmd("rtc_stress", M_START, 60):
                print(f"[FAIL] round {rnd}: the detector never started")
                dump_failure_evidence()
                return 1

            # Wait for THIS round's verdict only.
            deadline = time.time() + args.timeout
            serial = read_serial()[start:]
            while time.time() < deadline:
                serial = read_serial()[start:]
                if M_PASS in serial or M_FAIL in serial:
                    break
                time.sleep(1)

            if M_FAIL in serial:
                torn = parse_total(serial, "[RTC_STRESS] FAIL torn=")
                print(f"[FAIL] round {rnd}: the driver handed back a time "
                      f"assembled from mixed CMOS registers (torn={torn})")
                dump_failure_evidence()
                return 1
            if M_PASS not in serial:
                print(f"[FAIL] round {rnd}: no verdict line — the detector "
                      f"stalled or the app died")
                dump_failure_evidence()
                return 1

            checked = parse_total(serial, M_PASS)
            if not checked:
                print(f"[FAIL] round {rnd}: PASS with no samples "
                      f"(checked={checked}) — the assertions did not run")
                dump_failure_evidence()
                return 1
            # The app counts a declined call and an untouched buffer separately
            # from a tear, because they are different bugs. A PASS line that
            # does not carry zeroes for both is not a pass.
            if "refused=0 unwritten=0" not in serial:
                print(f"[FAIL] round {rnd}: the detector saw refused/unwritten "
                      f"samples — the kernel declined a call or wrote nothing")
                dump_failure_evidence()
                return 1
            checked_total += checked
            print(f"[OK] round {rnd}: checked={checked} torn=0")

        full = read_serial()
        if "[PANIC]" in full:
            print("[FAIL] a panic appeared during the run")
            dump_failure_evidence()
            return 1

        print(f"[OK] rtc: smp={args.smp} checked={checked_total} torn=0")
        return 0
    finally:
        err = b""
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
        if qemu.stderr is not None:
            try:
                err = qemu.stderr.read() or b""
            except (OSError, ValueError):
                err = b""
        if err.strip():
            print("[qemu stderr] " + err.decode("utf-8", "replace").strip()[:400])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repeat", type=int, default=3,
                    help="detector runs inside one boot (default 3)")
    ap.add_argument("--smp", type=int, default=4,
                    help="guest CPUs (default 4; the tear needs >1)")
    ap.add_argument("--timeout", type=int, default=240, help="seconds per round")
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    args = ap.parse_args()

    if not os.path.exists(args.iso):
        print(f"[FAIL] {args.iso} not found — run `make iso` first")
        return 2

    return run_guest(args)


if __name__ == "__main__":
    sys.exit(main())
