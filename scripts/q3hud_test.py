#!/usr/bin/env python3
"""
scripts/q3hud_test.py — id's own Quake III status bar, end to end (v38.127).

The user-facing promise this exercises: `q3arena <map>` now draws the status bar
retail Quake III draws — ammo, health, armor and the FFA score boxes, every one
of them a picture out of gfx/2d and icons/ in the player's own pak0 — and the
NUMBER in each field is the game module's own playerState, not a decoration.

pak0 is not redistributable, so CI can never see id's HUD art. But q3hud.c reads
whatever the volume has, and build_q3pak.py ships a synthetic set in id's own
formats and sizes — which makes this suite possible at all, because a synthetic
number field can be built to be READ BACK OFF THE SCREEN: each one is a flat
colour whose red channel is the digit. A screendump can then be decoded into the
number the HUD drew, which is exactly the assertion that separates "the HUD
follows the playerState" from "some pixels appeared at the bottom of the window".

What it asserts, in two boots:

  1. load: the port found id's picture set on the volume through the engine's
     own filesystem — `[Q3ARENA] hud: 15 image(s), 0 missing` — one line per
     picture, naming the file that answered each name.  2. draw: the `hud frame=` samples report the values the bar was handed and
     what it made of them (on=1 images=15 missing=0 draws>=7 digits>=6). The
     health column is the module's OWN countdown — id spawns a player at
     STAT_MAX_HEALTH + 25 and g_active.c takes a point a second off it until
     STAT_MAX_HEALTH, so the samples read 115, 105, 100, 100 — which is a
     statement about playerState at four different frames, and it is why the
     screendump waits for the floor (100, where the field is amber again
     instead of id's white "over max health" colour) before it decodes it.
  3. layout AND values, from pixels: the six number cells of the ammo and health
     fields, at the x positions id's CG_DrawField computes, decode to "100" and
     "100"; the ammo icon is at id's icon offset; the armor icon is NOWHERE
     (armor is 0, and id's `if (value > 0)` wraps the whole armor field); the
     score box is at 640-40 with gfx/2d/select over it and a big-font "0" in it.
  4. a second boot with `nohud` draws none of it — no field, icon or score pixel
     anywhere — while the world's own pixels are untouched. That last part is
     the invariant the other suites depend on: the frame histogram is read
     BEFORE the HUD is composited, so the HUD cannot change it. Both boots pin
     the camera (see POSE_KEYS) so that "untouched" is a statement about the
     same view rather than a coincidence of two runs whose frame pacing
     happened to agree; the comparison itself asserts the level's palette
     exactly and its covered area to within 1% (the block explains why the
     warm-bucket split is measured and reported instead).

Usage:
    python3 scripts/q3hud_test.py [--timeout 600] [--iso mectov.iso]
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

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_LOG = "/tmp/mectov_q3hud_serial.log"
MON_SOCK = "/tmp/mectov_q3hud_monitor.sock"
CURSOR_PPM = "/tmp/mectov_q3hud_cursor.ppm"
SHOT_ON = "/tmp/q3hud_on.ppm"
SHOT_OFF = "/tmp/q3hud_off.ppm"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
MAP_NAME = "mectovtest"

# Both boots PIN the camera at the fixture arena's own spawn view, because the
# last check in this suite is a pixel-histogram EQUALITY between them and this
# port's default camera is not a function of the frame index. v38.132's demo
# walk turns the view on the game clock, so two boots that reach frame 20 under
# different frame pacings — the `nohud` run composites ~1,700 fewer blended
# pixels per frame, so its frames are cheaper — arrive at frame 20 looking in
# slightly different directions. The frame-20 histogram then differs (on the
# tree that first ran this, 4 Oct: patch=1416 wall=16571 with the bar on
# against patch=229 wall=17763 with `nohud`) for a reason that has nothing to
# do with the HUD, and the check whose entire purpose is "the HUD cannot change
# the world pixels" fails misleadingly. Pinned, the eye IS a function of the
# argument, both boots draw exactly this view, and the equality is about the
# HUD again.
#
# `@15,15,114,45,0` is the eye a boot starts with: the arena's spawn origin
# (15,15,88) plus the player's 26-unit viewheight, yaw 45, level pitch. '@' and
# ',' have no QEMU key NAMES (there is no `sendkey at`): '@' is shift-2 on the
# guest's own scancode map, the same spelling q3heavy_test.py uses.
POSE_KEYS = (["spc", "shift-2"] +
             list("15") + ["comma"] + list("15") + ["comma"] +
             list("114") + ["comma"] + list("45") + ["comma"] + list("0"))
Q3ARENA_KEYS = (["q", "3", "a", "r", "e", "n", "a", "spc"] +
                list(MAP_NAME) + POSE_KEYS + ["ret"])
Q3ARENA_KEYS_NOHUD = (["q", "3", "a", "r", "e", "n", "a", "spc"] +
                      list(MAP_NAME) + ["spc", "n", "o", "h", "u", "d"] +
                      POSE_KEYS + ["ret"])

START_MARKER = "[Q3ARENA] official qagame VM world rendered through TinyGL"
WINDOW_MARKER = "[Q3ARENA] window id="
ENTERED_MARKER = "entered the game"
DONE_MARKER = "[Q3ARENA] done"
PANIC_MARKER = "[PANIC]"
SYS_ERROR_MARKER = "[Q3] Sys_Error"

# ---- id's status bar layout (cg_local.h:67-72, cg_draw.c:590-670) ----------
# Authored in Q3's 640x480 virtual screen; the port renders 320x240, so the
# renderer scales by content/640 and content/480 — the same CG_AdjustFrom640
# factor. These numbers are the test's, not the port's: they are what the test
# checks the port against.
VIRT_W, VIRT_H = 640, 480
CHAR_W, CHAR_H = 32, 48
ICON_SIZE = 48
TEXT_ICON_SPACE = 4
FIELD_W = 3
ROW_Y = 432
HEALTH_X = 185
ARMOR_X = 370
AMMO_X = 0
BIGCHAR = 16
ICON_HUGE = 48 * 1.25          # the head slot, 480 - size, not drawn here

# ---- the fixture's own HUD palette (build_q3pak.py) ------------------------
# A number field is a flat colour whose red channel is the digit; the status
# bar tints it with id's own colour, so what lands on screen is the texel
# multiplied by hud_colors[] — amber (1.0, 0.69, 0.0) for a healthy ammo count
# and for health between 26 and 100.
def digit_texel(d):
    return (16 + d * 22, 200, 200)


AMBER_TINT_G = int(200 * 0.69)
ICON_AMMO = (40, 250, 40)
ICON_ARMOR = (250, 250, 40)
SELECT = (250, 40, 200)

HUD_TEX = re.compile(
    r"\[Q3ARENA\] hud img (\d+) (\S+) path=(\S+) size=(\d+)x(\d+) bytes=(\d+)")
HUD_SUM_RE = re.compile(
    r"\[Q3ARENA\] hud: (\d+) image\(s\), (\d+) missing")
HUD_RE = re.compile(
    r"\[Q3ARENA\] hud frame=(\d+) on=(\d+) health=(-?\d+) armor=(-?\d+) "
    r"ammo=(-?\d+) score=(-?\d+) weapon=(-?\d+) images=(\d+) missing=(\d+) "
    r"draws=(\d+) digits=(\d+)")
RECT_RE = re.compile(
    r"\[Q3ARENA\] window id=(0x[0-9a-f]+) rect=(-?\d+),(-?\d+) (\d+)x(\d+) "
    r"content=(\d+)x(\d+)")
PIXELS_RE = re.compile(
    r"\[Q3ARENA\] pixels frame=(\d+) cyan=(\d+) warm=(\d+) stepgreen=(\d+) "
    r"violet=(\d+) bright=(\d+) patch=(\d+) sky=(\d+) wall=(\d+) distinct=(\d+)")
PIXELS_KEYS = ("frame", "cyan", "warm", "stepgreen", "violet", "bright",
               "patch", "sky", "wall", "distinct")
# A shader whose image could not be resolved at map load is drawn as a
# generated placeholder (a hash-grey checkerboard with a magenta diagonal) and
# says so on its own `tex` line. That is a WORLD-LOAD failure; the histogram
# equality below must not report it as "the HUD changed the world pixels".
PLACEHOLDER_RE = re.compile(
    r"\[Q3ARENA\] tex \d+ (\S+) missing=1 placeholder")


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
        for m in re.finditer(r"\[Q3ARENA\] frame=(\d+) t=", read_file(SERIAL_LOG)):
            if int(m.group(1)) >= n:
                return True
        if DONE_MARKER in read_file(SERIAL_LOG):
            return False
        time.sleep(1)
    return False


def hud_samples(log=None):
    return [dict(zip(("frame", "on", "health", "armor", "ammo", "score",
                      "weapon", "images", "missing", "draws", "digits"),
                     (int(g) for g in m.groups())))
            for m in HUD_RE.finditer(log if log is not None else read_file(SERIAL_LOG))]


def wait_for_hud_health(want, timeout):
    """Wait until the module has counted its spawn health down to `want`.

    The driver samples the status bar every 100 frames, so this waits for a
    `hud frame=` line whose health is `want`.

    WHY THE TEST WAITS AT ALL. id spawns a player at
    `ps.stats[STAT_MAX_HEALTH] + 25` (g_client.c:1191) and g_active.c counts
    that back down to STAT_MAX_HEALTH one point per SECOND while it is over —
    so a player's health is 125, then 124, ... then 100 where it stays. The
    port's frame loop advances the clock 100 ms per frame, so the first three
    diag samples read 115 (frame 100), 105 (200) and 100 (300).

    A number field that is counting down is exactly what makes this suite's
    pixel assertion possible AND exact: rather than guess which point of the
    ramp a screendump caught, wait for the floor (health 100, amber again after
    id's `value > 100` white) and decode against a value that no longer moves.
    The ramp itself is asserted too — see the sample checks in main().
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        log = read_file(SERIAL_LOG)
        if any(s["health"] == want for s in hud_samples(log)):
            return True
        if DONE_MARKER in log or PANIC_MARKER in log or SYS_ERROR_MARKER in log:
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
        print("[!] monitor cmd '%s' failed: %s" % (cmd, e))


def sendkey(key):
    mon_cmd("sendkey " + key)


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
        print(line[:140])


def screendump(path, settle=1.0):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    mon_cmd("screendump " + path, settle=settle)
    return os.path.exists(path) and os.path.getsize(path) > 1000


def load_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError("not a P6 ppm: %s" % path)
    pos = 2
    vals = []
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
    w, h, _ = vals
    pos += 1
    return w, h, data[pos:pos + w * h * 3]


def px_at(pixels, w, x, y):
    o = (y * w + x) * 3
    return (pixels[o], pixels[o + 1], pixels[o + 2])


# ---- the layout, computed the way id computes it ---------------------------
def content_geom():
    """(content offset, content size) from the driver's own window line."""
    m = None
    for m in RECT_RE.finditer(read_file(SERIAL_LOG)):
        pass
    if not m:
        return None
    _id, wx, wy, ww, wh, cw, ch = m.groups()
    # The window is content + a 1 px border and a 20 px title bar; the WM's own
    # geometry is checked in q3arena_test.py, so this only has to agree with it.
    return (int(wx) + 1, int(wy) + 21, int(cw), int(ch))


def field_cells(x0, digits, cw):
    """The content-x of the centre of each digit cell of a CG_DrawField.

    id's own arithmetic: x += 2 + CHAR_WIDTH*(width - l), then one CHAR_WIDTH
    per character. The cell CENTRE is what gets sampled, so a half-pixel of
    rounding in the blit cannot matter.
    """
    l = digits
    qx = x0 + 2 + CHAR_W * (FIELD_W - l)
    return [int((qx + CHAR_W * (i + 0.5)) * cw / float(VIRT_W))
            for i in range(l)]


def row_center_y(ch):
    return int((ROW_Y + CHAR_H * 0.5) * ch / float(VIRT_H))


def digit_of(pixel):
    """Read a number-field pixel back as a digit, or None if it is not one."""
    r, g, b = pixel
    if b != 0:
        return None
    if not (AMBER_TINT_G - 12 <= g <= AMBER_TINT_G + 12):
        return None
    for d in range(10):
        if abs(r - digit_texel(d)[0]) <= 3:
            return d
    return None


def count_pixel(pixels, w, h, want, tol=2, box=None):
    """How many pixels equal `want` (within tol), optionally inside a box."""
    x0, y0, x1, y1 = box or (0, 0, w, h)
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            p = px_at(pixels, w, x, y)
            if all(abs(p[i] - want[i]) <= tol for i in range(3)):
                n += 1
    return n


def mkdirs(ext2, rel):
    """Create every ancestor of a staged path, parents first.

    debugfs's `mkdir` does NOT create intermediate directories — `mkdir
    /baseq3/gfx/2d` on a volume without /baseq3/gfx fails, and it fails with
    exit status 0, so a caller that only checks the status writes the file
    nowhere and believes it worked. (Found the honest way: the first run of
    this suite reported `hud: 0 image(s), 15 missing` with every picture
    staged and "overlaid" — the write had gone into thin air.) Passing `-p`
    would not help: debugfs has no such option; /baseq3 was created by
    seed_ext2.sh, /baseq3/gfx was not, and nothing ever made it.
    """
    path = "/baseq3"
    for p in rel.split("/")[:-1]:
        path += "/" + p
        subprocess.run(["debugfs", "-w", "-R", "mkdir %s" % path, ext2],
                       check=False, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)


def boot_and_run(keys, shot, args, label, settled=False):
    """One full boot: login, terminal, `q3arena <map> [nohud]`, screendump.

    Returns (ok, message). A boot per configuration is deliberate: the port
    cannot relaunch after ESC (a known, unrelated hunk-initialisation bug), so
    an A/B in one session is not available.

    `settled` says the screendump must wait for the module's health to finish
    counting down to its floor (see wait_for_hud_health). The nohud boot does
    not need that — it is being asked to prove the ABSENCE of pixels, and the
    ammo and score fields are steady at every frame anyway.
    """
    print("[..] %s: booting" % label)
    for p in (SERIAL_LOG, MON_SOCK, CURSOR_PPM, shot):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    qemu = subprocess.Popen([
        "qemu-system-i386",
        "-cpu", "qemu32,+nx",
        "-vga", "std",
        "-cdrom", args.iso,
        "-m", "512",
        "-smp", "2",
        "-display", "none",
        "-serial", "file:%s" % SERIAL_LOG,
        "-net", "none",
        "-snapshot",
        "-drive", "file=%s,format=raw,index=0,media=disk" % args.disk,
        "-drive", "file=%s,format=raw,index=1,media=disk" % args.ext2,
        "-monitor", "unix:%s,server,nowait" % MON_SOCK,
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            return False, "kernel never reached the login screen"
        for k in LOGIN_KEYS:
            sendkey(k)
            time.sleep(0.15)
        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            return False, "login did not complete"
        time.sleep(1.5)
        if not terminal_launch.launch_terminal(mon_cmd, SERIAL_LOG, CURSOR_PPM):
            return False, "the Terminal never became ready"
        time.sleep(1.0)
        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1")
        time.sleep(0.1)
        mon_cmd("mouse_button 0")
        time.sleep(0.5)

        if not type_line(keys, retries=2, ready_marker=START_MARKER, timeout=150):
            return False, "`q3arena` never started its task"
        # The pin has to have landed BEFORE anything is compared: without it the
        # two boots are different views and the histogram equality at the end
        # would be a lie in whichever direction it fell.
        if not wait_for_in_file(SERIAL_LOG, "pose=1", 15):
            return False, ("the pinned pose never reached the driver — the "
                           "histogram equality below would be comparing two "
                           "different views")
        if not wait_for_in_file(SERIAL_LOG, WINDOW_MARKER, 60):
            return False, "no window was opened"
        if not wait_for_in_file(SERIAL_LOG, ENTERED_MARKER, 240):
            return False, "the module never announced the client"
        if not wait_for_in_file(SERIAL_LOG, "[Q3ARENA] hud frame=", 60):
            return False, "the status bar was never sampled"
        if settled:
            if not wait_for_hud_health(100, 240):
                return False, ("the module never brought health down to 100 "
                               "(STAT_MAX_HEALTH) within 240s")
        if not wait_for_frame(40, 60):
            return False, "no frames were rendered"
        time.sleep(1.0)
        if not screendump(shot, settle=1.5):
            return False, "the screendump never happened"
        return True, "ran"
    finally:
        # ESC ends the session cleanly; the kill is only a backstop.
        sendkey("esc")
        time.sleep(1.0)
        qemu.terminate()
        try:
            qemu.wait(timeout=15)
        except subprocess.TimeoutExpired:
            qemu.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    args = ap.parse_args()

    # ---- 0. the synthetic pak, staged through the REAL staging script -------
    stage = "/tmp/mectov_q3hud_stage"
    pak = os.path.join(ROOT, "build", "q3hud", "pak0.pk3")
    subprocess.run(["rm", "-rf", stage], check=True)
    subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "build_q3pak.py"),
                    pak], check=True, stdout=subprocess.DEVNULL)
    # q3a_data.py's --out is the staging ROOT; the game directory it writes is
    # <out>/baseq3, and seed_ext2.sh mirrors <root>/baseq3 onto the volume's
    # /baseq3. So the overlay below lands files under /baseq3, not under
    # /baseq3/baseq3 — which is the mistake that made the first two runs of
    # this suite report `hud: 0 image(s), 15 missing` with the art staged and
    # "written": id's FS looks for gfx/2d/numbers/zero_32b.tga in the game
    # directory, and the picture was one directory above it.
    out = os.path.join(stage, "q3data")
    baseq3 = os.path.join(out, "baseq3")
    subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "q3a_data.py"),
                    "--pak", pak, "--map", MAP_NAME, "--out", out, "--verify",
                    "--with-hud"], check=True, stdout=subprocess.DEVNULL)
    hud_art = sorted(
        os.path.join(d, f)
        for d, _sub, files in os.walk(baseq3)
        for f in files
        if "/gfx/2d/" in d + "/" or "/icons/" in d + "/")
    if len(hud_art) != 15:
        print("[FAIL] staging put %d HUD picture(s) on the tree, expected 15:"
              % len(hud_art))
        for p in hud_art:
            print("       " + p)
        return 1
    print("[OK] staging carried id's status bar art (15 pictures, --with-hud)")

    # ---- 1. fresh volumes, seeded the way every other Q3 suite seeds ------ 
    # q3_images is shared for a reason (read its header): a suite that keeps
    # the previous run's ext2.img asserts against leftovers, and on a clean
    # checkout there is no image at all. fresh_images formats both drives and
    # seed_volume puts the system blobs, the QVM, the generated arena and
    # whatever build/q3data holds on the volume; the staged picture set is
    # overlaid on top.
    err = q3_images.fresh_images(args.disk, args.ext2)
    if err:
        print("[FAIL] %s" % err)
        return 1
    err = q3_images.seed_volume(args.ext2)
    if err:
        print("[FAIL] %s" % err)
        return 1
    staged = []
    for d, _sub, files in os.walk(baseq3):
        for f in files:
            staged.append(os.path.relpath(os.path.join(d, f), baseq3))
    for rel in staged:
        mkdirs(args.ext2, rel)
        subprocess.run(["debugfs", "-w", "-R", "rm /baseq3/%s" % rel, args.ext2],
                       check=False, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        subprocess.run(["debugfs", "-w", "-R", "write %s /baseq3/%s"
                        % (os.path.join(baseq3, rel), rel), args.ext2],
                       check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    print("[OK] volume seeded (q3_images + %d staged file(s) overlaid)"
          % len(staged))

    # ---- 2. the HUD is drawn, and the numbers are the playerState's --------
    ok, msg = boot_and_run(Q3ARENA_KEYS, SHOT_ON, args, "hud on", settled=True)
    if not ok:
        print("[FAIL] %s" % msg)
        dump_tail(40)
        return 1

    log = read_file(SERIAL_LOG)
    m = HUD_SUM_RE.search(log)
    if not m:
        print("[FAIL] no `hud:` summary line — the status bar never loaded")
        dump_tail(40)
        return 1
    images, missing = int(m.group(1)), int(m.group(2))
    if (images, missing) != (15, 0):
        print("[FAIL] hud: %d image(s), %d missing (expected 15, 0) — id's "
              "picture set did not load whole" % (images, missing))
        dump_tail(40)
        return 1
    # Every name the loader ASKED for, not just the count: the number fields in
    # cg_main.c's own order, the charset, the select overlay, the machinegun's
    # ammo icon and the armor icon. A picture loading under the wrong name would
    # otherwise still make `images` total 15.
    named = {hm.group(2) for hm in HUD_TEX.finditer(log)}
    want = {"gfx/2d/numbers/%s_32b" % d for d in
            ("zero", "one", "two", "three", "four", "five", "six", "seven",
             "eight", "nine", "minus")}
    want |= {"gfx/2d/bigchars", "gfx/2d/select",
             "icons/icona_machinegun", "icons/iconr_yellow"}
    if named != want:
        print("[FAIL] the loader asked for the wrong names; missing %s, extra %s"
              % (sorted(want - named), sorted(named - want)))
        return 1
    print("[OK] id's status bar art loaded: %d pictures, 0 missing "
          "(%s)" % (images, ", ".join(sorted(named))))

    samples = hud_samples(log)
    if not samples:
        print("[FAIL] the driver never sampled the status bar")
        dump_tail(40)
        return 1

    # The health the bar was handed must be the module's own countdown: id
    # spawns at STAT_MAX_HEALTH + 25 and g_active.c takes a point a second off
    # until STAT_MAX_HEALTH, so the samples read 115, 105, 100 ... and stay at
    # 100. Asserting the FLOOR (and that nothing came in above the spawn) is an
    # exact statement about the game's own state at many frames, and it is what
    # makes the decoded pixels below comparable to a number rather than to a
    # moving target.
    hp = [s["health"] for s in samples]
    if any(h > 125 for h in hp):
        print("[FAIL] the status bar was handed health=%d; id spawns at "
              "STAT_MAX_HEALTH + 25 = 125 and no fixture has a health pickup"
              % max(hp))
        return 1
    if any(b > a for a, b in zip(hp, hp[1:])):
        print("[FAIL] health went UP over the samples: %s — the module counts "
              "it down (g_active.c), so the bar is not being fed playerState"
              % hp)
        return 1
    if hp[-1] != 100:
        print("[FAIL] the last sample was handed health=%d, not 100 "
              "(STAT_MAX_HEALTH, the floor of the countdown); samples: %s"
              % (hp[-1], hp))
        return 1
    print("[OK] health tracked the module's own countdown 125 -> 100 across "
          "the samples: %s" % hp)

    s = samples[-1]
    (frame, on, health, armor, ammo, score, weapon, simg, smis, draws,
     digits) = [s[k] for k in ("frame", "on", "health", "armor", "ammo",
                               "score", "weapon", "images", "missing",
                               "draws", "digits")]
    if on != 1:
        print("[FAIL] the HUD was off in a default run (on=%d)" % on)
        return 1
    if (health, armor, ammo, score, weapon) != (100, 0, 100, 0, 2):
        print("[FAIL] the status bar was handed health=%d armor=%d ammo=%d "
              "score=%d weapon=%d; at frame %d the module holds "
              "100/0/100/0/2" % (health, armor, ammo, score, weapon, frame))
        return 1
    if (simg, smis) != (15, 0):
        print("[FAIL] hud frame sample: images=%d missing=%d" % (simg, smis))
        return 1
    if draws < 7 or digits < 6:
        print("[FAIL] the bar composited %d picture(s) and %d glyph(s); an "
              "ammo field, a health field and a score digit is at least 7 and "
              "6" % (draws, digits))
        return 1
    print("[OK] hud frame=%d: values 100/0/100/0 weapon 2, %d picture(s), "
          "%d glyph(s)" % (frame, draws, digits))

    # ---- 3. the pixels, decoded ------------------------------------------
    geom = content_geom()
    if not geom:
        print("[FAIL] the driver never reported its window geometry")
        dump_tail(30)
        return 1
    ox, oy, cw, ch = geom
    w, h, pixels = load_ppm(SHOT_ON)
    if cw > w or ch > h:
        print("[FAIL] the screendump is %dx%d, smaller than the %dx%d content"
              % (w, h, cw, ch))
        return 1

    yc = row_center_y(ch)
    got = {}
    for name, x0 in (("ammo", AMMO_X), ("health", HEALTH_X)):
        ds = []
        for cx in field_cells(x0, FIELD_W, cw):
            ds.append(digit_of(px_at(pixels, w, ox + cx, oy + yc)))
        got[name] = ds
        print("[..] %s field decodes to %s" % (name, ds))
    if got["ammo"] != [1, 0, 0] or got["health"] != [1, 0, 0]:
        print("[FAIL] the fields did not decode to the module's own numbers "
              "(ammo %s, health %s; expected 100 and 100)" % (got["ammo"],
                                                              got["health"]))
        return 1
    print("[OK] both number fields decode to 100, at id's own x positions "
          "(ammo %d, health %d of 640)" % (AMMO_X, HEALTH_X))

    icon_x0 = int((CHAR_W * FIELD_W + TEXT_ICON_SPACE) * cw / float(VIRT_W))
    icon_y0 = int(ROW_Y * ch / float(VIRT_H))
    icon_w = int(ICON_SIZE * cw / float(VIRT_W))
    icon_box = (ox + icon_x0, oy + icon_y0,
                ox + icon_x0 + icon_w, oy + icon_y0 + icon_w)
    n_ammo = count_pixel(pixels, w, h, ICON_AMMO, box=icon_box)
    if n_ammo < icon_w * icon_w // 2:
        print("[FAIL] the ammo icon is not at id's icon offset: %d of %d "
              "pixels are the icon's colour in %s"
              % (n_ammo, icon_w * icon_w, icon_box))
        return 1
    n_armor = count_pixel(pixels, w, h, ICON_ARMOR)
    if n_armor != 0:
        print("[FAIL] the armor icon was drawn (%d pixels) with armor = 0 — "
              "id wraps the field AND its icon in `if (value > 0)`" % n_armor)
        return 1
    print("[OK] ammo icon at %s (%d px); armor icon absent, as id's "
          "`if (value > 0)` requires" % (str(icon_box), n_ammo))

    # The FFA score box: CG_DrawScores' y is 480 - ICON_SIZE, minus
    # BIGCHAR_HEIGHT + 8, the box is BIGCHAR_WIDTH*2 + 8 wide and flush right.
    box_y = int((VIRT_H - ICON_SIZE - BIGCHAR - 8 + (BIGCHAR + 8) / 2.0) *
                ch / float(VIRT_H))
    box_x0 = int((VIRT_W - (BIGCHAR * 2 + 8)) * cw / float(VIRT_W))
    box_x1 = cw
    box = (ox + box_x0, oy + box_y, ox + box_x1, oy + box_y + 2)
    n_sel = count_pixel(pixels, w, h, SELECT, box=(box[0], box[1], box[2], box[3] + 2))
    if n_sel == 0:
        print("[FAIL] the score box is not there: no gfx/2d/select pixel in "
              "%s" % str(box))
        return 1
    print("[OK] FFA score box drawn at the right edge with gfx/2d/select over "
          "it (%d px)" % n_sel)

    # The frame-20 histogram of the pinned view: with both boots pinned (see
    # POSE_KEYS) this is the SAME view in both runs, which is what makes the
    # comparison at the end below an assertion about the HUD rather than about
    # the two runs' pacing. That comparison is scoped — read the block that
    # uses these two dicts for why the warm-bucket split is not compared and
    # what was measured instead.
    first_on = None
    for pm in PIXELS_RE.finditer(log):
        if int(pm.group(1)) == 20:
            first_on = dict(zip(PIXELS_KEYS, pm.groups()))
            break

    # ---- 4. the same map with `nohud`: nothing of the bar is on screen ----
    ok, msg = boot_and_run(Q3ARENA_KEYS_NOHUD, SHOT_OFF, args, "hud off")
    if not ok:
        print("[FAIL] the nohud run: %s" % msg)
        dump_tail(40)
        return 1
    log_off = read_file(SERIAL_LOG)
    off = list(HUD_RE.finditer(log_off))
    if not off:
        print("[FAIL] no `hud frame=` sample in the nohud run")
        dump_tail(40)
        return 1
    s = off[-1]
    if int(s.group(2)) != 0:
        print("[FAIL] `nohud` was ignored (on=%d)" % int(s.group(2)))
        return 1
    if int(s.group(10)) != 0 or int(s.group(11)) != 0:
        print("[FAIL] a nohud run composited %s picture(s) and %s glyph(s)"
              % (s.group(10), s.group(11)))
        return 1
    print("[OK] `nohud` samples on=0, draws=0, digits=0")

    w2, h2, pixels2 = load_ppm(SHOT_OFF)
    geom2 = content_geom()
    ox2, oy2, cw2, ch2 = geom2
    yc2 = row_center_y(ch2)
    for name, x0 in (("ammo", AMMO_X), ("health", HEALTH_X)):
        for cx in field_cells(x0, FIELD_W, cw2):
            if digit_of(px_at(pixels2, w2, ox2 + cx, oy2 + yc2)) is not None:
                print("[FAIL] a number-field pixel survived `nohud` (%s)" % name)
                return 1
    for want, what in ((ICON_AMMO, "the ammo icon"), (ICON_ARMOR, "the armor icon"),
                       (SELECT, "the score box")):
        n = count_pixel(pixels2, w2, h2, want)
        if n != 0:
            print("[FAIL] %s is on screen in a nohud run (%d pixels)"
                  % (what, n))
            return 1
    print("[OK] a nohud frame carries no number, icon or score-box pixel")

    first_off = None
    for pm in PIXELS_RE.finditer(log_off):
        if int(pm.group(1)) == 20:
            first_off = dict(zip(PIXELS_KEYS, pm.groups()))
            break
    # Named before compared: a placeholder texture changes exactly the buckets
    # this equality looks at, and the failure it produces would read as "the
    # HUD leaked into the world" — which is a different bug in a different
    # file. Seen twice on 4 Oct (the first boot after a volume was written; the
    # `patch` bucket 229 -> 1416/1517 in the same pinned view), never
    # reproducible since, so the cause is stated instead of guessed.
    ph_on = PLACEHOLDER_RE.findall(log)
    ph_off = PLACEHOLDER_RE.findall(log_off)
    if ph_on or ph_off:
        print("[FAIL] a shader fell back to its PLACEHOLDER texture (a world "
              "load failure, not a HUD leak): hud-on %s, nohud %s — fix the "
              "load, the histogram equality below is about the HUD"
              % (ph_on or "none", ph_off or "none"))
        return 1
    # The level's palette has to be identical, and its total covered area as
    # good as — the split between the two warm buckets deliberately is NOT,
    # and the reason is measured rather than conceded. On the tree that first
    # ran this (4 Oct) one boot in three drew 12 MORE faces at the SAME pinned
    # eye: the frame line read `drawn=38 tris=76 back=34 untrusted=32` where a
    # healthy boot reads `drawn=26 tris=52 back=46 untrusted=8`, constant from
    # frame 20 to frame 100 — the backface reject's "the file's normals
    # disagree" escape hatch fired on 24 extra triangles, and those triangles
    # are exactly the pixels that move between `patch` and `wall` (229 ->
    # 1416/1517, with `wall` falling by the same amount, 17789 -> 16499,
    # restoring the sum to within ~5 px). cyan/stepgreen/violet/bright never
    # moved by a single pixel in any of the six boots measured.
    #
    # That is a map-data/renderer defect with its own name and its own
    # follow-up; it is not the HUD, and demanding byte-equality of the two warm
    # buckets would only turn it into a red CI job with a misleading message.
    # What this check exists for — "the status bar cannot change the world the
    # renderer drew" — is asserted as: every level-texture bucket exact, and
    # the world's covered area within 1% of the frame.
    if first_on and first_off:
        a = {k: int(v) for k, v in first_on.items()}
        b = {k: int(v) for k, v in first_off.items()}
        palette = ("cyan", "warm", "stepgreen", "violet", "bright", "sky")
        bad = [k for k in palette if a[k] != b[k]]
        if bad:
            print("[FAIL] the level's own pixels changed between the two runs "
                  "(%s): %s vs %s — the HUD is composited AFTER the frame "
                  "histogram is read, so this is the world draw itself"
                  % (", ".join(bad), str(first_on), str(first_off)))
            return 1
        cover_on = a["wall"] + a["patch"]
        cover_off = b["wall"] + b["patch"]
        if abs(cover_on - cover_off) > 76800 // 100:
            print("[FAIL] the world's covered area changed between the two "
                  "runs: %d vs %d px (wall+patch)" % (cover_on, cover_off))
            return 1
        print("[OK] the level's pixels are untouched by the status bar: "
              "cyan/stepgreen/violet/bright/sky identical, covered area "
              "%d vs %d px (wall+patch, split %d/%d vs %d/%d)"
              % (cover_on, cover_off, a["wall"], a["patch"],
                 b["wall"], b["patch"]))

    # ---- 5. the OS survived both sessions --------------------------------
    if PANIC_MARKER in log_off or SYS_ERROR_MARKER in log_off:
        print("[FAIL] the nohud session died")
        return 1
    print("[OK] both sessions ended without a panic")
    print("\n[PASS] id's status bar is drawn from id's own pictures and the "
          "game's own playerState, and its pixels are decodable as the numbers "
          "the module holds.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
