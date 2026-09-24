/* q3_map.c — MCTBSP1 parser/loader (v38.104, Q3 phase 4). See q3_map.h.
 *
 * The tokenizer is deliberately minimal: a scanner over the FS_ReadFile
 * buffer (whitespace-separated tokens, '#' comments to EOL). This build has
 * no general sscanf, and pulling one in just to parse a 1.5 KB map is how
 * libc creeps back in.
 *
 * Validation policy: a directive with a bad or missing number fails the
 * WHOLE map (return -1, the client keeps its phase-3 fallback world and
 * prints one FAILED marker) — a half-loaded map is a bug, not a degraded
 * mode. fs_game/homepath reach this file exactly the way cm_load.c reads
 * real .bsp files, so the engine FS is genuinely exercised for game data.
 */
#include <stdint.h>
#include <string.h>

#include "q3_map.h"

extern void write_serial_string(const char *s);

/* ====================================================================== */
/* Embedded fallback — identical text to assets/maps/mectov1.map          */
/* (keep in sync). Used when /ext2 carries no mectov1.map so the phase-4  */
/* path (FS load -> parse -> render from data) still runs on any boot.    */
/* ====================================================================== */
const char Q3MAP_EMBEDDED_MECTOV1[] =
    "# Mectov OS map — MCTBSP1 format 1 (text, brush-based; no pak0 assets).\n"
    "# Source of truth for Q3 phase 4: the identical text is embedded as\n"
    "# Q3MAP_EMBEDDED_MECTOV1 in third_party/q3mectov/q3_map.c as the fallback\n"
    "# when /ext2 carries no mectov1.map — keep the two in sync.\n"
    "#\n"
    "# Directives (whitespace-separated tokens; '#' starts a comment):\n"
    "#   map   <name>                       map name (serial marker + sanity)\n"
    "#   bounds <half-extent>               horizontal backstop clamp for movement\n"
    "#   spawn <x> <y> <z> <yaw> <pitch>    player start (yaw 180 faces -Y)\n"
    "#   brush <minx miny minz maxx maxy maxz r g b>   solid AABB, per-face shading\n"
    "#   bot   <x y z w d h r g b>          spinning/bobbing box, z = base elevation\n"
    "#\n"
    "# Layout (Quake units, z up): 960x960 play field inside 192-high perimeter\n"
    "# walls, four 320-high pillars at +-320, a 320x240 magenta platform south of\n"
    "# centre (the collision + colour proof for the phase-4 test), three bots.\n"
    "MCTBSP1 1\n"
    "map mectov1\n"
    "bounds 480\n"
    "spawn 0 390 26 180 0\n"
    "brush -480 -480 -40 480 480 0 0.230 0.223 0.208\n"
    "brush -496 -496 -16 496 -480 192 0.420 0.360 0.280\n"
    "brush -496 480 -16 496 496 192 0.420 0.360 0.280\n"
    "brush -496 -480 -16 -480 480 192 0.420 0.360 0.280\n"
    "brush 480 -480 -16 496 480 192 0.420 0.360 0.280\n"
    "brush -352 -352 -16 -288 -288 320 0.620 0.550 0.400\n"
    "brush 288 -352 -16 352 -288 320 0.620 0.550 0.400\n"
    "brush -352 288 -16 -288 352 320 0.620 0.550 0.400\n"
    "brush 288 288 -16 352 352 320 0.620 0.550 0.400\n"
    "brush -160 -256 -16 160 -16 48 0.780 0.180 0.780\n"
    "bot 60 140 0 48 48 72 0.950 0.450 0.100\n"
    "bot -140 60 0 48 48 72 0.200 0.850 0.250\n"
    "bot 80 -136 48 48 48 72 0.900 0.250 0.200\n";

/* ====================================================================== */
/* scanner                                                                 */
/* ====================================================================== */

typedef struct {
    const char *p;
} scanner_t;

static void sc_skip_ws_comments(scanner_t *s) {
    for (;;) {
        char c = *s->p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { s->p++; continue; }
        if (c == '#') {
            while (*s->p && *s->p != '\n') s->p++;
            continue;
        }
        return;
    }
}

/* Next token; NULL when the buffer ends. Tokens longer than 47 chars are
 * truncated (none of the format's tokens come close). */
static const char *sc_next(scanner_t *s) {
    static char tok[48];
    int n = 0;
    sc_skip_ws_comments(s);
    if (*s->p == '\0') return NULL;
    while (*s->p && *s->p != ' ' && *s->p != '\t' &&
           *s->p != '\r' && *s->p != '\n' && *s->p != '#') {
        if (n < (int)sizeof(tok) - 1) tok[n++] = *s->p;
        s->p++;
    }
    tok[n] = '\0';
    return tok;
}

/* Plain float parse (no libc strtof in this build): sign, integer part,
 * fraction part; rejects anything else. Empty fraction ("5.") is accepted. */
static int parse_float_tok(const char *c, float *out) {
    float v = 0.0f, sign = 1.0f, m;
    if (!c || !*c) return -1;
    if (*c == '-') { sign = -1.0f; c++; }
    else if (*c == '+') { c++; }
    if (*c == '.') {                       /* ".5" */
        m = 0.1f;
        for (c++; *c >= '0' && *c <= '9'; c++) { v += (float)(*c - '0') * m; m *= 0.1f; }
        *out = v * sign;
        return (*c == '\0') ? 0 : -1;
    }
    if (*c < '0' || *c > '9') return -1;
    while (*c >= '0' && *c <= '9') v = v * 10.0f + (float)(*c++ - '0');
    if (*c == '.') {
        m = 0.1f;
        for (c++; *c >= '0' && *c <= '9'; c++) { v += (float)(*c - '0') * m; m *= 0.1f; }
    }
    *out = v * sign;
    return (*c == '\0') ? 0 : -1;
}

static int sc_float(scanner_t *s, float *out) {
    return parse_float_tok(sc_next(s), out);
}

/* ====================================================================== */
/* load                                                                    */
/* ====================================================================== */

static q3map_t q3map_inst;
static int     q3map_have;

const q3map_t *q3map_current(void) {
    return q3map_have ? &q3map_inst : 0;
}

/* tiny decimal writer for the load markers (signed, like the client's) */
static void ser_num(int v) {
    static char buf[13];
    int n = 0;
    unsigned u;
    if (v < 0) { write_serial_string("-"); u = (unsigned)(-v); }
    else       { u = (unsigned)v; }
    if (u == 0) { write_serial_string("0"); return; }
    while (u && n < 12) { buf[n++] = (char)('0' + (u % 10)); u /= 10; }
    for (int i = 0; i < n / 2; i++) {
        char t = buf[i]; buf[i] = buf[n - 1 - i]; buf[n - 1 - i] = t;
    }
    buf[n] = '\0';
    write_serial_string(buf);
}

int q3map_load(const char *qpath) {
    char     *buf = 0;
    long      len;
    scanner_t sc;
    q3map_t   m;
    const char *tok;
    float     ver;

    memset(&m, 0, sizeof(m));
    q3map_have = 0;
    if (!qpath || !*qpath) return -1;

    len = FS_ReadFile(qpath, (void **)&buf);
    if (len <= 0 || !buf) {
        write_serial_string("[Q3CL] map load FAILED (FS: not found) path=");
        write_serial_string(qpath);
        write_serial_string("\n");
        return -1;
    }
    sc.p = buf;

    tok = sc_next(&sc);
    if (!tok || strcmp(tok, Q3MAP_MAGIC) != 0) {
        write_serial_string("[Q3CL] map load FAILED (bad magic)\n");
        FS_FreeFile(buf);
        return -1;
    }
    if (sc_float(&sc, &ver) != 0 || (int)ver != Q3MAP_VERSION) {
        write_serial_string("[Q3CL] map load FAILED (bad version)\n");
        FS_FreeFile(buf);
        return -1;
    }

    int ok = 1;
    while (ok) {
        const char *dir = sc_next(&sc);
        if (!dir) break;

        if (!strcmp(dir, "map")) {
            tok = sc_next(&sc);
            if (!tok) { ok = 0; break; }
            int i = 0;
            while (tok[i] && i < (int)sizeof(m.name) - 1) { m.name[i] = tok[i]; i++; }
            m.name[i] = '\0';
        } else if (!strcmp(dir, "bounds")) {
            if (sc_float(&sc, &m.bounds_half) != 0 || m.bounds_half <= 0.0f) ok = 0;
        } else if (!strcmp(dir, "spawn")) {
            for (int i = 0; i < 5 && ok; i++)
                if (sc_float(&sc, &m.spawn[i]) != 0) ok = 0;
        } else if (!strcmp(dir, "brush")) {
            float v[9];
            for (int i = 0; i < 9 && ok; i++)
                if (sc_float(&sc, &v[i]) != 0) ok = 0;
            if (ok) {
                if (m.n_brushes >= Q3MAP_MAX_BRUSHES) { ok = 0; break; }
                m.brushes[m.n_brushes].mins[0] = v[0];
                m.brushes[m.n_brushes].mins[1] = v[1];
                m.brushes[m.n_brushes].mins[2] = v[2];
                m.brushes[m.n_brushes].maxs[0] = v[3];
                m.brushes[m.n_brushes].maxs[1] = v[4];
                m.brushes[m.n_brushes].maxs[2] = v[5];
                m.brushes[m.n_brushes].rgb[0]  = v[6];
                m.brushes[m.n_brushes].rgb[1]  = v[7];
                m.brushes[m.n_brushes].rgb[2]  = v[8];
                if (v[0] > v[3] || v[1] > v[4] || v[2] > v[5])
                    ok = 0;                               /* inverted box */
                else
                    m.n_brushes++;
            }
        } else if (!strcmp(dir, "bot")) {
            float v[9];
            for (int i = 0; i < 9 && ok; i++)
                if (sc_float(&sc, &v[i]) != 0) ok = 0;
            if (ok) {
                if (m.n_bots >= Q3MAP_MAX_BOTS) { ok = 0; break; }
                m.bots[m.n_bots].x = v[0];
                m.bots[m.n_bots].y = v[1];
                m.bots[m.n_bots].z = v[2];
                m.bots[m.n_bots].w = v[3];
                m.bots[m.n_bots].d = v[4];
                m.bots[m.n_bots].h = v[5];
                m.bots[m.n_bots].rgb[0] = v[6];
                m.bots[m.n_bots].rgb[1] = v[7];
                m.bots[m.n_bots].rgb[2] = v[8];
                if (v[3] <= 0.0f || v[4] <= 0.0f || v[5] <= 0.0f)
                    ok = 0;
                else
                    m.n_bots++;
            }
        } else {
            ok = 0;   /* unknown directive: the format is ours, fail loudly */
        }
    }
    FS_FreeFile(buf);

    if (!ok || !m.name[0] || m.bounds_half <= 0.0f || m.n_brushes < 1) {
        write_serial_string("[Q3CL] map load FAILED (parse/validate)\n");
        return -1;
    }

    q3map_inst = m;
    q3map_have = 1;

    write_serial_string("[Q3CL] map loaded name=");
    write_serial_string(m.name);
    write_serial_string(" brushes=");
    ser_num(m.n_brushes);
    write_serial_string(" bots=");
    ser_num(m.n_bots);
    write_serial_string(" spawn=");
    ser_num((int)m.spawn[0]); write_serial_string(",");
    ser_num((int)m.spawn[1]); write_serial_string(",");
    ser_num((int)m.spawn[2]);
    write_serial_string(" yaw=");
    ser_num((int)m.spawn[3]);
    write_serial_string("\n");
    return 0;
}
