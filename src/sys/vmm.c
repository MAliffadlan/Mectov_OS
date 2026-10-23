#include "../include/vmm.h"
#include "../include/mem.h"
#include "../include/utils.h"
#include "../include/serial.h"
#include "../include/spinlock.h"
#include "../include/acpi.h"   // smp_cpu_count / smp_lapic_ids (TLB shootdown)
#include "../include/apic.h"   // apic_get_id / apic_send_fixed_ipi / smp_lapic_addr
#include "../include/panic.h"  // panic_finish (tlb_self_test exit)
#include "../include/task.h"   // task_get_cid() for per-CPU cache

static spinlock_t vmm_lock = SPINLOCK_INIT;


// ============================================================
// Virtual Memory Manager — Layer di atas existing identity paging
// ============================================================
// Existing paging (mem.c) identity-maps 0..RAM (up to PHYS_MAX_PAGES*4KB).
// VMM adds physical frame tracking + per-process page directories
// WITHOUT touching the existing static page tables.
//
// page_dir = 0 → use global identity map (existing behavior)
// page_dir = any other value → use that page directory
//
// Physical frame tracking (single source of truth): the bitmap/refcount
// below is sized for PHYS_MAX_PAGES but its usable range is set once by
// phys_init() with the multiboot-detected RAM size (init_mem → phys_init),
// so a machine with 256MB or 512MB gets allocatable frames above 128MB.
// ============================================================

// ============================================================
// Buddy Allocator for Physical Frame Management
// ============================================================
// Orders 0-9: 4KB (1 page) to 2MB (512 pages)
// Free blocks tracked via doubly-linked lists, one per order.
// Metadata stored in-place in the first 8 bytes of each free frame.
//
// Allocation is a constant 10-way probe of the per-order heads (the smallest
// non-empty order wins) plus O(order) splits, so it does not scan memory at
// all — against the old allocator's worst case of walking 131k bits. Freeing
// coalesces eagerly: O(order) merges. The ceiling is the same order-9 block
// the allocator can describe, so the probe never grows with RAM size.

#define BUDDY_MAX_ORDER 9
#define BUDDY_ORDERS (BUDDY_MAX_ORDER + 1)
#define BUDDY_ALLOCATED 0xFF
// A frame parked in a per-CPU cache. Distinct from BUDDY_ALLOCATED so a
// second frame_free() of the same frame is detectable: the cache owns the
// frame, no caller does. Neither value is a valid order, so the coalescing
// guard already refuses to merge with either.
#define BUDDY_CACHED 0xFE

// Pinned frames are never released: the shared zero page, and every region
// phys_reserve_region() kept out of the allocator (the framebuffer and any
// other MMIO the kernel maps). refcount 255 is unreachable by the COW/shm
// bump paths (they stop below 255), so it is a safe "do not touch" marker.
#define FRAME_PINNED 255

// Free list node stored in-place in free frames
typedef struct buddy_node {
    struct buddy_node* next;
    struct buddy_node* prev;
} buddy_node_t;

// Per-order free list heads
static buddy_node_t* buddy_free_lists[BUDDY_ORDERS];

// ============================================================
// Per-CPU Frame Cache
// ============================================================
// Fast path for single-frame allocation: each CPU keeps a small local stock
// of frames so a burst (COW fault storm after fork, page-table build during
// clone) does not take the global vmm_lock once per frame.
//
// Accepted trade-off: a cache is never drained, so a CPU that has ever
// allocated holds up to PERCPU_CACHE_SIZE frames until it is asked for them
// again. System-wide that is at most MAX_CORES x 32 frames (2MB), and it is
// what makes the fast path lock-free. The cost is a producer/consumer split
// (free on CPU A, allocate on CPU B): B drains the free lists while A sits on
// stock, so B can report OOM with frames parked on A. Acceptable on this
// kernel's guest sizes; a drain-at-idle hook or a global cached-frame cap is
// the fix if it ever bites.
//
// A parked frame is NOT free. It carries BUDDY_CACHED in frame_order[] and 1
// in frame_ref_count[], so the buddy core can neither hand it out nor
// coalesce with it (BUDDY_CACHED is not a valid order), and it still counts
// in phys_used_count. It is held by a CPU instead of by a caller. Only when
// the cache overflows does a frame go back to the free lists, where
// buddy_free_locked drops it to refcount 0 and uncounts it.
//
// Lock order is one-way: vmm_lock -> cache lock, never the reverse. On the
// common paths the cache lock is dropped before vmm_lock is taken, so the
// two are not held together at all.
//
// The old bitmap allocator absorbed a double frame_free() because its
// refcount>0 test turned the second free into a no-op. That guard is kept:
// frame_free() claims a frame by writing BUDDY_CACHED *under vmm_lock* before
// the frame is parked, so a second free of the same frame — including one
// racing on another core, since the claim is serialized by vmm_lock — sees
// the marker and stops instead of parking the frame twice.

#define PERCPU_CACHE_SIZE 32
#define PERCPU_REFILL_THRESHOLD 8
#define PERCPU_REFILL_BATCH 16
#define PERCPU_FLUSH_KEEP (PERCPU_CACHE_SIZE / 2)

typedef struct {
    uint32_t frames[PERCPU_CACHE_SIZE];  // physical addresses
    int count;                           // frames currently held
    spinlock_t lock;                     // per-CPU: near-zero contention
} percpu_frame_cache_t;

static percpu_frame_cache_t percpu_caches[MAX_CORES];

// Allocator statistics. The per-CPU counters live in per-CPU slots so each
// core only ever writes its own — no atomics, no lock, no false sharing with
// a peer's counter (the existing per-CPU patterns — watchdog heartbeats,
// TLB shootdown acks — use the same shape). phys_get_stats() sums them.
static uint32_t stat_cache_hits[MAX_CORES];
static uint32_t stat_buddy_allocs[MAX_CORES];
static uint32_t stat_refill_frames[MAX_CORES];
static uint32_t stat_cached_frees[MAX_CORES];
static uint32_t stat_spilled_frees[MAX_CORES];
// These two are only ever touched under vmm_lock, so plain globals suffice.
static uint32_t stat_coalesces;
static uint32_t stat_oom;
static uint32_t stat_overfree;   // frame_free() on a frame nobody owns

static inline int this_cid(void) {
    int cid = task_get_cid();
    if (cid < 0 || cid >= MAX_CORES) cid = 0;   // clamp before AP bring-up
    return cid;
}

// Buddy core, defined below; the per-CPU cache calls back into them.
static uint32_t buddy_alloc_locked(void);
static void buddy_free_locked(uint32_t paddr);
// Free-list construction, defined below with the rest of the buddy layer.
// phys_reserve_region() needs both, and it runs before those definitions.
static int buddy_lists_built = 0;
static void buddy_rebuild_lists(void);

// Boot-time sanity check for the frame allocator (defined after the
// allocator itself; called at the end of phys_init).
static void phys_allocator_self_test(void);

// Bitmap for initialization (16k bytes: one bit per frame)
static uint8_t frame_bitmap[PHYS_MAX_PAGES / 8];

// Per-frame order tracking (131k bytes: one byte per frame). A byte is
// meaningful only when the frame is the head of a free block, where it holds
// that block's order (0-9). Otherwise it is BUDDY_ALLOCATED (0xFF, held by a
// caller or reserved) or BUDDY_CACHED (0xFE, parked in a per-CPU cache).
// Interior frames of a multi-frame block keep stale values; nothing reads
// them, because coalescing only ever inspects a block's aligned head.
static uint8_t frame_order[PHYS_MAX_PAGES];

uint8_t frame_ref_count[PHYS_MAX_PAGES];
static uint32_t phys_total_pages = 0;   // actual RAM / 4KB, from multiboot
// v38.100: reservation is adaptive — nominal KERNEL_RESERVED_PAGES capped at
// 75% of detected RAM (see phys_init), so small-RAM guests keep allocatable
// frames. All frame paths read this runtime value instead of the constant.
static uint32_t phys_reserved_pages = KERNEL_RESERVED_PAGES;
static uint32_t phys_used_count = 0;    // maintained used-frame total (O(1) meminfo)
static int vmm_initialized = 0;

// Shared zero page for lazy zero-fill (heap + user stack demand paging).
// Every demand-paged page maps here READ-ONLY until the first write COW-
// duplicates it into a private frame, so processes that allocate but barely
// touch memory share a single frame instead of one zeroed frame each.
// Pinned: refcount 255 (never sole-owner, so a single process can never get
// a writable alias) and frame_free() ignores it outright.
static uint32_t phys_zero_page = 0;

// ---- Frame bitmap: "this frame is NOT on a buddy free list" ----
//
// A set bit means the frame is out of the free lists — it is live, parked in
// a per-CPU cache, or reserved (kernel region, zero page, framebuffer/MMIO).
// A clear bit means the buddy allocator owns it and it sits on some order's
// free list.
//
// This is authoritative, not advisory: phys_reserve_region() relies on it to
// decide what still needs removing, and buddy_rebuild_lists() reconstructs
// the free lists from it alone. So it must be maintained by the allocator:
// buddy_alloc_locked() sets the bit, buddy_free_locked() clears it. A frame's
// bit and its membership in a free list are always changed together.
static void bitmap_set(int idx) {
    frame_bitmap[idx / 8] |= (1 << (idx % 8));
}
static void bitmap_clear(int idx) {
    frame_bitmap[idx / 8] &= ~(1 << (idx % 8));
}
static int bitmap_test(int idx) {
    return (frame_bitmap[idx / 8] >> (idx % 8)) & 1;
}

// ---- Buddy allocator helpers ----

static inline uint32_t buddy_address(uint32_t frame_idx, int order) {
    return frame_idx ^ (1u << order);
}

static inline int is_aligned_to_order(uint32_t frame_idx, int order) {
    return (frame_idx & ((1u << order) - 1)) == 0;
}

static void buddy_list_add(uint32_t frame_idx, int order) {
    uint32_t paddr = frame_idx * 4096;
    buddy_node_t* node = (buddy_node_t*)(uintptr_t)paddr;

    node->next = buddy_free_lists[order];
    node->prev = NULL;

    if (buddy_free_lists[order]) {
        buddy_free_lists[order]->prev = node;
    }
    buddy_free_lists[order] = node;
    frame_order[frame_idx] = (uint8_t)order;
}

static void buddy_list_remove(uint32_t frame_idx, int order) {
    uint32_t paddr = frame_idx * 4096;
    buddy_node_t* node = (buddy_node_t*)(uintptr_t)paddr;

    if (node->prev) {
        node->prev->next = node->next;
    } else {
        buddy_free_lists[order] = node->next;
    }

    if (node->next) {
        node->next->prev = node->prev;
    }

    frame_order[frame_idx] = BUDDY_ALLOCATED;
}

// Initialize the buddy layer from the bitmap state phys_init has built up.
// Called by phys_init() after the kernel reservation and zero page are
// marked. Only frame_order[] is seeded here; the free lists themselves are
// built by the first allocation (buddy_rebuild_lists), because linking a free
// block writes its node pointers into the frame itself and that pass touches
// every free page.
//
// That build does NOT actually wait for paging: phys_allocator_self_test()
// allocates, and it runs at the end of phys_init. So by the time
// paging_init() calls phys_reserve_region() for the framebuffer, the free
// lists exist — which is exactly why phys_reserve_region() cannot rely on
// setting a bitmap bit and must rebuild them instead.
static void buddy_init(void) {
    // Initialize free list heads (empty until first alloc)
    for (int i = 0; i <= BUDDY_MAX_ORDER; i++) {
        buddy_free_lists[i] = NULL;
    }

    // The bitmap already marks the kernel region and the zero page, so it
    // alone tells us which frames start out free. Anything outside
    // phys_total_pages is left BUDDY_ALLOCATED and never indexed.
    for (uint32_t i = 0; i < phys_total_pages; i++) {
        frame_order[i] = bitmap_test(i) ? BUDDY_ALLOCATED : 0;
    }
}

// Called exactly once, from init_mem(), with the RAM size reported by the
// multiboot header. This is the single point that decides how many physical
// frames exist; everything else (frame_alloc/frame_free bounds, /proc/meminfo,
// identity map in mem.c) derives from it.
void phys_init(uint32_t total_pages) {
    if (phys_total_pages != 0) return;  // already initialized
    phys_total_pages = total_pages;
    if (phys_total_pages > PHYS_MAX_PAGES) phys_total_pages = PHYS_MAX_PAGES;

    memset(frame_bitmap, 0, sizeof(frame_bitmap));
    memset(frame_ref_count, 0, sizeof(frame_ref_count));

    // Reserve the kernel + heap region. The nominal reservation is
    // KERNEL_RESERVED_PAGES (80MB, sized for the Q3 hunk/zone heap on big
    // guests), but it must adapt to small RAM: on a 64MB guest reserving
    // every frame left zero frames for user space and boot died. Keep a
    // 25% frame floor so small guests stay bootable. mem.c reads the result
    // back via phys_reserved_bytes() to bound the kernel heap to the same
    // boundary.
    uint32_t reserve_pages = KERNEL_RESERVED_PAGES;
    // Big guests take the larger nominal reservation (v38.105). The official
    // Q3 engine core cannot boot inside the 56MB heap the 80MB nominal yields
    // — see KERNEL_RESERVED_PAGES_BIG — and every guest with this much RAM can
    // afford it without touching the 25% frame floor applied below. Small
    // guests keep the old nominal bit-for-bit, so their boot/fork numbers do
    // not move.
    if (phys_total_pages >= KERNEL_RESERVED_PAGES_BIG_MIN) {
        reserve_pages = KERNEL_RESERVED_PAGES_BIG;
        write_serial_string("[PHYS] big-RAM guest: large kernel reservation.\n");
    }
    uint32_t min_free = phys_total_pages / 4;
    if (reserve_pages + min_free > phys_total_pages) {
        reserve_pages = phys_total_pages - min_free;
        write_serial_string("[PHYS] small-RAM guest: reservation capped.\n");
    }

    // The kmalloc heap is a physical span inside this reservation, so the
    // reservation must be big enough to hold the heap floor (24MB base +
    // 4MB) no matter how small the guest is — otherwise mem.c would clamp
    // the heap to something that pokes past the reserved region and the
    // allocator would hand the same bytes to a process. Raising the
    // reservation here can eat into the 25% frame floor; a guest too small
    // for both is refused loudly below instead of booting into corruption.
    uint32_t reserve_min = (KERNEL_HEAP_BASE_BYTES / 4096) +
                           (KERNEL_HEAP_MIN_BYTES / 4096);
    if (reserve_pages < reserve_min) {
        reserve_pages = reserve_min;
        write_serial_string("[PHYS] reservation raised to cover kernel heap.\n");
    }
    if (reserve_pages > phys_total_pages) reserve_pages = phys_total_pages;
    if (reserve_pages >= phys_total_pages) {
        // Zero frames left for user space: no task, stack or framebuffer can
        // ever be allocated. Say exactly why instead of dying later in an
        // unrelated-looking place.
        write_serial_string("[PHYS] FATAL: guest RAM below the supported floor (");
        write_serial_hex(KERNEL_MIN_RAM_BYTES);
        write_serial_string(" bytes) - no frames left for user space.\n");
    }
    phys_reserved_pages = reserve_pages;
    for (uint32_t i = 0; i < reserve_pages && i < phys_total_pages; i++) {
        bitmap_set(i);
        frame_ref_count[i] = 1;
    }
    phys_used_count = reserve_pages;

    write_serial_string("[PHYS] allocator: ");
    write_serial_hex(phys_total_pages * 4096);
    write_serial_string(" bytes RAM, ");
    write_serial_hex(reserve_pages * 4096);
    write_serial_string(" reserved for kernel.\n");

    // Grab one frame for the shared zero page and pin it. Runs before paging
    // is enabled (paging_init is later), so direct physical access is fine.
    for (uint32_t i = reserve_pages; i < phys_total_pages; i++) {
        if (!bitmap_test(i)) {
            phys_zero_page = i * 4096;
            bitmap_set(i);
            frame_ref_count[i] = 255;  // pinned: never sole-owner, never freed
            phys_used_count++;
            memset((void*)(uintptr_t)phys_zero_page, 0, 4096);
            write_serial_string("[PHYS] shared zero page @ ");
            write_serial_hex(phys_zero_page);
            write_serial_string("\n");
            break;
        }
    }

    // Initialize buddy allocator from current bitmap state
    buddy_init();

    // Initialize per-CPU caches
    for (int i = 0; i < MAX_CORES; i++) {
        percpu_caches[i].count = 0;
        percpu_caches[i].lock = (spinlock_t)SPINLOCK_INIT;
    }

    write_serial_string("[PHYS] buddy allocator initialized (orders 0-");
    write_serial_hex(BUDDY_MAX_ORDER);
    write_serial_string(").\n");
    write_serial_string("[PHYS] per-CPU caches ready: ");
    write_serial_hex(PERCPU_CACHE_SIZE);
    write_serial_string(" frames x ");
    write_serial_hex(MAX_CORES);
    write_serial_string(" CPUs.\n");

    phys_allocator_self_test();
}

// The shared zero page (0 if the allocator could not reserve one).
uint32_t phys_get_zero_page(void) { return phys_zero_page; }

// v38.100: the runtime kernel reservation, in bytes. mem.c derives the kernel
// heap ceiling from this so the heap can never grow into frames that the
// allocator believes are free — on small-RAM guests the reservation shrinks
// (see phys_init) and the heap must shrink with it. Reads a single aligned
// uint32 set once by phys_init; no lock needed.
uint32_t phys_reserved_bytes(void) { return phys_reserved_pages * 4096u; }

// Mark a physical region (e.g. the framebuffer, MMIO) as in use so the
// allocator never hands it out. Called from paging_init() with the framebuffer
// address/size once they are known. Safe to call before/after phys_init.
//
// This runs AFTER phys_init has already built the free lists (the boot
// self-test forces the first allocation), so setting the bitmap bit is not
// enough on its own: the frames may still be linked inside free blocks, and
// the reservation starts at an arbitrary frame that can sit in the MIDDLE of
// a coalesced block whose head carries the order. frame_order[] cannot
// describe such an interior frame, so rather than trying to unlink it, the
// free lists are rebuilt from the bitmap — the one structure that is
// authoritative for every frame. The cost is O(frames), paid once, at boot.
void phys_reserve_region(uint32_t start, uint32_t len) {
    if (start == 0 || len == 0) return;
    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&vmm_lock);
    uint32_t first = start / 4096;
    uint32_t last = (start + len + 4095) / 4096;
    if (last > phys_total_pages) last = phys_total_pages;
    int stole_from_free = 0;
    for (uint32_t i = first; i < last; i++) {
        if (!bitmap_test(i)) {
            // Was on a buddy free list until now, so it moves into "used".
            // A frame already reserved stays counted once.
            phys_used_count++;
            stole_from_free = 1;
        }
        bitmap_set(i);
        // Pinned: the framebuffer/MMIO must never be releasable by a stray
        // frame_free(), which would put it into a cache and hand it to a
        // process as ordinary writable memory.
        frame_ref_count[i] = FRAME_PINNED;
    }
    // Only needed if the lists exist; if they are built later, the bitmap
    // already excludes these frames.
    if (stole_from_free && buddy_lists_built) {
        buddy_rebuild_lists();
    }
    spin_unlock(&vmm_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
}

// Real used-frame count (reserved kernel region + live allocations) for
// /proc/meminfo — replaces the old `24MB + heap_used` approximation.
// v38.47: O(1) — the counter is maintained by phys_init / phys_reserve_region
// / frame_alloc / frame_free. Direct refcount bumps (COW copies, SysV shm
// attaches, fork clones) never move a frame between free and used, so they
// correctly leave it alone. Aligned uint32 reads are atomic on x86, so the
// read needs no lock — /proc no longer scans 131k entries under a spinlock
// with IRQs off.
uint32_t phys_get_used_pages(void) {
    return phys_used_count;
}

void vmm_init(void) {
    if (vmm_initialized) return;
    vmm_initialized = 1;
    // Frame tracking is set up earlier by phys_init() (called from init_mem
    // with the multiboot RAM size); this only marks the VMM address-space
    // layer as ready.
}

// Allocate one physical frame. Returns physical address or 0.
// Interrupt-safe: callers include load_mct_app (normal task context, IF=1)
// as well as the page fault handler and syscalls (IF=0 via interrupt gates).
// Disabling interrupts while holding the lock prevents the scheduler from
// preempting the holder (task.c convention), and restoring eflags keeps the
// caller's original IF state intact when this is nested in a cli section.
// IRQ/exception-context accessors for vmm_lock: the #PF (COW) handler runs
// with IF=0 on the per-CPU fault stack and needs a consistent view of
// frame_ref_count vs concurrent frame_alloc/frame_free on other cores. Plain
// spin is correct here (exception context, IF=0); a user fault never happens
// while the same CPU holds vmm_lock, so no self-deadlock.
void vmm_lock_acquire_irq(void) { spin_lock(&vmm_lock); }
void vmm_lock_release_irq(void) { spin_unlock(&vmm_lock); }

// Rebuild every free list from scratch, using the bitmap as the authority on
// what is free. Every free frame becomes its own order-0 block, so this
// necessarily discards any coalescing done so far — that is the price of
// being able to reserve an arbitrary frame range after the lists exist
// (phys_reserve_region), and it is paid once, at boot.
//
// Caller must hold vmm_lock.
static void buddy_rebuild_lists(void) {
    for (int o = 0; o <= BUDDY_MAX_ORDER; o++) {
        buddy_free_lists[o] = NULL;
    }
    for (uint32_t i = phys_reserved_pages; i < phys_total_pages; i++) {
        if (!bitmap_test(i)) {
            buddy_list_add(i, 0);
        }
    }
    buddy_lists_built = 1;
}

// Buddy allocator core (called by cache refill and fallback path)
// Caller must hold vmm_lock
static uint32_t buddy_alloc_locked(void) {
    // Lazy build on the first allocation; see buddy_init for why this is
    // deferred rather than done up front.
    if (!buddy_lists_built) {
        buddy_rebuild_lists();
    }

    // Always allocate order-0 (single 4KB frame)
    int order = 0;

    // Find smallest available block >= order
    int current_order = order;
    while (current_order <= BUDDY_MAX_ORDER && !buddy_free_lists[current_order]) {
        current_order++;
    }

    // Out of memory. Not counted here: frame_alloc() probes twice (cache
    // refill, then this direct call) and counts the failure once at the call
    // site, so a single exhausted allocation does not read as two.
    if (current_order > BUDDY_MAX_ORDER) {
        return 0;
    }

    // Remove block from free list
    uint32_t frame_idx = ((uintptr_t)buddy_free_lists[current_order]) / 4096;
    buddy_list_remove(frame_idx, current_order);

    // Split larger blocks down to requested order
    while (current_order > order) {
        current_order--;
        uint32_t buddy_idx = frame_idx + (1u << current_order);
        buddy_list_add(buddy_idx, current_order);
    }

    // Out of the free lists (buddy_list_remove already set frame_order) and
    // counted as used: the frame is going into a cache or straight to a
    // caller, and either way it is off the free lists. A frame parked in a
    // cache is therefore "used" for as long as it sits there, which is what
    // /proc/meminfo's FrameCached explains.
    bitmap_set(frame_idx);
    frame_ref_count[frame_idx] = 1;
    phys_used_count++;

    return frame_idx * 4096;
}

// ============================================================
// Per-CPU Cache Operations
// ============================================================
// See the lock-order note above the cache structure. Every critical section
// here holds only the cache lock and calls nothing that could take another
// lock; the refill and spill paths explicitly drop it before touching
// vmm_lock.

static inline percpu_frame_cache_t* this_cpu_cache(void) {
    return &percpu_caches[this_cid()];
}

// Pull a batch of order-0 frames out of the buddy allocator into `out`.
// Returns how many were obtained. Acquires vmm_lock; the cache lock must
// NOT be held by the caller.
static int percpu_fetch_batch(uint32_t* out, int want) {
    int n = 0;
    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&vmm_lock);
    for (int i = 0; i < want; i++) {
        uint32_t paddr = buddy_alloc_locked();
        if (paddr == 0) break;
        out[n++] = paddr;
    }
    spin_unlock(&vmm_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
    stat_refill_frames[this_cid()] += (uint32_t)n;
    return n;
}

// Top the cache up when it runs low. Called with the cache lock held;
// drops it for the duration of the buddy refill so the two locks are
// never nested in the wrong order, then re-acquires it. On return the
// cache lock IS held and `*eflags` holds the state to restore.
//
// Every frame pulled from the buddy lands in exactly one place: either
// the cache or back in the buddy free lists. Nothing is ever dropped on
// the floor and nothing is ever counted twice.
static void percpu_cache_refill(percpu_frame_cache_t* cache, uint32_t* eflags) {
    if (cache->count >= PERCPU_REFILL_THRESHOLD) return;   // still stocked

    // Drop the cache lock before touching vmm_lock (one-way lock order).
    spin_unlock_irqrestore(&cache->lock, *eflags);

    uint32_t batch[PERCPU_REFILL_BATCH];
    int got = percpu_fetch_batch(batch, PERCPU_REFILL_BATCH);

    // Sort the batch: what fits stays, the rest goes back. The cache can
    // only be full here if another core shares this cache (LAPIC ids are
    // masked to MAX_CORES, so that needs more cores than the mask holds).
    uint32_t surplus[PERCPU_REFILL_BATCH];
    int dropped = 0;

    *eflags = spin_lock_irqsave(&cache->lock);
    for (int i = 0; i < got; i++) {
        if (cache->count < PERCPU_CACHE_SIZE) {
            cache->frames[cache->count++] = batch[i];
        } else {
            surplus[dropped++] = batch[i];
        }
    }
    spin_unlock_irqrestore(&cache->lock, *eflags);

    if (dropped > 0) {
        uint32_t vflags;
        __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(vflags));
        spin_lock(&vmm_lock);
        for (int i = 0; i < dropped; i++) buddy_free_locked(surplus[i]);
        spin_unlock(&vmm_lock);
        __asm__ __volatile__("push %0; popfl" : : "r"(vflags));
    }

    // The caller (percpu_cache_alloc) expects the cache lock held on return.
    *eflags = spin_lock_irqsave(&cache->lock);
}

// Fast path: hand out one frame from this CPU's cache. Returns 0 only
// when the buddy allocator is genuinely out of memory.
static uint32_t percpu_cache_alloc(void) {
    percpu_frame_cache_t* cache = this_cpu_cache();
    uint32_t eflags = spin_lock_irqsave(&cache->lock);

    if (cache->count < PERCPU_REFILL_THRESHOLD) {
        percpu_cache_refill(cache, &eflags);   // lock still held on return
    }

    uint32_t paddr = 0;
    if (cache->count > 0) {
        paddr = cache->frames[--cache->count];
        stat_cache_hits[this_cid()]++;
        // Back in a caller's hands: no longer parked. This byte is what
        // lets a stray frame_free() tell "owned by a caller" from "sitting
        // in a cache" without taking vmm_lock here — the whole point of the
        // cache. Safe without a lock because a parked frame has exactly one
        // owner (this cache), and nothing else writes this byte meanwhile.
        frame_order[paddr / 4096] = BUDDY_ALLOCATED;
    }

    spin_unlock_irqrestore(&cache->lock, eflags);
    return paddr;
}

// Push a frame back into this CPU's cache. When the cache is full, half
// of it (plus this frame) is handed back to the buddy allocator in one
// critical section, so a steady free stream costs one vmm_lock per ~16
// frees instead of one per free.
static void percpu_cache_free(uint32_t paddr) {
    percpu_frame_cache_t* cache = this_cpu_cache();
    uint32_t eflags = spin_lock_irqsave(&cache->lock);

    if (cache->count < PERCPU_CACHE_SIZE) {
        cache->frames[cache->count++] = paddr;
        // Marked parked so a stray frame_free() of this paddr is seen as an
        // over-free rather than treating the frame as caller-owned.
        frame_order[paddr / 4096] = BUDDY_CACHED;
        stat_cached_frees[this_cid()]++;
        spin_unlock_irqrestore(&cache->lock, eflags);
        return;
    }

    // Full: spill the tail of the cache (keeping half) plus this frame.
    uint32_t spill[PERCPU_CACHE_SIZE + 1];
    int n = 0;
    while (cache->count > PERCPU_FLUSH_KEEP) {
        spill[n++] = cache->frames[--cache->count];
    }
    spill[n++] = paddr;
    stat_spilled_frees[this_cid()] += (uint32_t)n;
    spin_unlock_irqrestore(&cache->lock, eflags);

    uint32_t vflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(vflags));
    spin_lock(&vmm_lock);
    for (int i = 0; i < n; i++) buddy_free_locked(spill[i]);
    spin_unlock(&vmm_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(vflags));
}

// Boot-time self-test for the frame allocator.
//
// The failure this guards against is silent: if the buddy bookkeeping is
// wrong, the same frame can be handed to two owners and the corruption only
// surfaces later as a random kernel crash. Counting "did we allocate
// something" proves nothing, so the test checks the property that actually
// rules double-ownership out:
//
//   The free lists are a PARTITION of exactly the frames the bitmap calls
//   free — every free frame is on exactly one list, at the head of a block
//   aligned to that list's order and carrying that order, no frame appears
//   twice, and no free-listed frame overlaps a live/cached/reserved one.
//
// A wrong merge, an unlinked frame, or a frame left in a cache AND on a free
// list all break that partition, so one scan catches all three. A second
// scan after the stress below verifies it still holds once the allocator has
// split, merged, parked and spilled real traffic.
//
// Runs before paging_init, so every frame address is a direct physical
// access; nothing here dereferences a frame's contents.
#define SELFTEST_CHURN 128

// Scratch for the partition scan: bit set = frame seen on a free list.
// BSS, not stack — 16KB would not fit an early-boot frame.
static uint8_t selftest_seen[PHYS_MAX_PAGES / 8];

static void selftest_fail(const char* what) {
    write_serial_string("[PHYS] selftest FAIL: ");
    write_serial_string(what);
    write_serial_string("\n");
}

// Walk every free list and confirm it partitions the bitmap's free set.
// Returns NULL when the invariant holds, else a description of what broke.
// Caller must hold vmm_lock (or be single-threaded, as at boot).
static const char* selftest_check_partition(void) {
    memset(selftest_seen, 0, sizeof(selftest_seen));
    uint32_t scanned = 0;

    for (int order = 0; order <= BUDDY_MAX_ORDER; order++) {
        for (buddy_node_t* n = buddy_free_lists[order]; n != NULL; n = n->next) {
            // A corrupted list can point at itself; cap the walk so a bug
            // reports instead of hanging the boot.
            if (++scanned > phys_total_pages) return "free list too long";

            uint32_t head = (uint32_t)((uintptr_t)n / 4096);
            if (head < phys_reserved_pages || head >= phys_total_pages)
                return "block head outside RAM";
            if ((uint32_t)frame_order[head] != (uint32_t)order)
                return "block head has wrong order";
            if (!is_aligned_to_order(head, order))
                return "block head not aligned to its order";

            for (uint32_t k = 0; k < (1u << order); k++) {
                uint32_t f = head + k;
                if (f >= phys_total_pages) return "block runs past RAM";
                if (bitmap_test(f)) return "free-listed frame marked used";
                if ((selftest_seen[f / 8] >> (f % 8)) & 1)
                    return "frame is on two free lists";
                selftest_seen[f / 8] |= (uint8_t)(1 << (f % 8));
            }
        }
    }

    // Anything the bitmap calls free must have been found on a list.
    for (uint32_t f = phys_reserved_pages; f < phys_total_pages; f++) {
        int on_list = (selftest_seen[f / 8] >> (f % 8)) & 1;
        if (!on_list && !bitmap_test(f)) return "free frame on no list";
    }
    return NULL;
}

// Push every parked frame back into the buddy free lists, so the used-frame
// count can be compared exactly instead of within a cache's worth of slack.
static void selftest_flush_caches(void) {
    for (int c = 0; c < MAX_CORES; c++) {
        percpu_frame_cache_t* cache = &percpu_caches[c];
        uint32_t frames[PERCPU_CACHE_SIZE];
        uint32_t eflags = spin_lock_irqsave(&cache->lock);
        int n = cache->count;
        for (int i = 0; i < n; i++) frames[i] = cache->frames[i];
        cache->count = 0;
        spin_unlock_irqrestore(&cache->lock, eflags);
        if (n == 0) continue;

        // Counted as spilled so the stats ledger stays complete: these
        // frames left a cache for the free lists, exactly what that counter
        // means. Without this, the boot test would silently unbalance the
        // counters and every later reconciliation would be off by a constant.
        stat_spilled_frees[this_cid()] += (uint32_t)n;

        uint32_t vflags;
        __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(vflags));
        spin_lock(&vmm_lock);
        for (int i = 0; i < n; i++) buddy_free_locked(frames[i]);
        spin_unlock(&vmm_lock);
        __asm__ __volatile__("push %0; popfl" : : "r"(vflags));
    }
}

static void phys_allocator_self_test(void) {
    // --- 1. Fast path: distinct frames, free then reuse ----------------
    uint32_t a = frame_alloc();
    uint32_t b = frame_alloc();
    if (a == 0 || b == 0 || a == b) {
        selftest_fail("alloc returned bad frames");
        return;
    }
    frame_free(b);
    uint32_t c = frame_alloc();
    if (c == a) {
        selftest_fail("handed out a live frame");
        return;
    }
    frame_free(a);
    frame_free(c);

    // --- 2. Over-free must be inert, not corrupting --------------------
    // Freeing a frame nobody owns used to be a no-op; it must stay one, or
    // the frame ends up parked twice and goes to two callers.
    //
    // Note on the accounting being asserted here: a parked frame is already
    // counted as used (it was counted when the refill pulled it off the free
    // list), so parking, and the next hand-out from the cache, move a frame
    // between "live" and "parked" without moving phys_used_count at all. The
    // count only changes when a frame crosses the free-list boundary. So the
    // over-free must leave it exactly where it was.
    uint32_t used_before_probe = phys_get_used_pages();
    uint32_t d = frame_alloc();
    if (d == 0) {
        selftest_fail("alloc failed before over-free probe");
        return;
    }
    frame_free(d);                 // parked: count unchanged
    uint32_t overfree_before = stat_overfree;
    frame_free(d);                 // second free: must do nothing
    uint32_t overfree_after = stat_overfree;
    if (overfree_after != overfree_before + 1) {
        selftest_fail("double free was not detected");
        return;
    }
    if (phys_get_used_pages() != used_before_probe) {
        selftest_fail("double free moved the used count");
        return;
    }
    // If the over-free had parked d a second time, the cache would hold two
    // copies of it and these two pops would both return d. (One of them
    // legitimately IS d: the cache is LIFO and d was just parked.)
    uint32_t e = frame_alloc();
    uint32_t f = frame_alloc();
    if (e == 0 || f == 0 || e == f) {
        selftest_fail("over-free put a frame into circulation twice");
        return;
    }
    frame_free(e);
    frame_free(f);

    // --- 3. Churn: no frame may ever be handed out twice ---------------
    // Flush first so the baseline below has an empty cache and the final
    // comparison can be exact rather than within one cache's worth of slack.
    selftest_flush_caches();
    uint32_t used_before = phys_get_used_pages();

    static uint32_t batch[SELFTEST_CHURN];
    for (int i = 0; i < SELFTEST_CHURN; i++) {
        batch[i] = frame_alloc();
        if (batch[i] == 0) {
            selftest_fail("churn alloc failed");
            return;
        }
        for (int j = 0; j < i; j++) {
            if (batch[i] == batch[j]) {
                selftest_fail("duplicate frame handed out");
                return;
            }
        }
    }

    // Partition must hold with 128 frames live.
    const char* bad = selftest_check_partition();
    if (bad) {
        selftest_fail(bad);
        return;
    }

    for (int i = 0; i < SELFTEST_CHURN; i++) frame_free(batch[i]);

    // Every frame is back on a free list once the caches are flushed, so the
    // used count must return EXACTLY to the post-flush baseline. A leak of a
    // single frame shows up now; the old check allowed a whole cache.
    selftest_flush_caches();
    uint32_t used_after = phys_get_used_pages();
    if (used_after != used_before) {
        write_serial_string("[PHYS] selftest FAIL: frames leaked, used ");
        write_serial_hex(used_before);
        write_serial_string(" -> ");
        write_serial_hex(used_after);
        write_serial_string("\n");
        return;
    }

    bad = selftest_check_partition();
    if (bad) {
        selftest_fail(bad);
        return;
    }

    // --- 4. Buddy layer: coalescing must produce real high-order blocks --
    // Driven directly, so the frees reach the free lists instead of being
    // absorbed by a cache. Allocate an aligned run, free it, and require
    // that a block of at least the run's order reappears: the merge guard
    // is what makes that possible, and nothing weaker tests it.
    const int N = 4;                        // 16 frames = 64KB
    uint32_t run[1 << N];
    uint32_t vflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(vflags));
    spin_lock(&vmm_lock);

    uint32_t coalesces_before = stat_coalesces;
    int got = 0;
    for (int i = 0; i < (1 << N); i++) {
        run[i] = buddy_alloc_locked();
        if (run[i] == 0) break;
        got++;
    }
    for (int i = 0; i < got; i++) buddy_free_locked(run[i]);
    uint32_t coalesces_delta = stat_coalesces - coalesces_before;
    uint32_t still_serves = buddy_alloc_locked();
    if (still_serves) buddy_free_locked(still_serves);

    // Did a high-order block actually come back? Only possible via merging.
    int max_order = -1;
    for (int o = BUDDY_MAX_ORDER; o >= 0; o--) {
        if (buddy_free_lists[o]) { max_order = o; break; }
    }
    const char* bad2 = selftest_check_partition();
    spin_unlock(&vmm_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(vflags));

    if (got < (1 << N)) {
        selftest_fail("buddy alloc exhausted during run test");
        return;
    }
    if (still_serves == 0) {
        selftest_fail("buddy empty after freeing the run");
        return;
    }
    if (bad2) {
        selftest_fail(bad2);
        return;
    }

    frame_alloc_stats_t st;
    phys_get_stats(&st);
    if (max_order < 1) {
        // The run need not land aligned, so this is not proof of a bug --
        // but it does mean this boot exercised no merge at all, and saying
        // "PASS" would overstate what was checked.
        write_serial_string("[PHYS] selftest PASS, coalesce unproven\n");
        return;
    }
    write_serial_string("[PHYS] selftest PASS (partition ok, max order ");
    write_serial_hex((uint32_t)max_order);
    write_serial_string(", ");
    write_serial_hex(coalesces_delta);
    write_serial_string(" merges, ");
    write_serial_hex(st.overfree);
    write_serial_string(" over-free caught)\n");
}

uint32_t frame_alloc(void) {
    // Fast path: try per-CPU cache first
    uint32_t paddr = percpu_cache_alloc();
    if (paddr != 0) return paddr;

    // Slow path: allocate directly from buddy (cache was empty and refill failed)
    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&vmm_lock);

    paddr = buddy_alloc_locked();
    if (paddr != 0) {
        stat_buddy_allocs[this_cid()]++;
        // This frame bypasses the cache, so it is handed straight to the
        // caller: live from here (buddy_alloc_locked already counted it).
    } else {
        // The cache refill already probed the free lists and came up empty,
        // so this second probe failing means the system is genuinely out of
        // frames. Counted here, once per failed frame_alloc() call, rather
        // than inside buddy_alloc_locked where the two probes would each
        // bump it.
        stat_oom++;
        write_serial_string("[VMM] OUT OF PHYSICAL FRAMES!\n");
    }

    spin_unlock(&vmm_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));

    return paddr;
}

// Return a released frame to the free lists and merge it as far up as its
// buddies allow. Callers: percpu_cache_free's spill path (the cache was
// full), percpu_cache_refill's surplus path (a refill overshot), and
// selftest_flush_caches. Caller must hold vmm_lock.
static void buddy_free_locked(uint32_t paddr) {
    uint32_t idx = paddr / 4096;

    // Bounds check
    if (idx < phys_reserved_pages || idx >= phys_total_pages) {
        return;
    }

    // Only reached on frames the caller has already disowned: frame_free()
    // dropped the last reference (leaving refcount 1 while the frame sat in
    // a cache), or a refill surplus is being handed straight back. So this
    // is the point where the frame stops counting as used and re-enters the
    // free lists. Refuse anything that is not actually being released.
    if (frame_ref_count[idx] >= FRAME_PINNED) return;

    frame_ref_count[idx] = 0;
    if (phys_used_count > 0) phys_used_count--;
    bitmap_clear(idx);   // back on a free list from here

    // Start coalescing at order 0
    int order = 0;
    uint32_t current_idx = idx;

    // Eagerly coalesce with buddies
    while (order < BUDDY_MAX_ORDER) {
        uint32_t buddy_idx = buddy_address(current_idx, order);

        // Merge only if the buddy really is a free block of this order.
        // `frame_order[buddy_idx] != order` is the load-bearing test: a
        // block head carries its own order, so anything else — including
        // BUDDY_ALLOCATED (0xFF) and BUDDY_CACHED (0xFE), neither of which
        // is a valid order — means "not a free block of this order".
        if (buddy_idx < phys_reserved_pages ||
            buddy_idx >= phys_total_pages ||
            frame_order[buddy_idx] != order ||
            !is_aligned_to_order(current_idx < buddy_idx ? current_idx : buddy_idx, order + 1)) {
            break;
        }

        // Remove buddy from its free list
        buddy_list_remove(buddy_idx, order);

        // Move to lower-addressed block for next iteration
        if (buddy_idx < current_idx) {
            current_idx = buddy_idx;
        }

        order++;
    }

    // Add the (possibly coalesced) block to free list
    buddy_list_add(current_idx, order);
    stat_coalesces += (uint32_t)order;   // merges performed on the way up
}

// Snapshot the allocator counters for /proc/meminfo. Per-CPU slots are
// summed here; a torn read of another core's counter can only ever make
// the reported total one operation stale, which is fine for stats.
void phys_get_stats(frame_alloc_stats_t* out) {
    if (!out) return;
    out->cache_hits = 0;
    out->buddy_allocs = 0;
    out->refill_frames = 0;
    out->cached_frees = 0;
    out->spilled_frees = 0;
    out->coalesces = 0;
    out->oom = 0;
    out->overfree = 0;
    out->cache_frames = 0;

    for (int i = 0; i < MAX_CORES; i++) {
        out->cache_hits += stat_cache_hits[i];
        out->buddy_allocs += stat_buddy_allocs[i];
        out->refill_frames += stat_refill_frames[i];
        out->cached_frees += stat_cached_frees[i];
        out->spilled_frees += stat_spilled_frees[i];
        out->cache_frames += (uint32_t)percpu_caches[i].count;
    }

    // Under vmm_lock: a coherent view of the counters the buddy core owns.
    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&vmm_lock);
    out->coalesces = stat_coalesces;
    out->oom = stat_oom;
    out->overfree = stat_overfree;
    spin_unlock(&vmm_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
}

// Release one reference to a frame. Everything that decides whether the
// frame actually leaves a caller's hands happens under vmm_lock, and the
// decision to release is recorded (BUDDY_CACHED) before the lock is dropped:
// that is what makes a second free of the same frame a no-op on this core,
// and what serializes two cores racing to free the last reference.
void frame_free(uint32_t paddr) {
    if (paddr == 0) return;
    if (paddr == phys_zero_page) return;  // pinned shared zero page

    // Handle refcount decrement under lock (COW, SysV shm)
    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&vmm_lock);

    uint32_t idx = paddr / 4096;

    // Bounds check
    if (idx < phys_reserved_pages || idx >= phys_total_pages) {
        spin_unlock(&vmm_lock);
        __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
        return;
    }

    // Nobody owns this frame: it is already parked in a cache, or its
    // refcount already reached zero. Freeing it again would park the same
    // frame twice and hand it to two callers. Counted, so a real over-free
    // shows up in /proc/meminfo instead of corrupting silently.
    if (frame_order[idx] == BUDDY_CACHED || frame_ref_count[idx] == 0) {
        stat_overfree++;
        spin_unlock(&vmm_lock);
        __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
        return;
    }

    // Pinned: the zero page and any phys_reserve_region() range (framebuffer,
    // MMIO). Releasing one would put device memory into a cache and hand it
    // to a process as ordinary writable memory.
    if (frame_ref_count[idx] >= FRAME_PINNED) {
        spin_unlock(&vmm_lock);
        __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
        return;
    }

    // Still shared (COW copy, SysV shm attach, fork clone): drop one
    // reference and leave the frame in place.
    if (frame_ref_count[idx] > 1) {
        frame_ref_count[idx]--;
        spin_unlock(&vmm_lock);
        __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
        return;
    }

    // Last reference. Claim the frame here, under the lock, by marking it
    // parked — after this point no other free() can decide the same thing
    // about it. The refcount stays 1 and phys_used_count is untouched: a
    // cached frame still counts as used until it is actually spilled back to
    // the free lists (buddy_free_locked), so the bookkeeping needs no lock
    // on the cache's fast paths.
    frame_order[idx] = BUDDY_CACHED;
    spin_unlock(&vmm_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));

    percpu_cache_free(paddr);
}


// ============================================================
// Page directory management
// ============================================================
// A page directory is 1024 entries (4096 bytes).
// Entry format: [31:12] = page table phys addr | [11:0] = flags
//
// For per-process address spaces, we create a new page directory
// that identity-maps the kernel region + framebuffer,
// then maps process-private pages at high addresses.

// The kernel identity base in every address space:
// Identity map 0 → 16MB (kernel code, data, heap)
// Also map framebuffer (we don't know the addr here, so caller must add it)

// Zero a page (helper since we don't have a generic zero-phys-page)
static void zero_phys_page(uint32_t paddr) {
    // The kernel identity map covers 0..RAM, so every allocatable frame is
    // directly addressable. Anything outside that range is a caller bug.
    if (paddr < (phys_total_pages * 4096)) {
        memset((void*)(uintptr_t)paddr, 0, 4096);
    } else {
        write_serial_string("[VMM] zero_phys_page: paddr out of range!\n");
    }
}

// ---- PAE walk helpers (v38.49) ----
// An address space is a PDPT frame (page_dir). `space_pt` resolves the PT
// for vaddr, optionally creating the PD/PT frames on the way. Parent PD/PTE
// entries are widened to PAGE_USER when a user page is being mapped (the
// CPU checks U/S at every level EXCEPT the PDPTE level — see below).
// Returns the PT's kernel VA (identity) or 0.
//
// PDPTE flag rule (KVM boot fix): PAE PDPTEs may carry ONLY the P bit.
// R/W and U/S are architecturally "ignored" there, but this host's KVM
// treats them as reserved bits and raises #GP(0) on every `mov cr3` into
// such a space — which is exactly what app launch does right after
// "switching cr3". Permissions live in the PD/PTE levels below.
static pte_t* space_pd(pte_t* pdpt, uint32_t vaddr, int create) {
    uint32_t pi = pdpt_index(vaddr);
    if (!(pdpt[pi] & PAGE_PRESENT)) {
        if (!create) return 0;
        uint32_t pd_frame = frame_alloc();
        if (pd_frame == 0) return 0;
        zero_phys_page(pd_frame);
        pdpt[pi] = (uint64_t)pd_frame | PAGE_PRESENT;
    }
    return (pte_t*)(uintptr_t)(uint32_t)(pdpt[pi] & PTE_ADDR_MASK);
}

static pte_t* space_pt(uint32_t pdpt_paddr, uint32_t vaddr, int create, int user) {
    pte_t* pdpt = (pte_t*)(uintptr_t)pdpt_paddr;
    pte_t* pd = space_pd(pdpt, vaddr, create);
    if (!pd) return 0;
    uint32_t di = pd_index(vaddr);
    if (!(pd[di] & PAGE_PRESENT)) {
        if (!create) return 0;
        uint32_t pt_frame = frame_alloc();
        if (pt_frame == 0) return 0;
        zero_phys_page(pt_frame);
        pd[di] = (uint64_t)pt_frame | PAGE_PRESENT | PAGE_RW;
    }
    if (user) {
        // The CPU requires PAGE_USER at every level from the PD down (the
        // PDPTE level ignores flags entirely). Identity-mapped PTEs inside a
        // cloned kernel PT still lack PAGE_USER individually, so widening
        // the PD keeps kernel memory protected.
        pd[di] |= PAGE_USER;
    }
    return (pte_t*)(uintptr_t)(uint32_t)(pd[di] & PTE_ADDR_MASK);
}

uint32_t vmm_create_address_space(void) {
    if (!vmm_initialized) vmm_init();

    // PDPT frame (only the first 32 bytes are meaningful; the frame keeps
    // the rest as scratch, PAE CR3 is 4K-aligned).
    uint32_t pdpt_paddr = frame_alloc();
    if (pdpt_paddr == 0) return 0;
    zero_phys_page(pdpt_paddr);
    pte_t* pdpt = (pte_t*)(uintptr_t)pdpt_paddr;

    // Clone every KERNEL page table from the boot structures into private
    // copies (identity map + APIC MMIO + framebuffer PTs). Sharing the
    // tables themselves would let user mappings corrupt the kernel's view;
    // cloning gives every space its own writable copy. Content is identical,
    // so fork() can later COW only the USER ptes inside these regions
    // (see vmm_clone_address_space).
    for (uint32_t pi = 0; pi < PDPT_ENTRIES; pi++) {
        if (!(boot_pdpt[pi] & PAGE_PRESENT)) continue;
        uint32_t pd_frame = frame_alloc();
        if (pd_frame == 0) {
            write_serial_string("[VMM] CLONE PD FAIL - out of frames!\n");
            vmm_free_address_space(pdpt_paddr);
            return 0;
        }
        zero_phys_page(pd_frame);
        pte_t* pd = (pte_t*)(uintptr_t)pd_frame;
        for (uint32_t j = 0; j < PT_ENTRIES; j++) {
            if (!(boot_pds[pi][j] & PAGE_PRESENT)) continue;
            uint32_t pt_frame = frame_alloc();
            if (pt_frame == 0) {
                write_serial_string("[VMM] CLONE PT FAIL - out of frames!\n");
                vmm_free_address_space(pdpt_paddr);
                return 0;
            }
            memcpy((void*)(uintptr_t)pt_frame,
                   (void*)(uintptr_t)(uint32_t)(boot_pds[pi][j] & PTE_ADDR_MASK), 4096);
            pd[j] = (uint64_t)pt_frame | (boot_pds[pi][j] & 0xFFF);
        }
        pdpt[pi] = (uint64_t)pd_frame | (boot_pdpt[pi] & 0xFFF);
    }

    // Map the signal trampoline page (user-readable, read-only) at the fixed
    // SIG_TRAMPOLINE_VA: signal handlers `ret` into it and it issues
    // SYS_SIGRETURN. If the frame allocation fails the address space still
    // works — default-action signals (which don't need the trampoline) are
    // unaffected. Kept in sync with SYS_SIGRETURN (75) in syscall.h.
    {
        uint32_t tp = frame_alloc();
        if (tp) {
            uint8_t* code = (uint8_t*)(uintptr_t)tp;
            code[0] = 0xB8; code[1] = 75; code[2] = 0; code[3] = 0; code[4] = 0; // mov eax, SYS_SIGRETURN
            code[5] = 0xCD; code[6] = 0x80;                                     // int $0x80
            code[7] = 0xC3;                                                     // ret (safety)
            vmm_map_page(pdpt_paddr, SIG_TRAMPOLINE_VA, tp, PAGE_PRESENT | PAGE_USER);
        }
    }

    write_serial_string("[VMM] Created address space (cloned PTs) OK\n");
    return pdpt_paddr;
}

void vmm_free_address_space(uint32_t page_dir) {
    if (page_dir == 0) return;  // Can't free global identity

    pte_t* pdpt = (pte_t*)(uintptr_t)page_dir;

    for (uint32_t pi = 0; pi < PDPT_ENTRIES; pi++) {
        if (!(pdpt[pi] & PAGE_PRESENT)) continue;
        pte_t* pd = (pte_t*)(uintptr_t)(uint32_t)(pdpt[pi] & PTE_ADDR_MASK);

        for (uint32_t j = 0; j < PT_ENTRIES; j++) {
            if (!(pd[j] & PAGE_PRESENT)) continue;
            pte_t* pt = (pte_t*)(uintptr_t)(uint32_t)(pd[j] & PTE_ADDR_MASK);

            if (boot_pds[pi][j] & PAGE_PRESENT) {
                // A CLONED kernel page table: free only PTEs the user
                // MODIFIED (differ from the boot table). Identity-mapped
                // frames must stay.
                pte_t* boot_pt =
                    (pte_t*)(uintptr_t)(uint32_t)(boot_pds[pi][j] & PTE_ADDR_MASK);
                for (uint32_t e = 0; e < PT_ENTRIES; e++) {
                    if ((pt[e] & PAGE_PRESENT) &&
                        !(pt[e] & PAGE_DEV) &&
                        (pt[e] & 0x000FFFFFFFFFF007ULL) != (boot_pt[e] & 0x000FFFFFFFFFF007ULL)) {
                        uint32_t page_paddr = (uint32_t)(pt[e] & PTE_ADDR_MASK);
                        if (page_paddr >= (phys_reserved_pages * 4096)) {
                            frame_free(page_paddr);
                        }
                    }
                }
            } else {
                // Entirely user-created: free ALL present page frames.
                for (uint32_t e = 0; e < PT_ENTRIES; e++) {
                    if ((pt[e] & PAGE_PRESENT) && !(pt[e] & PAGE_DEV)) {
                        uint32_t page_paddr = (uint32_t)(pt[e] & PTE_ADDR_MASK);
                        if (page_paddr >= (phys_reserved_pages * 4096)) {
                            frame_free(page_paddr);
                        }
                    }
                }
            }
            frame_free((uint32_t)(pd[j] & PTE_ADDR_MASK));   // the PT frame
        }
        frame_free((uint32_t)(pdpt[pi] & PTE_ADDR_MASK));    // the PD frame
    }

    frame_free(page_dir);   // the PDPT frame
    write_serial_string("[VMM] Freed address space\n");
}

int vmm_map_page(uint32_t page_dir, uint32_t vaddr, uint32_t paddr, uint64_t flags) {
    if (page_dir == 0) {
        // Use global mapping via page_map
        page_map(vaddr, paddr, (uint32_t)(flags & 0xFFF));
        return 0;
    }

    pte_t* pt = space_pt(page_dir, vaddr, 1, (flags & PAGE_USER) != 0);
    if (!pt) return -1;
    // Keep every flag INCLUDING the high ones (PAGE_NX lives at bit 63);
    // mask out only the address field and the PRESENT bit we re-add.
    pt[pt_index(vaddr)] = ((uint64_t)paddr & PTE_ADDR_MASK)
                          | (flags & ~PTE_ADDR_MASK & ~PAGE_PRESENT) | PAGE_PRESENT;
    return 0;
}

// Map a fresh Ring 3 stack into page_dir, ending at stack_top. Returns the
// initial ESP (stack_top), or 0.
//
// stack_top is per-task (thread_create picks a unique top below USER_STACK_TOP
// for every task slot), so sibling threads in one process never stomp each
// other's stacks like the old fixed USER_STACK_BOTTOM..USER_STACK_TOP mapping
// did. The page below the mapped range stays unmapped as a guard page.
//
// On partial failure the frames already mapped are left in place; every caller
// abandons the whole address space on failure, and vmm_free_address_space()
// reclaims them.
uint32_t vmm_setup_user_stack(uint32_t page_dir, uint32_t stack_top) {
    if (page_dir == 0) return 0;  // a Ring 3 task needs a private address space

    // Demand-paged: no frames are mapped eagerly. The #PF handler lazily
    // backs each page on first access (write faults get a private zeroed
    // frame, read faults the shared zero page), so a task that uses a few
    // hundred bytes of stack costs a few hundred bytes, not 16 frames. The
    // page below the task's stack range stays permanently unmapped as a guard
    // page — an overflow faults there and kills the task instead of silently
    // corrupting whatever sits underneath.
    return stack_top;
}

int vmm_unmap_page(uint32_t page_dir, uint32_t vaddr) {
    if (page_dir == 0) {
        // Use the CURRENT CR3 (a PDPT) to find the page table.
        uint32_t cr3_val;
        __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3_val));
        pte_t* cur_pdpt = (pte_t*)(uintptr_t)cr3_val;
        pte_t* pd = space_pd(cur_pdpt, vaddr, 0);
        if (pd && (pd[pd_index(vaddr)] & PAGE_PRESENT) &&
            pdpt_index(vaddr) == 0 && pd_index(vaddr) < mem_identity_tables()) {
            pte_t* pt = (pte_t*)(uintptr_t)(uint32_t)(pd[pd_index(vaddr)] & PTE_ADDR_MASK);
            pt[pt_index(vaddr)] = 0;
        }
        __asm__ __volatile__("invlpg (%0)" : : "r"(vaddr));
        return 0;
    }

    pte_t* pt = space_pt(page_dir, vaddr, 0, 0);
    if (!pt || !(pt[pt_index(vaddr)] & PAGE_PRESENT)) return -1;
    pt[pt_index(vaddr)] = 0;

    // TLB flush
    __asm__ __volatile__("invlpg (%0)" : : "r"(vaddr));
    return 0;
}

uint32_t vmm_unmap_page_free(uint32_t page_dir, uint32_t vaddr) {
    if (page_dir == 0) return 0;   // boot identity map: not owned here
    pte_t* pt = space_pt(page_dir, vaddr, 0, 0);
    if (!pt) return 0;
    uint32_t idx = pt_index(vaddr);
    uint64_t pte = pt[idx];
    if (!(pte & PAGE_PRESENT)) return 0;
    uint32_t paddr = (uint32_t)(pte & PTE_ADDR_MASK);
    pt[idx] = 0;
    __asm__ __volatile__("invlpg (%0)" : : "r"(vaddr));
    return paddr;
}

uint32_t vmm_alloc_page_at(uint32_t page_dir, uint32_t vaddr, uint64_t flags) {
    uint32_t paddr = frame_alloc();
    if (paddr == 0) {
        write_serial_string("[VMM] alloc_page_at: NO FRAME for vaddr=");
        write_serial_hex(vaddr);
        write_serial('\n');
        return 0;
    }

    if (vmm_map_page(page_dir, vaddr, paddr, flags) != 0) {
        frame_free(paddr);
        write_serial_string("[VMM] alloc_page_at: MAP FAILED\n");
        return 0;
    }
    return vaddr;
}

uint32_t vmm_clone_address_space(uint32_t src_page_dir) {
    if (src_page_dir == 0) {
        return vmm_create_address_space();
    }

    // Fresh space with kernel page tables already cloned by create().
    uint32_t dst_pdpt_paddr = vmm_create_address_space();
    if (dst_pdpt_paddr == 0) return 0;

    pte_t* src_pdpt = (pte_t*)(uintptr_t)src_page_dir;
    pte_t* dst_pdpt = (pte_t*)(uintptr_t)dst_pdpt_paddr;

    for (uint32_t pi = 0; pi < PDPT_ENTRIES; pi++) {
        if (!(src_pdpt[pi] & PAGE_PRESENT)) continue;
        pte_t* src_pd = (pte_t*)(uintptr_t)(uint32_t)(src_pdpt[pi] & PTE_ADDR_MASK);
        pte_t* dst_pd = (pte_t*)(uintptr_t)(uint32_t)(dst_pdpt[pi] & PTE_ADDR_MASK);

        for (uint32_t j = 0; j < PT_ENTRIES; j++) {
            if (!(src_pd[j] & PAGE_PRESENT)) continue;

            if (boot_pds[pi][j] & PAGE_PRESENT) {
                // Kernel-region page table: dst already has its own clone
                // with the kernel PTEs identical. Only the USER ptes inside
                // need COW copying. (The old code re-allocated a full PT
                // here and overwrote dst's clone — leaking a frame per
                // kernel PT per fork.)
                pte_t* src_pt = (pte_t*)(uintptr_t)(uint32_t)(src_pd[j] & PTE_ADDR_MASK);
                pte_t* dst_pt = (pte_t*)(uintptr_t)(uint32_t)(dst_pd[j] & PTE_ADDR_MASK);
                // vmm_lock (v38.55): this walk both COW-marks SOURCE ptes and
                // bumps frame_ref_count — the same state a COW fault on
                // another core mutates under vmm_lock. Unlocked, a faulting
                // peer could promote a page between our read and our
                // RO+COW store, or bump a refcount that frame_free was about
                // to drop to zero. No frame_alloc inside, so the whole walk
                // is one short critical section.
                vmm_lock_acquire_irq();
                for (uint32_t e = 0; e < PT_ENTRIES; e++) {
                    pte_t spe = src_pt[e];
                    if (!(spe & PAGE_PRESENT) || !(spe & PAGE_USER)) continue;
                    // Device/PFN mappings (PAGE_DEV, e.g. the Ring 3
                    // framebuffer): the frame is MMIO outside the RAM
                    // allocator — never COW it and never refcount it (the
                    // old unbounded refcount bump would write out of the
                    // bitmap for a PA above phys_total_pages). The child
                    // does not inherit the mapping.
                    if (spe & PAGE_DEV) continue;
                    if ((spe & PAGE_RW) && !(spe & PAGE_SHARED)) {
                        src_pt[e] = (spe & ~PAGE_RW) | PAGE_COW;   // COW the source
                        spe = src_pt[e];
                    }
                    dst_pt[e] = spe;
                    uint32_t page_paddr = (uint32_t)(spe & PTE_ADDR_MASK);
                    if (page_paddr >= (phys_reserved_pages * 4096)) {
                        if (frame_ref_count[page_paddr / 4096] < 255)
                            frame_ref_count[page_paddr / 4096]++;
                        // At 255 the frame is pinned — never freed. Acceptable.
                    }
                }
                vmm_lock_release_irq();
                // Propagate any USER widening the source had on the PD.
                // (PDPTEs carry no flags — see the PDPTE flag rule above.)
                dst_pd[j] |= (src_pd[j] & PAGE_USER);
            } else {
                // Purely user region: allocate a fresh PT for dst and COW
                // every present entry.
                uint32_t dst_pt_paddr = frame_alloc();
                if (dst_pt_paddr == 0) {
                    vmm_free_address_space(dst_pdpt_paddr);
                    return 0;
                }
                zero_phys_page(dst_pt_paddr);

                pte_t* src_pt = (pte_t*)(uintptr_t)(uint32_t)(src_pd[j] & PTE_ADDR_MASK);
                pte_t* dst_pt = (pte_t*)(uintptr_t)dst_pt_paddr;

                // Same vmm_lock discipline as the kernel-region walk above;
                // frame_alloc/zero_phys_page deliberately run outside it.
                vmm_lock_acquire_irq();
                for (uint32_t e = 0; e < PT_ENTRIES; e++) {
                    pte_t spe = src_pt[e];
                    if (!(spe & PAGE_PRESENT)) continue;
                    if (spe & PAGE_DEV) continue;  // device mapping: not inherited, never refcounted
                    if ((spe & PAGE_USER) && (spe & PAGE_RW) && !(spe & PAGE_SHARED)) {
                        src_pt[e] = (spe & ~PAGE_RW) | PAGE_COW;   // COW the source
                        spe = src_pt[e];
                    }
                    dst_pt[e] = spe;
                    uint32_t page_paddr = (uint32_t)(spe & PTE_ADDR_MASK);
                    if (page_paddr >= (phys_reserved_pages * 4096)) {
                        if (frame_ref_count[page_paddr / 4096] < 255)
                            frame_ref_count[page_paddr / 4096]++;
                        // At 255 the frame is pinned — never freed. Acceptable.
                    }
                }
                vmm_lock_release_irq();
                dst_pd[j] = (uint64_t)dst_pt_paddr | (src_pd[j] & 0xFFF);
            }
        }
    }

    // Flush TLB of the current active directory just in case it was src_page_dir
    uint32_t active_cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(active_cr3));
    if (active_cr3 == src_page_dir) {
        __asm__ __volatile__("mov %0, %%cr3" : : "r"(src_page_dir));
    }
    // NOTE (v38.55, closed v38.66): the COW-marking above can race a SIBLING
    // thread running this address space on another core — its TLB may keep a
    // stale WRITABLE entry for a page we just marked read-only, letting it
    // bypass the COW fault until its next CR3 reload. schedule() reloads CR3
    // every timer tick, so that exposure never exceeds ~1 tick; v38.66's TLB
    // shootdown closes it immediately. The flush cannot run here: fork_common
    // calls this function while holding task_lock with IF=0, and the shootdown
    // waits on peer acks (a peer spinning IF=0 on task_lock could not answer).
    // fork_common therefore detects the sibling-on-peer-core case under
    // task_lock and fires vmm_tlb_shootdown() right after it drops the lock.

    write_serial_string("[VMM] Cloned COW address space successfully\n");
    return dst_pdpt_paddr;
}

void vmm_switch_page_dir(uint32_t page_dir) {
    if (page_dir != 0) {
        __asm__ __volatile__("mov %0, %%cr3" : : "r"(page_dir));
    }
}

// ============================================================
// TLB shootdown IPI (v38.66)
// ============================================================
// The gap this closes (documented at v38.55 in vmm_clone_address_space):
// when one core mutates an address space that a SIBLING thread runs on
// another core (COW-marking at fork, unmap/munmap of shared page tables),
// the peer's TLB can keep serving the OLD mapping until its next CR3 reload.
// schedule() reloads CR3 on every timer tick (<= 1 ms), so the stale window
// is bounded; the shootdown makes the flush immediate at the mutation.
//
// Protocol (all state is plain volatile — senders are serialized by
// tlb_sd_lock, so exactly one shootdown is in flight system-wide):
//   1. Sender bumps tlb_sd_gen, records tlb_sd_target_pd, then sends a
//      directed fixed IPI on TLB_SD_VECTOR to every OTHER real core.
//   2. Each peer's handler (irq_common_stub -> irq_handler, so EOI is sent
//      before dispatch) records tlb_sd_seen[cid] = gen and — only if it is
//      CURRENTLY RUNNING the target page_dir — reloads its own CR3 (a full
//      TLB flush; under PAE without PCID, entries for any non-current CR3
//      cannot exist, so the conditional is exact, not a heuristic).
//   3. Sender waits until every target's seen >= gen (bounded).
//
// Locking: the sender runs cli + tlb_sd_lock for the whole send+wait, the
// same cli-first discipline as frame_alloc/vmm_lock. This is deadlock-free
// because (a) only one shootdown can be in flight (single lock, and nobody
// ever waits on tlb_sd_lock while holding another lock — it is taken and
// released entirely inside this function), and (b) a peer blocked IF=0 on
// some OTHER lock finishes that section without needing tlb_sd_lock, then
// takes the pending IPI. The ack wait is bounded; if it ever trips (a peer
// genuinely hung IF=0 — which the watchdog would also catch), the sender
// logs and proceeds, degrading to the pre-shootdown flush-at-next-reload
// behavior instead of hanging.
#define TLB_MAX_CORES 16
static spinlock_t tlb_sd_lock = SPINLOCK_INIT;
static volatile uint32_t tlb_sd_gen = 0;              // bumped per shootdown
static volatile uint32_t tlb_sd_target_pd = 0;        // page_dir being flushed
static volatile uint32_t tlb_sd_seen[TLB_MAX_CORES];  // per-core highest gen acked

#define TLB_ACK_MAX_SPINS 20000000

// CI self-test recorders (boot arg `tlb_self_test`, section below): declared
// before the shootdown handler because it counts into them in test mode. All
// plain volatile stores — the handler runs in IRQ context and the BSP reads
// them only after the ack protocol proves the handler already ran.
static int tlb_test_mode = 0;
static volatile uint32_t tlb_test_recv[TLB_MAX_CORES];     // IPIs handled
static volatile uint32_t tlb_test_reload[TLB_MAX_CORES];   // CR3 reloads done

// Runs on every core (shared IDT + handler table). EOI already sent by
// irq_handler. Reads the current shootdown request and acks it; reloads CR3
// only when this core is running the target page directory (or the target is
// the shared identity space, page_dir == 0).
void vmm_tlb_sd_handler(registers_t* r) {
    (void)r;
    uint32_t cid = apic_get_id() & 15;
    if (cid >= TLB_MAX_CORES) cid = 0;

    uint32_t gen = tlb_sd_gen;
    if (tlb_sd_seen[cid] < gen) {
        tlb_sd_seen[cid] = gen;   // ack first: even a non-matching core answers
        uint32_t cr3;
        __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
        uint32_t want = tlb_sd_target_pd;
        if (want == 0 || (cr3 & 0xFFFFF000) == (want & 0xFFFFF000)) {
            // Full flush: reloading CR3 with the same value invalidates every
            // non-global TLB entry (no PCID on this target).
            __asm__ __volatile__("mov %0, %%cr3" : : "r"(cr3));
            if (tlb_test_mode && cid < TLB_MAX_CORES) tlb_test_reload[cid]++;
        }
    }
    if (tlb_test_mode && cid < TLB_MAX_CORES) tlb_test_recv[cid]++;
}

// Wait for every target core to ack `gen`. Returns 0 when all acked, 1 on
// bound trip (caller proceeds degraded).
static int tlb_wait_acks(int ntargets, const uint8_t* targets, uint32_t gen) {
    uint32_t spins = 0;
    for (;;) {
        int all = 1;
        for (int t = 0; t < ntargets; t++) {
            uint32_t cid = targets[t] & 15;
            if (cid >= TLB_MAX_CORES) cid = 0;
            if (tlb_sd_seen[cid] < gen) { all = 0; break; }
        }
        if (all) return 0;
        if (++spins >= TLB_ACK_MAX_SPINS) return 1;
        __asm__ __volatile__("pause");
    }
}

void vmm_tlb_shootdown(uint32_t page_dir) {
    extern uint32_t smp_lapic_addr;
    if (smp_lapic_addr == 0 || smp_cpu_count <= 1) return;   // UP: no peers

    uint32_t self = apic_get_id() & 15;
    uint8_t targets[TLB_MAX_CORES];
    int ntargets = 0;

    uint32_t n = smp_cpu_count;
    if (n > TLB_MAX_CORES) n = TLB_MAX_CORES;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cid = smp_lapic_ids[i] & 15;
        if (cid == self) continue;
        if (ntargets < TLB_MAX_CORES) targets[ntargets++] = (uint8_t)smp_lapic_ids[i];
    }
    if (ntargets == 0) return;

    uint32_t eflags;
    __asm__ __volatile__("pushfl; pop %0; cli" : "=r"(eflags));
    spin_lock(&tlb_sd_lock);

    uint32_t gen = ++tlb_sd_gen;
    tlb_sd_target_pd = page_dir;
    for (int t = 0; t < ntargets; t++) {
        apic_send_fixed_ipi(targets[t], TLB_SD_VECTOR);
    }

    if (tlb_wait_acks(ntargets, targets, gen) != 0) {
        write_serial_string("[VMM] TLB shootdown: ack timeout on ");
        write_serial_hex(page_dir);
        write_serial_string(" - continuing degraded (flush at next CR3 reload)\n");
    }

    spin_unlock(&tlb_sd_lock);
    __asm__ __volatile__("push %0; popfl" : : "r"(eflags));
}

// ============================================================
// CI self-test (`tlb_self_test` boot arg, v38.66)
// ============================================================
// Verifies the shootdown machinery deterministically (counter-based, immune
// to TCG timing): the BSP parks the first AP inside the TLB_TEST_VECTOR
// handler with IF=1, then fires two shootdowns at it:
//   MATCH     vmm_tlb_shootdown(boot_cr3)   — the AP IS running that space,
//             so its handler must reload CR3 (recv+1 AND reload+1).
//   NO-MATCH  vmm_tlb_shootdown(scratch pd) — no core runs it, so the
//             handler must ack but skip the reload (recv+1, reload +0).
// The ack protocol is proven by vmm_tlb_shootdown itself returning only
// after every target acked (a lost IPI would trip the bound and log).
static volatile int tlb_test_parked = 0;   // AP inside the park handler
static volatile int tlb_test_exit = 0;     // BSP asks the AP to leave
static volatile int tlb_test_exited = 0;

static int tlb_has_word(const char* cmd, const char* word) {
    if (!cmd || !word) return 0;
    int wlen = 0;
    while (word[wlen]) wlen++;
    const char* p = cmd;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        int i = 0;
        while (p[i] && p[i] != ' ' && word[i] && p[i] == word[i]) i++;
        if (!word[i] && (p[i] == ' ' || p[i] == '\0')) return 1;
        while (*p && *p != ' ') p++;
    }
    return 0;
}

void vmm_parse_cmdline(const char* cmd) {
    if (tlb_has_word(cmd, "tlb_self_test")) tlb_test_mode = 1;
}

// AP side: parks with IF=1 so the 0x61 shootdown IPI can nest in while the
// BSP runs its assertions, then leaves when asked.
void vmm_tlb_test_handler(registers_t* r) {
    (void)r;
    if (!tlb_test_mode) return;
    __asm__ __volatile__("sti");
    tlb_test_parked = 1;
    while (!tlb_test_exit) {
        __asm__ __volatile__("pause");
    }
    tlb_test_exited = 1;
}

static void tlb_test_wait_volatile(volatile int* flag, int want) {
    uint32_t spins = 0;
    while (*flag != want) {
        if (++spins >= TLB_ACK_MAX_SPINS) return;  // bound: fail open, report below
        __asm__ __volatile__("pause");
    }
}

void vmm_self_test_tick(void) {
    if (!tlb_test_mode) return;

    extern uint32_t smp_lapic_addr;
    if (smp_lapic_addr == 0 || smp_cpu_count <= 1) {
        write_serial_string("[TLB_SD] self-test skipped (SMP off)\n");
        return;
    }

    // Target: the first APIC id that is not the BSP (watchdog convention).
    uint32_t bsp = apic_get_id() & 15;
    uint8_t target = 0xFF;
    uint32_t n = smp_cpu_count;
    if (n > TLB_MAX_CORES) n = TLB_MAX_CORES;
    for (uint32_t i = 0; i < n; i++) {
        if ((smp_lapic_ids[i] & 15) != bsp) { target = smp_lapic_ids[i]; break; }
    }
    if (target == 0xFF) {
        write_serial_string("[TLB_SD] self-test skipped (no AP found)\n");
        return;
    }
    uint32_t apcid = target & 15;

    char buf[160];
    char* p = buf;
    const char* s;
    for (s = "[TLB_SD] SELF TEST on CPU "; *s; s++) *p++ = *s;
    *p++ = (char)('0' + apcid);
    for (s = " via vector "; *s; s++) *p++ = *s;
    *p++ = '0'; *p++ = 'x';
    for (int i = 28; i >= 0; i -= 4) {
        int nib = (TLB_TEST_VECTOR >> i) & 0xF;
        *p++ = (char)(nib < 10 ? '0' + nib : 'A' + (nib - 10));
    }
    for (s = "\n"; *s; s++) *p++ = *s;
    *p = '\0';
    write_serial_string(buf);

    // Park the AP. From here the AP is inside vmm_tlb_test_handler with IF=1
    // (its idle CR3 active), so the 0x61 shootdown IPI can nest in.
    apic_send_fixed_ipi(target, TLB_TEST_VECTOR);
    tlb_test_wait_volatile(&tlb_test_parked, 1);

    // MATCH: the AP currently runs the boot identity space, so its handler
    // must both ack AND reload CR3.
    extern uint32_t tasks_get_boot_cr3(void);
    vmm_tlb_shootdown(tasks_get_boot_cr3());

    // NO-MATCH: a scratch page_dir nobody runs — handler acks, skips reload.
    vmm_tlb_shootdown(0x0BADF000u);

    // Release the AP.
    tlb_test_exit = 1;
    tlb_test_wait_volatile(&tlb_test_exited, 1);

    tlb_test_mode = 0;   // one shot: stop recording before we read the counters
    uint32_t recv = (apcid < TLB_MAX_CORES) ? tlb_test_recv[apcid] : 0;
    uint32_t reload = (apcid < TLB_MAX_CORES) ? tlb_test_reload[apcid] : 0;
    int ok = (tlb_test_exited == 1) && (recv >= 2) && (reload >= 1) &&
             (recv - reload <= 1);   // exactly one non-matching shootdown skipped

    p = buf;
    for (s = ok ? "[TLB_SD] SELF TEST PASS - recv=" : "[TLB_SD] SELF TEST FAIL - recv="; *s; s++) *p++ = *s;
    {
        char t[12]; int ti = 0; uint32_t v = recv;
        if (v == 0) { *p++ = '0'; } else { while (v > 0) { t[ti++] = (char)('0' + v % 10); v /= 10; } while (ti > 0) *p++ = t[--ti]; }
    }
    for (s = " reload="; *s; s++) *p++ = *s;
    {
        char t[12]; int ti = 0; uint32_t v = reload;
        if (v == 0) { *p++ = '0'; } else { while (v > 0) { t[ti++] = (char)('0' + v % 10); v /= 10; } while (ti > 0) *p++ = t[--ti]; }
    }
    for (s = " (conditional flush verified)\n"; *s; s++) *p++ = *s;
    *p = '\0';
    write_serial_string(buf);

    // Exit cleanly for CI (panic=reboot + QEMU -no-reboot).
    extern void panic_finish(void);
    panic_finish();
}