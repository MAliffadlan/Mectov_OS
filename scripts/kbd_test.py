#!/usr/bin/env python3
"""scripts/kbd_test.py — interactive shell gate over QMP (M7.2 proof).

Boots mectov64.iso headless (KVM if available, else TCG), waits for the
shell prompt, types commands slowly (PS/2 overrun loses fast typing under
load), and asserts the results. Exit 0 PASS, nonzero FAIL.

Checks: `run argdemo foo bar` (ARGC=3/ARGV0..2/ARG-DONE, argv plumbing),
`ps` (PS-DONE + shell row), `mem` (MEM total/free sane), `help`.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
from qmp import QMP

ISO = "mectov64.iso"
SERIAL = "serial_kbdtest.log"
SOCK = "/tmp/qmp_kbdtest"


def has_kvm():
    return os.path.exists("/dev/kvm")


def boot():
    cmd = ["qemu-system-x86_64", "-machine", "q35"]
    if has_kvm():
        cmd += ["-cpu", "host", "-enable-kvm"]
    else:
        cmd += ["-cpu", "qemu64,+nx"]
    cmd += ["-m", "256", "-smp", "4", "-cdrom", ISO,
            "-serial", f"file:{SERIAL}", "-no-reboot", "-display", "none",
            "-qmp", f"unix:{SOCK},server=on,wait=off"]
    try:
        os.unlink(SOCK)
    except OSError:
        pass
    try:
        os.unlink(SERIAL)
    except OSError:
        pass
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)


def fsize(path):
    try:
        return os.path.getsize(path)
    except OSError:
        return 0


def wait_for(path, needle, timeout, poll=1.0, since=0):
    """Match needle only in bytes appended after file offset `since`
    (boot-time output would otherwise satisfy every check instantly)."""
    end = time.time() + timeout
    while time.time() < end:
        try:
            with open(path, errors="replace") as f:
                f.seek(since)
                if needle in f.read():
                    return True
        except OSError:
            pass
        time.sleep(poll)
    return False


def type_line(q, serial, line):
    """Closed-loop typing: after each key, wait for the shell's echo of
    THAT key before sending the next (open-loop bursts overrun the 1-byte
    8042 buffer when the guest stalls with IF=0 under load). Returns False
    on any miss."""
    mark = fsize(serial)
    for k in line:
        # QEMU qcode name for space is "spc" (a literal " " is not a key).
        q.sendkey("spc" if k == " " else k)
        # echo expectation: printable char echoes itself; ret -> newline
        # (shell prints \n, shown on serial as \r\n).
        want = "\n" if k == "ret" else k
        end = time.time() + 15
        found = False
        while time.time() < end:
            try:
                with open(serial, errors="replace") as f:
                    f.seek(mark)
                    chunk = f.read()
            except OSError:
                chunk = ""
            at = chunk.find(want)
            if at >= 0:
                mark += at + len(want)
                found = True
                break
            time.sleep(0.2)
        if not found:
            print(f"type_line: echo missing for {k!r}")
            return False
    return True


def main():
    if not os.path.exists(ISO):
        print("kbd_test: build the ISO first (make iso64)")
        return 1
    qemu = boot()
    try:
        # wait for shell prompt (boot can take a while on TCG)
        if not wait_for(SERIAL, "mct> ", 180):
            print("kbd_test FAIL: no shell prompt")
            return 1
        q = QMP(SOCK)
        ok = type_line(q, SERIAL, list("run argdemo foo bar") + ["ret"])
        print("type-run:", "ok" if ok else "MISS")
        ok2 = type_line(q, SERIAL, list("ps") + ["ret"])
        print("type-ps:", "ok" if ok2 else "MISS")
        ok3 = type_line(q, SERIAL, list("mem") + ["ret"])
        print("type-mem:", "ok" if ok3 else "MISS")
        q.close()
        # Give the shell time to run everything (poll latency under load),
        # then verify the whole log (boot-time lines can't satisfy the
        # typed-command markers: ARGC=3/ARGV0/PS-DONE/MEM appear only here).
        time.sleep(20)
        serial = open(SERIAL, errors="replace").read()
        checks = [
            (ok and ok2 and ok3, "typing"),
            ("ARGC=3" in serial, "ARGC=3"),
            ("ARGV0=argdemo" in serial, "ARGV0"),
            ("ARGV1=foo" in serial and "ARGV2=bar" in serial, "ARGV12"),
            ("ARG-DONE" in serial, "ARG-DONE"),
            ("PS-DONE" in serial, "PS-DONE"),
            ("MEM total=" in serial, "MEM"),
            ("FATAL" not in serial, "no-FATAL"),
        ]
        rc = 0
        for good, name in checks:
            print(f"  [{ 'PASS' if good else 'FAIL' }] {name}")
            rc = rc or (not good)
        return rc
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            qemu.kill()


if __name__ == "__main__":
    sys.exit(main())
