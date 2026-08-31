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

if [ ! -f "fat32.img" ]; then
    echo "[!] Membuat fat32.img baru..."
    dd if=/dev/zero of=fat32.img bs=1M count=16 2>/dev/null
    mkfs.fat -F 32 -S 512 fat32.img > /dev/null 2>&1
fi

# USB 3.0 stick (v38.56): FAT32 image behind qemu-xhci, attached to the
# SuperSpeed bus. The kernel registers it as drive 8; mount it from the
# shell with `mount /usb fat32 8`.
if [ ! -f "usb.img" ]; then
    echo "[!] Membuat usb.img baru..."
    dd if=/dev/zero of=usb.img bs=1M count=16 2>/dev/null
    mkfs.fat -F 32 -S 512 usb.img > /dev/null 2>&1
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

# --- Audio backend: pilih yang paling stabil ---
# `pa` (PulseAudio-over-PipeWire) bisa nyangkut di host tertentu dan bikin
# main loop QEMU ke-block -> window beku padahal guest (KVM) tetap hidup
# (gejala "freeze Doom" sejak sound aktif). Prioritas: pipewire native
# (paling baru, robust), lalu pa, lalu none (bisu tapi tidak bisa macet).
# Override manual:  MECTOV_AUDIO=none|pa|pipewire|alsa ./run.sh
AUDIO_DRIVER="${MECTOV_AUDIO:-}"
if [ -z "$AUDIO_DRIVER" ]; then
    if qemu-system-i386 -audiodev help 2>&1 | grep -q '^pipewire$'; then
        AUDIO_DRIVER=pipewire
    else
        AUDIO_DRIVER=pa
    fi
fi
AUDIO_ARGS="-audiodev id=snd0,driver=$AUDIO_DRIVER -device sb16,audiodev=snd0"
echo "[*] Audio backend: $AUDIO_DRIVER (override: MECTOV_AUDIO=...)"

echo "[*] Menjalankan Mectov OS di QEMU (VBE GRUB Mode)..."
echo "[*] Serial debug output -> serial_debug.log"
qemu-system-i386 -enable-kvm -cpu host \
    -vga std \
    -cdrom mectov.iso \
    -m 128 \
    -smp 4 \
    $AUDIO_ARGS \
    -net nic,model=rtl8139 -net user \
    -chardev socket,id=char0,host=127.0.0.1,port=45454,server=on,wait=off,logfile=serial_debug.log -serial chardev:char0 \
    -drive file=disk.img,format=raw,index=0,media=disk \
    -drive file=ext2.img,format=raw,index=1,media=disk \
    -drive file=fat32.img,format=raw,index=3,media=disk \
    -device qemu-xhci,id=xhci0 \
    -drive file=usb.img,format=raw,if=none,id=usbd0 \
    -device usb-storage,drive=usbd0,bus=xhci0.0

echo "[*] Menghentikan Mectov Web Gateway Proxy..."
kill $GATEWAY_PID 2>/dev/null

# Salin ke log.txt agar mudah diakses
if [ -f "serial_debug.log" ]; then
    cp serial_debug.log log.txt
    echo "[*] Log aktivitas OS terbaru disimpan ke log.txt"
fi