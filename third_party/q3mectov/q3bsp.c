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
 *   * Lightmaps come out of the file (v38.114). Every surface that names a
 *     lightmap page is sampled at each vertex's own page-space coordinate and
 *     the result is stored as the vertex colour the renderer modulates its
 *     texture with — which is what id's lightmap stage does on the GPU. A
 *     surface without a page (q3dm1 has 75 of them, and every generated fixture
 *     map has nothing but them) keeps the v38.113 fallback: the face's own unit
 *     normal shaded against a fixed sun. That fallback is also what makes the
 *     screendump tests assertable — an unlit face would be indistinguishable
 *     from a hole.
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

/* ----------------------------------------------------------------------- */
/* Shader-script definitions (v38.111).                                    */
/*                                                                        */
/* A .bsp only ever names its textures — the definitions that say WHICH     */
/* image a name means live in a shader script under scripts/. id's FS can   */
/* them (FS_GetFileList), so this walks the same list, parses each file in  */
/* place, and returns pointers INTO the loaded bytes; the caller frees      */
/* buffers, not names. The parser is deliberately minimal: it finds         */
/* definition blocks, the first REAL image each names (a map/clampmap      */
/* operand, or an animMap's first frame) and the two flags the draw pass    */
/* reads off that same stage — additive blending and cull-none. skyParms    */
/* faces and tcMod are the lightmap phase's work and are skipped.           */
/*                                                                          */
/* v38.125: taking the operand VERBATIM was the bug. id's scripts spell the */
/* extension ("map textures/skies/killsky_1.tga") and the renderer then    */
/* probed "…killsky_1.tga.jpg", so every such shader — the sky, the lava,   */
/* the torches — came out as the placeholder checkerboard while the two     */
/* images it named sat on the volume. The rule id's own tools follow, and   */
/* the one scripts/q3a_data.py already implements: engine-provided          */
/* ($lightmap, $whiteimage, …) operands are not files, keep looking; a real */
/* operand with an extension names that file, and the renderer strips the   */
/* extension before trying id's extension order.                            */
/* ----------------------------------------------------------------------- */

/* Skip whitespace and the two comment forms Q3's sources are full of: a     */
/* commented-out `map` line is common, and treating it as live would bind a  */
/* name to an image the level never draws.                                  */
static const char *decl_skip_ws(const char *p, const char *end) {
    for (;;) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' ||
                           *p == '\n'))
            p++;
        if (p + 1 < end && p[0] == '/' && p[1] == '/') {
            p += 2;
            while (p < end && *p != '\n') p++;
            continue;
        }
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
            p = (p + 1 < end) ? p + 2 : end;
            continue;
        }
        return p;
    }
}

/* One token (quoted, bare, or a single brace), returned as [start, end)     */
/* with the quotes excluded. Returns p on "nothing here" (token == end).      */
/* A brace is a token in its own right — the caller counts them to find the  */
/* body of a definition. Scanning braces as *separators* (the first version  */
/* did) yields a zero-length token that does not advance p, so the parser    */
/* bailed out at the first '{' and every script produced zero definitions.   */
static const char *decl_token(const char *p, const char *end,
                              const char **tok, int *tokLen) {
    p = decl_skip_ws(p, end);
    if (p >= end) { *tok = p; *tokLen = 0; return p; }
    if (*p == '"') {
        const char *s = ++p;
        while (p < end && *p != '"') p++;
        *tok = s; *tokLen = (int)(p - s);
        return (p < end) ? p + 1 : p;
    }
    if (*p == '{' || *p == '}') {
        *tok = p; *tokLen = 1;
        return p + 1;
    }
    {
        const char *s = p;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\r' &&
               *p != '\n' && *p != '{' && *p != '}')
            p++;
        *tok = s; *tokLen = (int)(p - s);
        return p;
    }
}

/* Case-insensitive compare of a token against a literal, lexer-style.       */
/* Case-insensitive token compare against `lit`, which MUST be spelled in
 * lower case: the TOKEN is folded, the literal is not. v38.125 spelled two of
 * these in the file's own mixed case ("GL_ONE", "animMap") and they could
 * never match — the flame's blendFunc pair and id's animMap-only torch stages
 * silently fell through. A literal with an upper-case letter in it is a bug
 * here, not a style choice. */
static int decl_is(const char *t, int n, const char *lit) {
    int i;
    for (i = 0; i < n; i++) {
        char a = t[i];
        char b = lit[i];
        if (!b) return 0;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (a != b) return 0;
    }
    return lit[i] == '\0';
}

/* v38.128: numbers out of a shader script. `.1`, `0.05`, `-2` and `3` all
 * occur in id's own scripts, and this file links no libc, so there is no
 * strtod to lean on. Returns 1 when the token held a number. */
static int decl_float(const char *t, int n, float *out) {
    int i = 0, neg = 0, any = 0;
    float v = 0.0f, f = 0.1f;
    if (n <= 0 || !out) return 0;
    if (t[i] == '-' || t[i] == '+') { neg = (t[i] == '-'); i++; }
    while (i < n && t[i] >= '0' && t[i] <= '9') {
        v = v * 10.0f + (float)(t[i] - '0');
        i++;
        any = 1;
    }
    if (i < n && t[i] == '.') {
        i++;
        while (i < n && t[i] >= '0' && t[i] <= '9') {
            v += (float)(t[i] - '0') * f;
            f *= 0.1f;
            i++;
            any = 1;
        }
    }
    if (!any) return 0;
    *out = neg ? -v : v;
    return 1;
}

static int decl_atoi(const char *t, int n) {
    int i = 0, neg = 0, v = 0;
    if (n > 0 && (t[0] == '-' || t[0] == '+')) { neg = (t[0] == '-'); i = 1; }
    for (; i < n; i++) {
        if (t[i] < '0' || t[i] > '9') break;
        v = v * 10 + (t[i] - '0');
    }
    return neg ? -v : v;
}

int q3bsp_read_shader_decls(q3bsp_shader_decls_t *out) {
    /* id's FS_GetFileList is buffer-shaped: NUL-separated names, count      */
    /* returned. One call lists every shader script on the search path —    */
    /* or none, which is success-with-zero-decls and leaves the renderer's   */
    /* direct-image fallback resolving every name. */
    char listbuf[4096];
    int numFiles;
    const char *fname;
    int cap = 0, num = 0, bufCap, numBufs = 0;
    q3bsp_shader_decl_t *decls = NULL;
    unsigned char **buffers = NULL;
    int rc = 0, i;

    if (!out) return -1;
    out->decls = NULL; out->num = 0;
    out->buffers = NULL; out->numBuffers = 0;

    numFiles = FS_GetFileList("scripts", ".shader", listbuf, (int)sizeof(listbuf));
    if (numFiles <= 0) {
        Com_Printf("[Q3BSP] shader scripts: none listed (%d)\n", numFiles);
        return 0;
    }

    cap = 16;
    decls = (q3bsp_shader_decl_t *)kmalloc((uint32_t)(cap * sizeof(*decls)));
    bufCap = numFiles;
    buffers = (unsigned char **)kmalloc((uint32_t)(bufCap * sizeof(*buffers)));
    if (!decls || !buffers) {
        if (decls) kfree(decls);
        if (buffers) kfree(buffers);
        return -5;
    }

    fname = listbuf;
    for (i = 0; i < numFiles && rc == 0; i++, fname += strlen(fname) + 1) {
        unsigned char *buf = NULL;
        int len = 0;
        const char *p, *end;
        char qpathBuf[96];
        int pl = 0;

        /* Rebuild "scripts/<name>.shader" from the listed relative name.     */
        {
            const char *src = fname;
            const char *lit = "scripts/";
            while (*lit && pl < (int)sizeof(qpathBuf) - 1) qpathBuf[pl++] = *lit++;
            while (*src && pl < (int)sizeof(qpathBuf) - 1) qpathBuf[pl++] = *src++;
            qpathBuf[pl] = '\0';
        }
        /* id lowercases every qpath it stores; match that behaviour so       */
        /* FS_ReadFile resolves the file on a case-folding FS too.            */
        for (int q = 7; q < pl; q++)
            if (qpathBuf[q] >= 'A' && qpathBuf[q] <= 'Z')
                qpathBuf[q] += 32;

        if (q3bsp_read_file(qpathBuf, &buf, &len) != 0 || !buf || len <= 0)
            continue;                       /* vanished or unreadable: skip */
        if (numBufs >= bufCap) {
            q3bsp_free_file(buf);
            continue;
        }
        buffers[numBufs++] = buf;

        p = (const char *)buf;
        end = p + len;
        while (p < end) {
            const char *nameTok; int nameLen;
            int depth = 0;
            int captured = 0;
            int add = 0, culloff = 0, curDecl = -1;

            p = decl_token(p, end, &nameTok, &nameLen);
            if (nameLen <= 0) break;

            /* Flags may sit between the name and the opening brace; skip     */
            /* until the brace that opens this definition's body.             */
            while (p < end) {
                const char *t; int tl;
                p = decl_token(p, end, &t, &tl);
                if (tl == 1 && t[0] == '{') { depth = 1; break; }
                if (tl == 0) break;
            }
            if (!depth) break;              /* truncated file: stop cleanly */

            /* Walk the body tracking brace depth; capture the FIRST real    */
            /* image it names (nested stages included) — a definition binds  */
            /* one name to one image here, and stage ordering stays the      */
            /* lightmap phase's concern. `stageDepth` is the depth inside    */
            /* the stage that supplied the image: the blendFunc that belongs */
            /* to THAT stage is what decides how the image is drawn, so the  */
            /* additive search stops at the stage's closing brace (a glow    */
            /* layer three stages down must not turn the base image         */
            /* additive — that is textures/gothic_light/pentagram_light1_1K).*/
            {
                int capDepth = -1;          /* depth inside the captured stage */
                int blendWant = 0;          /* 1: want src, 2: want dst */
                int pairAdd = 0;            /* last blendFunc was GL_ONE GL_ONE */
                int pairDepth = -1;         /* the depth that blendFunc sat at */
                const char *blA = NULL; int blALen = 0;
                /* v38.128: the definition's stages, accumulated LOCALLY and
                 * copied onto the decl at the end. Local, because the decl
                 * itself is only created when the first real image turns up —
                 * and a stage's `{` (and even its `map` line) can be read
                 * before that. `skyparms`/`surfaceparm sky` are definition
                 * level and are staged the same way. */
                int stageIdx = -1;          /* the stage being read, -1 = none */
                int lnstages = 0;
                int lsky = 0, lcloud = 0, lfarbox = 0;
                q3bsp_shader_stage_t lstages[Q3BSP_MAX_STAGES];
                memset(lstages, 0, sizeof(lstages));

                while (p < end && depth > 0) {
                    const char *t; int tl;
                    p = decl_token(p, end, &t, &tl);
                    if (tl <= 0) break;
                    if (tl == 1 && t[0] == '{') {
                        depth++;
                        /* A brace at depth 2 opens a STAGE (depth 1 is the
                         * definition's own body). */
                        if (depth == 2) {
                            stageIdx = (lnstages < Q3BSP_MAX_STAGES) ? lnstages
                                                                     : -1;
                            if (stageIdx >= 0) {
                                memset(&lstages[stageIdx], 0,
                                       sizeof(lstages[stageIdx]));
                                lnstages++;
                            }
                        }
                        continue;
                    }
                    if (tl == 1 && t[0] == '}') {
                        depth--;
                        if (capDepth >= 0 && depth < capDepth) capDepth = -1;
                        if (depth < 2) stageIdx = -1;
                        continue;
                    }
                    if (blendWant == 1) { blA = t; blALen = tl; blendWant = 2; continue; }
                    if (blendWant == 2) {
                        blendWant = 0;
                        pairAdd = decl_is(blA, blALen, "gl_one") &&
                                  decl_is(t, tl, "gl_one");
                        pairDepth = depth;
                        /* Only the stage that supplied the image may flip it  */
                        /* to additive: a glow stage further down (or further  */
                        /* up) describes a different layer.                    */
                        if (capDepth >= 0 && depth == capDepth && pairAdd) add = 1;
                        if (stageIdx >= 0 && pairAdd) lstages[stageIdx].additive = 1;
                        continue;
                    }
                    if (decl_is(t, tl, "blendfunc")) {
                        blendWant = 1;
                        continue;
                    }
                    /* v38.128: the definition says it is a sky. */
                    if (depth == 1 && decl_is(t, tl, "surfaceparm")) {
                        const char *sp; int spl;
                        p = decl_token(p, end, &sp, &spl);
                        if (decl_is(sp, spl, "sky")) lsky = 1;
                        continue;
                    }
                    /* v38.128: skyparms <farbox> <cloudheight> <nearbox>.
                     * `-` in the middle is "no cloud layer"; q3dm1's own
                     * sky.shader asks for 384. The far box (`env/space1/
                     * space1` in the maps that use one) is recorded as a FLAG,
                     * not a name: id draws it as six separate images, this
                     * renderer builds only the cloud layer, and a map that asks
                     * for one should say so in the log rather than quietly come
                     * out with half its sky (see q3sky.h). */
                    if (depth == 1 && decl_is(t, tl, "skyparms")) {
                        const char *f1, *f2, *f3; int l1, l2, l3;
                        p = decl_token(p, end, &f1, &l1);
                        p = decl_token(p, end, &f2, &l2);
                        p = decl_token(p, end, &f3, &l3);
                        if (!(l1 == 1 && f1[0] == '-')) lfarbox = 1;
                        if (!(l2 == 1 && f2[0] == '-')) lcloud = decl_atoi(f2, l2);
                        continue;
                    }
                    /* v38.128: a stage's own texcoordinate animation. id
                     * applies these IN FILE ORDER, in place (ioquake3's
                     * RB_CalcScrollTexCoords / RB_CalcScaleTexCoords), which
                     * is why the order is preserved rather than folded into
                     * one scale/offset pair. Types this renderer does not
                     * implement (turb/stretch/rotate) are skipped, not
                     * mis-recorded. */
                    if (depth == 2 && stageIdx >= 0 && decl_is(t, tl, "tcmod")) {
                        const char *ty, *va; int tyl, val;
                        float a = 0.0f, b = 0.0f;
                        int isScroll, okA, okB;
                        p = decl_token(p, end, &ty, &tyl);
                        isScroll = decl_is(ty, tyl, "scroll");
                        if (!isScroll && !decl_is(ty, tyl, "scale")) continue;
                        p = decl_token(p, end, &va, &val);
                        okA = decl_float(va, val, &a);
                        p = decl_token(p, end, &va, &val);
                        okB = decl_float(va, val, &b);
                        if (okA && okB &&
                            lstages[stageIdx].numTcMods < Q3BSP_MAX_TCMODS) {
                            q3bsp_tcmod_t *tm = &lstages[stageIdx].tcmod[
                                lstages[stageIdx].numTcMods++];
                            tm->type = isScroll ? Q3BSP_TCMOD_SCROLL
                                                : Q3BSP_TCMOD_SCALE;
                            tm->a = a;
                            tm->b = b;
                        }
                        continue;
                    }
                    if (depth == 2 && stageIdx >= 0 &&
                        decl_is(t, tl, "depthwrite")) {
                        lstages[stageIdx].depthWrite = 1;
                        continue;
                    }
                    if (decl_is(t, tl, "cull")) {
                        const char *c; int cl;
                        p = decl_token(p, end, &c, &cl);
                        if (decl_is(c, cl, "none") || decl_is(c, cl, "disable"))
                            culloff = 1;
                        continue;
                    }
                    /* EVERY `map` line is consumed, whether or not the
                     * definition already has an image: the operand is what a
                     * stage draws, and leaving it in the token stream both
                     * loses the stage's own image (v38.128's cloud layer) and
                     * lets a filename be mistaken for a keyword. */
                    if (decl_is(t, tl, "map") || decl_is(t, tl, "clampmap") ||
                        decl_is(t, tl, "animmap")) {
                        const char *im; int iml;
                        p = decl_token(p, end, &im, &iml);
                        if (decl_is(t, tl, "animmap")) {
                            /* animMap <fps> <frame0> <frame1> … — the first   */
                            /* FRAME is the operand after the rate.           */
                            p = decl_token(p, end, &im, &iml);
                        }
                        /* $lightmap and friends are engine-provided, not    */
                        /* files: keep walking to the stage that names one.  */
                        if (iml > 0 && im[0] != '$') {
                            /* The stage's own first image (v38.128). A
                             * definition binds one name to one image here,
                             * because that is what the level's slots need;
                             * the stages are kept beside it because a SKY
                             * draws its second stage as its own layer (id's
                             * clouds), which is a different question from
                             * "which image is this shader". */
                            if (stageIdx >= 0 && !lstages[stageIdx].image) {
                                lstages[stageIdx].image = im;
                                lstages[stageIdx].imageLen = iml;
                            }
                            if (captured)
                                continue;       /* stage image: done      */
                            if (num >= cap) {
                                int nc = cap * 2;
                                q3bsp_shader_decl_t *nd =
                                    (q3bsp_shader_decl_t *)kmalloc(
                                        (uint32_t)(nc * sizeof(*nd)));
                                if (!nd) { rc = -5; break; }
                                for (int c = 0; c < num; c++) nd[c] = decls[c];
                                kfree(decls);
                                decls = nd;
                                cap = nc;
                            }
                            curDecl = num;
                            decls[num].name = nameTok;
                            decls[num].nameLen = nameLen;
                            decls[num].image = im;
                            decls[num].imageLen = iml;
                            decls[num].additive = 0;
                            decls[num].cullNone = 0;
                            decls[num].sky = 0;
                            decls[num].skyFarBox = 0;
                            decls[num].cloudHeight = 0;
                            decls[num].numStages = 0;
                            memset(decls[num].stages, 0, sizeof(decls[num].stages));
                            num++;
                            captured = 1;
                            /* Keep watching the rest of this stage for the  */
                            /* blendFunc that describes how the image is     */
                            /* drawn — and honour one written BEFORE it.     */
                            capDepth = depth;
                            if (pairDepth == depth) add = pairAdd;
                        }
                    }
                }
                /* The flags are only known once the stage has closed. */
                if (curDecl >= 0) {
                    int k;
                    decls[curDecl].additive = add;
                    decls[curDecl].cullNone = culloff;
                    decls[curDecl].sky = lsky;
                    decls[curDecl].skyFarBox = lfarbox;
                    decls[curDecl].cloudHeight = lcloud;
                    decls[curDecl].numStages = lnstages;
                    for (k = 0; k < lnstages; k++)
                        decls[curDecl].stages[k] = lstages[k];
                }
            }
        }
    }

    if (rc != 0) {
        for (int b = 0; b < numBufs; b++) q3bsp_free_file(buffers[b]);
        kfree(buffers);
        if (decls) kfree(decls);
        return rc;
    }
    if (num == 0) {
        for (int b = 0; b < numBufs; b++) q3bsp_free_file(buffers[b]);
        kfree(buffers);
        kfree(decls);
        Com_Printf("[Q3BSP] shader scripts: %d file(s) listed, no usable definition\n",
                   numFiles);
        return 0;                           /* nothing usable in any file */
    }
    Com_Printf("[Q3BSP] shader scripts: %d file(s) listed, %d definition(s)\n",
               numFiles, num);
    out->decls = decls;
    out->num = num;
    out->buffers = buffers;
    out->numBuffers = numBufs;
    return 0;
}

void q3bsp_free_decls(q3bsp_shader_decls_t *d) {
    int i;
    if (!d) return;
    for (i = 0; i < d->numBuffers; i++)
        if (d->buffers && d->buffers[i]) q3bsp_free_file(d->buffers[i]);
    if (d->buffers) kfree(d->buffers);
    if (d->decls) kfree(d->decls);
    d->decls = NULL; d->num = 0;
    d->buffers = NULL; d->numBuffers = 0;
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
            c[i][j].lm[0] = v->lightmap[0];
            c[i][j].lm[1] = v->lightmap[1];
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
 *
 * The same argument carries the lightmap coordinate (v38.114): q3map writes a
 * patch's lightmap st from its lightmap projection, which is linear in position
 * for the very same reason, so the identical weights reproduce it exactly. A
 * curved patch's interior really is lit differently from its rim, and this is
 * what lets that gradient reach the screen instead of one flat quad colour.
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
    out->lm[0] = out->lm[1] = 0.0f;
    out->light[0] = out->light[1] = out->light[2] = 0;
    for (k = 0; k < 3; k++) out->normal[k] = 0.0f;
    for (j = 0; j < 3; j++) {
        for (i = 0; i < 3; i++) {
            float w = bu[i] * bv[j];
            for (k = 0; k < 3; k++) {
                out->xyz[k] += w * c[i][j].xyz[k];
                out->normal[k] += w * c[i][j].normal[k];
            }
            out->st[0] += w * c[i][j].st[0];
            out->st[1] += w * c[i][j].st[1];
            out->lm[0] += w * c[i][j].lm[0];
            out->lm[1] += w * c[i][j].lm[1];
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

/*
=================
bsp_lightmap_shift

id's R_ColorShiftLightingBytes (tr_map.c), applied to a whole page at load: shift
left, and when any channel overflows, scale the texel by 255/max instead of
clipping each channel — a saturated torch then stays orange rather than turning
white. Done once here rather than per sample, which is also what id does (it
shifts at texture upload and lets the GPU filter the result).
=================
*/
static void bsp_lightmap_shift(unsigned char *p, int count, int shift) {
    int i;

    if (!p || shift <= 0) return;
    for (i = 0; i + 2 < count; i += 3) {
        int r = p[i] << shift, g = p[i + 1] << shift, b = p[i + 2] << shift;

        if ((r | g | b) > 255) {
            int mx = r > g ? r : g;
            mx = mx > b ? mx : b;
            if (mx > 0) {
                r = r * 255 / mx;
                g = g * 255 / mx;
                b = b * 255 / mx;
            }
        }
        p[i] = (unsigned char)r;
        p[i + 1] = (unsigned char)g;
        p[i + 2] = (unsigned char)b;
    }
}

/* floor() without libm: the kernel links no floating-point math library, and
 * the only caller feeds it a range small enough that a cast plus a sign check
 * is exact. */
static int bsp_floor_i(float x) {
    int i = (int)x;
    return (x < (float)i) ? i - 1 : i;
}

/*
=================
bsp_lightmap_sample

One bilinear sample of a 128x128 RGB page at a page-space coordinate, written
out as 0..255 per channel.

The coordinate comes out of a lump as four bytes of float, so it is clamped
before any arithmetic: a NaN or a 1e9 would otherwise index the page. Clamping
(rather than wrapping) is also what id's GL_CLAMP sampling does, so a vertex
sitting exactly on a page edge fades to the edge texel instead of to the far
side of the page. Texel i is centred at (i + 0.5) / 128, hence the -0.5.
=================
*/
static void bsp_lightmap_sample(const unsigned char *page, const float uv[2],
                                unsigned char out[3]) {
    float fu, fv, tx, ty;
    int x0, y0, x1, y1, ch;

    fu = uv[0];
    fv = uv[1];
    if (!(fu > 0.0f)) fu = 0.0f; else if (fu > 1.0f) fu = 1.0f;
    if (!(fv > 0.0f)) fv = 0.0f; else if (fv > 1.0f) fv = 1.0f;
    fu = fu * (float)Q3BSP_LIGHTMAP_DIM - 0.5f;
    fv = fv * (float)Q3BSP_LIGHTMAP_DIM - 0.5f;
    x0 = bsp_floor_i(fu);
    y0 = bsp_floor_i(fv);
    tx = fu - (float)x0;
    ty = fv - (float)y0;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x0 > Q3BSP_LIGHTMAP_DIM - 1) x0 = Q3BSP_LIGHTMAP_DIM - 1;
    if (y0 > Q3BSP_LIGHTMAP_DIM - 1) y0 = Q3BSP_LIGHTMAP_DIM - 1;
    x1 = (x0 < Q3BSP_LIGHTMAP_DIM - 1) ? x0 + 1 : x0;
    y1 = (y0 < Q3BSP_LIGHTMAP_DIM - 1) ? y0 + 1 : y0;

    for (ch = 0; ch < 3; ch++) {
        const unsigned char *p = page + ch;
        float a = (float)p[(y0 * Q3BSP_LIGHTMAP_DIM + x0) * 3];
        float b = (float)p[(y0 * Q3BSP_LIGHTMAP_DIM + x1) * 3];
        float c = (float)p[(y1 * Q3BSP_LIGHTMAP_DIM + x0) * 3];
        float d = (float)p[(y1 * Q3BSP_LIGHTMAP_DIM + x1) * 3];
        float lo = a + (b - a) * tx;
        float hi = c + (d - c) * tx;
        float v  = lo + (hi - lo) * ty;

        if (v < 0.0f) v = 0.0f;
        if (v > 255.0f) v = 255.0f;
        out[ch] = (unsigned char)(v + 0.5f);
    }
}

/* The face's derived fields — centre, plane normal, radius, baked light, and
 * the light its vertices end up carrying — all come from its vertices, so
 * planar and tessellated faces share one path. */
static void bsp_face_finish(q3bsp_mesh_t *m, q3bsp_face_t *f) {
    int j;
    const float *a = m->verts[f->firstVert].xyz;
    const float *b = m->verts[f->firstVert + 1].xyz;
    const float *c = m->verts[f->firstVert + 2].xyz;
    float u[3], w[3];

    f->center[0] = f->center[1] = f->center[2] = 0.0f;
    /* v38.117: the face's FRONT normal is the mean of the file's own vertex
     * normals (q3map writes them pointing at the visible side; on a curved
     * patch the Bezier combination already produced per-vertex normals). This
     * is what backface culling reads — id's R_CullDotTri uses exactly these
     * — and unlike the winding normal it is right on EVERY planar face.
     * `normal` (below) keeps its winding-derived meaning for the baked sun. */
    f->frontNormal[0] = f->frontNormal[1] = f->frontNormal[2] = 0.0f;
    for (j = 0; j < f->numVerts; j++) {
        const q3bsp_vert_t *p = &m->verts[f->firstVert + j];
        f->center[0] += p->xyz[0];
        f->center[1] += p->xyz[1];
        f->center[2] += p->xyz[2];
        f->frontNormal[0] += p->normal[0];
        f->frontNormal[1] += p->normal[1];
        f->frontNormal[2] += p->normal[2];
    }
    f->center[0] /= (float)f->numVerts;
    f->center[1] /= (float)f->numVerts;
    f->center[2] /= (float)f->numVerts;
    {
        /* v38.132: measure whether that mean is MEANINGFUL before letting
         * anything reject a face with it.
         *
         * Two ways it fails, and both are real on retail q3dm1:
         *
         *  (a) the file's per-vertex normals disagree — dot < 0 for some pair.
         *      That is a brush-derived face whose normals run along the brush,
         *      not at the visible side, so their mean points at neither. This
         *      is the case the renderer's own comment recorded: the whole view
         *      went black at the park pose, 1927 of 1930 PVS-visible faces
         *      rejected, 3 drawn.
         *
         *  (b) they cancel — the pre-normalize magnitude is ~0, and
         *      bsp_normalize then hands back a fixed (0,0,1), a confident
         *      answer to a question nobody could answer.
         *
         * A gentle tessellated curve legitimately has normals that differ by
         * tens of degrees; dot stays well above 0 there, so real curvature is
         * NOT mistaken for (a). Only genuinely opposed normals trip it.
         *
         * Measured once, at load, from bytes already in the file. */
        const q3bsp_vert_t *p0 = &m->verts[f->firstVert];
        float minpair = 1.0f;
        float mag2;
        for (j = 0; j < f->numVerts; j++) {
            const q3bsp_vert_t *pj = &m->verts[f->firstVert + j];
            float d = p0->normal[0] * pj->normal[0] +
                      p0->normal[1] * pj->normal[1] +
                      p0->normal[2] * pj->normal[2];
            if (d < minpair) minpair = d;
        }
        mag2 = f->frontNormal[0] * f->frontNormal[0] +
               f->frontNormal[1] * f->frontNormal[1] +
               f->frontNormal[2] * f->frontNormal[2];
        /* numVerts < 2 cannot disagree with itself; one vertex is taken at its
         * word (that is the tessellator's degenerate case, not a brush). */
        f->cullTrusted = (f->numVerts < 2 || minpair > 0.0f) && mag2 > 1e-6f;
    }
    bsp_normalize(f->frontNormal);

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

    /* v38.114: the light this face's vertices carry. A face whose surface names
     * a page is sampled out of it — once, here, because the draw path is
     * per-frame and this is per-map. Every other face leaves `light` zeroed and
     * keeps `f->light` (the sun), which is what the renderer tests on
     * lightmapNum.
     *
     * The mean over the lit faces is kept only so the log can say how bright
     * the level came out: on a retail map the lightmaps are the difference
     * between 9 pages of data and a camera looking at a cave. */
    if (m->lightmaps && f->lightmapNum >= 0 && f->lightmapNum < m->numLightmaps) {
        const unsigned char *page = m->lightmaps +
            (int)f->lightmapNum * (Q3BSP_LIGHTMAP_DIM * Q3BSP_LIGHTMAP_DIM * 3);
        int sum[3];

        sum[0] = sum[1] = sum[2] = 0;
        for (j = 0; j < f->numVerts; j++) {
            q3bsp_vert_t *v = &m->verts[f->firstVert + j];
            bsp_lightmap_sample(page, v->lm, v->light);
            sum[0] += v->light[0];
            sum[1] += v->light[1];
            sum[2] += v->light[2];
        }
        m->litMean[0] += sum[0];
        m->litMean[1] += sum[1];
        m->litMean[2] += sum[2];
        m->litVerts += f->numVerts;
        m->facesLit++;
    } else {
        f->lightmapNum = -1;
        m->facesUnlit++;
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

    /* Filling pass only: which faces each surface (a lump index) produced.
     * Faces are appended surface by surface, so a surface's faces are always
     * CONTIGUOUS — which is what lets a leaf name a range instead of a list.
     * NULL in the counting pass. */
    int  *surfFirst;
    int  *surfCount;

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
    f->lightmapNum = s->lightmapNum;
    for (j = 0; j < s->numVerts; j++) {
        const drawVert_t *v = &b->dv[s->firstVert + j];
        q3bsp_vert_t *dst = &b->m->verts[*vertIdx + j];
        dst->xyz[0] = v->xyz[0];
        dst->xyz[1] = v->xyz[1];
        dst->xyz[2] = v->xyz[2];
        dst->st[0] = v->st[0];
        dst->st[1] = v->st[1];
        dst->lm[0] = v->lightmap[0];
        dst->lm[1] = v->lightmap[1];
        /* v38.117: the file's own vertex normal — the authoritative front side
         * (id culls planar faces with it in R_CullDotTri). */
        dst->normal[0] = v->normal[0];
        dst->normal[1] = v->normal[1];
        dst->normal[2] = v->normal[2];
        dst->light[0] = dst->light[1] = dst->light[2] = 0;
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
                    f->lightmapNum = s->lightmapNum;
                    /* v38.116: wound A B C D. The old A D C B reproduced id's
                     * COLLISION facet (cm_patch.c builds cross(C-A,B-A) from
                     * (i,j),(i+1,j),(i+1,j+1)) and that order still stands in
                     * the collision world — but the RENDER side now culls
                     * backfaces with id's front-face convention (GL_CW,
                     * q3world_render.c), and measured on the fixture map the
                     * tessellated quads came out opposite to the planar faces'
                     * screen winding: with A D C B the curve vanished under the
                     * cull while every wall stayed, and with A B C D both draw.
                     * Patches are lit from lightmaps (v38.114), not from the
                     * winding normal, so only the fixture's unlit-curve flat
                     * shade changes with this. */
                    b->m->verts[*vertIdx + 0] = a;
                    b->m->verts[*vertIdx + 1] = vb;
                    b->m->verts[*vertIdx + 2] = vc;
                    b->m->verts[*vertIdx + 3] = vd;
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
        int pw, ph, firstFace = faceIdx;

        if (b->m->numShaders > 0 && (s->shaderNum < 0 || s->shaderNum >= b->m->numShaders))
            b->shaderOverflow++;

        if (s->surfaceType == MST_PATCH) {
            int bi, bj, quads = 0, pverts = 0;

            b->patchSurfaces++;
            if (!bsp_patch_valid(s, b->numDv, &pw, &ph)) {
                b->patchSkipped++;
                goto next_surface;
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
            goto next_surface;
        }

        if (s->surfaceType != MST_PLANAR || s->numVerts < 3 ||
            s->firstVert < 0 || s->firstVert + s->numVerts > b->numDv) {
            b->skipped++;
            goto next_surface;
        }
        if (b->verts + s->numVerts > Q3BSP_MAX_VERTS) {
            b->truncated = 1;
            break;
        }
        if (emit) bsp_planar_emit(b, s, &faceIdx, &vertIdx);
        b->planarFaces++;
        b->faces++;
        b->verts += s->numVerts;

next_surface:               /* v38.112: record this surface's face range */
        if (b->surfFirst) {
            b->surfFirst[i] = firstFace;
            b->surfCount[i] = faceIdx - firstFace;
        }
    }
}

/* ----------------------------------------------------------------------- */
/* The map's own tree, its leaves and its PVS (v38.112).                   */
/*                                                                        */
/* Everything here is the file's data, read the way id's collision loader  */
/* (cm_load.c) reads it — the same lumps, the same row-major visibility    */
/* matrix, the same front-of-plane rule as CM_PointLeafnum_r. Why a second */
/* copy at all: q3bsp.c must be able to answer "which of MY faces can a    */
/* camera at p see" without assuming which map the collision module is     */
/* holding right now, and the face list is this module's own (a surface    */
/* became one face, or — for a patch — many).                              */
/*                                                                        */
/* The one thing worth trusting over cleverness here: a WRONG tree culls   */
/* geometry that is really there, which is far worse than not culling. So  */
/* every parse step is validated against the lump's own lengths and the    */
/* result is cross-checked against id's CM_PointLeafnum (which has been    */
/* tracing this very map since v38.107) before it is allowed to cull       */
/* anything. A disagreement turns culling off and says so in the log.      */
/* ----------------------------------------------------------------------- */

static void bsp_vis_reset(q3bsp_vis_t *v) {
    if (!v) return;
    v->planes = NULL; v->nodes = NULL;
    v->leafCluster = NULL; v->leafFirstFace = NULL; v->leafNumFaces = NULL;
    v->leafFaces = NULL;
    v->vis = NULL; v->bitOfs = NULL;
    v->numPlanes = v->numNodes = v->numLeafs = v->numLeafFaces = 0;
    v->visLen = 0; v->numClusters = 0;
    v->ready = 0; v->hasVis = 0;
}

static void bsp_vis_free(q3bsp_vis_t *v) {
    if (!v) return;
    if (v->planes) kfree(v->planes);
    if (v->nodes) kfree(v->nodes);
    if (v->leafCluster) kfree(v->leafCluster);
    if (v->leafFirstFace) kfree(v->leafFirstFace);
    if (v->leafNumFaces) kfree(v->leafNumFaces);
    if (v->leafFaces) kfree(v->leafFaces);
    if (v->vis) kfree(v->vis);
    if (v->bitOfs) kfree(v->bitOfs);
    bsp_vis_reset(v);
}

/* Copy the tree, the leaves' surface lists and the visibility matrix out of
 * the file. Best effort by design: any failure leaves vis.ready == 0 and the
 * renderer draws everything, which is exactly what it did before v38.112. */
static void bsp_load_vis(q3bsp_mesh_t *m, const unsigned char *buf,
                         const dheader_t *hdr, const int *surfFirst,
                         const int *surfCount) {
    q3bsp_vis_t *v = &m->vis;
    const dnode_t *dn = (const dnode_t *)(buf + hdr->lumps[LUMP_NODES].fileofs);
    const dleaf_t *dl = (const dleaf_t *)(buf + hdr->lumps[LUMP_LEAFS].fileofs);
    const dplane_t *dp = (const dplane_t *)(buf + hdr->lumps[LUMP_PLANES].fileofs);
    const int *ls = (const int *)(buf + hdr->lumps[LUMP_LEAFSURFACES].fileofs);
    int numLs = hdr->lumps[LUMP_LEAFSURFACES].filelen / (int)sizeof(int);
    int i, j, total = 0, at = 0;

    bsp_vis_reset(v);

    v->numPlanes = hdr->lumps[LUMP_PLANES].filelen / (int)sizeof(dplane_t);
    v->numNodes  = hdr->lumps[LUMP_NODES].filelen / (int)sizeof(dnode_t);
    v->numLeafs  = hdr->lumps[LUMP_LEAFS].filelen / (int)sizeof(dleaf_t);
    if (v->numPlanes <= 0 || v->numNodes <= 0 || v->numLeafs <= 0) return;
    if (v->numPlanes > Q3BSP_MAX_PLANES || v->numNodes > Q3BSP_MAX_NODES ||
        v->numLeafs > Q3BSP_MAX_LEAFS) {
        Com_Printf("[Q3BSP] vis: tree too large (planes=%d nodes=%d leafs=%d) "
                   "— culling off\n", v->numPlanes, v->numNodes, v->numLeafs);
        bsp_vis_reset(v);
        return;
    }

    v->planes = (float (*)[4])kmalloc((uint32_t)(v->numPlanes * (int)sizeof(float[4])));
    v->nodes  = (q3bsp_node_t *)kmalloc((uint32_t)(v->numNodes * (int)sizeof(q3bsp_node_t)));
    v->leafCluster = (int *)kmalloc((uint32_t)(v->numLeafs * (int)sizeof(int)));
    v->leafFirstFace = (int *)kmalloc((uint32_t)(v->numLeafs * (int)sizeof(int)));
    v->leafNumFaces  = (int *)kmalloc((uint32_t)(v->numLeafs * (int)sizeof(int)));
    if (!v->planes || !v->nodes || !v->leafCluster ||
        !v->leafFirstFace || !v->leafNumFaces) {
        Com_Printf("[Q3BSP] vis: out of memory — culling off\n");
        bsp_vis_free(v);
        return;
    }

    for (i = 0; i < v->numPlanes; i++) {
        v->planes[i][0] = dp[i].normal[0];
        v->planes[i][1] = dp[i].normal[1];
        v->planes[i][2] = dp[i].normal[2];
        v->planes[i][3] = dp[i].dist;
    }
    for (i = 0; i < v->numNodes; i++) {
        if (dn[i].planeNum < 0 || dn[i].planeNum >= v->numPlanes) {
            Com_Printf("[Q3BSP] vis: node %d has a bad plane (%d) — culling off\n",
                       i, dn[i].planeNum);
            bsp_vis_free(v);
            return;
        }
        v->nodes[i].planeNum = dn[i].planeNum;
        v->nodes[i].children[0] = dn[i].children[0];
        v->nodes[i].children[1] = dn[i].children[1];
    }

    /* First: how many of OUR faces each leaf owns. A leaf names surfaces; a
     * surface owns a contiguous run of faces (patches expand to many). Two
     * facts about the file shape the sanity check here: the list is indexed
     * by RAW surface number (id's cm.surfaces keeps NULLs for non-patches
     * but never renumbers), and a surface appears once per leaf it touches —
     * q3dm1's 113 patch surfaces are listed ~150 times each, so the summed
     * face count is NOT a bound (31828 for a 6780-face mesh is legitimate).
     * id's own renderer dedups with a marked array; our bound is DISTINCT
     * surfaces, which can never exceed the file's surface count. */
    for (i = 0; i < v->numLeafs; i++) {
        int fs = dl[i].firstLeafSurface, nl = dl[i].numLeafSurfaces;
        v->leafCluster[i] = dl[i].cluster;
        v->leafFirstFace[i] = 0;
        v->leafNumFaces[i] = 0;
        if (fs < 0 || nl < 0 || fs > numLs || nl > numLs - fs) continue;
        for (j = 0; j < nl; j++) {
            int si = ls[fs + j];
            if (si < 0 || si >= m->fileSurfaces) continue;
            total += surfCount[si];
        }
    }
    if (total > 0) {
        unsigned char *seen = (unsigned char *)kmalloc(
            (uint32_t)m->fileSurfaces);
        int distinct = 0;
        if (!seen) {
            Com_Printf("[Q3BSP] vis: out of memory checking the leaf "
                       "surface lists — culling off\n");
            bsp_vis_free(v);
            return;
        }
        memset(seen, 0, (size_t)m->fileSurfaces);
        for (i = 0; i < v->numLeafs && distinct <= m->fileSurfaces; i++) {
            int fs = dl[i].firstLeafSurface, nl = dl[i].numLeafSurfaces;
            if (fs < 0 || nl < 0 || fs > numLs || nl > numLs - fs) continue;
            for (j = 0; j < nl; j++) {
                int si = ls[fs + j];
                if (si < 0 || si >= m->fileSurfaces) continue;
                if (!seen[si]) {
                    seen[si] = 1;
                    distinct++;
                }
            }
        }
        kfree(seen);
        if (distinct > m->fileSurfaces) {   /* impossible without a bad index */
            Com_Printf("[Q3BSP] vis: leaf surface lists name %d surfaces for "
                       "a %d-surface file — culling off\n",
                       distinct, m->fileSurfaces);
            bsp_vis_free(v);
            return;
        }
        v->leafFaces = (int *)kmalloc((uint32_t)(total * (int)sizeof(int)));
        if (!v->leafFaces) {
            Com_Printf("[Q3BSP] vis: out of memory for the leaf face lists — "
                       "culling off\n");
            bsp_vis_free(v);
            return;
        }
        for (i = 0; i < v->numLeafs; i++) {
            int fs = dl[i].firstLeafSurface, nl = dl[i].numLeafSurfaces;
            if (fs < 0 || nl < 0 || fs > numLs || nl > numLs - fs) continue;
            v->leafFirstFace[i] = at;
            for (j = 0; j < nl; j++) {
                int si = ls[fs + j], k;
                if (si < 0 || si >= m->fileSurfaces) continue;
                for (k = 0; k < surfCount[si]; k++)
                    v->leafFaces[at++] = surfFirst[si] + k;
            }
            v->leafNumFaces[i] = at - v->leafFirstFace[i];
        }
        v->numLeafFaces = at;
    }

    v->ready = 1;

    /* --- the visibility matrix ------------------------------------------- */
    /* On disk, exactly as cm_load.c reads it: two ints (cluster count and the
     * row stride in bytes) followed by the rows, one bit per cluster. id's
     * collision module turns this into CM_ClusterPVS(); this is the same
     * matrix, kept here because the renderer needs it per frame. */
    {
        const int vl = LUMP_VISIBILITY;
        int vlen = hdr->lumps[vl].filelen;
        const unsigned char *vb = buf + hdr->lumps[vl].fileofs;
        int ncl, cbytes;

        if (vlen >= 8) {
            memcpy(&ncl, vb, 4);
            memcpy(&cbytes, vb + 4, 4);
            /* The row stride is bit-packed: (clusters+7)/8 bytes, and q3map
             * pads at most a little. The upper bound keeps the stride * count
             * product inside 32 bits (this kernel has no 64-bit divide and
             * does not want an overflowed offset either). */
            if (ncl > 0 && ncl <= Q3BSP_MAX_CLUSTERS &&
                cbytes >= (ncl + 7) / 8 && cbytes <= 16384 &&
                ncl <= (vlen - 8) / cbytes) {
                v->vis = (unsigned char *)kmalloc((uint32_t)(vlen - 8));
                if (v->vis) {
                    memcpy(v->vis, vb + 8, (size_t)(vlen - 8));
                    v->visLen = vlen - 8;
                    v->numClusters = ncl;
                    v->bitOfs = (int *)kmalloc((uint32_t)(ncl * (int)sizeof(int)));
                    if (v->bitOfs) {
                        for (i = 0; i < ncl; i++) v->bitOfs[i] = i * cbytes;
                        v->hasVis = 1;
                    } else {
                        kfree(v->vis);
                        v->vis = NULL;
                        v->visLen = 0;
                    }
                }
            }
        }
    }

    Com_Printf("[Q3BSP] vis: planes=%d nodes=%d leafs=%d faces-in-leafs=%d "
               "clusters=%d%s\n", v->numPlanes, v->numNodes, v->numLeafs,
               v->numLeafFaces, v->numClusters,
               v->hasVis ? "" : " (no PVS lump: every face drawn)");
}

/* The camera's leaf, child for child as CM_PointLeafnum_r (cm_test.c) does it:
 * front of the plane is children[0], the leaf number is (-1 - child). */
int q3bsp_leaf_for_point(const q3bsp_mesh_t *m, const float p[3]) {
    const q3bsp_vis_t *v;
    int num = 0, guard;

    if (!m || !p) return -1;
    v = &m->vis;
    if (!v->ready || !v->nodes || !v->planes) return -1;

    for (guard = 0; num >= 0; guard++) {
        const q3bsp_node_t *n;
        const float *pl;
        float d;

        if (num >= v->numNodes || guard > v->numNodes + 1) return -1;
        n = &v->nodes[num];
        pl = v->planes[n->planeNum];
        d = pl[0] * p[0] + pl[1] * p[1] + pl[2] * p[2] - pl[3];
        num = (d < 0.0f) ? n->children[1] : n->children[0];
    }
    num = -1 - num;
    if (num < 0 || num >= v->numLeafs) return -1;
    return num;
}

/* Does our copy of the tree send the same points to the same leaves id's
 * collision module does? Sampled at real geometry (face centres) and the
 * world's centre; any disagreement is treated as "this tree is not to be
 * trusted" and culling stays off. */
static void bsp_vis_check_against_cm(q3bsp_mesh_t *m) {
    q3bsp_vis_t *v = &m->vis;
    int i, checked = 0, bad = 0, step;

    if (!v->ready || !m->numFaces) return;
    step = m->numFaces / 8;
    if (step < 1) step = 1;
    for (i = 0; i < m->numFaces; i += step) {
        int mine = q3bsp_leaf_for_point(m, m->faces[i].center);
        int theirs = CM_PointLeafnum(m->faces[i].center);
        checked++;
        if (mine != theirs) bad++;
    }
    if (bad) {
        Com_Printf("[Q3BSP] vis: our leaf walk disagrees with CM_PointLeafnum "
                   "at %d/%d sample points — culling off\n", bad, checked);
        v->ready = 0;
        v->hasVis = 0;
        return;
    }
    Com_Printf("[Q3BSP] vis: leaf walk agrees with CM_PointLeafnum at all %d "
               "sample point(s)\n", checked);
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

    /* --- lightmaps: the level's baked lighting (v38.114) -------------------
     *
     * The lump is a whole number of 128x128x3 pages. Anything left over (a
     * deliberately malformed file) is ignored rather than sampled, and pages
     * past Q3BSP_MAX_LIGHTMAPS, or a failed allocation, leave the map fully
     * sun-lit — with `lightmapDropped` set so the log can say which of the two
     * happened. */
    {
        const int ll = LUMP_LIGHTMAPS;
        const int pageBytes = Q3BSP_LIGHTMAP_DIM * Q3BSP_LIGHTMAP_DIM * 3;
        int pages = hdr->lumps[ll].filelen / pageBytes;

        if (pages > Q3BSP_MAX_LIGHTMAPS) {
            out->lightmapDropped = pages - Q3BSP_MAX_LIGHTMAPS;
            pages = Q3BSP_MAX_LIGHTMAPS;
        }
        if (pages > 0) {
            int n = pages * pageBytes;
            unsigned char *p = (unsigned char *)kmalloc((uint32_t)n);

            if (p) {
                int sum[3], k, i2;
                memcpy(p, buf + hdr->lumps[ll].fileofs, (size_t)n);
                out->lightmaps = p;
                out->numLightmaps = pages;
                out->lightmapDim = Q3BSP_LIGHTMAP_DIM;
                out->lightmapBytes = n;
                /* The raw mean is reported before the shift: it is the honest
                 * answer to "how dark is this map's lighting in the file?", and
                 * litMean below is the answer to "how bright did the map come
                 * out here?". */
                sum[0] = sum[1] = sum[2] = 0;
                for (i2 = 0; i2 < n; i2 += 3) {
                    sum[0] += p[i2];
                    sum[1] += p[i2 + 1];
                    sum[2] += p[i2 + 2];
                }
                for (k = 0; k < 3; k++) out->lightmapMean[k] = sum[k] / (n / 3);
                bsp_lightmap_shift(p, n, Q3BSP_LIGHTMAP_SHIFT);
                out->lightmapShift = Q3BSP_LIGHTMAP_SHIFT;
            } else {
                out->lightmapDropped = pages;
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

        /* v38.112: the filling pass also records which faces each SURFACE
         * produced, because the leaves in the file name surfaces and the
         * renderer culls faces. */
        b.surfFirst = (int *)kmalloc((uint32_t)(out->fileSurfaces * (int)sizeof(int)));
        b.surfCount = (int *)kmalloc((uint32_t)(out->fileSurfaces * (int)sizeof(int)));
        if (!b.surfFirst || !b.surfCount) {
            if (b.surfFirst) kfree(b.surfFirst);
            if (b.surfCount) kfree(b.surfCount);
            kfree(out->faces);
            kfree(out->verts);
            out->faces = NULL;
            out->verts = NULL;
            q3bsp_free_file(buf);
            return -5;
        }
        for (i = 0; i < out->fileSurfaces; i++) {
            b.surfFirst[i] = 0;
            b.surfCount[i] = 0;
        }

        /* v38.142: the size accumulators MUST restart for the emit pass.
         * They still hold the count pass totals here (b.verts = 71747 on
         * q3dm7), so without this the emit truncation checks fire against
         * stale totals and the pass silently stops mid-map (q3dm7 emitted
         * 11852 of 16840 faces, the rest uninitialized heap, first-frame
         * page fault in face_outside_frustum). faceCount/vertCount above
         * already saved the count pass sizes for the allocations. */
        b.faces = b.verts = 0;
        b.planarFaces = b.patchesDrawn = b.patchQuads = b.patchVerts = 0;
        b.truncated = 0;
        bsp_build(&b, 1);                    /* fill them */
        out->numFaces = faceCount;
        out->numVerts = vertCount;
        out->truncated |= b.truncated;       /* an emit truncation must show */

        bsp_load_vis(out, buf, hdr, b.surfFirst, b.surfCount);
        bsp_vis_check_against_cm(out);
        kfree(b.surfFirst);
        kfree(b.surfCount);
    }

    q3bsp_free_file(buf);
    out->valid = 1;
    return 0;
}

void q3bsp_free(q3bsp_mesh_t *m) {
    if (!m) return;
    if (m->faces) kfree(m->faces);
    if (m->verts) kfree(m->verts);
    if (m->lightmaps) kfree(m->lightmaps);
    m->faces = NULL;
    m->verts = NULL;
    m->lightmaps = NULL;
    m->numLightmaps = 0;
    bsp_vis_free(&m->vis);
    m->valid = 0;
}
