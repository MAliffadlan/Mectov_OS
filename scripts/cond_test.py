#!/usr/bin/env python3
"""
scripts/cond_test.py — mutex + condition-variable regression test (v38.27).

Boots mectov.iso on 4 SMP cores, logs in, launches the Terminal, runs the
Ring 3 `conddemo` app (`run /apps/conddemo.mct`) and verifies from the
serial log that the futex-based pthread-style primitives hold up under
real parallel load:

  * MUTEX STRESS — 4 threads bump a shared counter LOCK_ITERS times under
    a futex mutex; the final value must be EXACTLY 4 * LOCK_ITERS. On a
    broken lock (lost update on 4 cores) it would land strictly below.
  * PRODUCER/CONSUMER — a bounded buffer (8 slots) with 2 producers and
    2 consumers using condvar wait/signal; every one of the 3000 items
    must be consumed exactly once (no loss, no duplicates, no corruption),
    which exercises both the not-full and not-empty wait paths heavily.

The kernel must never panic during the whole run.

v38.160: --repeat N runs the demo N times inside ONE boot, each round matched
against only the bytes appended after that round started (offset tracking, so
round k>1 can never be satisfied by round k-1's markers), and --smp overrides
the vCPU count. ANY failed round fails the suite — that is what turns a
~1-in-4-6 per-run race into a gate that can actually see it, and it is the
shape that pinned audit F0's scheduler bug (the fix made the same 60-round run
green; see the v38.160 README row).

Two v38.160 verdicts on top of the app's own markers:
  * a "[WATCH] parked-frame" line in the serial log (a READY/RUNNING task whose
    saved frame is an exit/dead park loop — resumed into dead code) FAILS the
    run outright: that is the corruption signature of the slot-reuse bug;
  * "[WATCH] create-skip" is reported as survived evidence (the scheduler
    dodged a slot whose dead predecessor was still current on a core), not as a
    failure. With the fix that collision is harmless by construction.

A stall ("no verdict within Ns") also dumps a full QEMU-monitor post-mortem
(registers of every vCPU, current_task, rq[0..3], tasks[], two kernel stacks,
the futex table) so the next occurrence is diagnosable without a rerun.

Usage:
    python3 scripts/cond_test.py [--timeout 300] [--repeat 1] [--smp 4]
    python3 scripts/cond_test.py --repeat 60 --timeout 3000   # deep stress
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import time

import terminal_launch  # corner-reset + screendump-verified icon double-click

SERIAL_LOG = "/tmp/mectov_cond_serial.log"
MON_SOCK = "/tmp/mectov_cond_monitor.sock"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

RUN_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s", "slash",
            "c", "o", "n", "d", "d", "e", "m", "o", "dot", "m", "c", "t", "ret"]


def read_from(path, offset):
    """Decoded text of `path` from byte `offset` on ('' if unreadable yet).

    Binary read + explicit seek: a byte offset is exact, whereas text-mode
    seek only accepts opaque cookies. Substring checks against this slice are
    what makes --repeat rounds independent — an earlier round's markers sit
    BELOW the offset and can never satisfy a later round (the same run-scoped
    offset discipline the Q3 relaunch suite needed).
    """
    try:
        with open(path, "rb") as f:
            f.seek(offset)
            return f.read().decode("utf-8", errors="replace")
    except (FileNotFoundError, OSError):
        return ""


def log_offset(path):
    """Current size of the serial log = the next round's search origin."""
    try:
        return os.path.getsize(path)
    except OSError:
        return 0


def wait_for_in_file(path, needle, timeout, offset=0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if needle in read_from(path, offset):
            return True
        time.sleep(1)
    return False


def wait_for_any_in_file(path, needles, timeout, offset=0):
    """Wait for the FIRST of `needles` in the new bytes; returns it or None."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        text = read_from(path, offset)
        for needle in needles:
            if needle in text:
                return needle
        time.sleep(1)
    return None


def mon_cmd(cmd, read=False, timeout=3):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(timeout)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(0.15)
        out = b""
        if read:
            try:
                while True:
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    out += chunk
            except socket.timeout:
                pass
        s.close()
        return out.decode(errors="replace")
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")
        return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--repeat", type=int, default=1,
                    help="run the demo N times in one boot; any failed round fails the suite")
    ap.add_argument("--smp", type=int, default=None,
                    help="vCPU count (default: $MCTOV_SMP, else 4)")
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

    smp = str(args.smp) if args.smp is not None else os.environ.get("MCTOV_SMP", "4")
    qemu_cmd = [
        "qemu-system-i386",
        "-cpu", "qemu32,+nx",
        "-vga", "std",
        "-cdrom", args.iso,
        "-m", "128",
        "-smp", smp,
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        "-net", "none",
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    if args.kvm:
        qemu_cmd.append("-enable-kvm")
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(f"[OK] QEMU up: smp={smp} rounds={args.repeat} timeout={args.timeout}s")

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

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(
                mon_cmd, SERIAL_LOG, "/tmp/mectov_cond_cursor.ppm"):
            print("[FAIL] the Terminal never became ready — see the [launch] report above")
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

        # ---- app rounds ----------------------------------------------------
        # Every round types `run /apps/conddemo.mct` into the focused terminal
        # and waits for its OWN verdict. The search origin is the serial log
        # size taken just before the round's command is typed, so markers from
        # earlier rounds can never satisfy a later one. Any failed round fails
        # the suite: --repeat turns a rare per-run race into a gate that can
        # see it.
        # Phase-aware budget: the whole run is deadline-bound (boot + launch
        # already consumed part of it) and the remaining time is split across
        # the rounds still to go. On the 2-core TCG runner boot+launch can
        # take over two minutes, so budgets come from what's LEFT, not from a
        # fixed constant.
        import time as _t
        deadline = _t.time() + args.timeout
        rounds_pass = 0
        failed_rounds = []
        for rnd in range(1, args.repeat + 1):
            rounds_left = args.repeat - rnd + 1
            budget = max(60, int((deadline - _t.time()) / rounds_left))
            off = log_offset(SERIAL_LOG)
            # Re-focus the terminal every round: the previous round's app exit
            # releases the foreground and the WM may hand focus elsewhere, in
            # which case every keystroke of this round goes nowhere and the
            # harness would misreport "never started". (Observed once under
            # KVM: round 4's keys vanished; the round-1 click is not enough.)
            mon_cmd("mouse_move 300 176")
            time.sleep(0.1)
            mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
            time.sleep(0.5)
            launched = False
            start_deadline = _t.time() + min(budget, 120)
            for _ in range(3):
                for _ in range(24):
                    mon_cmd("sendkey backspace")
                for k in RUN_KEYS:
                    mon_cmd("sendkey " + k)
                    time.sleep(0.12)
                mon_cmd("sendkey ret")
                left = start_deadline - _t.time()
                if wait_for_in_file(SERIAL_LOG, "[CONDDEMO] start", max(5, left), off):
                    launched = True
                    break
                time.sleep(1.0)
                if _t.time() >= start_deadline:
                    break
            if not launched:
                print(f"[FAIL] round {rnd}/{args.repeat}: conddemo never started")
                return 1

            t0 = _t.time()
            verdict = wait_for_any_in_file(
                SERIAL_LOG,
                ["[CONDDEMO] ALL PASS", "[CONDDEMO] FAIL", "[PANIC]"],
                budget, off)
            took = int(_t.time() - t0)
            if verdict == "[CONDDEMO] ALL PASS":
                rounds_pass += 1
                print(f"[OK] round {rnd}/{args.repeat}: [CONDDEMO] ALL PASS ({took}s)")
                continue
            if verdict is None:
                print(f"[FAIL] round {rnd}/{args.repeat}: no verdict within {budget}s")
                print("=== conddemo did NOT finish — scheduler/sync post-mortem ===")
                # QEMU 8.2 monitor: `info registers -c N` is unsupported;
                # select the CPU first, then dump registers. Addresses below
                # come from `nm myos.bin.debug` (globals keep their addresses
                # across re-builds of this tree): current_task, timer_ticks,
                # rq[4] (64 tids + count = 65 dwords, stride 0x104), tasks[]
                # (sizeof(task_t)=1520, base 0xa61140), thread 8's kernel
                # stack window (its saved interrupt frame, whose EIP sits at
                # frame+0x34, is what tells us WHERE the stuck thread is),
                # and the 64-entry futex table (68 dwords each).
                def dump_mem(label, cmd, tmo=6):
                    print(f"--- {label} ---")
                    out = mon_cmd(cmd, read=True, timeout=tmo)
                    for ln in out.splitlines():
                        ln = ln.strip()
                        if re.match(r"^[0-9a-fA-F]{8,16}:", ln):
                            print("   ", ln[:160])

                def postmortem(n):
                    print(f"=== snapshot {n} ===")
                    for c in range(int(smp)):
                        mon_cmd(f"cpu {c}")
                        print(f"--- snapshot {n}: vCPU {c} registers ---")
                        for ln in mon_cmd("info registers", read=True).splitlines():
                            ln = ln.strip()
                            if "=" in ln and not ln.startswith("(qemu)"):
                                print("   ", ln[:150])
                    mon_cmd("cpu 0")
                    dump_mem(f"snapshot {n}: current_task", "xp /16wx 0x001cd140")
                    dump_mem(f"snapshot {n}: timer_ticks", "xp /1wx 0x002ec048")
                    if n == 1:
                        for i, base in enumerate(("0x00a600e0", "0x00a601e4",
                                                  "0x00a602e8", "0x00a603ec")):
                            dump_mem(f"snapshot 1: rq[{i}]", f"xp /65wx {base}")
                        for tid, addr in ((0, "0x00a61140"), (4, "0x00a62900"),
                                          (5, "0x00a62ef0"), (6, "0x00a634e0"),
                                          (7, "0x00a63ad0"), (8, "0x00a640c0"),
                                          (9, "0x00a646b0")):
                            dump_mem(f"snapshot 1: tasks[{tid}]",
                                     f"xp /380wx {addr}", tmo=10)
                    # t8 stack: arena slot [0xac1000,0xaca000); window under the
                    # top covers the frame even when the thread sits shallow.
                    dump_mem(f"snapshot {n}: kstack t8", "xp /1024wx 0x00ac9000", tmo=10)
                    if n == 1:
                        dump_mem("snapshot 1: kstack t5", "xp /1024wx 0x00aae000", tmo=10)
                    for j, base in enumerate(("0x005927c0", "0x005938c0",
                                              "0x005949c0", "0x00595ac0")):
                        dump_mem(f"snapshot {n}: futex[{j*16}..{j*16+15}]",
                                 f"xp /1088wx {base}", tmo=10)

                postmortem(1)
                time.sleep(3)
                postmortem(2)
                # Serial tail is the primary post-mortem evidence: the app's
                # phase heartbeats show exactly where progress stopped.
                print("=== serial tail (post-mortem) ===")
                lines = [l for l in read_from(SERIAL_LOG, 0).splitlines()
                         if "[LOAD]" not in l]
                for l in lines[-40:]:
                    print("   ", l[:130])
                return 1
            # App-reported failure (or a panic): keep the exact lines, then
            # keep going so a --repeat run reports how many rounds failed
            # instead of stopping at the first.
            sig = [l for l in read_from(SERIAL_LOG, off).splitlines()
                   if "[CONDDEMO] FAIL" in l or "[PANIC]" in l]
            print(f"[FAIL] round {rnd}/{args.repeat}: {verdict} ({took}s)")
            for l in sig[-3:]:
                print("   ", l[:160])
            failed_rounds.append(rnd)

        if failed_rounds:
            print(f"[FAIL] cond_test: smp={smp} runs={rounds_pass}/{args.repeat} "
                  f"ALL PASS; failed rounds {failed_rounds}")
            return 1

        if "[PANIC]" in read_from(SERIAL_LOG, 0):
            print("[FAIL] kernel panicked during the condvar run")
            return 1
        print("[OK] no kernel panic in the whole condvar run")

        # v38.160: kernel-side stall signature. sched_integrity_sweep (1 Hz,
        # BSP) prints "[WATCH] parked-frame" when a READY/RUNNING task's saved
        # frame is an exit/dead park loop — i.e. a slot was resumed into dead
        # code (the condvar stall: the thread never runs, its parent parks in
        # waitpid forever, the round dies with no verdict). The rounds above
        # already fail on a stall; this catches the same corruption when it
        # happens to heal itself before the per-round budget expires.
        # "[WATCH] create-skip" is informational: the scheduler dodged a slot
        # whose dead predecessor was still current on a core (the window this
        # release closes) — surviving that is the point, so it is not a
        # failure, just evidence the race is still being exercised.
        watch_log = read_from(SERIAL_LOG, 0)
        if "[WATCH] parked-frame" in watch_log:
            print("[FAIL] a task was resumed into an exit park loop "
                  "(scheduler slot-reuse corruption)")
            for l in watch_log.splitlines():
                if "[WATCH]" in l:
                    print("   ", l[:160])
            return 1
        skipped = watch_log.count("[WATCH] create-skip")
        print(f"[OK] no resume-into-park-loop corruption ({skipped} create-skip "
              f"event(s) survived)")

        time.sleep(5)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after the condvar demo")
        # The one line that says what this gate actually proved: which SMP
        # shape it ran under and how many rounds passed. CI logs are read for
        # "did the cross-core path run?", so make it readable at a glance.
        print(f"[OK] cond_test: smp={smp} runs={rounds_pass}/{args.repeat} ALL PASS")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
