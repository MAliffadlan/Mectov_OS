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

#define TABLE_PER_DIR 1024

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

uint32_t vmm_create_address_space(void) {
    if (!vmm_initialized) vmm_init();
    
    // Allocate physical frame for page directory
    uint32_t pd_paddr = frame_alloc();
    if (pd_paddr == 0) return 0;
    zero_phys_page(pd_paddr);
    
    uint32_t* pd = (uint32_t*)(uintptr_t)pd_paddr;
    
    // IMPORTANT: Always copy from the KERNEL's boot page directory (task 0),
    // NOT from whatever CR3 is currently active!
    extern uint32_t tasks_get_boot_cr3(void);
    uint32_t kernel_cr3 = tasks_get_boot_cr3();
    uint32_t* kernel_pd = (uint32_t*)(uintptr_t)(kernel_cr3 & 0xFFFFF000);
    
    // CLONE each kernel page table into a private copy.
    // We MUST NOT share page table pointers with the kernel, because when
    // user-space code maps pages (e.g., libc at 0x03000000 = pd_idx 12),
    // vmm_map_page would modify the KERNEL's page table, corrupting the
    // identity map for ALL tasks. By cloning, each address space gets its
    // own writable copy of the page tables.
    for (uint32_t i = 0; i < 1024; i++) {
        if (kernel_pd[i] & PAGE_PRESENT) {
            uint32_t kernel_pt_paddr = kernel_pd[i] & 0xFFFFF000;
            uint32_t kernel_pt_flags = kernel_pd[i] & 0xFFF;
            
            // Allocate a new physical frame for this page table
            uint32_t new_pt_paddr = frame_alloc();
            if (new_pt_paddr == 0) {
                write_serial_string("[VMM] CLONE PT FAIL - out of frames!\n");
                // Free already-allocated page tables and PD
                for (uint32_t j = 0; j < i; j++) {
                    if (pd[j] & PAGE_PRESENT) {
                        uint32_t cloned_pt = pd[j] & 0xFFFFF000;
                        // Only free if it's not the kernel's original PT
                        if (cloned_pt != (kernel_pd[j] & 0xFFFFF000)) {
                            frame_free(cloned_pt);
                        }
                    }
                }
                frame_free(pd_paddr);
                return 0;
            }
            
            // Copy the entire page table (4096 bytes = 1024 entries × 4 bytes)
            uint32_t* src_pt = (uint32_t*)(uintptr_t)kernel_pt_paddr;
            uint32_t* dst_pt = (uint32_t*)(uintptr_t)new_pt_paddr;
            for (uint32_t e = 0; e < 1024; e++) {
                dst_pt[e] = src_pt[e];
            }
            
            // Point the new PD entry to the CLONED page table
            pd[i] = new_pt_paddr | kernel_pt_flags;
        }
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
            vmm_map_page(pd_paddr, SIG_TRAMPOLINE_VA, tp, PAGE_PRESENT | PAGE_USER);
        }
    }

    write_serial_string("[VMM] Created address space (cloned PTs) OK\n");
    return pd_paddr;
}

void vmm_free_address_space(uint32_t page_dir) {
    if (page_dir == 0) return;  // Can't free global identity
    
    uint32_t* pd = (uint32_t*)(uintptr_t)page_dir;
    
    // Get kernel page directory to distinguish user-modified entries
    extern uint32_t tasks_get_boot_cr3(void);
    uint32_t* kernel_pd = (uint32_t*)(uintptr_t)(tasks_get_boot_cr3() & 0xFFFFF000);
    
    for (int i = 0; i < TABLE_PER_DIR; i++) {
        if (!(pd[i] & PAGE_PRESENT)) continue;
        
        uint32_t pt_paddr = pd[i] & 0xFFFFF000;
        uint32_t* pt = (uint32_t*)(uintptr_t)pt_paddr;
        
        if ((kernel_pd[i] & PAGE_PRESENT)) {
            // This PD entry corresponds to a CLONED kernel page table.
            // We need to find PTEs that the user MODIFIED (differ from kernel)
            // and free only those frames. Identity-mapped frames must stay.
            uint32_t kernel_pt_paddr = kernel_pd[i] & 0xFFFFF000;
            uint32_t* kernel_pt = (uint32_t*)(uintptr_t)kernel_pt_paddr;
            
            for (int j = 0; j < 1024; j++) {
                if ((pt[j] & PAGE_PRESENT) && (pt[j] & 0xFFFFF007) != (kernel_pt[j] & 0xFFFFF007)) {
                    // User-modified PTE — free the user's physical frame
                    uint32_t page_paddr = pt[j] & 0xFFFFF000;
                    if (page_paddr >= (KERNEL_RESERVED_PAGES * 4096)) {
                        frame_free(page_paddr);
                    }
                }
            }
        } else {
            // This PD entry is entirely user-created (no kernel counterpart).
            // Free ALL present page frames.
            for (int j = 0; j < 1024; j++) {
                if (pt[j] & PAGE_PRESENT) {
                    uint32_t page_paddr = pt[j] & 0xFFFFF000;
                    if (page_paddr >= (KERNEL_RESERVED_PAGES * 4096)) {
                        frame_free(page_paddr);
                    }
                }
            }
        }
        
        // Free the cloned/user page table frame itself
        frame_free(pt_paddr);
    }
    
    // Free page directory itself
    frame_free(page_dir);
    write_serial_string("[VMM] Freed address space\n");
}

int vmm_map_page(uint32_t page_dir, uint32_t vaddr, uint32_t paddr, uint32_t flags) {
    if (page_dir == 0) {
        // Use global mapping via page_map
        page_map(vaddr, paddr, flags);
        return 0;
    }
    
    uint32_t pd_idx = vaddr >> 22;
    uint32_t pt_idx = (vaddr >> 12) & 0x3FF;
    
    uint32_t* pd = (uint32_t*)(uintptr_t)page_dir;
    
    // Check if page table exists
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        // Allocate a new page table
        uint32_t pt_paddr = frame_alloc();
        if (pt_paddr == 0) return -1;
        zero_phys_page(pt_paddr);
        pd[pd_idx] = pt_paddr | PAGE_PRESENT | PAGE_RW | PAGE_USER;
        write_serial_string("[VMM] new PT at ");
        write_serial_hex(pt_paddr);
        write_serial_string(" for pd_idx=");
        write_serial_hex(pd_idx);
        write_serial('\n');
    }
    
    // The CPU requires PAGE_USER at EVERY level, so a user page under a
    // kernel-only PDE is unreachable from Ring 3. This matters because
    // vmm_create_address_space() clones the kernel's identity-map page tables
    // (which are kernel-only since paging_init stopped setting PAGE_USER), and
    // on a machine with enough RAM the user window at 0x08000000 falls inside
    // one of them. Widening the PDE is safe: this page table is always this
    // address space's private clone, and the identity-mapped PTEs inside it
    // still lack PAGE_USER individually, so kernel memory stays protected.
    if (flags & PAGE_USER) pd[pd_idx] |= PAGE_USER;

    uint32_t pt_paddr = pd[pd_idx] & 0xFFFFF000;
    uint32_t* pt = (uint32_t*)(uintptr_t)pt_paddr;
    pt[pt_idx] = (paddr & 0xFFFFF000) | (flags & 0xFFF) | PAGE_PRESENT;

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
    uint32_t pd_idx = vaddr >> 22;
    uint32_t pt_idx = (vaddr >> 12) & 0x3FF;
    
    if (page_dir == 0) {
        // Use current CR3 page directory to find the page table
        uint32_t cr3_val;
        __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3_val));
        uint32_t* cur_pd = (uint32_t*)(uintptr_t)(cr3_val & 0xFFFFF000);
        if ((cur_pd[pd_idx] & PAGE_PRESENT) && pd_idx < mem_identity_tables()) {
            uint32_t pt_paddr = cur_pd[pd_idx] & 0xFFFFF000;
            uint32_t* pt = (uint32_t*)(uintptr_t)pt_paddr;
            pt[pt_idx] = 0;
        }
        __asm__ __volatile__("invlpg (%0)" : : "r"(vaddr));
        return 0;
    }
    
    uint32_t* pd = (uint32_t*)(uintptr_t)page_dir;
    if (!(pd[pd_idx] & PAGE_PRESENT)) return -1;
    
    uint32_t pt_paddr = pd[pd_idx] & 0xFFFFF000;
    uint32_t* pt = (uint32_t*)(uintptr_t)pt_paddr;
    pt[pt_idx] = 0;
    
    // TLB flush
    __asm__ __volatile__("invlpg (%0)" : : "r"(vaddr));
    return 0;
}

uint32_t vmm_alloc_page_at(uint32_t page_dir, uint32_t vaddr, uint32_t flags) {
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
    
    // Create new independent address space with kernel mappings already cloned
    uint32_t dst_pd_paddr = vmm_create_address_space();
    if (dst_pd_paddr == 0) return 0;
    
    uint32_t* src_pd = (uint32_t*)(uintptr_t)src_page_dir;
    uint32_t* dst_pd = (uint32_t*)(uintptr_t)dst_pd_paddr;
    
    extern uint32_t tasks_get_boot_cr3(void);
    uint32_t kernel_cr3 = tasks_get_boot_cr3();
    uint32_t* kernel_pd = (uint32_t*)(uintptr_t)(kernel_cr3 & 0xFFFFF000);
    
    // Scan all user-space page directory entries (index >= 8, covering 32MB+)
    for (uint32_t i = 8; i < 1024; i++) {
        if (!(src_pd[i] & PAGE_PRESENT)) continue;
        
        // If it's a kernel page table that has not been modified, it was already handled
        if (i < 32 && (kernel_pd[i] & PAGE_PRESENT) && (src_pd[i] & 0xFFFFF000) == (kernel_pd[i] & 0xFFFFF000)) {
            continue;
        }
        
        // Allocate a new physical frame for this cloned page table
        uint32_t dst_pt_paddr = frame_alloc();
        if (dst_pt_paddr == 0) {
            vmm_free_address_space(dst_pd_paddr);
            return 0;
        }
        zero_phys_page(dst_pt_paddr);
        
        uint32_t* src_pt = (uint32_t*)(uintptr_t)(src_pd[i] & 0xFFFFF000);
        uint32_t* dst_pt = (uint32_t*)(uintptr_t)dst_pt_paddr;
        
        for (uint32_t e = 0; e < 1024; e++) {
            if (!(src_pt[e] & PAGE_PRESENT)) continue;
            
            // Mark user-space writable pages as COW and Read-Only — UNLESS the
            // page is shared memory (PAGE_SHARED): shm pages must stay shared
            // across fork, so they are copied as-is (RW, same physical frame)
            // and the refcount bump below keeps them alive for both tasks.
            if ((src_pt[e] & PAGE_USER) && (src_pt[e] & PAGE_RW) &&
                !(src_pt[e] & PAGE_SHARED)) {
                src_pt[e] &= ~PAGE_RW;  // clear RW
                src_pt[e] |= PAGE_COW;  // set COW
            }
            
            // Copy the page table entry
            dst_pt[e] = src_pt[e];
            
            // Increment the reference count of the physical frame
            uint32_t page_paddr = src_pt[e] & 0xFFFFF000;
            if (page_paddr >= (KERNEL_RESERVED_PAGES * 4096)) {
                if (frame_ref_count[page_paddr / 4096] < 255)
                    frame_ref_count[page_paddr / 4096]++;
                // At 255 the frame is pinned — never freed. Acceptable.
            }
        }
        
        // Link to new directory
        dst_pd[i] = dst_pt_paddr | (src_pd[i] & 0xFFF);
    }
    
    // Flush TLB of the current active directory just in case it was src_page_dir
    uint32_t active_cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(active_cr3));
    if (active_cr3 == src_page_dir) {
        __asm__ __volatile__("mov %0, %%cr3" : : "r"(src_page_dir));
    }
    
    write_serial_string("[VMM] Cloned COW address space successfully\n");
    return dst_pd_paddr;
}

void vmm_switch_page_dir(uint32_t page_dir) {
    if (page_dir != 0) {
        __asm__ __volatile__("mov %0, %%cr3" : : "r"(page_dir));
    }
}