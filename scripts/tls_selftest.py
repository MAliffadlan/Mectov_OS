#!/usr/bin/env python3
# tls_selftest.py — CI gate for the v38.162 TLS 1.3 engine.
#
# Boots the ISO, logs in, opens the Terminal and runs
# `run /apps/tlsselftest.mct`, then reads the serial log. The app needs no
# network: every input is a committed fixture, so this gate answers the
# question that matters for a TLS stack -- "does it refuse the wrong thing?"
# -- rather than the easier one, "can it connect?".
#
# The app prints one [PASS] or [FAIL] line per assertion and a final tally
# ("[INFO] checks N, failures 0"). This gate checks three things:
#
#   1. the tally exists and reports zero failures, and the count is at least
#      TLS_MIN_CHECKS -- a stack that answers two assertions and dies would
#      otherwise look identical to a green run from the outside;
#   2. no [FAIL] line appears anywhere in the log, so a failure reported
#      before the tally cannot be hidden by it;
#   3. the kernel did not panic.
#
# It also fails on the old markers if the app is missing from the image
# entirely, which is the failure mode a stale VFS entry produces.
import argparse
import os
import re
import socket
import subprocess
import sys
import time

SERIAL_LOG = "/tmp/mectov_tls_serial.log"
MON_SOCK = "/tmp/mectov_tls_monitor.sock"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

# Floor, not an exact match: adding checks must not break the gate, but
# silently losing most of them must. Keep in step with apps/tlsselftest.c.
TLS_MIN_CHECKS = 53

DONE_MARK = "[INFO] TLS selftest OK"
FAIL_MARK = "[INFO] TLS selftest FAILED"
TALLY_RE = re.compile(r"\[INFO\] checks (\d+), failures (\d+)")


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


def read_log():
    try:
        with open(SERIAL_LOG, "r", errors="replace") as f:
            return f.read()
    except (FileNotFoundError, OSError):
        return ""


def mon_cmd(cmd, wait=0.15):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(wait)
        s.close()
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")


def type_line(keys):
    names = {" ": "spc", "/": "slash", ".": "dot", "_": "shift-minus"}
    for k in keys:
        mon_cmd("sendkey " + names.get(k, k))
        time.sleep(0.12)


def send_keys(keys):
    for k in keys:
        mon_cmd("sendkey " + k)
        time.sleep(0.12)


def run_app_cmd(name, marker, wait):
    """Type `run /apps/<name>.mct`, clearing the line and retrying until the
    marker appears. The terminal may still be settling when the first
    keystrokes land, so a single attempt is not reliable."""
    for _ in range(3):
        for _ in range(40):
            mon_cmd("sendkey backspace")
        type_line(["r", "u", "n", " ", "/", "a", "p", "p", "s", "/"] +
                  list(name) + [".", "m", "c", "t", "ret"])
        if wait_for_in_file(SERIAL_LOG, marker, wait):
            return True
        time.sleep(1.0)
    return False


def check_results():
    """Return (ok, message). All the log reading is done in one place so the
    three conditions above are evaluated against the same snapshot."""
    log = read_log()

    fails = [ln for ln in log.splitlines() if "[FAIL]" in ln]
    if fails:
        for ln in fails[:10]:
            print("  " + ln.strip())
        if len(fails) > 10:
            print(f"  ... and {len(fails) - 10} more")
        return False, f"{len(fails)} assertion(s) failed inside the guest"

    m = TALLY_RE.search(log)
    if not m:
        return False, "the guest never printed a check tally (did the app run?)"

    checks, failures = int(m.group(1)), int(m.group(2))
    if failures != 0:
        return False, f"the guest counted {failures} failures"
    if checks < TLS_MIN_CHECKS:
        return False, (f"only {checks} checks ran, expected at least "
                       f"{TLS_MIN_CHECKS} -- the app stopped early")
    if DONE_MARK not in log:
        return False, f"no '{DONE_MARK}' line"
    if FAIL_MARK in log:
        return False, "the guest reported TLS selftest FAILED"

    return True, f"{checks} checks passed, 0 failed"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=540)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
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
        "-cdrom", args.iso,
        "-m", "128",
        "-smp", "4",
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        "-net", "none",
        # -snapshot: this suite never writes anything worth keeping, and the
        # real images must not be touched.
        "-drive", f"file={args.disk},format=raw,index=0,media=disk,snapshot=on",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk,snapshot=on",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    qemu = subprocess.Popen(qemu_cmd)
    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] entropy", args.timeout):
            print("[FAIL] kernel never reached entropy init")
            return 1
        print("[OK] booted, entropy initialized")

        if not wait_for_in_file(SERIAL_LOG, "[K] login", 90):
            print("[FAIL] kernel never reached login screen")
            return 1
        send_keys(LOGIN_KEYS)
        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] logged in with default password, desktop running")

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(
                mon_cmd, SERIAL_LOG, "/tmp/mectov_tls_cursor.ppm"):
            print("[FAIL] the Terminal never became ready — see the [launch] report above")
            return 1
        wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30)
        time.sleep(1.0)

        # Focus the terminal window (click inside it), so the command lands.
        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)

        if not run_app_cmd("tlsselftest", DONE_MARK, 120):
            print("[FAIL] tlsselftest.mct did not report 'TLS selftest OK'")
            # Show what the guest did say, since a partial run is the
            # interesting failure here.
            for ln in read_log().splitlines():
                if "tlsselftest" in ln or ln.startswith("[FAIL]") or ln.startswith("[INFO]"):
                    print("  " + ln.strip())
            return 1

        ok, msg = check_results()
        if not ok:
            print(f"[FAIL] {msg}")
            return 1
        print(f"[OK] TLS engine self-test: {msg}")

        if wait_for_in_file(SERIAL_LOG, "[PANIC]", 5):
            print("[FAIL] kernel panic on the run")
            return 1
        print("[OK] no kernel panic")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import terminal_launch
    sys.exit(main())
