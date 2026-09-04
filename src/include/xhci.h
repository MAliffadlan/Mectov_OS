#ifndef XHCI_H
#define XHCI_H

#include "types.h"

// ============================================================
// xHCI host controller + USB 3.0 mass-storage (v38.56)
// ============================================================
// PCI class 0x0C / subclass 0x03 / prog_if 0x30 (xHCI). The controller is
// found on PCI as a 64-bit BAR0 memory region (QEMU's qemu-xhci =
// vendor 1b36 device 000d, 64KB BAR, lands in the identity-mapped PCI
// MMIO window). Devices behind it enumerate through the xHCI protocol:
// port reset -> Enable Slot -> Address Device (input context) -> device +
// configuration descriptors -> Configure Endpoint (bulk IN/OUT) -> USB
// mass-storage BOT (CBW/data/CSW) wrapping SCSI commands.
//
// SuperSpeed (USB 3.0) devices sit on the host's USB3 ports (QEMU bus
// "xhci.<n>.0" side; PORTSC speed = 4). The same enumeration flow covers
// USB2 high-speed devices (speed = 3) — only the control-endpoint max
// packet size differs (512 vs 64).
//
// Mass-storage devices register as drives 8+ (USB_DRIVE_BASE) on the
// existing ATA sector API, exactly like AHCI drives 4+ (v38.50): the
// ata.c dispatcher routes drive >= USB_DRIVE_BASE here, so ext2/FAT32/
// pcache and runtime `mount` work on USB volumes with zero FS changes.
//
// Completion model: poll, like the AHCI driver — everything runs under
// xhci_lock with IRQs off; the event ring is drained by polling the cycle
// bit, no interrupter enable is armed. (The kernel has no MSI support and
// the IOAPIC masks unknown GSI lines; a future version may wire INTx.)
//
// QEMU facts this driver relies on (hw/usb/hcd-xhci.c, v8.2.2):
//   * DBOFF = 0x2000, RTSOFF = 0x1000 (fixed OFF_DOORBELL/OFF_RUNTIME)
//   * ports at operational + 0x400 + 0x10*n
//   * HCSPARAMS2 = 0xF -> MaxScratchpadBufs = 0 (no scratchpad support)
//   * USBCMD.HCRST completes synchronously (CNR never sets)

#define USB_DRIVE_BASE 8          // drives 0-3 IDE, 4-7 AHCI, 8+ USB
#define USB_MAX_DRIVES 4          // mass-storage units we bring up
#define USB_MAX_HID 4             // HID keyboard/mouse devices we bring up

// What usb_enumerate found behind a port (kind field of the device struct).
#define USB_KIND_NONE    0
#define USB_KIND_STORAGE 1
#define USB_KIND_HID_KBD 2   // HID boot-protocol keyboard (iface 03/01/01)
#define USB_KIND_HID_MOUSE 3 // HID boot-protocol mouse    (iface 03/01/02)

// Detect + bring up the controller and every mass-storage / HID device
// behind it (call after pci_scan, next to ahci_init). Safe when no
// controller exists: logs and leaves. Registered drives report 1 from
// usb_present. HID devices are left armed; the main loop keeps the stream
// alive by calling xhci_hid_poll().
void xhci_init(void);
int  usb_present(void);

// Drain pending HID interrupt-IN reports into the shared keyboard scancode
// queue / mouse state (see keyboard.h kbd_feed_scancode + mouse.h
// mouse_hid_report). No-op in microseconds when there is no controller or
// no HID device. Must be called from the same single-writer contexts the
// PS/2 IRQs feed from — the kernel main loop and the login gate, once per
// iteration, before input is read. PS/2 remains the fallback: when no USB
// HID keyboard/mouse is present the PS/2 devices are the only feeders.
void xhci_hid_poll(void);

// Sector API on a USB drive number (drive as passed to the ATA layer,
// i.e. USB_DRIVE_BASE + device). count is clamped like the IDE batch API.
// Returns 0 on success, -1 on error (controller absent is also -1).
int usb_read_sectors(int drive, uint32_t lba, int count, uint8_t* buf);
int usb_write_sectors(int drive, uint32_t lba, int count, const uint8_t* buf);

#endif
