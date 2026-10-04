#!/usr/bin/env python3
"""scripts/build_elf64.py — build a freestanding PIE ELF64 for Mectov/64.

Mirrors scripts/build_elf.py (32-bit): ET_DYN PIE linked at offset 0 with
-fPIE, static (no dynamic relocations), so the in-kernel ELF64 loader can
map it at any base (M5 uses a fixed USER_ELF_BASE; ASLR is an M7 follow-up
needing the CSPRNG).

  python3 scripts/build_elf64.py demos/execchild64.c demos/execchild64.elf

The shared demos/entry.S provides _start (linked first via the object
order); every demo defines demo_main.
"""
import os
import subprocess
import sys


def build(src_c, out_elf):
    print(f"[*] Building {src_c} -> {out_elf} (ELF64 ET_DYN PIE)")
    stem = os.path.splitext(src_c)[0]
    o_file = f"{stem}.o"
    entry_o = "demos/entry64.o"
    entry_s = "demos/entry.S"
    if (not os.path.exists(entry_o) or
            os.path.getmtime(entry_o) < os.path.getmtime(entry_s)):
        try:
            subprocess.run(["gcc", "-m64", "-ffreestanding",
                            "-fno-stack-protector", "-fno-pie", "-fno-pic",
                            "-static", "-O2", "-mno-red-zone", "-nostdlib",
                            "-c", entry_s, "-o", entry_o], check=True)
        except subprocess.CalledProcessError:
            print("[!] entry.S build failed")
            return 1
    try:
        subprocess.run(["gcc", "-m64", "-ffreestanding", "-fno-stack-protector",
                        "-mno-red-zone", "-fno-asynchronous-unwind-tables",
                        "-fPIE", "-static", "-O2", "-nostdlib", "-Wall",
                        "-Wextra", "-I.", "-c", src_c, "-o", o_file],
                       check=True)
        subprocess.run(["ld", "-m", "elf_x86_64", "--no-warn-rwx-segments",
                        "-pie", "-e", "_start", "-s",
                        entry_o, o_file, "-o", out_elf], check=True)
    except subprocess.CalledProcessError:
        print("[!] build failed")
        return 1
    finally:
        try:
            os.remove(o_file)
        except OSError:
            pass
    print(f"[+] {out_elf} created (ELF64 ET_DYN PIE at offset 0)")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: build_elf64.py <source.c> <output.elf>")
        sys.exit(1)
    sys.exit(build(sys.argv[1], sys.argv[2]))
