/* q3world_render.h — draw a loaded .bsp as textured geometry (v38.108/Q3 phase 8).
 *
 * The backend in q3cl_render.c owns the ZBuffer, the projection and the camera;
 * this module owns the world geometry, its textures and the per-face shading.
 * Split that way because the two have different lifetimes: the camera changes
 * every frame, the mesh and its textures are loaded once.
 *
 * Textures are decoded from the game data on the volume (TGA, the format id's
 * tools emit) and are reported one by one in the serial log. A shader whose
 * image is missing gets a generated placeholder instead of silently drawing
 * white: on a machine with no retail pak the difference between "the map has a
 * texture I do not have" and "the renderer is broken" matters.
 */
#ifndef Q3WORLD_RENDER_H
#define Q3WORLD_RENDER_H

#include "q3bsp.h"

/* Load the textures named by the mesh's shaders. Returns the number of
 * shaders it produced an image for (real or placeholder). Safe to call again
 * for another mesh: the previous cache is released first. */
int  q3w_load(const q3bsp_mesh_t *m);
void q3w_unload(void);

/* Draw every face that survives culling, from the camera at `cam` looking
 * along `fwd` (unit). `right` is the camera's right vector, used only for the
 * horizontal cull; it may be NULL, in which case that test is skipped. */
void q3w_draw(const q3bsp_mesh_t *m, const float cam[3], const float fwd[3],
              const float right[3]);

/* Last frame's accounting (reset by each q3w_draw). Any pointer may be NULL. */
void q3w_frame_stats(int *facesDrawn, int *trisDrawn, int *facesCulled);
/* Load-time accounting. */
void q3w_load_stats(int *shaders, int *fromDisk, int *placeholders);

/* Classify a finished 0x00RRGGBB framebuffer into the world's palette buckets
 * (plus the clear colour, which is what an empty scene is made of). Returns
 * the number of pixels examined; `distinct` counts 4-bit-per-channel colours,
 * which is what separates a textured surface from a flat fill. */
int q3w_histogram(const uint32_t *px, int pitch, int w, int h,
                  int *cyan, int *warm, int *stepgreen, int *violet,
                  int *bright, int *sky, int *distinct);

#endif /* Q3WORLD_RENDER_H */
