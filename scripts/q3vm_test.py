#!/usr/bin/env python3
"""
scripts/q3vm_test.py — end-to-end test for the v38.105 Quake III Arena port
(Q3 phase 5): the OFFICIAL id Software game module, running as Quake VM
bytecode inside the Mectov kernel.

Why this is the "real thing" and not the earlier phases' engine-shaped demo:

  * third_party/q3a is id-Software/Quake-III-Arena taken verbatim (the 2005 GPL
    source release). The kernel's engine core, its QVM loader and its bytecode
    interpreter are compiled from that source, not from a fork.
  * qagame.qvm is built from that same source by id's OWN tools — the lcc
    compiler with the bytecode backend and the q3asm assembler, both vendored
    from the same repository (scripts/build_qvm.sh).
  * The module executes through id's interpreter (qcommon/vm.c +
    vm_interpreted.c) and calls back into the kernel through a trap surface
    modelled on the retail engine's own SV_GameSystemCalls().

What it proves, in order:

  1. serial: the q3vm task starts and stages the bytecode from /ext2 into the
     engine's homepath — the ENGINE's filesystem loads the module, the same
     path it would use for a real mod, and no pak0 is involved.
  2. the staged bytecode is byte-for-byte the file this checkout built (size
     compared against build/vm/qagame.qvm) — so the guest really is running
     official-source bytecode and not a stand-in.
  3. id's own loader reports on it: "Loading vm file vm/qagame.qvm." comes from
     the engine, and the symbol table parsed out of the vendored .map proves
     id's debug info survived the build.
  4. the engine names the module itself: `vminfo` (id's console command) prints
     "qagame : interpreted" — a registered, interpreted VM.
  5. GAME_INIT runs real game code: the module makes traps (cvar registration,
     `trap_LocateGameData`, filesystem probing) and returns through
     vmMain. Trap counts, G_ERROR count and `locate_game_data` payload are all
     asserted, so "it ran 0 instructions" cannot pass.
  6. the game loop really runs (v38.106): the driver replays the retail
     server's connect sequence (CLIENT_CONNECT / USERINFO_CHANGED / BEGIN —
     "Mectov entered the game" comes back through trap_SendServerCommand)
     and then 200 GAME_RUN_FRAME + GAME_CLIENT_THINK pairs with level time
     advancing FRAMETIME per frame. id's own Pmove accelerates the player
     across the floor plane: the sampled playerState origin moves forward by
     hundreds of units and the player lands on the floor (ground =
     ENTITYNUM_WORLD, z at the 24-unit standing offset) — official gameplay
     physics executing inside the bytecode.
  7. the kernel survives it: the task parks in hlt, QEMU stays up, and a shell
     command issued afterwards still produces its serial marker (the desktop
     was not wedged by running the game VM).

Usage:
    python3 scripts/q3vm_test.py [--timeout 480] [--iso mectov-q3.iso]

`make check-q3vm` builds the MECTOV_Q3=1 ISO as mectov.iso; CI passes
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

SERIAL_LOG = "/tmp/mectov_q3vm_serial.log"
MON_SOCK = "/tmp/mectov_q3vm_monitor.sock"
CURSOR_PPM = "/tmp/mectov_q3vm_cursor.ppm"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
Q3VM_KEYS = ["q", "3", "v", "m", "ret"]

START_MARKER = "[Q3VM] official Quake III Arena bytecode path"
VM_CREATED_MARKER = "[Q3VM] vm created name=qagame interpret=bytecode symbols="
LOADING_MARKER = "Loading vm file vm/qagame.qvm."
HUNK_MARKER = "qagame loaded in "
PANIC_MARKER = "[PANIC]"
SYS_ERROR_MARKER = "[Q3] Sys_Error"
MINFO_MARKER = "Registered virtual machines:"
CALL_MARKER = "[Q3VM] calling vmMain(GAME_INIT)"
RETURNED_MARKER = "[Q3VM] GAME_INIT returned "
ENTERED_MARKER = "entered the game"
CONNECT_MARKER = "[Q3VM] client 0: GAME_CLIENT_CONNECT"
BEGIN_MARKER = "[Q3VM] client 0: GAME_CLIENT_BEGIN"
FRAME_LOOP_MARKER = "[Q3VM] frame loop: 200 frames x 50 msec"
FRAME_DONE_MARKER = "[Q3VM] frame loop done"
MOVEMENT_MARKER = "[Q3VM] movement x0="
DONE_MARKER = "[Q3VM] done"
FAILED_MARKER = "[Q3VM] FAILED"

# per-frame playerState samples straight out of the VM's data segment
FRAME_RE = re.compile(
    r"\[Q3VM\] frame (\d+) t=(\d+) origin=\((-?\d+),(-?\d+),(-?\d+)\) "
    r"ground=(-?\d+) velocity=\((-?\d+),(-?\d+),(-?\d+)\)")
ENTITYNUM_WORLD = 1022        # MAX_GENTITIES-2, q_shared.h
STAND_Z = 33                  # spawn 24 + the +9 the game adds on top

# ---- the generated test arena (scripts/build_test_bsp.py, v38.107) ---------
# Its numbers are what let the assertions below be exact rather than vague: the
# arena is our own content, so the collision answers are known in advance — and
# two of them cannot come out of the hand-written world this release replaced
# (one floor plane at z=0, fraction 1.0 for every sideways move).
WORLD_REAL_MARKER = "[Q3VM] world: CM_LoadMap("
WORLD_FALLBACK_MARKER = "[Q3VM] world: no /ext2/baseq3/maps/mectovtest.bsp"
SPAWN_ORIGIN_MARKER = "[Q3VM] world: map spawn origin=["
BSP_NAME = "maps/mectovtest.bsp"
BSP_FLOOR_TOP = 64            # the floor's TOP face; the fake world had 0
BSP_WALL_X = -512             # the -X wall's inner face
BSP_SPAWN = (-384, -384, 120)  # the map's info_player_deathmatch "origin"
STAND_Z_FLOOR = BSP_FLOOR_TOP + 24   # player mins[2] = -24, so it rests 24 above
PLAYER_HALF_WIDTH = 15        # DEFAULT_MINS_2, so it stops ~15 short of a wall

WORLD_RE = re.compile(
    r"\[Q3VM\] world: CM_LoadMap\((\S+)\) shaders=(-?\d+) planes=(-?\d+) "
    r"brushes=(-?\d+) brushsides=(-?\d+) nodes=(-?\d+) leafs=(-?\d+) "
    r"models=(-?\d+)")
ENTITY_CHARS_RE = re.compile(
    r"\[Q3VM\] world: entity string chars=(-?\d+) checksum=(0x[0-9a-f]+)")
SPAWN_ORIGIN_RE = re.compile(r"\[Q3VM\] world: map spawn origin=\[([^\]]*)\]")
PROBE_DOWN_RE = re.compile(
    r"\[Q3VM\] world: trace down fraction=(-?[\d.]+) endz=(-?[\d.]+) "
    r"normal=\((-?[\d.]+) (-?[\d.]+) (-?[\d.]+)\) contents=(-?\d+)")
PROBE_X_RE = re.compile(
    r"\[Q3VM\] world: trace -x fraction=(-?[\d.]+) endx=(-?[\d.]+) "
    r"normal=\((-?[\d.]+) (-?[\d.]+) (-?[\d.]+)\) contents=(-?\d+)")

LOCATE_RE = re.compile(
    r"\[Q3VM\] locate_game_data entities=(-?\d+) sizeof_gentity=(-?\d+) "
    r"vm_ofs=(0x[0-9a-f]+)")
STAGED_QVM_RE = re.compile(
    r"\[Q3VM\] qvm at /ext2/baseq3/vm/qagame\.qvm bytes=(-?\d+)")
STAGED_MAP_RE = re.compile(
    r"\[Q3VM\] map at /ext2/baseq3/vm/qagame\.map bytes=(-?\d+)")
HUNK_RE = re.compile(r"qagame loaded in (\d+) bytes on the hunk")
VM_CREATED_RE = re.compile(
    r"\[Q3VM\] vm created name=(\S+) interpret=(\S+) symbols=(-?\d+) "
    r"codeLength=(-?\d+) dataMask=(-?\d+)")
RETURNED_RE = re.compile(
    r"\[Q3VM\] GAME_INIT returned (-?\d+) traps=(-?\d+) total=(-?\d+) "
    r"errors=(-?\d+) unhandled=(-?\d+)")

# The bytecode this checkout built; the guest must be running exactly it.
LOCAL_QVM = "build/vm/qagame.qvm"


def read_file(path):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read()
    except (FileNotFoundError, OSError):
        return ""


def wait_for_in_file(path, needle, timeout):
    """Wait for a serial marker, but give up at once if the kernel panicked —
    a dead guest is never going to print the marker, and waiting out the full
    timeout turns one crash into eight minutes."""
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=480)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK, CURSOR_PPM):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    local_qvm_size = None
    if os.path.exists(LOCAL_QVM):
        local_qvm_size = os.path.getsize(LOCAL_QVM)

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

        # The QVM is ~460 KB; staging it plus Com_Init and a full GAME_INIT
        # under TCG takes a while, so wait on markers rather than on a sleep.
        if not type_line(Q3VM_KEYS, retries=2, ready_marker=START_MARKER,
                         timeout=120):
            print("[FAIL] the `q3vm` command never started its task")
            dump_tail()
            return 1
        print("[OK] q3vm task started (official id source engine core)")

        if not wait_for_in_file(SERIAL_LOG, DONE_MARKER, args.timeout):
            print("[FAIL] the QVM session never completed")
            log = read_file(SERIAL_LOG)
            for line in log.splitlines():
                if FAILED_MARKER in line or SYS_ERROR_MARKER in line:
                    print("       " + line[:140])
            dump_tail(40)
            return 1

        log = read_file(SERIAL_LOG)
        if FAILED_MARKER in log:
            print("[FAIL] the port reported a failure:")
            for line in log.splitlines():
                if FAILED_MARKER in line:
                    print(line[:140])
            return 1

        # ---- 1. the bytecode lives on the game volume -----------------------
        m = STAGED_QVM_RE.search(log)
        if not m:
            print("[FAIL] no qvm marker (the game volume has no vm/qagame.qvm)")
            dump_tail()
            return 1
        staged = int(m.group(1))
        if staged < 100000:
            print(f"[FAIL] the bytecode looks too small ({staged} bytes)")
            return 1
        if local_qvm_size is not None and staged != local_qvm_size:
            print(f"[FAIL] the guest sees {staged} bytes but this checkout built "
                  f"{local_qvm_size} — not the same bytecode")
            return 1
        print(f"[OK] qagame.qvm sits on the ext2 game volume "
              f"(/ext2/baseq3/vm/qagame.qvm, {staged} bytes) — exactly the "
              f"bytecode this checkout built")

        mm = STAGED_MAP_RE.search(log)
        if not mm or int(mm.group(1)) < 1000:
            print("[FAIL] the symbol map is not on the volume (no id debug info)")
            return 1
        print(f"[OK] id's symbol map is there too ({mm.group(1)} bytes)")

        # ---- 2. id's loader accepts it -------------------------------------
        if LOADING_MARKER not in log:
            print(f"[FAIL] id's VM loader never ran ('{LOADING_MARKER}')")
            dump_tail(40)
            return 1
        if HUNK_MARKER not in log:
            print("[FAIL] id's VM loader never reported the hunk footprint")
            dump_tail(40)
            return 1
        hunk = HUNK_RE.search(log)
        print(f"[OK] id's VM loader read the module through the engine FS "
              f"({hunk.group(1) if hunk else '?'} bytes on the hunk)")

        mc = VM_CREATED_RE.search(log)
        if not mc:
            print(f"[FAIL] no '{VM_CREATED_MARKER}' marker")
            dump_tail(40)
            return 1
        name, interpret, symbols, code_len, data_mask = (
            mc.group(1), mc.group(2), int(mc.group(3)), int(mc.group(4)),
            int(mc.group(5)))
        if name != "qagame" or interpret != "bytecode":
            print(f"[FAIL] wrong module: name={name} interpret={interpret}")
            return 1
        if code_len < 100000 or data_mask < 100000:
            print(f"[FAIL] implausible VM segments: code={code_len} "
                  f"dataMask={data_mask}")
            return 1
        print(f"[OK] one QVM registered: qagame, interpreted "
              f"(code={code_len} bytes, data={data_mask + 1} bytes)")

        # ---- 3. debug symbols came from id's .map --------------------------
        if symbols < 100:
            print(f"[FAIL] only {symbols} symbols loaded from qagame.map")
            return 1
        syms = re.findall(r"\[Q3VM\] symbol (0x[0-9a-f]+) (\S+)", log)
        if len(syms) < 3:
            print("[FAIL] no symbol names printed (map parse failed)")
            return 1
        print(f"[OK] {symbols} symbols parsed from id's .map; first: "
              + ", ".join(s[1] for s in syms[:3]))

        # ---- 4. the engine itself names the module --------------------------
        if MINFO_MARKER not in log:
            print("[FAIL] the engine's own `vminfo` output is missing")
            dump_tail(40)
            return 1
        vminfo = log[log.index(MINFO_MARKER):log.index(MINFO_MARKER) + 400]
        if "qagame : interpreted" not in vminfo.replace("  ", " "):
            print("[FAIL] `vminfo` does not list qagame as interpreted")
            print(vminfo[:200])
            return 1
        print("[OK] the engine reports the module through `vminfo`: "
              "qagame : interpreted")

        # ---- 5. GAME_INIT really executed ----------------------------------
        if CALL_MARKER not in log:
            print("[FAIL] GAME_INIT was never called (module not run)")
            dump_tail(40)
            return 1
        mr = RETURNED_RE.search(log)
        if not mr:
            print("[FAIL] GAME_INIT never returned")
            dump_tail(40)
            return 1
        ret, traps, total, errors, unhandled = (int(mr.group(i))
                                               for i in range(1, 6))
        if traps < 50:
            print(f"[FAIL] the module made only {traps} traps — it did not "
                  "really initialise")
            return 1
        if errors > 0:
            print(f"[FAIL] the game module raised {errors} G_ERROR(s):")
            for line in log.splitlines():
                if "[Q3VM] G_ERROR" in line:
                    print(line[:140])
            return 1
        print(f"[OK] vmMain(GAME_INIT) ran official id game code and returned "
              f"{ret} ({traps} traps, {unhandled} unhandled)")

        ml = LOCATE_RE.search(log)
        if not ml:
            print("[FAIL] the module never called trap_LocateGameData")
            dump_tail(40)
            return 1
        ents, sizeof_g, ofs = int(ml.group(1)), int(ml.group(2)), ml.group(3)
        if ents <= 0 or sizeof_g <= 0:
            print(f"[FAIL] implausible game data: entities={ents} "
                  f"sizeof_gentity={sizeof_g}")
            return 1
        print(f"[OK] the module registered its world: {ents} gentities of "
              f"{sizeof_g} bytes at {ofs} in the VM's data segment")

        # ---- 5b. the module's own spawn path ran clean -----------------------
        if "Game Initialization" not in log:
            print("[FAIL] the module never printed its own Game Initialization banner")
            return 1
        if "unhandled trap" in log:
            print("[FAIL] the module made traps the kernel does not serve:")
            for line in log.splitlines():
                if "unhandled trap" in line:
                    print(line[:140])
            return 1
        if "SpawnEntities: no entities" in log:
            print("[FAIL] the worldspawn entity string never reached the module")
            return 1
        if "items registered" not in log:
            print("[FAIL] the module did not reach its item registration stage")
            return 1
        print("[OK] official G_InitGame completed: worldspawn spawned, "
              "items registered, no unhandled traps")

        # ---- 5c. the retail connect sequence ran ----------------------------
        if CONNECT_MARKER not in log or BEGIN_MARKER not in log:
            print("[FAIL] the server connect sequence never ran")
            dump_tail(40)
            return 1
        if ENTERED_MARKER not in log:
            print("[FAIL] the module never announced the client (no 'entered "
                  "the game' through trap_SendServerCommand)")
            dump_tail(40)
            return 1
        print("[OK] retail connect sequence: ClientConnect -> userinfo -> "
              "ClientBegin, 'Mectov entered the game' and all")

        # ---- 5d. the collision world is id's own .bsp (v38.107) -------------
        # Until this release G_TRACE answered from a hand-written floor plane at
        # z=0, so "the official gameplay code runs" was true but "in a level"
        # was not. Everything asserted here is new information: the loader's own
        # account of what it built, the level's entity text, and two traces
        # whose answers the old world could not have produced.
        if WORLD_FALLBACK_MARKER in log:
            print("[FAIL] the map never loaded — the port fell back to its "
                  "minimal world (was the test arena seeded onto /ext2?)")
            return 1
        if WORLD_REAL_MARKER not in log:
            print("[FAIL] id's own collision loader (CM_LoadMap) never ran")
            dump_tail(40)
            return 1
        wm = WORLD_RE.search(log)
        if not wm:
            print("[FAIL] CM_LoadMap did not report what it built")
            dump_tail(40)
            return 1
        bsp_name = wm.group(1)
        shaders, planes, brushes, brushsides, nodes, leafs, models = \
            (int(wm.group(i)) for i in range(2, 9))
        if bsp_name != BSP_NAME:
            print(f"[FAIL] the wrong map was loaded: {bsp_name}")
            return 1
        if min(shaders, planes, brushes, brushsides, nodes, leafs, models) <= 0:
            print(f"[FAIL] CM_LoadMap built an empty world: shaders={shaders} "
                  f"planes={planes} brushes={brushes} brushsides={brushsides} "
                  f"nodes={nodes} leafs={leafs} models={models}")
            return 1
        print(f"[OK] id's own collision loader built the world from "
              f"{bsp_name}: {shaders} shaders, {planes} planes, {brushes} "
              f"brushes, {brushsides} brushsides, {nodes} node, {leafs} leaf, "
              f"{models} model")

        mchar = ENTITY_CHARS_RE.search(log)
        if not mchar or int(mchar.group(1)) < 100:
            print("[FAIL] the level's entity text is missing or empty")
            return 1
        print(f"[OK] the level's own entity text reached the module "
              f"({mchar.group(1)} chars, checksum {mchar.group(2)})")

        if SPAWN_ORIGIN_MARKER not in log:
            print("[FAIL] no info_player_deathmatch in the level's entity text")
            dump_tail(40)
            return 1
        so = SPAWN_ORIGIN_RE.search(log)
        spawn = tuple(int(v) for v in so.group(1).split()) if so else None
        if spawn != BSP_SPAWN:
            print(f"[FAIL] the module's spawn is {spawn}, not the map's "
                  f"{BSP_SPAWN}")
            return 1
        print(f"[OK] the module's spawn point came out of the map's own entity "
              f"string: origin={spawn}")

        # A downward trace must stop on the arena's floor slab, whose top face
        # is z = 64. The old world's single plane sat at z = 0, so its answer
        # was 0 — this number is the map's geometry, not a convention.
        pd = PROBE_DOWN_RE.search(log)
        if not pd:
            print("[FAIL] the driver's downward probe produced no trace")
            dump_tail(40)
            return 1
        pfrac, endz, nx, ny, nz, contents = (float(pd.group(1)),
                                            float(pd.group(2)),
                                            float(pd.group(3)),
                                            float(pd.group(4)),
                                            float(pd.group(5)),
                                            int(pd.group(6)))
        if abs(nz - 1.0) > 0.01 or abs(nx) > 0.01 or abs(ny) > 0.01:
            print(f"[FAIL] the surface under the player is not horizontal: "
                  f"normal=({nx} {ny} {nz})")
            return 1
        if abs(endz - BSP_FLOOR_TOP) > 1.0:
            print(f"[FAIL] the floor is at z={endz}, not the generated arena's "
                  f"{BSP_FLOOR_TOP} — that is not this map's geometry")
            return 1
        if contents != 1:            # CONTENTS_SOLID
            print(f"[FAIL] the floor is not CONTENTS_SOLID (got {contents})")
            return 1
        print(f"[OK] a downward trace from the player stops on the map's floor "
              f"slab: fraction={pfrac:.3f} endz={endz:.3f} normal=(0 0 1) "
              f"contents=SOLID")

        # The sideways trace is the one the old world could never get right: it
        # had no walls, so every horizontal move came back fraction 1.0 (open
        # air all the way). Stopping on the -X face at x = -512 with a +X plane
        # normal means real brushes are being swept.
        px = PROBE_X_RE.search(log)
        if not px:
            print("[FAIL] the driver's sideways probe produced no trace")
            dump_tail(40)
            return 1
        xfrac, endx, wnx = float(px.group(1)), float(px.group(2)), float(px.group(3))
        if not (0.20 <= xfrac <= 0.30):
            print(f"[FAIL] the wall probe hit at fraction={xfrac}, expected "
                  f"~0.25 (spawn x=-384, wall at {BSP_WALL_X}, 512-unit trace)")
            return 1
        if abs(endx - BSP_WALL_X) > 1.0:
            print(f"[FAIL] the wall probe stopped at x={endx}, not the wall "
                  f"face {BSP_WALL_X}")
            return 1
        if wnx < 0.99:
            print(f"[FAIL] the wall's plane normal points the wrong way: "
                  f"({wnx})")
            return 1
        print(f"[OK] a sideways trace stops on the map's wall — the hand-written "
              f"world had no walls and returned fraction 1.0 for this: "
              f"fraction={xfrac:.3f} endx={endx:.3f} normal=(1 0 0)")

        # ---- 5e. the game loop moved the player -----------------------------
        if FRAME_LOOP_MARKER not in log:
            print("[FAIL] the game loop never started")
            dump_tail(40)
            return 1
        if FRAME_DONE_MARKER not in log:
            print("[FAIL] the game loop never finished (module died mid-run?)")
            for line in log.splitlines():
                if "G_ERROR" in line or "unhandled trap" in line:
                    print("       " + line[:140])
            dump_tail(40)
            return 1
        frames = FRAME_RE.findall(log)
        if len(frames) < 2:
            print(f"[FAIL] only {len(frames)} playerState samples in the log")
            return 1
        first = frames[0]
        last = frames[-1]

        # The spawn has to be the MAP's, not a number the port chose: the old
        # hand-written world handed the module "0 0 24" and the player started
        # at the origin. Frame 0 is the spawn instant, before the loop ticks.
        if (int(first[2]), int(first[3])) != (BSP_SPAWN[0], BSP_SPAWN[1]):
            print(f"[FAIL] the player spawned at ({first[2]},{first[3]}), not at "
                  f"the map's spawn point ({BSP_SPAWN[0]},{BSP_SPAWN[1]})")
            return 1

        dx = int(last[2]) - int(first[2])
        if dx <= 500:
            print(f"[FAIL] the player barely moved: x {first[2]} -> {last[2]} "
                  f"(delta {dx}) — Pmove is not really running")
            return 1
        ground = int(last[5])
        z = int(last[4])
        if ground != ENTITYNUM_WORLD:
            print(f"[FAIL] the player never landed on the world "
                  f"(ground={ground}) — trace.entityNum is the caller's to set "
                  f"(ENTITYNUM_WORLD for the world model, ENTITYNUM_NONE for a "
                  f"miss)")
            return 1
        # It stands on the arena's floor slab, so its eye height is the floor's
        # top face plus the 24-unit box stand-off — 88, not the 24 the z=0 plane
        # used to produce.
        if not (STAND_Z_FLOOR - 4 <= z <= STAND_Z_FLOOR + 4):
            print(f"[FAIL] standing height z={z}; the map's floor top is "
                  f"{BSP_FLOOR_TOP} so id's collision should hold the player "
                  f"near {STAND_Z_FLOOR} (floor + the 24-unit player box)")
            return 1
        # The arena is walled, so no amount of walking can leave it. Without
        # walls — which is exactly what the old world had — the player slid off
        # the map forever and still "landed".
        last_x, last_y = int(last[2]), int(last[3])
        bound = 512 - PLAYER_HALF_WIDTH + 2
        if abs(last_x) > bound or abs(last_y) > bound:
            print(f"[FAIL] the player left the arena: ({last_x},{last_y}) is past "
                  f"the walls at +/-512 — the map's brushes did not block it")
            return 1
        print(f"[OK] official gameplay code moved the player: frame "
              f"{first[0]} ({first[2]},{first[3]},{first[4]}) -> frame "
              f"{last[0]} ({last_x},{last_y},{z}) — {dx} units of x in "
              f"{int(last[1]) - int(first[1])} msec of game time, spawned at the "
              f"map's own point, stopped by the map's walls, standing on the "
              f"map's floor (ground=WORLD)")
        mm2 = re.search(r"\[Q3VM\] movement x0=(-?\d+) x1=(-?\d+) delta=(-?\d+)", log)
        if mm2 and int(mm2.group(3)) <= 0:
            print("[FAIL] the driver's own accounting says the player did not "
                  "advance")
            return 1

        # ---- 7. the kernel survived it --------------------------------------
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited with code {qemu.returncode}")
            return 1
        print("[OK] the OS stayed alive after running the game VM")

        print("[PASS] Quake III Arena, official id source, running in Mectov OS")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
