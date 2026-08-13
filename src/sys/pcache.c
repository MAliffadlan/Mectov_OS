// pcache.c — whole-file data cache for MECTOVFS (see pcache.h)
//
// A small LRU table of (node_idx, size) -> heap buffer. Every access is
// guarded by pcache_lock (irqsave in process context; the mmap fault path
// already runs with IF=0, so cli/popfl there is a no-op). Buffers come from
// kmalloc, so heap_lock is a leaf below pcache_lock — no path takes heap_lock
// and then pcache_lock, so the ordering vfs_lock > pcache_lock > heap_lock
// (with ata_lock beneath) never deadlocks.

#include "../include/pcache.h"
#include "../include/spinlock.h"
#include "../include/mem.h"
#include "../include/timer.h"
#include "../include/utils.h"

typedef struct {
    int in_use;
    int node_idx;
    int size;
    uint32_t last_tick;  // LRU clock (touched on every lookup)
    char* data;          // kmalloc'd buffer, capacity >= size
    int cap;             // allocated capacity in bytes
} pcache_entry_t;

static pcache_entry_t pcache[PCACHE_ENTRIES];
static spinlock_t pcache_lock = SPINLOCK_INIT;

void pcache_init(void) {
    memset(pcache, 0, sizeof(pcache));
}

char* pcache_lookup(int node, int size) {
    if (node < 0 || size <= 0 || size > PCACHE_MAX_FILE) return NULL;

    uint32_t ef = spin_lock_irqsave(&pcache_lock);
    for (int i = 0; i < PCACHE_ENTRIES; i++) {
        if (pcache[i].in_use && pcache[i].node_idx == node &&
            pcache[i].size == size) {
            pcache[i].last_tick = get_ticks();
            char* d = pcache[i].data;
            spin_unlock_irqrestore(&pcache_lock, ef);
            return d;
        }
    }
    spin_unlock_irqrestore(&pcache_lock, ef);
    return NULL;
}

static void pcache_drop_slot(int slot) {
    if (pcache[slot].data) {
        kfree(pcache[slot].data);
        pcache[slot].data = NULL;
    }
    pcache[slot].cap = 0;
    pcache[slot].in_use = 0;
    pcache[slot].node_idx = -1;
    pcache[slot].size = 0;
}

void pcache_insert(int node, const char* data, int size) {
    if (node < 0 || size < 0 || data == NULL) return;

    uint32_t ef = spin_lock_irqsave(&pcache_lock);

    // Reuse an existing entry for this node, else a free slot, else evict
    // the least-recently-used entry.
    int slot = -1;
    for (int i = 0; i < PCACHE_ENTRIES; i++) {
        if (pcache[i].in_use && pcache[i].node_idx == node) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < PCACHE_ENTRIES; i++) {
            if (!pcache[i].in_use) { slot = i; break; }
        }
    }
    if (slot < 0) {
        uint32_t oldest = 0xFFFFFFFFu;
        int best = 0;
        for (int i = 0; i < PCACHE_ENTRIES; i++) {
            if (pcache[i].last_tick < oldest) {
                oldest = pcache[i].last_tick;
                best = i;
            }
        }
        slot = best;
    }
    pcache_drop_slot(slot);

    // Oversized or empty files are not cached (an empty file would need a
    // 0-byte read anyway, which callers short-circuit).
    if (size == 0 || size > PCACHE_MAX_FILE) {
        spin_unlock_irqrestore(&pcache_lock, ef);
        return;
    }

    char* nb = (char*)kmalloc(size);
    if (!nb) {  // heap OOM: leave the slot empty, reads fall back to disk
        spin_unlock_irqrestore(&pcache_lock, ef);
        return;
    }
    memcpy(nb, data, size);
    pcache[slot].data = nb;
    pcache[slot].cap = size;
    pcache[slot].node_idx = node;
    pcache[slot].size = size;
    pcache[slot].last_tick = get_ticks();
    pcache[slot].in_use = 1;
    spin_unlock_irqrestore(&pcache_lock, ef);
}

void pcache_invalidate(int node) {
    if (node < 0) return;
    uint32_t ef = spin_lock_irqsave(&pcache_lock);
    for (int i = 0; i < PCACHE_ENTRIES; i++) {
        if (pcache[i].in_use && pcache[i].node_idx == node) {
            pcache_drop_slot(i);
            break;
        }
    }
    spin_unlock_irqrestore(&pcache_lock, ef);
}

void pcache_invalidate_all(void) {
    uint32_t ef = spin_lock_irqsave(&pcache_lock);
    for (int i = 0; i < PCACHE_ENTRIES; i++) pcache_drop_slot(i);
    spin_unlock_irqrestore(&pcache_lock, ef);
}
