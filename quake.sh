#!/bin/bash
# quake.sh — one command from "I have the demo zip" to playing it on Mectov OS.
#
# What this is NOT: it does not ship, download or bundle any id Software data.
# `pak0.pk3` is the user's own copy — git-ignored at assets/q3/ — and this
# script's job is the mechanical chain around it:
#
#   pak0.pk3  ->  q3a_data.py (stage ONE map and its textures)
#             ->  MECTOV_Q3=1 make iso   (the kernel with the Q3 client in it)
#             ->  MECTOV_Q3=1 ./run.sh   (QEMU, the real desktop)
#   then: SPACE -> password mectov123 -> Terminal -> `q3arena <map>`
#
# The default ISO (`make` / `./run.sh` without MECTOV_Q3=1) stays lean: the Q3
# client and its third-party tree only enter the build under that flag.
#
# Usage:
#   ./quake.sh                 # stage + play q3dm1 (the demo's first map)
#   ./quake.sh q3dm7           # or q3dm17 / q3tourney2 — any staged map name
#   MECTOV_KVM=0 ./quake.sh    # force TCG (run.sh reads this too)
#   MECTOV_AUDIO=none ./quake.sh
#   MECTOV_QUAKE_DRY=1 ./quake.sh   # do everything EXCEPT launching QEMU
#   MECTOV_GPU=1 ./quake.sh         # attach a virtual GPU (boot, then try `gpustat`)
#
# NOTE: plain `MECTOV_GPU=1 ./run.sh` (without MECTOV_Q3=1) is the classic trap:
# run.sh always does a full clean rebuild, and without MECTOV_Q3=1 in the
# environment that rebuild ships WITHOUT the Quake engine — `q3arena` then
# honestly says "engine not compiled in". Go through this script (or add
# MECTOV_Q3=1 Q3ARENA_SCALE=2 yourself) and the engine is in the build.
#
# Re-staging only happens when the map (or, since v38.124, the weapon view
# model) is not on the staged tree yet; delete build/q3data to force it, or drop
# a retail pak0.pk3 at assets/q3/pak0.pk3 and stage any map the full game ships.
set -u

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT" || exit 1

MAP="${1:-q3dm1}"
PAK="${MECTOV_PAK0:-$ROOT/assets/q3/pak0.pk3}"
DEMO_ZIP="${MECTOV_DEMO_ZIP:-$HOME/Downloads/Quake 3 Arena Demo.zip}"
STAGED="$ROOT/build/q3data/baseq3/maps/$MAP.bsp"
# v38.124: `--with-viewmodel` is what puts id's own machinegun .md3 on the
# volume. A tree staged before that flag existed has the map but not the model,
# and shipping a silent fallback gun to the one person this is for would be the
# same mistake v38.123 made — so the check is "map AND model", not just the map.
STAGED_VM="$ROOT/build/q3data/baseq3/models/weapons2/machinegun/machinegun.md3"

say() { echo "[quake] $*"; }

# ---- 1. the game data: the user's own pak0.pk3 ------------------------------
if [ ! -f "$PAK" ]; then
    if [ -f "$DEMO_ZIP" ]; then
        say "pak0.pk3 belum ada — mengekstrak dari demo zip:"
        say "  $DEMO_ZIP"
        mkdir -p "$(dirname "$PAK")"
        if ! python3 - "$DEMO_ZIP" "$PAK" <<'PY'
import os, sys, zipfile
src, dst = sys.argv[1], sys.argv[2]
want = [n for n in zipfile.ZipFile(src).namelist()
        if n.lower().endswith("demoq3/pak0.pk3")]
if not want:
    sys.exit("no demoq3/pak0.pk3 inside %s" % src)
z = zipfile.ZipFile(src)
entry = want[0]                     # the outer zip holds it twice; either is fine
h = 0
with z.open(entry) as f, open(dst, "wb") as out:
    while True:
        b = f.read(1 << 20)
        if not b:
            break
        out.write(b)
print("[quake]   extracted %s (%d bytes)" % (entry, os.path.getsize(dst)))
PY
        then
            say "ekstraksi gagal — $PAK tidak jadi. Cek isi zip-nya, atau pakai"
            say "pak0.pk3 sendiri: MECTOV_PAK0=/path/ke/pak0.pk3 ./quake.sh"
            exit 1
        fi
    else
        cat <<EOF
[quake] pak0.pk3 tidak ditemukan: $PAK

Quake III Arena's game data is NOT redistributable and this repo ships none of
it — but id released the DEMO for free, and its data is enough for this port:

  1. Get "Quake 3 Arena Demo.zip" (97,722,669 bytes, md5 46cc890aa685b5fe083190f6d8ac38db)
     from the archive.org item "Q3A-Demo" — or use your own retail pak0.pk3.
  2. Either drop it at   assets/q3/pak0.pk3  (git-ignored), or point this
     script at your copy:  MECTOV_PAK0=/path/to/pak0.pk3 ./quake.sh
     (or let this script extract it:  MECTOV_DEMO_ZIP=/path/to/demo.zip ./quake.sh)

See the README's "Game data" section for the same steps in detail.
EOF
        exit 1
    fi
fi
say "pak0.pk3: $PAK ($(stat -c%s "$PAK") bytes)"

# ---- 2. stage the map (and its textures) onto the volume tree ---------------
if [ ! -f "$STAGED" ] || [ ! -f "$STAGED_VM" ]; then
    say "staging map '$MAP' (bsp + its shader scripts + the images they name)"
    say "  + the machinegun view model (--with-viewmodel)..."
    python3 scripts/q3a_data.py --pak "$PAK" --map "$MAP" \
        --out build/q3data --verify --with-viewmodel || exit 1
else
    say "map '$MAP' sudah ter-stage: $STAGED"
fi

# ---- 3. build the Q3 ISO, then hand over to the normal loader -------------
# Q3ARENA_SCALE=2: render tetap 320x240 (fps tidak berubah), tapi jendela dan
# blit-nya 2x — 640x480, gaya DOOM. Override: Q3ARENA_SCALE=1 ./quake.sh
say "building the MECTOV_Q3=1 ISO (incremental; first build takes a while)..."
# EXPORTED, not prefix-assigned: run.sh rebuilds again from scratch, and the
# scale must survive that rebuild (a prefix assignment lives only for this one
# command — v38.115 regression where the ISO silently came out at scale 1 and
# the window looked blurry at 322x262 instead of 642x502).
export Q3ARENA_SCALE="${Q3ARENA_SCALE:-2}"
MECTOV_Q3=1 make iso || exit 1

cat <<EOF

[quake] Siap. Yang bakal kamu lihat di QEMU:
  1. Login screen -> tekan SPACE, password: mectov123, lalu Enter.
  2. Di desktop, klik icon "Terminal" (kiri atas).
  3. Di Terminal Mectov, ketik:   q3arena $MAP
  4. Kontrol: W/panah atas = maju, S = mundur, A/D = geser, panah kiri/kanan
     = putar, mouse = lihat, ESC = keluar dari peta.
     Karakter jalan sendiri sampai kamu tekan tombol (mode demo).
  5. Yang sudah ada: fps di pojok kanan atas saja (sejak v38.126 — teksnya
     font AA kernel yang halus, digambar setelah upscale jadi nggak ikut
     dikotak-kotakin; F3 = panel detail vm/gl/blit/wm/other kalau lu mau
     ngukur), lompat (SPACE), duck (C), nembak (klik kiri /
     CTRL), dan senjata first-person id yang asli (models/weapons2/machinegun,
     dari demo pak0 — muzzle flash ikut nyala saat nembak, dan sejak v38.125
     flash-nya di-blend additive seperti shader id sendiri, jadi nggak lagi
     nutup layar hitam). Tekstur peta juga udah lewat jalur shader script yang
     benar: langit, lava, dan api obor ke-load dari demo pak0.
     Yang BELUM ada: tangan (model pemain, upper.md3), suara, menu, animasi
     view model, dan stage shader lanjutan (tcMod scroll/scale, blend 2 stage,
     skybox env/*) — jadi langit & lava masih diam, nggak gerak kayak retail.
     Peta q3dm1 sendiri 2.000+ wajah di software renderer, jadi di TCG
     hitungannya ~0.2-2 fps: lambat itu batas emulasi QEMU, bukan crash.
     Sejak v38.125 jam game-nya juga nggak merayap lagi di host selambat itu:
     dulu tick-nya kepotong tiap frame kalau frame-nya lewat 250 ms, jadi
     dunia jalan ~1/10 kecepatan asli (di host kencang, 40+ fps, nggak
     kelihatan sama sekali).
     Log seri: serial_debug.log (atau /tmp/mectov_q3*_serial.log).

EOF

if [ "${MECTOV_QUAKE_DRY:-0}" = "1" ]; then
    say "MECTOV_QUAKE_DRY=1: berhenti di sini. Yang akan dijalankan:"
    say "  MECTOV_Q3=1 ./run.sh"
    exit 0
fi

MECTOV_Q3=1 exec ./run.sh
