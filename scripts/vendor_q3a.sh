#!/bin/bash
# scripts/vendor_q3a.sh — (re)vendor the OFFICIAL id Software Quake III Arena
# source into third_party/q3a.
#
# Upstream: https://github.com/id-Software/Quake-III-Arena
# It is the 2005 GPL source drop (engine + game/cgame/ui modules + id's own
# QVM toolchain: lcc + q3asm). It contains NO game data — no .pk3, no maps,
# no models, no sounds. Those live in the retail Quake III Arena release and
# are not redistributable; you must supply your own.
#
# What gets vendored, and why:
#   code/qcommon   the engine core we build into the kernel (cmd/cvar/common/
#                  files/msg/huffman/md4/net_chan + the QVM loader, vm.c, and
#                  the portable bytecode interpreter, vm_interpreted.c)
#   code/game      q_shared.c/q_math.c (the engine core needs them) and the
#                  whole module — it is what id's lcc compiles into qagame.qvm
#   code/null      the platform stubs id ships for headless builds
#   q3asm          id's bytecode assembler (builds the .qvm)
#   lcc            id's C compiler with the bytecode backend (builds the .asm
#                  that q3asm consumes); trimmed of native backends and IDE
#                  projects, none of which the bytecode target uses
#
# Everything is copied VERBATIM — no patches. The build script touches one
# upstream file (lcc/lburg/gram.c) only to defeat make's yacc rule; see
# scripts/build_qvm.sh.
#
# Usage:
#   scripts/vendor_q3a.sh                 # clone upstream and vendor
#   scripts/vendor_q3a.sh --from <dir>    # vendor from an existing checkout
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/third_party/q3a"
UPSTREAM_URL="https://github.com/id-Software/Quake-III-Arena"

SRC=""
while [ $# -gt 0 ]; do
    case "$1" in
        --from) SRC="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ -z "$SRC" ]; then
    SRC="$(mktemp -d)/Quake-III-Arena"
    echo "[vendor] cloning $UPSTREAM_URL"
    git clone --quiet --depth 1 "$UPSTREAM_URL" "$SRC"
fi

[ -d "$SRC/code/qcommon" ] || { echo "[vendor] $SRC is not a Quake-III-Arena checkout" >&2; exit 1; }
COMMIT="$(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo unknown)"

echo "[vendor] vendoring into $DEST (commit $COMMIT)"
rm -rf "$DEST"
mkdir -p "$DEST/code" "$DEST/q3asm" "$DEST/lcc"

copy_tree() { # $1 = source dir, $2 = dest dir — copies the tree, structure intact
    mkdir -p "$2"
    cp -R "$1/." "$2/"
    find "$2" -type f \( -name '*.vcproj' -o -name '*.sln' -o -name '*.bat' \
         -o -name '*.def' -o -name '*.lnt' -o -name '*.dsp' -o -name '*.dsw' \) -delete
}

copy_tree "$SRC/code/qcommon" "$DEST/code/qcommon"
copy_tree "$SRC/code/game"    "$DEST/code/game"
copy_tree "$SRC/code/null"    "$DEST/code/null"

# Header-only cross-module dependencies of the files above. id's modules
# include each other by relative path, so these exact headers have to sit at
# the same place they do upstream, or the vendored .c files will not compile.
#   game/*.c          -> ../../ui/menudef.h        (top-level ui/)
#   qcommon/unzip.c   -> ../client/client.h
copy_file() { mkdir -p "$(dirname "$2")"; cp -p "$1" "$2"; }
copy_file "$SRC/ui/menudef.h"              "$DEST/ui/menudef.h"
copy_file "$SRC/code/client/client.h"      "$DEST/code/client/client.h"
copy_file "$SRC/code/client/keys.h"        "$DEST/code/client/keys.h"
copy_file "$SRC/code/client/snd_public.h"  "$DEST/code/client/snd_public.h"
copy_file "$SRC/code/renderer/tr_public.h" "$DEST/code/renderer/tr_public.h"
copy_file "$SRC/code/ui/ui_public.h"       "$DEST/code/ui/ui_public.h"
copy_file "$SRC/code/ui/keycodes.h"        "$DEST/code/ui/keycodes.h"
copy_file "$SRC/code/cgame/cg_public.h"    "$DEST/code/cgame/cg_public.h"
copy_file "$SRC/code/cgame/tr_types.h"    "$DEST/code/cgame/tr_types.h"
copy_tree "$SRC/q3asm"        "$DEST/q3asm"

# lcc: keep only what the bytecode target actually builds. bin/ ships prebuilt
# win32 binaries, and the per-architecture runtime sources (x86/, alpha/, mips/,
# sparc/ and include/<arch>/) are for native code generation, which we never do.
for d in src cpp etc lburg include lib; do
    copy_tree "$SRC/lcc/$d" "$DEST/lcc/$d"
done
cp -p "$SRC/lcc/makefile" "$SRC/lcc/custom.mk" "$SRC/lcc/README" "$DEST/lcc/"
cp -p "$SRC/lcc/COPYRIGHT" "$DEST/lcc/" 2>/dev/null || true

cp -p "$SRC/COPYING.txt" "$SRC/README.txt" "$DEST/"

cat > "$DEST/UPSTREAM.md" <<EOF
# third_party/q3a — official id Software Quake III Arena source

Upstream : $UPSTREAM_URL
Commit   : $COMMIT
License  : GNU GPL v2 (see COPYING.txt). Vendored files are unmodified.

This directory is the *official* Quake III Arena engine source release, taken
verbatim from id Software's own repository. It is what the \`MECTOV_Q3=1\`
kernel build compiles its engine core from — not a fork.

## Vendored

| path          | what                                                       |
|---------------|------------------------------------------------------------|
| \`code/qcommon\` | engine core: cmd, cvar, common, files, msg, huffman, md4, net_chan, the QVM loader (\`vm.c\`) and the portable bytecode interpreter (\`vm_interpreted.c\`) |
| \`code/game\`    | \`q_shared.c\` / \`q_math.c\` (the core needs them) plus the full game module that id's lcc compiles into \`qagame.qvm\` |
| \`code/null\`    | id's headless platform stubs                               |
| \`q3asm\`        | id's QVM assembler                                         |
| \`lcc\`          | id's C compiler with the bytecode backend                  |

A handful of headers are vendored on their own because the files above include
them across module boundaries by relative path: \`ui/menudef.h\` (the game
module's menu constants), \`code/client/{client,keys,snd_public}.h\`,
\`code/renderer/tr_public.h\`, \`code/ui/{ui_public,keycodes}.h\` and
\`code/cgame/{cg_public,tr_types}.h\` (pulled in by \`code/qcommon/unzip.c\`
and \`code/null/*.c\`).

\`lcc\` is trimmed to the bytecode toolchain (\`src\`, \`cpp\`, \`etc\`, \`lburg\`,
\`include\`, \`lib\`). The prebuilt win32 binaries in \`bin/\` and the native
runtime backends (\`x86/\`, \`alpha/\`, \`mips/\`, \`sparc/\`) are not used by the
bytecode target and are not vendored; neither are the IDE project files.

## Not vendored

The upstream repository also carries the client (\`code/client\`), renderer
(\`code/renderer\`), server, botlib and the map tools. They are not needed by
the kernel port, so they stay upstream — clone the URL above to get them.

## Game data

**None of the data is here, and none of it is GPL.** Quake III Arena's maps,
models, textures and sounds live in \`pak0.pk3\` in the retail release and are
covered by its own EULA. The GPL grants you the source, not the assets. The
kernel therefore boots the *engine* and runs bytecode built from this source;
loading real arena content needs your own copy of the game data.

Refresh with \`scripts/vendor_q3a.sh\`.
EOF

echo "[vendor] done: $(find "$DEST" -type f | wc -l) files, $(du -sh "$DEST" | cut -f1)"
