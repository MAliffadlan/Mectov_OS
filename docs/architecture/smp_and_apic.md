# Multi-Core (SMP) & APIC Architecture

Mectov OS features Symmetric Multiprocessing (SMP) support on x86 32-bit hardware, allowing the kernel to boot and utilize up to 16 CPU cores (tested with 4 CPU cores under QEMU `-smp 4`).

---

## ⚡ ACPI & MADT Parsing (`src/sys/acpi.c`)

1. **RSDP & RSDT Lookup**:
   - Searches the Extended BIOS Data Area (EBDA) and BIOS memory range (`0x000E0000` to `0x00100000`) for the `"RSD PTR "` signature and validates the 20-byte checksum.
   - Obtains the RSDT (Root System Description Table) physical memory address.

2. **MADT (Multiple APIC Description Table) Parsing**:
   - Locates the `"APIC"` signature in RSDT pointers.
   - Extracts the physical address of the Local APIC (`0xFEE00000`) and I/O APIC (`0xFEC00000`).
   - Parses **Type 0 (Processor Local APIC)** entries to build `smp_lapic_ids[]` array and record active CPU APIC IDs.
   - Parses **Type 1 (I/O APIC)** entries to record the I/O APIC base address.
   - Parses **Type 2 (Interrupt Source Override - ISO)** entries to dynamically map legacy ISA IRQs to Global System Interrupts (GSIs). On QEMU, legacy PIT IRQ0 is overridden to GSI 2 (`smp_pit_gsi = 2`).

---

## 🔔 APIC & IOAPIC Initialization (`src/sys/apic.c`)

1. **Legacy PIC Disabling**:
   - Sends `0xFF` to I/O ports `0x21` and `0xA1` to completely mask and disable the legacy 8259 Programmable Interrupt Controller (PIC).

2. **Local APIC (LAPIC)**:
   - Identity maps Local APIC page `0xFEE00000` in `vmm.c`.
   - Enables LAPIC by setting Spurious Interrupt Vector Register (SIVR) bit 8 and vector 0xFF.
   - Sends End-of-Interrupt (EOI) via `apic_send_eoi()` by writing `0` to offset `0xB0`.

3. **I/O APIC Routing**:
   - Identity maps I/O APIC page `0xFEC00000`.
   - Routes hardware interrupts via 64-bit Redirection Table entries:
     - **PIT Timer**: Dynamic GSI `smp_pit_gsi` mapped to Vector 32 (IRQ 0).
     - **PS/2 Keyboard**: GSI 1 mapped to Vector 33 (IRQ 1).
     - **PS/2 Mouse**: GSI 12 mapped to Vector 44 (IRQ 12).
     - Other redirection table pins are masked (`0x10000`) to prevent spurious interrupts.

---

## 🔄 Application Processor (AP) Boot Protocol (`src/sys/smp.c`)

1. **Trampoline Placement (`smp_trampoline.asm`)**:
   - The 16-bit real mode trampoline code is copied into low physical memory at address `0x8000`.
   - The Bootstrap Processor (BSP) stores the current page directory (`CR3`) and entry stack pointers into memory variables accessible by the AP trampoline.

2. **INIT-SIPI-SIPI Sequence**:
   - **INIT IPI**: BSP sends an INIT Inter-Processor Interrupt to the target AP via Local APIC ICR (Interrupt Command Register).
   - **Delay**: Waits 10 milliseconds.
   - **Startup IPI (SIPI 1)**: Sends SIPI with vector `0x08` (pointing to address `0x8000`).
   - **Delay**: Waits 200 microseconds.
   - **Startup IPI (SIPI 2)**: Sends second SIPI if AP is not yet awake.

3. **AP Execution Flow**:
   - AP starts in 16-bit Real Mode at physical address `0x8000`.
   - Enables Protected Mode (sets CR0 PE bit).
   - Loads boot GDT and transitions to 32-bit Protected Mode.
   - Loads kernel page directory (`CR3`) and enables Paging (sets CR0 PG bit).
   - Initializes its own per-core GDT, IDT, Local APIC, and enters the idle loop.
