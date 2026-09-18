#!/usr/bin/env python3
"""Mectov OS desktop icon generator.

Designs the desktop icons as *vector* shapes and rasterizes them with analytic
signed-distance-field coverage, so edges come out properly anti-aliased instead
of the blocky pixel-art the OS shipped before.

Emits two files:

  src/include/icon_glyphs.h   32x32 4-bit coverage masks (two layers) + the
                              tile palette, consumed by src/gui/desktop.c
  scripts/icon_preview.html   a faithful JS port of the C renderer (same
                              integer math, same tables) so the preview shows
                              exactly what the kernel draws

Usage:  python3 scripts/gen_icons.py
"""

import math
import os

# ---------------------------------------------------------------------------
# Canvas / tile geometry. These constants are emitted into the header and used
# by both the C renderer and the JS preview, so all three stay in lockstep.
# ---------------------------------------------------------------------------
GLYPH_PX = 32          # glyph mask resolution
TILE_PX = 44           # tile edge in framebuffer pixels
TILE_R = 11            # tile corner radius
GLYPH_OFF = (TILE_PX - GLYPH_PX) // 2   # glyph inset inside the tile

# ---------------------------------------------------------------------------
# Palette. One coherent family: every tile is a 500->700 style vertical
# gradient with matched saturation/luminance, so the desktop reads as a set
# instead of twelve unrelated loud squares.
# ---------------------------------------------------------------------------
PALETTE = [
    # label      top        bottom
    ("Terminal", 0x3A424E, 0x232A33),   # graphite
    ("Browser",  0x3B82F6, 0x1D4ED8),   # azure
    ("Explorer", 0xF5A623, 0xC1770B),   # amber
    ("SysInfo",  0x22B8CF, 0x0E7490),   # cyan
    ("Clock",    0x14B8A6, 0x0F766E),   # teal
    ("PCI",      0x8B5CF6, 0x6D28D9),   # violet
    ("Snake",    0x2BC55E, 0x15803D),   # green
    ("Calc",     0x6366F1, 0x4338CA),   # indigo
    ("Task Mgr", 0x64748B, 0x334155),   # slate
    ("Flappy",   0xFB8C3C, 0xC2410C),   # orange
    ("Notepad",  0xEC5AA0, 0xBE185D),   # pink
    ("ELF Demo", 0xF0524B, 0xB91C1C),   # red
]


# ---------------------------------------------------------------------------
# Signed distance functions (32x32 grid, y grows downward, pixel units)
# ---------------------------------------------------------------------------
def _clamp(v, lo, hi):
    return lo if v < lo else (hi if v > hi else v)


def sd_rrect(px, py, cx, cy, hw, hh, r):
    qx = abs(px - cx) - (hw - r)
    qy = abs(py - cy) - (hh - r)
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r


def sd_circle(px, py, cx, cy, r):
    return math.hypot(px - cx, py - cy) - r


def sd_seg(px, py, x1, y1, x2, y2, w):
    dx, dy = x2 - x1, y2 - y1
    ll = dx * dx + dy * dy
    t = 0.0 if ll == 0 else _clamp(((px - x1) * dx + (py - y1) * dy) / ll, 0.0, 1.0)
    return math.hypot(px - (x1 + t * dx), py - (y1 + t * dy)) - w * 0.5


def sd_ring(px, py, cx, cy, r, w):
    return abs(math.hypot(px - cx, py - cy) - r) - w * 0.5


def sd_rrect_ring(px, py, cx, cy, hw, hh, r, t):
    return abs(sd_rrect(px, py, cx, cy, hw, hh, r)) - t * 0.5


def sd_ellipse_ring(px, py, cx, cy, rx, ry, t):
    # Normalized radial distance, rescaled by the smaller radius: good enough
    # for a globe meridian where rx >> ry never happens.
    d = math.hypot((px - cx) / rx, (py - cy) / ry)
    return abs(d - 1.0) * min(rx, ry) - t * 0.5


def sd_tri(px, py, pts, round_r):
    # Intersection of three half-planes = triangle; normals are flipped to
    # point away from the centroid so the sign convention matches the rest.
    cxm = sum(p[0] for p in pts) / 3.0
    cym = sum(p[1] for p in pts) / 3.0
    d = -1e9
    for i in range(3):
        ax, ay = pts[i]
        bx, by = pts[(i + 1) % 3]
        nx, ny = by - ay, -(bx - ax)          # left normal
        ln = math.hypot(nx, ny) or 1.0
        nx, ny = nx / ln, ny / ln
        if (cxm - ax) * nx + (cym - ay) * ny > 0:
            nx, ny = -nx, -ny
        d = max(d, (px - ax) * nx + (py - ay) * ny)
    return d - round_r


# Shape constructors (readable design table below).
def RR(cx, cy, hw, hh, r):
    return ("rr", cx, cy, hw, hh, r)


def C(cx, cy, r):
    return ("c", cx, cy, r)


def S(x1, y1, x2, y2, w):
    return ("s", x1, y1, x2, y2, w)


def RING(cx, cy, r, w):
    return ("ring", cx, cy, r, w)


def FRAME(cx, cy, hw, hh, r, t):
    return ("frame", cx, cy, hw, hh, r, t)


def ERING(cx, cy, rx, ry, t):
    return ("ering", cx, cy, rx, ry, t)


def TRI(pts, r):
    return ("tri", pts, r)


def sd_shape(sh, px, py):
    k = sh[0]
    if k == "rr":
        _, cx, cy, hw, hh, r = sh
        return sd_rrect(px, py, cx, cy, hw, hh, r)
    if k == "c":
        _, cx, cy, r = sh
        return sd_circle(px, py, cx, cy, r)
    if k == "s":
        _, x1, y1, x2, y2, w = sh
        return sd_seg(px, py, x1, y1, x2, y2, w)
    if k == "ring":
        _, cx, cy, r, w = sh
        return sd_ring(px, py, cx, cy, r, w)
    if k == "frame":
        _, cx, cy, hw, hh, r, t = sh
        return sd_rrect_ring(px, py, cx, cy, hw, hh, r, t)
    if k == "ering":
        _, cx, cy, rx, ry, t = sh
        return sd_ellipse_ring(px, py, cx, cy, rx, ry, t)
    if k == "tri":
        _, pts, r = sh
        return sd_tri(px, py, pts, r)
    raise ValueError("unknown shape " + k)


# ---------------------------------------------------------------------------
# Glyph designs. Each entry is a list of (op, layer, shape):
#   op    "add" fills, "sub" cuts away (shows the tile through the glyph)
#   layer 1 = bright white, 2 = 55% white (secondary/dimmer element)
# Everything is designed on the 32x32 grid with a ~4px margin.
# ---------------------------------------------------------------------------
GLYPHS = {
    "Terminal": [
        ("add", 1, FRAME(16, 16, 12.5, 10.5, 3.5, 2.2)),
        ("add", 1, S(5.0, 11.0, 27.0, 11.0, 2.0)),      # title bar separator
        ("add", 1, S(9.5, 14.6, 13.5, 18.6, 2.2)),      # '>'
        ("add", 1, S(13.5, 18.6, 9.5, 22.6, 2.2)),
        ("add", 1, S(17.0, 22.6, 22.0, 22.6, 2.2)),     # '_'
    ],
    "Browser": [
        ("add", 1, RING(16, 16, 12.2, 2.0)),
        ("add", 1, ERING(16, 16, 6.2, 12.2, 1.7)),      # meridian
        ("add", 1, S(4.2, 16.0, 27.8, 16.0, 1.7)),      # equator
    ],
    "Explorer": [
        ("add", 2, RR(11.4, 10.4, 7.4, 3.0, 1.6)),     # folder tab (behind, flush left)
        ("add", 1, RR(16.0, 19.6, 12.0, 6.8, 2.6)),     # folder body
    ],
    "SysInfo": [
        ("add", 1, FRAME(16, 13.5, 12.0, 8.5, 2.6, 2.1)),
        ("add", 1, S(10.0, 19.2, 10.0, 14.8, 2.4)),     # chart bars
        ("add", 1, S(16.0, 19.2, 16.0, 11.6, 2.4)),
        ("add", 1, S(22.0, 19.2, 22.0, 16.4, 2.4)),
        ("add", 1, S(16.0, 22.0, 16.0, 25.4, 2.4)),     # stand
        ("add", 1, S(11.5, 26.6, 20.5, 26.6, 2.6)),     # foot
    ],
    "Clock": [
        ("add", 1, RING(16, 16, 11.2, 2.3)),
        ("add", 1, S(16.0, 16.0, 16.0, 7.4, 2.4)),      # minute hand: straight up
        ("add", 1, S(16.0, 16.0, 21.6, 20.6, 2.6)),     # hour hand: down-right
        ("add", 1, C(16.0, 16.0, 2.1)),                 # centre cap
    ],
    "PCI": [
        ("add", 2, FRAME(16.0, 16.0, 12.0, 9.5, 2.4, 2.0)),   # board outline
        ("add", 1, RR(16.0, 16.0, 5.0, 5.0, 1.4)),      # chip
        ("add", 1, S(9.5, 16.0, 5.6, 16.0, 1.6)),        # traces
        ("add", 1, S(22.5, 16.0, 26.4, 16.0, 1.6)),
        ("add", 1, S(16.0, 9.5, 16.0, 5.6, 1.6)),
        ("add", 1, S(16.0, 22.5, 16.0, 26.4, 1.6)),
        ("add", 1, C(7.6, 10.2, 1.3)),                   # vias
        ("add", 1, C(24.4, 21.8, 1.3)),
    ],
    "Snake": [
        ("add", 1, S(11.0, 22.5, 11.0, 13.0, 5.4)),     # body
        ("add", 1, S(11.0, 13.0, 20.0, 13.0, 5.4)),
        ("add", 1, C(21.8, 13.0, 3.5)),                 # head
        ("sub", 1, C(23.3, 11.6, 1.25)),                # eye
        ("add", 2, C(22.4, 22.6, 2.4)),                 # food
    ],
    "Calc": [
        ("add", 2, RR(16.0, 16.0, 9.5, 11.0, 3.0)),     # shell
        ("add", 1, RR(16.0, 10.0, 6.5, 2.6, 1.0)),      # display
        ("add", 1, C(11.5, 16.0, 1.7)), ("add", 1, C(16.0, 16.0, 1.7)),
        ("add", 1, C(20.5, 16.0, 1.7)), ("add", 1, C(11.5, 20.5, 1.7)),
        ("add", 1, C(16.0, 20.5, 1.7)), ("add", 1, C(20.5, 20.5, 1.7)),
        ("add", 1, C(11.5, 25.0, 1.7)), ("add", 1, C(16.0, 25.0, 1.7)),
        ("add", 1, C(20.5, 25.0, 1.7)),
    ],
    "Task Mgr": [
        ("add", 1, C(8.0, 11.0, 2.0)),
        ("add", 1, S(13.5, 11.0, 24.5, 11.0, 2.4)),
        ("add", 1, C(8.0, 18.0, 2.0)),
        ("add", 1, S(13.5, 18.0, 21.5, 18.0, 2.4)),
        ("add", 1, C(8.0, 25.0, 2.0)),
        ("add", 1, S(13.5, 25.0, 18.5, 25.0, 2.4)),
    ],
    "Flappy": [
        ("add", 1, C(14.5, 17.0, 8.0)),                 # body
        ("add", 1, TRI([(21.4, 14.6), (21.4, 20.0), (28.6, 17.3)], 1.2)),  # beak
        ("sub", 1, RR(11.6, 20.3, 4.4, 2.0, 1.6)),      # wing
        ("sub", 1, C(18.9, 13.5, 2.0)),                 # eye
    ],
    "Notepad": [
        ("add", 1, RR(15.5, 16.5, 9.5, 11.0, 2.6)),     # page
        ("sub", 1, C(25.8, 5.2, 4.8)),                  # folded corner
        ("sub", 1, S(10.0, 12.8, 21.0, 12.8, 1.6)),     # text lines
        ("sub", 1, S(10.0, 16.8, 21.0, 16.8, 1.6)),
        ("sub", 1, S(10.0, 20.8, 21.0, 20.8, 1.6)),
        ("sub", 1, S(10.0, 24.8, 16.5, 24.8, 1.6)),
    ],
    "ELF Demo": [
        ("add", 1, FRAME(16, 16, 12.0, 12.0, 4.6, 2.2)),
        ("add", 1, TRI([(13.4, 10.4), (13.4, 21.6), (22.4, 16.0)], 1.3)),
    ],
    "generic": [
        ("add", 1, FRAME(16, 15.5, 12.0, 10.0, 2.6, 2.0)),
        ("add", 1, S(4.4, 10.6, 27.6, 10.6, 1.8)),
    ],
}


def rasterize_cover(shapes):
    """Rasterize to two 32x32 coverage grids (0..15), one per layer.

    Coverage comes straight from the signed distance (cov = 0.5 - d clamped),
    which is the standard analytic anti-aliasing approximation: no supersampling
    and no polygon scan conversion needed, and the result is identical in the
    C renderer's preview port.
    """
    n = GLYPH_PX
    out = {1: [[0] * n for _ in range(n)], 2: [[0] * n for _ in range(n)]}
    for y in range(n):
        py = y + 0.5
        for x in range(n):
            px = x + 0.5
            adds = {1: None, 2: None}
            subs = {1: None, 2: None}
            for op, layer, sh in shapes:
                d = sd_shape(sh, px, py)
                tgt = adds if op == "add" else subs
                if tgt[layer] is None or d < tgt[layer]:
                    tgt[layer] = d
            for layer in (1, 2):
                a = adds[layer]
                if a is None:
                    continue
                cov = _clamp(0.5 - a, 0.0, 1.0)
                s = subs[layer]
                if s is not None:
                    cov = _clamp(cov - _clamp(0.5 - s, 0.0, 1.0), 0.0, 1.0)
                out[layer][y][x] = int(round(cov * 15.0))
    return out


def pack_mask(grid):
    """4 bits per pixel, low nibble = even x (matches the C reader)."""
    n = GLYPH_PX
    rows = []
    for y in range(n):
        row = grid[y]
        bs = []
        for xb in range(0, n, 2):
            bs.append((row[xb] & 0x0F) | ((row[xb + 1] & 0x0F) << 4))
        rows.append(bs)
    return rows


def emit_header(path, glyphs):
    out = []
    out.append("// src/include/icon_glyphs.h — 32x32 anti-aliased desktop icon")
    out.append("// glyphs + tile palette (generated).")
    out.append("//")
    out.append("// GENERATED by scripts/gen_icons.py — do not hand-edit. The shapes")
    out.append("// live in that script as vector SDFs; it rasterizes them with analytic")
    out.append("// coverage so edges are properly anti-aliased, then emits both this")
    out.append("// header and scripts/icon_preview.html (a faithful JS port of the C")
    out.append("// renderer in src/gui/desktop.c).")
    out.append("//")
    out.append("// Mask encoding: 4 bits of coverage (0..15) per pixel, two pixels per")
    out.append("// byte (low nibble = even x), 16 bytes per row, 32 rows per layer.")
    out.append("// Layer 1 = bright, layer 2 = 55% white (secondary element).")
    out.append("#ifndef ICON_GLYPHS_H")
    out.append("#define ICON_GLYPHS_H")
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("#define ICON_TILE_PX   %d" % TILE_PX)
    out.append("#define ICON_TILE_R    %d" % TILE_R)
    out.append("#define ICON_GLYPH_PX  %d" % GLYPH_PX)
    out.append("#define ICON_GLYPH_OFF %d" % GLYPH_OFF)
    out.append("")
    for name in glyphs:
        out.append("// %s" % name)
    out.append("")
    for name in glyphs:
        hi, mid = glyphs[name]
        key = name.lower().replace(" ", "_")
        for tag, grid in (("hi", hi), ("mid", mid)):
            out.append("static const uint8_t g_%s_%s[%d*%d] = {" % (key, tag, GLYPH_PX, GLYPH_PX // 2))
            for row in pack_mask(grid):
                out.append("    " + " ".join("0x%02X," % b for b in row))
            out.append("};")
        out.append("")
    out.append("typedef struct {")
    out.append("    const char* label;")
    out.append("    uint32_t    top;     // tile gradient, top")
    out.append("    uint32_t    bottom;  // tile gradient, bottom")
    out.append("    const uint8_t* hi;   // bright glyph layer")
    out.append("    const uint8_t* mid;  // secondary glyph layer")
    out.append("} icon_glyph_t;")
    out.append("")
    out.append("static const icon_glyph_t icon_glyphs[] = {")
    for label, top, bot in PALETTE:
        key = label.lower().replace(" ", "_")
        out.append('    { "%s", 0x%06X, 0x%06X, g_%s_hi, g_%s_mid },' % (label, top, bot, key, key))
    out.append('    { 0, 0x3A424E, 0x232A33, g_generic_hi, g_generic_mid },' )
    out.append("};")
    out.append("")
    out.append("#define ICON_GLYPH_COUNT ((int)(sizeof(icon_glyphs)/sizeof(icon_glyphs[0])))")
    out.append("")
    out.append("#endif")
    out.append("")
    with open(path, "w") as f:
        f.write("\n".join(out))


PREVIEW_TEMPLATE = r"""<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Mectov Icon Preview</title>
<style>
  body { background:#20242b; color:#dde1e7; font-family:sans-serif; padding:18px; }
  h3 { margin:22px 0 8px; font-size:13px; color:#9aa3ad; text-transform:uppercase; letter-spacing:.06em; }
  .row { display:flex; gap:34px; flex-wrap:wrap; align-items:flex-start; }
  .cell { text-align:center; }
  canvas { display:block; image-rendering:pixelated; }
  .lbl { font-size:11px; color:#8b949e; margin-top:6px; }
  .wall { background:linear-gradient(160deg,#2b3a4a 0%,#3d4a5c 45%,#6b5f52 100%); padding:20px; border-radius:10px; }
  .wall2 { background:#dfe4ea; padding:20px; border-radius:10px; }
  .grid { display:flex; gap:26px; flex-wrap:wrap; }
</style>
</head>
<body>
<h3>Desktop wallpaper (1x &mdash; actual framebuffer pixels)</h3>
<div class="wall"><div class="grid" id="wallgrid"></div></div>

<h3>Light background &mdash; edge definition</h3>
<div class="wall2"><div class="grid" id="lightgrid"></div></div>

<h3>Zoom &mdash; 6x, anti-aliasing / bevel detail</h3>
<div id="grid"></div>

<script>
const TILE = __TILE__, RAD = __RAD__, GPX = __GPX__, GOFF = __GOFF__;
const PALETTE = __PALETTE__;

// ---- integer helpers: identical to src/gui/desktop.c ----
function isqrt32(v) {           // floor(sqrt(v)), v > 0
  // The seed must be an EVEN power of two (the loop halves r each step).
  let r = 0, bit = 1 << 14;
  while (bit > v) bit >>= 2;
  while (bit) {
    if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
    else r >>= 1;
    bit >>= 2;
  }
  return r;
}
// Left edge of the rounded rect at row dy, in 1/16 pixel units.
function edge16(dy, r) {
  const r16 = 16 * r;
  const dy16 = r16 - (16 * dy + 8);
  return r16 - isqrt32(r16 * r16 - dy16 * dy16);
}
function div255(t) { return ((t + 128) * 257) >> 16; }
function blendPx(buf, w, x, y, col, a) {
  if (a <= 0) return;
  if (a > 255) a = 255;
  const i = (y * w + x) * 4;
  const ia = 255 - a;
  buf[i]     = div255(buf[i]     * ia + ((col >> 16) & 255) * a);
  buf[i + 1] = div255(buf[i + 1] * ia + ((col >> 8) & 255) * a);
  buf[i + 2] = div255(buf[i + 2] * ia + (col & 255) * a);
  buf[i + 3] = 255;
}
function fillRow(buf, w, x, y, len, col) {
  for (let i = 0; i < len; i++) {
    const o = (y * w + x + i) * 4;
    buf[o] = (col >> 16) & 255; buf[o + 1] = (col >> 8) & 255; buf[o + 2] = col & 255; buf[o + 3] = 255;
  }
}
function mix(a, b, num, den) {   // per channel: (a*(den-num) + b*num) / den
  const ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
  const br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
  const k = den - num;
  const r = ((ar * k + br * num) / den) | 0;
  const g = ((ag * k + bg * num) / den) | 0;
  const bl = ((ab * k + bb * num) / den) | 0;
  return (r << 16) | (g << 8) | bl;
}
// ---- tile: vertical gradient + 2px bevel, anti-aliased rounded corners ----
function drawTile(buf, w, ox, oy, top, bot) {
  for (let y = 0; y < TILE; y++) {
    const t = Math.floor((y * 255) / (TILE - 1));
    let col = mix(top, bot, t, 255);
    if (y === 0) col = mix(col, 0xFFFFFF, 38, 255);
    else if (y === 1) col = mix(col, 0xFFFFFF, 15, 255);
    if (y === TILE - 1) col = mix(col, 0x000000, 42, 255);
    else if (y === TILE - 2) col = mix(col, 0x000000, 17, 255);
    const dy = y < RAD ? y : (y >= TILE - RAD ? TILE - 1 - y : RAD);
    let e = 0;
    if (y < RAD || y >= TILE - RAD) e = edge16(dy, RAD);
    const lx = Math.floor(e / 16), lc = 16 - (e & 15);
    const x0 = Math.floor((e + 15) / 16), x1 = TILE - x0;
    if (x1 > x0) fillRow(buf, w, ox + x0, oy + y, x1 - x0, col);
    if (e & 15) {
      blendPx(buf, w, ox + lx, oy + y, col, lc * 16);
      blendPx(buf, w, ox + TILE - 1 - lx, oy + y, col, lc * 16);
    }
  }
}
// ---- glyph: 4-bit coverage masks, two layers, source-over ----
function nib(m, off, x) { const b = m[off + (x >> 1)]; return (x & 1) ? (b >> 4) : (b & 15); }
function drawGlyph(buf, w, ox, oy, hi, mid) {
  for (let y = 0; y < GPX; y++) {
    const off = y * (GPX >> 1);
    for (let x = 0; x < GPX; x++) {
      const a1 = nib(hi, off, x), a2 = nib(mid, off, x);
      if (!a1 && !a2) continue;
      const a = a1 ? a1 * 17 : Math.floor(a2 * 143 / 15);
      blendPx(buf, w, ox + x, oy + y, 0xF4F7FA, a);
    }
  }
}
function decode(hex) {
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16);
  return out;
}
function renderIcon(entry) {
  const w = TILE, buf = new Uint8ClampedArray(w * TILE * 4);
  drawTile(buf, w, 0, 0, entry.top, entry.bot);
  drawGlyph(buf, w, GOFF, GOFF, decode(entry.hi), decode(entry.mid));
  return buf;
}
function toCanvas(buf, scale) {
  const w = TILE, tmp = document.createElement('canvas');
  tmp.width = w; tmp.height = w;
  tmp.getContext('2d').putImageData(new ImageData(buf, w, w), 0, 0);
  const c = document.createElement('canvas');
  c.width = w * scale; c.height = w * scale;
  const ctx = c.getContext('2d');
  ctx.imageSmoothingEnabled = false;
  ctx.drawImage(tmp, 0, 0, w * scale, w * scale);
  return c;
}
function cell(entry, scale, showLabel) {
  const d = document.createElement('div'); d.className = 'cell';
  d.appendChild(toCanvas(renderIcon(entry), scale));
  if (showLabel) {
    const l = document.createElement('div'); l.className = 'lbl';
    l.textContent = entry.label; d.appendChild(l);
  }
  return d;
}
for (const e of PALETTE) {
  document.getElementById('wallgrid').appendChild(cell(e, 1, true));
  document.getElementById('lightgrid').appendChild(cell(e, 1, true));
  document.getElementById('grid').appendChild(cell(e, 6, true));
}
</script>
</body>
</html>
"""


def emit_preview(path, glyphs):
    masks = {}
    for name in glyphs:
        hi, mid = glyphs[name]
        key = name.lower().replace(" ", "_")
        masks[key] = {
            "hi": "".join("%02x" % b for row in pack_mask(hi) for b in row),
            "mid": "".join("%02x" % b for row in pack_mask(mid) for b in row),
        }
    pal = []
    for label, top, bot in PALETTE:
        key = label.lower().replace(" ", "_")
        pal.append('{label:%s,top:0x%06X,bot:0x%06X,hi:"%s",mid:"%s"}'
                   % (jsstr(label), top, bot, masks[key]["hi"], masks[key]["mid"]))
    html = PREVIEW_TEMPLATE
    html = html.replace("__TILE__", str(TILE_PX))
    html = html.replace("__RAD__", str(TILE_R))
    html = html.replace("__GPX__", str(GLYPH_PX))
    html = html.replace("__GOFF__", str(GLYPH_OFF))
    html = html.replace("__PALETTE__", "[\n" + ",\n".join(pal) + "\n]")
    with open(path, "w") as f:
        f.write(html)


def jsstr(s):
    return '"' + s.replace('"', '\\"') + '"'


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    glyphs = {}
    for name, shapes in GLYPHS.items():
        cov = rasterize_cover(shapes)
        glyphs[name] = (cov[1], cov[2])
    emit_header(os.path.join(root, "src/include/icon_glyphs.h"), glyphs)
    emit_preview(os.path.join(root, "scripts/icon_preview.html"), glyphs)
    print("wrote %d glyphs -> src/include/icon_glyphs.h + scripts/icon_preview.html" % len(glyphs))


if __name__ == "__main__":
    main()
