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

# --- KVM vs TCG ---
# -enable-kvm -cpu host dipakai kalau /dev/kvm ada DAN entry sukses. Di
# beberapa host (VM tanpa nested virt, CPU model tertentu) KVM gagal dengan
# "KVM: entry failed, hardware error 0x0" -> QEMU langsung paused. Kernel
# didukung penuh di TCG (semua test CI jalan TCG), jadi fallback otomatis:
# cek KVM dulu dengan boot singkat; kalau entry gagal, jalankan ulang tanpa
# KVM. Override manual:  MECTOV_KVM=0 ./run.sh  (paksa TCG)
#                        MECTOV_KVM=1 ./run.sh  (paksa KVM, tanpa fallback)
KVM_FLAGS=""
if [ "${MECTOV_KVM:-auto}" != "0" ]; then
    KVM_FLAGS="-enable-kvm -cpu host"
fi
# Jumlah core. Di KVM, 4 core itu ideal. Di TCG (emulasi software), 4 core
# justru LAMBAT: tiap vCPU = thread host + spinlock SMP bikin thrash, GUI
# bisa 2-3x lebih lambat dari 1-2 core. Fallback TCG otomatis turun ke 2.
# Override manual:  MECTOV_SMP=1 ./run.sh
SMP="${MECTOV_SMP:-4}"
run_qemu() {
    qemu-system-i386 $KVM_FLAGS \
    -vga std \
    -cdrom mectov.iso \
    -m 128 \
    -smp "$SMP" \
    $AUDIO_ARGS \
    -net nic,model=rtl8139 -net user \
    -chardev socket,id=char0,host=127.0.0.1,port=45454,server=on,wait=off,logfile=serial_debug.log -serial chardev:char0 \
    -drive file=disk.img,format=raw,index=0,media=disk \
    -drive file=ext2.img,format=raw,index=1,media=disk \
    -drive file=fat32.img,format=raw,index=3,media=disk \
    -device qemu-xhci,id=xhci0 \
    -drive file=usb.img,format=raw,if=none,id=usbd0 \
    -device usb-storage,drive=usbd0,bus=xhci0.0
}

if [ -n "$KVM_FLAGS" ]; then
    # Coba KVM dulu; stderr ditangkap ke file sementara. Kalau entry gagal,
    # QEMU mati < 8 detik dan/atau stderr berisi "KVM: entry failed" ->
    # restart dengan TCG. Kalau KVM sehat, QEMU jalan terus sampai window
    # ditutup / OS shutdown (exit normal, bukan fallback).
    KVM_ERR=$(mktemp)
    run_qemu 2>"$KVM_ERR" &
    QEMU_PID=$!
    sleep 8
    if grep -q "KVM: entry failed" "$KVM_ERR" 2>/dev/null || ! kill -0 $QEMU_PID 2>/dev/null; then
        echo "[!] KVM gagal (entry failed) - fallback ke TCG (tanpa KVM)."
        echo "    Hint: biasanya karena VirtualBox/VM lain lagi megang VT-x, atau "
        echo "    nested virtualization mati. Matiin VirtualBox dulu terus jalanin lagi."
        # QEMU yang macet di state KVM-paused kadang mengabaikan SIGTERM dan
        # tetap memegang port chardev 45454 -> restart gagal "Address already
        # in use". Pastikan mati: TERM dulu, 5 detik, kalau bandel kill -9.
        kill $QEMU_PID 2>/dev/null
        for _ in 1 2 3 4 5; do
            kill -0 $QEMU_PID 2>/dev/null || break
            sleep 1
        done
        kill -9 $QEMU_PID 2>/dev/null
        wait $QEMU_PID 2>/dev/null
        # Bersihkan sisa instansi lama yang masih pegang chardev kita.
        pkill -f 'qemu-system-i386.*port=45454' 2>/dev/null
        sleep 1
        KVM_FLAGS=""
        # TCG lebih cepat dengan lebih sedikit core (lihat catatan SMP di atas)
        if [ -z "${MECTOV_SMP:-}" ] && [ "$SMP" -gt 2 ]; then
            echo "[*] Turunkan SMP $SMP -> 2 untuk TCG (MECTOV_SMP=n untuk override)."
            SMP=2
        fi
        # Kalau QEMU TCG mati < 5 detik, tampilkan exit code-nya — biasanya
        # port 45454 masih dipinjam atau display/audio bermasalah.
        TCG_START=$(date +%s)
        run_qemu
        TCG_RC=$?
        if [ $TCG_RC -ne 0 ] || [ $(( $(date +%s) - TCG_START )) -lt 5 ]; then
            echo "[!] QEMU TCG keluar cepat (rc=$TCG_RC) - cek error di atas / serial_debug.log"
        fi
    else
        wait $QEMU_PID
    fi
    rm -f "$KVM_ERR"
else
    run_qemu
fi

echo "[*] Menghentikan Mectov Web Gateway Proxy..."
kill $GATEWAY_PID 2>/dev/null

# Salin ke log.txt agar mudah diakses
if [ -f "serial_debug.log" ]; then
    cp serial_debug.log log.txt
    echo "[*] Log aktivitas OS terbaru disimpan ke log.txt"
fi