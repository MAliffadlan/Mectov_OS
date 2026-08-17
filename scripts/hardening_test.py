#!/usr/bin/env python3
# hardening_test.py — CI test for v38.52 kernel hardening.
#
# Verifies, end to end:
#   1. The kernel boots with the CSPRNG initialized ([K] entropy in the log).
#   2. `passwd mectov123 hunter2` switches /etc/passwd to the salted SHA-256
#      format: the NEW password unlocks the lock screen, the OLD one is
#      rejected (a plaintext file would still accept the old password).
#   3. apps/hardening_test.mct (run from the shell) proves from Ring 3 that
#      SYS_GETRANDOM succeeds with fresh non-zero bytes, /dev/random reads
#      differ across reads, and /etc/passwd is "<16-hex>:<64-hex>" with no
#      plaintext password in it — "ALL TESTS PASSED" on serial.
#   4. Zero [PANIC] on the whole run.
#
# Uses TEMP COPIES of disk.img/ext2.img (with -snapshot OFF on the copies) so
# the passwd change persists for the host-side/file checks without ever
# touching the developer's real images.
import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

SERIAL_LOG = "/tmp/mectov_hardening_serial.log"
MON_SOCK = "/tmp/mectov_hardening_monitor.sock"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
# From the locked gate: SPACE opens the panel (consumed), then the password.
NEW_PW_KEYS = ["spc", "h", "u", "n", "t", "e", "r", "2", "ret"]
OLD_PW_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
# From an ALREADY-OPEN panel (wrong-password shake): no leading SPACE, it
# would type a literal space into the field.
PANEL_PW_KEYS = ["h", "u", "n", "t", "e", "r", "2", "ret"]


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


def mon_cmd(cmd, wait=0.15):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(wait)
        s.close()
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")


def screendump(path):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    mon_cmd("screendump " + path)
    time.sleep(0.3)
    return os.path.exists(path)


def load_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6 header: P6\n<w> <h>\n255\n
    parts = data.split(b"\n", 3)
    dims = parts[1].split()
    w, h = int(dims[0]), int(dims[1])
    px = parts[3]
    return w, h, px


def px_at(px, w, x, y):
    o = (y * w + x) * 3
    return px[o], px[o + 1], px[o + 2]


def is_taskbar(px, w, h):
    # Taskbar is a full-width black strip at the bottom; wallpaper there is
    # colored, so a dark row at y = h - 10 with some amber START text works.
    dark = 0
    for x in range(200, 800, 10):
        r, g, b = px_at(px, w, x, h - 10)
        if r < 40 and g < 40 and b < 40:
            dark += 1
    return dark >= 40


def clock_amber(px, w, h):
    for y in range(h - 165, h - 105, 4):
        for x in range(50, 300, 4):
            r, g, b = px_at(px, w, x, y)
            if r > 160 and g > 120 and b < 130:
                return True
    return False


def is_locked(px, w, h):
    return (not is_taskbar(px, w, h)) and clock_amber(px, w, h)


def wait_screen(pred, dump_path, tries, settle=0.5):
    for _ in range(tries):
        time.sleep(settle)
        if not screendump(dump_path):
            continue
        w, h, px = load_ppm(dump_path)
        if pred(w, h, px):
            return (w, h, px)
    return None


def type_line(keys):
    # QEMU sendkey wants scancode NAMES: ' ' -> spc, '/' -> slash, '.' -> dot,
    # '_' -> shift-minus (a raw '_' is an invalid name and is silently
    # dropped).
    names = {" ": "spc", "/": "slash", ".": "dot", "_": "shift-minus"}
    for k in keys:
        mon_cmd("sendkey " + names.get(k, k))
        time.sleep(0.12)


def run_app_cmd(name, marker, wait):
    """Type `run /apps/<name>.mct`, clearing the line and retrying until the
    marker appears in the serial log."""
    for _ in range(3):
        for _ in range(40):
            mon_cmd("sendkey backspace")
        type_line(["r", "u", "n", " ", "/", "a", "p", "p", "s", "/"] +
                  list(name) + [".", "m", "c", "t", "ret"])
        if wait_for_in_file(SERIAL_LOG, marker, wait):
            return True
        time.sleep(1.0)
    return False


def send_keys(keys):
    for k in keys:
        mon_cmd("sendkey " + k)
        time.sleep(0.12)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=540)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    # Temp copies of the drive images: the passwd change must persist (no
    # -snapshot) but must never touch the user's real images.
    tmpdir = tempfile.mkdtemp(prefix="mectov_hardening_")
    tdisk = os.path.join(tmpdir, "disk.img")
    text2 = os.path.join(tmpdir, "ext2.img")
    shutil.copy(args.disk, tdisk)
    shutil.copy(args.ext2, text2)

    qemu_cmd = [
        "qemu-system-i386",
        "-cpu", "qemu32,+nx",
        "-vga", "std",
        "-cdrom", args.iso,
        "-m", "128",
        "-smp", "4",
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        "-net", "none",
        "-drive", f"file={tdisk},format=raw,index=0,media=disk",
        "-drive", f"file={text2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    qemu = subprocess.Popen(qemu_cmd)
    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] entropy", args.timeout):
            print("[FAIL] kernel never reached entropy init")
            return 1
        print("[OK] booted, entropy initialized")
        if not wait_for_in_file(SERIAL_LOG, "[K] login", 90):
            print("[FAIL] kernel never reached login screen")
            return 1
        send_keys(LOGIN_KEYS)
        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] logged in with default password, desktop running")

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(
                mon_cmd, SERIAL_LOG, "/tmp/mectov_hardening_cursor.ppm"):
            print("[FAIL] terminal did not launch")
            return 1
        wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30)
        time.sleep(1.0)

        # Focus the terminal window (click inside it).
        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)

        # ---- 1. Change the password: passwd mectov123 hunter2 ----
        type_line(["p", "a", "s", "s", "w", "d", " ", "m", "e", "c", "t",
                   "o", "v", "1", "2", "3", " ", "h", "u", "n", "t", "e",
                   "r", "2", "ret"])
        time.sleep(1.0)

        # ---- 2. Lock, then unlock with the NEW password ----
        type_line(["l", "o", "c", "k", "ret"])
        locked = wait_screen(lambda w, h, px: is_locked(px, w, h),
                             "/tmp/mectov_hardening_locked1.ppm", 25)
        if locked is None:
            print("[FAIL] `lock` did not show the login gate")
            return 1
        print("[OK] locked after password change")
        send_keys(NEW_PW_KEYS)
        desk = wait_screen(lambda w, h, px: is_taskbar(px, w, h),
                           "/tmp/mectov_hardening_unlocked1.ppm", 25)
        if desk is None:
            print("[FAIL] NEW password did not unlock — hashing broken?")
            return 1
        print("[OK] new password unlocks (salted hash accepted)")

        # ---- 3. Lock again, old password must be REJECTED ----
        type_line(["l", "o", "c", "k", "ret"])
        locked = wait_screen(lambda w, h, px: is_locked(px, w, h),
                             "/tmp/mectov_hardening_locked2.ppm", 25)
        if locked is None:
            print("[FAIL] second `lock` did not show the login gate")
            return 1
        send_keys(OLD_PW_KEYS)
        time.sleep(2.0)
        # A wrong password leaves the panel open (shake) — the desktop
        # (taskbar) must NOT come back. The clock is hidden while the panel
        # is open, so check the taskbar, not is_locked().
        still = wait_screen(lambda w, h, px: not is_taskbar(px, w, h),
                            "/tmp/mectov_hardening_stilllocked.ppm", 6, settle=0.4)
        if still is None:
            print("[FAIL] OLD password unlocked the desktop — plaintext path!")
            return 1
        print("[OK] old password rejected (no plaintext fallback on hashed file)")

        # ---- 4. Recover with the new password, then run the Ring-3 app ----
        # The panel is already open after the rejection, so no leading SPACE.
        send_keys(PANEL_PW_KEYS)
        desk = wait_screen(lambda w, h, px: is_taskbar(px, w, h),
                           "/tmp/mectov_hardening_unlocked2.ppm", 25)
        if desk is None:
            print("[FAIL] could not unlock with new password after rejection")
            return 1
        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.4)
        if not run_app_cmd("hardening_test", "ALL TESTS PASSED", 60):
            print("[FAIL] hardening_test.mct did not report ALL TESTS PASSED")
            return 1
        print("[OK] SYS_GETRANDOM + /dev/random + hashed passwd file verified from Ring 3")

        if wait_for_in_file(SERIAL_LOG, "[PANIC]", 5):
            print("[FAIL] kernel panic on the run")
            return 1
        print("[OK] no kernel panic")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
        shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import terminal_launch
    sys.exit(main())
