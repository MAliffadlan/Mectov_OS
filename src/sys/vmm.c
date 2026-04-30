#include "../include/vmm.h"
#include "../include/mem.h"
#include "../include/utils.h"
#include "../include/serial.h"

// ============================================================
// Virtual Memory Manager — Layer di atas existing identity paging
// ============================================================
// Existing paging (mem.c) identity-maps 0–128MB.
// VMM adds physical frame tracking + per-process page directories
// WITHOUT touching the existing static page tables.
//
// page_dir = 0 → use global identity map (existing behavior)
// page_dir = any other value → use that page directory
//
// Frame bitmap: tracks which 4KB physical frames are in use
// ============================================================

#define TOTAL_PHYSICAL_MB 128
#define TOTAL_PHYSICAL_PAGES (TOTAL_PHYSICAL_MB * 256)  // 128MB / 4KB = 32768
#define FRAME_BITMAP_SIZE (TOTAL_PHYSICAL_PAGES / 8)

// One static bitmap for the whole system
static uint8_t frame_bitmap[FRAME_BITMAP_SIZE];
static int vmm_initialized = 0;

// Mark kernel region (first 16MB) as reserved
#define KERNEL_RESERVED_PAGES (16 * 256)  // 16MB

static void bitmap_set(int idx) {
    frame_bitmap[idx / 8] |= (1 << (idx % 8));
}
static void bitmap_clear(int idx) {
    frame_bitmap[idx / 8] &= ~(1 << (idx % 8));
}
static int bitmap_test(int idx) {
    return (frame_bitmap[idx / 8] >> (idx % 8)) & 1;
}

void vmm_init(void) {
    if (vmm_initialized) return;
    vmm_initialized = 1;
    
    // Clear bitmap
    memset(frame_bitmap, 0, FRAME_BITMAP_SIZE);
    
    // Reserve kernel region (first 16MB)
    for (int i = 0; i < KERNEL_RESERVED_PAGES; i++) {
        bitmap_set(i);
    }
    
    write_serial_string("[VMM] Initialized. ");
    write_serial_string("Reserved ");
    write_serial_hex(KERNEL_RESERVED_PAGES * 4096);
    write_serial_string(" bytes for kernel.\n");
}

// Allocate one physical frame. Returns physical address or 0.
static uint32_t frame_alloc(void) {
    for (int i = KERNEL_RESERVED_PAGES; i < TOTAL_PHYSICAL_PAGES; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            return (uint32_t)i * 4096;
        }
    }
    write_serial_string("[VMM] OUT OF PHYSICAL FRAMES!\n");
    return 0;
}

static void frame_free(uint32_t paddr) {
    if (paddr == 0) return;
    int idx = paddr / 4096;
    if (idx >= KERNEL_RESERVED_PAGES && idx < TOTAL_PHYSICAL_PAGES) {
        bitmap_clear(idx);
    }
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
    // Temporarily identity-map if not already, then memset
    // Since we use identity mapping 0→128MB, if paddr < 128MB, just use direct pointer
    if (paddr < (TOTAL_PHYSICAL_MB * 1024 * 1024)) {
        memset((void*)(uintptr_t)paddr, 0, 4096);
    } else {
        // Need to map temporarily — for now assert not happening
        write_serial_string("[VMM] zero_phys_page: paddr out of range!\n");
    }
}

// Clone a page directory entry (for kernel region mapping)
static void clone_pde(uint32_t* dst_pd, uint32_t* src_pd, int idx) {
    dst_pd[idx] = src_pd[idx];
}

uint32_t vmm_create_address_space(void) {
    if (!vmm_initialized) vmm_init();
    
    // Allocate physical frame for page directory
    uint32_t pd_paddr = frame_alloc();
    if (pd_paddr == 0) return 0;
    zero_phys_page(pd_paddr);
    
    uint32_t* pd = (uint32_t*)(uintptr_t)pd_paddr;
    
    // Get current page directory from CR3
    uint32_t cr3_val;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3_val));
    uint32_t* cur_pd = (uint32_t*)(uintptr_t)(cr3_val & 0xFFFFF000);
    
    // Identity map kernel region (first 32 page table entries = 32*4MB = 128MB)
    // copy from current page directory
    uint32_t num_tables = 32;
    for (uint32_t i = 0; i < num_tables; i++) {
        if (cur_pd[i] & PAGE_PRESENT) {
            // Allocate a new page table, clone entries
            uint32_t pt_paddr = frame_alloc();
            if (pt_paddr == 0) {
                vmm_free_address_space(pd_paddr);
                return 0;
            }
            zero_phys_page(pt_paddr);
            
            uint32_t* new_pt = (uint32_t*)(uintptr_t)pt_paddr;
            uint32_t* old_pt = (uint32_t*)(uintptr_t)(cur_pd[i] & 0xFFFFF000);
            
            for (int j = 0; j < 1024; j++) {
                new_pt[j] = old_pt[j];
            }
            
            pd[i] = pt_paddr | (cur_pd[i] & 0xFFF);
        }
    }
    
    write_serial_string("[VMM] Created address space at ");
    write_serial_hex(pd_paddr);
    write_serial('\n');
    
    return pd_paddr;
}

void vmm_free_address_space(uint32_t page_dir) {
    if (page_dir == 0) return;  // Can't free global identity
    
    uint32_t* pd = (uint32_t*)(uintptr_t)page_dir;
    
    // Free page tables
    for (int i = 0; i < TABLE_PER_DIR; i++) {
        if (pd[i] & PAGE_PRESENT) {
            uint32_t pt_paddr = pd[i] & 0xFFFFF000;
            // Free each page in the page table
            uint32_t* pt = (uint32_t*)(uintptr_t)pt_paddr;
            for (int j = 0; j < 1024; j++) {
                if (pt[j] & PAGE_PRESENT) {
                    uint32_t page_paddr = pt[j] & 0xFFFFF000;
                    // Don't free kernel region pages
                    if (page_paddr >= (KERNEL_RESERVED_PAGES * 4096)) {
                        frame_free(page_paddr);
                    }
                }
            }
            frame_free(pt_paddr);
        }
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
    }
    
    uint32_t pt_paddr = pd[pd_idx] & 0xFFFFF000;
    uint32_t* pt = (uint32_t*)(uintptr_t)pt_paddr;
    pt[pt_idx] = (paddr & 0xFFFFF000) | (flags & 0xFFF) | PAGE_PRESENT;
    
    return 0;
}

int vmm_unmap_page(uint32_t page_dir, uint32_t vaddr) {
    uint32_t pd_idx = vaddr >> 22;
    uint32_t pt_idx = (vaddr >> 12) & 0x3FF;
    
    if (page_dir == 0) {
        // Use current CR3 page directory to find the page table
        uint32_t cr3_val;
        __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3_val));
        uint32_t* cur_pd = (uint32_t*)(uintptr_t)(cr3_val & 0xFFFFF000);
        if ((cur_pd[pd_idx] & PAGE_PRESENT) && pd_idx < 32) {
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
    if (paddr == 0) return 0;
    
    if (vmm_map_page(page_dir, vaddr, paddr, flags) != 0) {
        frame_free(paddr);
        return 0;
    }
    return vaddr;
}

uint32_t vmm_find_free_region(uint32_t page_dir, uint32_t size) {
    (void)page_dir;
    // Simple: allocate starting from 0x08000000 (128MB)
    // For now, this just returns the next available region
    // In a real implementation, we'd scan page tables
    static uint32_t next_region = 0x08000000;
    uint32_t result = next_region;
    next_region += size;
    // Align to 4KB
    next_region = (next_region + 0xFFF) & 0xFFFFF000;
    return result;
}

uint32_t vmm_clone_address_space(uint32_t src_page_dir) {
    (void)src_page_dir;
    // COW implementation would go here
    // For now, just create new address space
    return vmm_create_address_space();
}

void vmm_switch_page_dir(uint32_t page_dir) {
    if (page_dir != 0) {
        __asm__ __volatile__("mov %0, %%cr3" : : "r"(page_dir));
    }
}