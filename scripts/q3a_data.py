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
    scripts/q3a_data.py --pak ... --with-viewmodel
    scripts/q3a_data.py --pak ... --with-hud
    scripts/q3a_data.py --pak ... --with-models --with-sounds

v38.124: --with-viewmodel stages models/weapons2/machinegun/* — the three .md3
parts and the two JPEGs the port's own view model draws (q3viewmodel.c). The
Quake III Arena DEMO pak0 ships it, so `--with-viewmodel` on a demo install is
the whole first-person weapon.

v38.127: --with-hud stages the pictures id's own status bar is drawn from — the
11 number fields, the bigchars atlas, the score box's select overlay and the
armor/ammo icons (q3hud.c). They are listed by name because nothing in the .bsp
refers to them; the engine's cgame registers them itself.

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


# v38.127: every picture id's own status bar draws, by the name cg_main.c and
# cg_weapons.c register them under (no extension — resolve_image() probes the
# extension id actually shipped, which is .tga for all of these). The number
# fields are sb_nums[] (cg_main.c:826-838), the charset is CG_DrawChar's 16x16
# atlas, select is the score box's "this one is yours" overlay, and the icons
# are CG_DrawStatusBar's armor icon and one ammo icon per weapon.
HUD_IMAGES = [
    "gfx/2d/numbers/zero_32b",
    "gfx/2d/numbers/one_32b",
    "gfx/2d/numbers/two_32b",
    "gfx/2d/numbers/three_32b",
    "gfx/2d/numbers/four_32b",
    "gfx/2d/numbers/five_32b",
    "gfx/2d/numbers/six_32b",
    "gfx/2d/numbers/seven_32b",
    "gfx/2d/numbers/eight_32b",
    "gfx/2d/numbers/nine_32b",
    "gfx/2d/numbers/minus_32b",
    "gfx/2d/bigchars",
    "gfx/2d/select",
    "icons/iconr_yellow",
    "icons/icona_machinegun",
    "icons/icona_shotgun",
    "icons/icona_grenade",
    "icons/icona_rocket",
    "icons/icona_lightning",
    "icons/icona_railgun",
    "icons/icona_plasma",
    "icons/icona_bfg",
]


# v38.141: the demo's full map list (scripts/arenas.txt). --all stages all of
# these; the default stays one map so CI keeps its fast path.
ALL_MAPS = ("q3dm1", "q3dm17", "q3dm7", "q3tourney2")


def plan(pak, mapname, with_models, with_sounds, with_viewmodel=False,
         with_hud=False):
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

    if with_viewmodel:
        # v38.124: the weapon the port actually holds. One weapon's worth of
        # .md3 + textures is ~48 KB, against megabytes for --with-models' whole
        # player/weapon set — and q3viewmodel.c needs exactly these files:
        # the body, its _barrel and _flash parts and the two JPEGs they name.
        for entry in pak.glob("models/weapons2/machinegun/", ".md3") + \
                pak.glob("models/weapons2/machinegun/", ".skin") + \
                pak.glob("models/weapons2/machinegun/", ".tga") + \
                pak.glob("models/weapons2/machinegun/", ".jpg"):
            want(entry)

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
        # v38.141: a player model is only usable with its animation config.
        for entry in pak.glob("models/players/", ".cfg"):
            want(entry)
    if with_hud:
        # v38.127: id's own status bar (third_party/tinygl/q3hud.c). 22 pictures,
        # ~330 KB against this pak's 46 MB — and unlike the world's textures
        # these are NOT reachable from the .bsp: nothing in the level names
        # them, so they have to be listed. The eleven number fields are what a
        # health/ammo/armor readout is drawn from, so a status bar without them
        # is not a smaller status bar, it is no status bar.
        for base in HUD_IMAGES:
            src = pak.resolve_image(base)
            if src:
                want(src)

    if with_sounds:
        for entry in pak.glob("sound/", ".wav"):
            want(entry)

    return wanted, unresolved, shader_names


def plan_full(pak):
    """Non-map extras for --all, as (source entry, staged relative path).

    Data only: nothing here needs engine code, and nothing here changes what
    the default single-map path stages. Small text/config plus id's prebuilt
    cgame/ui bytecode (runs in the VM interpreter the port already has — the
    game logic itself stays built from source as qagame.qvm)."""
    wanted = {}

    def want(src, dst=None):
        wanted.setdefault(norm(dst or src), norm(src))

    def files(prefix):
        # glob("") matches directories too; those are not extractable.
        return [e for e in pak.glob(prefix, "") if not e.endswith("/")]

    # Match-end stingers (no CD audio in the demo; these two wavs are it).
    for entry in pak.glob("music/", ".wav"):
        want(entry)
    # Map preview art (for a future map picker).
    for entry in pak.glob("levelshots/", ".jpg") + \
            pak.glob("levelshots/", ".tga"):
        want(entry)
    # Menu art + data (menu/*, full gfx/icons, every shader). Data only.
    for entry in files("menu/"):
        want(entry)
    for entry in pak.glob("gfx/", ".jpg") + pak.glob("gfx/", ".tga") + \
            pak.glob("icons/", ".jpg") + pak.glob("icons/", ".tga"):
        want(entry)
    for entry in pak.glob("scripts/", ".shader"):
        want(entry)
    for name in ("scripts/arenas.txt", "scripts/bots.txt"):
        if pak.exists(name):
            want(name)
    # Bot personalities (runtime definitions; the .c framework is engine
    # sources and stays out of the data tree).
    for entry in files("botfiles/bots/"):
        want(entry)
    # Recorded demos + id's own intro movies (future player/playback work).
    for entry in pak.glob("video/", ".roq") + pak.glob("demos/", ".dm3"):
        want(entry)
    # id's prebuilt client/menu bytecode (runs in the existing VM
    # interpreter; qagame stays built from source).
    for entry in ("vm/cgame.qvm", "vm/ui.qvm"):
        if pak.exists(entry):
            want(entry)
    return wanted


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
    ap.add_argument("--with-viewmodel", action="store_true",
                    help="also stage the machinegun view model (v38.124: the .md3 "
                         "parts and textures the first-person gun is drawn from)")
    ap.add_argument("--with-hud", action="store_true",
                    help="also stage id's status bar art (v38.127: the number "
                         "fields, charset, armor/ammo icons and the score-box "
                         "overlay q3hud.c draws health/armor/ammo/score from)")
    ap.add_argument("--with-sounds", action="store_true", help="also stage every sound")
    ap.add_argument("--all", action="store_true",
                    help="stage everything the demo ships (v38.141: all 4 maps, "
                         "player models, sounds, HUD, menu art, bot data, "
                         "movies, prebuilt cgame/ui). The default stays one "
                         "map so CI keeps its fast path.")
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
    if args.all:
        args.with_models = args.with_sounds = True
        args.with_viewmodel = args.with_hud = True
        maps = [m for m in ALL_MAPS if pak.exists("maps/%s.bsp" % m)]
        if not maps:
            die("no demo maps in this pak (tried %s)" % ", ".join(ALL_MAPS))
    else:
        maps = [args.map]
    wanted = {}
    unresolved = []
    shader_names = []
    for mapname in maps:
        w, u, s = plan(pak, mapname, args.with_models, args.with_sounds,
                       args.with_viewmodel, args.with_hud)
        wanted.update(w)
        unresolved += u
        shader_names += s
    n_full = 0
    if args.all:
        full = plan_full(pak)
        n_full = len(full)
        wanted.update(full)

    print("[q3data] pak      : %s (%s)" % (args.pak, human(os.path.getsize(args.pak))))
    print("[q3data] map      : %s" % (", ".join(maps) +
          (" + full demo data (%d extra files)" % n_full if args.all else "")))
    print("[q3data] shaders  : %d referenced by the map(s), %d file(s) to stage"
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
        for vmap in maps:
            bsp_path = os.path.join(baseq3, "maps", vmap + ".bsp")
            with open(bsp_path, "rb") as fh:
                staged_bsp = fh.read()
            missing = []
            for name in bsp_shader_names(staged_bsp):
                base = os.path.join(baseq3, *norm(name).split("/"))
                if not any(os.path.exists(base + e) for e in ("",) + IMAGE_EXTS):
                    missing.append(name)
            if missing:
                print("[q3data] VERIFY %s: %d shader name(s) have no file staged — the "
                      "renderer will draw those faces untextured:" % (vmap, len(missing)))
                for m in missing[:20]:
                    print("[q3data]    %s" % m)
            else:
                print("[q3data] VERIFY %s: every shader name resolved to a file" % vmap)


if __name__ == "__main__":
    main()
