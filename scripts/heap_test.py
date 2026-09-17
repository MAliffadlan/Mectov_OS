#!/usr/bin/env python3
"""scripts/heap_test.py — M10 kernel heap gate driven from Ring-3.

The allocator has a kernel-side selftest (M10 HEAP SELFTEST OK) that runs
pre-STI on boot's own page tables. What that cannot prove is the property the
rest of the port depends on: the arena is mapped in a *task's* address space
too, so kmalloc/kfree work from Ring-3 syscalls and from any CR3.

So this test boots, types `kmem` into the 64-bit shell (a Ring-3 task with its
own PML4), runs `run hello` in between to churn tasks, and then types `kmem`
again. Each `kmem` performs a real kmalloc/write/read/kfree round trip on the
caller's CR3 (`probe=ok`) and prints the live counters.

Checks: probe round trip ok every time, probes/allocs advance, nothing leaks
(live=0 every call, mapped pages constant, bad=0, oom=0), and no FATAL.
Exit 0 PASS, nonzero FAIL.
"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
from qmp import QMP

ISO = "mectov64.iso"
SERIAL = "serial_heaptest.log"
SOCK = "/tmp/qmp_heaptest"


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
    for p in (SOCK, SERIAL):
        try:
            os.unlink(p)
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
    """Closed-loop typing: wait for the echo of each key before the next one
    (open-loop bursts overrun the 1-byte 8042 buffer under load)."""
    mark = fsize(serial)
    for k in line:
        q.sendkey("spc" if k == " " else k)
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


FIELDS = ("allocs", "frees", "live", "freeblk", "largest", "pages", "oom",
          "bad", "probes")


def parse(line):
    """'KMEM probe=ok allocs=6 frees=6 live=0 ...' -> (probe, {field: int})."""
    m = re.search(r"KMEM probe=(\w+)\s+(.*)", line)
    if not m:
        return None, {}
    vals = {}
    for k, v in re.findall(r"(\w+)=(\d+)", m.group(2)):
        if k in FIELDS:
            vals[k] = int(v)
    return m.group(1), vals


def main():
    if not os.path.exists(ISO):
        print("heap_test: build the ISO first (make iso64)")
        return 1
    qemu = boot()
    try:
        if not wait_for(SERIAL, "mct> ", 180):
            print("heap_test FAIL: no shell prompt")
            return 1
        q = QMP(SOCK)
        typed = type_line(q, SERIAL, list("kmem") + ["ret"])
        print("type-kmem:", "ok" if typed else "MISS")
        typed2 = type_line(q, SERIAL, list("run hello") + ["ret"])
        print("type-run:", "ok" if typed2 else "MISS")
        typed3 = type_line(q, SERIAL, list("kmem") + ["ret"])
        print("type-kmem2:", "ok" if typed3 else "MISS")
        q.close()
        time.sleep(25)  # let the shell (and the spawned task) finish

        serial = open(SERIAL, errors="replace").read()
        boots = re.findall(r"^\[K64\] M10 HEAP SELFTEST OK.*$", serial,
                           re.MULTILINE)
        lines = [l for l in serial.splitlines() if l.startswith("KMEM ")]
        reads = [parse(l) for l in lines]
        probes = [v for p, v in reads if v]
        oks = [p == "ok" for p, v in reads]
        print(f"  boot selftest lines: {len(boots)}")
        for l in lines:
            print("  " + l.strip())

        checks = [
            (typed and typed2 and typed3, "typing"),
            (len(boots) == 1, "kernel-side M10 selftest ran once"),
            (len(reads) >= 2, f"kmem answered ({len(reads)} calls)"),
            (all(oks), "probe round trip ok from Ring-3"),
            (all(p["bad"] == 0 for p in probes), "no heap corruption caught"),
            # oom=1 is the boot selftest's deliberate over-window request; what
            # matters is that normal use never adds to it.
            (all(p["oom"] == 1 for p in probes), "no OOM during normal use"),
            (all(p["pages"] > 0 for p in probes), "arena is frame-backed"),
            (all(p["live"] == 0 for p in probes), "live bytes 0 (probe freed)"),
            ("KMEM-DONE" in serial, "KMEM-DONE"),
            ("FATAL" not in serial, "no-FATAL"),
            ("[K64] FAIL" not in serial, "no-FAIL"),
        ]
        if len(probes) >= 2:
            checks += [
                (probes[-1]["allocs"] > probes[0]["allocs"],
                 "kernel heap used Ring-3 allocations"),
                (probes[-1]["probes"] - probes[0]["probes"] == len(probes) - 1,
                 "one probe round trip per kmem call"),
                (probes[-1]["pages"] == probes[0]["pages"],
                 "mapped pages did not grow (no leak)"),
                (probes[-1]["freeblk"] <= 3,
                 "free list stayed short (coalescing)"),
            ]
        rc = 0
        for good, name in checks:
            print(f"  [{'PASS' if good else 'FAIL'}] {name}")
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
