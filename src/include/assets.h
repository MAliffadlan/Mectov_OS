#ifndef ASSETS_H
#define ASSETS_H

#include "types.h"

// System-asset loader (debloat v38.81). doom1.wad / wallpaper.bin /
// music.wav live as plain files on /ext2 (seeded by scripts/seed_ext2.sh)
// instead of linked .data blobs, and are loaded on first use:
//
// - wallpaper: 1024x768x32 raw, cached resident after the first paint
//   (same lifetime the old static had). NULL when missing/bad-sized —
//   callers fall back to a flat fill.
// - WAD/music: loaded by their owners (doom_libc caches the WAD for the
//   session; vfs seeding copies music.wav into VFS at boot).
//
// All helpers take the VFS lock internally and are safe from task 0 and
// Ring-0 app contexts. Never call with vfs_lock held.

#define WP_W            1024
#define WP_H            768
#define WP_BYTES        (WP_W * WP_H * 4)

// Pointer to the cached 1024x768 wallpaper, or NULL. Loads once.
const uint32_t* assets_wallpaper(void);

// Read a whole system file into a kmalloc'd buffer. Returns the byte
// count, or -1 when missing/empty/over `max`. Caller kfree()s *out.
int assets_read(const char* path, uint8_t** out, uint32_t max);

// Open a system file for streaming: returns the VFS node with its size in
// *size_out, or -1. The caller reads windows with vfs_read_file_offset()
// (no big allocation — the doom WAD streams this way so gameplay never
// needs a contiguous multi-MB heap block).
int assets_open(const char* path, uint32_t* size_out);

#endif
