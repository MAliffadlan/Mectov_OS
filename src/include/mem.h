#ifndef MEM_H
#define MEM_H

#include "types.h"

#define PAGE_SIZE 4096
#define MEMORY_BITMAP_SIZE (1024 * 1024 / 8) // Support up to 4GB RAM

void init_mem(unsigned int mem_size);
void* kmalloc(unsigned int size);
void kfree(void* ptr);

// Statistik memori
unsigned int get_total_memory();
unsigned int get_used_memory();
unsigned int get_free_memory();

#endif