#!/usr/bin/env python3
"""
scripts/nxtest.py — W^X regression for PAE + NX (v38.49, mmap case v38.164).

Verifies that EXECUTING CODE FROM USER DATA is killed by SIGSEGV, once per
source of data. Each probe gets its OWN QEMU boot:

  probe "stack": run /apps/nxtest.mct  places a `ret` on its stack
  probe "mmap":  run /apps/mmapnx.mct places a `ret` in an mmap'd page

For each probe:

  1. the kernel booted with NX active     ("[MEM] PAE paging on (NX enabled...")
  2. the app placed code in the data and called it — and DIED
     ("[W^X] execute fault" + "[CRASH] Ring 3 fault")
  3. the probe's FAIL line never appears  (executing data would mean NX is off)
  4. the OS stayed alive afterwards

Why one boot per probe rather than both in a single boot: a Ring 3 SIGSEGV is
cleaned up correctly (the task dies, the terminal is released) but the kernel
does not return to a usable prompt afterwards — the watchdog keeps logging a
zombie entry and never lets the shell take another command. That is a separate,
pre-existing defect. Booting per probe also keeps each probe's log free of the
other probe's markers, so every assertion below is a plain presence check with
no cross-probe counting.

Usage:
    python3 scripts/nxtest.py [--timeout 240]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

import terminal_launch

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

# Each entry: (label, path typed at the `run` prompt, app start marker,
#              app FAIL marker)
PROBES = [
    ("stack", "/apps/nxtest.mct", "NXTEST start", "NXTEST FAIL"),
    ("mmap", "/apps/mmapnx.mct", "MMAPNX start", "MMAPNX FAIL"),
]


# QEMU's sendkey takes key NAMES, not raw characters, for anything that is not
# a plain letter/digit — "/" and "." must be sent as slash/dot.
KEYMAP = {"/": "slash", ".": "dot", " ": "spc"}


def key_seq(path):
    """Translate `run <path>` into QEMU sendkey names."""
    return (["r", "u", "n", "spc"]
            + [KEYMAP.get(ch, ch) for ch in path]
            + ["ret"])


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


def read_file(path):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read()
    except (FileNotFoundError, OSError):
        return ""


class Session:
    """One QEMU boot with its own serial log and monitor socket."""

    def __init__(self, label, iso, disk, ext2, ram):
        self.label = label
        self.serial = f"/tmp/mectov_nx_{label}.log"
        self.mon_sock = f"/tmp/mectov_nx_{label}_monitor.sock"
        self.cursor = f"/tmp/mectov_nx_{label}_cursor.ppm"
        for p in (self.serial, self.mon_sock, self.cursor):
            try:
                os.unlink(p)
            except FileNotFoundError:
                pass
        self.qemu = subprocess.Popen([
            "qemu-system-i386",
            "-cpu", "qemu32,+nx",
            "-vga", "std",
            "-cdrom", iso,
            "-m", ram,
            "-smp", "4",
            "-display", "none",
            "-serial", f"file:{self.serial}",
            "-net", "none",
            "-drive", f"file={disk},format=raw,index=0,media=disk",
            "-drive", f"file={ext2},format=raw,index=1,media=disk",
            "-monitor", f"unix:{self.mon_sock},server,nowait",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def cmd(self, cmd):
        try:
            s = socket.socket(socket.AF_UNIX)
            s.connect(self.mon_sock)
            s.sendall((cmd + "\n").encode())
            time.sleep(0.15)
            s.close()
        except OSError as e:
            print(f"[!] monitor cmd '{cmd}' failed: {e}")

    def alive(self):
        return self.qemu.poll() is None

    def close(self):
        self.qemu.kill()
        try:
            self.qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


def run_probe(label, path, start_marker, fail_marker, args):
    s = Session(label, args.iso, args.disk, args.ext2, args.ram)
    try:
        if not wait_for_in_file(s.serial, "[K] login", args.timeout):
            print(f"[FAIL:{label}] kernel never reached login screen")
            return 1
        print(f"[OK:{label}] booted to login screen")

        # Match only the NX half of the banner: since v38.164 it also carries an
        # SMEP state, so pinning the whole old string would fail on a correct
        # build. SMEP's own consistency is gated by scripts/cpuid_test.py.
        if not wait_for_in_file(s.serial, "[MEM] PAE paging on (NX enabled", 15):
            print(f"[FAIL:{label}] PAE boot banner missing NX enabled")
            return 1
        print(f"[OK:{label}] PAE active with NX enabled")

        for k in LOGIN_KEYS:
            s.cmd("sendkey " + k)
            time.sleep(0.15)

        if not wait_for_in_file(s.serial, "BOOTED KERNEL LOOP", 90):
            print(f"[FAIL:{label}] login did not complete")
            return 1
        print(f"[OK:{label}] logged in, desktop running")

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(s.cmd, s.serial, s.cursor):
            print(f"[FAIL:{label}] the Terminal never became ready — see the [launch] report above")
            return 1
        if not wait_for_in_file(s.serial, "ipc_create key=0x0000DEAD", 30):
            print(f"[FAIL:{label}] terminal never became ready")
            return 1
        time.sleep(1.0)

        s.cmd("mouse_move 300 176")
        time.sleep(0.1)
        s.cmd("mouse_button 1"); time.sleep(0.1); s.cmd("mouse_button 0")
        time.sleep(0.5)

        started = False
        for _ in range(3):
            for _ in range(32):
                s.cmd("sendkey backspace")
            for k in key_seq(path):
                s.cmd("sendkey " + k)
                time.sleep(0.12)
            if wait_for_in_file(s.serial, start_marker, 25):
                started = True
                break
            time.sleep(1.0)
        if not started:
            print(f"[FAIL:{label}] app at {path} never started")
            return 1
        print(f"[OK:{label}] app running ({path})")

        # THE assertion: the execute-from-data fetch fault must be treated as a
        # W^X violation and kill the task — never demand-mapped into existence.
        if not wait_for_in_file(s.serial, "[W^X] execute fault", 30):
            print(f"[FAIL:{label}] no execute fault - {label} code was NOT blocked")
            return 1
        print(f"[OK:{label}] execute fault recognized as W^X violation")
        if not wait_for_in_file(s.serial, "[CRASH] Ring 3 fault", 30):
            print(f"[FAIL:{label}] SIGSEGV was not delivered to the task")
            return 1
        print(f"[OK:{label}] task killed by SIGSEGV")

        # And the FAIL line must never appear.
        if fail_marker in read_file(s.serial):
            print(f"[FAIL:{label}] {label} code executed - NX is not active")
            return 1
        print(f"[OK:{label}] no '{fail_marker}' line")

        time.sleep(5)
        if not s.alive():
            print(f"[FAIL:{label}] QEMU exited early with code {s.qemu.returncode}")
            return 1
        print(f"[OK:{label}] OS stayed alive after the NX kill")
        return 0
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--ram", default="128")
    ap.add_argument("--probe", choices=[p[0] for p in PROBES], action="append",
                    help="run only these probes (default: all)")
    args = ap.parse_args()

    rc = 0
    for label, path, start_marker, fail_marker in PROBES:
        if args.probe and label not in args.probe:
            continue
        print(f"\n=== probe '{label}' ({path}) ===")
        if run_probe(label, path, start_marker, fail_marker, args):
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
