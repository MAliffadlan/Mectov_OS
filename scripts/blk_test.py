#!/usr/bin/env python3
"""scripts/blk_test.py — M11 block-layer gate (legacy ATA PIO + ATAPI).

The kernel selftest proves the driver's *own* invariants (IDENTIFY, both command
paths agree, a re-read is stable, the ISO's PVD structure). What it cannot do is
know what the bytes *should* be — the guest has no copy of the host's files.

So this test closes the loop: it boots the ISO with blkdisk.img attached, then
compares the FNV-1a 64 hashes the guest printed against the hashes it computes
from the same bytes on the host side. A driver bug that returns plausible-looking
data (wrong sector, half a sector, stale tail) cannot survive that.

Checks: both devices identified with the right sector size, ISO9660 PVD at LBA 16
via ATAPI READ(12), disk sector 0 magic, disk sector 40 via LBA28 and LBA48 with
identical hashes matching the host, re-read stable, no FATAL.
Exit 0 PASS, nonzero FAIL.
"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mk_blkdisk import SECTORS, expected_hash, fnv1a64, sector

ISO = "mectov64.iso"
DISK = "blkdisk.img"
SERIAL = "serial_blktest.log"
SOCK = "/tmp/qmp_blktest"
ISO_PVD_LBA = 16
DISK_LBA = 40


def boot():
    cmd = ["qemu-system-x86_64", "-machine", "pc", "-cpu", "qemu64,+nx",
           "-m", "256", "-smp", "4", "-cdrom", ISO,
           "-drive", f"file={DISK},format=raw,if=ide,index=1,media=disk",
           "-serial", f"file:{SERIAL}", "-no-reboot", "-display", "none",
           "-qmp", f"unix:{SOCK},server=on,wait=off"]
    for p in (SOCK, SERIAL):
        try:
            os.unlink(p)
        except OSError:
            pass
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)


def main():
    if not os.path.exists(ISO):
        print("blk_test: build the ISO first (make iso64)")
        return 1
    if not os.path.exists(DISK):
        print(f"blk_test: {DISK} missing (python3 scripts/mk_blkdisk.py)")
        return 1

    qemu = boot()
    try:
        # The gate lines appear during boot (pre-STI selftest), so wait for the
        # selftest marker rather than the shell prompt.
        end = time.time() + 180
        serial = ""
        while time.time() < end:
            try:
                serial = open(SERIAL, errors="replace").read()
            except OSError:
                serial = ""
            if "M11 BLK SELFTEST OK" in serial or "FAIL: blk" in serial:
                break
            time.sleep(1.0)
        time.sleep(2)
        serial = open(SERIAL, errors="replace").read()

        want_iso = fnv1a64(open(ISO, "rb").read()[ISO_PVD_LBA * 2048:
                                                  (ISO_PVD_LBA + 1) * 2048])
        want_disk = expected_hash(DISK_LBA)

        ata_all = re.search(r"blk: ide0 slave ATA\s+model=\"([^\"]*)\" sector="
                            r"(\d+) sectors=(\d+) lba48=([01])", serial)
        cd = re.search(r"blk: ide1 master ATAPI model=\"([^\"]*)\" sector=(\d+)",
                       serial)
        iso_line = re.search(r"blk: iso PVD lba16 type=1 id=CD001 vol=\"([^\"]*)\""
                             r" fnv=(0x[0-9A-F]{16})", serial)
        disk = re.search(r"blk: disk sector0 magic=ok sectors=(\d+) "
                         r"lba28 fnv=(0x[0-9A-F]{16}) lba48 fnv=(0x[0-9A-F]{16}) "
                         r"same=([01]) stable=([01])", serial)

        print(f"  host: iso lba{ISO_PVD_LBA} fnv=0x{want_iso:016X}")
        print(f"  host: disk lba{DISK_LBA} fnv=0x{want_disk:016X}")
        for line in serial.splitlines():
            if line.startswith("[K64] blk:"):
                print("  " + line.strip())

        checks = [
            (ata_all is not None and ata_all.group(1) != "", "ATA disk identified"),
            (ata_all is not None and ata_all.group(2) == "512",
             "ATA sector size 512"),
            (ata_all is not None and int(ata_all.group(3)) == SECTORS,
             f"ATA capacity {SECTORS} sectors"),
            (ata_all is not None and ata_all.group(4) == "1", "ATA LBA48 support"),
            (cd is not None, "ATAPI CD identified"),
            (cd is not None and cd.group(2) == "2048", "CD sector size 2048"),
            (iso_line is not None and iso_line.group(1).strip() == "ISOIMAGE",
             "ISO9660 PVD read via ATAPI READ(12)"),
            (iso_line is not None and int(iso_line.group(2), 16) == want_iso,
             "ISO sector 16 hash matches the host's"),
            (disk is not None and int(disk.group(1)) == SECTORS,
             "disk capacity from IDENTIFY"),
            (disk is not None and disk.group(4) == "1", "LBA28/LBA48 agree"),
            (disk is not None and disk.group(5) == "1", "re-read is stable"),
            (disk is not None and int(disk.group(2), 16) == want_disk,
             "LBA28 hash matches the host's"),
            (disk is not None and int(disk.group(3), 16) == want_disk,
             "LBA48 hash matches the host's"),
            ("M11 BLK SELFTEST OK" in serial, "kernel M11 selftest"),
            ("FATAL" not in serial, "no-FATAL"),
            ("FAIL: blk" not in serial, "no blk FAIL"),
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
