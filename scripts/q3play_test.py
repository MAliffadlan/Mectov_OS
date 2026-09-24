#!/usr/bin/env python3
"""
scripts/q3play_test.py — end-to-end test for the v38.103 Quake III CLIENT port
(Q3 phase 3): the engine's CL_Init/CL_Frame driving the TinyGL renderer, with
keyboard and captured-mouse input routed in through the window manager.

What it proves, in order:

  1. serial: [Q3CL] key commands registered / client init / world ready — the
     client layer runs inside Com_Init (engine cvar + bind + command systems
     are up) and opened its WM window with a live TinyGL context.
  2. serial: [Q3CL] frame=… pos=… yaw=… — CL_Frame is being called by the
     engine's own frame loop (Com_Frame -> CL_Frame), so the renderer is
     connected to the engine and not merely rasterizing on its own.
  3. screendump pixels: the window shows a first-person arena. Four colour axes
     are asserted together — sky blue, warm wall/floor greys, orange and green
     bots — plus the bright crosshair at the window centre. A composited flat
     desktop, a black window or a failed render cannot satisfy that.
  4. INPUT, keyboard: `sendkey w 600` (held) must produce
     "[Q3CL] key down w bind=+forward" and move the camera: pos=… changes
     between frame markers. That is the whole chain WM raw scancode -> client
     ring -> Com_QueueEvent -> Com_EventLoop -> CL_KeyEvent -> +forward ->
     CL_Frame position.
  5. INPUT, mouse: while the window holds the capture, injected relative
     motion must produce "[Q3CL] mouse dx=… yaw=…" with yaw changing, and the
     rendered frame must differ from the pre-input one (the view turned).
  6. ESC quits: "[Q3CL] ESC -> quit" then "loop done"/"done", and the desktop
     comes back (the Q3 window's pixels are gone).

Usage:
    python3 scripts/q3play_test.py [--timeout 480] [--iso mectov-q3.iso]

`make check-q3play` builds the MECTOV_Q3=1 ISO as mectov.iso and runs this
with the default --iso, like the q3/q3gl suites; CI passes mectov-q3.iso.
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import time

import terminal_launch

SERIAL_LOG = "/tmp/mectov_q3play_serial.log"
MON_SOCK = "/tmp/mectov_q3play_monitor.sock"
CURSOR_PPM = "/tmp/mectov_q3play_cursor.ppm"
SHOT_SCENE = "/tmp/q3play_scene.ppm"
SHOT_AIM = "/tmp/q3play_aim.ppm"
SHOT_MOVED = "/tmp/q3play_moved.ppm"
SHOT_QUIT = "/tmp/q3play_quit.ppm"

LOGIN_KEYS = ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]
Q3PLAY_KEYS = ["q", "3", "p", "l", "a", "y", "ret"]

READY_MARKER = "[Q3CL] world ready"
FRAME_MARKER = "[Q3CL] frame="
MOUSE_MARKER = "[Q3CL] mouse dx="
KEY_MARKER = "[Q3CL] key down w bind=+forward"
QUIT_MARKER = "[Q3CL] ESC -> quit"

# Window geometry as CL_StartHunkUsers() computes it: 322x262 centred in the
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
    deadline = time.time() + timeout
    while time.time() < deadline:
        if needle in read_file(path):
            return True
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


def sendkey(key, hold_ms=None):
    mon_cmd(f"sendkey {key}" + (f" {hold_ms}" if hold_ms else ""))


def type_line(keys, retries=3, ready_marker=None, timeout=60):
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


def screendump(path, settle=1.0):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    mon_cmd(f"screendump {path}", settle=settle)
    return os.path.exists(path) and os.path.getsize(path) > 1000


def load_ppm_pixels(path):
    """Return (width, height, bytes RGB) for a P6 ppm (QEMU's header is
    `P6\\n<w> <h>\\n255\\n`, but parse whitespace-separated tokens)."""
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


def window_rect(w, h):
    wx = (w - WIN_W) // 2
    wy = (h - TASKBAR_H - WIN_H) // 2
    return wx + 1, wy + 1 + TITLEBAR_H, CONTENT_W, CONTENT_H


def count_colors(px, w, x0, y0, x1, y1, step=2):
    """Classify sampled pixels of a region into the Q3 scene's colour axes."""
    sky = warm = orange = green = bright = sample = 0
    for y in range(y0, y1, step):
        base = y * w * 3
        for x in range(x0, x1, step):
            o = base + x * 3
            r, g, b = px[o], px[o + 1], px[o + 2]
            sample += 1
            if b > r + 40 and b > g + 30:
                sky += 1
            elif r > g > b and r > 48:
                warm += 1
            if r > g + 60 and g > b + 40:
                orange += 1
            if g > r + 60 and g > b + 60:
                green += 1
            if r + g + b > 600:
                bright += 1
    return {"sky": sky, "warm": warm, "orange": orange, "green": green,
            "bright": bright, "sample": sample}


def assert_scene_pixels(path):
    """(ok, detail): the Q3 window must show the rendered arena."""
    w, h, px = load_ppm_pixels(path)
    x0, y0, cw, ch = window_rect(w, h)
    c = count_colors(px, w, x0, y0, x0 + cw, y0 + ch)
    detail = (f"{c['detail'] if 'detail' in c else ''}"
              f"sky={c['sky']} warm={c['warm']} orange={c['orange']} "
              f"green={c['green']} bright={c['bright']} (of {c['sample']})")
    # Thresholds are deliberately low (the sampled count is ~9600 at step 2)
    # but the four axes must all be present: sky above the walls, warm
    # wall/floor geometry, both bot colours, and lit/crosshair pixels.
    ok = (c["sky"] >= 400 and c["warm"] >= 1500 and
          c["orange"] >= 8 and c["green"] >= 4 and c["bright"] >= 4)
    return ok, detail


def centre_is_bright(path):
    """The crosshair sits at the content centre: a few bright pixels there."""
    w, h, px = load_ppm_pixels(path)
    x0, y0, cw, ch = window_rect(w, h)
    cx, cy = x0 + cw // 2, y0 + ch // 2
    hits = 0
    for y in range(cy - 6, cy + 7):
        base = y * w * 3
        for x in range(cx - 6, cx + 7):
            o = base + x * 3
            if px[o] + px[o + 1] + px[o + 2] > 600:
                hits += 1
    return hits, (cx, cy)


def last_frame_state(log):
    """Last {pos: (x, y, z), yaw: int, pitch: int} from the frame markers."""
    st = None
    for m in re.finditer(
            r"\[Q3CL\] frame=(\d+) t=(\d+) pos=(-?\d+),(-?\d+) z=(-?\d+) "
            r"yaw=(-?\d+) pitch=(-?\d+)", log):
        st = {"frame": int(m.group(1)),
              "pos": (int(m.group(3)), int(m.group(4)), int(m.group(5))),
              "yaw": int(m.group(6)), "pitch": int(m.group(7))}
    return st


def tail_state_before(log, marker, field):
    """Value of a frame-marker field from the last marker BEFORE `marker`."""
    idx = log.find(marker)
    if idx < 0:
        return None
    st = last_frame_state(log[:idx])
    return st[field] if st else None


def sampled_diff(path_a, path_b, step=99):
    _wa, _ha, pa = load_ppm_pixels(path_a)
    _wb, _hb, pb = load_ppm_pixels(path_b)
    return sum(1 for x, y in zip(pa[::step], pb[::step]) if x != y)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=420)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK, SHOT_SCENE, SHOT_AIM, SHOT_MOVED, SHOT_QUIT,
              CURSOR_PPM):
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
            for line in read_file(SERIAL_LOG).splitlines()[-25:]:
                print(line[:130])
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
            print("[FAIL] terminal did not launch")
            return 1
        if not wait_for_in_file(SERIAL_LOG, "ipc_create key=0x0000DEAD", 30):
            print("[FAIL] terminal never became ready")
            return 1
        time.sleep(1.0)
        mon_cmd("mouse_move 300 176")
        time.sleep(0.1)
        mon_cmd("mouse_button 1"); time.sleep(0.1); mon_cmd("mouse_button 0")
        time.sleep(0.5)

        if not type_line(Q3PLAY_KEYS, retries=2, ready_marker=READY_MARKER,
                         timeout=90):
            print("[FAIL] q3play never reported its world ready")
            for line in read_file(SERIAL_LOG).splitlines()[-30:]:
                print(line[:130])
            return 1
        print("[OK] client layer up (window + TinyGL world ready)")

        if not wait_for_in_file(SERIAL_LOG, FRAME_MARKER, 60):
            print("[FAIL] no [Q3CL] frame= markers (CL_Frame not running)")
            return 1
        print("[OK] CL_Frame ticking inside the engine's frame loop")

        time.sleep(6)
        if not screendump(SHOT_SCENE):
            print("[FAIL] screendump failed")
            return 1
        ok, detail = assert_scene_pixels(SHOT_SCENE)
        print(f"     scene: {detail}")
        if not ok:
            print("[FAIL] the Q3 window does not show the rendered arena")
            return 1
        w, h, px = load_ppm_pixels(SHOT_SCENE)
        sx0, sy0, scw, sch = window_rect(w, h)
        scene_sky = count_colors(px, w, sx0, sy0, sx0 + scw, sy0 + sch)["sky"]
        print("[OK] window shows the rendered arena (sky/wall/bots)")

        hits, centre = centre_is_bright(SHOT_SCENE)
        if hits < 4:
            print(f"[FAIL] no crosshair at the window centre {centre} "
                  f"(bright pixels={hits})")
            return 1
        print(f"[OK] crosshair rendered at {centre} (bright pixels={hits})")

        # ---- keyboard input: hold W, the camera must move forward ----------
        before = last_frame_state(read_file(SERIAL_LOG))
        sendkey("w", 600)
        if not wait_for_in_file(SERIAL_LOG, KEY_MARKER, 20):
            print("[FAIL] 'sendkey w' never reached CL_KeyEvent as +forward")
            for line in read_file(SERIAL_LOG).splitlines()[-20:]:
                print(line[:130])
            return 1
        print("[OK] WM raw scancode -> Com_QueueEvent -> CL_KeyEvent -> +forward")
        if not wait_for_in_file(SERIAL_LOG, "[Q3CL] key up w", 20):
            print("[FAIL] key release never reached the client (stuck keys)")
            return 1
        print("[OK] key release also delivered (movement keys can be released)")

        deadline = time.time() + 25
        moved = None
        while time.time() < deadline:
            moved = last_frame_state(read_file(SERIAL_LOG))
            if moved and before and moved["frame"] > before["frame"]:
                break
            time.sleep(1)
        if not (before and moved and moved["frame"] > before["frame"]):
            print("[FAIL] frame markers stopped after the key press")
            return 1
        dy = moved["pos"][1] - before["pos"][1]
        if abs(dy) < 4:
            print(f"[FAIL] camera did not move on +forward "
                  f"(pos {before['pos']} -> {moved['pos']})")
            return 1
        print(f"[OK] +forward moved the camera {dy} units "
              f"(pos {before['pos']} -> {moved['pos']})")

        # ---- mouse input: relative motion must turn the view --------------
        sendkey("s", 600)     # walk back, keeps the run symmetric
        time.sleep(1.5)
        if not screendump(SHOT_AIM):
            print("[FAIL] screendump A failed")
            return 1
        yaw_before = last_frame_state(read_file(SERIAL_LOG))["yaw"]
        for _ in range(8):
            mon_cmd("mouse_move 120 0")
            time.sleep(0.15)
        if not wait_for_in_file(SERIAL_LOG, MOUSE_MARKER, 20):
            print("[FAIL] mouse capture never delivered a CL_MouseEvent")
            for line in read_file(SERIAL_LOG).splitlines()[-20:]:
                print(line[:130])
            return 1
        m = re.search(r"\[Q3CL\] mouse dx=(\d+) dy=(\d+) yaw=(-?\d+)", 
                      read_file(SERIAL_LOG))
        print(f"[OK] mouse routed into the engine "
              f"(dx={m.group(1)} dy={m.group(2)} yaw={m.group(3)})")
        time.sleep(2)
        if not screendump(SHOT_MOVED):
            print("[FAIL] screendump B failed")
            return 1
        now = last_frame_state(read_file(SERIAL_LOG))
        if now is None or abs(now["yaw"] - yaw_before) < 3:
            print(f"[FAIL] yaw did not change from mouse motion "
                  f"({yaw_before} -> {now['yaw'] if now else None})")
            return 1
        print(f"[OK] mouse look changed yaw {yaw_before} -> {now['yaw']}")
        diff = sampled_diff(SHOT_AIM, SHOT_MOVED)
        if diff < 80:
            print(f"[FAIL] the frame did not change after turning (diff={diff})")
            return 1
        print(f"[OK] the rendered view changed after the turn (diff={diff})")

        # ---- ESC quits and gives the desktop back -------------------------
        sendkey("esc")
        if not wait_for_in_file(SERIAL_LOG, QUIT_MARKER, 20):
            print("[FAIL] ESC was not routed to the client")
            return 1
        if not wait_for_in_file(SERIAL_LOG, "[Q3CL] done", 30):
            print("[FAIL] the client loop did not shut down cleanly")
            for line in read_file(SERIAL_LOG).splitlines()[-20:]:
                print(line[:130])
            return 1
        print("[OK] ESC shut the client down (window closed, loop done)")
        # The desktop repaints the window's rectangle on the next compositor
        # pass, which can take a couple of seconds under TCG — poll instead of
        # assuming one fixed delay is enough.
        after = None
        deadline = time.time() + 25
        while time.time() < deadline:
            time.sleep(3)
            if not screendump(SHOT_QUIT):
                continue
            w, h, px = load_ppm_pixels(SHOT_QUIT)
            x0, y0, cw, ch = window_rect(w, h)
            after = count_colors(px, w, x0, y0, x0 + cw, y0 + ch)
            if after["sky"] < 400:
                break
        if after is None or after["sky"] >= scene_sky // 4:
            print(f"[FAIL] the Q3 window is still on screen after quit "
                  f"(sky={after['sky'] if after else None}, "
                  f"window had {scene_sky})")
            return 1
        print(f"[OK] desktop restored after the client shut down "
              f"(sky={after['sky']})")

        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive through the whole session")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
