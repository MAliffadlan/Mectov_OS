#!/usr/bin/env python3
"""quake_focus.py - bring the QEMU window forward, and keep it there.

Why this exists
---------------
This desktop keeps several maximized windows around (the terminal that started
QEMU, the editor, a browser, Telegram). A window nobody clicked can end up
underneath them - and a *buried* QEMU/GTK window receives no mouse and no
keyboard events at all.  The guest then never sees a single click, stays in its
no-input auto-demo, and its screen looks frozen even while the engine keeps
drawing frames at full rate.  That is indistinguishable from "the game never
appeared" if you are looking at the wrong window.

This script finds the running QEMU window and:
  * activates it (_NET_ACTIVE_WINDOW), which raises + focuses it through the WM
  * raises the WM frame directly (XRaiseWindow on the parent), for WMs that
    restack lazily
  * with --pin (default), adds _NET_WM_STATE_ABOVE so it stays on top while you
    play; --off removes that state again

v38.149: activation alone is not enough, and the player's 12:46 session is the
proof.  The game held the mouse capture for a whole six-minute run while
receiving ZERO pointer events: look_s was 32, 26, 19, 30, 7 in the first five
one-second windows and 0 in the 103 after them, idle_in climbing to 370310 ms,
with the capture still held (comp_s ~27/s).  Nothing in the guest could have
produced that - the host simply stopped delivering.

Two host-side states do exactly this, and both are cleared by the same thing:
  * the QEMU frontend never GRABBED the pointer, so motion is mapped from the
    host pointer's position inside the window.  That is coarse for slow
    movement and it stops dead the moment the pointer reaches the window edge -
    "nengok pelan pake mouse patah patah", then nothing.
  * the window lost INPUT FOCUS, so the frontend is not even asked.
QEMU's SDL/GTK frontend starts the grab on a button press inside the window
(and releases it on Ctrl+Alt+G), which is why --grab exists: it synthesises one
click into the QEMU window with XTest.  QEMU consumes that press to start the
grab, so the click does not reach the guest.  Clicking also focuses the window
under the pointer, so it covers the second cause as well.

Usage:
    scripts/quake_focus.py              # activate + always-on-top
    scripts/quake_focus.py --off        # drop always-on-top
    scripts/quake_focus.py --no-pin     # only activate, do not pin
    scripts/quake_focus.py --grab       # ...and click into the window once, so
                                        # the frontend grabs the pointer
    scripts/quake_focus.py --list       # list candidate windows and exit

Needs X11 (libX11 via ctypes) and a running $DISPLAY.  No python-xlib needed.
--grab additionally needs libXtst.so.6; without it the flag reports that and
does everything else.  Release the pointer back to the host with Ctrl+Alt+G.
"""

import ctypes
import os
import sys
from ctypes import (POINTER, Structure, byref, c_bool, c_char_p, c_int, c_long,
                    c_uint, c_ulong, c_void_p, create_string_buffer)

NET_WM_STATE_REMOVE = 0
NET_WM_STATE_ADD = 1
SUBSTRUCTURE_NOTIFY = 1 << 19
SUBSTRUCTURE_REDIRECT = 1 << 20
CLIENT_MESSAGE = 33


class XClientMessageEvent(Structure):
    _fields_ = [
        ("type", c_int),
        ("serial", c_ulong),
        ("send_event", c_int),
        ("display", c_void_p),
        ("window", c_ulong),
        ("message_type", c_ulong),
        ("format", c_int),
        ("data", c_long * 5),
    ]


class XClassHint(Structure):
    _fields_ = [("res_name", c_char_p), ("res_class", c_char_p)]


def load_x11():
    xl = ctypes.CDLL("libX11.so.6")
    xl.XOpenDisplay.restype = c_void_p
    xl.XOpenDisplay.argtypes = [c_char_p]
    xl.XDefaultRootWindow.restype = c_ulong
    xl.XDefaultRootWindow.argtypes = [c_void_p]
    xl.XInternAtom.restype = c_ulong
    xl.XInternAtom.argtypes = [c_void_p, c_char_p, c_bool]
    xl.XQueryTree.argtypes = [c_void_p, c_ulong, POINTER(c_ulong),
                              POINTER(c_ulong), POINTER(POINTER(c_ulong)),
                              POINTER(c_uint)]
    xl.XGetClassHint.argtypes = [c_void_p, c_ulong, POINTER(XClassHint)]
    xl.XRaiseWindow.argtypes = [c_void_p, c_ulong]
    xl.XSendEvent.argtypes = [c_void_p, c_ulong, c_bool, c_long, c_void_p]
    xl.XFlush.argtypes = [c_void_p]
    xl.XSync.argtypes = [c_void_p, c_bool]
    return xl


def load_xtst():
    """XTest, for the one click --grab needs. Returns None when libXtst is not
    installed; the caller treats that as "everything else still works"."""
    try:
        xt = ctypes.CDLL("libXtst.so.6")
    except OSError:
        return None
    xt.XTestFakeMotionEvent.argtypes = [c_void_p, c_int, c_int, c_int, c_ulong]
    xt.XTestFakeButtonEvent.argtypes = [c_void_p, c_uint, c_bool, c_ulong]
    return xt


def window_rect_on_screen(xl, dpy, root, win):
    """(x, y, w, h) of `win` in root coordinates, or None.

    XGetGeometry gives the position RELATIVE TO THE PARENT (the WM frame), so
    the origin is translated through XTranslateCoordinates rather than trusted
    - getting that wrong would put the synthetic click somewhere else on the
desktop.  Scalars only, on purpose: a ctypes XWindowAttributes has to match
    the C struct field for field, and a mismatch here would corrupt memory."""
    root_ret = c_ulong()
    x = c_int()
    y = c_int()
    w = c_uint()
    h = c_uint()
    bw = c_uint()
    depth = c_uint()
    xl.XGetGeometry.argtypes = [c_void_p, c_ulong, POINTER(c_ulong), POINTER(c_int),
                                POINTER(c_int), POINTER(c_uint), POINTER(c_uint),
                                POINTER(c_uint), POINTER(c_uint)]
    if not xl.XGetGeometry(dpy, win, byref(root_ret), byref(x), byref(y),
                           byref(w), byref(h), byref(bw), byref(depth)):
        return None
    dx = c_int()
    dy = c_int()
    child = c_ulong()
    xl.XTranslateCoordinates.argtypes = [c_void_p, c_ulong, c_ulong, c_int, c_int,
                                         POINTER(c_int), POINTER(c_int),
                                         POINTER(c_ulong)]
    if not xl.XTranslateCoordinates(dpy, win, root, 0, 0, byref(dx), byref(dy),
                                    byref(child)):
        return None
    return (dx.value, dy.value, int(w.value), int(h.value))


def synthetic_grab_click(xl, dpy, root, client):
    """One press+release at the centre of `client`, through XTest.

    Fake motion first so the click lands inside the window even if the pointer
    was parked on another monitor; then button 1 down and up. QEMU's frontend
    takes the press to START THE POINTER GRAB (see this file's docstring) and
    does not forward it to the guest, and the WM focuses the window as a side
    effect of the click.

    Deliberately NOT repeated by the caller's focus loop: a click that reaches
    the guest is "fire" plus "take the controls", so one per invocation is the
    whole contract."""
    xt = load_xtst()
    if xt is None:
        sys.stderr.write("quake_focus: --grab needs libXtst.so.6 (not "
                         "installed); skipping the click\n")
        return False
    rect = window_rect_on_screen(xl, dpy, root, client)
    if rect is None:
        sys.stderr.write("quake_focus: cannot read the window geometry for "
                         "the grab click\n")
        return False
    wx, wy, ww, wh = rect
    cx, cy = wx + ww // 2, wy + wh // 2
    default_screen = xl.XDefaultScreen(dpy) if hasattr(xl, "XDefaultScreen") else 0
    xt.XTestFakeMotionEvent(dpy, int(default_screen), cx, cy, 0)
    xl.XFlush(dpy)
    xt.XTestFakeButtonEvent(dpy, 1, True, 0)
    xt.XTestFakeButtonEvent(dpy, 1, False, 0)
    xl.XSync(dpy, False)
    print("quake_focus: clicked centre of 0x%x at (%d,%d) to start the pointer "
          "grab (release it with Ctrl+Alt+G)" % (client, cx, cy))
    return True


def children_of(xl, dpy, win):
    root = c_ulong()
    parent = c_ulong()
    kids = POINTER(c_ulong)()
    n = c_uint()
    if not xl.XQueryTree(dpy, win, byref(root), byref(parent), byref(kids), byref(n)):
        return []
    out = [kids[i] for i in range(n.value)]
    return out


def class_of(xl, dpy, win):
    hint = XClassHint()
    if not xl.XGetClassHint(dpy, win, byref(hint)):
        return ""
    parts = []
    for p in (hint.res_name, hint.res_class):
        if p:
            parts.append(p.decode("utf-8", "replace"))
    return " ".join(parts)


def find_qemu(xl, dpy, root):
    """Return [(client, frame, class_string), ...] for every QEMU window."""
    found = []
    for top in children_of(xl, dpy, root):
        for cand in [top] + children_of(xl, dpy, top):
            cls = class_of(xl, dpy, cand)
            if "qemu" in cls.lower():
                found.append((cand, top, cls))
    return found


def send_client_message(xl, dpy, root, win, atom, data):
    buf = create_string_buffer(192)
    ev = XClientMessageEvent.from_buffer(buf)
    ev.type = CLIENT_MESSAGE
    ev.serial = 0
    ev.send_event = 1
    ev.display = dpy
    ev.window = win
    ev.message_type = atom
    ev.format = 32
    for i, v in enumerate(data[:5]):
        ev.data[i] = v
    mask = SUBSTRUCTURE_NOTIFY | SUBSTRUCTURE_REDIRECT
    xl.XSendEvent(dpy, root, False, mask, ctypes.cast(buf, c_void_p))


def main(argv):
    pin = True
    do_list = False
    do_grab = False
    for a in argv[1:]:
        if a == "--off":
            pin = False
        elif a == "--no-pin":
            pin = False
        elif a == "--pin":
            pin = True
        elif a == "--grab":
            do_grab = True
        elif a == "--list":
            do_list = True
        elif a in ("-h", "--help"):
            print(__doc__.strip())
            return 0
        else:
            sys.stderr.write("quake_focus: unknown option %r\n" % a)
            return 2

    display = os.environ.get("DISPLAY", ":0.0")
    xl = load_x11()
    dpy = ctypes.c_void_p(xl.XOpenDisplay(display.encode()))
    if not dpy:
        sys.stderr.write("quake_focus: cannot open display %s\n" % display)
        return 1
    root = xl.XDefaultRootWindow(dpy)

    wins = find_qemu(xl, dpy, root)
    if not wins:
        sys.stderr.write("quake_focus: no QEMU window on %s "
                         "(is the VM running?)\n" % display)
        return 1
    if do_list:
        for client, frame, cls in wins:
            print("client=0x%x frame=0x%x class=%s" % (client, frame, cls))
        return 0

    atom_active = xl.XInternAtom(dpy, b"_NET_ACTIVE_WINDOW", False)
    atom_state = xl.XInternAtom(dpy, b"_NET_WM_STATE", False)
    atom_above = xl.XInternAtom(dpy, b"_NET_WM_STATE_ABOVE", False)
    action = NET_WM_STATE_ADD if pin else NET_WM_STATE_REMOVE

    for client, frame, cls in wins:
        # ask the WM to focus + raise it (this is what clicking the taskbar does)
        send_client_message(xl, dpy, root, client, atom_active, [1, 0, 0, 0, 0])
        # pin it above other windows (or unpin), source indication = 1
        send_client_message(xl, dpy, root, client, atom_state,
                            [action, atom_above, 0, 1, 0])
        # and raise the WM frame directly, in case the WM restacks lazily
        xl.XRaiseWindow(dpy, frame)
        xl.XFlush(dpy)
        print("quake_focus: 0x%x (%s) %s" %
              (client, cls, "activated + always-on-top" if pin
               else "activated, always-on-top removed"))
    xl.XSync(dpy, False)
    # v38.149: the click goes to ONE window (the first candidate), after the
    # activation loop has already raised and focused every candidate. Sending a
    # press into each of them would be a burst of clicks nobody asked for.
    if do_grab:
        synthetic_grab_click(xl, dpy, root, wins[0][0])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
