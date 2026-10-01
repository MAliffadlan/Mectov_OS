#!/bin/bash
# Mectov OS 64-bit launcher — the mainline (x86_64-port branch).
# Does NOT touch the 32-bit flow (run.sh / mectov.iso / disk images).
#   ./run64.sh              interactive QEMU (display on)
#   ./run64.sh --headless   build + boot 60s headless, check serial64.log
# M6: 4 vCPUs so AP bring-up is really exercised (TCG handles our tiny
# kernel fine; override with MECTOV64_SMP=n).
# M11: machine is `pc`, not `q35` — q35 puts every -cdrom/-drive behind the
# ICH9 AHCI controller (MMIO, no 0x1F0 channel at all), while pc exposes the
# legacy IDE ports the block driver drives. It is also the machine the 32-bit
# line boots, and the kernel's M1-M10 gates pass on it unchanged.
SMP="${MECTOV64_SMP:-4}"
MEM="${MECTOV64_MEM:-256}"
MACHINE="${MECTOV64_MACHINE:-pc}"
HEADLESS=0
if [ "${1:-}" = "--headless" ]; then HEADLESS=1; fi

echo "[*] Building myos64.bin..."
make myos64.bin || { echo "[-] myos64 build failed"; exit 1; }
echo "[*] Building mectov64.iso (multiboot2)..."
make iso64 MECTOV64_CMDLINE="${MECTOV64_CMDLINE:-}" || { echo "[-] iso64 failed"; exit 1; }
# M11 fixture: deterministic 64KiB ATA disk the kernel identifies, reads with
# both the LBA28 and LBA48 commands, and hashes (scripts/blk_test.py checks the
# hash against this host's own computation of the same file).
python3 scripts/mk_blkdisk.py blkdisk.img >/dev/null || \
    { echo "[-] blkdisk.img fixture failed"; exit 1; }
rm -f serial64.log

QEMU_ARGS=(-machine "$MACHINE" -cpu qemu64,+nx -m "$MEM" -smp "$SMP"
    -cdrom mectov64.iso
    -drive file=blkdisk.img,format=raw,if=ide,index=1,media=disk
    -serial file:serial64.log
    -no-reboot)

if [ "$HEADLESS" = "1" ]; then
    echo "[*] Headless boot ($MACHINE, ${MEM}MB, smp=$SMP, 60s) -> serial64.log"
    if command -v timeout >/dev/null 2>&1; then
        timeout 60 qemu-system-x86_64 "${QEMU_ARGS[@]}" -display none || true
    else
        qemu-system-x86_64 "${QEMU_ARGS[@]}" -display none &
        QP=$!; sleep 60; kill $QP 2>/dev/null; wait $QP 2>/dev/null || true
    fi
    echo "--- serial64.log ---"
    cat serial64.log 2>/dev/null || echo "(no serial output)"
    echo "--------------------"
    PASS=0
    grep -q "KERNEL64.*boot start" serial64.log 2>/dev/null && \
    grep -q "EFER.LME=1" serial64.log 2>/dev/null && \
    grep -q "BOOTED KERNEL64 LOOP" serial64.log 2>/dev/null && \
    grep -q "M2 GDT64 OK" serial64.log 2>/dev/null && \
    grep -q "EXC3 OK" serial64.log 2>/dev/null && \
    grep -q "M3 SELFTEST OK" serial64.log 2>/dev/null && \
    grep -q "M10 HEAP SELFTEST OK" serial64.log 2>/dev/null && \
    grep -qE "heap: arena 0x[0-9a-f]+ max [0-9]+MB frame-backed" serial64.log 2>/dev/null && \
    grep -q "M11 BLK SELFTEST OK" serial64.log 2>/dev/null && \
    grep -q "blk: iso PVD lba16 type=1 id=CD001" serial64.log 2>/dev/null && \
    grep -qE "blk: disk sector0 magic=ok .*lba28 fnv=0x[0-9A-F]{16} lba48 fnv=0x[0-9A-F]{16} same=1" serial64.log 2>/dev/null && \
    grep -qE "blk: iso reread rc=0 stable=1" serial64.log 2>/dev/null && \
    grep -q "cons: framebuffer console live" serial64.log 2>/dev/null && \
    grep -qE "cons: [0-9]+x[0-9]+ cells \([0-9]+x[0-9]+ fb" serial64.log 2>/dev/null && \
    grep -q "mouse: PS/2 aux on" serial64.log 2>/dev/null && \
    grep -qE "cons: rehomed [0-9]+x[0-9]+ cells" serial64.log 2>/dev/null && \
    grep -qE "gui: desktop up win=[0-9]+,[0-9]+,[0-9]+,[0-9]+ client=" serial64.log 2>/dev/null && \
    grep -q "M6 SMP OK" serial64.log 2>/dev/null && \
    grep -q "M6 ncpus=4" serial64.log 2>/dev/null && \
    grep -q "M6 IPI-OK 3/3 TLB-OK 3/3" serial64.log 2>/dev/null && \
    grep -q "M4 TASK OK" serial64.log 2>/dev/null && \
    grep -q "HELLO-DONE" serial64.log 2>/dev/null && \
    grep -q "FPU d=500500.000000 f=1000.000000 ld=1000" serial64.log 2>/dev/null && \
    grep -q "FPU-DONE" serial64.log 2>/dev/null && \
    grep -q "CLONE-DONE" serial64.log 2>/dev/null && \
    test "$(grep -c "WORKER-DONE" serial64.log 2>/dev/null)" -ge 2 && \
    grep -q "FORK child reread 1229782938247303441" serial64.log 2>/dev/null && \
    grep -q "FORK parent reread 2459565876494606882" serial64.log 2>/dev/null && \
    grep -q "FORK-CHILD-DONE" serial64.log 2>/dev/null && \
    grep -q "FORK-DONE" serial64.log 2>/dev/null && \
    grep -q "EXEC-PRE" serial64.log 2>/dev/null && \
    grep -q "EXECCHILD-RAN" serial64.log 2>/dev/null && \
    ! grep -q "EXEC-POST" serial64.log 2>/dev/null && \
    ! grep -q "EXEC-FAIL" serial64.log 2>/dev/null && \
    grep -q "SHELLTEST-DONE" serial64.log 2>/dev/null && \
    ! grep -q "SHELLTEST-MISMATCH" serial64.log 2>/dev/null && \
    ! grep -q "SHELLTEST-SPAWN-FAIL" serial64.log 2>/dev/null && \
    grep -q "MCT SHELL" serial64.log 2>/dev/null && \
    grep -q "BRK touched=64 frames=" serial64.log 2>/dev/null && \
    grep -q "BRK-DONE" serial64.log 2>/dev/null && \
    ! grep -q "BRK-MISMATCH" serial64.log 2>/dev/null && \
    ! grep -q "BRK-GROW-FAIL" serial64.log 2>/dev/null && \
    ! grep -q "BRK-SHRINK-FAIL" serial64.log 2>/dev/null && \
    grep -q "NX-OK" serial64.log 2>/dev/null && \
    grep -q "RO-OK" serial64.log 2>/dev/null && \
    grep -q "NX-DONE" serial64.log 2>/dev/null && \
    ! grep -q "NX-SURVIVED" serial64.log 2>/dev/null && \
    ! grep -q "RO-SURVIVED" serial64.log 2>/dev/null && \
    ! grep -q "BADSTATUS" serial64.log 2>/dev/null && \
    test "$(grep -a -o "base=[0-9]*" serial64.log 2>/dev/null | sort -u | wc -l)" -ge 2 && \
    grep -q "SMP-DONE" serial64.log 2>/dev/null && \
    test "$(grep -a "CPU-WORKER" serial64.log 2>/dev/null | grep -o "cpu=[0-9]" | sort -u | wc -l)" -ge 4 && \
    ! grep -q "FATAL" serial64.log 2>/dev/null && \
    ! grep -q "FAIL" serial64.log 2>/dev/null && \
    grep -q "tick 500" serial64.log 2>/dev/null && PASS=1
    if [ "$PASS" = "1" ]; then
        echo "[+] M11 BOOT OK: SMP + fork/exec + shell + brk/demand + W^X + ASLR + GUI desktop + heap + ATA/ATAPI"
        exit 0
    else
        echo "[-] M11 BOOT FAIL: markers missing (see serial64.log above)"
        exit 1
    fi
else
    echo "[*] Interactive QEMU (close window to exit)..."
    qemu-system-x86_64 "${QEMU_ARGS[@]}"
fi
