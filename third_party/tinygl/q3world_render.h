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
/* v38.131: loading progress for the early boot window. done/total count the
 * world textures q3w_load has processed; stage is 1 while the collision world
 * builds, 2 while textures decode, 0 when idle. */
extern volatile int q3w_progress_stage;
extern volatile int q3w_progress_done;
extern volatile int q3w_progress_total;

/* v38.132: a progress hook, so the driver can make the boot window repaint
 * WHILE the textures decode.
 *
 * q3w_progress_done is volatile and updated every iteration of the decode loop,
 * but nothing was reading it from a paint path — the window was painted once at
 * open and then showed that same stale image for the whole load. A caller that
 * wants live progress (the driver does: it invalidates its window) registers
 * here and gets called as the count advances.
 *
 * It is a plain function pointer on purpose: this file does not include wm.h,
 * and the renderer must not know what a window is. Pass 0 to unregister.
 *
 * v38.133: the hook RETURNS a request, and the loop honours it.
 *
 * It used to fire every Q3W_PROGRESS_STEP textures on the theory that 12 calls
 * across a 94-texture map were enough for a bar to move. That is still true for
 * painting — but the hook is now also the only place the driver can answer a
 * user pressing ESC, and a step gate made the answer arrive up to STEP decodes
 * late (seconds, on a real map). It is called on EVERY texture instead; the
 * body is a dirty-flag set and one int read, next to a JPEG decode, so the
 * gate was never paying its way. A non-zero return asks q3w_load to stop
 * early: it breaks, leaves the undecoded slots as they were, and still runs
 * its own cleanup and summaries, so a cancelled load tears down through the
 * ordinary path. */
typedef int (*q3w_progress_fn_t)(int done, int total);
void q3w_set_progress_hook(q3w_progress_fn_t fn);

/* Draw every face that survives culling, from the camera at `cam` looking
 * along `fwd` (unit).
 *
 * Culling has two stages (v38.112). First the map's own visibility data: the
 * camera is located in the .bsp's tree, the leaf names a cluster and the PVS
 * matrix says which clusters are visible, so a retail map's thousands of
 * surfaces cost what its author's compiler said they should. Then the six
 * planes of the projection: `right` is the camera's right vector and
 * `tan_hx`/`tan_hy` are the half-extents the backend passed to glFrustum
 * (tan(fov/2) horizontally and vertically), so a face outside the real view
 * cone is dropped rather than merely depth-tested. `right` may be NULL for the
 * hand-built fallback paths, in which case only depth culling applies.
 *
 * v38.128: `time_sec` is the game clock the sky's tcMods animate against and
 * `z_far` the projection's far plane (the cloud box is zFar/1.75 across). Both
 * are ignored by a map with no sky, and the sky pass runs inside this call,
 * before the opaque pass, because a background layer is the world draw's first
 * customer (see q3sky.h). */
void q3w_draw(const q3bsp_mesh_t *m, const float cam[3], const float fwd[3],
              const float right[3], float tan_hx, float tan_hy,
              float time_sec, float z_far);

/* Last frame's accounting (reset by each q3w_draw). Any pointer may be NULL. */
void q3w_frame_stats(int *facesDrawn, int *trisDrawn, int *facesCulled);

/* How much of the last frame the PVS kept, and where the camera was in the
 * map's tree: cluster/leaf are -1 when there was no PVS to consult, and
 * `marked` is -1 in that case (every face was drawn). `total` is the mesh's
 * face count, so "marked/total" is the culling ratio a test can assert on. */
void q3w_vis_stats(int *marked, int *total, int *cluster, int *leafs);

/* Last frame's culling split: faces dropped because the PVS could not see their
 * leaf, faces dropped by the six frustum planes, and how many planes were built
 * (0 = no camera basis). Exists so a frame line can say WHICH stage culled —
 * "the level vanished" is otherwise indistinguishable between them. */
void q3w_cull_stats(int *byVis, int *byFrustum, int *planes, int *byBack);

/* v38.121: front-to-back draw order. q3w_draw sorts the faces that survive
 * culling by squared distance to the camera (nearest first) so the early
 * z-test rejects occluded pixels before their texel is sampled; `on` = 0
 * restores the v38.120 index order. q3w_sort_stats reports whether the sort
 * is enabled and whether the last frame was actually emitted sorted (it is
 * not under memory pressure or when the caller turned it off). */
void q3w_set_sort(int on);
void q3w_sort_stats(int *enabled, int *pool, int *sorted);

/* v38.132: the backface reject. q3w_set_cull(0) is `q3arena <map> nocull`.
 * q3w_cull_untrusted() reports how many faces the reject considered but drew
 * anyway because the file's own vertex normals disagree, so the log can show
 * how much of the candidate set was actually cullable — a reject that silently
 * eats the level is the failure this guards against. q3w_cull_enabled() is the
 * raw switch, for the frame line to print. */
void q3w_set_cull(int on);
int  q3w_cull_untrusted(void);
int  q3w_cull_enabled(void);

/* Load-time accounting. */
void q3w_load_stats(int *shaders, int *fromDisk, int *placeholders);

/* v38.125: how a shader script says its shaders are drawn, and what the draw
 * pass did with it. `addShaders` counts the definitions with a GL_ONE GL_ONE
 * stage (every flame in the demo; id draws those in a pass of its own, `sort
 * additive`), `cullShaders` those with `cull none` (fire and lava are two
 * sided). `addFaces` is how many faces the LAST frame's additive pass drew and
 * `addFacesRun` the run's total. Zero everywhere on a map whose scripts ask
 * for neither — the fixture arena does not. */
void q3w_flag_stats(int *addShaders, int *cullShaders, int *addFaces,
                    int *addFacesRun);

/* v38.128: the sky's face accounting. `boxFaces` is how many faces the last
 * frame handed to the cloud box (they are drawn by it, counted in `drawn`, and
 * NOT emitted as geometry); `fallbackFaces` how many carried a sky shader the box
 * could not take — no cloud layer, the `nosky` A/B, or an unallocatable pool —
 * and were drawn as ordinary surfaces. The `*Run` pair are the session totals. */
void q3w_sky_stats(int *boxFaces, int *boxFacesRun, int *fallbackFaces,
                   int *fallbackRun);

/* v38.124: the world's image path, for the view model's .md3 surfaces.
 *
 * An .md3 surface names its image the way a file is named — extension
 * included, sometimes in capitals ("…/machinegun.tga", "…/f_machinegun.TGA")
 * — while a .bsp shader name never has one. q3w_resolve_image takes either:
 * a known extension is stripped, then the resolver runs exactly as it does for
 * the level (shader script first, then the name as a path, then id's own
 * extension order, which prefers the .jpg the retail data actually ships).
 * Returns 0 with a malloc'd RGB buffer (kfree via q3w_release_image). */
int  q3w_resolve_image(const char *name, char *resolved, int rsize,
                       unsigned char **rgb, int *w, int *h);
/* GLuint spelled out: this header is included where TinyGL's gl.h is not. */
unsigned int q3w_upload_image(const unsigned char *rgb, int w, int h);
void q3w_release_image(unsigned char *rgb);

/* v38.116: where the GL phase's time goes, in TSC cycles since the last drain
 * (tgl_cyc.h). `vert` is every glVertex3f — transform, texture mapping, the
 * clip cascade — and `fill` is the raster alone, which is nested in `vert`, so
 * `vert - fill` is setup and `fill / vert` is the fill share. `tri` counts the
 * triangles that reached the raster, which is what makes a cycle count
 * comparable between two frames that drew different views. */
void q3w_glsplit_stats(unsigned long long *vert, unsigned long long *fill,
                       unsigned int *tri);
void q3w_glsplit_reset(void);

/* v38.126: the pass's NON-raster split, same TSC units and the same drain. The
 * fork executes GL ops immediately (there is no op queue to blame), so what the
 * vertex counter does not cover inside a slow draw phase is this file's own
 * per-face work: `prep` = PVS marking + the backface/six-plane tests + the sort,
 * `total` = the whole q3w_draw call. `total - prep - vert` is what the emit
 * loops spend outside the transform. */
void q3w_cpu_stats(unsigned long long *prep, unsigned long long *total);
/* Shaded fragments since the last drain (see tgl_cyc.h): pixels that passed the
 * depth test. Over the frame's 76800 pixels this is the overdraw ratio. */
void q3w_frag_stats(unsigned int *n);
/* The raw TSC, for the driver's one-time cycles-per-millisecond calibration:
 * every *_kc number in the dev lines is a ratio until this rate is known. */
unsigned long long q3w_tsc(void);

/* How many faces the last frame drew out of a lightmap page (v38.114), and how
 * many the whole run has drawn that way. Both zero for a map with no lightmap
 * lump — which is every generated fixture arena — so a test can tell "the level
 * is lit from the file" from "the fallback sun is still doing the work" without
 * reading a pixel. */
void q3w_light_stats(int *litFacesLast, int *litFacesTotal);

/* Classify a finished 0x00RRGGBB framebuffer into the world's palette buckets
 * (plus the clear colour, which is what an empty scene is made of). Returns
 * the number of pixels examined; `distinct` counts 4-bit-per-channel colours,
 * which is what separates a textured surface from a flat fill.
 *
 * `patch` is the bucket for the tessellated (curved) surface's magenta, a colour
 * chosen because no other texture in the level can produce it at any light
 * level — so "the curve reached the screen" is a question a pixel count can
 * answer. `wall` (v38.117) is the wall texture's own colour at the light this
 * renderer bakes — dark orange-brown, r>g>b — which the other buckets reject:
 * the old `warm` test only counted a z-fight artefact, never a shaded wall. */
int q3w_histogram(const uint32_t *px, int pitch, int w, int h,
                  int *cyan, int *warm, int *stepgreen, int *violet,
                  int *bright, int *patch, int *sky, int *distinct,
                  int *wall);

#endif /* Q3WORLD_RENDER_H */
