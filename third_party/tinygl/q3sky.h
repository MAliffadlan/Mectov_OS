/* q3sky.h — id's own sky: the camera-centred cloud layer (v38.128).
 *
 * WHAT id DOES WITH A SKY LIKE q3dm1's. `scripts/sky.shader` describes
 * `textures/skies/tim_hell` as `surfaceparm sky` + `skyparms - 384 -` + two
 * stages: `killsky_1.tga` (opaque) and `killsky_2.tga` (blendFunc GL_ONE GL_ONE),
 * each with its own `tcMod scroll`/`scale`. In id's renderer (tr_sky.c's
 * RB_StageIteratorSky) the map's own sky SURFACES are never drawn as geometry:
 * they are clipped into the six sides of a sky box (`RB_ClipSkyPolygons`) only
 * to answer "which directions can this camera see sky in", and then
 * `R_BuildCloudData` throws the surface tessellation away and fills a box of
 * that shape CENTRED ON THE CAMERA, `cloudHeight` units above which the cloud
 * plane sits. The texture coordinates are not planar: every box direction is
 * traced to the cloud plane and the resulting direction becomes the coordinate
 * (R_InitSkyTexCoords' acos projection), so the layers slide and parallax the way
 * a real sky does instead of sitting still on a wall.
 *
 * That is what this module ports: the clip, the box, the projection and the
 * per-stage animation. The map's sky surfaces keep exactly one job — telling the
 * clip which sides to build — which is the job id gives them.
 *
 * WHY IT IS A BACKGROUND PASS. id draws sky fragments at the far end of the
 * depth range (`qglDepthRange(1,1)`), so anything else the level draws later
 * wins. This rasterizer has no depth-range call, but it has the same effect more
 * cheaply: the sky is emitted FIRST, with depth writes off, into a depth buffer
 * that `q3ref_begin_frame` has just cleared — so every world pixel that follows
 * overwrites it and a pixel the level does not paint stays sky.
 *
 * WHAT IS NOT HERE: id also draws two optional BOXES around the cloud layer —
 * `skyparms <farbox> <cloudheight> <nearbox>`. Both name six images
 * (`env/space1/space1_{rt,bk,lf,ft,up,dn}`) that the shader's own stages do not,
 * and this port builds neither: a definition whose sky has no cloud height keeps
 * the pre-v38.128 geometry fallback (its faces are drawn like any other
 * surface), and one that has both gets its clouds and no far box. q3dm1's
 * `skyparms - 384 -` asks for neither box, so the map this release was measured
 * on is exact; the gap is reported in the load line's `farbox=` field.
 *
 * The stage texcoords are id's own arithmetic, in id's own order: `tcMod scroll`
 * adds the fractional part of `speed * shaderTime` and `tcMod scale` multiplies
 * what is already there, applied in the order the script wrote them (ioquake3's
 * RB_CalcScrollTexCoords / RB_CalcScaleTexCoords — the GPL renderer in this
 * repo parses these keywords but carries no implementation of them).
 */
#ifndef Q3SKY_H
#define Q3SKY_H

#include <stdint.h>

#include "q3bsp.h"

#define Q3SKY_MAX_STAGES Q3BSP_MAX_STAGES

/* How many sky definitions one map may draw. id's renderer has a single sky
 * shader per batch and a retail map carries one or two (q3dm1: one; the
 * fixtures: one), so a small table costs nothing and a map past it simply gets
 * the geometry fallback for the extra definitions. */
#define Q3SKY_MAX_SKY 4

/* One stage, as the renderer needs it: which texture, how it blends, and the
 * tcMods in file order. `tex` is a GL texture id the owner allocated — the sky
 * never reads a file itself, and `q3sky_reset()` is what deletes them. */ 
typedef struct {
    unsigned int  tex;                   /* 0 = this stage had no image */
    int           additive;              /* blendFunc GL_ONE GL_ONE */
    int           numTcMods;
    unsigned char tcType[Q3BSP_MAX_TCMODS];
    float         tcA[Q3BSP_MAX_TCMODS];
    float         tcB[Q3BSP_MAX_TCMODS];
} q3sky_stage_t;

typedef struct {
    int          cloudHeight;            /* skyparms' middle operand; 0 = none */
    int          numStages;
    q3sky_stage_t stages[Q3SKY_MAX_STAGES];
    char         name[Q3BSP_MAX_NAME];
} q3sky_shader_t;

/* Register (or clear, with NULL) the sky shader a mesh's shader slot uses.
 * Registration happens at load, once the stage images are uploaded, and a
 * second registration for the SAME slot replaces the first. A definition whose
 * `skyparms` names no cloud height is not registered: it has no box to build,
 * and q3sky_draw could not answer for it. */
void q3sky_register_shader(int shaderNum, const q3sky_shader_t *sh);

/* Is this shader slot a drawable sky? (registered AND carrying a cloud layer.) */
int  q3sky_is_sky(int shaderNum);

/* Turn the whole pass off, for an A/B on one ISO (`nosky`). */
void q3sky_set_enabled(int on);
int  q3sky_enabled(void);

/* Drop every registration and delete the stage textures it owned. Called by the
 * world module's unload, which owns the mesh lifetime; the lookup tables are
 * freed with it. */
void q3sky_reset(void);

/* Build and draw the cloud layer for one frame.
 *
 *   m       the loaded mesh (its faces and verts are what gets clipped)
 *   faces   the indices of the faces whose shader is a sky, gathered by the
 *           world draw's own cull pass — already PVS/frustum filtered, which is
 *           exactly the set id clips (`RB_ClipSkyPolygons` runs on the tess of
 *           the visible sky surfaces)
 *   n       how many
 *   cam     the eye, which is also the centre of the box
 *   timeSec the game clock in seconds (id's refdef.floatTime)
 *   zFar    the projection's far plane: the box is zFar/1.75 across, as id's
 *           MakeSkyVec sizes it
 *
 * Called by q3w_draw, before the opaque pass, with depth writes off. Every face
 * handed to it is counted as a drawn face by the caller — the box replaces the
 * geometry the face would otherwise have contributed. */
void q3sky_draw(const q3bsp_mesh_t *m, const int *faces, int n,
                const float cam[3], float timeSec, float zFar);

/* Load-time and per-frame accounting. Any pointer may be NULL:
 *   registered   sky shaders registered at load (0 or 1 in practice)
 *   cloud        the cloudHeight the last frame built its box from
 *   stages       stages with an image the last frame drew
 *   sides        box sides the last frame filled
 *   tris         box triangles the last frame submitted
 *   trisRun      run total of those
 * The faces that carried a sky shader WITHOUT a cloud layer never reach this
 * module: the world draw keeps them and reports them itself. */
void q3sky_stats(int *registered, int *cloud, int *stages, int *sides,
                 int *tris, int *trisRun);

#endif /* Q3SKY_H */
