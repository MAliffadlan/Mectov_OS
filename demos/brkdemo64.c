/* M7.3 brkdemo: demand paging proof. Grows the heap 4MB, touches every
 * 16th page with a pattern, verifies reads back, and shows via meminfo
 * that untouched pages cost nothing (delta ~= touched + tables). */
#include "sys64.h"

void demo_main(void) {
    DLINE(l);
    meminfo_t before, after;
    d_meminfo(&before);
    u64 base = (u64)d_brk(0);
    if (!base) {
        dl_s(&l, "BRK-NOBASE");
        dl_nl(&l);
        return;
    }
    u64 size = 4ULL * 1024 * 1024;
    u64 nb = (u64)d_brk(base + size);
    if (nb != base + size) {
        dl_s(&l, "BRK-GROW-FAIL");
        dl_nl(&l);
        return;
    }
    /* Touch every 16th page (256 pages of 1024); verify pattern. */
    volatile u64 *heap = (volatile u64 *)base;
    u64 touched = 0;
    for (u64 p = 0; p < 1024; p += 16) {
        heap[p * 512] = 0xB000000000000000ULL + p;
        touched++;
    }
    for (u64 p = 0; p < 1024; p += 16) {
        if (heap[p * 512] != 0xB000000000000000ULL + p) {
            dl_s(&l, "BRK-MISMATCH ");
            dl_u(&l, p);
            dl_nl(&l);
            return;
        }
    }
    d_meminfo(&after);
    u64 used = before.free_frames - after.free_frames;
    dl_s(&l, "BRK touched=");
    dl_u(&l, touched);
    dl_s(&l, " frames=");
    dl_u(&l, used);
    dl_nl(&l);
    /* Shrink back to base; brk must report base. */
    if ((u64)d_brk(base) != base) {
        dl_s(&l, "BRK-SHRINK-FAIL");
        dl_nl(&l);
        return;
    }
    dl_s(&l, "BRK-DONE");
    dl_nl(&l);
}
