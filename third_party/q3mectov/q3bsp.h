/* q3bsp.h — the RENDER-side view of a Quake III .bsp (v38.109, Q3 phase 8).
 *
 * Collision and rendering read the same file but want different things out of
 * it, and id splits them the same way (qcommon/cm_load.c vs the renderer's
 * loader). CM_* keeps planes, brushes and leafs — the things a trace touches.
 * This module keeps what a camera needs: the surfaces' triangles, their
 * texture coordinates, the shader name each one is drawn with, and a per-face
 * normal it can shade from.
 *
 * Lighting comes from the file's own lightmaps (v38.114): q3map's 128x128 RGB
 * pages are loaded and sampled at each vertex's lightmap coordinate, and the
 * result rides out as the vertex colour the TinyGL backend modulates the
 * texture with. Until v38.113 the only light here was a face normal dotted
 * against a fixed sun, which is what a map with no lightmap lump still gets.
 *
 * Curved surfaces are tessellated here too (v38.109). A patch mesh stores a
 * control GRID, not vertices, so a quarter of the work the renderer needs from
 * a .bsp is arithmetic rather than copying; the alternative — leaving patches
 * out — draws every arch and dome in a retail map as a hole. Each tessellated
 * quad becomes an ordinary four-vertex face, so nothing downstream of this
 * header has a new case to handle.
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

/* --- the BSP tree, kept for culling (v38.112) ------------------------------
 *
 * v38.111 drew every face of the level every frame and culled by a single
 * lateral test against the camera's right vector. That is fine for a generated
 * arena (74 faces) and hopeless for a retail map (thousands of surfaces): the
 * map's own visibility information — the tree, its leaves and the PVS the
 * compiler baked into the VISIBILITY lump — is exactly the data that says what
 * a camera in one leaf can see.
 *
 * The caps are size guards on hostile input, not budgets: id's own limits are
 * MAX_MAP_NODES 0x10000 and MAX_MAP_CLUSTERS 0x10000, and the arrays are
 * allocated to the map's real counts, so a 200-node map costs 200 nodes. A map
 * that exceeds a cap simply gets no culling (the renderer falls back to drawing
 * everything) rather than a truncated tree, because a WRONG tree would cull
 * geometry that is really there. */
#define Q3BSP_MAX_PLANES    0x10000
#define Q3BSP_MAX_NODES     0x10000
#define Q3BSP_MAX_LEAFS     0x10000
#define Q3BSP_MAX_CLUSTERS  0x10000

/* --- lightmaps (v38.114) --------------------------------------------------
 *
 * q3map bakes a level's lighting into 128x128 RGB pages (this is id's
 * LIGHTMAP_SIZE; every Q3 .bsp uses it) and packs each surface's lighting into
 * a sub-rectangle of one page. A surface names its page in lightmapNum and its
 * sub-rectangle in lightmapX/lightmapY/W/H, and every vertex carries the
 * coordinates INTO THE PAGE (0..1 across all 128 texels, not across the
 * sub-rectangle) in drawVert_t.lightmap. Measured on q3dm1: 9 pages, 442,368
 * bytes, and a surface's vertex coordinates land inside its own sub-rectangle
 * to the texel — which is why this loader trusts the file's coordinates instead
 * of projecting anything itself.
 *
 * The cap is a heap guard, not a map limit: 64 pages cost 3 MB, q3dm1 uses 9.
 * A map with more pages gets its first 64 and reports the rest as dropped, and
 * surfaces naming a dropped page fall back to the baked sun like any other
 * unlit face. */
#define Q3BSP_LIGHTMAP_DIM   128
#define Q3BSP_MAX_LIGHTMAPS  64

/* How far each page's bytes are shifted left when it is loaded.
 *
 * A Q3 lightmap on disk is DARK — q3dm1's pages average 33,22,18 per channel —
 * because id's own loader brightens them at upload: tr_map.c
 * R_ColorShiftLightingBytes() does `in[i] << shift` with
 * `shift = r_mapOverBrightBits - tr.overbrightBits`, and when the video card
 * cannot do gamma (which is the software rasterizer's case, and so this port's)
 * id sets overbrightBits = 0 to go with the map's 2, giving a shift of 2. The
 * brightened result is then compensated in hardware by the monitor's gamma
 * ramp; this port has no ramp, so the shift is applied and the result is drawn
 * as-is. That keeps a lit level in the same range the pre-lightmap sun put it
 * in rather than at a quarter of it — on q3dm1 the average vertex comes out at
 * 111/255 against the sun's flat 139, but with the spread the flat light could
 * not have: 28.6% of sampled vertices land above that 139 and 28.8% below 60,
 * which is the difference between a lit room and a grey one.
 *
 * The overflow rule is id's too: when any channel passes 255 the whole texel is
 * scaled by 255/max rather than each channel clipping, so a torch keeps its
 * hue instead of turning white. */
#define Q3BSP_LIGHTMAP_SHIFT 2

typedef struct {
    int planeNum;
    int children[2];        /* negative = -(leaf + 1), exactly as the file says */
} q3bsp_node_t;

typedef struct {
    int numPlanes, numNodes, numLeafs;
    float            (*planes)[4];      /* [numPlanes]: normal[0..2], dist */
    q3bsp_node_t      *nodes;           /* [numNodes]  */
    int               *leafCluster;     /* [numLeafs]: -1 = no cluster (always drawn) */
    int               *leafFirstFace;   /* [numLeafs] into leafFaces */
    int               *leafNumFaces;    /* [numLeafs] */
    int               *leafFaces;       /* face indices, grouped by leaf */
    int                numLeafFaces;

    unsigned char     *vis;             /* the VISIBILITY lump, copied out of the file */
    int                visLen;
    int                numClusters;
    int               *bitOfs;          /* [numClusters]: byte offset of each cluster's PVS */

    int                ready;           /* tree usable: a camera can be located */
    int                hasVis;          /* ready AND the PVS row can be walked */
} q3bsp_vis_t;

typedef struct {
    float xyz[3];
    float st[2];
    /* Page-space lightmap coordinate (v38.114): what drawVert_t.lightmap held,
     * or — for a tessellated patch — the Bezier combination of the control
     * points' coordinates, which is exact for the same reason st is (see
     * bsp_patch_eval). */
    float lm[2];
    /* v38.117: the FILE's vertex normal (drawVert_t.normal), copied for planar
     * faces and Bezier-combined for patch quads. It marks the authoritative
     * front side for backface culling — id's own renderer culls with exactly
     * this normal — and never touches shading. */
    float normal[3];
    /* The light this vertex ended up with, 0..255 per channel, resolved once at
     * load: the lightmap sampled bilinearly at (lm), or the baked sun when the
     * face has no page. Id's lightmap stage MODULATES the diffuse texture, and
     * TinyGL draws one texture at a time, so the product has to happen before
     * the draw — here, into the vertex colour that TGL_FEATURE_LIT_TEXTURES
     * already multiplies into every pixel. */
    unsigned char light[3];
} q3bsp_vert_t;

typedef struct {
    int   firstVert;        /* index into q3bsp_mesh_t.verts */
    int   numVerts;
    int   shaderNum;        /* index into q3bsp_mesh_t.shaderNames */
    float center[3];        /* mean of the face's vertices (culling) */
    float radius;           /* distance from center to the farthest vertex */
    float normal[3];        /* unit plane normal, from the first triangle */
    /* v38.117: the face's FRONT normal — the mean of the file's per-vertex
     * normals (drawVert_t.normal), Bezier-combined on patch quads. This is
     * what backface culling reads, exactly the data id's R_CullDotTri culls
     * with; `normal` above keeps serving the baked-sun fallback. */
    float frontNormal[3];
    /* v38.132: is `frontNormal` trustworthy enough to REJECT this face with?
     *
     * Set at load from the agreement of the file's own per-vertex normals (see
     * bsp_face_finish). 1 = they all point the same way, so the mean describes
     * a real front side. 0 = they disagree or cancel, which is what q3map
     * emits for a brush-derived face where the normals run along the BRUSH
     * rather than at the visible surface — the case this port's own comment
     * recorded ("walls and the curved cove vanished under either sign").
     *
     * A 0 face is NEVER rejected: a face drawn that need not have been is a
     * wasted triangle, a face wrongly rejected is a hole in the level. This is
     * what makes the cull fail-safe on retail maps while keeping it where it is
     * measured to be correct.
     *
     * Purely derived at load from data already in the file — never persisted,
     * so adding it changes nothing on disk. */
    int   cullTrusted;
    float light;            /* baked face light 0..1 against the sun */
    int   lightmapNum;      /* page in q3bsp_mesh_t.lightmaps, -1 = sun-lit */
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
    int   planarFaces;              /* MST_PLANAR faces drawn one-to-one */
    int   patchSurfaces;            /* MST_PATCH (curved) in the lump */
    int   patchesDrawn;             /* of those, tessellated and drawn */
    int   patchQuads;               /* quads the tessellator emitted */
    int   patchVerts;               /* vertices those quads cost */
    int   patchSkipped;             /* a patch that could not be tessellated */
    int   skippedFaces;             /* no vertices / no index */
    int   truncated;                /* hit Q3BSP_MAX_VERTS */
    int   shaderNumOverflow;        /* a shaderNum out of range */

    float mins[3], maxs[3];         /* the world model's bounds */

    /* Lightmaps (v38.114). NULL/0 when the file carries none (every generated
     * fixture map does not) — and then every face keeps the baked sun, which is
     * exactly what v38.113 drew. */
    unsigned char *lightmaps;       /* numLightmaps * DIM * DIM * 3, RGB, shifted */
    int   numLightmaps;
    int   lightmapDim;
    int   lightmapBytes;
    int   lightmapShift;            /* the shift those bytes carry */
    int   lightmapDropped;          /* pages past Q3BSP_MAX_LIGHTMAPS */
    int   facesLit;                 /* faces shaded from a page */
    int   facesUnlit;               /* faces that kept the baked sun */
    int   litVerts;                 /* vertices those lit faces spent */
    int   lightmapMean[3];          /* mean byte of the pages, 0..255 */
    int   litMean[3];               /* sum of the lit vertices' light, /litVerts */

    /* Culling input (v38.112). Zeroed when the map carries no usable tree, and
     * every consumer treats that as "draw everything" — the pre-v38.112
     * behaviour, kept as the fallback on purpose. */
    q3bsp_vis_t vis;
} q3bsp_mesh_t;

/* The shader-name world as files describe it (v38.111). Every .shader script
 * under scripts/ on the engine's search path is read through FS_GetFileList + FS_ReadFile and
 * parsed in place; the returned name/image pointers point INTO those buffers.
 * The caller frees buffers, not names, via q3bsp_free_decls(). Returns 0 on
 * success (any number of decls, including none); -1 on a bad pointer, -5 on
 * an allocation failure. */
/* v38.128: what one STAGE of a definition asks for. Until this release the
 * parser kept only the first image of the first stage, which is all the world
 * draw needed — but the sky is not one image: `textures/skies/tim_hell` is a
 * base layer plus an ADDITIVE cloud layer, each with its own `tcMod`, and id
 * animates both with the game clock. A stage is therefore image + how it is
 * blended + the tcMods in the order the file wrote them (id applies them in
 * that order, in place — see RB_CalcScrollTexCoords/RB_CalcScaleTexCoords). */
#define Q3BSP_MAX_STAGES  4
#define Q3BSP_MAX_TCMODS  2
#define Q3BSP_TCMOD_SCROLL 0        /* tcMod scroll <s> <t>, texture units per SECOND */
#define Q3BSP_TCMOD_SCALE  1        /* tcMod scale  <s> <t>, a multiplier          */

typedef struct {
    unsigned char type;             /* Q3BSP_TCMOD_* */
    float         a, b;
} q3bsp_tcmod_t;

typedef struct {
    const char   *image;            /* the stage's first real image, or NULL */
    int           imageLen;
    int           additive;         /* this stage's blendFunc is GL_ONE GL_ONE */
    int           depthWrite;       /* the stage says depthWrite */
    int           numTcMods;
    q3bsp_tcmod_t tcmod[Q3BSP_MAX_TCMODS];
} q3bsp_shader_stage_t;

typedef struct q3bsp_shader_decl_s {
    const char *name;               /* into the file buffer, NOT NUL-terminated */
    int         nameLen;
    const char *image;              /* first real image operand, or NULL */
    int         imageLen;
    /* v38.125: the flags the draw pass needs, taken from the SAME stage the
     * image came out of — id's torches are `animMap … blendFunc GL_ONE GL_ONE`
     * in one stage, and drawing them opaquely paints their black background
     * over the level (exactly the muzzle flash's failure). `cullNone` is the
     * definition-level `cull none|disable` (fire and lava are two-sided). */
    int         additive;           /* that stage blends GL_ONE GL_ONE */
    int         cullNone;           /* the definition turns culling off */
    /* v38.128: the sky. `sky` is the definition-level `surfaceparm sky` — the
     * faces it names are the ones id's renderer does NOT draw as geometry (it
     * uses their directions to pick which sides of the sky box to fill), and
     * `cloudHeight` is `skyparms`' middle operand (0 = no cloud layer). The
     * stages above carry the layers those faces are drawn with. */
    int         sky;
    /* v38.128: `skyparms`' FIRST operand, as a flag: 1 when the definition
     * names a six-image far box. The renderer keeps it for the log only — the
     * box itself is not built (see third_party/tinygl/q3sky.h), and a map that
     * asks for one is otherwise indistinguishable from one that does not. */
    int         skyFarBox;
    int         cloudHeight;
    int         numStages;
    q3bsp_shader_stage_t stages[Q3BSP_MAX_STAGES];
} q3bsp_shader_decl_t;

typedef struct q3bsp_shader_decls_s {
    q3bsp_shader_decl_t *decls;
    int                  num;
    unsigned char      **buffers;
    int                  numBuffers;
} q3bsp_shader_decls_t;

int  q3bsp_read_shader_decls(q3bsp_shader_decls_t *out);
void q3bsp_free_decls(q3bsp_shader_decls_t *d);

/* Read `qpath` through the engine filesystem (FS_ReadFile, so the same search
 * path the .bsp was loaded from collision-wise) and build the mesh.
 * Returns 0 on success; negative on failure, with `out` zeroed. */
int  q3bsp_load(const char *qpath, q3bsp_mesh_t *out);
void q3bsp_free(q3bsp_mesh_t *m);

/* Which leaf of the map's own tree contains `p`? Returns a leaf number, or -1
 * when the map has no usable tree (vis.ready == 0) or `p` walks into a node
 * that does not exist. The walk is id's CM_PointLeafnum_r, child for child:
 * front of the plane (d >= 0) is children[0], the leaf is (-1 - child). */
int  q3bsp_leaf_for_point(const q3bsp_mesh_t *m, const float p[3]);

/* The renderer's two file needs, served through the engine FS so textures are
 * found (or not found) exactly like the map was. Both return 0 on success;
 * q3bsp_free_file() releases what q3bsp_read_file() allocated. */
int  q3bsp_read_file(const char *qpath, unsigned char **data, int *len);
void q3bsp_free_file(unsigned char *data);

#endif /* Q3BSP_H */
