#!/usr/bin/env python3
"""
scripts/ptr_trunc_audit.py — count the kernel's pointer-to-int truncation sites,
and stop the number growing silently.

`docs/architecture/why-32bit.md` measures the 32-bit line by compiling every
`src/**/*.c` under a 64-bit flag set and counting `-Wpointer-to-int-cast`. Those
casts are all correct at 32 bits (the great majority are *required* by hardware
— IDT gate bases, GDT descriptor bases, ESP/EIP, ISA DMA's 24-bit physical
addresses, the AP trampoline — or by the `int 0x80` register ABI). None is a
live bug. The risk they carry is the opposite of the one the doc originally
implied: a reader who "fixes" a hardware-mandated cast into `uintptr_t` breaks
the 32-bit ABI. See that document for the categorisation.

So this is a RATCHET, not a to-do list. It fails when the count grows, so a new
careless truncation cannot land unnoticed, and it stays quiet when the count
falls — a reduction is a fix, not a regression.

Two things the raw measurement gets wrong, both handled here:

  * Count DISTINCT `path:line`, never warnings. The inline `sys_*` wrappers in
    `src/include/syscall.h` are re-reported by every translation unit that
    includes the header, so a per-file tally is inflated several times over
    (696 warnings, 113 real sites at the time of writing).
  * Filter to real diagnostics. `-fdiagnostics-plain-output` still emits
    "In file included from" chains and GCC's own headers, which look like
    `path:N` if matched loosely — a naive grep counted 388.

Usage:
    python3 scripts/ptr_trunc_audit.py            # report; --check is implied
    python3 scripts/ptr_trunc_audit.py --check    # exit 1 if the count grew
    python3 scripts/ptr_trunc_audit.py --list     # also print every site
    python3 scripts/ptr_trunc_audit.py --no-check # report only, never fail
    python3 scripts/ptr_trunc_audit.py --update   # print a new BASELINE line

Takes about half a minute: it compiles ~160 files to /dev/null (nothing is
written to disk). Needs a gcc that can target x86-64 — `gcc-multilib` provides
it, and the CI build job already installs that.
"""
import argparse
import os
import re
import subprocess
import sys
from collections import Counter

# Measured 2026-10-23 over 159 files in src/. The document recorded 112 on
# 2026-10-06; the single addition is a second site in src/drivers/sb16.c.
# Raising this is a deliberate act (`--update` prints the line), not a reflex.
BASELINE = 113

# The flag set the document prescribes. -m64 is the whole trick: it makes the
# compiler see the casts that a 32-bit build cannot warn about.
FLAGS = [
    "-m64", "-std=gnu99", "-ffreestanding", "-O2", "-march=x86-64",
    "-mcmodel=kernel", "-mno-red-zone", "-fno-pie", "-fno-pic",
    "-mgeneral-regs-only",
]

# Only lines that end in the diagnostic we are counting, and only when they
# start with a real `path:line` — this is what drops the include chains and
# GCC's own headers.
SITE_RE = re.compile(r"^(\S+\.(?:c|h)):(\d+):.*pointer-to-int-cast")

# Scope matches the document: src/ only. third_party/ is vendored (Quake III,
# TinyGL) and kernel.c lives at the repo root, so neither is in the count.
SCOPE = "src/*.c"


def repo_root():
    return subprocess.run(["git", "rev-parse", "--show-toplevel"],
                          capture_output=True, text=True, check=True).stdout.strip()


def source_files():
    r = subprocess.run(["git", "ls-files", SCOPE],
                       capture_output=True, text=True, check=True)
    return [f for f in r.stdout.split() if f.endswith(".c")]


def scan(root, files):
    """Return the set of distinct `path:line` sites."""
    sites = set()
    for rel in files:
        r = subprocess.run(
            ["gcc", *FLAGS, "-Wpointer-to-int-cast",
             "-fdiagnostics-plain-output", "-c", rel, "-o", os.devnull],
            cwd=root, capture_output=True, text=True)
        for line in r.stderr.splitlines():
            m = SITE_RE.match(line)
            if m:
                # Normalise `src/sys/../include/x.h` to `src/include/x.h` so a
                # header site counts once no matter which TU reported it.
                sites.add((os.path.normpath(m.group(1)), int(m.group(2))))
    return sites


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="exit 1 when the count exceeds BASELINE (default)")
    ap.add_argument("--no-check", action="store_true",
                    help="report only; never fail")
    ap.add_argument("--list", action="store_true",
                    help="print every site, not just the per-file table")
    ap.add_argument("--update", action="store_true",
                    help="print the BASELINE line to paste, then exit")
    ap.add_argument("--baseline", type=int, default=BASELINE,
                    help=f"compare against this instead of {BASELINE}")
    args = ap.parse_args()
    if args.check and args.no_check:
        ap.error("--check and --no-check are mutually exclusive")

    root = repo_root()
    files = source_files()
    sites = scan(root, files)

    per_file = Counter(p for p, _ in sites)
    total = len(sites)

    for path, n in per_file.most_common():
        print(f"[ptr] {path:<40} {n}")
    print(f"[ptr] {total} distinct sites across {len(per_file)} files "
          f"({len(files)} files scanned)")

    if args.update:
        print()
        print(f"BASELINE = {total}")
        return 0

    if args.list:
        print()
        for path, line in sorted(sites):
            print(f"       {path}:{line}")

    if args.no_check:
        return 0

    if total > args.baseline:
        print(f"[FAIL] pointer-truncation sites grew: {total} "
              f"(baseline {args.baseline})")
        print( "       A new cast from a pointer to a narrower integer landed.")
        print( "       If it is hardware- or ABI-mandated (IDT/GDT base, ESP/EIP,")
        print( "       ISA DMA, AP trampoline) that is expected — say so in the")
        print( "       commit and raise BASELINE with `--update`. If not, it is")
        print( "       probably a truncation that should use uintptr_t.")
        return 1

    if total < args.baseline:
        # A ratchet only holds against growth. Fewer sites is a fix, but the
        # baseline should come down with it or the next regression hides in
        # the slack.
        print(f"[OK] {total} sites — below baseline {args.baseline}; "
              f"lower it with `--update`")
        return 0

    print(f"[OK] {total} sites, unchanged from baseline")
    return 0


if __name__ == "__main__":
    sys.exit(main())
