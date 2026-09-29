#!/usr/bin/env python3
"""
scripts/q3jump_test.py — jump + fire through id's OWN player state (v38.122).

Why this suite exists
---------------------
v38.122 wires the two usercmd channels the driver never filled: `upmove` and
`buttons`. Everything downstream is already id's own code — PM_CheckJump gives
velocity[2] = JUMP_VELOCITY (270) while grounded and gates the next jump on
PMF_JUMP_HELD until the intent is released; PM_Weapon takes one
ammo[WP_MACHINEGUN] per shot and raises EV_FIRE_WEAPON, which the server turns
into FireWeapon/Bullet_Fire. The port does none of that itself: it only builds
the usercmd. So the evidence that the chain works is id's OWN playerState,
read straight out of the shared data segment the module told the driver about
at trap_LocateGameData.

The suite drives the game with scheduled actions instead of injected keys
(`q3arena mectovtest jump=4 fire=3`): the knobs add jump/fire intent on a fixed
frame grid, which makes the assertions host-speed independent — no "did the key
land in time" timing games.

What is asserted, and why each one is evidence:

  1. the knobs reached the driver: the args line reports `jump=4 fire=3`. A
     scheduler that silently ignored its argument would leave the rest of the
     run asserting nothing at all.
  2. A JUMP REALLY HAPPENED, in id's own physics: after the first scheduled
     jump intent there is a `play` line with vel_z > 100 (spawn ground state is
     vel_z = 0; JUMP_VELOCITY is 270) and ground=1023 (ENTITYNUM_NONE — the
     Pmove jumped away from the floor). Only PM_CheckJump can produce that
     pair.
  3. THE PLAYER LANDED: a later `play` line reads ground back OFF
     ENTITYNUM_NONE (the fixture floor is a world brush: 1022) with vel_z <= 0.
  4. THE GUN REALLY FIRED: ammo_mg ends below the 100 the FFA spawn gives
     (g_client.c ClientSpawn). Only PM_Weapon's fire path decrements it.
  5. The run is CLEAN: no panic, no Sys_Error, module shuts down on ESC and
     the kernel stays alive.

Usage:
    python3 scripts/q3jump_test.py [--timeout 600] [--iso mectov.iso] [--kvm]
    [--no-seed]

`make check-q3jump` builds the MECTOV_Q3=1 ISO as mectov.iso and runs this.
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

SERIAL_LOG = "/tmp/mectov_q3jump_serial.log"
MON_SOCK = "/tmp/mectov_q3jump_monitor.sock"
CURSOR_PPM = "/tmp/mectov_q3jump_cursor.ppm"
QEMU_ERR_LOG = "/tmp/mectov_q3jump_qemu_err.log"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
# `q3arena mectovtest jump=4 fire=3` — the fixture arena, 4 scheduled jumps
# (one every 40 frames) and 3 fire bursts (20 frames of BUTTON_ATTACK each,
# machinegun cadence inside the burst = up to 10 shots per burst).
Q3ARENA_KEYS = (list("q3arena") + ["spc"] + list("mectovtest") + ["spc"] +
                list("jump") + ["equal"] + list("4") + ["spc"] +
                list("fire") + ["equal"] + list("3") + ["ret"])

START_MARKER = "official qagame VM world rendered through TinyGL"
WINDOW_MARKER = "[Q3ARENA] window id="
PANIC_MARKER = "[PANIC]"
SYS_ERROR_MARKER = "Sys_Error"
DONE_MARKER = "[Q3VM] done"

# ENTITYNUM_NONE is 1023 in bg_public.h; standing ground reads as the world
# entity (1022) or a mover (0). The fixture arena's floor is a world brush, so
# grounded reads 1022 there — "landed" means NOT airborne any more.
GROUND_NONE = 1023

ARGS_RE = re.compile(
    r"\[Q3ARENA\] args: map='(\S+)' fullscreen=(\d+) pose=(\d+) sort=(\d+) "
    r"jump=(\d+) fire=(\d+)")
# v38.124: the viewmodel line reports what was DRAWN, not just that the driver
# fed it state — model/parts is q3viewmodel.c's own count (0 = the volume has
# no .md3 and the v38.123 silhouette ran instead), surf/tris the geometry it
# parsed, drawn the triangles the LAST PASS submitted (equal to tris when the
# whole model reached the rasterizer, MINUS the flash's share on a frame the
# weapon was not firing — id draws the flash only while firing, and the loader
# line carries its triangle count as flash_tris), tex the body image, box the
# screen rectangle the pass projected, and distinct the 4-bit colours inside
# that box (the number a flat hand-drawn slab cannot produce). `flash` is the
# LIVE firing state that decided which of those two totals `drawn` is, and
# `latched` the sticky "a tick ran the fire path" flag — one number per
# question, since a 100-frame sample grid cannot catch a 20-frame burst. Named
# groups: this line grew three times in three releases and index arithmetic
# grew bugs with it.
VM_RE = re.compile(
    r"\[Q3ARENA\] viewmodel frame=(?P<frame>\d+) weapon=(?P<weapon>\d+) "
    r"flash=(?P<flash>\d+) latched=(?P<latched>\d+) ground=(?P<ground>\d+) "
    r"bob=(?P<bob>\d+) "
    r"model=(?P<model>\d+) surf=(?P<surf>\d+) tris=(?P<tris>\d+) "
    r"drawn=(?P<drawn>\d+) tex=(?P<tw>\d+)x(?P<th>\d+) "
    r"box=(?P<x0>-?\d+),(?P<y0>-?\d+),(?P<x1>-?\d+),(?P<y1>-?\d+) "
    r"distinct=(?P<distinct>\d+)")
# The loader's own one-line verdict, printed once per session — the SAME field
# shape whether it loaded id's model or fell back, so `parts` decides which
# branch a suite takes instead of whether the line parses at all.
VMLOAD_RE = re.compile(
    r"\[Q3VIEWMODEL\] load base=(?P<base>\S+) parts=(?P<parts>\d+) "
    r"surf=(?P<surf>\d+) tris=(?P<tris>\d+) tex=(?P<tw>\d+)x(?P<th>\d+) "
    r"muzzle=(?P<mx>-?\d+),(?P<my>-?\d+),(?P<mz>-?\d+) "
    r"chain=(?P<chain>\d+) barrel=(?P<barrel>-?\d+) flash=(?P<flash>-?\d+) "
    r"flash_tris=(?P<ftris>\d+)")
PLAY_RE = re.compile(
    r"\[Q3ARENA\] play frame=(\d+) up=(-?\d+) btn=(\d+) weapon=(\d+) "
    r"ammo_mg=(-?\d+) health=(-?\d+) ground=(\d+) jumps=(\d+) shots=(\d+) "
    r"vel_z=(-?\d+)")
JUMPHIT_RE = re.compile(
    r"\[Q3ARENA\] jump hit frame=(\d+) vel_z=(-?\d+) ground=(\d+) jumps=(\d+)")



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


def parse_play(text=None):
    text = read_file(SERIAL_LOG) if text is None else text
    out = []
    for m in PLAY_RE.finditer(text):
        out.append(dict(zip(
            ("frame", "up", "btn", "weapon", "ammo", "health", "ground",
             "jumps", "shots", "vel_z"), (int(g) for g in m.groups()))))
    return out


def mon_cmd(cmd, settle=0.15):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(settle)
        s.close()
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")


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
                         "rebuilding them (development only)")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK, CURSOR_PPM):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    # The fixture arena and its textures, rebuilt rather than inherited: this
    # suite never needs a retail map, so it must not depend on one being
    # staged (see scripts/q3_images.py).
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
    print(f"[q3jump] accel: {'KVM' if args.kvm else 'TCG'}")
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
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] the Terminal never created its IPC queue")
            dump_tail()
            return 1
        time.sleep(1.0)
        mon_cmd("mouse_move 300 176", settle=0.1)
        mon_cmd("mouse_button 1", settle=0.1)
        mon_cmd("mouse_button 0", settle=0.5)

        print("[q3jump] launching q3arena mectovtest jump=4 fire=3")
        launched = False
        for attempt in range(3):
            mon_cmd("mouse_move 300 176", settle=0.1)
            mon_cmd("mouse_button 1", settle=0.1)
            mon_cmd("mouse_button 0", settle=3.0)
            for _ in range(24):
                mon_cmd("sendkey backspace", settle=0.05)
            for k in Q3ARENA_KEYS:
                mon_cmd("sendkey " + k, settle=0.12)
            if wait_for_in_file(SERIAL_LOG, WINDOW_MARKER, 60):
                launched = True
                break
            print(f"[q3jump] retry launch ({attempt + 1})")
        if not launched:
            print("[FAIL] no game window")
            dump_tail()
            return 1
        if not wait_for_in_file(SERIAL_LOG, START_MARKER, args.timeout):
            print("[FAIL] the game module never announced its world")
            dump_tail()
            return 1

        # 1. the knobs reached the driver.
        m = ARGS_RE.search(read_file(SERIAL_LOG))
        if not m:
            print("[FAIL] no [Q3ARENA] args line with the v38.122 fields")
            dump_tail()
            return 1
        got = (m.group(1), int(m.group(5)), int(m.group(6)))
        if got != ("mectovtest", 4, 3):
            print(f"[FAIL] args: map='{got[0]}' jump={got[1]} fire={got[2]}, "
                  f"wanted map='mectovtest' jump=4 fire=3")
            return 1
        print(f"[OK] the knobs reached the driver: jump={got[1]} fire={got[2]}")

        # 2. a jump REALLY happened, in id's own physics: the driver prints a
        #    one-shot `jump hit` line at the instant a game tick consumed the
        #    intent — at that moment velocity[2] is exactly JUMP_VELOCITY and
        #    ground is ENTITYNUM_NONE. Only PM_CheckJump can produce that
        #    pair, and unlike the sampled play lines it cannot be missed by
        #    unlucky sampling.
        deadline = time.time() + args.timeout
        airborne = None
        landed = None
        while time.time() < deadline and (airborne is None or landed is None):
            for m in JUMPHIT_RE.finditer(read_file(SERIAL_LOG)):
                if airborne is None:
                    airborne = dict(
                        zip(("frame", "vel_z", "ground", "jumps"),
                            (int(g) for g in m.groups())))
            for p in parse_play():
                if airborne is not None and landed is None and \
                        p["jumps"] >= 1 and p["vel_z"] <= 0 and \
                        p["ground"] != GROUND_NONE:
                    landed = p
            time.sleep(1)
            if PANIC_MARKER in read_file(SERIAL_LOG) or \
                    SYS_ERROR_MARKER in read_file(SERIAL_LOG):
                break
        if airborne is None:
            print("[FAIL] no `jump hit` line with vel_z=270 and "
                  "ground=1023 (ENTITYNUM_NONE) — the intent never reached "
                  "PM_CheckJump, or the scheduled intent never became a "
                  "usercmd. play lines:")
            for p in parse_play()[-6:]:
                print("    " + str(p))
            dump_tail(20)
            return 1
        if airborne["vel_z"] <= 100 or airborne["ground"] != GROUND_NONE:
            print(f"[FAIL] jump hit reads wrong: vel_z={airborne['vel_z']} "
                  f"ground={airborne['ground']} (want vel_z>100, "
                  f"ground={GROUND_NONE})")
            return 1
        print(f"[OK] a jump happened in id's own Pmove: frame={airborne['frame']} "
              f"vel_z={airborne['vel_z']} ground={airborne['ground']} "
              f"(JUMP_VELOCITY=270)")
        if landed is None:
            # Not fatal by itself — the fixture floor is close, TCG sampling
            # every 100 frames may land after the landing — but there must be
            # SOME later line with ground back off ENTITYNUM_NONE and jumps
            # counted.
            later = [p for p in parse_play()
                     if p["frame"] > airborne["frame"] and
                     p["ground"] != GROUND_NONE]
            if not later:
                print("[FAIL] the player never returned to the ground "
                      "(no later play line with ground != ENTITYNUM_NONE)")
                return 1
            landed = later[0]
        print(f"[OK] the player landed: frame={landed['frame']} "
              f"vel_z={landed['vel_z']} ground={landed['ground']}")

        # 3. THE GUN REALLY FIRED: ammo below the spawn's 100.
        deadline = time.time() + args.timeout
        spent = None
        while time.time() < deadline:
            rows = parse_play()
            fired = [p for p in rows if p["shots"] >= 1 and p["ammo"] < 100]
            if fired:
                spent = fired[0]
                break
            time.sleep(1)
            if PANIC_MARKER in read_file(SERIAL_LOG) or \
                    SYS_ERROR_MARKER in read_file(SERIAL_LOG):
                break
        if spent is None:
            print("[FAIL] ammo_mg never dropped below the spawn 100 with "
                  "shots>=1 — BUTTON_ATTACK never reached PM_Weapon, or the "
                  "fire chain broke. play lines:")
            for p in parse_play()[-6:]:
                print("    " + str(p))
            dump_tail(20)
            return 1
        print(f"[OK] the machinegun fired through id's own chain: "
              f"frame={spent['frame']} shots={spent['shots']} "
              f"ammo_mg={spent['ammo']} (spawn 100, PM_Weapon -1 per shot)")

        # 3b. THE GUN IS ON SCREEN, and it is id's own state driving it: every
        #     sampled frame reads weapon=2 (machinegun) and reports a screen box
        #     with room in it — the gun is really painted, not a stub. `latched`
        #     is the driver's STICKY latch of "a tick ran id's own fire path":
        #     the every-100th-frame sample grid cannot catch a 20-frame
        #     WEAPON_FIRING window, so the driver latches weaponstate==3 (and
        #     the TORSO_ATTACK anim id plays alongside it) the first time it
        #     sees them. ammo_mg<100 in step 3 already proved shots happen; the
        #     latch proves the STATE the flash draws from was hit. The bob phase
        #     must MOVE while the scheduled walk runs: a frozen bobCycle would
        #     mean the gun ignores the playerState. `flash` is the live state
        #     of the sampled frame and is what `drawn` has to agree with.
        #
        #     v38.124: WHICH gun is a second question, and the suite answers it
        #     from the loader's own verdict rather than from the pixels. A
        #     volume that carries the .md3 (this port's `--with-viewmodel`
        #     staging, and q3viewmodel_test's synthetic parts) must show
        #     model>=1 with the geometry the loader reported and a textured,
        #     many-coloured box in the lower right; a volume with no pak0 (CI's
        #     fixture volume) must show model=0 and be honest about it, which is
        #     exactly what the fallback exists for. Neither case may pass while
        #     claiming the other.
        deadline = time.time() + args.timeout
        vm_ok = None
        vm_fail = ""
        while time.time() < deadline and vm_ok is None:
            text = read_file(SERIAL_LOG)
            rows = [m.groupdict() for m in VM_RE.finditer(text)]
            loads = [m.groupdict() for m in VMLOAD_RE.finditer(text)]
            if rows and loads:
                ld = loads[0]
                parts = int(ld["parts"])
                # Every sample must be the machinegun, with the walk bob having
                # actually moved, and a box with room in it in the lower-right
                # half: the gun's own reported rectangle, whichever gun it is.
                bad = [r for r in rows if r["weapon"] != "2"]
                flashes = [r for r in rows if r["latched"] == "1"]
                bobs = {r["bob"] for r in rows}
                boxes = [r for r in rows
                         if int(r["x1"]) > int(r["x0"]) and
                         int(r["y1"]) > int(r["y0"]) and
                         int(r["x0"]) > 320 // 2]
                if parts >= 1:
                    # The flash is in `tris` but not in `drawn` on a frame the
                    # weapon is not firing; `flash_tris` (the loader's count
                    # for that part) makes the check exact either way.
                    ftris = int(ld["ftris"])
                    good = [r for r in boxes
                            if int(r["model"]) == parts and
                            int(r["tris"]) == int(ld["tris"]) and
                            int(r["drawn"]) == (int(ld["tris"]) if
                                                r["flash"] == "1" else
                                                int(ld["tris"]) - ftris) and
                            int(r["tw"]) == int(ld["tw"]) and
                            int(r["distinct"]) >= 8]
                    if not bad and flashes and len(bobs) >= 2 and good:
                        vm_ok = (flashes[0], len(rows), len(bobs), good[0])
                    else:
                        vm_fail = (f"the loader loaded {parts} part(s)/"
                                   f"{ld['surf']} surface(s)/{ld['tris']} "
                                   f"triangle(s) (flash {ftris}) but "
                                   f"{len(good)}/{len(rows)} rows report that "
                                   f"model drawn whole with a textured box in "
                                   f"the lower right; drawn values: "
                                   + str(sorted({(r['flash'], r['drawn'])
                                                 for r in boxes})))
                else:
                    if not bad and flashes and len(bobs) >= 2 and boxes:
                        vm_ok = (flashes[0], len(rows), len(bobs), boxes[0])
                    else:
                        vm_fail = (f"no .md3 on the volume (parts=0) and the "
                                   f"fallback's own rows do not hold up: "
                                   f"{len(boxes)}/{len(rows)} rows have a box, "
                                   f"weapon=2 everywhere: {not bad}")
            if vm_ok is None and (PANIC_MARKER in read_file(SERIAL_LOG) or
                                  SYS_ERROR_MARKER in read_file(SERIAL_LOG)):
                break
            time.sleep(1)
        if vm_ok is None:
            rows = [m.groupdict() for m in VM_RE.finditer(read_file(SERIAL_LOG))]
            loads = [m.groupdict() for m in
                     VMLOAD_RE.finditer(read_file(SERIAL_LOG))]
            print(f"[FAIL] the viewmodel lines never proved the gun: "
                  f"{len(rows)} samples, {len(loads)} loader line(s)"
                  + ("" if not rows else
                     f", weapon=2 everywhere: "
                     f"{all(r['weapon'] == '2' for r in rows)}, "
                     f"fire-path latch seen: "
                     f"{any(r['latched'] == '1' for r in rows)}, "
                     f"distinct bobs: {len({r['bob'] for r in rows})}"))
            if vm_fail:
                print(f"       {vm_fail}")
            dump_tail(20)
            return 1
        flash_row, nsamples, nbobs, box = vm_ok
        if int(box["model"]) >= 1:
            print(f"[OK] id's own view model is on screen: {nsamples} samples "
                  f"all weapon=2, loader reports model={box['model']} "
                  f"surf={box['surf']} tris={box['tris']} drawn={box['drawn']} "
                  f"(flash={box['flash']}, {box['tris']} minus the flash part "
                  f"when it was not firing) "
                  f"tex={box['tw']}x{box['th']}, screen box "
                  f"({box['x0']},{box['y0']})-({box['x1']},{box['y1']}) with "
                  f"{box['distinct']} distinct colours, fire-path latch set at "
                  f"frame={flash_row['frame']} (WEAPON_FIRING/TORSO_ATTACK), "
                  f"bob moved through {nbobs} phases")
        else:
            print(f"[OK] the view model path is honest with no .md3 on the "
                  f"volume: {nsamples} samples all weapon=2, model=0 (the "
                  f"documented fallback — see the [Q3VIEWMODEL] line for why), "
                  f"screen box ({box['x0']},{box['y0']})-({box['x1']},{box['y1']}) "
                  f"with {box['distinct']} distinct colours, fire-path latch set "
                  f"at frame={flash_row['frame']} "
                  f"(WEAPON_FIRING/TORSO_ATTACK), bob moved through {nbobs} "
                  f"phases")

        # 4. health is sane (the player did not gib itself or starve): 1..100
        bad = [p for p in parse_play() if p["health"] <= 0 or p["health"] > 125]
        if bad:
            print(f"[FAIL] health went insane on the play line: {bad[0]}")
            return 1
        print("[OK] health stayed sane through the session")

        # 5. clean exit on ESC, kernel alive.
        mon_cmd("sendkey esc", settle=0.2)
        if not wait_for_in_file(SERIAL_LOG, DONE_MARKER, 90):
            print("[FAIL] ESC did not shut the module down")
            dump_tail()
            return 1
        if PANIC_MARKER in read_file(SERIAL_LOG):
            print("[FAIL] panic after quit")
            return 1
        print("[OK] the module shut down cleanly; the kernel is still alive")

        print("[PASS] q3jump")
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
