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
  env/mectovtest/sky_*.tga   six skybox faces for the skyParms in the script

The curve shader's definition deliberately changes its extension: resolving
"texture" without reading the script would load curve.tga; resolving it
through the definition loads curve_jpg.jpg. That difference is what the test
can assert on.

Usage:
    python3 scripts/build_q3pak.py                # -> build/q3pak/pak0.pk3
    python3 scripts/build_q3pak.py /tmp/pak0.pk3  # explicit output
"""
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


def jpeg_bytes(px, size, quality=85):
    """Encode the RGB pixel array as a baseline JPEG via PIL."""
    from PIL import Image
    im = Image.frombytes("RGB", (size, size), bytes(px))
    import io
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=quality)
    return buf.getvalue()


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

textures/mectovtest/sky
{
    qer_editorimage env/mectovtest/sky
    surfaceparm sky
    surfaceparm noimpact
    surfaceparm nolightmap
    skyParms env/mectovtest/sky 512 -
    {
        map env/mectovtest/sky
        tcMod scale 2 -1
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

    entries = {}
    entries["maps/mectovtest.bsp"] = bsp_data

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

    px = checker(TEX, 16, SKY_A, SKY_B)
    grid_lines(px, TEX, 16, 2, SKY_B)
    sky_tga = tga_bytes(px, TEX)
    for face in ("rt", "bk", "lf", "ft", "up", "dn"):
        entries["env/mectovtest/sky_%s.tga" % face] = sky_tga

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
