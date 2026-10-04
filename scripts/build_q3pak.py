#!/usr/bin/env python3
"""scripts/build_q3pak.py — build a SYNTHETIC pak0.pk3 (v38.111).

Why this exists
---------------
The retail-map path — q3a_data.py staging a .bsp and its textures out of a
user's OWN pak0.pk3 through id's shader scripts — cannot be tested against
retail data (pak0 is not redistributable, so CI has none and never will).
But a pak0 is an ordinary zip, and everything q3a_data.py does is driven by
the zip's CONTENTS: the .bsp's shader-name lump, the scripts/*.shader
sources, the image files at the paths the definitions name. So the test
arena (build_test_bsp.py) is wrapped in a genuine pk3 — same zip container,
same scripts/ layout, same shader-source language — and q3a_data.py stages
THE ARENA out of it exactly as a user would stage q3dm1.

What the pak carries
--------------------
  maps/mectovtest.bsp        the generated arena (build_test_bsp.py)
  scripts/mectovtest.shader  a definition for textures/mectovtest/curve that
                             binds the name to curve_jpg (a JPEG!) — the one
                             texture that must resolve through the shader
                             SCRIPT rather than the direct-image fallback
  textures/mectovtest/*.tga  the four textures the arena names directly
  textures/mectovtest/curve_jpg.jpg  the JPEG (magenta checker, same palette)
  textures/mectovtest/sky_cloud{0,1}.tga  v38.128: the sky fixture's two cloud
                             layers — the opaque base and the ADDITIVE one — a
                             bold checker and a mostly-black field with bright
                             patches, so "the box drew layer 1 too" is a count
                             of white pixels and "the layer moved" is a count
                             of pixels that changed between two dumps
  gfx/2d/numbers/*.tga       v38.127: id's status bar art, synthetic but in
  gfx/2d/bigchars.tga        id's own format and sizes — the eleven number
  gfx/2d/select.tga          fields, the 16x16-cell charset atlas, the score
  icons/icona_machinegun.tga box's overlay and two icons (q3hud.c). Each
  icons/iconr_yellow.tga     number field is a flat colour whose red channel is
                             the digit, so a screendump can be READ as numbers.

The curve shader's definition deliberately changes its extension: resolving
"texture" without reading the script would load curve.tga; resolving it
through the definition loads curve_jpg.jpg. That difference is what the test
can assert on.

v38.128 adds maps/mectovsky.bsp: the SAME arena (same generator, same room)
with its ceiling painted a cloud-layer sky, plus that sky's two layers. It is a
second map for the same reason the arena is a map at all — a level's shader lump
names the shaders its surfaces use, and only a surface can make the renderer
build a sky box. The sky shader is written the way q3dm1's
textures/skies/tim_hell is (`skyParms - <height> -`, an opaque base layer and an
additive cloud layer, each with its own tcMod scroll), so what this pak exercises
in CI is the same code path the retail map does.

Usage:
    python3 scripts/build_q3pak.py                # -> build/q3pak/pak0.pk3
    python3 scripts/build_q3pak.py /tmp/pak0.pk3  # explicit output
"""
import math
import os
import struct
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import build_test_bsp  # noqa: E402  (generates the arena .bsp + TGA textures)

OUT_DEFAULT = os.path.join(ROOT, "build", "q3pak", "pak0.pk3")

TEX = build_test_bsp.TEX_SIZE
# Same saturated palette family as the arena's other textures (all multiples
# of 8, so the renderer's 5-bit texture path is lossless).
FLOOR_G = build_test_bsp.TEX_FLOOR_G
WALL_B = build_test_bsp.TEX_WALL_B
CURVE_A = build_test_bsp.TEX_CURVE       # magenta family, matches curve.tga
CURVE_B = build_test_bsp.TEX_CURVE_G
SKY_A = (64, 96, 208)                    # sky blue, distinct from every face
SKY_B = (40, 64, 152)


def checker(size, cell, color_a, color_b):
    px = bytearray()
    for y in range(size):
        row_b = (y // cell) % 2
        for x in range(size):
            on = ((x // cell) + row_b) % 2 == 0
            px += bytes(color_a if on else color_b)
    return px


def grid_lines(px, size, step, width, color):
    for y in range(size):
        for x in range(size):
            if (x % step) < width or (y % step) < width:
                o = (y * size + x) * 3
                px[o:o + 3] = bytes(color)


def tga_bytes(px, size):
    header = struct.pack("<BBBHHBHHHHBB",
                         0, 0, 2, 0, 0, 0, 0, 0, size, size, 24, 0x20)
    body = bytearray()
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        body += bytes((b, g, r))            # TGA stores BGR
    return header + bytes(body)


def mosaic(size, cell, colors, seed=0x9E3779B9):
    """`size` x `size` RGB rows of `cell`-texel tiles, each tile one of
    `colors` picked by a hash of the tile's own coordinates.

    Deterministic (same bytes every run) and APERIODIC (no tile pattern repeats
    before the whole texture does), which is what a "the layer moved" pixel
    assertion needs from the texture it measures — see the sky fixture's layers
    in build_pak below.
    """
    px = bytearray()
    for y in range(size):
        cy = y // cell
        for x in range(size):
            cx = x // cell
            h = (cx * 73856093) ^ (cy * 19349663) ^ seed
            h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
            px += bytes(colors[(h >> 7) % len(colors)])
    return px


def jpeg_bytes(px, size, quality=85):
    """Encode the RGB pixel array as a baseline JPEG via PIL."""
    from PIL import Image
    im = Image.frombytes("RGB", (size, size), bytes(px))
    import io
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=quality)
    return buf.getvalue()


def wav_bytes(rate, bits, ms, freq=440.0):
    """Build a mono PCM RIFF/WAVE byte string (v38.129).

    The shape id's own WAV writer emits and the loader (q3sound.c) parses:
    RIFF/WAVE with a bare fmt (tag 1 = PCM) and a data chunk, 22050 Hz mono,
    8 or 16 bits. The sample data is a half-amplitude sine burst (clamped to
    u8's unsigned range when bits=8) so a mix of several channels can only
    come out smaller, never clipped. Chunk layout is the simple contiguous
    kind — fmt then data — which is what id's files are; the loader walks
    chunk headers precisely so it would also survive a cbNM extra chunk.
    """
    n = rate * ms // 1000
    if bits == 16:
        body = bytearray()
        for i in range(n):
            v = int(16000.0 * math.sin(2.0 * math.pi * freq * i / rate))
            body += struct.pack("<h", v)
    else:
        body = bytearray()
        for i in range(n):
            v = 128 + int(60.0 * math.sin(2.0 * math.pi * freq * i / rate))
            body.append(max(0, min(255, v)))
    fmt = struct.pack("<HHIIHH", 1, 1, rate, rate * bits // 8,
                      bits // 8, bits)
    riff = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + \
        b"data" + struct.pack("<I", len(body)) + bytes(body)
    # riff already STARTS with the WAVE form id, so the RIFF wrapper is the
    # id + size + those 4 bytes — nothing more.
    return b"RIFF" + struct.pack("<I", len(riff)) + riff


def md3_part(name, surfaces, tags, bounds):
    """One .md3 file, written the way id's exporter writes it (v38.124).

    Layout, straight out of qfiles.h:113-209 and verified field-for-field
    against the retail demo's machinegun.md3: a 108-byte md3Header_t, then
    numFrames*56 bytes of md3Frame_t, then numFrames*numTags*112 bytes of
    md3Tag_t, then the surfaces. Inside a surface: its own 108-byte
    md3Surface_t, then the shaders (68 bytes each), then triangles, then st,
    then xyzNormals — and every one of those ofs* fields is RELATIVE TO THE
    START OF THAT SURFACE, with ofsEnd ending the surface's chunk. The loader
    this feeds (third_party/tinygl/q3viewmodel.c) reads exactly that, and the
    retail file's own offsets land on those chunk boundaries to the byte.

    `surfaces` is a list of (surface_name, shader_name, verts, tris, uvs)
    where verts are model-space floats, `tris` index into them and `uvs` are
    (s, t) pairs. `tags` is a list of (tag_name, origin) — frame 0 only, which
    is all these parts have. `bounds` is (mins, maxs)."""
    MD3_IDENT = 0x33504449                  # ('3'<<24)+('P'<<16)+('D'<<8)+'I'
    blobs = []
    for sname, shader, verts, tris, uvs in surfaces:
        nv, nt = len(verts), len(tris)
        ofs_shaders = 108
        ofs_tri = ofs_shaders + 68          # one md3Shader_t per surface here
        ofs_st = ofs_tri + nt * 12
        ofs_xyz = ofs_st + nv * 8
        ofs_end = ofs_xyz + nv * 8
        # ident, name[64], then ten ints: flags, numFrames, numShaders,
        # numVerts, numTriangles, ofsTriangles, ofsShaders, ofsSt,
        # ofsXyzNormals, ofsEnd.
        hdr = struct.pack("<i64siiiiiiiiii", MD3_IDENT, sname.encode()[:63],
                          0, 1, 1, nv, nt, ofs_tri, ofs_shaders, ofs_st,
                          ofs_xyz, ofs_end)
        sh = struct.pack("<64si", shader.encode()[:63], 0)
        tri = b"".join(struct.pack("<3i", *t) for t in tris)
        st = b"".join(struct.pack("<2f", *uv) for uv in uvs)
        # xyz as fixed point (md3_XYZ_SCALE, 1/64) plus a packed normal. The
        # normal is written flat (0) on purpose: the port draws view models
        # full bright from a fixed light, so nothing here reads it, and
        # pretending otherwise would be a number no test could justify.
        xyz = b"".join(struct.pack("<3hH", int(x * 64), int(y * 64), int(z * 64), 0)
                       for x, y, z in verts)
        blobs.append(hdr + sh + tri + st + xyz)

    frame = struct.pack("<3f", *bounds[0]) + struct.pack("<3f", *bounds[1]) + \
        struct.pack("<3f", 0.0, 0.0, 0.0) + struct.pack("<f", 16.0) + \
        b"from mectovtest\x00"      # 16 bytes, id's own exporter writes "from ASE"
    assert len(frame) == 56
    tagblobs = b""
    for tname, origin in tags:
        tagblobs += struct.pack("<64s", tname.encode()[:63])
        tagblobs += struct.pack("<3f", *origin)
        tagblobs += struct.pack("<9f", 1.0, 0.0, 0.0,
                                           0.0, 1.0, 0.0,
                                           0.0, 0.0, 1.0)
    assert len(tagblobs) == len(tags) * 112

    ofs_frames = 108
    ofs_tags = ofs_frames + len(frame)
    ofs_surfaces = ofs_tags + len(tagblobs)
    head = struct.pack("<ii64siiiiiiiii", MD3_IDENT, 15, name.encode()[:63],
                       0, 1, len(tags), len(surfaces), 0, ofs_frames, ofs_tags,
                       ofs_surfaces, ofs_surfaces + sum(len(b) for b in blobs))
    assert len(head) == 108
    return head + frame + tagblobs + b"".join(blobs)


def box_verts(x0, x1, half_yz):
    """A 2x2-cross-section box between two x planes: 8 vertices, 12 triangles,
    every face mapped to the whole texture so the checker shows on all sides."""
    h = half_yz
    verts = [(x0, -h, -h), (x0, h, -h), (x0, h, h), (x0, -h, h),
             (x1, -h, -h), (x1, h, -h), (x1, h, h), (x1, -h, h)]
    tris = [(0, 1, 2), (0, 2, 3),                 # x0 cap
            (4, 6, 5), (4, 7, 6),                 # x1 cap
            (0, 4, 5), (0, 5, 1),                 # -z
            (3, 2, 6), (3, 6, 7),                 # +z
            (0, 3, 7), (0, 7, 4),                 # -y
            (1, 5, 6), (1, 6, 2)]                 # +y
    uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0),
           (0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    return verts, tris, uvs


def shader_source():
    """The arena's scripts/mectovtest.shader, as id's tools would write it."""
    return """\
// mectov synthetic pak0 -- definitions for the generated test arena.
// The curve shader is REBOUND here to a JPEG (curve_jpg): the .bsp's shader
// lump only names "textures/mectovtest/curve", so the only way the renderer
// can find curve_jpg.jpg is by reading this file -- which is exactly what the
// retail-map test asserts on.
textures/mectovtest/curve
{
    {
        map textures/mectovtest/curve_jpg
        blendFunc GL_ONE GL_ZERO
    }
}

// v38.128: the cloud sky the sky fixture map's ceiling uses. id's own shape,
// the one q3dm1's textures/skies/tim_hell is written in: `skyParms - 512 -`
// (no six-image far box) with an opaque base layer and an ADDITIVE cloud layer,
// each with its own tcMod. The suite's proof that the sky ANIMATES is two
// screendumps of a pinned camera taken a while apart (the world is identical,
// the sky is not), and the two speeds are chosen so that proof cannot be an
// accident of arithmetic: `tcMod scroll` adds the FRACTIONAL part of
// speed*time, so a gap that happens to be a whole number of periods puts the
// layer back exactly where it started and a working sky would look frozen.
// At 0.011 units/second a layer needs ~91 seconds of game clock to come
// round, which no gap this suite uses can reach (game time never runs faster
// than wall time), while even a few seconds of game clock move it by whole
// texels of the mosaic below.
//
// Stage 0 is the opaque base: a bold blue checker. Stage 1 is additive, so it can
// only ADD light: a white/black checker, which is id's own recipe for clouds
// (`blendFunc GL_ONE GL_ONE` on killsky_2). It makes "layer 1 was drawn" a count
// of near-white pixels in the sky band that the base layer (brightest colour
// 64,96,208) cannot produce at any light level.
textures/mectovtest/sky
{
    qer_editorimage textures/mectovtest/sky_cloud0
    surfaceparm sky
    surfaceparm noimpact
    surfaceparm nolightmap
    skyParms - 512 -
    {
        map textures/mectovtest/sky_cloud0
        tcMod scroll 0.011 0.013
    }
    {
        map textures/mectovtest/sky_cloud1
        blendFunc GL_ONE GL_ONE
        tcMod scroll 0.007 0.009
    }
}

textures/mectovtest/floor
{
    {
        map textures/mectovtest/floor
    }
}

textures/mectovtest/wall
{
    {
        map textures/mectovtest/wall
    }
}

textures/mectovtest/ceiling
{
    {
        map textures/mectovtest/ceiling
    }
}

textures/mectovtest/step
{
    {
        map textures/mectovtest/step
    }
}

// ---- v38.125: the four probe shaders build_test_bsp.py adds ---------------
// No surface uses them (see SHADERS there), so they change no pixel; what they
// DO change is the log, which is where the renderer's shader-script rules are
// asserted. Each one is a real pattern from the demo's own scripts that the
// engine used to get wrong:
//
//   probe_dollar     q3dm1's sky, lava and glow shaders all open with
//                    `map $lightmap`; taking that operand literally sent the
//                    renderer looking for a file called "$lightmap.jpg" and
//                    the definition's real image was never seen.
//   probe_anim       id's torches are animMap-ONLY stages; a parser that only
//                    looks for `map` resolves nothing for them.
//   probe_extension  id's scripts spell the extension (".../killsky_1.tga") and
//                    the packed file is the .jpg. The name here is deliberately
//                    NOT an image path, so a shader-name fallback cannot pass
//                    this probe by accident.
//   probe_flame      a GL_ONE GL_ONE stage with `cull none` - the two flags the
//                    draw pass needs (id draws fire additively, after the
//                    opaque world).
textures/mectovtest/probe_dollar
{
    surfaceparm nolightmap
    {
        map $lightmap
        rgbGen identity
    }
    {
        map textures/mectovtest/probe_dollar_real.tga
        rgbGen identity
        blendFunc GL_DST_COLOR GL_ZERO
    }
}

textures/mectovtest/probe_anim
{
    surfaceparm nolightmap
    surfaceparm trans
    {
        animMap 10 textures/mectovtest/probe_anim0.tga textures/mectovtest/probe_anim1.tga
        blendFunc GL_ONE GL_ONE
    }
}

textures/mectovtest/probe_extension
{
    {
        map textures/mectovtest/probe_ext_target.tga
    }
}

textures/mectovtest/probe_flame
{
    cull none
    surfaceparm nolightmap
    surfaceparm trans
    {
        map textures/mectovtest/probe_flame.tga
        blendFunc GL_ONE GL_ONE
    }
}
"""


def build_pak(out_path):
    arena_dir = os.path.join(ROOT, "build", "q3data")
    # build() grew a seventh return value in v38.112 (the PVS fixture's stats,
    # None for the arena) — unpack it explicitly rather than by arity.
    # with_probes: this pak ships scripts/mectovtest.shader, so the four probe
    # shaders' definitions are on the volume with it — which is the condition
    # for their names to be in the .bsp's lump at all (see build_test_bsp.build).
    bsp_data, _planes, _brushes, _surfaces, _verts, _idx, _vis = \
        build_test_bsp.build(with_probes=True)
    # v38.128: the sky fixture — the same room, the same generator, one shader
    # swapped (see build_test_bsp.build(sky_ceiling=True)). with_probes: its
    # shader lump has to carry the sky name, and the sky definition lives in the
    # same scripts/mectovtest.shader this pak ships.
    sky_data, _p2, _b2, _s2, _v2, _i2, _vis2 = \
        build_test_bsp.build(with_probes=True, sky_ceiling=True)

    entries = {}
    entries["maps/mectovtest.bsp"] = bsp_data
    entries["maps/mectovsky.bsp"] = sky_data

    px = checker(TEX, 16, CURVE_A, CURVE_B)
    grid_lines(px, TEX, 16, 2, CURVE_B)
    entries["textures/mectovtest/curve_jpg.jpg"] = jpeg_bytes(px, TEX)
    # The direct-fallback texture, kept tiny and BRIGHT — if resolution ever
    # took the fallback instead of the script's definition, the logged path
    # (and the pixels) would say so. Also proves the parser ignores curve.tga
    # when a definition exists... it is simply not referenced by the script.
    px = checker(TEX, 8, (255, 240, 0), (40, 40, 0))
    entries["textures/mectovtest/curve.tga"] = tga_bytes(px, TEX)

    # The four textures the arena's OTHER shaders name. They resolve through
    # the definitions above (which just echo the same path), so the staged
    # tree ends up complete for every shader name in the .bsp.
    for name, blob in sorted(build_test_bsp.texture_files().items()):
        if name.endswith("/curve"):
            continue                    # curve is deliberately rebound above
        entries[name + ".tga"] = blob

    # ---- v38.128: the sky fixture's two cloud layers ----------------------
    # Stage 0 (opaque, the base): a MOSAIC of blue tones, not a checker.
    # A checker's period is two cells, so a scroll whose phase landed on a
    # multiple of it would put the layer back where it started and make a
    # working sky fail a "the layer moved" assertion. A mosaic of aperiodic
    # cells has no period shorter than the texture itself, which is what makes
    # that measurement a measurement.
    entries["textures/mectovtest/sky_cloud0.tga"] = tga_bytes(
        mosaic(TEX, 8, [SKY_A, SKY_B, (88, 120, 232), (24, 40, 104)]), TEX)
    # Stage 1 (additive): black with sparse near-white cells. Two properties
    # matter and both are deliberate: additive compositing can only ADD light,
    # so `the additive layer was drawn` is a count of near-white pixels in the
    # sky band that the base layer (brightest colour SKY_A at full brightness)
    # can never produce; and the bright cells are spread over the whole texture
    # rather than sitting in one patch, because the clip only fills the
    # sub-rectangle of a box side the level's ceiling actually covers — a
    # one-patch texture could fall entirely outside it and make a working sky
    # look like a broken one.
    entries["textures/mectovtest/sky_cloud1.tga"] = tga_bytes(
        mosaic(TEX, 8, [(0, 0, 0), (0, 0, 0), (0, 0, 0), (248, 248, 248)]), TEX)

    px = checker(TEX, 16, FLOOR_G, WALL_B)
    grid_lines(px, TEX, 16, 2, WALL_B)
    entries["env/mectovtest/sky_alt.tga"] = tga_bytes(px, TEX)

    # ---- v38.125: the probe shaders' images (see shader_source above) ------
    # Four shaders no surface uses, so none of these files is ever drawn: they
    # exist so the engine's resolution shows up in the log. Two of them are
    # deliberately spelled differently from the file that ships —
    # probe_ext_target is asked for as .tga and only the .jpeg-ish .jpg exists,
    # which is exactly the demo's killsky_1 (script says .tga, pak ships .jpg).
    px = checker(TEX, 16, CURVE_B, FLOOR_G)
    grid_lines(px, TEX, 16, 2, FLOOR_G)
    entries["textures/mectovtest/probe_dollar_real.tga"] = tga_bytes(px, TEX)

    px = checker(TEX, 16, (248, 168, 64), (96, 48, 16))
    grid_lines(px, TEX, 16, 2, (96, 48, 16))
    entries["textures/mectovtest/probe_anim0.tga"] = tga_bytes(px, TEX)
    px = checker(TEX, 16, (232, 120, 32), (80, 40, 16))
    grid_lines(px, TEX, 16, 2, (80, 40, 16))
    entries["textures/mectovtest/probe_anim1.tga"] = tga_bytes(px, TEX)

    # NB: no probe_ext_target.tga on purpose — the script spells .tga and the
    # only file is the .jpg, so the resolver must strip the extension and then
    # prefer the JPEG (id's own order), not append a second extension.
    px = checker(TEX, 16, (216, 216, 96), (72, 72, 24))
    grid_lines(px, TEX, 16, 2, (72, 72, 24))
    entries["textures/mectovtest/probe_ext_target.jpg"] = jpeg_bytes(px, TEX)

    px = checker(TEX, 16, (255, 200, 64), (0, 0, 0))
    grid_lines(px, TEX, 16, 2, (255, 120, 32))
    entries["textures/mectovtest/probe_flame.tga"] = tga_bytes(px, TEX)

    entries["scripts/mectovtest.shader"] = shader_source().encode("ascii")

    # ---- v38.124: a synthetic weapon view model -------------------------
    # The port's first-person gun is id's own .md3 (q3viewmodel.c), and CI can
    # never have the demo pak0 that ships the real one — so the same trick the
    # .bsp staging uses applies here: the loader is driven by the ZIP'S
    # CONTENTS, so a small model in id's format exercises the whole path. What
    # it is built to catch, none of which a retail-only test could run in CI:
    #   * offsets relative to the surface (a loader that treats them as
    #     absolute parses frame data as shader names and loads nothing);
    #   * a shader name that NAMES an extension the pak does not ship —
    #     "machinegun.tga" while machinegun.jpg is what exists, and
    #     "f_machinegun.TGA" in capitals, exactly as retail spells them;
    #   * the tag chain (body tag_barrel + barrel tag_flash must agree with
    #     the body's own tag_flash, or the loader drops both parts);
    #   * a flash drawn only while the module's weaponstate says FIRING.
    # The checker is deliberately loud (8 stripes) so the on-screen box's
    # distinct-colour count is far above what a flat silhouette can produce.
    gun_tex = bytearray()
    stripes = [(240, 40, 40), (40, 240, 40), (40, 40, 240), (240, 240, 40),
               (240, 40, 240), (40, 240, 240), (250, 250, 250), (20, 20, 20)]
    for y in range(TEX):
        for x in range(TEX):
            gun_tex += bytes(stripes[(x * 8 // TEX) % len(stripes)])
    # Both images ship as JPEG under a name that says .tga/.TGA, which is
    # exactly what retail does: the loader has to strip the extension it was
    # given, lower-case the capitals, and then find the file id actually
    # shipped. The flash gets its own palette so the two are distinguishable.
    flash_tex = bytearray()
    for y in range(TEX):
        for x in range(TEX):
            flash_tex += bytes((252, 244, 190) if ((x // 8 + y // 8) % 2)
                               else (255, 168, 32))
    entries["models/weapons2/machinegun/machinegun.jpg"] = jpeg_bytes(gun_tex, TEX)
    entries["models/weapons2/machinegun/f_machinegun.jpg"] = jpeg_bytes(flash_tex, TEX)

    body_v, body_t, body_uv = box_verts(0.0, 5.0, 1.0)
    barrel_v, barrel_t, barrel_uv = box_verts(0.0, 10.0, 0.4)
    flash_v = [(-0.5, -2.0, 0.0), (-0.5, 2.0, 0.0),
               (0.5, 0.0, -2.0), (0.5, 0.0, 2.0)]
    flash_t = [(0, 1, 2), (0, 1, 3), (2, 3, 0), (2, 3, 1)]
    flash_uv = [(0.0, 0.0), (1.0, 1.0), (1.0, 0.0), (0.0, 1.0)]

    entries["models/weapons2/machinegun/machinegun.md3"] = md3_part(
        "machinegun",
        [("w_machinegun_body", "models/weapons2/machinegun/machinegun.tga",
          body_v, body_t, body_uv)],
        [("tag_weapon", (0.0, 0.0, 0.0)),
         ("tag_barrel", (5.0, 0.0, 0.0)),
         ("tag_flash", (15.0, 0.0, 0.0))],
        ((-0.0, -1.0, -1.0), (5.0, 1.0, 1.0)))
    entries["models/weapons2/machinegun/machinegun_barrel.md3"] = md3_part(
        "machinegun_barrel",
        [("b_barrel", "models/weapons2/machinegun/machinegun.tga",
          barrel_v, barrel_t, barrel_uv)],
        [("tag_weapon", (-5.0, 0.0, 0.0)),
         ("tag_barrel", (0.0, 0.0, 0.0)),
         ("tag_flash", (10.0, 0.0, 0.0))],
        ((0.0, -0.4, -0.4), (10.0, 0.4, 0.4)))
    entries["models/weapons2/machinegun/machinegun_flash.md3"] = md3_part(
        "machinegun_flash",
        [("f_machinegun", "models/weapons2/machinegun/f_machinegun.TGA",
          flash_v, flash_t, flash_uv)],
        [("tag_weapon", (-15.0, 0.0, 0.0)),
         ("tag_barrel", (-10.0, 0.0, 0.0)),
         ("tag_flash", (0.0, 0.0, 0.0))],
        ((-0.5, -2.0, -2.0), (0.5, 2.0, 2.0)))

    # ---- v38.127: id's own status bar art (third_party/tinygl/q3hud.c) -----
    # The HUD is a picture pipeline, so the only honest way to test it is to
    # make the pictures DECODABLE FROM A SCREENSHOT: every number field is a
    # flat colour whose red channel IS the digit, and the icons, the charset
    # cells and the score box's overlay are flat colours of their own. A test
    # can then read the health and ammo fields straight out of a screendump and
    # say which NUMBER the HUD drew — which is what "the values came from the
    # playerState" means, as opposed to "some pixels appeared at the bottom".
    #
    # The tint matters here: the status bar multiplies each field by id's own
    # colour (amber for a healthy level, white above 100, grey while firing), so
    # the red channel is what survives — 16 + digit*22 against amber's 1.0, 0.69
    # and 0.0 for g and b, i.e. (16+22d, 138, 0) on screen for a normal field.
    for d, word in enumerate(["zero", "one", "two", "three", "four", "five",
                              "six", "seven", "eight", "nine"]):
        px = bytearray()
        for _ in range(32 * 32):
            px += bytes((16 + d * 22, 200, 200))
        entries["gfx/2d/numbers/%s_32b.tga" % word] = tga_bytes(px, 32)
    px = bytearray(bytes((240, 240, 240)) * (32 * 32))
    entries["gfx/2d/numbers/minus_32b.tga"] = tga_bytes(px, 32)

    # The charset is a 256x256 atlas of 16x16 cells (id's CG_DrawChar: 0.0625),
    # so the cell for a digit is at (col*16, row*16) with row = ch>>4. The score
    # box draws with a white tint, so these colours come through unchanged.
    px = bytearray(bytes((24, 24, 32)) * (256 * 256))
    for d in range(10):
        ch = ord("0") + d
        row, col = ch >> 4, ch & 15
        for y in range(16 * row, 16 * row + 16):
            for x in range(16 * col, 16 * col + 16):
                o = (y * 256 + x) * 3
                px[o:o + 3] = bytes((16 + d * 22, 200, 200))
    entries["gfx/2d/bigchars.tga"] = tga_bytes(px, 256)

    # select.tga is the "this box is yours" overlay id paints over the score box
    # a player's own score sits in.
    px = bytearray(bytes((250, 40, 200)) * (32 * 32))
    entries["gfx/2d/select.tga"] = tga_bytes(px, 32)

    # Two icons, and they are a positive/negative pair: the fixture's
    # playerState has ammo but NO armor, so CG_DrawStatusBar draws the ammo
    # icon and must not draw the armor one. One file proves each side.
    px = bytearray(bytes((40, 250, 40)) * (32 * 32))
    entries["icons/icona_machinegun.tga"] = tga_bytes(px, 32)
    px = bytearray(bytes((250, 250, 40)) * (32 * 32))
    entries["icons/iconr_yellow.tga"] = tga_bytes(px, 32)

    # ---- v38.129: the sound fixture ----------------------------------------
    # The driver (q3_vm.c) names sixteen map sfx by PATH and registers them at
    # open; CI has no pak0, so the fixture carries every one of those paths as
    # a small synthetic WAV — same container entries as the demo pak would
    # stage, same directory shape. Formats mirror the real data they stand in
    # for (all 22050 Hz mono PCM/tag 1): 16-bit for the weapon/footstep/land
    # group, 8-bit for jumppad — so the loader's both sample-width paths are
    # exercised by the same map that drives the events. Each sound is a sine
    # burst whose frequency derives from its name, so a log could tell them
    # apart even if every line were the same shape.
    sfx_16 = [
        ("sound/weapons/machinegun/machgf1b.wav", 1180, 60),
        ("sound/weapons/machinegun/machgf2b.wav", 1260, 60),
        ("sound/weapons/machinegun/machgf3b.wav", 1340, 60),
        ("sound/weapons/machinegun/machgf4b.wav", 1420, 60),
        ("sound/player/footsteps/step1.wav", 260, 90),
        ("sound/player/footsteps/step2.wav", 290, 90),
        ("sound/player/footsteps/step3.wav", 320, 90),
        ("sound/player/footsteps/step4.wav", 350, 90),
        ("sound/player/footsteps/clank1.wav", 520, 90),
        ("sound/player/footsteps/clank2.wav", 560, 90),
        ("sound/player/footsteps/clank3.wav", 600, 90),
        ("sound/player/footsteps/clank4.wav", 640, 90),
        ("sound/player/land1.wav", 180, 120),
        ("sound/player/visor/jump1.wav", 880, 100),
        ("sound/weapons/noammo.wav", 220, 100),
    ]
    for path, freq, ms in sfx_16:
        entries[path] = wav_bytes(22050, 16, ms, float(freq))
    # The one 8-bit sound, matching the demo pak's world/jumppad.wav format.
    entries["sound/world/jumppad.wav"] = wav_bytes(22050, 8, 150, 740.0)

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as z:
        for name in sorted(entries):
            z.writestr(name, entries[name])

    total = sum(len(v) for v in entries.values())
    print("[q3pak] wrote %s" % out_path)
    print("[q3pak]   %d entries, %d bytes raw (%s)" % (
        len(entries), total,
        "%.1f KB" % (os.path.getsize(out_path) / 1024.0)))
    for name in sorted(entries):
        print("[q3pak]     %s (%d bytes)" % (name, len(entries[name])))


if __name__ == "__main__":
    build_pak(sys.argv[1] if len(sys.argv) > 1 else OUT_DEFAULT)
