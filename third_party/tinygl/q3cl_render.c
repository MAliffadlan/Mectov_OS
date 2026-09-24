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

#define TILE 128.0f

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
     * Each gets a base band, a body and a top rim. (Writing these as two
     * coordinate arrays is how this first shipped with two of the four walls
     * drawn as diagonals across the arena — the +Y entry read (-A,A)->(A,-A).
     * Explicit segments keep the geometry readable.) */
    const float xs[4] = { -A,  A, -A, -A };
    const float ys[4] = { -A, -A,  A, -A };
    const float xe[4] = { -A,  A,  A,  A };
    const float ye[4] = {  A,  A,  A, -A };

    for (int i = 0; i < 4; i++) {
        /* body */
        wall_strip(xs[i], ys[i], xe[i], ye[i], 32.0f, Q3REF_WALL_H - 24.0f,
                   0.42f, 0.36f, 0.28f);
        /* floor-level base band (also hides any z-fighting at z=0) */
        wall_strip(xs[i], ys[i], xe[i], ye[i], 0.0f, 32.0f,
                   0.26f, 0.22f, 0.18f);
        /* top rim */
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

/* Three "bots": coloured boxes that spin and bob, so the scene is provably
 * alive frame to frame before any input is injected. */
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
    glRotatef(cam_pitch, 1.0f, 0.0f, 0.0f);
    glRotatef(cam_yaw, 0.0f, 0.0f, 1.0f);
    glRotatef(90.0f, 1.0f, 0.0f, 0.0f);
    glTranslatef(-cam_x, -cam_y, -cam_z);

    floor_grid();
    walls();
    pillars();
    bots(t);
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
    if (fb) { ZB_close(fb); fb = NULL; }
    if (fb_pbuf) { kfree(fb_pbuf); fb_pbuf = NULL; }
    if (r_inited) { glClose(); r_inited = 0; }
}

void q3ref_set_camera(float x, float y, float z, float yaw_deg, float pitch_deg) {
    cam_x = x; cam_y = y; cam_z = z;
    cam_yaw = yaw_deg; cam_pitch = pitch_deg;
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
