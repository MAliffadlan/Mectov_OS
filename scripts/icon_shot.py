#!/usr/bin/env python3
"""One-shot desktop screendump: boots mectov.iso (like boot_test.py), logs in,
waits for the desktop to settle, then captures a QEMU screendump to
/tmp/mectov_desktop.ppm for visual inspection of the icon redesign."""
import os, signal, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_LOG = "/tmp/iconshot_serial.log"
MON_SOCK = "/tmp/iconshot_monitor.sock"
OUT_PPM = "/tmp/mectov_desktop.ppm"
ISO = os.path.join(ROOT, "mectov.iso")
DISK = "/tmp/disk_copy.img"   # copies: run.sh/QEMU may hold locks on the originals
EXT2 = "/tmp/ext2_copy.img"

# name-to-scancode map for sendkey (subset used to type "root\n")
SCAN = {}
for i, row in enumerate([
    "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"
]):
    for j, ch in enumerate(row):
        SCAN[ch] = f"{i+2}-{hex(0x02 + j)[2:]}" if False else None
# Simpler: use known sendkey names for login (matches boot_test.py LOGIN_KEYS style)
LOGIN_SEQ = ["spc", "r", "o", "o", "t", "ret"] if False else None

def mon(cmd):
    """Send one command to the QEMU monitor over a unix socket."""
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(0.2)
        s.close()
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")

def main():
    for p in (SERIAL_LOG, MON_SOCK, OUT_PPM):
        try: os.unlink(p)
        except OSError: pass
    open(SERIAL_LOG, "w").close()

    qemu = subprocess.Popen([
        "qemu-system-i386",
        "-cpu", "qemu32,+nx",
        "-vga", "std",
        "-cdrom", ISO,
        "-m", "128",
        "-smp", os.environ.get("MCTOV_SMP", "2"),
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        "-net", "none",
        "-snapshot",
        "-drive", f"file={DISK},format=raw,index=0,media=disk",
        "-drive", f"file={EXT2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        deadline = time.time() + 180
        while time.time() < deadline:
            if "[K] login" in open(SERIAL_LOG, errors="replace").read():
                break
            if qemu.poll() is not None:
                print(f"[FAIL] qemu exited early: {qemu.returncode}")
                return 1
            time.sleep(0.5)
        else:
            print("[FAIL] no login screen")
            return 1
        print("[OK] login screen")

        # Type credentials (same as boot_test.py LOGIN_KEYS — the lock screen
        # eats the first keypress, hence the leading space), then Enter.
        keys = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
        for k in keys:
            mon(f"sendkey {k}")
            time.sleep(0.15)

        deadline = time.time() + 120
        while time.time() < deadline:
            if "BOOTED KERNEL LOOP" in open(SERIAL_LOG, errors="replace").read():
                break
            if qemu.poll() is not None:
                print(f"[FAIL] qemu exited early: {qemu.returncode}")
                return 1
            time.sleep(0.5)
        else:
            print("[FAIL] login incomplete")
            return 1
        print("[OK] desktop up")

        time.sleep(8)  # let icons/taskbar/wallpaper settle

        mon(f"screendump {OUT_PPM}")
        time.sleep(1)
        if os.path.exists(OUT_PPM) and os.path.getsize(OUT_PPM) > 1000:
            print(f"[OK] screendump -> {OUT_PPM} ({os.path.getsize(OUT_PPM)} bytes)")
            return 0
        print("[FAIL] no screendump")
        return 1
    finally:
        qemu.kill()
        try: qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            qemu.send_signal(signal.SIGKILL)

if __name__ == "__main__":
    sys.exit(main())
