/* q3hud.c — id's own Quake III status bar, drawn from id's own images (v38.127).
 *
 * WHAT THIS IS. `CG_DrawStatusBar()` from id's cgame/cg_draw.c, ported: the
 * ammo field and its icon, the health field, the armor field and its icon, and
 * the FFA score boxes at the bottom right. Every layout number and every colour
 * is id's own, quoted at the point of use, and every picture is the one the game
 * itself ships:
 *
 *   gfx/2d/numbers/{zero..nine,minus}_32b.tga   11 fields, 32x32, 32-bit
 *   gfx/2d/bigchars.tga                         the 256x256 16x16-cell charset
 *   gfx/2d/select.tga                           the "this one is yours" overlay
 *   icons/icona_<weapon>.tga                    the ammo icon
 *   icons/iconr_yellow.tga                      the armor icon
 *
 * (cg_main.c:834-838 registers the number fields, :871 selectShader, :964
 * armorIcon, :1875 the charset; cg_weapons.c takes each weapon's ammoIcon from
 * bg_itemlist, which is what names icons/icona_*.)
 *
 * WHY IT IS NOT DRAWN THROUGH GL. Every one of those pictures is 32-bit RGBA
 * with real alpha, and id's HUD blends with GL_SRC_ALPHA. This port's rasterizer
 * cannot express that blend at all (third_party/tinygl/src/misc.c's
 * TGL_BLEND_FUNC implements GL_ONE/GL_ONE and the ONE_MINUS_* cases; a
 * GL_SRC_ALPHA request falls through to GL_ONE). So the HUD composites itself
 * into the finished framebuffer, the way v38.110's perf panel and v38.126's fps
 * readout already do — which also means it adds nothing to the world pass's
 * cost, and the frame histogram the suites assert on stays the 3D pass.
 *
 * WHERE IT IS DRAWN. In Q3's virtual 640x480 screen, scaled to whatever the
 * renderer is drawing at: id's CG_AdjustFrom640 (cg_drawtools.c:33) multiplies
 * x/y/w/h by cgs.screenXScale/YScale, so the layout below can be id's numbers
 * verbatim. At the port's 320x240 that is a factor of 0.5, and a number arrives
 * at 16x24 pixels — the same HUD retail draws at 640x480.
 *
 * WHAT IS NOT HERE, because the port has no data for it: the head slot
 * (CG_DrawStatusBarHead needs models/players/<model>/head_*.md3), the 3D ammo
 * and armor models (this draws id's 2D icon path instead), the team background
 * bar (CG_DrawTeamBackground returns unless PERS_TEAM is red or blue, and this
 * port's playerState is TEAM_FREE), and CG_DrawPowerups (no powerups exist
 * without an item pickup path).
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "q3hud.h"
#include "q3bsp.h"          /* q3bsp_read_file / q3bsp_free_file / kmalloc */

extern void *kmalloc(uint32_t size);
extern void  kfree(void *p);
extern void  write_serial_string(const char *s);

/* --- id's constants -------------------------------------------------------
 * cg_local.h:67-72 and q_shared.h:508-518. CHAR_WIDTH/HEIGHT are the size a
 * number field is drawn at (out of a 32x32 source, so the digits come out tall);
 * ICON_SIZE is the ammo/armor icon; TEXT_ICON_SPACE is the gap between a field
 * and its icon; STAT_MINUS is the frame of cgs.media.numberShaders that holds
 * the minus sign. */
#define HUD_STAT_MINUS       10
#define HUD_ICON_SIZE        48
#define HUD_CHAR_WIDTH       32
#define HUD_CHAR_HEIGHT      48
#define HUD_TEXT_ICON_SPACE   4
#define HUD_BIGCHAR_WIDTH    16        /* q_shared.h: BIGCHAR_WIDTH  */
#define HUD_BIGCHAR_HEIGHT   16        /* q_shared.h: BIGCHAR_HEIGHT */
#define HUD_SCORE_NOT_PRESENT (-1)     /* cg_local.h: SCORE_NOT_PRESENT */

/* id's virtual screen, the space every number below is authored in. */
#define HUD_VIRT_W           640.0f
#define HUD_VIRT_H           480.0f

/* CG_DrawStatusBar's own y for every field: 432 of 480, i.e. the numbers' 48
 * pixel row sits flush against the bottom of the screen. */
#define HUD_ROW_Y            432.0f
#define HUD_HEALTH_X         185.0f
#define HUD_ARMOR_X          370.0f
#define HUD_AMMO_X             0.0f
#define HUD_FIELD_WIDTH        3

/* --- small integer formatting (Com_sprintf("%i"/"%2i"), no libc) ---------
 * Defined before the loader below, which prints sizes and byte counts. */
static int hud_uint(char *dst, unsigned u, int pad, char padch) {
    char tmp[12];
    int n = 0, i = 0;
    if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
    while (pad > n) { dst[i++] = padch; pad--; }
    while (n) dst[i++] = tmp[--n];
    dst[i] = '\0';
    return i;
}

static int hud_int(char *dst, int v, int pad, char padch) {
    int i = 0;
    unsigned u;
    if (v < 0) {
        dst[i++] = '-';
        u = 0u - (unsigned)v;         /* two's complement, no overflow */
        if (pad > 0) pad--;           /* the sign takes a column, as %2i does */
    } else {
        u = (unsigned)v;
    }
    i += hud_uint(dst + i, u, pad, padch);
    return i;
}

/* Append, for the load lines. Returns the string offset. */
static int hud_cat(char *dst, int o, int cap, const char *s) {
    while (*s && o < cap - 1) dst[o++] = *s++;
    dst[o] = '\0';
    return o;
}

/* --- the pictures --------------------------------------------------------
 * One slot per file. The number fields are indices 0..10 so a digit IS a slot
 * (cg_main.c's sb_nums[] order: zero..nine, minus). A slot's `name` is the
 * compile-time table entry for everything except the ammo icon, which id picks
 * from the weapon. */
enum {
    HUD_IMG_NUM0 = 0,
    HUD_IMG_MINUS = HUD_IMG_NUM0 + 10,
    HUD_IMG_CHARSET,
    HUD_IMG_AMMOICON,
    HUD_IMG_ARMORICON,
    HUD_IMG_SELECT,
    HUD_IMG_COUNT
};

static const char *const hud_name[HUD_IMG_COUNT] = {
    "gfx/2d/numbers/zero_32b",
    "gfx/2d/numbers/one_32b",
    "gfx/2d/numbers/two_32b",
    "gfx/2d/numbers/three_32b",
    "gfx/2d/numbers/four_32b",
    "gfx/2d/numbers/five_32b",
    "gfx/2d/numbers/six_32b",
    "gfx/2d/numbers/seven_32b",
    "gfx/2d/numbers/eight_32b",
    "gfx/2d/numbers/nine_32b",
    "gfx/2d/numbers/minus_32b",
    "gfx/2d/bigchars",
    "icons/icona_machinegun",
    "icons/iconr_yellow",
    "gfx/2d/select",
};

typedef struct {
    const char    *name;       /* the ammo icon's own file; see hud_name_of() */
    int            state;      /* 0 = not tried, 1 = loaded, -1 = not on the volume */
    int            w, h;
    unsigned char *rgba;       /* w*h*4, top-down */
    char           path[80];
} hud_img_t;

static hud_img_t hud_img[HUD_IMG_COUNT];
static int hud_loaded;         /* the first q3hud_draw() did the load pass */
static int hud_images;         /* pictures decoded off the volume         */
static int hud_missing;        /* named by id, absent from the volume     */
static int hud_draws;          /* pictures composited this run            */
static int hud_digits;         /* number glyphs composited this run       */
static int hud_ammo_wp = -2;   /* the weapon the ammo icon slot was chosen for */

/* The current frame's values, straight from the game's playerState. */
static int hud_health, hud_armor, hud_ammo, hud_weapon, hud_score, hud_firing;
static int hud_now_ms;

/* The painter's destination. */
static uint32_t *h_px;
static int       h_pitch, h_w, h_h;
static float     h_sx = 1.0f, h_sy = 1.0f;

/* --- 32-bit TGA -> RGBA ---------------------------------------------------
 * The world module's decoder (q3world_render.c's tga_decode) throws the alpha
 * channel away because GL wants RGB; a HUD picture IS its alpha channel, so
 * this reads the same files for what the other one drops. Both forms id's tools
 * write: uncompressed (type 2) and RLE (type 10), 24 or 32 bits per pixel,
 * either origin (the image descriptor's bit 5). 32-bit pixels are BGRA. */
static int hud_tga_decode(const unsigned char *raw, int len,
                          unsigned char **rgba_out, int *w_out, int *h_out) {
    int id_len, cmap, type, w, h, bpp, desc, bytes;
    const unsigned char *src;
    unsigned char *dst;
    int x, y;

    if (!raw || len < 18) return -1;
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

    dst = (unsigned char *)kmalloc((uint32_t)(w * h * 4));
    if (!dst) return -4;

    if (type == 2) {
        if ((int)(src - raw) + w * h * bytes > len) { kfree(dst); return -5; }
        for (y = 0; y < h; y++) {
            int row = (desc & 0x20) ? y : (h - 1 - y);
            for (x = 0; x < w; x++) {
                const unsigned char *p = src + ((size_t)y * w + x) * bytes;
                unsigned char *d = dst + ((size_t)row * w + x) * 4;
                d[0] = p[2]; d[1] = p[1]; d[2] = p[0];
                d[3] = (bytes == 4) ? p[3] : 255;
            }
        }
    } else {
        int total = w * h, done = 0;
        const unsigned char *p = src;
        while (done < total) {
            int count, rep;
            /* Written by both paths below before the first use (a repeat run
             * fills it here, a literal run inside the loop); zeroed so a
             * truncated stream cannot leak an uninitialised byte into a
             * pixel. */
            unsigned char px[4] = { 0, 0, 0, 255 };
            if ((int)(p - raw) >= len) { kfree(dst); return -6; }
            count = *p++;
            rep = count & 0x80;
            count = (count & 0x7F) + 1;
            if (rep) {
                if ((int)(p - raw) + bytes > len) { kfree(dst); return -7; }
                px[0] = p[2]; px[1] = p[1]; px[2] = p[0];
                px[3] = (bytes == 4) ? p[3] : 255;
                p += bytes;
            }
            while (count-- > 0 && done < total) {
                int row = (desc & 0x20) ? (done / w) : (h - 1 - done / w);
                int col = done % w;
                unsigned char *d = dst + ((size_t)row * w + col) * 4;
                if (!rep) {
                    if ((int)(p - raw) + bytes > len) { kfree(dst); return -8; }
                    px[0] = p[2]; px[1] = p[1]; px[2] = p[0];
                    px[3] = (bytes == 4) ? p[3] : 255;
                    p += bytes;
                }
                d[0] = px[0]; d[1] = px[1]; d[2] = px[2]; d[3] = px[3];
                done++;
            }
        }
    }

    *rgba_out = dst;
    *w_out = w;
    *h_out = h;
    return 0;
}

/* Which file a slot wants. Every slot but one wants the table's entry; the ammo
 * icon wants whatever the weapon says, and a NULL there is id's `if (icon)`
 * coming out false — a slot with no picture to load, not a missing one. */
static const char *hud_name_of(int idx) {
    if (idx == HUD_IMG_AMMOICON) return hud_img[idx].name;
    return hud_name[idx];
}

/* Load one picture, once. Q3's own files carry their extension (.tga for every
 * gfx/2d and icons/ asset the demo ships); the shaderless convention would have
 * the engine append one, so both are probed — .tga first, because that is what
 * the pak holds, then .jpg for a pak that spells it the other way. A miss is
 * remembered, so a volume without HUD art costs one failed lookup rather than
 * one per frame. */
static int hud_load(int idx) {
    static const char *const exts[] = { ".tga", ".jpg", "" };
    hud_img_t *im;
    const char *want;
    unsigned char *raw = NULL;
    int len = 0, e;

    if (idx < 0 || idx >= HUD_IMG_COUNT) return 0;
    im = &hud_img[idx];
    if (im->state) return im->state > 0;
    want = hud_name_of(idx);
    if (!want) { im->state = -1; return 0; }   /* id draws no icon for it */

    for (e = 0; e < 3; e++) {
        char path[80];
        int pl = 0, i;

        pl = hud_cat(path, pl, (int)sizeof(path), want);
        pl = hud_cat(path, pl, (int)sizeof(path), exts[e]);

        if (q3bsp_read_file(path, &raw, &len) == 0 && raw && len > 0) {
            int w = 0, h = 0;
            unsigned char *rgba = NULL;
            if (hud_tga_decode(raw, len, &rgba, &w, &h) == 0) {
                int o = 0;
                char lb[200];

                im->state = 1;
                im->w = w;
                im->h = h;
                im->rgba = rgba;
                for (i = 0; i <= pl; i++) im->path[i] = path[i];
                hud_images++;

                /* One line per picture, the same shape the world's texture
                 * loader prints: which name, what file answered it, how big.
                 * A HUD that loaded nothing has to be visible in the log, not
                 * merely absent from the screen. */
                o = hud_cat(lb, o, (int)sizeof(lb), "[Q3ARENA] hud img ");
                o += hud_uint(lb + o, (unsigned)idx, 0, ' ');
                o = hud_cat(lb, o, (int)sizeof(lb), " ");
                o = hud_cat(lb, o, (int)sizeof(lb), want);
                o = hud_cat(lb, o, (int)sizeof(lb), " path=");
                o = hud_cat(lb, o, (int)sizeof(lb), path);
                o = hud_cat(lb, o, (int)sizeof(lb), " size=");
                o += hud_uint(lb + o, (unsigned)w, 0, ' ');
                o = hud_cat(lb, o, (int)sizeof(lb), "x");
                o += hud_uint(lb + o, (unsigned)h, 0, ' ');
                o = hud_cat(lb, o, (int)sizeof(lb), " bytes=");
                o += hud_uint(lb + o, (unsigned)len, 0, ' ');
                write_serial_string(lb);

                q3bsp_free_file(raw);
                return 1;
            }
            /* Read but not decodable: reported as missing rather than passed
             * off as HUD art. */
        }
        if (raw) { q3bsp_free_file(raw); raw = NULL; }
    }

    im->state = -1;
    hud_missing++;
    return 0;
}

static void h_blend(int x, int y, int r, int g, int b, int a) {
    uint32_t *p;
    uint32_t d;
    int dr, dg, db;
    if (a <= 0 || x < 0 || y < 0 || x >= h_w || y >= h_h) return;
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    if (a > 255) a = 255;
    p = h_px + (size_t)y * h_pitch + x;
    d = *p;
    dr = (int)((d >> 16) & 0xFF);
    dg = (int)((d >> 8) & 0xFF);
    db = (int)(d & 0xFF);
    /* id blends with GL_SRC_ALPHA: dst*(1-a) + src*a. Integer, so both present
     * paths composite the same pixels. */
    dr = (dr * (255 - a) + r * a) / 255;
    dg = (dg * (255 - a) + g * a) / 255;
    db = (db * (255 - a) + b * a) / 255;
    *p = ((uint32_t)dr << 16) | ((uint32_t)dg << 8) | (uint32_t)db;
}

/* CG_FillRect (cg_drawtools.c): the solid 33%-alpha quads behind the score
 * boxes and the team overlay. */
static void h_fill(float qx, float qy, float qw, float qh, const float *rgba) {
    int x0 = (int)(qx * h_sx + 0.5f), y0 = (int)(qy * h_sy + 0.5f);
    int x1 = x0 + (int)(qw * h_sx + 0.5f), y1 = y0 + (int)(qh * h_sy + 0.5f);
    int x, y;
    int a = (int)(rgba[3] * 255.0f + 0.5f);
    int r = (int)(rgba[0] * 255.0f + 0.5f);
    int g = (int)(rgba[1] * 255.0f + 0.5f);
    int b = (int)(rgba[2] * 255.0f + 0.5f);
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) h_blend(x, y, r, g, b, a);
}

/* The one primitive every picture goes through: a REGION of an image (in source
 * pixels) stretched into a rect in virtual-screen units, modulated by id's
 * current colour — which is what trap_R_SetColor means, a multiply of the
 * texel's colour and the texel's alpha.
 *
 * The port draws at 320x240, so a status-bar digit arrives at 16x24 out of a
 * 32x32 source: id's GPU gets there with a linear filter, and this averages the
 * source box each destination pixel covers — the same thing by hand, with no
 * sampler state to set. */
static void h_blit(int idx, float ssx, float ssy, float ssw, float ssh,
                   float qx, float qy, float qw, float qh, const float *tint) {
    const hud_img_t *im;
    int x0, y0, dw, dh, dx, dy;

    if (!hud_load(idx)) return;
    im = &hud_img[idx];
    if (!im->rgba || im->w <= 0 || im->h <= 0) return;
    if (ssw <= 0.0f || ssh <= 0.0f) return;

    x0 = (int)(qx * h_sx + 0.5f);
    y0 = (int)(qy * h_sy + 0.5f);
    dw = (int)(qw * h_sx + 0.5f);
    dh = (int)(qh * h_sy + 0.5f);
    if (dw <= 0 || dh <= 0) return;

    for (dy = 0; dy < dh; dy++) {
        int sy0 = (int)(ssy + (float)dy * ssh / (float)dh);
        int sy1 = (int)(ssy + (float)(dy + 1) * ssh / (float)dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy0 < 0) sy0 = 0;
        if (sy1 > im->h) sy1 = im->h;
        for (dx = 0; dx < dw; dx++) {
            int sx0 = (int)(ssx + (float)dx * ssw / (float)dw);
            int sx1 = (int)(ssx + (float)(dx + 1) * ssw / (float)dw);
            int sumr = 0, sumg = 0, sumb = 0, suma = 0, n = 0;
            int sx, sy;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx0 < 0) sx0 = 0;
            if (sx1 > im->w) sx1 = im->w;
            for (sy = sy0; sy < sy1; sy++) {
                const unsigned char *row = im->rgba + ((size_t)sy * im->w) * 4;
                for (sx = sx0; sx < sx1; sx++) {
                    const unsigned char *p = row + (size_t)sx * 4;
                    sumr += p[0]; sumg += p[1]; sumb += p[2]; suma += p[3];
                    n++;
                }
            }
            if (!n) continue;
            h_blend(x0 + dx, y0 + dy,
                    (int)((float)sumr / n * tint[0]),
                    (int)((float)sumg / n * tint[1]),
                    (int)((float)sumb / n * tint[2]),
                    (int)((float)suma / n * tint[3]));
        }
    }
    hud_draws++;
}

/* CG_DrawPic (cg_drawtools.c:108): the whole picture into the rect. */
static void h_pic(int idx, float qx, float qy, float qw, float qh,
                  const float *tint) {
    if (!hud_load(idx)) return;
    h_blit(idx, 0.0f, 0.0f, (float)hud_img[idx].w, (float)hud_img[idx].h,
           qx, qy, qw, qh, tint);
}

/* CG_DrawChar (cg_drawtools.c:122): the charset is a 16x16 grid of cells in a
 * 256x256 atlas — the cell is (ch>>4, ch&15) and id's own 0.0625 is the cell
 * size in texture units. A space draws nothing but still advances, which is what
 * right-aligns "%2i" inside the score box. */
static void h_char(int ch, float qx, float qy, float qw, float qh,
                   const float *tint) {
    const hud_img_t *cs;
    float cw, chh;
    ch &= 255;
    if (ch == ' ') return;
    if (!hud_load(HUD_IMG_CHARSET)) return;
    cs = &hud_img[HUD_IMG_CHARSET];
    if (!cs->rgba) return;
    cw = (float)cs->w / 16.0f;
    chh = (float)cs->h / 16.0f;
    h_blit(HUD_IMG_CHARSET, (float)(ch & 15) * cw, (float)(ch >> 4) * chh,
           cw, chh, qx, qy, qw, qh, tint);
}

/* CG_DrawStringExt's inner loop with noSpacing set (cg_drawtools.c): one cell
 * per character. The string here is always a formatted number, so there are no
 * ^-colour codes to parse (id's loop handles them; nothing reaching this can
 * contain one). */
static void h_string(float qx, float qy, const char *s, const float *tint) {
    for (; s && *s; s++) {
        h_char(*s, qx, qy, HUD_BIGCHAR_WIDTH, HUD_BIGCHAR_HEIGHT, tint);
        qx += HUD_BIGCHAR_WIDTH;
    }
}

/* CG_DrawField (cg_draw.c:229). The clamp switch, the "+2" and the one-cell-per
 * -remaining-digit step are id's. A value WIDER than the field prints its first
 * `width` characters — id truncates the string's length, not the formatting. */
static void h_field(float qx, float qy, int width, int value,
                    const float *tint) {
    char num[16];
    int l, i;
    if (width < 1) return;
    if (width > 5) width = 5;
    switch (width) {
    case 1:
        value = value > 9 ? 9 : value;
        value = value < 0 ? 0 : value;
        break;
    case 2:
        value = value > 99 ? 99 : value;
        value = value < -9 ? -9 : value;
        break;
    case 3:
        value = value > 999 ? 999 : value;
        value = value < -99 ? -99 : value;
        break;
    case 4:
        value = value > 9999 ? 9999 : value;
        value = value < -999 ? -999 : value;
        break;
    default:
        break;
    }
    hud_int(num, value, 0, ' ');
    l = (int)strlen(num);
    if (l > width) l = width;
    qx += 2.0f + (float)(HUD_CHAR_WIDTH * (width - l));
    for (i = 0; i < l; i++) {
        int frame = (num[i] == '-') ? HUD_STAT_MINUS : (num[i] - '0');
        h_pic(HUD_IMG_NUM0 + frame, qx, qy, HUD_CHAR_WIDTH, HUD_CHAR_HEIGHT,
              tint);
        qx += HUD_CHAR_WIDTH;
        hud_digits++;
    }
}

/* The ammo icon is id's per-weapon icon (cg_weapons.c takes it from bg_itemlist,
 * whose ammo entries name icons/icona_*). A weapon whose file this table does
 * not name keeps the machinegun's, because the machinegun is the only weapon
 * this port's world can hold; WP_GAUNTLET takes no ammo and has no icon at all,
 * which is id's `if (icon)` test coming out NULL. */
static const char *hud_ammo_icon_name(int wp) {
    switch (wp) {
    case 2: return "icons/icona_machinegun";
    case 3: return "icons/icona_shotgun";
    case 4: return "icons/icona_grenade";
    case 5: return "icons/icona_rocket";
    case 6: return "icons/icona_lightning";
    case 7: return "icons/icona_railgun";
    case 8: return "icons/icona_plasma";
    case 9: return "icons/icona_bfg";
    default: return NULL;
    }
}

static void hud_pick_ammo_icon(int wp) {
    const char *n = hud_ammo_icon_name(wp);
    if (wp == hud_ammo_wp) return;
    hud_ammo_wp = wp;
    if (hud_img[HUD_IMG_AMMOICON].rgba) kfree(hud_img[HUD_IMG_AMMOICON].rgba);
    hud_img[HUD_IMG_AMMOICON].rgba = NULL;
    hud_img[HUD_IMG_AMMOICON].state = 0;
    hud_img[HUD_IMG_AMMOICON].w = hud_img[HUD_IMG_AMMOICON].h = 0;
    hud_img[HUD_IMG_AMMOICON].name = n;   /* NULL: no icon for this weapon */
}

/* --- the status bar ------------------------------------------------------ */
static const float hud_colors[4][4] = {
    /* cg_draw.c:600-605 — normal, low health, weapon firing, health > 100 */
    { 1.0f, 0.69f, 0.0f, 1.0f },
    { 1.0f, 0.2f,  0.2f, 1.0f },
    { 0.5f, 0.5f,  0.5f, 1.0f },
    { 1.0f, 1.0f,  1.0f, 1.0f },
};
static const float hud_white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

/* CG_DrawScores' FFA branch (cg_draw.c:1150-1205) — the small two-box score
 * display at the bottom right, called from CG_DrawLowerRight with
 * y = 480 - ICON_SIZE.
 *
 * id shows two boxes: the leader's score, and yours. This port knows exactly one
 * score (the module's own playerState), so s1 stays SCORE_NOT_PRESENT and the
 * single box is the s2 one — which is also the one id paints red and overlays
 * with gfx/2d/select when the score in it is yours and it is not the leader's. */
static void h_scores(void) {
    float y = HUD_VIRT_H - HUD_ICON_SIZE;      /* CG_DrawLowerRight */
    int score = hud_score;
    int s1 = HUD_SCORE_NOT_PRESENT, s2 = HUD_SCORE_NOT_PRESENT;
    int spectator = 0;
    char s[8];
    float w, x;

    y -= HUD_BIGCHAR_HEIGHT + 8.0f;
    if (score == HUD_SCORE_NOT_PRESENT) return;
    if (s1 != score) s2 = score;
    if (s2 == HUD_SCORE_NOT_PRESENT) return;

    hud_int(s, s2, 2, ' ');                    /* va("%2i", s2) */
    w = (float)strlen(s) * HUD_BIGCHAR_WIDTH + 8.0f;
    x = HUD_VIRT_W - w;
    if (!spectator && score == s2 && score != s1) {
        const float red[4] = { 1.0f, 0.0f, 0.0f, 0.33f };
        h_fill(x, y - 4.0f, w, HUD_BIGCHAR_HEIGHT + 8.0f, red);
        h_pic(HUD_IMG_SELECT, x, y - 4.0f, w, HUD_BIGCHAR_HEIGHT + 8.0f,
              hud_white);
    } else {
        const float grey[4] = { 0.5f, 0.5f, 0.5f, 0.33f };
        h_fill(x, y - 4.0f, w, HUD_BIGCHAR_HEIGHT + 8.0f, grey);
    }
    h_string(x + 4.0f, y, s, hud_white);
}

/* CG_DrawStatusBar (cg_draw.c:590-670). The order is id's: the team background
 * (skipped — see the header), the ammo field and its icon, the health field,
 * then the armor field and its icon. The head slot between health and armor is
 * the one part this port cannot draw: it is a player model. */
static void h_statusbar(void) {
    int value;

    /* ammo: the field at x=0, the icon after it */
    if (hud_weapon) {
        value = hud_ammo;
        if (value > -1) {
            const float *color;
            if (hud_firing) color = hud_colors[2];     /* WEAPON_FIRING: grey */
            else if (value >= 0) color = hud_colors[0];
            else color = hud_colors[1];
            h_field(HUD_AMMO_X, HUD_ROW_Y, HUD_FIELD_WIDTH, value, color);
            hud_pick_ammo_icon(hud_weapon);
            if (hud_ammo_icon_name(hud_weapon))
                h_pic(HUD_IMG_AMMOICON,
                      HUD_CHAR_WIDTH * 3 + HUD_TEXT_ICON_SPACE, HUD_ROW_Y,
                      HUD_ICON_SIZE, HUD_ICON_SIZE, hud_white);
        }
    }

    /* health */
    value = hud_health;
    if (value > 100) {
        h_field(HUD_HEALTH_X, HUD_ROW_Y, HUD_FIELD_WIDTH, value, hud_colors[3]);
    } else if (value > 25) {
        h_field(HUD_HEALTH_X, HUD_ROW_Y, HUD_FIELD_WIDTH, value, hud_colors[0]);
    } else if (value > 0) {
        /* id flashes the field between amber and red below 26 health:
         * color = (cg.time >> 8) & 1 (cg_draw.c:645). */
        h_field(HUD_HEALTH_X, HUD_ROW_Y, HUD_FIELD_WIDTH, value,
                hud_colors[(hud_now_ms >> 8) & 1]);
    } else {
        h_field(HUD_HEALTH_X, HUD_ROW_Y, HUD_FIELD_WIDTH, value, hud_colors[1]);
    }

    /* Armor is only drawn when there is any — id's `if (value > 0)` wraps the
     * field AND its icon, which is why a screenshot with no armor shows neither.
     * (id also calls CG_ColorForHealth() here and feeds the result to
     * trap_R_SetColor() on the next line, then overwrites it with colors[0]
     * before anything is drawn: Q3A's armour field is colors[0] whatever the
     * health/armor ratio is. That call is dead code in id's own source and is
     * not reproduced here.) */
    value = hud_armor;
    if (value > 0) {
        h_field(HUD_ARMOR_X, HUD_ROW_Y, HUD_FIELD_WIDTH, value, hud_colors[0]);
        h_pic(HUD_IMG_ARMORICON, HUD_ARMOR_X + HUD_CHAR_WIDTH * 3 +
              HUD_TEXT_ICON_SPACE, HUD_ROW_Y, HUD_ICON_SIZE, HUD_ICON_SIZE,
              hud_white);
    }
}

/* --- entry points -------------------------------------------------------- */

void q3hud_set(int health, int armor, int ammo, int weapon, int score,
               int firing, int now_ms) {
    hud_health = health;
    hud_armor = armor;
    hud_ammo = ammo;
    hud_weapon = weapon;
    hud_score = score;
    hud_firing = firing;
    hud_now_ms = now_ms;
}

void q3hud_draw(uint32_t *px, int pitch, int w, int h) {
    int i;

    if (!px || w <= 0 || h <= 0) return;
    h_px = px;
    h_pitch = pitch;
    h_w = w;
    h_h = h;

    /* id's CG_AdjustFrom640: every number below is authored in 640x480. */
    h_sx = (float)w / HUD_VIRT_W;
    h_sy = (float)h / HUD_VIRT_H;

    /* The ammo icon's file depends on the weapon the module is holding, so the
     * slot is chosen before the load pass and re-chosen if it changes. */
    hud_pick_ammo_icon(hud_weapon);

    if (!hud_loaded) {
        int o = 0;
        char lb[120];
        hud_loaded = 1;
        for (i = 0; i < HUD_IMG_COUNT; i++) hud_load(i);
        o = hud_cat(lb, o, (int)sizeof(lb), "[Q3ARENA] hud: ");
        o += hud_uint(lb + o, (unsigned)hud_images, 0, ' ');
        o = hud_cat(lb, o, (int)sizeof(lb), " image(s), ");
        o += hud_uint(lb + o, (unsigned)hud_missing, 0, ' ');
        o = hud_cat(lb, o, (int)sizeof(lb), " missing (id's own status bar art)");
        write_serial_string(lb);
    }

    /* Nothing decoded at all: this volume carries no HUD art (CI's fixture
     * arena), so there is nothing to draw and the frame is untouched. */
    if (hud_images == 0) return;

    h_statusbar();
    h_scores();
}

void q3hud_unload(void) {
    int i;
    for (i = 0; i < HUD_IMG_COUNT; i++) {
        if (hud_img[i].rgba) kfree(hud_img[i].rgba);
        hud_img[i].rgba = NULL;
        hud_img[i].state = 0;
        hud_img[i].w = hud_img[i].h = 0;
    }
    hud_loaded = 0;
    hud_images = hud_missing = 0;
    hud_ammo_wp = -2;
}

void q3hud_stats(int *images, int *missing, int *draws, int *digits) {
    if (images) *images = hud_images;
    if (missing) *missing = hud_missing;
    if (draws) *draws = hud_draws;
    if (digits) *digits = hud_digits;
}
