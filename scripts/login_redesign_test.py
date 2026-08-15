#!/usr/bin/env python3
"""
scripts/login_redesign_test.py — CI test for the redesigned Mectov login screen.

Boots mectov.iso in QEMU and verifies the new gate flow:
  1. Lock screen shows a clock + full date pinned to the BOTTOM-LEFT corner.
  2. A non-SPACE keypress does NOT open the password entry (SPACE-only dismiss).
  3. SPACE opens the password entry — and while typing, the wallpaper BLURS and
     the bottom-left clock + version footer DISAPPEAR so the panel is the only
     thing in focus.
  4. ~4s of no typing reverts to the lock screen (idle fallback).
  5. SPACE again + correct password logs in (BOOTED KERNEL LOOP).

Wallpaper is 1024x768 fullscreen, so pixels near the screen edges should show
wallpaper content (not solid filler) — verified via screendump sampling. Blur
is measured as the mean horizontal neighbor-difference in a wallpaper-only
region (sharp on the lock screen, low after the box blur).

Exit code 0 = pass, 1 = fail.
"""
import argparse
import os
import socket
import subprocess
import sys
import time

SERIAL_LOG = "/tmp/mectov_login_serial.log"
MON_SOCK = "/tmp/mectov_login_monitor.sock"


def wait_for_in_file(path, needle, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(path, "r", errors="replace") as f:
                if needle in f.read():
                    return True
        except (FileNotFoundError, OSError):
            pass
        time.sleep(1)
    return False


def mon_cmd(cmd, wait=0.2):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(MON_SOCK)
        s.sendall((cmd + "\n").encode())
        time.sleep(wait)
        s.close()
    except OSError as e:
        print(f"[!] monitor cmd '{cmd}' failed: {e}")


def screendump(path, wait=0.4):
    mon_cmd(f"screendump {path}", wait=wait)
    # QEMU writes the PPM when the command completes
    deadline = time.time() + 5
    while time.time() < deadline:
        if os.path.exists(path):
            return True
        time.sleep(0.2)
    return False


def load_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = map(int, line.split())
        f.readline()  # maxval
        data = f.read()
    px = []
    for i in range(0, len(data), 3):
        px.append((data[i], data[i + 1], data[i + 2]))
    return w, h, px


def px_at(px, w, x, y):
    return px[y * w + x]


def mean_neighbor_diff(px, w, x0, y0, x1, y1):
    """Mean |p[x+1]-p[x]| over a region — high on sharp detail, low after blur."""
    total, n = 0, 0
    for y in range(y0, y1):
        row = y * w
        for x in range(x0, x1 - 1):
            a, b = px[row + x], px[row + x + 1]
            total += abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])
            n += 1
    return total / max(n, 1)


def count_dim_text(px, w, y0, y1, cx, half):
    """Count pixels close to the footer dim-gray (0x8A8172) in a horizontal band."""
    n = 0
    for y in range(y0, y1):
        for x in range(cx - half, cx + half):
            r, g, b = px[y * w + x]
            if abs(r - 0x8A) < 20 and abs(g - 0x81) < 20 and abs(b - 0x72) < 20:
                n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--iso", default="mectov.iso")
    ap.add_argument("--disk", default="disk.img")
    ap.add_argument("--ext2", default="ext2.img")
    args = ap.parse_args()

    for p in (SERIAL_LOG, MON_SOCK):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    qemu = subprocess.Popen([
        "qemu-system-i386",
        "-vga", "std",
        "-cdrom", args.iso,
        "-m", "128",
        "-smp", "4",
        "-display", "none",
        "-serial", f"file:{SERIAL_LOG}",
        "-net", "none",
        "-snapshot",   # never write the drive images (a run.sh instance may hold them)
        "-drive", f"file={args.disk},format=raw,index=0,media=disk",
        "-drive", f"file={args.ext2},format=raw,index=1,media=disk",
        "-monitor", f"unix:{MON_SOCK},server,nowait",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        if not wait_for_in_file(SERIAL_LOG, "[K] login", args.timeout):
            print("[FAIL] kernel never reached login screen")
            return 1
        print("[OK] booted to login screen")

        time.sleep(2)  # let the lock screen render a couple frames

        # ---- 1. Lock screen: wallpaper fills the full screen + bottom-left clock
        dump1 = "/tmp/mectov_login_lock.ppm"
        if not screendump(dump1):
            print("[FAIL] could not capture lock-screen dump")
            return 1
        w, h, px = load_ppm(dump1)
        if w != 1024 or h != 768:
            print(f"[WARN] unexpected resolution {w}x{h} (expecting 1024x768)")
        # Wallpaper is a dark warm image; a full-bleed stretch means top-left,
        # top-right and bottom-right corners all carry actual wallpaper pixels.
        # Sample a few scattered points — they must NOT all be identical filler.
        samples = [(10, 10), (w - 10, 10), (w - 10, h - 10), (512, 384), (100, 700)]
        vals = {px_at(px, w, x, y) for x, y in samples}
        print(f"[i] wallpaper corner samples: {sorted(vals)[:3]}... ({len(vals)} distinct)")
        if len(vals) < 3:
            print("[FAIL] wallpaper does not look full-bleed (samples too uniform)")
            return 1
        # Clock lives at bottom-left (x~40..360, y~h-180..h-100): expect bright
        # amber-ish pixels there (the HH:MM:SS digits), distinct from wallpaper.
        amber_found = False
        for y in range(h - 165, h - 105, 4):
            for x in range(50, 300, 4):
                r, g, b = px_at(px, w, x, y)
                if r > 160 and g > 120 and b < 130:  # phosphor amber
                    amber_found = True
                    break
            if amber_found:
                break
        print("[OK] bottom-left clock glyphs detected" if amber_found
              else "[FAIL] no amber clock glyphs at bottom-left")
        if not amber_found:
            return 1
        # The lock screen is deliberately bare: no "press SPACE to sign in"
        # hint, no "v38.xx SMP MECTOVFS 1024x768" footer (both used IC_DIM
        # 0x8A8172, bottom-center). Both bands must be free of that dim text
        # (the bottom-left date uses bright off-white, never matches).
        dim_text = (count_dim_text(px, w, h - 112, h - 88, w // 2, 220) +
                    count_dim_text(px, w, h - 60, h - 36, w // 2, 220))
        print(f"[i] lock-screen dim text pixels (hint+footer bands): {dim_text}")
        if dim_text > 40:
            print("[FAIL] lock screen still shows hint/footer text")
            return 1
        print("[OK] lock screen is bare (no hint, no footer)")

        # ---- 2. Non-SPACE key must NOT open the password entry
        mon_cmd("sendkey a")
        time.sleep(0.5)
        dump2 = "/tmp/mectov_login_noopen.ppm"
        screendump(dump2)
        w2, h2, px2 = load_ppm(dump2)
        # Password panel is a centered warm-charcoal rounded rect ~380x178 at
        # y=(768-178)/2+56=351..529. Its fill (0x16130F) differs from wallpaper.
        r, g, b = px_at(px2, w2, w2 // 2, 440)
        print(f"[i] center pixel after pressing 'a': rgb({r},{g},{b})")
        panel = (abs(r - 0x16) < 12 and abs(g - 0x13) < 12 and abs(b - 0x0F) < 12)
        if panel:
            print("[FAIL] non-SPACE key opened the password panel")
            return 1
        print("[OK] non-SPACE key ignored (still lock screen)")

        # ---- 3. SPACE opens the password entry
        mon_cmd("sendkey spc")
        # The first password frame also builds the cached blurred wallpaper
        # (a few seconds under TCG) — retry the dump until the panel shows.
        # The panel must be seen on TWO consecutive dumps: the old bug flashed
        # the panel for a single frame and instantly reverted, so one sighting
        # is not proof it stayed. Dump fast (~0.45 s/cycle) because under TCG
        # the guest clock can outrun wall time, shrinking the 4 s window to
        # ~2 s — a slow cycle can miss it entirely.
        panel = False
        seen = 0
        w3, h3, px3 = None, None, None
        for _ in range(40):
            time.sleep(0.3)
            dump3 = "/tmp/mectov_login_panel.ppm"
            screendump(dump3, wait=0.15)
            w3, h3, px3 = load_ppm(dump3)
            r, g, b = px_at(px3, w3, w3 // 2, 440)
            seen = (seen + 1) if (abs(r - 0x16) < 12 and abs(g - 0x13) < 12
                                 and abs(b - 0x0F) < 12) else 0
            if seen >= 2:
                panel = True
                break
        if not panel:
            print(f"[FAIL] SPACE did not open a persistent password panel (center rgb({r},{g},{b}), consecutive sightings={seen})")
            return 1
        print("[OK] SPACE opened the password panel")
        print("[OK] password panel stays on screen")

        # ---- 3b. The open fade/blur transition must settle before the
        # content checks: mid-fade the background is only half blurred and
        # the clock is still fading out. Re-dump until the wallpaper
        # sharpness drops below the blur bar (and the panel is still up).
        # (Lock-screen sharpness, measured on the dump1 wallpaper, is the
        # reference for the blur bar.)
        sharp = mean_neighbor_diff(px, w, 60, 40, 460, 220)
        settled = False
        for _ in range(20):
            time.sleep(0.4)
            dump3s = "/tmp/mectov_login_settle.ppm"
            screendump(dump3s, wait=0.15)
            w3s, h3s, px3s = load_ppm(dump3s)
            r, g, b = px_at(px3s, w3s, w3s // 2, 440)
            if not (abs(r - 0x16) < 12 and abs(g - 0x13) < 12
                    and abs(b - 0x0F) < 12):
                print("[FAIL] password panel disappeared before the fade settled")
                return 1
            soft = mean_neighbor_diff(px3s, w3s, 60, 40, 460, 220)
            if soft < sharp * 0.70:
                settled = True
                w3, h3, px3 = w3s, h3s, px3s   # content checks use this frame
                break
        if not settled:
            print("[FAIL] blurred background never settled (fade stuck?)")
            return 1
        print("[OK] fade/blur transition settled")

        # ---- 3c. Password screen: clock + footer gone, wallpaper blurred
        # Clock region (bottom-left) must no longer hold amber glyphs.
        amber_gone = True
        for y in range(h3 - 165, h3 - 105, 4):
            for x in range(50, 300, 4):
                r, g, b = px_at(px3, w3, x, y)
                if r > 160 and g > 120 and b < 130:
                    amber_gone = False
                    break
            if not amber_gone:
                break
        print("[OK] clock hidden while typing password" if amber_gone
              else "[FAIL] clock still visible on password screen")
        if not amber_gone:
            return 1

        # (The version footer was removed from the lock screen entirely in
        # v38.33, and the password screen never drew one — the "lock screen
        # is bare" check above covers it.)

        # Blur: sharpness in a wallpaper-only region (top-left) must drop a lot.
        # A 5-tap box blur typically cuts the mean horizontal neighbor-diff to
        # ~45-60% of the sharp copy (the wallpaper is dark and smooth there,
        # so the reduction is modest); without blur the ratio is ~100%. The
        # 70% bar cleanly separates the two. (soft is recomputed here on the
        # settled dump for the diagnostic line.)
        soft = mean_neighbor_diff(px3, w3, 60, 40, 460, 220)
        print(f"[i] wallpaper sharpness (mean neighbor diff): lock={sharp:.2f}, "
              f"password={soft:.2f} (ratio {soft/sharp:.2f})")
        if soft < sharp * 0.70:
            print("[OK] wallpaper blurred on password screen")
        else:
            print("[FAIL] wallpaper does not look blurred (needs < 70% of lock sharpness)")
            return 1

        # ---- 4. Idle ~4s reverts to the lock screen through the close
        # cross-fade (the panel fades out while the clock fades back in, so
        # retry through the animation instead of checking one frame).
        time.sleep(5)  # > 4s without any key: the close transition starts
        panel_gone = False
        for _ in range(24):
            dump4 = "/tmp/mectov_login_revert.ppm"
            screendump(dump4, wait=0.15)
            w4, h4, px4 = load_ppm(dump4)
            r, g, b = px_at(px4, w4, w4 // 2, 440)
            if not (abs(r - 0x16) < 12 and abs(g - 0x13) < 12
                    and abs(b - 0x0F) < 12):
                panel_gone = True
                break
            time.sleep(0.5)
        if not panel_gone:
            print("[FAIL] password panel still visible after 4s idle")
            return 1
        print("[OK] 4s idle reverted to lock screen")
        # ...and the clock fades back in at the bottom-left corner.
        amber_back = False
        for _ in range(20):
            dump4b = "/tmp/mectov_login_clockback.ppm"
            screendump(dump4b, wait=0.15)
            w4b, h4b, px4b = load_ppm(dump4b)
            for y in range(h4b - 165, h4b - 105, 4):
                for x in range(50, 300, 4):
                    r, g, b = px_at(px4b, w4b, x, y)
                    if r > 160 and g > 120 and b < 130:
                        amber_back = True
                        break
                if amber_back:
                    break
            if amber_back:
                break
            time.sleep(0.5)
        print("[OK] clock returned after idle revert" if amber_back
              else "[FAIL] clock missing after idle revert")
        if not amber_back:
            return 1

        # ---- 5. Login still works: SPACE + password
        for k in ["spc", "m", "e", "c", "t", "o", "v", "1", "2", "3", "ret"]:
            mon_cmd("sendkey " + k)
            time.sleep(0.15)

        if not wait_for_in_file(SERIAL_LOG, "BOOTED KERNEL LOOP", 90):
            print("[FAIL] login did not complete (no BOOTED KERNEL LOOP)")
            return 1
        print("[OK] login succeeded, desktop loop running")

        time.sleep(3)
        if qemu.poll() is not None:
            print(f"[FAIL] QEMU exited early with code {qemu.returncode}")
            return 1
        print("[OK] OS stayed alive")
        return 0
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass


if __name__ == "__main__":
    sys.exit(main())
