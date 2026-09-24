# third_party/q3a — official id Software Quake III Arena source

Upstream : https://github.com/id-Software/Quake-III-Arena
Commit   : dbe4ddb10315479fc00086f08e25d968b4b43c49
License  : GNU GPL v2 (see COPYING.txt). Vendored files are unmodified.

This directory is the *official* Quake III Arena engine source release, taken
verbatim from id Software's own repository. It is what the `MECTOV_Q3=1`
kernel build compiles its engine core from — not a fork.

## Vendored

| path          | what                                                       |
|---------------|------------------------------------------------------------|
| `code/qcommon` | engine core: cmd, cvar, common, files, msg, huffman, md4, net_chan, the QVM loader (`vm.c`) and the portable bytecode interpreter (`vm_interpreted.c`) |
| `code/game`    | `q_shared.c` / `q_math.c` (the core needs them) plus the full game module that id's lcc compiles into `qagame.qvm` |
| `code/null`    | id's headless platform stubs                               |
| `q3asm`        | id's QVM assembler                                         |
| `lcc`          | id's C compiler with the bytecode backend                  |

A handful of headers are vendored on their own because the files above include
them across module boundaries by relative path: `ui/menudef.h` (the game
module's menu constants), `code/client/{client,keys,snd_public}.h`,
`code/renderer/tr_public.h`, `code/ui/{ui_public,keycodes}.h` and
`code/cgame/{cg_public,tr_types}.h` (pulled in by `code/qcommon/unzip.c`
and `code/null/*.c`).

`lcc` is trimmed to the bytecode toolchain (`src`, `cpp`, `etc`, `lburg`,
`include`, `lib`). The prebuilt win32 binaries in `bin/` and the native
runtime backends (`x86/`, `alpha/`, `mips/`, `sparc/`) are not used by the
bytecode target and are not vendored; neither are the IDE project files.

## Not vendored

The upstream repository also carries the client (`code/client`), renderer
(`code/renderer`), server, botlib and the map tools. They are not needed by
the kernel port, so they stay upstream — clone the URL above to get them.

## Game data

**None of the data is here, and none of it is GPL.** Quake III Arena's maps,
models, textures and sounds live in `pak0.pk3` in the retail release and are
covered by its own EULA. The GPL grants you the source, not the assets. The
kernel therefore boots the *engine* and runs bytecode built from this source;
loading real arena content needs your own copy of the game data.

Refresh with `scripts/vendor_q3a.sh`.
