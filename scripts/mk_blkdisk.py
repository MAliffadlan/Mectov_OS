#!/usr/bin/env python3
"""scripts/mk_blkdisk.py — deterministic legacy-ATA test image (M11).

Builds a tiny raw disk whose every byte is a pure function of its sector index,
so the guest driver and this host script can agree on the expected contents
without shipping any fixture blobs. Sector 0 carries a magic + parameters the
kernel checks structurally; any sector can be hashed and compared.

Usage:
    python3 scripts/mk_blkdisk.py [path]        # default: blkdisk.img
    python3 scripts/mk_blkdisk.py --hash 40     # FNV-1a 64 of one sector
"""
import os
import struct
import sys

SECTOR = 512
SECTORS = 128  # 64 KiB: enough to read a mid-disk LBA and still be instant
MAGIC = b"MECTOV-BLK-64\x00\x00\x00"
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1


def fnv1a64(data):
    h = FNV_OFFSET
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK64
    return h


def sector(s):
    """Deterministic bytes for sector `s` (matches the kernel's expectation of
    nothing at all except sector 0's magic — the hash gate is host-side)."""
    if s == 0:
        head = MAGIC + struct.pack("<II", SECTOR, SECTORS)
        body = bytes(((i * 29 + 7) & 0xFF) for i in range(len(head), SECTOR))
        return head + body
    return bytes(((s * 17 + i * 31 + ((s * i) >> 3)) & 0xFF)
                 for i in range(SECTOR))


def build(path):
    with open(path, "wb") as f:
        for s in range(SECTORS):
            f.write(sector(s))
    return path


def expected_hash(s):
    return fnv1a64(sector(s))


def main():
    args = sys.argv[1:]
    if args and args[0] == "--hash":
        which = int(args[1], 0)
        print(f"0x{expected_hash(which):016X}")
        return 0
    path = args[0] if args else "blkdisk.img"
    # Idempotent: only rewrite when missing or wrong size/content hash.
    if os.path.exists(path) and os.path.getsize(path) == SECTOR * SECTORS:
        with open(path, "rb") as f:
            if fnv1a64(f.read()) == fnv1a64(b"".join(sector(s)
                                                     for s in range(SECTORS))):
                print(f"[=] {path} up to date ({SECTORS} sectors)")
                return 0
    build(path)
    print(f"[+] {path}: {SECTORS} x {SECTOR}B, sector40 hash "
          f"0x{expected_hash(40):016X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
