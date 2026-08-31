#!/bin/bash
# Temporary bring-up helper (v38.56 xHCI development) — boots the freshly
# built ISO headless with the qemu-xhci + usb-storage fleet and prints the
# xHCI lines from the serial log. Not part of the test suite; usb_test.py
# is the CI-facing harness.
set -u
cd "$(dirname "$0")/.."   # project root (this script lives in scripts/)
rm -f serial_debug.log
timeout 150s qemu-system-i386 \
    -cpu qemu32,+nx -m 128 -smp 4 -display none -vga std \
    -cdrom mectov.iso \
    -serial file:serial_debug.log \
    -net none \
    -drive file=disk.img,format=raw,index=0,media=disk \
    -drive file=ext2.img,format=raw,index=1,media=disk \
    -device qemu-xhci,id=xhci0 \
    -drive file=usb.img,format=raw,if=none,id=usbd0 \
    -device usb-storage,drive=usbd0,bus=xhci0.0 \
    >/dev/null 2>&1
echo "--- xHCI serial lines ---"
grep -n "XHCI\|xhci\|MOUNT" serial_debug.log | head -30
echo "--- last 3 lines ---"
tail -3 serial_debug.log
