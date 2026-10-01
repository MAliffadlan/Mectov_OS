#!/usr/bin/env python3
"""scripts/fs_test.py — M12 read-only filesystem gate (ISO9660 + ext2, Ring-3).

Two layers are checked, because they can fail independently:

  1. the kernel's pre-STI FS selftest — both mounts come up, the ISO root is
     walked, /boot/myos64.bin is opened and read, an ext2 file's bytes and an
     indirect-block file's hash are verified, and the error paths (ENOENT,
     EISDIR, past-EOF) return the exact codes;
  2. the same work driven from Ring-3 over the SYS64_FSOP syscall, typed into
     the shell by this test: `fs` (mount table), `ls /boot 0`, `hash64 ...`
     and `cat`, so the syscall layer, its request-struct ABI and its
     user-pointer handling are exercised from user mode, not just in-kernel.

What makes it a *gate*: the host recomputes every hash it can from its own
bytes — the ISO is walked with a small ISO9660 reader below, the ext2 files are
regenerated with mk_ext2disk's own content function — and the guest's numbers
must match. A wrong extent, a missed indirect block, a half-read sector or a
syscall that quietly returns zeros cannot survive that.

Exit 0 PASS, nonzero FAIL.
"""
import os
import re
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qmp import QMP
from mk_blkdisk import fnv1a64
from mk_ext2disk import file_bytes as ext2_bytes

ISO = "mectov64.iso"
DISK = "blkdisk.img"
EXT2 = "ext2test.img"
SERIAL = "serial_fstest.log"
SOCK = "/tmp/qmp_fstest"
ISO_LBA16 = 16


# ---- host-side ISO9660 reader (only what the test needs) -------------------

def iso_dir_records(iso, lba, size):
    """Yield (name, extent_lba, extent_size, is_dir) for one directory."""
    data = iso[lba * 2048:(lba + (size + 2047) // 2048) * 2048]
    off = 0
    while off < len(data):
        rlen = data[off]
        if rlen == 0:  # padding to the end of this block
            off = (off // 2048 + 1) * 2048
            continue
        rec = data[off:off + rlen]
        ext = struct.unpack_from("<I", rec, 2)[0]
        sz = struct.unpack_from("<I", rec, 10)[0]
        is_dir = bool(rec[25] & 0x02)
        nlen = rec[32]
        name = rec[33:33 + nlen].decode("latin-1")
        if rec[33] > 1:  # skip the "." / ".." pseudo entries
            if ";" in name:
                name = name.split(";")[0]
            name = name.rstrip(".").strip()
            yield name, ext, sz, is_dir
        off += rlen


def iso_find(iso, path):
    """Resolve an absolute ISO9660 path -> (extent_lba, size)."""
    root = iso[ISO_LBA16 * 2048 + 156:ISO_LBA16 * 2048 + 190]
    lba, size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
    for comp in [c for c in path.split("/") if c]:
        hit = None
        for name, ext, sz, is_dir in iso_dir_records(iso, lba, size):
            if name.upper() == comp.upper():
                hit = (ext, sz)
                break
        if hit is None:
            raise KeyError(path)
        lba, size = hit
    return lba, size


def iso_file_bytes(iso, path):
    lba, size = iso_find(iso, path)
    return iso[lba * 2048:lba * 2048 + size]


# ---- qemu + typing (same approach as kbd_test.py) -------------------------

def boot():
    cmd = ["qemu-system-x86_64", "-machine", "pc", "-cpu", "qemu64,+nx",
           "-m", "256", "-smp", "4", "-cdrom", ISO,
           "-drive", f"file={DISK},format=raw,if=ide,index=1,media=disk",
           "-drive", f"file={EXT2},format=raw,if=ide,index=3,media=disk",
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


def wait_for(needle, timeout, since=0, poll=1.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with open(SERIAL, errors="replace") as f:
                f.seek(since)
                if needle in f.read():
                    return True
        except OSError:
            pass
        time.sleep(poll)
    return False


# QEMU qcode names for the punctuation a path needs (a literal "/" is not a
# key name, it is silently dropped by sendkey — which then makes the shell echo
# mismatch look like a guest bug).
KEYNAME = {" ": "spc", "/": "slash", ".": "dot", "-": "minus"}


def type_line(q, line):
    """Closed-loop typing: wait for each key's echo before the next one, because
    a burst overruns the 1-byte 8042 buffer while the guest runs with IF=0."""
    mark = fsize(SERIAL)
    for k in line:
        q.sendkey(KEYNAME.get(k, k))
        want = "\n" if k == "ret" else k
        end = time.time() + 15
        found = False
        while time.time() < end:
            try:
                with open(SERIAL, errors="replace") as f:
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
    for path, hint in ((ISO, "make iso64"),
                       (DISK, "python3 scripts/mk_blkdisk.py"),
                       (EXT2, "python3 scripts/mk_ext2disk.py")):
        if not os.path.exists(path):
            print(f"fs_test: {path} missing ({hint})")
            return 1

    iso = open(ISO, "rb").read()
    want = {
        "/boot/myos64.bin": fnv1a64(iso_file_bytes(iso, "/boot/myos64.bin")),
        "/big.bin": fnv1a64(ext2_bytes("/big.bin")),
        "/sub/deep/leaf.bin": fnv1a64(ext2_bytes("/sub/deep/leaf.bin")),
    }

    qemu = boot()
    try:
        if not wait_for("mct> ", 200):
            print("fs_test FAIL: no shell prompt")
            return 1
        q = QMP(SOCK)
        # Everything the shell prints is *after* this offset: boot-time output
        # (the kernel's own FS selftest prints the same strings) must not be
        # able to satisfy a Ring-3 check.
        prompt_at = fsize(SERIAL)
        cmds = ["fs", "ls /boot 0", "hash64 /big.bin 1",
                "hash64 /boot/myos64.bin 0", "cat /hello.txt 1",
                "stat /nope.txt 1"]
        typed = all(type_line(q, list(c) + ["ret"]) for c in cmds)
        print("typing:", "ok" if typed else "MISS")
        q.close()
        time.sleep(25)  # let the shell drain the queue under TCG
        serial = open(SERIAL, errors="replace").read()
        tail = serial[prompt_at:]
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            qemu.kill()

    print("  host: /boot/myos64.bin fnv=0x%016X" % want["/boot/myos64.bin"])
    print("  host: /big.bin         fnv=0x%016X" % want["/big.bin"])
    for line in serial.splitlines():
        if line.startswith(("[K64] fs:", "FS ", "LS ", "LS-DONE", "HASH64",
                            "CAT ", "CAT-DONE", "STAT", "  ")):
            print("  " + line.rstrip())

    def guest_hash(path):
        m = re.search(r"HASH64 mnt=\d+ path=%s fnv=(0x[0-9A-F]{16})"
                      % re.escape(path), tail)
        return int(m.group(1), 16) if m else None

    iso_mount = re.search(r"fs: mount0 iso9660 blk=(\d+) root=(\d+) "
                          r"block_size=2048 label=\"ISOIMAGE", serial)
    ext_mount = re.search(r"fs: mount1 ext2 blk=(\d+) blocks=(\d+) "
                          r"block_size=(\d+) inodes=(\d+) label=\"MECTOV64\"",
                          serial)
    kern_iso = re.search(r"fs: iso root entries=(\d+) /boot/myos64.bin "
                         r"size=(\d+) fnv=(0x[0-9A-F]{16})", serial)
    kern_ext = re.search(r"fs: ext2 label=\"MECTOV64\" /hello.txt=\"hello from "
                         r"ext2\" /big.bin size=40960 fnv=(0x[0-9A-F]{16}) "
                         r"leaf fnv=(0x[0-9A-F]{16})", serial)
    ls_boot = re.search(r"LS-DONE n=(\d+)", tail)

    checks = [
        (iso_mount is not None, "kernel mounts ISO9660 from the boot CD"),
        (ext_mount is not None and int(ext_mount.group(2)) == 4096,
         "kernel mounts ext2 from the ATA disk (4096 blocks)"),
        (ext_mount is not None and ext_mount.group(3) == "1024",
         "ext2 block size from the superblock"),
        (kern_iso is not None and int(kern_iso.group(3), 16) == want["/boot/myos64.bin"],
         "kernel-side hash of /boot/myos64.bin matches the host"),
        (kern_iso is not None and int(kern_iso.group(2)) ==
         len(iso_file_bytes(iso, "/boot/myos64.bin")),
         "ISO file size matches the host's directory record"),
        (kern_ext is not None and int(kern_ext.group(1), 16) == want["/big.bin"],
         "kernel-side hash of ext2 /big.bin (indirect blocks) matches"),
        (kern_ext is not None and int(kern_ext.group(2), 16) ==
         want["/sub/deep/leaf.bin"],
         "kernel-side hash of /sub/deep/leaf.bin (3 nested levels) matches"),
        ("M12 FS SELFTEST OK" in serial, "kernel M12 selftest"),
        ("FAIL: fs" not in serial, "no fs FAIL"),
        (typed, "typed commands echoed"),
        ("FS-DONE mounts=2" in tail, "Ring-3 `fs` sees both mounts"),
        ("FS mnt=0 iso9660" in tail and "FS mnt=1 ext2" in tail,
         "Ring-3 mount table types"),
        (guest_hash("/big.bin") == want["/big.bin"],
         "Ring-3 hash64 /big.bin matches the host"),
        (guest_hash("/boot/myos64.bin") == want["/boot/myos64.bin"],
         "Ring-3 hash64 /boot/myos64.bin matches the host"),
        (ls_boot is not None and int(ls_boot.group(1)) >= 1,
         "Ring-3 `ls /boot` lists entries"),
        ("MYOS64.BIN" in tail, "ISO level-1 name is shown, `;1` stripped"),
        ("hello from ext2" in tail, "Ring-3 `cat /hello.txt 1` prints the file"),
        ("CAT-DONE bytes=16" in tail, "cat read exactly the file's length"),
        ("STAT rc=2" in tail and "STAT-DONE" in tail,
         "Ring-3 stat of a missing path returns ENOENT exactly"),
        ("FATAL" not in serial, "no-FATAL"),
    ]
    rc = 0
    for good, name in checks:
        print(f"  [{'PASS' if good else 'FAIL'}] {name}")
        rc = rc or (not good)
    return rc


if __name__ == "__main__":
    sys.exit(main())
