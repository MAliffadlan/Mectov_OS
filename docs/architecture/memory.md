# Memory Management Architecture

Mectov OS implements a two-tier memory manager comprising a Physical Memory Manager (PMM) and a Virtual Memory Manager (VMM), featuring **PAE 3-level x86 paging with 64-bit PTEs and NX (W^X)** since v38.49, process heap isolation, and page table safety guarantees. (The intro said "two-level x86 paging" until v38.161; the description two sections below has been the accurate one since the PAE migration.)

---

## 🧠 Physical Memory Manager (PMM) (`src/sys/mem.c`)

1. **Page Frame Bitmap**:
   - Manages physical RAM in 4KB page frames (`4096` bytes).
   - Uses a bit array (`pmm_bitmap[]`) where `0` represents a free page and `1` represents an allocated page.

2. **Memory Map Parsing**:
   - `init_mem(uint32_t mem_size)` takes the *scalar* size the caller read from the Multiboot **header** (`mem_lower`/`mem_upper`-derived, see `kernel.c`); it does **not** walk the Multiboot mmap array.
   - Computes total pages by flooring: `total_pages = mem_size / PAGE_SIZE`.
   - The distinction has teeth (v38.156): RAM size and the *mapped span* are different quantities, and the ACPI table bound bug lived exactly in that gap — a table pointer is only valid if paging_init() mapped it, not merely if it fits in RAM.
   - Reserves the first 1MB of physical memory (BIOS/IVT/EBDA/VGA MMIO) and the kernel executable code region (`_kernel_start` to `_kernel_end`).

3. **Allocation API**:
   - `pmm_alloc_page()`: Scans bitmap for first free bit, marks as used, returns physical address.
   - `pmm_free_page(uint32_t page_addr)`: Clears target bit in bitmap.

---

## 🌐 Virtual Memory Manager (VMM) & Paging (`src/sys/vmm.c`)

1. **PAE 3-Level Paging (v38.49+, 64-bit PTEs)**:
   - **PDPT (4 entries) → PD (512) → PT (512)**: `pte_t` 64-bit, `PAGE_NX` (63), `PAGE_DEV` (11, scanout), `PAGE_COW`/`PAGE_SHARED`.
   - Virtual Address Breakdown: `[PDPT 2 bits | PD 9 bits | PT 9 bits | Offset 12 bits]` — CR4.PAE + EFER.NXE, `-cpu qemu32,+nx`.
   - Legacy 2-level description kept for history; current kernel is PAE-only.

2. **Identity Mapping & Special Regions**:
   - `0x00000000` – `0x00400000` (First 4MB): Identity mapped for Kernel Code & System Data structures (Present, Read/Write, Supervisor).
   - **VBE Framebuffer**: Identity mapped based on `framebuffer_addr` and size provided by Multiboot info.
   - **MMIO Devices**: Local APIC (`0xFEE00000`) and I/O APIC (`0xFEC00000`) explicitly identity mapped in PDE `1019` and `1018`.

3. **Process Heap Isolation (`loader.c` & `syscall.c`)**:
   - Userland executable binaries (`.mct`) are loaded starting at virtual address `0x08000000` (128MB).
   - Initial heap pointer (`heap_ptr`) is positioned immediately after the loaded application pages (`0x08000000 + (num_pages * 4096)`), preventing heap allocations from overwriting program code.
   - Maximum user heap growth is bounded at `0x08F00000` to prevent collision with shared libraries loaded at `0x09000000`.

---

## 🔒 Task Tear-Down & Memory Safety

1. **Address Space Use-After-Free Prevention (`task.c`)**:
   - During task termination (`task_exit`), the kernel MUST NOT free the page directory while the CPU `CR3` register points to it.
   - `task_cleanup()` switches the active CPU `CR3` to the kernel boot page directory (`tasks[0].page_dir`) *before* invoking `vmm_free_address_space()`.

2. **Ext2 VFS Traversal Bounds Check (`src/sys/ext2.c`)**:
   - Validates node indices (`new_dir >= 0` and `new_file >= 0`) during Ext2 VFS tree population to prevent array underflow writes (`fs_nodes[-1]`) when the VFS node limit is reached. That limit is `MAX_NODES = 2048` (`src/include/vfs.h`; layout v5 since v38.141) — the 64 quoted here until v38.161 was the pre-v38.23 value.

---

## 🛡️ PAE + NX / W^X (v38.49)

The kernel moved from 2-level (1024-entry, 32-bit PTE) paging to **PAE
3-level** — PDPT (4 × 512MB) → PD (512 × 2MB) → PT (512 × 4KB) — with
64-bit entries (`pte_t`), and enables **EFER.NXE** so bit 63 of an entry
means no-execute.

- **Layout**: every address space is a PDPT frame (`task.page_dir` / CR3);
  the boot structures are static identity globals (`boot_pdpt` /
  `boot_pds` / 256×2MB identity PTs in `mem.c`). CR4.PAE is set before
  CR0.PG; the SMP trampoline sets it per-AP before enabling paging, and
  every AP repeats the EFER.NXE MSR write (`paging_enable_nxe`).
- **W^X policy**: every user DATA mapping carries PAGE_NX — heap and stack
  demand-zero pages, anonymous `mmap`, file-backed `mmap`, SysV shared
  memory, and `SYS_VMM_ALLOC`. Only images (`.mct` / `.elf` plus the shared
  library) and the signal trampoline are meant to stay executable. A
  page-fault with the I/D bit set (instruction fetch) is NEVER demand-mapped
  — it logs `[W^X] execute fault` and kills the task with SIGSEGV (or panics
  in Ring 0).
  Every site is gated on `paging_nx_enabled()`, because without EFER.NXE bit
  63 is a reserved PTE bit and setting it there would fault the mapping
  itself — `config-matrix` and `ram-sweep` boot a pre-NX CPU on purpose. The
  flags are `uint64_t` for the same reason: `PAGE_NX` is bit 63, and a
  `uint32_t` silently truncates it to 0.
  Two probes cover the policy, one QEMU boot each (`scripts/nxtest.py`):
  `apps/nxtest.mct` places a `ret` on its stack and `apps/mmapnx.mct` places
  one in an `mmap`'d page; each asserts the fetch fault and the SIGSEGV.
- **Known gap (v38.164 audit)**: two other sites *intend* PAGE_NX but store
  it in a `uint32_t`, so the assignment truncates to 0 and no NX is applied —
  the fault-in argument page (`syscall.c`, `SYS_...FAULTIN`) and the
  framebuffer window (`task.c`). Neither is covered by a probe. Fixing them
  means widening those two locals to `uint64_t`.
- **Fork fix found during migration**: the old clone path re-allocated a
  fresh PT for every kernel-region PDE and overwrote the copy
  `vmm_create_address_space` had just made — leaking ~120 frames (≈0.5MB)
  per fork. The PAE clone COW-copies only the USER ptes inside
  kernel-region tables and reuses the create-time copies.
- **QEMU note**: the default `qemu32` CPU model lacks the NX CPUID bit —
  all harnesses now pass `-cpu qemu32,+nx` (the kernel degrades cleanly
  to PAE-without-NX and logs `NX unavailable` if the feature is absent).

### What the paging stack does *not* defend against (v38.164)

Measured, not assumed. A standalone CPUID probe under every CPU model the
harnesses use:

| `-cpu` | max basic leaf | SMEP (7:EBX[7]) | SMAP (7:EBX[14]) |
|---|---|---|---|
| `qemu32` (the default) | **0x04** | leaf absent | leaf absent |
| `pentium3` | 0x03 | leaf absent | leaf absent |
| `core2duo` | 0x0A | no | no |
| `Nehalem` | 0x0B | no | no |
| `qemu64` | 0x0D | no | no |

- **SMEP is implemented but never exercised.** `paging_enable_smep()` sets
  CR4.SMEP when the CPU reports it, and the boot banner says which of three
  states it landed in (`enabled` / `cpu-absent` / `cpuid-leaf7-absent`). No
  i386 QEMU model advertises the bit, so under CI it is always a no-op. The
  code is correct for bare metal; the *coverage* does not exist.
- **SMAP is impossible here, by design.** It requires 4-level paging
  (Intel SDM Vol 3A §4.6) and this kernel is PAE **3**-level, so CR4.SMAP
  would `#GP`. Even ignoring that, no i386 QEMU model exposes it, and every
  deliberate user-memory access in the syscall layer would need `stac`/`clac`
  bracketing.
- **The probe bug this uncovered**: `paging_enable_smep()` issued `cpuid`
  with EAX=7 without first asking CPUID.0 for the maximum leaf. Intel SDM
  Vol 2A makes an out-of-range leaf's EBX/ECX/EDX *undefined*, and because
  CPUID is normally a comparison chain the bad leaf falls through and returns
  a real-looking register — measured EBX = 0x3F on `qemu32`, whose real
  maximum is 4. The old code then tested bit 7 of that garbage.
  `paging_enable_nxe()` had the same shape against extended leaf
  0x80000001. Both now gate on the maximum leaf first, and
  `scripts/cpuid_test.py` (CI, `boot-core`) fails if the check is removed —
  verified by reintroducing the bug and watching the suite catch it.
- **gcc's `-fstack-protector` is not usable on this kernel.** i386 gcc
  supports exactly one guard location, `%gs:0x14`, and `%gs` here is the
  per-task TLS segment whose base is the user TCB — offset 0x14 lands inside
  the app-visible scratch area (`MCT_TLS_SCRATCH_OFFSET = 16`). There is no
  `-mstack-protector-guard=` option for i386 that avoids this. Doing it
  properly needs a per-CPU kernel canary loaded at the three Ring 0 entry
  points (`irq_common_stub`, `isr_common_stub`, `isr128`) — the most
  safety-critical code in the kernel. Not attempted; the heap already carries
  magic + canary (`sys/mem.c`), the stack does not.
- **Still missing**: kernel ASLR (the image is linked at a fixed 1 MB,
  `linker.ld`), stack canaries outside the heap, SMAP, and any form of
  page-out — exhaustion is an OOM, never a swap.
