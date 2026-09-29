/* q3viewmodel.c — id's own weapon view model (.md3), loaded and drawn (v38.124).
 *
 * See q3viewmodel.h for what this is and why it exists. The three decisions
 * worth stating here, because each is a place this could have lied:
 *
 *  1. The .md3 layout is read from BYTE OFFSETS, not by casting the file to a
 *     struct. id's own structs (third_party/q3a/code/qcommon/qfiles.h:113-209)
 *     document the layout this follows field for field, but that header pulls
 *     id's q_shared.h and is not on this unit's include path; more to the
 *     point, a compiler is free to pad a struct and the file is not. Every
 *     offset below is cited to the chunk it reads.
 *
 *  2. Every surface offset is relative to the START OF THAT SURFACE, which is
 *     how id's loader reads them ((byte *)surface + surface->ofsShaders) and
 *     what the demo data actually contains: for machinegun.md3 the surface at
 *     file offset 500 has shaders at +108 (right after its 108-byte header),
 *     triangles at +176, st at +2228, xyzNormals at +3732 and ofsEnd 5236 —
 *     every one of them exactly where the chunk sizes say they should be, and
 *     the sum lands on the file's own length. Reading them as absolute offsets
 *     parses frames as shader names, which is what the first attempt at this
 *     loader did.
 *
 *  3. The tag chain is VERIFIED, not assumed. The demo's data says the barrel
 *     sits at the body's tag_barrel (9.84, -0.31, 0.17) and that the barrel's
 *     own tag_flash is (15.72, 0.02, 0.00) beyond that, while the body's
 *     tag_flash claims the muzzle is at 25.56 — the two agree to 0.02 units.
 *     That agreement is checked at load time and reported; if a part ever
 *     disagreed, this drops the barrel and the flash rather than drawing them
 *     somewhere plausible-looking and wrong.
 *
 * Not here, deliberately: the hands. The retail first-person view shows them
 * because id draws the PLAYER's upper model (models/players/<model>/upper.md3,
 * 153 frames, skinned, with its own tag_weapon) as the view entity and hangs
 * the weapon off it. The demo's models/weapons2/machinegun/machinegun_hand.md3
 * looks like it would supply them but carries zero surfaces — 16 frames of
 * animation and three tags, no geometry — so there is nothing in this file to
 * draw and this release says so instead of inventing a hand.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <TGL/gl.h>

#include "q3viewmodel.h"
#include "q3world_render.h"    /* q3w_resolve_image/upload/release + q3bsp.h */

extern void  write_serial_string(const char *s);
extern void *kmalloc(uint32_t size);
extern void  kfree(void *p);

/* --- the file format (qfiles.h:113-209) ---------------------------------- */
#define VM_IDENT        0x33504449   /* MD3_IDENT: the bytes "IDP3" (qfiles.h:113) */
#define VM_VERSION      15           /* MD3_VERSION (qfiles.h:114) */
#define VM_XYZ_SCALE    (1.0f / 64.0f)  /* md3_XYZ_SCALE (qfiles.h:127) */
#define VM_MAX_SURF     4            /* MD3_MAX_SURFACES is 32 (qfiles.h:122); a weapon part here has 1 */
#define VM_MAX_VERTS    4096         /* MD3_MAX_VERTS per surface (qfiles.h:121) */
#define VM_MAX_TRIS     8192         /* MD3_MAX_TRIANGLES per surface (qfiles.h:120) */

/* md3Header_t: ident, version, name[64], then nine ints. */
#define H_NUMFRAMES     76           /* after flags(72) */
#define H_NUMTAGS       80
#define H_NUMSURFACES   84
#define H_OFSFRAMES     92
#define H_OFSTAGS       96
#define H_OFSSURFACES  100
#define H_SIZE         108

/* md3Surface_t: ident, name[64], then ten ints. */
#define S_NAME           4           /* 64 bytes */
#define S_NUMSHADERS    76
#define S_NUMVERTS      80
#define S_NUMTRIS       84
#define S_OFSTRIS       88
#define S_OFSSHADERS    92
#define S_OFSST         96
#define S_OFSXYZ       100
#define S_OFSEND       104
#define S_SIZE         108

#define T_TAG_SIZE     112           /* md3Tag_t: name[64] + origin(12) + axis(36) */
#define T_TAG_ORIGIN    64
#define F_BOUNDS_SIZE   24           /* md3Frame_t: bounds[2][3] then localOrigin, radius, name[16] */

/* --- placement ------------------------------------------------------------
 *
 * The view model is drawn in CAMERA space: +x right, +y up, -z forward, in Q3
 * units, with the model's own axes folded onto the camera's. id's models are
 * authored X forward, Y left, Z up, so the mapping is a fixed rotation —
 * X -> -Zc, Y -> -Xc, Z -> +Yc — and no scale factor appears anywhere: this
 * port's projection is glFrustum with tan(45deg) = 1 at 4:3 (q3cl_render.c,
 * Q3REF_FOV_DEG), which is id's own default of a 90 degree horizontal FOV, so
 * a model unit is a world unit is a screen unit exactly as in the retail game.
 *
 * The offsets are the retail framing: the grip a little right of centre and
 * below the eye, so the 25.6-unit barrel runs away toward the crosshair while
 * the receiver fills the corner. `muzzle` lands near (191, 145) on a 320x240
 * view and the grip near (252, 194) — the classic bottom-right diagonal.
 */
#define VM_GUN_RIGHT    7.5f
#define VM_GUN_UP      (-8.0f)
#define VM_GUN_FWD     13.0f

/* Recoil: id's view model kicks toward the eye and slightly up inside the
 * WEAPON_FIRING window, which is the only state that also draws the flash. */
#define VM_KICK_BACK    0.9f
#define VM_KICK_UP      0.35f
#define VM_KICK_FRAMES  5            /* one machinegun cycle at ~50 fps */

/* 1 Q3 unit spans ~12 pixels at the grip's 13-unit distance (0.5 * rw /
 * tan_x / 13 with rw=320, tan_x=1): the v38.123 walk bob was ~6 pixels, so its
 * amplitude is 0.5 unit here — the same motion, expressed in the units the
 * model actually lives in. */
#define VM_BOB_UNITS    (1.0f / 12.0f)

typedef struct {
    float *v;                     /* 5 floats per vertex: x, y, z, s, t */
    int   *idx;                   /* 3 ints per triangle, into this surface */
    int    nverts, ntris;
    unsigned int tex;             /* 0 = no image resolved: the surface is NOT drawn */
    int    texw, texh;
} vm_surface_t;

typedef struct {
    vm_surface_t surf[VM_MAX_SURF];
    int   nsurf;
    int   ntris;
    int   tex_ok;                 /* at least one surface has an image */
    float bounds[2][3];           /* frame 0 box, model space */
    int   have_bounds;
    float tag_barrel[3];          /* body: where the barrel attaches */
    float tag_flash[3];           /* body/barrel: the muzzle */
    int   have_barrel, have_flash;
} vm_part_t;

static vm_part_t vm_body, vm_barrel, vm_flash;
static int   vm_parts;            /* 0 = nothing loaded; 1..3 = usable */
static int   vm_surfaces, vm_tris;
static int   vm_texw = 0, vm_texh = 0;
static float vm_muzzle[3];        /* body space, from the verified tag chain */
static int   vm_chain_ok;

static int   vm_firing, vm_grounded, vm_bob;
static int   vm_kick;
static int   vm_box[4];           /* last drawn screen box, framebuffer pixels */
static int   vm_drawn;            /* triangles the last pass actually emitted */
static int   vm_flash_mode;       /* v38.125: last pass drew the flash additively */

/* --- tiny readers (no casting, see the header comment) -------------------- */
static int rd32(const unsigned char *p) {
    return (int)((unsigned)p[0] | ((unsigned)p[1] << 8) |
                 ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24));
}
static int rd16(const unsigned char *p) {
    return (int)(short)((unsigned)p[0] | ((unsigned)p[1] << 8));
}
static float rdf(const unsigned char *p) {
    union { unsigned u; float f; } b;
    b.u = (unsigned)rd32(p);
    return b.f;
}

static int vm_name_is(const unsigned char *p, const char *want) {
    int i = 0;
    while (want[i]) {
        if (p[i] != (unsigned char)want[i]) return 0;
        i++;
    }
    return p[i] == 0;
}

static int vm_int(char *dst, int v) {          /* same shape as q3world_render's */
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

static int vm_str(char *dst, const char *s) {
    int n = 0;
    while (s[n]) { dst[n] = s[n]; n++; }
    dst[n] = '\0';
    return n;
}

/* --- loading ------------------------------------------------------------- */

static void vm_free_part(vm_part_t *part) {
    for (int s = 0; s < VM_MAX_SURF; s++) {
        vm_surface_t *sf = &part->surf[s];
        if (sf->v) { kfree(sf->v); sf->v = NULL; }
        if (sf->idx) { kfree(sf->idx); sf->idx = NULL; }
        if (sf->tex) { GLuint id = sf->tex; glDeleteTextures(1, &id); sf->tex = 0; }
        sf->nverts = sf->ntris = 0;
    }
    memset(part, 0, sizeof(*part));
}

/* Parse one .md3 into `part`. Returns 0 on success, a negative code otherwise:
 * -1 file not on the volume, -2 not an MD3, -3 surface table refused (a count
 * or a size that cannot be this format), -4 no surface resolved an image, -5
 * out of memory. A part with no texture is refused on purpose: TinyGL keeps
 * the LAST bound texture when a draw does not bind one, so an unstaged image
 * would otherwise paint a wall texture onto the gun. */
static int vm_load_part(const char *qpath, vm_part_t *part) {
    unsigned char *raw = NULL;
    int len = 0, rc = 0, s, t, i;
    int nsurf, ntags, ofs_surf, ofs_tags, ofs_frames;
    int drew_ok = 0;

    vm_free_part(part);
    if (q3bsp_read_file(qpath, &raw, &len) != 0 || !raw) {
        if (raw) q3bsp_free_file(raw);
        return -1;
    }
    if (len < H_SIZE || rd32(raw) != VM_IDENT || rd32(raw + 4) != VM_VERSION) {
        q3bsp_free_file(raw);
        return -2;
    }

    nsurf      = rd32(raw + H_NUMSURFACES);
    ntags      = rd32(raw + H_NUMTAGS);
    ofs_surf   = rd32(raw + H_OFSSURFACES);
    ofs_tags   = rd32(raw + H_OFSTAGS);
    ofs_frames = rd32(raw + H_OFSFRAMES);

    if (nsurf <= 0 || nsurf > VM_MAX_SURF || ofs_surf <= 0 || ofs_surf >= len) {
        q3bsp_free_file(raw);
        return -3;
    }

    /* Frame 0's box: the model's own size, which is what places the screen box
     * the serial line reports (md3Frame_t, qfiles.h:128). */
    if (ofs_frames > 0 && ofs_frames + F_BOUNDS_SIZE <= len) {
        for (i = 0; i < 3; i++) {
            part->bounds[0][i] = rdf(raw + ofs_frames + i * 4);
            part->bounds[1][i] = rdf(raw + ofs_frames + 12 + i * 4);
        }
        part->have_bounds = 1;
    }

    /* Frame 0's tags (numFrames * numTags records follow each other; qfiles.h:135). */
    for (t = 0; t < ntags; t++) {
        const unsigned char *tg;
        if (ofs_tags <= 0 || ofs_tags + (t + 1) * T_TAG_SIZE > len) break;
        tg = raw + ofs_tags + t * T_TAG_SIZE;
        if (vm_name_is(tg, "tag_barrel")) {
            for (i = 0; i < 3; i++) part->tag_barrel[i] = rdf(tg + T_TAG_ORIGIN + i * 4);
            part->have_barrel = 1;
        } else if (vm_name_is(tg, "tag_flash")) {
            for (i = 0; i < 3; i++) part->tag_flash[i] = rdf(tg + T_TAG_ORIGIN + i * 4);
            part->have_flash = 1;
        }
    }

    {
        int off = ofs_surf;
        for (s = 0; s < nsurf; s++) {
            const unsigned char *sb;
            int nsh, nv, nt, o_tri, o_sh, o_st, o_xyz, o_end;
            char shname[128];
            vm_surface_t *sf;

            if (off + S_SIZE > len) { rc = -3; break; }
            sb = raw + off;
            nsh   = rd32(sb + S_NUMSHADERS);
            nv    = rd32(sb + S_NUMVERTS);
            nt    = rd32(sb + S_NUMTRIS);
            o_tri = rd32(sb + S_OFSTRIS);
            o_sh  = rd32(sb + S_OFSSHADERS);
            o_st  = rd32(sb + S_OFSST);
            o_xyz = rd32(sb + S_OFSXYZ);
            o_end = rd32(sb + S_OFSEND);

            if (nv <= 0 || nv > VM_MAX_VERTS || nt <= 0 || nt > VM_MAX_TRIS ||
                o_st <= 0 || o_xyz <= 0 || o_tri <= 0) {
                rc = -3;
                break;
            }

            sf = &part->surf[s];
            sf->v = (float *)kmalloc((uint32_t)nv * 5u * (uint32_t)sizeof(float));
            sf->idx = (int *)kmalloc((uint32_t)nt * 3u * (uint32_t)sizeof(int));
            if (!sf->v || !sf->idx) { rc = -5; break; }
            sf->nverts = nv;
            sf->ntris = nt;
            part->nsurf++;
            part->ntris += nt;

            for (i = 0; i < nv; i++) {
                /* md3St_t (qfiles.h:183): two floats, one per vertex. */
                sf->v[i * 5 + 3] = rdf(sb + o_st + i * 8);
                sf->v[i * 5 + 4] = rdf(sb + o_st + i * 8 + 4);
                /* md3XyzNormal_t (qfiles.h:188): three shorts and a packed
                 * normal, frame 0 first — the pieces here are single-frame. */
                sf->v[i * 5 + 0] = (float)rd16(sb + o_xyz + i * 8 + 0) * VM_XYZ_SCALE;
                sf->v[i * 5 + 1] = (float)rd16(sb + o_xyz + i * 8 + 2) * VM_XYZ_SCALE;
                sf->v[i * 5 + 2] = (float)rd16(sb + o_xyz + i * 8 + 4) * VM_XYZ_SCALE;
            }
            for (i = 0; i < nt * 3; i++) {
                int vi = rd32(sb + o_tri + i * 4);
                sf->idx[i] = (vi >= 0 && vi < nv) ? vi : 0;
            }

            /* The surface's image: id's exporter writes the literal file name
             * here ("models/weapons2/machinegun/machinegun.tga", and for the
             * flash ".../f_machinegun.TGA") even though the pak ships the
             * .jpg, so the resolver strips the extension it names and falls
             * back through id's own order. */
            shname[0] = '\0';
            if (nsh > 0 && o_sh > 0 && o_sh + 64 <= len - off) {
                int k = 0;
                for (i = 0; i < 64 && sb[o_sh + i]; i++) shname[k++] = (char)sb[o_sh + i];
                shname[k] = '\0';
            }
            if (shname[0]) {
                unsigned char *rgb = NULL;
                int tw = 0, th = 0;
                char resolved[128];
                resolved[0] = '\0';
                if (q3w_resolve_image(shname, resolved, (int)sizeof(resolved),
                                      &rgb, &tw, &th) == 0 && rgb) {
                    sf->tex = q3w_upload_image(rgb, tw, th);
                    sf->texw = (sf->tex ? tw : 0);
                    sf->texh = (sf->tex ? th : 0);
                    q3w_release_image(rgb);
                    if (sf->tex) drew_ok = 1;   /* the part is drawable */
                }
            }

            if (o_end <= 0 || off + o_end > len) break;
            off += o_end;              /* ofsEnd is surface-relative too */
        }
    }

    q3bsp_free_file(raw);

    if (rc < 0) { vm_free_part(part); return rc; }
    if (!drew_ok) { vm_free_part(part); return -4; }
    part->tex_ok = 1;
    return 0;
}

int q3vm_load(const char *base) {
    char p[128];
    int n = 0, i;
    int body_rc, barrel_rc = -1, flash_rc = -1;
    char line[320];

    q3vm_unload();
    if (!base || !base[0]) return 0;

    /* <base>.md3, then the two parts id chains to it by tag. */
    for (i = 0; base[i] && n < (int)sizeof(p) - 16; i++) p[n++] = base[i];
    {
        static const char *ext = ".md3";
        int k = 0;
        while (ext[k]) p[n++] = ext[k++];
    }
    p[n] = '\0';
    body_rc = vm_load_part(p, &vm_body);
    if (body_rc != 0) {
        vm_parts = 0;
        /* The SAME field shape as the success line below, all zeros, plus the
         * rc and the reason. That is not cosmetic: a suite decides between
         * "id's model is on screen" and "the documented fallback ran" from
         * this line, and the first version of it printed only `parts=0 rc=N`,
         * which matches no parser — so a volume with no .md3 (CI's fixture
         * volume) looked like a suite failure instead of the fallback it was.
         * barrel/flash are -1 here because neither part was ever attempted. */
        {
            int m = 0;
            m += vm_str(line + m, "[Q3VIEWMODEL] load base=");
            m += vm_str(line + m, base);
            m += vm_str(line + m, " parts=0 surf=0 tris=0 tex=0x0 "
                                  "muzzle=0,0,0 chain=0 barrel=-1 flash=-1 "
                                  "flash_tris=0 rc=");
            m += vm_int(line + m, body_rc);
            m += vm_str(line + m, " - no view model on the volume, the driver "
                                  "draws its procedural fallback\n");
            write_serial_string(line);
        }
        return 0;
    }
    vm_parts = 1;

    /* Barrel: <base>_barrel.md3. */
    {
        int m = 0;
        for (i = 0; base[i] && m < (int)sizeof(p) - 16; i++) p[m++] = base[i];
        {
            static const char *mid = "_barrel.md3";
            int k = 0;
            while (mid[k]) p[m++] = mid[k++];
        }
        p[m] = '\0';
        barrel_rc = vm_load_part(p, &vm_barrel);
    }
    if (barrel_rc == 0) vm_parts = 2;

    /* The muzzle: the barrel's tag_flash beyond the body's tag_barrel. The
     * body's own tag_flash must agree with it — that agreement is id's own
     * data being self-consistent, and the cheapest possible check that this
     * parser read the tags correctly at all. */
    vm_chain_ok = 0;
    if (barrel_rc == 0 && vm_body.have_barrel && vm_barrel.have_flash) {
        float dx, dy, dz;
        for (i = 0; i < 3; i++)
            vm_muzzle[i] = vm_body.tag_barrel[i] + vm_barrel.tag_flash[i];
        if (vm_body.have_flash) {
            dx = vm_muzzle[0] - vm_body.tag_flash[0];
            dy = vm_muzzle[1] - vm_body.tag_flash[1];
            dz = vm_muzzle[2] - vm_body.tag_flash[2];
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dz < 0) dz = -dz;
            vm_chain_ok = (dx <= 1.0f && dy <= 1.0f && dz <= 1.0f);
        }
        if (!vm_chain_ok) vm_parts = 1;      /* drop the barrel, say so below */
    }

    /* Flash: <base>_flash.md3, placed at that muzzle. */
    if (vm_parts == 2 && vm_chain_ok) {
        int m = 0;
        for (i = 0; base[i] && m < (int)sizeof(p) - 16; i++) p[m++] = base[i];
        {
            static const char *mid = "_flash.md3";
            int k = 0;
            while (mid[k]) p[m++] = mid[k++];
        }
        p[m] = '\0';
        flash_rc = vm_load_part(p, &vm_flash);
        if (flash_rc == 0) vm_parts = 3;
    }

    /* The reported texture is the BODY's: it is the gun the user sees, and the
     * flash's smaller image must not stand in for it. */
    if (vm_body.nsurf > 0 && vm_body.surf[0].tex) {
        vm_texw = vm_body.surf[0].texw;
        vm_texh = vm_body.surf[0].texh;
    } else {
        vm_texw = vm_texh = 0;
    }

    vm_surfaces = vm_body.nsurf + (vm_parts >= 2 ? vm_barrel.nsurf : 0) +
                  (vm_parts >= 3 ? vm_flash.nsurf : 0);
    vm_tris = vm_body.ntris + (vm_parts >= 2 ? vm_barrel.ntris : 0) +
              (vm_parts >= 3 ? vm_flash.ntris : 0);

    /* One line, integers only (this unit has no float printing): the muzzle in
     * tenths, the triangle count, the body texture, and whether the tag chain
     * checked out. This is what q3viewmodel_test asserts on. */
    {
        int m = 0;
        m += vm_str(line + m, "[Q3VIEWMODEL] load base=");
        m += vm_str(line + m, base);
        m += vm_str(line + m, " parts=");   m += vm_int(line + m, vm_parts);
        m += vm_str(line + m, " surf=");    m += vm_int(line + m, vm_surfaces);
        m += vm_str(line + m, " tris=");    m += vm_int(line + m, vm_tris);
        m += vm_str(line + m, " tex=");     m += vm_int(line + m, vm_texw);
        line[m++] = 'x';                    m += vm_int(line + m, vm_texh);
        m += vm_str(line + m, " muzzle=");
        m += vm_int(line + m, (int)(vm_muzzle[0] * 10.0f));
        line[m++] = ',';
        m += vm_int(line + m, (int)(vm_muzzle[1] * 10.0f));
        line[m++] = ',';
        m += vm_int(line + m, (int)(vm_muzzle[2] * 10.0f));
        m += vm_str(line + m, " chain=");   m += vm_int(line + m, vm_chain_ok);
        m += vm_str(line + m, " barrel=");  m += vm_int(line + m, barrel_rc);
        m += vm_str(line + m, " flash=");   m += vm_int(line + m, flash_rc);
        /* The flash's own triangle count, because it is the one part whose
         * DRAWN total depends on state: id draws it only while the weapon
         * fires, so "the pass submitted every triangle the loader parsed" is
         * `tris` only on a firing frame and `tris - flash_tris` otherwise. The
         * suites need that number to assert the exact relation instead of a
         * range. */
        m += vm_str(line + m, " flash_tris=");
        m += vm_int(line + m, vm_parts >= 3 ? vm_flash.ntris : 0);
        line[m++] = '\n';
        line[m] = '\0';
        write_serial_string(line);
    }
    return vm_parts;
}

void q3vm_unload(void) {
    vm_free_part(&vm_body);
    vm_free_part(&vm_barrel);
    vm_free_part(&vm_flash);
    vm_parts = 0;
    vm_surfaces = vm_tris = 0;
    vm_texw = vm_texh = 0;
    vm_chain_ok = 0;
    vm_kick = 0;
    vm_drawn = 0;
    vm_flash_mode = 0;
    vm_box[0] = vm_box[1] = vm_box[2] = vm_box[3] = 0;
    vm_muzzle[0] = vm_muzzle[1] = vm_muzzle[2] = 0.0f;
}

int q3vm_loaded(void) { return vm_parts; }

void q3vm_set_state(int firing, int grounded, int bob) {
    /* Held fire keeps the kick up for as long as id's own weaponstate says the
     * weapon is firing (a machinegun is automatic, so the retail gun does not
     * settle between shots); releasing it lets the kick decay. */
    if (firing) vm_kick = VM_KICK_FRAMES;
    else if (vm_kick > 0) vm_kick--;
    vm_firing = firing;
    vm_grounded = grounded;
    vm_bob = bob & 255;
}

void q3vm_stats(int *parts, int *surfaces, int *tris, int *texw, int *texh) {
    if (parts) *parts = vm_parts;
    if (surfaces) *surfaces = vm_surfaces;
    if (tris) *tris = vm_tris;
    if (texw) *texw = vm_texw;
    if (texh) *texh = vm_texh;
}

/* Triangles the LAST q3vm_draw submitted to the rasterizer. Equal to the
 * loaded count whenever the model is drawn whole — which is the point: a
 * loader that parsed 267 triangles and a pass that emits none is exactly the
 * failure "the gun is in the data but not on the screen". The ONE documented
 * exception is the muzzle flash, which id draws only on firing frames: on any
 * other frame this reads (loaded triangles - the flash's own count, reported
 * as flash_tris on the load line). */
int q3vm_drawn_tris(void) { return vm_drawn; }

/* How the LAST q3vm_draw submitted the muzzle flash: 1 = additive
 * (glBlendFunc(GL_ONE, GL_ONE), which is what id's own definition in
 * models.shader asks for), 0 = no flash was drawn. Separates "the flame is on
 * the screen" from "the flame was composited the way id composites it" — the
 * first version of this pass drew it opaque, and its black surround covered
 * the screen. */
int q3vm_flash_mode(void) { return vm_flash_mode; }

/* --- drawing ------------------------------------------------------------- */

static void vm_emit(const vm_part_t *part, const float ox, const float oy,
                    const float oz) {
    for (int s = 0; s < part->nsurf; s++) {
        const vm_surface_t *sf = &part->surf[s];
        if (!sf->v || !sf->idx || !sf->tex || !sf->nverts || !sf->ntris) continue;
        vm_drawn += sf->ntris;
        glBindTexture(GL_TEXTURE_2D, sf->tex);
        glBegin(GL_TRIANGLES);
        for (int t = 0; t < sf->ntris; t++) {
            for (int k = 0; k < 3; k++) {
                const float *v = &sf->v[sf->idx[t * 3 + k] * 5];
                glTexCoord2f(v[3], v[4]);
                glVertex3f(v[0] + ox, v[1] + oy, v[2] + oz);
            }
        }
        glEnd();
    }
}

/* The same mapping the two glRotatef calls below perform, written out so the
 * reported screen box is computed with it instead of a second guess:
 * cam = (-y, z, -x) + (right, up, -fwd). Returns 0 when the point is at or
 * behind the near plane and cannot be projected. */
static int vm_project(const float p[3], float gx, float gy, float gz,
                      float tan_x, float tan_y, int w, int h,
                      int *px, int *py) {
    float cx = -p[1] + gx;
    float cy =  p[2] + gy;
    float cz = -p[0] - gz;
    float ndc_x, ndc_y;
    if (cz > -1.0f) return 0;
    ndc_x = (cx / -cz) / tan_x;
    ndc_y = (cy / -cz) / tan_y;
    *px = (int)((ndc_x * 0.5f + 0.5f) * (float)w);
    *py = (int)((0.5f - ndc_y * 0.5f) * (float)h);   /* framebuffer rows are top-down */
    return 1;
}

static void vm_screen_box(float gx, float gy, float gz,
                          float tan_x, float tan_y, int w, int h) {
    float lo[3], hi[3];
    int x0 = w, y0 = h, x1 = -1, y1 = -1, i, c;

    vm_box[0] = vm_box[1] = vm_box[2] = vm_box[3] = 0;
    if (!vm_body.have_bounds) return;

    for (i = 0; i < 3; i++) {
        lo[i] = vm_body.bounds[0][i];
        hi[i] = vm_body.bounds[1][i];
    }
    /* The barrel is part of the silhouette; the flash is not (it is drawn only
     * on the frames the weapon fires, and a box that blinked would be a worse
     * thing for a test to assert on than a gun-only one). */
    if (vm_parts >= 2 && vm_chain_ok && vm_barrel.have_bounds) {
        for (i = 0; i < 3; i++) {
            float a = vm_barrel.bounds[0][i] + vm_body.tag_barrel[i];
            float b = vm_barrel.bounds[1][i] + vm_body.tag_barrel[i];
            if (a < lo[i]) lo[i] = a;
            if (b > hi[i]) hi[i] = b;
        }
    }

    for (c = 0; c < 8; c++) {
        float p[3];
        int px = 0, py = 0;
        p[0] = (c & 1) ? hi[0] : lo[0];
        p[1] = (c & 2) ? hi[1] : lo[1];
        p[2] = (c & 4) ? hi[2] : lo[2];
        if (!vm_project(p, gx, gy, gz, tan_x, tan_y, w, h, &px, &py)) continue;
        if (px < x0) x0 = px;
        if (px > x1) x1 = px;
        if (py < y0) y0 = py;
        if (py > y1) y1 = py;
    }
    if (x1 < 0 || y1 < 0) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w - 1) x1 = w - 1;
    if (y1 > h - 1) y1 = h - 1;
    vm_box[0] = x0; vm_box[1] = y0; vm_box[2] = x1; vm_box[3] = y1;
}

int q3vm_draw(float tan_x, float tan_y, int w, int h, int *box) {
    float gx, gy, gz, bob_y, kick;

    if (box) { box[0] = box[1] = box[2] = box[3] = 0; }
    vm_drawn = 0;
    if (!vm_parts || w <= 0 || h <= 0) return 0;

    /* Walk bob (the module's own bobCycle, doubled per step pair exactly as
     * PM_WalkBob moves the view) and the air lift, both in model units; then
     * recoil, which pulls the gun toward the eye and up. */
    bob_y = vm_grounded ? ((float)(((vm_bob * 6) >> 8) - 3) * VM_BOB_UNITS) : -4.0f * VM_BOB_UNITS;
    kick = (vm_kick > 0) ? 1.0f : 0.0f;
    gx = VM_GUN_RIGHT;
    gy = VM_GUN_UP + bob_y + kick * VM_KICK_UP;
    gz = VM_GUN_FWD - kick * VM_KICK_BACK;

    /* The weapon is drawn over the world, not inside it: clearing depth makes
     * the pass independent of whatever the map put within arm's reach (id's
     * own answer is a depth range, which this rasterizer does not have — see
     * clear.c's glopClear, whose depth half is exactly this call). */
    glClear(GL_DEPTH_BUFFER_BIT);

    /* Texturing is a MODE, and the world pass turns it off again when it is
     * done (q3w_draw's closing glDisable(GL_TEXTURE_2D)) — so a pass drawn
     * after it would rasterize the model's triangles in the current colour
     * instead of its texture. The first build did exactly that and the gun
     * came out as a white silhouette, which is how this line was found. */
    glEnable(GL_TEXTURE_2D);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(gx, gy, -gz);          /* camera space: -z is forward */
    glRotatef(90.0f, 0.0f, 0.0f, 1.0f); /* X -> -Zc, then Y -> -Xc, Z -> +Yc */
    glRotatef(90.0f, 0.0f, 1.0f, 0.0f);
    /* Full bright: id lights the view model from a fixed point so it reads the
     * same in a dark corridor as in a lit room, and the world pass left the
     * current colour at whatever the last face set. */
    glColor3f(1.0f, 1.0f, 1.0f);

    vm_emit(&vm_body, 0.0f, 0.0f, 0.0f);
    if (vm_parts >= 2 && vm_chain_ok)
        vm_emit(&vm_barrel, vm_body.tag_barrel[0], vm_body.tag_barrel[1],
                vm_body.tag_barrel[2]);

    /* v38.125: the muzzle flash is ADDITIVE, and that is id's own
     * instruction, not a taste call. The demo's scripts/models.shader says:
     *
     *   models/weapons2/machinegun/f_machinegun
     *   {  sort additive ; cull disable
     *      { map …/f_machinegun.tga ; blendfunc GL_ONE GL_ONE } }
     *
     * Drawn opaque (v38.124, and the user's screenshot of it), the flash's
     * image contributes its BLACK surround: a 24-triangle star whose quads are
     * 30-40 units across, 25.6 units from the eye at a 90 degree FOV, so the
     * surround covered most of the screen and firing blacked the view out.
     * Additive compositing adds the image's brightness and nothing else, which
     * is exactly how the retail frame reads: a flare at the muzzle, the world
     * still visible around it. `cull disable` likewise: the star is a set of
     * crossing quads, so half of them face away and culling would drop them.
     *
     * This fork supports it (TGL_FEATURE_BLEND 1, GL_ONE/GL_ONE in its
     * TGL_BLEND_FUNC switch — GL_SRC_ALPHA is NOT implemented, so additive is
     * also the only blending available here). Order matters for the same
     * reason it does in id: body and barrel are already down, and nothing is
     * drawn after the flash, so no depth trickery is needed beyond the clear
     * at the top of this pass. */
    vm_flash_mode = 0;
    if (vm_parts >= 3 && vm_chain_ok && vm_firing) {
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        vm_emit(&vm_flash, vm_muzzle[0], vm_muzzle[1], vm_muzzle[2]);
        glDisable(GL_BLEND);
        vm_flash_mode = 1;
    }

    glDisable(GL_TEXTURE_2D);      /* leave the mode as the world left it */

    if (tan_x > 0.0f && tan_y > 0.0f) {
        vm_screen_box(gx, gy, gz, tan_x, tan_y, w, h);
        if (box) {
            box[0] = vm_box[0]; box[1] = vm_box[1];
            box[2] = vm_box[2]; box[3] = vm_box[3];
        }
    }
    return 1;
}
