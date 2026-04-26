#include "../include/mem.h"
#include "../include/vga.h"

static uint8_t mem_bitmap[MEMORY_BITMAP_SIZE];
static uint32_t total_pages = 0;
static uint32_t used_pages = 0;

#define HEAP_START 0x100000 
#define HEAP_SIZE  (10 * 1024 * 1024) 
static uint8_t* heap_ptr = (uint8_t*)HEAP_START;
static uint32_t heap_used = 0;

// Struktur Paging (Wajib 4096-byte aligned)
uint32_t page_directory[1024] __attribute__((aligned(4096)));
uint32_t first_page_table[1024] __attribute__((aligned(4096)));

void init_mem(uint32_t mem_size) {
    total_pages = mem_size / PAGE_SIZE;
    used_pages = 0;
    for (uint32_t i = 0; i < MEMORY_BITMAP_SIZE; i++) mem_bitmap[i] = 0;
    uint32_t reserved_pages = 0x100000 / PAGE_SIZE;
    for (uint32_t i = 0; i < reserved_pages; i++) {
        mem_bitmap[i / 8] |= (1 << (i % 8));
        used_pages++;
    }
}

void paging_init() {
    // 1. Isi Page Directory
    // Kita buat identity mapping untuk 4MB pertama (Kernel + VGA)
    for(int i = 0; i < 1024; i++) {
        // Atur sebagai Read/Write tapi tidak Present dulu
        page_directory[i] = 0 | 2; 
    }

    // 2. Isi Page Table pertama (Mengkover 0MB - 4MB)
    for(unsigned int i = 0; i < 1024; i++) {
        // Alamat Fisik = Alamat Virtual (Identity Map)
        // Atur: Present (1) & Read/Write (2)
        first_page_table[i] = (i * 4096) | 3;
    }

    // 3. Pasang Page Table ke Page Directory entri pertama
    page_directory[0] = ((uint32_t)first_page_table) | 3;

    // 4. Kasih tau CPU lokasi Page Directory (Register CR3)
    __asm__ __volatile__("mov %0, %%cr3": : "r"(page_directory));

    // 5. NYALAKAN PAGING (Set bit 31 di Register CR0)
    uint32_t cr0;
    __asm__ __volatile__("mov %%cr0, %0": "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ __volatile__("mov %0, %%cr0": : "r"(cr0));

    print("[+] Paging Engine Enabled (Identity Mapping 4MB OK)\n", 0x0A);
}

void* kmalloc(uint32_t size) {
    if (heap_used + size > HEAP_SIZE) {
        print("Error: Out of Memory!\n", 0x0C);
        return NULL;
    }
    void* ptr = (void*)(heap_ptr + heap_used);
    heap_used += size;
    used_pages = (0x100000 + heap_used) / PAGE_SIZE;
    return ptr;
}

void kfree(void* ptr) { (void)ptr; }
unsigned int get_total_memory() { return total_pages * PAGE_SIZE; }
unsigned int get_used_memory() { return (0x100000 + heap_used); }
unsigned int get_free_memory() { return get_total_memory() - get_used_memory(); }
