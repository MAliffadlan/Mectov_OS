/* tgl_cyc.h — TSC-level split of what "gl_ms" is made of (v38.116).
 *
 * The millisecond clock this kernel exposes has 10 ms resolution, and a frame
 * at this stage of the port costs 30-45 ms of GL — so per-frame ms timings can
 * say "the GL phase is the frame" but never "which half of the GL phase". The
 * answer decides the next optimisation: cutting triangle setup (transform +
 * clip + perspective) and cutting raster fill (per-pixel texture + depth) are
 * different releases.
 *
 * Two accumulators, both written inside TinyGL's own hot path and drained once
 * per sampled frame by q3world_render.c:
 *
 *   tgl_cyc_vert — every glVertex3f: modelview/projection transform, texture
 *                  coordinate mapping, the clip cascade, and (for the third
 *                  vertex of a triangle) the raster call nested inside it;
 *   tgl_cyc_fill — only the raster call that gl_draw_triangle_fill() makes.
 *
 * So `vert - fill` is the setup cost and `fill` is the fill cost. rdtsc is
 * invariant under KVM here, and the couple of dozen cycles per triangle it
 * costs are noise next to the ~10^4 cycles a triangle actually takes.
 */
#ifndef TGL_CYC_H
#define TGL_CYC_H

static inline unsigned long long tgl_rdtsc(void) {
    unsigned lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | (unsigned long long)lo;
}

/* Defined in clip.c. */
extern unsigned long long tgl_cyc_fill;   /* raster only */
extern unsigned int tgl_n_fill;           /* triangles that reached the raster */
/* Defined in vertex.c. */
extern unsigned long long tgl_cyc_vert;   /* the whole glVertex3f path */

#endif /* TGL_CYC_H */
