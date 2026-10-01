#!/usr/bin/env python3
"""scripts/build_mct64.py — build a flat MCT2 Ring-3 image for Mectov/64.

MCT2 is the 64-bit successor of the 32-bit .mct format: a 48-byte header
(v2; v1 was 40 bytes, flat RW) plus a raw code blob, loaded at a FIXED
base (like 32-bit MCT at 0x08000000; per-process PML4s make fixed bases
collision-free). v2 page-seals text (RX) from data (RW|NX).

  python3 scripts/build_mct64.py demos/hello64.c demos/hello64.mct 0x40000000

Header (little-endian): magic u32 "MCT2", version u32=1, load_base u64,
entry_off u64 (entry - base, 0 for the shared entry.S), code_size u64,
bss_size u64 (exact, no padding — the kernel maps precisely).

The shared demos/entry.S is linked FIRST so _start is at offset 0.
"""
import os
import struct
import subprocess
import sys

MCT2_MAGIC = 0x3243544D  # "MCT2"
MCT2_VERSION = 2        # v2 adds text_size for W^X (v1 loads flat RW)
MCT2_HDR = struct.Struct("<IIQQQQQ")  # 48 bytes


def ensure_entry():
    """Build the shared demos/entry.S when missing OR stale (a stale
    entry64.o silently drops new entry points like _start_args -> the
    kernel would enter garbage; seen live as a null-read in argdemo)."""
    src, obj = "demos/entry.S", "demos/entry64.o"
    if (os.path.exists(obj) and
            os.path.getmtime(obj) >= os.path.getmtime(src)):
        return True
    try:
        subprocess.run(["gcc", "-m64", "-ffreestanding", "-fno-stack-protector",
                        "-fno-pie", "-fno-pic", "-static", "-O2",
                        "-mno-red-zone", "-nostdlib",
                        "-c", src, "-o", obj],
                       check=True)
        return True
    except subprocess.CalledProcessError:
        print("[!] entry.S build failed")
        return False


def build(src_c, out_mct, base):
    print(f"[*] Building {src_c} -> {out_mct} (MCT2 @ {base})")
    if not ensure_entry():
        return 1
    stem = os.path.splitext(src_c)[0]
    o_file = f"{stem}.o"
    elf_file = f"{stem}.elf"
    bin_file = f"{stem}.bin"
    ld_file = f"{stem}.ld"
    entry_o = "demos/entry64.o"

    with open(ld_file, "w") as f:
        # NOTE: `. = ALIGN(4096)` before .data keeps text+rodata page-sealed
        # so the kernel can map RX vs RW without sharing a page (W^X, M7.3).
        # objcopy does NOT pad that file gap; the explicit pad below does.
        f.write(f"""
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)
SECTIONS {{
    . = {base};
    .text : {{ *(.text*) }}
    .rodata : {{ *(.rodata*) }}
    . = ALIGN(4096);
    .data : {{ *(.data*) }}
    .bss : {{ *(.bss*) *(COMMON) }}
    /DISCARD/ : {{ *(.eh_frame) *(.note*) *(.comment) }}
}}
""")
    try:
        subprocess.run(["gcc", "-m64", "-ffreestanding", "-fno-stack-protector",
                        "-fno-pie", "-fno-pic", "-static", "-O2",
                        "-mno-red-zone", "-nostdlib", "-Wall", "-Wextra",
                        "-I.", "-c", src_c, "-o", o_file], check=True)
        subprocess.run(["ld", "-m", "elf_x86_64", "--no-warn-rwx-segments",
                        "-T", ld_file, entry_o, o_file, "-o", elf_file],
                       check=True)
        subprocess.run(["objcopy", "-O", "binary", elf_file, bin_file],
                       check=True)
    except subprocess.CalledProcessError:
        print("[!] build failed")
        return 1

    try:
        nm = subprocess.check_output(["nm", elf_file]).decode()
        entry = 0
        for line in nm.splitlines():
            p = line.split()
            if len(p) >= 3 and p[2] == "_start":
                entry = int(p[0], 16) - int(base, 16)
                break
        size = subprocess.check_output(["size", elf_file]).decode()
        parts = size.splitlines()[1].split()
        text_raw = int(parts[0])
        bss = int(parts[2])
        # Page-sealed text (matches the ALIGN(4096) in the ld script above).
        text_sz = (text_raw + 4095) & ~4095
        with open(bin_file, "rb") as f:
            code = f.read()
        # Pad text to text_size explicitly: objcopy does NOT pad the file
        # gap before page-aligned .data, so without this the data content
        # would sit at the wrong offset (VMA says base+text_size but the
        # bytes would follow text_raw). Padded blob layout is flat and
        # contiguous: [text+pad][data][bss-implicit].
        if len(code) < text_sz:
            code = code + b"\0" * (text_sz - len(code))
    except Exception as e:
        print(f"[!] header probe failed: {e}")
        return 1

    hdr = MCT2_HDR.pack(MCT2_MAGIC, MCT2_VERSION, int(base, 16), entry,
                        len(code), bss, text_sz)
    with open(out_mct, "wb") as f:
        f.write(hdr)
        f.write(code)
    print(f"[+] {out_mct}: entry_off=0x{entry:x} code={len(code)} bss={bss} text={text_sz}")
    for tmp in (o_file, bin_file, ld_file):
        try:
            os.remove(tmp)
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 4:
        print("Usage: build_mct64.py <source.c> <output.mct> <BASE-hex>")
        sys.exit(1)
    sys.exit(build(sys.argv[1], sys.argv[2], sys.argv[3]))
