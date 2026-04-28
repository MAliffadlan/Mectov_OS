#ifndef MEM_H
#define MEM_H

#include "types.h"

#define PAGE_SIZE 4096

// Paging Flags
#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4

void init_mem(uint32_t mem_size);
void paging_init(uint32_t fb_addr, uint32_t fb_size);
 // AKTIFKAN PAGING
void* kmalloc(uint32_t size);
void kfree(void* ptr);
void page_map(uint32_t vaddr, uint32_t paddr, uint32_t flags);

unsigned int get_total_memory();
unsigned int get_used_memory();
unsigned int get_free_memory();

#endif
