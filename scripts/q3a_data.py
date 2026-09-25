#!/usr/bin/env python3
"""scripts/q3a_data.py — stage the game data ONE Quake III map needs (v38.107).

Why this exists
---------------
The kernel can now load a real .bsp through id's own collision model, and the
next phases render it. Neither is testable or runnable without the retail game
data — and that data is **not** redistributable: the GPL covers id's source and
id's bytecode, never pak0.pk3's maps, textures, models and sounds. So it has to
come from the user's own copy, and it has to be staged out of the game and into
this port.

What it stages, and why it is a *walk* rather than "unzip everything"
-------------------------------------------------------------------
pak0.pk3 is ~460 MB and the kernel reads whole files into RAM (the ext2 layer
has no read-at-offset path yet), so blindly extracting it is both wasteful and
slow. What a map actually needs is a closed set:

  maps/<map>.bsp              the level itself
  maps/<map>.aas              bot navigation (botlib, from a later phase)
  scripts/*.shader            the definitions naming every texture the .bsp's
                              shader lump refers to
  textures/...                the images those definitions name, plus the
                              conventional <shadername>.tga/.jpg fallback for
                              names with no definition (shaderless textures)
  env/...                     skybox faces named by skyParms
  models/**.md3 + .skin       (--with-models) player and weapon models

The images are found by parsing the .shader sources, not by guessing: a Q3
texture is reachable only through its shader, and a shader can name several
images (animMap chains, skyParms, tcMod) or none at all ($lightmap).

Output is a LOOSE-FILE tree under build/q3data/baseq3/... — the same layout the
engine's filesystem would find inside a pk3, and the same tree
scripts/seed_ext2.sh mirrors onto the volume as /baseq3/... The port reads data
out of a directory install the way ioquake3 reads it out of a pk3; doing the
unzipping here means the kernel needs no zip-capable streaming reader.

Usage:
    scripts/q3a_data.py --pak ~/q3/pak0.pk3                 # default map q3dm1
    scripts/q3a_data.py --pak ~/q3/pak0.pk3 --map q3dm17
    scripts/q3a_data.py --pak ~/q3/pak0.pk3 --list           # plan only
    scripts/q3a_data.py --pak ... --with-models --with-sounds

Honesty note: this script's job is to be *verifiable*. `--verify` re-reads the
staged .bsp and reports any shader name that resolved to no file at all, so a
partial or surprising extraction says so instead of looking finished. The script
was developed against a synthetic pak0 built from the generated test arena
(scripts/build_test_bsp.py) because no retail data may be committed here — see
README "Game data".
"""
import argparse
import os
import re
import struct
import sys
import zipfile

LUMP_SHADERS = 1
LUMP_ENTITIES = 0
MAX_QPATH = 64
SHADER_RECORD = MAX_QPATH + 8

IMAGE_EXTS = (".tga", ".jpg", ".jpeg", ".png")

SKY_SUFFIXES = ("rt", "bk", "lf", "ft", "up", "dn")


def die(msg):
    print("[q3data] error: %s" % msg, file=sys.stderr)
    sys.exit(2)


def read_lump(data, index):
    (offset, length) = struct.unpack_from("<2i", data, 8 + index * 8)
    if offset < 0 or length < 0 or offset + length > len(data):
        die("lump %d is out of range" % index)
    return data[offset:offset + length]


def bsp_shader_names(data):
    """Every shader name the level's geometry refers to (the shader lump)."""
    lump = read_lump(data, LUMP_SHADERS)
    if len(lump) % SHADER_RECORD:
        die("shader lump is not a multiple of %d bytes" % SHADER_RECORD)
    names = []
    for i in range(0, len(lump), SHADER_RECORD):
        raw = lump[i:i + MAX_QPATH].split(b"\0", 1)[0]
        if raw:
            names.append(raw.decode("latin-1"))
    return names


def bsp_entity_models(data):
    """model="..." keys in the entity lump that name loose model files.

    Inline models (``*3``) point into the bsp itself and are skipped; anything
    starting with ``models/`` (a func_static's md3, a mapmodel) is a real file
    the level will fail to load without.
    """
    text = read_lump(data, LUMP_ENTITIES).decode("latin-1", "replace")
    return sorted(set(re.findall(r'"model"\s+"(models/[^"]+)"', text)))


TOKEN_RE = re.compile(r'"[^"]*"|\S+')


def tokenize_shader_source(text):
    """Split a .shader source into tokens, with // and /* */ comments removed.

    Comments have to go first: a commented-out ``map`` line is extremely common
    in id's shader sources, and treating it as live would pull images the level
    never asks for.
    """
    stripped = re.sub(r"//[^\n]*", " ", text)
    stripped = re.sub(r"/\*.*?\*/", " ", stripped, flags=re.S)
    out = []
    for m in TOKEN_RE.finditer(stripped):
        t = m.group(0)
        out.append(t[1:-1] if t.startswith('"') and t.endswith('"') else t)
    return out


def parse_shaders(text):
    """Map shader name -> list of image paths it names.

    A Q3 .shader file is a sequence of ``name [surfaceparm ...] { body }``
    blocks. Only the body matters here: ``map``/``clampmap``/``animMap`` take
    image paths, ``skyParms`` takes a skybox base name (or "-" for none), and
    ``$``-prefixed operands are engine-provided ($lightmap, $whiteimage, …) and
    name no file.
    """
    tokens = tokenize_shader_source(text)
    shaders = {}
    i = 0
    while i < len(tokens):
        name = tokens[i]
        i += 1
        # surfaceparms and other flags between the name and the opening brace
        while i < len(tokens) and tokens[i] != "{":
            i += 1
        if i >= len(tokens):
            break
        i += 1  # consume '{'
        images = []
        depth = 1
        while i < len(tokens) and depth:
            t = tokens[i]
            if t == "{":
                depth += 1
            elif t == "}":
                depth -= 1
            elif t in ("map", "clampmap") and i + 1 < len(tokens):
                operand = tokens[i + 1]
                if not operand.startswith("$"):
                    images.append(operand)
                i += 1
            elif t == "animMap" and i + 1 < len(tokens):
                # animMap <fps> <image> <image> ...  (frames until a non-image)
                i += 2
                while i < len(tokens) and not tokens[i].startswith("$") \
                        and tokens[i] not in ("}") and "/" in tokens[i]:
                    images.append(tokens[i])
                    i += 1
                continue
            elif t == "skyParms" and i + 1 < len(tokens):
                base = tokens[i + 1]
                if base not in ("-", "0"):
                    for suffix in SKY_SUFFIXES:
                        # id's own sky shaders spell the far box in full —
                        # `skyParms env/space1/space1 512 -` — and the engine
                        # just appends the face suffix, giving
                        # env/space1/space1_rt. Some third-party sources pass
                        # the bare name instead. Offer both and let resolution
                        # pick whichever the pak actually has, because guessing
                        # wrong here means a black sky, not an error.
                        images.append("%s_%s" % (base, suffix))
                        if not base.startswith("env/"):
                            images.append("env/%s_%s" % (base, suffix))
                i += 1
            i += 1
        if name and name != "{":
            shaders.setdefault(name, []).extend(images)
    return shaders


def norm(path):
    return path.replace("\\", "/").lstrip("/").lower()


class Pak(object):
    """A pak0.pk3, plus the loose files already staged before it."""

    def __init__(self, path):
        self.zip = zipfile.ZipFile(path)
        self.names = [norm(n) for n in self.zip.namelist()]
        self.by_name = {}
        for original, key in zip(self.zip.namelist(), self.names):
            self.by_name.setdefault(key, original)

    def exists(self, path):
        return norm(path) in self.by_name

    def open(self, path):
        return self.zip.open(self.by_name[norm(path)])

    def resolve_image(self, path):
        """The staged name for an image, trying Q3's extension fallbacks."""
        base, ext = os.path.splitext(norm(path))
        candidates = [base + ext] if ext else []
        candidates += [base + e for e in IMAGE_EXTS]
        for c in candidates:
            if c in self.by_name:
                return c
        return None

    def glob(self, prefix, suffix):
        prefix = norm(prefix)
        return [n for n in self.by_name if n.startswith(prefix) and n.endswith(suffix)]


def plan(pak, mapname, with_models, with_sounds):
    """Everything to extract, as (source entry, staged relative path)."""
    wanted = {}

    def want(src, dst=None):
        wanted.setdefault(norm(dst or src), norm(src))

    bsp = "maps/%s.bsp" % mapname
    if not pak.exists(bsp):
        die("%s is not in this pak (is it the full game? the demo ships 4 maps)" % bsp)
    want(bsp)
    if pak.exists("maps/%s.aas" % mapname):
        want("maps/%s.aas" % mapname)

    data = pak.open(bsp).read()
    shader_names = bsp_shader_names(data)

    # Parse every shader source in the pak, then keep only the blocks this
    # level's geometry actually names.
    definitions = {}
    for entry in pak.glob("scripts/", ".shader"):
        definitions.update(parse_shaders(
            pak.open(entry).read().decode("latin-1", "replace")))

    unresolved = []
    for name in shader_names:
        images = definitions.get(name)
        if images is None and definitions:
            # No shader block: Q3 lets a texture stand alone, addressed by the
            # same path the shader would have had.
            images = [name]
            unresolved.append(name)
        for image in images or []:
            src = pak.resolve_image(image)
            if src:
                want(src)
            elif definitions.get(name) is not None:
                unresolved.append("%s -> %s" % (name, image))

    for entry in pak.glob("scripts/", ".shader"):
        # Only the shader sources that contributed a needed name are worth
        # staging; re-check by re-parsing is overkill, so stage the files that
        # contain at least one wanted definition.
        text = pak.open(entry).read().decode("latin-1", "replace")
        if any(n in text for n in shader_names):
            want(entry)

    for model in bsp_entity_models(data):
        src = pak.by_name.get(norm(model) + ".md3") or pak.by_name.get(norm(model))
        if src:
            want(src)
        # A model is only usable with its skin and animation config.
        for extra in (".skin", ".cfg"):
            if pak.exists(model + extra):
                want(model + extra)

    if with_models:
        for entry in pak.glob("models/players/", ".md3") + \
                pak.glob("models/players/", ".skin") + \
                pak.glob("models/players/", ".jpg") + \
                pak.glob("models/players/", ".tga") + \
                pak.glob("models/weapons2/", ".md3") + \
                pak.glob("models/weapons2/", ".skin") + \
                pak.glob("models/weapons2/", ".tga") + \
                pak.glob("models/weapons2/", ".jpg"):
            want(entry)
    if with_sounds:
        for entry in pak.glob("sound/", ".wav"):
            want(entry)

    return wanted, unresolved, shader_names


def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return "%.1f %s" % (n, unit)
        n /= 1024.0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--pak", default=os.environ.get("MECTOV_PAK0"),
                    help="path to your own pak0.pk3 (or set MECTOV_PAK0)")
    ap.add_argument("--map", default="q3dm1", help="map to stage (default q3dm1)")
    ap.add_argument("--out", default=None, help="staging root (default build/q3data)")
    ap.add_argument("--list", action="store_true", help="print the plan, extract nothing")
    ap.add_argument("--verify", action="store_true",
                    help="after extracting, report names that resolved to nothing")
    ap.add_argument("--with-models", action="store_true", help="also stage player/weapon models")
    ap.add_argument("--with-sounds", action="store_true", help="also stage every sound")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if not args.pak:
        args.pak = os.path.join(root, "assets", "q3", "pak0.pk3")
    if not os.path.exists(args.pak):
        die("no pak0.pk3 at %s — pass --pak or set MECTOV_PAK0.\n"
            "         Quake III Arena's game data is not redistributable; it must\n"
            "         come from your own copy (Steam/GOG/retail CD). See README." % args.pak)
    if not zipfile.is_zipfile(args.pak):
        die("%s is not a zip — a pak0.pk3 is an ordinary zip archive" % args.pak)

    outdir = args.out or os.path.join(root, "build", "q3data")
    baseq3 = os.path.join(outdir, "baseq3")

    pak = Pak(args.pak)
    wanted, unresolved, shader_names = plan(pak, args.map, args.with_models, args.with_sounds)

    print("[q3data] pak      : %s (%s)" % (args.pak, human(os.path.getsize(args.pak))))
    print("[q3data] map      : %s" % args.map)
    print("[q3data] shaders  : %d referenced by the map, %d file(s) to stage"
          % (len(shader_names), len(wanted)))
    if unresolved:
        print("[q3data] unresolved (%d) — textures with no file under any "
              "extension, usually a shader the pak defines elsewhere:" % len(unresolved))
        for u in sorted(unresolved)[:20]:
            print("[q3data]    %s" % u)
        if len(unresolved) > 20:
            print("[q3data]    ... and %d more" % (len(unresolved) - 20))

    if args.list:
        total = 0
        for dst in sorted(wanted):
            try:
                total += pak.zip.getinfo(pak.by_name[dst]).file_size
            except KeyError:
                pass
        print("[q3data] plan: %d file(s), about %s" % (len(wanted), human(total)))
        for dst in sorted(wanted)[:40]:
            print("[q3data]    %s" % dst)
        if len(wanted) > 40:
            print("[q3data]    ... and %d more" % (len(wanted) - 40))
        return

    staged = 0
    total = 0
    for dst in sorted(wanted):
        entry = pak.by_name[dst]
        target = os.path.join(baseq3, dst)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with pak.open(entry) as src, open(target, "wb") as out:
            payload = src.read()
            out.write(payload)
        staged += 1
        total += len(payload)
    print("[q3data] staged %d file(s), %s -> %s" % (staged, human(total), baseq3))
    print("[q3data] next: scripts/seed_ext2.sh ext2.img   "
          "(or ./run.sh, which sizes the volume from this payload)")

    if args.verify:
        bsp_path = os.path.join(baseq3, "maps", args.map + ".bsp")
        with open(bsp_path, "rb") as fh:
            staged_bsp = fh.read()
        missing = []
        for name in bsp_shader_names(staged_bsp):
            base = os.path.join(baseq3, *norm(name).split("/"))
            if not any(os.path.exists(base + e) for e in ("",) + IMAGE_EXTS):
                missing.append(name)
        if missing:
            print("[q3data] VERIFY: %d shader name(s) have no file staged — the "
                  "renderer will draw those faces untextured:" % len(missing))
            for m in missing[:20]:
                print("[q3data]    %s" % m)
        else:
            print("[q3data] VERIFY: every shader name in %s resolved to a file"
                  % os.path.basename(bsp_path))


if __name__ == "__main__":
    main()
