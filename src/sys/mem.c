#include "../include/mem.h"
#include "../include/vga.h"
#include "../include/utils.h"

static uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t page_tables[32][1024] __attribute__((aligned(4096)));  // 32 × 4MB = 128MB identity map
static uint32_t fb_page_tables[2][1024] __attribute__((aligned(4096))); 

static uint8_t *mem_bitmap = NULL;
static uint32_t total_pages = 0;
static uint32_t used_pages = 0;

void init_mem(uint32_t mem_size) {
    total_pages = mem_size / PAGE_SIZE;
    mem_bitmap = (uint8_t*)kmalloc((total_pages / 8) + 1);
    used_pages = 0;
    if (mem_bitmap) {
        memset(mem_bitmap, 0, (total_pages / 8) + 1);
    }
    uint32_t reserved_pages = 0x1000000 / PAGE_SIZE; // 16MB reserved for kernel/stack
    if (reserved_pages > total_pages) reserved_pages = total_pages;
    for (uint32_t i = 0; i < reserved_pages; i++) {
        if (mem_bitmap) mem_bitmap[i / 8] |= (1 << (i % 8));
        used_pages++;
    }
}

void paging_init(uint32_t fb_paddr, uint32_t fb_size) {
    (void)fb_size;
    memset(page_directory, 0, sizeof(page_directory));
    
    // Map available physical memory (up to 128MB)
    uint32_t num_tables = total_pages / 1024;
    if (num_tables == 0) num_tables = 8;
    if (num_tables > 32) num_tables = 32;

    for(uint32_t t = 0; t < num_tables; t++) {
        for(unsigned int j = 0; j < 1024; j++) {
            page_tables[t][j] = ((t * 1024 + j) * 4096) | 7; // Present, R/W, User
        }
        page_directory[t] = ((uint32_t)page_tables[t]) | 7;
    }

    // Map Framebuffer separately if it's above our identity mapped RAM
    if (fb_paddr >= (num_tables * 0x400000)) {
        uint32_t dir_start = fb_paddr >> 22;
        uint32_t base_phys = fb_paddr & 0xFFC00000;
        for(int t = 0; t < 2; t++) {
            for(int i = 0; i < 1024; i++) {
                fb_page_tables[t][i] = (base_phys + (t * 1024 + i) * 4096) | 7;
            }
            page_directory[dir_start + t] = ((uint32_t)fb_page_tables[t]) | 7;
        }
    }

    __asm__ __volatile__("mov %0, %%cr3": : "r"(page_directory));
    uint32_t cr0;
    __asm__ __volatile__("mov %%cr0, %0": "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ __volatile__("mov %0, %%cr0": : "r"(cr0));
}

// Map a virtual address to a physical address explicitly
void page_map(uint32_t vaddr, uint32_t paddr, uint32_t flags) {
    uint32_t pd_idx = vaddr >> 22;
    uint32_t pt_idx = (vaddr >> 12) & 0x3FF;
    
    // We reuse the static page tables for the first 128MB.
    // If we map outside, we need dynamically allocated tables, but for now we just 
    // update the static table entries.
    if (pd_idx < 32) {
        page_tables[pd_idx][pt_idx] = (paddr & 0xFFFFF000) | flags;
        page_directory[pd_idx] = ((uint32_t)page_tables[pd_idx]) | 7;
    }
}


typedef struct block_meta {
    uint32_t size;
    int free;
    struct block_meta *next;
} block_meta;

#define META_SIZE sizeof(block_meta)

static void *global_base = NULL;
static uint32_t heap_used = 0;

static block_meta *find_free_block(block_meta **last, uint32_t size) {
    block_meta *current = global_base;
    while (current && !(current->free && current->size >= size)) {
        *last = current;
        current = current->next;
    }
    return current;
}

static block_meta *request_space(block_meta* last, uint32_t size) {
    uint32_t total_ram = total_pages * 4096;
    uint32_t max_heap = (total_ram > 0x1000000) ? (total_ram - 0x1000000) : (10 * 1024 * 1024);
    
    if (heap_used + size + META_SIZE > max_heap) return NULL;
    
    block_meta *block = (block_meta*)((uint8_t*)0x1000000 + heap_used);
    heap_used += size + META_SIZE;
    
    if (last) last->next = block;
    block->size = size;
    block->next = NULL;
    block->free = 0;
    return block;
}

void* kmalloc(uint32_t size) {
    if (size == 0) return NULL;
    if (size % 4 != 0) size += 4 - (size % 4); // Align 4 bytes

    if (!global_base) { // First call
        block_meta *block = request_space(NULL, size);
        if (!block) return NULL;
        global_base = block;
        return (void*)(block + 1);
    }

    block_meta *last = global_base;
    block_meta *block = find_free_block(&last, size);
    if (!block) {
        block = request_space(last, size);
        if (!block) return NULL;
    } else {
        block->free = 0;
    }
    return (void*)(block + 1);
}

void kfree(void* p) {
    if (!p) return;
    block_meta *block = (block_meta*)p - 1;
    block->free = 1;

    // Forward coalescing: merge with next block if it's also free
    while (block->next && block->next->free) {
        block->size += META_SIZE + block->next->size;
        block->next = block->next->next;
    }

    // Backward coalescing: find previous block and merge if free
    if (global_base != block) {
        block_meta *prev = global_base;
        while (prev && prev->next != block) prev = prev->next;
        if (prev && prev->free) {
            prev->size += META_SIZE + block->size;
            prev->next = block->next;
        }
    }
}

unsigned int get_total_memory() { return total_pages * 4096; }
unsigned int get_used_memory() { return (16 * 1024 * 1024) + heap_used; }
unsigned int get_free_memory() { return get_total_memory() - get_used_memory(); }
