/* q3hud.h — id's own Quake III status bar, drawn from id's own images (v38.127).
 *
 * The port had no HUD at all: a crosshair of its own and, since v38.126, one
 * fps number in the corner. What the reference screenshot of retail q3dm1 shows
 * at the bottom of the screen is CG_DrawStatusBar() from id's cgame — ammo,
 * health, armor and the FFA score boxes, every one of them a picture out of
 * gfx/2d and icons/ in pak0. This module is that function, ported.
 *
 * It is deliberately NOT a GL client: TinyGL's only blend mode is additive
 * (see TGL_BLEND_FUNC), and every HUD picture is 32-bit RGBA meant to be
 * alpha-blended. So the HUD bypasses the rasterizer entirely and composites
 * itself into the finished 320x240 ZBuffer, exactly the way the perf overlay
 * and the window manager's own blit already do — which also means the HUD
 * costs the world pass nothing.
 *
 * The layout numbers are id's, not the port's: the status bar is authored in
 * Q3's 640x480 virtual screen (cg_local.h's CHAR_WIDTH 32 / CHAR_HEIGHT 48 /
 * ICON_SIZE 48 / TEXT_ICON_SPACE 4, q_shared.h's BIGCHAR 16x16) and scaled to
 * whatever the renderer is actually drawing at, the way CG_AdjustFrom640 does.
 */
#ifndef Q3HUD_H
#define Q3HUD_H

#include <stdint.h>

/* The values CG_DrawStatusBar() reads, straight out of the game module's
 * playerState — the driver passes them, this module never touches the VM.
 *
 *   health/armor  ps->stats[STAT_HEALTH] / ps->stats[STAT_ARMOR]  (bg_public.h)
 *   ammo          ps->ammo[ps->weapon], -1 when the weapon takes none
 *   weapon        ps->weapon, bg_public.h's WP_* number: it picks the ammo icon
 *   score         ps->persistant[PERS_SCORE], -1 = SCORE_NOT_PRESENT
 *   firing        ps->weaponstate == WEAPON_FIRING, which greys the ammo count
 *   now_ms        the frame clock: health below 26 flashes in id's table
 *
 * Any value may be passed as -1 to mean "id would not draw this field". */
void q3hud_set(int health, int armor, int ammo, int weapon, int score,
               int firing, int now_ms);

/* Composite the status bar into a finished 0x00RRGGBB framebuffer. Called by
 * the driver after the frame histogram has been read (so the suites' pixel
 * evidence stays the 3D pass and nothing else) and before the frame is
 * presented. Images are loaded from the engine volume on the first call and
 * cached; a volume without them draws nothing and says so, once. */
void q3hud_draw(uint32_t *px, int pitch, int w, int h);

/* Release the cached images (the meshes' lifetime, not a frame's). */
void q3hud_unload(void);

/* Load-time and run totals. Any pointer may be NULL:
 *   images    pictures decoded off the volume
 *   missing   pictures id's status bar names that this volume does not have
 *   draws     pictures composited (run total)
 *   digits    number glyphs composited (run total) */
void q3hud_stats(int *images, int *missing, int *draws, int *digits);

#endif /* Q3HUD_H */
