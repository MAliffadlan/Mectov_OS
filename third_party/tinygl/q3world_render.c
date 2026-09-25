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
 *  3. Missing lightmaps are replaced by a baked face normal against a fixed
 *     sun, applied as the vertex colour (TGL_FEATURE_LIT_TEXTURES is on, so the
 *     colour modulates the texture). That is what makes floor, walls and
 *     ceiling read as different surfaces in a screendump.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <TGL/gl.h>
#include "zbuffer.h"

#include "q3world_render.h"

extern void  write_serial_string(const char *s);
extern void *kmalloc(uint32_t size);
extern void  kfree(void *p);

/* Beyond this the face is dropped: the projection's far plane is 4096, and the
 * test arena is ~1200 units across, so nothing real is lost. */
#define Q3W_FAR        3500.0f
#define Q3W_MAX_TEX    Q3BSP_MAX_SHADERS
#define Q3W_PH_SIZE    64          /* placeholder / working texture side */

typedef struct {
    int  used;
    int  fromDisk;
    int  width, height;
    char path[Q3BSP_MAX_NAME + 8];
} q3w_slot_t;

static GLuint    tex_id[Q3W_MAX_TEX];
static q3w_slot_t tex_slot[Q3W_MAX_TEX];
static int       tex_shaders;      /* shaders the last load walked */
static int       tex_from_disk;
static int       tex_placeholders;
static int       tex_inited;

static int  v_faces, v_tris, v_culled;   /* last frame */

/* --- serial helpers (this TU has no formatted output of its own) -------- */
static void wr_int(int v) {
    static char buf[13];
    int n = 0;
    unsigned u;
    if (v < 0) { write_serial_string("-"); u = (unsigned)(-v); } else u = (unsigned)v;
    if (u == 0) { write_serial_string("0"); return; }
    while (u && n < 12) { buf[n++] = (char)('0' + (u % 10)); u /= 10; }
    for (int i = 0; i < n / 2; i++) {
        char t = buf[i];
        buf[i] = buf[n - 1 - i];
        buf[n - 1 - i] = t;
    }
    buf[n] = '\0';
    write_serial_string(buf);
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

/* "textures/x/floor" -> "textures/x/floor.tga"; a name that already carries an
 * extension is left alone (some maps ship shader names with one). */
static void tex_path_for(const char *shader, char *dst, int size) {
    int i = 0, dot = -1, slash = -1;
    for (i = 0; shader[i] && i < size - 5; i++) {
        dst[i] = shader[i];
        if (shader[i] == '.') dot = i;
        if (shader[i] == '/') slash = i;
    }
    if (dot > slash) { dst[i] = '\0'; return; }
    dst[i++] = '.';
    dst[i++] = 't';
    dst[i++] = 'g';
    dst[i++] = 'a';
    dst[i] = '\0';
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
        tex_slot[i].path[0] = '\0';
    }
    tex_shaders = tex_from_disk = tex_placeholders = 0;
    tex_inited = 0;
    v_faces = v_tris = v_culled = 0;
}

int q3w_load(const q3bsp_mesh_t *m) {
    unsigned char *ph = NULL;

    q3w_unload();
    if (!m || !m->valid || m->numShaders <= 0) return 0;

    tex_shaders = m->numShaders;
    write_serial_string("[Q3ARENA] textures: ");
    wr_int(tex_shaders);
    write_serial_string(" shader(s) named by the map\n");

    ph = (unsigned char *)kmalloc((uint32_t)(Q3W_PH_SIZE * Q3W_PH_SIZE * 3));
    if (!ph) return 0;

    for (int i = 0; i < tex_shaders && i < Q3W_MAX_TEX; i++) {
        unsigned char *raw = NULL;
        int len = 0, w = 0, h = 0;
        unsigned char *rgb = NULL;
        GLuint id;
        q3w_slot_t *slot = &tex_slot[i];

        tex_path_for(m->shaderNames[i], slot->path, (int)sizeof(slot->path));
        slot->used = 1;

        if (q3bsp_read_file(slot->path, &raw, &len) == 0 &&
            tga_decode(raw, len, &rgb, &w, &h) == 0) {
            id = upload_rgb(rgb, w, h);
            slot->fromDisk = 1;
            slot->width = w;
            slot->height = h;
            tex_from_disk++;
            write_serial_string("[Q3ARENA] tex ");
            wr_int(i);
            write_serial_string(" ");
            write_serial_string(slot->path);
            write_serial_string(" tga=");
            wr_int(w);
            write_serial_string("x");
            wr_int(h);
            write_serial_string(" bytes=");
            wr_int(len);
            write_serial_string(" gl=");
            wr_int((int)id);
            write_serial_string("\n");
            kfree(rgb);
        } else {
            placeholder_rgb(m->shaderNames[i], ph);
            id = upload_rgb(ph, Q3W_PH_SIZE, Q3W_PH_SIZE);
            slot->fromDisk = 0;
            slot->width = Q3W_PH_SIZE;
            slot->height = Q3W_PH_SIZE;
            tex_placeholders++;
            write_serial_string("[Q3ARENA] tex ");
            wr_int(i);
            write_serial_string(" ");
            write_serial_string(slot->path);
            write_serial_string(" missing=1 placeholder gl=");
            wr_int((int)id);
            write_serial_string("\n");
        }
        tex_id[i] = id;
        if (raw) q3bsp_free_file(raw);
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

/* The same classification the test performs on a screendump, applied to the
 * frame the renderer just produced. It exists because a screendump assertion
 * is at the mercy of the compositor: the window may be behind another window,
 * be moved by the WM, or simply not have been redrawn yet — this port's first
 * version of that test measured the wallpaper and reported a floor. Reading the
 * renderer's own buffer removes every one of those failure modes, and keeps the
 * screendump as evidence for a human rather than as the pass/fail gate. */
int q3w_histogram(const uint32_t *px, int pitch, int w, int h,
                  int *cyan, int *warm, int *stepgreen, int *violet,
                  int *bright, int *sky, int *distinct) {
    unsigned char seen[4096 / 8];
    int n = 0, distinct_count = 0;
    int c_cyan = 0, c_warm = 0, c_step = 0, c_violet = 0, c_bright = 0, c_sky = 0;

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
            if (g > r + 25 && b > r + 25) c_cyan++;
            else if (r > g + 15 && g > b + 10 && r > 60) c_warm++;
            else if (g > r + 12 && g > b + 40 && g > 80) c_step++;
            else if (b > r + 5 && b > g + 5 && r >= 30 && r < 80 &&
                     (r > g ? r - g : g - r) < 8) c_violet++;
            if (r + g + b > 600) c_bright++;
        }
    }

    if (cyan) *cyan = c_cyan;
    if (warm) *warm = c_warm;
    if (stepgreen) *stepgreen = c_step;
    if (violet) *violet = c_violet;
    if (bright) *bright = c_bright;
    if (sky) *sky = c_sky;
    if (distinct) *distinct = distinct_count;
    return n;
}

void q3w_frame_stats(int *facesDrawn, int *trisDrawn, int *facesCulled) {
    if (facesDrawn) *facesDrawn = v_faces;
    if (trisDrawn) *trisDrawn = v_tris;
    if (facesCulled) *facesCulled = v_culled;
}

static void emit_vert(const q3bsp_vert_t *v) {
    glTexCoord2f(v->st[0], v->st[1]);
    glVertex3f(v->xyz[0], v->xyz[1], v->xyz[2]);
}

void q3w_draw(const q3bsp_mesh_t *m, const float cam[3], const float fwd[3],
              const float right[3]) {
    int cur_shader = -1;
    int started = 0;

    v_faces = v_tris = v_culled = 0;
    if (!m || !m->valid || !m->numFaces || !tex_inited) return;

    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glShadeModel(GL_FLAT);

    for (int i = 0; i < m->numFaces; i++) {
        const q3bsp_face_t *f = &m->faces[i];
        float dx = f->center[0] - cam[0];
        float dy = f->center[1] - cam[1];
        float dz = f->center[2] - cam[2];
        float depth = dx * fwd[0] + dy * fwd[1] + dz * fwd[2];

        if (depth < -f->radius || depth > Q3W_FAR) {   /* behind, or far away */
            v_culled++;
            continue;
        }
        if (right) {
            /* Half the horizontal field of view, with the face's own extent as
             * slack: the side plane through the camera is fwd*sin + right*cos. */
            float side = dx * right[0] + dy * right[1] + dz * right[2];
            if (side < -f->radius - 0.75f * depth ||
                side >  f->radius + 0.75f * depth) {
                v_culled++;
                continue;
            }
        }

        if (f->shaderNum != cur_shader) {
            if (started) glEnd();
            glBindTexture(GL_TEXTURE_2D,
                          tex_id[f->shaderNum < Q3W_MAX_TEX ? f->shaderNum : 0]);
            glBegin(GL_TRIANGLES);
            started = 1;
            cur_shader = f->shaderNum;
        }
        glColor3f(f->light, f->light, f->light);
        for (int j = 1; j + 1 < f->numVerts; j++) {
            emit_vert(&m->verts[f->firstVert]);
            emit_vert(&m->verts[f->firstVert + j]);
            emit_vert(&m->verts[f->firstVert + j + 1]);
        }
        v_faces++;
        v_tris += f->numVerts - 2;
    }
    if (started) glEnd();
    glDisable(GL_TEXTURE_2D);
}
