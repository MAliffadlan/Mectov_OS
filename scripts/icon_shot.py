#!/usr/bin/env python3
"""One-shot desktop screendump for eyeballing the icon artwork.

Boots mectov.iso (like boot_test.py), logs in, waits for the desktop to
settle, then captures a QEMU screendump to /tmp/mectov_desktop.ppm.

    python3 scripts/icon_shot.py [--out /tmp/shot.ppm] [--disk ...] [--ext2 ...]

By default it boots the /tmp disk copies (created on demand from disk.img /
ext2.img) so it can run alongside an interactive ./run.sh QEMU instance, which
holds block-level locks on the originals.
"""
import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from desktop_capture import capture, ROOT

OUT_PPM = "/tmp/mectov_desktop.ppm"
DISK = "/tmp/disk_copy.img"
EXT2 = "/tmp/ext2_copy.img"


def ensure_copy(src, dst):
    if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src):
        shutil.copyfile(src, dst)
    return dst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=OUT_PPM)
    ap.add_argument("--iso", default=os.path.join(ROOT, "mectov.iso"))
    ap.add_argument("--disk", default=None)
    ap.add_argument("--ext2", default=None)
    ap.add_argument("--smp", default=None)
    args = ap.parse_args()

    disk = args.disk or ensure_copy(os.path.join(ROOT, "disk.img"), DISK)
    ext2 = args.ext2 or ensure_copy(os.path.join(ROOT, "ext2.img"), EXT2)

    ok = capture(args.out, iso=args.iso, disk=disk, ext2=ext2, smp=args.smp)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
