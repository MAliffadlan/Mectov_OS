import sys
import os
import shutil
import struct
import subprocess
import tempfile

# Magic Number "MCT1"
MCT_MAGIC = 0x4D435431

def build_app(sources, output_mct):
    """Compile and link one or more C sources into a flat .mct app image.

    sources is a list. A single-source app is the common case (and the original
    signature); multi-source exists for apps that bundle a library, such as
    tlsselftest.mct, which links apps/lib/tls/*.c so the guest checks exercise
    the same object code the browser will use at runtime.
    """
    if isinstance(sources, str):
        sources = [sources]
    print(f"[*] Building {' '.join(sources)} -> {output_mct}")

    base_name = os.path.splitext(sources[0])[0]
    elf_file = f"{base_name}.elf"
    bin_file = f"{base_name}.bin"
    ld_file = f"{base_name}.ld"

    # Object files go in a per-build temp directory, NOT next to their sources.
    # Two targets that share a source (browser.mct and tlsselftest.mct both link
    # apps/lib/tls/*.c) otherwise compile to the same paths, and `make -j8` runs
    # them in parallel: one build's cleanup deleted the other's objects between
    # its compile and its link step, which surfaced as "ld: cannot find
    # apps/lib/tls/tls_hash.o" on an otherwise clean tree.
    o_dir = tempfile.mkdtemp(prefix="mct_objs_")
    o_files = [os.path.join(o_dir, os.path.splitext(os.path.basename(s))[0] + ".o")
               for s in sources]
    
    # 1. Create Linker Script
    # Ini memastikan entry point ada di offset 0 dan sections berurutan
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
    /DISCARD/ : { *(.eh_frame) *(.note*) *(.comment) }
}
""")

    # 2. Compile
    # -fno-stack-protector: don't require libc's stack check
    # -fno-asynchronous-unwind-tables: prevent eh_frame generation
    # -fno-pie -fno-pic: prevent GOT/PLT generation which breaks flat binaries
    # MCT_CFLAGS_EXTRA (env): appended flags for apps that need more than the
    # soft-float baseline — e.g. fputest enables SSE for its inline asm
    # (with -mno-sse the compiler rejects %%xmm register names outright).
    extra_flags = os.environ.get("MCT_CFLAGS_EXTRA", "").split()
    for src, obj in zip(sources, o_files):
        try:
            subprocess.run(["gcc", "-m32", "-ffreestanding", "-fno-stack-protector", "-fno-asynchronous-unwind-tables", "-fno-pie", "-fno-pic", "-static", "-O2", "-msoft-float", "-mno-80387", "-mno-sse", "-mno-mmx", "-I.", "-c", src, "-o", obj] + extra_flags, check=True)
        except subprocess.CalledProcessError:
            print(f"[!] Compilation failed: {src}")
            return 1

    # 3. Link
    # --no-warn-rwx-segments (v38.161, audit F10): GNU ld 2.39+ warns once per
    # flat app image that a LOAD segment is RWX. That is intentional here and
    # already documented (docs/architecture/memory.md): W^X covers user
    # heap/stack/mmap, while a loaded .mct keeps its text executable because
    # there is no dynamic linker to split segments. The 36 notes per clean
    # build were not defects, but they made the project's "0 warnings" claim
    # unreadable — the 64-bit linkers already pass this flag; the 32-bit ones
    # now do too.
    try:
        subprocess.run(["ld", "-m", "elf_i386", "-T", ld_file] + o_files + ["-o", elf_file,
                        "--no-warn-rwx-segments"], check=True)
    except subprocess.CalledProcessError:
        print("[!] Linking failed!")
        return 1

    # 4. Extract raw binary
    try:
        subprocess.run(["objcopy", "-O", "binary", elf_file, bin_file], check=True)
    except subprocess.CalledProcessError:
        print("[!] Binary extraction failed!")
        return 1

    # 5. Build .mct header
    try:
        with open(bin_file, "rb") as f:
            code_data = f.read()
    except FileNotFoundError:
        print("[!] Binary file not found!")
        return 1

    code_size = len(code_data)
    entry_point = 0
    try:
        nm_out = subprocess.check_output(["nm", elf_file]).decode()
        for line in nm_out.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[2] == "_start":
                addr = int(parts[0], 16)
                entry_point = addr - 0x08000000
                break
    except Exception as e:
        print(f"[*] Warning: Could not find _start, using offset 0. {e}")
        
    bss_val = 0
    try:
        size_out = subprocess.check_output(["size", elf_file]).decode()
        lines = size_out.splitlines()
        if len(lines) >= 2:
            parts = lines[1].split()
            bss_val = int(parts[2])
    except Exception as e:
        print(f"[*] Warning: Could not parse BSS size from size utility. {e}")
        
    data_size = bss_val + 16384 # Give dynamic BSS size + 16KB padding for runtime heap/stack safety

    # Struct format: 4 uint32 (16 bytes header)
    # <I = little-endian uint32
    header = struct.pack("<IIII", MCT_MAGIC, entry_point, code_size, data_size)

    # 6. Write final .mct
    with open(output_mct, "wb") as f:
        f.write(header)
        f.write(code_data)
        
    print(f"[+] Success! {output_mct} created.")
    print(f"    - Magic: 0x{MCT_MAGIC:X}")
    print(f"    - Entry: 0x{entry_point:X}")
    print(f"    - Code Size: {code_size} bytes")
    print(f"    - Data/BSS Size: {data_size} bytes")

    # Cleanup temporary files
    shutil.rmtree(o_dir, ignore_errors=True)
    os.remove(bin_file)
    os.remove(ld_file)
    return 0

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 build_mct.py <source.c> [more.c ...] <output.mct>")
        sys.exit(1)

    # The last argument is the output; everything before it is a source. That
    # keeps the two-argument form byte-identical to the original call.
    sys.exit(build_app(sys.argv[1:-1], sys.argv[-1]))
