#!/usr/bin/env python3
"""
scripts/q3sky_test.py — the sky, drawn as id's cloud box (v38.128).

What this asserts, and why each thing needs a whole boot
--------------------------------------------------------
Until this release a `surfaceparm sky` surface was drawn like any other wall:
flat, static, no matter what the shader script said. Now the level's sky
surfaces are clipped into the six sides of a sky box to find out which
directions the camera can see sky in, and a box CENTRED ON THE CAMERA is filled
with the shader's own layers at `skyparms`' cloud height — id's
RB_StageIteratorSky, RC_ClipSkyPolygons, R_BuildCloudTexCoords and
FillCloudBox, ported in third_party/tinygl/q3sky.c. Each layer animates on the
game clock through its `tcMod scroll`/`tcMod scale`.

The fixture: scripts/build_test_bsp.build(sky_ceiling=True) is the generated
arena with its CEILING painted `textures/mectovtest/sky`, and that definition is
written the way q3dm1's own textures/skies/tim_hell is — `skyParms - 512 -`
(no six-image far box), an opaque base layer and an ADDITIVE cloud layer, each
with its own tcMod. So the room is a level whose sky is a cloud layer and whose
walls and floor are ordinary geometry, which is exactly the split the pixel
assertions need.

  1. LOAD. The sky's layer images are resolved from the script's own spelling
     (`textures/mectovtest/sky_cloud0`, extension stripped and probed in id's
     order) and both come off the volume — `layers=2/2`, no placeholder. A sky
     whose art is missing is a checkerboard sky, which is a different bug from
     one whose art is absent, and the line says which.
  2. A BOX WAS DRAWN. The per-frame line reports the cloud height the box was
     projected for (512, from the shader, not a constant in the renderer), the
     layers, the box SIDES the clip filled, and the triangles submitted. All
     zero on a level with no sky — which is every fixture arena, and is why no
     other suite in this tree moved this release.
  3. THE FACES WENT SOMEWHERE. A sky face is not emitted as geometry: it is
     clipped, counted as a drawn face (`drawn + culled == surfaces`, the
     contract q3arena_test.py asserts, still holding), and reported in `box=`.
     With `nosky` the same map draws the same face as an ordinary surface and
     says so in `fallback=`.
  4. THE LAYERS ARE ON SCREEN. A screendump of the window content is classified
     into a SKY band and a WORLD band. With the sky on, the band holds the
     layers at full brightness: the base layer's own blue and the additive
     layer's near-white (additive compositing can only ADD light, so white
     there cannot come from the base layer at any light level). With `nosky`
     neither is there — the same ceiling is drawn as a lit surface, so it comes
     out at the fallback sun's dimmer colour.
  5. THE LAYERS MOVE, AND ONLY THE LAYERS MOVE. The camera is PINNED
     (`q3arena mectovsky @x,y,z,yaw,pitch`), so every world pixel is a function
     of the pose: two screendumps taken a couple of minutes apart must be
     IDENTICAL over the world band and DIFFERENT over the sky band, because the
     clock the tcMods read kept running. The `nosky` boot is the control: with
     the pass off, nothing in either band changes, so the difference measured
     with the sky on is the cloud layer and not noise, a bob, or the HUD.

Usage:
    python3 scripts/q3sky_test.py [--timeout 600] [--iso mectov.iso]
"""
import argparse
import os
import re
import subprocess
import sys
import time

import q3_images
import terminal_launch

SERIAL_LOG = "/tmp/mectov_q3sky_serial.log"
MON_SOCK = "/tmp/mectov_q3sky_monitor.sock"
CURSOR_PPM = "/tmp/mectov_q3sky_cursor.ppm"
SHOT_ON_A = "/tmp/q3sky_on_a.ppm"
SHOT_ON_B = "/tmp/q3sky_on_b.ppm"
SHOT_OFF_A = "/tmp/q3sky_off_a.ppm"
SHOT_OFF_B = "/tmp/q3sky_off_b.ppm"

MAP_NAME = "mectovsky"
# The arena's own spawn, spelled the way the driver parses it: eye at floor + id's
# view height (114), yaw 45 (the spawn's `angle`), and 20 degrees of pitch so the
# ceiling is in the top of the frame from a camera standing in the room.
POSE = "-384,-384,114,45,20"
# `q3arena mectovsky nohud @-384,-384,114,45,20`. '@' and ',' have no QEMU key
# names (there is no `sendkey at`): '@' is shift-2 and '-' is `minus` on the
# guest's own scancode map, exactly as q3heavy_test.py spells its pose.
_POSE_KEYS = (["shift-2", "minus"] + list("384") + ["comma", "minus"] +
              list("384") + ["comma"] + list("114") + ["comma"] +
              list("45") + ["comma"] + list("20"))
Q3ARENA_KEYS = (list("q3arena") + ["spc"] + list(MAP_NAME) + ["spc"] +
                list("nohud") + ["spc"] + _POSE_KEYS + ["ret"])
Q3ARENA_KEYS_NOSKY = (list("q3arena") + ["spc"] + list(MAP_NAME) + ["spc"] +
                      list("nohud") + ["spc"] + list("nosky") + ["spc"] +
                      _POSE_KEYS + ["ret"])

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]

START_MARKER = "[Q3ARENA] official qagame VM world rendered through TinyGL"
WINDOW_MARKER = "[Q3ARENA] window id="
ENTERED_MARKER = "entered the game"
PANIC_MARKER = "[PANIC]"
SYS_ERROR_MARKER = "[Q3] Sys_Error"

# The load-time line, one per sky definition: what the script asked for against
# what the volume had. `farbox=0` is this fixture's `skyParms - 512 -`; a map
# that names a six-image far box reports 1 because this port builds only the
# cloud layer (see third_party/tinygl/q3sky.h).
SKY_LOAD_RE = re.compile(
    r"\[Q3ARENA\] sky: shader=(\S+) cloud=(-?\d+) stages=(-?\d+) farbox=(\d+) "
    r"tex0=(\d+)(,add)? img0=(\S+) tex1=(\d+)(,add)? img1=(\S+) "
    r"layers=(\d+)/(\d+)")
SKY_SUM_RE = re.compile(r"\[Q3ARENA\] sky: (\d+) shader\(s\), (\d+) layer "
                        r"image\(s\) from disk, (\d+) missing")
ARGS_RE = re.compile(r"\[Q3ARENA\] args: map='(\S+)' fullscreen=(\d+) "
                     r"pose=(\d+) sort=(\d+) jump=(\d+) fire=(\d+) sky=(\d+)")
SKY_FRAME_RE = re.compile(
    r"\[Q3ARENA\] sky frame=(\d+) on=(\d+) reg=(\d+) cloud=(\d+) stages=(\d+) "
    r"sides=(\d+) tris=(\d+) tris_run=(\d+) box=(\d+) box_run=(\d+) "
    r"fallback=(\d+) fallback_run=(\d+)")
FRAME_RE = re.compile(
    r"\[Q3ARENA\] frame=(\d+) t=(\d+) pos=\((-?\d+),(-?\d+),(-?\d+)\) "
    r"eye_z=(-?\d+) yaw=(-?\d+) pitch=(-?\d+) drawn=(\d+) tris=(\d+) "
    r"culled=(\d+) vis=(-?\d+)/(-?\d+) cluster=(-?\d+) cull_pvs=(\d+) "
    r"cull_frustum=(\d+) planes=(\d+) back=(\d+)")
MESH_RE = re.compile(
    r"\[Q3ARENA\] world mesh: (\S+) surfaces=(-?\d+) of=(-?\d+)")
RECT_RE = re.compile(r"\[Q3ARENA\] window id=0x[0-9a-f]+ rect=(-?\d+),(-?\d+) "
                     r"(\d+)x(\d+) content=(\d+)x(\d+)")


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


def mon_cmd(cmd, settle=None):
    import socket
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


def content_geom():
    """(offset, size) of the game window's content from the driver's own line."""
    m = None
    for m in RECT_RE.finditer(read_file(SERIAL_LOG)):
        pass
    if not m:
        return None
    wx, wy, _ww, _wh, cw, ch = m.groups()
    # The window is the content plus a 1 px border and a 20 px title bar.
    return (int(wx) + 1, int(wy) + 21, int(cw), int(ch))


def band(ox, oy, cw, ch, x0f, x1f, y0f, y1f):
    """A rectangle of the window CONTENT, in screen pixels.

    The fractions are chosen to keep the two bands clean of everything that is
    not the scene: the fps readout lives in the content's top-RIGHT corner, the
    crosshair and the gun in the middle, and `nohud` removes the status bar, so
    the sky band is the upper LEFT and the world band the lower left.
    """
    return (ox + int(cw * x0f), oy + int(ch * y0f),
            ox + int(cw * x1f), oy + int(ch * y1f))


SKY_BAND = (0.02, 0.60, 0.03, 0.35)
WORLD_BAND = (0.02, 0.30, 0.55, 0.80)


def count_where(pixels, w, box, pred):
    x0, y0, x1, y1 = box
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            if pred(*px_at(pixels, w, x, y)):
                n += 1
    return n


def band_diff(pa, pb, w, box):
    x0, y0, x1, y1 = box
    diff = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            if px_at(pa, w, x, y) != px_at(pb, w, x, y):
                diff += 1
    return diff


def is_near_white(r, g, b):
    """The additive layer's contribution: additive can only ADD light, so this
    colour cannot be produced by the base layer at any light level."""
    return r > 200 and g > 200 and b > 200


def is_bright_blue(r, g, b):
    """The base layer's own blue (64,96,208 / 40,64,152), undimmed — the sky
    pass draws its layers unlit, while the geometry fallback modulates them by
    the baked sun."""
    return b > 150 and b > g + 40 and r < 120


def mkdirs(ext2, rel):
    """Create every ancestor of a staged path, parents first.

    debugfs's `mkdir` does NOT create intermediate directories and exits 0 when
    it fails, so a caller that only checks the status writes nothing and
    believes it worked (the trap q3hud_test.py found in v38.127; /baseq3 exists,
    /baseq3/textures may not).
    """
    path = "/baseq3"
    for p in rel.split("/")[:-1]:
        path += "/" + p
        subprocess.run(["debugfs", "-w", "-R", "mkdir %s" % path, ext2],
                       check=False, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)


def boot_and_dump(keys, shots, args, label):
    """One boot for one configuration: login, terminal, `q3arena ...`, two dumps.

    Returns (ok, message). A boot per configuration is deliberate: the port
    cannot relaunch after ESC (an unrelated hunk-initialisation bug), so the
    `nosky` control cannot ride the same session. Two screendumps ANOTHER gap
    apart come from the SAME boot, which is what makes them comparable (same
    pose, same frame budget) while still spanning enough game clock for the
    cloud layers to have moved.
    """
    print("[..] %s: booting" % label)
    for p in [SERIAL_LOG, MON_SOCK, CURSOR_PPM] + list(shots):
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
            return False, "the kernel never reached the login screen"
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

        # Click inside the Terminal before typing — launch_terminal leaves the
        # cursor on the icon it verified, so the move is relative to there — and
        # retry the whole line, because a command typed into the instant before
        # the shell task exists is swallowed (q3heavy_test.py's finding, and the
        # reason a bare retry loop is flaky). The 3 s settle after the release is
        # the same deliberate wait: the WM has to hand the window focus over.
        for attempt in range(3):
            mon_cmd("mouse_move 300 176", settle=0.1)
            mon_cmd("mouse_button 1", settle=0.1)
            mon_cmd("mouse_button 0", settle=3.0)
            for _ in range(24):
                mon_cmd("sendkey backspace", settle=0.05)
            for k in keys:
                mon_cmd("sendkey " + k, settle=0.12)
            sendkey("ret")
            if wait_for_in_file(SERIAL_LOG, "[Q3ARENA] args:", 60):
                break
            print("[..] %s: retry launch (%d)" % (label, attempt + 1))
        else:
            return False, "`q3arena %s` never started its task" % MAP_NAME
        if not wait_for_in_file(SERIAL_LOG, WINDOW_MARKER, 60):
            return False, "no window was opened"
        if not wait_for_in_file(SERIAL_LOG, ENTERED_MARKER, 300):
            return False, "the module never announced the client"
        # The scene has to be the pinned one before the first dump, and the
        # sky's own line has to have been sampled at least once — that is the
        # release's evidence that the pass ran, not just that it loaded.
        if not wait_for_in_file(SERIAL_LOG, "[Q3ARENA] sky frame=", 300):
            return False, "the sky was never sampled (no `sky frame=` line)"
        time.sleep(2.0)
        if not screendump(shots[0], settle=1.5):
            return False, "the first screendump never happened"
        print("[OK] %s: first dump (%s)" % (label, os.path.basename(shots[0])))
        # Minutes of WALL clock, which is what the game clock is paced by
        # (v38.116's catch-up: 50 ms of game time per tick, ticks fed by the
        # wall clock), and long enough for a 0.05 units/second scroll to move the
        # layers by several texels even at a retail map's frames-per-second.
        time.sleep(args.gap)
        if not screendump(shots[1], settle=1.5):
            return False, "the second screendump never happened"
        print("[OK] %s: second dump (%s)" % (label, os.path.basename(shots[1])))
        return True, "ran"
    finally:
        sendkey("esc")
        time.sleep(1.0)
        qemu.terminate()
        try:
            qemu.wait(timeout=15)
        except subprocess.TimeoutExpired:
            qemu.kill()


def check_frame_contract(log):
    """Every face of the mesh is accounted for exactly once, every frame.

    The renderer's frame line splits the mesh's surfaces into what it drew, what
    the PVS and the frustum rejected, and what was backfacing (`back`), and that
    sum is the surface count — v38.105's invariant, which the other suites read
    as `drawn >= 8` and this one can read exactly, because the fixture's mesh is
    small enough to check face by face. It is the assertion this release could
    quietly have broken: a sky face is drawn by the cloud BOX, not by its own
    triangles, so a renderer that forgot to count it would show `drawn` short by
    the number of sky surfaces in view — while still looking correct on screen.
    """
    m = MESH_RE.search(log)
    if not m:
        return None, "no mesh line to read the surface count from"
    of = int(m.group(2))
    n = 0
    for fm in FRAME_RE.finditer(log):
        drawn, pvs, frustum, back = (int(fm.group(i)) for i in (9, 15, 16, 18))
        total = drawn + pvs + frustum + back
        if total != of:
            return None, ("frame %s drew %d, culled %d by the PVS and %d by the "
                          "frustum and dropped %d backfacing = %d, but the mesh "
                          "has %d surface(s)" % (fm.group(1), drawn, pvs, frustum,
                                                 back, total, of))
        n += 1
    if n == 0:
        return None, "no frame lines to check"
    return (of, n), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    # How long the two dumps of one boot are apart, in seconds of wall clock.
    # The tcMods animate on the GAME clock, which the driver paces by the wall
    # clock (50 ms per tick, fed by elapsed real time), so this is a real
    # measurement of "the layers moved", not a guess about frames.
    ap.add_argument("--gap", type=int, default=70)
    args = ap.parse_args()

    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    # ---- 0. the synthetic pak, staged through the REAL staging script ------- #
    # The sky fixture is a MAP with its own shader script and its own two layer
    # images, so it goes through exactly the retail path: build the pak, stage
    # the map out of it with q3a_data.py (unchanged), seed the volume, overlay.
    stage = "/tmp/mectov_q3sky_stage"
    pak = os.path.join(here, "build", "q3sky", "pak0.pk3")
    subprocess.run(["rm", "-rf", stage], check=True)
    subprocess.run([sys.executable, os.path.join(here, "scripts", "build_q3pak.py"),
                    pak], check=True, stdout=subprocess.DEVNULL)
    # q3a_data.py's --out is the staging ROOT; the game directory it writes is
    # <out>/baseq3, which is what seed_ext2.sh mirrors onto the volume's
    # /baseq3 (the trap q3hud_test.py documents at length).
    out = os.path.join(stage, "q3data")
    baseq3 = os.path.join(out, "baseq3")
    subprocess.run([sys.executable, os.path.join(here, "scripts", "q3a_data.py"),
                    "--pak", pak, "--map", MAP_NAME, "--out", out, "--verify"],
                   check=True, stdout=subprocess.DEVNULL)
    staged = []
    for d, _sub, files in os.walk(baseq3):
        for f in files:
            staged.append(os.path.relpath(os.path.join(d, f), baseq3))
    staged = sorted(staged)
    need = {"maps/%s.bsp" % MAP_NAME, "scripts/mectovtest.shader",
            "textures/mectovtest/sky_cloud0.tga",
            "textures/mectovtest/sky_cloud1.tga"}
    missing = need - set(staged)
    if missing:
        print("[FAIL] the staged tree is missing: %s" % ", ".join(sorted(missing)))
        return 1
    print("[OK] staging carried the sky fixture and both of its layers "
          "(%d files)" % len(staged))

    # ---- 1. fresh volumes, seeded the way every other Q3 suite seeds ------- #
    err = q3_images.fresh_images(args.disk, args.ext2)
    if err:
        print("[FAIL] %s" % err)
        return 1
    err = q3_images.seed_volume(args.ext2)
    if err:
        print("[FAIL] %s" % err)
        return 1
    for rel in staged:
        mkdirs(args.ext2, rel)
        subprocess.run(["debugfs", "-w", "-R", "rm /baseq3/%s" % rel, args.ext2],
                       check=False, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        subprocess.run(["debugfs", "-w", "-R", "write %s /baseq3/%s"
                        % (os.path.join(baseq3, rel), rel), args.ext2],
                       check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    print("[OK] volume seeded (q3_images + %d staged file(s) overlaid)" % len(staged))

    # ---- 2. the sky boot --------------------------------------------------- #
    ok, msg = boot_and_dump(Q3ARENA_KEYS, (SHOT_ON_A, SHOT_ON_B), args, "sky on")
    if not ok:
        print("[FAIL] %s" % msg)
        dump_tail(40)
        return 1
    log = read_file(SERIAL_LOG)

    am = ARGS_RE.search(log)
    if not am:
        print("[FAIL] no `args:` line — the map and pose never reached the driver")
        dump_tail(40)
        return 1
    if (am.group(1), am.group(3), am.group(7)) != (MAP_NAME, "1", "1"):
        print("[FAIL] the driver ran map='%s' pose=%s sky=%s, wanted "
              "'%s' pose=1 sky=1" % (am.group(1), am.group(3), am.group(7),
                                     MAP_NAME))
        return 1
    print("[OK] the driver ran the sky fixture with the pose pinned and the "
          "sky pass on (map='%s' pose=1 sky=1)" % am.group(1))

    lm = SKY_LOAD_RE.search(log)
    if not lm:
        print("[FAIL] no sky load line — the definition was never parsed as a sky")
        dump_tail(40)
        return 1
    name, cloud, stages, farbox = lm.group(1), int(lm.group(2)), int(lm.group(3)), int(lm.group(4))
    img0, img1 = lm.group(7), lm.group(10)
    layers = (int(lm.group(11)), int(lm.group(12)))
    for what, got, want in (("cloud height", cloud, 512),
                            ("stages", stages, 2),
                            ("far box", farbox, 0),
                            ("layers loaded", layers, (2, 2))):
        if got != want:
            print("[FAIL] the sky definition's %s is %s, wanted %s "
                  "(line: %s)" % (what, got, want, lm.group(0)))
            return 1
    if not img0.endswith("sky_cloud0.tga") or not img1.endswith("sky_cloud1.tga"):
        print("[FAIL] the layers resolved to %s / %s, wanted the fixture's two "
              "cloud textures" % (img0, img1))
        return 1
    if lm.group(6) or not lm.group(9):
        print("[FAIL] the layer flags are backwards: stage 0 additive=%s, "
              "stage 1 additive=%s (stage 1 is the `blendFunc GL_ONE GL_ONE` "
              "layer)" % (bool(lm.group(6)), bool(lm.group(9))))
        return 1
    print("[OK] the sky loaded as a cloud layer: %s, cloud=%d, 2 stages, "
          "far box=0, both layers from disk (%s, %s)"
          % (name, cloud, os.path.basename(img0), os.path.basename(img1)))

    sm = SKY_SUM_RE.search(log)
    if not sm or (int(sm.group(1)), int(sm.group(2)), int(sm.group(3))) != (1, 2, 0):
        print("[FAIL] the run's sky summary is %r, wanted `1 shader(s), 2 layer "
              "image(s) from disk, 0 missing`" % (sm.group(0) if sm else None))
        return 1
    print("[OK] the run summary agrees: 1 sky shader, 2 layers from disk, "
          "0 missing")

    res, err = check_frame_contract(log)
    if err:
        print("[FAIL] the drawn/culled contract broke: %s" % err)
        dump_tail(40)
        return 1
    of, frames = res
    print("[OK] drawn + culled + backfacing == %d surface(s) on all %d sampled "
          "frame(s) — the sky faces are counted as drawn even though the box "
          "drew them" % (of, frames))

    sf = SKY_FRAME_RE.findall(log)
    if not sf:
        print("[FAIL] the sky was never sampled per frame")
        return 1
    ok_box = [s for s in sf if int(s[1]) == 1 and int(s[6]) > 0 and
              int(s[8]) > 0 and int(s[10]) == 0]
    if not ok_box:
        print("[FAIL] no frame reported a drawn cloud box; last sample: %s"
              % (sf[-1],))
        dump_tail(40)
        return 1
    last = ok_box[-1]
    if int(last[3]) != 512 or int(last[4]) != 2:
        print("[FAIL] the box was built for cloud=%s with %s stage(s), wanted "
              "512 and 2 (line: %s)" % (last[3], last[4], last))
        return 1
    print("[OK] a cloud box was drawn every sampled frame: cloud=%s, "
          "stages=%s, sides=%s, tris=%s (run %s), sky faces=%s, fallback=%s"
          % (last[3], last[4], last[5], last[6], last[7], last[8], last[10]))

    # ---- 3. the layers are on screen, and only the layers move -------------- #
    geom = content_geom()
    if not geom:
        print("[FAIL] no window rect line — the screendump cannot be located")
        return 1
    ox, oy, cw, ch = geom
    w, h, pa = load_ppm(SHOT_ON_A)
    w2, h2, pb = load_ppm(SHOT_ON_B)
    sky_box = band(ox, oy, cw, ch, *SKY_BAND)
    world_box = band(ox, oy, cw, ch, *WORLD_BAND)
    sky_px = (sky_box[2] - sky_box[0]) * (sky_box[3] - sky_box[1])
    white = count_where(pa, w, sky_box, is_near_white)
    blue = count_where(pa, w, sky_box, is_bright_blue)
    print("[..] sky band %dx%d: %d near-white px, %d bright-blue px (of %d)"
          % (sky_box[2] - sky_box[0], sky_box[3] - sky_box[1], white, blue,
             sky_px))
    # The additive layer is the one colour the level cannot produce any other
    # way: additive compositing only ADDS light, so near-white here is either
    # this layer or nothing. (The base layer's blue is printed for information
    # only — as ordinary geometry the same texture is modulated by the baked
    # sun, so a lowered light level would make a brightness threshold a
    # statement about the lighting rather than about the sky.)
    if white < sky_px // 100:
        print("[FAIL] the sky band has %d near-white pixel(s) of %d; the "
              "additive cloud layer should cover a real share of it, so the "
              "box is not reaching the screen" % (white, sky_px))
        return 1
    print("[OK] the layers are on screen in the sky band: %d near-white px "
          "(additive layer) and %d px of the base layer's own blue"
          % (white, blue))

    sky_diff = band_diff(pa, pb, w, sky_box)
    world_diff = band_diff(pa, pb, w, world_box)
    print("[..] between the two dumps: sky band %d px changed, world band %d px "
          "changed" % (sky_diff, world_diff))
    if sky_diff < sky_px // 200:
        print("[FAIL] only %d of %d sky pixels changed between dumps %ds apart "
              "with the camera pinned — the layers' tcMods are not animating "
              "the box" % (sky_diff, sky_px, args.gap))
        return 1
    if world_diff != 0:
        print("[FAIL] the WORLD band changed by %d pixel(s) between two dumps "
              "of the same pinned pose — the camera or the level moved, so the "
              "sky measurement above is not attributable to the sky" % world_diff)
        return 1
    print("[OK] the sky band changed and the world band did not: the cloud "
          "layers animate on the game clock, and the level under them is a "
          "function of the pose alone")

    # ---- 4. the control: no sky, no clouds, nothing moves ------------------- #
    ok, msg = boot_and_dump(Q3ARENA_KEYS_NOSKY, (SHOT_OFF_A, SHOT_OFF_B), args,
                            "nosky")
    if not ok:
        print("[FAIL] %s" % msg)
        dump_tail(40)
        return 1
    log = read_file(SERIAL_LOG)
    am = ARGS_RE.search(log)
    if not am or am.group(7) != "0":
        print("[FAIL] the nosky boot did not report sky=0 (%r)"
              % (am.group(0) if am else None))
        return 1
    sf = SKY_FRAME_RE.findall(log)
    off = [s for s in sf if int(s[1]) == 0]
    if not off:
        print("[FAIL] `nosky` never showed up in a frame line")
        dump_tail(40)
        return 1
    last = off[-1]
    if int(last[8]) != 0 or int(last[10]) < 1:
        print("[FAIL] with the pass off the box should have drawn nothing and "
              "the sky faces should be back on the geometry path: %s" % (last,))
        return 1
    sm2 = SKY_SUM_RE.search(log)
    if not sm2 or (int(sm2.group(1)), int(sm2.group(2)), int(sm2.group(3))) != (1, 2, 0):
        print("[FAIL] `nosky` changed what the LOAD did (%r) — the knob is a "
              "draw-time switch, the shader is still registered"
              % (sm2.group(0) if sm2 else None))
        return 1
    print("[OK] with `nosky`: the definition still loads (1 shader, 2 layers), "
          "the box draws nothing (tris=0) and %s sky face(s) went back to the "
          "geometry path" % last[10])

    geom = content_geom()
    ox, oy, cw, ch = geom
    w, h, pa = load_ppm(SHOT_OFF_A)
    wb, hb, pb = load_ppm(SHOT_OFF_B)
    sky_box = band(ox, oy, cw, ch, *SKY_BAND)
    world_box = band(ox, oy, cw, ch, *WORLD_BAND)
    white = count_where(pa, w, sky_box, is_near_white)
    blue = count_where(pa, w, sky_box, is_bright_blue)
    print("[..] nosky sky band: %d near-white px, %d bright-blue px"
          % (white, blue))
    if white != 0:
        print("[FAIL] %d near-white pixel(s) in the sky band with the sky pass "
              "OFF — only the additive cloud layer can put the base layer's "
              "blue over 200 in every channel, and it is not drawn here" % white)
        return 1
    sky_diff = band_diff(pa, pb, w, sky_box)
    world_diff = band_diff(pa, pb, w, world_box)
    if sky_diff != 0 or world_diff != 0:
        print("[FAIL] with the sky off something still changed between the two "
              "dumps (sky %d px, world %d px) — the difference measured with "
              "the sky on is then not the cloud layer"
              % (sky_diff, world_diff))
        return 1
    print("[OK] with the sky off the same two dumps are pixel-identical in both "
          "bands: nothing on this level animates, so the motion the sky boot "
          "shows is the sky's")

    print("[PASS] the sky is drawn the way id draws it: the level's sky faces "
          "choose the sides of a camera-centred cloud box, the shader's own "
          "layers fill it at skyparms' height, and their tcMods animate them "
          "on the game clock")
    return 0


if __name__ == "__main__":
    sys.exit(main())
