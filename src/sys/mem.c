#include "../include/mem.h"
#include "../include/vga.h"

// Bitmap untuk melacak halaman memori fisik (1 bit per 4KB)
static uint8_t mem_bitmap[MEMORY_BITMAP_SIZE];
static uint32_t total_pages = 0;
static uint32_t used_pages = 0;

// Penanda akhir kernel di memori (dari linker script biasanya, kita buat statis dulu)
// Asumsi kernel < 1MB, heap mulai di 1MB
#define HEAP_START 0x100000 
#define HEAP_SIZE  (10 * 1024 * 1024) // 10MB Heap awal

static uint8_t* heap_ptr = (uint8_t*)HEAP_START;
static uint32_t heap_used = 0;

void init_mem(uint32_t mem_size) {
    total_pages = mem_size / PAGE_SIZE;
    used_pages = 0;

    // Reset bitmap
    for (uint32_t i = 0; i < MEMORY_BITMAP_SIZE; i++) {
        mem_bitmap[i] = 0;
    }

    // Tandai 1MB pertama sebagai terpakai (Kernel + BIOS + VGA)
    uint32_t reserved_pages = 0x100000 / PAGE_SIZE;
    for (uint32_t i = 0; i < reserved_pages; i++) {
        mem_bitmap[i / 8] |= (1 << (i % 8));
        used_pages++;
    }
}

// Implementasi kmalloc sangat sederhana (Bump Allocator)
// Untuk OS hobi, ini cukup stabil asalkan tidak sering kfree
void* kmalloc(uint32_t size) {
    if (heap_used + size > HEAP_SIZE) {
        print("Error: Out of Memory!\n", 0x0C);
        return NULL;
    }

    void* ptr = (void*)(heap_ptr + heap_used);
    heap_used += size;
    
    // Update used_pages (estimasian)
    used_pages = (0x100000 + heap_used) / PAGE_SIZE;
    
    return ptr;
}

void kfree(void* ptr) {
    // kfree di hobi OS seringkali kosong karena implementasi free list itu rumit
    // Kita biarkan kosong dulu untuk menjaga stabilitas kernel
}

unsigned int get_total_memory() { return total_pages * PAGE_SIZE; }
unsigned int get_used_memory() { return (0x100000 + heap_used); }
unsigned int get_free_memory() { return get_total_memory() - get_used_memory(); }
