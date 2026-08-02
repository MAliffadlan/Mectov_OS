#include "../include/mem.h"
#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/spinlock.h"

static spinlock_t heap_lock = SPINLOCK_INIT;


static uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t page_tables[64][1024] __attribute__((aligned(4096)));  // 64 × 4MB = 256MB identity map
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
    uint32_t reserved_pages = 0x3000000 / PAGE_SIZE; // 48MB reserved for kernel/stack/heap
    if (reserved_pages > total_pages) reserved_pages = total_pages;
    for (uint32_t i = 0; i < reserved_pages; i++) {
        if (mem_bitmap) mem_bitmap[i / 8] |= (1 << (i % 8));
        used_pages++;
    }
}

void paging_init(uint32_t fb_paddr, uint32_t fb_size) {
    (void)fb_size;
    memset(page_directory, 0, sizeof(page_directory));
    
    // Map available physical memory
    uint32_t num_tables = (total_pages + 1023) / 1024;
    if (num_tables == 0) num_tables = 8;
    if (num_tables > 64) num_tables = 64;

    // KERNEL-ONLY: no PAGE_USER here. This map covers kernel .text, .data,
    // .bss (including the task table), the kmalloc heap and the page tables
    // themselves. Marking it user-accessible — as this did until v35 — let any
    // Ring 3 program read and write all of it with a plain store, no syscall
    // involved, which meant there was no Ring 0/Ring 3 boundary at all.
    //
    // Pages a process is genuinely allowed to touch get PAGE_USER individually
    // from vmm_map_page(): its image and heap at 0x08000000, its stack at
    // USER_STACK_TOP, and the framebuffer tables built below.
    for(uint32_t t = 0; t < num_tables; t++) {
        for(unsigned int j = 0; j < 1024; j++) {
            page_tables[t][j] = ((t * 1024 + j) * 4096) | PAGE_PRESENT | PAGE_RW;
        }
        page_directory[t] = ((uint32_t)page_tables[t]) | PAGE_PRESENT | PAGE_RW;
    }

    // Map Framebuffer separately if it's above our identity mapped RAM
    if (fb_paddr >= (num_tables * 4 * 1024 * 1024)) {
        uint32_t fb_pde_idx = fb_paddr >> 22;
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 1024; j++) {
                fb_page_tables[i][j] = (fb_paddr + i * 0x400000 + j * 0x1000) | PAGE_PRESENT | PAGE_RW | PAGE_USER;
            }
            page_directory[fb_pde_idx + i] = ((uint32_t)fb_page_tables[i]) | PAGE_PRESENT | PAGE_RW | PAGE_USER;
        }
    }

    // Map APIC (0xFEE00000) and IOAPIC (0xFEC00000)
    // They both fall into the same 4MB page directory entry (0x3FB, which is 1019)
    static uint32_t apic_page_table[1024] __attribute__((aligned(4096)));
    for (int j = 0; j < 1024; j++) {
        apic_page_table[j] = (0xFEC00000 + j * 0x1000) | PAGE_PRESENT | PAGE_RW;
    }
    // Set caching disabled for APIC regions
    apic_page_table[0x200] |= 0x18; // 0xFEE00000 offset 0x200 pages
    apic_page_table[0x000] |= 0x18; // 0xFEC00000 offset 0
    page_directory[1019] = ((uint32_t)apic_page_table) | PAGE_PRESENT | PAGE_RW;

    __asm__ __volatile__("mov %0, %%cr3": : "r"(page_directory));
    uint32_t cr0;
    __asm__ __volatile__("mov %%cr0, %0": "=r"(cr0));
    cr0 |= 0x80000000;  // PG  — enable paging
    cr0 |= 0x00010000;  // WP  — honour read-only pages in supervisor mode too.
                        // Without this a Ring 0 write silently succeeds on a
                        // read-only page, which would skip the COW handler in
                        // idt.c whenever the kernel is the one writing.
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
        // Keep the PDE kernel-only unless this mapping explicitly asked to be
        // user-reachable — see the note in paging_init().
        uint32_t pde_flags = PAGE_PRESENT | PAGE_RW;
        if (flags & PAGE_USER) pde_flags |= PAGE_USER;
        page_directory[pd_idx] = ((uint32_t)page_tables[pd_idx]) | pde_flags;
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
    uint32_t max_heap = 24 * 1024 * 1024; // 24MB max heap (heap base=24MB, so grows to 48MB — matching KERNEL_RESERVED_PAGES)
    
    if (size > max_heap || size + META_SIZE < size || heap_used + size + META_SIZE > max_heap) return NULL;
    
    block_meta *block = (block_meta*)((uint8_t*)0x1800000 + heap_used);
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

    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&heap_lock);

    void* result = NULL;
    if (!global_base) { // First call
        block_meta *block = request_space(NULL, size);
        if (block) {
            global_base = block;
            result = (void*)(block + 1);
        }
    } else {
        block_meta *last = global_base;
        block_meta *block = find_free_block(&last, size);
        if (!block) {
            block = request_space(last, size);
            if (block) {
                result = (void*)(block + 1);
            }
        } else {
            // Block splitting to save memory and reduce internal fragmentation
            if (block->size >= size + META_SIZE + 4) {
                block_meta *new_block = (block_meta*)((uint8_t*)block + META_SIZE + size);
                new_block->size = block->size - size - META_SIZE;
                new_block->free = 1;
                new_block->next = block->next;
                
                block->size = size;
                block->next = new_block;
            }
            block->free = 0;
            result = (void*)(block + 1);
        }
    }

    spin_unlock(&heap_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
    return result;
}

void kfree(void* p) {
    if (!p) return;

    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&heap_lock);

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

    spin_unlock(&heap_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
}

void* krealloc(void* ptr, uint32_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }
    if (new_size % 4 != 0) new_size += 4 - (new_size % 4);

    block_meta *block = (block_meta*)ptr - 1;
    if (block->size >= new_size) return ptr; // Already big enough

    // Allocate new block, copy data, free old
    void* new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;
    uint32_t copy_size = block->size < new_size ? block->size : new_size;
    memcpy(new_ptr, ptr, copy_size);
    kfree(ptr);
    return new_ptr;
}

void* kcalloc(uint32_t num, uint32_t size) {
    if (num != 0 && size > 0xFFFFFFFF / num) return NULL;  // overflow guard
    uint32_t total = num * size;
    void* ptr = kmalloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void kmalloc_stats(void (*print_fn)(const char*, unsigned char)) {
    char buf[64];
    block_meta *curr = global_base;
    uint32_t total_free = 0, total_used = 0, largest_free = 0, frag_count = 0;

    while (curr) {
        if (curr->free) {
            total_free += curr->size;
            if (curr->size > largest_free) largest_free = curr->size;
            frag_count++;
        } else {
            total_used += curr->size;
        }
        curr = curr->next;
    }

    // Convert numbers to string and print
    print_fn("Heap Allocator Stats:\n", 0x0B);
    print_fn("  Heap Base      : 0x1800000\n", 0x0F);
    print_fn("  Heap Used      : ", 0x0F);
    // Simple int to string
    int n = heap_used;
    int i = 0;
    if (n == 0) { buf[i++] = '0'; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    buf[i] = '\0';
    // Reverse
    for (int j = 0; j < i/2; j++) { char t = buf[j]; buf[j] = buf[i-1-j]; buf[i-1-j] = t; }
    buf[i] = ' '; buf[i+1] = 'B'; buf[i+2] = '\n'; buf[i+3] = '\0';
    print_fn(buf, 0x0A);

    n = total_used; i = 0;
    if (n == 0) { buf[i++] = '0'; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    buf[i] = '\0';
    for (int j = 0; j < i/2; j++) { char t = buf[j]; buf[j] = buf[i-1-j]; buf[i-1-j] = t; }
    buf[i] = ' '; buf[i+1] = 'B'; buf[i+2] = '\n'; buf[i+3] = '\0';
    print_fn("  Total Used     : ", 0x0F); print_fn(buf, 0x0A);

    n = total_free; i = 0;
    if (n == 0) { buf[i++] = '0'; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    buf[i] = '\0';
    for (int j = 0; j < i/2; j++) { char t = buf[j]; buf[j] = buf[i-1-j]; buf[i-1-j] = t; }
    buf[i] = ' '; buf[i+1] = 'B'; buf[i+2] = '\n'; buf[i+3] = '\0';
    print_fn("  Total Free     : ", 0x0F); print_fn(buf, 0x0A);

    n = largest_free; i = 0;
    if (n == 0) { buf[i++] = '0'; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    buf[i] = '\0';
    for (int j = 0; j < i/2; j++) { char t = buf[j]; buf[j] = buf[i-1-j]; buf[i-1-j] = t; }
    buf[i] = ' '; buf[i+1] = 'B'; buf[i+2] = '\n'; buf[i+3] = '\0';
    print_fn("  Largest Free   : ", 0x0F); print_fn(buf, 0x0A);

    n = frag_count; i = 0;
    if (n == 0) { buf[i++] = '0'; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    buf[i] = '\0';
    for (int j = 0; j < i/2; j++) { char t = buf[j]; buf[j] = buf[i-1-j]; buf[i-1-j] = t; }
    buf[i] = '\n'; buf[i+1] = '\0';
    print_fn("  Free Fragments : ", 0x0F); print_fn(buf, 0x0A);
}

unsigned int get_total_memory() { return total_pages * 4096; }
unsigned int get_used_memory() { return (24 * 1024 * 1024) + heap_used; }
unsigned int get_free_memory() { return get_total_memory() - get_used_memory(); }
