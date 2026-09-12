// System-asset loader (debloat v38.81): on-demand files from /ext2.
// See assets.h for the contract.

#include "../include/assets.h"
#include "../include/vfs.h"
#include "../include/mem.h"
#include "../include/serial.h"

static uint32_t* wp_cached = NULL;

int assets_read(const char* path, uint8_t** out, uint32_t max) {
    if (!path || !out || max == 0) return -1;
    *out = NULL;
    int node = vfs_get_node(path);
    if (node < 0) return -1;
    int sz = fs_nodes[node].size;
    if (sz <= 0 || (uint32_t)sz > max) return -1;
    uint8_t* buf = (uint8_t*)kmalloc((uint32_t)sz);
    if (!buf) return -1;
    int rd = vfs_read_file(path, (char*)buf, sz);
    if (rd != sz) {
        kfree(buf);
        return -1;
    }
    *out = buf;
    return sz;
}

const uint32_t* assets_wallpaper(void) {
    if (wp_cached) return wp_cached;
    uint8_t* buf = NULL;
    int sz = assets_read("/ext2/wallpaper.bin", &buf, WP_BYTES);
    if (sz != WP_BYTES) {
        if (buf) kfree(buf);
        write_serial_string("[ASSETS] wallpaper.bin missing/bad — flat fallback\n");
        return NULL;
    }
    wp_cached = (uint32_t*)buf;
    return wp_cached;
}

int assets_open(const char* path, uint32_t* size_out) {
    if (!path || !size_out) return -1;
    int node = vfs_get_node(path);
    if (node < 0) return -1;
    int sz = fs_nodes[node].size;
    if (sz <= 0) return -1;
    *size_out = (uint32_t)sz;
    return node;
}
