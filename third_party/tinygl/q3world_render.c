/* q3world_render.c — the .bsp world renderer for the Q3 backend (v38.108).
 *
 * Phase 8 of the Q3 port, and the end of the line the last three releases were
 * walking towards: until now the level the official id game module was running
 * in existed only as collision (v38.107) and as serial log. This module takes
 * the same .bsp's surfaces, textures them from the game data on the volume, and
 * puts them on screen through TinyGL — so what the window shows is the module's
 * own level, not geometry this port made up.
 *
 * Three decisions worth stating, because each is a place this could have lied:
 *
 *  1. Textures come off the volume, through the same engine FS the map came
 *     from (q3bsp_read_file). TinyGL's glTexImage2D upsamples every image to
 *     TGL_FEATURE_TEXTURE_DIM (256) with nearest-neighbour, so a 64x64 working
 *     size costs nothing in fidelity and keeps the decode small.
 *  2. A missing image becomes a generated placeholder, and the log says
 *     "missing" — never a silent white surface. The test asserts the *decoded*
 *     textures, so a placeholder can never be mistaken for a loaded one.
 *  3. Lighting is the level's own lightmaps (v38.114), applied as the vertex
 *     colour (TGL_FEATURE_LIT_TEXTURES is on, so the colour modulates the
 *     texture). TinyGL draws one texture per primitive and Q3's lightmap stage
 *     is a second one, so the product of map and texture has to be taken
 *     before the draw: the loader samples each vertex's lightmap coordinate and
 *     leaves the result in the vertex, and this file emits one glColor3f per
 *     vertex — TinyGL interpolates those per pixel exactly as a GPU would.
 *     A face whose surface names no page keeps the v38.113 fallback, a baked
 *     face normal against a fixed sun, drawn flat (one glColor3f per face).
 *     That fallback is what makes floor, walls and ceiling of a generated
 *     fixture arena — which has no lightmap lump at all — read as different
 *     surfaces in a screendump.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <math.h>              /* the kernel's q3 math stub: sqrt for |n| */
#include <TGL/gl.h>
#include "zbuffer.h"

#include "q3world_render.h"
#include "q3jpeg.h"
#include "src/tgl_cyc.h"     /* v38.116: setup-vs-fill cycle split */

extern void  write_serial_string(const char *s);
extern void *kmalloc(uint32_t size);
extern void  kfree(void *p);

/* Beyond this the face is dropped: the backend's projection reaches 4096, but
 * the test arena is ~1200 units across and a retail q3dm map is a few thousand
 * at most, so nothing a camera can resolve is lost. */
#define Q3W_FAR        3500.0f
#define Q3W_MAX_TEX    Q3BSP_MAX_SHADERS
#define Q3W_PH_SIZE    64          /* placeholder / working texture side */

typedef struct {
    int  used;
    int  fromDisk;
    int  width, height;
    /* v38.125: how the shader script says this shader is drawn. `additive`
     * (a stage whose blendFunc is GL_ONE GL_ONE — every flame in the demo)
     * means the image carries its brightness in a black surround: drawn
     * opaque, that surround is painted over the level. `cullNone` is the
     * definition's `cull none`; the module culls backfaces by hand, so the
     * flag has to defeat that test or half of a two-sided flame vanishes. */
    int  additive;
    int  cullNone;
    char path[Q3BSP_MAX_NAME + 8];
} q3w_slot_t;

static GLuint    tex_id[Q3W_MAX_TEX];
static q3w_slot_t tex_slot[Q3W_MAX_TEX];
static int       tex_shaders;      /* shaders the last load walked */
static int       tex_from_disk;
static int       tex_placeholders;
static int       tex_inited;

/* Does the module's own backface test apply to this shader? (id culls with
 * `cull none` defeated; two-sided flames and lava are exactly that.) */
static int w_cull_none(int shaderNum) {
    if (shaderNum < 0 || shaderNum >= Q3W_MAX_TEX) return 0;
    return tex_slot[shaderNum].cullNone;
}

/* Bind a shader's image AND its blend state. The blend half matters only for
 * an additive shader drawn outside the additive pass (the pass is skipped
 * when its pool could not be allocated) — inside the pass the state is set
 * once for the whole list. */
static void w_bind_shader(int shaderNum) {
    int s = (shaderNum >= 0 && shaderNum < Q3W_MAX_TEX) ? shaderNum : 0;
    glBindTexture(GL_TEXTURE_2D, tex_id[s]);
    if (tex_slot[s].additive) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
    } else {
        glDisable(GL_BLEND);
    }
}

static int  v_faces, v_tris, v_culled;   /* last frame */
static int  v_lit_faces;                 /* of those, drawn from a lightmap */
static int  t_lit_faces;                 /* run total, for the closing line */

/* v38.125: what the shader scripts asked for, and what the draw pass made of
 * it. `add_shaders`/`cull_shaders` are load-time (the definitions are read
 * once); `add_faces` is per frame and `t_add_faces` for the run, so a test can
 * tell "the flame's definition is additive" from "a flame actually went
 * through the additive pass" — the difference between a parsed flag and a
 * drawn one, which is what v38.124's `drawn` counter taught this file. */
static int  add_shaders, cull_shaders;
static int  v_add_faces, t_add_faces;

/* --- v38.112: culling -----------------------------------------------------
 *
 * Two stages, both driven by data the map already carries:
 *
 *   1. PVS. The camera is located in the map's own tree (q3bsp_leaf_for_point),
 *      that leaf names a cluster, and the visibility matrix baked by q3map says
 *      which clusters are visible from it. Every face belonging to a visible
 *      leaf is marked; the rest are not drawn. This is the mechanism that makes
 *      a retail map's thousands of surfaces affordable — it is id's own
 *      R_MarkLeaves, over this module's faces instead of the lump's.
 *   2. Frustum. The marked faces are then tested against the six real planes of
 *      the projection the backend set up (glFrustum(-sx, sx, -hh*sx, hh*sx,
 *      1, 4096) with sx = tan(fov/2)), sphere against plane. Before this the
 *      lateral test was a single plane with a fixed 0.75 slope and there was no
 *      vertical test at all.
 *
 * When the map carries no usable tree or no PVS lump, every face is marked,
 * which is exactly what v38.111 drew. */
static unsigned char *vis_mark;            /* [vis_mark_cap] */
static int            vis_mark_cap;
static int            v_marked, v_vis_all; /* faces the PVS kept; 1 = no PVS */
static int            v_cluster = -2, v_leaf = -2, v_vis_leafs;
static int            v_total_faces;       /* the mesh the last frame drew */
static int            v_culled_vis, v_culled_frustum;   /* per stage, last frame */
static int            v_culled_back;        /* v38.117: faces facing away */
static int            v_planes_logged;     /* one plane dump per run */
static float          v_planes[6][4];      /* (normal, offset), see below */
static int            v_planes_n;

/* Mark every face the PVS from `cam`'s leaf allows. Returns the number of
 * faces marked; sets v_vis_all when there was nothing to cull by. */
static int vis_mark_faces(const q3bsp_mesh_t *m, const float cam[3]) {
    const q3bsp_vis_t *v = &m->vis;
    int i, n = 0, leafs = 0;

    if (!vis_mark || vis_mark_cap < m->numFaces) {
        unsigned char *nb = (unsigned char *)kmalloc((uint32_t)m->numFaces);
        if (!nb) return 0;
        if (vis_mark) kfree(vis_mark);
        vis_mark = nb;
        vis_mark_cap = m->numFaces;
    }
    memset(vis_mark, 0, (size_t)m->numFaces);
    v_vis_all = 0;
    v_leaf = -1;
    v_cluster = -1;
    v_vis_leafs = 0;

    v_leaf = q3bsp_leaf_for_point(m, cam);

    if (v->ready && v->hasVis && v_leaf >= 0 &&
        v->leafCluster[v_leaf] >= 0 &&
        v->leafCluster[v_leaf] < v->numClusters) {
        const unsigned char *pvs;
        int cluster = v->leafCluster[v_leaf];

        v_cluster = cluster;
        pvs = v->vis + v->bitOfs[cluster];
        for (i = 0; i < v->numLeafs; i++) {
            int c = v->leafCluster[i];

            /* A leaf with no cluster (-1) belongs to no PVS row: id draws it
             * unconditionally, and so does this. */
            if (c >= 0) {
                if (c >= v->numClusters) continue;
                if (!(pvs[c >> 3] & (1 << (c & 7)))) continue;
            }
            leafs++;
            for (int k = 0; k < v->leafNumFaces[i]; k++) {
                int fi = v->leafFaces[v->leafFirstFace[i] + k];
                if (fi >= 0 && fi < m->numFaces && !vis_mark[fi]) {
                    vis_mark[fi] = 1;
                    n++;
                }
            }
        }
        v_vis_leafs = leafs;
        /* A copy of this pass used to be kept so the frustum stage could not
         * take a PVS 'kept' vote away from the next frame. v38.116 drops it:
         * the frustum stage is now exact per face (face_outside_frustum), and
         * an exact test needs no second opinion. */
        if (n) return n;
    }

    /* No tree, no PVS lump, a camera outside every leaf, or a PVS row that
     * marked nothing: draw the level. The last case is deliberate — a map whose
     * vis data cannot see the camera's own leaf is broken data, and a black
     * screen would be a worse answer than an unculled one. */
    memset(vis_mark, 1, (size_t)m->numFaces);
    v_vis_all = 1;
    return m->numFaces;
}

/* The six planes of the projection the backend installs, as (normal, offset)
 * so that a point is inside when dot(n,p) - o >= 0. Unnormalised on purpose:
 * the sphere test scales the radius by |n| instead. */
static int frustum_planes(const float cam[3], const float fwd[3],
                          const float right[3], float tan_hx, float tan_hy) {
    float up[3], f[3], r[3], o;
    int i, n = 0;

    if (tan_hx <= 0.0f) tan_hx = 1.0f;          /* the 90-degree default */
    if (tan_hy <= 0.0f) tan_hy = tan_hx;

    for (i = 0; i < 3; i++) { f[i] = fwd[i]; r[i] = right[i]; }
    /* up = right x fwd: the camera basis rows are (right, up, -fwd), so this is
     * the third row of the same rotation (see q3cl_render.c's cam_basis). */
    up[0] = right[1] * fwd[2] - right[2] * fwd[1];
    up[1] = right[2] * fwd[0] - right[0] * fwd[2];
    up[2] = right[0] * fwd[1] - right[1] * fwd[0];

    /* The interior of the view cone is |x| <= tan_hx*z and |y| <= tan_hy*z,
     * which is four planes through the camera, all written so that "inside" is
     * dot(n, p - cam) >= 0:
     *
     *     x + tan_hx*z >= 0   ->  n =  right + tan_hx*fwd
     *    -x + tan_hx*z >= 0   ->  n = -right + tan_hx*fwd
     *     y + tan_hy*z >= 0   ->  n =    up + tan_hy*fwd
     *    -y + tan_hy*z >= 0   ->  n =   -up + tan_hy*fwd
     *
     * The sign has to move to the BASIS vector for the second plane of each
     * pair. Writing +-tan*fwd on an unchanged basis (this function's first
     * version) gives one plane per pair with its interior on the wrong side:
     * at 45 degrees yaw the pair collapses to x = cam.x and y = cam.y, so one
     * half of the level is culled no matter where the camera looks — which at
     * the spawn pose is precisely the north-east half, i.e. the step block and
     * the whole curved cove. The arena suite's "the curved surface never
     * appeared on screen" is what caught it. */
    {
        float t[3];
        int s;
        for (s = 0; s < 2; s++) {
            float sc = s ? -1.0f : 1.0f;
            for (i = 0; i < 3; i++) t[i] = sc * r[i] + tan_hx * f[i];
            o = t[0] * cam[0] + t[1] * cam[1] + t[2] * cam[2];
            for (i = 0; i < 3; i++) v_planes[n][i] = t[i];
            v_planes[n][3] = o;
            n++;
        }
        for (s = 0; s < 2; s++) {
            float sc = s ? -1.0f : 1.0f;
            for (i = 0; i < 3; i++) t[i] = sc * up[i] + tan_hy * f[i];
            o = t[0] * cam[0] + t[1] * cam[1] + t[2] * cam[2];
            for (i = 0; i < 3; i++) v_planes[n][i] = t[i];
            v_planes[n][3] = o;
            n++;
        }
    }
    /* near (z = 1) and far (z = Q3W_FAR, not the projection's own 4096), in
     * dot(n,p) - o form. */
    o = f[0] * cam[0] + f[1] * cam[1] + f[2] * cam[2];
    for (i = 0; i < 3; i++) v_planes[n][i] = f[i];
    v_planes[n][3] = o + 1.0f;
    n++;
    for (i = 0; i < 3; i++) v_planes[n][i] = -f[i];
    /* dot(-f, p) - o = -(dot(f,p)) + dot(f,cam) + FAR = FAR - z. Getting this
     * sign wrong inverts the plane and culls the whole level — which is
     * exactly what the first version of this did, and what the arena suite's
     * "the most faces a frame drew was 0" caught. */
    v_planes[n][3] = -(o + Q3W_FAR);
    n++;
    return n;
}

/* v38.117: backface rejection by the face's OWN normal, no winding involved.
 *
 * The face stores a unit normal (bsp_face_finish derives it from the winding;
 * for a tessellated patch quad it is the true surface normal at that quad, not
 * a plane constant). A face whose normal points AWAY from the camera can only
 * show its back — and the rasterizer would still fill every one of its pixels
 * and let the depth test throw them away afterwards, which is the single
 * largest line item left in the fill budget (measured on the user's own
 * q3dm1 session: 39.8 of 92.4 Mcycles per frame).
 *
 * This is deliberately NOT GL_CULL_FACE. That path needs the screen winding to
 * agree with the stored winding — and the two culling experiments that died in
 * v38.116 died exactly there: the fixture's planar faces and its tessellated
 * quads disagree about what CW/CCW means. A dot product has no winding:
 * `dot(n, center - cam) >= 0` says "the eye is on or behind the face's front
 * half-space", which is true for a backface whichever way the indices happen
 * to be wound. The EPSILON keeps near-edge-on faces (walls seen along their
 * plane, where the dot is within float noise of zero) drawn rather than
 * guessed away; in world units it is about a wall's thickness.
 *
 * A face at or behind the eye passes the frustum's near plane only marginally,
 * so skip the test entirely when the face center is behind the camera — the
 * near plane already handles it. */
#define Q3W_BACKFACE_EPS 4.0f

/* v38.117, second convention: id's own rule, not a winding rule.
 *
 * The first attempt read the winding-derived normal — and the user's own
 * session proved both winding conventions wrong somewhere: walls and the
 * curved cove vanished under either sign. A serial dump of the file's own
 * normal data then showed why winding was never the right source: q3dm1's
 * planar faces carry per-vertex normals from q3map that can point along the
 * BRUSH side rather than the visible face (all -X at the park view, including
 * the faces the screen shows). id's renderer never uses screen winding for a
 * BSP face either: R_CullDotTri culls a face when
 * `dot(verts[i].normal, dir) <= 0` with dir = verts[0].xyz - cameraPos, using
 * the FILE's per-vertex normal. Do the same, per face, with the face's own
 * mean front normal: a face is drawn only when its file normal points toward
 * the eye, or its mean lightmap/poly color says it is double-sided.
 *
 * `frontNormal` below is the mean of the file's vertex normals, resolved once
 * per face at load (see bsp_face_finish). Patches get theirs from the same
 * Bezier combination that builds their position, so a curved surface culls
 * quad by quad with the true surface normal at that quad. */
static int face_is_backface(const q3bsp_face_t *f, const float cam[3]) {
    float dx = f->center[0] - cam[0];
    float dy = f->center[1] - cam[1];
    float dz = f->center[2] - cam[2];
    float side;

    if (dx * dx + dy * dy + dz * dz < 4.0f)
        return 0;               /* eye essentially on the surface: draw it */
    side = f->frontNormal[0] * dx + f->frontNormal[1] * dy +
           f->frontNormal[2] * dz;
    /* back = the file's front normal points AWAY from the eye. With
     * d = center - cam, a face looking AT the camera has dot < 0 (the park
     * view's own ground truth: wall -23, wall -15, floor -74 — all drawn), a
     * face looking away has dot > 0 (cull it). id's rule is the same test at
     * epsilon 0; ours keeps a 4-unit dead zone so edge-on faces are drawn,
     * not guessed away. */
    return side >= Q3W_BACKFACE_EPS;
}

/* Exact per-face reject against the planes built above (v38.116).
 *
 * A face is dropped only when ALL of its own vertices lie behind ONE plane, so
 * a partially visible face can never be dropped — that is the property the
 * bounding-sphere test did not have, and it is why v38.113 shipped a quorum
 * instead ("drop a face only when the PVS and the frustum agree", so a face the
 * PVS kept was never dropped by the frustum at all). On q3dm1 the quorum agreed
 * to drop nothing: every face the PVS kept was emitted, ~99% of them outside the
 * view cone, and TinyGL paid the transform + clip cascade to discover that.
 * Measured this release: 5430 triangles emitted per frame, 68 of them reaching
 * the raster. The history is not decoration: the sphere test's `d < -lim`
 * threshold is exactly an inexact version of the test below, and an inexact
 * frustum test is what hid the floor in v38.113.
 *
 * The planes are unnormalised, so the slack is in their own units: with |n|
 * near 1 it is about two world units, which keeps a face whose vertex sits on a
 * plane out of the argument entirely. */
#define Q3W_CULL_SLACK 2.0f

static int face_outside_frustum(const q3bsp_mesh_t *m, const q3bsp_face_t *f) {
    const int first = f->firstVert;
    int nv = f->numVerts;
    int i, v;

    if (!v_planes_n || nv <= 0) return 0;
    for (i = 0; i < v_planes_n; i++) {
        const float *p = v_planes[i];
        int behind = 1;

        for (v = 0; v < nv; v++) {
            const float *xyz = m->verts[first + v].xyz;
            float d = p[0] * xyz[0] + p[1] * xyz[1] + p[2] * xyz[2] - p[3];

            if (d >= -Q3W_CULL_SLACK) {
                behind = 0;
                break;
            }
        }
        if (behind) return 1;
    }
    return 0;
}

/* --- serial helpers ------------------------------------------------------
 * wr_int only formats now; the CALLER writes, and every line a test parses is
 * one write. write_serial_string is atomic per call, so a line assembled from
 * several calls could be (and was — a stray [LOAD] landed mid-line) split by
 * another task's output; texture lines are written whole. */
static int wr_int(char *dst, int v) {
    static char buf[13];
    int n = 0, i = 0;
    unsigned u;
    if (v < 0) { dst[i++] = '-'; u = (unsigned)(-v); } else u = (unsigned)v;
    if (u == 0) buf[n++] = '0';
    while (u && n < 12) { buf[n++] = (char)('0' + (u % 10)); u /= 10; }
    while (n) dst[i++] = buf[--n];
    dst[i] = '\0';
    return i;
}

/* v38.125: the draw flags a shader's definition asked for, appended to a tex
 * line — " flags=add", "culloff", or both. Nothing at all for the common
 * case, so every existing line keeps the shape the suites already parse. */
static int wr_shader_flags(char *dst, const q3w_slot_t *slot) {
    int i = 0;
    if (!slot || (!slot->additive && !slot->cullNone)) { dst[0] = '\0'; return 0; }
    for (const char *s = " flags="; *s; s++) dst[i++] = *s;
    if (slot->additive) { for (const char *s = "add"; *s; s++) dst[i++] = *s; }
    if (slot->additive && slot->cullNone) dst[i++] = ',';
    if (slot->cullNone) { for (const char *s = "culloff"; *s; s++) dst[i++] = *s; }
    dst[i] = '\0';
    return i;
}

/* --- TGA --------------------------------------------------------------- */
/* id's own tools write TGA, and this decodes what they write: uncompressed or
 * RLE colour-mapped-off images, 24 or 32 bits per pixel, either origin (the
 * image descriptor's bit 5 says which). Returns 0 and a malloc'd RGB buffer
 * (3 bytes per pixel, top-down) on success. */
static int tga_decode(const unsigned char *raw, int len, unsigned char **rgb_out,
                      int *w_out, int *h_out) {
    int id_len, cmap, type, w, h, bpp, desc, bytes;
    const unsigned char *src;
    unsigned char *rgb;
    int x, y;

    if (len < 18) return -1;
    id_len = raw[0];
    cmap   = raw[1];
    type   = raw[2];
    w      = raw[12] | (raw[13] << 8);
    h      = raw[14] | (raw[15] << 8);
    bpp    = raw[16];
    desc   = raw[17];

    if (cmap != 0 || (type != 2 && type != 10) || w <= 0 || h <= 0 ||
        w > 1024 || h > 1024 || (bpp != 24 && bpp != 32)) {
        return -2;
    }
    bytes = bpp / 8;
    src = raw + 18 + id_len;
    if ((int)(src - raw) > len) return -3;

    rgb = (unsigned char *)kmalloc((uint32_t)(w * h * 3));
    if (!rgb) return -4;

    if (type == 2) {                       /* uncompressed */
        if ((int)(src - raw) + w * h * bytes > len) { kfree(rgb); return -5; }
        for (y = 0; y < h; y++) {
            int row = (desc & 0x20) ? y : (h - 1 - y);
            for (x = 0; x < w; x++) {
                const unsigned char *p = src + ((size_t)y * w + x) * bytes;
                unsigned char *d = rgb + ((size_t)row * w + x) * 3;
                d[0] = p[2]; d[1] = p[1]; d[2] = p[0];
            }
        }
    } else {                               /* RLE */
        int total = w * h, done = 0;
        const unsigned char *p = src;
        while (done < total) {
            int count, rep;
            unsigned char px[4];
            if ((int)(p - raw) >= len) { kfree(rgb); return -6; }
            count = *p++;
            rep = count & 0x80;
            count = (count & 0x7F) + 1;
            if (done + count > total) { kfree(rgb); return -7; }
            if (rep) {
                if ((int)(p - raw) + bytes > len) { kfree(rgb); return -6; }
                px[0] = p[0]; px[1] = p[1]; px[2] = p[2];
                p += bytes;
                for (int i = 0; i < count; i++, done++) {
                    int row = (desc & 0x20) ? done / w : (h - 1 - done / w);
                    unsigned char *d = rgb + ((size_t)row * w + (done % w)) * 3;
                    d[0] = px[2]; d[1] = px[1]; d[2] = px[0];
                }
            } else {
                for (int i = 0; i < count; i++, done++) {
                    int row = (desc & 0x20) ? done / w : (h - 1 - done / w);
                    unsigned char *d = rgb + ((size_t)row * w + (done % w)) * 3;
                    d[0] = p[2]; d[1] = p[1]; d[2] = p[0];
                    p += bytes;
                }
                if ((int)(p - raw) > len) { kfree(rgb); return -6; }
            }
        }
    }

    *rgb_out = rgb;
    *w_out = w;
    *h_out = h;
    return 0;
}

/* A shader with no image on the volume still has to be drawable, and the
 * placeholder must be recognisable as one: a checkerboard whose two greys come
 * from the name's hash, with a magenta diagonal, so no screendump assertion
 * about real texture colours can ever pass by accident. */
static void placeholder_rgb(const char *name, unsigned char *px) {
    unsigned h = 2166136261u;
    int i, a, b;
    for (i = 0; name[i]; i++) {
        h ^= (unsigned char)name[i];
        h *= 16777619u;
    }
    a = 40 + (int)(h & 0x7F);
    b = 40 + (int)((h >> 8) & 0x7F);
    for (int y = 0; y < Q3W_PH_SIZE; y++) {
        for (int x = 0; x < Q3W_PH_SIZE; x++) {
            unsigned char *d = px + ((size_t)y * Q3W_PH_SIZE + x) * 3;
            int on = (((x >> 4) + (y >> 4)) & 1);
            int delta = x - y;
            int diag = (delta < 0 ? -delta : delta) < 3;
            d[0] = (unsigned char)(diag ? 255 : (on ? a : b));
            d[1] = (unsigned char)(diag ? 0 : (on ? a : b));
            d[2] = (unsigned char)(diag ? 255 : (on ? a : b));
        }
    }
}

static GLuint upload_rgb(const unsigned char *rgb, int w, int h) {
    GLuint id = 0;
    glGenTextures(1, &id);
    if (!id) return 0;
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, 3, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, (void *)rgb);
    return id;
}

/* --- shader-name -> image resolution (v38.111) ------------------------- */
/* A .bsp only NAMES its textures; which file a name means is decided by    */
/* a shader script (the definitions id's tools wrote beside the map) or,    */
/* when no definition exists, by the name itself standing alone as a path — */
/* Q3's "shaderless texture" convention. Each candidate path is then tried  */
/* with the extensions id's renderer tries: the image may be JPEG (every    */
/* retail texture set ships them) or TGA.                                  */

/* Case-insensitive fixed-length compare (the kernel side has no strncasecmp). */
static int q3w_ncaseeq(const char *a, const char *b, int n) {
    int i;
    for (i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return 1;
}

/* The parsed definitions, read once per process: id's own FS enumerates the
 * scripts, q3bsp.c parses them in place, and the pointers stay valid for the
 * session (a few KB at most). An empty set is cached too — a second walk
 * cannot find files the first did not. */
static q3bsp_shader_decls_t s_decls;
static int s_decls_tried;

static const q3bsp_shader_decl_t *q3w_decls_ready(void) {
    if (!s_decls_tried) {
        s_decls_tried = 1;
        q3bsp_read_shader_decls(&s_decls);
    }
    return s_decls.num > 0 ? &s_decls : NULL;
}

/* Best definition for a name: one that covers it (its name begins with the
 * shader's), shortest such name wins — "textures/x/floor" beats
 * "textures/x/floor_lit" as the definition of "textures/x/floor". Duplicate
 * definitions resolve to the LAST one (id keeps the last too). */
static const q3bsp_shader_decl_t *q3w_decl_for(const char *name, int len) {
    const q3bsp_shader_decls_t *d = q3w_decls_ready();
    const q3bsp_shader_decl_t *best = NULL;
    int i;
    if (!d) return NULL;
    for (i = 0; i < d->num; i++) {
        const q3bsp_shader_decl_t *c = &d->decls[i];
        if (c->nameLen < len) continue;
        if (q3w_ncaseeq(c->name, name, len) &&
            (!best || c->nameLen < best->nameLen))
            best = c;
    }
    return best;
}

/* Which decoder a resolved path needs, read off the path's OWN last dot.
 * Deciding from the path rather than from the extension argument is what lets
 * one probe serve both spellings: a base that already carries its extension
 * (id's scripts write "map …/killsky_1.tga") and a bare base the caller adds
 * an extension to. 0 = an extension this port does not decode (.png &c). */
static int q3w_image_fmt(const char *path) {
    const char *dot = NULL, *q;
    for (q = path; *q; q++) if (*q == '.') dot = q;
    if (!dot) return 0;
    if (q3w_ncaseeq(dot, ".jpg", 4) || q3w_ncaseeq(dot, ".jpeg", 5)) return 'j';
    if (q3w_ncaseeq(dot, ".tga", 4)) return 't';
    return 0;
}

/* The bare image name behind a qpath: copy `name` (lower-cased, qpaths are)
 * into `out` and drop a trailing .jpg/.jpeg/.tga/.png. THE naming rule id's
 * tools follow and the one scripts/q3a_data.py already implements: a shader
 * script spelling "…/killsky_1.tga" names the sky whose packed file is
 * killsky_1.jpg — the extension in the script is a convention, not a fact.
 * Returns the new length, or -1 when `out` is too small. In-place is legal
 * (out <= in). */
static int q3w_strip_image_ext(const char *name, int len, char *out, int outsize) {
    static const char *const exts[] = { ".jpg", ".jpeg", ".tga", ".png" };
    int i, cut = -1, n = 0;

    if (!name || len <= 0 || len + 1 > outsize) return -1;
    for (i = 0; i < len; i++) {
        char ch = name[i];
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        if (ch == '.') cut = n;
        out[n++] = ch;
    }
    out[n] = '\0';
    if (cut > 0) {
        for (i = 0; i < 4; i++) {
            int el = 0, k;
            while (exts[i][el]) el++;
            if (n - cut != el) continue;
            for (k = 0; k < el; k++) {
                if (out[cut + k] != exts[i][k]) break;
            }
            if (k == el) { out[cut] = '\0'; n = cut; break; }
        }
    }
    return n;
}

/* Try ONE candidate path + ONE extension. Returns 0 and an RGB buffer on a
 * real decode; negative on miss/failure. JPEG goes through q3jpeg (baseline
 * only, fixed point, no 64-bit divide); TGA through the decoder above. A
 * file that exists in an extension this port cannot decode (.png) is skipped
 * so later candidates still get their chance. An EMPTY extension is the
 * "exactly as written" probe: the path is the base itself. */
static int q3w_try_image(const char *base, int blen, const char *ext,
                         char *resolved, int rsize,
                         unsigned char **rgb, int *w, int *h, int *bytes) {
    char path[128];
    int pl = 0, i, fmt;
    unsigned char *raw = NULL;
    int len = 0;

    if (blen <= 0 || blen + 5 >= (int)sizeof(path)) return -9;
    for (i = 0; i < blen; i++) {
        char ch = base[i];
        if (ch >= 'A' && ch <= 'Z') ch += 32;   /* qpaths are lowercase */
        path[pl++] = ch;
    }
    for (const char *e = ext; *e; e++) path[pl++] = *e;
    path[pl] = '\0';

    if (q3bsp_read_file(path, &raw, &len) != 0 || !raw || len <= 0)
        return -1;

    fmt = q3w_image_fmt(path);
    if (fmt == 'j') {                           /* .jpg / .jpeg */
        int dw = 0, dh = 0, rc;
        unsigned char *out;
        if (q3jpeg_dims(raw, len, &dw, &dh) != 0 || dw <= 0 || dh <= 0) {
            q3bsp_free_file(raw);
            return -2;
        }
        out = (unsigned char *)kmalloc((uint32_t)dw * dh * 3);
        if (!out) { q3bsp_free_file(raw); return -8; }
        rc = q3jpeg_decode(raw, len, out, &dw, &dh);
        q3bsp_free_file(raw);
        if (rc != 0) { kfree(out); return -3; }
        *rgb = out;
        *w = dw;
        *h = dh;
    } else if (fmt == 't') {                    /* .tga */
        int rc = tga_decode(raw, len, rgb, w, h);
        q3bsp_free_file(raw);
        if (rc != 0) return -4;
    } else {                                    /* .png &c: exists, not ours */
        q3bsp_free_file(raw);
        return -5;
    }
    *bytes = len;
    if (pl < rsize) {
        for (i = 0; i <= pl; i++) resolved[i] = path[i];
    }
    return 0;
}

/* Resolve one shader name to a decoded RGB image. Candidates, in order:
 *
 *   1. the definition's image operand EXACTLY as the script spells it — the
 *      script may name the very file the volume holds, extension and all;
 *   2. the same operand with a known image extension removed, probed in id's
 *      extension order (.jpg before .tga — which is why the retail JPEGs win);
 *   3. the shader name itself, Q3's "shaderless texture" convention.
 *
 * v38.125: (1) and (2) are the fix for the demo's sky, lava and fire. id's
 * scripts spell the extension, so the previous single candidate appended a
 * SECOND one and the renderer hunted for "textures/skies/killsky_1.tga.jpg"
 * while killsky_1.jpg sat on the volume — every such shader fell back to the
 * placeholder checkerboard (the level's sky was a grey grid). (2) is also
 * what a `map $lightmap` first stage needs: that operand is engine-provided,
 * so q3bsp_read_shader_decls keeps looking and the definition's REAL image is
 * what arrives here (textures/skin/surface8_trans → textures/skin/surface8). */
static int q3w_resolve_shader_image(const char *shaderName,
                                    char *resolved, int rsize,
                                    unsigned char **rgb, int *w, int *h,
                                    int *bytes) {
    char sbase[128];
    int slen = 0, blen, e, rc;
    static const char *const exts[] = { ".jpg", ".jpeg", ".tga", ".png" };
    const q3bsp_shader_decl_t *d;

    while (shaderName[slen] && slen < 63) slen++;

    d = q3w_decl_for(shaderName, slen);
    if (d && d->imageLen > 0) {
        rc = q3w_try_image(d->image, d->imageLen, "", resolved, rsize,
                           rgb, w, h, bytes);
        if (rc == 0) return 0;
        if (rc == -8) return rc;                /* out of memory: stop */
        blen = q3w_strip_image_ext(d->image, d->imageLen, sbase,
                                   (int)sizeof(sbase));
        if (blen > 0) {
            for (e = 0; e < 4; e++) {
                rc = q3w_try_image(sbase, blen, exts[e], resolved, rsize,
                                   rgb, w, h, bytes);
                if (rc == 0) return 0;
                if (rc == -8) return rc;
            }
        }
    }
    for (e = 0; e < 4; e++) {                   /* the shaderless-texture path */
        rc = q3w_try_image(shaderName, slen, exts[e], resolved, rsize,
                           rgb, w, h, bytes);
        if (rc == 0) return 0;
        if (rc == -8) return rc;
    }
    return -1;
}

/* --- v38.124: the same resolution for a caller whose name is a FILE NAME ---
 *
 * A .bsp shader name never carries an extension ("textures/gothic_wall/…"),
 * so q3w_resolve_shader_image appends each candidate. An .md3 surface shader
 * name is the opposite: id's exporter wrote the literal file name, extension
 * and all — the demo's machinegun body says "models/weapons2/machinegun/
 * machinegun.tga" and its flash says "…/f_machinegun.TGA" (capital TGA, and
 * the file that actually exists is the .jpg). Passing those straight to the
 * resolver would look for "machinegun.tga.jpg". So: strip a known image
 * extension, then hand the bare base to the one resolution path — the shader
 * scripts, the name itself, and id's extension order (.jpg before .tga, which
 * is why the retail JPEGs win) all still apply, and the uppercase spelling
 * survives because both the resolver and q3bsp_read_file lower-case paths.
 *
 * Exported for q3viewmodel.c rather than copied: a second decoder for the same
 * two formats is exactly what would drift. */
int q3w_resolve_image(const char *name, char *resolved, int rsize,
                      unsigned char **rgb, int *w, int *h) {
    char base[128];
    int n = 0, i, bytes = 0;

    if (!name) return -1;
    for (i = 0; name[i] && n < (int)sizeof(base) - 1; i++) base[n++] = name[i];
    if (n <= 0) return -1;
    /* v38.125: the same strip the world's own definitions go through — one
     * helper, so the two paths cannot drift apart again (this is exactly the
     * rule that was missing from the world side: a name that spells its own
     * extension must not have a second one appended). */
    n = q3w_strip_image_ext(base, n, base, (int)sizeof(base));
    if (n <= 0) return -1;
    return q3w_resolve_shader_image(base, resolved, rsize, rgb, w, h, &bytes);
}

/* Upload an already-decoded image. The world's own cache is slot-indexed by
 * shader (above) and is wiped by every q3w_load, so a caller with a different
 * lifetime — the view model's three surfaces — must own its ids and upload
 * through here instead of borrowing a slot. */
GLuint q3w_upload_image(const unsigned char *rgb, int w, int h) {
    if (!rgb || w <= 0 || h <= 0) return 0;
    return upload_rgb(rgb, w, h);
}

void q3w_release_image(unsigned char *rgb) {
    if (rgb) kfree(rgb);
}

void q3w_unload(void) {
    for (int i = 0; i < Q3W_MAX_TEX; i++) {
        if (tex_id[i]) {
            GLuint id = tex_id[i];
            glDeleteTextures(1, &id);
        }
        tex_id[i] = 0;
        tex_slot[i].used = 0;
        tex_slot[i].fromDisk = 0;
        tex_slot[i].additive = 0;
        tex_slot[i].cullNone = 0;
        tex_slot[i].path[0] = '\0';
    }
    tex_shaders = tex_from_disk = tex_placeholders = 0;
    add_shaders = cull_shaders = 0;
    tex_inited = 0;
    v_faces = v_tris = v_culled = 0;
    v_add_faces = t_add_faces = 0;

}

int q3w_load(const q3bsp_mesh_t *m) {
    unsigned char *ph = NULL;

    q3w_unload();
    if (!m || !m->valid || m->numShaders <= 0) return 0;

    tex_shaders = m->numShaders;
    {
        static char sbuf[64];
        int i = 0;
        for (const char *s = "[Q3ARENA] textures: "; *s; s++) sbuf[i++] = *s;
        i += wr_int(sbuf + i, tex_shaders);
        for (const char *s = " shader(s) named by the map\n"; *s; s++) sbuf[i++] = *s;
        sbuf[i] = '\0';
        write_serial_string(sbuf);
    }
    /* One line that separates the two ways a level can come out dark: pages=0
     * means the file carried no lightmap data to render; pages=9 with a `raw`
     * near zero means the page was read but the sampling is wrong; pages=9 with
     * a plausible `raw` and `litmean` but a dark screendump means the
     * modulation never reached the rasterizer. `raw` is the page as the file
     * stores it, `litmean` what the levels' vertices actually carry after the
     * overbright shift (v38.114). */
    {
        static char lb[256];
        int p = 0, k, litMean[3];
        const char *s;

        for (k = 0; k < 3; k++)
            litMean[k] = (m->litVerts > 0) ? (m->litMean[k] / m->litVerts) : 0;
        s = "[Q3ARENA] lightmaps: pages=";
        while (*s) lb[p++] = *s++;
        p += wr_int(lb + p, m->numLightmaps);
        s = " dim="; while (*s) lb[p++] = *s++;
        p += wr_int(lb + p, m->lightmapDim);
        s = " bytes="; while (*s) lb[p++] = *s++;
        p += wr_int(lb + p, m->lightmapBytes);
        s = " shift="; while (*s) lb[p++] = *s++;
        p += wr_int(lb + p, m->lightmapShift);
        s = " raw="; while (*s) lb[p++] = *s++;
        for (k = 0; k < 3; k++) {
            p += wr_int(lb + p, m->lightmapMean[k]);
            lb[p++] = (k == 2) ? ' ' : ',';
        }
        s = "lit="; while (*s) lb[p++] = *s++;
        p += wr_int(lb + p, m->facesLit);
        s = " unlit="; while (*s) lb[p++] = *s++;
        p += wr_int(lb + p, m->facesUnlit);
        s = " litmean="; while (*s) lb[p++] = *s++;
        for (k = 0; k < 3; k++) {
            p += wr_int(lb + p, litMean[k]);
            lb[p++] = (k == 2) ? ' ' : ',';
        }
        s = "dropped="; while (*s) lb[p++] = *s++;
        p += wr_int(lb + p, m->lightmapDropped);
        lb[p++] = '\n';
        lb[p] = '\0';
        write_serial_string(lb);
    }

    ph = (unsigned char *)kmalloc((uint32_t)(Q3W_PH_SIZE * Q3W_PH_SIZE * 3));
    if (!ph) return 0;

    for (int i = 0; i < tex_shaders && i < Q3W_MAX_TEX; i++) {
        unsigned char *rgb = NULL;
        int w = 0, h = 0, bytes = 0;
        char rpath[128];
        GLuint id;
        q3w_slot_t *slot = &tex_slot[i];

        slot->used = 1;
        {
            int sl = 0;
            while (m->shaderNames[i][sl] && sl < Q3BSP_MAX_NAME - 1) sl++;
            for (int k = 0; k < sl; k++) slot->path[k] = m->shaderNames[i][k];
            slot->path[sl] = '\0';
            /* The definition's draw flags come from the script, not from the
             * BSP, and they are known even when no image was found — a
             * missing image must not lose the blend mode. */
            {
                const q3bsp_shader_decl_t *dd =
                    q3w_decl_for(m->shaderNames[i], sl);
                slot->additive = (dd && dd->additive) ? 1 : 0;
                slot->cullNone = (dd && dd->cullNone) ? 1 : 0;
                if (slot->additive) add_shaders++;
                if (slot->cullNone) cull_shaders++;
            }
        }

        if (q3w_resolve_shader_image(m->shaderNames[i], rpath,
                                     (int)sizeof(rpath), &rgb, &w, &h,
                                     &bytes) == 0 && rgb) {
            const char *fp = rpath;
            const char *fdot = 0;
            id = upload_rgb(rgb, w, h);
            slot->fromDisk = 1;
            slot->width = w;
            slot->height = h;
            tex_from_disk++;
            for (const char *q = rpath; *q; q++) if (*q == '.') fdot = q;
            /* One write: the suite's TEX_RE parses every field of this line. */
            {
                static char tbuf[Q3BSP_MAX_NAME + 200];
                int p = 0;
                for (const char *s = "[Q3ARENA] tex "; *s; s++) tbuf[p++] = *s;
                p += wr_int(tbuf + p, i);
                tbuf[p++] = ' ';
                for (int k = 0; slot->path[k]; k++) tbuf[p++] = slot->path[k];
                for (const char *s = " path="; *s; s++) tbuf[p++] = *s;
                for (; *fp; fp++) tbuf[p++] = *fp;
                for (const char *s = " fmt="; *s; s++) tbuf[p++] = *s;
                for (const char *s = (fdot && fdot[1] == 'j') ? "jpg" : "tga"; *s; s++)
                    tbuf[p++] = *s;
                for (const char *s = " size="; *s; s++) tbuf[p++] = *s;
                p += wr_int(tbuf + p, w);
                tbuf[p++] = 'x';
                p += wr_int(tbuf + p, h);
                for (const char *s = " bytes="; *s; s++) tbuf[p++] = *s;
                p += wr_int(tbuf + p, bytes);
                for (const char *s = " gl="; *s; s++) tbuf[p++] = *s;
                p += wr_int(tbuf + p, (int)id);
                /* v38.125: how the script says this shader is drawn, appended
                 * after the fields the suites already parse. */
                p += wr_shader_flags(tbuf + p, slot);
                tbuf[p++] = '\n';
                tbuf[p] = '\0';
                write_serial_string(tbuf);
            }
            kfree(rgb);
        } else {
            placeholder_rgb(m->shaderNames[i], ph);
            id = upload_rgb(ph, Q3W_PH_SIZE, Q3W_PH_SIZE);
            slot->fromDisk = 0;
            slot->width = Q3W_PH_SIZE;
            slot->height = Q3W_PH_SIZE;
            tex_placeholders++;
            {
                static char tbuf[Q3BSP_MAX_NAME + 96];
                int p = 0;
                for (const char *s = "[Q3ARENA] tex "; *s; s++) tbuf[p++] = *s;
                p += wr_int(tbuf + p, i);
                tbuf[p++] = ' ';
                for (int k = 0; slot->path[k]; k++) tbuf[p++] = slot->path[k];
                for (const char *s = " missing=1 placeholder gl="; *s; s++) tbuf[p++] = *s;
                p += wr_int(tbuf + p, (int)id);
                p += wr_shader_flags(tbuf + p, slot);
                tbuf[p++] = '\n';
                tbuf[p] = '\0';
                write_serial_string(tbuf);
            }
        }
        tex_id[i] = id;
    }

    kfree(ph);
    tex_inited = 1;
    return tex_from_disk + tex_placeholders;
}

void q3w_load_stats(int *shaders, int *fromDisk, int *placeholders) {
    if (shaders) *shaders = tex_shaders;
    if (fromDisk) *fromDisk = tex_from_disk;
    if (placeholders) *placeholders = tex_placeholders;
}

/* v38.125: the additive/cull-none census. `addShaders`/`cullShaders` count the
 * definitions that asked for it (load-time), `addFaces` the faces the last
 * additive pass drew and `addFacesRun` the run's total. */
void q3w_flag_stats(int *addShaders, int *cullShaders, int *addFaces,
                    int *addFacesRun) {
    if (addShaders) *addShaders = add_shaders;
    if (cullShaders) *cullShaders = cull_shaders;
    if (addFaces) *addFaces = v_add_faces;
    if (addFacesRun) *addFacesRun = t_add_faces;
}

/* How many of the last frame's faces came out of a lightmap page, and how many
 * the whole run has drawn that way. Zero for every frame of a map with no
 * lightmap lump, which is what makes this assertable rather than decorative. */
void q3w_light_stats(int *litFacesLast, int *litFacesTotal) {
    if (litFacesLast) *litFacesLast = v_lit_faces;
    if (litFacesTotal) *litFacesTotal = t_lit_faces;
}

/* The same classification the test performs on a screendump, applied to the
 * frame the renderer just produced. It exists because a screendump assertion
 * is at the mercy of the compositor: the window may be behind another window,
 * be moved by the WM, or simply not have been redrawn yet — this port's first
 * version of that test measured the wallpaper and reported a floor. Reading the
 * renderer's own buffer removes every one of those failure modes, and keeps the
 * screendump as evidence for a human rather than as the pass/fail gate. */
int q3w_histogram(const uint32_t *px, int pitch, int w, int h,
                  int *cyan, int *warm, int *stepgreen, int *violet,
                  int *bright, int *patch, int *sky, int *distinct,
                  int *wall) {
    unsigned char seen[4096 / 8];
    int n = 0, distinct_count = 0;
    int c_cyan = 0, c_warm = 0, c_step = 0, c_violet = 0, c_bright = 0, c_sky = 0;
    int c_patch = 0;
    int c_wall = 0;

    if (!px || w <= 0 || h <= 0) return 0;
    memset(seen, 0, sizeof(seen));

    for (int y = 0; y < h; y++) {
        const uint32_t *row = px + (size_t)y * pitch;
        for (int x = 0; x < w; x++) {
            uint32_t p = row[x];
            int r = (int)((p >> 16) & 0xFF);
            int g = (int)((p >> 8) & 0xFF);
            int b = (int)(p & 0xFF);
            int key, d;
            n++;
            key = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
            if (!(seen[key >> 3] & (1 << (key & 7)))) {
                seen[key >> 3] |= (unsigned char)(1 << (key & 7));
                distinct_count++;
            }
            /* The clear colour: everything a frame with nothing drawn is made
             * of (glClearColor(0.10, 0.18, 0.45) -> 25,46,115). */
            d = r - 25; if (d < 0) d = -d;
            if (d <= 12) {
                int dg = g - 46, db = b - 115;
                if (dg < 0) dg = -dg;
                if (db < 0) db = -db;
                if (dg <= 12 && db <= 12) { c_sky++; continue; }
            }
            /* The tessellated surface's magenta, tested by ratio rather than by
             * absolute level so the shading cannot hide it: red and blue both
             * more than twice green. No planar texture here is anywhere near
             * that (the sandstone's red is only ~1.2x its green), and the light
             * this renderer bakes never scales one channel more than another. */
            if (r > 40 && r > g * 2 && b > g * 2) c_patch++;
            else if (g > r + 25 && b > r + 25) c_cyan++;
            else if (r > g + 15 && g > b + 10 && r > 60) c_warm++;
            /* v38.117: the step/stepgreen bucket follows the same fix as the
             * wall bucket — the old thresholds (g>80) were calibrated on the
             * z-fight artefacts the pre-cull draw produced, not on the
             * texture's real shaded colour. The step texture at the baked
             * 0.4 sun is (61,70,29)/(45,54,16)/(80,83,48): green over red
             * slightly, green over blue strongly, mid-to-dark. */
            else if (g > r + 4 && g > b + 20 && g >= 45 && g < 110) c_step++;
            else if (b > r + 5 && b > g + 5 && r >= 30 && r < 80 &&
                     (r > g ? r - g : g - r) < 8) c_violet++;
            /* v38.117: the WALL bucket — a dark orange-brown, r > g > b with a
             * warm gap between red and green. This is the colour the wall
             * texture actually takes under this renderer: the generated
             * arena's sandstone at its baked 0.4 sun (67,54,38), and a
             * q3dm1-staged wall texture under lightmap shading, whose warm
             * pixels this bucket now sees (previously they were invisible:
             * the old warm test, r>60 && g>b+10, was tuned to a z-fight
             * artefact on the fixture's wall tops and rejected every
             * correctly-shaded wall pixel — see the pixels-line note in
             * q3_vm.c). */
            else if (r > g + 4 && g > b + 4 && r < 120 &&
                     (r > g ? r - g : g - r) <= g / 2) c_wall++;
            if (r + g + b > 600) c_bright++;
        }
    }

    if (cyan) *cyan = c_cyan;
    if (warm) *warm = c_warm;
    if (stepgreen) *stepgreen = c_step;
    if (violet) *violet = c_violet;
    if (bright) *bright = c_bright;
    if (patch) *patch = c_patch;
    if (sky) *sky = c_sky;
    if (distinct) *distinct = distinct_count;
    if (wall) *wall = c_wall;
    return n;
}

void q3w_frame_stats(int *facesDrawn, int *trisDrawn, int *facesCulled) {
    if (facesDrawn) *facesDrawn = v_faces;
    if (trisDrawn) *trisDrawn = v_tris;
    if (facesCulled) *facesCulled = v_culled;
}

/* v38.116: drain TinyGL's own cycle counters (tgl_cyc.h). Nothing here reads
 * the millisecond clock — that is the point: only the TSC can split a 35 ms GL
 * phase into setup and fill. */
void q3w_glsplit_stats(unsigned long long *vert, unsigned long long *fill,
                       unsigned int *tri) {
    if (vert) *vert = tgl_cyc_vert;
    if (fill) *fill = tgl_cyc_fill;
    if (tri) *tri = tgl_n_fill;
}

void q3w_glsplit_reset(void) {
    tgl_cyc_vert = 0;
    tgl_cyc_fill = 0;
    tgl_n_fill = 0;
}

/* `lit` = this vertex carries its own lightmap-sampled colour. The colours are
 * bytes because that is all a 128x128 RGB page holds; the divide is by 255.0f
 * rather than an integer division on purpose (the rasterizer's per-pixel
 * multiplier is a float). */
static void emit_vert(const q3bsp_vert_t *v, int lit) {
    glTexCoord2f(v->st[0], v->st[1]);
    if (lit)
        glColor3f((float)v->light[0] * (1.0f / 255.0f),
                  (float)v->light[1] * (1.0f / 255.0f),
                  (float)v->light[2] * (1.0f / 255.0f));
    glVertex3f(v->xyz[0], v->xyz[1], v->xyz[2]);
}

/* Emit one face as ONE GL_TRIANGLE_FAN of its own vertices.
 *
 * v38.119 briefly replaced this with a Sutherland-Hodgman clip of the face
 * against the view frustum in world space, on the theory that TinyGL's own
 * clipper was amplifying each face into ~19x as many raster triangles. It was
 * not, and the clip is gone again, because measuring said so twice over: on
 * one ISO, at one pinned pose, `noclip` (this path) ran 57 fps where the clip
 * ran 47, with gl 13.4 ms/frame against 17.5, for 8% fewer raster triangles -
 * the polygon pass costs more than the triangle splits it avoids. And there
 * were almost no splits to avoid: `glcyc tris` is a 20-FRAME total (it is
 * drained with the cycle counters) while the frame line's `tris` is one
 * frame's fan count, so `1197 source -> 20660 raster` compared two different
 * spans. Per frame a heavy q3dm1 view submits ~930 fan triangles and the
 * raster sees ~880 of them. The amplification was a units error in the
 * measurement, not a property of the renderer. */
static void emit_face(const q3bsp_mesh_t *m, const q3bsp_face_t *f, int lit) {
    const q3bsp_vert_t *fv = &m->verts[f->firstVert];
    int nv = f->numVerts;
    int i;

    glBegin(GL_TRIANGLE_FAN);
    emit_vert(&fv[0], lit);
    for (i = 1; i < nv; i++) emit_vert(&fv[i], lit);
    glEnd();
}

/* --- v38.121: front-to-back draw order ------------------------------------
 *
 * The user's v38.120 session (walking q3dm1, KVM) showed the shape of the
 * remaining cost: fill_kc holds ~97% of the raster cycles while `tris` stays
 * flat — pixels are being filled 2-3 times over, because faces were emitted
 * in BSP INDEX order, so far walls filled first, near walls filled over them,
 * and every overdraw pixel paid the full textured path before its z-test
 * rejected it. The fill also swung window to window (164-814 ms) exactly with
 * the camera's view, which is why walking felt unstable while standing still
 * (one view, warmed caches) read 56 fps.
 *
 * The fix is the order id's own engine draws in: collect the faces that
 * survive culling, sort them NEAREST first, and let the early z-test turn
 * every occluded pixel into a ~15-cycle rejection instead of an ~80-cycle
 * fill. Pixels rendered are identical either way (same z-winner per pixel for
 * opaque faces), so the suites' histograms stay exactly what they assert on.
 * Sort is an in-place heapsort — O(n log n) worst case, no recursion, no
 * 64-bit divide (this link has no __udivdi3) — over squared distances, so no
 * sqrt. `nosort` on the command line restores the v38.120 index order for A/B.
 */
static int    q3w_sort_enabled = 1;
static int   *dl_idx;              /* faces to draw this frame            */
static float *dl_key;              /* squared distance, cam -> face centre */
static int    dl_cap;
static int    dl_entries;          /* faces the list holds this frame     */
static int    dl_sorted;           /* 1 when the emitted order was sorted */

/* v38.125: the additive list. id draws a shader whose stages blend
 * GL_ONE GL_ONE in its own pass after the opaque world (`sort additive` in
 * models.shader), because an additive image contributed in place both paints
 * its black surround over what is already there and, with depth writes on,
 * punches a hole where the geometry behind it is drawn afterwards. Sizing is
 * the same worst case as the sort pool: every face could be additive. */
static int   *add_idx;
static int    add_cap;
static int    add_n;

void q3w_set_sort(int on) { q3w_sort_enabled = on; }

void q3w_sort_stats(int *enabled, int *n, int *sorted) {
    if (enabled) *enabled = q3w_sort_enabled;
    if (n) *n = dl_cap;              /* pool capacity: the worst-case count */
    if (sorted) *sorted = dl_sorted;
}

static void dl_grow(int need) {
    int cap;
    if (dl_cap >= need) return;
    cap = dl_cap ? dl_cap * 2 : 2048;
    while (cap < need) cap *= 2;
    kfree(dl_idx);
    kfree(dl_key);
    kfree(add_idx);
    dl_idx = (int *)kmalloc((uint32_t)cap * sizeof(int));
    dl_key = (float *)kmalloc((uint32_t)cap * sizeof(float));
    add_idx = (int *)kmalloc((uint32_t)cap * sizeof(int));
    dl_cap = (dl_idx && dl_key) ? cap : 0;
    add_cap = (dl_idx && dl_key && add_idx) ? cap : 0;
    if (!dl_cap) {
        kfree(dl_idx);
        kfree(dl_key);
        dl_idx = 0;
        dl_key = 0;
    }
    if (!add_cap) {
        /* No additive list: such faces are then drawn inside the main pass,
         * which w_bind_shader still blends — one frame's worth of worse
         * order, never a black box. */
        kfree(add_idx);
        add_idx = 0;
    }
}

static void dl_sift(float *k, int *ix, int root, int end) {
    for (;;) {
        int child = 2 * root + 1;
        if (child > end) break;
        if (child + 1 <= end && k[child + 1] > k[child]) child++;
        if (k[root] >= k[child]) break;
        {
            float kf = k[root]; k[root] = k[child]; k[child] = kf;
            int xi = ix[root]; ix[root] = ix[child]; ix[child] = xi;
        }
        root = child;
    }
}

static void dl_heapsort(int n) {
    int i;
    for (i = n / 2 - 1; i >= 0; i--)
        dl_sift(dl_key, dl_idx, i, n - 1);
    for (i = n - 1; i > 0; i--) {
        float kf = dl_key[0]; dl_key[0] = dl_key[i]; dl_key[i] = kf;
        int xi = dl_idx[0]; dl_idx[0] = dl_idx[i]; dl_idx[i] = xi;
        dl_sift(dl_key, dl_idx, 0, i - 1);
    }
}

/* Per-face GL state + emit, in the order the draw list names them. The
 * shader/lit logic is byte-for-byte the loop it replaces (v38.108..38.120). */
static int g_started, g_shader;

static void dl_emit(const q3bsp_mesh_t *m, const q3bsp_face_t *f) {
    int lit;
    if (f->shaderNum != g_shader) {
        if (g_started) glEnd();
        glBindTexture(GL_TEXTURE_2D,
                      tex_id[f->shaderNum < Q3W_MAX_TEX ? f->shaderNum : 0]);
        glBegin(GL_TRIANGLES);
        g_started = 1;
        g_shader = f->shaderNum;
    }
    /* The loader sets lightmapNum only when it really has that page, so
     * this one test is what picks lighting (v38.114) over the sun. */
    lit = (f->lightmapNum >= 0);
    if (!lit) glColor3f(f->light, f->light, f->light);
    emit_face(m, f, lit);
    if (lit) { v_lit_faces++; t_lit_faces++; }
    v_faces++;
    v_tris += f->numVerts - 2;
}

void q3w_vis_stats(int *marked, int *total, int *cluster, int *leafs) {
    if (marked) *marked = v_vis_all ? -1 : v_marked;
    if (total) *total = v_total_faces;
    if (cluster) *cluster = v_cluster;
    if (leafs) *leafs = v_vis_leafs;
}

void q3w_cull_stats(int *byVis, int *byFrustum, int *planes, int *byBack) {
    if (byVis) *byVis = v_culled_vis;
    if (byFrustum) *byFrustum = v_culled_frustum;
    if (planes) *planes = v_planes_n;
    if (byBack) *byBack = v_culled_back;
}

/* One line, once per run: the planes culling is measured against. Serial
 * output is the only debugger this kernel has, and a wrong sign in one of
 * these six is invisible in every number except "the level vanished". */
static void frustum_log_once(const float cam[3], const float fwd[3]) {
    char lb[256];
    int p = 0;
    const char *s;

    if (v_planes_logged) return;
    v_planes_logged = 1;
    s = "[Q3ARENA] frustum: cam=(";
    for (int k = 0; s[k] && p < 200; k++) lb[p++] = s[k];
    for (int k = 0; k < 3; k++) {
        p += wr_int(lb + p, (int)cam[k]);
        lb[p++] = k == 2 ? ')' : ',';
    }
    lb[p++] = ' ';
    s = "fwd=(";
    for (int k = 0; s[k] && p < 220; k++) lb[p++] = s[k];
    for (int k = 0; k < 3; k++) {
        p += wr_int(lb + p, (int)(fwd[k] * 1000.0f));
        lb[p++] = k == 2 ? ')' : ',';
    }
    lb[p++] = '\n';
    lb[p] = '\0';
    write_serial_string(lb);
    for (int i = 0; i < v_planes_n; i++) {
        p = 0;
        s = "[Q3ARENA] plane ";
        for (int k = 0; s[k] && p < 200; k++) lb[p++] = s[k];
        p += wr_int(lb + p, i);
        lb[p++] = ':';
        lb[p++] = ' ';
        /* Scaled by 1000 like `fwd` above: these normals are unnormalised and
         * their components are fractions, so `(int)0.9999` prints as 0 and a
         * correct up plane reads as a zero normal — which cost this release a
         * diagnostic round of "is the vertical pair degenerate?". */
        for (int k = 0; k < 4; k++) {
            p += wr_int(lb + p, (int)(v_planes[i][k] * (k < 3 ? 1000.0f : 1.0f)));
            lb[p++] = (k == 3) ? '\n' : ' ';
        }
        lb[p] = '\0';
        write_serial_string(lb);
    }
}void q3w_draw(const q3bsp_mesh_t *m, const float cam[3], const float fwd[3],
              const float right[3], float tan_hx, float tan_hy) {
    int vis = 0;
    static int logged_cluster = -999;

    v_faces = v_tris = v_culled = 0;
    v_lit_faces = 0;
    v_add_faces = 0;
    v_marked = 0;
    v_culled_vis = v_culled_frustum = 0;
    v_culled_back = 0;
    v_total_faces = 0;
    if (!m || !m->valid || !m->numFaces || !tex_inited) return;
    v_total_faces = m->numFaces;

    glEnable(GL_TEXTURE_2D);
    /* v38.116: backface culling was tried here and SHIPPED OFF, deliberately.
     * The prize is real — roughly half the PVS-kept faces face away from the
     * camera — but the experiment surfaced two facts that do not fit in one
     * release: (1) this fork's glFrontFace op was genuinely BROKEN (misc.c
     * stored the raw GL enum into the 0/1 flag clip.c XORs; FIXED this release,
     * which is why every earlier attempt had measured a silent no-op), and
     * (2) with the cull actually active, the fixture's walls vanish under BOTH
     * front-face settings while floor and ceiling survive CW — a wall-winding
     * property nobody has explained yet. Culling stays OFF until that is
     * understood on its own milestone; half a scene is not a tuning knob. */
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    /* v38.114: per-vertex light needs per-vertex colours, and GL_FLAT collapses
     * a lightmap gradient to whichever vertex the rasterizer took the colour
     * from. A mesh with no lightmaps keeps GL_FLAT: every face emits one colour
     * for all of its vertices either way, so a fixture arena draws pixel for
     * pixel what it drew before this release. */
    glShadeModel(m->numLightmaps > 0 ? GL_SMOOTH : GL_FLAT);

    /* Stage 1: what the map's own PVS says a camera here can see. */
    if (m->vis.ready) {
        v_marked = vis_mark_faces(m, cam);
        vis = (v_marked > 0);
        if (logged_cluster != v_cluster) {
            char lb[128];
            int p = 0;
            const char *s = "[Q3ARENA] vis: cluster=";
            logged_cluster = v_cluster;
            for (int k = 0; s[k] && p < 100; k++) lb[p++] = s[k];
            p += wr_int(lb + p, v_cluster);
            s = " leaf=";
            for (int k = 0; s[k] && p < 110; k++) lb[p++] = s[k];
            p += wr_int(lb + p, v_leaf);
            s = " leafs=";
            for (int k = 0; s[k] && p < 116; k++) lb[p++] = s[k];
            p += wr_int(lb + p, v_vis_leafs);
            s = " marked=";
            for (int k = 0; s[k] && p < 122; k++) lb[p++] = s[k];
            p += wr_int(lb + p, v_marked);
            lb[p++] = '/';
            p += wr_int(lb + p, m->numFaces);
            lb[p++] = '\n';
            lb[p] = '\0';
            write_serial_string(lb);
        }
    }

    /* Stage 2: the projection's own planes. Without a camera basis (the
     * hand-built fallback paths draw with none) only the depth test applies. */
    v_planes_n = 0;
    if (right) v_planes_n = frustum_planes(cam, fwd, right, tan_hx, tan_hy);
    frustum_log_once(cam, fwd);

    /* v38.121: the cull pass builds a front-to-back draw list when the sort
     * is on (glcyc says fill is ~97% of the raster and the user's walk read
     * fill swings 164-814 ms/window — index order overdraws 2-3x and pays the
     * full texel path on every overdraw pixel). ONE pass: the pool is sized to
     * numFaces once at load (the first A/B ran the cull twice to size a
     * buffer and gave back most of what the sort saved). `nosort` keeps the
     * v38.120 inline emit exactly as it was, for A/B on one ISO. */
    dl_sorted = 0;
    dl_entries = 0;
    add_n = 0;
    if (q3w_sort_enabled) {
        dl_grow(m->numFaces);
        if (!dl_cap) q3w_sort_enabled = 0;   /* out of memory: v38.120 path */
    }

    if (q3w_sort_enabled) {
        int count = 0;
        for (int i = 0; i < m->numFaces; i++) {
            const q3bsp_face_t *f = &m->faces[i];
            float dx = f->center[0] - cam[0];
            float dy = f->center[1] - cam[1];
            float dz = f->center[2] - cam[2];

            if (vis && !vis_mark[i]) {                    /* not in the PVS */
                v_culled++;
                v_culled_vis++;
                continue;
            }
            if (!w_cull_none(f->shaderNum) && face_is_backface(f, cam)) {
                /* v38.117: counted but NOT double-counted in v_culled — the
                 * frame line's arithmetic (`drawn + culled == numFaces`) is
                 * the PVS stage's contract and the suites assert it. v38.125:
                 * a `cull none` shader (fire, lava) skips the test — it is two
                 * sided, and id draws it that way. */
                v_culled_back++;
                continue;
            }
            if (v_planes_n) {
                if (face_outside_frustum(m, f)) {
                    v_culled++;
                    v_culled_frustum++;
                    continue;
                }
            } else {
                float depth = dx * fwd[0] + dy * fwd[1] + dz * fwd[2];
                if (depth < -f->radius || depth > Q3W_FAR) {
                    v_culled++;
                    v_culled_frustum++;
                    continue;
                }
            }
            /* Additive faces are collected for the second pass below: they
             * must land on top of the opaque world, not inside it. */
            if (add_cap && tex_slot[f->shaderNum < Q3W_MAX_TEX ? f->shaderNum : 0].additive) {
                if (add_n < add_cap) { add_idx[add_n++] = i; continue; }
            }
            dl_key[count] = dx * dx + dy * dy + dz * dz;
            dl_idx[count] = i;
            count++;
        }
        dl_heapsort(count);        /* ascending distance = nearest first */
        dl_entries = count;
        dl_sorted = 1;
    }

    {
        int started = 0;
        int cur_shader = -1;
        int n_emit = dl_sorted ? dl_entries : m->numFaces;
        for (int k = 0; k < n_emit; k++) {
            const q3bsp_face_t *f;
            int lit;
            if (dl_sorted) {
                f = &m->faces[dl_idx[k]];   /* nearest-first, culls already run */
            } else {
                /* v38.120 inline path: culls run here, in index order. */
                int i = k;
                float dx, dy, dz, depth;
                f = &m->faces[i];
                dx = f->center[0] - cam[0];
                dy = f->center[1] - cam[1];
                dz = f->center[2] - cam[2];
                if (vis && !vis_mark[i]) {
                    v_culled++; v_culled_vis++;
                    continue;
                }
                if (!w_cull_none(f->shaderNum) && face_is_backface(f, cam)) {
                    v_culled_back++;
                    continue;
                }
                if (v_planes_n) {
                    if (face_outside_frustum(m, f)) {
                        v_culled++; v_culled_frustum++;
                        continue;
                    }
                } else {
                    depth = dx * fwd[0] + dy * fwd[1] + dz * fwd[2];
                    if (depth < -f->radius || depth > Q3W_FAR) {
                        v_culled++; v_culled_frustum++;
                        continue;
                    }
                }
                /* Same split as the sorted path: additive last. */
                if (add_cap && tex_slot[f->shaderNum < Q3W_MAX_TEX ? f->shaderNum : 0].additive) {
                    if (add_n < add_cap) { add_idx[add_n++] = i; continue; }
                }
            }
            if (f->shaderNum != cur_shader) {
                if (started) glEnd();
                w_bind_shader(f->shaderNum);
                glBegin(GL_TRIANGLES);
                started = 1;
                cur_shader = f->shaderNum;
            }
            /* The loader sets lightmapNum only when it really has that page,
             * so this one test is what picks lighting (v38.114) over the sun. */
            lit = (f->lightmapNum >= 0);
            if (!lit) glColor3f(f->light, f->light, f->light);
            /* v38.118: emit the face as ONE GL_TRIANGLE_FAN instead of a
             * manual triangle list that re-sent the first vertex 2*(n-3)
             * extra times — a 15-vertex face cost 39 glVertex3f where 15
             * carry all the information, so ~2.6x of the transform/clip work
             * per face was spent re-deriving identical vertices. The fork's
             * fan rolls vertex[1] forward and draws (v0,vk,vk+1) — identical
             * triangles, winding and per-vertex colours to the list it
             * replaces; only the redundant re-emissions are gone (measured
             * on the user's session: the vertex path p50 11.5 Mcyc/frame,
             * where this duplication lives). Costs one glBegin/glEnd per face
             * instead of per shader batch — hundreds, not thousands, of state
             * switches.
             *
             * v38.119: the fan is now cut to the view frustum first
             * (emit_face above) so the clipper inside TinyGL has nothing left
             * to split.
             *
             * v38.121: with the sort on, fans are emitted nearest-first, so
             * the early z-test rejects occluded pixels before their texel is
             * ever sampled. */
            emit_face(m, f, lit);
            if (lit) { v_lit_faces++; t_lit_faces++; }
            v_faces++;
            v_tris += f->numVerts - 2;
        }
        if (started) glEnd();
    }

    /* v38.125: the additive pass — id's `sort additive`, and the reason the
     * demo's torches and the muzzle flash stop being black boxes. Every face
     * collected above is a shader whose stage blends GL_ONE GL_ONE: drawn in
     * place it would (a) paint the image's black surround over the level and
     * (b) with the depth buffer written, punch a hole wherever geometry
     * behind it is drawn later. Here: depth TEST stays on (a wall in front
     * still occludes the fire), depth WRITE goes off (additive layers must
     * not hide each other, and nothing after them needs their depth), and the
     * blend is set once for the whole list — additive is commutative, so the
     * order inside the list is irrelevant. */
    if (add_n > 0) {
        int started = 0, cur_shader = -1;
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glDepthMask(GL_FALSE);
        for (int k = 0; k < add_n; k++) {
            const q3bsp_face_t *f = &m->faces[add_idx[k]];
            int lit;
            if (f->shaderNum != cur_shader) {
                if (started) glEnd();
                w_bind_shader(f->shaderNum);
                glBegin(GL_TRIANGLES);
                started = 1;
                cur_shader = f->shaderNum;
            }
            lit = (f->lightmapNum >= 0);
            if (!lit) glColor3f(f->light, f->light, f->light);
            emit_face(m, f, lit);
            if (lit) { v_lit_faces++; t_lit_faces++; }
            v_faces++;
            v_tris += f->numVerts - 2;
        }
        if (started) glEnd();
        v_add_faces = add_n;
        t_add_faces += add_n;
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }
    glDisable(GL_CULL_FACE);
    glDisable(GL_TEXTURE_2D);
}
