/* M10: kernel heap on top of the M3 PMM + VMM.
 *
 * A port of the 32-bit allocator (src/sys/mem.c): same block_meta free list,
 * same magic + canary hardening, same kmalloc/kfree/krealloc/kcalloc surface,
 * same counters. One structural difference: the arena is no longer a fixed
 * 24MB window at 0x1800000 that can only run out.
 *
 *   HEAP_VA_BASE 0x90000000 (2.25GB), up to 64MB, supervisor RW+NX
 *
 * That VA sits between the ranges M3/M4 already own: above the user window
 * (1-2GB: demo images + per-task stacks) and above the M3 selftest scratch
 * (0x80000000) whose PDPT slot it shares. Pages are backed lazily — the first
 * block that needs VA past the mapped tail maps a fresh frame there. The VA
 * stays CONTINUOUS whatever the frames are, which is what lets the block list
 * stay a straight port: a block may span two pages and both sides are mapped.
 *
 * Every mapping goes into pml4_boot explicitly (vmm_map_page_in), never into
 * "the current root". kmalloc is reachable from a task context, and a PTE
 * created in that task's private tables would be invisible to every other
 * task; boot's supervisor tables are shared by pointer into every clone (M5),
 * so a heap page mapped once is visible from every address space — and
 * vmm_teardown_space correctly leaves it alone because it compares against
 * pml4_boot.
 *
 * Lock order: heap_lock -> mem_lock (taken inside pmm_alloc and
 * vmm_map_page_in on the growth path), never the reverse.
 */
#include "cpu64.h"
#include "spin64.h"

extern u64 pml4_boot[];

#define HEAP_VA_BASE 0x90000000ULL
#define HEAP_MAX (64ULL * 1024 * 1024)
#define HEAP_ALIGN 16ULL
#define HEAP_PAGE 4096ULL
/* Header (32B) + trailing canary (8B) + 8B pad: a payload of size 16k puts the
 * next block on a 16-byte boundary too. */
#define HEAP_META 48ULL

#define HEAP_MAGIC_ALLOC 0xA110CA8EULL
#define HEAP_MAGIC_FREE 0xF2EEF2EEULL
#define HEAP_CANARY 0xCA5CA5E5CA5CA5E5ULL

typedef struct block_meta {
    u64 magic; /* ALLOC (live) / FREE (on the free list) */
    u64 size;  /* payload bytes (16-aligned) */
    u64 free;
    struct block_meta *next;
} block_meta;

static spin64_t heap_lock = SPIN64_INIT;
static block_meta *global_base;
static u64 heap_used;   /* arena bytes carved into blocks (from HEAP_VA_BASE) */
static u64 heap_mapped; /* arena bytes backed by frames */
static u64 heap_pages;  /* frames mapped into the arena */
static u64 heap_growths;
static u64 heap_allocs, heap_frees, heap_oom;
static u64 heap_canary_failures, heap_magic_failures;
static u64 heap_probes, heap_probe_failures;

static inline void *b_payload(block_meta *b) {
    return (void *)((u8 *)b + sizeof(block_meta));
}
static inline u64 *b_canary(block_meta *b) {
    return (u64 *)((u8 *)b + sizeof(block_meta) + b->size);
}

static void heap64_halt(void) {
    for (;;) __asm__ __volatile__("cli; hlt");
}

/* Allocator corruption: report and halt instead of handing back a corrupted
 * free list. Lock-free serial on purpose — the heap lock may be held here. */
static void heap64_panic(const char *what, void *p, void *ra) {
    s_raws("[K64] FATAL: heap ");
    s_raws(what);
    s_raws(" ptr=");
    s_rawx((u64)p);
    s_raws(" caller=");
    s_rawx((u64)ra);
    s_raws("\n");
    heap64_halt();
}

/* Map one arena page through boot's tables (see the file comment). */
static int heap_map_page(u64 va, u64 pa) {
    u64 fl = VMM_RW | (cpu_nx_enabled() ? VMM_NX : 0);
    return vmm_map_page_in(pml4_boot, va, pa, fl);
}

/* Back the arena up to `need_end` (heap_lock held). Fresh frames are zeroed:
 * the PMM recycles frames from torn-down tasks, and kmalloc must not hand a
 * dead task's bytes to its next caller. */
static int heap_extend(u64 need_end) {
    while (heap_mapped < need_end) {
        u64 va = HEAP_VA_BASE + heap_mapped;
        u64 pa = pmm_alloc();
        if (!pa) return -1;
        if (heap_map_page(va, pa)) {
            pmm_free(pa);
            return -1;
        }
        for (u64 i = 0; i < HEAP_PAGE; i += 8) ((volatile u64 *)(va + i))[0] = 0;
        heap_mapped += HEAP_PAGE;
        heap_pages++;
        heap_growths++;
    }
    return 0;
}

static block_meta *find_free_block(block_meta **last, u64 size) {
    block_meta *cur = global_base;
    while (cur && !(cur->free && cur->size >= size)) {
        *last = cur;
        cur = cur->next;
    }
    return cur;
}

/* Carve a new block at the arena tail (heap_lock held). `last` is the current
 * tail when the arena is already grown. */
static block_meta *request_space(block_meta *last, u64 size) {
    if (size > HEAP_MAX || size + HEAP_META < size ||
        heap_used + size + HEAP_META > HEAP_MAX)
        return 0;
    u64 need_end = heap_used + size + HEAP_META;
    if (need_end > heap_mapped &&
        heap_extend((need_end + HEAP_PAGE - 1) & ~(HEAP_PAGE - 1)))
        return 0;

    block_meta *block = (block_meta *)(HEAP_VA_BASE + heap_used);
    heap_used = need_end;
    if (last) last->next = block;
    block->magic = HEAP_MAGIC_ALLOC;
    block->size = size;
    block->free = 0;
    block->next = 0;
    return block;
}

void *kmalloc(u64 size) {
    if (!size) return 0;
    if (size & (HEAP_ALIGN - 1)) size = (size + HEAP_ALIGN - 1) & ~(HEAP_ALIGN - 1);

    u64 f = spin64_lock_irqsave(&heap_lock);
    void *result = 0;
    block_meta *block;
    if (!global_base) {
        block = request_space(0, size);
        if (block) {
            global_base = block;
            result = b_payload(block);
        } else {
            heap_oom++;
        }
    } else {
        block_meta *last = global_base;
        block = find_free_block(&last, size);
        if (!block) {
            block = request_space(last, size);
            if (block) {
                result = b_payload(block);
            } else {
                heap_oom++;
            }
        } else {
            /* A block on the free list must carry the FREE magic: anything
             * else means the list itself was corrupted (a write past a
             * neighbour's payload, or a wild kfree). */
            if (block->magic != HEAP_MAGIC_FREE) {
                heap_magic_failures++;
                heap64_panic("free-list header corrupted", block,
                             __builtin_return_address(0));
            }
            /* Split when the tail can hold a worthwhile block of its own. */
            if (block->size >= size + HEAP_META + HEAP_ALIGN) {
                block_meta *nb = (block_meta *)((u8 *)block + HEAP_META + size);
                nb->magic = HEAP_MAGIC_FREE;
                nb->size = block->size - size - HEAP_META;
                nb->free = 1;
                nb->next = block->next;
                *b_canary(nb) = HEAP_CANARY;
                block->size = size;
                block->next = nb;
            }
            block->magic = HEAP_MAGIC_ALLOC;
            block->free = 0;
            result = b_payload(block);
        }
    }
    if (result) {
        *b_canary((block_meta *)result - 1) = HEAP_CANARY;
        heap_allocs++;
    }
    spin64_unlock_irqrestore(&heap_lock, f);
    return result;
}

void kfree(void *p) {
    if (!p) return;
    u64 f = spin64_lock_irqsave(&heap_lock);

    block_meta *block = (block_meta *)p - 1;
    void *ra = __builtin_return_address(0);
    /* Range check first: a wild pointer must not make us read an unmapped
     * address while "verifying" its header. */
    if ((u64)block < HEAP_VA_BASE ||
        (u64)block + HEAP_META > HEAP_VA_BASE + heap_mapped) {
        heap_magic_failures++;
        heap64_panic("kfree of an out-of-arena pointer", p, ra);
    }
    /* A live block carries ALLOC magic: a pointer that never came from kmalloc
     * (or a second kfree) fails here, before it corrupts the free list. */
    if (block->magic != HEAP_MAGIC_ALLOC) {
        heap_magic_failures++;
        heap64_panic("kfree of non-allocated pointer (double free or bad pointer)",
                     p, ra);
    }
    if (*b_canary(block) != HEAP_CANARY) {
        heap_canary_failures++;
        heap64_panic("heap overflow detected (canary overwritten)", p, ra);
    }
    if (block->free) {
        heap_magic_failures++;
        heap64_panic("double free (block already free)", p, ra);
    }

    block->magic = HEAP_MAGIC_FREE;
    block->free = 1;
    heap_frees++;

    /* Forward coalescing: merge with the next block while it is free too. */
    while (block->next && block->next->free) {
        block->size += HEAP_META + block->next->size;
        block->next = block->next->next;
    }
    *b_canary(block) = HEAP_CANARY; /* payload end moved */

    /* Backward coalescing: find the predecessor and merge if it is free. */
    if (global_base != block) {
        block_meta *prev = global_base;
        while (prev && prev->next != block) prev = prev->next;
        if (prev && prev->free) {
            prev->size += HEAP_META + block->size;
            prev->next = block->next;
            *b_canary(prev) = HEAP_CANARY;
        }
    }
    spin64_unlock_irqrestore(&heap_lock, f);
}

void *krealloc(void *p, u64 new_size) {
    if (!p) return kmalloc(new_size);
    if (!new_size) {
        kfree(p);
        return 0;
    }
    block_meta *b = (block_meta *)p - 1;
    if (b->size >= new_size) return p;
    void *np = kmalloc(new_size);
    if (!np) return 0;
    u64 copy = b->size < new_size ? b->size : new_size;
    for (u64 i = 0; i < copy; i++) ((u8 *)np)[i] = ((u8 *)p)[i];
    kfree(p);
    return np;
}

void *kcalloc(u64 n, u64 size) {
    if (n && size > (~0ULL) / n) return 0;
    u64 total = n * size;
    void *p = kmalloc(total);
    if (p)
        for (u64 i = 0; i < total; i++) ((u8 *)p)[i] = 0;
    return p;
}

/* Snapshot for `kmem` (Ring-3) and the selftest. The list walk needs the
 * lock: another CPU may be splitting/merging right now. */
void heap64_stats(kmem64_t *s) {
    u64 f = spin64_lock_irqsave(&heap_lock);
    s->arena_base = HEAP_VA_BASE;
    s->arena_max = HEAP_MAX;
    s->arena_used = heap_used;
    s->arena_mapped = heap_mapped;
    s->pages = heap_pages;
    s->growths = heap_growths;
    s->allocated = 0;
    s->free_bytes = 0;
    s->blocks = 0;
    s->free_blocks = 0;
    s->largest_free = 0;
    for (block_meta *b = global_base; b; b = b->next) {
        if (b->free) {
            s->free_bytes += b->size;
            s->free_blocks++;
            if (b->size > s->largest_free) s->largest_free = b->size;
        } else {
            s->allocated += b->size;
            s->blocks++;
        }
    }
    s->allocs = heap_allocs;
    s->frees = heap_frees;
    s->oom = heap_oom;
    s->canary_failures = heap_canary_failures;
    s->magic_failures = heap_magic_failures;
    s->probes = heap_probes;
    s->probe_failures = heap_probe_failures;
    spin64_unlock_irqrestore(&heap_lock, f);
}

/* A heap round trip Ring-3 can trigger (SYS64_KMEMPROBE). This is what proves
 * the arena is mapped in the CALLING task's address space: a task runs on its
 * own PML4, not boot's, so a heap pointer that only existed in boot's tables
 * would fault here. Returns the byte count it managed, 0 on failure. */
u64 heap64_probe(u64 bytes) {
    if (!bytes || bytes > (1ULL << 20)) return 0;
    u8 *b = (u8 *)kmalloc(bytes);
    if (!b) {
        heap_probe_failures++;
        return 0;
    }
    for (u64 i = 0; i < bytes; i++) b[i] = (u8)(i ^ 0x5A);
    for (u64 i = 0; i < bytes; i++) {
        if (b[i] != (u8)(i ^ 0x5A)) {
            heap_probe_failures++;
            kfree(b);
            return 0;
        }
    }
    kfree(b);
    heap_probes++;
    return bytes;
}

void heap64_init(void) {
    global_base = 0;
    heap_used = heap_mapped = 0;
    heap_pages = heap_growths = 0;
    heap_allocs = heap_frees = heap_oom = 0;
    heap_canary_failures = heap_magic_failures = 0;
    heap_probes = heap_probe_failures = 0;

    /* M10 lesson: back the FIRST page eagerly, right here, BEFORE any address
     * space is cloned. Sharing boot's supervisor tables with a clone is only
     * true below the PDPT level (clone_level always fresh-copies PML4 entries),
     * so a PDPT/PD created *after* a task exists would be visible on boot's
     * CR3 only — the task would take a #PF on its first heap pointer.
     * Mapping one page now wires PDPT[2] + its PD into boot's tables, and every
     * later growth only adds PTEs/new PTs under those shared frames. The frame
     * is not wasted: it is the arena's first page. */
    u64 f = spin64_lock_irqsave(&heap_lock);
    int ok = heap_extend(HEAP_PAGE) == 0;
    spin64_unlock_irqrestore(&heap_lock, f);
    if (!ok) {
        s_raws("[K64] FAIL: heap cannot map its first page\n");
        heap64_halt();
    }
    s_puts("[K64] heap: arena ");
    s_hex64(HEAP_VA_BASE);
    s_puts(" max ");
    s_dec64(HEAP_MAX >> 20);
    s_puts("MB frame-backed, kmalloc/kfree/krealloc/kcalloc live\n");
}

/* ---- selftest (runs pre-STI, right after mem64_selftest) ---- */

static void heap_check(int ok, const char *what) {
    if (ok) return;
    s_raws("[K64] FAIL: heap ");
    s_raws(what);
    s_raws("\n");
    heap64_halt();
}

void heap64_selftest(void) {
    u64 free_pre = pmm_free_frames();

    /* 1. Fresh allocations: 16-aligned, packed back to back, first page zeroed
     *    (a recycled frame must not leak its old contents). */
    u8 *a = (u8 *)kmalloc(64);
    u8 *b = (u8 *)kmalloc(64);
    u8 *c = (u8 *)kmalloc(200);
    heap_check(a && b && c, "selftest alloc returned 0");
    heap_check(!((u64)a & 15) && !((u64)b & 15) && !((u64)c & 15),
               "payload not 16-aligned");
    heap_check((u8 *)b == a + HEAP_META + 64 && (u8 *)c == b + HEAP_META + 64,
               "fresh blocks are not packed (header size/alignment)");
    int zeroed = 1;
    for (u64 i = 0; i < 64; i++)
        if (a[i]) zeroed = 0;
    heap_check(zeroed, "fresh page not zeroed");

    /* 2. The arena VA must be a *mapping*, not the frame address itself: write
     *    through the VA, read the same bytes through the identity alias. */
    u64 pa = vmm_translate((u64)a);
    heap_check(pa != 0 && (pa & 0xFFF) == ((u64)a & 0xFFF),
               "heap VA not mapped to a frame");
    for (u64 i = 0; i < 64; i++) a[i] = (u8)(i * 3 + 1);
    int alias_ok = 1;
    for (u64 i = 0; i < 64; i++)
        if (((volatile u8 *)pa)[i] != (u8)(i * 3 + 1)) alias_ok = 0;
    heap_check(alias_ok, "alias mismatch (PTE points at the wrong frame)");

    /* 3. Split + first-fit reuse: freeing `a` must hand the same address back
     *    for a smaller request (64 < 16 + META + 16, so no split here). */
    kfree(a);
    u8 *a2 = (u8 *)kmalloc(16);
    heap_check(a2 == a, "first-fit did not reuse the freed block");
    kfree(b);
    kfree(a2);

    /* 4. Coalescing: the two 64-byte blocks plus the split tail only fit a
     *    176-byte request after they merged. */
    u8 *big = (u8 *)kmalloc(64 + HEAP_META + 64);
    heap_check(big == a, "adjacent free blocks did not coalesce");
    kfree(big);

    /* 5. A block spanning pages: fill every byte, check it back, and verify
     *    the three pages are three different frames. */
    u64 span = 12288;
    u8 *m = (u8 *)kmalloc(span);
    heap_check(m != 0, "multi-page alloc failed");
    for (u64 i = 0; i < span; i++) m[i] = (u8)(i * 7 + 5);
    int span_ok = 1;
    for (u64 i = 0; i < span; i++)
        if (m[i] != (u8)(i * 7 + 5)) span_ok = 0;
    heap_check(span_ok, "multi-page block lost bytes");
    u64 p0 = vmm_translate((u64)m), p1 = vmm_translate((u64)m + 4096),
        p2 = vmm_translate((u64)m + 8192);
    heap_check(p0 && p1 && p2 && p0 != p1 && p1 != p2,
               "multi-page block aliases frames");
    kfree(m);

    /* 6. kcalloc zeroes; krealloc preserves the payload. */
    u64 *z = (u64 *)kcalloc(32, 8);
    heap_check(z != 0, "kcalloc failed");
    int z_ok = 1;
    for (int i = 0; i < 32; i++)
        if (z[i]) z_ok = 0;
    heap_check(z_ok, "kcalloc not zeroed");
    for (int i = 0; i < 32; i++) z[i] = 0x1000u + (u64)i;
    u64 *z2 = (u64 *)krealloc(z, 8192);
    heap_check(z2 != 0, "krealloc failed");
    int r_ok = 1;
    for (int i = 0; i < 32; i++)
        if (z2[i] != 0x1000u + (u64)i) r_ok = 0;
    heap_check(r_ok, "krealloc lost data");
    kfree(z2);

    /* 7. Growth accounting: the arena is backed, page-exact, and grew. */
    heap_check(heap_pages > 0 && heap_mapped >= heap_used,
               "arena not backed by frames");
    heap_check(heap_pages * HEAP_PAGE == heap_mapped, "page accounting off");
    heap_check(pmm_free_frames() < free_pre, "no frame was actually taken");

    /* 8. OOM is clean: refused, counted, arena untouched. */
    u64 used_pre = heap_used, oom_pre = heap_oom;
    heap_check(kmalloc(HEAP_MAX + 4096) == 0, "over-window alloc succeeded");
    heap_check(heap_oom == oom_pre + 1, "OOM not counted");
    heap_check(heap_used == used_pre, "failed alloc changed the arena");

    /* 9. Everything freed must collapse into ONE free block covering the whole
     *    arena — the strongest statement that both coalescing directions work. */
    kfree(c);
    kmem64_t s;
    heap64_stats(&s);
    heap_check(s.blocks == 0, "live blocks remain after freeing everything");
    heap_check(s.free_blocks == 1, "free list did not collapse into one block");
    heap_check(s.free_bytes == heap_used - HEAP_META, "collapsed size wrong");

    s_puts("[K64] M10 HEAP SELFTEST OK (align/pack/split/coalesce/span/alias/"
           "OOM/collapse exact) pages=");
    s_dec64(heap_pages);
    s_puts(" arena=");
    s_dec64(heap_used);
    s_puts(" frames_free=");
    s_dec64(pmm_free_frames());
    s_puts("\n");
}
