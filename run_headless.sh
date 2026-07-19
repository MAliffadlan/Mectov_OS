#!/bin/bash
# Cek apakah disk.img ada
if [ ! -f "disk.img" ]; then
    echo "[!] Membuat disk.img baru..."
    dd if=/dev/zero of=disk.img bs=512 count=2048 2>/dev/null
fi

# Rebuild kernel
make clean && make

# Setup ISO directory
mkdir -p iso/boot/grub
cp myos.bin iso/boot/
cat << 'EOF' > iso/boot/grub/grub.cfg
serial --unit=0 --speed=115200
terminal_output serial
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

echo "[*] Menjalankan Mectov OS di QEMU (VBE GRUB Mode)..."
echo "[*] Serial debug output -> serial_debug.log"
timeout 15s qemu-system-i386 \
  -enable-kvm -cpu host \
  -cdrom mectov.iso \
  -vga std \
  -m 256M \
  -smp 4 \
  -no-reboot \
  -serial file:serial_debug.log \
  -display none \
  -drive file=disk.img,format=raw,index=0,media=disk