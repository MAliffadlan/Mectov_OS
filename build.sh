#!/bin/bash
echo "[*] Mengompilasi Mectov OS (Modular Build)..."
make clean && make
if [ $? -eq 0 ]; then
    echo "[+] Kompilasi Berhasil: myos.bin telah dibuat."
else
    echo "[-] Kompilasi Gagal!"
    exit 1
fi
