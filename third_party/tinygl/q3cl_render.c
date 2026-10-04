/* q3cl_render.c — Mectov TinyGL renderer backend for the ioquake3 client
 * (v38.103, Q3 phase 3).
 *
 *
 * Phase 2 (q3gl_window.c) proved the TinyGL software rasterizer runs on this
 * kernel and that a client can present into a WM window. This backend is the
 * next step: a real first-person world the engine's input can drive. The
 * client owns the window, the frame pacing and the input routing; here we
 * only own the ZBuffer, the projection and the arena.
 *
 * Two deliberate simplifications keep the frame budget inside what TCG can
 * push at 320x240: no textures (per-face shading instead of GL lighting, so
 * no normals and no light setup) and a flat checkerboard floor, which is the
 * cheapest thing that still reads as a Quake map in a screendump.
 *
 * Pixel format: TinyGL 32-bit PIXEL is 0x00RRGGBB, the Mectov framebuffer is
 * 0x00BBGGRR — q3ref_blit() does the one rotate-by-16 swizzle per pixel, in
 * the compositor's draw pass (never in the render task).
 */
#include <stdint.h>
#include <stddef.h>
#include <math.h>

#include <TGL/gl.h>
#include "zbuffer.h"
#include "q3cl_render.h"
#include "q3world_render.h"   /* phase 8: the .bsp world and its textures */
#include "q3viewmodel.h"      /* v38.124: id's own weapon view model (.md3) */
#include "q3hud.h"            /* v38.127: id's own status bar (gfx/2d, icons) */
#include "q3sky.h"            /* v38.128: the sky's cloud box (nosky / stats) */
/* v38.126: the fps readout's face. A pure data header (unsigned char tables,
 * no other includes, no kernel types) — the same anti-aliased DejaVu Sans Mono
 * 8x16 the kernel's own draw_char_px() blends with, so the readout cannot
 * drift from the rest of the OS's text. */
#include "../../src/include/font_aa.h"

extern void  write_serial_string(const char *s);
extern void *kmalloc(uint32_t size);
extern void  kfree(void *p);
/* v38.119: the exclusive fullscreen present path (src/drivers/vga.c).
 * v38.125: the target is a pure getter and the claim (damage + swap request)
 * moved to vga_fullscreen_present(), called once the frame is written — see
 * the comment on vga_fullscreen_target(). */
extern uint32_t *vga_fullscreen_target(int *w, int *h, int *pitch_px);
extern void      vga_fullscreen_present(void);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* --- state ------------------------------------------------------------ */
static ZBuffer *fb;
/* v38.113: presentation buffer. One 3D frame takes seconds on TCG while the
 * WM recomposites windows on its own schedule — letting the compositor copy
 * the live framebuffer meant grabbing it MID-RASTER: the new frame's finished
 * top rows above the clear colour of the rest. Every "blue floor" screenshot
 * this port has shown (the user's session included) was exactly that tear.
 * Instead of merely declining mid-frame blits (with r_in_frame below — that
 * deadlocks a TCG guest, whose compositor never runs outside the frame), the
 * finished frame is SNAPSHOT to this buffer at end_frame, and the compositor
 * always scales from it: tear impossible, the window always shows a complete
 * frame, one extra 320x240 copy per frame.
 *
 * v38.146: TWO buffers, not one. The single snapshot had a race the comment
 * above denied: q3ref_present_frame (game task) WRITES present_buf while
 * q3ref_blit (kernel main loop) READS it, with no lock — and locks are out
 * (see r_in_frame: they deadlock a TCG guest). When the blit overlaps the
 * snapshot the window shows the top half of one frame and the bottom half
 * of the next: invisible while still (consecutive frames match), a visible
 * horizontal tear while turning (consecutive frames differ most) — exactly
 * the "flicker pas nengok". The fix is a pointer flip: the game always
 * writes the BACK buffer, the compositor always reads the FRONT, and the
 * flip itself is one int store (atomic on x86, no lock, no wait). Cost: one
 * more 300 KB buffer, allocated once. Readers must copy present_front to a
 * local FIRST and index with that (a second flip mid-blit must not redirect
 * the read halfway — that would reintroduce the same tear). */
static GLuint *present_buf[2];
static volatile int present_front;
/* 1 while a 3D frame is being rastered; blit ignores it (see present_buf). */
static int r_in_frame = 0;
static GLuint  *fb_pbuf;
static int      rw, rh;
static int      r_inited;

static float cam_x = 0.0f, cam_y = 320.0f, cam_z = Q3REF_EYE_H;
static float cam_yaw = 180.0f;   /* degrees; there is no "default" facing */
static float cam_pitch = 0.0f;

/* Phase 8 camera mode. The yaw/pitch pair above can express any attitude in
 * the plane, but the module hands us an arbitrary forward vector (its own
 * ps.viewangles through id's AngleVectors), so the camera can also be set as a
 * full basis. The matrix is the standard view transform written the way TinyGL
 * reads glLoadMatrixf: row 0 = right, row 1 = up, row 2 = -forward, with the
 * translation folded in, so eye = R * (world - origin). */
static int   cam_use_basis;
static float cam_basis[16];
static float cam_org[3];
static float cam_fwd[3];
static float cam_right[3];

/* The .bsp world this backend draws instead of the built-in arena, and the
 * module's own level comes with its own textures (q3world_render.c). */
static const q3bsp_mesh_t *r_bsp;

/* v38.112: the half-extents of the projection installed by q3ref_begin_frame
 * (tan(fov/2) horizontally and vertically), so the world renderer can cull
 * against the SAME view cone the rasterizer uses instead of a copy of the
 * numbers. Set every frame; the defaults are the 90-degree square view. */
static float proj_tan_x = 1.0f, proj_tan_y = 1.0f;
/* v38.128: the far plane the same glFrustum installed. The sky sizes its cloud
 * box from it (zFar/1.75, id's MakeSkyVec), so the two must be one number rather
 * than a 4096 written twice. */
static float proj_z_far = 4096.0f;

int  q3ref_ready(void)   { return r_inited; }
int  q3ref_width(void)   { return rw; }
int  q3ref_height(void)  { return rh; }

static void crosshair(void);
static void arena(double time_sec);

/* --- helpers ---------------------------------------------------------- */

static float sh(float c, float f) {
    c *= f;
    if (c > 1.0f) return 1.0f;
    if (c < 0.0f) return 0.0f;
    return c;
}

static void quad(float x0, float y0, float z0,
                 float x1, float y1, float z1,
                 float x2, float y2, float z2,
                 float x3, float y3, float z3) {
    glVertex3f(x0, y0, z0);
    glVertex3f(x1, y1, z1);
    glVertex3f(x2, y2, z2);
    glVertex3f(x3, y3, z3);
}

/* One axis-aligned box, each face shaded by a fixed factor. Without GL
 * lighting this is what makes a box read as a box instead of a silhouette. */
static void shaded_box(float cx, float cy, float z0, float w, float d, float h,
                       float r, float g, float b) {
    float x0 = cx - w * 0.5f, x1 = cx + w * 0.5f;
    float y0 = cy - d * 0.5f, y1 = cy + d * 0.5f;
    float z1 = z0 + h;

    glColor3f(sh(r, 1.00f), sh(g, 1.00f), sh(b, 1.00f));   /* top    */
    glBegin(GL_QUADS);
    quad(x0, y0, z1, x1, y0, z1, x1, y1, z1, x0, y1, z1);
    glEnd();

    glColor3f(sh(r, 0.30f), sh(g, 0.30f), sh(b, 0.30f));   /* bottom */
    glBegin(GL_QUADS);
    quad(x0, y0, z0, x0, y1, z0, x1, y1, z0, x1, y0, z0);
    glEnd();

    glColor3f(sh(r, 0.78f), sh(g, 0.78f), sh(b, 0.78f));   /* -Y */
    glBegin(GL_QUADS);
    quad(x0, y0, z0, x1, y0, z0, x1, y0, z1, x0, y0, z1);
    glEnd();

    glColor3f(sh(r, 0.62f), sh(g, 0.62f), sh(b, 0.62f));   /* +Y */
    glBegin(GL_QUADS);
    quad(x0, y1, z0, x0, y1, z1, x1, y1, z1, x1, y1, z0);
    glEnd();

    glColor3f(sh(r, 0.86f), sh(g, 0.86f), sh(b, 0.86f));   /* -X */
    glBegin(GL_QUADS);
    quad(x0, y0, z0, x0, y0, z1, x0, y1, z1, x0, y1, z0);
    glEnd();

    glColor3f(sh(r, 0.52f), sh(g, 0.52f), sh(b, 0.52f));   /* +X */
    glBegin(GL_QUADS);
    quad(x1, y0, z0, x1, y1, z0, x1, y1, z1, x1, y0, z1);
    glEnd();
}

/* Vertical strip of wall between two floor points, from zb to zt. */
static void wall_strip(float x0, float y0, float x1, float y1,
                       float zb, float zt, float r, float g, float b) {
    glColor3f(r, g, b);
    glBegin(GL_QUADS);
    quad(x0, y0, zb, x1, y1, zb, x1, y1, zt, x0, y0, zt);
    glEnd();
}

/* --- world ------------------------------------------------------------ */

/* Phase 4 (v38.104): when the client loaded an MCTBSP1 map, the world —
 * geometry, bots included — comes from that data and nothing here is
 * hardcoded; the phase-3 arena below is only the fallback for a failed or
 * missing map load. The client calls q3ref_set_map() once after
 * q3map_load(). */
#include "../q3mectov/q3_map.h"

static const q3map_t *r_map;   /* NULL = phase-3 hardcoded arena fallback */

void q3ref_set_map(const struct q3map_s *m) {
    r_map = (const q3map_t *)m;
}

#define TILE 128.0f

/* ---- phase-3 hardcoded arena (fallback when no map is loaded) -------- */

static void floor_grid(void) {
    for (int iy = 0; iy < 8; iy++) {
        for (int ix = 0; ix < 8; ix++) {
            float x0 = -Q3REF_ARENA_HALF + ix * TILE;
            float y0 = -Q3REF_ARENA_HALF + iy * TILE;
            int dark = (ix + iy) & 1;
            float b = dark ? 0.15f : 0.23f;
            glColor3f(b, b * 0.97f, b * 0.88f);   /* warm grey, like q3dm1 */
            glBegin(GL_QUADS);
            quad(x0, y0, 0.0f, x0 + TILE, y0, 0.0f,
                 x0 + TILE, y0 + TILE, 0.0f, x0, y0 + TILE, 0.0f);
            glEnd();
        }
    }
}

static void walls(void) {
    const float A = Q3REF_ARENA_HALF;
    /* The four arena walls, as (x0,y0)->(x1,y1) floor segments: -X, +X, +Y, -Y.
     * Explicit segments, not shared coordinate arrays (v38.103 lesson). */
    const float xs[4] = { -A,  A, -A, -A };
    const float ys[4] = { -A, -A,  A, -A };
    const float xe[4] = { -A,  A,  A,  A };
    const float ye[4] = {  A,  A,  A, -A };

    for (int i = 0; i < 4; i++) {
        wall_strip(xs[i], ys[i], xe[i], ye[i], 32.0f, Q3REF_WALL_H - 24.0f,
                   0.42f, 0.36f, 0.28f);
        wall_strip(xs[i], ys[i], xe[i], ye[i], 0.0f, 32.0f,
                   0.26f, 0.22f, 0.18f);
        wall_strip(xs[i], ys[i], xe[i], ye[i],
                   Q3REF_WALL_H - 24.0f, Q3REF_WALL_H, 0.55f, 0.48f, 0.36f);
    }
}

static void pillars(void) {
    const float p = Q3REF_PILLAR_HALF;
    shaded_box(-p, -p, 0.0f, 64.0f, 64.0f, Q3REF_PILLAR_H, 0.62f, 0.55f, 0.40f);
    shaded_box( p, -p, 0.0f, 64.0f, 64.0f, Q3REF_PILLAR_H, 0.62f, 0.55f, 0.40f);
    shaded_box(-p,  p, 0.0f, 64.0f, 64.0f, Q3REF_PILLAR_H, 0.62f, 0.55f, 0.40f);
    shaded_box( p,  p, 0.0f, 64.0f, 64.0f, Q3REF_PILLAR_H, 0.62f, 0.55f, 0.40f);
}

/* Three "bots": coloured boxes that spin and bob, so the fallback scene is
 * provably alive frame to frame before any input is injected. */
static void bots(double t) {
    static const float bx[3] = {    0.0f, -140.0f,  150.0f };
    static const float by[3] = { -200.0f, -320.0f, -330.0f };
    static const float col[3][3] = {
        { 0.95f, 0.45f, 0.10f },   /* orange */
        { 0.20f, 0.85f, 0.25f },   /* green  */
        { 0.90f, 0.25f, 0.20f },   /* red    */
    };

    for (int i = 0; i < 3; i++) {
        float spin = (float)(t * (60.0 + 20.0 * i));
        float bob  = 6.0f + 6.0f * (float)sin(t * 1.7 + (double)i);
        glPushMatrix();
        glTranslatef(bx[i], by[i], bob);
        glRotatef(spin, 0.0f, 0.0f, 1.0f);
        shaded_box(0.0f, 0.0f, 0.0f, 48.0f, 48.0f, 72.0f,
                   col[i][0], col[i][1], col[i][2]);
        glPopMatrix();
    }
}

/* ---- phase-4 map world ----------------------------------------------- */

static void map_world(void) {
    const q3map_t *m = r_map;

    /* Floor checkers: one quad per 128-unit tile inside the map's bounds.
     * Kept from the phase-3 look deliberately — the floor is the cheapest
     * orientation cue and the walls/bruchs carry the map's own colours. */
    {
        float H = m->bounds_half;
        int   n = 8;
        float tile = (2.0f * H) / (float)n;
        for (int iy = 0; iy < n; iy++) {
            for (int ix = 0; ix < n; ix++) {
                float x0 = -H + ix * tile;
                float y0 = -H + iy * tile;
                int dark = (ix + iy) & 1;
                float b = dark ? 0.15f : 0.23f;
                glColor3f(b, b * 0.97f, b * 0.88f);
                glBegin(GL_QUADS);
                quad(x0, y0, 0.0f, x0 + tile, y0, 0.0f,
                     x0 + tile, y0 + tile, 0.0f, x0, y0 + tile, 0.0f);
                glEnd();
            }
        }
    }

    /* The map itself: every brush is a shaded box, bots included (their
     * boxes come straight from the map data — the phase-3 duplicate bot
     * tables in this file and in the client are gone). */
    for (int i = 0; i < m->n_brushes; i++) {
        const float *mn = m->brushes[i].mins;
        const float *mx = m->brushes[i].maxs;
        float cx = (mn[0] + mx[0]) * 0.5f;
        float cy = (mn[1] + mx[1]) * 0.5f;
        float w  = mx[0] - mn[0];
        float d  = mx[1] - mn[1];
        float h  = mx[2] - mn[2];
        shaded_box(cx, cy, mn[2], w, d, h,
                   m->brushes[i].rgb[0], m->brushes[i].rgb[1], m->brushes[i].rgb[2]);
    }
    for (int i = 0; i < m->n_bots; i++) {
        glPushMatrix();
        glTranslatef(m->bots[i].x, m->bots[i].y, m->bots[i].z);
        shaded_box(0.0f, 0.0f, 0.0f, m->bots[i].w, m->bots[i].d, m->bots[i].h,
                   m->bots[i].rgb[0], m->bots[i].rgb[1], m->bots[i].rgb[2]);
        glPopMatrix();
    }
}

/* Camera-space crosshair. Drawn FIRST (before the world transform) so it is
 * 10 units in front of the eye in view space; the depth test then lets the
 * world draw over it only where geometry is genuinely nearer. */
static void crosshair(void) {
    const float d = 10.0f;
    float u  = 2.0f * d / (float)rw;    /* world units per screen pixel */
    float hl = 6.0f * u, ht = 1.0f * u;

    glColor3f(0.95f, 1.00f, 0.95f);
    glBegin(GL_QUADS);
    quad(-hl, -ht, -d,  hl, -ht, -d,  hl, ht, -d, -hl, ht, -d);
    quad(-ht, -hl, -d,  ht, -hl, -d,  ht, hl, -d, -ht, hl, -d);
    glEnd();
}

static void arena(double time_sec) {
    static double t0 = -1.0;
    extern uint32_t get_ticks(void);
    double t = get_ticks() / 1000.0;
    if (t0 < 0.0) t0 = t;

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    crosshair();

    /* Camera: yaw about world Z, then pitch about the camera's right axis
     * (the Rx(90) puts the camera's -Z along a horizontal world axis and its
     * +Y along world +Z, i.e. z-up). Positive pitch looks UP here; the
     * client negates the mouse delta before it gets here. */
    if (cam_use_basis) {
        glLoadMatrixf(cam_basis);
    } else {
        glRotatef(cam_pitch, 1.0f, 0.0f, 0.0f);
        glRotatef(cam_yaw, 0.0f, 0.0f, 1.0f);
        glRotatef(90.0f, 1.0f, 0.0f, 0.0f);
        glTranslatef(-cam_x, -cam_y, -cam_z);
    }

    /* Phase 8: a .bsp loaded by the game module's own world is the whole scene
     * when there is one — that is the point of the phase. The hand-built arena
     * and the MCTBSP1 map stay reachable as fallbacks, because a build with no
     * game data on the volume must still render something assertable. */
    if (r_bsp && r_bsp->valid) {
        /* v38.128: time_sec is the game clock the sky's tcMods scroll against
         * (id's refdef.floatTime) and proj_z_far the far plane the box is sized
         * from — both ignored by a level with no sky. */
        q3w_draw(r_bsp, cam_org, cam_fwd, cam_right, proj_tan_x, proj_tan_y,
                 (float)time_sec, proj_z_far);
        return;
    }

    if (r_map) {
        /* Phase 4: the loaded map is the whole world (its brushes include
         * floor, walls and pillars; bots come from the same data). */
        map_world();
        return;
    }

    /* Fallback: the phase-3 hardcoded arena (a map load that failed must
     * still leave a live, assertable world behind). */
    floor_grid();
    walls();
    pillars();
    bots(t);
    (void)t;
}

/* --- API -------------------------------------------------------------- */

int q3ref_init(int w, int h) {
    if (r_inited) return 0;
    if (w < 64 || h < 64 || w > 2048 || h > 2048) return -1;

    rw = w;
    rh = h;
    fb_pbuf = (GLuint *)kmalloc((uint32_t)(rw * rh) * (uint32_t)sizeof(GLuint));
    if (!fb_pbuf) return -1;

    fb = ZB_open(rw, rh, ZB_MODE_RGBA, fb_pbuf);
    if (!fb) {
        kfree(fb_pbuf);
        fb_pbuf = NULL;
        return -1;
    }
    /* Presentation snapshot: same size as the render buffer, x2 for the
     * flip (v38.146). Failing to allocate is not fatal — blit falls back
     * to the live buffer; a half allocation frees its half. */
    present_buf[0] = (GLuint *)kmalloc((uint32_t)(rw * rh) * (uint32_t)sizeof(GLuint));
    present_buf[1] = (GLuint *)kmalloc((uint32_t)(rw * rh) * (uint32_t)sizeof(GLuint));
    if (!present_buf[0] || !present_buf[1]) {
        if (present_buf[0]) { kfree(present_buf[0]); present_buf[0] = NULL; }
        if (present_buf[1]) { kfree(present_buf[1]); present_buf[1] = NULL; }
    }
    present_front = 0;
    glInit(fb);
    glClearColor(0.10f, 0.18f, 0.45f, 1.0f);   /* sky */
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);      /* winding-agnostic: wrong faces still draw */
    glDisable(GL_LIGHTING);
    glShadeModel(GL_FLAT);
    r_inited = 1;
    return 0;
}

void q3ref_shutdown(void) {
    q3w_unload();
    q3vm_unload();   /* v38.124: the view model owns its own textures */
    q3hud_unload();  /* v38.127: the status bar owns its own decoded pictures */
    r_bsp = NULL;
    if (fb) { ZB_close(fb); fb = NULL; }
    if (fb_pbuf) { kfree(fb_pbuf); fb_pbuf = NULL; }
    if (present_buf[0]) { kfree(present_buf[0]); present_buf[0] = NULL; }
    if (present_buf[1]) { kfree(present_buf[1]); present_buf[1] = NULL; }
    if (r_inited) { glClose(); r_inited = 0; }
}

void q3ref_set_camera(float x, float y, float z, float yaw_deg, float pitch_deg) {
    cam_x = x; cam_y = y; cam_z = z;
    cam_yaw = yaw_deg; cam_pitch = pitch_deg;
    cam_use_basis = 0;
}

void q3ref_set_camera_basis(const float origin[3], const float forward[3]) {
    float f[3], r[3], u[3], len, h;
    union { float f; int i; } b;

    f[0] = forward[0]; f[1] = forward[1]; f[2] = forward[2];
    len = f[0] * f[0] + f[1] * f[1] + f[2] * f[2];
    if (len <= 1e-9f) { f[0] = 0.0f; f[1] = 1.0f; f[2] = 0.0f; }
    else {
        b.f = len; b.i = 0x5f3759df - (b.i >> 1);
        h = b.f; h = h * (1.5f - 0.5f * len * h * h);
        h = h * (1.5f - 0.5f * len * h * h);
        f[0] *= h; f[1] *= h; f[2] *= h;
    }

    /* right = forward x worldUp. That is exactly the basis the yaw/pitch path
     * builds (at yaw 0 it is +X, at yaw 180 -X), so a camera that arrives
     * either way frames the world identically. Straight up or down has no
     * horizontal component to cross with, so pick a stable fallback. */
    r[0] = f[1]; r[1] = -f[0]; r[2] = 0.0f;
    len = r[0] * r[0] + r[1] * r[1];
    if (len <= 1e-9f) { r[0] = 1.0f; r[1] = 0.0f; r[2] = 0.0f; }
    else {
        b.f = len; b.i = 0x5f3759df - (b.i >> 1);
        h = b.f; h = h * (1.5f - 0.5f * len * h * h);
        h = h * (1.5f - 0.5f * len * h * h);
        r[0] *= h; r[1] *= h; r[2] *= h;
    }
    /* up = right x forward */
    u[0] = r[1] * f[2] - r[2] * f[1];
    u[1] = r[2] * f[0] - r[0] * f[2];
    u[2] = r[0] * f[1] - r[1] * f[0];

    cam_org[0] = origin[0]; cam_org[1] = origin[1]; cam_org[2] = origin[2];
    cam_fwd[0] = f[0]; cam_fwd[1] = f[1]; cam_fwd[2] = f[2];
    cam_right[0] = r[0]; cam_right[1] = r[1]; cam_right[2] = r[2];

    /* glLoadMatrixf takes the STANDARD OpenGL column-major array (TinyGL's
     * glopLoadMatrix transposes as it stores), so the view matrix M with rows
     * (right, up, -forward) is written column by column:
     *   M * (p,1) = (right.(p-cam), up.(p-cam), -forward.(p-cam)).
     * Writing it row-wise — the obvious first mistake, and this port's — makes
     * the transform a transposed rotation, which quietly points the camera
     * somewhere else entirely and renders nothing but the clear colour. */
    cam_basis[0]  = r[0];  cam_basis[1]  = u[0];  cam_basis[2]  = -f[0];
    cam_basis[3]  = 0.0f;
    cam_basis[4]  = r[1];  cam_basis[5]  = u[1];  cam_basis[6]  = -f[1];
    cam_basis[7]  = 0.0f;
    cam_basis[8]  = r[2];  cam_basis[9]  = u[2];  cam_basis[10] = -f[2];
    cam_basis[11] = 0.0f;
    cam_basis[12] = -(r[0] * cam_org[0] + r[1] * cam_org[1] + r[2] * cam_org[2]);
    cam_basis[13] = -(u[0] * cam_org[0] + u[1] * cam_org[1] + u[2] * cam_org[2]);
    cam_basis[14] = f[0] * cam_org[0] + f[1] * cam_org[1] + f[2] * cam_org[2];
    cam_basis[15] = 1.0f;
    cam_use_basis = 1;
}

void q3ref_set_bsp(const q3bsp_mesh_t *mesh) {
    r_bsp = mesh;
    cam_use_basis = 0;
    if (mesh) q3w_load(mesh);
    else q3w_unload();
}

void q3ref_bsp_stats(int *facesDrawn, int *trisDrawn, int *facesCulled,
                     int *shaders, int *fromDisk, int *placeholders) {
    q3w_frame_stats(facesDrawn, trisDrawn, facesCulled);
    q3w_load_stats(shaders, fromDisk, placeholders);
}

/* v38.125: the shader scripts' draw flags, straight through to the module that
 * parsed them. The driver reports the load-time census once and the run total
 * at the end, so "the demo's flames are additive" and "a flame was actually
 * drawn additively" are two separate numbers. */
void q3ref_draw_flag_stats(int *addShaders, int *cullShaders, int *addFaces,
                           int *addFacesRun) {
    q3w_flag_stats(addShaders, cullShaders, addFaces, addFacesRun);
}

/* v38.124: the GL split is reported as of the END OF THE WORLD PASS.
 *
 * The counters tgl_cyc.h keeps are global to TinyGL, and the driver now draws
 * one more thing through the same rasterizer every frame (the weapon view
 * model's own depth-cleared pass, below). Reporting the raw counters would have
 * folded the gun's triangles into `glcyc` and moved numbers the suites already
 * compare — a change in the measurement, not in the world. So the world's
 * totals are snapshotted when its pass ends and that snapshot is what the
 * driver reads; anything drawn later never appears in them, and the drain/reset
 * at the end of the sampled window still zeroes the live counters. */
static unsigned long long gls_vert, gls_fill;
static unsigned int       gls_tri;
/* v38.126: the same treatment for the two v38.126 counters — the world's shaded
 * fragments (the overdraw numerator) and its non-raster CPU split. Snapshotted
 * with the cycles so the view model drawn afterwards cannot land in the world's
 * numbers, exactly like vert/fill above. */
static unsigned int       gls_frag;
static unsigned long long gls_prep, gls_total;

/* v38.124: the view model pass's OWN share of the same cycles, drained here
 * too. The world's numbers above deliberately exclude it (a pass drawn after
 * the world must not move the numbers the suites compare); without a separate
 * account the gun's cost would simply vanish from the profile, which is the
 * one way a new per-frame pass can hide a frame regression. */
static unsigned long long vm_cyc_vert, vm_cyc_fill;
static unsigned int       vm_cyc_tri;
static unsigned int       vm_cyc_frag;      /* v38.126: the gun's own fragments */

void q3ref_glsplit_stats(unsigned long long *vert, unsigned long long *fill,
                         unsigned int *tri) {
    q3w_glsplit_stats(&gls_vert, &gls_fill, &gls_tri);
    if (vert) *vert = gls_vert;
    if (fill) *fill = gls_fill;
    if (tri)  *tri  = gls_tri;
}

void q3ref_glsplit_reset(void) {
    q3w_glsplit_reset();
    gls_vert = gls_fill = 0;
    gls_tri = 0;
    vm_cyc_vert = vm_cyc_fill = 0;
    vm_cyc_tri = 0;
    gls_frag = vm_cyc_frag = 0;
    gls_prep = gls_total = 0;
}

/* v38.126: the world pass's shaded fragments and its non-raster CPU split. */
void q3ref_world_split(unsigned int *frag, unsigned long long *prep,
                       unsigned long long *total) {
    if (frag)  *frag  = gls_frag;
    if (prep)  *prep  = gls_prep;
    if (total) *total = gls_total;
}

/* v38.126: the view model's own shaded fragments (the gun is 267 triangles into
 * a ~143x110 box, so its overdraw ratio is a different question from the
 * world's). */
void q3ref_viewmodel_frag(unsigned int *frag) {
    if (frag) *frag = vm_cyc_frag;
}


void q3ref_vis_stats(int *marked, int *total, int *cluster, int *leafs) {
    q3w_vis_stats(marked, total, cluster, leafs);
}

void q3ref_cull_stats(int *byVis, int *byFrustum, int *planes, int *byBack) {
    q3w_cull_stats(byVis, byFrustum, planes, byBack);
}

/* v38.114: whether the level's lightmaps are what the frames are lit by. */
void q3ref_light_stats(int *litFacesLast, int *litFacesTotal) {
    q3w_light_stats(litFacesLast, litFacesTotal);
}

/* v38.128: the sky. `on` is the pass's effective state (the `nosky` knob), the
 * rest is what the box builder did on the last frame — the cloud height it
 * projected for, the sides and layers it filled, and the triangles it submitted
 * — so "a sky shader was found" and "a cloud box was drawn" stay two different
 * numbers. See q3sky_stats. */
void q3ref_sky_stats(int *on, int *registered, int *cloud, int *stages,
                     int *sides, int *tris, int *trisRun) {
    if (on) *on = q3sky_enabled();
    q3sky_stats(registered, cloud, stages, sides, tris, trisRun);
}

/* The faces the world draw handed to the box (and those it had to keep), which
 * is the world module's accounting rather than the box's. */
void q3ref_sky_face_stats(int *boxFaces, int *boxFacesRun, int *fallbackFaces,
                          int *fallbackRun) {
    q3w_sky_stats(boxFaces, boxFacesRun, fallbackFaces, fallbackRun);
}

void q3ref_set_sky(int on) {
    q3sky_set_enabled(on);
}

/* What is actually in the finished frame, straight out of the ZBuffer: the
 * renderer's own pixel evidence, independent of where the WM put the window or
 * what drew over it. A frame that is all clear colour is a failure this
 * reports immediately (sky ~= total) instead of showing up as a beautiful
 * screenshot of the desktop. */
int q3ref_frame_histogram(int *cyan, int *warm, int *stepgreen, int *violet,
                          int *bright, int *patch, int *sky, int *distinct,
                          int *wall) {
    if (!fb || !fb->pbuf) return 0;
    return q3w_histogram((const uint32_t *)fb->pbuf, fb->linesize / 4, rw, rh,
                         cyan, warm, stepgreen, violet, bright, patch, sky,
                         distinct, wall);
}

void q3ref_begin_frame(void) {
    r_in_frame = 1;
    glViewport(0, 0, rw, rh);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    {
        float hh = (float)rh / (float)rw;
        /* fov from Q3REF_FOV_DEG (horizontal); tan(45) = 1 for the default */
        float sx = (float)tan(Q3REF_FOV_DEG * 0.5 * M_PI / 180.0);
        /* v38.144: near 4.0 was tried here for depth precision (16-bit
         * z-buffer) and REVERTED the same hour: the fixture map's camera
         * sits under 4 units from its floor, and nose-against-wall views
         * do the same — near geometry vanishes instead of shimmering.
         * Distant-trim shimmer stays a minification-aliasing job
         * (nearest-only sampler), not a depth job. */
        glFrustum(-1.0f * sx, 1.0f * sx, -hh * sx, hh * sx, 1.0f, 4096.0f);
        /* the world renderer culls against these exact planes */
        proj_tan_x = sx;
        proj_tan_y = hh * sx;
        proj_z_far = 4096.0f;                 /* v38.128: the sky's box size */
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void q3ref_draw_world(double time_sec) {
    arena(time_sec);
    /* The world pass is over: freeze its GL split (see q3ref_glsplit_stats).
     * v38.126: the fragment and non-raster-CPU counters are frozen here too,
     * for the same reason — the view model's pass runs through the same
     * rasterizer afterwards. */
    q3w_glsplit_stats(&gls_vert, &gls_fill, &gls_tri);
    q3w_frag_stats(&gls_frag);
    q3w_cpu_stats(&gls_prep, &gls_total);
}

void q3ref_end_frame(void) {
    glFlush();
}

/* v38.116: a finished frame is not a PRESENTED one. The perf HUD is painted
 * into the ZBuffer by the driver after the histogram has been read, so the
 * snapshot the compositor blits has to be taken after that — moving it into
 * end_frame (v38.113) is why the overlay the log and the README describe never
 * actually reached the window: the snapshot was HUD-free, while the histogram
 * read the live buffer and stayed HUD-free exactly as the suites need. One flat
 * 320x240 copy, taken before the next frame's clear overwrites it; r_in_frame
 * stays set until this call so the compositor cannot copy a half-painted HUD. */
void q3ref_present_frame(void) {
    /* v38.146: write the BACK buffer, then flip. The compositor reads only
     * the front (see q3ref_blit), so a snapshot can never land mid-blit. */
    int back = 1 - present_front;
    if (present_buf[back] && fb && fb->pbuf) {
        const GLuint *s = (const GLuint *)fb->pbuf;
        GLuint *d = present_buf[back];
        int n = rw * rh;
        /* v38.152: was a per-pixel GLuint loop. At SCALE=2 (1280x960) that is
         * 1.23 million iterations every frame, and TGL_CFLAGS (-O1,
         * -mno-sse -mno-mmx -march=i686) leaves GCC no way to widen it —
         * measured ~34 ms/frame of gl-end time that tracked resolution and
         * not the scene. memcpy here is the wide `rep movsl` from q3_kernel.c
         * (stubs/string.h aliases it to q3_memcpy), so this becomes one string
         * op instead. */
        memcpy(d, s, (size_t)n * sizeof(GLuint));
        /* The copy must land before the flip becomes visible (a compiler
         * reordering the loop past the store would reintroduce the tear
         * on paper; x86 itself orders store-store). */
        __asm__ __volatile__("" ::: "memory");
        present_front = back;
    }
    r_in_frame = 0;
}

/* ---- v38.110: the in-window perf HUD ----------------------------------
 * Drawn straight into the finished ZBuffer (0x00RRGGBB), after end_frame and
 * before the WM blits the buffer to the window — so the user sees the fps and
 * where the frame budget goes, while the frame histogram (and therefore the
 * suite's pixel evidence) stays exactly what the 3D pass produced. The HUD
 * colours are 0xFF...-heavy whites and pure primaries; q3w_histogram's buckets
 * can only ever COUNT them (bright/warm), never subtract from the buckets the
 * suite asserts on.
 *
 * v38.120: the user asked for it smaller and neater — half-size 4x8 glyphs,
 * a right-aligned value column, a thinner bar, and a panel that alpha-blends
 * 25% toward black instead of painting a solid slab, with the corner nearest
 * the view chamfered so the block tapers away. Blending toward black means
 * text/panel pixels are always DARKER than the scene under them: they can
 * still only add to the histogram's bright/warm counts, never invent a new
 * family, and the panel colour itself is 0x000000 so it contributes nothing
 * to a bucket that the scene alone could not produce. */
/* v38.126: ov_fps starts at -1 = "no window sampled yet", so the readout has
 * nothing to draw (and draws nothing) on the frames before the first one. */
static int ov_fps = -1, ov_vm_ms, ov_gl_ms, ov_blit_ms, ov_wm_ms, ov_other_ms;
/* v38.126: the six-number panel is opt-in now — F3 toggles it (default off). */
static int ov_detail;

void q3ref_set_perf_overlay(int fps, int vm_ms, int gl_ms, int blit_ms,
                            int wm_ms, int other_ms) {
    ov_fps = fps; ov_vm_ms = vm_ms; ov_gl_ms = gl_ms;
    ov_blit_ms = blit_ms; ov_wm_ms = wm_ms; ov_other_ms = other_ms;
}

void q3ref_set_perf_detail(int on) { ov_detail = on ? 1 : 0; }

/* ---- v38.127: id's own status bar --------------------------------------
 * The HUD is not a GL client. Every picture id's status bar draws is 32-bit
 * RGBA blended with GL_SRC_ALPHA, and this rasterizer's only blend mode is
 * additive (src/misc.c's TGL_BLEND_FUNC), so q3hud.c composites the bar into
 * the finished 0x00RRGGBB ZBuffer by hand — the same buffer the perf panel
 * writes into, and the same one q3ref_present_frame() snapshots for the
 * compositor. The driver reads the values out of the game module's
 * playerState; nothing here talks to the VM. */
void q3ref_set_hud(int health, int armor, int ammo, int weapon, int score,
                   int firing, int now_ms) {
    q3hud_set(health, armor, ammo, weapon, score, firing, now_ms);
}

void q3ref_draw_hud(void) {
    if (!fb || !fb->pbuf) return;
    q3hud_draw((uint32_t *)fb->pbuf, fb->linesize / 4, rw, rh);
}

void q3ref_hud_stats(int *images, int *missing, int *draws, int *digits) {
    q3hud_stats(images, missing, draws, digits);
}

static void ov_px(int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= rw || y >= rh) return;
    ((uint32_t *)fb->pbuf)[(size_t)y * (fb->linesize / 4) + x] = c;
}

/* v38.120: the panel is no longer a solid slab over the world — the frame
 * the user is playing IS the content, so the backing panel alpha-blends at
 * 25% over whatever is under it (smoked glass): the scene stays readable
 * through the HUD, and the HUD text stays readable over any scene because
 * it blends toward black, never toward the scene's own colours. */
static void ov_blend_px(int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= rw || y >= rh) return;
    uint32_t *p = &((uint32_t *)fb->pbuf)[(size_t)y * (fb->linesize / 4) + x];
    *p = ((((*p & 0x00FCFCFCu) * 3u) + (c & 0x00FCFCFCu)) >> 2);
}

/* The user asked for a smaller, neater readout: a hand-drawn 4x8 mini font
 * (one nibble per ROW, MSB nibble = left column, stored high-aligned, so
 * the renderer masks 0x80>>col) covering exactly the glyphs the HUD prints
 * — its six labels plus the digits — so the whole panel fits in ~128x56
 * instead of the old 236x140 that covered half the view. Every glyph was
 * rendered and eyeballed ("fps vm gl blit wm other 0-9") before being
 * committed. */
static const unsigned char ov_font_m[26][8] = {
    0x60,0x90,0x90,0x90,0x90,0x90,0x60,0x00,  /* 0 */
    0x20,0x60,0x20,0x20,0x20,0x20,0x70,0x00,  /* 1 */
    0x60,0x90,0x10,0x20,0x40,0x80,0xf0,0x00,  /* 2 */
    0x00,0xe0,0x10,0x10,0x60,0x10,0xe0,0x00,  /* 3 */
    0x00,0x20,0x60,0xa0,0xf0,0x20,0x20,0x00,  /* 4 */
    0x00,0xf0,0x80,0xe0,0x10,0x10,0xe0,0x00,  /* 5 */
    0x60,0x80,0x80,0xe0,0x90,0x90,0x60,0x00,  /* 6 */
    0xf0,0x10,0x20,0x20,0x40,0x40,0x40,0x00,  /* 7 */
    0x60,0x90,0x90,0x60,0x90,0x90,0x60,0x00,  /* 8 */
    0x60,0x90,0x90,0x70,0x10,0x10,0x60,0x00,  /* 9 */
    0x80,0x80,0xe0,0x90,0x90,0x90,0xe0,0x00,  /* b */
    0x60,0x90,0x90,0xf0,0x80,0x80,0x60,0x00,  /* e */
    0x60,0x80,0x80,0xe0,0x80,0x80,0x80,0x00,  /* f */
    0x60,0x90,0x90,0x70,0x10,0x10,0x60,0x00,  /* g */
    0x80,0x80,0x80,0x90,0x90,0x90,0x90,0x00,  /* h */
    0x40,0x00,0x40,0x40,0x40,0x40,0x40,0x00,  /* i */
    0x00,0x40,0x40,0x40,0x40,0x40,0x40,0x60,  /* l */
    0xa0,0xd0,0xf0,0xc0,0xa0,0xa0,0xa0,0xa0,  /* m */
    0x60,0x90,0x90,0x90,0x90,0x90,0x60,0x00,  /* o */
    0xe0,0x90,0x90,0xe0,0x80,0x80,0x80,0x00,  /* p */
    0xc0,0xa0,0x80,0x80,0x80,0x80,0x80,0x00,  /* r */
    0x60,0x80,0x80,0x60,0x10,0x10,0xc0,0x00,  /* s */
    0x40,0x40,0xe0,0x40,0x40,0x40,0x60,0x00,  /* t */
    0x00,0x90,0x90,0x90,0x90,0x60,0x00,0x00,  /* v */
    0x00,0x90,0x90,0x90,0x90,0xf0,0x60,0x00,  /* w */
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,  /* space / other */
};

static void ov_char_m(int x, int y, char ch, uint32_t c) {
    static const char order[] = "0123456789befghilmoprstvw";
    int idx = -1;
    if (ch >= '0' && ch <= '9') {
        idx = ch - '0';
    } else {
        for (int i = 10; order[i]; i++) {
            if (order[i] == ch) { idx = i; break; }
        }
    }
    const unsigned char *g = ov_font_m[idx >= 0 ? idx : 25];
    for (int row = 0; row < 8; row++) {
        unsigned char bits = g[row];
        if (!bits) continue;
        for (int col = 0; col < 4; col++)
            if (bits & (0x80 >> col))   /* high nibble: table is 0x?0 */
                ov_px(x + col, y + row, c);
    }
}

static void ov_str_m(int x, int y, const char *s, uint32_t c) {
    for (; *s; s++, x += 4) ov_char_m(x, y, *s, c);
}

/* Right-aligned number: the value ends at x+w, so digits hug their label
 * instead of leaving a ragged gap after a 1-digit value. Digit count comes
 * from the buffer index — no strlen, this links freestanding. */
static void ov_num_m(int x, int y, int w, int v, uint32_t c) {
    char b[12];
    int i = (int)sizeof(b) - 1, neg = v < 0;
    unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    b[i] = '\0';
    do { b[--i] = '0' + (char)(u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    ov_str_m(x + w - (int)(sizeof(b) - 1 - i) * 4, y, &b[i], c);
}


/* v38.126: the panel is KEPT and no longer drawn by default — the whole block
 * below is what the user put a red box around (six numbers, four rows, and a
 * 5 px cost bar, ~124x56 px of 25%-blended panel over a 320x240 frame). It is
 * now behind the F3 toggle; what a player sees instead is ov_fps_readout().
 * Nothing is deleted, because the numbers are this port's own instrument and a
 * release that cannot show them cannot be profiled. */
void q3ref_draw_perf_overlay(void) {
    if (!ov_detail) return;
    static const uint32_t C_VM    = 0x00FFD24D;  /* amber  — VM step        */
    static const uint32_t C_GL    = 0x0000E5C0;  /* teal   — software GL    */
    static const uint32_t C_BLIT  = 0x004D9EFF;  /* blue   — window blit    */
    static const uint32_t C_WM    = 0x0000CC44;  /* green  — full WM pass   */
    static const uint32_t C_OTHER = 0x00B0B0B0;  /* grey   — the rest       */
    static const uint32_t C_TXT   = 0x00FFFFFF;
    static const uint32_t C_PANEL = 0x00000000;  /* blend base: black @25%  */
    static const uint32_t C_SHAD  = 0x00000000;  /* bar underline (solid)   */

    if (!fb || !fb->pbuf) return;
    int tx = rw - 128, ty = 6;          /* top-right mini block, 4x8 glyphs */

    /* Backing panel, alpha-blended at 25%: readable on any scene, and the
     * corner nearest the view (bottom-left) is chamfered so the block tapers
     * away instead of ending in a hard slab edge. */
    int px0 = tx - 4, px1 = rw - 4;
    int py0 = ty - 3, py1 = ty + 46;    /* 4 text lines + the 6px bar */
    int chf = 8;                         /* chamfer depth, px */
    for (int y = py0; y <= py1; y++) {
        int t = y - (py1 - chf);         /* 0..chf across the bottom rows */
        if (t < 0) t = 0;
        for (int x = px0 + t; x <= px1; x++)
            ov_blend_px(x, y, C_PANEL);
    }

    /* Labels left, values right-aligned to one column — the same numbers the
     * serial perf line carries, fps first. */
    int vr = tx + 120;                   /* value right edge (4px in from px1) */
    ov_str_m(tx,      ty,      "fps", C_TXT);
    ov_num_m(tx + 16, ty,      22,    ov_fps,      C_TXT);
    ov_str_m(tx + 48, ty,      "vm",  C_VM);
    ov_num_m(tx + 64, ty,      vr - (tx + 64), ov_vm_ms, C_TXT);

    ov_str_m(tx,      ty + 10, "gl",  C_GL);
    ov_num_m(tx + 16, ty + 10, vr - (tx + 16), ov_gl_ms, C_TXT);

    ov_str_m(tx,      ty + 20, "blit", C_BLIT);
    ov_num_m(tx + 16, ty + 20, 22,    ov_blit_ms,  C_TXT);
    ov_str_m(tx + 48, ty + 20, "wm",  C_WM);
    ov_num_m(tx + 64, ty + 20, vr - (tx + 64), ov_wm_ms, C_TXT);

    ov_str_m(tx,      ty + 30, "other", C_OTHER);
    ov_num_m(tx + 16, ty + 30, vr - (tx + 16), ov_other_ms, C_TXT);

    /* The bar: one cell per 2 ms of frame budget, draw order. Thinner (5px)
     * and inset so it stays inside the panel. */
    int by = ty + 40;
    int bx = px0 + 2;
    int bw = px1 - 2 - bx;
    int cell_w = 5;
    int cells = bw / cell_w;
    int used = 0;
    struct { int ms; uint32_t c; } seg[5] = {
        { ov_vm_ms, C_VM }, { ov_gl_ms, C_GL }, { ov_blit_ms, C_BLIT },
        { ov_wm_ms, C_WM }, { ov_other_ms, C_OTHER },
    };
    for (int s = 0; s < 5; s++) {
        int n = (seg[s].ms + 1) / 2;   /* ceil, so 1 ms still shows one cell */
        for (int k = 0; k < n && used < cells; k++, used++)
            for (int yy = by; yy < by + 5; yy++)
                for (int xx = bx + used * cell_w; xx < bx + (used + 1) * cell_w; xx++)
                    ov_px(xx, yy, seg[s].c);   /* solid: the bar IS the datum */
    }
    for (int xx = bx; xx < bx + cells * cell_w; xx++)
        ov_px(xx, by + 5, C_SHAD);
}

/* ---- v38.126: the fps readout ------------------------------------------
 *
 * The whole of the default HUD, and the answer to three requests at once:
 * only fps, in the top-right corner, and the text must not look ugly.
 *
 * It is drawn AFTER the upscale — into the buffer the frame is being presented
 * from, the WM's content buffer in a window and the back buffer in fullscreen
 * — which is why it is here and not in q3ref_draw_perf_overlay()'s ZBuffer.
 * That is the whole point: everything inside the 320x240 frame is doubled by
 * the blit, so the old hand-drawn 4x8 glyphs reached the screen as 8x16 blocks
 * of doubled pixels. The readout has no reason to obey the render resolution —
 * the render resolution is the 3D picture, and it is not being lowered.
 *
 * The face is the kernel's anti-aliased 8x16 (font_aa.h), so at the window's
 * own resolution the text is a real 8x16 glyph per character, the size a game's
 * own fps counter is and small enough to sit in the corner. It is anchored
 * INSIDE the image rect on purpose: both present paths repaint that rect from
 * scratch every frame, so a shorter number ("fps 9" after "fps 120") leaves no
 * stale glyph pixels and no clear-pass is needed. A near-black offset copy is
 * drawn under it — the shadow, not a panel — because the scene under it can be
 * the q3dm1 lava field, which is exactly as bright as white text. */
#define OV_MIX(bg, fg, ia, a) \
    ((unsigned)(((((bg) * (ia) + (fg) * (a)) + 128u) * 257u) >> 16))

/* v38.148 pitch override, defined with the other blit state below; declared
 * here because the raw writers (glyphs, readout, blit loops) all live above
 * that point in this file. 0 = tightly packed rows (stride == cw). */
static int blit_pitch;

static void ov_aa_px(uint32_t *dst, int cw, int ch, int x, int y,
                     uint32_t fg, unsigned cov, int sc) {
    unsigned a = cov * 17u;                 /* 4-bit coverage -> 0..255 */
    if (!a) return;
    if (sc < 1) sc = 1;
    unsigned ia = 255u - a;
    unsigned fr = (fg >> 16) & 0xFFu, fgn = (fg >> 8) & 0xFFu, fb = fg & 0xFFu;
    for (int j = 0; j < sc; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= ch) continue;
        /* v38.148: stride honors the pitch override (direct-present into a
         * back-buffer region); bounds stay on the logical cw/ch. */
        int st = blit_pitch > 0 ? blit_pitch : cw;
        uint32_t *row = dst + (size_t)yy * (size_t)st;
        for (int i = 0; i < sc; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= cw) continue;
            uint32_t bg = row[xx];
            unsigned r = OV_MIX((bg >> 16) & 0xFFu, fr, ia, a);
            unsigned g = OV_MIX((bg >>  8) & 0xFFu, fgn, ia, a);
            unsigned b = OV_MIX( bg        & 0xFFu, fb, ia, a);
            row[xx] = (r << 16) | (g << 8) | b;
        }
    }
}

static void ov_aa_str(uint32_t *dst, int cw, int ch, int x, int y,
                      const char *s, uint32_t fg, int sc) {
    int adv = FONT_AA_ADV * sc;
    for (; *s; s++, x += adv) {
        int uc = (unsigned char)*s;
        int gi = (uc >= FONT_AA_FIRST && uc <= FONT_AA_LAST) ? (uc - FONT_AA_FIRST) : 95;
        for (int j = 0; j < FONT_AA_CELL_H; j++) {
            const unsigned char *row = font_aa_data[gi][j];
            for (int i = 0; i < FONT_AA_CELL_W; i++) {
                unsigned cov = row[i];
                if (cov) ov_aa_px(dst, cw, ch, x + i * sc, y + j * sc, fg, cov, sc);
            }
        }
    }
}

static void ov_fps_readout(uint32_t *dst, int cw, int ch, int sc,
                           int ox, int oy, int dw, int dh) {
    /* While the F3 panel is up it carries the same fps in its own first row,
     * and the panel's top-right value lands under this readout — one number,
     * not two overlapping ones. */
    if (!dst || ov_fps < 0 || ov_detail) return;

    char num[8];
    unsigned u = (unsigned)(ov_fps > 999999 ? 999999 : ov_fps);
    int n = 0;
    do { num[n++] = (char)('0' + (u % 10u)); u /= 10u; } while (u && n < 7);
    for (int i = 0, k = n - 1; i < k; i++, k--) {
        char t = num[i]; num[i] = num[k]; num[k] = t;
    }
    num[n] = '\0';

    if (sc < 1) sc = 1;
    int adv  = FONT_AA_ADV * sc;
    int pad  = 4 * sc;                      /* margin from the frame's corner */
    int gap  = 2 * sc;                      /* between "fps" and the number   */
    int wlbl = 3 * adv;                     /* "fps"                           */
    int wnum = n * adv;
    int x = ox + dw - pad - (wlbl + gap + wnum);
    int y = oy + pad;
    if (x < ox) x = ox;
    int hg = FONT_AA_CELL_H * sc;
    if (y + hg > oy + dh) y = oy + dh - hg;
    if (y < oy) y = oy;
    if (y + hg > ch) y = ch - hg;
    if (y < 0) y = 0;

    /* Shadow pass first: one offset copy in near-black, then the real text on
     * top. The label is grey, the value white, the background is the game. */
    ov_aa_str(dst, cw, ch, x + sc, y + sc, "fps", 0x00101010, sc);
    ov_aa_str(dst, cw, ch, x + sc + wlbl + gap, y + sc, num, 0x00101010, sc);
    ov_aa_str(dst, cw, ch, x, y, "fps", 0x00B4B4B4, sc);
    ov_aa_str(dst, cw, ch, x + wlbl + gap, y, num, 0x00FFFFFF, sc);
}

/* --- v38.123/v38.124: the weapon on screen ---------------------------------
 *
 * The gun the user IS holding. v38.123 drew a procedural machinegun silhouette
 * here because the staged data set carried no weapon models; that premise was
 * wrong — the demo pak0 ships models/weapons2/machinegun/ — and v38.124 draws
 * id's own model instead (q3viewmodel.c). What survives below is the FALLBACK,
 * used only when the volume has no .md3 for this weapon, and the motion inputs
 * it reads are still the module's own playerState, per frame:
 *   - bobCycle: PM_AddEvent's footstep phase (0..255 per step pair) — the gun
 *     rides the same walk bob the view does, so it swings with the strides;
 *   - groundEntityNum: a jump lifts the gun (and tilts it) while airborne;
 *   - weaponstate == WEAPON_FIRING and weaponTime: id's own fire gate — the
 *     muzzle flash and the recoil kick live ONLY inside that window, so the
 *     sprite cannot lie about a shot the module did not fire.
 * One flat silhouette + one rim tone + a two-tone flash: at 320x240 the
 * readable part is the shape and the motion, not shading.
 */
static int vm_have, vm_firing, vm_grounded, vm_bob;
static int vm_kick;                       /* frames of recoil left */

/* --- v38.124: id's own weapon, or the fallback -----------------------------
 *
 * The producer of the "senjata apaan ini" frame is worth keeping: the demo
 * pak0 DOES ship the machinegun view model, and all this ever needed was a
 * loader for the format (q3viewmodel.c). So the path below is the real one —
 * id's body, barrel and flash, textured from id's own JPEGs, placed at id's
 * own proportions under this port's already-90-degree FOV — and the v38.123
 * silhouette survives only as the fallback for a volume that has no .md3 at
 * all (CI's fixture volume, or a machine that never staged a pak0). The two
 * can never both draw: q3vm_loaded() is what chooses, and q3vm_load() logs
 * which one it is, so a screendump of the fake gun always has a serial line
 * saying why. */
static int vm_gl_box[4];                  /* screen box of the last GL pass */
static int vm_gl_distinct;                /* 4-bit colours inside that box  */
int q3ref_viewmodel_load(const char *base) { return q3vm_load(base); }

void q3ref_viewmodel_cycles(unsigned long long *vert, unsigned long long *fill,
                            unsigned int *tri) {
    if (vert) *vert = vm_cyc_vert;
    if (fill) *fill = vm_cyc_fill;
    if (tri) *tri = vm_cyc_tri;
}

void q3ref_set_viewmodel(int firing, int grounded, int bob) {
    vm_have = 1;
    vm_firing = firing;
    vm_grounded = grounded;
    vm_bob = bob & 255;
    /* Kick lasts as long as id's own muzzle-flash event would read (one
     * machinegun cycle ≈ 100 ms ≈ 5 rendered frames at 50 fps). */
    if (firing && vm_kick == 0) vm_kick = 5;
    else if (!firing && vm_kick > 0) vm_kick--;
    /* The model keeps its own copy of the same three inputs (it needs them in
     * model units, and it owns the flash), so the state is not duplicated in
     * behaviour — only in representation. */
    q3vm_set_state(firing, grounded, bob);
}

/* How many 4-bit-per-channel colours the model's own screen box contains: the
 * cheapest honest answer to "is that shaded textured geometry or a flat slab",
 * which is exactly what the v38.123 complaint was. Same 4096-bit set the world
 * histogram uses, restricted to the box the GL pass reported.
 *
 * v38.125 adds the two ends of that range: near-black and near-white pixels.
 * The muzzle flash is a bright star on a black image, so `dark` is what a
 * pixel test can assert ABOUT THE BLENDING — an additive pass cannot make a
 * pixel darker than the frame without the flash, and an opaque one paints the
 * image's black surround over everything it covers. */
static int vm_gl_bright, vm_gl_dark;

static int vm_box_distinct(int x0, int y0, int x1, int y1) {
    unsigned char seen[512];
    int i, count = 0, bright = 0, dark = 0;

    vm_gl_bright = vm_gl_dark = 0;
    if (!fb || !fb->pbuf) return 0;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > rw - 1) x1 = rw - 1;
    if (y1 > rh - 1) y1 = rh - 1;
    if (x1 < x0 || y1 < y0) return 0;
    for (i = 0; i < 512; i++) seen[i] = 0;

    for (int y = y0; y <= y1; y++) {
        const uint32_t *row = (const uint32_t *)fb->pbuf +
                              (size_t)y * (size_t)(fb->linesize / 4);
        for (int x = x0; x <= x1; x++) {
            uint32_t p = row[x];
            int key = (int)((((p >> 20) & 0x0F) << 8) |
                            (((p >> 12) & 0x0F) << 4) |
                             ((p >> 4) & 0x0F));
            if (!(seen[key >> 3] & (1 << (key & 7)))) {
                seen[key >> 3] |= (unsigned char)(1 << (key & 7));
                count++;
            }
            {
                int r = (int)((p >> 16) & 0xFF);
                int g = (int)((p >> 8) & 0xFF);
                int b = (int)(p & 0xFF);
                int sum = r + g + b;
                if (sum >= 600) bright++;        /* mean channel >= 200 */
                else if (sum <= 30) dark++;      /* mean channel <= 10  */
            }
        }
    }
    vm_gl_bright = bright;
    vm_gl_dark = dark;
    return count;
}

int q3ref_viewmodel_flash(void) { return q3vm_flash_mode(); }

void q3ref_viewmodel_box(int *distinct, int *bright, int *dark) {
    if (distinct) *distinct = vm_gl_distinct;
    if (bright) *bright = vm_gl_bright;
    if (dark) *dark = vm_gl_dark;
}

void q3ref_viewmodel_stats(int *parts, int *surfaces, int *tris,
                           int *drawn, int *texw, int *texh, int *x0, int *y0,
                           int *x1, int *y1, int *distinct) {
    q3vm_stats(parts, surfaces, tris, texw, texh);
    if (drawn) *drawn = q3vm_drawn_tris();
    if (x0) *x0 = vm_gl_box[0];
    if (y0) *y0 = vm_gl_box[1];
    if (x1) *x1 = vm_gl_box[2];
    if (y1) *y1 = vm_gl_box[3];
    if (distinct) *distinct = vm_gl_distinct;
}

void q3ref_draw_viewmodel(void) {
    if (!vm_have || !fb || !fb->pbuf) return;

    /* The real model first: one GL pass of its own, drawn here (after the
     * histogram, like the silhouette it replaces) so the suites' pixel
     * evidence stays the world's alone, and with the depth buffer cleared so
     * the gun is never clipped by whatever the map put within arm's reach. */
    if (q3vm_loaded()) {
        int box[4];
        unsigned long long c0v = 0, c0f = 0, c1v = 0, c1f = 0;
        unsigned int c0t = 0, c1t = 0, c0g = 0, c1g = 0;
        box[0] = box[1] = box[2] = box[3] = 0;
        q3w_glsplit_stats(&c0v, &c0f, &c0t);
        q3w_frag_stats(&c0g);
        if (q3vm_draw(proj_tan_x, proj_tan_y, rw, rh, box)) {
            q3w_glsplit_stats(&c1v, &c1f, &c1t);
            q3w_frag_stats(&c1g);
            vm_cyc_vert += c1v - c0v;
            vm_cyc_fill += c1f - c0f;
            vm_cyc_tri += c1t - c0t;
            vm_cyc_frag += c1g - c0g;
            vm_gl_box[0] = box[0];
            vm_gl_box[1] = box[1];
            vm_gl_box[2] = box[2];
            vm_gl_box[3] = box[3];
            vm_gl_distinct = vm_box_distinct(box[0], box[1], box[2], box[3]);
        }
        return;
    }
    vm_gl_box[0] = vm_gl_box[1] = vm_gl_box[2] = vm_gl_box[3] = 0;
    vm_gl_distinct = 0;

    static const uint32_t GUN_BODY = 0x00282830;  /* dark steel            */
    static const uint32_t GUN_RIM  = 0x00485058;  /* lit top edge          */
    static const uint32_t GRIP     = 0x00181820;  /* darker composite grip */
    static const uint32_t FLASH    = 0x00FFE86B;  /* muzzle flash core     */
    static const uint32_t FLASH_R  = 0x00FF9A2A;  /* flash rim             */

    /* Walk bob: bobCycle moves 0..255 across one step PAIR, so the gun dips
     * twice per cycle (each foot) — the same doubled frequency PM_WalkBob
     * gives the view. Airborne lifts the whole gun up 4 px instead. */
    int bob_y = vm_grounded ? ((vm_bob * 6) >> 8) - 3 : -4;
    int kick  = (vm_kick > 0) ? 3 : 0;          /* recoil pushes toward the eye */

    /* Anchor: right-bottom, leaving the perf HUD (top-right) and the crosshair
     * (centre) clear. kick raises the gun (closer to the eye), bob lowers it. */
    int ax = rw - 92 + kick;                    /* gun left edge               */
    int ay = rh - 52 + bob_y - kick;            /* gun top edge                */

    /* Receiver body 56x14 with a 2 px rim on top. */
    for (int y = 0; y < 14; y++)
        for (int x = 0; x < 56; x++)
            ov_px(ax + x, ay + y, (y == 0) ? GUN_RIM : GUN_BODY);

    /* Barrel: 26 px long, 4 px tall, centre-left, pointing at the view's
     * vanishing point (up-right); 1 px hotter line along its top. */
    int bx0 = ax + 8, by0 = ay - 8;
    for (int x = 0; x < 26; x++) {
        for (int y = 0; y < 4; y++)
            ov_px(bx0 + x + x / 3, by0 + y - x / 8, GUN_BODY);
        ov_px(bx0 + x + x / 3, by0 - x / 8, GUN_RIM);
    }

    /* Muzzle: 3 px cap at the barrel tip — the flash's anchor. */
    int mx = bx0 + 26 + 8, my = by0 - 3;        /* tip after the slope         */
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 3; x++)
            ov_px(mx + x, my + y, GUN_RIM);

    /* Grip + trigger guard: raked 30 deg, grip darker than the body. */
    for (int i = 0; i < 14; i++)
        for (int y = 0; y < 5; y++)
            ov_px(ax + 38 + i / 2, ay + 14 + i + y, GRIP);
    for (int x = 0; x < 8; x++)                  /* guard under the receiver  */
        ov_px(ax + 30 + x, ay + 16, GUN_BODY);

    /* Sight: one post + one blade, so aiming has a reference. */
    for (int y = 1; y <= 4; y++) ov_px(ax + 20, ay - y, GUN_RIM);
    ov_px(ax + 21, ay - 4, GUN_RIM);

    /* The muzzle flash — only while id's own weaponstate says FIRING. Two
     * tones: a hot core and an orange rim, sized by the kick phase so it
     * pops on the first frame and shrinks out. */
    if (vm_firing) {
        int r = (vm_kick >= 4) ? 5 : 3;          /* big on shot, small after  */
        for (int y = -r; y <= r; y++)
            for (int x = -r; x <= r; x++) {
                int d2 = x * x + y * y;
                if (d2 > r * r) continue;
                uint32_t c = (d2 <= (r - 2) * (r - 2) || r < 4) ? FLASH : FLASH_R;
                ov_px(mx + 1 + x, my - 1 + y, c);
            }
    }

    /* v38.124: the fallback reports the rectangle it painted, so the viewmodel
     * line means the same thing whichever gun is on screen — box and distinct
     * describe it, and the loader's own `parts=` says which one to expect. The
     * distinct count is the tell between them: this silhouette is five flat
     * tones (body, rim, grip, flash core, flash rim) where id's texture gives
     * dozens. The box covers barrel tip, muzzle flash and grip, clamped to the
     * view. */
    {
        int bx0 = ax, by0 = ay - 17, bx1 = ax + 56, by1 = ay + 33;
        if (bx0 < 0) bx0 = 0;
        if (by0 < 0) by0 = 0;
        if (bx1 > rw - 1) bx1 = rw - 1;
        if (by1 > rh - 1) by1 = rh - 1;
        vm_gl_box[0] = bx0;
        vm_gl_box[1] = by0;
        vm_gl_box[2] = bx1;
        vm_gl_box[3] = by1;
        vm_gl_distinct = vm_box_distinct(bx0, by0, bx1, by1);
    }
}

/* One pixel, no channel shuffling: TinyGL's 32-bit PIXEL and the Mectov
 * framebuffer share the same 0x00RRGGBB layout (the kernel's own theme
 * constants land on screen unchanged — GUI_DESKTOP 0x0011111B reads back as
 * (17,17,27) in a screendump). Phase 2 shipped this blit with an R/B swizzle
 * copied from an older assumption; the gears demo's blue gear really was
 * drawn red. Fixed here and in q3gl_window.c. */
/* v38.146: max blit scale to cap present-pixel work. Default 2× (640×480
 * content) — the game renders 320×240, upscaling 2× is still sharp enough
 * for 320×240 assets, and it caps the per-frame blit/composite/QEMU-upload
 * cost at 4× native regardless of how big the user drags the window.
 * Override at runtime with Q3REF_MAX_BLIT_SCALE env / future console var. */
#ifndef Q3REF_MAX_BLIT_SCALE
#define Q3REF_MAX_BLIT_SCALE 2
#endif

static int blit_scale = 1;
static int blit_max_scale = Q3REF_MAX_BLIT_SCALE;
/* v38.148: destination pitch override (pixels/row); the storage lives here
 * (tentatively declared above the glyph writers), 0 = tightly packed.
 * Read by every raw writer (blit loops, fps readout, AA glyphs): bounds
 * checks stay on cw/ch (logical size), only the row stride changes. */
void q3ref_set_blit_pitch(int p) { blit_pitch = (p > 0) ? p : 0; }

void q3ref_set_blit_scale(int s) { blit_scale = (s > 0) ? s : 1; }
int  q3ref_blit_scale(void)      { return blit_scale; }
void q3ref_set_blit_max_scale(int s) { blit_max_scale = (s > 0) ? s : 1; }
int  q3ref_blit_max_scale(void)      { return blit_max_scale; }

/* --- v38.119: fullscreen present -------------------------------------------
 *
 * The same 320x240 frame, written straight into the back buffer at screen size
 * instead of into a WM window's content buffer. Why this exists: with the game
 * in a window the desktop composites every frame — desktop_draw() repaints the
 * wallpaper and every window into the back buffer, wm_draw_all() redraws the
 * game window's chrome, its app draw callback copies the frame into the content
 * buffer, and only then does swap_buffers() push the damage to VRAM. That is a
 * full desktop repaint per game frame on the same single guest core the
 * renderer runs on, and none of it is visible while playing. Here the game's
 * own frame IS the screen: one scaled write into the back buffer, and the
 * kernel main loop's exclusive branch just swaps it (kernel.c).
 *
 * The scaling is nearest-neighbour with a 16.16 step accumulator and stretches
 * to the whole screen. A 4:3 screen and a 4:3 render still scale uniformly
 * (320x240 -> 1024x768 is exactly 3.2x on both axes), so the picture is not
 * distorted; a non-4:3 screen would stretch, which is the deliberate trade
 * against letterbox bars (the render resolution itself is untouched).
 *
 * The source is the SAME tear-proof snapshot the window blit uses: the kernel
 * main loop swaps the back buffer whenever it likes, including in the middle
 * of the next frame, so presenting the live ZBuffer would tear. The extra
 * 320x240 copy is one flat memcpy next to a 786k-pixel scale. */
void q3ref_present_fullscreen(void) {
    const GLuint *src;
    uint32_t *bb;
    int w = 0, h = 0, pitch = 0, x, y;
    unsigned stepx, stepy;
    int spitch;

    q3ref_present_frame();          /* the snapshot, after the HUD */
    if (!fb || !fb->pbuf || rw <= 0 || rh <= 0) return;
    bb = vga_fullscreen_target(&w, &h, &pitch);
    if (!bb || w <= 0 || h <= 0 || pitch <= 0) return;

    /* v38.146: pin the front index for the whole scale — a flip mid-loop
     * must not redirect the source halfway (same tear, new address). */
    { int front = present_front;
      src = present_buf[front] ? present_buf[front] : (const GLuint *)fb->pbuf; }
    spitch = fb->linesize / 4;
    stepx = (unsigned)(((unsigned)rw << 16) / (unsigned)w);
    stepy = (unsigned)(((unsigned)rh << 16) / (unsigned)h);
    if (!stepx) stepx = 1;
    if (!stepy) stepy = 1;

    for (y = 0; y < h; y++) {
        const GLuint *s = src + (size_t)(((unsigned)y * stepy) >> 16) * (size_t)spitch;
        uint32_t *d = bb + (size_t)y * (size_t)pitch;
        unsigned u = 0;
        for (x = 0; x < w; x++) {
            d[x] = s[u >> 16];
            u += stepx;
        }
    }
    /* v38.126: the fps readout, after the upscale — `bb` is the same surface
     * the scale loop just wrote and the one this path presents, so drawing it
     * here (before the present claim) puts it inside the damage rect. Twice the
     * cell when the screen is at least 2x the render, so fullscreen text is not
     * a speck on a 1024x768 panel while the window's own readout stays 8x16. */
    {
        int fsc = (rw > 0 && w >= 2 * rw) ? 2 : 1;
        ov_fps_readout(bb, pitch, h, fsc, 0, 0, w, h);
    }

    /* v38.125: the frame is complete — NOW claim the screen. Asking for the
     * swap before the write is a torn present whose missing rows cannot be
     * recovered: swap_buffers() resets the damage rect when it runs. */
    vga_fullscreen_present();
}


void q3ref_blit(uint32_t *dst, int cw, int ch) {
    if (!fb || !fb->pbuf || !dst || cw <= 0 || ch <= 0) return;
    /* Always the last COMPLETE frame: the snapshot end_frame took, or — if
     * its allocation failed — the live buffer (pre-38.113 behaviour).
     * v38.146: read the pinned front buffer (see present_fullscreen). */
    int front = present_front;
    const GLuint *src = present_buf[front] ? present_buf[front] : (const GLuint *)fb->pbuf;
    int pitch = fb->linesize / 4;   /* pixels per row */
    int sc = blit_scale;
    if (sc > blit_max_scale) sc = blit_max_scale;   /* v38.146 cap */
    /* Source pixels actually shown: the whole buffer, times the scale that
     * still fits the window's content area (falls back to 1:1 letterboxed). */
    if (rw * sc > cw || rh * sc > ch) sc = 1;
    int dw = (rw * sc <= cw) ? rw * sc : ((cw < rw) ? cw : rw);
    int dh = (rh * sc <= ch) ? rh * sc : ((ch < rh) ? ch : rh);
    int ox = (cw - dw) / 2; if (ox < 0) ox = 0;
    int oy = (ch - dh) / 2; if (oy < 0) oy = 0;

    for (int y = 0; y < dh; y++) {
        const GLuint *s = src + (long)(y / sc) * pitch;
        /* v38.148: destination stride honors the pitch override (direct
         * present into a back-buffer region whose rows are fb_width wide);
         * the drawn rect (ox/oy/dw/dh) is unchanged. */
        int dst_st = blit_pitch > 0 ? blit_pitch : cw;
        uint32_t *d = dst + (size_t)(oy + y) * (size_t)dst_st + ox;
        if (sc == 1) {
            for (int x = 0; x < dw; x++) d[x] = s[x];
        } else {
            for (int x = 0; x < rw; x++) {
                uint32_t v = s[x];
                for (int k = 0; k < sc; k++) d[x * sc + k] = v;
            }
        }
    }
    /* v38.126: the fps readout — the default HUD, drawn at the window's own
     * resolution instead of being doubled with the picture (sc = 1 here; the
     * fullscreen path scales it with the screen). Inside the image rect, which
     * this loop just repainted in full, so no stale pixels can survive a
     * shorter number. */
    ov_fps_readout(dst, cw, ch, 1, ox, oy, dw, dh);
}
