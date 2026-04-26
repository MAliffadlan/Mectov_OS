#ifndef MEM_H
#define MEM_H

#include "types.h"

#define PAGE_SIZE 4096
#define MEMORY_BITMAP_SIZE (1024 * 1024 / 8)

// Paging Flags
#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4

void init_mem(uint32_t mem_size);
void paging_init(); // AKTIFKAN PAGING
void* kmalloc(uint32_t size);
void kfree(void* ptr);

unsigned int get_total_memory();
unsigned int get_used_memory();
unsigned int get_free_memory();

#endif
