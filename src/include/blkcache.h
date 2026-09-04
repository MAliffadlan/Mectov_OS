#ifndef BLCACHE_H
#define BLCACHE_H

#include "types.h"

// blkcache.h — sector-level LRU read cache for the ATA block layer (v38.62)
//
// Mectov's filesystem stack caches whole MECTOVFS files (pcache.c), but the
// drivers below re-read raw sectors constantly: ext2 block reads (data,
// directories, inode tables, bitmaps, indirect blocks), FAT32 cluster + FAT
// sector reads (walking a long cluster chain re-reads the same FAT sectors
// once per entry), and any repeated whole-file read that outgrows the page
// cache (e.g. the 160 KB /bench.big benchmark) all pay PIO/DMA per access.
// This cache sits at the ata_read/write_sectors_drive entry points and
// serves repeated sector ranges straight from RAM.
//
// Coherence is write-through: every ATA write invalidates the affected
// range AFTER it reaches the disk (see blkcache_invalidate), and a reader
// only inserts data whose generation still matches the disk state, so a
// stale fill racing a concurrent write is discarded, never cached.
//
// Locking: blkcache_lock is a leaf. The read-miss path in ata.c holds it
// across the real ATA op (bounded — the ATA layer serializes all I/O under
// ata_lock anyway, and nothing takes blkcache_lock while holding ata_lock,
// so the documented order is vfs > pcache > blkcache > ata). All entry
// points are safe from the #PF handler's lock-free VFS path: cache data
// lives in static .bss and the critical sections never fault.
//
// Entries are fixed 512-byte sectors. Sizes are chosen so a full 160 KB
// /bench.big read (320 sectors) plus headroom stays resident.

#define BLKCACHE_ENTRIES 512   // 512 * 512 B = 256 KB of cached sectors
#define BLKCACHE_SECTOR 512

void blkcache_init(void);      // prints the banner; .bss is already zeroed

// Serve `count` whole sectors at (drive, lba) from the cache into buf.
// Returns 1 when EVERY sector hit (buf filled), 0 on a miss (buf untouched,
// and *gen_out receives the write-generation snapshot for blkcache_commit).
int blkcache_read(int drive, uint32_t lba, int count, unsigned char* buf,
                  uint32_t* gen_out);

// Insert the sectors just read into the cache (call only after the read
// succeeded). Discarded if a concurrent write invalidated the generation
// captured at the miss, so stale data can never be cached.
void blkcache_commit(int drive, uint32_t lba, int count,
                     const unsigned char* buf, uint32_t gen);

// Drop any cached sectors in (drive, lba .. lba+count) and bump the write
// generation. Called by the ATA write paths AFTER the data reached the disk.
void blkcache_invalidate(int drive, uint32_t lba, int count);

#endif
