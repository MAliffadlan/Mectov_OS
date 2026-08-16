"""Measure Mectov OS per-frame render time (us) from serial [REND] markers.

Boots QEMU headless with a monitor socket, logs in, then drives phases
(idle, mouse moves, window drag, resize) and reports min/avg/max render
time per phase. Pass --kvm to run with KVM acceleration (real-timing).

This is a dev tool: it needs the temporary [REND] marker in kernel.c
full_redraw() to be re-added first, e.g.

    static int f = 0;
    if ((++f & 0x7) == 0) {
        write_serial_string("[REND] ");
        write_serial_hex(last_render_us);
        write_serial_string(" us\n");
    }

at the end of full_redraw(), then rebuild + regenerate mectov.iso.
"""
import os
import re
import subprocess
import sys
import time

import terminal_launch

SERIAL_LOG = "/tmp/mectov_render_serial.log"
MON_SOCK = "/tmp/mectov_render_monitor.sock"
DUMP = "/tmp/mectov_render_dump.ppm"
LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

REND_RE = re.compile(rb"\[REND\] 0x([0-9a-fA-F]+) us")


def mon_cmd(cmd):
    import socket
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(0.15)
        s.close()
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")


def samples_since(path, start_pos):
    """Return list of render-time us values logged after start_pos."""
    out = []
    try:
        with open(path, "rb") as f:
            f.seek(start_pos)
            data = f.read()
        for m in REND_RE.finditer(data):
            v = int(m.group(1), 16)
            if v < (1 << 31):  # skip negative wraps (timer rollover artifact)
                out.append(v)
    except OSError:
        pass
    return out


def stats(label, vals):
    if not vals:
        print(f"[{label}] no samples")
        return
    vals.sort()
    n = len(vals)
    avg = sum(vals) // n
    print(f"[{label}] n={n} min={vals[0]} avg={avg} max={vals[-1]} us")


def wait_for_in_file(path, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, "r", errors="replace") as f:
                if needle in f.read():
                    return True
        except (FileNotFoundError, OSError):
            pass
        time.sleep(1)
    return False


def main():
    for p in (SERIAL_LOG, MON_SOCK, DUMP):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    qemu_cmd = [
        "qemu-system-i386",
        "-cpu", "qemu32,+nx",
        "-vga", "std",
        "-cdrom", "mectov.iso",
        "-m", "128",
        "-smp", "4",
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        "-net", "none",
        "-drive", "file=disk.img,format=raw,index=0,media=disk",
        "-drive", "file=ext2.img,format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    if "--kvm" in sys.argv:
        qemu_cmd.insert(1, "-enable-kvm")
        qemu_cmd.insert(2, "-cpu")
        qemu_cmd.insert(3, "host")
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", 120):
            print("[FAIL] never reached login")
            return 1
        print("[OK] booted to login")
        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)
        time.sleep(3)
        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            mon_cmd("screendump " + DUMP)
            time.sleep(1)
            try:
                with open(SERIAL_LOG, "rb") as f:
                    data = f.read()
                for line in data.splitlines()[-15:]:
                    print("  |", line[:130].decode(errors="replace"))
            except OSError:
                pass
            return 1
        print("[OK] logged in, desktop loop running")
        time.sleep(2)

        # ---- Phase A: idle ----
        pos = os.path.getsize(SERIAL_LOG)
        time.sleep(6)
        stats("idle", samples_since(SERIAL_LOG, pos))

        # ---- Phase B: mouse moves over desktop (no button) ----
        pos = os.path.getsize(SERIAL_LOG)
        for i in range(40):
            dx = (i % 8) * 40 - 140
            dy = ((i // 8) % 4) * 60 - 90
            mon_cmd(f"mouse_move {dx} {dy}")
            time.sleep(0.05)
        time.sleep(2)
        stats("mousemove", samples_since(SERIAL_LOG, pos))

        # ---- Phase C: open terminal ----
        if not terminal_launch.launch_terminal(mon_cmd, SERIAL_LOG, DUMP):
            print("[FAIL] terminal did not launch")
            return 1
        time.sleep(2)

        # ---- Phase D: drag the terminal window ----
        pos = os.path.getsize(SERIAL_LOG)
        mon_cmd("mouse_move 300 176")  # over the terminal body (tests use this spot)
        time.sleep(0.4)
        mon_cmd("mouse_button 1")       # press on titlebar-ish / body
        time.sleep(0.2)
        for i in range(30):
            mon_cmd("mouse_move 8 5")   # hold and drag right-down
            time.sleep(0.05)
        mon_cmd("mouse_button 0")
        time.sleep(2)
        stats("drag", samples_since(SERIAL_LOG, pos))

        # ---- Phase E: resize drag (bottom-right corner) ----
        pos = os.path.getsize(SERIAL_LOG)
        mon_cmd("mouse_button 1")
        time.sleep(0.2)
        for i in range(30):
            mon_cmd("mouse_move -6 -4")
            time.sleep(0.05)
        mon_cmd("mouse_button 0")
        time.sleep(2)
        stats("resize", samples_since(SERIAL_LOG, pos))

        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
