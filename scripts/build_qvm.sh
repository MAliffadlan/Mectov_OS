#!/bin/bash
# scripts/build_qvm.sh — build the official Quake III Arena game module into
# Quake VM bytecode (qagame.qvm) using id Software's OWN toolchain.
#
# Upstream is third_party/q3a (id-Software/Quake-III-Arena, see UPSTREAM.md):
#   q3lcc  compiles each official code/game/*.c to bytecode assembly (.asm)
#   q3asm  assembles those .asm files into qagame.qvm
#
# The result is genuine Quake III Arena game bytecode: the same .qvm the retail
# engine loads, produced from unmodified id source by unmodified id tools. The
# Mectov kernel executes it with the official interpreter (code/qcommon/vm.c +
# vm_interpreted.c). No game data is involved — bytecode needs none.
#
# Output: obj/vm/qagame.qvm  and  obj/vm/qagame.map (symbol names, so the
# kernel can print real function names while it runs the VM).
#
# Usage: scripts/build_qvm.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
Q3A="$ROOT/third_party/q3a"
# Output deliberately lives outside obj/: flipping MECTOV_Q3 makes the build
# system wipe obj/ wholesale (v38.99 stamp), and the bytecode has to outlive
# that — the two ISO variants are built back to back, and the ext2 image is
# seeded with the .qvm afterwards.
OUT="${Q3VM_OUT:-$ROOT/build/vm}"
TOOLS="$OUT/tools"
BUILD="$OUT/build"
GAME="$Q3A/code/game"

HOSTCC="${HOSTCC:-cc}"

[ -d "$GAME" ] || { echo "[qvm] $GAME missing — run scripts/vendor_q3a.sh" >&2; exit 1; }

mkdir -p "$OUT" "$TOOLS" "$BUILD"

# Nothing to do if the bytecode is newer than every input it is built from.
if [ -f "$OUT/qagame.qvm" ]; then
    # -print -quit stops at the first hit: piping into `head` would trip
    # `set -o pipefail` with SIGPIPE the moment find has more to say.
    newest=$(find "$GAME" "$Q3A/q3asm" \
                  \( -name '*.c' -o -name '*.h' -o -name '*.asm' \) \
                  -newer "$OUT/qagame.qvm" -print -quit)
    if [ -z "$newest" ] && [ "$0" -ot "$OUT/qagame.qvm" ]; then
        echo "[qvm] up to date: $OUT/qagame.qvm"
        exit 0
    fi
fi

# --- id's toolchain, built once and cached in obj/ -------------------------
if [ ! -x "$TOOLS/q3asm" ] || [ ! -x "$TOOLS/q3lcc" ] || \
   [ ! -x "$TOOLS/q3rcc" ] || [ ! -x "$TOOLS/q3cpp" ]; then
    echo "[qvm] building id's toolchain (q3asm + lcc)"
    # q3asm is three plain C files.
    $HOSTCC -O2 -w -o "$TOOLS/q3asm" "$Q3A/q3asm/q3asm.c" "$Q3A/q3asm/cmdlib.c"

    # lcc is built in a scratch copy so third_party/q3a stays exactly as
    # vendored/modified-only-by-this-script. BUILDDIR redirects every object,
    # generated .c and the four binaries away from the source tree.
    rm -rf "$BUILD/lcc"
    cp -R "$Q3A/lcc" "$BUILD/lcc"
    mkdir -p "$BUILD/sys"
    # Upstream commits lburg/gram.c (the yacc output) but make still prefers to
    # regenerate it from gram.y, which needs a yacc this project does not
    # depend on. Refreshing the committed file's timestamp defeats that rule
    # without modifying a byte of upstream source.
    touch "$BUILD/lcc/lburg/gram.c"
    make -s -C "$BUILD/lcc" all BUILDDIR="$BUILD/sys" >/dev/null
    cp "$BUILD/sys/lcc"  "$TOOLS/q3lcc"
    cp "$BUILD/sys/rcc"  "$TOOLS/q3rcc"
    cp "$BUILD/sys/cpp"  "$TOOLS/q3cpp"
fi
export PATH="$TOOLS:$PATH"

# --- compile the official game module to bytecode --------------------------
# Source list and ORDER are upstream's code/game/game.sh + game.q3asm. The
# order is load-bearing: q3asm lays the modules out in list order and the VM's
# entry point (vmMain, in g_main) must land at bytecode instruction 0.
#
# g_syscalls.c is deliberately absent: upstream guards it with
# "#error Do not use in VM build" because the VM gets its trap stubs from the
# hand-written g_syscalls.asm instead, which q3asm assembles alongside.
Q3_MODULES="g_main bg_misc bg_lib bg_pmove bg_slidemove q_math q_shared \
ai_dmnet ai_dmq3 ai_team ai_main ai_chat ai_cmd ai_vcmd \
g_active g_arenas g_bot g_client g_cmds g_combat g_items g_mem g_misc \
g_missile g_mover g_session g_spawn g_svcmds g_target g_team g_trigger \
g_utils g_weapon"

WORK="$OUT/qagame"
rm -rf "$WORK"
mkdir -p "$WORK"
cd "$WORK"

LCC="q3lcc -DQ3_VM -S -Wf-target=bytecode -Wf-g -I$GAME"
for m in $Q3_MODULES; do
    $LCC "$GAME/$m.c"
done
cp "$GAME/g_syscalls.asm" .

{
    echo "-o qagame.qvm"
    echo "g_main"
    echo "g_syscalls"
    for m in $Q3_MODULES; do
        [ "$m" = g_main ] && continue
        echo "$m"
    done
} > qagame.q3asm

q3asm -f qagame.q3asm

cp qagame.qvm "$OUT/qagame.qvm"
cp qagame.map "$OUT/qagame.map" 2>/dev/null || true
echo "[qvm] built $OUT/qagame.qvm ($(wc -c < "$OUT/qagame.qvm") bytes)"
