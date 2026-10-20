#!/usr/bin/env python3
"""scripts/wc_test.py — `wc` builtin regression test (v38.166).

Boots mectov.iso once, logs in, opens the Terminal and drives the `wc`
builtin, then checks the numbers it reports against the real file sizes.

Why this suite exists (v38.166 bug fix):
  The old `wc` read the target into a 2048-byte stack buffer and counted
  that. vfs_read_file() clamps the read to the caller's max_size, so every
  file larger than 2047 bytes was reported as exactly 2047 bytes, and the
  line/word counts came from the file's head only. `wc /ext2/music.wav`
  (55169 bytes) claimed 2047 — 96% of the count missing — while still
  printing a clean-looking row. The old code also kept a pointer to `wbuf`
  after the block that declared it had closed (stack-use-after-scope,
  confirmed by AddressSanitizer).

The suite asserts:
  1. `wc` on a file SMALLER than the old 2047-byte window is exact.
  2. `wc` on a file LARGER than that window reports the real byte count,
     not the clamp.
  3. `wc` on a missing file still reports "file not found" and does not
     print a bogus count row.

Usage:
    python3 scripts/wc_test.py [--timeout 300]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import terminal_launch  # noqa: E402

SERIAL_LOG = "/tmp/mectov_wc_serial.log"
MON_SOCK = "/tmp/mectov_wc_monitor.sock"
CURSOR_PPM = "/tmp/mectov_wc_cursor.ppm"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

# music.wav is seeded into the ext2 volume by scripts/seed_ext2.sh and is
# 55169 bytes — far past the old 2047-byte window, so it is the case that
# used to report a truncated count. Lines and bytes are compared exactly.
# The word count is NOT compared against GNU wc: coreutils 9.4 takes a
# locale/multibyte path on binary data and reports 473 for this file, while
# every byte-wise model (tr, awk, split) reports 696 — the kernel follows
# the byte-wise POSIX isspace() rule.
MUSIC_PATH = "/ext2/music.wav"
MUSIC_SIZE = 55169
MUSIC_LINES = 158

# mectov1.map is seeded next to music.wav: small enough to have been correct
# even before the fix, so it guards against the fix breaking the simple path.
# Being TEXT, all three counts must match host `wc -l -w -c` exactly.
MAP_PATH = "/ext2/mectov1.map"
MAP_SIZE = 1629
MAP_LINES = 32
MAP_WORDS = 298


def wait_for_in_file(path, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, "r", errors="replace") as f:
                if needle in f.read():
                    return True
        except OSError:
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
        print("[!] monitor cmd '%s' failed: %s" % (cmd, e))


def type_command(keys):
    for k in keys:
        mon_cmd("sendkey " + k)
        time.sleep(0.12)
    mon_cmd("sendkey ret")


# QEMU's monitor `sendkey` wants key NAMES, not the literal characters.
KEYMAP = {
    "/": "slash",
    " ": "spc",
    ".": "dot",
    "-": "minus",
    "_": "shift-minus",
}


def path_keys(path):
    """Turn a path into monitor key names."""
    out = []
    for ch in path:
        out.append(KEYMAP.get(ch, ch))
    return out


def serial_bytes():
    """The raw serial log. Offsets into this are BYTE offsets — the log holds
    non-UTF-8 bytes, so a decoded string's length is not a usable offset."""
    try:
        with open(SERIAL_LOG, "rb") as f:
            return f.read()
    except OSError:
        return b""


def serial_text():
    return serial_bytes().decode("utf-8", errors="replace")


def serial_offset():
    """Current byte offset of the serial log."""
    return len(serial_bytes())


def wc_rows(text):
    """Every [SH] wc <lines> <words> <bytes> <path> row, as tuples.

    The serial stream interleaves progress dots with the kernel's output, so
    a row can arrive as `.[SH] wc ...` — searching for the marker anywhere in
    the line (rather than requiring it at column 0) is what makes this
    detection reliable.
    """
    rows = []
    marker = "[SH] wc "
    for line in text.splitlines():
        idx = line.find(marker)
        if idx < 0:
            continue
        parts = line[idx:].split()
        # [SH] wc <hex-lines> <hex-words> <hex-bytes> <path...>
        if len(parts) < 5:
            continue
        try:
            lines = int(parts[2], 16)
            words = int(parts[3], 16)
            nbytes = int(parts[4], 16)
        except ValueError:
            continue
        rows.append((lines, words, nbytes, " ".join(parts[5:])))
    return rows


def run_wc(path, timeout=60, attempts=3):
    """Type `wc <path>` in the guest shell and return the parsed row.

    The terminal window needs keyboard focus and a clean input line before
    the command lands, otherwise the keystrokes go to the desktop and the
    command never runs — that is the flakiness that made an earlier draft
    of this suite report "no row" for whichever file it tried first.
    """
    for attempt in range(attempts):
        before = serial_offset()
        # Clear whatever is on the prompt line.
        for _ in range(32):
            mon_cmd("sendkey backspace")
        time.sleep(0.3)
        type_command(["w", "c", "spc"] + path_keys(path))

        deadline = time.time() + timeout
        while time.time() < deadline:
            text = serial_bytes()[before:].decode("utf-8", errors="replace")
            rows = wc_rows(text)
            if rows:
                time.sleep(1.5)
                return wc_rows(serial_bytes()[before:].decode("utf-8",
                                                              errors="replace"))[-1]
            time.sleep(0.5)

        print("       (attempt %d/%d for %s produced no row — refocusing)"
              % (attempt + 1, attempts, path))
        # Re-focus the terminal window and try again.
        mon_cmd("mouse_move 300 176"); time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(1.0)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK, CURSOR_PPM):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    qemu = subprocess.Popen([
        "qemu-system-i386", "-cpu", "qemu32,+nx", "-vga", "std",
        "-cdrom", args.iso, "-m", "128", "-smp", "4",
        "-display", "none", "-serial", f"file:{SERIAL_LOG}", "-net", "none",
        "-snapshot",
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    failures = []
    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached the login screen")
            return 1
        print("[OK] reached login screen")

        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)
        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete (no BOOTED KERNEL LOOP)")
            return 1
        print("[OK] logged in")
        time.sleep(1.5)

        if not terminal_launch.launch_terminal(mon_cmd, SERIAL_LOG, CURSOR_PPM):
            print("[FAIL] the Terminal did not launch")
            return 1
        print("[OK] Terminal launched")
        # The terminal signals readiness by creating its IPC queue; without
        # this wait the first command is typed into the desktop, not the shell.
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] terminal never became ready")
            return 1
        time.sleep(1.0)

        # Click into the terminal window so the shell takes keystrokes.
        mon_cmd("mouse_move 300 176"); time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)

        # --- Case 1: a file SMALLER than the old 2047-byte window ----------
        # mectov1.map is a small text file seeded next to music.wav.
        small = run_wc(MAP_PATH)
        if small is None:
            print("[FAIL] no [SH] wc row for %s" % MAP_PATH)
            failures.append("small-file: no row")
        else:
            lines, words, nbytes, path = small
            print("small file: lines=%d words=%d bytes=%d path=%s"
                  % (lines, words, nbytes, path))
            if (lines, words, nbytes) != (MAP_LINES, MAP_WORDS, MAP_SIZE):
                failures.append(
                    "small-file: got (%d, %d, %d), host wc says (%d, %d, %d)"
                    % (lines, words, nbytes, MAP_LINES, MAP_WORDS, MAP_SIZE))

        # --- Case 2: a file LARGER than the old window ----------------------
        big = run_wc(MUSIC_PATH)
        if big is None:
            print("[FAIL] no [SH] wc row for %s" % MUSIC_PATH)
            failures.append("large-file: no row")
        else:
            lines, words, nbytes, path = big
            print("large file: lines=%d words=%d bytes=%d path=%s"
                  % (lines, words, nbytes, path))
            if nbytes == 2047:
                failures.append("large-file: still clamped to the old 2047 window")
            if (lines, nbytes) != (MUSIC_LINES, MUSIC_SIZE):
                failures.append(
                    "large-file: got lines=%d bytes=%d, host says lines=%d bytes=%d"
                    % (lines, nbytes, MUSIC_LINES, MUSIC_SIZE))

        # --- Case 3: a missing file must not print a count row -------------
        before = serial_offset()
        type_command(["w", "c", "spc", "slash", "d", "o", "e", "s",
                      "n", "o", "t", "e", "x", "i", "s", "t"])
        time.sleep(3)
        after = serial_bytes()[before:].decode("utf-8", errors="replace")
        if "[SH] wc error file not found" not in after:
            failures.append("missing-file: no 'file not found' message")
        else:
            print("[OK] missing file reports 'file not found'")
        if wc_rows(after):
            failures.append("missing-file: a count row was printed anyway")

        print("")
        if failures:
            print("[FAIL] %d check(s) failed:" % len(failures))
            for f in failures:
                print("       - %s" % f)
            return 1
        print("[PASS] wc reports the whole file, not the first 2047 bytes")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())