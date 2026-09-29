#!/usr/bin/env python3
"""
scripts/virtiogpu_test.py — virtio-gpu transport + data path, end to end (v38.115).

Why this suite exists
---------------------
v38.115 adds the kernel's first MODERN virtio-pci driver: `virtio-gpu-pci`
(1AF4:1050) has no legacy interface at all (measured: `info pci` shows no I/O
BAR0 even with `disable-modern=on`, where virtio-blk-pci in the same run does
appear as 1AF4:1001 with BAR0 I/O), so the driver must walk the device's vendor
capabilities for the common-config/notify/ISR/device-config regions, negotiate
the 64-bit feature word and notify by a memory write. Asserting "the driver
loaded" is not enough for any of that: every one of those steps has a silent
failure mode that ends in a device that answers nothing.

So this suite checks the two halves separately, and each half is evidence for a
different claim:

  1. THE TRANSPORT IS REAL — the boot log must show the four regions the
     capability walk found and mapped (with `notify_mul`, which only exists in
     a modern notification capability), the device's offered feature words, the
     negotiated queue size, and the device's own report of scanout 0. A driver
     that guessed BAR numbers or skipped the handshake would fail here, because
     the numbers come from the device, not from the driver.

  2. THE DATA PATH IS REAL, IN BOTH DIRECTIONS, BYTE FOR BYTE — at init the
     driver builds a 128x128 test resource whose pattern is a magenta marker
     plus a red/blue checkerboard (16x16 blocks), hands it to the device
     (create 2D -> attach backing -> transfer to host) and puts it on scanout
     0. The suite:
       * recomputes the pattern FNV-1a checksum that the driver logged, from its
         OWN copy of the pattern definition — so the log's `ck=` proves what the
         driver handed the device;
       * asks QEMU's monitor for a `screendump` OF THAT DEVICE (head 0 of
         `vgpu0`, not the machine's -vga console) and compares every one of the
         16384 pixels against the same pattern. That is the host reading the
         guest's memory through its own resource and scanning it out: guest
         memory -> resource -> scanout -> pixels.
     A transfer that silently did nothing, a resource attached to the wrong
     buffer, or a scanout pointing at another resource all fail as pixels, which
     is the only place they would show up.

  3. THE SHELL CAN SEE IT — `gpustat` prints the same state in the window and
     mirrors it to serial as one `[GPU] gpustat:` line; the suite logs in, runs
     the command and requires that line to repeat the very checksum the boot
     self-test logged. So the command reads the live driver state rather than a
     constant.

  4. NO PANIC, and the 2D device is honest about 3D: `virtio-gpu-pci` offers no
     virgl feature, so the driver must report `readback: 3d=skip no-virgl-feature`
     (with `--gl`, on a device that does offer it, the suite instead requires the
     capset query and prints what the host answered about 3D readback).

One measured caveat decides how `--gl` verifies anything at all: QEMU cannot
screendump a virgl console (`Error: no surface` — the scanout is a host GL
texture and egl-headless keeps no readable surface for it), while the same
command happily returns pixels for the 2D device. So on the GL path the driver's
own 3D round trip (transfer to host, poison the guest buffer, transfer back, and
the bytes must be the pattern again) is the data-path proof, and the suite
accepts that in place of the screenshot — but only when the round trip really
did return the pattern.

The device is attached by this script — nothing else in the tree attaches one,
so the suite owns its own QEMU command line (same shape as virtio_test.py's) and
needs no MECTOV_Q3 variant: it runs on the default ISO.

    python3 scripts/virtiogpu_test.py [--timeout 360] [--iso mectov.iso] [--gl]

`--gl` switches to `virtio-gpu-gl-pci` under `-display egl-headless` (a real GL
context with no window) and additionally requires virgl + a capset; it is
opt-in because a CI host may have no EGL/GL at all. `--require-3d` goes one
step further and fails when the host refuses the 3D readback round trip.
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import time

import terminal_launch

SERIAL_LOG = "/tmp/mectov_virtiogpu_serial.log"
MON_SOCK = "/tmp/mectov_virtiogpu_monitor.sock"
SHOT = "/tmp/mectov_virtiogpu_scanout.ppm"
CURSOR_PPM = "/tmp/mectov_virtiogpu_cursor.ppm"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

DEV_ID = "vgpu0"
W, H = 128, 128
TEST_ID = 1

BOOT_MARKER = "[K] login"
PANIC_MARKER = "[PANIC]"
READY_MARKER = "[VIRTIO-GPU] ready"
SELFTEST_MARKER = "[VIRTIO-GPU] selftest: "
READBACK_MARKER = "[VIRTIO-GPU] readback: "
GPUSTAT_MARKER = "[GPU] gpustat: "

SELFTEST_RE = re.compile(
    r"\[VIRTIO-GPU\] selftest: res=0x([0-9A-F]+) dim=(\d+)x(\d+) fmt=(\d+) "
    r"attach=ok transfer=ok scanout=ok flush=ok ck=0x([0-9A-F]+)")
GPUSTAT_RE = re.compile(
    r"\[GPU\] gpustat: bus=(\d+) slot=(\d+) virgl=(\d+) edid=(\d+) selftest=(\w+) "
    r"ck=0x([0-9A-F]+) scanout=(\d+)x(\d+) qsize=(\d+) capsets=(\d+) readback=(\w+)")


# ---- the pattern, defined once in Python exactly as the driver defines it ----
# 16x16 blocks: magenta marker at the origin, then a red/blue checkerboard.
# Stored as B8G8R8X8 (byte order B,G,R,X), which is what the driver writes and
# what the resource's format declares.
def expected_pixel(x, y):
    if x < 16 and y < 16:
        return (255, 0, 255)                     # magenta
    if ((x >> 4) + (y >> 4)) & 1:
        return (255, 0, 0)                       # red
    return (0, 0, 255)                           # blue


def pattern_bytes():
    buf = bytearray()
    for y in range(H):
        for x in range(W):
            r, g, b = expected_pixel(x, y)
            buf += bytes((b, g, r, 0))
    return bytes(buf)


def fnv1a(data):
    h = 2166136261
    for byte in data:
        h = ((h ^ byte) * 16777619) & 0xFFFFFFFF
    return h


def wait_for_in_file(path, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, "r", errors="replace") as f:
                if needle in f.read():
                    return True
        except (FileNotFoundError, OSError):
            pass
        time.sleep(0.5)
    return False


def read_log():
    try:
        with open(SERIAL_LOG, "r", errors="replace") as f:
            return f.read()
    except (FileNotFoundError, OSError):
        return ""


def mon_cmd(cmd):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(0.15)
        s.close()
        return True
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")
        return False


def send_keys(keys):
    names = {" ": "spc", "/": "slash", ".": "dot", "-": "minus"}
    for k in keys:
        if k.isupper():
            mon_cmd("sendkey shift-" + k.lower())
        else:
            mon_cmd("sendkey " + names.get(k, k))
        time.sleep(0.12)


def scrub_and_type(keys, needle, what, wait=25):
    """Clear the shell line, type `keys`, and wait for the serial marker."""
    for _ in range(3):
        for _ in range(48):
            mon_cmd("sendkey backspace")
        send_keys(keys)
        if wait_for_in_file(SERIAL_LOG, needle, wait):
            return True
        time.sleep(1.0)
    print(f"[FAIL] {what} (never saw '{needle}')")
    return False


def load_ppm(path):
    """Return (width, height, pixels) of a binary PPM, header parsed leniently."""
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m:
        raise ValueError("not a binary PPM")
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, data[m.end():]


def ensure_images(disk, ext2, fat32):
    """Create only what is missing.

    Deliberately NOT the virtio_test.py behaviour of rebuilding every image:
    ext2.img on a developer's box carries staged retail game data (q3dm1's
    assets are megabytes) and this suite has no business wiping that to check a
    GPU driver. Nothing here reads the volumes' contents, so an existing image
    is left exactly as it is.
    """
    steps = []
    if not os.path.exists(disk):
        steps.append(["dd", "if=/dev/zero", f"of={disk}", "bs=512",
                      "count=4096", "status=none"])
    if not os.path.exists(ext2):
        steps.append(["dd", "if=/dev/zero", f"of={ext2}", "bs=1M",
                      "count=16", "status=none"])
        steps.append(["mkfs.ext2", "-F", ext2])
    if not os.path.exists(fat32):
        steps.append(["dd", "if=/dev/zero", f"of={fat32}", "bs=1M",
                      "count=16", "status=none"])
        steps.append(["mkfs.fat", "-F", "32", "-S", "512", fat32])
    for s in steps:
        r = subprocess.run(s, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode != 0:
            print(f"[FAIL] image step failed: {' '.join(s)}")
            return 1
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=360)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--fat32", default="fat32.img")
    ap.add_argument("--gl", action="store_true",
                    help="attach virtio-gpu-gl-pci under -display egl-headless")
    ap.add_argument("--require-3d", action="store_true",
                    help="fail when the host refuses the 3D readback round trip")
    args = ap.parse_args()

    if ensure_images(args.disk, args.ext2, args.fat32) != 0:
        return 1

    for p in (SERIAL_LOG, MON_SOCK, SHOT):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    if args.gl:
        gpu_args = ["-device", f"virtio-gpu-gl-pci,id={DEV_ID}",
                    "-display", "egl-headless"]
        display_args = []
    else:
        gpu_args = ["-device", f"virtio-gpu-pci,id={DEV_ID}"]
        display_args = ["-display", "none"]

    qemu_cmd = [
        "qemu-system-i386",
        "-cpu", "qemu32,+nx",
        "-vga", "std",
        "-cdrom", args.iso,
        "-m", "128",
        "-smp", "4",
        *display_args,
        "-serial", f"file:{SERIAL_LOG}",
        "-net", "none",
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-drive", f"file={args.fat32},format=raw,index=3,media=disk",
        *gpu_args,
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    print(f"[i] attaching {'virtio-gpu-gl-pci' if args.gl else 'virtio-gpu-pci'} "
          f"as id={DEV_ID}")
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    failures = []
    try:
        if not wait_for_in_file(SERIAL_LOG, BOOT_MARKER, args.timeout):
            print("[FAIL] kernel never reached the login screen")
            return 1
        print("[OK] booted to the login screen")

        # ---- 1. the modern transport came up -----------------------------
        if not wait_for_in_file(SERIAL_LOG, "pci 1af4:1050 bus=", 10):
            print("[FAIL] the driver did not claim a virtio-gpu device")
            return 1
        print("[OK] driver claimed 1af4:1050")
        if not wait_for_in_file(SERIAL_LOG, "regions common=0x", 10):
            print("[FAIL] no region line: the capability walk did not finish")
            return 1
        regions = re.search(r"regions common=0x([0-9A-F]+) notify=0x([0-9A-F]+) "
                            r"isr=0x([0-9A-F]+) devcfg=0x([0-9A-F]+) notify_mul=(\d+)",
                            read_log())
        if not regions:
            print("[FAIL] region line malformed")
            return 1
        common, notify, isr, devcfg, mul = regions.groups()
        for name, addr in (("common", common), ("notify", notify),
                           ("isr", isr), ("devcfg", devcfg)):
            if int(addr, 16) == 0 or int(addr, 16) >= 0x100000000:
                failures.append(f"{name} region address {addr} is not addressable")
        if int(mul) == 0:
            failures.append("notify_off_multiplier is 0")
        print(f"[OK] four regions mapped: common={common} notify={notify} "
              f"isr={isr} devcfg={devcfg} notify_mul={mul}")

        if not wait_for_in_file(SERIAL_LOG, "features dev_lo=", 10):
            print("[FAIL] no feature line: negotiation did not reach the log")
            return 1
        feats = re.search(r"features dev_lo=0x([0-9A-F]+) dev_hi=0x([0-9A-F]+) "
                          r"virgl=(\d+) edid=(\d+) qsize=(\d+) notify_off=(\d+)",
                          read_log())
        if not feats:
            print("[FAIL] feature line malformed")
            return 1
        dev_hi = int(feats.group(2), 16)
        virgl, edid, qsize, notify_off = (int(feats.group(i)) for i in (3, 4, 5, 6))
        if dev_hi & 1 == 0:
            failures.append("device did not offer VIRTIO_F_VERSION_1")
        if qsize < 8:
            failures.append(f"negotiated queue size too small ({qsize})")
        print(f"[OK] features negotiated: dev_hi=0x{dev_hi:08X} virgl={virgl} "
              f"edid={edid} qsize={qsize} notify_off={notify_off}")
        if args.gl:
            if virgl != 1:
                failures.append("--gl run but the device offered no virgl feature")
            if not wait_for_in_file(SERIAL_LOG, "[VIRTIO-GPU] capset0 id=", 10):
                failures.append("--gl run but no capset was reported")
            else:
                cap = re.search(r"capset0 id=(\d+) ver=(\d+) size=0x([0-9A-F]+)",
                                read_log())
                print(f"[OK] capset0 id={cap.group(1)} ver={cap.group(2)} "
                      f"size=0x{cap.group(3)}")
        else:
            if virgl != 0:
                failures.append("the plain 2D device unexpectedly offered virgl")

        # ---- 2. the data path, guest side --------------------------------
        if not wait_for_in_file(SERIAL_LOG, SELFTEST_MARKER, 20):
            print("[FAIL] the driver's self-test never logged a result")
            return 1
        st = SELFTEST_RE.search(read_log())
        if not st:
            print("[FAIL] self-test not acknowledged: "
                  f"{[l for l in read_log().splitlines() if 'selftest' in l]}")
            return 1
        res_id, dim_w, dim_h, fmt, ck = (int(st.group(1), 16), int(st.group(2)),
                                        int(st.group(3)), int(st.group(4)),
                                        int(st.group(5), 16))
        want_ck = fnv1a(pattern_bytes())
        if (res_id, dim_w, dim_h) != (TEST_ID, W, H):
            failures.append(f"unexpected test resource: id={res_id} {dim_w}x{dim_h}")
        if ck != want_ck:
            failures.append(f"pattern checksum mismatch: driver 0x{ck:08X} "
                            f"vs suite 0x{want_ck:08X}")
        else:
            print(f"[OK] driver built the {W}x{H} pattern and logged its FNV-1a "
                  f"ck=0x{ck:08X} (suite recomputed the same bytes)")
        if not wait_for_in_file(SERIAL_LOG, READY_MARKER, 10):
            print("[FAIL] driver never reported ready")
            return 1

        # 2b. the 3D path is reported honestly
        readback_ok = False
        if not wait_for_in_file(SERIAL_LOG, READBACK_MARKER, 10):
            failures.append("no readback line: the driver did not report on 3D")
        else:
            rb = [l for l in read_log().splitlines() if READBACK_MARKER in l][0]
            print(f"[i] {rb.split('] ', 1)[1]}")
            readback_ok = "3d=ok" in rb
            if not args.gl and "3d=skip" not in rb:
                failures.append("2D-only device should have skipped the 3D probe")
            if args.gl and args.require_3d and not readback_ok:
                failures.append("--require-3d but the host refused the round trip")

        # ---- 2c. the data path, host side: pixels -------------------------
        # QEMU's monitor screendump takes the DEVICE and head, so this reads the
        # virtio-gpu's own console, not the machine's -vga framebuffer.
        #
        # On a GL device this is unavailable, and measured so: QEMU answers
        # `Error: no surface` for a virgl console's screendump (the scanout is a
        # host GL texture, and egl-headless keeps no readable surface for it) even
        # though the same command returns pixels for the 2D device. That is why
        # the driver's 3D round trip matters: on a virgl host, transfer-to-host +
        # transfer-from-host is the only way to read the GPU's own output back.
        # So in --gl mode the byte-exact round trip IS the data-path proof, and a
        # missing screenshot is reported, not failed — but only when the round
        # trip actually produced the pattern back.
        shot_problem = None
        if not mon_cmd(f"screendump {SHOT} {DEV_ID} 0"):
            shot_problem = "could not ask for a screendump"
            pw = ph = 0
            pixels = b""
        else:
            time.sleep(1.0)
            try:
                pw, ph, pixels = load_ppm(SHOT)
            except (OSError, ValueError) as e:
                shot_problem = f"screendump unavailable: {e}"
                pw = ph = 0
                pixels = b""
        if shot_problem is None:
            if pw >= W and ph >= H:
                bad = 0
                first_bad = None
                for y in range(H):
                    row = y * pw * 3
                    for x in range(W):
                        off = row + x * 3
                        got = (pixels[off], pixels[off + 1], pixels[off + 2])
                        if got != expected_pixel(x, y):
                            bad += 1
                            if first_bad is None:
                                first_bad = (x, y, got, expected_pixel(x, y))
                if bad:
                    failures.append(
                        f"screendump of {DEV_ID} differs from the pattern in "
                        f"{bad}/{W * H} pixels, first at {first_bad}")
                else:
                    print(f"[OK] screendump {os.path.basename(SHOT)} ({pw}x{ph}) "
                          f"matches the pattern in all {W * H} pixels — guest "
                          f"memory -> resource -> scanout -> pixels")
            else:
                failures.append(f"screendump too small: {pw}x{ph}")
        elif readback_ok:
            print(f"[i] {shot_problem} — for a GL console QEMU keeps no readable "
                  f"surface; the byte-exact 3D round trip above is the data-path "
                  f"proof instead")
        else:
            failures.append(f"{shot_problem} and no 3D round trip either: "
                            f"nothing verified the data path")

        # ---- 3. the command reads the live state --------------------------
        send_keys(LOGIN_KEYS)
        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            failures.append("login did not complete")
        else:
            print("[OK] logged in")
            time.sleep(1.5)
            if not terminal_launch.launch_terminal(mon_cmd, SERIAL_LOG, CURSOR_PPM):
                failures.append("the Terminal never became ready")
            else:
                time.sleep(1.0)
                mon_cmd("mouse_move 300 176")
                time.sleep(0.1)
                mon_cmd("mouse_button 1")
                time.sleep(0.1)
                mon_cmd("mouse_button 0")
                time.sleep(0.5)
                if scrub_and_type(list("gpustat") + ["ret"], GPUSTAT_MARKER,
                                  "gpustat"):
                    gp = GPUSTAT_RE.search(read_log())
                    if not gp:
                        failures.append("gpustat line malformed")
                    else:
                        g_ck = int(gp.group(6), 16)
                        print(f"[OK] gpustat ran: bus={gp.group(1)} "
                              f"slot={gp.group(2)} virgl={gp.group(3)} "
                              f"selftest={gp.group(5)} ck=0x{g_ck:08X} "
                              f"scanout={gp.group(7)}x{gp.group(8)} "
                              f"qsize={gp.group(9)} readback={gp.group(11)}")
                        if gp.group(5) != "ok":
                            failures.append("gpustat reports a failed self-test")
                        if g_ck != ck:
                            failures.append(
                                f"gpustat checksum 0x{g_ck:08X} is not the "
                                f"self-test's 0x{ck:08X}: the command is not "
                                f"reading live driver state")

        # ---- 4. still alive ----------------------------------------------
        if qemu.poll() is not None:
            failures.append(f"QEMU exited early with code {qemu.returncode}")
        if PANIC_MARKER in read_log():
            failures.append("kernel panicked during the run")
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass

    if failures:
        print("[FAIL] virtio-gpu:")
        for f in failures:
            print("       - " + f)
        return 1
    print("[PASS] virtio-gpu: modern transport up, 2D resource round trip "
          "byte-exact through the device's own scanout, gpustat live, no panic")
    return 0


if __name__ == "__main__":
    sys.exit(main())
