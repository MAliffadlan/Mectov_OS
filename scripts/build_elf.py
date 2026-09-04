#!/usr/bin/env python3
"""
scripts/build_elf.py — build a freestanding PIE ELF32 executable for Mectov OS.

Unlike build_mct.py (proprietary MCT header + raw blob), this produces a
standard ELF32 binary that the in-kernel ELF loader parses directly:

  python3 scripts/build_elf.py apps/elfdemo.c elfdemo.elf

v38.63: the binary is now an ET_DYN **PIE** linked at offset 0, compiled with
-fPIE. The i386 -fPIE codegen is PC-relative (every global/function reference
goes through __x86.get_pc_thunk addressing computed from the runtime PC), and
a static link emits NO dynamic relocations, so the kernel can load the image
at any page-aligned virtual base and apply ASLR (random base from the kernel
CSPRNG) with zero fix-ups. Legacy ET_EXEC binaries linked at an absolute
address still load (unrandomized, as before — their code cannot move).

The linker script is deliberately NOT used: `ld -pie` requires its default
PHDR layout ("PHDR segment not covered by LOAD segment" with a custom
SECTIONS script). ENTRY is passed via -e _start instead.
"""
import os
import subprocess
import sys


def build_elf(c_file, output_elf):
    print(f"[*] Building {c_file} -> {output_elf}")

    base_name = os.path.splitext(c_file)[0]
    o_file = f"{base_name}.o"

    try:
        # -fPIE (was -fno-pie -fno-pic): position-independent codegen whose
        # references are all PC-relative — the property ASLR relies on.
        subprocess.run(["gcc", "-m32", "-ffreestanding", "-fno-stack-protector",
                        "-msoft-float", "-mno-80387", "-mno-sse", "-mno-mmx",
                        "-fno-asynchronous-unwind-tables", "-fPIE",
                        "-static", "-O2", "-I.", "-c", c_file, "-o", o_file],
                       check=True)
        # ld -pie + -e _start: ET_DYN at offset 0. -s strips the symtab; the
        # loader applies relocations from stored addends, so it is not needed.
        subprocess.run(["ld", "-m", "elf_i386", "-pie", "-e", "_start", "-s",
                        o_file, "-o", output_elf], check=True)
    except subprocess.CalledProcessError:
        print("[!] build failed")
        return 1
    finally:
        try:
            os.remove(o_file)
        except OSError:
            pass

    print(f"[+] {output_elf} created (ELF32 ET_DYN PIE at offset 0, ASLR-ready)")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 build_elf.py <source.c> <output.elf>")
        sys.exit(1)
    sys.exit(build_elf(sys.argv[1], sys.argv[2]))
