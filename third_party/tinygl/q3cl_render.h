/* q3cl_render.h — Mectov TinyGL renderer backend for the ioquake3 client
 * (v38.103, Q3 phase 3).
 *
 * Phase 2 proved TinyGL can rasterize into a WM window; this backend is the
 * piece the client loop needs instead: an actual first-person world with a
 * camera the engine's input events can move. The renderer owns nothing but
 * the ZBuffer and the camera — the client owns the window, the input routing
 * and the frame pacing, and presents the finished frame through
 * q3ref_blit() from its WM draw callback (compositor context), exactly like
 * the gears demo does.
 *
 * World convention (Quake's, z up):
 *   yaw   0 rad faces -Y, increasing yaw turns right (toward +X)
 *   pitch 0 is level, positive pitch looks DOWN (like Q3's cl.viewangles)
 * Units are Quake units: the arena is 1024x1024 with 192 unit walls.
 */
#ifndef Q3CL_RENDER_H
#define Q3CL_RENDER_H

#include <stdint.h>

/* Arena geometry, shared with the client so movement clamping and the world
 * the renderer draws can never disagree. */
#define Q3REF_ARENA_HALF   512.0f
#define Q3REF_WALL_H       192.0f
#define Q3REF_PILLAR_H     320.0f
#define Q3REF_PILLAR_HALF  320.0f
#define Q3REF_EYE_H         26.0f      /* camera height above the floor */
#define Q3REF_FOV_DEG       90.0f

/* 0 = ready, -1 = no memory / ZB_open failed. */
int  q3ref_init(int w, int h);
void q3ref_shutdown(void);
int  q3ref_ready(void);
int  q3ref_width(void);
int  q3ref_height(void);

/* Clear + set up the projection for this frame. */
void q3ref_begin_frame(void);
/* Camera pose for the following q3ref_draw_world() (degrees for yaw/pitch). */
void q3ref_set_camera(float x, float y, float z, float yaw_deg, float pitch_deg);
/* Phase 4: draw the world from a loaded MCTBSP1 map (see
 * third_party/q3mectov/q3_map.h). NULL restores the phase-3 hardcoded
 * arena. Forward-declared here so this header stays dependency-free —
 * only q3cl_render.c pulls the real struct in. */
struct q3map_s;
void q3ref_set_map(const struct q3map_s *m);
/* Phase 8: draw a real .bsp (the game module's own level) instead of the
 * built-in arena. Passing NULL drops back to the arena / MCTBSP1 map. The
 * mesh must outlive the renderer's use of it — the caller owns it. */
struct q3bsp_mesh_s;                 /* tag from third_party/q3mectov/q3bsp.h */
void q3ref_set_bsp(const struct q3bsp_mesh_s *m);
/* Camera as a full basis, for a caller whose forward vector comes from the game
 * module (origin + unit forward; the up vector follows the world's Z). */
void q3ref_set_camera_basis(const float origin[3], const float forward[3]);
/* Last frame's draw accounting + the texture cache state (for logging/tests).
 * Any pointer may be NULL. */
void q3ref_bsp_stats(int *facesDrawn, int *trisDrawn, int *facesCulled,
                     int *shaders, int *fromDisk, int *placeholders);
/* Classify the finished frame straight out of the ZBuffer: the renderer's own
 * account of what it drew, independent of the WM and the compositor. Returns
 * the number of pixels examined; any output pointer may be NULL. */
int  q3ref_frame_histogram(int *cyan, int *warm, int *stepgreen, int *violet,
                           int *bright, int *sky, int *distinct);
/* Draw the arena. time_sec drives the animated props. */
void q3ref_draw_world(double time_sec);
void q3ref_end_frame(void);

/* Swizzle the finished 0x00RRGGBB ZBuffer into the WM's 0x00BBGGRR content
 * buffer, centred. Runs in the compositor's draw pass, never in the render
 * task. */
void q3ref_blit(uint32_t *dst, int cw, int ch);

#endif /* Q3CL_RENDER_H */
