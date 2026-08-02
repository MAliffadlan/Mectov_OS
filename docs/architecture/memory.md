# Memory Management Architecture

Mectov OS implements a two-tier memory manager comprising a Physical Memory Manager (PMM) and a Virtual Memory Manager (VMM), featuring two-level x86 paging, process heap isolation, and page table safety guarantees.

---

## 🧠 Physical Memory Manager (PMM) (`src/sys/mem.c`)

1. **Page Frame Bitmap**:
   - Manages physical RAM in 4KB page frames (`4096` bytes).
   - Uses a bit array (`pmm_bitmap[]`) where `0` represents a free page and `1` represents an allocated page.

2. **Memory Map Parsing**:
   - `init_mem(uint32_t mem_size)` parses the Multiboot memory map provided by GRUB.
   - Computes total pages: `total_pages = (mem_size + 4095) / 4096`.
   - Reserves the first 1MB of physical memory (BIOS/IVT/EBDA/VGA MMIO) and the kernel executable code region (`_kernel_start` to `_kernel_end`).

3. **Allocation API**:
   - `pmm_alloc_page()`: Scans bitmap for first free bit, marks as used, returns physical address.
   - `pmm_free_page(uint32_t page_addr)`: Clears target bit in bitmap.

---

## 🌐 Virtual Memory Manager (VMM) & Paging (`src/sys/vmm.c`)

1. **Two-Level Paging Architecture**:
   - **Page Directory (1024 PDE entries)**: Points to 1024 Page Tables.
   - **Page Table (1024 PTE entries)**: Maps virtual addresses to 4KB physical frames.
   - Virtual Address Breakdown: `[PDE Index (10 bits) | PTE Index (10 bits) | Offset (12 bits)]`.

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
   - Validates node indices (`new_dir >= 0` and `new_file >= 0`) during Ext2 VFS tree population to prevent array underflow writes (`fs_nodes[-1]`) when maximum VFS node limit (64) is reached.
