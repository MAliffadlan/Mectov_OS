#ifndef MEM_H
#define MEM_H

#include "types.h"

#define PAGE_SIZE 4096
#define KERNEL_RESERVED_PAGES (48 * 256)  // 48MB reserved for kernel+modules

// ---- PAE paging (v38.49) ----
// Three-level paging with 64-bit entries: PDPT (4 × 512MB) -> PD (512 × 2MB)
// -> PT (512 × 4KB). The kernel keeps its identity map (phys == virt for all
// frames below the ceiling), so page-table structures remain directly
// addressable; only the entry type, the index math and the frame mask change.
typedef uint64_t pte_t;
#define PT_ENTRIES      512
#define PDPT_ENTRIES    4
// Physical frame address inside any entry (frames live below 4GB here, but
// keep the full PAE-width mask so high MMIO e.g. the ~4GB framebuffer works).
#define PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL
#define PAGE_NX         (1ULL << 63)  // no-execute; only valid with EFER.NXE

static inline uint32_t pdpt_index(uint32_t v) { return v >> 30; }
static inline uint32_t pd_index(uint32_t v)   { return (v >> 21) & 0x1FF; }
static inline uint32_t pt_index(uint32_t v)   { return (v >> 12) & 0x1FF; }

// Paging Flags (low bits are identical in non-PAE and PAE entries; the
// software bits PAGE_COW/PAGE_SHARED sit in available bits 9/10).
#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4
#define PAGE_COW     0x200
#define PAGE_SHARED  0x400   // shared memory page (never COW'd on fork)

void init_mem(uint32_t mem_size);
void paging_init(uint32_t fb_addr, uint32_t fb_size);
// Enable EFER.NXE on the CURRENT cpu (BSP in paging_init, every AP in
// ap_main — MSRs are per-core). 1 when NX is active (CPUID-gated); page
// fillers consult this before setting PAGE_NX, because without NXE bit 63
// is reserved and its presence faults.
int  paging_nx_enabled(void);
void paging_enable_nxe(void);
// Boot (kernel) paging structures — identity-mapped globals, used by
// vmm.c to clone kernel page tables into fresh address spaces.
extern pte_t boot_pdpt[PDPT_ENTRIES];
extern pte_t boot_pds[PDPT_ENTRIES][PT_ENTRIES];
// How many 2MB identity-map page tables paging_init mapped (vmm.c bounds).
uint32_t mem_identity_tables(void);
 // AKTIFKAN PAGING
void* kmalloc(uint32_t size);
void kfree(void* ptr);
void* krealloc(void* ptr, uint32_t new_size);
void* kcalloc(uint32_t num, uint32_t size);
void kmalloc_stats(void (*print_fn)(const char*, unsigned char));

// Allocator snapshot for /proc/meminfo (kernel heap hardening v38.4).
typedef struct {
    uint32_t heap_base;
    uint32_t heap_used;       // committed arena bytes (payload + headers + canaries)
    uint32_t allocated;       // live payload bytes
    uint32_t free_bytes;      // free payload bytes
    uint32_t blocks;          // live block count
    uint32_t free_blocks;     // free block count
    uint32_t largest_free;    // largest contiguous free payload
    uint32_t allocs;          // total kmalloc calls that succeeded
    uint32_t frees;           // total kfree calls on live blocks
    uint32_t oom_count;       // failed allocations (caller got NULL)
    uint32_t canary_failures; // buffer overflows caught at free time
    uint32_t magic_failures;  // double frees / bad pointers caught at free time
} kmalloc_stats_t;
void kmalloc_get_stats(kmalloc_stats_t* s);
void page_map(uint32_t vaddr, uint32_t paddr, uint32_t flags);

unsigned int get_total_memory();
unsigned int get_used_memory();
unsigned int get_free_memory();

#endif