/* q3viewmodel.h — id's own weapon view model, drawn as geometry (v38.124).
 *
 * Until v38.123 the gun in the bottom-right corner was a silhouette this port
 * drew by hand, because the staged data set carried no weapon models. That was
 * a wiring answer to what was actually a content question: the machinegun view
 * model IS available — it is in the freely distributable Quake III Arena demo
 * pak0.pk3, at models/weapons2/machinegun/ — and what was missing was a loader
 * for the format it is stored in. This module is that loader, plus the pass
 * that draws it.
 *
 * What id ships for one weapon is three .md3 files chained by their own tags:
 *
 *   machinegun.md3          the body; tag_weapon is the grip (the origin),
 *                           tag_barrel is where the barrel part attaches
 *   machinegun_barrel.md3   the barrel, whose tag_flash is the muzzle
 *   machinegun_flash.md3    the muzzle flash, a 24-triangle star centred on
 *                           its origin, drawn only while the weapon fires
 *
 * The demo's machinegun_hand.md3 is deliberately NOT used: it carries 16
 * animation frames and three tags but zero surfaces (verified against the
 * file), so the hands a retail first-person view shows come from the player
 * model (models/players/<model>/upper.md3) — a much larger piece of work this
 * module does not pretend to have done. What is here is id's own gun, at id's
 * own proportions, with id's own textures.
 *
 * Split from q3cl_render.c for the usual reason in this directory: the backend
 * owns the ZBuffer, the projection and the camera; this module owns a piece of
 * geometry with its own lifetime (loaded once when the weapon is known) and its
 * own textures, and the two meet at four functions.
 *
 * Everything here is freestanding (same rules as q3world_render.c: no libc, no
 * printf, no 64-bit divide): the serial lines it emits are assembled by hand.
 */
#ifndef Q3VIEWMODEL_H
#define Q3VIEWMODEL_H

/* Load `base` (a qpath with NO extension, e.g.
 * "models/weapons2/machinegun/machinegun") plus its `_barrel` and `_flash`
 * parts. Returns the number of parts loaded: 0 means the volume has no view
 * model for this weapon and the caller should draw its fallback; 1..3 is a
 * usable model (the body is always part 1). Logs one line either way, so a
 * fallback can never be mistaken for a loaded model. */
int  q3vm_load(const char *base);
void q3vm_unload(void);
int  q3vm_loaded(void);

/* Per-frame state, read straight out of the module's playerState by the
 * driver: `firing` is weaponstate == WEAPON_FIRING (the only thing that draws
 * the flash), `grounded` and `bob` drive the walk bob and the air lift. */
void q3vm_set_state(int firing, int grounded, int bob);

/* Draw the model as its own GL pass: depth is cleared first, so the weapon is
 * never clipped by the world it stands in (id does the same with a depth range
 * the hardware here does not have), and the projection is left exactly as the
 * frame set it up — this port's 90 degree horizontal FOV at 4:3 is id's own
 * default, which is why no scale factor appears anywhere below.
 *
 * `tan_x`/`tan_y` are the projection's half-extents (the backend's
 * proj_tan_x/proj_tan_y) and `w`/`h` the viewport, used to report the model's
 * screen box. Returns 1 if geometry was drawn, 0 if there is nothing loaded.
 * The box is written to `box` as x0,y0,x1,y1 in framebuffer pixels (top-down);
 * any pointer may be NULL. */
int  q3vm_draw(float tan_x, float tan_y, int w, int h, int *box);

/* What the last q3vm_load parsed: parts loaded, surfaces and triangles across
 * them, and the body's texture size. Any pointer may be NULL. */
void q3vm_stats(int *parts, int *surfaces, int *tris, int *texw, int *texh);

/* Triangles the last q3vm_draw actually submitted. Equal to the loaded count
 * whenever the whole model was drawn — a loader that parsed geometry and a
 * pass that emits none is the "in the data, not on the screen" failure, and
 * this is the number that separates the two. */
int  q3vm_drawn_tris(void);

/* v38.125: 1 = the last q3vm_draw submitted the muzzle flash ADDITIVELY
 * (glBlendFunc(GL_ONE, GL_ONE)) — id's own instruction for
 * models/weapons2/machinegun/f_machinegun in models.shader — 0 = no flash was
 * drawn. Reported separately from the triangle count because the first version
 * of this pass drew the flash opaque, and the only thing wrong with it on
 * screen was how it was composited: a black surround over the whole view. */
int  q3vm_flash_mode(void);

#endif /* Q3VIEWMODEL_H */
