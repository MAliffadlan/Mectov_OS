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
/* v38.125: the additive/cull-none census of the shader scripts, and how many
 * faces the last frame (and the run) drew in the additive pass. See
 * q3w_flag_stats — this is its renderer-side spelling. */
void q3ref_draw_flag_stats(int *addShaders, int *cullShaders, int *addFaces,
                           int *addFacesRun);
/* v38.112: how much of the last frame the .bsp's PVS kept (marked/total), and
 * where the camera sat in the map's tree. `marked` is -1 when the map carries
 * no PVS and every face was drawn; cluster/leaf are -1 then too. */
void q3ref_vis_stats(int *marked, int *total, int *cluster, int *leafs);
/* v38.112: which culling stage dropped the last frame's faces. */
void q3ref_cull_stats(int *byVis, int *byFrustum, int *planes, int *byBack);
/* v38.114: how many faces the last frame drew out of a .bsp lightmap page, and
 * the run's total. Zero on every mesh without a lightmap lump, which is every
 * generated fixture arena — so "the level is lit from the file" is a number a
 * test can read rather than a look a human has to judge. */
void q3ref_light_stats(int *litFacesLast, int *litFacesTotal);
/* v38.128: the sky. `on` is the pass's effective state (the driver's `nosky`
 * knob turns it off), and the rest is what the cloud box did on the last frame:
 * `registered` sky shaders found at load, the cloud `height` it projected for,
 * `stages` layers drawn, `sides` box sides filled, and the `tris`/`trisRun` it
 * submitted. All zero on a map with no cloud-layer sky — every generated fixture
 * arena. */
void q3ref_sky_stats(int *on, int *registered, int *cloud, int *stages,
                     int *sides, int *tris, int *trisRun);
/* The other half of the sky's accounting, and the world draw's: how many faces
 * the last frame handed to the cloud box (the run's total second) and how many
 * carried a sky shader the box could not take, i.e. were drawn as geometry. */
void q3ref_sky_face_stats(int *boxFaces, int *boxFacesRun, int *fallbackFaces,
                          int *fallbackRun);
/* `nosky`: off means the sky faces are drawn as ordinary textured geometry
 * again, which is the A/B this release is measured against. */
void q3ref_set_sky(int on);

/* Classify the finished frame straight out of the ZBuffer: the renderer's own
 * account of what it drew, independent of the WM and the compositor. Returns
 * the number of pixels examined; any output pointer may be NULL. */
int  q3ref_frame_histogram(int *cyan, int *warm, int *stepgreen, int *violet,
                           int *bright, int *patch, int *sky, int *distinct,
                           int *wall);
/* Draw the arena. time_sec drives the animated props. */
void q3ref_draw_world(double time_sec);
void q3ref_end_frame(void);

/* v38.110: the in-window perf HUD. The driver feeds the phase costs it
 * measured (MILLISECONDS of kernel tick) once per sampled frame, then the
 * overlay is rendered AFTER q3ref_end_frame() — so the finished frame
 * carries an fps counter and a per-phase cost bar, while the frame histogram
 * (taken before the overlay) never sees it and the suite's pixel assertions
 * stay exact. Any value may be zero; zero phases simply draw no segment. */
void q3ref_set_perf_overlay(int fps, int vm_ms, int gl_ms, int blit_ms,
                            int wm_ms, int other_ms);
void q3ref_draw_perf_overlay(void);
/* v38.126: the six-number panel is opt-in (F3 in the driver) and OFF by
 * default; the driver calls this from its key handler. `fps` above is the
 * LAST SECOND's fps, which is what the readout shows — the run average is a
 * different number and stays in the log's perf line. */
void q3ref_set_perf_detail(int on);
/* v38.123: the first-person gun. `firing` is the module's own
 * weaponstate == WEAPON_FIRING, `grounded` is groundEntityNum !=
 * ENTITYNUM_NONE, `bob` is ps.bobCycle (0..255 footstep phase). Drawn after
 * the overlay so the HUD stays on top.
 *
 * v38.124: q3ref_viewmodel_load() points it at id's own .md3 parts
 * ("models/weapons2/machinegun/machinegun", no extension) and what is drawn
 * becomes that model — geometry, textures and tag-chained muzzle flash
 * (q3viewmodel.c). A load of 0 parts leaves the v38.123 silhouette in place as
 * the fallback, which is what a volume without a pak0 gets. The stats call
 * reports which of the two is on screen: parts/surfaces/triangles of the
 * model, the triangles the LAST PASS submitted (`drawn`, which must equal the
 * loaded count when the model is drawn whole), its texture, the screen box the
 * pass covered and how many distinct 4-bit colours are inside it. */
int  q3ref_viewmodel_load(const char *base);
void q3ref_set_viewmodel(int firing, int grounded, int bob);
void q3ref_viewmodel_stats(int *parts, int *surfaces, int *tris,
                           int *drawn, int *texw, int *texh, int *x0, int *y0,
                           int *x1, int *y1, int *distinct);

/* v38.125: 1 when the last pass composited the muzzle flash ADDITIVELY (id's
 * own blendfunc for models/weapons2/machinegun/f_machinegun). The flash's image
 * is a bright star on black, so "was it drawn" and "was it composited the way
 * id composites it" are different questions — v38.124 answered only the first,
 * and the opaque answer blacked out the view. */
int  q3ref_viewmodel_flash(void);

/* What the last pass left inside the box q3ref_viewmodel_stats reports:
 * distinct 4-bit colours, pixels that are nearly white and pixels that are
 * nearly black. The black count is what makes additive testable — additive
 * compositing can only ADD light, so a firing frame can never be darker than
 * an idle one, while an opaque flash paints its black surround over the view. */
void q3ref_viewmodel_box(int *distinct, int *bright, int *dark);

void q3ref_draw_viewmodel(void);

/* v38.127: id's own status bar — ammo, health, armor and the FFA score boxes,
 * composited into the ZBuffer out of the game's own pictures (gfx/2d/numbers,
 * gfx/2d/bigchars, gfx/2d/select, icons/icona_*, icons/iconr_yellow).
 *
 * The values are the game module's playerState, read by the driver; `firing` is
 * id's own `weaponstate == WEAPON_FIRING && weaponTime > 100` (the ammo field
 * greys while the gun is mid-shot) and `now_ms` is the frame clock the
 * low-health flash reads. Called after the view model and after the frame
 * histogram has been taken — so the suites' pixel evidence stays the 3D pass —
 * and before q3ref_present_frame(), which is what the compositor blits. */
void q3ref_set_hud(int health, int armor, int ammo, int weapon, int score,
                   int firing, int now_ms);
void q3ref_draw_hud(void);
void q3ref_hud_stats(int *images, int *missing, int *draws, int *digits);

/* v38.116: snapshot the finished frame for the compositor. Call it LAST in a
 * frame — after the HUD, which is painted into the same ZBuffer — because the
 * compositor blits this snapshot and nothing else. */
void q3ref_present_frame(void);

/* v38.116: the cycle split inside gl_ms — setup (transform + clip) vs raster
 * fill — drained once per sampled frame. See tgl_cyc.h for what is counted. */
void q3ref_glsplit_stats(unsigned long long *vert, unsigned long long *fill,
                         unsigned int *tri);
void q3ref_glsplit_reset(void);

/* v38.124: the view model pass's own share of those cycles, drained with
 * q3ref_glsplit_reset(). The world's numbers above deliberately EXCLUDE it
 * (see the snapshot note in q3cl_render.c); this is where they are instead. */
void q3ref_viewmodel_cycles(unsigned long long *vert, unsigned long long *fill,
                            unsigned int *tri);

/* v38.126: the world pass's shaded fragments (pixels that passed the depth
 * test — divided by the frame it is the overdraw ratio) and its NON-raster CPU
 * split in TSC cycles: `prep` is PVS marking + the per-face cull tests + the
 * sort, `total` is the whole world draw, so `total - prep - vert` is what the
 * emit loops spend outside the transform. The fork executes GL ops immediately
 * (no op queue), which is why "the GL phase is slower than its vertex path"
 * had to be measured here rather than blamed on marshalling. */
void q3ref_world_split(unsigned int *frag, unsigned long long *prep,
                       unsigned long long *total);
/* v38.126: the view model's own shaded fragments, same units and window. */
void q3ref_viewmodel_frag(unsigned int *frag);

/* Swizzle the finished 0x00RRGGBB ZBuffer into the WM's 0x00BBGGRR content
 * buffer, centred. Runs in the compositor's draw pass, never in the render
 * task. */
void q3ref_blit(uint32_t *dst, int cw, int ch);

/* v38.113: integer upscale at blit time (DOOM-style 2x). 1 = off. The 3D
 * pass keeps its resolution; only the compositor's copy is scaled. */
void q3ref_set_blit_scale(int s);
int  q3ref_blit_scale(void);
/* v38.148: destination row pitch (pixels) for q3ref_blit. 0 = tightly packed
 * (dst rows are cw wide — the content-buffer case). The WM's direct-present
 * path sets this to the back-buffer stride while the game draws into a
 * back-buffer region (whose rows are fb_width wide, not cw wide); without it
 * every row lands cw short and the picture skews diagonally. Reset to 0
 * afterwards. */
void q3ref_set_blit_pitch(int p);

/* v38.119: fullscreen present — snapshot the frame (like q3ref_present_frame)
 * and upscale it straight into the back buffer. Only valid while the VGA driver
 * is in exclusive mode (vga_fullscreen_enter). */
void q3ref_present_fullscreen(void);

#endif /* Q3CL_RENDER_H */
