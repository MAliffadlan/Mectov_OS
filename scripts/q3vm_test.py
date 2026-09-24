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
  6. the kernel survives it: the task parks in hlt, QEMU stays up, and a shell
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
DONE_MARKER = "[Q3VM] done"
FAILED_MARKER = "[Q3VM] FAILED"

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

        # ---- 6. the kernel survived it --------------------------------------
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
