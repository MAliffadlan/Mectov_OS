#!/bin/bash
# scripts/build_qvm_ui.sh — build id's CLASSIC Quake III Arena 1.32 user
# interface into Quake VM bytecode (q3ui.qvm) using id's OWN toolchain.
#
# Why this file exists (v38.150), because the obvious plan does not work:
# the retail pak ships vm/ui.qvm, and it is tempting to just run that. But the
# pak's ui.qvm is Team Arena's UI REWRITE — a menu engine whose entire menu
# tree (main menu, single player, setup, controls…) is DATA, loaded from
# ui/*.menu via ui/menus.txt. Measured on the demo pak: zero ui/ entries, zero
# .menu files, zero .txt files beyond scripts/arenas.txt and scripts/bots.txt.
# The GPL source drop ships ui/menus.txt (the list of menu files) but not the
# ~37 .menu files it points at, and the retail paks that carry them are not
# redistributable. So pak0's ui.qvm boots into a UI with no menus in it.
#
# code/q3_ui is the ORIGINAL, hardcoded 1.32 interface: ui_menu.c builds the
# main menu with Menu_AddItem in code, ui_splevel.c the single-player levels,
# ui_setup.c the options, ui_addbots.c the bot list. Its only data is the art
# that IS in the pak (menu/art/*.tga, gfx/2d/bigchars.tga) — so compiling it
# is what makes "the official menu" possible here.
#
# Same toolchain and same shape as scripts/build_qvm.sh: id's lcc compiles each
# .c to bytecode assembly, id's q3asm assembles them into the .qvm, and the
# kernel runs it with id's own interpreter (code/qcommon/vm.c). The module
# list and its ORDER are upstream's q3_ui.sh + q3_ui.q3asm, because q3asm lays
# the modules out in list order and vmMain (in ui_main) must land at bytecode
# instruction 0.
#
# Output: build/vm/q3ui.qvm (+ .map). It is NOT named vm/ui.qvm on purpose: the
# Team Arena bytecode pak0 ships under that name stays available and untouched.
#
# Usage: scripts/build_qvm_ui.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
Q3A="$ROOT/third_party/q3a"
OUT="${Q3VM_OUT:-$ROOT/build/vm}"
TOOLS="$OUT/tools"
BUILD="$OUT/build"
UI="$Q3A/code/q3_ui"
GAME="$Q3A/code/game"

HOSTCC="${HOSTCC:-cc}"

[ -d "$UI" ] || { echo "[qvm-ui] $UI missing — run scripts/vendor_q3a.sh" >&2; exit 1; }
[ -f "$Q3A/code/ui/ui_syscalls.asm" ] || {
    echo "[qvm-ui] code/ui/ui_syscalls.asm missing — run scripts/vendor_q3a.sh" >&2; exit 1; }

mkdir -p "$OUT" "$TOOLS" "$BUILD"

# Nothing to do if the bytecode is newer than every input it is built from.
if [ -f "$OUT/q3ui.qvm" ]; then
    newest=$(find "$UI" "$Q3A/code/ui" "$GAME" "$Q3A/q3asm" \
                  \( -name '*.c' -o -name '*.h' -o -name '*.asm' \) \
                  -newer "$OUT/q3ui.qvm" -print -quit)
    if [ -z "$newest" ] && [ "$0" -ot "$OUT/q3ui.qvm" ]; then
        echo "[qvm-ui] up to date: $OUT/q3ui.qvm"
        exit 0
    fi
fi

# --- id's toolchain, built once and cached (shared with build_qvm.sh) ------
if [ ! -x "$TOOLS/q3asm" ] || [ ! -x "$TOOLS/q3lcc" ] || \
   [ ! -x "$TOOLS/q3rcc" ] || [ ! -x "$TOOLS/q3cpp" ]; then
    echo "[qvm-ui] building id's toolchain (q3asm + lcc)"
    $HOSTCC -O2 -w -o "$TOOLS/q3asm" "$Q3A/q3asm/q3asm.c" "$Q3A/q3asm/cmdlib.c"
    rm -rf "$BUILD/lcc"
    cp -R "$Q3A/lcc" "$BUILD/lcc"
    mkdir -p "$BUILD/sys"
    touch "$BUILD/lcc/lburg/gram.c"
    make -s -C "$BUILD/lcc" all BUILDDIR="$BUILD/sys" >/dev/null
    cp "$BUILD/sys/lcc"  "$TOOLS/q3lcc"
    cp "$BUILD/sys/rcc"  "$TOOLS/q3rcc"
    cp "$BUILD/sys/cpp"  "$TOOLS/q3cpp"
fi
export PATH="$TOOLS:$PATH"

# --- compile the official UI module to bytecode ---------------------------
# Upstream q3_ui.sh's source list, in its order. bg_misc/bg_lib/q_math/q_shared
# come from code/game: the UI links the same background-module helpers the game
# module does (item names, rank strings, maths).
Q3_UI_MODULES="ui_main ui_cdkey ui_ingame ui_confirm ui_setup \
bg_misc bg_lib q_math q_shared \
ui_gameinfo ui_atoms ui_connect ui_controls2 ui_demo2 ui_mfield ui_credits \
ui_menu ui_options ui_display ui_sound ui_network ui_playermodel ui_players \
ui_playersettings ui_preferences ui_qmenu ui_serverinfo ui_servers2 \
ui_sparena ui_specifyserver ui_splevel ui_sppostgame ui_startserver \
ui_team ui_video ui_cinematics ui_spskill ui_addbots ui_removebots \
ui_loadconfig ui_saveconfig ui_teamorders ui_mods"

WORK="$OUT/q3ui"
rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK"

LCC="q3lcc -DQ3_VM -S -Wf-target=bytecode -Wf-g -I$UI -I$GAME -I$Q3A/code/cgame -I$Q3A/code/ui"
for m in $Q3_UI_MODULES; do
    src="$UI/$m.c"
    [ -f "$src" ] || src="$GAME/$m.c"     # the four shared bg_/q_ modules
    $LCC "$src"
done

# The trap stubs. Upstream assembles them as ../../ui/ui_syscalls (ui.def's
# import list in code/ui/ui_syscalls.asm); the module list names it ui_syscalls.
cp "$Q3A/code/ui/ui_syscalls.asm" .

{
    echo "-o q3ui.qvm"
    for m in $Q3_UI_MODULES; do
        echo "$m"
    done
    echo "ui_syscalls"
} > q3ui.q3asm

q3asm -f q3ui.q3asm

cp q3ui.qvm "$OUT/q3ui.qvm"
cp q3ui.map "$OUT/q3ui.map" 2>/dev/null || true
echo "[qvm-ui] built $OUT/q3ui.qvm ($(wc -c < "$OUT/q3ui.qvm") bytes)"