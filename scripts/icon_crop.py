#!/usr/bin/env python3
"""Crop the desktop screenshot the icon suite cares about, and write an HTML
viewer so the result can be eyeballed (and screenshotted) without a GUI image
viewer. Usage: python3 scripts/icon_crop.py [ppm] [out.png]"""
import os, struct, sys, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    assert parts[0].strip() == b"P6", parts[0]
    w, h = map(int, parts[1].split())
    px = parts[3] if len(parts[3]) == w * h * 3 else parts[2] + b"\n" + parts[3]
    return w, h, px


def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    hdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", hdr)
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    ppm = sys.argv[1] if len(sys.argv) > 1 else "/tmp/mectov_desktop.ppm"
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/Pictures/icons_v3895.png")
    w, h, px = read_ppm(ppm)
    print("source %dx%d" % (w, h))
    cw, ch = min(560, w), min(470, h)
    rows = []
    for y in range(ch):
        base = (y * w) * 3
        rows.append(bytes(px[base:base + cw * 3]))
    write_png(out, cw, ch, rows)
    print("wrote %s (%dx%d)" % (out, cw, ch))


if __name__ == "__main__":
    main()
