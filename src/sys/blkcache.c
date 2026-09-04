// blkcache.c — sector-level LRU read cache for the ATA block layer (v38.62)
//
// See blkcache.h for the design contract: write-through coherence via
// post-write invalidation plus a generation check that keeps a reader from
// ever caching data that raced a concurrent write. Static .bss entries (no
// kmalloc), leaf lock, safe from the #PF path.
//
// LRU discipline: every hit/commit stamps the entry with a monotonic
// counter; eviction picks the oldest stamp. The counter is 32-bit and wraps
// after ~4e9 cache operations — on a wrap the oldest-stamp scan briefly
// prefers freshly wrapped entries; harmless for a cache (worst case a few
// evictions of still-hot sectors).

#include "../include/blkcache.h"
#include "../include/spinlock.h"
#include "../include/serial.h"
#include "../include/utils.h"

typedef struct {
    uint32_t lba;
    int      drive;
    uint32_t stamp;         // LRU clock (monotonic, wraps)
    uint8_t  valid;
    unsigned char data[BLKCACHE_SECTOR];
} blkcache_entry_t;

static blkcache_entry_t blkcache[BLKCACHE_ENTRIES];
static spinlock_t blkcache_lock = SPINLOCK_INIT;
static uint32_t blkcache_stamp = 0;   // LRU clock source
static uint32_t blkcache_gen = 0;     // write generation (see header)

void blkcache_init(void) {
    write_serial_string("[BLCACHE] ");
    write_serial_hex(BLKCACHE_ENTRIES);
    write_serial_string(" sector entries (");
    write_serial_hex((uint32_t)(BLKCACHE_ENTRIES * BLKCACHE_SECTOR / 1024));
    write_serial_string(" KB)\n");
}

static blkcache_entry_t* blkcache_find_slot(int drive, uint32_t lba) {
    for (int i = 0; i < BLKCACHE_ENTRIES; i++) {
        if (blkcache[i].valid && blkcache[i].drive == drive &&
            blkcache[i].lba == lba) return &blkcache[i];
    }
    return NULL;
}

int blkcache_read(int drive, uint32_t lba, int count, unsigned char* buf,
                  uint32_t* gen_out) {
    if (count < 1 || !buf) return 0;
    uint32_t ef = spin_lock_irqsave(&blkcache_lock);

    // Full-range hit only: the filesystem drivers read in fixed batches and
    // repeat the SAME ranges, so requiring every sector of the request to be
    // resident keeps the common case a single memcpy. A partial hit falls
    // through to a normal I/O + commit (which refreshes the whole range).
    for (int s = 0; s < count; s++) {
        blkcache_entry_t* e = blkcache_find_slot(drive, lba + (uint32_t)s);
        if (!e) {
            if (gen_out) *gen_out = blkcache_gen;
            spin_unlock_irqrestore(&blkcache_lock, ef);
            return 0;
        }
    }
    for (int s = 0; s < count; s++) {
        blkcache_entry_t* e = blkcache_find_slot(drive, lba + (uint32_t)s);
        e->stamp = ++blkcache_stamp;
        memcpy(buf + s * BLKCACHE_SECTOR, e->data, BLKCACHE_SECTOR);
    }
    spin_unlock_irqrestore(&blkcache_lock, ef);
    return 1;
}

void blkcache_commit(int drive, uint32_t lba, int count,
                     const unsigned char* buf, uint32_t gen) {
    if (count < 1 || !buf) return;
    uint32_t ef = spin_lock_irqsave(&blkcache_lock);

    // A write completed while this read was in flight: the bytes we read may
    // predate it, so caching them would serve stale data. Discard — the next
    // read after the write warms the fresh copy instead.
    if (gen != blkcache_gen) {
        spin_unlock_irqrestore(&blkcache_lock, ef);
        return;
    }

    for (int s = 0; s < count; s++) {
        uint32_t l = lba + (uint32_t)s;
        blkcache_entry_t* e = blkcache_find_slot(drive, l);
        if (!e) {
            // Take a free slot, else evict the least-recently-used entry.
            int free_slot = -1, lru = 0;
            for (int i = 0; i < BLKCACHE_ENTRIES; i++) {
                if (!blkcache[i].valid) { free_slot = i; break; }
                if (blkcache[i].stamp < blkcache[lru].stamp) lru = i;
            }
            e = (free_slot >= 0) ? &blkcache[free_slot] : &blkcache[lru];
            e->valid = 1;
            e->drive = drive;
            e->lba = l;
        }
        e->stamp = ++blkcache_stamp;
        memcpy(e->data, buf + s * BLKCACHE_SECTOR, BLKCACHE_SECTOR);
    }
    spin_unlock_irqrestore(&blkcache_lock, ef);
}

void blkcache_invalidate(int drive, uint32_t lba, int count) {
    if (count < 1) return;
    uint32_t ef = spin_lock_irqsave(&blkcache_lock);
    blkcache_gen++;  // any read in flight now holds a stale snapshot
    for (int i = 0; i < BLKCACHE_ENTRIES; i++) {
        if (blkcache[i].valid && blkcache[i].drive == drive &&
            blkcache[i].lba >= lba && blkcache[i].lba < lba + (uint32_t)count) {
            blkcache[i].valid = 0;
        }
    }
    spin_unlock_irqrestore(&blkcache_lock, ef);
}

uint32_t blkcache_gen_now(void) {
    uint32_t ef = spin_lock_irqsave(&blkcache_lock);
    uint32_t g = blkcache_gen;
    spin_unlock_irqrestore(&blkcache_lock, ef);
    return g;
}
