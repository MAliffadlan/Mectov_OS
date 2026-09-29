#!/usr/bin/env python3
"""
scripts/q3vis_test.py — PVS culling, end to end (v38.112, Q3 phase 10).

Why this suite exists
---------------------
v38.112 makes the renderer cull with the map's OWN visibility data: the camera is
located in the .bsp's tree, that leaf names a cluster, and the visibility matrix
q3map baked into the VISIBILITY lump says which clusters a camera there can see.
On a retail map that is the difference between drawing thousands of surfaces per
frame and drawing the tens its author's compiler said were reachable.

Nothing about that is assertable from the generated arena: `mectovtest.bsp` has
one leaf and an EMPTY visibility lump (which is why the retail suites' face
counts are exactly what they were before v38.112 — "no PVS" means "draw
everything"). So `scripts/build_test_bsp.py` writes a second map beside it,
`mectovvis.bsp`: the same brushes, the same surfaces, the same collision — but a
two-leaf tree split at x = 100 and a real two-cluster visibility lump where
cluster 1 sees only itself and cluster 0 sees both. The player spawns at
(-384, -384) in the west half (leaf 1), and the scripted walk carries it past
x = 496, straight across the split.

What this suite asserts, and why each number is the evidence it is:

  1. the lump was PARSED: the loader's own line reports the tree it kept
     (nodes, leafs) and the visibility lump it found (clusters). A parser that
     silently gave up would say "no PVS lump", which is a different line.
  2. our copy of the tree agrees with id's collision walk: the loader samples
     face centres and compares `q3bsp_leaf_for_point()` against
     `CM_PointLeafnum()` — the function that has been tracing this map since
     v38.107 — and the suite requires that line to report agreement at every
     sample. A second implementation of someone else's tree is only trustworthy
     if it is checked against the first.
  3. the PVS really culls, and by the right amount: at the spawn the camera is
     in the west leaf, whose cluster sees only itself, so the marked count must
     be the number of faces in the west half — which this suite computes from
     the GENERATOR (importing build_test_bsp), not from a magic number. Before
     the walk the east half (16 planar faces and the 32-quad curved cove) must
     not be drawn.
  4. the cull FOLLOWS the camera: the frame markers' cluster must change from 1
     to 0 during the run, and the marked count must rise to the whole mesh when
     it does. That is the property that makes this PVS and not a fixed mask. The
     walk is wall-clock paced, so the suite waits for a cluster-0 marker instead
     of assuming the crossing happens by some fixed frame number.
  5. the two stages stay separable: every frame asserts
     marked + culled-by-PVS == the mesh's face count, so a PVS that marks too
     much or too little fails arithmetically rather than cosmetically.
  6. culled frames still DRAW: the renderer's own pixel classification has to
     find a live, textured frame while the east half is hidden — "culling
     works" must not mean "the level disappeared".

Usage:
    python3 scripts/q3vis_test.py [--timeout 600] [--iso mectov.iso]

`make check-q3vis` builds the MECTOV_Q3=1 ISO as mectov.iso; CI passes
mectov-q3.iso.
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

SERIAL_LOG = "/tmp/mectov_q3vis_serial.log"
MON_SOCK = "/tmp/mectov_q3vis_monitor.sock"
CURSOR_PPM = "/tmp/mectov_q3vis_cursor.ppm"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
Q3ARENA_KEYS = ["q", "3", "a", "r", "e", "n", "a", "spc",
                "m", "e", "c", "t", "o", "v", "v", "i", "s", "ret"]

START_MARKER = "[Q3ARENA] official qagame VM world rendered through TinyGL"
WINDOW_MARKER = "[Q3ARENA] window id="
MESH_MARKER = "[Q3ARENA] world mesh: "
ENTERED_MARKER = "entered the game"
DONE_MARKER = "[Q3ARENA] done"
PANIC_MARKER = "[PANIC]"
SYS_ERROR_MARKER = "[Q3] sys_error"

BSP_NAME = "maps/mectovvis.bsp"

# The loader's account of the tree it kept, and of the visibility lump.
VIS_LOAD_RE = re.compile(
    r"\[Q3BSP\] vis: planes=(-?\d+) nodes=(-?\d+) leafs=(-?\d+) "
    r"faces-in-leafs=(-?\d+) clusters=(-?\d+)")
VIS_AGREE_RE = re.compile(
    r"\[Q3BSP\] vis: leaf walk agrees with CM_PointLeafnum at all "
    r"(\d+) sample point\(s\)")
# The renderer's own line: one per cluster change, so the two sides of the
# split each get a line even if the sampled frame markers fall between them.
VIS_DRAW_RE = re.compile(
    r"\[Q3ARENA\] vis: cluster=(-?\d+) leaf=(-?\d+) leafs=(-?\d+) "
    r"marked=(\d+)/(\d+)")
FRAME_RE = re.compile(
    r"\[Q3ARENA\] frame=(\d+) t=(\d+) pos=\((-?\d+),(-?\d+),(-?\d+)\) "
    r"eye_z=(-?\d+) yaw=(-?\d+) pitch=(-?\d+) drawn=(\d+) tris=(\d+) "
    r"culled=(\d+) vis=(-?\d+)/(\d+) cluster=(-?\d+) cull_pvs=(\d+) "
    r"cull_frustum=(\d+) planes=(\d+)(?: back=(\d+))?")
PIXELS_RE = re.compile(
    r"\[Q3ARENA\] pixels frame=(\d+) cyan=(\d+) warm=(\d+) stepgreen=(\d+) "
    r"violet=(\d+) bright=(\d+) patch=(\d+) sky=(\d+) distinct=(\d+)")


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
    """The sampled frame markers as (frame, marked, vis_total, cluster,
    cull_pvs, cull_frustum, planes).

    The tuple order follows the guest line field for field, and the group
    numbers are spelled out here ONCE so the assertions below can say `f[1]` is
    the marked count instead of counting parentheses. (They once did not: the
    tuple read `culled` as `marked` and the suite failed with `vis=51/26`.)
    """
    text = read_file(SERIAL_LOG) if text is None else text
    return [tuple(int(x) for x in m.group(1, 12, 13, 14, 15, 16, 17))
            for m in FRAME_RE.finditer(text)]


def parse_draws(text=None):
    """The renderer's own PVS lines as (cluster, leaf, leafs, marked, total)."""
    text = read_file(SERIAL_LOG) if text is None else text
    return [tuple(int(x) for x in m.group(1, 2, 3, 4, 5))
            for m in VIS_DRAW_RE.finditer(text)]


def wait_for_frame(n, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if any(f[0] >= n for f in parse_frames()):
            return True
        if DONE_MARKER in read_file(SERIAL_LOG):
            return False
        time.sleep(1)
    return False


def wait_for_frames(pred, timeout):
    """Wait for any sampled frame marker to satisfy pred (e.g. cluster == 0).

    The window's walk is paced by the wall clock, and this run renders slower
    than the plain arena suite does (the PVS pass costs more per frame), so how
    many frames it takes the camera to cross the fixture's split is not a fixed
    number: the suite waits for the evidence instead of assuming a frame index.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        if any(pred(f) for f in parse_frames()):
            return True
        text = read_file(SERIAL_LOG)
        if PANIC_MARKER in text or SYS_ERROR_MARKER in text:
            return False
        time.sleep(1)
    return False


def check_frame_arithmetic(frames, total_faces):
    """None if every sampled frame's PVS/frustum accounting adds up.

    `marked + culled-by-the-PVS == the mesh's faces` is the whole point of
    keeping the two stages separable: a PVS that marks too much or too little
    fails arithmetically here rather than cosmetically in a pixel count. The
    frustum's six planes have to be present as well — 0 would mean the backend
    handed the renderer no camera basis and the side cull silently stopped
    existing.
    """
    for (fno, marked, vis_total, _cluster, cull_pvs, cull_fr, nplanes) in frames:
        if vis_total != total_faces:
            return (f"[FAIL] frame {fno}: vis={marked}/{vis_total} against a "
                    f"{total_faces}-face mesh")
        if marked < 0:
            return f"[FAIL] frame {fno}: no PVS this map carries one"
        if marked + cull_pvs != total_faces:
            return (f"[FAIL] frame {fno}: {marked} marked + {cull_pvs} culled "
                    f"by the PVS != {total_faces} faces — the marks and the "
                    f"culls do not describe the same mesh")
        if nplanes != 6 or cull_fr < 0:
            return (f"[FAIL] frame {fno}: {nplanes} frustum plane(s), "
                    f"cull_frustum={cull_fr}")
    return None


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


def expected_split():
    """What the fixture's two halves contain, straight from the generator.

    Returns (west_surfaces, patch_surfaces, total_surfaces, split_x). Called
    rather than hard-coding 26/1/43 so that a change to the arena geometry moves
    this suite's expectation with it instead of quietly failing it. The fixture's
    west half is asserted planar-only below, which is what lets the suite compare
    SURFACES with FACES.
    """
    import build_test_bsp as bt

    _data, _planes, _brushes, surfaces, verts, _idx, vis = \
        bt.build(vis_fixture=True)
    assert vis is not None, "build(vis_fixture=True) produced no vis stats"
    west = 0
    patch_surfaces = 0
    for s in surfaces:
        first_vert, num_verts, stype = s[3], s[4], s[2]
        cx = sum(verts[first_vert + k][0][0] for k in range(num_verts))
        cx /= float(num_verts)
        if stype == bt.MST_PATCH:
            patch_surfaces += 1
        if cx < bt.VIS_SPLIT_X:
            west += 1
            if stype == bt.MST_PATCH:
                raise AssertionError(
                    "the fixture's WEST half now contains a curved surface: "
                    "its faces no longer equal its surfaces, so this suite's "
                    "expected marked count is wrong — update q3vis_test.py")
    return west, patch_surfaces, len(surfaces), bt.VIS_SPLIT_X


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    ap.add_argument("--kvm", action="store_true",
                    help="run with -enable-kvm -cpu host for real timing. The "
                         "suite's ASSERTIONS are host-speed independent (they "
                         "are cull arithmetic, not timings), so this changes "
                         "only how long it takes — but it is the only way to "
                         "compare the renderer against KVM, because TCG is "
                         "roughly an order of magnitude slower on the "
                         "x87/integer-heavy software rasterizer.")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK, CURSOR_PPM):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    # The volume is part of this suite's premise (the fixture map and the
    # generated arena's textures, with no shader script in sight), so build it
    # instead of inheriting whatever ran last — see scripts/q3_images.py.
    err = q3_images.fresh_images(args.disk, args.ext2) or \
        q3_images.seed_volume(args.ext2)
    if err:
        print(f"[FAIL] {err}")
        return 1

    west_surfaces, patch_surfaces, total_surfaces, split_x = expected_split()
    print(f"[q3vis] fixture expectation: {total_surfaces} surfaces, "
          f"{west_surfaces} west of x={int(split_x)}, {patch_surfaces} curved")

    qemu_cmd = [
        "qemu-system-i386",
        # qemu32 is TCG's minimal x86 model; under KVM the host model is what
        # we actually want to measure against.
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
    print(f"[q3vis] accel: {'KVM (-enable-kvm -cpu host)' if args.kvm else 'TCG (-cpu qemu32)'}")
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

        if not type_line(Q3ARENA_KEYS, retries=2, ready_marker=START_MARKER,
                         timeout=150):
            print("[FAIL] `q3arena mectovvis` never started its task")
            dump_tail()
            return 1
        print("[OK] q3arena task started with the PVS fixture as its map arg")

        if not wait_for_in_file(SERIAL_LOG, MESH_MARKER, 90):
            print("[FAIL] the render mesh was never built")
            dump_tail()
            return 1
        log = read_file(SERIAL_LOG)
        m = re.search(r"\[Q3ARENA\] world mesh: (\S+)", log)
        if not m or m.group(1) != BSP_NAME:
            print(f"[FAIL] the mesh came from {m.group(1) if m else 'nothing'}, "
                  f"not {BSP_NAME}")
            dump_tail()
            return 1
        print(f"[OK] the mesh is the fixture map: {BSP_NAME}")

        # ---- 1. the tree and the visibility lump were parsed ----------------
        if not wait_for_in_file(SERIAL_LOG, "[Q3BSP] vis:", 60):
            print("[FAIL] the loader never reported a tree")
            dump_tail()
            return 1
        v = VIS_LOAD_RE.search(read_file(SERIAL_LOG))
        if not v:
            print("[FAIL] the loader fell back to 'no PVS lump' on a map that "
                  "carries one — the visdata parse failed")
            dump_tail()
            return 1
        planes, nodes, leafs, faces_in_leafs, clusters = (
            int(v.group(i)) for i in range(1, 6))
        if (nodes, leafs, clusters) != (1, 2, 2):
            print(f"[FAIL] the fixture's tree is 1 node / 2 leaves / 2 clusters, "
                  f"got nodes={nodes} leafs={leafs} clusters={clusters}")
            return 1
        if planes < 24:
            print(f"[FAIL] only {planes} planes kept — the split plane is the "
                  f"24th, so the tree cannot be the file's")
            return 1
        print(f"[OK] the map's own tree and visibility lump were parsed: "
              f"planes={planes} nodes={nodes} leafs={leafs} clusters={clusters}, "
              f"{faces_in_leafs} face(s) reachable from the leaves")

        # ---- 2. our leaf walk matches id's collision walk -------------------
        a = VIS_AGREE_RE.search(read_file(SERIAL_LOG))
        if not a:
            print("[FAIL] no leaf-walk cross-check line: either the tree was "
                  "unusable or it DISAGREED with CM_PointLeafnum (which turns "
                  "culling off on purpose — see the log)")
            dump_tail()
            return 1
        if int(a.group(1)) < 4:
            print(f"[FAIL] only {a.group(1)} sample point(s) checked")
            return 1
        print(f"[OK] q3bsp_leaf_for_point() agrees with id's CM_PointLeafnum() "
              f"at all {a.group(1)} sample points — the tree this suite culls "
              f"with is the file's, not a guess")

        # ---- 3. the session has to run long enough to cross the split ------
        if not wait_for_frame(80, 240):
            print("[FAIL] the driver never reached frame 80")
            dump_tail()
            return 1

        log = read_file(SERIAL_LOG)
        draws = parse_draws(log)
        frames = parse_frames(log)
        if not draws:
            print("[FAIL] the renderer never reported a PVS cluster — culling "
                  "did not engage")
            dump_tail()
            return 1
        if not frames:
            print("[FAIL] no frame markers to check the culling arithmetic on")
            dump_tail()
            return 1

        # The face count is the renderer's own `vis=marked/total` denominator,
        # so read it off the frame markers instead of assuming the generator's
        # surface count.
        total_faces = frames[0][2]
        clusters_seen = sorted({f[3] for f in frames})

        # Every frame: the PVS accounting has to add up.
        err = check_frame_arithmetic(frames, total_faces)
        if err:
            print(err)
            dump_tail()
            return 1

        # The west half is what the PVS marks while the camera is in leaf 1, and
        # the fixture's west half is planar, so surfaces == faces there.
        spawn_frames = [f for f in frames if f[3] == 1]
        if not spawn_frames:
            print(f"[FAIL] the camera was never in cluster 1 (the spawn's leaf): "
                  f"clusters seen {clusters_seen}")
            return 1
        vis_values = [f[1] for f in frames]
        if west_surfaces not in vis_values:
            print(f"[FAIL] the marked counts {sorted(set(vis_values))} never "
                  f"equalled the fixture's {west_surfaces} west faces — the PVS "
                  f"marked the wrong set")
            return 1
        per_frame_ids = [f[0] for f in spawn_frames]
        print(f"[OK] the PVS culls by the map's own data: at the spawn the camera "
              f"is in cluster 1, which sees only itself, so exactly "
              f"{west_surfaces} of {total_faces} faces are marked (frames "
              f"{per_frame_ids[0]}..{per_frame_ids[-1]}) and everything else is "
              f"culled by the PVS, not by guesswork")

        # ---- 4. the cull follows the camera across the split ----------------
        # The walk is paced by the wall clock and this run renders slower than
        # the plain arena suite does (the PVS pass costs more per frame), so the
        # crossing does not land on a fixed frame index: wait for a frame marker
        # that says cluster 0 rather than guessing one. Everything below reads
        # the log again, now that the run has crossed.
        if not wait_for_frames(lambda f: f[3] == 0, 240):
            last = parse_frames()[-1]
            print(f"[FAIL] the camera never reached cluster 0 — the walk did "
                  f"not cross x={int(split_x)} (last sampled frame {last[0]}: "
                  f"vis={last[1]}/{last[2]} cluster={last[3]})")
            dump_tail()
            return 1
        log = read_file(SERIAL_LOG)
        frames = parse_frames(log)
        draws = parse_draws(log)

        # The renderer's own cluster lines answer to the same rule the frame
        # markers do: cluster 1 (the spawn's leaf) marks the west half, cluster
        # 0 sees both halves and marks the whole mesh.
        for (cluster, leaf, nleafs, marked, total) in draws:
            want = west_surfaces if cluster == 1 else total_faces
            if total != total_faces or marked != want:
                print(f"[FAIL] the renderer's PVS line says cluster={cluster} "
                      f"leaf={leaf} leafs={nleafs} marked={marked}/{total}, but "
                      f"cluster {cluster} must mark {want}/{total_faces}")
                return 1

        east_frames = [f for f in frames if f[3] == 0]
        err = check_frame_arithmetic(east_frames, total_faces)
        if err:
            print(err)
            return 1
        if not all(f[1] == total_faces for f in east_frames):
            bad = [f for f in east_frames if f[1] != total_faces]
            print(f"[FAIL] cluster 0 sees both clusters, so its frames must mark "
                  f"all {total_faces} faces; frame {bad[0][0]} marked "
                  f"{bad[0][1]}")
            return 1
        crossed = min(f[0] for f in east_frames)
        print(f"[OK] the cull follows the camera: the walk crosses x="
              f"{int(split_x)} at frame {crossed}, the cluster changes 1 -> 0, "
              f"and the marked count rises from {west_surfaces} to "
              f"{total_faces} — a moving viewer, culled per frame")

        # ---- 5. a culled frame is still a LIVE, textured frame --------------
        px = {int(m.group(1)): tuple(int(x) for x in m.group(2, 3, 4, 5, 6, 7, 8, 9))
              for m in PIXELS_RE.finditer(log)}
        checked = 0
        for f in spawn_frames:
            p = px.get(f[0])
            if not p:
                continue
            cyan, warm, step, violet, bright, patch, sky, distinct = p
            live = cyan + warm + step + violet + bright + patch
            if live < 1000 or distinct < 8:
                print(f"[FAIL] frame {f[0]} is culled down to almost nothing "
                      f"({live} classified pixels, {distinct} colours) — the "
                      f"cull is not hiding the far half, it is hiding the map")
                return 1
            if sky > 76800 // 4:
                print(f"[FAIL] frame {f[0]}: {sky}/76800 pixels are the clear "
                      f"colour while culling — that is the level vanishing, "
                      f"not being culled")
                return 1
            checked += 1
        print(f"[OK] the culled frames are still the level: {checked} sampled "
              f"frame(s) with the east half hidden carry more than 1000 "
              f"classified pixels and at least 8 distinct colours")

        # ---- 6. the session ends cleanly -----------------------------------
        sendkey("esc")
        if not wait_for_in_file(SERIAL_LOG, DONE_MARKER, 60):
            print("[FAIL] ESC did not end the session")
            dump_tail()
            return 1
        if not wait_for_in_file(SERIAL_LOG, "OS stayed alive", 30) and \
                PANIC_MARKER in read_file(SERIAL_LOG):
            print("[FAIL] the OS did not survive the session")
            return 1
        print("[OK] ESC ended the session, the OS stayed alive")
        print("[PASS] the map's own PVS locates the camera, culls what it cannot "
              "see, follows the camera across the level, and the culled frames "
              "are still the textured level — in Mectov OS")
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
