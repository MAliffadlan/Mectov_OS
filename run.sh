#!/bin/bash
# Bersihkan binary lama di awal untuk menjamin rebuild bersih total
make clean_all

# Cek apakah disk.img ada
if [ ! -f "disk.img" ]; then
    echo "[!] Membuat disk.img baru..."
    dd if=/dev/zero of=disk.img bs=512 count=2048 2>/dev/null
fi

if [ ! -f "ext2.img" ]; then
    echo "[!] Membuat ext2.img baru..."
    dd if=/dev/zero of=ext2.img bs=1M count=2 2>/dev/null
    mkfs.ext2 -F ext2.img > /dev/null 2>&1
fi

# Rebuild kernel (akan mengompilasi semua MCT dinamis secara bersih)
make

# Setup ISO directory
mkdir -p iso/boot/grub
cp myos.bin iso/boot/
# grub.cfg is REQUIRED — without it GRUB falls to its rescue shell.
cat << 'EOF' > iso/boot/grub/grub.cfg
set timeout=0
set default=0
menuentry "Mectov OS" {
    multiboot /boot/myos.bin
    boot
}
EOF

# Gunakan xorriso yang sudah didownload lokal untuk bikin ISO bootable Mectov
export PATH=/home/mectov/my-os/xbin/usr/bin:$PATH
export LD_LIBRARY_PATH=/home/mectov/my-os/xbin/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
grub-mkrescue -o mectov.iso iso >/dev/null 2>&1

echo "[*] Menghentikan instansi lama Web Gateway Proxy (jika ada)..."
pkill -f gateway.py 2>/dev/null
sleep 0.5

echo "[*] Menjalankan Mectov Web Gateway Proxy di background..."
python3 scripts/gateway.py > gateway.log 2>&1 &
GATEWAY_PID=$!

# Bersihkan log serial lama
rm -f serial_debug.log log.txt

echo "[*] Menjalankan Mectov OS di QEMU (VBE GRUB Mode)..."
echo "[*] Serial debug output -> serial_debug.log"
qemu-system-i386 -enable-kvm -cpu host \
    -vga std \
    -cdrom mectov.iso \
    -m 128 \
    -smp 4 \
    -audiodev pa,id=snd0 \
    -device sb16,audiodev=snd0 \
    -machine pcspk-audiodev=snd0 \
    -net nic,model=rtl8139 -net user \
    -chardev socket,id=char0,host=127.0.0.1,port=45454,server=on,wait=off,logfile=serial_debug.log -serial chardev:char0 \
    -drive file=disk.img,format=raw,index=0,media=disk \
    -drive file=ext2.img,format=raw,index=1,media=disk

echo "[*] Menghentikan Mectov Web Gateway Proxy..."
kill $GATEWAY_PID 2>/dev/null

# Salin ke log.txt agar mudah diakses
if [ -f "serial_debug.log" ]; then
    cp serial_debug.log log.txt
    echo "[*] Log aktivitas OS terbaru disimpan ke log.txt"
fi