# AHCI / SATA Host Controller

Mectov OS speaks SATA natively: an AHCI driver (`src/drivers/ahci.c:320`) that finds an Intel ICH9-style HBA on PCI and carries SATA disks to the existing drive/VFS stack.

---

## 🎯 Scope (v38.50)

* **Host controller**: PCI `01/06/01` (AHCI) — QEMU `ich9-ahci` (`-device ahci,id=ahci`), BAR5 32-bit MMIO in the identity-mapped window `0xFE000000..0xFF000000` (`mem.c` 8 PTs, PCD|PWT).
* **Ports**: Implements Port Implemented `PI` scan, `SSTS.DET=3` check, ATAPI signature `0xEB14` skip — only ATA disks.
* **Drive numbering**: `AHCI_DRIVE_BASE=4` — ports register as `4..7` (`AHCI_MAX_PORTS=4`) on the same sector API as IDE `0..3`; `ext2`/`FAT32`/`pcache` and `mount` work unchanged.

---

## 🔌 How a SATA disk becomes `drive 4`

```
+---------------------------------------------------------------+
|  ext2 / FAT32 / pcache / `mount /sata fat32 4`                |
+---------------------------------------------------------------+
|  ATA sector API dispatch (ata.c): drive >= AHCI_DRIVE_BASE    |
+---------------------------------------------------------------+
|  AHCI: RBs LBA48 READ DMA EXT 0x25 / WRITE DMA EXT 0x35 via  |
|  PRD + H2D FIS, poll PxCI under ahci_lock                     |
+---------------------------------------------------------------+
|  PCI 01/06/01, BAR5 32-bit MMIO in uncached window             |
+---------------------------------------------------------------+
```

### Bring-up

1. Scan PCI `01/06/01`, BAR5 `& ~0xF` must be inside window, `page_map` with `0x18` (PCD|PWT), enable `PCI CMD 0x6` (bus-master).
2. Set `GHC.AE=31`, read `PI`, for each set bit check `PxSSTS.DET` and `PxSIG` (skip ATAPI).
3. `ahci_port_start(p)` — stop engine (`CMD.ST/FRE` clear, wait `CR/FR`), program `PxCLB`/`PxFB` (low/high, `CLB` 1K, `FB` 256), clear `PxIS`/`PxSERR`, set `SUD|FRE|ST`.

DMA structures per port (`ahci_port_mem_t` 4K-aligned): `clist[32]` 1K, `rfis` 256, `table` 4K (`cfis` 64 + `prdt[1]`), plus one `64K`-aligned `ahci_bounce` shared under `ahci_lock`.

### Command issue (slot 0, one PRD)

1. Header `flags=5|C|W`, `prdtl=1`, `ctba=table` — `prdbc` field occupies DW1 (bug: omitting it put `ctba` in the wrong slot).
2. H2D FIS `0x27` `C=1` — LBA48: `f[4..6]=LBA23:0`, `f[7]=0x40`, `f[8..10]=LBA47:24`, `f[12]=count`, opcode `0x25`/`0x35`.
3. PRD `dba=bounce`, `dbc_i=(len-1)|31` — `len<=64K` (128 sectors) so one entry suffices and never crosses 64K.
4. Clear `PxIS`/`PxSERR`, wait `PxTFD & 0x88==0`, write `PxCI=1`, poll `PxCI &1==0` or `IS_TFES` error.

---

## 🔒 Locking & completion

Poll model under `ahci_lock` with IRQs off — mirrors `ata_lock`. `PxIE=0`, no MSI. `CONFIG` `MaxSlotsEn` not needed (AHCI). Every register offset verified against QEMU `hw/ide/ahci.c`.

---

## 🧪 Testing

`scripts/ahci_test.py` (CI: "AHCI/SATA driver regression") boots `ich9-ahci` + FAT32 `sata.img` on `drive 4`:

1. `[AHCI] controller @` + `port N SATA sig=` + `-> drive 4` + `[AHCI] ready`
2. `mount /sata fat32 4` — BPB via AHCI
3. `cp /sata/HELLO.TXT /sata/COPY.TXT` + host `mcopy` byte-compare
