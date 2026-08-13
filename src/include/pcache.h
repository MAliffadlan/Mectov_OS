#ifndef PCACHE_H
#define PCACHE_H

#include "types.h"

// ---- Whole-file data cache for MECTOVFS (drive 0) ----
//
// The MECTOVFS on-disk model is whole-file: every read pulls the file's
// sectors off the ATA drive one 512-byte PIO transfer at a time, and every
// write rewrites the file plus the 256-sector node table. Files are small
// (apps, configs, text), so a whole-file RAM cache makes repeated reads —
// `cat`, `grep`, app launches, mmap fault-ins — cost one memcpy instead of
// a disk round-trip per sector.
//
// Cache coherence is write-through: vfs_write_file() updates the cache after
// the disk write succeeds, so a cached entry always equals the on-disk
// bytes. Entries are evicted LRU-style when the table fills, and dropped on
// node delete/rename/oversize. Only FS_FILE (MECTOVFS) nodes are cached;
// ext2/FAT32 files keep their driver-level I/O.
//
// Locking: pcache_lock sits between vfs_lock and ata_lock in the global
// ordering (vfs_lock > pcache_lock > ata_lock), and heap_lock sits below it
// (pcache buffers come from kmalloc). The mmap page-fault path
// (vfs_read_file_offset, no vfs_lock) takes pcache_lock directly — bounded
// waits only, never a vfs_lock re-entry, so it cannot self-deadlock.

#define PCACHE_MAX_FILE  (128 * 1024)  // bigger files bypass the cache
#define PCACHE_ENTRIES   24

void pcache_init(void);

// Return a pointer to the cached copy of `node` iff an entry exists with a
// matching size (the caller must hold vfs_lock or be in the no-lock offset
// path). Returns NULL on miss. Touches the entry's LRU clock.
char* pcache_lookup(int node, int size);

// Store (or replace) the full contents of `node`. Called after a successful
// disk write so the cache mirrors the disk, and after a first read so the
// file is primed for the next one. Files bigger than PCACHE_MAX_FILE drop
// any existing entry. Copies the data into its own kmalloc'd buffer.
void pcache_insert(int node, const char* data, int size);

// Drop the entry for `node` (node deleted / renamed / recreated).
void pcache_invalidate(int node);

void pcache_invalidate_all(void);

#endif
