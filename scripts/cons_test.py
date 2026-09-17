#!/usr/bin/env python3
"""scripts/cons_test.py — console text gate (screenshot proof), M8 + M9.

The console has no serial trace of its own (it *is* the serial trace, drawn),
so this test judges the pixels instead. M9 moved the console into a desktop
window, so the view is read from the kernel's own marker:
`gui: desktop up win=.. client=x,y,w,h` (falling back to the full screen when
the desktop is disabled).

  1. ink        — the window's client area carries a sane amount of glyph
                  ink: a boot log is text, not a smear or an empty panel.
  2. glyph-exact— every client text row is compared against EVERY line the
                  serial log contains, rendered with the same 8x16 font table
                  (src/drivers/font8x16.c). A row matches only if its ink mask
                  is pixel-identical, so >= 8 matching rows means the console
                  draws the kernel's real output, in order, with the right
                  font — not just "some pixels".
  3. cursor     — exactly one 8x2 amber underline inside the client: painted
                  once, so no stale underline survived a scroll or a wrap.
  4. no leftovers— zero M8 default-glyph pixels (0xD4D4D4) anywhere. The M9
                  desktop recolours the console to the ink palette, so any
                  grey glyph pixel is a whole-screen scrawl left behind by a
                  print that raced the desktop transition.

Exit 0 PASS, nonzero FAIL. Dump kept at .check/cons_shot.ppm for eyeballing.
"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
from qmp import QMP

ISO = "mectov64.iso"
SERIAL = "serial_cons.log"
SOCK = "/tmp/qmp_cons"
DUMP = os.path.join(".check", "cons_shot.ppm")
FONT = "src/drivers/font8x16.c"

CELL_W, CELL_H = 8, 16
BG = (0x00, 0x00, 0x00)
M8_FG = (0xD4, 0xD4, 0xD4)  # M8 default glyph colour (leftovers detector)
CURSOR = (0xE0, 0xA9, 0x4F)  # console underline (IC_AMBER)
CLIENT_RE = re.compile(
    r"gui: desktop up win=\d+,\d+,\d+,\d+ client=(\d+),(\d+),(\d+),(\d+)")


def has_kvm():
    return os.path.exists("/dev/kvm")


def boot():
    cmd = ["qemu-system-x86_64", "-machine", "q35"]
    if has_kvm():
        cmd += ["-cpu", "host", "-enable-kvm"]
    else:
        cmd += ["-cpu", "qemu64,+nx"]
    cmd += ["-m", "256", "-smp", "4", "-cdrom", ISO,
            "-serial", f"file:{SERIAL}", "-no-reboot", "-display", "none",
            "-qmp", f"unix:{SOCK},server=on,wait=off"]
    for p in (SOCK, SERIAL):
        try:
            os.unlink(p)
        except OSError:
            pass
    return subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)


def wait_for(path, needle, timeout, poll=1.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with open(path, errors="replace") as f:
                if needle in f.read():
                    return True
        except OSError:
            pass
        time.sleep(poll)
    return False


def load_ppm(path):
    """P6 PPM (QEMU screendump) -> (w, h, pixel bytes)."""
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    assert parts[0] == b"P6", parts[0]
    w, h = map(int, parts[1].split())
    assert int(parts[2]) == 255
    return w, h, parts[3]


def load_font(path):
    """Parse src/drivers/font8x16.c into 256 ink masks (16 rows of 8 bits)."""
    rows = {}
    pat = re.compile(r"\[0x([0-9A-Fa-f]{2})\]\s*=\s*\{([^}]*)\}")
    with open(path) as f:
        for line in f:
            m = pat.search(line)
            if not m:
                continue
            ch = int(m.group(1), 16)
            rows[ch] = [int(v, 16) for v in m.group(2).split(",") if v.strip()]
    assert len(rows) == 256, f"font parse got {len(rows)} glyphs"
    return rows


def cell_mask(pix, w, x0, y0, bg=(0, 0, 0)):
    """Per-scanline ink bits of one 8x16 cell (bit 7 = leftmost pixel).

    `bg` is the cell background: the M8 console drew on black, the M9 window
    on the desktop's panel charcoal, so "ink" is "differs from bg" rather than
    "non-black"."""
    out = []
    for j in range(CELL_H):
        bits = 0
        base = ((y0 + j) * w + x0) * 3
        for i in range(CELL_W):
            o = base + i * 3
            if (pix[o], pix[o + 1], pix[o + 2]) != bg:
                bits |= 0x80 >> i
        out.append(bits)
    return out


def row_mask(pix, w, cols, y0, x0=0, bg=(0, 0, 0)):
    """Ink bits of a whole text row: every cell, left to right, from `x0`."""
    out = []
    for c in range(cols):
        out.extend(cell_mask(pix, w, x0 + c * CELL_W, y0, bg))
    return out


def render_line(font, text):
    """Ink mask of the first `len(text)` cells (what the kernel would draw)."""
    mask = []
    for x in range(len(text)):
        ch = font.get(ord(text[x]))
        if ch is None or len(ch) != CELL_H:
            return None
        for j in range(CELL_H):
            mask.append(ch[j])
    return mask


def main():
    if not os.path.exists(ISO):
        print("cons_test: build the ISO first (make iso64)")
        return 1
    if not os.path.exists(FONT):
        print(f"cons_test: missing {FONT}")
        return 1
    os.makedirs(os.path.dirname(DUMP), exist_ok=True)
    font = load_font(FONT)
    qemu = boot()
    try:
        # The shell prompt means the demos + shell have printed plenty, so the
        # screen holds scrolled output (not just the boot preamble).
        if not wait_for(SERIAL, "mct> ", 180):
            print("cons_test FAIL: no shell prompt")
            return 1
        time.sleep(3)  # let the console flush the lines printed after it
        q = QMP(SOCK)
        try:
            os.unlink(DUMP)
        except OSError:
            pass
        q.hmp(f"screendump {os.path.abspath(DUMP)}")
        for _ in range(40):  # screendump is written asynchronously
            if os.path.exists(DUMP) and os.path.getsize(DUMP) > 1000:
                break
            time.sleep(0.25)
        q.close()

        with open(SERIAL, errors="replace") as f:
            serial = f.read()
        lines = [ln.rstrip("\r") for ln in serial.split("\n") if ln.strip()]
        w, h, pix = load_ppm(DUMP)
        # M9: the console lives in the desktop window's client area.
        m = CLIENT_RE.search(serial)
        if m:
            vx, vy, vw, vh = (int(g) for g in m.groups())
        else:
            vx, vy, vw, vh = 0, 0, w, h
        cols, rows = vw // CELL_W, vh // CELL_H
        vx = max(0, min(vx, w - 8))
        vy = max(0, min(vy, h - 16))
        cols = min(cols, (w - vx) // CELL_W)
        rows = min(rows, (h - vy) // CELL_H)
        print(f"cons_test: screen {w}x{h}, view {vw}x{vh}+{vx}+{vy} -> "
              f"{cols}x{rows} cells, {len(lines)} serial lines")

        def px(x, y):
            o = (y * w + x) * 3
            return (pix[o], pix[o + 1], pix[o + 2])

        # The cell background is whatever dominates the view (black for a
        # full-screen M8 console, panel charcoal inside the M9 window).
        counts = {}
        for y in range(vy, vy + rows * CELL_H, 2):
            for x in range(vx, vx + cols * CELL_W, 2):
                c = px(x, y)
                counts[c] = counts.get(c, 0) + 1
        bg = max(counts, key=counts.get)
        print(f"cons_test: view background = {bg}")

        # --- 1. ink inside the view ---
        ink = 0
        for y in range(vy, vy + rows * CELL_H):
            for x in range(vx, vx + cols * CELL_W):
                if px(x, y) != bg:
                    ink += 1
        view_px = rows * CELL_H * cols * CELL_W
        ink_ok = 2000 <= ink <= int(0.5 * view_px)
        print(f"  [{'PASS' if ink_ok else 'FAIL'}] view ink pixels = {ink} "
              f"of {view_px}")
        rc = 0 if ink_ok else 1

        # --- 2. glyph-exact rows (see the module docstring) ---
        rendered = []
        for ln in lines:
            m2 = render_line(font, ln[:cols])
            if m2 and sum(bin(b).count("1") for b in m2) >= 8:  # non-blank
                rendered.append((ln, m2))
        matched = []
        for r in range(rows):
            mask = row_mask(pix, w, cols, vy + r * CELL_H, vx, bg)
            if sum(bin(b).count("1") for b in mask) < 8:
                continue
            for ln, m2 in rendered:
                # a short line leaves the rest of the row at the client's bg
                if len(m2) <= len(mask) and mask[:len(m2)] == m2 \
                        and not any(mask[len(m2):]):
                    matched.append((r, ln))
                    break
        match_ok = len(matched) >= 8
        print(f"  [{'PASS' if match_ok else 'FAIL'}] glyph-exact rows = "
              f"{len(matched)} of {rows}")
        for r, ln in matched[:3]:
            print(f"          row {r}: {ln[:48]}")
        rc = rc or (not match_ok)

        # --- 3. cursor: exactly one 8x2 underline block inside the view ---
        cur = []
        for y in range(vy, vy + rows * CELL_H):
            for x in range(vx, vx + cols * CELL_W):
                if px(x, y) == CURSOR:
                    cur.append((x, y))
        if cur:
            xs = [p[0] for p in cur]
            ys = [p[1] for p in cur]
            cur_ok = (len(cur) == CELL_W * 2 and len(set(xs)) == CELL_W
                      and len(set(ys)) == 2 and (min(xs) - vx) % CELL_W == 0
                      and (min(ys) - vy) % CELL_H == CELL_H - 2)
        else:
            cur_ok = False
        print(f"  [{'PASS' if cur_ok else 'FAIL'}] console cursor pixels = "
              f"{len(cur)}{' at ' + str(cur[0]) if cur else ''}")
        rc = rc or (not cur_ok)

        # --- 4. no M8 full-screen leftovers ---
        grey = sum(1 for o in range(0, len(pix), 3)
                   if (pix[o], pix[o + 1], pix[o + 2]) == M8_FG)
        left_ok = grey == 0
        print(f"  [{'PASS' if left_ok else 'FAIL'}] M8 grey glyph pixels = {grey}")
        rc = rc or (not left_ok)

        print(f"cons_test: dump = {DUMP}")
        print("PASS" if rc == 0 else "FAIL")
        return rc
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            qemu.kill()


if __name__ == "__main__":
    sys.exit(main())
