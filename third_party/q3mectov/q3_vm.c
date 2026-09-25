/* q3_vm.c — Quake III Arena, from id Software's OFFICIAL source (v38.107).
 *
 * Phase 5 of the Q3 port. Phases 1-4 built an engine out of a fork and hand-
 * rolled a game loop; this one runs the real thing:
 *
 *   - the engine core is compiled from third_party/q3a, the unmodified
 *     id-Software/Quake-III-Arena release (see its UPSTREAM.md);
 *   - qagame.qvm is real Quake III Arena game bytecode, produced from that
 *     same source by id's own tools (lcc + q3asm, scripts/build_qvm.sh);
 *   - it executes here through id's own QVM loader (qcommon/vm.c) and its
 *     portable bytecode interpreter (qcommon/vm_interpreted.c).
 *
 * Phase 6 goes further than init: the driver runs the retail server's own
 * sequence — CLIENT_CONNECT, CLIENT_USERINFO_CHANGED, CLIENT_BEGIN, then
 * GAME_RUN_FRAME + GAME_CLIENT_THINK per frame — so id's real gameplay code
 * executes, with a synthetic client walking through it. The trap_* calls
 * come back into the kernel through Q3VM_SystemCalls below, which is the
 * kernel-side twin of the retail engine's own SV_GameSystemCalls().
 *
 * The whole thing is staged like the phase-4 map (v38.104): the QVM travels
 * /ext2 -> tmpfs homepath, so the ENGINE'S filesystem — not a back door — is
 * what loads the module, exactly as it would load a real mod.
 *
 * Phase 7 (v38.107) gives the module a world. Until then G_TRACE answered from
 * a hand-written floor plane at z = 0, so "the official gameplay code runs" was
 * true but "in a level" was not: no brushes, no walls, no entity string from a
 * map. The kernel now compiles id's own collision model (cm_load/cm_trace/
 * cm_test/cm_patch/cm_polylib) and CM_LoadMap()s a real .bsp through the
 * engine's filesystem — the level's entity text reaches G_InitGame through
 * CM_EntityString(), and every trap_Trace is id's own brush trace.
 *
 * Design rule: never park on failure. Every step reports and returns, so a
 * broken step costs one FAILED marker instead of a dead machine.
 */
#include <stdint.h>
#include <string.h>

#include "../q3a/code/game/q_shared.h"
#include "../q3a/code/qcommon/qcommon.h"
#include "../q3a/code/qcommon/cm_public.h"
#include "../q3a/code/game/g_public.h"
/* id's VM-private header, for read-only inspection of what the loader built
 * (id's own qcommon/vm.c includes it exactly like this). */
#include "../q3a/code/qcommon/vm_local.h"
/* v38.107: the collision model's private header, included for the same reason —
 * to read back what CM_LoadMap actually built (cm.numBrushes, cm.numPlanes, …)
 * so the log reports the real thing instead of the port's opinion of it. */
#include "../q3a/code/qcommon/cm_local.h"

/* Mectov platform services */
extern void write_serial_string(const char *s);

/* Official engine core entry points (qcommon/common.c, qcommon/cmd.c) */
extern void Com_Printf(const char *fmt, ...);
extern int  Com_Milliseconds(void);

/* v38.108: the windowed path (`q3arena`) — the render mesh for the module's own
 * level, the TinyGL backend that draws it, and the WM window it presents into. */
#include "q3bsp.h"
#include "../tinygl/q3cl_render.h"
#include "../../src/include/wm.h"
#include "../../src/include/theme.h"

/* VFS (src/sys/vfs.c) */
extern int  vfs_mkdir(const char *path);
extern int  vfs_get_node(const char *path);
extern int  vfs_create_file(const char *path);
extern int  vfs_write_file(const char *path, const char *data, int size);
extern unsigned int vfs_get_file_size(int node);

/* ===== serial helpers (the rest of the port writes these by hand too) ==== */

/* reportf — one formatted line, one atomic write.
 *
 * The suite parses these lines out of the shared serial log with regexes, and
 * write_serial_string is atomic per CALL (it takes the serial lock with
 * interrupts off). A line assembled from a dozen calls is therefore a dozen
 * windows in which any other task's output can interleave — which happened for
 * real: a mesh line came out as "verts=.[LOAD] c0=0x00000044 296 ..." and the
 * suite's regex found nothing, so a green build failed on log shape alone.
 * Every line a test parses is now formatted whole into this buffer and written
 * in a single call; callers that truly stream (the engine's own Com_Printf
 * path, human-only lines) keep using write_serial_string directly.
 *
 * The buffer is static because the report lines are long (the mesh line runs
 * ~200 characters) and this driver's stacks are budgeted; all reportf call
 * sites are on one task's thread, serialized by construction.
 *
 * Formatting goes through q3_vsnprintf (the stub stdio.h's name for the port
 * shim that reaches the kernel's own vsnprintf), which is already in scope
 * here — declaring the kernel symbol directly under the name `vsnprintf` is
 * impossible in this TU, because the stub #defines that name to q3_vsnprintf
 * and the signatures disagree. */
static void reportf(const char *fmt, ...) {
    static char rbuf[512];
    va_list ap;
    va_start(ap, fmt);
    q3_vsnprintf(rbuf, (size_t)sizeof(rbuf), fmt, ap);
    va_end(ap);
    write_serial_string(rbuf);
}

/* Append a decimal int to dst (nul-terminated); returns chars written. The
 * hand-composed lines below (two hex-bearing ones and the symbol lines) use
 * this instead of reportf, because they mix formats reportf has no verb for. */
static int rpt_int(char *dst, int v) {
    static char nb[13];
    int n = 0, i = 0;
    unsigned u;
    if (v < 0) { dst[i++] = '-'; u = (unsigned)(-v); } else u = (unsigned)v;
    if (u == 0) nb[n++] = '0';
    while (u) { nb[n++] = (char)('0' + u % 10); u /= 10; }
    while (n) dst[i++] = nb[--n];
    dst[i] = '\0';
    return i;
}

/* vm_ofs/ps_ofs print as 0x%08x, and "0x%x" of a small value would regress the
 * suite's (0x[0-9a-f]+) match shape it has asserted since v38.106 — so the
 * zero-padded form stays available through the same one-write discipline. */
static void reportf_hex0(const char *label, unsigned int v) {
    static char hbuf[64];
    char hex[9];
    int i;
    for (i = 7; i >= 0; i--) { hex[i] = "0123456789abcdef"[v & 0xF]; v >>= 4; }
    hex[8] = '\0';
    for (i = 0; label[i] && i < (int)sizeof(hbuf) - 12; i++) hbuf[i] = label[i];
    hbuf[i] = '\0';
    for (int j = 0; j < 8 && i < (int)sizeof(hbuf) - 1; j++) hbuf[i++] = hex[j];
    hbuf[i] = '\0';
    write_serial_string(hbuf);
}

static void vm_ser_int(int v) {
    char buf[16];
    int n = 0, neg = 0;
    unsigned int u;
    if (v < 0) { neg = 1; u = (unsigned int)(-v); } else { u = (unsigned int)v; }
    if (u == 0) buf[n++] = '0';
    while (u) { buf[n++] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) buf[n++] = '-';
    for (int i = 0; i < n / 2; i++) { char t = buf[i]; buf[i] = buf[n - 1 - i]; buf[n - 1 - i] = t; }
    buf[n] = '\0';
    write_serial_string(buf);
}

static void vm_ser_hex(unsigned int v) {
    static const char d[] = "0123456789abcdef";
    char buf[12];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) buf[2 + i] = d[(v >> (28 - 4 * i)) & 0xF];
    buf[10] = '\0';
    write_serial_string(buf);
}

/* ===== where the game data lives ======================================== */

/* The ext2 volume is this OS's game disc, and it is told to the engine as
 * such: `+set fs_cdpath /ext2` plus the usual gamedir makes the engine's own
 * search path /ext2/baseq3, so FS_ReadFile("vm/qagame.qvm") opens
 * /ext2/baseq3/vm/qagame.qvm through Sys_FOpen -> the Mectov VFS.
 *
 * Nothing is copied first. Earlier phases staged the (1.5 KB) map file into
 * the tmpfs homepath, which is capped at 256 KB per file — a 470 KB module
 * cannot go there, and it should not: having id's filesystem read game data
 * off the volume is the point. */
#define Q3VM_QVM_PATH "/ext2/baseq3/vm/qagame.qvm"
#define Q3VM_MAP_PATH "/ext2/baseq3/vm/qagame.map"

/* The collision world (v38.107, Q3 phase 7).
 *
 * Until this release every trace the game module received came from
 * q3vm_trace_world() below: one infinite floor plane at z = 0 and nothing else,
 * which is why the player could only fall or walk on that single surface. The
 * kernel now compiles id's OWN collision model (qcommon/cm_load.c,
 * cm_trace.c, cm_test.c, cm_patch.c, cm_polylib.c — see the Makefile) and loads
 * a real .bsp through the engine's filesystem, so `trap_Trace` answers with
 * id's brushes, planes and surface flags.
 *
 * Two path forms, one file: the FS-relative name is what CM_LoadMap hands to
 * FS_ReadFile (search path /ext2/baseq3, see Sys_DefaultCDPath), and the /ext2
 * form is what the VFS can stat to tell whether the map exists at all.
 *
 * scripts/build_test_bsp.py generates mectovtest.bsp: our own arena, no id
 * assets, built so that "did the real loader run?" is answerable from this
 * log — its floor's top face is z = 64 (the fake world put the player at
 * exactly z = 24) and it is walled (the fake world returned fraction 1.0 for
 * every sideways trace). Before this phase that content did not have to exist
 * because there was nothing that could load it.
 *
 * A bare build with no game data still boots: the loader reports the missing
 * map and the old minimal world stays in place, so `q3vm` runs end to end on an
 * empty install instead of dying at G_InitGame. */
#define Q3VM_BSP_FS_PATH  "maps/mectovtest.bsp"
#define Q3VM_BSP_VFS_PATH "/ext2/baseq3/maps/mectovtest.bsp"

/* bg_public.h's "what stops a player" mask, spelled out here because this
 * translation unit deliberately does not include the bg layer; every constant
 * in it comes from q_shared.h. */
#define Q3VM_MASK_PLAYERSOLID \
    (CONTENTS_SOLID|CONTENTS_PLAYERCLIP|CONTENTS_BODY|CONTENTS_CORPSE)

static int q3vm_world_real = 0;         /* 1 = id's CM_LoadMap succeeded */
static int q3vm_world_checksum = 0;
/* The running VM, kept so the world probes can read the player's own
 * playerState out of its data segment (same cast q3vm_log_player_state does).
 * Set once, right after VM_Create. */
static vm_t *q3vm_vm = 0;

static int q3vm_file_bytes(const char *path) {
    int node = vfs_get_node(path);
    if (node < 0) return -1;
    return (int)vfs_get_file_size(node);
}

static void q3vm_report_data(void) {
    write_serial_string("[Q3VM] official Quake III Arena bytecode path (id-Software source)\n");
    write_serial_string("[Q3VM] qvm at " Q3VM_QVM_PATH " bytes=");
    vm_ser_int(q3vm_file_bytes(Q3VM_QVM_PATH));
    write_serial_string("\n");
    write_serial_string("[Q3VM] map at " Q3VM_MAP_PATH " bytes=");
    vm_ser_int(q3vm_file_bytes(Q3VM_MAP_PATH));
    write_serial_string("\n");
}

/* ===== the trap surface the game module calls back into ================= */

#define VMA(x)  VM_ArgPtr(args[x])

static int vm_syscalls;         /* total traps the module made */
static int vm_errors;           /* G_ERROR traps (game-side fatal conditions) */
static int vm_unhandled;        /* traps with no kernel handler yet */

/* The game's trap_LocateGameData hands us where the world lives in the VM's
 * data segment: the gentity array and the playerState array. The retail
 * server reads those pointers directly (that is the whole point of the trap);
 * so do we, to prove the module's world really moves. */
static int vm_gentity_ofs = -1;
static int vm_ps_ofs = -1;
static int vm_num_entities;
static int vm_sizeof_gentity;

/* The level time the driver feeds into each GAME_RUN_FRAME. G_GET_USERCMD
 * stamps the same time into the usercmd so the module's own command-time
 * gating (ClientThink_real: msec = serverTime - ps.commandTime) accepts it. */
static int q3vm_frame_time;

#define Q3VM_FRAMETIME  50              /* id's own FRAMETIME (g_local.h) */
#define Q3VM_FRAME_COUNT 200            /* 10 seconds of game time */

/* ===== soft-float math for the shared traps ============================= */
/* qcommon.h's sharedTraps_t: the lcc backend compiles the module's
 * sin/cos/atan2/sqrt into these traps instead of in-VM float code. The
 * kernel has no libm (it builds -msoft-float), so these are home-grown:
 * range reduction plus short series, ~1e-6 precision — plenty for id's
 * movement math. Floats travel as IEEE-754 bit patterns in the int args. */

static float bits_to_float(int i) { union { int i; float f; } u; u.i = i; return u.f; }
static int   float_to_bits(float f) { union { float f; int i; } u; u.f = f; return u.i; }

static float q3vm_fsqrt(float v) {
    float x;
    if (!(v > 0.0f)) return 0.0f;       /* 0 and NaN both map to 0 */
    x = (v > 1.0f) ? v : 1.0f;
    for (int i = 0; i < 32; i++) x = 0.5f * (x + v / x);
    return x;
}

static float q3vm_fsin(float x) {
    const float PI = 3.14159265358979f;
    const float TWO_PI = 6.28318530717959f;
    float sign, a, x2;
    if (!(x > -1e9f && x < 1e9f)) return 0.0f;  /* NaN or absurd */
    /* wrap to the nearest multiple of 2*pi, landing in [-pi, pi] */
    x = x - TWO_PI * (float)(int)(x * (1.0f / TWO_PI)
                                  + (x >= 0.0f ? 0.5f : -0.5f));
    if (x > PI)  x -= TWO_PI;
    if (x < -PI) x += TWO_PI;
    /* mirror so the series only ever sees |a| <= pi/2 */
    sign = (x < 0.0f) ? -1.0f : 1.0f;
    a = x * sign;
    if (a > PI * 0.5f) a = PI - a;
    x2 = a * a;
    return sign * (a * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f
          + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f
          + x2 * (-1.0f / 39916800.0f)))))));
}

static float q3vm_fcos(float x) {
    return q3vm_fsin(x + 1.57079632679490f);
}

static float q3vm_fatan(float z) {
    int neg = (z < 0.0f), big;
    float a, u2, r;
    if (!(z > -1e9f && z < 1e9f)) return 0.0f;
    a = neg ? -z : z;
    big = (a > 1.0f);
    if (big) a = 1.0f / a;
    /* atan(a) = 4*atan(u) after two half-angle steps u = a/(1+sqrt(1+a^2));
     * that keeps |u| <= 0.414, where a 5-term series is ~1e-5 accurate */
    a = a / (1.0f + q3vm_fsqrt(1.0f + a * a));
    a = a / (1.0f + q3vm_fsqrt(1.0f + a * a));
    u2 = a * a;
    r = a * (1.0f + u2 * (-1.0f / 3.0f + u2 * (1.0f / 5.0f
        + u2 * (-1.0f / 7.0f + u2 * (1.0f / 9.0f)))));
    r = 4.0f * r;
    if (big) r = 1.57079632679490f - r;
    return neg ? -r : r;
}

static float q3vm_fatan2(float y, float x) {
    const float PI = 3.14159265358979f;
    if (x == 0.0f) {
        if (y == 0.0f) return 0.0f;
        return (y > 0.0f) ? PI * 0.5f : -PI * 0.5f;
    }
    {
        float r = q3vm_fatan(y / x);
        if (x < 0.0f) r += (y >= 0.0f) ? PI : -PI;
        return r;
    }
}

/* ===== the world Pmove moves through ==================================== */
/* No .bsp is involved, so the collision world is the minimal one that keeps
 * id's own physics honest: a floor plane at z = 0 and nothing else. The
 * player box (mins[2] == -24) rests on it, walks on it, and falls onto it —
 * every one of those paths is id's bg_pmove.c running inside the bytecode. */

static void q3vm_trace_world(trace_t *tr, const vec3_t start,
                             const vec3_t mins, const vec3_t maxs,
                             const vec3_t end) {
    float startBottom = start[2] + mins[2];
    float endBottom   = end[2] + mins[2];
    (void)maxs;
    memset(tr, 0, sizeof(*tr));
    VectorSet(tr->plane.normal, 0.0f, 0.0f, 1.0f);
    if (startBottom >= 0.0f && endBottom < 0.0f) {
        /* the swept box crosses the floor plane: clip the move there */
        float frac = (0.0f - startBottom) / (endBottom - startBottom);
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        tr->fraction = frac;
        tr->endpos[0] = start[0] + (end[0] - start[0]) * frac;
        tr->endpos[1] = start[1] + (end[1] - start[1]) * frac;
        tr->endpos[2] = start[2] + (end[2] - start[2]) * frac;
        tr->plane.dist = 0.0f;
        tr->plane.type = PLANE_Z;
        tr->plane.signbits = 0;
        tr->contents = CONTENTS_SOLID;
        tr->entityNum = ENTITYNUM_WORLD;
        return;
    }
    tr->fraction = 1.0f;                /* empty air: nothing to hit */
    tr->endpos[0] = end[0];
    tr->endpos[1] = end[1];
    tr->endpos[2] = end[2];
    tr->entityNum = ENTITYNUM_NONE;
}

/* ===== loading the map through id's own loader ========================== */

/* A float with three decimals, by integer math — the kernel has no libm and
 * this is only ever used for log lines the test suite parses. The string form
 * (f3s) is what the one-write report lines consume; it rotates through eight
 * slots because a single line can carry five of these at once. */
static const char *f3s(float v) {
    static char fbuf[8][16];
    static int fidx;
    char *dst = fbuf[fidx++ & 7];
    int neg = 0, whole, frac, d;
    long s;
    int p = 0;
    if (v < 0.0f) { neg = 1; v = -v; }
    s = (long)(v * 1000.0f + 0.5f);
    whole = (int)(s / 1000);
    frac  = (int)(s % 1000);
    if (neg && (whole || frac)) dst[p++] = '-';
    {
        char digits[16];
        int n = 0;
        if (whole == 0) digits[n++] = '0';
        while (whole) { digits[n++] = (char)('0' + whole % 10); whole /= 10; }
        while (n) dst[p++] = digits[--n];
    }
    dst[p++] = '.';
    for (d = 100; d; d /= 10) dst[p++] = (char)('0' + (frac / d) % 10);
    dst[p] = '\0';
    return dst;
}

static void q3vm_world_load(void) {
    if (q3vm_file_bytes(Q3VM_BSP_VFS_PATH) <= 0) {
        write_serial_string("[Q3VM] world: no " Q3VM_BSP_VFS_PATH
                            " — minimal fallback (single floor plane at z=0)\n");
        return;
    }
    /* id's loader: reads the lump table, validates the version, hunks the
     * shaders/planes/brushes/leafs/nodes/models and the entity string. It
     * Com_Error()s on anything malformed, which is why the existence check
     * above happens first — a missing file is a clean report, a corrupt one is
     * id's own diagnostic. */
    CM_LoadMap(Q3VM_BSP_FS_PATH, qfalse, &q3vm_world_checksum);
    q3vm_world_real = 1;
    reportf("[Q3VM] world: CM_LoadMap(" Q3VM_BSP_FS_PATH
            ") shaders=%d planes=%d brushes=%d brushsides=%d nodes=%d leafs=%d models=%d",
            cm.numShaders, cm.numPlanes, cm.numBrushes, cm.numBrushSides,
            cm.numNodes, cm.numLeafs, cm.numSubModels);
    /* chars=%d checksum=0x%08x, composed into one buffer by hand: reportf has
     * no hex verb, and two writes would open the interleave window this exists
     * to close. */
    {
        static char ebuf[80];
        int i = 0;
        unsigned int c = (unsigned int)q3vm_world_checksum;
        for (const char *s = "[Q3VM] world: entity string chars="; *s; s++) ebuf[i++] = *s;
        i += rpt_int(ebuf + i, cm.numEntityChars);
        for (const char *s = " checksum=0x"; *s; s++) ebuf[i++] = *s;
        for (int k = 7; k >= 0; k--) ebuf[i++] = "0123456789abcdef"[(c >> (k * 4)) & 0xF];
        ebuf[i] = '\0';
        write_serial_string(ebuf);
    }
}

/* Where the module's own spawn point came from. The retail server hands the
 * game its level's entity text (SV_InitGameVM -> CM_EntityString); printing the
 * first info_player_deathmatch origin out of that text is the difference
 * between "the module walked its real spawn path" and "the port invented a
 * spawn at 0 0 24". */
static void q3vm_world_report_spawn(void) {
    const char *base, *p;
    char num[24];
    int n;
    if (!q3vm_world_real) return;  /* (origin line is one write, below) */
    base = CM_EntityString();
    p = strstr(base, "info_player_deathmatch");
    if (!p) {
        write_serial_string("[Q3VM] world: entity string has no info_player_deathmatch\n");
        return;
    }
    p = strstr(p, "origin");
    if (!p) return;
    p += 6;                                  /* past the key's own text */
    while (*p && *p != '"') p++;             /* the key's closing quote */
    if (*p) p++;                             /* step over it */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return;                   /* key with no quoted value */
    p++;
    for (n = 0; *p && *p != '"' && n < (int)sizeof(num) - 1; n++) num[n] = *p++;
    num[n] = '\0';
    reportf("[Q3VM] world: map spawn origin=[%s]", num);
}

/* Two probes whose answers only real brush collision can produce, both run
 * from where the module actually put the player:
 *
 *   down  the floor's TOP face is z = 64 in the generated arena, so a hit
 *         there is a number the old z = 0 plane could not have produced;
 *   -X    the -X wall's inner face is x = -512, and a sideways trace STOPS.
 *         The old world had one infinite floor plane and returned fraction
 *         1.0 (empty air) for every horizontal move.
 *
 * The suite asserts on these lines: "id's loader ran" is then checkable from
 * the log rather than by trusting the port. */
static void q3vm_world_probe(void) {
    trace_t tr;
    vec3_t start, endpos, zero;
    playerState_t *ps;
    if (!q3vm_world_real || vm_ps_ofs < 0) return;
    ps = (playerState_t *)(q3vm_vm->dataBase + (vm_ps_ofs & q3vm_vm->dataMask));
    VectorCopy(ps->origin, start);
    VectorClear(zero);

    reportf("[Q3VM] world: probe from (%d %d %d)",
            (int)start[0], (int)start[1], (int)start[2]);

    /* straight down */
    VectorCopy(start, endpos);
    endpos[2] -= 256.0f;
    CM_BoxTrace(&tr, start, endpos, zero, zero, 0, Q3VM_MASK_PLAYERSOLID, 0);
    reportf("[Q3VM] world: trace down fraction=%s endz=%s normal=(%s %s %s) contents=%d",
            f3s(tr.fraction), f3s(tr.endpos[2]),
            f3s(tr.plane.normal[0]), f3s(tr.plane.normal[1]), f3s(tr.plane.normal[2]),
            tr.contents);

    /* straight -X, into the wall */
    VectorCopy(start, endpos);
    endpos[0] -= 512.0f;
    CM_BoxTrace(&tr, start, endpos, zero, zero, 0, Q3VM_MASK_PLAYERSOLID, 0);
    reportf("[Q3VM] world: trace -x fraction=%s endx=%s normal=(%s %s %s) contents=%d",
            f3s(tr.fraction), f3s(tr.endpos[0]),
            f3s(tr.plane.normal[0]), f3s(tr.plane.normal[1]), f3s(tr.plane.normal[2]),
            tr.contents);

    /* what the player is standing on, straight from id's point-contents */
    {
        vec3_t feet;
        VectorCopy(start, feet);
        feet[2] -= 24.0f;
        reportf("[Q3VM] world: point contents at feet=%d", CM_PointContents(feet, 0));
    }
}

/* The usercmd the windowed loop builds (see q3vm_fill_usercmd). They live up
 * here because the trap surface below reads them, while the windowed driver at
 * the bottom of the file is what fills them in. */
static int   q3vm_cmd_live;                     /* 1 = windowed input in use */
static int   q3vm_cmd_forward, q3vm_cmd_right;  /* -127..127, this frame */
/* The angles this driver ASKS for, accumulated from input. Deliberately not the
 * module's ps.viewangles: the module adds its own delta_angles (seeded by
 * SetClientViewAngle at spawn) to whatever a command carries, so feeding the
 * viewangles back in counts that delta again on every frame and the view walks
 * itself away — which is exactly what happened here the first time. The
 * module's angles are used for the CAMERA, these for the COMMAND. */
static float q3vm_cmd_yaw, q3vm_cmd_pitch;

/* One synthetic player. serverTime carries the frame's level time; id's own
 * code clamps and resyncs anything else.
 *
 * Headless (`q3vm`) that is the whole command: full throttle forward, angles
 * left at zero — the exact command v38.107's assertions were written against,
 * so the log the regression suite checks cannot shift because a window was
 * added. Windowed (`q3arena`) the command carries what the WM delivered:
 * movement keys, plus the module's own viewangles with the user's pending
 * mouse/arrow deltas on top. */
static void q3vm_fill_usercmd(usercmd_t *cmd, int serverTime) {
    memset(cmd, 0, sizeof(*cmd));
    cmd->serverTime = serverTime;
    if (!q3vm_cmd_live) {
        cmd->forwardmove = 127;
        return;
    }
    cmd->forwardmove = (signed char)q3vm_cmd_forward;
    cmd->rightmove = (signed char)q3vm_cmd_right;
    cmd->angles[YAW] = ANGLE2SHORT(q3vm_cmd_yaw);
    cmd->angles[PITCH] = ANGLE2SHORT(q3vm_cmd_pitch);
    cmd->angles[ROLL] = 0;
}

/* The userinfo string the retail server would keep per client. "ip" is
 * localhost so ClientConnect skips password logic and marks the local client;
 * name feeds pers.netname ("Mectov entered the game"); team 0 = TEAM_FREE. */
static void q3vm_get_userinfo(char *dst, int size) {
    static const char ui[] = "\\ip\\localhost\\name\\Mectov\\team\\0";
    int n = 0;
    if (size > 0) {
        while (ui[n] && n < size - 1) { dst[n] = ui[n]; n++; }
        dst[n] = '\0';
    }
}

static void q3vm_log_server_command(int clientNum, const char *text) {
    reportf("[Q3VM] server command to %d: %s", clientNum, text);
}

/* ===== reading the module's world back out of the VM ==================== */
/* level.clients[0].ps sits at the offset the module handed us through
 * trap_LocateGameData, inside the same data segment the interpreter runs.
 * Casting it out is exactly what the retail server does — shared memory,
 * no interface — and the result is id's own playerState_t to inspect. */
static void q3vm_log_player_state(vm_t *vm, int frame, int levelTime) {
    playerState_t *ps;
    if (vm_ps_ofs < 0) return;
    ps = (playerState_t *)(vm->dataBase + (vm_ps_ofs & vm->dataMask));
    reportf("[Q3VM] frame %d t=%d origin=(%d,%d,%d) ground=%d velocity=(%d,%d,%d)",
            frame, levelTime,
            (int)ps->origin[0], (int)ps->origin[1], (int)ps->origin[2],
            ps->groundEntityNum,
            (int)ps->velocity[0], (int)ps->velocity[1], (int)ps->velocity[2]);
}

static int q3vm_ps_x(vm_t *vm) {
    playerState_t *ps;
    if (vm_ps_ofs < 0) return 0;
    ps = (playerState_t *)(vm->dataBase + (vm_ps_ofs & vm->dataMask));
    return (int)ps->origin[0];
}

int Q3VM_SystemCalls(int *args) {
    vm_syscalls++;
    switch (args[0]) {
    /* ---- VM-internal helper traps (g_syscalls.asm: memset/memcpy/strncpy)
     * The lcc-built module reaches libc through the same syscall door:
     * id 100 = memset, 101 = memcpy, 102 = strncpy. bg_lib.c ships in-VM
     * fallbacks, but lcc emits the trap form at many call sites, so without
     * these every memset/strncpy inside the module is a silent no-op — the
     * empty G_ERROR message in the first bring-up was exactly that. */
    case 100:
        memset(VMA(1), args[2], args[3]);
        return args[1];
    case 101:
        memcpy(VMA(1), VMA(2), args[3]);
        return args[1];
    case 102:
        strncpy((char *)VMA(1), (const char *)VMA(2), args[3]);
        if (args[3] > 0) ((char *)VMA(1))[args[3] - 1] = '\0';
        return args[1];

    /* ---- general Quake services: implemented for real ------------------ */
    case G_PRINT:
        Com_Printf("%s", (const char *)VMA(1));
        return 0;
    case G_ERROR:
        /* The retail engine makes this ERR_DROP (which unwinds the frame). We
         * have no unwinder in a kernel task, so the message is logged and the
         * call returns; the game module's own error paths then unwind. */
        vm_errors++;
        write_serial_string("[Q3VM] G_ERROR: ");
        write_serial_string((const char *)VMA(1));
        write_serial_string("\n");
        return 0;
    case G_MILLISECONDS:
        return Sys_Milliseconds();
    case G_CVAR_REGISTER:
        Cvar_Register((vmCvar_t *)VMA(1), (const char *)VMA(2), (const char *)VMA(3), args[4]);
        return 0;
    case G_CVAR_UPDATE:
        Cvar_Update((vmCvar_t *)VMA(1));
        return 0;
    case G_CVAR_SET:
        Cvar_Set((const char *)VMA(1), (const char *)VMA(2));
        return 0;
    case G_CVAR_VARIABLE_INTEGER_VALUE:
        return Cvar_VariableIntegerValue((const char *)VMA(1));
    case G_CVAR_VARIABLE_STRING_BUFFER:
        Cvar_VariableStringBuffer((const char *)VMA(1), (char *)VMA(2), args[3]);
        return 0;
    case G_ARGC:
        return Cmd_Argc();
    case G_ARGV:
        Cmd_ArgvBuffer(args[1], (char *)VMA(2), args[3]);
        return 0;
    case G_SEND_CONSOLE_COMMAND:
        Cbuf_ExecuteText(args[1], (const char *)VMA(2));
        return 0;

    /* ---- filesystem: the same calls the retail engine serves ----------- */
    case G_FS_FOPEN_FILE:
        return FS_FOpenFileByMode((const char *)VMA(1), (fileHandle_t *)VMA(2), (fsMode_t)args[3]);
    case G_FS_READ:
        FS_Read2(VMA(1), args[2], args[3]);
        return 0;
    case G_FS_WRITE:
        FS_Write(VMA(1), args[2], args[3]);
        return 0;
    case G_FS_FCLOSE_FILE:
        FS_FCloseFile(args[1]);
        return 0;
    case G_FS_GETFILELIST:
        return FS_GetFileList((const char *)VMA(1), (const char *)VMA(2), (char *)VMA(3), args[4]);
    case G_FS_SEEK:
        return FS_Seek(args[1], args[2], args[3]);

    case G_LOCATE_GAME_DATA:
        vm_gentity_ofs    = args[1];
        vm_num_entities   = args[2];
        vm_sizeof_gentity = args[3];
        vm_ps_ofs         = args[4];
        return 0;

    /* ---- the entity string: what the retail server gets from the .bsp ----
     * G_SpawnEntitiesFromString() tokenises the level's entity text through
     * G_GET_ENTITY_TOKEN, exactly like SV_InitGameVM does via
     * CM_EntityString(). With no .bsp we hand the module a minimal worldspawn
     * — the same thing a real map file begins with — so the module walks its
     * real spawn path instead of erroring out. COM_Parse over a static buffer
     * mirrors id's own sv.entityParsePoint mechanic. */
    case G_GET_ENTITY_TOKEN: {
        static char entbuf[] =
            "{ \"classname\" \"worldspawn\" }\n"
            "{ \"classname\" \"info_player_deathmatch\" "
            "\"origin\" \"0 0 24\" }\n";
        static char *entp = 0;
        const char *s;
        if (!entp) {
            /* v38.107: with a real map loaded this is the level's OWN entity
             * text — what SV_InitGameVM hands G_SpawnEntitiesFromString()
             * through CM_EntityString(). The literal below survives only for an
             * install that has no .bsp at all. */
            entp = q3vm_world_real ? CM_EntityString() : entbuf;
        }
        s = COM_Parse(&entp);
        Q_strncpyz((char *)VMA(1), s, args[2]);
        if (!entp && !s[0]) return 0;   /* end of spawn string */
        return 1;
    }

    /* ---- shared math traps (sharedTraps_t in qcommon.h) ----------------- */
    case TRAP_SIN:
        return float_to_bits(q3vm_fsin(bits_to_float(args[1])));
    case TRAP_COS:
        return float_to_bits(q3vm_fcos(bits_to_float(args[1])));
    case TRAP_ATAN2:
        return float_to_bits(q3vm_fatan2(bits_to_float(args[1]),
                                         bits_to_float(args[2])));
    case TRAP_SQRT:
        return float_to_bits(q3vm_fsqrt(bits_to_float(args[1])));

    /* botlib is not built; BotTestAAS() asks once per think and bails when
     * the answer is no — so say no, silently */
    case BOTLIB_AAS_INITIALIZED:
        return 0;

    /* ---- no level is loaded, so these are deliberately inert ----------- */
    /* They exist because GAME_INIT registers its world with the server. With
     * no .bsp (that needs pak0) there is nothing to link, trace or query; the
     * module is told so instead of being handed fake geometry. */
    case G_SET_CONFIGSTRING:
    case G_GET_CONFIGSTRING: {
        /* One backing table shared by SET and GET (a real server keeps
         * sv.configstrings; the game's own G_FindConfigstringIndex reads
         * strings back through G_GET_CONFIGSTRING). */
        static char cs[MAX_CONFIGSTRINGS][128];
        if (args[0] == G_SET_CONFIGSTRING) {
            if (args[1] >= 0 && args[1] < MAX_CONFIGSTRINGS)
                Q_strncpyz(cs[args[1]], (const char *)VMA(2), sizeof(cs[0]));
            return 0;
        }
        {
            const char *src = (args[1] >= 0 && args[1] < MAX_CONFIGSTRINGS)
                              ? cs[args[1]] : "";
            Q_strncpyz((char *)VMA(2), src, args[3]);
        }
        return 0;
    }
    case G_GET_SERVERINFO:
        if (args[2] > 0) ((char *)VMA(1))[0] = '\0';
        return 0;
    case G_TRACE:
    case G_TRACECAPSULE: {
        /* ( trace_t *results, start, mins, maxs, end, passEntityNum, contentMask )
         * — the argv order is g_syscalls.c's trap_Trace/trap_TraceCapsule.
         *
         * v38.107: id's OWN collision model answers this when a .bsp is
         * loaded, so the module gets real brushes, real planes and the real
         * content mask. The boxes are copied out of the VM first because
         * CM_BoxTrace takes non-const vec3_t and there is no reason for id's
         * code to write back into the module's data segment.
         *
         * passEntityNum is deliberately ignored: it exists to skip the
         * entities the trace is passing through, and the world model is the
         * only solid thing in this world. */
        trace_t *tr = (trace_t *)VMA(1);
        vec3_t start, endp, mins, maxs;
        VectorCopy((const float *)VMA(2), start);
        VectorCopy((const float *)VMA(3), mins);
        VectorCopy((const float *)VMA(4), maxs);
        VectorCopy((const float *)VMA(5), endp);
        if (q3vm_world_real) {
            CM_BoxTrace(tr, start, endp, mins, maxs, 0, args[7],
                        (args[0] == G_TRACECAPSULE) ? 1 : 0);
            /* CM_Trace never touches entityNum — the CALLER owns it. The
             * retail engine's SV_GameTrace sets it right after this same call,
             * and id's own code depends on it: q_shared.h documents
             * trace_t.entityNum as an entity number "or ENTITYNUM_NONE,
             * ENTITYNUM_WORLD", and bg_pmove.c's PM_AddTouchEnt returns early
             * for ENTITYNUM_WORLD precisely because the world is not a
             * touchable entity. Left unset, a world hit reported entity 0 — the
             * worldspawn gentity — so the player "stood on" entity 0 with
             * ps.groundEntityNum == 0 (v38.107 bring-up saw exactly that:
             * landed at the floor's z with ground=0). */
            tr->entityNum = (tr->fraction != 1.0f) ? ENTITYNUM_WORLD : ENTITYNUM_NONE;
        } else {
            q3vm_trace_world(tr, start, mins, maxs, endp);
        }
        return 0;
    }

    case G_GET_USERCMD:
        /* ( int clientNum, usercmd_t *cmd ) — the cmd pointer is args[2] */
        q3vm_fill_usercmd((usercmd_t *)VMA(2), q3vm_frame_time);
        return 0;
    case G_GET_USERINFO:
        q3vm_get_userinfo((char *)VMA(2), args[3]);
        return 0;
    case G_SET_USERINFO:
        return 0;                       /* the server owns userinfo; nothing to do */
    case G_SEND_SERVER_COMMAND:
        /* prints, scores, the "entered the game" line: all visible in the log */
        q3vm_log_server_command(args[1], (const char *)VMA(2));
        return 0;
    case G_DROP_CLIENT:
        write_serial_string("[Q3VM] drop client ");
        vm_ser_int(args[1]);
        write_serial_string(": ");
        write_serial_string((const char *)VMA(2));
        write_serial_string("\n");
        return 0;

    case G_SET_BRUSH_MODEL:
    case G_LINKENTITY:
    case G_UNLINKENTITY:
    case G_ADJUST_AREA_PORTAL_STATE:
    case G_BOT_FREE_CLIENT:
    case G_DEBUG_POLYGON_CREATE:
    case G_DEBUG_POLYGON_DELETE:
        return 0;
    case G_POINT_CONTENTS:
        /* ( const vec3_t point, int passEntityNum ) — id's own query when a
         * .bsp is loaded (v38.107); an empty world answers "nothing" (0). */
        if (q3vm_world_real)
            return CM_PointContents((const float *)VMA(1), 0);
        return 0;
    case G_IN_PVS:
    case G_IN_PVS_IGNORE_PORTALS:
    case G_AREAS_CONNECTED:
        return 0;                       /* empty world: everything is empty */
    case G_ENTITIES_IN_BOX:
    case G_ENTITY_CONTACT:
    case G_ENTITY_CONTACTCAPSULE:
        return 0;                       /* no entities are in any box */
    case G_BOT_ALLOCATE_CLIENT:
        return -1;                      /* no free client slots */
    case G_REAL_TIME:
        return Sys_Milliseconds();
    case G_SNAPVECTOR:
        Sys_SnapVector((float *)VMA(1));
        return 0;

    default:
        /* Bots and AAS live in botlib, which the kernel does not build. Log
         * once per trap number so the log stays readable and still shows
         * exactly what the module asked for. */
        vm_unhandled++;
        write_serial_string("[Q3VM] unhandled trap #");
        vm_ser_int(args[0]);
        write_serial_string(" args=");
        vm_ser_int(args[1]);
        write_serial_string(",");
        vm_ser_int(args[2]);
        write_serial_string("\n");
        return 0;
    }
}

/* ===== why the native-compiler half of the VM is absent ================ */
/* vm.c references these unconditionally; upstream ships them in vm_x86.c
 * (32-bit only), which compiles bytecode to native code at load time. That is
 * a pointless trade in a kernel that has no libc, no mmap and no signal
 * handling: VM_Create(..., VMI_BYTECODE) never takes that branch, so the
 * interpreter in vm_interpreted.c is the only engine that runs here. Upstream
 * itself stubs them out the same way under its DLL_ONLY builds. */
void VM_Compile(vm_t *vm, vmHeader_t *header) {
	(void)vm; (void)header;
	write_serial_string("[Q3VM] VM_Compile: native codegen is not built\n");
}
int VM_CallCompiled(vm_t *vm, int *args) {
	(void)vm; (void)args;
	write_serial_string("[Q3VM] VM_CallCompiled: native codegen is not built\n");
	return 0;
}

/* ===== the world on screen: the `q3arena` path (v38.108) =============== */
/* v38.108 takes the level the module has been running inside since v38.107 —
 * collision, entity string, spawn point — and draws it: the same .bsp's
 * surfaces through TinyGL, textured from the game data on the volume, with the
 * camera following the module's own playerState (origin + viewheight, forward
 * out of id's AngleVectors on ps.viewangles). What the window shows is
 * therefore not this port's idea of a level; it is the geometry the official
 * game code is standing in.
 *
 * Input travels the other way: WM scancodes and captured mouse motion become
 * the usercmd the module consumes in GAME_CLIENT_THINK, so id's own Pmove does
 * the moving. With no key pressed the driver holds forward — that keeps the
 * demo self-driving (and CI-assertable); the first key the window receives
 * hands control to the user.
 */
#define Q3ARENA_W          320
#define Q3ARENA_H          240
#define Q3ARENA_FRAMES     300          /* 15 s of game time at FRAMETIME */
#define Q3ARENA_WALL_MS    150000       /* stop regardless, so CI cannot hang */
#define Q3ARENA_TURN_DEG   130.0f       /* arrow-key turn rate, per second */
#define Q3ARENA_SENS       0.18f        /* mouse degrees per pixel */

#define SC_ESC   0x01
#define SC_W     0x11
#define SC_A     0x1E
#define SC_S     0x1F
#define SC_D     0x20
#define SC_SPACE 0x39
#define SC_UP    0x48
#define SC_LEFT  0x4B
#define SC_RIGHT 0x4D
#define SC_DOWN  0x50

static int   a_win = -1;                /* the window, -1 = none */
static int   a_quit;                    /* set by ESC / window close */
static int   a_input_seen;              /* a key has reached the window */
static int   a_frames;                  /* frames rendered */
static int   a_faces_drawn, a_tris_drawn;   /* totals across the run */
static unsigned char a_keys[128];       /* scancode -> held (no 0x80 bit) */

static unsigned a_next_ms;              /* when the next frame is due */
static unsigned a_start_ms;             /* when the frame loop began */
static q3bsp_mesh_t a_mesh;

static playerState_t *q3vm_ps(vm_t *vm) {
    if (!vm || vm_ps_ofs < 0) return 0;
    return (playerState_t *)(vm->dataBase + (vm_ps_ofs & vm->dataMask));
}

extern int get_win_index(int wid);

static void q3arena_win_draw(int id, int cx, int cy, int cw, int ch) {
    int idx = get_win_index(id);
    (void)cx; (void)cy;
    if (idx < 0) return;
    if (wm_wins[idx].resizing) return;
    if (!wm_wins[idx].content_buffer) return;
    q3ref_blit(wm_wins[idx].content_buffer, cw, ch);
}

/* The WM delivers raw scancodes to this window (wm_request_scancodes), press
 * and release alike, which is the only way to hold a movement key. */
static void q3arena_win_key(int id, char c, uint8_t sc) {
    uint8_t base = sc & 0x7F;
    int down = !(sc & 0x80);
    (void)id; (void)c;
    if (base >= 128) return;
    a_keys[base] = (unsigned char)down;
    a_input_seen = 1;
    if (base == SC_ESC && down) a_quit = 1;
}

/* Relative motion while this window owns the capture, exactly like the client
 * layer's mouse path. Flushed by the next usercmd. */
static void q3arena_win_mouse(int id, int dx, int dy, int btn) {
    (void)id; (void)btn;
    if (wm_capture_owner() != a_win) return;
    a_input_seen = 1;
    q3vm_cmd_yaw   -= (float)dx * Q3ARENA_SENS;
    q3vm_cmd_pitch -= (float)dy * Q3ARENA_SENS;
    if (q3vm_cmd_pitch > 85.0f) q3vm_cmd_pitch = 85.0f;
    if (q3vm_cmd_pitch < -85.0f) q3vm_cmd_pitch = -85.0f;
}

/* Load the render mesh for the map the collision world came from and hand it
 * to the renderer, which decodes its textures. Failing here is not fatal: the
 * renderer falls back to its built-in arena, which is what a build with no
 * game data on the volume must still be able to show. */
static void q3arena_world_mesh(void) {
    int rc = q3bsp_load(Q3VM_BSP_FS_PATH, &a_mesh);
    if (rc != 0 || !a_mesh.valid) {
        reportf("[Q3ARENA] world mesh: FAILED to build from " Q3VM_BSP_FS_PATH
                " (rc=%d) — falling back to the built-in arena", rc);
        return;
    }
    /* One line, one write: the suite's MESH_RE parses every field of this, and
     * this was the line that died of interleaving ("verts=.[LOAD] c0=..."). */
    reportf("[Q3ARENA] world mesh: " Q3VM_BSP_FS_PATH
            " surfaces=%d of=%d verts=%d shaders=%d planar=%d patches=%d "
            "patchdrawn=%d patchquads=%d patchverts=%d patchskipped=%d "
            "skipped=%d truncated=%d",
            a_mesh.numFaces, a_mesh.fileSurfaces, a_mesh.numVerts,
            a_mesh.numShaders, a_mesh.planarFaces, a_mesh.patchSurfaces,
            a_mesh.patchesDrawn, a_mesh.patchQuads, a_mesh.patchVerts,
            a_mesh.patchSkipped, a_mesh.skippedFaces, a_mesh.truncated);
    q3ref_set_bsp(&a_mesh);
}

static void q3arena_open(void) {
    int ww, wh, wx, wy;
    extern uint32_t fb_width, fb_height;

    if (q3ref_init(Q3ARENA_W, Q3ARENA_H) != 0) {
        write_serial_string("[Q3ARENA] FATAL: TinyGL renderer init failed\n");
        return;
    }
    write_serial_string("[Q3ARENA] TinyGL renderer ready w=");
    vm_ser_int(Q3ARENA_W);
    write_serial_string(" h=");
    vm_ser_int(Q3ARENA_H);
    write_serial_string("\n");

    ww = Q3ARENA_W + 2;
    wh = Q3ARENA_H + TITLEBAR_H + 2;
    wx = ((int)fb_width - ww) / 2;  if (wx < 0) wx = 0;
    wy = ((int)fb_height - TASKBAR_H_PX - wh) / 2; if (wy < 0) wy = 0;

    /* The title says what this is on purpose: id's game module's own level,
     * drawn by this port — not the retail game. */
    a_win = wm_open(wx, wy, ww, wh, "Quake III — official qagame VM",
                    q3arena_win_draw, q3arena_win_key, NULL, q3arena_win_mouse);
    if (a_win < 0) {
        write_serial_string("[Q3ARENA] FATAL: could not open WM window\n");
        q3ref_shutdown();
        return;
    }
    /* The rect is logged because the test has to know where the window ended
     * up: the WM owns placement, and a screendump assertion that guesses the
     * geometry silently measures the desktop instead (which is exactly how the
     * first version of this test passed a violet count it should not have). */
    reportf("[Q3ARENA] window id=0x%x rect=%d,%d %dx%d content=%dx%d "
            "title=\"Quake III — official qagame VM\"",
            (unsigned)a_win, wx, wy, ww, wh, Q3ARENA_W, Q3ARENA_H);

    wm_request_scancodes(a_win, 1);
    if (wm_capture_mouse(a_win, 1)) {
        int cx = 0, cy = 0;
        wm_capture_center(&cx, &cy);
        write_serial_string("[Q3ARENA] mouse captured (WASD/arrows move, mouse looks, ESC quits)\n");
    } else {
        write_serial_string("[Q3ARENA] WARNING: mouse capture refused\n");
    }

    q3arena_world_mesh();
}

static void q3arena_close(void) {
    if (a_win >= 0) {
        wm_capture_mouse(a_win, 0);
        wm_request_scancodes(a_win, 0);
        wm_close(a_win);
        a_win = -1;
    }
    q3ref_shutdown();
}

/* One frame: hand the module this frame's command, let it run its own frame,
 * then draw the world from the playerState it produced. */
static void q3arena_frame(vm_t *vm, int frame) {
    playerState_t *ps;
    vec3_t eye, fwd;
    int drawn = 0, tris = 0, culled = 0;
    int shaders = 0, from_disk = 0, ph = 0;
    int cyan = 0, warm = 0, stepgreen = 0, violet = 0, bright = 0, sky = 0;
    int patch = 0;
    int distinct = 0;
    unsigned target = a_next_ms;

    /* --- input -> usercmd ------------------------------------------------ */
    {
        int fm = 0, rm = 0;
        if (a_keys[SC_W] || a_keys[SC_UP]) fm += 127;
        if (a_keys[SC_S] || a_keys[SC_DOWN]) fm -= 127;
        if (a_keys[SC_D]) rm += 127;
        if (a_keys[SC_A]) rm -= 127;
        if (!a_input_seen) fm = 127;    /* self-driving demo until a key lands */
        if (a_keys[SC_LEFT])  q3vm_cmd_yaw += Q3ARENA_TURN_DEG * ((float)Q3VM_FRAMETIME / 1000.0f);
        if (a_keys[SC_RIGHT]) q3vm_cmd_yaw -= Q3ARENA_TURN_DEG * ((float)Q3VM_FRAMETIME / 1000.0f);
        while (q3vm_cmd_yaw > 180.0f)  q3vm_cmd_yaw -= 360.0f;
        while (q3vm_cmd_yaw < -180.0f) q3vm_cmd_yaw += 360.0f;
        q3vm_cmd_forward = fm;
        q3vm_cmd_right = rm;
    }

    q3vm_frame_time += Q3VM_FRAMETIME;
    VM_Call(vm, GAME_RUN_FRAME, q3vm_frame_time);
    VM_Call(vm, GAME_CLIENT_THINK, 0);

    ps = q3vm_ps(vm);

    /* --- the module's viewpoint is the camera ---------------------------- */
    if (ps) {
        VectorCopy(ps->origin, eye);
        eye[2] += (float)ps->viewheight;
        AngleVectors(ps->viewangles, fwd, NULL, NULL);
        q3ref_set_camera_basis(eye, fwd);
    }
    q3ref_begin_frame();
    q3ref_draw_world((double)q3vm_frame_time / 1000.0);
    q3ref_end_frame();
    q3ref_bsp_stats(&drawn, &tris, &culled, &shaders, &from_disk, &ph);
    q3ref_frame_histogram(&cyan, &warm, &stepgreen, &violet, &bright, &patch,
                          &sky, &distinct);
    if (a_win >= 0) wm_invalidate(a_win);

    a_frames++;
    a_faces_drawn += drawn;
    a_tris_drawn += tris;

    if ((frame % 20) == 0) {
        if (ps) {
            reportf("[Q3ARENA] frame=%d t=%d pos=(%d,%d,%d) eye_z=%d yaw=%d "
                    "pitch=%d drawn=%d tris=%d culled=%d",
                    frame, q3vm_frame_time,
                    (int)ps->origin[0], (int)ps->origin[1], (int)ps->origin[2],
                    (int)eye[2], (int)ps->viewangles[YAW], (int)ps->viewangles[PITCH],
                    drawn, tris, culled);
        } else {
            reportf("[Q3ARENA] frame=%d t=%d drawn=%d tris=%d culled=%d",
                    frame, q3vm_frame_time, drawn, tris, culled);
        }

        /* The finished frame, read back out of the renderer's own buffer: one
         * line per sampled frame that says what the camera actually saw. */
        reportf("[Q3ARENA] pixels frame=%d cyan=%d warm=%d stepgreen=%d "
                "violet=%d bright=%d patch=%d sky=%d distinct=%d",
                frame, cyan, warm, stepgreen, violet, bright, patch, sky,
                distinct);
    }

    /* Pace to the module's own frame time (~20 fps) instead of spinning, so the
     * desktop keeps its timeslice and the run stays watchable in TCG. A frame
     * that overran its budget drops the deficit rather than catching up. */
    a_next_ms += (unsigned)Q3VM_FRAMETIME;
    for (;;) {
        unsigned now = (unsigned)Com_Milliseconds();
        if (now >= target || a_quit) break;
        __asm__ __volatile__("hlt");
    }
    {
        unsigned now = (unsigned)Com_Milliseconds();
        if (now > a_next_ms + 250u) a_next_ms = now;
    }
    (void)shaders; (void)from_disk; (void)ph;
    if (a_win >= 0 && wm_is_open(a_win) == 0) {
        write_serial_string("[Q3ARENA] window closed\n");
        a_quit = 1;
    }
}

/* ===== driver ========================================================== */

static void q3vm_park(void) {
    /* A forked kernel task entry must never return (v38.99). */
    for (;;) __asm__ __volatile__("hlt");
}

static void q3_drive(int windowed) {
    static char cmdline[176];
    static const char args[] =
        "+set dedicated 1 "        /* server-style core: no client, no GL */
        "+set com_hunkMegs 20 "    /* the VM needs data+code+ipointers (~4.5MB) */
        "+set com_zoneMegs 6 "
        "+set com_maxfps 0 "
        "+set com_busyWait 1 "
        "+set developer 1 "        /* vm.c gates vm/qagame.map on `developer` */
        "+set fs_game baseq3 "
        "+set fs_cdpath /ext2";   /* the volume the game data lives on */

    for (int i = 0; i < (int)sizeof(args); i++) cmdline[i] = args[i];

    /* id's FS_InitFilesystem() fatals out ("Couldn't load default.cfg") unless
     * a default.cfg is reachable from the search path. Homepath is tmpfs, so
     * the engine's own config I/O stays RAM-resident and never touches ATA
     * (v38.99), and this is the same file q3/q3play stage. */
    vfs_mkdir("/tmp/q3home");
    vfs_mkdir("/tmp/q3home/baseq3");
    if (vfs_create_file("/tmp/q3home/baseq3/default.cfg") >= 0)
        vfs_write_file("/tmp/q3home/baseq3/default.cfg",
                       "// mectov q3vm cfg\n", 19);

    q3vm_report_data();

    /* A missing qvm is a clean, reportable failure — not a reason to die. */
    if (q3vm_file_bytes(Q3VM_QVM_PATH) <= 0) {
        write_serial_string("[Q3VM] FAILED: no qagame.qvm at " Q3VM_QVM_PATH
                            " (scripts/build_qvm.sh + seed_ext2.sh)\n");
        q3vm_park();
    }	Com_Init(cmdline);
	write_serial_string("[Q3VM] Com_Init done\n");

	/* VM_LoadSymbols() bails unless com_developer is set. That cvar is named
	 * "developer" (common.c: com_developer = Cvar_Get("developer", ...)), and
	 * id ties symbol loading to it, so make sure it is on before the loader
	 * runs — belt and braces on top of the +set above. */
	Cvar_Set("developer", "1");

    /* v38.107: load the collision world through id's own loader. It has to
     * happen before the module runs — G_InitGame walks the level's entity
     * string during GAME_INIT, and ClientSpawn needs the brushes by
     * CLIENT_BEGIN. */
    q3vm_world_load();
    q3vm_world_report_spawn();

    /* Windowed only: the renderer, the window and the level's textures come up
     * before the module runs, so GAME_INIT and the connect sequence happen with
     * the world already drawable. A failure here (no renderer, no window) is
     * reported and the session continues headless rather than dying. */
    if (windowed) {
        write_serial_string("[Q3ARENA] official qagame VM world rendered through "
                            "TinyGL (id Software source)\n");
        q3arena_open();
        q3vm_cmd_live = 1;
    }

    /* id's loader: reads vm/qagame.qvm through the engine FS, validates the
     * VM_MAGIC header, allocates the data/code segments on the hunk and
     * prepares the bytecode for the interpreter. */
    vm_t *vm = VM_Create("qagame", Q3VM_SystemCalls, VMI_BYTECODE);
    if (!vm) {
        write_serial_string("[Q3VM] FAILED: VM_Create returned NULL\n");
        q3vm_park();
    }

    q3vm_vm = vm;
    reportf("[Q3VM] vm created name=qagame interpret=bytecode symbols=%d "
            "codeLength=%d dataMask=%d",
            vm->numSymbols, vm->codeLength, vm->dataMask);

    /* Print a couple of real symbol names out of id's own .map file, so the
     * log proves the symbol table came across. */
    {
        int names = 0;
        for (vmSymbol_t *s = vm->symbols; s && names < 3; s = s->next, names++) {
            static char sbuf[96];
            int i = 0;
            for (const char *t = "[Q3VM] symbol 0x"; *t; t++) sbuf[i++] = *t;
            {
                unsigned int v = (unsigned int)s->symValue;
                for (int k = 7; k >= 0; k--)
                    sbuf[i++] = "0123456789abcdef"[(v >> (k * 4)) & 0xF];
            }
            sbuf[i++] = ' ';
            for (int k = 0; s->symName[k] && i < (int)sizeof(sbuf) - 2; k++)
                sbuf[i++] = s->symName[k];
            sbuf[i++] = '\n';
            sbuf[i] = '\0';
            write_serial_string(sbuf);
        }
    }

    /* Let the engine report the VM it just built, through its own console
     * command — same output a retail server prints for `vminfo`. */
    Cbuf_ExecuteText(EXEC_NOW, "vminfo\n");

    /* Bytecode really executing: GAME_INIT runs G_InitGame(), which walks the
     * game module's cvar table, registers its world with the server and prints
     * through trap_Printf -> our serial log. Same call the retail engine makes
     * (sv_game.c: VM_Call( gvm, GAME_INIT, svs.time, Com_Milliseconds(), restart )). */
    write_serial_string("[Q3VM] calling vmMain(GAME_INIT) — official id game code\n");
    int before = vm_syscalls;
    int r = VM_Call(vm, GAME_INIT, Com_Milliseconds(), Com_Milliseconds(), 0);
    reportf("[Q3VM] GAME_INIT returned %d traps=%d total=%d errors=%d unhandled=%d",
            r, vm_syscalls - before, vm_syscalls, vm_errors, vm_unhandled);

    reportf("[Q3VM] locate_game_data entities=%d sizeof_gentity=%d",
            vm_num_entities, vm_sizeof_gentity);
    /* vm_ofs printed as one line (0x%08x appended by hand — reportf's verbs do
     * not include hex) with ps_ofs riding the same buffer, so the suite's
     * LOCATE_RE keeps matching a single, uninterleavable line. */
    {
        static char lbuf[112];
        int i = 0;
        unsigned int g = (unsigned int)vm_gentity_ofs;
        for (const char *s = "[Q3VM] locate_game_data entities="; *s; s++) lbuf[i++] = *s;
        i += rpt_int(lbuf + i, vm_num_entities);
        for (const char *s = " sizeof_gentity="; *s; s++) lbuf[i++] = *s;
        i += rpt_int(lbuf + i, vm_sizeof_gentity);
        for (const char *s = " vm_ofs=0x"; *s; s++) lbuf[i++] = *s;
        for (int k = 7; k >= 0; k--) lbuf[i++] = "0123456789abcdef"[(g >> (k * 4)) & 0xF];
        for (const char *s = " ps_ofs=0x"; *s; s++) lbuf[i++] = *s;
        for (int k = 7; k >= 0; k--)
            lbuf[i++] = "0123456789abcdef"[(((unsigned int)vm_ps_ofs) >> (k * 4)) & 0xF];
        lbuf[i] = '\0';
        write_serial_string(lbuf);
    }

    /* ---- the retail server's connect sequence (sv_client.c order) -------
     * ClientConnect reads userinfo (name "Mectov", ip localhost),
     * ClientUserinfoChanged propagates it into pers.netname, ClientBegin
     * spawns the player at the deathmatch spot and prints the classic
     * "Mectov entered the game" through trap_SendServerCommand. */
    write_serial_string("[Q3VM] client 0: GAME_CLIENT_CONNECT\n");
    VM_Call(vm, GAME_CLIENT_CONNECT, 0, qtrue, 0);
    write_serial_string("[Q3VM] client 0: GAME_CLIENT_USERINFO_CHANGED\n");
    VM_Call(vm, GAME_CLIENT_USERINFO_CHANGED, 0);
    write_serial_string("[Q3VM] client 0: GAME_CLIENT_BEGIN\n");
    VM_Call(vm, GAME_CLIENT_BEGIN, 0);

    /* Where the module decided to put the player, straight out of its own
     * playerState (frame 0 = the spawn instant, before the loop advances
     * time). Neither of these numbers is the port's to choose any more: the
     * origin comes from the map's info_player_deathmatch and the ground from
     * id's brushes. Then the two probes, whose answers only real collision
     * can produce (see q3vm_world_probe). */
    write_serial_string("[Q3VM] player spawn:\n");
    q3vm_log_player_state(vm, 0, 0);
    q3vm_world_probe();

    /* ---- the game loop: id's real G_RunFrame + ClientThink --------------
     * The same two vmMain calls the retail server frame makes
     * (SV_Frame -> SV_GameFrame: GAME_RUN_FRAME then per-client
     * GAME_CLIENT_THINK). Level time advances FRAMETIME per frame — the
     * server owns the clock, exactly like upstream. Each think hands the
     * module that frame's usercmd, so id's own bg_pmove.c moves the player:
     * headless that is full throttle forward (the v38.107 regression, byte for
     * byte), while `q3arena` feeds the window's input and renders every frame
     * it runs. */
    {
        int frame;
        int x0 = q3vm_ps_x(vm), x1;
        unsigned t_start = (unsigned)Com_Milliseconds();

        reportf("[Q3VM] frame loop: %d frames x %d msec",
                windowed ? Q3ARENA_FRAMES : Q3VM_FRAME_COUNT, Q3VM_FRAMETIME);
        q3vm_frame_time = 0;

    if (windowed) {
        a_start_ms = t_start;
        a_next_ms = t_start;
            for (frame = 1; frame <= Q3ARENA_FRAMES; frame++) {
                q3arena_frame(vm, frame);
                if (a_quit) break;
                if ((unsigned)Com_Milliseconds() - t_start > Q3ARENA_WALL_MS) {
                    write_serial_string("[Q3ARENA] wall-clock budget reached\n");
                    break;
                }
            }
        } else {
            for (frame = 1; frame <= Q3VM_FRAME_COUNT; frame++) {
                q3vm_frame_time += Q3VM_FRAMETIME;
                VM_Call(vm, GAME_RUN_FRAME, q3vm_frame_time);
                VM_Call(vm, GAME_CLIENT_THINK, 0);
                if (frame == 1) x0 = q3vm_ps_x(vm);
                if (frame % 20 == 0) q3vm_log_player_state(vm, frame, q3vm_frame_time);
            }
        }
        x1 = q3vm_ps_x(vm);
        reportf("[Q3VM] movement x0=%d x1=%d delta=%d", x0, x1, x1 - x0);

        if (windowed) {
            reportf("[Q3ARENA] render totals: frames=%d faces=%d tris=%d wall_ms=%d",
                    a_frames, a_faces_drawn, a_tris_drawn,
                    (int)((unsigned)Com_Milliseconds() - t_start));
        }
        write_serial_string("[Q3VM] frame loop done\n");
    }

    /* Windowed teardown happens before the module is shut down, exactly like
     * the client layer: the WM window and the renderer go away while the
     * module's world is still alive, so nothing can be drawn from a freed VM. */
    if (windowed) {
        q3arena_close();
        q3bsp_free(&a_mesh);
        write_serial_string("[Q3ARENA] done\n");
    }

    /* id's own shut-down order (sv_main.c SV_ShutdownGameProgs): the module
     * gets GAME_SHUTDOWN before its VM goes away. */
    VM_Call(vm, GAME_SHUTDOWN, qfalse);
    write_serial_string("[Q3VM] GAME_SHUTDOWN done\n");

    VM_Free(vm);
    write_serial_string("[Q3VM] done\n");
    q3vm_park();
}

/* ---- the two entry points the shell commands fork into ---------------- */
/* `q3vm` (v38.105) stays headless: the evidence for the VM milestone is the
 * serial log, and CI's regression for it must not depend on a window.
 * `q3arena` (v38.108) is the same session with the level drawn. */
void q3vm_start(void)   { q3_drive(0); }
void q3arena_start(void) { q3_drive(1); }
