#ifndef MEM_H
#define MEM_H

#include "types.h"

#define PAGE_SIZE 4096
#define KERNEL_RESERVED_PAGES (48 * 256)  // 48MB reserved for kernel+modules

// Paging Flags
#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4
#define PAGE_COW     0x200
#define PAGE_SHARED  0x400   // shared memory page (never COW'd on fork)

void init_mem(uint32_t mem_size);
void paging_init(uint32_t fb_addr, uint32_t fb_size);
// How many 4MB identity-map tables paging_init mapped (for vmm.c bounds).
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