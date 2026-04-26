#include "../include/mem.h"
#include "../include/vga.h"
#include "../include/utils.h"

static uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t page_tables[8][1024] __attribute__((aligned(4096)));  // 8 × 4MB = 32MB identity map
static uint32_t fb_page_tables[2][1024] __attribute__((aligned(4096))); 

static uint8_t mem_bitmap[MEMORY_BITMAP_SIZE];
static uint32_t total_pages = 0;
static uint32_t used_pages = 0;

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

void paging_init(uint32_t fb_paddr, uint32_t fb_size) {
    (void)fb_size;
    memset(page_directory, 0, sizeof(page_directory));
    
    for(int t = 0; t < 8; t++) {
        for(unsigned int j = 0; j < 1024; j++) {
            page_tables[t][j] = ((t * 1024 + j) * 4096) | 3; 
        }
        page_directory[t] = ((uint32_t)page_tables[t]) | 3;
    }

    if (fb_paddr >= 0x1000000) {
        uint32_t dir_start = fb_paddr >> 22;
        uint32_t base_phys = fb_paddr & 0xFFC00000;
        for(int t = 0; t < 2; t++) {
            for(int i = 0; i < 1024; i++) {
                fb_page_tables[t][i] = (base_phys + (t * 1024 + i) * 4096) | 3;
            }
            page_directory[dir_start + t] = ((uint32_t)fb_page_tables[t]) | 3;
        }
    }

    __asm__ __volatile__("mov %0, %%cr3": : "r"(page_directory));
    uint32_t cr0;
    __asm__ __volatile__("mov %%cr0, %0": "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ __volatile__("mov %0, %%cr0": : "r"(cr0));
}

void* kmalloc(uint32_t size) {
    static uint8_t* heap = (uint8_t*)0x1000000;
    static uint32_t used = 0;
    if (used + size > 10 * 1024 * 1024) return NULL;
    void* p = (void*)(heap + used);
    used += size;
    return p;
}

void kfree(void* p) { (void)p; }

unsigned int get_total_memory() { return total_pages * 4096; }
unsigned int get_used_memory() { return (16 * 1024 * 1024) + used_pages * 4096; }
unsigned int get_free_memory() { return get_total_memory() - get_used_memory(); }
