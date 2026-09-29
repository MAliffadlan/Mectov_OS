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
/* v38.121: front-to-back face sort (q3world_render.h pulls q3bsp.h, whose
 * include path this unit does not carry — the two externs are declared here
 * instead). */
extern void q3w_set_sort(int on);
extern void q3w_sort_stats(int *enabled, int *pool, int *sorted);
#include "../../src/include/wm.h"
#include "../../src/include/theme.h"

/* v38.119: the exclusive fullscreen present path (src/drivers/vga.c). Declared
 * here rather than by including vga.h: this TU also pulls in id's engine
 * headers, and vga.h's globals (cx, cy, cur_col) collide with them. */
extern void vga_fullscreen_enter(void);
extern void vga_fullscreen_leave(void);
extern int  vga_fullscreen_active(void);

/* v38.119: the microsecond profile clock (src/drivers/timer.c). Declared here
 * for the same reason as the vga_* trio above — timer.h pulls in kernel types
 * this TU already has other names for — and it is one symbol. */
extern uint32_t timer_get_us(void);

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
 * empty install instead of dying at G_InitGame.
 *
 * v38.111: WHICH map is the map arg's business now. cmd_q3arena validates a
 * [a-z0-9_] name from the command line and hands it to this task through its
 * launch_arg; an empty arg keeps the generated test arena, so every existing
 * regression and CI run behaves exactly as before. The two strings below are
 * built at run time from it. */
static char q3vm_bsp_name[64]  = "mectovtest";   /* without maps/ and .bsp */
static char q3vm_bsp_fs[80]    = "maps/mectovtest.bsp";
static char q3vm_bsp_vfs[112]  = "/ext2/baseq3/maps/mectovtest.bsp";

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
int q3vm_frame_time;   /* non-static for the v38.117 [BF] diagnostic */

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
    if (q3vm_file_bytes(q3vm_bsp_vfs) <= 0) {
        write_serial_string("[Q3VM] world: no ");
        write_serial_string(q3vm_bsp_vfs);
        write_serial_string(" — minimal fallback (single floor plane at z=0)\n");
        return;
    }
    /* id's loader: reads the lump table, validates the version, hunks the
     * shaders/planes/brushes/leafs/nodes/models and the entity string. It
     * Com_Error()s on anything malformed, which is why the existence check
     * above happens first — a missing file is a clean report, a corrupt one is
     * id's own diagnostic. */
    CM_LoadMap(q3vm_bsp_fs, qfalse, &q3vm_world_checksum);
    q3vm_world_real = 1;
    reportf("[Q3VM] world: CM_LoadMap(%s) shaders=%d planes=%d brushes=%d "
            "brushsides=%d nodes=%d leafs=%d models=%d",
            q3vm_bsp_fs, cm.numShaders, cm.numPlanes, cm.numBrushes,
            cm.numBrushSides, cm.numNodes, cm.numLeafs, cm.numSubModels);
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
/* v38.122: the vertical and button channels (upmove/BUTTON_ATTACK), filled
 * from the window's key/mouse state or the scheduled jump=/fire= knobs.
 * Declared here, above q3vm_fill_usercmd, which writes them into the cmd. */
static int   a_up;                      /* -127..127, this frame */
static int   a_btn;                     /* cmd.buttons, this frame */
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
    /* v38.122: the vertical and button channels the driver never used to
     * fill. Pmove id: upmove >= 10 while grounded = jump (gate PMF_JUMP_HELD
     * forces a release first), upmove < 0 = duck; buttons & BUTTON_ATTACK
     * (=1, q_shared.h's first button bit) in PM_Weapon = fire (ammo-- then
     * EV_FIRE_WEAPON, which the server turns into FireWeapon). Headless q3vm
     * never gets here (upmove/buttons stay 0, so that suite's log shape is
     * unchanged). */
    cmd->upmove = (signed char)a_up;
    cmd->buttons = (unsigned char)a_btn;
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
/* v38.116: the cap is in RENDER frames now, and the render clock is free-run
 * (server ticks stay 50 ms of game time each, but several can land in one
 * rendered frame). 10800 frames at ~60 fps is the same ~180 s of play the old
 * 3600-frame cap gave at 20 fps; CI ends sessions long before this either way. */
#define Q3ARENA_FRAMES     10800        /* ~3 min of play; ESC ends sooner */
/* v38.116: minimum wall time per RENDERED frame (com_maxfps-style limiter).
 * 15 ms ~= a 66 fps ceiling; without it a free-running loop would starve the
 * desktop's timeslice — the old code spent this wait on the 20 Hz server tick
 * instead, which is exactly what capped fps at 20. */
#define RENDER_MIN_MS      15
#define Q3ARENA_WALL_MS    600000       /* stop regardless, so CI cannot hang */
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
#define SC_C     0x2E    /* v38.122: duck (upmove < 0) */
#define SC_LCTRL 0x1D    /* v38.122: attack hold (fire alias) */
#define SC_F3    0x3D    /* v38.126: perf detail panel (default off) */

static int   a_win = -1;                /* the window, -1 = none */
static int   a_quit;                    /* set by ESC / window close */
/* v38.119 test/driver knobs, all from the `q3arena` argument (see
 * q3vm_apply_map_arg): the camera can be PINNED at an exact pose so a heavy
 * view is reproducible frame for frame (the walk the bench drives is not), the
 * world-space frustum clip can be turned off for an A/B on one ISO, and the
 * present path can be taken over (fullscreen). */
static int   a_fullscreen;
static int   a_sort = 1;        /* v38.121: front-to-back face sort (nosort=off) */
/* v38.122: gameplay input. a_up feeds cmd.upmove (127 = jump intent, -127 =
 * duck), a_btn feeds cmd.buttons (BUTTON_ATTACK). The two jump=/fire= knobs
 * schedule the same inputs from the command line so CI can assert id's own
 * Pmove/FireWeapon chain without injecting keys; a_jump_pending/a_fire_left
 * are the scheduled actions' remaining state. a_shots/a_jumps count INTENTS
 * for the play line; the module's own reaction is read out of ps. */
static int   a_jump_left;               /* scheduled jumps remaining  */
static int   a_fire_left;               /* scheduled fire bursts left */
static int   a_fire_t;                  /* frames left in the current burst */
static unsigned a_jump_at;              /* frame the next jump fires on  */
static unsigned a_fire_at;              /* frame the next burst starts on */
/* v38.122: bg_public.h's values, spelled out — g_public.h does not pull that
 * header, and the numbers are stable across every Q3A derivative.
 *   BUTTON_ATTACK = 1      (q_shared.h: first usercmd button bit)
 *   WP_MACHINEGUN = 2      (bg_public.h weapon enum, after WP_NONE/GAUNTLET)
 *   STAT_HEALTH   = 0      (bg_public.h statIndex_t)
 *   ENTITYNUM_NONE= 1023   (bg_public.h; what ps.groundEntityNum reads while
 *                           airborne — the world entity is 0) */
#define Q3VM_BUTTON_ATTACK   1
#define Q3VM_WP_MACHINEGUN   2
#define Q3VM_STAT_HEALTH     0
static int   a_jumps, a_shots;          /* intent counters (diag line) */
static int   vm_firing_diag;            /* v38.123: STICKY latch — any tick that ran the fire path; sampled diag line reports it (never cleared) */
static unsigned a_jump_seen;            /* frames a jump intent was held */
static unsigned a_fire_seen;            /* frames BUTTON_ATTACK was held */
static int   a_jump_gap = 40;           /* frames between scheduled jumps  */
static int   a_fire_gap = 20;           /* frames between scheduled bursts */
static int   a_jump_t;                  /* the current jump's tick phase  */
static unsigned a_jump_tick0;           /* tick count when the intent was raised */
static int   a_pose_set;
static float a_pose_x, a_pose_y, a_pose_z, a_pose_yaw, a_pose_pitch;
static int   a_input_seen;              /* a key has reached the window */
static int   a_frames;                  /* frames rendered */
static int   a_faces_drawn, a_tris_drawn;   /* totals across the run */
static unsigned char a_keys[128];       /* scancode -> held (no 0x80 bit) */

static unsigned a_next_ms;              /* when the next frame is due */
static unsigned a_start_ms;             /* when the frame loop began */
static q3bsp_mesh_t a_mesh;

/* ---- v38.110 perf accounting -------------------------------------------
 * Where a frame's wall time goes: the module (two VM_Call), the software
 * renderer (begin_frame..end_frame), everything else the driver does
 * (histogram, camera, pacing remainder = idle), plus — read out of the WM —
 * what the compositor charged to this window (app draw = q3ref_blit + HUD,
 * and the content-buffer blit into the desktop's back buffer) and its whole
 * wm_draw_all pass.
 *
 * The clock is the kernel's millisecond tick (get_ticks), NOT rdtsc: the
 * first version converted rdtsc to microseconds, and under QEMU TCG the
 * virtual TSC between timer interrupts is garbage — phase deltas came out
 * negative, or seconds-large for a one-second window (vm_us=209,322,519),
 * because translated blocks advance the TSC in irregular leaps. The tick
 * counter is IRQ-driven and monotonic — exactly the property a phase meter
 * needs — and 1 ms resolution is ample for a 50 ms frame budget.
 *
 * No calibration, no 64-bit divide (which the freestanding kernel cannot
 * even link: __udivdi3), and structurally incapable of the busy-wait-under-
 * IF=0 deadlock the WM's first calibration had: reading the clock never
 * waits. */
extern uint32_t get_ticks(void);
/* Pacing clock: get_ticks() at 10 ms is exactly the resolution a 15 ms frame
 * budget wants, and every existing repro of "46 fps, not 66" is a statement
 * about THIS clock, so pacing stays on it. */
static int a_ms(void) { return (int)(unsigned)get_ticks(); }
static int a_ms_since(int t0) { return a_ms() - t0; }

/* v38.119: the PROFILE clock, in microseconds. Every *_ms field below used to
 * be the difference of two get_ticks() samples, so any phase cheaper than one
 * tick read as zero unless it happened to straddle a boundary: `blit_ms=0
 * draw_ms=0 wm_ms=0` in window after window was not "the compositor is free",
 * it was a resolution artefact, and it hid the answer to the only question that
 * mattered (where the 25 ms frame goes). timer_get_us() = tick ms + latched PIT
 * count, so a 0.4 ms phase measures as 0.4 ms. Accumulated in us, printed in
 * ms; a negative delta (PIT latch racing the tick rollover) is clamped, never
 * added. */
static int a_us(void) { return (int)(unsigned)timer_get_us(); }
static int a_us_since(int t0) { int d = a_us() - t0; return d > 0 ? d : 0; }
static int a_us_vm, a_us_gl, a_us_other, a_us_idle;
/* v38.116 sub-profile of gl_ms: where the render phase's time actually sits
 * (viewport+clear / the world draw itself / end-of-frame present snapshot).
 * Printed only in the deep-profile line so the perf line stays parseable. */
static int a_us_gl_begin, a_us_gl_draw, a_us_gl_end;
/* v38.119: the parts of "other" that are not the GL phase. `o_stats` is the
 * per-frame counter readback, `o_hist` the every-20th-frame pixel histogram,
 * `o_hud` the perf overlay painted into the finished frame, `o_pres` the
 * snapshot the compositor blits. They exist because "other_ms" was 25% of the
 * frame and named nothing. */
static int a_us_o_stats, a_us_o_hist, a_us_o_hud, a_us_o_pres;
/* v38.116 server clock catch-up state: game time advances in 50 ms ticks
 * decoupled from the render loop (see q3arena_frame). */
static unsigned a_srv_last_ms;          /* wall time of the last server tick */
static int a_srv_acc;                   /* accumulated remainder, msec */
static int a_srv_ticks;                 /* ticks the camera has not consumed yet */
static int a_srv_ticks_win;             /* v38.118: ticks since the last sample line */
static unsigned a_srv_ticks_total;      /* v38.122: every tick ever run (jump intent release) */
static int a_srv_ms;                    /* real ms those ticks represent */
static int a_last_glfps;                /* render-only instantaneous fps */
static int    a_perf_frames;            /* frames included in the accounting */
static int    a_last_fps;               /* whole-run fps (the perf line's `fps=`) */
static int    a_last_fps_win;           /* v38.126: the fps the HUD shows — this
                                         * window's, not the run average      */
static unsigned a_win_ms;               /* v38.126: start of the fps window    */
static int    a_perf_detail;            /* v38.126: F3 -> the six-number panel */

/* v38.118: camera interpolation. The server still simulates on its 50 ms tick
 * (Pmove untouched), but the RENDERED camera must not teleport once per tick:
 * at 44 fps that is 2-3 frames of frozen pose, then a ~16-unit jump - the
 * stutter the user measures as "patah-patah saat gerak, semua map". The frame
 * camera is lerped between the last two tick-end poses with the same frac-
 *tional phase the server accumulator holds, so the eye moves EVERY rendered
 * frame while G_RunFrame's tick grid stays exactly what a 20 Hz server makes. */
static vec3_t a_cam_prev_pos;           /* pose at the previous tick end */
static vec3_t a_cam_cur_pos;            /* pose at the latest tick end */
static float  a_cam_prev_yaw, a_cam_prev_pitch;
static float  a_cam_cur_yaw,  a_cam_cur_pitch;
static int    a_cam_have;               /* pose pairs collected */
static int    a_last_cam_alpha;         /* for the sample log line */

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
    /* v38.122: the vertical and fire channels. SPACE = jump intent (Pmove
     * wants upmove >= 10 while grounded, and its PMF_JUMP_HELD gate requires
     * a real release before the next jump — holding SPACE auto-hops at id's
     * own cadence). C = duck (upmove < 0, PMF_DUCKED). Left-CTRL = fire alias
     * for keyboards without a mouse; the mouse button is the primary fire.
     * The driver holds NOTHING back: it just reflects key state, exactly like
     * a real client's in_*.c would. */
    if (base == SC_SPACE)      a_up = down ?  127 : 0;
    else if (base == SC_C)     a_up = down ? -127 : 0;
    else if (base == SC_LCTRL) {
        if (down) a_btn |=  Q3VM_BUTTON_ATTACK;
        else      a_btn &= ~Q3VM_BUTTON_ATTACK;
    }
    /* v38.126: F3 brings the six-number perf panel back (it is what the user
     * boxed and asked to lose, kept as an instrument, hidden by default). The
     * corner fps readout is always on; this only adds the detail. F3 reaches
     * this handler untouched: with a game window holding the raw-scancode
     * focus the kernel consumes only F12 (GDB stub), Ctrl+C/Z and Ctrl+Alt+L. */
    if (base == SC_F3 && down) {
        a_perf_detail = !a_perf_detail;
        q3ref_set_perf_detail(a_perf_detail);
        /* write_serial_string, not reportf: this runs in the compositor's
         * context (the WM calls the key handler), while reportf's scratch
         * buffer belongs to the frame task — the ESC branch below writes the
         * same way for the same reason. */
        write_serial_string("[Q3ARENA] perf detail ");
        write_serial_string(a_perf_detail ? "on (F3)\n" : "off (F3)\n");
    }
    if (base == SC_ESC && down) {
        /* v38.119: in fullscreen the first ESC hands the screen back to the
         * desktop and keeps playing in the window (the same window, the same
         * mouse capture); the next ESC quits, exactly as it did before. */
        if (a_fullscreen) {
            a_fullscreen = 0;
            vga_fullscreen_leave();
            write_serial_string("[Q3ARENA] fullscreen off (ESC)\n");
        } else {
            a_quit = 1;
        }
    }
}

/* Relative motion while this window owns the capture, exactly like the client
 * layer's mouse path. Flushed by the next usercmd. */
static void q3arena_win_mouse(int id, int dx, int dy, int btn) {
    (void)id;
    /* v38.122: button bit 0 = left = BUTTON_ATTACK, press and release alike
     * (the kernel main loop only calls wm_capture_event when the button state
     * CHANGES or the pointer moved, so this is edge-correct). */
    if (btn & 1) a_btn |=  Q3VM_BUTTON_ATTACK;
    else         a_btn &= ~Q3VM_BUTTON_ATTACK;
    if (wm_capture_owner() != a_win) return;
    a_input_seen = 1;
    q3vm_cmd_yaw   -= (float)dx * Q3ARENA_SENS;
    /* v38.118 sign fix: the driver feeds id's REAL qagame, whose pitch is
     * positive-DOWN (AngleVectors: forward[2] = -sin(pitch)), and the mouse
     * driver delivers +dy for downward motion (mouse.c: "PS/2 +y is up;
     * screen space is +y down"). Down-mouse must therefore ADD to pitch —
     * the old minus made every look direction vertical-inverted, exactly
     * the "mouse ke bawah, gamenya nengok ke atas" the user reported. */
    q3vm_cmd_pitch += (float)dy * Q3ARENA_SENS;
    if (q3vm_cmd_pitch > 85.0f) q3vm_cmd_pitch = 85.0f;
    if (q3vm_cmd_pitch < -85.0f) q3vm_cmd_pitch = -85.0f;
}

/* Load the render mesh for the map the collision world came from and hand it
 * to the renderer, which decodes its textures. Failing here is not fatal: the
 * renderer falls back to its built-in arena, which is what a build with no
 * game data on the volume must still be able to show. */
static void q3arena_world_mesh(void) {
    int rc = q3bsp_load(q3vm_bsp_fs, &a_mesh);
    if (rc != 0 || !a_mesh.valid) {
        reportf("[Q3ARENA] world mesh: FAILED to build from %s (rc=%d) — "
                "falling back to the built-in arena", q3vm_bsp_fs, rc);
        return;
    }
    /* One line, one write: the suite's MESH_RE parses every field of this, and
     * this was the line that died of interleaving ("verts=.[LOAD] c0=..."). */
    reportf("[Q3ARENA] world mesh: %s surfaces=%d of=%d verts=%d shaders=%d "
            "planar=%d patches=%d patchdrawn=%d patchquads=%d patchverts=%d "
            "patchskipped=%d skipped=%d truncated=%d",
            q3vm_bsp_fs,
            a_mesh.numFaces, a_mesh.fileSurfaces, a_mesh.numVerts,
            a_mesh.numShaders, a_mesh.planarFaces, a_mesh.patchSurfaces,
            a_mesh.patchesDrawn, a_mesh.patchQuads, a_mesh.patchVerts,
            a_mesh.patchSkipped, a_mesh.skippedFaces, a_mesh.truncated);
    /* v38.114: how much of that mesh the level's own lightmaps light. Printed
     * for every map, fixture included — a fixture that says lit=0 unlit=74 is
     * the release's proof that the sun fallback is still what a map with no
     * lightmap lump gets. */
    reportf("[Q3ARENA] lightmap faces: pages=%d dim=%d lit=%d unlit=%d "
            "dropped=%d",
            a_mesh.numLightmaps, a_mesh.lightmapDim, a_mesh.facesLit,
            a_mesh.facesUnlit, a_mesh.lightmapDropped);
    q3ref_set_bsp(&a_mesh);

    /* v38.125: what the map's shader scripts asked of the draw pass. `add`
     * counts definitions with a GL_ONE GL_ONE stage (id draws those in a pass
     * of its own — `sort additive`) and `culloff` those with `cull none` (fire
     * and lava are two-sided). Both are read from the scripts, not the BSP, so
     * they are also the proof that the scripts were parsed for more than image
     * names. A fixture arena's scripts ask for neither: additive=0 culloff=0 is
     * that suite's "nothing changed" reading. */
    {
        int f_add = 0, f_cull = 0;
        q3ref_draw_flag_stats(&f_add, &f_cull, NULL, NULL);
        reportf("[Q3ARENA] draw flags: additive=%d culloff=%d", f_add, f_cull);
    }
}

/* v38.124: id's own weapon view model, loaded ONCE, on the first frame the
 * module has a playerState — the weapon in it is what names the model, so the
 * choice is id's own state rather than a constant in this driver. A load of 0
 * parts (no pak0 staged, or a weapon with no row in the table) leaves the
 * v38.123 silhouette as the fallback, and q3viewmodel.c logs which of the two
 * it is: the fake gun was only ever a problem while it was indistinguishable
 * from the real thing.
 *
 * Called before the frame's clock starts: the file reads and the two JPEG
 * decodes are a one-off ~10 ms and must not land inside a measured phase. */
static int a_vm_tried;

static void q3arena_viewmodel(vm_t *vm) {
    playerState_t *ps;

    if (a_vm_tried) return;
    ps = q3vm_ps(vm);
    if (!ps) return;
    a_vm_tried = 1;

    /* bg_public.h's weapon_t: WP_MACHINEGUN = 2. This port arms exactly one
     * weapon (ammo_mg is the only ammo it manages), so the table has exactly
     * one row; anything else loads nothing and draws the fallback, and says
     * which weapon it did not have a model for. The path has no extension:
     * q3viewmodel.c adds .md3, _barrel.md3 and _flash.md3 itself. */
    if ((int)ps->weapon == 2) {
        q3ref_viewmodel_load("models/weapons2/machinegun/machinegun");
    } else {
        reportf("[Q3ARENA] viewmodel: weapon=%d has no staged view model path "
                "- procedural fallback", (int)ps->weapon);
    }
}

static void q3arena_open(void) {
    int ww, wh, wx, wy;
    extern uint32_t fb_width, fb_height;
#ifndef Q3ARENA_SCALE
#define Q3ARENA_SCALE 1
#endif

    if (q3ref_init(Q3ARENA_W, Q3ARENA_H) != 0) {
        write_serial_string("[Q3ARENA] FATAL: TinyGL renderer init failed\n");
        return;
    }
    write_serial_string("[Q3ARENA] TinyGL renderer ready w=");
    vm_ser_int(Q3ARENA_W);
    write_serial_string(" h=");
    vm_ser_int(Q3ARENA_H);
    write_serial_string("\n");

    /* v38.113: integer upscale at blit time, DOOM-style — the 3D pass keeps
     * the cheap 320x240 (the fps must not move), but the window and the copy
     * to it are Q3ARENA_SCALE x bigger, so on a 1024-wide desktop the game
     * fills 2x more screen at the same render cost. */
#if Q3ARENA_SCALE > 1
    q3ref_set_blit_scale(Q3ARENA_SCALE);
    q3w_set_sort(a_sort);   /* v38.121: `nosort` restores the v38.120 order */
#endif
    ww = Q3ARENA_W * Q3ARENA_SCALE + 2;
    wh = Q3ARENA_H * Q3ARENA_SCALE + TITLEBAR_H + 2;
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
    /* v38.110: opt this window into the compositor's perf accounting, so the
     * breakdown can separate the game window's share (app draw + content
     * blit) from the rest of the desktop's composite pass. */
    wm_tag_q3_game(a_win);
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

    /* v38.119: take the present path over if the argument asked for it. The
     * window stays (it owns the input routes); what stops is the desktop
     * compositing, which is one core's worth of work the game needs more. */
    if (a_fullscreen) {
        vga_fullscreen_enter();
        write_serial_string("[Q3ARENA] fullscreen: direct present (ESC returns to the window)\n");
    }

    q3arena_world_mesh();
}

static void q3arena_close(void) {
    if (vga_fullscreen_active()) vga_fullscreen_leave();
    if (a_win >= 0) {
        wm_capture_mouse(a_win, 0);
        wm_request_scancodes(a_win, 0);
        wm_close(a_win);
        a_win = -1;
    }
    q3ref_shutdown();
}

/* One frame: hand the module this frame's command, let it run its own frame,
 * then draw the world from the playerState it produced. Every phase is timed
 * in microseconds (v38.110) and folded into a window-1s rolling sum that the
 * sampled frames report as one atomic perf line. */
static void q3arena_frame(vm_t *vm, int frame) {
    playerState_t *ps;
    vec3_t eye, fwd;
    int drawn = 0, tris = 0, culled = 0;
    int shaders = 0, from_disk = 0, ph = 0;
    /* v38.112: the map's own visibility data, per sampled frame. -1 marks
     * mean "no PVS in this map": every face was drawn. */
    int vis_marked = -1, vis_total = 0, vis_cluster = -1, vis_leafs = 0;
    int cull_pvs = 0, cull_frustum = 0, cull_planes = 0, cull_back = 0;
    int cyan = 0, warm = 0, stepgreen = 0, violet = 0, bright = 0, sky = 0;
    int wall = 0;
    int patch = 0;
    int distinct = 0;
    unsigned target = a_next_ms;

    /* v38.124: the view model loads before this frame's clock starts. */
    q3arena_viewmodel(vm);

    int f_t0 = a_us();
    /* v38.116: the pixel histogram and the culling counters are read once per
     * SAMPLED frame (frame % 20), not every frame — on q3dm1 the histogram
     * alone was 7.4 ms of "other" per frame at 20 fps, i.e. ~1.5 fps of pure
     * bookkeeping. Locals now keep their LAST sampled values so the lines
     * emitted in the sample block stay byte-identical with the old shape. */
    int sample = (frame % 20) == 0;

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
    /* v38.122: scheduled jump=/fire= actions (the suite's hands). Jump holds
     * the intent for ONE game tick then releases and waits out its gap —
     * PMF_JUMP_HELD needs a real release before the next jump counts. Fire is
     * a BURST: 20 frames of BUTTON_ATTACK, then released until the next burst
     * comes due — PM_Weapon's weaponTime paces the shots at id's own 100 ms
     * machinegun cadence. Both only ever ADD intent; real key state is
     * applied on top of it afterwards. */
    /* Jump: the intent is raised on a rendered frame and held until one GAME
     * TICK has consumed it (a_srv_ticks_total moves), then released. Holding
     * for a fixed number of RENDERED frames loses jumps under the v38.116
     * catch-up — most rendered frames run zero ticks, so a one-frame intent
     * usually expired unseen. The hold is safe because PMF_JUMP_HELD blocks a
     * second jump until the release. a_jumps counts CONFIRMED jumps (a tick
     * took the intent), not raises. */
    if (a_jump_t && a_srv_ticks_total != a_jump_tick0) {
        playerState_t *pj = q3vm_ps(vm);
        a_jump_t = 0;
        a_up = 0;                       /* the hold is over */
        a_jump_left--;
        a_jumps++;
        a_jump_at = frame + (unsigned)a_jump_gap;
        /* The tick that consumed the intent has run, so ps is post-jump
         * RIGHT NOW: velocity[2] = JUMP_VELOCITY (270), ground = ENTITYNUM_NONE.
         * Sampling this through the every-100th-frame play line races the
         * ~23-frame airtime; this one-shot line is the suite's guaranteed
         * evidence that PM_CheckJump ran. */
        if (pj)
            reportf("[Q3ARENA] jump hit frame=%d vel_z=%d ground=%d jumps=%d",
                    frame, (int)pj->velocity[2], (int)pj->groundEntityNum,
                    a_jumps);
    }
    /* v38.125: a SCHEDULED jump waits for the ground. The frame grid says when
     * the driver wants to jump; Pmove says whether it can, and the two come
     * apart whenever the tick feed starves — on a slow host the rendered frames
     * race ahead of the game clock, the spawn drop has not finished by the
     * frame the grid names, and the intent is then consumed by a tick in
     * mid-air where PM_CheckJump refuses (it fires only for
     * groundEntityNum != ENTITYNUM_NONE). v38.124 called that a "jump hit" and
     * the suite read vel_z=-239 for a jump that never happened. Waiting for the
     * floor here is what a player does with the key and makes the evidence
     * honest at any frame rate: the tick that consumes the intent is a tick in
     * which PM_CheckJump can actually run. 1023 is ENTITYNUM_NONE. */
    if (a_jump_left && !a_jump_t && frame >= a_jump_at) {
        playerState_t *pg = q3vm_ps(vm);
        if (pg && pg->groundEntityNum != 1023) {
            a_up = 127;
            a_jump_seen++;
            a_jump_t = 1;
            a_jump_tick0 = a_srv_ticks_total;
        }
    }
    if (a_fire_left) {
        if (frame >= a_fire_at) {
            a_btn |= Q3VM_BUTTON_ATTACK;
            a_fire_seen++;
            a_shots++;
            if (++a_fire_t >= 20) {     /* the burst is done */
                a_fire_t = 0;
                a_fire_left--;
                a_fire_at = frame + (unsigned)a_fire_gap;
                if (!a_fire_left)       /* the LAST burst: hand off the button */
                    a_btn &= ~Q3VM_BUTTON_ATTACK;
            }
        } else {
            a_btn &= ~Q3VM_BUTTON_ATTACK;
        }
    }
    /* v38.123: the sampled diag line CANNOT catch the flash on its own grid —
     * it prints every 100th rendered frame while a burst holds BUTTON_ATTACK
     * for only 20 — so the driver latches "a tick ran the fire path" into a
     * sticky flag the line always reports (weaponstate returns to WEAPON_READY
     * between bursts, but the latch proves the state machine got there). The
     * enum lives in bg_public.h (TORSO_ATTACK=7, not in this unit's includes);
     * ps.torsoAnim is the module's own record of having played the attack. */
    {
        playerState_t *pf = q3vm_ps(vm);
        if (pf) {
            if (pf->weaponstate == 3) vm_firing_diag = 1;
            else if ((pf->torsoAnim & ~128/*ANIM_TOGGLEBIT, bg_public.h:541*/) == 7)
                vm_firing_diag = 1;
        }
    }
    /* Real key state lands AFTER the scheduler, so a human overrides the
     * knobs: SPACE held keeps the intent up even when no jump is scheduled,
     * and a released SPACE clears it (the scheduler's one-tick hold already
     * reset a_up itself). */
    if (a_keys[SC_SPACE])  a_up = 127;
    else if (a_keys[SC_C]) a_up = -127;
    else if (!a_jump_t) a_up = a_keys[SC_C] ? -127 : 0;

    /* v38.116 server catch-up: game time is still 50 ms per tick and paced by
     * the WALL clock — G_RunFrame sees exactly the tick grid a 20 Hz server
     * produces, so Pmove's physics are unchanged — but 0..2 ticks now run per
     * RENDERED frame, so a fast renderer is no longer chained to the module's
     * tick. A stall caps at 125 ms of catch-up (2 ticks) rather than spiralling. */
    {
        unsigned now = (unsigned)Com_Milliseconds();
        int dt = (int)(now - a_srv_last_ms);
        a_srv_last_ms = now;
        a_srv_acc += dt;
        if (a_srv_acc > 125) a_srv_acc = 125;
        while (a_srv_acc >= Q3VM_FRAMETIME) {
            int t0;
            a_srv_acc -= Q3VM_FRAMETIME;
            q3vm_frame_time += Q3VM_FRAMETIME;
            t0 = a_us();
            VM_Call(vm, GAME_RUN_FRAME, q3vm_frame_time);
            VM_Call(vm, GAME_CLIENT_THINK, 0);
            a_us_vm += a_us_since(t0);
            a_srv_ticks++;
            a_srv_ticks_total++;
            a_srv_ticks_win++;
            a_srv_ms += Q3VM_FRAMETIME;
        }
    }

    ps = q3vm_ps(vm);

    /* --- the module's viewpoint is the camera (v38.118: interpolated) -----
     * Record the fresh tick pose, then draw from the phase-correct point
     * BETWEEN the last two tick ends: alpha = a_srv_acc / 50 (the wall-clock
     * phase since `cur` was consumed). The eye therefore sits ~one tick
     * behind the simulation - exactly the deal Q3's own client makes: it
     * renders between server snapshots and hides the delay with prediction,
     * which a VM-local driver has no equivalent of, so the cost is a flat
     * 50 ms of pose latency and the benefit is a camera that moves on EVERY
     * rendered frame instead of 20 times a second. The first frame and the
     * frame after a teleport/respawn (pose jumped > 512 units) snap instead
     * of lerp. Angles lerp through the wrap (yaw lives on -180..180); pitch
     * needs none. Eye height rides in at draw time, not in the poses. */
    if (ps) {
        if (a_cam_have) {
            float dx = ps->origin[0] - a_cam_cur_pos[0];
            float dy = ps->origin[1] - a_cam_cur_pos[1];
            float dz = ps->origin[2] - a_cam_cur_pos[2];
            if (dx * dx + dy * dy + dz * dz > 512.0f * 512.0f) a_cam_have = 0;
        }
        if (!a_cam_have) {
            a_cam_have = 1;
            VectorCopy(ps->origin, a_cam_prev_pos);
            VectorCopy(ps->origin, a_cam_cur_pos);
            a_cam_prev_yaw = a_cam_cur_yaw = ps->viewangles[YAW];
            a_cam_prev_pitch = a_cam_cur_pitch = ps->viewangles[PITCH];
        }
        if (a_srv_ticks) {
            VectorCopy(a_cam_cur_pos, a_cam_prev_pos);
            a_cam_prev_yaw = a_cam_cur_yaw;
            a_cam_prev_pitch = a_cam_cur_pitch;
            VectorCopy(ps->origin, a_cam_cur_pos);
            a_cam_cur_yaw = ps->viewangles[YAW];
            a_cam_cur_pitch = ps->viewangles[PITCH];
            a_srv_ticks = 0;
        }
        {
            float alpha = (float)a_srv_acc * (1.0f / (float)Q3VM_FRAMETIME);
            float dyaw = a_cam_cur_yaw - a_cam_prev_yaw;
            vec3_t e;
            if (dyaw > 180.0f) dyaw -= 360.0f;
            if (dyaw < -180.0f) dyaw += 360.0f;
            if (alpha < 0.0f) alpha = 0.0f;
            if (alpha > 1.0f) alpha = 1.0f;
            e[0] = a_cam_prev_pos[0] + (a_cam_cur_pos[0] - a_cam_prev_pos[0]) * alpha;
            e[1] = a_cam_prev_pos[1] + (a_cam_cur_pos[1] - a_cam_prev_pos[1]) * alpha;
            e[2] = a_cam_prev_pos[2] + (a_cam_cur_pos[2] - a_cam_prev_pos[2]) * alpha;
            e[2] += (float)ps->viewheight;
            {
                /* Reuse the module's own AngleVectors through a temp angle
                 * vector - no freestanding trig, identical convention. */
                vec3_t va;
                va[YAW] = a_cam_prev_yaw + dyaw * alpha;
                va[PITCH] = a_cam_prev_pitch +
                            (a_cam_cur_pitch - a_cam_prev_pitch) * alpha;
                va[ROLL] = 0.0f;
                AngleVectors(va, fwd, NULL, NULL);
                a_last_cam_alpha = (int)(alpha * 100.0f);
            }
            VectorCopy(e, eye);
        }
        /* v38.119: a pinned pose overrides the interpolated camera for tests
         * (the bench drives a deterministic walk, but a heavy VIEW needs an
         * exact pose; the module keeps simulating and moving its player, only
         * the eye is pinned, so the scene is reproducible frame for frame). */
        if (a_pose_set) {
            vec3_t va;
            eye[0] = a_pose_x; eye[1] = a_pose_y; eye[2] = a_pose_z;
            va[YAW] = a_pose_yaw; va[PITCH] = a_pose_pitch; va[ROLL] = 0.0f;
            AngleVectors(va, fwd, NULL, NULL);
        }
        q3ref_set_camera_basis(eye, fwd);
    }
    {
        int t0 = a_us();
        q3ref_begin_frame();
        int t1 = a_us();
        q3ref_draw_world((double)q3vm_frame_time / 1000.0);
        int t2 = a_us();
        q3ref_end_frame();
        int t3 = a_us();
        {
            /* These are differences of the timestamps captured ABOVE. Calling
             * the clock again here (a_us_since) would measure from the phase's
             * start to NOW, which is past the end of all three - the first
             * version did exactly that and reported a GL phase ~3x its real
             * cost, which is how a 1.9 ms clear, 10 ms world and 0.1 ms present
             * became `gl_ms=600` in a 360 ms window. */
            int d_begin = t1 - t0;          /* viewport + clear */
            int d_draw  = t2 - t1;          /* the world itself   */
            int d_end   = t3 - t2;          /* the present snapshot */
            if (d_begin < 0) d_begin = 0;
            if (d_draw  < 0) d_draw  = 0;
            if (d_end   < 0) d_end   = 0;
            a_us_gl += d_begin + d_draw + d_end;
            /* v38.116: where inside the render phase the time sits — the
             * clear, the world itself, or the present snapshot. Drives the
             * next optimisation instead of a guess. glfps is this frame's own
             * render-only rate: what the limiter removed, the log shows. */
            a_us_gl_begin += d_begin;
            a_us_gl_draw += d_draw;
            a_us_gl_end += d_end;
        }
        a_last_glfps = (t3 > t0) ? 1000000 / (t3 - t0) : 0;
    }
    {
        int t0 = a_us();
        int o_stats, o_hist, o_hud;
        q3ref_bsp_stats(&drawn, &tris, &culled, &shaders, &from_disk, &ph);
        /* v38.112: how much of the mesh the map's own PVS kept this frame. */
        q3ref_vis_stats(&vis_marked, &vis_total, &vis_cluster, &vis_leafs);
        q3ref_cull_stats(&cull_pvs, &cull_frustum, &cull_planes, &cull_back);
        o_stats = a_us_since(t0);
        if (sample) {
            q3ref_frame_histogram(&cyan, &warm, &stepgreen, &violet, &bright,
                                  &patch, &sky, &distinct, &wall);
        }
        o_hist = a_us_since(t0) - o_stats;
        /* v38.119: the overlay is painted EVERY frame (it is the user's only
         * live readout) into the same 320x240 buffer the world just filled —
         * v38.120 shrank it to a ~124x56 mini panel (4x8 glyphs, 25% blend)
         * after the old solid 236x140 slab covered half the view — and its
         * cost is picked up here, separately from the present snapshot below,
         * because the two are the only sizable things left in "other" and
         * they are fixed by completely different changes. */
        q3ref_draw_perf_overlay();  /* F3 panel only, and only if toggled on:
                                     * the default HUD (the fps readout) is
                                     * painted after the upscale instead, in
                                     * q3ref_blit/q3ref_present_fullscreen */
        /* v38.123: the gun rides the same buffer — state straight out of the
         * module's own playerState (weaponstate/groundEntityNum/bobCycle),
         * drawn after the overlay so the readout stays on top. Cost folds
         * into o_hud: it is the same category of fixed per-frame overlay. */
        if (ps) {
            extern void q3ref_set_viewmodel(int firing, int grounded, int bob);
            extern void q3ref_draw_viewmodel(void);
            q3ref_set_viewmodel(
                ps->weaponstate == 3 /* WEAPON_FIRING, bg_public.h */,
                ps->groundEntityNum != 1023 /* ENTITYNUM_NONE */,
                ps->bobCycle);
            q3ref_draw_viewmodel();
            /* vm_firing_diag deliberately NOT touched here: a plain
             * per-frame assignment erased the sticky latch before the
             * frame%100 diag line ever sampled it (v38.123 regression —
             * the flash column always read 0). Latch-only, see above. */
        }
        o_hud = a_us_since(t0) - o_stats - o_hist;
        a_us_o_stats += o_stats > 0 ? o_stats : 0;
        a_us_o_hist += o_hist > 0 ? o_hist : 0;
        a_us_o_hud += o_hud > 0 ? o_hud : 0;
        /* v38.116: the snapshot the compositor blits is taken HERE, after the
         * HUD, so the fps counter and the cost bar are actually on screen.
         * v38.119: in fullscreen the same snapshot is upscaled straight into
         * the back buffer instead of being handed to the WM window — the
         * kernel main loop then only swaps it (no desktop composite). */
        {
            int p0 = a_us();
            if (a_fullscreen) q3ref_present_fullscreen();
            else              q3ref_present_frame();
            { int p1 = a_us(); if (p1 > p0) a_us_o_pres += p1 - p0; }
            /* v38.125: not while fullscreen owns the screen. v38.118 made
             * wm_invalidate() schedule a composition (it sets needs_redraw),
             * which is right for a window and wrong here: the window is not the
             * screen any more, and the invalidation is a standing invitation for
             * the kernel's swap to land in the MIDDLE of the next frame's
             * upscale — and a swap that lands mid-write presents a torn frame
             * AND clears the damage rect, so the rows it had not reached are
             * left off screen until something marks them dirty again. The
             * exclusive path claims the screen itself, after the write
             * (vga_fullscreen_present); the window's buffer is re-invalidated
             * the moment ESC hands the screen back, because this line runs
             * again on that very frame. */
            if (a_win >= 0 && !a_fullscreen) wm_invalidate(a_win);
        }
        a_us_other += a_us_since(t0);
    }

    a_frames++;
    a_faces_drawn += drawn;
    a_tris_drawn += tris;

    if ((frame % 20) == 0) {
        /* v38.110: one atomic perf line — phase costs over the window since
         * the last sampled frame (20 game frames = 1 s of game time), plus
         * the compositor's charges for this window (accumulated since the
         * last sample, then drained). sum_ms is that window's wall time, so
         * the parts are directly comparable to the whole. */
        {
            int pass_ms = 0, blit_ms = 0, draw_ms = 0;
            int fps = 0;
            /* The four phases are accumulated in us and reported in ms; the
             * sum is taken in us first so rounding cannot make the parts
             * disagree with the whole by more than the truncation. */
            int vm_ms = a_us_vm / 1000, gl_ms = a_us_gl / 1000;
            int other_ms = a_us_other / 1000, idle_ms = a_us_idle / 1000;
            int sum_ms = (a_us_vm + a_us_gl + a_us_other + a_us_idle) / 1000;
            unsigned elapsed = (unsigned)Com_Milliseconds() - a_start_ms;
            if (elapsed > 0) fps = (a_frames * 1000) / (int)elapsed;
            a_last_fps = fps;
            /* v38.126: the HUD shows THIS window's fps (20 frames over the
             * window's wall ms, same clock the limiter and the server feed
             * use), not the run average. The user's own session is why: at
             * frame 4900 the average read 20 fps while the window's own phases
             * summed to 45 ms/frame -> 22 fps, and the parked view read 50. An
             * average that lags the game by minutes answers "berapa fps nya"
             * with a number about the whole session. The perf line keeps the
             * average in `fps=` — that field is what the suites read. */
            {
                unsigned wnow = (unsigned)Com_Milliseconds();
                unsigned wms = wnow - a_win_ms;
                a_win_ms = wnow;
                a_last_fps_win = (wms > 0) ? (int)((20u * 1000u) / wms) : 0;
            }
            wm_q3_times(&pass_ms, &blit_ms, &draw_ms);
            reportf("[Q3ARENA] perf frame=%d fps=%d vm_ms=%d gl_ms=%d "
                    "blit_ms=%d draw_ms=%d wm_ms=%d other_ms=%d idle_ms=%d "
                    "sum_ms=%d",
                    frame, fps,
                    vm_ms, gl_ms, blit_ms, draw_ms, pass_ms,
                    other_ms, idle_ms, sum_ms);
            /* The in-window HUD shows the same numbers the log carries: the
             * window-1s phase costs and the run's fps. The overlay itself is
             * painted every frame (after this frame's histogram was taken, so
             * the suite's pixel evidence stays exactly what the 3D pass made). */
            q3ref_set_perf_overlay(a_last_fps_win, vm_ms, gl_ms,
                                   blit_ms, pass_ms, other_ms);
            a_perf_frames += 20;
            a_us_vm = a_us_gl = a_us_other = a_us_idle = 0;
            /* v38.116 deep profile: where gl_ms sits (begin = viewport+clear,
             * draw = the world, end = the present snapshot), and what the two
             * decoupled clocks did over this window: srv_ticks x 50 ms of game
             * time actually simulated, and the render-only fps.
             *
             * v38.119: this and the two glcyc/ps blocks below are DEV
             * diagnostics — no suite parses them — and they are only printed
             * every fifth window now. Serial output is not free: at 115200 8N1
             * the sampled block above is ~1 KB of text, which is 87 ms of UART
             * time per window, and that cost lands in `other_ms` where it was
             * 25% of a heavy frame (it is what the 4-6 ms residual in `other`
             * was). Cutting the dev lines to 1 window in 5 gives ~10% of the
             * frame back on a view where the serial line was the second
             * largest consumer after the raster. The accumulators are still
             * RESET every window, so the numbers printed here still describe
             * the last 20 frames exactly as before. */
            if ((frame % 100) == 0) {
                {
                    int s_en = 0, s_pool = 0, s_sorted = 0;
                    q3w_sort_stats(&s_en, &s_pool, &s_sorted);
                    reportf("[Q3ARENA] f2b frame=%d sort=%d sorted=%d pool=%d",
                            frame, s_en, s_sorted, s_pool);
                }
                reportf("[Q3ARENA] glsplit frame=%d begin=%d draw=%d end=%d "
                        "srv_ticks=%d srv_ms=%d glfps=%d stats=%d hist=%d "
                        "hud=%d pres=%d fps_win=%d detail=%d",
                        frame, a_us_gl_begin / 1000, a_us_gl_draw / 1000,
                        a_us_gl_end / 1000,
                        a_srv_ticks_win, a_srv_ms, a_last_glfps,
                        a_us_o_stats / 1000, a_us_o_hist / 1000,
                        a_us_o_hud / 1000, a_us_o_pres / 1000,
                        a_last_fps_win, a_perf_detail);
            }
            a_us_o_stats = a_us_o_hist = a_us_o_hud = a_us_o_pres = 0;
            /* v38.116: and where INSIDE draw the time is. The 10 ms kernel
             * clock cannot split a 35 ms GL phase, so TinyGL accumulates TSC
             * cycles around the glVertex path (transform + clip + the raster
             * nested in it) and around the raster alone; the two are drained
             * here as per-frame kcycles plus the fill share. `tris` is the
             * number the raster saw, which is what makes the cycles
             * comparable across frames that drew different views. */
            {
                unsigned long long vc = 0, fc = 0;
                unsigned int tn = 0;
                int vkc, fkc, fpc;
                q3ref_glsplit_stats(&vc, &fc, &tn);
                /* The window is 20 frames, so per-frame costs are the 20-frame
                 * totals over 20 — but a freestanding kernel link has no
                 * __udivdi3, and a 20-frame window can exceed 2^32 cycles.
                 * Shifting first (2^20 = "megacycles") keeps every division in
                 * 32-bit: at 2.2e9 cycles per window the shifted value is 2100,
                 * and x1000 puts the result in kcycles per frame. */
                vkc = (int)(((unsigned)(vc >> 20)) * 1000u / 20u);
                fkc = (int)(((unsigned)(fc >> 20)) * 1000u / 20u);
                fpc = vc ? (int)(((unsigned)(fc >> 20)) * 100u /
                                 ((unsigned)(vc >> 20) ? (unsigned)(vc >> 20) : 1u))
                         : 0;
                /* `tris` in this line is a 20-FRAME total (it is drained with
                 * the cycle counters) while the frame line's `tris` is one
                 * frame's fan count. Comparing the two directly is how v38.119
                 * came to believe the raster was amplifying each face ~19x — it
                 * was comparing 20 frames against one. */
                if ((frame % 100) == 0) {
                    /* v38.124: the view model's own share, same units and the
                     * same 20-frame window. The world's numbers above exclude
                     * it on purpose; WITHOUT this the gun's cost would simply
                     * vanish from the profile, which is the one way a new pass
                     * can hide a frame regression. */
                    unsigned long long vmv = 0, vmf = 0;
                    unsigned int vmt = 0;
                    q3ref_viewmodel_cycles(&vmv, &vmf, &vmt);
                    reportf("[Q3ARENA] glcyc frame=%d tris=%d vert_kc=%d "
                            "fill_kc=%d fill_pct=%d vm_tris=%d vm_fill_kc=%d",
                            frame, (int)tn, vkc, fkc, fpc, (int)vmt,
                            (int)(((unsigned)(vmf >> 20)) * 1000u / 20u));
                }
                q3ref_glsplit_reset();
            }
            /* v38.122: the gameplay channel, on the record. up/btn are what
             * this driver built into the usercmd; weapon/ammo/health are read
             * STRAIGHT OUT of the module's ps (shared memory — the same read
             * the retail server makes), so the suite asserts id's own state
             * machine, not the port's bookkeeping. ammo indexes by weapon:
             * ps->ammo[WP_MACHINEGUN] starts at 100 (FFA spawn, g_client.c)
             * and PM_Weapon takes one per shot. */
            if ((frame % 100) == 0 && ps) {
                /* bg_public.h's enums are not in this unit's include path
                 * (g_public.h does not pull it), and the values are stable
                 * across every Q3A derivative: weapon 2 = WP_MACHINEGUN,
                 * stats[0] = STAT_HEALTH, ammo is indexed by the same weapon
                 * enum. Spell the numbers, cite the header. */
                reportf("[Q3ARENA] play frame=%d up=%d btn=%d weapon=%d "
                        "ammo_mg=%d health=%d ground=%d jumps=%d shots=%d "
                        "vel_z=%d",
                        frame, a_up, a_btn, (int)ps->weapon,
                        (int)ps->ammo[Q3VM_WP_MACHINEGUN],
                        (int)ps->stats[Q3VM_STAT_HEALTH],
                        (int)ps->groundEntityNum,
                        a_jumps, a_shots, (int)ps->velocity[2]);
                /* v38.123: the viewmodel's inputs, same sampling grid —
                 * flash is id's own WEAPON_FIRING, bob the raw bobCycle,
                 * ground the air state. v38.124 split the flash column in
                 * two, because one number was doing two jobs: `flash` is the
                 * LIVE weaponstate this frame (literally the value handed to
                 * q3ref_set_viewmodel above, and therefore the one that decides
                 * whether the flash's triangles were submitted), and `latched`
                 * is the sticky "a tick ran the fire path" flag a 100-frame
                 * sampling grid needs to see a 20-frame window at all. Reading
                 * the latch as if it were the drawing state made `drawn` look
                 * wrong on every non-firing sample.
                 *
                 * v38.124: the same line now reports WHAT was drawn, because
                 * "a gun is on screen" stopped being enough the moment there
                 * were two possible guns. model/parts is the .md3 loader's own
                 * count (0 = no model on the volume, the silhouette below is
                 * what ran), surf/tris are the geometry it parsed, drawn the
                 * triangles the last pass SUBMITTED (equal to tris whenever the
                 * whole model reached the rasterizer, minus the flash's share
                 * when this frame was not firing — flash_tris is on the loader
                 * line), tex the body's image, box the model's screen
                 * rectangle as the draw pass projected it, and distinct the
                 * number of 4-bit colours inside that box — the one number a
                 * flat hand-drawn slab can never produce. The old px anchor
                 * (rw-92+kick) is gone with the silhouette it described;
                 * model=0 is the signal to read the fallback's field instead.
                 *
                 * v38.125 adds the two fields that make the FLASH testable:
                 * add is 1 when this pass submitted the muzzle flash
                 * additively (id's own blendfunc for f_machinegun; drawing it
                 * opaque painted its black surround over the screen, which is
                 * what the user's screenshot showed) and add=0 whenever no
                 * flash was drawn, so the pair (flash, add) says both "it
                 * fired this frame" and "it was composited id's way". dark is
                 * the count of near-black pixels inside the box: additive can
                 * only add light, so a firing frame's dark count can never
                 * beat the idle one's. */
                {
                    int vm_parts = 0, vm_surf = 0, vm_tris = 0, vm_drawn = 0;
                    int vm_tw = 0, vm_th = 0, vm_dist = 0;
                    int vm_bright = 0, vm_dark = 0;
                    int vx0 = 0, vy0 = 0, vx1 = 0, vy1 = 0;
                    q3ref_viewmodel_stats(&vm_parts, &vm_surf, &vm_tris,
                                          &vm_drawn, &vm_tw, &vm_th, &vx0,
                                          &vy0, &vx1, &vy1, &vm_dist);
                    q3ref_viewmodel_box(&vm_dist, &vm_bright, &vm_dark);
                    reportf("[Q3ARENA] viewmodel frame=%d weapon=%d flash=%d "
                            "latched=%d ground=%d bob=%d model=%d surf=%d "
                            "tris=%d drawn=%d tex=%dx%d box=%d,%d,%d,%d "
                            "distinct=%d add=%d bright=%d dark=%d",
                            frame, (int)ps->weapon,
                            ps->weaponstate == 3 /* the live state the draw
                                                  * above was given */,
                            vm_firing_diag,
                            ps->groundEntityNum != 1023,
                            ps->bobCycle & 255,
                            vm_parts, vm_surf, vm_tris, vm_drawn,
                            vm_tw, vm_th,
                            vx0, vy0, vx1, vy1, vm_dist,
                            q3ref_viewmodel_flash(), vm_bright, vm_dark);
                }
            }
            a_us_gl_begin = a_us_gl_draw = a_us_gl_end = 0;
            a_srv_ticks = 0;
            a_srv_ticks_win = 0;
            a_srv_ms = 0;
        }
    if (ps) {
        reportf("[Q3ARENA] frame=%d t=%d pos=(%d,%d,%d) eye_z=%d yaw=%d "
                "pitch=%d drawn=%d tris=%d culled=%d vis=%d/%d cluster=%d "
                "cull_pvs=%d cull_frustum=%d planes=%d back=%d cam_alpha=%d",
                frame, q3vm_frame_time,
                (int)ps->origin[0], (int)ps->origin[1], (int)ps->origin[2],
                (int)eye[2], (int)ps->viewangles[YAW], (int)ps->viewangles[PITCH],
                drawn, tris, culled, vis_marked, vis_total, vis_cluster,
                cull_pvs, cull_frustum, cull_planes, cull_back,
                a_last_cam_alpha);
        /* v38.113 diagnostics: WHY is the player where it is? The walk gate in
         * the module is `msec = cmd.serverTime - ps.commandTime; if (msec < 1)
         * return;` — and the counters below say whether that gate ever opens
         * (commandTime stuck at 0), whether Pmove is even in play (velocity
         * and groundEntity stuck at their spawn values), and whose turn it
         * is (pm_type). No suite reads this line and it is one of the longest
         * in the block, so v38.119 prints it once every fifth window; the
         * information a pixel/suite assertion needs is in the frame line. */
        if ((frame % 100) == 0) {
            reportf("[Q3ARENA] ps frame=%d cmd_time=%d commandTime=%d "
                    "pm_type=%d vel=(%d,%d,%d) ground=%d flags=0x%x "
                    "input_seen=%d cmd_fm=%d cmd_yaw=%d",
                    frame, q3vm_frame_time, (int)ps->commandTime,
                    (int)ps->pm_type, (int)ps->velocity[0],
                    (int)ps->velocity[1], (int)ps->velocity[2],
                    (int)ps->groundEntityNum,
                    (unsigned)ps->pm_flags, a_input_seen,
                    (int)q3vm_cmd_forward, (int)q3vm_cmd_yaw);
        }
    } else {
            reportf("[Q3ARENA] frame=%d t=%d drawn=%d tris=%d culled=%d "
                    "vis=%d/%d cluster=%d cull_pvs=%d cull_frustum=%d planes=%d back=%d",
                    frame, q3vm_frame_time, drawn, tris, culled,
                    vis_marked, vis_total, vis_cluster,
                    cull_pvs, cull_frustum, cull_planes, cull_back);
        }

        /* The finished frame, read back out of the renderer's own buffer: one
         * line per sampled frame that says what the camera actually saw.
         * v38.117 adds `wall` — the wall textures' own colour at the light
         * this renderer bakes, which the buckets before it all rejected (the
         * old `warm` count was a z-fight artefact on the fixture's wall tops,
         * not a wall pixel). */
        reportf("[Q3ARENA] pixels frame=%d cyan=%d warm=%d stepgreen=%d "
                "violet=%d bright=%d patch=%d sky=%d wall=%d distinct=%d",
                frame, cyan, warm, stepgreen, violet, bright, patch, sky,
                wall, distinct);
    }

    /* v38.116 pacing: the render loop runs free. Its only delay is a frame
     * limiter (com_maxfps-style): when a frame finished early, wait out the
     * remainder of RENDER_MIN_MS so the desktop keeps a timeslice. There is no
     * waiting for the server any more — server ticks are caught up at the top
     * of the next frame — so idle_ms now means limiter wait, not a locked 20 Hz. */
    a_next_ms += (unsigned)RENDER_MIN_MS;
    {
        int t0 = a_us();
        for (;;) {
            unsigned now = (unsigned)Com_Milliseconds();
            if (now >= target || a_quit) break;
            __asm__ __volatile__("hlt");
        }
        a_us_idle += a_us_since(t0);
    }
    /* v38.125: an end-of-frame "drop the drift" clause lived here —
     *     if (now > a_srv_last_ms + 250u) a_srv_last_ms = now;
     * — and it is what starved the tick feed on a slow host. Moving the
     * REFERENCE forward is not the same as dropping the DRIFT: the next
     * frame's catch-up then measures the time since the END of the previous
     * frame (a few microseconds) instead of since its start, so dt collapses to
     * zero and the game clock crawls. Measured on TCG before this change: 0.3
     * ticks per rendered frame at 333 ms/frame, i.e. the module simulated about
     * a tenth of real time, and the earlier reading of that as "this VM's clock
     * disagrees with itself" (v38.119) was this clause, not the clock. The
     * spiral it was guarding against cannot happen anyway: the tick block caps
     * the accumulator at 125 ms — 2 ticks per frame — whatever the stall was,
     * so a 5-second stall still advances the world by exactly 100 ms. Nothing
     * changes on a host whose frames are under 250 ms — the clause never fired
     * there, which is why every real session (40+ fps) has always read a
     * healthy srv_ticks. */
    /* Whatever this frame's own bookkeeping cost (camera, counters, the perf
     * line itself) is not a phase anyone named — charge it to "other" so the
     * parts of sum_ms stay honest against the whole. */
    {
        int spent = a_us_since(f_t0);
        int known = a_us_vm + a_us_gl + a_us_other + a_us_idle;
        if (spent > known) a_us_other += spent - known;
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

/* v38.111: map selection. cmd_q3arena passes "q3arena" for the generated test
 * arena (so every existing regression keeps its meaning) or "q3arena <name>"
 * for a staged retail map. The name is validated HERE as well — the launch_arg
 * string is attacker-controlled in principle, and only [a-z0-9_] may ever be
 * part of a maps/*.bsp qpath this driver opens. Anything odd (empty, too long,
 * other characters) silently keeps the default arena.
 *
 * v38.119: the payload is a token list, still validated character by character:
 *     *     q3arena [<map>] [fullscreen] [@x,y,z,yaw[,pitch]]
     *
     * `fullscreen` takes the present path over (vga_fullscreen_*), and `@...`
     * pins the camera at an exact pose (integers; the eye position and the view
     * angles, in degrees) so a heavy view can be replayed frame for frame —
     * that is how the heavy-view suite measures, and how the render cost of one
     * fixed view is compared between builds. Every token is optional and
     * order-free; an unknown token invalidates the whole argument and the run
     * falls back to "generated arena, windowed", which is what every existing
     * suite asserts. */
#define Q3VM_TOKEN_MAX 24

/* Parse one signed integer. Returns the new index, or -1. */
static int q3vm_arg_int(const char *s, int i, int *out) {
    int neg = 0, v = 0, digits = 0;
    if (s[i] == '-') { neg = 1; i++; }
    while (s[i] >= '0' && s[i] <= '9' && digits < 7) {
        v = v * 10 + (s[i] - '0');
        i++; digits++;
    }
    if (!digits) return -1;
    *out = neg ? -v : v;
    return i;
}

static int q3vm_arg_pose(const char *s, int i) {
    int c[5], k;
    for (k = 0; k < 5; k++) c[k] = 0;
    if (s[i] != '@') return -1;
    i++;
    for (k = 0; k < 5; k++) {
        int ni = q3vm_arg_int(s, i, &c[k]);
        if (ni < 0) return -1;
        i = ni;
        if (k == 4) break;
        if (s[i] == ',') { i++; continue; }
        if (k >= 3) break;              /* 4th field (pitch) is optional */
        return -1;
    }
    if (s[i] != '\0' && s[i] != ' ') return -1;
    a_pose_x = (float)c[0]; a_pose_y = (float)c[1]; a_pose_z = (float)c[2];
    a_pose_yaw = (float)c[3]; a_pose_pitch = (float)c[4];
    a_pose_set = 1;
    return i;
}

static void q3vm_apply_map_arg(void) {
    extern int get_current_task(void);
    extern const char* task_get_launch_arg(int tid);
    const char *arg = task_get_launch_arg(get_current_task());
    static const char prefix[] = "q3arena ";
    int i = 0, n = 0, seen_map = 0;

    if (!arg) return;
    while (prefix[i] && arg[i] == prefix[i]) i++;
    if (prefix[i]) return;                       /* not "q3arena ..." */

    /* Token walk. A bad token aborts the whole parse (name included) so a
     * half-understood argument can never half-configure a run. Whitespace is a
     * separator and nothing else: the first version forgot to consume it, so
     * the space after the map name hit the "unknown character" arm and every
     * multi-token line ("q3dm1 fullscreen", any pinned pose) silently ran the
     * generated arena instead. */
    while (arg[i]) {
        if (arg[i] == ' ') { i++; continue; }
        if ((arg[i] >= 'a' && arg[i] <= 'z') || (arg[i] >= '0' && arg[i] <= '9') ||
            arg[i] == '_') {
            /* a word: `fullscreen`, or the map name. Every character has to be
             * part of the [a-z0-9_] alphabet the qpath allows — a word is never
             * skipped just because its HEAD looks legal. */
            int w0 = i, wl = 0;
            while (arg[i] && arg[i] != ' ' && arg[i] != '=') {
                char ch = arg[i];
                if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_'))
                    return;
                i++; wl++;
            }
            if (wl == 0 || wl >= Q3VM_TOKEN_MAX) return;
            if (wl == 10) {
                static const char kw[] = "fullscreen";
                int k = 0;
                while (kw[k] && arg[w0 + k] == kw[k]) k++;
                if (k == 10) { a_fullscreen = 1; continue; }
            }
            if (wl == 6) {
                static const char kw[] = "nosort";
                int k = 0;
                while (kw[k] && arg[w0 + k] == kw[k]) k++;
                if (k == 6) { a_sort = 0; continue; }   /* v38.121 A/B knob */
            }
            /* v38.122: `=` is NOT in the word alphabet, and the walk used to
             * treat anything else as a fatal (the whole parse silently
             * discarded). Stop at `=` instead so the kv form can be handled
             * here: jump=<n> / fire=<n> schedule n actions from frame 40, one
             * every a_jump_gap / a_fire_gap frames. A word of any other shape
             * followed by `=` is still a fatal. */
            if (arg[i] == '=' && wl > 0 && wl < 5) {
                static const char kwj[] = "jump";
                static const char kwf[] = "fire";
                int isj = (wl == 4), isf = (wl == 4), k2;
                for (k2 = 0; k2 < wl; k2++) {
                    if (arg[w0 + k2] != kwj[k2]) isj = 0;
                    if (arg[w0 + k2] != kwf[k2]) isf = 0;
                }
                if (!isj && !isf) return;
                i = w0 + wl + 1;                  /* past the '=' */
                {
                    int val = 0, digits = 0;
                    while (arg[i] >= '0' && arg[i] <= '9' && digits < 3) {
                        val = val * 10 + (arg[i] - '0'); i++; digits++;
                    }
                    if (!digits) return;          /* 'jump=' with no number */
                    if (arg[i] != ' ' && arg[i] != '\0') return;
                    if (isj) {
                        a_jump_left = val;
                        a_jump_at = 40;
                    } else {
                        a_fire_left = val;
                        a_fire_at = 40;
                    }
                }
                continue;
            }
            if (seen_map) return;                /* two names: not a map arg */
            seen_map = 1;
            n = wl;
            for (int k = 0; k < n; k++) q3vm_bsp_name[k] = arg[w0 + k];
            q3vm_bsp_name[n] = '\0';
            continue;
        }
        if (arg[i] == '@') {
            int ni = q3vm_arg_pose(arg, i);
            if (ni < 0) { a_pose_set = 0; return; }
            i = ni;
            continue;
        }
        return;                                  /* unknown character */
    }
    if (arg[i] != '\0') return;

    /* The knobs apply whether or not a map name came with them. */
    reportf("[Q3ARENA] args: map='%s' fullscreen=%d pose=%d sort=%d "
            "jump=%d fire=%d",
            n ? q3vm_bsp_name : "(generated arena)",
            a_fullscreen, a_pose_set, a_sort, a_jump_left, a_fire_left);
    if (a_pose_set)
        reportf("[Q3ARENA] pose pinned: eye=(%d,%d,%d) yaw=%d pitch=%d",
                (int)a_pose_x, (int)a_pose_y, (int)a_pose_z,
                (int)a_pose_yaw, (int)a_pose_pitch);

    if (n == 0) return;
    {
        const char *dot = ".bsp";
        int p = 0, q = 0, k;
        for (; "maps/"[p]; p++) q3vm_bsp_fs[p] = "maps/"[p];
        for (k = 0; k < n; k++) q3vm_bsp_fs[p + k] = q3vm_bsp_name[k];
        p += n;
        for (k = 0; dot[k]; k++) q3vm_bsp_fs[p + k] = dot[k];
        q3vm_bsp_fs[p + 4] = '\0';
        for (; "/ext2/baseq3/maps/"[q]; q++) q3vm_bsp_vfs[q] = "/ext2/baseq3/maps/"[q];
        for (k = 0; k < n; k++) q3vm_bsp_vfs[q + k] = q3vm_bsp_name[k];
        q += n;
        for (k = 0; dot[k]; k++) q3vm_bsp_vfs[q + k] = dot[k];
        q3vm_bsp_vfs[q + 4] = '\0';
    }
    reportf("[Q3VM] map arg: '%s'", q3vm_bsp_name);
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

    /* Map selection first: the world load below opens whatever this resolved. */
    q3vm_apply_map_arg();

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
     * through trap_Printf -> our serial log. The first argument is LEVEL TIME,
     * and a real server starts it at 0 (svs.time), never at the wall clock:
     * v38.105 passed Com_Milliseconds() here and worked only because loading
     * was fast — on q3dm1 the 80 s texture load put level.time ~80 s AHEAD of
     * the driver's frame clock, so id's own gate in ClientThink_real
     * (`msec = cmd.serverTime - ps.commandTime; if (msec < 1) return;`)
     * rejected every command and the player stood frozen for the whole
     * session. The driver owns the server clock (q3vm_frame_time starts at 0
     * too), so the module and the driver now start on the same timeline. The
     * second argument stays the wall clock: it is only a random seed. */
    write_serial_string("[Q3VM] calling vmMain(GAME_INIT) — official id game code\n");
    int before = vm_syscalls;
    int r = VM_Call(vm, GAME_INIT, 0, Com_Milliseconds(), 0);
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
        a_win_ms  = t_start;   /* v38.126: the fps window starts here too */
        a_srv_last_ms = t_start;   /* v38.116: both clocks start together */
        a_srv_acc = 0;
        a_cam_have = 0;
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
            {
                int lit_last = 0, lit_total = 0;
                int f_addfaces = 0;
                q3ref_light_stats(&lit_last, &lit_total);
                q3ref_draw_flag_stats(NULL, NULL, NULL, &f_addfaces);
                reportf("[Q3ARENA] render totals: frames=%d faces=%d tris=%d "
                        "wall_ms=%d litfaces=%d litfaces_total=%d "
                        "addfaces=%d",
                        a_frames, a_faces_drawn, a_tris_drawn,
                        (int)((unsigned)Com_Milliseconds() - t_start),
                        lit_last, lit_total, f_addfaces);
            }
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
