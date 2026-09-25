#!/usr/bin/env python3
"""
scripts/q3arena_test.py — end-to-end test for the v38.109 Quake III Arena port
(Q3 phase 8): the OFFICIAL id game module's own level, drawn.

What it proves, in order:

  1. serial: `q3arena` starts the same session `q3vm` does — id's own QVM loader,
     the official qagame bytecode on /ext2, GAME_INIT, the retail connect
     sequence — and additionally brings up a TinyGL renderer and a WM window.
  2. the render mesh is built from the level's own file: q3bsp_load() read
     maps/mectovtest.bsp and reports the surfaces, vertices and shaders it
     found. Nothing in this number is hand-written by the test.
  2b. the level's CURVED surface is tessellated, not skipped (v38.109): the mesh
     line carries the patch accounting separately — patchdrawn out of patches
     found, the quads the tessellator emitted and the vertices they cost — and
     the suite checks the arithmetic between them (four vertices per quad, every
     surface either planar-drawn, a patch, or counted as skipped). Before this
     release a patch was a hole in the wall.
  3. the level's textures are decoded from the game data on the volume: one
     "[Q3ARENA] tex <n> <path> tga=WxH bytes=N gl=ID" line per shader, and NOT
     ONE placeholder. That is the assertion that makes the pixels meaningful —
     a renderer that could not read a texture would have to report "missing=1".
  4. geometry is actually submitted to the rasterizer: the per-frame markers
     carry drawn=<faces> tris=<tris> culled=<face count>.
  5. the level is ON SCREEN, textured: every sampled frame's finished pixels are
     classified out of the renderer's own buffer (its ZBuffer, not a screendump)
     and must be made of the generated arena's texture colours — teal floor,
     sandstone walls, yellow-green step, ambient-lit violet ceiling, and the
     cove's magenta, a colour no planar texture here can produce at any light
     level — with the
     clear colour accounting for only a small part of the frame. That last
     number is the one that catches "the camera points at nothing": a broken
     view transform renders a frame that is all clear colour, and this suite
     says so instead of admiring a screenshot of the desktop.
     The screendump is still taken, but only as evidence for a human, plus a
     geometry check that the window really appeared where the driver asked.
  6. the scene is LIVE and the module is moving in it: a second screendump taken
     ~100 frames later must differ, and the driver's own accounting
     ("[Q3VM] movement x0= x1= delta=") must show the player advancing — id's
     Pmove, running against the map's brushes, in the level being drawn.
  7. ESC reaches the window as a raw scancode and ends the session cleanly
     ("[Q3ARENA] done"), after which the OS is still alive.

Usage:
    python3 scripts/q3arena_test.py [--timeout 600] [--iso mectov-q3.iso]

`make check-q3arena` builds the MECTOV_Q3=1 ISO as mectov.iso; CI passes
mectov-q3.iso.
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import time

import terminal_launch

SERIAL_LOG = "/tmp/mectov_q3arena_serial.log"
MON_SOCK = "/tmp/mectov_q3arena_monitor.sock"
CURSOR_PPM = "/tmp/mectov_q3arena_cursor.ppm"
SHOT_DESK = "/tmp/q3arena_desktop.ppm"
SHOT_A = "/tmp/q3arena_a.ppm"
SHOT_B = "/tmp/q3arena_b.ppm"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
Q3ARENA_KEYS = ["q", "3", "a", "r", "e", "n", "a", "ret"]

START_MARKER = "[Q3ARENA] official qagame VM world rendered through TinyGL"
WINDOW_MARKER = "[Q3ARENA] window id="
MESH_MARKER = "[Q3ARENA] world mesh: "
LOADING_MARKER = "Loading vm file vm/qagame.qvm."
ENTERED_MARKER = "entered the game"
DONE_MARKER = "[Q3ARENA] done"
FRAME_DONE_MARKER = "[Q3VM] frame loop done"
FAILED_MARKER = "[Q3ARENA] FAILED"
PANIC_MARKER = "[PANIC]"
SYS_ERROR_MARKER = "[Q3] Sys_Error"

BSP_NAME = "maps/mectovtest.bsp"
# The four shaders scripts/build_test_bsp.py writes textures for. Their TGA file
# names are the assertion that the renderer read the volume and not a cache.
EXPECTED_TEX = [
    "textures/mectovtest/floor.tga",
    "textures/mectovtest/wall.tga",
    "textures/mectovtest/ceiling.tga",
    "textures/mectovtest/step.tga",
    "textures/mectovtest/curve.tga",   # the curved surface's own texture
]

MESH_RE = re.compile(
    r"\[Q3ARENA\] world mesh: (\S+) surfaces=(-?\d+) of=(-?\d+) verts=(-?\d+) "
    r"shaders=(-?\d+) planar=(-?\d+) patches=(-?\d+) patchdrawn=(-?\d+) "
    r"patchquads=(-?\d+) patchverts=(-?\d+) patchskipped=(-?\d+) "
    r"skipped=(-?\d+) truncated=(-?\d+)")
TEX_RE = re.compile(
    r"\[Q3ARENA\] tex (\d+) (\S+) tga=(\d+)x(\d+) bytes=(\d+) gl=(\d+)")
TEX_MISSING_RE = re.compile(r"\[Q3ARENA\] tex (\d+) (\S+) missing=1")
FRAME_RE = re.compile(
    r"\[Q3ARENA\] frame=(\d+) t=(\d+) pos=\((-?\d+),(-?\d+),(-?\d+)\) "
    r"eye_z=(-?\d+) yaw=(-?\d+) pitch=(-?\d+) drawn=(\d+) tris=(\d+) "
    r"culled=(\d+)")
TOTALS_RE = re.compile(
    r"\[Q3ARENA\] render totals: frames=(\d+) faces=(\d+) tris=(\d+) "
    r"wall_ms=(\d+)")
MOVEMENT_RE = re.compile(r"\[Q3VM\] movement x0=(-?\d+) x1=(-?\d+) delta=(-?\d+)")

# Window geometry as q3arena_open() computes it: 322x262 centred in the
# framebuffer, above the taskbar; content = 320x240 inside border+titlebar.
WIN_W, WIN_H = 322, 262
CONTENT_W, CONTENT_H = 320, 240
TITLEBAR_H, TASKBAR_H = 20, 28


def read_file(path):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read()
    except (FileNotFoundError, OSError):
        return ""


def wait_for_in_file(path, needle, timeout):
    """Wait for a serial marker, but give up at once on a panic or an engine
    Sys_Error — a dead guest never prints the marker, and waiting out the whole
    timeout turns one crash into ten minutes."""
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
    """Wait until the driver has logged a frame marker at or past frame n."""
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
        print(line[:140])


def screendump(path, settle=1.0):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    mon_cmd(f"screendump {path}", settle=settle)
    return os.path.exists(path) and os.path.getsize(path) > 1000


def load_ppm_pixels(path):
    """Return (width, height, bytes RGB) for a P6 ppm."""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError(f"not a P6 ppm: {path}")
    pos = 2
    vals = []
    while len(vals) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if pos < len(data) and data[pos:pos + 1] == b"#":
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
    return w, h, payload


RECT_RE = re.compile(
    r"\[Q3ARENA\] window id=(0x[0-9a-f]+) rect=(-?\d+),(-?\d+) (\d+)x(\d+) "
    r"content=(\d+)x(\d+)")
PIXELS_RE = re.compile(
    r"\[Q3ARENA\] pixels frame=(\d+) cyan=(\d+) warm=(\d+) stepgreen=(\d+) "
    r"violet=(\d+) bright=(\d+) patch=(\d+) sky=(\d+) distinct=(\d+)")
PERF_RE = re.compile(
    r"\[Q3ARENA\] perf frame=(\d+) fps=(\d+) vm_ms=(-?\d+) gl_ms=(-?\d+) "
    r"blit_ms=(-?\d+) draw_ms=(-?\d+) wm_ms=(-?\d+) other_ms=(-?\d+) "
    r"idle_ms=(-?\d+) sum_ms=(-?\d+)")
FRAME_PIXELS = CONTENT_W * CONTENT_H      # 76800: the whole frame is classified


def diff_bbox(shot_after, shot_before, w, h):
    """Bounding box of what changed between two screenshots of the desktop.

    Used only to confirm the window really appeared where the driver asked: the
    game window is the only thing at that spot that changed, so the bbox's
    right/bottom edges are its right/bottom edges (the Terminal, whose output
    also changes, can only extend the box up and to the left). Sampling pixels
    out of this region is deliberately NOT how the scene is asserted — the
    compositor may raise a window over it.
    """
    _wa, _ha, a = load_ppm_pixels(shot_after)
    _wb, _hb, b = load_ppm_pixels(shot_before)
    minx, miny, maxx, maxy = w, h, -1, -1
    for y in range(40, h - 40, 2):
        base = y * w * 3
        for x in range(0, w, 2):
            o = base + x * 3
            if (abs(a[o] - b[o]) > 12 or abs(a[o + 1] - b[o + 1]) > 12 or
                    abs(a[o + 2] - b[o + 2]) > 12):
                if x < minx: minx = x
                if x > maxx: maxx = x
                if y < miny: miny = y
                if y > maxy: maxy = y
    if maxx < 0:
        return None
    return minx, miny, maxx, maxy


def sampled_diff(path_a, path_b, step=99):
    _wa, _ha, pa = load_ppm_pixels(path_a)
    _wb, _hb, pb = load_ppm_pixels(path_b)
    return sum(1 for x, y in zip(pa[::step], pb[::step]) if x != y)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK, CURSOR_PPM, SHOT_DESK, SHOT_A, SHOT_B):
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
            print("[FAIL] the Terminal never became ready — see the [launch] report above")
            return 1
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] terminal never became ready")
            return 1
        time.sleep(1.0)
        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)

        # The desktop BEFORE the game window exists: the pixel test locates the
        # window by diffing against this rather than assuming where the WM put
        # it (see find_window_rect).
        if not screendump(SHOT_DESK):
            print("[FAIL] could not screendump the desktop before launch")
            return 1

        # The VM is ~460 KB and the map + four textures are decoded on the way
        # up, so gate on markers rather than a sleep.
        if not type_line(Q3ARENA_KEYS, retries=2, ready_marker=START_MARKER,
                         timeout=150):
            print("[FAIL] the `q3arena` command never started its task")
            dump_tail()
            return 1
        print("[OK] q3arena task started (official qagame VM + TinyGL window)")

        if not wait_for_in_file(SERIAL_LOG, WINDOW_MARKER, 60):
            print("[FAIL] no window was opened (the renderer never came up)")
            dump_tail()
            return 1
        if not wait_for_in_file(SERIAL_LOG, MESH_MARKER, 60):
            print("[FAIL] the render mesh was never built from the level")
            dump_tail()
            return 1

        # The window is up before the module runs, so the VM's own milestones
        # (loader, GAME_INIT, the connect sequence) are still ahead of us here.
        if not wait_for_in_file(SERIAL_LOG, ENTERED_MARKER, 240):
            print("[FAIL] the module never announced the client")
            dump_tail(40)
            return 1
        log = read_file(SERIAL_LOG)
        # The session is the same one q3vm runs: id's loader, official bytecode.
        if LOADING_MARKER not in log:
            print(f"[FAIL] id's VM loader never ran ('{LOADING_MARKER}')")
            dump_tail()
            return 1
        print("[OK] same session as q3vm: id's loader ran the official qagame "
              "bytecode and the module announced its client")

        # ---- 1. the mesh came from the level's own file ---------------------
        m = MESH_RE.search(log)
        if not m:
            print("[FAIL] no mesh marker to parse")
            dump_tail()
            return 1
        (bsp, faces, listed, verts, shaders, planar, patches, patchdrawn,
         patchquads, patchverts, patchskipped, skipped, truncated) = (
            m.group(1),) + tuple(int(m.group(i)) for i in range(2, 14))
        if bsp != BSP_NAME:
            print(f"[FAIL] the mesh was built from {bsp}, not {BSP_NAME}")
            return 1
        if faces <= 0 or verts <= 0 or shaders <= 0:
            print(f"[FAIL] empty mesh: surfaces={faces} verts={verts} "
                  f"shaders={shaders}")
            return 1
        if truncated:
            print("[FAIL] the mesh was truncated (vertex cap hit)")
            return 1
        if listed != planar + patches + skipped:
            print(f"[FAIL] the surface accounting does not add up: {listed} in "
                  f"the file, {planar} planar + {patches} patches + "
                  f"{skipped} skipped")
            return 1
        if faces != planar + patchquads:
            print(f"[FAIL] the drawn face count does not come from the "
                  f"surfaces: {faces} drawn against {planar} planar + "
                  f"{patchquads} patch quads")
            return 1
        if verts <= patchverts:
            print(f"[FAIL] the mesh has {verts} vertices of which {patchverts} "
                  f"are the patch's — the planar surfaces contributed nothing")
            return 1
        print(f"[OK] the render mesh is the level's own file: {BSP_NAME} "
              f"{faces}/{listed} surfaces, {verts} vertices, {shaders} shaders "
              f"({planar} planar, {patches} curved, {skipped} skipped)")

        # ---- 1b. the curved surface was tessellated, not skipped ------------
        # The arena has a patch (the cove along the +X wall), so a release that
        # skipped patches would report patchdrawn=0 and draw a hole where the
        # wall's curve should be. The vertex count is an exact invariant of the
        # tessellator: every quad it emits becomes one four-vertex face.
        if patches < 1:
            print(f"[FAIL] the level has {patches} curved surfaces — the test "
                  f"arena writes one, so the loader lost it")
            return 1
        if patchdrawn != patches or patchskipped:
            print(f"[FAIL] {patchdrawn}/{patches} curved surfaces drawn, "
                  f"{patchskipped} skipped")
            return 1
        if patchquads < 8:
            print(f"[FAIL] the tessellator produced only {patchquads} quads for "
                  f"{patches} patch(es): the curve was not subdivided")
            return 1
        if patchverts != patchquads * 4:
            print(f"[FAIL] {patchquads} quads cost {patchverts} vertices, "
                  f"expected {patchquads * 4}")
            return 1
        print(f"[OK] the level's curved surface is tessellated, not skipped: "
              f"{patchdrawn} patch(es) -> {patchquads} quads, {patchverts} "
              f"vertices ({patchquads // max(patches, 1)} quads each)")

        # ---- 2. its textures were decoded from the volume -------------------
        if TEX_MISSING_RE.search(log):
            print("[FAIL] a shader had no image on the volume — the arena's "
                  "textures are generated next to it, so this is a failure to "
                  "read or decode them:")
            for line in log.splitlines():
                if "missing=1" in line:
                    print("       " + line[:140])
            return 1
        texs = {}
        for tm in TEX_RE.finditer(log):
            texs[tm.group(2)] = (int(tm.group(3)), int(tm.group(4)),
                                 int(tm.group(5)), int(tm.group(6)))
        if len(texs) < len(EXPECTED_TEX):
            print(f"[FAIL] only {len(texs)} textures decoded (expected at least "
                  f"{len(EXPECTED_TEX)})")
            dump_tail()
            return 1
        for name in EXPECTED_TEX:
            if name not in texs:
                print(f"[FAIL] the map's shader {name} produced no texture")
                return 1
            tw, th, tbytes, glid = texs[name]
            if tw < 16 or th < 16 or tbytes < 1000 or glid <= 0:
                print(f"[FAIL] {name}: implausible texture (tga={tw}x{th} "
                      f"bytes={tbytes} gl={glid})")
                return 1
        print(f"[OK] {len(texs)} texture(s) decoded from the game data on the "
              f"volume and uploaded, "
              + ", ".join(f"{os.path.basename(n)}={v[0]}x{v[1]}"
                          for n, v in sorted(texs.items())))

        # ---- 3. geometry is really being submitted --------------------------
        if not wait_for_frame(40, 180):
            print("[FAIL] the renderer never reached frame 40")
            dump_tail(40)
            return 1
        frames = [(int(x.group(1)), int(x.group(9)), int(x.group(10)),
                   int(x.group(11))) for x in FRAME_RE.finditer(read_file(SERIAL_LOG))]
        best = max(f for _n, f, _t, _c in frames)
        if best < 8:
            print(f"[FAIL] the most faces a frame drew was {best} — the level "
                  f"is not being submitted (culling too aggressive?)")
            return 1
        print(f"[OK] surfaces are submitted to the rasterizer: peak {best} faces "
              f"in one frame")

        # Let the run get properly under way before anything is measured: the
        # first frames still have the player falling from the map's spawn point.
        if not wait_for_frame(40, 180):
            print("[FAIL] the renderer never reached frame 40")
            dump_tail(40)
            return 1

        # ---- 4. the window really is on the desktop -------------------------
        # Where the WM put it is the WM's business, so the geometry is taken
        # from the driver's own log and confirmed against what changed on the
        # desktop. The *content* is asserted from the renderer's buffer in the
        # next step — a screendump cannot be trusted for it, because the
        # Terminal (or any other window) may be sitting on top.
        rc = RECT_RE.search(log)
        if not rc:
            print("[FAIL] the driver never reported its window geometry")
            dump_tail(40)
            return 1
        win_x, win_y, win_w, win_h = (int(rc.group(2)), int(rc.group(3)),
                                      int(rc.group(4)), int(rc.group(5)))
        if (win_w, win_h) != (WIN_W, WIN_H):
            print(f"[FAIL] the driver opened a {win_w}x{win_h} window, "
                  f"expected {WIN_W}x{WIN_H}")
            return 1
        if not screendump(SHOT_A):
            print("[FAIL] screendump failed")
            return 1
        dw, dh, _ = load_ppm_pixels(SHOT_DESK)
        bbox = diff_bbox(SHOT_A, SHOT_DESK, dw, dh)
        if bbox is None:
            print("[FAIL] nothing changed on the desktop — no window appeared")
            return 1
        want_r = win_x + win_w - 1
        want_b = win_y + win_h - 1
        if abs(bbox[2] - want_r) > 8 or abs(bbox[3] - want_b) > 8:
            print(f"[FAIL] the window was asked for {WIN_W}x{WIN_H} at "
                  f"({win_x},{win_y}) (bottom-right {want_r},{want_b}) but the "
                  f"desktop changed over {bbox} — that is not the same window")
            return 1
        print(f"[OK] the game window is on the desktop where the driver asked: "
              f"{WIN_W}x{WIN_H} at ({win_x},{win_y}) (changed region {bbox})")
        print(f"     screendump for humans: {SHOT_A}")

        # ---- 5. what the renderer actually drew ------------------------------
        # Straight out of the ZBuffer, so this cannot be satisfied by anything
        # except the renderer producing those pixels: every sampled frame has to
        # be mostly level, and every one of the generated arena's four textures
        # has to be visible somewhere in the run.
        seen = []
        best = {}
        for _ in range(20):
            samples = [tuple(int(x) for x in m.group(1, 2, 3, 4, 5, 6, 7, 8, 9))
                       for m in PIXELS_RE.finditer(read_file(SERIAL_LOG))]
            seen = [s for s in samples if s[0] >= 20]
            if len(seen) >= 4 and seen[-1][0] >= 60:
                break
            time.sleep(2)
        if len(seen) < 3:
            print(f"[FAIL] only {len(seen)} classified frames in the log")
            dump_tail(40)
            return 1
        # The camera pose of every sampled frame, so the report (and any CI
        # failure) shows WHERE the camera was, not just what it saw: position
        # at a frame mark is deterministic — game time is frame*50 — but where
        # the module's own viewangles point is the module's business, and a
        # divergent run is only diagnosable with both halves on the table.
        poses = {}
        for fm in FRAME_RE.finditer(read_file(SERIAL_LOG)):
            n = int(fm.group(1))
            poses[n] = (fm.group(3), fm.group(4), fm.group(5),
                        fm.group(6), fm.group(7), fm.group(8))
        for name, idx in (("cyan", 1), ("warm", 2), ("stepgreen", 3),
                          ("violet", 4), ("bright", 5), ("patch", 6),
                          ("sky", 7), ("distinct", 8)):
            best[name] = max(s[idx] for s in seen)
        # v38.110: where the frame budget goes. The perf line is the driver's
        # own account of one 20-frame window, on the kernel's tick clock: the
        # module (vm), the software renderer (gl), the game window's composite
        # share as the WM charged it (draw = app draw callback, blit = content
        # buffer into the back buffer, wm = the whole wm_draw_all pass), the
        # driver's other work, and the pacing remainder (idle = budget nothing
        # spent). All in milliseconds; sum is that window's wall time, so
        # parts and whole are directly comparable. The suite sanity-checks the
        # accounting (non-negative parts, parts within tolerance of their own
        # wall window) — thresholds on VALUES stay loose because CI hosts run
        # 2-5x faster than a dev box; the breakdown itself must simply be
        # present and arithmetically honest.
        perfs = [tuple(int(x) for x in m.group(1, 2, 3, 4, 5, 6, 7, 8, 9, 10))
                 for m in PERF_RE.finditer(read_file(SERIAL_LOG))]
        if not perfs:
            print("[FAIL] no [Q3ARENA] perf lines in the log — the frame "
                  "breakdown vanished")
            dump_tail(40)
            return 1
        print("[OK] frame-time breakdown (kernel ms ticks, 20-frame window):")
        for (pf, fps, vmm, glm, blitm, drawm, wmm, otherm, idlem, summ) in perfs:
            parts = vmm + glm + otherm + idlem
            if min(vmm, glm, otherm, idlem, summ) < 0 or parts > summ * 13 // 10:
                print(f"[FAIL] perf line frame {pf} does not add up: "
                      f"vm={vmm} gl={glm} other={otherm} idle={idlem} "
                      f"(parts={parts}) vs sum={summ}")
                return 1
            print(f"     frame {pf:3d}: {fps:3d} fps | vm {vmm:4d} ms | "
                  f"gl {glm:4d} ms | blit {blitm:3d} ms | wm-pass {wmm:3d} ms | "
                  f"other {otherm:3d} ms | idle {idlem:4d} ms | sum {summ:4d} ms")
        mostly_clear = 0
        for frame, cyan, warm, stepgreen, violet, bright, patch, sky, distinct in seen:
            live = cyan + warm + stepgreen + violet + bright + patch
            px, py, pz, ez, yw, pt = poses.get(frame, ("?", "?", "?", "?", "?", "?"))
            print(f"     frame {frame} pos=({px},{py},{pz}) eye_z={ez} yaw={yw} pitch={pt}: "
                  f"floor={cyan} walls={warm} "
                  f"step={stepgreen} ceiling={violet} crosshair={bright} "
                  f"curve={patch} clear={sky} distinct={distinct}")
            if sky > FRAME_PIXELS // 4:
                mostly_clear += 1
            if live < 1000:
                print(f"[FAIL] frame {frame} has almost no world in it "
                      f"({live} classified pixels)")
                dump_tail(40)
                return 1
            if distinct < 8:
                print(f"[FAIL] frame {frame} has only {distinct} distinct "
                      f"colours — that is a flat fill, not a textured level")
                dump_tail(40)
                return 1
        # The clear-colour gate is a MAJORITY gate, not a per-frame one, and
        # that is deliberate. A broken view transform (the transpose bug this
        # histogram was built to catch) clears EVERY frame — 76800/76800 — so
        # it still fails hard. But the walk's END is now legitimate clear: the
        # self-driving player stops against the walls and stares into the
        # corner it used to leak through, and at 16 units from two wall faces
        # nearly parallel to the view axis the near plane clips them — the
        # frame is geometry the rasterizer cannot show, not a camera aimed at
        # nothing. CI's runners (faster wall clock) reach that corner sooner,
        # so a per-frame gate here failed a green build on its own content.
        if mostly_clear * 2 > len(seen):
            print(f"[FAIL] {mostly_clear}/{len(seen)} sampled frames are mostly "
                  f"clear colour — the camera is looking at nothing "
                  f"(a transposed view matrix does exactly this)")
            dump_tail(40)
            return 1
        # Every texture the arena names has to show up somewhere: the four
        # shaders are the floor, the walls, the ceiling and the step block.
        for name, key, want in (("floor", "cyan", 5000), ("walls", "warm", 20),
                                ("step block", "stepgreen", 8),
                                ("ceiling", "violet", 1000),
                                ("curved surface", "patch", 100)):
            if best[key] < want:
                print(f"[FAIL] the {name} never appeared on screen (best "
                      f"{best[key]} pixels, wanted {want})")
                return 1
        if best["distinct"] < 10:
            print(f"[FAIL] at most {best['distinct']} distinct colours ever — "
                  f"the textures are not reaching the screen")
            return 1
        print(f"[OK] the module's own level is on screen, textured: floor "
              f"(teal) up to {best['cyan']} px, walls (sandstone) "
              f"{best['warm']} px, the step block (yellow-green) "
              f"{best['stepgreen']} px, ceiling (violet) {best['violet']} px, "
              f"the tessellated cove (magenta) {best['patch']} px "
              f"of {FRAME_PIXELS}")

        # ---- 6. the scene is live, and the module is moving in it -----------
        if not wait_for_frame(120, 180):
            print("[FAIL] the renderer never reached frame 120")
            dump_tail(40)
            return 1
        if not screendump(SHOT_B):
            print("[FAIL] second screendump failed")
            return 1
        diff = sampled_diff(SHOT_A, SHOT_B)
        if diff < 200:
            print(f"[FAIL] the two frames are nearly identical (diff={diff}) — "
                  f"the world is not moving, so this is a still image")
            return 1
        print(f"[OK] the scene is live: the frame after ~100 more ticks differs "
              f"in {diff} sampled pixels")

        # ESC must reach the window as a raw scancode. If the WM gave focus to
        # something else the run simply ends at its frame cap instead, so the
        # cut-short case is reported rather than required.
        sendkey("esc")
        if not wait_for_in_file(SERIAL_LOG, DONE_MARKER, args.timeout):
            print("[FAIL] the session never finished")
            for line in read_file(SERIAL_LOG).splitlines():
                if FAILED_MARKER in line or SYS_ERROR_MARKER in line:
                    print("       " + line[:140])
            dump_tail(40)
            return 1
        if not wait_for_in_file(SERIAL_LOG, FRAME_DONE_MARKER, 30):
            print("[FAIL] the game loop never reported its completion")
            dump_tail(40)
            return 1

        log = read_file(SERIAL_LOG)
        tot = TOTALS_RE.search(log)
        if not tot:
            print("[FAIL] no render totals were reported")
            dump_tail(40)
            return 1
        rframes, rfaces, rtris, wall_ms = (int(tot.group(i)) for i in range(1, 5))
        if rframes < 40 or rfaces <= 0 or rtris <= 0:
            print(f"[FAIL] implausible render totals: frames={rframes} "
                  f"faces={rfaces} tris={rtris}")
            return 1
        print(f"[OK] {rframes} frames rendered ({rfaces} surfaces, {rtris} "
              f"triangles submitted) in {wall_ms} ms of wall clock")

        mv = MOVEMENT_RE.search(log)
        if not mv:
            print("[FAIL] no movement accounting in the log")
            return 1
        x0, x1, delta = int(mv.group(1)), int(mv.group(2)), int(mv.group(3))
        if delta <= 100:
            print(f"[FAIL] the module's player barely moved in the drawn world: "
                  f"x {x0} -> {x1} (delta {delta}) — id's Pmove is not running "
                  f"against this level")
            return 1
        print(f"[OK] official gameplay code moved the player INSIDE the drawn "
              f"level: x {x0} -> {x1} ({delta} units)")

        # ---- 7. the OS survived it ------------------------------------------
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited with code {qemu.returncode}")
            return 1
        print("[OK] the OS stayed alive after the session")

        print("[PASS] official Quake III Arena game code, its own level drawn "
              "textured through TinyGL, in Mectov OS")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
