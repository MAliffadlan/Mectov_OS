#include "../include/mem.h"
#include "../include/vga.h"
#include "../include/utils.h"

static uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t page_tables[4][1024] __attribute__((aligned(4096)));

static uint8_t mem_bitmap[MEMORY_BITMAP_SIZE];
static uint32_t total_pages = 0;
static uint32_t used_pages = 0;

#define HEAP_START 0x1000000 
#define HEAP_SIZE  (10 * 1024 * 1024) 
static uint8_t* heap_ptr = (uint8_t*)HEAP_START;
static uint32_t heap_used = 0;

void init_mem(uint32_t mem_size) {
    total_pages = mem_size / PAGE_SIZE;
    used_pages = 0;
    memset(mem_bitmap, 0, sizeof(mem_bitmap));
    uint32_t reserved_pages = 0x1000000 / PAGE_SIZE;
    for (uint32_t i = 0; i < reserved_pages; i++) {
        mem_bitmap[i / 8] |= (1 << (i % 8));
        used_pages++;
    }
}

void paging_init() {
    memset(page_directory, 0, sizeof(page_directory));
    memset(page_tables, 0, sizeof(page_tables));

    // Identity map 16MB pertama
    for(int t = 0; t < 4; t++) {
        for(unsigned int j = 0; j < 1024; j++) {
            uint32_t phys_addr = (t * 1024 + j) * 4096;
            page_tables[t][j] = phys_addr | 3; 
        }
        page_directory[t] = ((uint32_t)page_tables[t]) | 3;
    }

    __asm__ __volatile__("mov %0, %%cr3": : "r"(page_directory));
    uint32_t cr0;
    __asm__ __volatile__("mov %%cr0, %0": "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ __volatile__("mov %0, %%cr0": : "r"(cr0));
}

void* kmalloc(uint32_t size) {
    if (heap_used + size > HEAP_SIZE) return NULL;
    void* ptr = (void*)(heap_ptr + heap_used);
    heap_used += size;
    return ptr;
}
void kfree(void* ptr) { (void)ptr; }
unsigned int get_total_memory() { return total_pages * PAGE_SIZE; }
unsigned int get_used_memory() { return (0x1000000 + heap_used); }
unsigned int get_free_memory() { return get_total_memory() - get_used_memory(); }
