#!/bin/bash
# Bersihkan binary lama di awal untuk menjamin rebuild bersih total
# (dilewati oleh MECTOV_SKIP_BUILD=1 — jalur cepat `quake.sh --fast`, yang
#  memakai mectov.iso yang sudah ada supaya mulai main tidak bayar rebuild)
# Peringatan "ISO ini bakal kehilangan engine Q3": pesan yang sama juga muncul
# di depan tiap perintah q3* di dalam OS, dan run.sh selalu rebuild bersih —
# jadi `./run.sh` tanpa MECTOV_Q3=1 yang dijalankan sesudah sesi main akan
# MENGGANTI ISO ber-engine dengan ISO tanpa engine, dan yang kelihatan cuma
# `q3arena: engine not compiled in` di tengah OS yang sudah jalan. Satu baris
# di sini jauh lebih murah daripada satu rebuild yang salah.
if [ "${MECTOV_SKIP_BUILD:-0}" != "1" ] && [ "${MECTOV_Q3:-0}" != "1" ] && [ -f mectov.iso ]; then
    echo "[!] Build ini TANPA engine Quake III (MECTOV_Q3 bukan 1), padahal ISO yang ada"
    echo "    sekarang ($(stat -c%s mectov.iso) bytes, $(date -r mectov.iso '+%Y-%m-%d %H:%M')) akan diganti."
    echo "    Kalau tujuannya main:  MECTOV_Q3=1 ./run.sh   atau   ./quake.sh [map]"
    echo "    (Lanjut dalam 5 detik - Ctrl-C untuk batal.)"
    sleep 5
fi

if [ "${MECTOV_SKIP_BUILD:-0}" != "1" ]; then
    make clean_all
fi

# Cek apakah disk.img ada
if [ ! -f "disk.img" ]; then
    echo "[!] Membuat disk.img baru..."
    dd if=/dev/zero of=disk.img bs=512 count=4096 2>/dev/null
fi

# Volume size follows the staged Q3 payload (v38.107). One retail map's
# textures and models are tens of MB — far past this image — while the
# generated test arena is ~14 KB. So a bare build keeps the 16 MB image
# unchanged, and only an install with data staged by scripts/q3a_data.py grows
# to 512 MB. The kernel's ext2 driver walks at most 32 block groups, which is
# why the big image uses 4 KB blocks (512 MB / 4 KB = 4 groups, and 4 KB blocks
# also lift the per-file ceiling from ~64 MB to ~4 GB).
Q3DATA_KB=$(du -sk build/q3data 2>/dev/null | cut -f1)
Q3DATA_KB=${Q3DATA_KB:-0}
if [ "$Q3DATA_KB" -gt 8192 ]; then
    EXT2_MIB=512
    EXT2_MKFS="-b 4096 -m 0"
    if [ -f ext2.img ] && [ "$(stat -c%s ext2.img)" -lt 536870912 ]; then
        echo "[!] ext2.img terlalu kecil untuk game data Q3 — dibuat ulang (512MB)..."
        rm -f ext2.img
    fi
else
    EXT2_MIB=16
    EXT2_MKFS=""
fi

if [ ! -f "ext2.img" ]; then
    echo "[!] Membuat ext2.img baru (${EXT2_MIB}MB)..."
    dd if=/dev/zero of=ext2.img bs=1M count=0 seek="$EXT2_MIB" 2>/dev/null
    mkfs.ext2 -F $EXT2_MKFS ext2.img > /dev/null 2>&1
fi
# System blobs (debloat v38.81): the kernel loads doom1.wad /
# wallpaper.bin / music.wav from /ext2 on demand instead of embedding
# them. Top up every launch (idempotent, ~1 s) so old/small images gain
# the blobs without a full recreate.
if [ -f "ext2.img" ] && [ "$(stat -c%s ext2.img)" -lt 8388608 ]; then
    echo "[!] ext2.img < 8MB: too small for the system blobs (wallpaper/doom/music)."
    echo "    Guest falls back gracefully, but for the full desktop: rm ext2.img && ./run.sh"
fi
bash scripts/seed_ext2.sh ext2.img

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

# VirtIO-Blk disk (v38.78, opt-in): FAT32 image behind a transitional
# virtio-blk-pci controller. The kernel registers it as drive 12; mount it
# from the shell with `mount /vblk fat32 12`. Attached to QEMU only when
# MECTOV_VIRTIO=1 (default off — plain boots stay exactly as before).
if [ ! -f "virtio.img" ]; then
    echo "[!] Membuat virtio.img baru..."
    dd if=/dev/zero of=virtio.img bs=1M count=16 2>/dev/null
    mkfs.fat -F 32 -S 512 virtio.img > /dev/null 2>&1
fi

if [ "${MECTOV_SKIP_BUILD:-0}" = "1" ]; then
    # ---- jalur cepat (quake.sh --fast): pakai ISO yang sudah ada -----------
    # Tidak ada make clean_all, make, atau grub-mkrescue. Volume (ext2.img)
    # tetap di-seed di atas, jadi game data yang sudah di-stage tetap ada.
    if [ ! -f mectov.iso ]; then
        echo "[!] MECTOV_SKIP_BUILD=1 tapi mectov.iso tidak ada."
        echo "    Build dulu sekali tanpa flag itu:  MECTOV_Q3=1 ./run.sh"
        exit 1
    fi
    echo "[*] MECTOV_SKIP_BUILD=1: lewati make clean_all + make all32 + grub-mkrescue."
    echo "[*]    ISO: mectov.iso ($(stat -c%s mectov.iso) bytes, $(date -r mectov.iso '+%Y-%m-%d %H:%M'))"
else
    # Rebuild kernel (akan mengompilasi semua MCT dinamis secara bersih).
    # `all32` eksplisit: bare `make` sekarang membangun kernel 64-bit
    # (mainline x86_64, 1 Okt); skrip ini membangun ISO 32-bit.
    make all32

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

    # Toolchain lokal opsional untuk bikin ISO bootable Mectov. Hanya dipakai kalau
    # direktorinya benar-benar ada; kalau tidak, pakai grub-mkrescue dari PATH sistem.
    # Override: MECTOV_XBIN=/opt/xbin ./run.sh
    MECTOV_XBIN="${MECTOV_XBIN:-/home/mectov/my-os/xbin/usr}"
    if [ -d "$MECTOV_XBIN/bin" ]; then
        export PATH="$MECTOV_XBIN/bin:$PATH"
    fi
    if [ -d "$MECTOV_XBIN/lib/x86_64-linux-gnu" ]; then
        export LD_LIBRARY_PATH="$MECTOV_XBIN/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
    fi
    grub-mkrescue -o mectov.iso iso >/dev/null 2>&1
fi

echo "[*] Menghentikan instansi lama Web Gateway Proxy (jika ada)..."
pkill -f gateway.py 2>/dev/null
sleep 0.5

echo "[*] Menjalankan Mectov Web Gateway Proxy di background..."
python3 scripts/gateway.py > gateway.log 2>&1 &
GATEWAY_PID=$!

# Bersihkan log serial lama — TAPI jangan buang sesi sebelumnya: serial_debug.log
# ditimpa tiap launch, jadi laporan "freeze" selalu kehilangan buktinya tepat
# saat orang relaunch (v38.133: serial_debug.log sesi 14:51 user hilang begitu
# dia menutup window dan menjalankan lagi). Rotasi: yang lama disimpan sebagai
# serial_debug.prev.log, yang baru ditulis di atasnya.
if [ -f serial_debug.log ]; then
    mv -f serial_debug.log serial_debug.prev.log
fi
rm -f log.txt

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
# Preflight /dev/kvm. Ini penting dan bukan hiasan: kalau QEMU tidak bisa buka
# /dev/kvm (grup kvm belum kepakai di sesi ini, ACL-nya belum ada), QEMU mati
# dalam < 8 detik dan blok fallback di bawah menjalankan ulang semuanya di TCG
# TANPA satu baris pun yang bilang guest-nya sekarang emulasi software. Yang
# kelihatan cuma "Q3-nya kok 2 fps" — sekitar 7x lebih lambat dari KVM di
# rasterizer software ini. Jadi periksa hak akses DULU, dan kalau TCG memang
# yang jalan, katakan di baris pertama.
if [ -n "$KVM_FLAGS" ] && { [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; }; then
    echo "[!] /dev/kvm tidak bisa dibaca/ditulis oleh $(id -un) - KVM dilewati, jalan di TCG."
    if ! id -nG | tr ' ' '\n' | grep -qx kvm; then
        echo "    User $(id -un) belum ada di grup kvm. Sekali saja:  sudo usermod -aG kvm $(id -un)"
        echo "    lalu LOGOUT/LOGIN (atau 'newgrp kvm') sebelum ./run.sh lagi."
    else
        echo "    User sudah di grup kvm, tapi node-nya (root:kvm 0660) belum memberi akses."
        echo "    Fix cepat:  sudo setfacl -m u:$(id -un):rw /dev/kvm   (hilang tiap udev bikin ulang node)"
    fi
    echo "    Kalau memang mau TCG: MECTOV_KVM=0 ./run.sh  (default akan tetap mengingatkan)."
    KVM_FLAGS=""
fi
# Jumlah core. Di KVM, 4 core itu ideal. Di TCG (emulasi software), 4 core
# justru LAMBAT: tiap vCPU = thread host + spinlock SMP bikin thrash, GUI
# bisa 2-3x lebih lambat dari 1-2 core. Fallback TCG otomatis turun ke 2.
# Override manual:  MECTOV_SMP=1 ./run.sh
SMP="${MECTOV_SMP:-4}"
# VirtIO-Blk controller (v38.78, opt-in): MECTOV_VIRTIO=1 attaches the
# virtio.img disk as drive 12 via the legacy interface the kernel drives.
VIRTIO_ARGS=""
if [ "${MECTOV_VIRTIO:-0}" = "1" ]; then
    VIRTIO_ARGS="-drive file=virtio.img,format=raw,if=none,id=vd0 -device virtio-blk-pci,drive=vd0,disable-modern=on"
fi
# VirtIO-GPU (v38.115, opt-in): MECTOV_GPU=1 attaches a virtio-gpu-pci device
# (1AF4:1050, the MODERN virtio-pci layout — this device has no legacy
# interface at all, unlike the blk device above), which is what `gpustat` and
# the driver's own round-trip self-test talk to. It renders nothing by itself,
# so the OS window keeps coming from -vga std.
#   MECTOV_GPU=1    plain device (2D scanout, no 3D capsets)
#   MECTOV_GPU=gl   virtio-gpu-gl-pci + a GL-capable display backend, which is
#                   what offers the virgl 3D command set. Override the backend
#                   with MECTOV_GPU_DISPLAY if gtk,gl=on is not built in
#                   (e.g. MECTOV_GPU_DISPLAY=sdl,gl=on).
GPU_ARGS=""
case "${MECTOV_GPU:-0}" in
    1)  GPU_ARGS="-device virtio-gpu-pci,id=vgpu0" ;;
    gl) GPU_ARGS="-device virtio-gpu-gl-pci,id=vgpu0 -display ${MECTOV_GPU_DISPLAY:-gtk,gl=on}" ;;
esac
# Display backend (opt-in): MECTOV_DISPLAY=sdl uses SDL instead of the
# default gtk. Measured on a busy host desktop with the Q3 window animating:
# gtk's main thread sits at ~96% and the host window stops repainting even
# though the guest keeps rendering behind it, while sdl sits at ~6% with the
# same guest, game and flags. Use it for Quake sessions:
#   MECTOV_DISPLAY=sdl MECTOV_Q3=1 ./run.sh
# (passes through ./quake.sh too, which execs run.sh with the environment).
DISPLAY_ARGS="-display ${MECTOV_DISPLAY:-gtk}"
run_qemu() {
    qemu-system-i386 $KVM_FLAGS \
    -vga std \
    -cdrom mectov.iso \
    -m 512 \
    -smp "$SMP" \
    $DISPLAY_ARGS \
    $AUDIO_ARGS \
    -net nic,model=rtl8139 -net user \
    -chardev socket,id=char0,host=127.0.0.1,port=45454,server=on,wait=off,logfile=serial_debug.log -serial chardev:char0 \
    -drive file=disk.img,format=raw,index=0,media=disk \
    -drive file=ext2.img,format=raw,index=1,media=disk \
    -drive file=fat32.img,format=raw,index=3,media=disk \
    -device qemu-xhci,id=xhci0 \
    -drive file=usb.img,format=raw,if=none,id=usbd0 \
    -device usb-storage,drive=usbd0,bus=xhci0.0 \
    $VIRTIO_ARGS \
    $GPU_ARGS
}

if [ -n "$KVM_FLAGS" ]; then
    # Coba KVM dulu; stderr ditangkap ke file sementara. Kalau entry gagal,
    # QEMU mati < 8 detik dan/atau stderr berisi "KVM: entry failed" ->
    # restart dengan TCG. Kalau KVM sehat, QEMU jalan terus sampai window
    # ditutup / OS shutdown (exit normal, bukan fallback).
    KVM_ERR=$(mktemp)
    echo "[*] Akselerasi: KVM (-enable-kvm -cpu host, SMP $SMP)"
    run_qemu 2>"$KVM_ERR" &
    QEMU_PID=$!
    sleep 8
    if grep -q "KVM: entry failed" "$KVM_ERR" 2>/dev/null || ! kill -0 $QEMU_PID 2>/dev/null; then
        echo "[!] KVM gagal - fallback ke TCG (tanpa KVM)."
        # Alasan sebenarnya dari QEMU, bukan tebakan. "Permission denied" di
        # /dev/kvm dan "entry failed" itu dua masalah beda dan jalan keluarnya
        # juga beda; menyembunyikan stderr-nya bikin yang pertama mustahil
        # didiagnosis dari log run.sh.
        if [ -s "$KVM_ERR" ]; then
            echo "    QEMU bilang:"
            sed -n '1,3p' "$KVM_ERR" | sed 's/^/      /'
        fi
        echo "    Kalau 'Could not access KVM kernel module: Permission denied' -> grup/ACL"
        echo "    /dev/kvm (lihat blok preflight di atas), bukan masalah VT-x."
        echo "    Kalau 'KVM: entry failed' -> biasanya VirtualBox/VM lain lagi megang"
        echo "    VT-x, atau nested virtualization mati. Matiin VirtualBox dulu."
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
        echo "[*] Akselerasi: TCG (emulasi software, SMP $SMP) - Q3 di sini jauh lebih lambat dari KVM."
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
    echo "[*] Akselerasi: TCG (emulasi software) - tanpa KVM"
    run_qemu
fi

echo "[*] Menghentikan Mectov Web Gateway Proxy..."
kill $GATEWAY_PID 2>/dev/null

# Salin ke log.txt agar mudah diakses
if [ -f "serial_debug.log" ]; then
    cp serial_debug.log log.txt
    echo "[*] Log aktivitas OS terbaru disimpan ke log.txt"
fi