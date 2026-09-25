/* q3bsp.h — the RENDER-side view of a Quake III .bsp (v38.108, Q3 phase 8).
 *
 * Collision and rendering read the same file but want different things out of
 * it, and id splits them the same way (qcommon/cm_load.c vs the renderer's
 * loader). CM_* keeps planes, brushes and leafs — the things a trace touches.
 * This module keeps what a camera needs: the surfaces' triangles, their
 * texture coordinates, the shader name each one is drawn with, and a per-face
 * normal it can shade from.
 *
 * The mesh is plain C data on purpose. The renderer lives in the TinyGL
 * directory and is compiled with TinyGL's flags (no id headers on its include
 * path), so the two meet at this header and nowhere else.
 *
 * Ownership: q3bsp_load() allocates through the kernel allocator; the caller
 * owns the result and hands it to q3bsp_free(). Nothing here is shared with
 * CM_LoadMap's world — a corrupt render mesh can never affect collision, which
 * is the property that lets the port boot with no map at all.
 */
#ifndef Q3BSP_H
#define Q3BSP_H

#include <stdint.h>

/* Id's shader cap in a .bsp is 1024 (MAX_SHADERS), but a real map rarely names
 * more than a few hundred, and each name costs 64 bytes here. Over the cap the
 * mesh reports `shadersTruncated` and the extra surfaces fall back to shader 0
 * instead of being dropped — a wrong texture beats a missing wall. */
#define Q3BSP_MAX_SHADERS 256
#define Q3BSP_MAX_NAME    64

/* Ceiling on the expanded vertex array. A face's vertices are copied out of the
 * lump contiguously (one drawVert_t each), so a big map costs 44 bytes per
 * vertex here; past the cap the loader stops adding faces and says so. */
#define Q3BSP_MAX_VERTS   120000

typedef struct {
    float xyz[3];
    float st[2];
} q3bsp_vert_t;

typedef struct {
    int   firstVert;        /* index into q3bsp_mesh_t.verts */
    int   numVerts;
    int   shaderNum;        /* index into q3bsp_mesh_t.shaderNames */
    float center[3];        /* mean of the face's vertices (culling) */
    float radius;           /* distance from center to the farthest vertex */
    float normal[3];        /* unit plane normal, from the first triangle */
    float light;            /* baked face light 0..1 against the sun */
} q3bsp_face_t;

/* Tagged so the renderer's header can forward-declare it without pulling in the
 * id-derived structs this file's implementation uses. */
typedef struct q3bsp_mesh_s {
    int   valid;
    char  name[Q3BSP_MAX_NAME];      /* the qpath that was read */

    int   numShaders;
    char  shaderNames[Q3BSP_MAX_SHADERS][Q3BSP_MAX_NAME];
    int   shadersTruncated;

    int   numFaces;
    int   numVerts;
    q3bsp_face_t *faces;
    q3bsp_vert_t *verts;

    /* Reporting: how much of the file the mesh covers, so the log can say what
     * was skipped rather than quietly drawing a hole. */
    int   fileSurfaces;             /* surfaces in the lump */
    int   patchSurfaces;            /* MST_PATCH (curved) — not drawn yet */
    int   skippedFaces;             /* no vertices / no index */
    int   truncated;                /* hit Q3BSP_MAX_VERTS */
    int   shaderNumOverflow;        /* a shaderNum out of range */

    float mins[3], maxs[3];         /* the world model's bounds */
} q3bsp_mesh_t;

/* Read `qpath` through the engine filesystem (FS_ReadFile, so the same search
 * path the .bsp was loaded from collision-wise) and build the mesh.
 * Returns 0 on success; negative on failure, with `out` zeroed. */
int  q3bsp_load(const char *qpath, q3bsp_mesh_t *out);
void q3bsp_free(q3bsp_mesh_t *m);

/* The renderer's two file needs, served through the engine FS so textures are
 * found (or not found) exactly like the map was. Both return 0 on success;
 * q3bsp_free_file() releases what q3bsp_read_file() allocated. */
int  q3bsp_read_file(const char *qpath, unsigned char **data, int *len);
void q3bsp_free_file(unsigned char *data);

#endif /* Q3BSP_H */
