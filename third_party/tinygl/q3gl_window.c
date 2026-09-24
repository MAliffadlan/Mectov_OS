/* q3gl_window.c — TinyGL platform layer for Mectov OS (v38.102, Q3 phase 2).
 *
 * Phase 1 (`q3`) proved the ioquake3 engine core boots as a timer-dependent
 * kernel task. Phase 2 proves a real 3D pipeline runs in that context: the
 * TinyGL software rasterizer (vendored at third_party/tinygl, MIT) renders a
 * lit, animated gears scene into an off-screen ZBuffer that is blitted into
 * a normal WM window by the compositor — the same draw_fn + wm_invalidate
 * flow DOOM uses in windowed mode (v38.29).
 *
 * The shell command `q3gl` forks this as its own kernel task (same reasoning
 * as `q3`: the task must run with interrupts enabled for the frame-rate
 * limiter to see time advance). ESC closes the window and ends the task;
 * the scene keeps rotating while the desktop stays fully interactive.
 *
 * Pixel notes: TinyGL is built with TGL_FEATURE_RENDER_BITS=32, where a
 * PIXEL stores R in bits 16-23, G in 8-15, B in 0-7 — byte for byte the same
 * 0x00RRGGBB layout the Mectov framebuffer and every kernel theme constant
 * use, so the blit is a plain copy (v38.103: it used to swap R and B, which
 * drew the blue gear red; the kernel's GUI_DESKTOP 0x0011111B reads back as
 * (17,17,27) in a screendump, which is what settled it). The ZBuffer is
 * opened with our own pbuf so there is exactly one buffer to copy, and the
 * copy happens in the window draw callback (compositor context), not in the
 * render loop.
 */
#include <stdint.h>
#include <stddef.h>

/* --- kernel API ------------------------------------------------------- */
extern void  write_serial_string(const char *s);
extern void  write_serial_hex(uint32_t v);
extern uint32_t get_ticks(void);          /* ms since boot (PIT 1000 Hz)   */
extern void *kmalloc(uint32_t size);
extern void  kfree(void *p);

/* Window manager: the DOOM-window pattern (doomgeneric_mectov.c v38.29). */
#include "../../src/include/theme.h"   /* TITLEBAR_H / TASKBAR_H_PX */
#include "../../src/include/wm.h"
extern int get_win_index(int wid);

/* --- TinyGL ----------------------------------------------------------- */
/* math.h is the Q3 stub (sin/cos -> q3_sin/q3_cos over the kernel FPU). */
#include <math.h>
#include <TGL/gl.h>
#include "zbuffer.h"

#define Q3GL_W 320
#define Q3GL_H 240

static ZBuffer *gl_fb;
static int      gl_win_id = -1;
static volatile int gl_running;
static int      gl_inited;

/* [Q3GL] diagnostic counter — serial print every N frames (test marker). */
static uint32_t gl_frame_counter;
#define Q3GL_TICK_EVERY 30

/* ---------------------------------------------------------------- */
/* Scene: the classic Mesa/GLUT gears demo (Brian Paul), trimmed to
 * what the Q3 frame budget can rasterize: no stipple, no text, flat
 * gear geometry with per-gear color + one light.               */
/* ---------------------------------------------------------------- */

static GLfloat view_rotx = 20.0f, view_roty = 30.0f;
static GLint   gear1, gear2, gear3;
static GLfloat angle;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void gear(GLfloat inner_radius, GLfloat outer_radius, GLfloat width,
                 GLint teeth, GLfloat tooth_depth) {
    GLint   i;
    GLfloat r0, r1, r2, angle, da;
    GLfloat u, v, len;

    r0 = inner_radius;
    r1 = outer_radius - tooth_depth / 2.0f;
    r2 = outer_radius + tooth_depth / 2.0f;
    da = 2.0f * (GLfloat)M_PI / (GLfloat)teeth / 4.0f;

    glShadeModel(GL_FLAT);
    glNormal3f(0.0f, 0.0f, 1.0f);

    /* front face */
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i <= teeth; i++) {
        angle = i * 2.0f * (GLfloat)M_PI / (GLfloat)teeth;
        glVertex3f(r0 * cos(angle), r0 * sin(angle), width * 0.5f);
        glVertex3f(r1 * cos(angle), r1 * sin(angle), width * 0.5f);
        glVertex3f(r0 * cos(angle), r0 * sin(angle), width * 0.5f);
        glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), width * 0.5f);
    }
    glEnd();

    /* front sides of teeth */
    glBegin(GL_QUADS);
    da = 2.0f * (GLfloat)M_PI / (GLfloat)teeth / 4.0f;
    for (i = 0; i < teeth; i++) {
        angle = i * 2.0f * (GLfloat)M_PI / (GLfloat)teeth;
        glVertex3f(r1 * cos(angle), r1 * sin(angle), width * 0.5f);
        glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), width * 0.5f);
        glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), width * 0.5f);
        glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), width * 0.5f);
    }
    glEnd();

    glNormal3f(0.0f, 0.0f, -1.0f);

    /* back face */
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i <= teeth; i++) {
        angle = i * 2.0f * (GLfloat)M_PI / (GLfloat)teeth;
        glVertex3f(r1 * cos(angle), r1 * sin(angle), -width * 0.5f);
        glVertex3f(r0 * cos(angle), r0 * sin(angle), -width * 0.5f);
        glVertex3f(r0 * cos(angle), r0 * sin(angle), -width * 0.5f);
        glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), -width * 0.5f);
    }
    glEnd();

    /* back sides of teeth */
    glBegin(GL_QUADS);
    da = 2.0f * (GLfloat)M_PI / (GLfloat)teeth / 4.0f;
    for (i = 0; i < teeth; i++) {
        angle = i * 2.0f * (GLfloat)M_PI / (GLfloat)teeth;
        glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), -width * 0.5f);
        glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), -width * 0.5f);
        glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), -width * 0.5f);
        glVertex3f(r1 * cos(angle), r1 * sin(angle), -width * 0.5f);
    }
    glEnd();

    /* outward faces of teeth */
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i < teeth; i++) {
        angle = i * 2.0f * (GLfloat)M_PI / (GLfloat)teeth;
        glVertex3f(r1 * cos(angle), r1 * sin(angle), width * 0.5f);
        glVertex3f(r1 * cos(angle), r1 * sin(angle), -width * 0.5f);
        u = r2 * cos(angle + da) - r1 * cos(angle);
        v = r2 * sin(angle + da) - r1 * sin(angle);
        len = sqrt(u * u + v * v);
        u /= len; v /= len;
        glNormal3f(v, -u, 0.0f);
        glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), width * 0.5f);
        glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), -width * 0.5f);
        glNormal3f(cos(angle), sin(angle), 0.0f);
        glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), width * 0.5f);
        glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), -width * 0.5f);
        u = r1 * cos(angle + 3 * da) - r2 * cos(angle + 2 * da);
        v = r1 * sin(angle + 3 * da) - r2 * sin(angle + 2 * da);
        glNormal3f(v, -u, 0.0f);
        glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), width * 0.5f);
        glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), -width * 0.5f);
        glNormal3f(cos(angle), sin(angle), 0.0f);
    }
    glEnd();

    /* inside radius cylinder */
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i <= teeth; i++) {
        angle = i * 2.0f * (GLfloat)M_PI / (GLfloat)teeth;
        glNormal3f(-cos(angle), -sin(angle), 0.0f);
        glVertex3f(r0 * cos(angle), r0 * sin(angle), -width * 0.5f);
        glVertex3f(r0 * cos(angle), r0 * sin(angle), width * 0.5f);
    }
    glEnd();
}

static void draw_scene(void) {
    angle += 2.0f;
    glPushMatrix();
    glRotatef(view_rotx, 1.0f, 0.0f, 0.0f);
    glRotatef(view_roty, 0.0f, 1.0f, 0.0f);

    glPushMatrix();
    glTranslatef(-3.0f, -2.0f, 0.0f);
    glRotatef(angle, 0.0f, 0.0f, 1.0f);
    glCallList(gear1);
    glPopMatrix();

    glPushMatrix();
    glTranslatef(3.1f, -2.0f, 0.0f);
    glRotatef(-2.0f * angle - 9.0f, 0.0f, 0.0f, 1.0f);
    glCallList(gear2);
    glPopMatrix();

    glPushMatrix();
    glTranslatef(-3.1f, 4.2f, 0.0f);
    glRotatef(-2.0f * angle - 25.0f, 0.0f, 0.0f, 1.0f);
    glCallList(gear3);
    glPopMatrix();

    glPopMatrix();
}

/* ---------------------------------------------------------------- */
/* WM window plumbing                                                  */
/* ---------------------------------------------------------------- */

/* Copy the ZBuffer into the window content buffer. Runs in the compositor
 * context (the main loop's draw pass), NOT the render task. TinyGL's 32-bit
 * PIXEL and the framebuffer share the 0x00RRGGBB layout, so this is a copy. */
static void q3gl_win_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id; (void)cx; (void)cy;
    if (!gl_fb || !gl_fb->pbuf || cw <= 0 || ch <= 0) return;
    int idx = get_win_index(id);
    if (idx < 0) return;
    uint32_t *dst = wm_wins[idx].content_buffer;
    if (!dst || wm_wins[idx].resizing) return;

    const GLuint *src = (const GLuint *)gl_fb->pbuf;
    int dw = (cw < Q3GL_W) ? cw : Q3GL_W;
    int dh = (ch < Q3GL_H) ? ch : Q3GL_H;
    int ox = (cw - dw) / 2; if (ox < 0) ox = 0;
    int oy = (ch - dh) / 2; if (oy < 0) oy = 0;
    int pitch = gl_fb->linesize / 4;  /* pixels per row */

    for (int y = 0; y < dh; y++) {
        const GLuint *s = src + (long)y * pitch;
        uint32_t *d = dst + (size_t)(oy + y) * cw + ox;
        for (int x = 0; x < dw; x++) d[x] = s[x];
    }
    /* Any letterbox strips keep the WM's dark clear color. */
}

/* ESC (scancode 0x01, or extended-release pair 0xF0 0x01) closes the demo. */
static void q3gl_win_key(int id, char c, uint8_t sc) {
    (void)id; (void)c;
    if ((sc & 0x7F) == 0x01) {
        write_serial_string("[Q3GL] ESC - shutting down\n");
        gl_running = 0;
    }
}

static void q3gl_scene_init(void) {
    static GLfloat pos[4]     = {5.0f, 5.0f, 10.0f, 0.0f};
    static GLfloat red[4]     = {1.0f, 0.0f, 0.0f, 0.0f};
    static GLfloat green[4]   = {0.0f, 1.0f, 0.0f, 0.0f};
    static GLfloat blue[4]    = {0.0f, 0.0f, 1.0f, 0.0f};
    static GLfloat white[4]   = {1.0f, 1.0f, 1.0f, 0.0f};
    static GLfloat shininess  = 5.0f;

    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, white);
    glLightfv(GL_LIGHT0, GL_SPECULAR, white);
    glEnable(GL_CULL_FACE);
    glEnable(GL_LIGHT0);
    glEnable(GL_LIGHTING);
    glEnable(GL_DEPTH_TEST);

    gear1 = glGenLists(1);
    glNewList(gear1, GL_COMPILE);
    glMaterialfv(GL_FRONT, GL_DIFFUSE, blue);
    glMaterialfv(GL_FRONT, GL_SPECULAR, white);
    glMaterialfv(GL_FRONT, GL_SHININESS, &shininess);
    glColor3fv(blue);
    gear(1.0f, 4.0f, 1.0f, 20, 0.7f);
    glEndList();

    gear2 = glGenLists(1);
    glNewList(gear2, GL_COMPILE);
    glMaterialfv(GL_FRONT, GL_DIFFUSE, red);
    glMaterialfv(GL_FRONT, GL_SPECULAR, white);
    glColor3fv(red);
    gear(0.5f, 2.0f, 2.0f, 10, 0.7f);
    glEndList();

    gear3 = glGenLists(1);
    glNewList(gear3, GL_COMPILE);
    glMaterialfv(GL_FRONT, GL_DIFFUSE, green);
    glMaterialfv(GL_FRONT, GL_SPECULAR, white);
    glColor3fv(green);
    gear(1.3f, 2.0f, 0.5f, 10, 0.7f);
    glEndList();

    glEnable(GL_NORMALIZE);
}

/* ---------------------------------------------------------------- */
/* Startup / teardown                                                  */
/* ---------------------------------------------------------------- */

static void q3gl_cleanup(void) {
    if (gl_win_id >= 0) {
        wm_close(gl_win_id);
        gl_win_id = -1;
    }
    if (gl_fb) {
        ZB_close(gl_fb);
        gl_fb = NULL;
    }
    if (gl_inited) {
        glClose();
        gl_inited = 0;
    }
}

static void q3gl_task_entry(void);

/* Parked task exit: like q3_exit_marker, returning from a forked kernel
 * task entry would jump to a garbage return address — park in hlt forever. */
static void q3gl_park(void) {
    for (;;) __asm__ __volatile__("hlt");
}

static void q3gl_task_entry(void) {
    write_serial_string("[Q3GL] task started, opening TinyGL context\n");

    uint32_t pbuf_bytes = Q3GL_W * Q3GL_H * sizeof(GLuint);
    GLuint *pbuf = (GLuint *)kmalloc(pbuf_bytes);
    if (!pbuf) {
        write_serial_string("[Q3GL] FATAL: no memory for pixel buffer\n");
        q3gl_park();
    }

    gl_fb = ZB_open(Q3GL_W, Q3GL_H, ZB_MODE_RGBA, pbuf);
    if (!gl_fb) {
        write_serial_string("[Q3GL] FATAL: ZB_open failed\n");
        kfree(pbuf);
        q3gl_park();
    }
    glInit(gl_fb);
    gl_inited = 1;

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glViewport(0, 0, Q3GL_W, Q3GL_H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    {
        GLfloat h = (GLfloat)Q3GL_H / (GLfloat)Q3GL_W;
        glFrustum(-1.0f, 1.0f, -h, h, 5.0f, 60.0f);
    }
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -45.0f);
    q3gl_scene_init();

    int ww = Q3GL_W + 2;
    int wh = Q3GL_H + TITLEBAR_H + 2;
    extern uint32_t fb_width, fb_height;
    int wx = ((int)fb_width - ww) / 2;  if (wx < 0) wx = 0;
    int wy = ((int)fb_height - TASKBAR_H_PX - wh) / 2; if (wy < 0) wy = 0;
    gl_win_id = wm_open(wx, wy, ww, wh, "TinyGL Gears",
                        q3gl_win_draw, q3gl_win_key, NULL, NULL);
    if (gl_win_id < 0) {
        write_serial_string("[Q3GL] FATAL: could not open WM window\n");
        q3gl_cleanup();
        q3gl_park();
    }
    write_serial_string("[Q3GL] window id=");
    write_serial_hex((uint32_t)gl_win_id);
    write_serial_string("\n");
    write_serial_string("[Q3GL] scene ready\n");
    extern volatile int needs_redraw;
    needs_redraw = 1;

    gl_running = 1;
    while (gl_running) {
        uint32_t t0 = get_ticks();
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        draw_scene();
        glFlush();

        /* Present through the compositor (draw_fn + invalidate), like the
         * windowed DOOM path — never touch the framebuffer from here. */
        extern void wm_invalidate(int id);
        wm_invalidate(gl_win_id);
        needs_redraw = 1;

        if (++gl_frame_counter % Q3GL_TICK_EVERY == 0) {
            write_serial_string("[Q3GL] frame=");
            write_serial_hex(gl_frame_counter);
            write_serial_string(" t=");
            write_serial_hex(get_ticks() / 1000);
            write_serial_string("\n");
        }

        /* ~20 fps cap: TinyGL + TCG burn a lot of host CPU for nothing
         * above this. See docs on Q3 frame limiting. */
        while (get_ticks() - t0 < 50) {
            __asm__ __volatile__("hlt");
        }
    }

    write_serial_string("[Q3GL] loop done, cleaning up\n");
    q3gl_cleanup();
    write_serial_string("[Q3GL] done\n");
    q3gl_park();
}

/* Called from the `q3gl` shell builtin (already forked into our own task). */
void q3gl_start(void) {
    q3gl_task_entry();
}
