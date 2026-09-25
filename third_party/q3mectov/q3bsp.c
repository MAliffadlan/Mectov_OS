/* q3bsp.c — build the render mesh from a Quake III .bsp (v38.109, Q3 phase 8).
 *
 * See q3bsp.h for why this is separate from id's collision loader. Nothing in
 * this file is id source; it reads the format id's q3map writes (qfiles.h
 * structs, from the vendored tree, so the layout cannot drift) and produces
 * plain C data for the renderer.
 *
 * What is drawn and what is not, deliberately:
 *   * MST_PLANAR surfaces are expanded into triangle fans (a face's vertices
 *     are contiguous and wound as a fan — that is how id's own tessellator
 *     walks them). Their texture coordinates come straight out of the lump.
 *   * MST_PATCH (curved) surfaces are tessellated: the control grid is walked
 *     block by block, each 3x3 block is evaluated as the quadratic Bezier
 *     surface id's collision code also treats it as (cm_patch.c subdivides the
 *     identical grid for tracing), and the quads come out as ordinary faces.
 *     Until v38.109 these were counted and skipped, which drew every arch and
 *     dome of a retail map as a hole.
 *   * Lightmaps are ignored: the face's own unit normal is shaded against a
 *     fixed sun instead. That is honest for a generated arena with no lightmap
 *     lump, and it is what makes a screendump assertable at all (a dark
 *     unlit face would be indistinguishable from a hole).
 *   * Nothing is culled by contents. A non-solid decorative patch draws exactly
 *     like a structural one — id's renderer draws patches by surface type too,
 *     and it is what lets the generated arena test the curve without changing
 *     the collision world the walking tests assert on.
 */
#include "../q3a/code/game/q_shared.h"
#include "../q3a/code/qcommon/qcommon.h"
#include "../q3a/code/qcommon/qfiles.h"

#include "q3bsp.h"

extern void  write_serial_string(const char *s);
extern void *kmalloc(uint32_t size);
extern void  kfree(void *p);

/* The sun the face lighting is baked against: high and off to one side, so
 * floor, walls and ceiling each land at a different brightness. */
#define Q3BSP_SUN_X  0.45f
#define Q3BSP_SUN_Y  0.35f
#define Q3BSP_SUN_Z  0.82f
#define Q3BSP_AMBIENT 0.40f

/* --- curved surfaces ------------------------------------------------------
 *
 * id's patch meshes are quadratic Bezier surfaces laid out as a control grid in
 * which interpolating and approximating points alternate: every 3x3 block of
 * the grid is one quadratic patch, which is why the grid's dimensions must be
 * odd ("even sizes are invalid for quadratic meshes", cm_patch.c). The blocks
 * are evaluated here; cm_patch.c subdivides the identical grid for tracing, so
 * the drawn surface and the collided surface describe the same shape.
 *
 * Each block is subdivided uniformly into N x N quads. N comes from how far the
 * block's control points stray from the plane through its four corners, because
 * that distance bounds how far the surface itself can bow away from it: a flat
 * block is one quad, a tight arch gets more. Every quad is emitted as an
 * ordinary four-vertex face, so the renderer keeps its single triangle-fan path
 * and no downstream code ever learns that patches exist.
 */
#define Q3BSP_PATCH_MAX_DIM    33      /* control points per axis (id allows 129) */
#define Q3BSP_PATCH_FLAT       1.0f    /* a block this flat is a single quad */
#define Q3BSP_PATCH_TARGET     1.5f    /* curve error we are willing to leave */
#define Q3BSP_PATCH_MAX_SUBDIV 8

/* The two file access points the renderer needs, over the engine's FS. */
int q3bsp_read_file(const char *qpath, unsigned char **data, int *len) {
    void *buf = NULL;
    int n;
    if (data) *data = NULL;
    if (len) *len = 0;
    if (!qpath || !data || !len) return -1;
    n = FS_ReadFile(qpath, &buf);
    if (n <= 0 || !buf) return -2;
    *data = (unsigned char *)buf;
    *len = n;
    return 0;
}

void q3bsp_free_file(unsigned char *data) {
    if (data) FS_FreeFile(data);
}

static void bsp_copy_name(char *dst, const char *src) {
    int i = 0;
    while (i < Q3BSP_MAX_NAME - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* The kernel has no libm on this path: the bit-trick estimate plus Newton
 * iterations is plenty for a geometry decision. */
static float bsp_fsqrt(float x) {
    union { float f; int i; } u;
    float h;
    if (x <= 0.0f) return 0.0f;
    u.f = x;
    u.i = (u.i >> 1) + 0x1fc00000;
    h = u.f;
    h = 0.5f * (h + x / h);
    h = 0.5f * (h + x / h);
    h = 0.5f * (h + x / h);
    return h;
}

static void bsp_normalize(float *v) {
    float len = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (len <= 1e-12f) { v[0] = 0.0f; v[1] = 0.0f; v[2] = 1.0f; return; }
    {
        union { float f; int i; } u;
        float h;
        u.f = len;
        u.i = 0x5f3759df - (u.i >> 1);
        h = u.f;
        h = h * (1.5f - 0.5f * len * h * h);
        h = h * (1.5f - 0.5f * len * h * h);
        h = h * (1.5f - 0.5f * len * h * h);
        v[0] *= h; v[1] *= h; v[2] *= h;
    }
}

/* Quadratic Bernstein weights. */
static void bsp_bern2(float t, float *b) {
    b[0] = (1.0f - t) * (1.0f - t);
    b[1] = 2.0f * t * (1.0f - t);
    b[2] = t * t;
}

/*
=================
bsp_patch_block

Copy the control points of one 3x3 block. The lump stores a patch's grid as
points[j*width + i] (cm_patch.c: "grid.points[i][j] = points[j*width + i]"), so
the i index here is a stride -1 and j a stride of width, exactly as id reads it.
=================
*/
static void bsp_patch_block(const drawVert_t *dv, int firstVert, int width,
                            int bi, int bj, q3bsp_vert_t c[3][3]) {
    int i, j;
    for (j = 0; j < 3; j++) {
        for (i = 0; i < 3; i++) {
            const drawVert_t *v = &dv[firstVert + (2 * bj + j) * width + (2 * bi + i)];
            c[i][j].xyz[0] = v->xyz[0];
            c[i][j].xyz[1] = v->xyz[1];
            c[i][j].xyz[2] = v->xyz[2];
            c[i][j].st[0] = v->st[0];
            c[i][j].st[1] = v->st[1];
        }
    }
}

/*
=================
bsp_patch_eval

One point of one block, in position and in texture coordinates.

st is evaluated with the SAME weights as xyz, and that is exact rather than an
approximation: q3map bakes a patch's texture coordinates as a linear function of
position (the texvec projection), so combining the control points' st the same
way the control points' positions are combined reproduces that function at every
subdivision level — no drift as N grows, no seam between neighbouring blocks.
=================
*/
static void bsp_patch_eval(const q3bsp_vert_t c[3][3], float u, float v,
                           q3bsp_vert_t *out) {
    float bu[3], bv[3];
    int i, j, k;

    bsp_bern2(u, bu);
    bsp_bern2(v, bv);
    for (k = 0; k < 3; k++) out->xyz[k] = 0.0f;
    out->st[0] = out->st[1] = 0.0f;
    for (j = 0; j < 3; j++) {
        for (i = 0; i < 3; i++) {
            float w = bu[i] * bv[j];
            for (k = 0; k < 3; k++) out->xyz[k] += w * c[i][j].xyz[k];
            out->st[0] += w * c[i][j].st[0];
            out->st[1] += w * c[i][j].st[1];
        }
    }
}

/*
=================
bsp_patch_subdiv

How many quads one block needs, from how far its control points stray from the
bilinear patch through its four corners. A quadratic Bezier sits half as far
from its chord as its control point does, and N chords cut that error by N^2, so
N is the first value whose error lands under Q3BSP_PATCH_TARGET units.
=================
*/
static int bsp_patch_subdiv(const q3bsp_vert_t c[3][3]) {
    float dev2 = 0.0f;
    int i, j, k;

    for (j = 0; j < 3; j++) {
        for (i = 0; i < 3; i++) {
            float u = 0.5f * (float)i, v = 0.5f * (float)j, d2 = 0.0f;
            for (k = 0; k < 3; k++) {
                /* The bilinear through the corners: what this block degenerates
                 * to when it is flat, at the parameter its control point would
                 * occupy there. */
                float lin = (1.0f - u) * (1.0f - v) * c[0][0].xyz[k]
                          + u * (1.0f - v) * c[2][0].xyz[k]
                          + u * v * c[2][2].xyz[k]
                          + (1.0f - u) * v * c[0][2].xyz[k];
                float d = c[i][j].xyz[k] - lin;
                d2 += d * d;
            }
            if (d2 > dev2) dev2 = d2;
        }
    }
    if (dev2 <= Q3BSP_PATCH_FLAT * Q3BSP_PATCH_FLAT) return 1;
    {
        float dev = bsp_fsqrt(dev2);
        int n = 2;
        while (n < Q3BSP_PATCH_MAX_SUBDIV &&
               (float)(n * n) * (2.0f * Q3BSP_PATCH_TARGET) < dev) {
            n++;
        }
        return n;
    }
}

/* A patch is a legal one only if both dimensions are odd and at least 3 (id's
 * own rules — it Com_Errors on anything else) and its grid lies in the lump. */
static int bsp_patch_valid(const dsurface_t *s, int numDv, int *pw, int *ph) {
    int w = s->patchWidth, h = s->patchHeight;
    if (w < 3 || h < 3) return 0;
    if (w > Q3BSP_PATCH_MAX_DIM || h > Q3BSP_PATCH_MAX_DIM) return 0;
    if ((w & 1) == 0 || (h & 1) == 0) return 0;
    if (s->firstVert < 0 || s->firstVert + w * h > numDv) return 0;
    *pw = w;
    *ph = h;
    return 1;
}

/* The face's derived fields — centre, plane normal, radius, baked light — all
 * come from its vertices, so planar and tessellated faces share one path. */
static void bsp_face_finish(q3bsp_mesh_t *m, q3bsp_face_t *f) {
    int j;
    const float *a = m->verts[f->firstVert].xyz;
    const float *b = m->verts[f->firstVert + 1].xyz;
    const float *c = m->verts[f->firstVert + 2].xyz;
    float u[3], w[3];

    f->center[0] = f->center[1] = f->center[2] = 0.0f;
    for (j = 0; j < f->numVerts; j++) {
        const float *p = m->verts[f->firstVert + j].xyz;
        f->center[0] += p[0];
        f->center[1] += p[1];
        f->center[2] += p[2];
    }
    f->center[0] /= (float)f->numVerts;
    f->center[1] /= (float)f->numVerts;
    f->center[2] /= (float)f->numVerts;

    /* The normal from the winding (id's own normal[] on a planar face is just
     * this repeated per vertex; deriving it means a map with no normals still
     * shades). */
    for (j = 0; j < 3; j++) {
        u[j] = b[j] - a[j];
        w[j] = c[j] - a[j];
    }
    f->normal[0] = u[1] * w[2] - u[2] * w[1];
    f->normal[1] = u[2] * w[0] - u[0] * w[2];
    f->normal[2] = u[0] * w[1] - u[1] * w[0];
    bsp_normalize(f->normal);
    if (f->normal[0] == 0.0f && f->normal[1] == 0.0f &&
        f->normal[2] == 1.0f && u[0] == 0.0f && u[1] == 0.0f && u[2] == 0.0f)
        f->normal[2] = -1.0f;   /* degenerate: keep it out of the sun */

    f->radius = 0.0f;
    for (j = 0; j < f->numVerts; j++) {
        const float *p = m->verts[f->firstVert + j].xyz;
        float dx = p[0] - f->center[0];
        float dy = p[1] - f->center[1];
        float dz = p[2] - f->center[2];
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > f->radius) f->radius = d2;
    }
    {
        union { float f; int i; } u2;
        float h;
        u2.f = f->radius;
        if (f->radius > 0.0f) {
            u2.i = 0x5f3759df - (u2.i >> 1);
            h = u2.f;
            h = h * (1.5f - 0.5f * f->radius * h * h);
            f->radius *= h;
        }
    }

    {
        float d = f->normal[0] * Q3BSP_SUN_X +
                  f->normal[1] * Q3BSP_SUN_Y +
                  f->normal[2] * Q3BSP_SUN_Z;
        if (d < 0.0f) d = 0.0f;
        if (d > 1.0f) d = 1.0f;
        f->light = Q3BSP_AMBIENT + (1.0f - Q3BSP_AMBIENT) * d;
    }
}

/* One pass over the level's surfaces. A curved surface is a single entry in the
 * lump that expands into many faces, so the caller runs this twice: once with
 * emit == 0 to size the arrays, once with emit == 1 to fill them. Both passes
 * apply the same vertex cap in the same order, so the second cannot overflow
 * what the first measured.
 *
 * The counters live in the builder and are read after the counting pass (the
 * filling pass accumulates them again, which is why the caller takes its copy
 * in between). */
typedef struct {
    q3bsp_mesh_t     *m;
    const dsurface_t *sv;
    const drawVert_t *dv;
    int   numDv;

    int   faces, verts;
    int   planarFaces;
    int   patchSurfaces, patchesDrawn, patchSkipped;
    int   patchQuads, patchVerts;
    int   skipped, truncated, shaderOverflow;
} bsp_build_t;

static int bsp_shader_num(const bsp_build_t *b, int num) {
    if (num < 0 || num >= b->m->numShaders) return 0;
    return num;
}

static void bsp_planar_emit(bsp_build_t *b, const dsurface_t *s, int *faceIdx,
                            int *vertIdx) {
    q3bsp_face_t *f = &b->m->faces[*faceIdx];
    int j;

    f->firstVert = *vertIdx;
    f->numVerts = s->numVerts;
    f->shaderNum = bsp_shader_num(b, s->shaderNum);
    for (j = 0; j < s->numVerts; j++) {
        const drawVert_t *v = &b->dv[s->firstVert + j];
        q3bsp_vert_t *dst = &b->m->verts[*vertIdx + j];
        dst->xyz[0] = v->xyz[0];
        dst->xyz[1] = v->xyz[1];
        dst->xyz[2] = v->xyz[2];
        dst->st[0] = v->st[0];
        dst->st[1] = v->st[1];
    }
    bsp_face_finish(b->m, f);
    *vertIdx += s->numVerts;
    (*faceIdx)++;
}

static void bsp_patch_emit(bsp_build_t *b, const dsurface_t *s, int pw, int ph,
                           int *faceIdx, int *vertIdx) {
    int bi, bj;

    for (bj = 0; bj < (ph - 1) / 2; bj++) {
        for (bi = 0; bi < (pw - 1) / 2; bi++) {
            q3bsp_vert_t c[3][3];
            int n, qi, qj;

            bsp_patch_block(b->dv, s->firstVert, pw, bi, bj, c);
            n = bsp_patch_subdiv(c);
            for (qj = 0; qj < n; qj++) {
                for (qi = 0; qi < n; qi++) {
                    float u0 = (float)qi / (float)n, u1 = (float)(qi + 1) / (float)n;
                    float v0 = (float)qj / (float)n, v1 = (float)(qj + 1) / (float)n;
                    q3bsp_vert_t a, vb, vc, vd;
                    q3bsp_face_t *f = &b->m->faces[*faceIdx];

                    bsp_patch_eval(c, u0, v0, &a);
                    bsp_patch_eval(c, u1, v0, &vb);
                    bsp_patch_eval(c, u1, v1, &vc);
                    bsp_patch_eval(c, u0, v1, &vd);

                    f->firstVert = *vertIdx;
                    f->numVerts = 4;
                    f->shaderNum = bsp_shader_num(b, s->shaderNum);
                    /* Wound A D C B, which is the triangle-fan order that puts
                     * the face's normal on the same side of the surface as id's
                     * collision facet for the same quad: cm_patch.c builds its
                     * planes from (i,j),(i+1,j),(i+1,j+1) as cross(C-A, B-A),
                     * and this order reproduces exactly that. */
                    b->m->verts[*vertIdx + 0] = a;
                    b->m->verts[*vertIdx + 1] = vd;
                    b->m->verts[*vertIdx + 2] = vc;
                    b->m->verts[*vertIdx + 3] = vb;
                    bsp_face_finish(b->m, f);
                    *vertIdx += 4;
                    (*faceIdx)++;
                }
            }
        }
    }
}

static void bsp_build(bsp_build_t *b, int emit) {
    int i, faceIdx = 0, vertIdx = 0;

    for (i = 0; i < b->m->fileSurfaces; i++) {
        const dsurface_t *s = &b->sv[i];
        int pw, ph;

        if (b->m->numShaders > 0 && (s->shaderNum < 0 || s->shaderNum >= b->m->numShaders))
            b->shaderOverflow++;

        if (s->surfaceType == MST_PATCH) {
            int bi, bj, quads = 0, pverts = 0;

            b->patchSurfaces++;
            if (!bsp_patch_valid(s, b->numDv, &pw, &ph)) {
                b->patchSkipped++;
                continue;
            }
            for (bj = 0; bj < (ph - 1) / 2; bj++) {
                for (bi = 0; bi < (pw - 1) / 2; bi++) {
                    q3bsp_vert_t c[3][3];
                    int n;
                    bsp_patch_block(b->dv, s->firstVert, pw, bi, bj, c);
                    n = bsp_patch_subdiv(c);
                    quads += n * n;
                    pverts += n * n * 4;
                }
            }
            if (b->verts + pverts > Q3BSP_MAX_VERTS) {
                b->truncated = 1;
                b->patchSkipped++;
                break;
            }
            if (emit) bsp_patch_emit(b, s, pw, ph, &faceIdx, &vertIdx);
            b->patchesDrawn++;
            b->patchQuads += quads;
            b->patchVerts += pverts;
            b->faces += quads;
            b->verts += pverts;
            continue;
        }

        if (s->surfaceType != MST_PLANAR || s->numVerts < 3 ||
            s->firstVert < 0 || s->firstVert + s->numVerts > b->numDv) {
            b->skipped++;
            continue;
        }
        if (b->verts + s->numVerts > Q3BSP_MAX_VERTS) {
            b->truncated = 1;
            break;
        }
        if (emit) bsp_planar_emit(b, s, &faceIdx, &vertIdx);
        b->planarFaces++;
        b->faces++;
        b->verts += s->numVerts;
    }
}

int q3bsp_load(const char *qpath, q3bsp_mesh_t *out) {
    unsigned char *buf = NULL;
    int len = 0;
    dheader_t *hdr;
    int i;

    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!qpath) return -1;

    if (q3bsp_read_file(qpath, &buf, &len) != 0 || buf == NULL ||
        len < (int)sizeof(dheader_t)) {
        return -2;
    }

    hdr = (dheader_t *)buf;
    if (hdr->ident != BSP_IDENT || hdr->version != BSP_VERSION) {
        q3bsp_free_file(buf);
        return -3;
    }
    /* Every lump this file touches has to sit inside the file. A short read or
     * a hand-edited header must fail here, not page-fault the renderer. */
    for (i = 0; i < HEADER_LUMPS; i++) {
        int o = hdr->lumps[i].fileofs, l = hdr->lumps[i].filelen;
        if (o < 0 || l < 0 || o > len || l > len - o) {
            q3bsp_free_file(buf);
            return -4;
        }
    }

    bsp_copy_name(out->name, qpath);

    /* --- shaders: the names the renderer loads textures for --------------- */
    {
        const int sl = LUMP_SHADERS;
        int count = hdr->lumps[sl].filelen / (int)sizeof(dshader_t);
        const dshader_t *sh = (const dshader_t *)(buf + hdr->lumps[sl].fileofs);
        if (count > Q3BSP_MAX_SHADERS) {
            out->shadersTruncated = count - Q3BSP_MAX_SHADERS;
            count = Q3BSP_MAX_SHADERS;
        }
        for (i = 0; i < count; i++) bsp_copy_name(out->shaderNames[i], sh[i].shader);
        out->numShaders = count;
    }

    /* --- the world model's bounds ---------------------------------------- */
    {
        const int ml = LUMP_MODELS;
        if (hdr->lumps[ml].filelen >= (int)sizeof(dmodel_t)) {
            const dmodel_t *dm = (const dmodel_t *)(buf + hdr->lumps[ml].fileofs);
            for (i = 0; i < 3; i++) {
                out->mins[i] = dm[0].mins[i];
                out->maxs[i] = dm[0].maxs[i];
            }
        }
    }

    /* --- surfaces --------------------------------------------------------- */
    out->fileSurfaces = hdr->lumps[LUMP_SURFACES].filelen / (int)sizeof(dsurface_t);
    {
        bsp_build_t b;
        int faceCount, vertCount;

        memset(&b, 0, sizeof(b));
        b.m = out;
        b.sv = (const dsurface_t *)(buf + hdr->lumps[LUMP_SURFACES].fileofs);
        b.dv = (const drawVert_t *)(buf + hdr->lumps[LUMP_DRAWVERTS].fileofs);
        b.numDv = hdr->lumps[LUMP_DRAWVERTS].filelen / (int)sizeof(drawVert_t);

        bsp_build(&b, 0);                    /* how much this level expands to */

        out->planarFaces = b.planarFaces;
        out->patchSurfaces = b.patchSurfaces;
        out->patchesDrawn = b.patchesDrawn;
        out->patchQuads = b.patchQuads;
        out->patchVerts = b.patchVerts;
        out->patchSkipped = b.patchSkipped;
        out->skippedFaces = b.skipped;
        out->truncated = b.truncated;
        out->shaderNumOverflow = b.shaderOverflow;

        faceCount = b.faces;
        vertCount = b.verts;
        if (faceCount <= 0 || vertCount <= 0) {
            q3bsp_free_file(buf);
            return -5;
        }
        out->faces = (q3bsp_face_t *)kmalloc((uint32_t)(faceCount * (int)sizeof(q3bsp_face_t)));
        out->verts = (q3bsp_vert_t *)kmalloc((uint32_t)(vertCount * (int)sizeof(q3bsp_vert_t)));
        if (!out->faces || !out->verts) {
            if (out->faces) kfree(out->faces);
            if (out->verts) kfree(out->verts);
            out->faces = NULL;
            out->verts = NULL;
            q3bsp_free_file(buf);
            return -5;
        }

        bsp_build(&b, 1);                    /* fill them */
        out->numFaces = faceCount;
        out->numVerts = vertCount;
    }

    q3bsp_free_file(buf);
    out->valid = 1;
    return 0;
}

void q3bsp_free(q3bsp_mesh_t *m) {
    if (!m) return;
    if (m->faces) kfree(m->faces);
    if (m->verts) kfree(m->verts);
    m->faces = NULL;
    m->verts = NULL;
    m->valid = 0;
}
