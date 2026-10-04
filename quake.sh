#!/bin/bash
# quake.sh — one command from "I have the demo zip" to playing it on Mectov OS.
#
# What this is NOT: it does not ship, download or bundle any id Software data.
# `pak0.pk3` is the user's own copy — git-ignored at assets/q3/ — and this
# script's job is the mechanical chain around it:
#
#   pak0.pk3  ->  q3a_data.py --all (stage the full demo: all 4 maps plus
#                player models, menu art, bot/arena data, movies, cgame/ui)
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
#   ./quake.sh --fast          # no build at all: reuse the existing mectov.iso
#                              # (the no-rebuild start; needs one real build first)
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

# Argumen: [--fast] [<map>], urutan bebas. MECTOV_QUAKE_FAST=1 sama dengan
# --fast: lewati semua build (make + grub-mkrescue) dan langsung main dengan
# ISO yang sudah ada — supaya mulai main tidak bayar rebuild penuh tiap kali.
FAST=0
MAP=""
for a in "$@"; do
    case "$a" in
        --fast) FAST=1 ;;
        *)      MAP="$a" ;;
    esac
done
MAP="${MAP:-q3dm1}"
[ "${MECTOV_QUAKE_FAST:-0}" = "1" ] && FAST=1
PAK="${MECTOV_PAK0:-$ROOT/assets/q3/pak0.pk3}"
DEMO_ZIP="${MECTOV_DEMO_ZIP:-$HOME/Downloads/Quake 3 Arena Demo.zip}"
STAGED="$ROOT/build/q3data/baseq3/maps/$MAP.bsp"
# v38.124: `--with-viewmodel` is what puts id's own machinegun .md3 on the
# volume. A tree staged before that flag existed has the map but not the model,
# and shipping a silent fallback gun to the one person this is for would be the
# same mistake v38.123 made — so the check is "map AND model", not just the map.
STAGED_VM="$ROOT/build/q3data/baseq3/models/weapons2/machinegun/machinegun.md3"
# v38.127: the same rule for the status bar. id's HUD art is what health, armor,
# ammo and the score are drawn from (q3hud.c), and a tree staged before that
# existed would run with no HUD at all — the map and the gun are not enough.
STAGED_HUD="$ROOT/build/q3data/baseq3/gfx/2d/numbers/zero_32b.tga"
# v38.129: the same rule for the sounds. The driver names sixteen map sfx by
# path and plays them through the SB16; a tree staged before this release has
# the map, the gun and the HUD but no sounds at all.
STAGED_SND="$ROOT/build/q3data/baseq3/sound/world/jumppad.wav"
# v38.141: full-demo staging. Beyond the map, the gun, the HUD and the sounds,
# a complete tree also has the player models (bots-to-be need bodies), the
# arena/bot definitions, id's prebuilt cgame/ui bytecode and the other three
# demo maps. Same rule: if any marker is missing, re-stage everything.
STAGED_PLAYERS="$ROOT/build/q3data/baseq3/models/players/visor/animation.cfg"
STAGED_ARENAS="$ROOT/build/q3data/baseq3/scripts/arenas.txt"
STAGED_CGAME="$ROOT/build/q3data/baseq3/vm/cgame.qvm"

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
if [ ! -f "$STAGED" ] || [ ! -f "$STAGED_VM" ] || [ ! -f "$STAGED_HUD" ] || [ ! -f "$STAGED_SND" ] || [ ! -f "$STAGED_PLAYERS" ] || [ ! -f "$STAGED_ARENAS" ] || [ ! -f "$STAGED_CGAME" ]; then
    say "staging map '$MAP' (bsp + its shader scripts + the images they name)"
    say "  + the machinegun view model (--with-viewmodel)..."
    say "  + id's status bar art (--with-hud)..."
    say "  + the map's sounds (--with-sounds)..."
    say "  + full demo data (--all: all 4 maps, player models, menu art,"
    say "    bot/arena definitions, movies, prebuilt cgame/ui)..."
    python3 scripts/q3a_data.py --pak "$PAK" --map "$MAP" \
        --out build/q3data --verify --with-viewmodel --with-hud --with-sounds --all || exit 1
else
    say "map '$MAP' sudah ter-stage: $STAGED"
fi

# ---- 3. build the Q3 ISO, then hand over to the normal loader -------------
# Q3ARENA_SCALE=2: render tetap 320x240 (fps tidak berubah), tapi jendela dan
# blit-nya 2x — 640x480, gaya DOOM. Override: Q3ARENA_SCALE=1 ./quake.sh
# EXPORTED, not prefix-assigned: run.sh rebuilds again from scratch, and the
# scale must survive that rebuild (a prefix assignment lives only for this one
# command — v38.115 regression where the ISO silently came out at scale 1 and
# the window looked blurry at 322x262 instead of 642x502).
export Q3ARENA_SCALE="${Q3ARENA_SCALE:-2}"
if [ "$FAST" = "1" ]; then
    # Jalur cepat: tidak ada make/clean_all/grub-mkrescue. run.sh melewati
    # build lewat MECTOV_SKIP_BUILD=1, volume tetap di-seed, ISO langsung pakai.
    if [ ! -f mectov.iso ]; then
        say "--fast diminta tapi mectov.iso belum ada — build dulu sekali tanpa --fast."
        exit 1
    fi
    if command -v strings >/dev/null 2>&1 && strings -a myos.bin 2>/dev/null | grep -q 'engine not compiled in'; then
        say "PERINGATAN: ISO ini dibuild TANPA MECTOV_Q3=1 — 'q3arena' bakal bilang"
        say "            \"engine not compiled in\". Build sekali tanpa --fast dulu."
    fi
    export MECTOV_SKIP_BUILD=1
    say "fast start: skip build, pakai mectov.iso ($(stat -c%s mectov.iso) bytes, $(date -r mectov.iso '+%d %b %H:%M'))"
else
    say "building the MECTOV_Q3=1 ISO (incremental; first build takes a while)..."
    MECTOV_Q3=1 make iso || exit 1
fi

cat <<EOF

[quake] Siap. Yang bakal kamu lihat di QEMU:
  1. Login screen -> tekan SPACE, password: mectov123, lalu Enter.
  2. Di desktop, klik icon "Terminal" (kiri atas).
  3. Di Terminal Mectov, ketik:   q3arena $MAP nolimit
     (tanpa \`nolimit\` sesinya berhenti sendiri di ~3 menit — frame 10800 — dan
      itu kelihatan seperti game yang tiba-tiba mati; dengan \`nolimit\` jalan
      terus sampai ESC. Log sesi 11:20 pemain sendiri tertulis nolimit=0.)
     Kalau jendela QEMU-nya ketutupan window lain (terminal/editor/browser):
     scripts/quake_focus.py menariknya ke depan, `--pin` menguncinya di atas.
     Sejak v38.135 window game juga naik sendiri tiap kali kamu gerakkan mouse
     atau tekan tombol di dalamnya.
     MOUSE (v38.149): kalau pandangan cuma jalan dikit lalu BERHENTI sama sekali
     — atau patah-patah pas digerakkan pelan — jendela QEMU-nya belum nge-grab
     pointer-nya. Script ini mengirim satu klik sintetis ~34 detik setelah start
     untuk memicu grab itu (klik juga memfokuskan jendelanya). Manual: klik
     sekali di dalam jendela QEMU. Lepas dari grab: Ctrl+Alt+G.
     Sejak v38.133 load-nya ngomong: window loading menampilkan stage +
     hitungan detik, dan terminal mencetak baris [quake] ... tiap ganti tahap.
     ESC kapan saja selama loading = batal dan balik ke desktop.
  4. Kontrol: W/panah atas = maju, S = mundur, A/D = geser, panah kiri/kanan =
     putar, mouse = lihat, SPACE = lompat, C = duck, klik kiri / CTRL = nembak,
     ESC = keluar dari peta. Karakter jalan sendiri sampai kamu tekan tombol.
   5. Yang sudah ada: HUD id sendiri (health/armor/ammo/score dari gfx/2d pak0),
      game tanpa suara (v38.142, mixer dicabut total), langit dome + awan
      yang scroll (skyparms), view model machinegun id yang asli
     (models/weapons2, dari demo pak0) plus muzzle flash additive, dan fps di
     pojok kanan atas apa adanya (F3 = panel detail vm/gl/blit/wm/other).
     Yang BELUM ada: tangan/model pemain, menu, animasi view model, bot/lawan,
     item pickup, pintu/lift, dan jaringan — jadi isinya masih jalan-jalan +
     nembak, bukan match penuh.
  6. Catatan bug yang masih ada: main -> ESC -> main lagi aman (2x per sesi);
     q3arena yang KETIGA dalam satu boot masih panic (bug lama, bukan v38.133).
     Kalau mau main lagi setelah itu: tutup QEMU, lalu ./quake.sh --fast.
  7. Log seri: serial_debug.log (atau /tmp/mectov_q3*_serial.log).

EOF

if [ "${MECTOV_QUAKE_DRY:-0}" = "1" ]; then
    say "MECTOV_QUAKE_DRY=1: berhenti di sini. Yang akan dijalankan:"
    if [ "$FAST" = "1" ]; then
        say "  MECTOV_SKIP_BUILD=1 MECTOV_Q3=1 ./run.sh"
    else
        say "  MECTOV_Q3=1 ./run.sh"
    fi
    exit 0
fi

# v38.135: jangan biarkan jendela VM terkubur. Desktop ini penuh window
# maximized (terminal, editor, browser, Telegram), dan jendela yang ketutupan
# TIDAK menerima satu pun event mouse/keyboard — guest-nya jalan terus, tapi
# tidak bisa dilihat, dan itu pernah dilaporkan sebagai "game-nya nggak
# muncul". run.sh mem-`exec` QEMU di foreground, jadi dorongannya di background:
# beberapa kali aktivasi di setengah menit pertama, TANPA --pin supaya cuma
# dorongan, bukan kunci. Sesudahnya bisa dipanggil manual:
#   python3 scripts/quake_focus.py            (aktifkan + always-on-top)
#   python3 scripts/quake_focus.py --no-pin   (aktifkan saja)
#   python3 scripts/quake_focus.py --off      (lepas always-on-top)
if [ -n "${DISPLAY:-}" ] && [ -f scripts/quake_focus.py ]; then
    ( for _ in 1 2 3 4 5 6; do
          sleep 4
          python3 scripts/quake_focus.py --no-pin >/dev/null 2>&1
      done
      # v38.149: activation alone was not enough. The guest takes the mouse
      # capture when the GAME starts, and the load is ~27 s on this host, so the
      # click that makes the frontend GRAB the pointer has to come after that,
      # not with the early nudges above (those fire at 4..24 s, while the load
      # is still running). Without the grab, motion is mapped from the host
      # pointer's position inside the window: coarse when you move slowly, and
      # dead the moment the pointer reaches the window edge. That is the
      # "nengok pelan pake mouse patah patah" report, and the player's 12:46
      # session is the measurement — the capture was held for the whole run
      # while zero pointer events arrived (look_s 32,26,19,30 then 0 for 103
      # windows; idle_in 370310 ms).
      # Two attempts, 10 s apart, to cover a load that runs long.
      for _ in 1 2; do
          sleep 10
          python3 scripts/quake_focus.py --no-pin --grab >/dev/null 2>&1
      done ) &
fi

MECTOV_Q3=1 exec ./run.sh
