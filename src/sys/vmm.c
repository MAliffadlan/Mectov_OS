#include "../include/vmm.h"
#include "../include/mem.h"
#include "../include/utils.h"
#include "../include/serial.h"
#include "../include/spinlock.h"

static spinlock_t vmm_lock = SPINLOCK_INIT;


// ============================================================
// Virtual Memory Manager — Layer di atas existing identity paging
// ============================================================
// Existing paging (mem.c) identity-maps 0..RAM (up to PHYS_MAX_PAGES*4KB).
// VMM adds physical frame tracking + per-process page directories
// WITHOUT touching the existing static page tables.
//
// page_dir = 0 → use global identity map (existing behavior)
// page_dir = any other value → use that page directory
//
// Physical frame tracking (single source of truth): the bitmap/refcount
// below is sized for PHYS_MAX_PAGES but its usable range is set once by
// phys_init() with the multiboot-detected RAM size (init_mem → phys_init),
// so a machine with 256MB or 512MB gets allocatable frames above 128MB.
// ============================================================

// One static bitmap for the whole system
static uint8_t frame_bitmap[PHYS_MAX_PAGES / 8];
uint8_t frame_ref_count[PHYS_MAX_PAGES];
static uint32_t phys_total_pages = 0;   // actual RAM / 4KB, from multiboot
static uint32_t phys_used_count = 0;    // maintained used-frame total (O(1) meminfo)
static int vmm_initialized = 0;

// Shared zero page for lazy zero-fill (heap + user stack demand paging).
// Every demand-paged page maps here READ-ONLY until the first write COW-
// duplicates it into a private frame, so processes that allocate but barely
// touch memory share a single frame instead of one zeroed frame each.
// Pinned: refcount 255 (never sole-owner, so a single process can never get
// a writable alias) and frame_free() ignores it outright.
static uint32_t phys_zero_page = 0;

// Kernel + heap region (first 48MB) is reserved; the define lives in mem.h so
// task.c (mmap/munmap frame checks) shares the same bound.

static void bitmap_set(int idx) {
    frame_bitmap[idx / 8] |= (1 << (idx % 8));
}
static void bitmap_clear(int idx) {
    frame_bitmap[idx / 8] &= ~(1 << (idx % 8));
}
static int bitmap_test(int idx) {
    return (frame_bitmap[idx / 8] >> (idx % 8)) & 1;
}

// Called exactly once, from init_mem(), with the RAM size reported by the
// multiboot header. This is the single point that decides how many physical
// frames exist; everything else (frame_alloc/frame_free bounds, /proc/meminfo,
// identity map in mem.c) derives from it.
void phys_init(uint32_t total_pages) {
    if (phys_total_pages != 0) return;  // already initialized
    phys_total_pages = total_pages;
    if (phys_total_pages > PHYS_MAX_PAGES) phys_total_pages = PHYS_MAX_PAGES;

    memset(frame_bitmap, 0, sizeof(frame_bitmap));
    memset(frame_ref_count, 0, sizeof(frame_ref_count));

    // Reserve the kernel + heap region (first 48MB)
    for (uint32_t i = 0; i < KERNEL_RESERVED_PAGES && i < phys_total_pages; i++) {
        bitmap_set(i);
        frame_ref_count[i] = 1;
    }
    phys_used_count = (KERNEL_RESERVED_PAGES < phys_total_pages)
                          ? KERNEL_RESERVED_PAGES : phys_total_pages;

    write_serial_string("[PHYS] allocator: ");
    write_serial_hex(phys_total_pages * 4096);
    write_serial_string(" bytes RAM, ");
    write_serial_hex(KERNEL_RESERVED_PAGES * 4096);
    write_serial_string(" reserved for kernel.\n");

    // Grab one frame for the shared zero page and pin it. Runs before paging
    // is enabled (paging_init is later), so direct physical access is fine.
    for (uint32_t i = KERNEL_RESERVED_PAGES; i < phys_total_pages; i++) {
        if (!bitmap_test(i)) {
            phys_zero_page = i * 4096;
            bitmap_set(i);
            frame_ref_count[i] = 255;  // pinned: never sole-owner, never freed
            phys_used_count++;
            memset((void*)(uintptr_t)phys_zero_page, 0, 4096);
            write_serial_string("[PHYS] shared zero page @ ");
            write_serial_hex(phys_zero_page);
            write_serial_string("\n");
            break;
        }
    }
}

// The shared zero page (0 if the allocator could not reserve one).
uint32_t phys_get_zero_page(void) { return phys_zero_page; }

// Mark a physical region (e.g. the framebuffer, MMIO) as in use so the
// allocator never hands it out. Called from paging_init() with the framebuffer
// address/size once they are known. Safe to call before/after phys_init.
void phys_reserve_region(uint32_t start, uint32_t len) {
    if (start == 0 || len == 0) return;
    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&vmm_lock);
    uint32_t first = start / 4096;
    uint32_t last = (start + len + 4095) / 4096;
    if (last > phys_total_pages) last = phys_total_pages;
    for (uint32_t i = first; i < last; i++) {
        if (frame_ref_count[i] == 0) phys_used_count++;  // overlapping reserves count once
        bitmap_set(i);
        frame_ref_count[i] = 1;
    }
    spin_unlock(&vmm_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
}

// Real used-frame count (reserved kernel region + live allocations) for
// /proc/meminfo — replaces the old `24MB + heap_used` approximation.
// v38.47: O(1) — the counter is maintained by phys_init / phys_reserve_region
// / frame_alloc / frame_free. Direct refcount bumps (COW copies, SysV shm
// attaches, fork clones) never move a frame between free and used, so they
// correctly leave it alone. Aligned uint32 reads are atomic on x86, so the
// read needs no lock — /proc no longer scans 131k entries under a spinlock
// with IRQs off.
uint32_t phys_get_used_pages(void) {
    return phys_used_count;
}

void vmm_init(void) {
    if (vmm_initialized) return;
    vmm_initialized = 1;
    // Frame tracking is set up earlier by phys_init() (called from init_mem
    // with the multiboot RAM size); this only marks the VMM address-space
    // layer as ready.
}

// Allocate one physical frame. Returns physical address or 0.
// Interrupt-safe: callers include load_mct_app (normal task context, IF=1)
// as well as the page fault handler and syscalls (IF=0 via interrupt gates).
// Disabling interrupts while holding the lock prevents the scheduler from
// preempting the holder (task.c convention), and restoring eflags keeps the
// caller's original IF state intact when this is nested in a cli section.
// IRQ/exception-context accessors for vmm_lock: the #PF (COW) handler runs
// with IF=0 on the per-CPU fault stack and needs a consistent view of
// frame_ref_count vs concurrent frame_alloc/frame_free on other cores. Plain
// spin is correct here (exception context, IF=0); a user fault never happens
// while the same CPU holds vmm_lock, so no self-deadlock.
void vmm_lock_acquire_irq(void) { spin_lock(&vmm_lock); }
void vmm_lock_release_irq(void) { spin_unlock(&vmm_lock); }

uint32_t frame_alloc(void) {
    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&vmm_lock);
    for (uint32_t i = KERNEL_RESERVED_PAGES; i < phys_total_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            frame_ref_count[i] = 1;
            phys_used_count++;
            spin_unlock(&vmm_lock);
            __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
            return (uint32_t)i * 4096;
        }
    }
    spin_unlock(&vmm_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
    write_serial_string("[VMM] OUT OF PHYSICAL FRAMES!\n");
    return 0;
}

void frame_free(uint32_t paddr) {
    if (paddr == 0) return;
    if (paddr == phys_zero_page) return;  // pinned shared zero page, never freed
    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&vmm_lock);
    uint32_t idx = paddr / 4096;
    if (idx >= KERNEL_RESERVED_PAGES && idx < phys_total_pages) {
        if (frame_ref_count[idx] > 0) {
            frame_ref_count[idx]--;
            if (frame_ref_count[idx] == 0) {
                bitmap_clear(idx);
                phys_used_count--;
            }
        }
    }
    spin_unlock(&vmm_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
}

// ============================================================
// Page directory management
// ============================================================
// A page directory is 1024 entries (4096 bytes).
// Entry format: [31:12] = page table phys addr | [11:0] = flags
//
// For per-process address spaces, we create a new page directory
// that identity-maps the kernel region + framebuffer,
// then maps process-private pages at high addresses.

// The kernel identity base in every address space:
// Identity map 0 → 16MB (kernel code, data, heap)
// Also map framebuffer (we don't know the addr here, so caller must add it)

// Zero a page (helper since we don't have a generic zero-phys-page)
static void zero_phys_page(uint32_t paddr) {
    // The kernel identity map covers 0..RAM, so every allocatable frame is
    // directly addressable. Anything outside that range is a caller bug.
    if (paddr < (phys_total_pages * 4096)) {
        memset((void*)(uintptr_t)paddr, 0, 4096);
    } else {
        write_serial_string("[VMM] zero_phys_page: paddr out of range!\n");
    }
}

// ---- PAE walk helpers (v38.49) ----
// An address space is a PDPT frame (page_dir). `space_pt` resolves the PT
// for vaddr, optionally creating the PD/PT frames on the way. Parent entries
// are widened to PAGE_USER when a user page is being mapped (the CPU checks
// U/S at every level). Returns the PT's kernel VA (identity) or 0.
static pte_t* space_pd(pte_t* pdpt, uint32_t vaddr, int create) {
    uint32_t pi = pdpt_index(vaddr);
    if (!(pdpt[pi] & PAGE_PRESENT)) {
        if (!create) return 0;
        uint32_t pd_frame = frame_alloc();
        if (pd_frame == 0) return 0;
        zero_phys_page(pd_frame);
        pdpt[pi] = (uint64_t)pd_frame | PAGE_PRESENT | PAGE_RW;
    }
    return (pte_t*)(uintptr_t)(uint32_t)(pdpt[pi] & PTE_ADDR_MASK);
}

static pte_t* space_pt(uint32_t pdpt_paddr, uint32_t vaddr, int create, int user) {
    pte_t* pdpt = (pte_t*)(uintptr_t)pdpt_paddr;
    pte_t* pd = space_pd(pdpt, vaddr, create);
    if (!pd) return 0;
    uint32_t di = pd_index(vaddr);
    if (!(pd[di] & PAGE_PRESENT)) {
        if (!create) return 0;
        uint32_t pt_frame = frame_alloc();
        if (pt_frame == 0) return 0;
        zero_phys_page(pt_frame);
        pd[di] = (uint64_t)pt_frame | PAGE_PRESENT | PAGE_RW;
    }
    if (user) {
        // The CPU requires PAGE_USER at EVERY level; identity-mapped PTEs
        // inside a cloned kernel PT still lack PAGE_USER individually, so
        // widening the parents keeps kernel memory protected.
        pdpt[pdpt_index(vaddr)] |= PAGE_USER;
        pd[di] |= PAGE_USER;
    }
    return (pte_t*)(uintptr_t)(uint32_t)(pd[di] & PTE_ADDR_MASK);
}

uint32_t vmm_create_address_space(void) {
    if (!vmm_initialized) vmm_init();

    // PDPT frame (only the first 32 bytes are meaningful; the frame keeps
    // the rest as scratch, PAE CR3 is 4K-aligned).
    uint32_t pdpt_paddr = frame_alloc();
    if (pdpt_paddr == 0) return 0;
    zero_phys_page(pdpt_paddr);
    pte_t* pdpt = (pte_t*)(uintptr_t)pdpt_paddr;

    // Clone every KERNEL page table from the boot structures into private
    // copies (identity map + APIC MMIO + framebuffer PTs). Sharing the
    // tables themselves would let user mappings corrupt the kernel's view;
    // cloning gives every space its own writable copy. Content is identical,
    // so fork() can later COW only the USER ptes inside these regions
    // (see vmm_clone_address_space).
    for (uint32_t pi = 0; pi < PDPT_ENTRIES; pi++) {
        if (!(boot_pdpt[pi] & PAGE_PRESENT)) continue;
        uint32_t pd_frame = frame_alloc();
        if (pd_frame == 0) {
            write_serial_string("[VMM] CLONE PD FAIL - out of frames!\n");
            vmm_free_address_space(pdpt_paddr);
            return 0;
        }
        zero_phys_page(pd_frame);
        pte_t* pd = (pte_t*)(uintptr_t)pd_frame;
        for (uint32_t j = 0; j < PT_ENTRIES; j++) {
            if (!(boot_pds[pi][j] & PAGE_PRESENT)) continue;
            uint32_t pt_frame = frame_alloc();
            if (pt_frame == 0) {
                write_serial_string("[VMM] CLONE PT FAIL - out of frames!\n");
                vmm_free_address_space(pdpt_paddr);
                return 0;
            }
            memcpy((void*)(uintptr_t)pt_frame,
                   (void*)(uintptr_t)(uint32_t)(boot_pds[pi][j] & PTE_ADDR_MASK), 4096);
            pd[j] = (uint64_t)pt_frame | (boot_pds[pi][j] & 0xFFF);
        }
        pdpt[pi] = (uint64_t)pd_frame | (boot_pdpt[pi] & 0xFFF);
    }

    // Map the signal trampoline page (user-readable, read-only) at the fixed
    // SIG_TRAMPOLINE_VA: signal handlers `ret` into it and it issues
    // SYS_SIGRETURN. If the frame allocation fails the address space still
    // works — default-action signals (which don't need the trampoline) are
    // unaffected. Kept in sync with SYS_SIGRETURN (75) in syscall.h.
    {
        uint32_t tp = frame_alloc();
        if (tp) {
            uint8_t* code = (uint8_t*)(uintptr_t)tp;
            code[0] = 0xB8; code[1] = 75; code[2] = 0; code[3] = 0; code[4] = 0; // mov eax, SYS_SIGRETURN
            code[5] = 0xCD; code[6] = 0x80;                                     // int $0x80
            code[7] = 0xC3;                                                     // ret (safety)
            vmm_map_page(pdpt_paddr, SIG_TRAMPOLINE_VA, tp, PAGE_PRESENT | PAGE_USER);
        }
    }

    write_serial_string("[VMM] Created address space (cloned PTs) OK\n");
    return pdpt_paddr;
}

void vmm_free_address_space(uint32_t page_dir) {
    if (page_dir == 0) return;  // Can't free global identity

    pte_t* pdpt = (pte_t*)(uintptr_t)page_dir;

    for (uint32_t pi = 0; pi < PDPT_ENTRIES; pi++) {
        if (!(pdpt[pi] & PAGE_PRESENT)) continue;
        pte_t* pd = (pte_t*)(uintptr_t)(uint32_t)(pdpt[pi] & PTE_ADDR_MASK);

        for (uint32_t j = 0; j < PT_ENTRIES; j++) {
            if (!(pd[j] & PAGE_PRESENT)) continue;
            pte_t* pt = (pte_t*)(uintptr_t)(uint32_t)(pd[j] & PTE_ADDR_MASK);

            if (boot_pds[pi][j] & PAGE_PRESENT) {
                // A CLONED kernel page table: free only PTEs the user
                // MODIFIED (differ from the boot table). Identity-mapped
                // frames must stay.
                pte_t* boot_pt =
                    (pte_t*)(uintptr_t)(uint32_t)(boot_pds[pi][j] & PTE_ADDR_MASK);
                for (uint32_t e = 0; e < PT_ENTRIES; e++) {
                    if ((pt[e] & PAGE_PRESENT) &&
                        (pt[e] & 0x000FFFFFFFFFF007ULL) != (boot_pt[e] & 0x000FFFFFFFFFF007ULL)) {
                        uint32_t page_paddr = (uint32_t)(pt[e] & PTE_ADDR_MASK);
                        if (page_paddr >= (KERNEL_RESERVED_PAGES * 4096)) {
                            frame_free(page_paddr);
                        }
                    }
                }
            } else {
                // Entirely user-created: free ALL present page frames.
                for (uint32_t e = 0; e < PT_ENTRIES; e++) {
                    if (pt[e] & PAGE_PRESENT) {
                        uint32_t page_paddr = (uint32_t)(pt[e] & PTE_ADDR_MASK);
                        if (page_paddr >= (KERNEL_RESERVED_PAGES * 4096)) {
                            frame_free(page_paddr);
                        }
                    }
                }
            }
            frame_free((uint32_t)(pd[j] & PTE_ADDR_MASK));   // the PT frame
        }
        frame_free((uint32_t)(pdpt[pi] & PTE_ADDR_MASK));    // the PD frame
    }

    frame_free(page_dir);   // the PDPT frame
    write_serial_string("[VMM] Freed address space\n");
}

int vmm_map_page(uint32_t page_dir, uint32_t vaddr, uint32_t paddr, uint64_t flags) {
    if (page_dir == 0) {
        // Use global mapping via page_map
        page_map(vaddr, paddr, (uint32_t)(flags & 0xFFF));
        return 0;
    }

    pte_t* pt = space_pt(page_dir, vaddr, 1, (flags & PAGE_USER) != 0);
    if (!pt) return -1;
    // Keep every flag INCLUDING the high ones (PAGE_NX lives at bit 63);
    // mask out only the address field and the PRESENT bit we re-add.
    pt[pt_index(vaddr)] = ((uint64_t)paddr & PTE_ADDR_MASK)
                          | (flags & ~PTE_ADDR_MASK & ~PAGE_PRESENT) | PAGE_PRESENT;
    return 0;
}

// Map a fresh Ring 3 stack into page_dir, ending at stack_top. Returns the
// initial ESP (stack_top), or 0.
//
// stack_top is per-task (thread_create picks a unique top below USER_STACK_TOP
// for every task slot), so sibling threads in one process never stomp each
// other's stacks like the old fixed USER_STACK_BOTTOM..USER_STACK_TOP mapping
// did. The page below the mapped range stays unmapped as a guard page.
//
// On partial failure the frames already mapped are left in place; every caller
// abandons the whole address space on failure, and vmm_free_address_space()
// reclaims them.
uint32_t vmm_setup_user_stack(uint32_t page_dir, uint32_t stack_top) {
    if (page_dir == 0) return 0;  // a Ring 3 task needs a private address space

    // Demand-paged: no frames are mapped eagerly. The #PF handler lazily
    // backs each page on first access (write faults get a private zeroed
    // frame, read faults the shared zero page), so a task that uses a few
    // hundred bytes of stack costs a few hundred bytes, not 16 frames. The
    // page below the task's stack range stays permanently unmapped as a guard
    // page — an overflow faults there and kills the task instead of silently
    // corrupting whatever sits underneath.
    return stack_top;
}

int vmm_unmap_page(uint32_t page_dir, uint32_t vaddr) {
    if (page_dir == 0) {
        // Use the CURRENT CR3 (a PDPT) to find the page table.
        uint32_t cr3_val;
        __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3_val));
        pte_t* cur_pdpt = (pte_t*)(uintptr_t)cr3_val;
        pte_t* pd = space_pd(cur_pdpt, vaddr, 0);
        if (pd && (pd[pd_index(vaddr)] & PAGE_PRESENT) &&
            pdpt_index(vaddr) == 0 && pd_index(vaddr) < mem_identity_tables()) {
            pte_t* pt = (pte_t*)(uintptr_t)(uint32_t)(pd[pd_index(vaddr)] & PTE_ADDR_MASK);
            pt[pt_index(vaddr)] = 0;
        }
        __asm__ __volatile__("invlpg (%0)" : : "r"(vaddr));
        return 0;
    }

    pte_t* pt = space_pt(page_dir, vaddr, 0, 0);
    if (!pt || !(pt[pt_index(vaddr)] & PAGE_PRESENT)) return -1;
    pt[pt_index(vaddr)] = 0;

    // TLB flush
    __asm__ __volatile__("invlpg (%0)" : : "r"(vaddr));
    return 0;
}

uint32_t vmm_alloc_page_at(uint32_t page_dir, uint32_t vaddr, uint64_t flags) {
    uint32_t paddr = frame_alloc();
    if (paddr == 0) {
        write_serial_string("[VMM] alloc_page_at: NO FRAME for vaddr=");
        write_serial_hex(vaddr);
        write_serial('\n');
        return 0;
    }

    if (vmm_map_page(page_dir, vaddr, paddr, flags) != 0) {
        frame_free(paddr);
        write_serial_string("[VMM] alloc_page_at: MAP FAILED\n");
        return 0;
    }
    return vaddr;
}

uint32_t vmm_clone_address_space(uint32_t src_page_dir) {
    if (src_page_dir == 0) {
        return vmm_create_address_space();
    }

    // Fresh space with kernel page tables already cloned by create().
    uint32_t dst_pdpt_paddr = vmm_create_address_space();
    if (dst_pdpt_paddr == 0) return 0;

    pte_t* src_pdpt = (pte_t*)(uintptr_t)src_page_dir;
    pte_t* dst_pdpt = (pte_t*)(uintptr_t)dst_pdpt_paddr;

    for (uint32_t pi = 0; pi < PDPT_ENTRIES; pi++) {
        if (!(src_pdpt[pi] & PAGE_PRESENT)) continue;
        pte_t* src_pd = (pte_t*)(uintptr_t)(uint32_t)(src_pdpt[pi] & PTE_ADDR_MASK);
        pte_t* dst_pd = (pte_t*)(uintptr_t)(uint32_t)(dst_pdpt[pi] & PTE_ADDR_MASK);

        for (uint32_t j = 0; j < PT_ENTRIES; j++) {
            if (!(src_pd[j] & PAGE_PRESENT)) continue;

            if (boot_pds[pi][j] & PAGE_PRESENT) {
                // Kernel-region page table: dst already has its own clone
                // with the kernel PTEs identical. Only the USER ptes inside
                // need COW copying. (The old code re-allocated a full PT
                // here and overwrote dst's clone — leaking a frame per
                // kernel PT per fork.)
                pte_t* src_pt = (pte_t*)(uintptr_t)(uint32_t)(src_pd[j] & PTE_ADDR_MASK);
                pte_t* dst_pt = (pte_t*)(uintptr_t)(uint32_t)(dst_pd[j] & PTE_ADDR_MASK);
                for (uint32_t e = 0; e < PT_ENTRIES; e++) {
                    pte_t spe = src_pt[e];
                    if (!(spe & PAGE_PRESENT) || !(spe & PAGE_USER)) continue;
                    if ((spe & PAGE_RW) && !(spe & PAGE_SHARED)) {
                        src_pt[e] = (spe & ~PAGE_RW) | PAGE_COW;   // COW the source
                        spe = src_pt[e];
                    }
                    dst_pt[e] = spe;
                    uint32_t page_paddr = (uint32_t)(spe & PTE_ADDR_MASK);
                    if (page_paddr >= (KERNEL_RESERVED_PAGES * 4096)) {
                        if (frame_ref_count[page_paddr / 4096] < 255)
                            frame_ref_count[page_paddr / 4096]++;
                        // At 255 the frame is pinned — never freed. Acceptable.
                    }
                }
                // Propagate any USER widening the source had on the parents.
                dst_pdpt[pi] |= (src_pdpt[pi] & PAGE_USER);
                dst_pd[j] |= (src_pd[j] & PAGE_USER);
            } else {
                // Purely user region: allocate a fresh PT for dst and COW
                // every present entry.
                uint32_t dst_pt_paddr = frame_alloc();
                if (dst_pt_paddr == 0) {
                    vmm_free_address_space(dst_pdpt_paddr);
                    return 0;
                }
                zero_phys_page(dst_pt_paddr);

                pte_t* src_pt = (pte_t*)(uintptr_t)(uint32_t)(src_pd[j] & PTE_ADDR_MASK);
                pte_t* dst_pt = (pte_t*)(uintptr_t)dst_pt_paddr;

                for (uint32_t e = 0; e < PT_ENTRIES; e++) {
                    pte_t spe = src_pt[e];
                    if (!(spe & PAGE_PRESENT)) continue;
                    if ((spe & PAGE_USER) && (spe & PAGE_RW) && !(spe & PAGE_SHARED)) {
                        src_pt[e] = (spe & ~PAGE_RW) | PAGE_COW;   // COW the source
                        spe = src_pt[e];
                    }
                    dst_pt[e] = spe;
                    uint32_t page_paddr = (uint32_t)(spe & PTE_ADDR_MASK);
                    if (page_paddr >= (KERNEL_RESERVED_PAGES * 4096)) {
                        if (frame_ref_count[page_paddr / 4096] < 255)
                            frame_ref_count[page_paddr / 4096]++;
                        // At 255 the frame is pinned — never freed. Acceptable.
                    }
                }
                dst_pd[j] = (uint64_t)dst_pt_paddr | (src_pd[j] & 0xFFF);
            }
        }
    }

    // Flush TLB of the current active directory just in case it was src_page_dir
    uint32_t active_cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(active_cr3));
    if (active_cr3 == src_page_dir) {
        __asm__ __volatile__("mov %0, %%cr3" : : "r"(src_page_dir));
    }

    write_serial_string("[VMM] Cloned COW address space successfully\n");
    return dst_pdpt_paddr;
}

void vmm_switch_page_dir(uint32_t page_dir) {
    if (page_dir != 0) {
        __asm__ __volatile__("mov %0, %%cr3" : : "r"(page_dir));
    }
}