#include "../include/mem.h"
#include "../include/vmm.h"
#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/serial.h"
#include "../include/spinlock.h"
#include "../include/msr.h"

static spinlock_t heap_lock = SPINLOCK_INIT;

// ---- Heap hardening (kernel locking/heap audit v38.4) ----
//
// Every allocation carries a header MAGIC and a trailing 4-byte CANARY:
//   [ block_meta (magic,size,free,next) ][ payload size bytes ][ canary ]
// kfree() verifies the magic (a pointer that never came from kmalloc, or a
// double free, is caught before it corrupts the free list) and the canary (a
// buffer overflow past the payload is caught at free time). Either failure
// prints a clear [KERNEL PANIC] and halts instead of silently corrupting
// memory. OOM returns NULL cleanly and is counted (callers that ignore the
// NULL are their own problem, but the allocator never hangs or loops).
#define HEAP_MAGIC_ALLOC 0xA110CA8Eu
#define HEAP_MAGIC_FREE  0xF2EEF2EEu
#define HEAP_CANARY      0xCA5CA5E5u

// Headers + trailing canary. A block occupies META_SIZE + payload bytes.
#define META_SIZE (sizeof(block_meta) + 4)

// Allocator counters, read by /proc/meminfo via kmalloc_get_stats().
static uint32_t heap_allocs = 0, heap_frees = 0;
static uint32_t heap_oom = 0;
static uint32_t heap_canary_failures = 0, heap_magic_failures = 0;

// Print a clear message and halt. Never returns. Used for heap corruption the
// allocator detects (bad magic, canary stomped) — the alternative was silent
// free-list corruption that crashed far from the cause.
static void heap_panic(const char* what, uint32_t addr, uint32_t caller_ra) {
    write_serial_string("\n[KERNEL PANIC] HEAP CORRUPTION: ");
    write_serial_string(what);
    write_serial_string(" @ 0x");
    write_serial_hex(addr);
    write_serial_string(" from ");
    write_serial_hex(caller_ra);
    write_serial_string("\n");
    extern void print(const char* s, unsigned char color);
    print("\n[KERNEL PANIC] Heap corruption: ", 0x0C);
    print(what, 0x0C);
    print("\n", 0x0C);
    extern void panic_finish(void);
    panic_finish();
}


// ---- Boot (kernel) paging structures (PAE, v38.49) ----
// 4 PDs (one per 512MB PDPT slot) + 256 identity PTs (each covers 2MB —
// 256 × 2MB = the 512MB ceiling) + 2 framebuffer PTs. All static BSS with
// virt == phys, so their addresses can be written into entries directly.
pte_t boot_pdpt[PDPT_ENTRIES] __attribute__((aligned(4096)));
pte_t boot_pds[PDPT_ENTRIES][PT_ENTRIES] __attribute__((aligned(4096)));
static pte_t page_tables[256][PT_ENTRIES] __attribute__((aligned(4096)));
static pte_t fb_page_tables[2][PT_ENTRIES] __attribute__((aligned(4096)));
// APIC MMIO PTs: 0xFEC00000 (I/O APIC) and 0xFEE00000 (LAPIC) sit in
// DIFFERENT 2MB PD regions under PAE (they shared one 4MB PDE before).
static pte_t apic_page_tables[2][PT_ENTRIES] __attribute__((aligned(4096)));

// 1 once EFER.NXE is enabled on the BSP (CPUID-gated). Filled by
// paging_init; consulted by page fillers before setting PAGE_NX.
static int nx_enabled = 0;
int paging_nx_enabled(void) { return nx_enabled; }

// get_cid() lives in task.c (needs LAPIC MMIO up). paging_init runs long
// before tasking, so a local read of the LAPIC id is the honest way to say
// "this is the BSP" during early boot. Safe pre-paging: identity access.
static int get_cid_safe(void) {
    return (*(volatile uint32_t*)0xFEE00020 >> 24) & 15;
}

// EFER.NXE on the CURRENT cpu. MSR writes are cheap and idempotent; the
// BSP does this inside paging_init before CR0.PG, every AP in ap_main.
void paging_enable_nxe(void) {
    uint32_t a, b, c, d;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(0x80000001));
    if (!(d & (1u << 20))) return;   // no NX feature -> leave bit 63 reserved
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
    if (get_cid_safe() == 0) nx_enabled = 1;
}

// total_pages is the SINGLE source of truth for physical RAM: kernel_main
// reads it from the multiboot header, init_mem stores it, phys_init (vmm.c)
// sizes the frame allocator from it, and paging_init identity-maps it.
static uint32_t total_pages = 0;
static uint32_t identity_tables = 8;  // how many 2MB PTs paging_init mapped

void init_mem(uint32_t mem_size) {
    total_pages = mem_size / PAGE_SIZE;
    if (total_pages > PHYS_MAX_PAGES) total_pages = PHYS_MAX_PAGES;
    // Physical frame allocator gets the same multiboot-derived size. The old
    // write-only mem_bitmap here was dead code; the real bitmap lives in vmm.c
    // and is initialized right here, so there is exactly one source of truth.
    phys_init(total_pages);
}

void paging_init(uint32_t fb_paddr, uint32_t fb_size) {
    (void)fb_size;
    memset(boot_pdpt, 0, sizeof(boot_pdpt));
    memset(boot_pds, 0, sizeof(boot_pds));

    // Map available physical memory (up to the 512MB ceiling). One PT per
    // 2MB region under PAE (was one per 4MB).
    uint32_t num_tables = (total_pages + (PT_ENTRIES - 1)) / PT_ENTRIES;
    if (num_tables == 0) num_tables = 4;
    if (num_tables > 256) num_tables = 256;
    identity_tables = num_tables;

    // KERNEL-ONLY: no PAGE_USER here. This map covers kernel .text, .data,
    // .bss (including the task table), the kmalloc heap and the page tables
    // themselves. Marking it user-accessible — as this did until v35 — let any
    // Ring 3 program read and write all of it with a plain store, no syscall
    // involved, which meant there was no Ring 0/Ring 3 boundary at all.
    //
    // Pages a process is genuinely allowed to touch get PAGE_USER individually
    // from vmm_map_page(): its image and heap at 0x08000000, its stack at
    // USER_STACK_TOP, and the framebuffer tables built below.
    for (uint32_t t = 0; t < num_tables; t++) {
        for (uint32_t j = 0; j < PT_ENTRIES; j++) {
            page_tables[t][j] = ((uint64_t)(t * PT_ENTRIES + j) * 4096)
                                | PAGE_PRESENT | PAGE_RW;
        }
        boot_pds[0][t] = ((uint32_t)(uintptr_t)page_tables[t]) | PAGE_PRESENT | PAGE_RW;
    }
    boot_pdpt[0] = ((uint32_t)(uintptr_t)boot_pds[0]) | PAGE_PRESENT | PAGE_RW;

    // Map Framebuffer separately if it's above our identity mapped RAM.
    // The fb sits around ~4GB on QEMU -> PDPT slot 3.
    if (fb_paddr >= (num_tables * 2 * 1024 * 1024)) {
        uint32_t fbi = pdpt_index(fb_paddr);
        uint32_t fbd = pd_index(fb_paddr);
        for (int i = 0; i < 2; i++) {
            for (uint32_t j = 0; j < PT_ENTRIES; j++) {
                fb_page_tables[i][j] = ((uint64_t)fb_paddr + i * 0x200000 + j * 0x1000)
                                       | PAGE_PRESENT | PAGE_RW | PAGE_USER;
            }
            boot_pds[fbi][fbd + i] = ((uint32_t)(uintptr_t)fb_page_tables[i])
                                     | PAGE_PRESENT | PAGE_RW | PAGE_USER;
        }
        if (!(boot_pdpt[fbi] & PAGE_PRESENT)) {
            boot_pdpt[fbi] = ((uint32_t)(uintptr_t)boot_pds[fbi]) | PAGE_PRESENT | PAGE_RW;
        }
    }

    // Keep the framebuffer out of the physical frame allocator: with the
    // bitmap now sized from the multiboot RAM report, the fb (which sits at
    // ~4GB on QEMU, but can be lower on real hardware) must never be handed
    // out as a free frame.
    phys_reserve_region(fb_paddr, fb_size);

    // Map APIC MMIO: 0xFEC00000 (I/O APIC) and 0xFEE00000 (LAPIC) each own
    // a 2MB PT under PAE. Both PD entries live in PDPT slot 3.
    for (int i = 0; i < 2; i++) {
        uint64_t base = 0xFEC00000ULL + (uint64_t)i * 0x200000;
        for (uint32_t j = 0; j < PT_ENTRIES; j++) {
            apic_page_tables[i][j] = (base + j * 0x1000) | PAGE_PRESENT | PAGE_RW;
        }
        // Strong uncached (PCD|PWT) on the register page of each region.
        apic_page_tables[i][0] |= 0x18;
        uint32_t idx = pd_index((uint32_t)base);
        boot_pds[3][idx] = ((uint32_t)(uintptr_t)apic_page_tables[i]) | PAGE_PRESENT | PAGE_RW;
    }
    if (!(boot_pdpt[3] & PAGE_PRESENT)) {
        boot_pdpt[3] = ((uint32_t)(uintptr_t)boot_pds[3]) | PAGE_PRESENT | PAGE_RW;
    }

    // Enable PAE (CR4.PAE) BEFORE paging, then EFER.NXE (so bit 63 means
    // no-execute rather than reserved), then load the PDPT and set PG/WP.
    // NXE is per-CPU; APs repeat it in ap_main via paging_enable_nxe().
    uint32_t cr4;
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 0x00000020;   // PAE
    __asm__ __volatile__("mov %0, %%cr4" : : "r"(cr4));
    paging_enable_nxe();

    __asm__ __volatile__("mov %0, %%cr3" : : "r"(boot_pdpt));
    uint32_t cr0;
    __asm__ __volatile__("mov %%cr0, %0": "=r"(cr0));
    cr0 |= 0x80000000;  // PG  — enable paging
    cr0 |= 0x00010000;  // WP  — honour read-only pages in supervisor mode too.
                        // Without this a Ring 0 write silently succeeds on a
                        // read-only page, which would skip the COW handler in
                        // idt.c whenever the kernel is the one writing.
    __asm__ __volatile__("mov %0, %%cr0": : "r"(cr0));

    write_serial_string("[MEM] PAE paging on (NX ");
    write_serial_string(nx_enabled ? "enabled)\n" : "unavailable)\n");
}

// Map a virtual address to a physical address explicitly in the BOOT tables.
// Two families of caller: identity-range tweaks (SMP trampoline pages) and
// APIC MMIO pages (apic.c) — both regions are static here.
void page_map(uint32_t vaddr, uint32_t paddr, uint32_t flags) {
    uint32_t pi = pdpt_index(vaddr);
    uint32_t di = pd_index(vaddr);
    uint32_t ti = pt_index(vaddr);

    if (pi == 0 && di < identity_tables) {
        page_tables[di][ti] = (paddr & 0xFFFFF000ULL) | flags;
        // Keep the PD entry kernel-only unless asked to be user-reachable.
        uint32_t pde_flags = PAGE_PRESENT | PAGE_RW;
        if (flags & PAGE_USER) pde_flags |= PAGE_USER;
        boot_pds[0][di] = ((uint32_t)(uintptr_t)page_tables[di]) | pde_flags;
    } else if (vaddr >= 0xFEC00000 && vaddr < 0xFF000000) {
        // The two APIC MMIO regions own PD slots in PDPT slot 3.
        int which = (vaddr >= 0xFEE00000) ? 1 : 0;
        apic_page_tables[which][ti] = (paddr & 0xFFFFF000ULL) | flags;
    }
}


typedef struct block_meta {
    uint32_t magic;   // HEAP_MAGIC_ALLOC (live) / HEAP_MAGIC_FREE (free)
    uint32_t size;    // payload bytes (4-aligned)
    int free;
    struct block_meta *next;
} block_meta;

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
    block->magic = HEAP_MAGIC_ALLOC;
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
        } else {
            heap_oom++;
        }
    } else {
        block_meta *last = global_base;
        block_meta *block = find_free_block(&last, size);
        if (!block) {
            block = request_space(last, size);
            if (block) {
                result = (void*)(block + 1);
            } else {
                heap_oom++;
            }
        } else {
            // A free block found in the list must carry the FREE magic; anything
            // else means the free list itself was corrupted (heap overflow from
            // a neighbouring allocation, or a wild kfree).
            if (block->magic != HEAP_MAGIC_FREE) {
                heap_magic_failures++;
                heap_panic("free-list header corrupted", (uint32_t)block,
                           (uint32_t)__builtin_return_address(0));
            }
            // Block splitting to save memory and reduce internal fragmentation
            if (block->size >= size + META_SIZE + 4) {
                block_meta *new_block = (block_meta*)((uint8_t*)block + META_SIZE + size);
                new_block->magic = HEAP_MAGIC_FREE;
                new_block->size = block->size - size - META_SIZE;
                new_block->free = 1;
                new_block->next = block->next;
                // The new free block gets its own canary at its payload end.
                *(uint32_t*)((uint8_t*)new_block + sizeof(block_meta) + new_block->size) = HEAP_CANARY;

                block->size = size;
                block->next = new_block;
            }
            block->magic = HEAP_MAGIC_ALLOC;
            block->free = 0;
            result = (void*)(block + 1);
        }
    }

    if (result) {
        // Stamp the canary after the payload so kfree() can detect overruns.
        *(uint32_t*)((uint8_t*)result + ((block_meta*)result - 1)->size) = HEAP_CANARY;
        heap_allocs++;
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

    // Magic check: a pointer that never came from kmalloc, or a second kfree
    // of the same block, fails here — before we deref/rewrite arbitrary
    // memory. (A live block carries ALLOC magic; a free one carries FREE.)
    if (block->magic != HEAP_MAGIC_ALLOC) {
        heap_magic_failures++;
        heap_panic("kfree of non-allocated pointer (double free or bad pointer)",
                   (uint32_t)p, (uint32_t)__builtin_return_address(0));
    }

    // Canary check: a write past the payload (buffer overflow) stomped it.
    if (*(uint32_t*)((uint8_t*)p + block->size) != HEAP_CANARY) {
        heap_canary_failures++;
        heap_panic("heap overflow detected (canary overwritten)",
                   (uint32_t)p, (uint32_t)__builtin_return_address(0));
    }

    block->magic = HEAP_MAGIC_FREE;
    block->free = 1;
    heap_frees++;

    // Forward coalescing: merge with next block if it's also free
    while (block->next && block->next->free) {
        block->size += META_SIZE + block->next->size;
        block->next = block->next->next;
    }
    // Moved the payload end; re-stamp the canary.
    *(uint32_t*)((uint8_t*)block + sizeof(block_meta) + block->size) = HEAP_CANARY;

    // Backward coalescing: find previous block and merge if free
    if (global_base != block) {
        block_meta *prev = global_base;
        while (prev && prev->next != block) prev = prev->next;
        if (prev && prev->free) {
            prev->size += META_SIZE + block->size;
            prev->next = block->next;
            *(uint32_t*)((uint8_t*)prev + sizeof(block_meta) + prev->size) = HEAP_CANARY;
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

// Real numbers from the unified frame allocator (vmm.c), not the old
// `24MB + heap_used` approximation which ignored actual frame usage.
unsigned int get_used_memory() { return phys_get_used_pages() * 4096; }
unsigned int get_free_memory() { return get_total_memory() - get_used_memory(); }

// How many 4MB identity-map tables are present (for vmm.c bounds checks).
uint32_t mem_identity_tables(void) { return identity_tables; }

// Snapshot the allocator state for /proc/meminfo. Walks the block list under
// the heap lock so the numbers are consistent (a concurrent alloc/free can
// otherwise produce a torn read).
void kmalloc_get_stats(kmalloc_stats_t* s) {
    if (!s) return;
    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&heap_lock);

    s->heap_base = 0x1800000;
    s->heap_used = heap_used;
    s->allocated = 0;
    s->free_bytes = 0;
    s->blocks = 0;
    s->free_blocks = 0;
    s->largest_free = 0;
    block_meta* curr = global_base;
    while (curr) {
        if (curr->free) {
            s->free_bytes += curr->size;
            s->free_blocks++;
            if (curr->size > s->largest_free) s->largest_free = curr->size;
        } else {
            s->allocated += curr->size;
            s->blocks++;
        }
        curr = curr->next;
    }
    s->allocs = heap_allocs;
    s->frees = heap_frees;
    s->oom_count = heap_oom;
    s->canary_failures = heap_canary_failures;
    s->magic_failures = heap_magic_failures;

    spin_unlock(&heap_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
}
