#!/usr/bin/env python3
"""
scripts/build_rust_mct.py — build a Rust Ring 3 app into Mectov's .mct format.

Mirrors build_mct.py but replaces gcc with rustc: a freestanding no_std Rust
binary is linked at 0x08000000 with entry `_start`, flattened with objcopy,
and wrapped in the MCT1 header. The kernel loader (loader.c) treats the
result identically to a C app — it maps code, zeroes BSS, and thread_create
provides the user stack.

Compiler: rustc targeting `i686-unknown-uefi` (the only built-in freestanding
32-bit x86 target with a prebuilt core; its C ABI is cdecl like every other
i686 target). SSE/MMX are disabled via -C target-feature so no FPU state
escapes (the kernel does not save it, matching -mno-sse -mno-mmx for C apps).
That target decorates extern "C" symbols with a leading underscore, so the
entry symbol is `__start` — the script resolves the real name from nm.

Usage:
    python3 scripts/build_rust_mct.py apps/rusthello.rs rusthello.mct
"""
import os
import shutil
import struct
import subprocess
import sys

# Magic Number "MCT1" (same as loader.h)
MCT_MAGIC = 0x4D435431
LINK_ADDR = 0x08000000


def find_rustc():
    """rustup installs to ~/.cargo/bin (not on PATH for make); fall back to it."""
    which = shutil.which("rustc")
    if which:
        return which
    home = os.path.expanduser("~/.cargo/bin/rustc")
    return home if os.path.exists(home) else "rustc"


def build_app(rs_file, output_mct):
    print(f"[*] Building {rs_file} -> {output_mct}")

    base_name = os.path.splitext(rs_file)[0]
    o_file = f"{base_name}.o"
    elf_file = f"{base_name}.elf"
    bin_file = f"{base_name}.bin"
    ld_file = f"{base_name}.ld"

    # 1. Compile: freestanding no_std object, no unwind, no SSE/MMX. The
    #    crate is compiled as a `lib` so `_start` is a plain exported symbol
    #    (a bin crate emits an undefined `_start` expecting a crt0 shim).
    rustc = find_rustc()
    try:
        subprocess.run(
            [rustc, "--target", "i686-unknown-uefi", "--crate-type", "lib",
             "--emit", "obj", "-C", "panic=abort", "-C", "opt-level=0",
             "-C", "target-feature=-mmx,-sse", rs_file, "-o", o_file],
            check=True,
        )
    except subprocess.CalledProcessError:
        print("[!] rustc failed!")
        return

    # 2. Linker script — same layout as build_mct.py (+ .rdata, which the UEFI
    #    target uses for read-only data, and .data.rel.ro for statics).
    with open(ld_file, "w") as f:
        f.write(
            f"""
OUTPUT_FORMAT("elf32-i386")
ENTRY(_start)
SECTIONS {{
    . = 0x{LINK_ADDR:X};
    .text : {{ *(.text*) }}
    .rodata : {{ *(.rodata*) *(.rdata*) }}
    .data : {{ *(.data*) *(.data.rel.ro*) }}
    .bss : {{ *(.bss*) *(COMMON) }}
    /DISCARD/ : {{ *(.eh_frame) *(.note*) *(.comment) *(.got*) }}
}}
"""
        )

    # 3. Link. Resolve the entry symbol dynamically: the UEFI target mangles
    #    `_start` to `__start` (leading-underscore decoration).
    entry_sym = "_start"
    try:
        nm_out = subprocess.check_output(["nm", o_file]).decode()
        if any(parts[2] == "__start" for parts in (ln.split() for ln in nm_out.splitlines() if len(ln.split()) >= 3)):
            entry_sym = "__start"
    except Exception:
        pass
    try:
        subprocess.run(
            ["ld", "-m", "elf_i386", "-e", entry_sym, "-T", ld_file, o_file, "-o", elf_file],
            check=True,
        )
    except subprocess.CalledProcessError:
        print("[!] Linking failed!")
        return

    # 4. Extract raw binary
    try:
        subprocess.run(["objcopy", "-O", "binary", elf_file, bin_file], check=True)
    except subprocess.CalledProcessError:
        print("[!] Binary extraction failed!")
        return

    # 5. Build .mct header (identical layout to build_mct.py)
    with open(bin_file, "rb") as f:
        code_data = f.read()
    code_size = len(code_data)

    entry_point = 0
    try:
        nm_out = subprocess.check_output(["nm", elf_file]).decode()
        for line in nm_out.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[2] in ("_start", "__start"):
                addr = int(parts[0], 16)
                entry_point = addr - LINK_ADDR
                break
    except Exception as e:
        print(f"[*] Warning: could not find entry, using offset 0. {e}")

    bss_val = 0
    try:
        size_out = subprocess.check_output(["size", elf_file]).decode()
        parts = size_out.splitlines()[1].split()
        bss_val = int(parts[2])
    except Exception:
        pass

    data_size = bss_val + 16384  # BSS + 16KB padding for runtime heap/stack safety

    header = struct.pack("<IIII", MCT_MAGIC, entry_point, code_size, data_size)

    with open(output_mct, "wb") as f:
        f.write(header)
        f.write(code_data)

    print(f"[+] Success! {output_mct} created (Rust).")
    print(f"    - Magic: 0x{MCT_MAGIC:X}")
    print(f"    - Entry: 0x{entry_point:X} ({entry_sym})")
    print(f"    - Code Size: {code_size} bytes")
    print(f"    - Data/BSS Size: {data_size} bytes")

    for tmp in (o_file, bin_file, ld_file):
        try:
            os.remove(tmp)
        except OSError:
            pass


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 build_rust_mct.py <source.rs> <output.mct>")
        sys.exit(1)
    build_app(sys.argv[1], sys.argv[2])
