#!/usr/bin/env python3
"""scripts/vfs_cycle_test.py — native VFS parent-cycle boot gate (audit H6).

`vfs_load_unlocked()` range-checks every parent index but a range-checked
parent can still form a cycle. A crafted node table with `home.parent=user`
and `user.parent=home` made `vfs_clear_children()`'s ancestor walk spin
forever during boot. The fix breaks cycles at the trust boundary and bounds
every walk.

This suite patches a COPY of disk.img to that shape and runs the normal boot
test against it. The pre-fix kernel hangs before the login screen; the fixed
kernel logs in. The original disk.img is never modified.

Usage: python3 scripts/vfs_cycle_test.py [--timeout 120]
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

# fs_node_t is a packed 512-byte record (src/include/vfs.h):
#   name[32], type(4), parent(4), size(4), data_sector(4), in_use(4), ...
NAME_OFF = 0
PARENT_OFF = 36
IN_USE_OFF = 48
NODE_SIZE = 512
NODE_TABLE_SECTOR = 1

# The shipped image's node 1 is /home and node 2 is /home/user. Verify those
# identities before patching so a layout change fails loudly instead of
# producing a test that silently stops testing anything.
EXPECTED = {1: b"home", 2: b"user"}


def patch_cycle(disk_path):
    with open(disk_path, "r+b") as f:
        for idx, name in EXPECTED.items():
            f.seek((NODE_TABLE_SECTOR + idx) * NODE_SIZE)
            node = bytearray(f.read(NODE_SIZE))
            if len(node) != NODE_SIZE:
                raise RuntimeError(f"short node {idx} in {disk_path}")
            if bytes(node[NAME_OFF:NAME_OFF + len(name)]) != name:
                raise RuntimeError(
                    f"node {idx} is not {name!r} (got "
                    f"{bytes(node[NAME_OFF:NAME_OFF + 16])!r}) — image layout changed")
            if struct.unpack_from("<i", node, IN_USE_OFF)[0] != 1:
                raise RuntimeError(f"node {idx} ({name.decode()}) is not in_use")
            f.seek((NODE_TABLE_SECTOR + idx) * NODE_SIZE)
        # home.parent = user, user.parent = home
        for idx, parent in ((1, 2), (2, 1)):
            f.seek((NODE_TABLE_SECTOR + idx) * NODE_SIZE + PARENT_OFF)
            f.write(struct.pack("<i", parent))
        f.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=120,
                    help="boot_test.py login budget (the hang gate)")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    args = ap.parse_args()

    if not os.path.exists(args.disk):
        print(f"[FAIL] {args.disk} is missing (check.py builds it before suites)")
        return 1

    tmpdir = tempfile.mkdtemp(prefix="mectov_vfs_cycle_")
    patched = os.path.join(tmpdir, "disk_cycle.img")
    try:
        shutil.copyfile(args.disk, patched)

        # check.py creates disk.img zeroed; the native node table is written
        # by the kernel's first boot. Run that initialization on the COPY with
        # writes enabled, then patch the now-existing home/user nodes.
        prep = subprocess.run([
            sys.executable, os.path.join(os.path.dirname(__file__), "boot_test.py"),
            "--disk", patched,
            "--ext2", args.ext2,
            "--timeout", str(args.timeout),
            "--write-disk",
        ])
        if prep.returncode != 0:
            print("[FAIL] the initialization boot did not complete")
            return 1
        print("[OK] initialized the VFS node table on the disk copy")

        try:
            patch_cycle(patched)
        except RuntimeError as e:
            print(f"[FAIL] cannot build the cycle fixture: {e}")
            return 1
        print("[OK] patched home<->user parent cycle into the disk copy")

        r = subprocess.run([
            sys.executable, os.path.join(os.path.dirname(__file__), "boot_test.py"),
            "--disk", patched,
            "--ext2", args.ext2,
            "--timeout", str(args.timeout),
        ])
        if r.returncode != 0:
            print("[FAIL] the kernel did not boot/login with a VFS parent cycle "
                  "(the pre-fix hang signature)")
            return 1
        print("[OK] boot + login survived the VFS parent cycle")
        return 0
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
