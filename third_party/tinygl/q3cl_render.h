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
/* Draw the arena. time_sec drives the animated props. */
void q3ref_draw_world(double time_sec);
void q3ref_end_frame(void);

/* Swizzle the finished 0x00RRGGBB ZBuffer into the WM's 0x00BBGGRR content
 * buffer, centred. Runs in the compositor's draw pass, never in the render
 * task. */
void q3ref_blit(uint32_t *dst, int cw, int ch);

#endif /* Q3CL_RENDER_H */
