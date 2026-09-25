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

extern void  write_serial_string(const char *s);
extern void *kmalloc(uint32_t size);
extern void  kfree(void *p);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* --- state ------------------------------------------------------------ */
static ZBuffer *fb;
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

int  q3ref_ready(void)   { return r_inited; }
int  q3ref_width(void)   { return rw; }
int  q3ref_height(void)  { return rh; }

static void crosshair(void);
static void arena(void);

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

static void arena(void) {
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
        q3w_draw(r_bsp, cam_org, cam_fwd, cam_right);
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
    r_bsp = NULL;
    if (fb) { ZB_close(fb); fb = NULL; }
    if (fb_pbuf) { kfree(fb_pbuf); fb_pbuf = NULL; }
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

/* What is actually in the finished frame, straight out of the ZBuffer: the
 * renderer's own pixel evidence, independent of where the WM put the window or
 * what drew over it. A frame that is all clear colour is a failure this
 * reports immediately (sky ~= total) instead of showing up as a beautiful
 * screenshot of the desktop. */
int q3ref_frame_histogram(int *cyan, int *warm, int *stepgreen, int *violet,
                          int *bright, int *patch, int *sky, int *distinct) {
    if (!fb || !fb->pbuf) return 0;
    return q3w_histogram((const uint32_t *)fb->pbuf, fb->linesize / 4, rw, rh,
                         cyan, warm, stepgreen, violet, bright, patch, sky,
                         distinct);
}

void q3ref_begin_frame(void) {
    glViewport(0, 0, rw, rh);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    {
        float hh = (float)rh / (float)rw;
        /* fov from Q3REF_FOV_DEG (horizontal); tan(45) = 1 for the default */
        float sx = (float)tan(Q3REF_FOV_DEG * 0.5 * M_PI / 180.0);
        glFrustum(-1.0f * sx, 1.0f * sx, -hh * sx, hh * sx, 1.0f, 4096.0f);
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void q3ref_draw_world(double time_sec) {
    (void)time_sec;
    arena();
}

void q3ref_end_frame(void) {
    glFlush();
}

/* ---- v38.110: the in-window perf HUD ----------------------------------
 * Drawn straight into the finished ZBuffer (0x00RRGGBB), after end_frame and
 * before the WM blits the buffer to the window — so the user sees the fps and
 * where the frame budget goes, while the frame histogram (and therefore the
 * suite's pixel evidence) stays exactly what the 3D pass produced. The HUD
 * colours are 0xFF...-heavy whites and pure primaries; q3w_histogram's buckets
 * can only ever COUNT them (bright/warm), never subtract from the buckets the
 * suite asserts on. */
static int ov_fps, ov_vm_ms, ov_gl_ms, ov_blit_ms, ov_wm_ms, ov_other_ms;

void q3ref_set_perf_overlay(int fps, int vm_ms, int gl_ms, int blit_ms,
                            int wm_ms, int other_ms) {
    ov_fps = fps; ov_vm_ms = vm_ms; ov_gl_ms = gl_ms;
    ov_blit_ms = blit_ms; ov_wm_ms = wm_ms; ov_other_ms = other_ms;
}

static void ov_px(int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= rw || y >= rh) return;
    ((uint32_t *)fb->pbuf)[(size_t)y * (fb->linesize / 4) + x] = c;
}

static void ov_char(int x, int y, char ch, uint32_t c) {
    extern unsigned char font8x16_data[256][16];
    const unsigned char *g = font8x16_data[(unsigned char)ch];
    for (int row = 0; row < 16; row++) {
        unsigned char bits = g[row];
        if (!bits) continue;
        for (int col = 0; col < 8; col++)
            if (bits & (0x80 >> col)) ov_px(x + col, y + row, c);
    }
}

static void ov_str(int x, int y, const char *s, uint32_t c) {
    for (; *s; s++, x += 8) ov_char(x, y, *s, c);
}

static void ov_num(int x, int y, int v, uint32_t c) {
    char b[16];
    int i = (int)sizeof(b) - 1, neg = v < 0;
    unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    b[i] = '\0';
    do { b[--i] = '0' + (char)(u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    ov_str(x, y, &b[i], c);
}

void q3ref_draw_perf_overlay(void) {
    static const uint32_t C_VM    = 0x00FFD24D;  /* amber  — VM step        */
    static const uint32_t C_GL    = 0x0000E5C0;  /* teal   — software GL    */
    static const uint32_t C_BLIT  = 0x004D9EFF;  /* blue   — window blit    */
    static const uint32_t C_WM    = 0x0000CC44;  /* green  — full WM pass   */
    static const uint32_t C_OTHER = 0x00B0B0B0;  /* grey   — the rest       */
    static const uint32_t C_TXT   = 0x00FFFFFF;
    static const uint32_t C_SHAD  = 0x00000000;

    if (!fb || !fb->pbuf) return;
    int tx = rw - 232, ty = 8;          /* top-right block, 8x16 glyphs */
    int ty2 = ty + 18;

    /* Backing panel: readable on any scene, cheap (one fill per row). */
    for (int y = ty - 4; y < ty2 + 62; y++)
        for (int x = tx - 6; x < rw - 4; x++) ov_px(x, y, 0x00101018);

    ov_num(tx,      ty,  ov_fps, C_TXT);
    ov_str(tx + 40, ty,  "fps", C_TXT);
    ov_str(tx + 88, ty,  "vm", C_VM);
    ov_num(tx + 120, ty, ov_vm_ms, C_TXT);
    ov_str(tx + 40, ty2, "gl", C_GL);
    ov_num(tx + 120, ty2, ov_gl_ms, C_TXT);
    ov_str(tx + 40, ty2 + 18, "blit", C_BLIT);
    ov_num(tx + 120, ty2 + 18, ov_blit_ms, C_TXT);
    ov_str(tx + 40, ty2 + 36, "wm", C_WM);
    ov_num(tx + 120, ty2 + 36, ov_wm_ms, C_TXT);
    ov_str(tx + 40, ty2 + 54, "other", C_OTHER);
    ov_num(tx + 120, ty2 + 54, ov_other_ms, C_TXT);

    /* The bar: one cell per 2 ms of the ~50 ms frame budget, draw order. */
    int by = ty2 + 76;
    int bx = tx - 6;
    int cell_w = (rw - 4 - bx) / 25;
    if (cell_w < 2) cell_w = 2;
    int cells = (rw - 4 - bx) / cell_w;
    int used = 0;
    struct { int ms; uint32_t c; } seg[5] = {
        { ov_vm_ms, C_VM }, { ov_gl_ms, C_GL }, { ov_blit_ms, C_BLIT },
        { ov_wm_ms, C_WM }, { ov_other_ms, C_OTHER },
    };
    for (int s = 0; s < 5; s++) {
        int n = seg[s].ms * 500;   /* ms -> half-ms units; cell = 500 units */
        n = (n + 499) / 500;      /* ceil, so 1 ms still shows one cell */
        for (int k = 0; k < n && used < cells; k++, used++)
            for (int yy = by; yy < by + 10; yy++)
                for (int xx = bx + used * cell_w; xx < bx + (used + 1) * cell_w; xx++)
                    ov_px(xx, yy, seg[s].c);
    }
    for (int xx = bx; xx < bx + cells * cell_w; xx++) ov_px(xx, by + 10, C_SHAD);
}

/* One pixel, no channel shuffling: TinyGL's 32-bit PIXEL and the Mectov
 * framebuffer share the same 0x00RRGGBB layout (the kernel's own theme
 * constants land on screen unchanged — GUI_DESKTOP 0x0011111B reads back as
 * (17,17,27) in a screendump). Phase 2 shipped this blit with an R/B swizzle
 * copied from an older assumption; the gears demo's blue gear really was
 * drawn red. Fixed here and in q3gl_window.c. */
void q3ref_blit(uint32_t *dst, int cw, int ch) {
    if (!fb || !fb->pbuf || !dst || cw <= 0 || ch <= 0) return;

    const GLuint *src = (const GLuint *)fb->pbuf;
    int dw = (cw < rw) ? cw : rw;
    int dh = (ch < rh) ? ch : rh;
    int ox = (cw - dw) / 2; if (ox < 0) ox = 0;
    int oy = (ch - dh) / 2; if (oy < 0) oy = 0;
    int pitch = fb->linesize / 4;   /* pixels per row */

    for (int y = 0; y < dh; y++) {
        const GLuint *s = src + (long)y * pitch;
        uint32_t *d = dst + (size_t)(oy + y) * cw + ox;
        for (int x = 0; x < dw; x++) d[x] = s[x];
    }
}
