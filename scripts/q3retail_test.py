#!/usr/bin/env python3
"""
scripts/q3retail_test.py — the retail-map path, end to end, with no id data
(v38.111, Q3 phase 9).

The user-facing promise this exercises: `q3a_data.py --pak <your own pak0>
--map q3dm1` stages a real Quake III map onto the volume, `q3arena q3dm1`
loads it — and every step of that pipeline is id's own format. pak0 is not
redistributable, so CI can never see the retail file — but a pak0 is an
ordinary zip, and every step of the pipeline is driven by the zip's
CONTENTS, not by which zip it is. So this suite builds a synthetic pak0
(scripts/build_q3pak.py: the generated arena .bsp, shader scripts, TGA
textures, one JPEG), feeds it through the REAL staging script
(q3a_data.py, unchanged), seeds the result onto the ext2 volume, and boots
the OS to assert the full retail path:

  1. staging: q3a_data.py resolved the .bsp's shader names THROUGH the
     script files — the staged tree must carry the script and the JPEG it
     names (curve_jpg.jpg), and must NOT carry the direct-fallback
     curve.tga (which the script never references).
  2. in-game: `q3arena mectovtest` runs the same official qagame session on
     that staged map. The mesh line names maps/mectovtest.bsp, the texture
     lines show the curve shader resolved to the JPEG through the script
     (fmt=jpg path=...curve_jpg.jpg), every other shader resolved to its
     TGA, and there is not one placeholder.
  3. the world is really drawn: frames are submitted and classified out of
     the renderer's own buffer — floor/walls/ceiling/step/curve palette,
     the scene is live, ESC ends the session, the OS survives.

The default-arena regression stays in q3arena_test.py; this suite only
covers the retail path on top of it.

Usage:
    python3 scripts/q3retail_test.py [--timeout 600] [--iso mectov-q3.iso]
"""
import argparse
import os
import re
import subprocess
import sys
import time

import build_test_bsp
import q3_images
import terminal_launch

SERIAL_LOG = "/tmp/mectov_q3retail_serial.log"
MON_SOCK = "/tmp/mectov_q3retail_monitor.sock"
CURSOR_PPM = "/tmp/mectov_q3retail_cursor.ppm"
SHOT_A = "/tmp/q3retail_a.ppm"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
Q3ARENA_KEYS = ["q", "3", "a", "r", "e", "n", "a", "spc",
                "m", "e", "c", "t", "o", "v", "t", "e", "s", "t", "ret"]

START_MARKER = "[Q3ARENA] official qagame VM world rendered through TinyGL"
WINDOW_MARKER = "[Q3ARENA] window id="
MESH_MARKER = "[Q3ARENA] world mesh: "
ENTERED_MARKER = "entered the game"
DONE_MARKER = "[Q3ARENA] done"
PANIC_MARKER = "[PANIC]"
SYS_ERROR_MARKER = "[Q3] Sys_Error"

BSP_NAME = "maps/mectovtest.bsp"
MAP_NAME = "mectovtest"
# v38.125: the .bsp this suite stages is scripts/build_test_bsp.py's arena WITH
# its four probe shaders (scripts/build_q3pak.py builds it that way, because the
# pak ships the .shader file that defines them). The count is read from the
# generator rather than written down, so it cannot drift: the mesh line has to
# agree with the lump the pak actually carries.
BSP_SHADERS = len(build_test_bsp.SHADERS)

MESH_RE = re.compile(
    r"\[Q3ARENA\] world mesh: (\S+) surfaces=(-?\d+) of=(-?\d+) verts=(-?\d+) "
    r"shaders=(-?\d+) planar=(-?\d+) patches=(-?\d+) patchdrawn=(-?\d+) "
    r"patchquads=(-?\d+) patchverts=(-?\d+) patchskipped=(-?\d+) "
    r"skipped=(-?\d+) truncated=(-?\d+)")
TEX_RE = re.compile(
    r"\[Q3ARENA\] tex (\d+) (\S+) path=(\S+) fmt=(\S+) size=(\d+)x(\d+) "
    r"bytes=(\d+) gl=(\d+)")
TEX_MISSING_RE = re.compile(r"\[Q3ARENA\] tex (\d+) (\S+) missing=1")
# v38.125: the draw flags a shader's definition asked for, appended after the
# fields above ("flags=add", "culloff" or "flags=add,culloff").
TEX_FLAGS_RE = re.compile(r"\[Q3ARENA\] tex \d+ (\S+).* flags=(\S+)")
# v38.125: the load-time census of those flags over the whole map.
DRAW_FLAGS_RE = re.compile(r"\[Q3ARENA\] draw flags: additive=(\d+) "
                           r"culloff=(\d+)")
# The parser's own report: how many scripts the engine FS listed and how many
# definitions came out of them. Asserted separately from the texture lines so
# a silent parse failure says "0 definitions", not "resolved to the .tga".
DECLS_RE = re.compile(r"\[Q3BSP\] shader scripts: (\d+) file\(s\) listed, "
                      r"(\d+) definition\(s\)")
FRAME_RE = re.compile(
    r"\[Q3ARENA\] frame=(\d+) t=(\d+) pos=\((-?\d+),(-?\d+),(-?\d+)\) "
    r"eye_z=(-?\d+) yaw=(-?\d+) pitch=(-?\d+) drawn=(\d+) tris=(\d+) "
    r"culled=(\d+)")
PIXELS_RE = re.compile(
    r"\[Q3ARENA\] pixels frame=(\d+) cyan=(\d+) warm=(\d+) stepgreen=(\d+) "
    r"violet=(\d+) bright=(\d+) patch=(\d+) sky=(\d+) wall=(\d+) distinct=(\d+)")
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
            if int(m.group(1)) >= n:
                return True
        if DONE_MARKER in read_file(SERIAL_LOG):
            return False
        time.sleep(1)
    return False


def mon_cmd(cmd, settle=None):
    try:
        s = socket_socket()
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(0.15 if settle is None else settle)
        s.close()
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")


def socket_socket():
    import socket
    return socket.socket(socket.AF_UNIX)


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
        print(line[:140])


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

    # ---- 0. build the synthetic pak and stage it through the real script --
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    # Fresh boot images every run: disk.img carries the VFS node table the
    # previous boot PERSISTED (vfs_save), and ext2.img has to exist and be
    # formatted before this suite stages into it. scripts/q3_images.py explains
    # why the rule is shared (v38.111's stale ext2 tree, v38.112's stale
    # shader) — this suite is the one whose fixture is the reason for it.
    err = q3_images.fresh_images(args.disk, args.ext2)
    if err:
        print(f"[FAIL] {err}")
        return 1

    stage = "/tmp/q3retail_stage"
    pak = os.path.join(stage, "pak0.pk3")
    out = os.path.join(stage, "out")
    subprocess.run(["rm", "-rf", stage], check=True)
    subprocess.run([sys.executable, os.path.join(here, "scripts", "build_q3pak.py"),
                    pak], check=True)
    subprocess.run([sys.executable, os.path.join(here, "scripts", "q3a_data.py"),
                    "--pak", pak, "--map", MAP_NAME, "--out", out, "--verify"],
                   check=True)
    baseq3 = os.path.join(out, "baseq3")

    # ---- 1. the staging decisions are the script's, not the fallback's ----
    staged = set()
    for dirpath, _dirs, files in os.walk(baseq3):
        for fn in files:
            rel = os.path.relpath(os.path.join(dirpath, fn), baseq3)
            staged.add(rel.replace(os.sep, "/"))
    need = {
        "maps/mectovtest.bsp",
        "scripts/mectovtest.shader",
        "textures/mectovtest/curve_jpg.jpg",
        "textures/mectovtest/floor.tga",
        "textures/mectovtest/wall.tga",
        "textures/mectovtest/ceiling.tga",
        "textures/mectovtest/step.tga",
        # v38.125: the four probe shaders' images. They belong to shaders no
        # surface uses, so they exist to drive the renderer's resolver through
        # the patterns the demo's own scripts are written in — and the STAGING
        # side has to carry them for that to be possible at all.
        "textures/mectovtest/probe_dollar_real.tga",
        "textures/mectovtest/probe_anim0.tga",
        "textures/mectovtest/probe_anim1.tga",
        "textures/mectovtest/probe_ext_target.jpg",
        "textures/mectovtest/probe_flame.tga",
    }
    missing = need - staged
    if missing:
        print("[FAIL] the staged tree is missing: " + ", ".join(sorted(missing)))
        return 1
    if "textures/mectovtest/curve.tga" in staged:
        print("[FAIL] curve.tga was staged — the shader script REBINDS curve "
              "to curve_jpg, so staging it means the definition was ignored")
        return 1
    # The extension probe only means anything if the spelled file does NOT
    # exist: the script asks for probe_ext_target.tga and the pak ships the
    # .jpg, exactly like the demo's "map textures/skies/killsky_1.tga" against
    # killsky_1.jpg. Staging a .tga here would let the renderer pass by
    # accident, so its absence is part of the fixture.
    if "textures/mectovtest/probe_ext_target.tga" in staged:
        print("[FAIL] probe_ext_target.tga was staged — the extension probe is "
              "only meaningful while the spelled file does not exist")
        return 1
    print("[OK] staging through q3a_data.py: script + JPEG staged, the "
          "unreferenced curve.tga correctly absent, the extension probe's "
          ".tga correctly absent (%d files)" % len(staged))

    # ---- 2. seed the volume the way a user would -------------------------
    # scripts/seed_ext2.sh writes the system blobs, the VM, productid.txt and
    # the default arena tree; the STAGED tree is then overlaid on top, so the
    # staged files win (same as q3a_data.py staging over an existing install).
    # The `rm` is what makes that sentence true: seed_ext2.sh runs
    # build_test_bsp.py into the volume too, so maps/mectovtest.bsp and the
    # arena's own textures already exist there — and debugfs's `write` REFUSES
    # an existing name ("Ext2 file already exists") while still exiting 0, so
    # the overlay silently kept the seeded copy. That was invisible while both
    # copies were the same five-shader arena; v38.125's probe shaders are in the
    # pak's .bsp only, so the stale copy made the probe definitions point at
    # names the mesh never carried and this suite failed. scripts/q3viewmodel_test.py
    # learned the same thing from the same trap (v38.124) and rm's first.
    subprocess.run(["bash", os.path.join(here, "scripts", "seed_ext2.sh"),
                    args.ext2], check=True)
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

    print("[OK] volume seeded (seed_ext2.sh + %d staged file(s) overlaid)"
          % len(staged))

    # ---- 3. boot and play the staged map by name --------------------------
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

        # Types "q3arena mectovtest" — the RETAIL path: a map chosen by name
        # from the staged game data.
        if not type_line(Q3ARENA_KEYS, retries=2, ready_marker=START_MARKER,
                         timeout=150):
            print("[FAIL] the `q3arena mectovtest` command never started its task")
            dump_tail()
            return 1
        print("[OK] q3arena task started with a map argument")

        if not wait_for_in_file(SERIAL_LOG, WINDOW_MARKER, 60):
            print("[FAIL] no window was opened")
            dump_tail()
            return 1
        if not wait_for_in_file(SERIAL_LOG, MESH_MARKER, 60):
            print("[FAIL] the render mesh was never built")
            dump_tail()
            return 1
        if not wait_for_in_file(SERIAL_LOG, ENTERED_MARKER, 240):
            print("[FAIL] the module never announced the client")
            dump_tail(40)
            return 1

        log = read_file(SERIAL_LOG)

        # The map argument really reached the driver, and it is OUR map.
        if f"[Q3VM] map arg: '{MAP_NAME}'" not in log:
            print("[FAIL] no 'map arg' line naming the staged map")
            dump_tail()
            return 1
        m = MESH_RE.search(log)
        if not m:
            print("[FAIL] no mesh marker to parse")
            dump_tail()
            return 1
        bsp = m.group(1)
        if bsp != BSP_NAME:
            print(f"[FAIL] the mesh was built from {bsp}, not {BSP_NAME}")
            return 1
        faces, verts, shaders = int(m.group(2)), int(m.group(4)), int(m.group(5))
        if faces <= 0 or verts <= 0 or shaders <= 0:
            print(f"[FAIL] empty mesh: surfaces={faces} verts={verts}")
            return 1
        if shaders != BSP_SHADERS:
            print(f"[FAIL] the loaded mesh carries {shaders} shader(s) but the "
                  f"pak's .bsp has {BSP_SHADERS} (scripts/build_test_bsp.SHADERS, "
                  f"probes included) — a seeded copy of the generated arena "
                  f"would explain it, and the probe shaders below could not be "
                  f"reached")
            return 1
        print(f"[OK] the mesh is the STAGED map's file: {bsp} ({faces} surfaces, "
              f"{verts} vertices, {shaders} shaders)")

        # ---- 4. resolution went through the script, not the fallback -------
        # First: the script itself had to be found and parsed. The engine FS has
        # to LIST scripts/*.shader (the VFS node table must be live this boot)
        # and the parser has to come out with definitions; otherwise the only
        # thing left is the direct-name fallback, which the .tga files satisfy.
        dm = DECLS_RE.search(log)
        if not dm:
            print("[FAIL] the shader-script parser never reported an outcome "
                  "(no '[Q3BSP] shader scripts:' line)")
            dump_tail(30)
            return 1
        nfiles, ndefs = int(dm.group(1)), int(dm.group(2))
        if nfiles < 1 or ndefs < 1:
            print(f"[FAIL] the shader script was not parsed: {nfiles} file(s) "
                  f"listed, {ndefs} definition(s)")
            dump_tail(30)
            return 1
        print(f"[OK] the engine FS listed {nfiles} script file(s) and the "
              f"parser produced {ndefs} shader definition(s)")

        if TEX_MISSING_RE.search(log):
            print("[FAIL] a shader had no image on the volume:")
            for line in log.splitlines():
                if "missing=1" in line:
                    print("       " + line[:140])
            return 1
        texs = {}
        for tm in TEX_RE.finditer(log):
            texs[tm.group(2)] = (tm.group(3), tm.group(4), int(tm.group(5)),
                                 int(tm.group(6)), int(tm.group(7)), int(tm.group(8)))
        curve = texs.get("textures/mectovtest/curve")
        if not curve:
            print("[FAIL] the curve shader produced no texture line at all")
            return 1
        cpath, cfmt = curve[0], curve[1]
        if cfmt != "jpg" or not cpath.endswith("curve_jpg.jpg"):
            print(f"[FAIL] the curve shader did NOT resolve through the shader "
                  f"script (got path={cpath} fmt={cfmt}) — the JPEG-naming "
                  f"definition was ignored")
            return 1
        print(f"[OK] the shader script resolved curve to the JPEG: {cpath} "
              f"fmt={cfmt}")
        for name, fmt in (("textures/mectovtest/floor", "tga"),
                          ("textures/mectovtest/wall", "tga"),
                          ("textures/mectovtest/ceiling", "tga"),
                          ("textures/mectovtest/step", "tga")):
            t = texs.get(name)
            if not t:
                print(f"[FAIL] {name} produced no texture line")
                return 1
            if t[1] != fmt or not t[0].endswith(fmt):
                print(f"[FAIL] {name} resolved to {t[0]} ({t[1]}), expected a "
                      f"direct {fmt} fallback")
                return 1
        print("[OK] the four directly-named textures resolved to their own "
              "TGA files (no definition needed)")

        # ---- 4b. the resolver's rules, on the demo's own patterns (v38.125) --
        # Four shaders no surface uses, each written the way the demo's own
        # scripts are written. Before v38.125 every one of them came out as the
        # placeholder checkerboard: the sky, the lava, every torch in q3dm1.
        probes = (
            ("textures/mectovtest/probe_dollar", "probe_dollar_real.tga",
             "a `map $lightmap` first stage: the engine-provided operand is not a "
             "file, so the definition's real image (stage two) has to win"),
            ("textures/mectovtest/probe_anim", "probe_anim0.tga",
             "an animMap-only definition: its first FRAME is the image (id's "
             "torches are written exactly this way)"),
            ("textures/mectovtest/probe_extension", "probe_ext_target.jpg",
             "a script that spells .tga while the pak ships the .jpg: the "
             "extension is stripped and id's order (.jpg first) decides"),
        )
        for name, want_suffix, why in probes:
            t = texs.get(name)
            if not t:
                print(f"[FAIL] {name} produced no texture line — {why}")
                return 1
            path, fmt = t[0], t[1]
            if not path.endswith(want_suffix):
                print(f"[FAIL] {name} resolved to {path} ({fmt}), expected "
                      f"...{want_suffix}: {why}")
                return 1
            print(f"[OK] {name} -> {path}")

        # The blend/cull flags come off the same definition. `add,culloff` is
        # id's fire (a GL_ONE GL_ONE stage with `cull none`), and the census
        # says how many definitions over the whole map asked for it — the
        # number the draw pass then acts on.
        flags = {m.group(1): m.group(2) for m in TEX_FLAGS_RE.finditer(log)}
        flame = flags.get("textures/mectovtest/probe_flame")
        if flame != "add,culloff":
            print(f"[FAIL] probe_flame's definition asked for `blendFunc "
                  f"GL_ONE GL_ONE` + `cull none` but the line says "
                  f"flags={flame!r} — the stage's own flags are not reaching "
                  f"the draw pass")
            return 1
        dm = DRAW_FLAGS_RE.search(log)
        if not dm:
            print("[FAIL] no draw-flags census line in the log")
            return 1
        if int(dm.group(1)) < 1 or int(dm.group(2)) < 1:
            print(f"[FAIL] the census found additive={dm.group(1)} "
                  f"culloff={dm.group(2)} definitions; the fixture defines one "
                  f"of each")
            return 1
        print(f"[OK] the additive/cull-none flags reached the draw pass: "
              f"{flame} on probe_flame, census additive={dm.group(1)} "
              f"culloff={dm.group(2)}")

        # ---- 5. the world is really drawn ----------------------------------
        if not wait_for_frame(40, 180):
            print("[FAIL] the renderer never reached frame 40")
            dump_tail(40)
            return 1
        frames = [(int(x.group(1)), int(x.group(9))) for x in FRAME_RE.finditer(read_file(SERIAL_LOG))]
        best = max(f for _n, f in frames)
        if best < 8:
            print(f"[FAIL] the most faces a frame drew was {best}")
            return 1
        print(f"[OK] surfaces are submitted: peak {best} faces in one frame")

        # v38.116: the driver histograms every 20 RENDER frames now (the render
        # clock is decoupled from the 50 ms server tick), so samples arrive
        # roughly twice as fast as before and the first four no longer span the
        # walk — views from ALONG the path are needed, not just the spawn.
        # "Settled" is positional: the auto-walk ends against a wall and the
        # frame markers keep coming with an unchanged pos, so once several
        # consecutive markers agree on (x, y), the views are done changing —
        # send ESC then instead of waiting out the whole session cap.
        seen = []
        deadline = time.time() + 240
        while time.time() < deadline:
            samples = [tuple(int(x) for x in m.group(1, 2, 3, 4, 5, 6, 7, 8,
                                                       9, 10))
                       for m in PIXELS_RE.finditer(read_file(SERIAL_LOG))]
            cur = [s for s in samples if s[0] >= 20]
            if len(cur) >= 4:
                seen = cur
            marks = [(int(m.group(1)), int(m.group(3)), int(m.group(4)))
                     for m in FRAME_RE.finditer(read_file(SERIAL_LOG))]
            if len(marks) >= 6:
                last = marks[-4:]
                if len({(x, y) for _f, x, y in last}) == 1:
                    break
            time.sleep(2)
        if not seen:
            print("[FAIL] no classified frames in the log")
            dump_tail(40)
            return 1
        best_px = {}
        for name, idx in (("cyan", 1), ("warm", 2), ("stepgreen", 3),
                          ("violet", 4), ("patch", 6), ("sky", 7),
                          ("wall", 8), ("distinct", 9)):
            best_px[name] = max(s[idx] for s in seen)
        for name, want in (("floor", 5000), ("walls", 200), ("step", 8),
                           ("ceiling", 1000), ("curve (JPEG)", 100)):
            key = {"floor": "cyan", "walls": "wall", "step": "stepgreen",
                   "ceiling": "violet", "curve (JPEG)": "patch"}[name]
            if best_px[key] < want:
                print(f"[FAIL] the {name} never appeared on screen (best "
                      f"{best_px[key]} px, wanted {want})")
                return 1
        if best_px["distinct"] < 10:
            print(f"[FAIL] at most {best_px['distinct']} distinct colours — "
                  f"the JPEG texture is not reaching the screen")
            return 1
        print(f"[OK] the staged map is on screen, textured through its script: "
              f"curve JPEG up to {best_px['patch']} px, floor {best_px['cyan']}, "
              f"distinct colours {best_px['distinct']}")

        # ---- 6. clean exit, OS alive ---------------------------------------
        sendkey("esc")
        if not wait_for_in_file(SERIAL_LOG, DONE_MARKER, args.timeout):
            print("[FAIL] the session never finished")
            dump_tail(40)
            return 1
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited with code {qemu.returncode}")
            return 1
        print("[OK] ESC ended the session, the OS stayed alive")
        print("[PASS] the retail-map path works end to end: a pak0-staged map "
              "with shader-script resolution and JPEG textures, in Mectov OS")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
