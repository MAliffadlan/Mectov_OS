#!/usr/bin/env python3
"""make check: run the full Mectov CI test battery locally.

Mirrors .github/workflows/build-boot-test.yml step-for-step (same suites,
same order, same --timeout values, same fresh disk images), so a green
`make check` locally is the same signal as a green CI run — without the
23-minute GitHub round trip.

Usage:
    python3 scripts/check.py              run every suite (CI parity)
    python3 scripts/check.py --quick      fast, high-signal subset
    python3 scripts/check.py --only boot  run one suite
    python3 scripts/check.py --list       print the suite manifest
    python3 scripts/check.py --keep-images   don't recreate disk/ext2/fat32
    python3 scripts/check.py --kvm           also run the fork/fputest KVM
                                             regressions (default: off — some
                                             hosts' KVM cannot run the qemu32
                                             machine even with /dev/kvm
                                             present; CI runs them)

Each suite's full output lands in .check/<name>.log; a concise table and a
summary are printed at the end (also saved to .check/summary.txt). Exit
code is 0 only when every suite passed.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG_DIR = os.path.join(ROOT, ".check")
ISO = os.path.join(ROOT, "mectov.iso")

# (name, test script, CI --timeout in seconds). Order matters: it mirrors
# the CI workflow, and several suites share the workspace disk.img, so the
# sequence is part of the contract.
SUITES = [
    ("boot",             "boot_test.py",              240),
    ("login",            "login_redesign_test.py",    300),
    ("lock",             "lock_test.py",              480),
    ("autolock",         "autolock_test.py",          480),
    ("hardening",        "hardening_test.py",         480),
    ("fbmap",            "fbmap_test.py",             480),
    ("taskbar_hover",    "taskbar_hover_test.py",     300),
    ("startmenu_search", "startmenu_search_test.py",  300),
    ("fork",             "fork_test.py",              240),
    ("jobcontrol",       "jobcontrol_test.py",        240),
    ("fputest",          "fputest.py",                240),
    ("nxtest",           "nxtest.py",                 240),
    # KVM regressions (opt-in via --kvm): CI runs them when /dev/kvm exists,
    # but a local /dev/kvm is not enough — some hosts' KVM cannot run the
    # qemu32 machine and the guest stalls for the full timeout (this box
    # does). Default off; fork-kvm is the mandatory SMP keyboard regression.
    ("fork_kvm",         "fork_test.py",              240, ["--kvm"], "kvm"),
    ("fputest_kvm",      "fputest.py",                240, ["--kvm"], "kvm"),
    ("fat32",            "fat32_test.py",             240),
    ("mount",            "mount_test.py",             240),
    ("ahci",             "ahci_test.py",              360),
    ("usb",              "usb_test.py",               360),
    ("socktest",         "socktest.py",               300),
    ("poweroff",         "poweroff_test.py",          240),
    ("pollselect",       "pollselect_test.py",        240),
    ("fuzz",             "fuzz_test.py",              300),
    ("iocache",          "iocache_test.py",           300),
    ("app_smoke",        "app_smoke_test.py",         360),
    ("doom",             "doom_test.py",              480),
    ("mmapfile",         "mmapfile_test.py",          240),
    ("syncfile",         "syncfile_test.py",           300),
    ("rusthello",        "rusthello_test.py",         240),
    ("lseekfile",        "lseekfile_test.py",         240),
    ("panic",            "panic_test.py",             240),
    ("perm",             "perm_test.py",              360),
    ("thread",           "thread_test.py",            360),
    ("cond",             "cond_test.py",              300),
    ("rlimit",           "rlimit_test.py",            360),
    ("ulimit",           "ulimit_test.py",            360),
    ("bigread",          "bigread_test.py",           420),
]

# Fast, high-signal subset for local iteration (~6-8 min TCG).
QUICK = {"boot", "fork", "jobcontrol", "fputest", "fuzz",
         "iocache", "app_smoke", "doom", "thread", "cond", "usb"}

TOOLS = ["qemu-system-i386", "mkfs.fat", "mkfs.ext2", "mcopy", "mmd"]


def suite_entries(args):
    """Expand the manifest into (name, script, timeout, extra, need) rows."""
    want = QUICK if args.quick else None
    out = []
    for row in SUITES:
        name, script, timeout = row[0], row[1], row[2]
        extra, need = (row[3], row[4]) if len(row) > 3 else ([], "")
        if want is not None and name not in want:
            continue
        if args.only and name not in args.only:
            continue
        if need == "kvm" and not args.kvm:
            continue  # KVM regressions are opt-in (host KVM may stall)
        if need == "kvm" and not kvm_available():
            print("[check] --kvm requested but /dev/kvm is unavailable "
                  "— skipping %s" % name)
            continue
        out.append((name, script, timeout, extra, need))
    return out


def kvm_available():
    return (os.path.exists("/dev/kvm") and
            os.access("/dev/kvm", os.R_OK | os.W_OK))


def preflight():
    missing = [t for t in TOOLS if shutil.which(t) is None]
    if missing:
        print("[check] missing host tools: " + ", ".join(missing))
        print("[check] install them (see .github/workflows/build-boot-test.yml)")
        sys.exit(2)
    if not os.path.exists(ISO):
        print("[check] %s not found — run `make check` (builds kernel + ISO first)." % ISO)
        sys.exit(2)


def recreate_images():
    """Fresh disk/ext2/fat32 images — CI parity (the workflow recreates them
    on every run). Pass --keep-images to skip; without it, whatever VFS state
    was saved into these images is reset to a clean boot."""
    print("[check] recreating disk.img / ext2.img / fat32.img (fresh, CI parity)")
    print("[check]   (use `make check CHECK_ARGS=--keep-images` to keep current images)")
    os.chdir(ROOT)
    subprocess.run(["dd", "if=/dev/zero", "of=disk.img", "bs=512",
                    "count=2048", "status=none"], check=True)
    subprocess.run(["dd", "if=/dev/zero", "of=ext2.img", "bs=1M",
                    "count=2", "status=none"], check=True)
    subprocess.run(["mkfs.ext2", "-F", "ext2.img"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["dd", "if=/dev/zero", "of=fat32.img", "bs=1M",
                    "count=16", "status=none"], check=True)
    subprocess.run(["mkfs.fat", "-F", "32", "-S", "512", "fat32.img"],
                   check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)


def run_suite(entry, index, total, verbose):
    name, script, timeout, extra, _ = entry
    log_path = os.path.join(LOG_DIR, name + ".log")
    cmd = [sys.executable, os.path.join("scripts", script),
           "--timeout", str(timeout)] + extra
    print("[check] [%2d/%2d] %-16s ..." % (index, total, name), end="", flush=True)
    t0 = time.time()
    with open(log_path, "wb") as logf:
        proc = subprocess.Popen(cmd, cwd=ROOT, stdout=logf,
                                stderr=subprocess.STDOUT)
        # The script already bounds itself with --timeout; this outer cap is
        # a safety net for a wedged QEMU/python (CI timeout + 5 min grace).
        try:
            proc.wait(timeout=timeout + 300)
            status = "PASS" if proc.returncode == 0 else "FAIL"
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            status = "TIMEOUT"
    wall = time.time() - t0
    if verbose:
        with open(log_path, "rb") as f:
            sys.stdout.buffer.write(b"\n" + f.read() + b"\n")
    print(" %-7s (%4ds)" % (status, int(wall)))
    if status != "PASS":
        tail = tail_lines(log_path, 30)
        print("--- tail of .check/%s.log ---" % name)
        print(tail)
    return (name, status, int(wall))


def tail_lines(path, n):
    with open(path, "rb") as f:
        data = f.read().decode("utf-8", "replace")
    lines = data.splitlines()
    return "\n".join(lines[-n:])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quick", action="store_true",
                    help="fast high-signal subset instead of the full battery")
    ap.add_argument("--only", action="append", metavar="NAME",
                    help="run only the named suite(s); repeatable")
    ap.add_argument("--list", action="store_true",
                    help="print the suite manifest and exit")
    ap.add_argument("--keep-images", action="store_true",
                    help="do not recreate disk.img/ext2.img/fat32.img")
    ap.add_argument("--kvm", action="store_true",
                    help="include the fork/fputest KVM regressions "
                         "(requires a KVM that can run the qemu32 machine)")
    ap.add_argument("--verbose", action="store_true",
                    help="stream each suite's full output to the console")
    args = ap.parse_args()

    if args.list:
        for row in SUITES:
            name, script, timeout = row[0], row[1], row[2]
            need = row[4] if len(row) > 3 else ""
            tag = " [needs --kvm]" if need == "kvm" else ""
            print("%-16s %s --timeout %d%s" % (name, script, timeout, tag))
        return 0

    os.makedirs(LOG_DIR, exist_ok=True)
    preflight()
    if not args.keep_images:
        recreate_images()

    entries = suite_entries(args)
    if not entries:
        print("[check] nothing to run (--only matched no suite)")
        return 1
    print("[check] %d suite(s) in %s" % (len(entries), ROOT))
    if not args.kvm:
        print("[check] KVM regressions skipped by default — add --kvm to "
              "include them (fork_kvm/fputest_kvm)")
    elif not kvm_available():
        print("[check] --kvm requested but /dev/kvm unavailable")
    total_t0 = time.time()
    results = []
    for i, entry in enumerate(entries, 1):
        results.append(run_suite(entry, i, len(entries), args.verbose))
    total_wall = int(time.time() - total_t0)

    # ---- concise report ----
    lines = []
    lines.append("%-16s %-8s %8s" % ("SUITE", "STATUS", "TIME"))
    lines.append("-" * 36)
    for name, status, wall in results:
        lines.append("%-16s %-8s %6ds" % (name, status, wall))
    passed = sum(1 for _, s, _ in results if s == "PASS")
    failed = sum(1 for _, s, _ in results if s != "PASS")
    lines.append("-" * 36)
    lines.append("TOTAL   %d passed, %d failed  (%dm %02ds)" %
                 (passed, failed, total_wall // 60, total_wall % 60))
    if not args.kvm:
        lines.append("(KVM regressions not run — pass --kvm to include them)")
    report = "\n".join(lines)
    print()
    print(report)
    with open(os.path.join(LOG_DIR, "summary.txt"), "w") as f:
        f.write(report + "\n")
    print("\n[check] logs in .check/, report in .check/summary.txt")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
