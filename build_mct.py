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
    ld_file = "app.ld"
    
    # 1. Create Linker Script
    # Ini memastikan entry point ada di offset 0 dan sections berurutan
    with open(ld_file, "w") as f:
        f.write("""
OUTPUT_FORMAT("elf32-i386")
ENTRY(_start)
SECTIONS {
    . = 0x02000000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
""")

    # 2. Compile
    # -fno-stack-protector: don't require libc's stack check
    # -fno-pie -fno-pic: prevent GOT/PLT generation which breaks flat binaries
    try:
        subprocess.run(["gcc", "-m32", "-ffreestanding", "-fno-stack-protector", "-fno-pie", "-fno-pic", "-static", "-O0", "-s", "-c", c_file, "-o", o_file], check=True)
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
                entry_point = addr - 0x02000000
                break
    except Exception as e:
        print(f"[*] Warning: Could not find _start, using offset 0. {e}")
        
    data_size = 16384 # Kasih 16KB untuk BSS (variabel global nol) dan memory runtime

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
    # os.remove(elf_file)
    os.remove(bin_file)
    os.remove(ld_file)

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 build_mct.py <source.c> <output.mct>")
        sys.exit(1)
        
    build_app(sys.argv[1], sys.argv[2])
