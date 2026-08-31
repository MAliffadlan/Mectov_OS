# xHCI / USB 3.0 Host Controller + Mass-Storage

Mectov OS speaks USB 3.0 natively: a from-scratch xHCI host-controller
driver (`src/drivers/xhci.c`) that enumerates devices through the xHCI
protocol and carries them all the way to the existing drive/VFS stack as
SCSI bulk-storage units.

---

## 🎯 Scope (v38.56)

* **Host controller**: any PCI `0C/03/30` (xHCI) — QEMU's `qemu-xhci`
  (vendor `1b36:000d`, 64KB 64-bit MMIO BAR in the identity-mapped window
  `0xFE000000..0xFF000000`).
* **Speeds**: SuperSpeed (USB 3.0, PORTSC speed 4) and high-speed
  (USB 2.0, speed 3). Full/low-speed devices are logged and skipped —
  they need the two-step EP0 max-packet-size probe.
* **Device class**: USB Mass-Storage, Bulk-Only Transport
  (interface class 08 / subclass 06 / protocol 50) wrapping SCSI
  (INQUIRY, TEST UNIT READY, READ CAPACITY(10), READ(10), WRITE(10)).
* **Drive numbering**: IDE = 0–3, AHCI = 4–7 (`AHCI_DRIVE_BASE`), USB = 8+
  (`USB_DRIVE_BASE`) on the same sector API — ext2/FAT32/pcache and
  `mount` work on USB volumes with zero filesystem changes.

---

## 🔌 How a USB stick becomes `drive 8`

```
+---------------------------------------------------------------+
|  ext2 / FAT32 / pcache / `mount /usb fat32 8`                 |
+---------------------------------------------------------------+
|  ATA sector API dispatch (ata.c): drive >= USB_DRIVE_BASE     |
+---------------------------------------------------------------+
|  SCSI over BOT: READ(10)/WRITE(10) in CBW -> data -> CSW      |
+---------------------------------------------------------------+
|  USB enumeration: port reset -> Enable Slot -> Address Device |
|  -> descriptors -> Set Config -> Configure Endpoint (bulk)    |
+---------------------------------------------------------------+
|  xHCI: command ring (CRCR), event ring + ERST (poll),         |
|  doorbells, DCBAA slot contexts, EP transfer rings            |
+---------------------------------------------------------------+
|  PCI 0C/03/30, BAR0 64-bit MMIO in the uncached window        |
+---------------------------------------------------------------+
```

### Controller bring-up
1. Halt (USBCMD.RUN=0 → USBSTS.HCH=1), then HCRST (QEMU resets
   synchronously; CNR is still polled for real hardware).
2. Command ring: 256 TRBs + Link TRB (TC toggles the cycle), armed via
   CRCR (base | cycle, low dword then high).
3. Event ring: one 256-TRB segment via ERSTSZ/ERSTBA; ERDP tracks the
   dequeue pointer (EHB cleared on every consume). **Poll model**:
   IMAN.IE stays off — completions are found by watching the event ring's
   cycle bit, exactly like the AHCI driver polls PxCI.
4. DCBAA (MaxSlots+1 pointers; QEMU reports no scratchpad buffers) and
   CONFIG.MaxSlotsEn, then RUN.

### Device enumeration (per connected port)
1. Port reset (PORTSC.PR, preserving PP), wait for PR to self-clear,
   clear the change latches (CSC/PRC are write-1-to-clear), check PED.
2. **Enable Slot** command — the completion event carries the slot id.
3. **Address Device** (BSR=0): input context = {drop=0, add=A0|A1} +
   slot context (route string = root port, root-port field) + EP0
   context (control type, MPS 512 for SS / 64 for HS, empty ring,
   DCS=1). QEMU assigns the USB address itself (= slot id).
4. GET DEVICE/CONFIGURATION DESCRIPTOR over EP0 (Setup/Data/Status
   TRBs, the 8 setup bytes inline via IDT), find the MSC BOT interface
   and its two bulk endpoints (DCI = 2*ep + dir).
5. SET CONFIGURATION + **Configure Endpoint** (add A0 + the two bulk
   DCIs, EP contexts with type bulk-out=2/bulk-in=6, MPS from the
   descriptor, ring + DCS).
6. SCSI over BOT: TEST UNIT READY (retried), INQUIRY (must be
   direct-access type 0), READ CAPACITY(10) (512-byte sectors) —
   then the device registers as `drive 8+n`.

### Data path (per sector batch, ≤128 sectors like the IDE/AHCI API)
CBW(31B) on the bulk-OUT ring → data Normal-TRB into the 64K-aligned
bounce buffer (DIR bit matching the ring direction) → CSW(13B) on the
bulk-IN ring, each phase awaited by its Transfer Event (IOC on the
phase's single TRB). The CSW signature/tag/status are validated before
the caller sees data.

---

## 🔒 Locking & completion model

Everything runs under `xhci_lock` with IRQs off (mirroring `ata_lock` /
`ahci_lock`): the transfer path polls the event ring for completion, so
no interrupt is ever needed — the kernel has no MSI support and the
IOAPIC masks unknown GSI lines, so an INTx dependency would add fragility
for nothing. A future version may wire INTx (unmask the GSI in
`ioapic_init`, route to a free vector, arm IMAN.IE) without changing
the transfer model.

All DMA structures are static, identity-mapped .bss with explicit
alignment (rings 64-byte, contexts 64-byte, ERST 64-byte, bounce 64K) —
kernel VA == physical address, so pointers are handed to the controller
as-is. Every structure, register offset, TRB bit and context bit used
here was verified against QEMU 8.2.2's `hw/usb/hcd-xhci.c` — the exact
hardware the driver runs on in CI.

---

## 🧪 Testing

`scripts/usb_test.py` (CI: "xHCI/USB 3.0 driver regression") boots with
`-device qemu-xhci,id=xhci0 -device usb-storage,drive=usbd0,bus=xhci0.0`
— `bus=xhci0.0` is the **SuperSpeed** bus, so the stick enumerates at
speed 5000 (a genuine USB 3.0 round trip). It asserts:

1. `[XHCI] controller @` + `USB3 -> drive 0x00000008` + `[XHCI] ready`
   during boot init.
2. `mount /usb fat32 8` → `[MOUNT] mounted /usb` — the FAT32 BPB is read
   through xHCI/BOT/SCSI (read-path proof).
3. `cp /usb/HELLO.TXT /usb/COPY.TXT` inside the volume, then the HOST
   reads COPY.TXT back with mtools and byte-compares it (write-path
   proof — the data really landed on the USB disk).
4. The OS stayed alive (no panic) after the round trip.
