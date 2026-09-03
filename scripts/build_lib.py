import sys
import os
import struct
import subprocess

MCT_MAGIC = 0x4D435431

def build_lib(c_file, output_mct):
    print(f"[*] Building Lib {c_file} -> {output_mct}")
    base_name = os.path.splitext(c_file)[0]
    o_file = f"{base_name}.o"
    elf_file = f"{base_name}.elf"
    bin_file = f"{base_name}.bin"
    ld_file = f"{base_name}.ld"
    
    with open(ld_file, "w") as f:
        f.write("""
OUTPUT_FORMAT("elf32-i386")
ENTRY(_start)
SECTIONS {
    . = 0x09000000;
    .text : { 
        KEEP(*(.export_table))
        *(.text*) 
    }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
""")

    try:
        subprocess.run(["gcc", "-m32", "-ffreestanding", "-fno-stack-protector", "-fno-pie", "-fno-pic", "-static", "-O2", "-msoft-float", "-mno-80387", "-mno-sse", "-mno-mmx", "-DBUILDING_LIBC", "-s", "-I.", "-c", c_file, "-o", o_file], check=True)
        subprocess.run(["ld", "-m", "elf_i386", "-T", ld_file, o_file, "-o", elf_file], check=True)
        subprocess.run(["objcopy", "-O", "binary", elf_file, bin_file], check=True)
    except subprocess.CalledProcessError:
        print("[!] Compilation failed!")
        return

    try:
        with open(bin_file, "rb") as f:
            code_data = f.read()
    except FileNotFoundError:
        print("[!] Binary file not found!")
        return

    code_size = len(code_data)
    entry_point = 0
    data_size = 4096 

    header = struct.pack("<IIII", MCT_MAGIC, entry_point, code_size, data_size)

    with open(output_mct, "wb") as f:
        f.write(header)
        f.write(code_data)
        
    print(f"[+] Lib Success! {output_mct} created.")

    os.remove(o_file)
    os.remove(bin_file)
    os.remove(ld_file)

if __name__ == "__main__":
    build_lib(sys.argv[1], sys.argv[2])
