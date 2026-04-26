#!/bin/bash
echo "[*] Mengompilasi Mectov OS..."
nasm -f elf32 boot.asm -o boot.o
gcc -m32 -c kernel.c -o kernel.o -std=gnu99 -ffreestanding -O2 -Wall -Wextra
ld -m elf_i386 -T linker.ld boot.o kernel.o -o myos.bin
if [ $? -eq 0 ]; then
    echo "[+] Kompilasi Berhasil: myos.bin telah dibuat."
else
    echo "[-] Kompilasi Gagal!"
    exit 1
fi