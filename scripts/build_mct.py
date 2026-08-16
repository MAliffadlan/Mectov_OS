import sys
import os
import struct
import subprocess

# Magic Number "MCT1"
MCT_MAGIC = 0x4D435431

def build_app(c_file, output_mct):
    print(f"[*] Building {c_file} -> {output_mct}")
    
    base_name = os.path.splitext(c_file)[0]
    o_file = f"{base_name}.o"
    elf_file = f"{base_name}.elf"
    bin_file = f"{base_name}.bin"
    ld_file = f"{base_name}.ld"
    
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
    try:
        subprocess.run(["gcc", "-m32", "-ffreestanding", "-fno-stack-protector", "-fno-asynchronous-unwind-tables", "-fno-pie", "-fno-pic", "-static", "-O0", "-g", "-msoft-float", "-mno-80387", "-mno-sse", "-mno-mmx", "-I.", "-c", c_file, "-o", o_file] + extra_flags, check=True)
    except subprocess.CalledProcessError:
        print("[!] Compilation failed!")
        return

    # 3. Link
    try:
        subprocess.run(["ld", "-m", "elf_i386", "-T", ld_file, o_file, "-o", elf_file], check=True)
    except subprocess.CalledProcessError:
        print("[!] Linking failed!")
        return

    # 4. Extract raw binary
    try:
        subprocess.run(["objcopy", "-O", "binary", elf_file, bin_file], check=True)
    except subprocess.CalledProcessError:
        print("[!] Binary extraction failed!")
        return

    # 5. Build .mct header
    try:
        with open(bin_file, "rb") as f:
            code_data = f.read()
    except FileNotFoundError:
        print("[!] Binary file not found!")
        return

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
    os.remove(o_file)
     
    os.remove(bin_file)
    os.remove(ld_file)

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 build_mct.py <source.c> <output.mct>")
        sys.exit(1)
        
    build_app(sys.argv[1], sys.argv[2])
