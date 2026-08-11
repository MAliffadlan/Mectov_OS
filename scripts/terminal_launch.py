"""Shared Terminal-launch helper for the QEMU boot tests.

Launches the Terminal app on the Mectov desktop by double-clicking its
desktop icon. The OS cursor starts at (400,300); we drive it to the
top-left corner (0,0) with small -127 moves (the kernel clamps at the
corner, so repeated negative moves are idempotent there), then one PS/2
packet to the icon center (60,64). A QEMU screendump verifies the cursor
is actually on the icon before any click — under slow TCG a dropped PS/2
packet would otherwise leave the cursor at an unknown spot and the
double-click misses. Retries from the corner until verified.

Moves stay one packet each with gaps so the 16-byte PS/2 buffer never
overflows (a burst of huge deltas floods it and corrupts alignment,
teleporting the cursor).
"""
import time

ICON_X, ICON_Y = 60, 64


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


def _load_ppm(path):
    """Parse a P6 PPM (QEMU `screendump` format) into (w, h, rgb-bytes)."""
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    assert parts[0] == b"P6", parts[0]
    w, h = map(int, parts[1].split())
    assert int(parts[2]) == 255
    return w, h, parts[3]


def cursor_at(path, x, y):
    """True if the white arrow cursor's tip sits at (x,y), from a PPM dump.

    The cursor is 24 rows tall: a dark (0x111111) outline with a white
    (0xFFFFFF) fill that grows one pixel per row. The tip column is dark
    and the fill runs along the diagonal, which is distinctive enough to
    separate it from text/glyphs on the desktop.
    """
    try:
        w, h, pix = _load_ppm(path)
    except (OSError, AssertionError):
        return False
    if x < 0 or y < 0 or x + 16 >= w or y + 24 >= h:
        return False

    def px(px_, py_):
        o = (py_ * w + px_) * 3
        return pix[o], pix[o + 1], pix[o + 2]

    WHITE = (255, 255, 255)
    DARK = (17, 17, 17)
    if px(x, y + 1) != DARK:          # outline column at the tip
        return False
    for k in range(1, 12):            # white fill along the diagonal
        if px(x + k, y + k) != WHITE:
            return False
    return True


def find_cursor_position(path):
    """Scan a PPM dump for the arrow cursor; return its tip (x,y) or None.

    The tip column is dark (0x111111) with the white fill on the diagonal
    below-right (one pixel wider per row), so we scan dark pixels and
    confirm the diagonal.
    """
    try:
        w, h, pix = _load_ppm(path)
    except (OSError, AssertionError):
        return None

    def px(px_, py_):
        o = (py_ * w + px_) * 3
        return pix[o], pix[o + 1], pix[o + 2]

    WHITE = (255, 255, 255)
    DARK = (17, 17, 17)
    for y in range(h - 24):
        for x in range(w - 16):
            if px(x, y) != DARK:
                continue
            good = True
            for k in range(1, 6):
                if px(x + k, y + k) != WHITE:
                    good = False
                    break
            if good:
                return (x, y)
    return None


def launch_terminal(mon, serial_log, dump_path, attempts=6):
    """Double-click the Terminal desktop icon; True if it launched.

    `mon` is a callable sending one QEMU monitor command. Returns True
    once `[LOADER] start` appears in serial_log (the terminal app
    loading). Retries from the corner with screendump verification.
    """
    verified_once = False
    for attempt in range(attempts):
        # Reset to the top-left corner: idempotent once clamped at (0,0).
        for _ in range(4):
            mon("mouse_move -127 -127")
            time.sleep(0.25)
        time.sleep(0.3)
        mon("mouse_move %d %d" % (ICON_X, ICON_Y))
        time.sleep(0.4)
        # Ground truth: only click when the cursor is verified on the icon.
        mon("screendump " + dump_path)
        time.sleep(0.4)
        if not cursor_at(dump_path, ICON_X, ICON_Y):
            pos = find_cursor_position(dump_path)
            print(f"[launch] attempt {attempt + 1}: cursor NOT on icon"
                  + (f" (found at {pos})" if pos else " (cursor not found in dump)"))
            continue
        verified_once = True
        print(f"[launch] attempt {attempt + 1}: cursor verified on icon, clicking")
        # Double-click with a 150ms hold. The desktop detects clicks by polling
        # mouse_btn vs its previous value in the main loop, so a too-short blip
        # can be missed when the guest is slow (TCG under CI), and a too-long
        # hold drags the icon / overshoots the double-click window. The kernel
        # scales the window to a real 0.8s via ticks_per_sec, so the pair just
        # needs to be under that in wall time (here ~0.56s). Extra clicks after
        # launch are harmless.
        for _ in range(5):
            mon("mouse_button 1")
            time.sleep(0.15)
            mon("mouse_button 0")
            time.sleep(0.05)
            mon("mouse_button 1")
            time.sleep(0.15)
            mon("mouse_button 0")
            time.sleep(0.05)
            if wait_for_in_file(serial_log, "[LOADER] start", 2):
                return True
    print(f"[launch] terminal did not launch (verified_once={verified_once})")
    return False
