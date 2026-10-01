#!/usr/bin/env python3
"""scripts/mk_ext2disk.py — deterministic ext2 test image (M12).

Builds ext2test.img with a handful of files whose contents are pure functions of
the path and offset, so the guest and this host script agree without shipping a
fixture blob. Uses only e2fsprogs (mkfs.ext2 + debugfs), so no root and no loop
mount are needed.

Layout (1 KiB blocks, 4 MiB, label MECTOV64):
    /hello.txt         16 B   "hello from ext2\\n"
    /big.bin           40 KiB deterministic pattern -> forces indirect blocks
    /sub/nested.txt    20 B
    /sub/deep/leaf.bin 3 KiB

Usage:
    python3 scripts/mk_ext2disk.py [path]        # default: ext2test.img
    python3 scripts/mk_ext2disk.py --hash big.bin 40
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

SIZE_MB = 4
BLOCK = 1024
LABEL = "MECTOV64"
MAGIC = 0xEF53

STREAM_FILES = ["/big.bin", "/sub/deep/leaf.bin"]
TEXT_FILES = {"/hello.txt": b"hello from ext2\n", "/sub/nested.txt": b"nested file\n"}
BIG_SIZE = 40 * 1024
LEAF_SIZE = 3 * 1024
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1


def fnv1a64(data):
    h = FNV_OFFSET
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK64
    return h


def content(path, size):
    """Deterministic bytes for a stream file: byte i is a function of the path
    hash and i, so the host can reproduce it without reading the image."""
    seed = fnv1a64(path.encode()) & 0xFFFF
    return bytes(((seed + i * 31 + ((seed * i) >> 5)) & 0xFF)
                 for i in range(size))


def file_bytes(path):
    if path in TEXT_FILES:
        return TEXT_FILES[path]
    if path == "/big.bin":
        return content(path, BIG_SIZE)
    if path == "/sub/deep/leaf.bin":
        return content(path, LEAF_SIZE)
    raise KeyError(path)


def run(cmd, **kw):
    p = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if p.returncode != 0:
        print(f"mk_ext2disk: {' '.join(cmd)} failed: {p.stderr.strip()}",
              file=sys.stderr)
        sys.exit(1)


def build(path):
    # Absolute: the debugfs calls below run with cwd=tmp, and a relative image
    # path would silently create a second (empty) file there.
    path = os.path.abspath(path)
    run(["mkfs.ext2", "-q", "-F", "-t", "ext2", "-b", str(BLOCK), "-I", "128",
         "-L", LABEL, path, str(SIZE_MB * 1024)])
    tmp = tempfile.mkdtemp(prefix="mkext2-")
    try:
        cmds = ["mkdir /sub", "mkdir /sub/deep"]
        for name, data in TEXT_FILES.items():
            src = os.path.join(tmp, name.replace("/", "_"))
            with open(src, "wb") as f:
                f.write(data)
            cmds.append(f"write {src} {name}")
        for name in STREAM_FILES:
            src = os.path.join(tmp, name.replace("/", "_"))
            with open(src, "wb") as f:
                f.write(file_bytes(name))
            cmds.append(f"write {src} {name}")
        for c in cmds:
            run(["debugfs", "-w", "-R", c, path], cwd=tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return path


def verify(path):
    with open(path, "rb") as f:
        f.seek(1024)  # superblock
        sb = f.read(1024)
    magic = struct.unpack_from("<H", sb, 56)[0]
    if magic != MAGIC:
        return f"superblock magic 0x{magic:04X} != 0x{MAGIC:04X}"
    return None


def main():
    args = sys.argv[1:]
    if args and args[0] == "--hash":
        name = "/" + args[1].lstrip("/")
        want = int(args[2]) if len(args) > 2 else len(file_bytes(name))
        print(f"0x{fnv1a64(file_bytes(name)[:want]):016X}")
        return 0
    path = args[0] if args else "ext2test.img"
    if shutil.which("mkfs.ext2") is None or shutil.which("debugfs") is None:
        print("mk_ext2disk: needs e2fsprogs (mkfs.ext2 + debugfs)")
        return 1
    build(path)
    bad = verify(path)
    if bad:
        print(f"[-] {path}: {bad}")
        return 1
    print(f"[+] {path}: ext2 {SIZE_MB}MiB, label={LABEL}, "
          f"/big.bin hash 0x{fnv1a64(file_bytes('/big.bin')):016X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
