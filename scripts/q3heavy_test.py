#!/usr/bin/env python3
"""
scripts/q3heavy_test.py — the exclusive fullscreen present and a pinned pose
(v38.119, Q3 phase 11).

Why this suite exists
---------------------
v38.119 adds two things to `q3arena` that no earlier suite could reach, because
both are about the PRESENT path and the CAMERA rather than about the level:

  * `q3arena <map> fullscreen` takes the present path away from the compositor
    (`vga_fullscreen_enter`): the descriptor stops being composited by the WM,
    the kernel main loop swaps the back buffer instead of running full_redraw(),
    and the driver upscales the finished 320x240 frame straight into it. ESC has
    to hand the game back to its window FIRST, and only quit on a second press —
    a fullscreen mode that quits the game, or that leaves a black screen behind,
    is worse than no fullscreen mode at all. That is the whole first half of this
    suite.

  * `q3arena <map> @x,y,z,yaw[,pitch]` pins the camera at an exact pose while the
    module keeps simulating and walking its player. It is how a heavy VIEW is
    made reproducible — and a camera that is "pinned" but still wobbles would
    silently invalidate every measurement taken with it, which is exactly the
    kind of failure that looks like data. The second half of this suite asserts
    the pin is exact: every sampled frame must report the SAME geometry.

What is asserted, and why each one is evidence:

  1. the argument reached the driver, and it is the one that was asked for: the
     args line reports `map='mectovtest' fullscreen=1 pose=1`. A fullscreen mode
     that silently fell back to the window would otherwise look identical from
     outside except for the screen contents.
  2. the game is on screen and alive: sampled frames keep arriving while
     fullscreen is active, with a nonzero face count.
  3. the DESKTOP IS GONE while fullscreen owns the screen. Read from a QEMU
     screendump, not from the renderer's buffer: the renderer's own histogram
     cannot see the present path at all (it classifies the 320x240 frame the
     rasterizer produced, which is the same image either way), so the only
     evidence that fullscreen replaced the desktop is the SCREEN.
     The check is a REFERENCE DUMP: this suite screendumps the desktop before
     it launches the game and then asks, pixel by pixel, how much of the
     fullscreen screen is still that desktop. Measured: 100% at the instant
     fullscreen is entered (the swap shows the last composited frame), 0-3%
     once the game presents, and ~95% again after ESC returns the window — so
     the two assertions are the same measurement read in opposite directions.
     v38.124 is why this replaced a colour COUNT: that bound was calibrated
     when the screen's only non-frame colours were the perf overlay's, and the
     real .md3 view model (its own 256x256 texture, 267 triangles) legitimately
     puts ~290 distinct 4-bit colours on screen — a number a gradient desktop
     would also produce, so the count could not tell the two apart. The frame's
     own palette stays printed next to it as context.
  4. the PIN is exact. Every sampled frame's `drawn` and `tris` must be equal,
     and equal to the same values the pose produced in the first window. This is
     the assertion that makes the pinned pose a fixture other measurements can be
     compared against.
  5. ESC #1 returns to the WINDOW, it does not quit: the driver must say so, the
     game must still be drawing frames after it, and the pin must still hold
     (the scene is the same one, now presented through the compositor).
  6. ESC #2 quits cleanly: the module tears down and the OS is still alive.

Usage:
    python3 scripts/q3heavy_test.py [--timeout 600] [--iso mectov.iso] [--kvm]

`make check-q3heavy` builds the MECTOV_Q3=1 ISO as mectov.iso. The suite's
assertions are host-speed independent (they are screen contents and geometry, not
timings), so --kvm changes only how long the run takes.
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import time

import q3_images
import terminal_launch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

SERIAL_LOG = "/tmp/mectov_q3heavy_serial.log"
MON_SOCK = "/tmp/mectov_q3heavy_monitor.sock"
CURSOR_PPM = "/tmp/mectov_q3heavy_cursor.ppm"
SHOT_FS = "/tmp/mectov_q3heavy_fullscreen.ppm"
SHOT_PIN = "/tmp/mectov_q3heavy_pinned.ppm"
SHOT_DESK = "/tmp/mectov_q3heavy_desktop.ppm"
QEMU_ERR_LOG = "/tmp/mectov_q3heavy_qemu_err.log"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
# `q3arena mectovtest fullscreen @496,496,114,45,0` — the fixture arena, the
# exclusive present path, and the arena's own spawn eye pose spelled out. '@'
# and ',' have no QEMU key NAMES (there is no `sendkey at`): '@' is shift-2 on
# the guest's own scancode map, so it is written as such here.
Q3ARENA_KEYS = (list("q3arena") + ["spc"] + list("mectovtest") + ["spc"] +
                list("fullscreen") + ["spc", "shift-2"] +
                list("496") + ["comma"] + list("496") + ["comma"] +
                list("114") + ["comma"] + list("45") + ["comma"] +
                list("0") + ["ret"])

START_MARKER = "[Q3ARENA] official qagame VM world rendered through TinyGL"
WINDOW_MARKER = "[Q3ARENA] window id="
PANIC_MARKER = "[PANIC]"
SYS_ERROR_MARKER = "Sys_Error"
DONE_MARKER = "[Q3VM] done"
FS_ON_MARKER = "[Q3ARENA] fullscreen: direct present (ESC returns to the window)"
FS_OFF_MARKER = "[Q3ARENA] fullscreen off (ESC)"

# The pose this suite pins, and the geometry it must reproduce. The numbers are
# the fixture arena's spawn: its eye (z = 114) and yaw 45 in world units.
POSE = "496,496,114,45,0"

ARGS_RE = re.compile(
    r"\[Q3ARENA\] args: map='(\S+)' fullscreen=(\d+) pose=(\d+)")
FRAME_RE = re.compile(
    r"\[Q3ARENA\] frame=(\d+) t=(\d+) pos=\((-?\d+),(-?\d+),(-?\d+)\) "
    r"eye_z=(-?\d+) yaw=(-?\d+) pitch=(-?\d+) drawn=(\d+) tris=(\d+) "
    r"culled=(\d+)")
FRAME_PIXELS_RE = re.compile(
    r"\[Q3ARENA\] pixels frame=(\d+) cyan=(\d+) warm=(\d+) stepgreen=(\d+) "
    r"violet=(\d+) bright=(\d+) patch=(\d+) sky=(\d+) wall=(\d+) distinct=(\d+)")


def read_file(path):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read()
    except (FileNotFoundError, OSError):
        return ""


def wait_for_in_file(path, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        text = read_file(path)
        if needle in text:
            return True
        if PANIC_MARKER in text or SYS_ERROR_MARKER in text:
            return False
        time.sleep(1)
    return False


def parse_frames(text=None):
    """Sampled frame markers as dicts, field for field."""
    text = read_file(SERIAL_LOG) if text is None else text
    out = []
    for m in FRAME_RE.finditer(text):
        out.append(dict(zip(
            ("frame", "t", "x", "y", "z", "eye_z", "yaw", "pitch",
             "drawn", "tris", "culled"), (int(g) for g in m.groups()))))
    return out


def wait_for_frame(n, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if any(f["frame"] >= n for f in parse_frames()):
            return True
        text = read_file(SERIAL_LOG)
        if PANIC_MARKER in text or SYS_ERROR_MARKER in text:
            return False
        time.sleep(1)
    return False


def mon_cmd(cmd, settle=0.15):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(settle)
        s.close()
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")


def screendump(path):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    mon_cmd(f"screendump {path}", settle=1.5)
    return os.path.exists(path) and os.path.getsize(path) > 1000


def ppm_colours(path):
    """(width, height, distinct 4-bit-per-channel colour count) of a P6 ppm.

    The count is quantised exactly like the guest's own histogram so the two are
    comparable ideas of "how many colours are on screen".
    """
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError(f"not a P6 ppm: {path}")
    pos, vals = 2, []
    while len(vals) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos] != 0x0A:
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        vals.append(int(data[start:pos]))
    w, h, _maxval = vals
    pos += 1
    payload = data[pos:pos + w * h * 3]
    seen = set()
    for i in range(0, len(payload) - 2, 3):
        seen.add((payload[i] >> 4) << 8 | (payload[i + 1] >> 4) << 4 |
                 (payload[i + 2] >> 4))
    return w, h, len(seen)


def desk_match(path):
    """Fraction of sampled pixels of `path` that are IDENTICAL to the pre-game
    desktop dump SHOT_DESK, in percent (-1 when either dump is unreadable).

    Sampled on a stride rather than every pixel: a desktop that is on screen at
    all covers most of it, and the stride makes the comparison cost nothing
    next to the screendump itself. Both dumps are the same resolution (the
    guest's framebuffer), so the byte offset of a pixel is comparable in both.
    """
    try:
        with open(path, "rb") as f:
            a = f.read()
        with open(SHOT_DESK, "rb") as f:
            b = f.read()
    except OSError:
        return -1.0
    pa, pb = a.split(b"\n", 3)[3], b.split(b"\n", 3)[3]
    same = tot = 0
    for i in range(0, min(len(pa), len(pb)) - 2, 3 * 17):
        tot += 1
        if pa[i:i + 3] == pb[i:i + 3]:
            same += 1
    return (100.0 * same / tot) if tot else -1.0


def dump_tail(lines=30):
    for line in read_file(SERIAL_LOG).splitlines()[-lines:]:
        print(line[:150])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--kvm", action="store_true",
                    help="run with -enable-kvm -cpu host (only speed changes)")
    ap.add_argument("--no-seed", action="store_true",
                    help="boot the images already in the workspace instead of "
                         "rebuilding them (development only: it makes the run "
                         "much faster once they are known good, and much less "
                         "reproducible if they are not)")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK, CURSOR_PPM, SHOT_FS, SHOT_PIN):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    # The fixture arena and its textures, rebuilt rather than inherited: this
    # suite never needs a retail map, so it must not depend on one being staged
    # (see scripts/q3_images.py).
    if not args.no_seed:
        err = q3_images.fresh_images(args.disk, args.ext2) or \
            q3_images.seed_volume(args.ext2)
        if err:
            print(f"[FAIL] {err}")
            return 1

    qemu_cmd = [
        "qemu-system-i386",
        *(["-cpu", "host"] if args.kvm else ["-cpu", "qemu32,+nx"]),
        "-vga", "std",
        "-cdrom", args.iso,
        "-m", "512",
        "-smp", "2",
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        "-net", "none",
        "-snapshot",
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ]
    if args.kvm:
        qemu_cmd.insert(1, "-enable-kvm")
    print(f"[q3heavy] accel: {'KVM' if args.kvm else 'TCG'}")
    # QEMU's stderr goes to a file, not /dev/null: a drive that is locked by
    # another instance or a bad option makes QEMU exit before it creates the
    # serial log, and without this line the symptom is an indistinguishable
    # "the kernel never reached login screen".
    qemu_err = open(QEMU_ERR_LOG, "w")
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL,
                            stderr=qemu_err)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached login screen")
            dump_tail()
            return 1
        print("[OK] booted to login screen")
        for k in LOGIN_KEYS:
            mon_cmd("sendkey " + k, settle=0.12)
        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", args.timeout):
            print("[FAIL] login incomplete")
            dump_tail()
            return 1
        print("[OK] logged in, desktop running")
        time.sleep(1.0)

        if not terminal_launch.launch_terminal(mon_cmd, SERIAL_LOG, CURSOR_PPM):
            print("[FAIL] terminal never became ready")
            dump_tail()
            return 1
        # The Terminal's IPC queue is the real readiness marker: the icon click
        # can land before the shell task exists, and a command typed into that
        # gap is swallowed (it is what makes a bare retry loop flaky).
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] the Terminal never created its IPC queue")
            dump_tail()
            return 1
        time.sleep(1.0)
        # Click inside the terminal before typing: launch_terminal leaves the
        # cursor on the icon it verified.
        mon_cmd("mouse_move 300 176", settle=0.1)
        mon_cmd("mouse_button 1", settle=0.1)
        mon_cmd("mouse_button 0", settle=0.5)

        # The desktop AS IT IS when the game launches: the reference every
        # "is the desktop on screen" assertion below is measured against.
        if not screendump(SHOT_DESK):
            print("[FAIL] no desktop reference screendump")
            return 1
        _dw, _dh, desk_colours = ppm_colours(SHOT_DESK)
        print(f"[OK] desktop reference: {_dw}x{_dh}, {desk_colours} colours")

        print(f"[q3heavy] launching q3arena mectovtest fullscreen @{POSE}")
        for attempt in range(3):
            mon_cmd("mouse_move 300 176", settle=0.1)
            mon_cmd("mouse_button 1", settle=0.1)
            mon_cmd("mouse_button 0", settle=3.0)
            for _ in range(24):
                mon_cmd("sendkey backspace", settle=0.05)
            for k in Q3ARENA_KEYS:
                mon_cmd("sendkey " + k, settle=0.12)
            if wait_for_in_file(SERIAL_LOG, WINDOW_MARKER, 60):
                break
            print(f"[q3heavy] retry launch ({attempt + 1})")
        else:
            print("[FAIL] no game window")
            dump_tail()
            return 1

        if not wait_for_in_file(SERIAL_LOG, START_MARKER, args.timeout):
            print("[FAIL] the game module never announced its world")
            dump_tail()
            return 1

        # 1. the argument that was asked for, and nothing else.
        m = ARGS_RE.search(read_file(SERIAL_LOG))
        if not m:
            print("[FAIL] no [Q3ARENA] args line — the driver never parsed "
                  "the command line")
            dump_tail()
            return 1
        got_map, got_fs, got_pose = m.group(1), int(m.group(2)), int(m.group(3))
        if (got_map, got_fs, got_pose) != ("mectovtest", 1, 1):
            print(f"[FAIL] args: map='{got_map}' fullscreen={got_fs} "
                  f"pose={got_pose}, wanted map='mectovtest' fullscreen=1 "
                  f"pose=1")
            return 1
        print(f"[OK] the command line reached the driver: map='{got_map}' "
              f"fullscreen=1 pose=1 (@{POSE})")

        if FS_ON_MARKER not in read_file(SERIAL_LOG):
            print(f"[FAIL] fullscreen was requested but the driver never took "
                  f"the present path (no '{FS_ON_MARKER}')")
            return 1
        print("[OK] the driver took the exclusive present path")

        # 2. the game is alive under fullscreen: frames keep arriving.
        if not wait_for_frame(60, args.timeout):
            print("[FAIL] no sampled frame at or past frame 60")
            dump_tail()
            return 1

        # 3. the desktop is gone. The renderer's own histogram cannot see the
        #    present path, so this reads the SCREEN.
        if not screendump(SHOT_FS):
            print("[FAIL] no screendump while fullscreen was active")
            return 1
        w, h, fs_colours = ppm_colours(SHOT_FS)
        game = [int(mm.group(10)) for mm in
                FRAME_PIXELS_RE.finditer(read_file(SERIAL_LOG))]
        if not game:
            print("[FAIL] no pixel histogram to compare the screen against")
            return 1
        palette = max(game)
        fs_desk = desk_match(SHOT_FS)
        print(f"[q3heavy] fullscreen screendump: {w}x{h}, {fs_colours} "
              f"colours (the frame's own palette is {palette}), {fs_desk:.1f}% "
              f"of it identical to the desktop reference")
        if fs_desk < 0:
            print("[FAIL] the fullscreen dump could not be compared with the "
                  "desktop reference")
            return 1
        if fs_desk > 40.0:
            print(f"[FAIL] {fs_desk:.1f}% of the fullscreen screen is still the "
                  f"pre-game desktop (the desktop itself measured 100% there): "
                  f"the game's own present path did not take the screen over "
                  f"(see {SHOT_FS} against {SHOT_DESK})")
            return 1
        print("[OK] the desktop is off screen: the screen is the game's own "
              "frame, not the desktop that was there when it launched")

        # 4. the pin is exact. Every sampled frame must report the same
        #    geometry, or the "reproducible view" this pose exists for is a
        #    fiction.
        frames = [f for f in parse_frames() if f["frame"] <= 120]
        if len(frames) < 3:
            print(f"[FAIL] only {len(frames)} sampled frame(s) to check the "
                  f"pin against")
            return 1
        shapes = {(f["drawn"], f["tris"]) for f in frames}
        if len(shapes) != 1:
            print(f"[FAIL] the pinned pose drew {len(shapes)} different "
                  f"scenes (drawn,tris) = {sorted(shapes)} — a moving camera "
                  f"would invalidate every measurement taken with it")
            return 1
        drawn, tris = shapes.pop()
        if drawn <= 0 or tris <= 0:
            print(f"[FAIL] the pinned pose drew nothing: drawn={drawn} "
                  f"tris={tris}")
            return 1
        eye_z = {f["eye_z"] for f in frames}
        if eye_z != {114}:
            print(f"[FAIL] pinned eye_z came out as {sorted(eye_z)}, wanted "
                  f"the pinned 114")
            return 1
        print(f"[OK] the pin is exact across {len(frames)} sampled frames: "
              f"eye_z=114, drawn={drawn}, tris={tris} every time")
        pre_esc_frames = parse_frames()[-1]["frame"]

        # 5. ESC #1 leaves fullscreen, it does not quit.
        print("[q3heavy] ESC #1: fullscreen -> window")
        mon_cmd("sendkey esc", settle=0.2)
        if not wait_for_in_file(SERIAL_LOG, FS_OFF_MARKER, 30):
            print(f"[FAIL] ESC did not leave fullscreen (no "
                  f"'{FS_OFF_MARKER}')")
            dump_tail()
            return 1
        if not wait_for_frame(pre_esc_frames + 40, args.timeout):
            print(f"[FAIL] the game stopped drawing after ESC "
                  f"(last frame {pre_esc_frames}) — ESC quit instead of "
                  f"returning to the window")
            dump_tail()
            return 1
        after = [f for f in parse_frames() if f["frame"] > pre_esc_frames + 20]
        shapes = {(f["drawn"], f["tris"]) for f in after}
        if shapes != {(drawn, tris)}:
            print(f"[FAIL] the windowed game is not the scene fullscreen was "
                  f"showing: {sorted(shapes)} after ESC against "
                  f"{(drawn, tris)} before")
            return 1
        if screendump(SHOT_PIN):
            _w, _h, win_colours = ppm_colours(SHOT_PIN)
            win_desk = desk_match(SHOT_PIN)
            print(f"[q3heavy] windowed screendump: {win_colours} colours "
                  f"(fullscreen was {fs_colours}), {win_desk:.1f}% identical "
                  f"to the desktop reference")
            if win_desk < 40.0:
                print(f"[FAIL] the desktop did not come back: only "
                      f"{win_desk:.1f}% of the screen after ESC is the desktop "
                      f"that was there before the game took it (see {SHOT_PIN})")
                return 1
        print("[OK] ESC returned the game to its window and it kept its scene")

        # 6. ESC #2 quits, and the kernel survives it.
        print("[q3heavy] ESC #2: quit")
        mon_cmd("sendkey esc", settle=0.2)
        if not wait_for_in_file(SERIAL_LOG, DONE_MARKER, 60):
            print("[FAIL] ESC did not shut the module down")
            dump_tail()
            return 1
        if PANIC_MARKER in read_file(SERIAL_LOG):
            print("[FAIL] panic after quit")
            return 1
        print("[OK] the module shut down cleanly; the kernel is still alive")

        print("[PASS] q3heavy")
        return 0
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            qemu.kill()
        qemu_err.close()
        if not os.path.exists(SERIAL_LOG):
            print(f"[FAIL] QEMU never created the serial log; its stderr is in "
                  f"{QEMU_ERR_LOG}:")
            for line in read_file(QEMU_ERR_LOG).splitlines()[-8:]:
                print("  " + line[:150])


if __name__ == "__main__":
    sys.exit(main())
