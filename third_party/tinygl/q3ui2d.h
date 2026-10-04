/* q3ui2d.h — the 2D software rasterizer the official Quake III UI draws into
 * (v38.150).
 *
 * Why a software blitter and not TinyGL: id's UI module (q3ui.qvm) never talks
 * to the renderer directly. It calls UI_R_DRAWSTRETCHPIC with a shader handle
 * and a normalized source rectangle, and expects a 2D of exactly the ui.qvm's
 * own vocabulary: set a color, stretch a sub-rectangle of an image, fill a
 * rect. That is a memcpy-shaped problem — a few hundred kilopixels per frame —
 * and routing it through the GL rasterizer (shaders, texture objects, a Z
 * buffer, the projection matrix) would cost more than it draws.
 *
 * Resolution, and why 640x480: the retail UI is virtualized. ui_main.c scales
 * every coordinate by
 *
 *     uis.scale = vidWidth / 640      uis.bias = aspect fixup
 *
 * so whatever glconfig we report, the UI lays itself out in a 640x480 space and
 * the engine's blit scales the result to the real framebuffer. Reporting
 * 640x480 keeps that scale at exactly 1.0 — the layout, the glyph grid and the
 * button art land on the pixel grid id intended — and the picture is then
 * area-averaged down to the window's 320x240 on the way out (q3ui2d_present).
 * Reporting 320x240 instead would halve the numbers and land every glyph on a
 * half pixel, which is a visible quality loss for no measurable gain: the
 * downscale touches 76 800 pixels, the UI itself draws far fewer.
 *
 * Pixel layout is 0x00RRGGBB, byte for byte what TinyGL's PIXEL, the Mectov
 * framebuffer and every kernel theme constant use — so presenting is a copy,
 * not a swizzle (the same conclusion v38.103 reached for the ZBuffer).
 */
#ifndef Q3UI2D_H
#define Q3UI2D_H

#include <stdint.h>

/* The virtualized UI surface. */
#define Q3UI_W 640
#define Q3UI_H 480

/* 0 on success, -1 no memory / already up. Idempotent. */
int  q3ui2d_init(void);
void q3ui2d_shutdown(void);
int  q3ui2d_ready(void);

/* The surface itself (Q3UI_W x Q3UI_H, 0x00RRGGBB, top-down). For tests that
 * want to look at what the UI drew. */
uint32_t *q3ui2d_surface(void);

/* Shader registry — the UI's qhandle_t is one of these. Names come straight
 * from id's ui/*.c ("menu/art/main", "gfx/2d/bigchars", "white").
 * Resolution, in order, mirroring what id's R_FindShader does for a name with
 * no shader-script entry (every UI picture is such a name — measured: the
 * staged base.shader names no ui/ or gfx/2d image):
 *   1. the name as given, plus each of .tga / .jpg / .jpeg / .png
 *   2. the same with any known image extension stripped first (id spells some
 *      names with the extension, and "menu/art/unknownmap.jpg" exists as a jpg)
 * A name that resolves to nothing still gets a handle: id draws a NULL shader
 * as nothing, and the UI must not crash on a missing picture. Returns 0 for
 * "no such image" (still a valid handle to draw nothing with). */
int  q3ui2d_register_shader(const char *name);

/* Color for everything that follows, exactly as the UI leaves it: NULL or an
 * rgba[4] of 0..1 floats (id's own convention; the UI never uses >1). */
void q3ui2d_set_color(const float *rgba);

/* R_DrawStretchPic's 2D half: dest rect in pixels (top-left origin), source
 * sub-rect in normalized top-down texcoords, blended src-alpha over. Handles
 * rotation `rotate` by ignoring it — no 1.32 UI screen passes one — and
 * reports it through q3ui2d_stats() so that is a measurement, not a silence. */
void q3ui2d_draw_stretch_pic(float x, float y, float w, float h,
                             float s0, float t0, float s1, float t1,
                             int shader, float rotate);

/* Filled rect in the current color (UI_FillRect; the outline variant is four
 * of these in id's own code). */
void q3ui2d_fill_rect(float x, float y, float w, float h);

/* Present the surface into a 0x00RRGGBB destination `dw` x `dh` with row pitch
 * `pitch` pixels (the WM content buffer: pitch == dw, centred by the caller
 * through cx/cy). Area-average downscale — an exact 2x2 box at 2:1, which is
 * the case that matters. */
void q3ui2d_present(uint32_t *dst, int dw, int dh, int pitch);

/* Zero the per-frame counters (draws, destination pixels, rotations) so the
 * stats below describe the next frame alone. Call before UI_REFRESH. */
void q3ui2d_frame_reset(void);

/* What the last frame did: registered shaders, live cached images, cached
 * bytes, draw calls, destination pixels written, shader requests that found
 * nothing, and stretch-pics that asked for rotation. Any pointer may be NULL. */
void q3ui2d_stats(int *shaders, int *images, int *cachedBytes, int *draws,
                  int *pixels, int *missing, int *rotated);

#endif /* Q3UI2D_H */