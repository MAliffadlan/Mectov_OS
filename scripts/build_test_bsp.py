#!/usr/bin/env python3
"""scripts/build_test_bsp.py — write the free test arena as a Quake III .bsp (v38.107).

Why this exists
---------------
v38.107 teaches the port to use id's OWN collision model (``CM_LoadMap`` /
``CM_BoxTrace``) instead of the hand-written world that only knew a floor plane
at z=0. That code path cannot be tested against a retail map: ``pak0.pk3`` is not
redistributable, so CI has no q3dm1.bsp and never will.

v38.108 adds the other half the same argument applies to: the renderer. The
arena's surfaces now carry real texture coordinates and the four textures they
name are written next to it (``baseq3/textures/mectovtest/*.tga``), so drawing
the level needs decoded image data exactly like a retail map does — while still
shipping no id art. The textures are generated patterns, not photographs, and
their colours are deliberately saturated and mutually distinct so a screendump
can be asserted on: "flat-shaded geometry could not produce this pixel" is what
makes the render test evidence rather than decoration.

So the test arena is *generated*, here, from nothing. It is our own content —
no id assets, no third-party data, a few kilobytes of arithmetic — and it is
deliberately built to make "did the real loader run?" answerable from the serial
log rather than by eyeballing:

  * the floor's top face sits at **z = 64**, not 0. The old fake world put the
    player at exactly z=24; landing at z=88 (64 + the 24-unit stand-off) is a
    fact only real brush collision can produce.
  * the room is walled, so a horizontal trace must STOP INSIDE the map. The fake
    world had one infinite floor plane and returned fraction 1.0 for any
    sideways move, which is precisely the bug this content exists to catch.

Format notes (id's v46 BSP, as parsed by cm_load.c and q3bsp.c)
--------------------------------------------------------------
Everything below is dictated by id's own loader, not invented here:

* ``CMod_LoadShaders`` needs at least one shader; every brush side and surface
  carries a shader index, and the shader's ``contentFlags`` become the brush's
  ``contents`` — so the shaders must claim ``CONTENTS_SOLID`` or nothing blocks.
* ``CM_BoundBrush`` reads a brush's **first six sides as the axial box**, in the
  order -X, +X, -Y, +Y, -Z, +Z, and derives ``bounds`` from their plane
  distances. Every brush here therefore emits those six sides first, exactly.
* Brush planes face OUT of the solid (``n·p = dist``, solid where ``n·p < dist``).
* ``CM_LoadMap`` rejects a map with no shaders/planes/nodes/leafs/models, so all
  five lumps are non-empty even though this arena is one leaf.
* Node children are ``-(leaf + 1)``, so a child of -1 is leaf 0.
* Texture coordinates (``st``) are in texture REPEATS, not texels: q3map divides
  the world coordinate by the texture's width, and so does this script, so one
  tile of a 64x64 texture covers 64 world units.
* The textures themselves are 24-bit uncompressed TGA — the format id's own
  tools emit and the simplest one a decoder can be held to.

The single-leaf tree is honest, not a shortcut: a leaf is just a region, and one
leaf holding every brush is a valid BSP. Point-contents, brush tracing and
``CM_BoxBrushes`` all still work through it (the root node sends every query to
leaf 0 and ``CM_StoreLeafs`` de-duplicates), it just gives up the acceleration
structure. For an arena with six brushes that costs nothing; a real map brings
its own tree from q3map.

Usage:
    scripts/build_test_bsp.py [output_dir]     # default: build/q3data
"""
import os
import struct
import sys

MAX_QPATH = 64
CONTENTS_SOLID = 0x1

# Texture side in texels. Also the world size of one texture tile (see
# face_st): a 64-unit brush face is covered by exactly one repeat, so the
# checkerboard below lines up with the geometry instead of drifting across it.
TEX_SIZE = 64

# Surface types (see qfiles.h). Planar faces are emitted for the renderer that
# arrives in the next phase; the collision loader only looks at MST_PATCH.
MST_PLANAR = 1

LUMP_ENTITIES = 0
LUMP_SHADERS = 1
LUMP_PLANES = 2
LUMP_NODES = 3
LUMP_LEAFS = 4
LUMP_LEAFSURFACES = 5
LUMP_LEAFBRUSHES = 6
LUMP_MODELS = 7
LUMP_BRUSHES = 8
LUMP_BRUSHSIDES = 9
LUMP_DRAWVERTS = 10
LUMP_DRAWINDEXES = 11
LUMP_FOGS = 12
LUMP_SURFACES = 13
LUMP_LIGHTMAPS = 14
LUMP_LIGHTGRID = 15
LUMP_VISIBILITY = 16
HEADER_LUMPS = 17

BSP_IDENT = 0x50534249          # 'IBSP' as a little-endian int32
BSP_VERSION = 46

# --- the arena -------------------------------------------------------------
# Interior: x,y in [-512, 512], z in [64, 256]. The floor is a solid slab whose
# TOP face is z = 64 — the single most useful number in this file.
FLOOR_TOP = 64
CEIL_BOTTOM = 256
HALF = 512
WALL = 64                       # wall thickness, outward from HALF
WALL_BOTTOM = 0

# Shader index -> (name, surfaceFlags, contentFlags). Several names so the
# renderer phase can tint floor/wall/ceiling/step apart without any texture file;
# all of them are solid, which is what makes them collide.
SHADERS = [
    ("textures/mectovtest/floor", 0, CONTENTS_SOLID),
    ("textures/mectovtest/wall", 0, CONTENTS_SOLID),
    ("textures/mectovtest/ceiling", 0, CONTENTS_SOLID),
    ("textures/mectovtest/step", 0, CONTENTS_SOLID),
]
SH_FLOOR, SH_WALL, SH_CEIL, SH_STEP = 0, 1, 2, 3

# Generated textures, one per shader (v38.108). 24-bit RGB rows, top-down; the
# TGA writer below flips them into the file's BGR order. Colours are multiples
# of 8 so the renderer's 5-bit-per-channel texture format is lossless for them.
TEX_FLOOR   = (24, 88, 96)      # teal plate
TEX_FLOOR_B = (8, 56, 64)
TEX_FLOOR_G = (0, 200, 208)     # bright cyan tile seams
TEX_WALL    = (168, 136, 96)    # warm sandstone
TEX_WALL_B  = (128, 96, 64)
TEX_WALL_M  = (72, 56, 40)      # mortar
TEX_CEIL    = (112, 112, 160)   # blue-violet (ambient-lit, so a dark face)
TEX_CEIL_B  = (80, 80, 120)
TEX_CEIL_G  = (176, 176, 216)
TEX_STEP    = (152, 176, 72)    # yellow-green metal
TEX_STEP_B  = (112, 136, 40)
TEX_STEP_G  = (200, 208, 120)

# (min, max, shader)
BRUSHES = [
    ((-HALF, -HALF, FLOOR_TOP - 64), (HALF, HALF, FLOOR_TOP), SH_FLOOR),
    ((-HALF, -HALF, CEIL_BOTTOM), (HALF, HALF, CEIL_BOTTOM + 64), SH_CEIL),
    ((-HALF - WALL, -HALF, WALL_BOTTOM), (-HALF, HALF, CEIL_BOTTOM), SH_WALL),
    ((HALF, -HALF, WALL_BOTTOM), (HALF + WALL, HALF, CEIL_BOTTOM), SH_WALL),
    ((-HALF, -HALF - WALL, WALL_BOTTOM), (HALF, -HALF, CEIL_BOTTOM), SH_WALL),
    ((-HALF, HALF, WALL_BOTTOM), (HALF, HALF + WALL, CEIL_BOTTOM), SH_WALL),
    # A waist-high block off to one side: something to stand on, walk into and
    # (in the renderer phase) see.
    ((128, 128, FLOOR_TOP), (256, 256, FLOOR_TOP + 64), SH_STEP),
]

# Spawn point. z is high enough that the player visibly falls onto the floor
# (it ends at FLOOR_TOP + 24, because the player box's mins[2] is -24).
SPAWN = (-384.0, -384.0, 120.0)

# The probe the driver runs at startup, and the numbers the suite asserts on.
# A point trace from the spawn heading -X must stop on the -X wall's INNER face:
# the face sits at x = -512, the trace starts at -384 and runs 512 units, so the
# hit is at fraction 0.25 with an outward (+X) plane normal.
WALL_PROBE_TO_X = -896.0

MAP_NAME = "mectovtest"


def face_st(normal, corner):
    """Texture coordinates for one corner of one axial face.

    The face's own normal picks the two tangent axes, so a brush's six faces
    tile independently instead of sharing one projection (which is what makes a
    textured box read as a box). One repeat per TEX_SIZE units.
    """
    x, y, z = corner
    ax, ay, az = abs(normal[0]), abs(normal[1]), abs(normal[2])
    if az >= ax and az >= ay:
        u, v = x, y                 # floor / ceiling
    elif ax >= ay:
        u, v = y, z                 # +-X wall
    else:
        u, v = x, z                 # +-Y wall
    return (u / float(TEX_SIZE), v / float(TEX_SIZE))


def f32(x):
    return struct.pack("<f", x)


def i32(x):
    return struct.pack("<i", x)


def shader_record(name, surface_flags, content_flags):
    raw = name.encode("ascii")
    if len(raw) >= MAX_QPATH:
        raise SystemExit("shader name too long: %s" % name)
    return raw + b"\0" * (MAX_QPATH - len(raw)) + i32(surface_flags) + i32(content_flags)


def box_faces(mins, maxs):
    """The six faces of an AABB, wound outward.

    Order matters as much as winding: this is the -X, +X, -Y, +Y, -Z, +Z order
    CM_BoundBrush insists on. Each entry is (plane_normal, plane_dist, corners).
    """
    x0, y0, z0 = mins
    x1, y1, z1 = maxs
    return [
        ((-1.0, 0.0, 0.0), -x0, ((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0))),
        ((1.0, 0.0, 0.0), x1, ((x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1))),
        ((0.0, -1.0, 0.0), -y0, ((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1))),
        ((0.0, 1.0, 0.0), y1, ((x0, y1, z0), (x0, y1, z1), (x1, y1, z1), (x1, y1, z0))),
        ((0.0, 0.0, -1.0), -z0, ((x0, y0, z0), (x0, y1, z0), (x1, y1, z0), (x1, y0, z0))),
        ((0.0, 0.0, 1.0), z1, ((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1))),
    ]


def checker(size, cell, color_a, color_b):
    """size x size RGB rows, alternating square tiles of cell texels."""
    px = bytearray()
    for y in range(size):
        row_b = (y // cell) % 2
        for x in range(size):
            on = ((x // cell) + row_b) % 2 == 0
            px += bytes(color_a if on else color_b)
    return px


def grid_lines(px, size, step, width, color):
    """Overwrite a grid of `width`-texel lines every `step` texels."""
    for y in range(size):
        for x in range(size):
            if (x % step) < width or (y % step) < width:
                o = (y * size + x) * 3
                px[o:o + 3] = bytes(color)


def tga_bytes(px, size):
    """Uncompressed 24-bit TGA, top-down (image descriptor bit 5)."""
    header = struct.pack("<BBBHHBHHHHBB",
                         0, 0, 2, 0, 0, 0, 0, 0, size, size, 24, 0x20)
    body = bytearray()
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        body += bytes((b, g, r))    # TGA stores BGR
    return header + bytes(body)


def texture_files():
    """{shader name: TGA bytes} for every shader in SHADERS."""
    out = {}

    px = checker(TEX_SIZE, 16, TEX_FLOOR, TEX_FLOOR_B)
    grid_lines(px, TEX_SIZE, 16, 2, TEX_FLOOR_G)
    out[SHADERS[SH_FLOOR][0]] = tga_bytes(px, TEX_SIZE)

    px = checker(TEX_SIZE, 16, TEX_WALL, TEX_WALL_B)
    grid_lines(px, TEX_SIZE, 32, 3, TEX_WALL_M)
    out[SHADERS[SH_WALL][0]] = tga_bytes(px, TEX_SIZE)

    px = checker(TEX_SIZE, 8, TEX_CEIL, TEX_CEIL_B)
    grid_lines(px, TEX_SIZE, 32, 2, TEX_CEIL_G)
    out[SHADERS[SH_CEIL][0]] = tga_bytes(px, TEX_SIZE)

    px = checker(TEX_SIZE, 8, TEX_STEP, TEX_STEP_B)
    grid_lines(px, TEX_SIZE, 16, 2, TEX_STEP_G)
    out[SHADERS[SH_STEP][0]] = tga_bytes(px, TEX_SIZE)
    return out


def entity_string():
    world_mins = (-HALF - WALL, -HALF - WALL, WALL_BOTTOM)
    world_maxs = (HALF + WALL, HALF + WALL, CEIL_BOTTOM + 64)
    return (
        "{\n"
        '"classname" "worldspawn"\n'
        '"message" "Mectov Test Arena"\n'
        '"_mectov_generated" "scripts/build_test_bsp.py"\n'
        '"world_mins" "%d %d %d"\n'
        '"world_maxs" "%d %d %d"\n'
        "}\n"
        "{\n"
        '"classname" "info_player_deathmatch"\n'
        '"origin" "%d %d %d"\n'
        '"angle" "45"\n'
        "}\n"
        "{\n"
        '"classname" "light"\n'
        '"origin" "0 0 %d"\n'
        '"light" "300"\n'
        "}\n"
        % (world_mins + world_maxs + (int(SPAWN[0]), int(SPAWN[1]), int(SPAWN[2]), CEIL_BOTTOM - 48))
    ).encode("ascii")


def build():
    lumps = [b""] * HEADER_LUMPS

    # --- planes, brush sides, brushes -------------------------------------
    planes = []          # (normal, dist)
    plane_index = {}

    def plane_num(normal, dist):
        key = (round(normal[0], 6), round(normal[1], 6), round(normal[2], 6), round(dist, 6))
        if key not in plane_index:
            plane_index[key] = len(planes)
            planes.append((normal, dist))
        return plane_index[key]

    brush_records = []
    side_records = []
    for mins, maxs, shader in BRUSHES:
        first_side = len(side_records)
        for normal, dist, _corners in box_faces(mins, maxs):
            side_records.append((plane_num(normal, dist), shader))
        brush_records.append((first_side, 6, shader))

    lumps[LUMP_PLANES] = b"".join(f32(n[0]) + f32(n[1]) + f32(n[2]) + f32(d) for n, d in planes)
    lumps[LUMP_BRUSHSIDES] = b"".join(i32(p) + i32(s) for p, s in side_records)
    lumps[LUMP_BRUSHES] = b"".join(i32(a) + i32(b) + i32(c) for a, b, c in brush_records)

    lumps[LUMP_SHADERS] = b"".join(shader_record(*s) for s in SHADERS)

    # --- renderable planar faces (for the renderer phase) ------------------
    verts = []
    indexes = []
    surfaces = []
    for mins, maxs, shader in BRUSHES:
        for normal, _dist, corners in box_faces(mins, maxs):
            first_vert = len(verts)
            first_index = len(indexes)
            for corner in corners:
                # st: texture repeats (see face_st); lightmap st stays 0 — there
                # are no lightmaps in a generated arena, and the renderer shades
                # each face from its own plane normal.
                verts.append((corner, face_st(normal, corner), (0.0, 0.0),
                              normal, (255, 255, 255, 255)))
            # The draw-index lump stores ABSOLUTE vertex numbers (id's renderer
            # reads them straight into the vertex arrays), so a quad wound
            # v0 v1 v2 / v0 v2 v3 is firstVert + those offsets.
            indexes.extend([first_vert, first_vert + 1, first_vert + 2,
                            first_vert, first_vert + 2, first_vert + 3])
            surfaces.append((shader, -1, MST_PLANAR, first_vert, 4, first_index, 6))

    lumps[LUMP_DRAWVERTS] = b"".join(
        b"".join(f32(v) for v in xyz) + b"".join(f32(v) for v in st)
        + b"".join(f32(v) for v in lightmap) + b"".join(f32(v) for v in normal)
        + bytes(color)
        for xyz, st, lightmap, normal, color in verts
    )
    lumps[LUMP_DRAWINDEXES] = b"".join(i32(i) for i in indexes)
    lumps[LUMP_SURFACES] = b"".join(
        b"".join(i32(v) for v in (shader, fog, stype, first_vert, num_verts, first_index, num_indexes,
                                  -1, 0, 0, 0, 0))
        + b"".join(f32(0.0) for _ in range(3))
        + b"".join(f32(0.0) for _ in range(9))
        + i32(0) + i32(0)
        for shader, fog, stype, first_vert, num_verts, first_index, num_indexes in surfaces
    )

    # --- one leaf holding every brush and every surface --------------------
    world_mins = (-HALF - WALL, -HALF - WALL, WALL_BOTTOM)
    world_maxs = (HALF + WALL, HALF + WALL, CEIL_BOTTOM + 64)
    leaf = (
        i32(0)                                   # cluster
        + i32(0)                                 # area
        + b"".join(i32(int(v)) for v in world_mins)
        + b"".join(i32(int(v)) for v in world_maxs)
        + i32(0) + i32(len(surfaces))            # firstLeafSurface, numLeafSurfaces
        + i32(0) + i32(len(brush_records))       # firstLeafBrush, numLeafBrushes
    )
    lumps[LUMP_LEAFS] = leaf
    lumps[LUMP_LEAFSURFACES] = b"".join(i32(i) for i in range(len(surfaces)))
    lumps[LUMP_LEAFBRUSHES] = b"".join(i32(i) for i in range(len(brush_records)))

    # --- one node: every query lands in leaf 0 (child = -(leaf + 1)) -------
    node = (
        i32(0)                                   # planeNum — any valid plane
        + i32(-1) + i32(-1)                      # children: leaf 0, leaf 0
        + b"".join(i32(int(v)) for v in world_mins)
        + b"".join(i32(int(v)) for v in world_maxs)
    )
    lumps[LUMP_NODES] = node

    # --- one model: the world ---------------------------------------------
    lumps[LUMP_MODELS] = (
        b"".join(f32(v) for v in world_mins)
        + b"".join(f32(v) for v in world_maxs)
        + i32(0) + i32(len(surfaces))            # firstSurface, numSurfaces
        + i32(0) + i32(len(brush_records))       # firstBrush, numBrushes
    )

    lumps[LUMP_ENTITIES] = entity_string()
    # An empty visibility lump means "no PVS": cm_load fills the cluster table
    # with 255 and treats everything as visible.
    lumps[LUMP_VISIBILITY] = b""
    lumps[LUMP_FOGS] = b""
    lumps[LUMP_LIGHTMAPS] = b""
    lumps[LUMP_LIGHTGRID] = b""

    # --- assemble ---------------------------------------------------------
    header_size = 8 + HEADER_LUMPS * 8
    body = []
    offset = header_size
    table = []
    for data in lumps:
        while offset % 4:
            body.append(b"\0")
            offset += 1
        table.append((offset, len(data)))
        body.append(data)
        offset += len(data)

    out = struct.pack("<i", BSP_IDENT) + struct.pack("<i", BSP_VERSION)
    out += b"".join(i32(o) + i32(l) for o, l in table)
    out += b"".join(body)
    return out, planes, brush_records, surfaces, verts, indexes


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "build", "q3data")
    target_dir = os.path.join(outdir, "baseq3", "maps")
    tex_dir = os.path.join(outdir, "baseq3", "textures", "mectovtest")
    os.makedirs(target_dir, exist_ok=True)
    os.makedirs(tex_dir, exist_ok=True)
    target = os.path.join(target_dir, MAP_NAME + ".bsp")

    data, planes, brush_records, surfaces, verts, indexes = build()
    with open(target, "wb") as fh:
        fh.write(data)

    texs = texture_files()
    tex_bytes = 0
    for name, blob in sorted(texs.items()):
        leaf = os.path.join(tex_dir, os.path.basename(name) + ".tga")
        with open(leaf, "wb") as fh:
            fh.write(blob)
        tex_bytes += len(blob)

    print("[testbsp] wrote %s" % target)
    print("[testbsp]   %d bytes, %d planes, %d brushes, %d surfaces, %d verts, %d indexes"
          % (len(data), len(planes), len(brush_records), len(surfaces), len(verts), len(indexes)))
    print("[testbsp] wrote %d texture(s) into %s (%d bytes, %dx%d TGA)"
          % (len(texs), tex_dir, tex_bytes, TEX_SIZE, TEX_SIZE))
    print("[testbsp]   floor top z=%d, spawn=(%d %d %d), -X wall face x=%d"
          % (FLOOR_TOP, SPAWN[0], SPAWN[1], SPAWN[2], -HALF))


if __name__ == "__main__":
    main()
