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

The launch has two failure modes and they need different patience:

* **the double-click never lands** (dropped PS/2 packet, cursor drifted) —
  retry from the verified corner;
* **the app starts but never becomes ready** — the Terminal creates its IPC
  queue microseconds into `_start()` (apps/terminal.c: sys_create_window ->
  setsid/tcsetpgrp -> add_default_aliases -> sys_ipc_create), so this is not a
  slow boot: either the guest was starved at that instant or the instance died
  on the way (a failed sys_create_window calls sys_exit() silently). The answer
  is to click again for a fresh instance and wait longer each round, and — if
  it still fails — to print the evidence instead of a bare one-liner.

`launch_terminal()` therefore returns only once the Terminal is READY, which is
the contract every suite in this tree relies on (it used to be two separate
steps in 38 files, each with its own 30s timeout and its own message).
"""
import time

ICON_X, ICON_Y = 60, 64

# The Terminal's IPC queue: keyboard routing is live from the moment it exists.
IPC_READY = "ipc_create key=0x0000DEAD"
# The loader prints this per app instance, so it also counts instances started.
LOADER_START = "[LOADER] start"

# Readiness patience per retry round, in seconds. An idle guest's first round
# is as short as it ever was; a loaded CI runner gets a second round with twice
# the room. Deliberately capped at two rounds: the whole failure path has to
# stay well inside the smallest suite timeout in check.py (240s for fork_test),
# including QEMU boot (~25s), or a killed suite would lose the report that says
# what went wrong.
READY_WAITS = (30, 60)


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


def _count(path, needle):
    """Occurrences of `needle` in a serial log (0 if the file is not there yet)."""
    try:
        with open(path, "r", errors="replace") as f:
            return f.read().count(needle)
    except OSError:
        return 0


def _serial_tail(path, lines=20):
    try:
        with open(path, "r", errors="replace") as f:
            return f.read().splitlines()[-lines:]
    except OSError:
        return []


def _wait_for_new(path, needle, before, timeout):
    """True once `needle` appears MORE often than `before`.

    Counting beats presence when retrying: an instance that started in an
    earlier round already put the marker in the log, so a presence check would
    report success for a click that launched nothing at all.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        if _count(path, needle) > before:
            return True
        time.sleep(0.5)
    return False


def _report_failure(serial_log, reason, verified, clicks, rounds, waited):
    """Print why the launch failed, with the evidence needed to act on it.

    A one-line "terminal never became ready" in CI leaves the reader guessing
    between a dropped click, a starved guest and an app that died early. The
    counts below separate those cases, and the serial tail shows the context
    (SYS_EXIT right after a [LOADER] start, a [WM] refusal, a PANIC ...).
    """
    starts = _count(serial_log, LOADER_START)
    exits = _count(serial_log, "SYS_EXIT code=")
    panics = _count(serial_log, "[PANIC]")
    print(f"[launch] FAILED: {reason}")
    print(f"[launch]   rounds: {rounds} (patience {READY_WAITS}), waited: {waited}s, "
          f"verified double-click attempts: {verified}, click pairs sent: {clicks}")
    print(f"[launch]   app starts seen: {starts} ('{LOADER_START}'), "
          f"SYS_EXIT lines: {exits}, panics: {panics}")
    if _count(serial_log, IPC_READY) == 0:
        print(f"[launch]   terminal IPC queue ('{IPC_READY}'): never created")
    tail = _serial_tail(serial_log)
    if tail:
        print(f"[launch]   last {len(tail)} serial lines:")
        for line in tail:
            print("[launch]     " + line[:120])
    else:
        print("[launch]   serial log missing or empty — QEMU likely failed to start")
    print("[launch]   how to read this:")
    if starts == 0:
        print("[launch]     the app never loaded at all -> the icon double-click is not")
        print("[launch]     reaching the desktop (cursor/PS2 path), not an app problem.")
    elif exits > 0:
        print("[launch]     the app loaded and then exited before creating its queue ->")
        print("[launch]     it died on the way in (sys_create_window failing exits silently,")
        print("[launch]     which is the low-RAM case), not a slow boot.")
    else:
        print("[launch]     the app loaded and is still running but never reached")
        print("[launch]     sys_ipc_create -> the guest was starved (loaded CI runner), which")
        print("[launch]     the escalating retries above are meant to absorb.")
    print("[launch]   re-run the suite before suspecting the kernel if the tail looks healthy.")


def _start_app(mon, serial_log, dump_path, attempts, pairs=5):
    """Verified double-click rounds until a NEW app instance starts loading.

    `pairs` is the click burst per verified attempt (a reduced burst is used
    when re-clicking in a later round, where the cursor is already known to be
    on the icon and the budget belongs to waiting).

    Returns (started, verified_attempts, click_pairs).
    """
    before = _count(serial_log, LOADER_START)
    verified = clicks = 0
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
        verified += 1
        print(f"[launch] attempt {attempt + 1}: cursor verified on icon, clicking")
        # Double-click with a hold that GROWS per attempt. The desktop detects
        # clicks by polling mouse_btn vs its previous value in the main loop, so
        # a too-short blip can be missed when the guest is slow (TCG under CI),
        # and a too-long hold drags the icon / overshoots the double-click
        # window. 0.15s is plenty on an idle guest; a starved desktop polls
        # slower, so a failed attempt (and a second instance) gets a longer
        # press instead of the identical one repeated. The kernel scales the
        # pair's window to a real 0.8s via ticks_per_sec, so the pair stays
        # under that in wall time (~0.56s at 0.15s, ~0.86s at 0.35s — the last
        # rounds trade the window for the chance a loaded guest sees the press;
        # extra clicks after a launch are harmless anyway).
        hold = 0.15 + 0.10 * attempt
        wait = 2 + attempt
        for _ in range(pairs):
            mon("mouse_button 1")
            time.sleep(hold)
            mon("mouse_button 0")
            time.sleep(0.05)
            mon("mouse_button 1")
            time.sleep(hold)
            mon("mouse_button 0")
            time.sleep(0.05)
            clicks += 1
            if _wait_for_new(serial_log, LOADER_START, before, wait):
                return True, verified, clicks
    return False, verified, clicks


def launch_terminal(mon, serial_log, dump_path, attempts=6, require_ready=True):
    """Launch the Terminal; True once it is ready to take keyboard input.

    `mon` is a callable sending one QEMU monitor command. With the default
    `require_ready=True` this returns True only after the Terminal's IPC queue
    exists in serial_log, retrying the icon click with escalating patience
    (READY_WAITS) when the app starts but never gets there. Pass
    `require_ready=False` for the rare caller that only needs a window up.
    """
    verified = clicks = 0

    if not require_ready:
        started, v, c = _start_app(mon, serial_log, dump_path, attempts)
        if not started:
            _report_failure(serial_log, "the Terminal never started loading",
                            v, c, 1, 0)
        return started

    waited = 0
    # Readiness is counted, not just detected: this boot may already carry an
    # IPC line from an earlier Terminal (a suite that re-launches one, or two
    # boots sharing a log), and "the marker exists somewhere" would call that
    # ready before THIS launch got anywhere.
    ipc_before = _count(serial_log, IPC_READY)
    for round_no, ready_wait in enumerate(READY_WAITS, start=1):
        # Later rounds take a short path: the cursor is already known to be on
        # the icon and the app already loaded once, so two verified attempts
        # with a two-pair burst are enough to spawn a fresh instance — the
        # round's budget belongs to waiting, not to re-proving the corner.
        started, v, c = _start_app(mon, serial_log, dump_path,
                                   attempts if round_no == 1 else 2,
                                   5 if round_no == 1 else 2)
        verified += v
        clicks += c
        if not started:
            print(f"[launch] round {round_no}: no new app instance started")
            continue
        t0 = time.time()
        if _wait_for_new(serial_log, IPC_READY, ipc_before, ready_wait):
            print(f"[launch] terminal ready (round {round_no}, {time.time() - t0:.1f}s "
                  f"of a {ready_wait}s budget)")
            return True
        waited += ready_wait
        if round_no < len(READY_WAITS):
            print(f"[launch] round {round_no}: app started but its IPC queue did not "
                  f"appear in {ready_wait}s — clicking the icon again for a fresh "
                  f"instance and waiting longer")

    _report_failure(serial_log, "the Terminal never became ready", verified, clicks,
                    len(READY_WAITS), waited)
    return False
