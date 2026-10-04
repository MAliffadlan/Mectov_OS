/* q3sky.c — id's own sky: the clip, the cloud box, and the layers' animation
 * (v38.128).
 *
 * WHAT THIS FILE IS. tr_sky.c, ported: the map's sky surfaces are clipped into
 * the six sides of a sky box to find out which directions the camera can see sky
 * in (RB_ClipSkyPolygons / ClipSkyPolygon / AddSkyPolygon), and then a box of
 * that shape CENTRED ON THE CAMERA is filled and drawn, one cloud layer per
 * shader stage, each layer's texture coordinates animated by its own tcMods
 * (R_BuildCloudData / FillCloudBox / FillCloudySkySide, and q3sky.h's header for
 * why this is a background pass). Nothing here reads a file or knows about the
 * engine: the caller hands over a mesh, the indices of the sky faces its own cull
 * pass kept, the eye, the clock and the far plane.
 *
 * THE THREE PIECES OF ID'S ARITHMETIC THAT MAKE IT A SKY RATHER THAN A WALL:
 *
 *  1. The clip (AddSkyPolygon) does not draw anything. It picks, per polygon,
 *     the box SIDE the polygon's summed direction points at (vec_to_st's axis
 *     choice) and accumulates that side's s/t bounds — so the bounds are the
 *     angular EXTENT of the sky the level actually exposes, and everything
 *     outside them is left unpainted. That is why a room with one sky ceiling
 *     paints sky only where the ceiling is.
 *  2. The box is the camera's, not the map's: FillCloudySkySide adds
 *     viewParms.or.origin to every generated point, so walking moves the sky
 *     with you and the clouds never get nearer — id's `MakeSkyVec`'s box is
 *     zFar/1.75 across, i.e. its corners land exactly on the far plane.
 *  3. The coordinates are NOT the box sides' planar projection. Every grid point
 *     is a direction; R_InitSkyTexCoords traces that direction out to a sphere of
 *     radiusWorld (4096) that has been raised by cloudHeight, and takes the arc
 *     cosines of the intersection's x and y — an angular coordinate pair that
 *     makes a flat layer read as a dome and slide, parallax and all, as the
 *     camera turns. It is computed once per cloud height, at load, with the zFar
 *     id hard-codes there (1024): the table belongs to the shader, not the view.
 *
 * WHAT IS DELIBERATELY DIFFERENT FROM id, and why it leaves the same pixels:
 *
 *  - No GL_TRIANGLE_STRIP count bookkeeping on the fill side. id builds the
 *    cloud grid into its shared tess arrays and lets RB_StageIteratorGeneric
 *    index them; here each side is emitted as one row strip per grid row, which
 *    covers exactly the quads FillCloudySkySide's index list covers.
 *  - The depth RANGE (`qglDepthRange(1,1)`) becomes "no depth test at all": the
 *    pass runs first, into a freshly cleared buffer, so there is nothing behind
 *    it to be beaten by. See q3sky.h.
 *  - tcMod math is id's own, in id's own order and in place (ioquake3's
 *    RB_CalcScrollTexCoords / RB_CalcScaleTexCoords, tr_shade_calc.c), with
 *    `tess.shaderTime = refdef.floatTime - shader->timeOffset` collapsed to the
 *    game clock because a sky shader has no timeOffset.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <math.h>              /* the kernel's stub: q3_sqrtf / q3_acos / q3_floor */
#include <TGL/gl.h>

#include "q3sky.h"

/* id's subdivision counts (tr_sky.c:26-27): the box side is an
 * 8x8 grid of quads, and every bound is rounded onto the 1/4 grid so the fill
 * only ever covers whole cells. */
#define SKY_SUBDIVISIONS      8
#define HALF_SKY_SUBDIVISIONS (SKY_SUBDIVISIONS / 2)

/* tr_sky.c:79-80. */
#define SKY_ON_EPSILON     0.1f
#define SKY_MAX_CLIP_VERTS 64

/* --- id's tables, verbatim ------------------------------------------------
 *
 * sky_clip is the six clip planes in the CYLINDER's space: a direction is
 * clipped against them one at a time, and a polygon that survives all six is on
 * the front side of every one — i.e. inside the 90-degree-per-axis cone id draws
 * sky for. vec_to_st turns a surviving polygon's direction into that side's s/t;
 * st_to_vec is the inverse, turning a grid coordinate back into a direction when
 * the box is generated. Both tables' zero-heavy shape is id's: the "2" entries
 * mean "use the box's own axis as the divisor", which is what makes the
 * projection planar across a side. */
static float sky_clip[6][3] = {
    { 1,  1, 0},
    { 1, -1, 0},
    { 0, -1, 1},
    { 0,  1, 1},
    { 1,  0, 1},
    {-1,  0, 1}
};

static int vec_to_st[6][3] = {
    {-2,  3,  1},
    { 2,  3, -1},

    { 1,  3,  2},
    {-1,  3, -2},

    {-2, -1,  3},
    {-2,  1, -3}
};

static int st_to_vec[6][3] = {
    { 3, -1,  2},
    {-3,  1,  2},

    { 1,  3,  2},
    {-1, -3,  2},

    {-2, -1,  3},        /* 0 degrees yaw, look straight up   */
    { 2, -1, -3}         /* look straight down                */
};

/* sky_mins[0..1][side] / sky_maxs[0..1][side]: the clipped extent of the sky on
 * each side, in that side's s and t. Cleared by clear_sky_box() and grown by
 * AddSkyPolygon. They live across the clip and the fill of one shader per frame. */
static float sky_mins[2][6], sky_maxs[2][6];

/* The cloud texture coordinates (R_InitSkyTexCoords): one for every grid point
 * of every side. `sky_cloud_height` is the height the table was built for, -1
 * before the first build. */
static float sky_cloud_tex[6][SKY_SUBDIVISIONS + 1][SKY_SUBDIVISIONS + 1][2];
static int   sky_cloud_height = -1;

/* The box points of the side being filled (FillCloudBox's MakeSkyVec loop), in
 * the same 0..SKY_SUBDIVISIONS index space as the table above so the two are
 * read with one pair of indices. Camera-relative: the caller's eye is added when
 * a vertex is emitted, exactly as FillCloudySkySide does it. */
static float sky_points[SKY_SUBDIVISIONS + 1][SKY_SUBDIVISIONS + 1][3];

/* --- the registry ---------------------------------------------------------
 *
 * One entry per shader slot the world load registered, and it is a scan of at
 * most Q3SKY_MAX_SKY int compares — cheap enough to run per face in the world's
 * cull pass, which is what q3sky_is_sky is for. */
static int            sky_reg_n;
static int            sky_reg_num[Q3SKY_MAX_SKY];
static q3sky_shader_t sky_reg[Q3SKY_MAX_SKY];

static int sky_on = 1;

/* Stats (see q3sky_stats). The run totals only ever grow. */
static int sky_st_cloud;
static int sky_st_stages, sky_st_sides, sky_st_tris;
static int sky_st_tris_run;

int q3sky_enabled(void) { return sky_on; }
void q3sky_set_enabled(int on) { sky_on = on ? 1 : 0; }

int q3sky_is_sky(int shaderNum) {
    int k;
    for (k = 0; k < sky_reg_n; k++) {
        if (sky_reg_num[k] == shaderNum)
            return sky_reg[k].cloudHeight > 0 && sky_reg[k].numStages > 0;
    }
    return 0;
}

void q3sky_register_shader(int shaderNum, const q3sky_shader_t *sh) {
    int k;

    if (shaderNum < 0 || shaderNum >= Q3BSP_MAX_SHADERS) return;

    for (k = 0; k < sky_reg_n; k++) {          /* a re-registration replaces */
        if (sky_reg_num[k] != shaderNum) continue;
        if (!sh) {
            int j;
            for (j = k; j + 1 < sky_reg_n; j++) {
                sky_reg[j] = sky_reg[j + 1];
                sky_reg_num[j] = sky_reg_num[j + 1];
            }
            sky_reg_n--;
            return;
        }
        sky_reg[k] = *sh;
        return;
    }
    if (!sh) return;                            /* nothing to clear */
    /* No cloud layer, no box: q3sky_is_sky has to answer no, so there is no
     * point keeping the entry, and the world draw's own fallback accounting
     * reports those faces (it is the world that decides to draw them). */
    if (sh->cloudHeight <= 0 || sh->numStages <= 0) return;
    if (sky_reg_n >= Q3SKY_MAX_SKY) return;     /* past the cap: geometry fallback */
    sky_reg[sky_reg_n] = *sh;
    sky_reg_num[sky_reg_n] = shaderNum;
    sky_reg_n++;
}

void q3sky_reset(void) {
    int k, st;
    for (k = 0; k < sky_reg_n; k++) {
        for (st = 0; st < sky_reg[k].numStages && st < Q3SKY_MAX_STAGES; st++) {
            if (sky_reg[k].stages[st].tex) {
                GLuint id = sky_reg[k].stages[st].tex;
                glDeleteTextures(1, &id);
                sky_reg[k].stages[st].tex = 0;
            }
        }
    }
    sky_reg_n = 0;
    sky_cloud_height = -1;                      /* the table belongs to a mesh */
    sky_st_cloud = sky_st_stages = sky_st_sides = sky_st_tris = 0;
    sky_st_tris_run = 0;
}

/* --- MakeSkyVec (tr_sky.c:262) -------------------------------------------
 *
 * A grid coordinate s,t in -1..1 on side `axis` becomes a direction in the box
 * space: b is the (s,t,1) corner scaled by the box's half-extent, and the
 * side's st_to_vec row picks which box axis each output component comes from,
 * negating where the table says so.
 *
 * id's function also computes the OUTER box's planar texcoords here (the
 * sky_min/sky_max clamp and the 1-t flip). This port builds no outer box (see
 * q3sky.h), and the cloud layer's coordinates come from the acos table instead,
 * so only the point half is ported. */
static void make_sky_point(float s, float t, int axis, float out[3], float boxSize) {
    float b[3];
    int j, k;

    b[0] = s * boxSize;
    b[1] = t * boxSize;
    b[2] = boxSize;
    for (j = 0; j < 3; j++) {
        k = st_to_vec[axis][j];
        out[j] = (k < 0) ? -b[-k - 1] : b[k - 1];
    }
}

/* --- AddSkyPolygon (tr_sky.c:47) -----------------------------------------
 *
 * Which side does this polygon belong to, and how far does it reach across it?
 * The side is chosen from the SUM of the polygon's directions (a polygon is
 * small in angle and its sum points at its middle), and every vertex is then
 * projected into that side's s/t by dividing two components of the direction —
 * dv is the component along the side's own axis, and the 0.001 test is id's
 * guard for a direction parallel to the side. */
static void add_sky_polygon(int nump, float *vecs) {
    float v[3], av[3];
    float s, t, dv;
    int axis, i, j;

    v[0] = v[1] = v[2] = 0.0f;
    for (i = 0; i < nump; i++) {
        v[0] += vecs[i * 3 + 0];
        v[1] += vecs[i * 3 + 1];
        v[2] += vecs[i * 3 + 2];
    }
    for (j = 0; j < 3; j++) av[j] = (v[j] < 0.0f) ? -v[j] : v[j];

    if (av[0] > av[1] && av[0] > av[2])
        axis = (v[0] < 0.0f) ? 1 : 0;
    else if (av[1] > av[2] && av[1] > av[0])
        axis = (v[1] < 0.0f) ? 3 : 2;
    else
        axis = (v[2] < 0.0f) ? 5 : 4;

    for (i = 0; i < nump; i++) {
        float *p = vecs + i * 3;
        j = vec_to_st[axis][2];
        dv = (j > 0) ? p[j - 1] : -p[-j - 1];
        if (dv < 0.001f)
            continue;                    /* don't divide by zero */
        j = vec_to_st[axis][0];
        s = (j < 0) ? -p[-j - 1] / dv : p[j - 1] / dv;
        j = vec_to_st[axis][1];
        t = (j < 0) ? -p[-j - 1] / dv : p[j - 1] / dv;

        if (s < sky_mins[0][axis]) sky_mins[0][axis] = s;
        if (t < sky_mins[1][axis]) sky_mins[1][axis] = t;
        if (s > sky_maxs[0][axis]) sky_maxs[0][axis] = s;
        if (t > sky_maxs[1][axis]) sky_maxs[1][axis] = t;
    }
}

/* --- ClipSkyPolygon (tr_sky.c:82) ----------------------------------------
 *
 * Clip one polygon against the six sky planes, one plane per recursion level,
 * and hand every surviving piece to AddSkyPolygon at stage 6. id passes BOTH the
 * front and the back half down to the next plane rather than keeping the front
 * one, so the pieces AddSkyPolygon sees tile the polygon's whole angular
 * neighbourhood; that is why a big sky surface over-fills its side rather than
 * leaving a hole at the edges, which for a background layer is the right way to
 * be wrong.
 *
 * `vecs` needs one vertex MORE than nump: the clip copies vertex 0 into the
 * closing slot (exactly as id does) so the wraparound edge can be tested. The
 * 64-vertex cap is id's MAX_CLIP_VERTS — a triangle can only grow by one vertex
 * per plane, so a caller never gets near it and a hostile file cannot blow the
 * stack. */
static void clip_sky_polygon(int nump, float *vecs, int stage) {
    float *norm;
    float dists[SKY_MAX_CLIP_VERTS];
    int   sides[SKY_MAX_CLIP_VERTS];
    float newv[2][SKY_MAX_CLIP_VERTS][3];
    int   newc[2];
    int   front, back, i, j;

    if (nump > SKY_MAX_CLIP_VERTS - 2) {
        /* id calls ri.Error here; a dropped polygon is the honest kernel
         * equivalent — it can only mean a corrupt file, and the log's sky line
         * would show the missing side. */
        return;
    }
    if (stage == 6) {
        add_sky_polygon(nump, vecs);
        return;
    }

    front = back = 0;
    norm = sky_clip[stage];
    for (i = 0; i < nump; i++) {
        float d = vecs[i * 3 + 0] * norm[0] + vecs[i * 3 + 1] * norm[1] +
                  vecs[i * 3 + 2] * norm[2];
        if (d > SKY_ON_EPSILON) { front = 1; sides[i] = 1; }
        else if (d < -SKY_ON_EPSILON) { back = 1; sides[i] = -1; }
        else sides[i] = 0;
        dists[i] = d;
    }

    if (!front || !back) {              /* not clipped: try the next plane */
        clip_sky_polygon(nump, vecs, stage + 1);
        return;
    }

    sides[nump] = sides[0];
    dists[nump] = dists[0];
    for (j = 0; j < 3; j++) vecs[nump * 3 + j] = vecs[j];
    newc[0] = newc[1] = 0;

    for (i = 0; i < nump; i++) {
        float *v = vecs + i * 3;
        if (sides[i] == 1) {
            for (j = 0; j < 3; j++) newv[0][newc[0]][j] = v[j];
            newc[0]++;
        } else if (sides[i] == -1) {
            for (j = 0; j < 3; j++) newv[1][newc[1]][j] = v[j];
            newc[1]++;
        } else {
            for (j = 0; j < 3; j++) {
                newv[0][newc[0]][j] = v[j];
                newv[1][newc[1]][j] = v[j];
            }
            newc[0]++;
            newc[1]++;
        }

        if (sides[i] == 0 || sides[i + 1] == 0 || sides[i + 1] == sides[i])
            continue;

        {
            float d = dists[i] / (dists[i] - dists[i + 1]);
            for (j = 0; j < 3; j++) {
                float e = v[j] + d * (v[j + 3] - v[j]);
                newv[0][newc[0]][j] = e;
                newv[1][newc[1]][j] = e;
            }
            newc[0]++;
            newc[1]++;
        }
    }

    clip_sky_polygon(newc[0], newv[0][0], stage + 1);
    clip_sky_polygon(newc[1], newv[1][0], stage + 1);
}

static void clear_sky_box(void) {
    int i;
    for (i = 0; i < 6; i++) {
        sky_mins[0][i] = sky_mins[1][i] = 9999.0f;
        sky_maxs[0][i] = sky_maxs[1][i] = -9999.0f;
    }
}

/* RB_ClipSkyPolygons (tr_sky.c:180): every triangle of the visible sky
 * surfaces, relative to the view origin — id clips the geometry the camera can
 * see, which is what makes the filled sides follow the level's shape. The
 * loader's faces are triangle fans, and emit_face walks them as (0,k,k+1), so
 * this does too. */
static void clip_sky_face(const q3bsp_mesh_t *m, const q3bsp_face_t *f,
                          const float cam[3]) {
    float p[4][3];                      /* one extra point for clipping */
    const q3bsp_vert_t *v0;
    int nv = f->numVerts, k, j;

    if (nv < 3) return;
    v0 = &m->verts[f->firstVert];
    for (k = 1; k + 1 < nv; k++) {
        const q3bsp_vert_t *a = &v0[0];
        const q3bsp_vert_t *b = &v0[k];
        const q3bsp_vert_t *c = &v0[k + 1];
        const q3bsp_vert_t *tv[3];
        tv[0] = a; tv[1] = b; tv[2] = c;
        for (j = 0; j < 3; j++) {
            p[j][0] = tv[j]->xyz[0] - cam[0];
            p[j][1] = tv[j]->xyz[1] - cam[1];
            p[j][2] = tv[j]->xyz[2] - cam[2];
        }
        clip_sky_polygon(3, p[0], 0);
    }
}

/* --- R_InitSkyTexCoords (tr_sky.c:621) -----------------------------------
 *
 * The projection that makes the layers a dome. For every grid point of every
 * side: take the direction, find where the ray from the eye meets a sphere of
 * radius radiusWorld whose centre is cloudHeight above the eye (the quadratic
 * solved for the parametric distance p), push that intersection up by
 * radiusWorld so it is measured from the world's centre, normalize, and take the
 * arc cosines of x and y. The result is an angular coordinate — 0..pi across
 * each axis — so a scroll of 0.05 units/second moves the SAME amount in texture
 * space everywhere on the dome while the on-screen step varies with the viewing
 * angle, which is what a cloud layer does and a planar projection cannot.
 *
 * zFar is fixed at 1024 here (id's own line) because this table is a property of
 * the cloud height, computed once at shader parse and then reused for every
 * frame at any zFar. */
static void init_sky_tex_coords(int height) {
    const float radiusWorld = 4096.0f;
    const float boxSize = 1024.0f / 1.75f;
    const float h = (float)height;
    int i, s, t;

    sky_cloud_height = height;
    for (i = 0; i < 6; i++) {
        for (t = 0; t <= SKY_SUBDIVISIONS; t++) {
            for (s = 0; s <= SKY_SUBDIVISIONS; s++) {
                float skyVec[3], v[3], p, v2, under, len;

                make_sky_point((float)(s - HALF_SKY_SUBDIVISIONS) /
                                   (float)HALF_SKY_SUBDIVISIONS,
                               (float)(t - HALF_SKY_SUBDIVISIONS) /
                                   (float)HALF_SKY_SUBDIVISIONS,
                               i, skyVec, boxSize);

                v2 = skyVec[0] * skyVec[0] + skyVec[1] * skyVec[1] +
                     skyVec[2] * skyVec[2];
                /* id's expression, term for term (the SQR(x2)*SQR(R) lead and
                 * then 2*SQR(x)*R*h + SQR(x)*SQR(h) for each axis). */
                under = skyVec[2] * skyVec[2] * radiusWorld * radiusWorld
                      + 2.0f * skyVec[0] * skyVec[0] * radiusWorld * h
                      + skyVec[0] * skyVec[0] * h * h
                      + 2.0f * skyVec[1] * skyVec[1] * radiusWorld * h
                      + skyVec[1] * skyVec[1] * h * h
                      + 2.0f * skyVec[2] * skyVec[2] * radiusWorld * h
                      + skyVec[2] * skyVec[2] * h * h;
                if (under < 0.0f) under = 0.0f;
                p = (1.0f / (2.0f * v2)) *
                    (-2.0f * skyVec[2] * radiusWorld + 2.0f * sqrtf(under));

                v[0] = skyVec[0] * p;
                v[1] = skyVec[1] * p;
                v[2] = skyVec[2] * p + radiusWorld;
                len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (len > 0.0f) { v[0] /= len; v[1] /= len; v[2] /= len; }

                sky_cloud_tex[i][t][s][0] = (float)acos((double)v[0]);
                sky_cloud_tex[i][t][s][1] = (float)acos((double)v[1]);
            }
        }
    }
}

/* --- the per-stage texcoord transform ------------------------------------
 *
 * ioquake3's RB_CalcScrollTexCoords / RB_CalcScaleTexCoords, applied in the
 * order the shader script wrote them (tr_shade.c's stage loop dispatches tcMods
 * in file order and each one edits the coordinates in place). Scroll takes the
 * FRACTIONAL part of speed*time before adding it, so a long session cannot drift
 * the coordinates into a range the texture unit cannot represent.
 */
static void sky_stage_texcoord(const q3sky_stage_t *stg, float timeSec,
                               int axis, int t, int s, float out[2]) {
    float u = sky_cloud_tex[axis][t][s][0];
    float v = sky_cloud_tex[axis][t][s][1];
    int m;

    for (m = 0; m < stg->numTcMods && m < Q3BSP_MAX_TCMODS; m++) {
        if (stg->tcType[m] == Q3BSP_TCMOD_SCROLL) {
            float ds = stg->tcA[m] * timeSec;
            float dt = stg->tcB[m] * timeSec;
            ds -= (float)floor((double)ds);
            dt -= (float)floor((double)dt);
            u += ds;
            v += dt;
        } else if (stg->tcType[m] == Q3BSP_TCMOD_SCALE) {
            u *= stg->tcA[m];
            v *= stg->tcB[m];
        }
    }
    out[0] = u;
    out[1] = v;
}

/* One vertex of the cloud box: the texture coordinate from the acos table (as
 * the stage's tcMods leave it) and the point, camera-centred. */
static void emit_sky_vert(const q3sky_stage_t *stg, float timeSec, int axis,
                          int t, int s, const float cam[3]) {
    float st[2];
    sky_stage_texcoord(stg, timeSec, axis, t, s, st);
    glTexCoord2f(st[0], st[1]);
    glVertex3f(cam[0] + sky_points[t][s][0],
               cam[1] + sky_points[t][s][1],
               cam[2] + sky_points[t][s][2]);
}

/* --- FillCloudBox / FillCloudySkySide (tr_sky.c:435, :398) ---------------
 *
 * Fill the box's sides that the clip says are visible and draw them, layer by
 * layer.
 *
 * The bounds are id's, including the two clamps that make the fill safe: every
 * value is rounded outward onto the half-subdivision grid (floor for the mins,
 * ceil for the maxs) and then clamped to ±HALF_SKY_SUBDIVISIONS — the extent of
 * one side's own grid, since s_skyPoints is indexed by grid cell. The bottom
 * side (i == 5) is skipped outright: id's `MIN_T = -HALF` branch draws the
 * ceiling and the four walls of the cloud box but never the floor, because
 * clouds below you read as a grey slab under the level.
 *
 * A side whose clipped extent is empty (mins >= maxs — including a side the clip
 * never touched, which is still at ±9999) is skipped, so the box that gets drawn
 * is exactly the box the level's sky geometry described.
 *
 * Stages: the opaque base layer first, then each additive layer on top. id's
 * RB_BuildCloudData walks the shader's stages and FillCloudySkySide adds the
 * INDEXES only for stage 0 (the geometry is shared by every stage), so the box
 * is one mesh drawn once per stage with that stage's texture and tcMods. TinyGL
 * can only blend additively (src/misc.c's TGL_BLEND_FUNC), which is exactly what
 * a `blendFunc GL_ONE GL_ONE` cloud layer wants.
 */
static void fill_cloud_box(const q3sky_shader_t *sh, const float cam[3],
                           float boxSize, float timeSec) {
    unsigned drewmask = 0;
    int i, stage;
    int nst = sh->numStages;
    if (nst > Q3SKY_MAX_STAGES) nst = Q3SKY_MAX_STAGES;

    for (i = 0; i < 6; i++) {
        int mins_subd[2], maxs_subd[2];
        float sm0, sm1, sx0, sx1;
        int s0, s1, t0, t1, t, s;
        int side_drew = 0;

        if (i == 5) continue;                    /* never the bottom */

        /* id's rounding + the emptiness test, on the RAW clip bounds (a side
         * the clip missed is still at 9999/-9999 and drops out here). */
        sm0 = (float)floor((double)sky_mins[0][i] * HALF_SKY_SUBDIVISIONS) /
              (float)HALF_SKY_SUBDIVISIONS;
        sm1 = (float)floor((double)sky_mins[1][i] * HALF_SKY_SUBDIVISIONS) /
              (float)HALF_SKY_SUBDIVISIONS;
        sx0 = (float)ceil((double)sky_maxs[0][i] * HALF_SKY_SUBDIVISIONS) /
              (float)HALF_SKY_SUBDIVISIONS;
        sx1 = (float)ceil((double)sky_maxs[1][i] * HALF_SKY_SUBDIVISIONS) /
              (float)HALF_SKY_SUBDIVISIONS;
        if (sm0 >= sx0 || sm1 >= sx1) continue;

        mins_subd[0] = (int)(sm0 * HALF_SKY_SUBDIVISIONS);
        mins_subd[1] = (int)(sm1 * HALF_SKY_SUBDIVISIONS);
        maxs_subd[0] = (int)(sx0 * HALF_SKY_SUBDIVISIONS);
        maxs_subd[1] = (int)(sx1 * HALF_SKY_SUBDIVISIONS);

        if (mins_subd[0] < -HALF_SKY_SUBDIVISIONS) mins_subd[0] = -HALF_SKY_SUBDIVISIONS;
        else if (mins_subd[0] > HALF_SKY_SUBDIVISIONS) mins_subd[0] = HALF_SKY_SUBDIVISIONS;
        if (mins_subd[1] < -HALF_SKY_SUBDIVISIONS) mins_subd[1] = -HALF_SKY_SUBDIVISIONS;
        else if (mins_subd[1] > HALF_SKY_SUBDIVISIONS) mins_subd[1] = HALF_SKY_SUBDIVISIONS;
        if (maxs_subd[0] < -HALF_SKY_SUBDIVISIONS) maxs_subd[0] = -HALF_SKY_SUBDIVISIONS;
        else if (maxs_subd[0] > HALF_SKY_SUBDIVISIONS) maxs_subd[0] = HALF_SKY_SUBDIVISIONS;
        if (maxs_subd[1] < -HALF_SKY_SUBDIVISIONS) maxs_subd[1] = -HALF_SKY_SUBDIVISIONS;
        else if (maxs_subd[1] > HALF_SKY_SUBDIVISIONS) maxs_subd[1] = HALF_SKY_SUBDIVISIONS;

        /* The side's grid, in the shared 0..8 index space (id offsets by
         * HALF_SKY_SUBDIVISIONS exactly like this, and reuses the array for the
         * next side — every index touched is written here before it is read). */
        t0 = mins_subd[1] + HALF_SKY_SUBDIVISIONS;
        t1 = maxs_subd[1] + HALF_SKY_SUBDIVISIONS;
        s0 = mins_subd[0] + HALF_SKY_SUBDIVISIONS;
        s1 = maxs_subd[0] + HALF_SKY_SUBDIVISIONS;
        if (s0 < 0) s0 = 0;
        if (t0 < 0) t0 = 0;
        if (s1 > SKY_SUBDIVISIONS) s1 = SKY_SUBDIVISIONS;
        if (t1 > SKY_SUBDIVISIONS) t1 = SKY_SUBDIVISIONS;

        for (t = t0; t <= t1; t++) {
            for (s = s0; s <= s1; s++) {
                make_sky_point((float)(s - HALF_SKY_SUBDIVISIONS) /
                                   (float)HALF_SKY_SUBDIVISIONS,
                               (float)(t - HALF_SKY_SUBDIVISIONS) /
                                   (float)HALF_SKY_SUBDIVISIONS,
                               i, sky_points[t][s], boxSize);
            }
        }

        for (stage = 0; stage < nst; stage++) {
            const q3sky_stage_t *stg = &sh->stages[stage];
            if (!stg->tex) continue;
            if (side_drew == 0) {
                sky_st_sides++;
                side_drew = 1;
            }
            drewmask |= 1u << stage;

            if (stg->additive) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE);
            } else {
                glDisable(GL_BLEND);
            }
            glBindTexture(GL_TEXTURE_2D, stg->tex);

            /* One triangle strip per row of the grid — the quads id's index
             * list would draw, in the same order, with one glBegin per row. */
            for (t = t0; t < t1; t++) {
                glBegin(GL_TRIANGLE_STRIP);
                for (s = s0; s <= s1; s++) {
                    emit_sky_vert(stg, timeSec, i, t, s, cam);
                    emit_sky_vert(stg, timeSec, i, t + 1, s, cam);
                }
                glEnd();
                sky_st_tris += 2 * (s1 - s0);
                sky_st_tris_run += 2 * (s1 - s0);
            }
        }
    }

    {
        int n = 0;
        for (stage = 0; stage < nst; stage++)
            if (drewmask & (1u << stage)) n++;
        sky_st_stages += n;
    }
}

void q3sky_draw(const q3bsp_mesh_t *m, const int *faces, int n,
                const float cam[3], float timeSec, float zFar) {
    float boxSize;
    int k;

    sky_st_cloud = 0;
    sky_st_stages = sky_st_sides = sky_st_tris = 0;

    if (!m || !m->valid || !faces || n <= 0 || !cam) return;
    if (!sky_on || sky_reg_n <= 0) return;
    if (zFar <= 0.0f) zFar = 4096.0f;
    /* boxSize = zFar/1.75 (id's "div sqrt(3)"): the box is half a unit across in
     * grid space and its CORNERS then land on the far plane, so nothing the sky
     * draws is ever clipped away by the projection. */
    boxSize = zFar / 1.75f;

    /* The whole pass is a background layer: nothing is behind it. Since
     * v38.143 it runs AFTER the generic pass, so the test (against the
     * world's real depth, writes still off) keeps exactly the pixels
     * nothing covered — same image as into a cleared buffer, minus the
     * overdraw. Cleared depth reads far (0), the box sits at far: equal
     * passes under the >= test. */
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_TEXTURE_2D);
    /* The shade model is left alone: every sky vertex carries the same colour, so
     * flat and smooth are the same pixels, and the world pass's own choice
     * (GL_FLAT on a map with no lightmaps) must survive this call. */
    glColor3f(1.0f, 1.0f, 1.0f);        /* the layers are unlit; MODULATE with
                                         * white is "the texture as authored" */

    for (k = 0; k < sky_reg_n; k++) {
        const q3sky_shader_t *sh = &sky_reg[k];
        int i, nf = 0;

        if (sh->cloudHeight <= 0 || sh->numStages <= 0) continue;
        if (sky_cloud_height != sh->cloudHeight)
            init_sky_tex_coords(sh->cloudHeight);

        clear_sky_box();
        for (i = 0; i < n; i++) {
            const q3bsp_face_t *f = &m->faces[faces[i]];
            if (f->shaderNum != sky_reg_num[k]) continue;
            clip_sky_face(m, f, cam);
            nf++;
        }
        if (!nf) continue;              /* this sky shader is not in view */
        sky_st_cloud = sh->cloudHeight;
        fill_cloud_box(sh, cam, boxSize, timeSec);
    }

    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

void q3sky_stats(int *registered, int *cloud, int *stages, int *sides,
                 int *tris, int *trisRun) {
    if (registered) *registered = sky_reg_n;
    if (cloud) *cloud = sky_st_cloud;
    if (stages) *stages = sky_st_stages;
    if (sides) *sides = sky_st_sides;
    if (tris) *tris = sky_st_tris;
    if (trisRun) *trisRun = sky_st_tris_run;
}
