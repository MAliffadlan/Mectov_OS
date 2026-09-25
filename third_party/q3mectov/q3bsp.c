/* q3bsp.c — build the render mesh from a Quake III .bsp (v38.108, Q3 phase 8).
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
 *   * MST_PATCH (curved) surfaces are counted and skipped. Drawing them needs
 *     the Bezier tessellator (cm_patch.c builds only the collision hull);
 *     skipping them is a visible hole, so the count is in the log.
 *   * Lightmaps are ignored: the face's own unit normal is shaded against a
 *     fixed sun instead. That is honest for a generated arena with no lightmap
 *     lump, and it is what makes a screendump assertable at all (a dark
 *     unlit face would be indistinguishable from a hole).
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

static void bsp_normalize(float *v) {
    float len = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (len <= 1e-12f) { v[0] = 0.0f; v[1] = 0.0f; v[2] = 1.0f; return; }
    /* The kernel has no libm on this path; one Newton step off a bit-trick
     * estimate is plenty for a shading normal. */
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

int q3bsp_load(const char *qpath, q3bsp_mesh_t *out) {
    unsigned char *buf = NULL;
    int len = 0;
    dheader_t *hdr;
    int i, nf, nv;
    const drawVert_t *dv;

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
    out->numFaces = out->fileSurfaces;

    out->faces = (q3bsp_face_t *)kmalloc((uint32_t)(out->numFaces * (int)sizeof(q3bsp_face_t)));
    dv = (const drawVert_t *)(buf + hdr->lumps[LUMP_DRAWVERTS].fileofs);
    {
        int numDv = hdr->lumps[LUMP_DRAWVERTS].filelen / (int)sizeof(drawVert_t);
        out->verts = (q3bsp_vert_t *)kmalloc((uint32_t)(Q3BSP_MAX_VERTS * (int)sizeof(q3bsp_vert_t)));
        if (!out->faces || !out->verts) {
            if (out->faces) kfree(out->faces);
            if (out->verts) kfree(out->verts);
            out->faces = NULL;
            out->verts = NULL;
            q3bsp_free_file(buf);
            return -5;
        }
        nv = 0;
        nf = 0;
        for (i = 0; i < out->fileSurfaces; i++) {
            const dsurface_t *s =
                (const dsurface_t *)(buf + hdr->lumps[LUMP_SURFACES].fileofs) + i;
            q3bsp_face_t *f;
            int j;

            if (s->surfaceType == MST_PATCH) {
                out->patchSurfaces++;
                continue;
            }
            if (s->surfaceType != MST_PLANAR || s->numVerts < 3 ||
                s->firstVert < 0 || s->firstVert + s->numVerts > numDv) {
                out->skippedFaces++;
                continue;
            }
            if (nv + s->numVerts > Q3BSP_MAX_VERTS) {
                out->truncated = 1;
                break;
            }

            f = &out->faces[nf];
            f->firstVert = nv;
            f->numVerts = s->numVerts;
            f->shaderNum = (s->shaderNum >= 0 && s->shaderNum < out->numShaders)
                               ? s->shaderNum : 0;
            if (s->shaderNum < 0 || s->shaderNum >= out->numShaders)
                out->shaderNumOverflow++;

            f->center[0] = f->center[1] = f->center[2] = 0.0f;
            for (j = 0; j < s->numVerts; j++) {
                const drawVert_t *v = &dv[s->firstVert + j];
                q3bsp_vert_t *dst = &out->verts[nv + j];
                dst->xyz[0] = v->xyz[0];
                dst->xyz[1] = v->xyz[1];
                dst->xyz[2] = v->xyz[2];
                dst->st[0] = v->st[0];
                dst->st[1] = v->st[1];
                f->center[0] += v->xyz[0];
                f->center[1] += v->xyz[1];
                f->center[2] += v->xyz[2];
            }
            f->center[0] /= (float)s->numVerts;
            f->center[1] /= (float)s->numVerts;
            f->center[2] /= (float)s->numVerts;

            /* Face normal from the winding (id's normal[] on a planar face is
             * just this repeated per vertex; deriving it means a map with no
             * normals still shades). */
            {
                const float *a = out->verts[nv].xyz;
                const float *b = out->verts[nv + 1].xyz;
                const float *c = out->verts[nv + 2].xyz;
                float u[3], w[3];
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
            }

            f->radius = 0.0f;
            for (j = 0; j < s->numVerts; j++) {
                const float *p = out->verts[nv + j].xyz;
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

            nv += s->numVerts;
            nf++;
        }
        out->numVerts = nv;
        out->numFaces = nf;
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
