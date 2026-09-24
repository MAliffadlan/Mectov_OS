#!/bin/bash
# scripts/seed_ext2.sh — seed system blobs into an ext2 image (debloat, v38.81).
#
# The kernel no longer embeds doom1.wad / wallpaper.bin / music.wav (7+ MB of
# always-resident .data); it loads them on demand from /ext2 instead. This
# script writes those files into a freshly mkfs'd image with host debugfs
# (e2fsprogs, already a build dep for mkfs.ext2). Idempotent per file:
# existing entries are deleted first so re-seeding never duplicates.
#
# Usage: scripts/seed_ext2.sh <ext2.img>
# Sources: ./doom1.wad, ./apps/music.wav, obj/wallpaper.bin (built on the
# fly via build_wallpaper.py when missing, e.g. run.sh seeds before make).
set -u
IMG="${1:?usage: seed_ext2.sh <ext2.img>}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [ ! -f "$IMG" ]; then
    echo "[seed] $IMG missing — skipping (guest falls back gracefully)" >&2
    exit 0
fi

WP="$ROOT/obj/wallpaper.bin"
if [ ! -f "$WP" ]; then
    mkdir -p "$ROOT/obj"
    if ! python3 "$ROOT/scripts/build_wallpaper.py" "$ROOT/assets/wallpaper.png" "$WP" >/dev/null 2>&1; then
        echo "[seed] wallpaper build failed — skipping wallpaper" >&2
        WP=""
    fi
fi

seed_one() { # $1 = host path, $2 = ext2 name
    [ -f "$1" ] || { echo "[seed] missing $1 — skipping $2" >&2; return 0; }
    debugfs -w -R "rm $2" "$IMG" >/dev/null 2>&1  # idempotent re-seed
    if debugfs -w -R "write $1 $2" "$IMG" >/dev/null 2>&1; then
        echo "[seed] $2 <- $1"
    else
        echo "[seed] FAILED writing $2" >&2
        return 1
    fi
}

seed_one "$ROOT/doom1.wad" doom1.wad
seed_one "$WP" wallpaper.bin
seed_one "$ROOT/apps/music.wav" music.wav
# Q3 phase 4 (v38.104): the Mectov map file — /ext2/mectov1.map, staged by
# q3play_start() into the engine FS as baseq3/maps/mectov1.map.
seed_one "$ROOT/assets/maps/mectov1.map" mectov1.map
echo "[seed] done: $(debugfs -R 'ls' "$IMG" 2>/dev/null | tr '\n' ' ')"
