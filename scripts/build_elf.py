#!/usr/bin/env python3
"""
scripts/build_elf.py — build a freestanding ELF32 executable for Mectov OS.

Unlike build_mct.py (proprietary MCT header + raw blob), this produces a
standard ELF32 ET_EXEC binary that the in-kernel ELF loader parses directly:

  python3 scripts/build_elf.py apps/elfdemo.c elfdemo.elf

The binary is linked at 0x08000000 with entry point _start, exactly like the
MCT layout, so existing Ring 3 apps compile unchanged — only the container
format differs (ELF program headers instead of a flat header+blob).
"""
import os
import subprocess
import sys


def build_elf(c_file, output_elf):
    print(f"[*] Building {c_file} -> {output_elf}")

    base_name = os.path.splitext(c_file)[0]
    o_file = f"{base_name}.o"
    ld_file = f"{base_name}.ld"

    with open(ld_file, "w") as f:
        f.write("""
OUTPUT_FORMAT("elf32-i386")
ENTRY(_start)
SECTIONS {
    . = 0x08000000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
    /DISCARD/ : { *(.eh_frame) *(.note*) *(.comment) *(.symtab) *(.strtab) *(.shstrtab) }
}
""")

    try:
        subprocess.run(["gcc", "-m32", "-ffreestanding", "-fno-stack-protector",
                        "-msoft-float", "-mno-80387", "-mno-sse", "-mno-mmx",
                        "-fno-asynchronous-unwind-tables", "-fno-pie", "-fno-pic",
                        "-static", "-O2", "-I.", "-c", c_file, "-o", o_file],
                       check=True)
        subprocess.run(["ld", "-m", "elf_i386", "-T", ld_file, "-s", o_file,
                        "-o", output_elf], check=True)
    except subprocess.CalledProcessError:
        print("[!] build failed")
        return 1
    finally:
        for p in (o_file, ld_file):
            try:
                os.remove(p)
            except OSError:
                pass

    print(f"[+] {output_elf} created (ELF32 ET_EXEC at 0x08000000)")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 build_elf.py <source.c> <output.elf>")
        sys.exit(1)
    sys.exit(build_elf(sys.argv[1], sys.argv[2]))
