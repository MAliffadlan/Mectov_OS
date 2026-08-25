#!/usr/bin/env python3
"""
scripts/fbmap_test.py — Ring 3 direct-framebuffer + scanout takeover regression.

Boots mectov.iso, logs in, runs `run /apps/fbmap.mct` and verifies the full
display-server handshake:

  1. SYS_FB_MAP grants the scanout ("[FBMAP] tid=" from the kernel)
  2. the kernel desktop SUPPRESSES its own rendering while the holder runs
     ("FBMAP INTERACTIVE" from the app after the animation phase)
  3. keystrokes route to the holder: the app exits on a typed key
     ("FBMAP key exit" + "FBMAP RELEASED")
  4. the desktop is repainted on handback ("[FBMAP] desktop restored")
  5. the OS stayed alive afterwards

Usage:
    python3 scripts/fbmap_test.py [--timeout 300]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

import terminal_launch

SERIAL_LOG = "/tmp/mectov_fb_serial.log"
MON_SOCK = "/tmp/mectov_fb_monitor.sock"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
RUN_KEYS = ["r", "u", "n", "spc", "slash", "a", "p", "p", "s",
            "slash", "f", "b", "m", "a", "p", "dot", "m", "c", "t", "ret"]


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


def wait_for_in_file_tail(path, needle, from_offset, timeout):
    """Like wait_for_in_file but only considers content AFTER `from_offset`
    bytes — used to detect the SECOND occurrence of a marker in one log."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, "r", errors="replace") as f:
                f.seek(from_offset)
                if needle in f.read():
                    return True
        except (FileNotFoundError, OSError):
            pass
        time.sleep(1)
    return False


def mon_cmd(cmd):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(0.15)
        s.close()
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

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
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached login screen")
            return 1
        print("[OK] booted to login screen")

        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)

        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] logged in, desktop running")

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(
                mon_cmd, SERIAL_LOG, "/tmp/mectov_fb_cursor.ppm"):
            print("[FAIL] terminal did not launch (icon double-click missed?)")
            return 1
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] terminal never became ready")
            return 1
        time.sleep(1.0)

        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)

        ok_run = False
        for _ in range(3):
            for _ in range(32):
                mon_cmd("sendkey backspace")
            for k in RUN_KEYS:
                mon_cmd("sendkey " + k)
                time.sleep(0.12)
            if wait_for_in_file(SERIAL_LOG, "FBMAP start", 25):
                ok_run = True
                break
            time.sleep(1.0)
        if not ok_run:
            print("[FAIL] fbmap never started")
            return 1
        print("[OK] fbmap running")

        # THE assertion: the kernel granted the scanout to Ring 3.
        if not wait_for_in_file(SERIAL_LOG, "[FBMAP] tid=", 30):
            print("[FAIL] kernel never granted the framebuffer mapping")
            return 1
        print("[OK] SYS_FB_MAP granted the scanout to Ring 3")

        # The app finished animating and went interactive: the takeover is
        # live (kernel desktop suppressed, input routed to the holder).
        if not wait_for_in_file(SERIAL_LOG, "FBMAP INTERACTIVE", 240):
            print("[FAIL] app never reached the interactive phase")
            return 1
        print("[OK] scanout takeover live, app interactive")

        # Type a key: it must route THROUGH the kernel pump to the holder,
        # which releases the scanout and exits.
        mon_cmd("sendkey q")
        if not wait_for_in_file(SERIAL_LOG, "FBMAP key exit", 60):
            print("[FAIL] typed key never reached the scanout holder")
            return 1
        print("[OK] keystroke routed through the takeover to Ring 3")

        if not wait_for_in_file(SERIAL_LOG, "FBMAP RELEASED", 30):
            print("[FAIL] sys_fb_release did not succeed")
            return 1
        print("[OK] scanout released cleanly")

        # Kernel noticed the handback and repainted the desktop.
        if not wait_for_in_file(SERIAL_LOG, "[FBMAP] desktop restored", 30):
            print("[FAIL] kernel never restored the desktop")
            return 1
        print("[OK] kernel restored the desktop on handback")

        # And the FAIL line must never appear.
        try:
            with open(SERIAL_LOG, "r", errors="replace") as f:
                if "FBMAP FAIL" in f.read():
                    print("[FAIL] framebuffer path failed inside the guest")
                    return 1
        except OSError:
            pass
        print("[OK] no FBMAP FAIL line")

        # ---- Scenario B: forced kill DURING the takeover ----
        # A wedged compositor must be killable: Ctrl+C (SIGINT to the fg
        # group) while the scanout is held must end the task, drop ownership
        # through the task_cleanup hook, and restore the desktop — no panic.
        time.sleep(1.0)
        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)
        ok_run2 = False
        log_mark = os.path.getsize(SERIAL_LOG)  # only look at NEW log content
        for _ in range(3):
            for _ in range(32):
                mon_cmd("sendkey backspace")
            for k in RUN_KEYS:
                mon_cmd("sendkey " + k)
                time.sleep(0.12)
            if wait_for_in_file_tail(SERIAL_LOG, "FBMAP start", log_mark, 25):
                ok_run2 = True
                break
            time.sleep(1.0)
        if not ok_run2:
            print("[FAIL] second fbmap never started")
            return 1
        if not wait_for_in_file_tail(SERIAL_LOG, "FBMAP INTERACTIVE", log_mark, 240):
            print("[FAIL] second takeover never went interactive")
            return 1
        print("[OK] second takeover live")
        mon_cmd("sendkey ctrl-c")
        if not wait_for_in_file_tail(SERIAL_LOG, "[FBMAP] owner tid", log_mark, 30):
            print("[FAIL] killed owner did not release the scanout")
            return 1
        print("[OK] SIGKILL-style teardown dropped scanout ownership")
        if not wait_for_in_file_tail(SERIAL_LOG, "[FBMAP] desktop restored", log_mark, 30):
            print("[FAIL] desktop not restored after forced kill")
            return 1
        print("[OK] desktop restored after forced kill")

        time.sleep(5)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive after the takeover roundtrip")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
