#!/usr/bin/env python3
"""
scripts/q3viewmodel_test.py — id's weapon view model, end to end, with no id
data (v38.124).

The user-facing promise this exercises: the gun in the bottom-right corner is
**id's own .md3**, loaded from the game data and drawn with its own textures —
`scripts/q3a_data.py --pak <your own pak0> --with-viewmodel` stages it, and the
release before this one drew a hand-made silhouette instead because the loader
for the format did not exist.

The demo pak0 that ships the real machinegun is not redistributable, so CI can
never see it. But the loader (third_party/tinygl/q3viewmodel.c) is driven by
the CONTENTS of the zip, not by which zip it is — so this suite builds a small
synthetic model in id's format (scripts/build_q3pak.py), stages it through the
REAL staging script (`--with-viewmodel`), overlays it on the volume, and boots
the OS to assert:

  1. staging: q3a_data.py's --with-viewmodel put the three .md3 parts and the
     two images on the volume — the exact files q3viewmodel.c opens.
  2. loading: the loader's one-line verdict. parts=3 is body + barrel + flash,
     surf/tris are what it parsed out of the files, tex is the body's image
     size, muzzle is the tag chain (body tag_barrel + barrel tag_flash) and
     chain=1 says it AGREES with the body's own tag_flash — the self-check that
     catches a mis-parsed tag without any pixel evidence.
  2b. the command line reached the driver: the `args` line proves the token
     walk consumed the whole line, knobs included. The map name alone proves
     nothing here — `mectovtest` is also the driver's compiled-in default, so
     an argument line this port never understood runs the fixture arena and
     looks correct (a lost `=` keystroke hid behind exactly that, see RUN_KEYS).
  3. drawing: every sampled frame reports the model with `drawn` equal to the
     triangles the loader parsed MINUS the flash's share whenever the sampled
     frame was not firing — id draws the muzzle flash only on firing frames, so
     the relation is exact, not a range: the whole model reached the rasterizer,
     not just into memory. Plus a screen box in the lower-right half, and a
     distinct 4-bit colour count far above what a flat slab can produce.
  4. the module's own fire path: the scheduled fire bursts put BUTTON_ATTACK
     through PM_Weapon (ammo_mg below the spawn 100) and the viewmodel line's
     sticky latch records WEAPON_FIRING, the only state that draws the flash.
     `flash` (live) and `latched` (sticky) are separate fields because they
     answer different questions: what was drawn this frame, and whether the
     state was ever reached at all.
  5. clean exit: ESC ends the session, the OS survives.

The fixture-arena regression (with whatever model the developer's own volume
carries, or none) stays in q3jump_test.py and q3arena_test.py; this suite is
the deterministic one, on data it built itself.

Usage:
    python3 scripts/q3viewmodel_test.py [--timeout 600] [--iso mectov.iso]
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

SERIAL_LOG = "/tmp/mectov_q3viewmodel_serial.log"
MON_SOCK = "/tmp/mectov_q3viewmodel_monitor.sock"
CURSOR_PPM = "/tmp/mectov_q3viewmodel_cursor.ppm"
SHOT_A = "/tmp/q3viewmodel_a.ppm"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
# `q3arena mectovtest fire=5` — five scheduled fire bursts, so BUTTON_ATTACK
# reaches id's own PM_Weapon whether or not a human is driving.
#
# `=` is "equal" in QEMU's key-name table, and NOT the character itself: the
# first version of this list sent `sendkey =`, which the monitor rejects as an
# unknown key — silently, on a monitor socket nobody reads back — so the
# terminal received `fire3`, the driver's argument walk aborted on that word
# (by design: a half-understood argument may never half-configure a run) and the
# run fell back to the DEFAULT map name and no fire bursts at all. The suite
# then failed three assertions later on `drawn`, three steps from the cause; the
# args-line check below is what now fails first, at the keystroke.
# The burst count is 5 rather than 3 for one measurable reason: the diag line
# samples every 100th frame, and a burst is 20 frames starting on that grid's
# own 40-frame cadence, so 3 bursts ([40,60) [79,99) [118,138)) never overlap a
# sample — the flash's 4 triangles were then only ever checked as ABSENT. The
# fifth burst opens at frame 196 and frame 200 is a sample, so the firing
# branch (`drawn` == tris) is exercised too. Frames are cheap here (the tick
# catch-up runs before the draw inside the frame), and ammo lasts: id's own
# 100 ms machinegun refire spends about half the held ticks.
RUN_KEYS = ["q", "3", "a", "r", "e", "n", "a", "spc",
            "m", "e", "c", "t", "o", "v", "t", "e", "s", "t",
            "spc", "f", "i", "r", "e", "equal", "5", "ret"]

START_MARKER = "[Q3ARENA] official qagame VM world rendered through TinyGL"
MESH_MARKER = "[Q3ARENA] world mesh: "
VMLOAD_MARKER = "[Q3VIEWMODEL] load base="
DONE_MARKER = "[Q3ARENA] done"
FRAME_DONE_MARKER = "[Q3VM] frame loop done"
FAILED_MARKER = "[Q3ARENA] FAILED"
PANIC_MARKER = "[PANIC]"
SYS_ERROR_MARKER = "[Q3] Sys_Error"

MAP_NAME = "mectovtest"
# What scripts/build_q3pak.py writes into the synthetic pak: a 12-triangle box
# for the body, a 12-triangle box for the barrel and a 4-triangle star for the
# flash, every one of them textured with a 64x64 eight-stripe checker.
VM_BASE = "models/weapons2/machinegun/machinegun"
VM_FILES = [
    "models/weapons2/machinegun/machinegun.md3",
    "models/weapons2/machinegun/machinegun_barrel.md3",
    "models/weapons2/machinegun/machinegun_flash.md3",
    "models/weapons2/machinegun/machinegun.jpg",
    "models/weapons2/machinegun/f_machinegun.jpg",
]
WANT_PARTS = 3
WANT_SURF = 3
WANT_FLASH_TRIS = 4       # the flash part's own triangles (id draws them only
                          # on firing frames, so `drawn` excludes them then)
WANT_TRIS = 28            # 12 + 12 + 4
WANT_TEX = 64
WANT_MUZZLE = "150,0,0"   # tag chain in tenths: (15.0, 0.0, 0.0)

ARGS_RE = re.compile(
    r"\[Q3ARENA\] args: map='(?P<map>\S+)' fullscreen=(?P<fs>\d+) "
    r"pose=(?P<pose>\d+) sort=(?P<sort>\d+) jump=(?P<jump>\d+) "
    r"fire=(?P<fire>\d+)")
VM_RE = re.compile(
    r"\[Q3ARENA\] viewmodel frame=(?P<frame>\d+) weapon=(?P<weapon>\d+) "
    r"flash=(?P<flash>\d+) latched=(?P<latched>\d+) ground=(?P<ground>\d+) "
    r"bob=(?P<bob>\d+) "
    r"model=(?P<model>\d+) surf=(?P<surf>\d+) tris=(?P<tris>\d+) "
    r"drawn=(?P<drawn>\d+) tex=(?P<tw>\d+)x(?P<th>\d+) "
    r"box=(?P<x0>-?\d+),(?P<y0>-?\d+),(?P<x1>-?\d+),(?P<y1>-?\d+) "
    r"distinct=(?P<distinct>\d+) "
    # v38.125: add = 1 when this pass submitted the flash ADDITIVELY (id's own
    # blendfunc for f_machinegun), bright/dark = near-white and near-black
    # pixels inside the box.
    r"add=(?P<add>\d+) bright=(?P<bright>\d+) dark=(?P<dark>\d+)")
VMLOAD_RE = re.compile(
    r"\[Q3VIEWMODEL\] load base=(?P<base>\S+) parts=(?P<parts>\d+) "
    r"surf=(?P<surf>\d+) tris=(?P<tris>\d+) tex=(?P<tw>\d+)x(?P<th>\d+) "
    r"muzzle=(?P<mx>-?\d+),(?P<my>-?\d+),(?P<mz>-?\d+) "
    r"chain=(?P<chain>\d+) barrel=(?P<barrel>-?\d+) flash=(?P<flash>-?\d+) "
    r"flash_tris=(?P<ftris>\d+)")
PLAY_RE = re.compile(
    r"\[Q3ARENA\] play frame=(?P<frame>\d+) up=(?P<up>-?\d+) btn=(?P<btn>\d+) "
    r"weapon=(?P<weapon>\d+) ammo_mg=(?P<ammo>-?\d+) health=(?P<health>-?\d+) "
    r"ground=(?P<ground>\d+) jumps=(?P<jumps>\d+) shots=(?P<shots>\d+) "
    r"vel_z=(?P<velz>-?\d+)")
FRAME_RE = re.compile(
    r"\[Q3ARENA\] frame=(?P<frame>\d+) t=(?P<t>\d+) "
    r"pos=\((?P<x>-?\d+),(?P<y>-?\d+),(?P<z>-?\d+)\) .*?drawn=(?P<drawn>\d+) "
    r"tris=(?P<tris>\d+)")
FRAME_PIXELS = 320 * 240


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


def wait_for_frame(n, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for m in FRAME_RE.finditer(read_file(SERIAL_LOG)):
            if int(m.group("frame")) >= n:
                return True
        if DONE_MARKER in read_file(SERIAL_LOG):
            return False
        time.sleep(1)
    return False


def mon_cmd(cmd, settle=None):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(0.15 if settle is None else settle)
        s.close()
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")


def sendkey(key):
    mon_cmd(f"sendkey {key}")


def type_line(keys, retries=3, ready_marker=None, timeout=90):
    for _ in range(retries):
        for _ in range(24):
            mon_cmd("sendkey backspace")
        for k in keys:
            mon_cmd("sendkey " + k)
            time.sleep(0.12)
        sendkey("ret")
        if wait_for_in_file(SERIAL_LOG, ready_marker, timeout):
            return True
        time.sleep(1.0)
    return False


def dump_tail(lines=30):
    for line in read_file(SERIAL_LOG).splitlines()[-lines:]:
        print(line[:150])


def screendump(path, settle=1.0):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    mon_cmd(f"screendump {path}", settle=settle)
    return os.path.exists(path) and os.path.getsize(path) > 1000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK, CURSOR_PPM, SHOT_A):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    stage = "/tmp/q3viewmodel_stage"
    pak = os.path.join(stage, "pak0.pk3")
    out = os.path.join(stage, "out")

    # ---- 0. fresh images, then the synthetic pak through the real stager ----
    err = q3_images.fresh_images(args.disk, args.ext2)
    if err:
        print(f"[FAIL] {err}")
        return 1
    subprocess.run(["rm", "-rf", stage], check=True)
    subprocess.run([sys.executable, os.path.join(here, "scripts", "build_q3pak.py"),
                    pak], check=True)
    subprocess.run([sys.executable, os.path.join(here, "scripts", "q3a_data.py"),
                    "--pak", pak, "--map", MAP_NAME, "--with-viewmodel",
                    "--out", out, "--verify"], check=True)
    baseq3 = os.path.join(out, "baseq3")

    # ---- 1. the staging decisions are the script's, not the fallback's -----
    staged = set()
    for dirpath, _dirs, files in os.walk(baseq3):
        for fn in files:
            rel = os.path.relpath(os.path.join(dirpath, fn), baseq3)
            staged.add(rel.replace(os.sep, "/"))
    missing = [f for f in VM_FILES if f not in staged]
    if missing:
        print("[FAIL] --with-viewmodel did not stage: " + ", ".join(missing))
        return 1
    print("[OK] staging through q3a_data.py --with-viewmodel: the three .md3 "
          "parts and both images are on the staged tree (%d files)" % len(staged))

    # ---- 2. seed the volume, then overlay the STAGED tree on top -----------
    err = q3_images.seed_volume(args.ext2)
    if err:
        print(f"[FAIL] {err}")
        return 1
    # The rm matters: seed_ext2.sh has just mirrored the developer's own
    # build/q3data over the volume, and on a machine where q3a_data.py staged
    # the demo pak that tree ALREADY has models/weapons2/machinegun/ — where
    # debugfs's `write` refuses an existing name ("Ext2 file already exists")
    # and this suite would silently test the retail model instead of the one it
    # built. The overlay is exactly what q3retail_test.py does, plus the rm its
    # fixture never needed. (It failed the honest way first: the loader reported
    # the demo's tris=267 where the synthetic model has 28.)
    for rel in sorted(staged):
        d = os.path.dirname(rel)
        if d and d != ".":
            subprocess.run(["debugfs", "-w", "-R", f"mkdir /baseq3/{d}",
                            args.ext2], check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["debugfs", "-w", "-R", f"rm /baseq3/{rel}",
                        args.ext2], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["debugfs", "-w", "-R",
                        f"write {os.path.join(baseq3, rel)} /baseq3/{rel}",
                        args.ext2], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("[OK] volume seeded and the staged view model overlaid (%d file(s))"
          % len(staged))

    qemu = subprocess.Popen([
        "qemu-system-i386",
        "-cpu", "qemu32,+nx",
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
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached the login screen")
            dump_tail()
            return 1
        for k in LOGIN_KEYS:
            sendkey(k)
            time.sleep(0.15)
        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete")
            return 1
        print("[OK] booted and logged in")

        time.sleep(1.5)
        if not terminal_launch.launch_terminal(mon_cmd, SERIAL_LOG, CURSOR_PPM):
            print("[FAIL] the Terminal never became ready")
            return 1
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] terminal never became ready")
            return 1
        time.sleep(1.0)

        # ---- 3. run the map with scheduled fire bursts ---------------------
        if not type_line(RUN_KEYS, retries=2, ready_marker=START_MARKER,
                         timeout=180):
            print("[FAIL] the `q3arena mectovtest fire=5` command never "
                  "started its task")
            dump_tail()
            return 1
        print("[OK] q3arena mectovtest fire=5 started")

        # ---- 3b. the command line REACHED the driver -----------------------
        # The map name is no evidence on its own: `mectovtest` is also the
        # driver's compiled-in default, so a command line this port never
        # understood runs the fixture arena anyway and looks fine (that is how
        # a lost `=` keystroke hid here — see RUN_KEYS). The args line is the
        # only proof the token walk consumed the whole line, knobs included.
        if not wait_for_in_file(SERIAL_LOG, "[Q3ARENA] args:", 30):
            print("[FAIL] the driver never printed its args line — the "
                  "argument walk aborted, so the map name and the knobs are "
                  "both defaults")
            dump_tail()
            return 1
        am = ARGS_RE.search(read_file(SERIAL_LOG))
        if not am:
            print("[FAIL] the args line is not in the documented shape")
            dump_tail()
            return 1
        got = (am.group("map"), int(am.group("jump")), int(am.group("fire")))
        if got != (MAP_NAME, 0, 5):
            print(f"[FAIL] args: map='{got[0]}' jump={got[1]} fire={got[2]}, "
                  f"wanted map='{MAP_NAME}' jump=0 fire=5")
            dump_tail()
            return 1
        print("[OK] the command line reached the driver whole: "
              f"map='{got[0]}' fire={got[2]}")
        if not wait_for_in_file(SERIAL_LOG, MESH_MARKER, 90):
            print("[FAIL] the world mesh was never built")
            dump_tail()
            return 1

        # ---- 4. the loader's own verdict ----------------------------------
        # The COMPLETE line, not its head. v38.157's serial writer appends to
        # a 16 KB ring that the timer tick drains, so a poller can grab the
        # file while the tail of a line is still queued — the head arrives
        # first, and a parser that fires on the marker alone reads a half-line
        # and fails on its own timing. Seen exactly that on this suite (4 Oct
        # run): the log ended at `base=models` and this parser reported "not
        # in the documented shape" about a line the guest had written whole.
        # The writer emits the line in ONE call with the newline last, so the
        # terminating newline is the completeness test.
        complete = re.compile(re.escape(VMLOAD_MARKER) + r"[^\n]*\n")
        deadline = time.time() + 120
        while time.time() < deadline:
            if complete.search(read_file(SERIAL_LOG)):
                break
            time.sleep(1)
        if not complete.search(read_file(SERIAL_LOG)):
            if VMLOAD_MARKER in read_file(SERIAL_LOG):
                print("[FAIL] the [Q3VIEWMODEL] line never finished arriving "
                      "(the log has its head but no newline after it)")
            else:
                print("[FAIL] the view model loader never reported anything "
                      "— the [Q3VIEWMODEL] line is missing entirely")
            dump_tail()
            return 1
        ld = [m.groupdict() for m in
              VMLOAD_RE.finditer(read_file(SERIAL_LOG))]
        if not ld:
            print("[FAIL] the [Q3VIEWMODEL] line is not in the documented "
                  "shape")
            dump_tail()
            return 1
        ld = ld[0]
        raw_line = [ln for ln in read_file(SERIAL_LOG).splitlines()
                    if VMLOAD_MARKER in ln]
        raw_line = raw_line[-1][:200] if raw_line else ""
        chain = [
            ("base", ld["base"], VM_BASE),
            ("parts", int(ld["parts"]), WANT_PARTS),
            ("surf", int(ld["surf"]), WANT_SURF),
            ("tris", int(ld["tris"]), WANT_TRIS),
            ("tex", (int(ld["tw"]), int(ld["th"])), (WANT_TEX, WANT_TEX)),
            ("muzzle", f"{ld['mx']},{ld['my']},{ld['mz']}", WANT_MUZZLE),
            ("chain", int(ld["chain"]), 1),
            ("barrel", int(ld["barrel"]), 0),
            ("flash", int(ld["flash"]), 0),   # 0 = the part loaded
            ("flash_tris", int(ld["ftris"]), WANT_FLASH_TRIS),
        ]
        for name, got, want in chain:
            if got != want:
                print(f"[FAIL] the loader's {name} is {got}, expected {want}")
                print(f"       loader line: {raw_line}")
                dump_tail(20)
                return 1
        print("[OK] the loader parsed the staged model: parts=%d surf=%d "
              "tris=%d tex=%dx%d muzzle=%s chain=%d (the tag chain agrees with "
              "the body's own tag_flash)" % (WANT_PARTS, WANT_SURF, WANT_TRIS,
                                             WANT_TEX, WANT_TEX, WANT_MUZZLE,
                                             int(ld["chain"])))

        # ---- 5. it is drawn, whole, in the lower right ---------------------
        deadline = time.time() + args.timeout
        rows = []
        while time.time() < deadline:
            rows = [m.groupdict() for m in
                    VM_RE.finditer(read_file(SERIAL_LOG))]
            if len(rows) >= 3:
                break
            if PANIC_MARKER in read_file(SERIAL_LOG) or \
                    SYS_ERROR_MARKER in read_file(SERIAL_LOG):
                break
            time.sleep(1)
        if len(rows) < 2:
            print(f"[FAIL] only {len(rows)} viewmodel sample(s) in the log")
            dump_tail(30)
            return 1

        bad = [r for r in rows if r["weapon"] != "2"]
        if bad:
            print(f"[FAIL] {len(bad)} sample(s) do not report the machinegun: "
                  f"{bad[0]}")
            return 1
        wrong_model = [r for r in rows if int(r["model"]) != WANT_PARTS or
                       int(r["surf"]) != WANT_SURF]
        if wrong_model:
            print(f"[FAIL] {len(wrong_model)} sample(s) report the wrong model "
                  f"(model/surf): {wrong_model[0]}")
            return 1
        # The pass must submit every triangle the loader parsed — except the
        # flash's, which id draws only on firing frames. The relation is
        # therefore exact, not a range, and `flash` here is the LIVE state that
        # decided it (`latched` is the sticky fire-path flag, next step).
        def want_drawn(r):
            return WANT_TRIS - (0 if r["flash"] == "1" else WANT_FLASH_TRIS)

        not_drawn = [r for r in rows if int(r["drawn"]) != want_drawn(r)]
        if not_drawn:
            print(f"[FAIL] {len(not_drawn)} sample(s) submitted the wrong "
                  f"triangle count: drawn={not_drawn[0]['drawn']} "
                  f"(flash={not_drawn[0]['flash']}) against tris={WANT_TRIS} "
                  f"minus {WANT_FLASH_TRIS} when not firing")
            print(f"       {not_drawn[0]}")
            return 1
        firing_rows = [r for r in rows if r["flash"] == "1"]
        if not firing_rows:
            # A hard failure, not a note: with fire=5 a burst is open on the
            # frame-200 sample by construction, so "no firing sample" means the
            # live flash state is not reaching this line at all.
            print("[FAIL] no sampled frame was inside a fire window (flash=0 "
                  "on all %d samples), so the half of the drawn-count check "
                  "that covers the flash never ran" % len(rows))
            dump_tail(20)
            return 1
        print("[OK] a sampled frame was inside a burst: frame=%s reads "
              "flash=1 with drawn=%s (flash state and geometry agree)"
              % (firing_rows[0]["frame"], firing_rows[0]["drawn"]))
        offscreen = [r for r in rows
                     if not (int(r["x1"]) > int(r["x0"]) and
                             int(r["y1"]) > int(r["y0"]))]
        if offscreen:
            print(f"[FAIL] {len(offscreen)} sample(s) report an empty screen "
                  f"box: {offscreen[0]}")
            return 1
        wrong_half = [r for r in rows if int(r["x0"]) <= 320 // 2]
        if wrong_half:
            print(f"[FAIL] the model is not in the right half of the view: "
                  f"{wrong_half[0]}")
            return 1
        flat = [r for r in rows if int(r["distinct"]) < 8]
        if flat:
            print(f"[FAIL] {len(flat)} sample(s) show fewer than 8 distinct "
                  f"colours inside the model's box — that is a flat fill, not "
                  f"a textured surface: {flat[0]}")
            return 1
        box = rows[-1]
        print("[OK] the model is drawn whole and textured: %d sample(s), every "
              "one model=%d tris=%d drawn=%d (the flash part, %d tri, only on "
              "firing frames), box (%s,%s)-(%s,%s) in the right half, %s "
              "distinct colours" %
              (len(rows), WANT_PARTS, WANT_TRIS, int(box["drawn"]),
               WANT_FLASH_TRIS, box["x0"], box["y0"],
               box["x1"], box["y1"], box["distinct"]))

        # ---- 6. id's own fire path reached WEAPON_FIRING -------------------
        # The flash is drawn only inside that window, and the 100-frame sample
        # grid cannot be relied on to land in a 20-frame burst — so the driver
        # latches "a tick ran id's own fire path" into `latched` (sticky), and
        # that latch is this suite's evidence the state existed. ammo_mg below
        # the 100 spawn is PM_Weapon's own accounting of the same shots.
        fired = None
        deadline = time.time() + args.timeout
        while time.time() < deadline and fired is None:
            plays = [m.groupdict() for m in
                     PLAY_RE.finditer(read_file(SERIAL_LOG))]
            for p in plays:
                if int(p["ammo"]) < 100 and int(p["shots"]) >= 1:
                    fired = p
                    break
            if fired is None:
                time.sleep(1)
        latched = [r for r in rows if r["latched"] == "1"]
        if fired is None:
            print("[FAIL] the scheduled bursts never took ammo — BUTTON_ATTACK "
                  "did not reach PM_Weapon")
            dump_tail(20)
            return 1
        if not latched:
            print("[FAIL] no sample carried the WEAPON_FIRING latch — the "
                  "muzzle flash's window was never entered, yet ammo was spent")
            dump_tail(20)
            return 1
        print("[OK] the module's own fire path ran: ammo_mg=%s (spawn 100) "
              "shots=%s, and the viewmodel latch recorded WEAPON_FIRING at "
              "frame=%s — the only state that draws the flash; the flash's "
              "%d triangles were the ones `drawn` held back on every "
              "non-firing sample"
              % (fired["ammo"], fired["shots"], latched[0]["frame"],
                 WANT_FLASH_TRIS))

        # ---- 6b. and it was COMPOSITED the way id composites it ------------
        # v38.125. `drawn` (section 5) answers "were the flash's triangles
        # submitted"; this answers "how were they blended", which is the part
        # that was wrong: the flash's image is a bright star on BLACK, and
        # submitted opaque its surround covered the view (the user's screenshot
        # was a black disc over the arena). id's definition says
        # `sort additive` + `blendfunc GL_ONE GL_ONE` + `cull disable`.
        #
        # The invariant a pixel test can hold: ADDITIVE COMPOSITING CAN ONLY
        # ADD LIGHT. The arena's own faces are all well above black (its
        # darkest texture is (8,56,64) at the shading floor, ~51/765), so a
        # near-black pixel inside the model's box means something painted
        # black there — which is exactly the opaque flash and nothing else in
        # this map. `add` is the pass's own account of the mode it used, so the
        # two halves check each other: the state says additive, the pixels say
        # nothing got darker.
        firing = [r for r in rows if r["flash"] == "1"]
        idle = [r for r in rows if r["flash"] == "0"]
        mislabelled = [r for r in rows if r["add"] != r["flash"]]
        if mislabelled:
            print(f"[FAIL] {len(mislabelled)} sample(s) report flash="
                  f"{mislabelled[0]['flash']} but add={mislabelled[0]['add']} — "
                  f"a flash was submitted with the wrong blend, or blend state "
                  f"was left on with no flash drawn")
            return 1
        dark = [r for r in rows if int(r["dark"]) > 0]
        if dark:
            print(f"[FAIL] {len(dark)} sample(s) left near-black pixels inside "
                  f"the model's box (worst: frame {dark[0]['frame']} "
                  f"dark={dark[0]['dark']} of box ({dark[0]['x0']},"
                  f"{dark[0]['y0']})-({dark[0]['x1']},{dark[0]['y1']})) — "
                  f"additive blending can only add light, so something painted "
                  f"BLACK over the view: the muzzle flash drawn opaque")
            return 1
        if not firing or not idle:
            print(f"[FAIL] the sample grid never caught both states: "
                  f"{len(firing)} firing, {len(idle)} idle sample(s) — the "
                  f"comparison this section is built on needs both")
            dump_tail(20)
            return 1
        if max(int(r["bright"]) for r in firing) <= 0:
            print("[FAIL] no firing sample has a single near-white pixel inside "
                  "the box — the flash contributed no light at all")
            return 1
        print("[OK] the flash is composited additively, id's way: %d firing "
              "sample(s) each report add=1 with 0 near-black pixels in the box "
              "(bright=%s) against %d idle sample(s) with add=0 (bright=%s) — "
              "an opaque flash painted its black surround over the arena, which "
              "is what `dark` counts"
              % (len(firing),
                 max(int(r["bright"]) for r in firing), len(idle),
                 max(int(r["bright"]) for r in idle)))

        # ---- 7. the world still renders and the OS survives ---------------
        if not wait_for_frame(60, 180):
            print("[FAIL] the renderer never reached frame 60")
            dump_tail(30)
            return 1
        frames = [m.groupdict() for m in FRAME_RE.finditer(read_file(SERIAL_LOG))]
        peak = max(int(f["drawn"]) for f in frames)
        if peak < 2:
            print(f"[FAIL] the most faces a frame drew was {peak} — the view "
                  f"model pass broke the world pass")
            return 1
        if screendump(SHOT_A):
            print(f"     screendump for humans: {SHOT_A}")
        print(f"[OK] the world is still drawn alongside it (peak {peak} faces "
              f"in one frame)")

        sendkey("esc")
        if not wait_for_in_file(SERIAL_LOG, DONE_MARKER, args.timeout):
            print("[FAIL] the session never finished")
            dump_tail(40)
            return 1
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited with code {qemu.returncode}")
            return 1
        print("[OK] ESC ended the session, the OS stayed alive")
        print("[PASS] id's own weapon view model — .md3 geometry, its textures, "
              "the tag-chained muzzle flash, driven by the module's own "
              "playerState — drawn in Mectov OS")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
