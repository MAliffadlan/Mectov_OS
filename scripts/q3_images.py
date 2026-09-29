#!/usr/bin/env python3
"""scripts/q3_images.py — fresh boot volumes for the Q3 suites (v38.112).

Why this is shared
------------------
Every Q3 suite boots the same two drives: `disk.img` (the MECTOVFS volume whose
node table the GUEST persists with vfs_save) and `ext2.img` (the engine's "CD":
`/ext2/baseq3/...` is the game directory id's own filesystem walks). Both carry
state from earlier runs, and that state has now answered a later suite's question
twice:

  * v38.111 — `disk.img`: a stale `/ext2` subtree is loaded before this run's
    `ext2.img` is even considered, so the guest browsed the previous boot's
    directory tree while reading the new files. Every suite that stages data
    therefore recreates its own images.
  * v38.112 — `ext2.img`: `q3retail` overlays a synthetic `pak0.pk3` whose
    `scripts/mectovtest.shader` REBINDS one texture name to a JPEG. That shader
    stays on the volume after the run, so a later suite that asserts the plain
    name-as-path convention (the generated arena expects `curve` to resolve to
    `curve.tga`, with no script in sight) is then falsified by its predecessor's
    leftovers rather than by anything in the kernel. It shows up as
    "textures/mectovtest/curve resolved to ..._jpg.jpg (jpg), expected the
    direct ..._tga fallback" — a real failure with a harness cause, which is
    exactly what the ordering of a CI job must not decide.

So: a suite whose assertions depend on what is on the volume builds the volume
first, with these two helpers. `fresh_images()` formats both drives (the ext2
one small — the suites stage at most a few MB) and `seed_volume()` runs
`scripts/seed_ext2.sh`, which writes the system blobs, the QVM and the GENERATED
arena into `build/q3data/baseq3` before mirroring it onto the volume. A suite
that stages its own game data then overlays it (see q3retail_test.py).

Usage:
    import q3_images
    err = q3_images.fresh_images(args.disk, args.ext2) or \\
          q3_images.seed_volume(args.ext2)
    if err:
        print(f"[FAIL] {err}")
        return 1

Both functions return None on success and an error string otherwise, so a
caller's failure message says which step broke instead of an exception.
"""
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))

EXT2_MIB = 16            # the generated arena + system blobs need ~2 MB
DISK_SECTORS = 4096      # 2 MB, the size run.sh and check.py use


def _payload_kb():
    """Size of build/q3data, because the volume has to carry it.

    CI stages nothing here (the generated arena is ~14 KB), but a user who ran
    scripts/q3a_data.py has 8+ MB of retail game data in this tree — and
    seed_ext2.sh mirrors ALL of it onto the volume. A fixed 16 MB image then
    fills up mid-seed and the arena's own textures silently fail to land
    (found the honest way: q3arena/q3retail FAILED with `missing=1` on every
    texture the day q3dm1 was staged into build/q3data). Same rule as
    run.sh and check.py: past 8 MB the volume is 512 MB with 4 KB blocks."""
    import os as _os
    root = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)),
                         _os.pardir, "build", "q3data")
    total = 0
    for dirpath, _dirs, files in _os.walk(root):
        for fn in files:
            try:
                total += _os.path.getsize(_os.path.join(dirpath, fn)) // 1024 + 1
            except OSError:
                pass
    return total


def fresh_images(disk="disk.img", ext2="ext2.img", ext2_mib=None):
    """Format both drives the suites boot from. None on success, else why.

    ext2_mib left as None means size-from-payload: 16 MB while build/q3data
    holds only the generated arena, 512 MB with 4 KB blocks once a staged
    retail map lives there (mirrored in by seed_ext2.sh)."""
    if ext2_mib is None:
        ext2_mib = 512 if _payload_kb() > 8192 else 16
    steps = [
        (disk, ["dd", "if=/dev/zero", f"of={disk}", "bs=512",
                f"count={DISK_SECTORS}", "status=none"]),
        (ext2, ["dd", "if=/dev/zero", f"of={ext2}", "bs=1M",
                f"count={ext2_mib}", "status=none"]),
        (ext2, ["mkfs.ext2", "-F"] + (["-b", "4096"] if ext2_mib >= 512 else [])
              + [ext2]),
    ]
    for path, cmd in steps:
        if subprocess.run(cmd, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode != 0:
            return f"could not create {path} ({cmd[0]} failed)"
    return None


def seed_volume(ext2="ext2.img"):
    """Run scripts/seed_ext2.sh over a freshly formatted image."""
    r = subprocess.run(["bash", os.path.join(HERE, "seed_ext2.sh"), ext2])
    if r.returncode != 0:
        return f"seed_ext2.sh failed on {ext2}"
    return None
