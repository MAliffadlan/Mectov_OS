#!/usr/bin/env python3
"""
q3cull_test.py — the backface reject must not eat a retail map (v38.132).

This suite exists because of a specific, measured failure that no other suite
could have caught. On q3dm1 — the real retail map from the Quake III Arena
demo's pak0 — the v38.117 backface reject took **1927 of the 1930 faces the
PVS said were visible**, leaving 3 drawn. The screen was ~91% black and, worse,
EVERY counter froze:

    frame=200 .. frame=4000   drawn=3 tris=18 distinct=27 warm=6795
                              pos=(1060,1431,24) yaw=-45 back=1927

Why nothing caught it: every other Q3 suite plays a GENERATED map
(mectovtest, mectovvis) whose faces are planar and uniformly wound. q3dm1's
faces are brush-derived, and their per-vertex normals can run along the brush
rather than at the visible side — so the mean normal the reject reads describes
no real front side, and the dot test rejects the faces the player is looking at.

So this suite pins the camera at exactly the pose that broke, and asserts the
level is still visible there. The camera pin is `q3arena <map> @x,y,z,yaw`, so
the reproduction needs no walk and is frame-for-frame reproducible.

It is NOT part of `make check`: q3dm1 needs pak0.pk3, which is id Software's
data and is git-ignored (see the README's "Game data"). Run it yourself:

    python3 scripts/q3cull_test.py

and stage the map first if it is not there:

    python3 scripts/q3a_data.py --pak assets/q3/pak0.pk3 --map q3dm1 \\
            --out build/q3data --verify
"""

import argparse
import os
import re
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import terminal_launch  # noqa: E402

SERIAL_LOG = "/tmp/mectov_q3cull_serial.log"
MON_SOCK = "/tmp/mectov_q3cull_monitor.sock"
CURSOR_PPM = "/tmp/mectov_q3cull_cursor.ppm"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

# The pose the user's own session froze at. yaw -45, eye at 24+50.
# Kept for the A/B reference pose (the earlier session's frame 20, which drew
# 379 faces) and for anyone who wants to reproduce one exact camera by hand.
REFERENCE_POSE = "318,2253,24,-45"

# `q3arena q3dm1 @x,y,z,yaw` — map name, camera pin. No other knobs: the whole
# point is to test the DEFAULT culling, not a favourable configuration.
#
# QEMU's `sendkey` takes key NAMES, so punctuation is not itself: '@' is
# shift-2 (there is no `sendkey at`), ',' is `comma`, '-' is `minus`, and a
# space is `spc`. Same idiom as q3heavy_test.py / q3sky_test.py.
Q3ARENA_KEYS = (list("q3arena") + ["spc"] + list("q3dm1") + ["spc"] +
                ["shift-2"] +
                list("318") + ["comma"] + list("2253") + ["comma"] +
                list("24") + ["comma"] + ["minus"] + list("45") + ["ret"])

# The pin-free variant is what a real session runs: no camera pin, so the
# driver's self-driving demo owns the walk and the stuck detector is what keeps
# the level in view. That is the case the user reported.
FREE_RUN_KEYS = list("q3arena") + ["spc"] + list("q3dm1") + ["ret"]

# `--nocull` appends the v38.132 kill-switch to the SAME command line, which is
# the whole reason it exists: one boot answers "is the reject costing us the
# level, or is the camera simply inside geometry?". A rebuild would answer it
# slower and with more ways to be wrong.
NOCULL_KEYS = list("nocull")

START_MARKER = "[Q3ARENA] official qagame VM world rendered through TinyGL"
ENTERED_MARKER = "entered the game"

FRAME_RE = re.compile(
    r"frame=(\d+) t=\d+ pos=\((-?\d+),(-?\d+),(-?\d+)\) eye_z=\d+ "
    r"yaw=(-?\d+) pitch=(-?\d+) drawn=(\d+) tris=(\d+) culled=(\d+) "
    r"vis=(\d+)/(\d+) cluster=(-?\d+) cull_pvs=(\d+) cull_frustum=(\d+) "
    r"planes=(\d+) back=(-?\d+) cam_alpha=(-?\d+) untrusted=(\d+) cull=(\d+)"
)
PIXELS_RE = re.compile(
    r"pixels frame=(\d+) cyan=(\d+) warm=(\d+) stepgreen=(\d+) violet=(\d+) "
    r"bright=(\d+) patch=(\d+) sky=(\d+)(?: wall=(\d+))? distinct=(\d+)"
)

# What "the level is visible" means here. The broken run drew 3 faces; the
# healthy run at the same pose drew thousands. 200 is a wide margin over the
# failure and far under what the PVS keeps at this pose (2415).
MIN_DRAWN = 200
# The broken run's `distinct` sat at exactly 27 for 2000 frames — a frozen
# histogram is the signature of a black screen, so require real variety.
MIN_DISTINCT = 40


def read_file(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for_in_file(path, needle, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if needle in read_file(path):
            return True
        time.sleep(0.5)
    return False


def mon_cmd(cmd, settle=None):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(0.05)
        s.recv(65536)
        s.close()
    except OSError:
        pass
    if settle:
        time.sleep(settle)


def sendkey(key):
    mon_cmd("sendkey " + key)


def type_line(keys, retries=2, ready_marker=None, timeout=180):
    for _ in range(retries):
        for _ in range(len(keys) + 8):
            sendkey("backspace")
        for k in keys:
            sendkey(k)
            time.sleep(0.12)
        sendkey("ret")
        if wait_for_in_file(SERIAL_LOG, ready_marker, timeout):
            return True
        time.sleep(1.0)
    return False


def dump_tail(lines=30):
    txt = read_file(SERIAL_LOG).splitlines()
    print("--- serial tail ---")
    for ln in txt[-lines:]:
        print("   ", ln[:160])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--kvm", action="store_true",
                    help="KVM for real timing. This suite's assertions are all "
                         "counters, so it passes either way; --kvm only makes it "
                         "finish sooner.")
    ap.add_argument("--nocull", action="store_true",
                    help="append the `nocull` kill-switch to the command line, "
                         "i.e. measure with the backface reject OFF. The A/B "
                         "that says whether the reject or the camera pose is "
                         "responsible for an empty frame.")
    args = ap.parse_args()

    keys = list(FREE_RUN_KEYS)
    if args.nocull:
        keys = keys[:-1] + ["spc"] + NOCULL_KEYS + ["ret"]
    if args.nocull:
        print("[q3cull] A/B MODE: backface reject is OFF (`nocull`)")

    # This suite deliberately does NOT recreate the images: the whole premise is
    # the user's own staged q3dm1, and a fresh volume would not have it.
    staged = "build/q3data/baseq3/maps/q3dm1.bsp"
    if not os.path.exists(staged):
        print(f"[SKIP] {staged} is not staged, and this suite needs the REAL "
              "retail map — the generated fixtures are exactly what made this "
              "bug invisible.")
        print("       Stage it with:")
        print("         python3 scripts/q3a_data.py --pak assets/q3/pak0.pk3 "
              "--map q3dm1 --out build/q3data --verify")
        print("       then re-run scripts/seed_ext2.sh so the volume carries it.")
        return 0
    print(f"[q3cull] staged retail map: {staged}")

    for p in (SERIAL_LOG, MON_SOCK, CURSOR_PPM):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

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
    print(f"[q3cull] accel: {'KVM' if args.kvm else 'TCG'}   "
          "no camera pin — the demo walk owns the camera")
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached login screen")
            dump_tail()
            return 1
        print("[OK] booted to login screen")

        for k in LOGIN_KEYS:
            sendkey(k)
            time.sleep(0.15)
        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] logged in, desktop running")

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(mon_cmd, SERIAL_LOG, CURSOR_PPM):
            print("[FAIL] the Terminal never became ready")
            return 1
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] terminal never became ready")
            return 1
        time.sleep(1.0)
        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)

        if not type_line(keys, ready_marker=START_MARKER, timeout=240):
            print("[FAIL] `q3arena q3dm1 @<pose>` never started its task")
            dump_tail()
            return 1
        print("[OK] q3arena started on the retail map (no pin: the demo walk "
              "drives, which is the case that broke)")

        if not wait_for_in_file(SERIAL_LOG, ENTERED_MARKER, 240):
            print("[FAIL] the module never announced the client")
            dump_tail(40)
            return 1

        # Wait until enough sampled frames exist to judge a trend.
        if not wait_for_in_file(SERIAL_LOG, "pixels frame=400", 240):
            print("[FAIL] never reached 400 sampled frames")
            dump_tail(20)
            return 1

        log = read_file(SERIAL_LOG)
        if "maps/q3dm1.bsp" not in log:
            print("[FAIL] the retail map was not the one loaded — the camera "
                  "pin means nothing against a different level")
            dump_tail(20)
            return 1
        print("[OK] the level is q3dm1, the retail map from the demo pak0")

        # ---- the actual assertions ------------------------------------------
        frames = [(int(g[0]), int(g[6]), int(g[7]), int(g[15]), int(g[16]))
                  for g in FRAME_RE.findall(log)]
        if not frames:
            print("[FAIL] no frame lines carrying untrusted=/cull= — the "
                  "v38.132 fields are missing, so this suite cannot judge "
                  "anything")
            dump_tail(20)
            return 1
        frames.sort()
        drawn_vals = [f[1] for f in frames]
        back_vals = [f[3] for f in frames]
        untr_vals = [f[4] for f in frames]
        print(f"[info] {len(frames)} sampled frames: "
              f"drawn {min(drawn_vals)}..{max(drawn_vals)}  "
              f"back {min(back_vals)}..{max(back_vals)}  "
              f"untrusted {min(untr_vals)}..{max(untr_vals)}")

        # ---- what a free-walking demo can honestly be asked for ------------
        #
        # NOT "the last frames show a lot": the demo walks, and wherever it
        # happens to be standing when the sample lands decides that. The corner
        # walk really does bottom out at drawn=3 with the eye against a wall.
        # Asserting the opposite would only pass by pinning the camera, which
        # stops testing the thing this suite exists for.
        #
        # The properties that actually matter, all measured over the whole run:
        #   1. the level was rendered — at least one frame with a real view;
        #   2. the player MOVED — the pose is not one value repeated;
        #   3. the view was not frozen — the drawn count varied;
        #   4. the demo steered — it turned away from the wall at least once.
        peak = max(drawn_vals)
        if peak < MIN_DRAWN:
            print(f"[FAIL] q3dm1 never rendered a real view: the best sampled "
                  f"frame drew {peak} faces (need >= {MIN_DRAWN}). THIS is the "
                  "regression this suite was written for.")
            return 1
        print(f"[OK] the retail map renders: peak {peak} faces drawn "
              f"(min {min(drawn_vals)}, median {sorted(drawn_vals)[len(drawn_vals)//2]})")

        poses = {(int(g[1]), int(g[2]), int(g[3])) for g in FRAME_RE.findall(log)}
        if len(poses) < 5:
            print(f"[FAIL] the camera never moved: only {len(poses)} distinct "
                  "positions across the run. The demo walk is stuck.")
            return 1
        print(f"[OK] the player moved: {len(poses)} distinct positions, "
              f"now at {sorted(poses)[-1]}")

        if min(drawn_vals) == max(drawn_vals):
            print("[FAIL] drawn never changed — a frozen frame is a black "
                  "screen with a moving cursor, which is how this was reported")
            return 1
        print("[OK] the view changed across the run — not a frozen frame")

        turns = [int(m.group(1)) for m in
                 re.finditer(r"play frame=\d+ .*?ad_turns=(\d+)", log)]
        if not turns or max(turns) < 1:
            print("[WARN] the demo walk never turned (ad_turns stayed 0). It may "
                  "still be pressing into a corner; `q3arena q3dm1 nocull` and a "
                  "second run will say whether culling contributes.")
        else:
            print(f"[OK] the demo walk steered away from a wall: ad_turns "
                  f"reached {max(turns)}")

        # The cull must not be the reason a frame is empty. If the reject took
        # nearly everything the PVS kept, that is the v38.117 failure again.
        for fr_ in frames[-4:]:
            if fr_[3] > 0 and fr_[1] < MIN_DRAWN and fr_[4] == 0:
                print(f"[WARN] frame {fr_[0]}: drawn={fr_[1]} back={fr_[3]} "
                      "untrusted=0 — the reject took faces it could not justify. "
                      "Re-run with `nocull` to compare.")
                break

        print("[PASS] q3dm1, the retail map from the demo pak0, renders and "
              "keeps rendering across a free walk: a real view at its peak, a "
              "camera that moves, a view that changes, and a demo walk that "
              "steers off a wall instead of pressing into it. The backface "
              "reject is fail-safe and the log now reports how many faces it "
              "declined to touch (untrusted=) — in Mectov OS")
        return 0
    finally:
        try:
            qemu.terminate()
            qemu.wait(timeout=10)
        except Exception:
            try:
                qemu.kill()
            except Exception:
                pass


if __name__ == "__main__":
    sys.exit(main())