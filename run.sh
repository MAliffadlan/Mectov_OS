#!/bin/bash
# Cek apakah disk.img ada
if [ ! -f "disk.img" ]; then
    echo "[!] Membuat disk.img baru..."
    dd if=/dev/zero of=disk.img bs=512 count=2048
fi

echo "[*] Menjalankan Mectov OS di QEMU..."
qemu-system-i386 -kernel myos.bin \
    -audiodev alsa,id=snd0 \
    -machine pcspk-audiodev=snd0 \
    -drive file=disk.img,format=raw,index=0,media=disk