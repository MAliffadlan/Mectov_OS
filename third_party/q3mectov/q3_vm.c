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

/* v38.129: the sound configstring base — bg_public.h:86, which g_public.h does
 * NOT pull into this unit's include graph, so spell it here. Guarded so a
 * future include of bg_public.h cannot collide with the real one.
 *   CS_SOUNDS = CS_MODELS(32) + MAX_MODELS(256)  ->  288 */
#ifndef CS_SOUNDS
#define CS_SOUNDS            (32 + 256)
#endif

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
/* v38.132: the backface reject, its `nocull` switch, and how many faces it
 * declined to reject (the fail-safe path). Same reason as above — the driver
 * does not carry the renderer's include path. */
extern void q3w_set_cull(int on);
extern int  q3w_cull_untrusted(void);
/* v38.132: the texture-decode progress hook (q3world_render.h). Typed the same
 * way as the hook itself so this TU needs no renderer header — and no wm.h in
 * the renderer, which is the point of the seam. */
typedef void (*q3w_progress_fn_t)(int done, int total);
extern void q3w_set_progress_hook(q3w_progress_fn_t fn);
extern int  q3w_cull_enabled(void);
#include "../../src/include/wm.h"
/* v38.131: the early boot window paints its own loading text — the kernel's
 * 8x16 font, the same glyph table the WM and the terminal render with. */
#include "../../src/include/font8x16.h"
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
        /* v38.150: FS_Read, not FS_Read2 — see the long note in
         * q3_uivm.c's UI_FS_READ. FS_FOpenFileByMode marks every FS_READ
         * handle "streamed", and FS_Read2 sends the read to
         * Sys_StreamedRead, which this port stubs to 0: the read came back
         * empty. FS_Read has no streamed branch and reads the handle's FILE
         * where the open left it. The game module never noticed (its file
         * access goes through FS_ReadFile, which uses FS_Read directly); the
         * UI's arena and bot lists did. */
        FS_Read(VMA(1), args[2], args[3]);
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
            /* v38.129: a SOUND configstring is the module registering a sfx
             * by path (G_SoundIndex -> trap_SetConfigstring(CS_SOUNDS+i)).
             * Register it in the sound bridge the moment it is named, so the
             * cache is warm before the first event that plays it. Index 0 is
             * id's "no sound" sentinel and never carries a name. */
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
 * instead, which is exactly what capped fps at 20.
 * v38.140: superseded by the 40 Hz phase-locked pacer (a_next_us) in the
 * frame loop; the constant is gone, the history stays. */
#define Q3ARENA_WALL_MS    600000       /* stop regardless, so CI cannot hang */
#define Q3ARENA_TURN_DEG   130.0f       /* arrow-key turn rate, per second */
#define Q3ARENA_SENS       0.10f        /* mouse degrees per pixel */
/* v38.149: runtime sensitivity (the q3menu SETUP row), in hundredths of a
 * degree per pixel (10 = 0.10). Integer across the API because the menu
 * lives in src/apps (soft-float kernel TU, no fp helpers linked); the float
 * conversion happens here, where the FPU is enabled. Defaults to
 * Q3ARENA_SENS.
 * v38.150: was 0.18 (18). That is EIGHT times id's own default (m_yaw /
 * m_pitch 0.022 in the retail client), and with a real mouse it turns every
 * ordinary hand motion into multi-degree jumps: 10 cm on an 800 DPI mouse is
 * ~3150 counts, i.e. 567 degrees — a turn and a half — where id's default
 * gives 69. The harness never saw it because it injects 2-count steps. The
 * "fps is stable but every mouse move stutters" report is exactly what
 * 8x-too-coarse angle steps look like, and no present-path change can fix a
 * step that big — the pixels are correct, there are just too few of them per
 * degree of turn. On top of that the motion was delivered ~4x on SMP (the
 * mouse_take_delta race, also v38.150), so the effective feel was 0.72.
 * v38.151: 0.05 proved the diagnosis (measured exactly 1x delivery) but reads
 * as "delay" to a hand trained on the old feel — 14x slower for the same
 * motion. 0.10 is the middle that stays smooth on bursty input while turning
 * a full circle without running out of desk; the menu still offers
 * 0.05..1.00 either way, and THAT is where personal feel belongs, not in
 * this default. */
static int   a_sens_hund = 10;
static float a_sens_value = Q3ARENA_SENS;
float q3arena_sens_value(void) { return a_sens_value; }
int q3arena_sens_hund(void) { return a_sens_hund; }
void q3arena_set_sens_hund(int h) {
    if (h < 5) h = 5;
    if (h > 100) h = 100;
    a_sens_hund = h;
    a_sens_value = (float)h * 0.01f;
}

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
static int   a_hud = 1;         /* v38.127: id's status bar (nohud=off) */
static int   a_sky = 1;         /* v38.128: the cloud box (nosky=off) */
static int   a_nolimit;         /* v38.131: play until ESC (nolimit) */
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
 *                           airborne — the world entity is 0)
 *
 * v38.127 adds the two the status bar needs. MISSIONPACK (Team Arena) inserts a
 * member between STAT_HOLDABLE_ITEM and STAT_WEAPONS, so the value below is the
 * BASE Q3A layout, which is the one the demo's qagame.qvm is compiled against:
 *   STAT_ARMOR = 3         (bg_public.h statIndex_t: HEALTH, HOLDABLE, WEAPONS,
 *                           ARMOR)
 *   PERS_SCORE = 0         (bg_public.h persIndex_t — "MUST NOT CHANGE") */
#define Q3VM_BUTTON_ATTACK   1
#define Q3VM_WP_MACHINEGUN   2
#define Q3VM_STAT_HEALTH     0
#define Q3VM_STAT_ARMOR      3
#define Q3VM_PERS_SCORE      0
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

/* v38.132: the self-driving demo, and how it stopped face-planting.
 *
 * Until a key reaches the window the driver holds `forwardmove` at 127, which
 * id's Pmove turns into "walk straight ahead". On an open map that tours
 * nicely; on q3dm1 it reaches a wall in a few hundred frames and then presses
 * into it FOREVER. Measured from the user's own session: spawn
 * (318,2253,47) drew 379 faces, the walk settled at (1060,1431,24) with the
 * eye at floor level, drawn=3, and every counter frozen for 2000 frames
 * (distinct=27, warm=6795, vel=(0,0,0), input_seen=0). From outside the window
 * that is indistinguishable from a renderer that draws nothing — which is
 * exactly how it was first reported.
 *
 * So the demo now watches its own progress: if the player has not moved for
 * Q3ARENA_AD_STUCK consecutive game ticks it is against something, and it
 * turns 90 degrees and keeps going. Only ever active while `!a_input_seen`, so
 * a human at the keyboard is never touched — the moment any key lands, these
 * statics go idle and the real controls own the camera.
 *
 * The turn is applied through the SAME accumulated yaw the arrow keys use, so
 * it cannot fight the view: it adds to q3vm_cmd_yaw exactly as a keypress would,
 * and the existing wrap keeps it in [-180,180]. */
static float  a_ad_lastx, a_ad_lasty;
static int    a_ad_stuck;               /* consecutive ticks without progress */
static int    a_ad_pending_turn;        /* degrees still to add on next frame */
static int    a_ad_turns;               /* total, for the diag line */
static int    a_ad_drawn;               /* last frame's face count */
static int    a_ad_peak;                /* best view this session has shown */
static int    a_ad_blind;               /* consecutive collapsed-looking frames */
/* v38.149: the idle rescue's OWN counter.
 *
 * It used to share a_ad_blind with the collapse detector above, and that
 * detector zeroes the counter on every frame whose view is NOT collapsed
 * (`if (drawn * Q3ARENA_AD_COLLAPSE >= a_ad_peak) a_ad_blind = 0;` in the
 * per-frame bookkeeping). A player parked at spawn looking at a perfectly good
 * wall is exactly that case, so the counter was reset on every single frame
 * and `++a_ad_blind >= 45` could never be reached — which is the one situation
 * v38.137 wrote the rescue for. The rescue has therefore never once fired.
 *
 * Measured on the player's 12:46 session: mouse+keyboard input stopped
 * arriving at ~5 s (look_s 32,26,19,30 then 0 for the next 103 windows) and
 * the rescue did not take the camera back in the following 375 s — hm=1 in all
 * 108 perf windows with idle_in climbing to 370310 ms. So the screen stayed a
 * frozen photograph for six minutes instead of handing itself back to the demo
 * tour. Sharing one counter between two features that want opposite resets
 * does not work: this one is cleared by INPUT arriving, and by nothing else. */
static int    a_idle_frames;            /* frames since the 5 s idle gate opened */
#define Q3ARENA_AD_STUCK 25             /* game ticks ≈ 1.25 s of no progress */
#define Q3ARENA_AD_STEP  6.0f           /* "moved" if origin shifted this far */
/* Steering thresholds, and why they are relative rather than absolute.
 *
 * The first version fired when `drawn` fell below a fixed 64 faces. That
 * misfired immediately on the generated arena (`mectovtest`), which LEGITIMATELY
 * shows only 2-27 faces at a time — it is a 26-surface box. The demo started
 * turning every few frames from frame 20, and the q3arena suite's straight-line
 * movement assertion (delta > 100) fell to -27: a regression in a suite that
 * was green, caused by a "fix" aimed at a different map.
 *
 * So the test is now the shape of the drop, not a count:
 *   - the session must have shown a real view at all (peak >= Q3ARENA_AD_PEAK),
 *     which is what separates "touring a level" from "in a shoebox";
 *   - and the current frame must have COLLAPSED relative to that peak, by the
 *     ratio below, for long enough.
 * On q3dm1 (peak 725) a fall to drawn=3 trips it. On the arena (peak 27) it
 * never arms, so that demo walks exactly as it did before. */
#define Q3ARENA_AD_PEAK     200         /* faces: below this, not worth steering */
#define Q3ARENA_AD_COLLAPSE  4           /* current * this < peak  => collapsed */
#define Q3ARENA_AD_BLIND_FRAMES 45      /* ~0.75 s of sim before turning */
static int   a_frames;                  /* frames rendered */
static int   a_faces_drawn, a_tris_drawn;   /* totals across the run */
static unsigned char a_keys[128];       /* scancode -> held (no 0x80 bit) */

/* ---- v38.136: what ends the self-driving tour, and what must not ---------
 *
 * The tour above was gated on `!a_input_seen`, and a_input_seen is set by ANY
 * event this window receives — mouse motion included. That is the whole of the
 * 23:39 report ("di gw game nya ga muncul, masih stuck", "qemu freeze"):
 *
 *   - the user clicks the VM window to focus it. The pointer is captured by
 *     the game, so that click arrives as mouse MOTION: a_input_seen flips to
 *     1 and the demo — the forward walk AND both stuck detectors — stops dead;
 *   - the first motion event after the capture carries no sane delta: it
 *     carries the host pointer's whole travel to the window, so one event
 *     snapped the view to pitch=+82 (the floor);
 *   - the camera then sits there. Measured in that session's log:
 *     pos=(928,1479,24) unchanged from frame 220 to 721 (500 frames at
 *     12-17 fps), drawn ~49 faces per frame, warm=1, sky=0 — a static,
 *     near-black rectangle. ESC still worked, and leaving the game gave the
 *     desktop back, which is why it read as "QEMU froze, then unfroze".
 *
 * So the hand-over is decided by INTENT, not by traffic:
 *   a_human_move  - a movement/action key or a mouse button: a player taking
 *                   control. Only this stops the tour.
 *   a_mouse_first - the first motion event after the capture is dropped: that
 *                   is the focus click's accumulated travel, not aiming.
 *   a_keys_down / a_last_input_ms - so the tour can be handed back when a
 *                   taken-over camera is left on a collapsed view, which is
 *                   the rescue that keeps a dead frame off the screen. */
static int   a_pump_ms;                  /* v38.137: time inside q3arena_pump */
static int   a_pump_calls;               /* v38.137: how many full composites */
static int   a_present_ms;               /* v38.137: last forced full present */
static int   a_human_move;
static int   a_mouse_first;
static int   a_keys_down;
static int   a_last_input_ms;           /* last key or mouse event */
static int   a_rescue_logged;
/* v38.139: the look path, counted. `patah-patah` was reported with no way to
 * tell three very different things apart — the horn not reaching the game at
 * all (host window unfocused, so QEMU delivers nothing: this count is zero),
 * the horn reaching it too rarely (delivered from the desktop's idle loop
 * instead of from the frame loop: see desktop_capture_pump in kernel.c), or
 * the frames themselves being uneven. These two counters plus comp/s and comp_ms
 * separate them from one session's log instead of another round of guesses. */
static int   a_look_ev;                 /* mouse events delivered to this window */
static int   a_key_ev;                  /* key events delivered to this window */
/* v38.149: last reading of the guest's own packet counter (mouse.c), so the
 * perf line can print a RATE. pkts_s > 0 with look_s == 0 means the packets
 * arrive and the guest drops them; both zero means the host stopped sending.
 * That is the one question the 12:46 session could not answer. */
static unsigned a_pkt_last;
#define Q3ARENA_IDLE_MS     5000        /* nothing at all this long = idle */
#define Q3ARENA_MOUSE_CLAMP 300         /* counts in one event, host artefact guard */

static unsigned a_next_us;              /* next 25 ms render boundary (a_us) */
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
/* v38.155: the previous frame's measured work (vm+gl+other, idle excluded),
 * in microseconds, for the pacer's step-up decision. Written at the end of a
 * frame, read at the start of the next. */
static int pace_work;
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
static int    a_us_recon;               /* v38.126: the clock's own charge     */
static int    a_last_frag, a_last_vm_frag;          /* v38.126: shaded px  */
static int    a_last_prep_kc, a_last_emit_kc;       /* v38.126: world CPU  */
static int    a_tsc_khz;                /* v38.126: measured rdtsc rate       */
static unsigned long long a_tsc_cal0;   /* v38.126: rdtsc at the calibration
                                         * baseline, and the microsecond clock
                                         * sampled beside it               */
static unsigned a_tsc_cal_us0;
static unsigned a_win_ms;               /* v38.126: start of the fps window    */
static int    a_perf_detail;            /* v38.126: F3 -> the six-number panel */

/* v38.126: one-time TSC calibration, from the frame loop itself. The TSC
 * counters TinyGL keeps (tgl_cyc.h) are a ratio until someone says how many
 * cycles this machine's millisecond is — and under KVM that is the HOST's
 * rate, not something the guest can assume or read from a CPUID leaf it can
 * trust, so it gets measured. No busy wait: the baseline is armed on the first
 * frame and the rate falls out of the next frame that lands at least 200 ms
 * later (~six frames, well before the dev lines start printing at frame 100).
 *
 * Two details: the window is capped at 400 ms so the cycle count stays inside
 * 32 bits (a plain 32-bit divide, since this link has no __udivdi3), and a
 * window that overran just re-arms instead of producing a rate — on TCG the
 * virtual TSC jumps in irregular leaps and a "rate" measured across a stall
 * would be fiction. That is why the glms line prints tsc_khz=0 and -1 for its
 * microseconds rather than a number that only looks measured. */
static void a_tsc_calibrate(void) {
    /* v38.129: spelled out — this unit does not include q3world_render.h, so
     * an undeclared call would both warn and (returning 64 bits on i386)
     * mis-link the read. */
    extern unsigned long long q3w_tsc(void);
    unsigned long long t;
    unsigned us;
    if (a_tsc_khz > 0) return;
    t = q3w_tsc();
    us = (unsigned)timer_get_us();
    if (a_tsc_cal_us0 == 0) { a_tsc_cal0 = t; a_tsc_cal_us0 = us ? us : 1u; return; }
    {
        unsigned long long dc = t - a_tsc_cal0;
        unsigned du = us - a_tsc_cal_us0;
        if (du >= 200000u) {
            if (du <= 400000u) a_tsc_khz = (int)((unsigned)dc / du);
            a_tsc_cal0 = t; a_tsc_cal_us0 = us;
        }
    }
}

/* v38.118: camera interpolation. The server still simulates on its 50 ms tick
 * (Pmove untouched), but the RENDERED camera must not teleport once per tick:
 * at 44 fps that is 2-3 frames of frozen pose, then a ~16-unit jump - the
 * stutter the user measures as "patah-patah saat gerak, semua map". The frame
 * camera is lerped between the last two tick-end poses with the same frac-
 *tional phase the server accumulator holds, so the eye moves EVERY rendered
 * frame while G_RunFrame's tick grid stays exactly what a 20 Hz server makes. */
static vec3_t a_cam_prev_pos;           /* pose at the previous tick end */
static vec3_t a_cam_cur_pos;            /* pose at the latest tick end */
static int    a_cam_have;               /* pose pairs collected */
static int    a_last_cam_alpha;         /* for the sample log line */

static playerState_t *q3vm_ps(vm_t *vm) {
    if (!vm || vm_ps_ofs < 0) return 0;
    return (playerState_t *)(vm->dataBase + (vm_ps_ofs & vm->dataMask));
}

/* v38.142: the sound tap (v38.129) lived here and is gone with the mixer —
 * events need no port-side handling without ears. */

extern int get_win_index(int wid);

/* v38.131: the texture loader's live progress (q3world_render.c) — read by
 * the boot window's draw. Declared here because q3world_render.h pulls
 * q3bsp.h, whose include path this unit does not carry (same as q3w_set_sort
 * above). */
extern volatile int q3w_progress_stage;
extern volatile int q3w_progress_done;
extern volatile int q3w_progress_total;

/* v38.131: the boot window exists from the FIRST second of a launch, and until
 * the first game frame lands its content is a loading screen: what stage is
 * running (collision world -> textures N/M -> starting the game VM) and a
 * bar. Before this, a launch showed nothing but the terminal prompt for two
 * to four minutes on q3dm1 — read as "it died" by exactly everyone, and the
 * user's screenshots recorded it as a bug. After the game starts, the same
 * window carries q3ref_blit as before. */
static int a_boot_ready;               /* first game frame has been drawn */
static const char *a_load_stage = "opening the window";

/* v38.133: what the load costs, in milliseconds, and how to get OUT of it.
 *
 * The 14:51 session behind the second "qemu nya freeze" report was healthy in
 * the guest from the first serial byte to the last: the driver's own paint
 * witness counted 600 loading-screen paints, then 4124 game paints at ~20 fps,
 * and the process' log stopped mid-line the moment the window was closed (no
 * panic, no fatal, no OOM). What the user actually met was a wait with no exit.
 * `q3arena_open()` took the mouse capture BEFORE the 94-texture decode — so
 * the cursor vanished and stayed gone for the whole load — and the boot
 * window's key handler dropped every key, ESC included ("the user is still
 * typing in the terminal" was the reasoning; the effect was that the one key
 * that could have ended the wait did nothing). A stuck-looking window plus a
 * keyboard that answers nothing is a freeze from the outside, whatever the
 * serial log says.
 *
 * Two fields and a handful of call sites turn that into a load that answers:
 * a_ms() is stamped from the first instruction of q3_drive, so the loading
 * screen can count seconds out loud, and a_load_cancel is the one thing the
 * compositor-side key handler sets for the game task to see — read at every
 * stage boundary and once per texture, so ESC lands within a second whatever
 * the load is doing.
 *
 * a_load_phase is for the shell that forked this task (q3arena_status below):
 * 0 nothing, 1 loading, 2 in the game, 3 ended — and every exit path reports 3
 * because q3vm_park() is where they all end. */
static int a_load_t0;                  /* a_ms() at q3_drive() entry         */
static int a_load_t[4];                /* engine/collision/textures/vm marks */
static volatile int a_load_cancel;     /* ESC pressed on the loading screen  */
static volatile int a_load_phase;
/* v38.134: defined below the progress hook that uses it, and called from
 * q3_kernel.c's q3_fread as well, so it cannot be static. */
void q3arena_pump(void);

/* v38.133: the boot window's progress hook, registered only around q3arena_open()
 * and unregistered after. It is the window invalidate, nothing else — the
 * renderer reaches pixels only through q3w_progress_done, which this does not
 * touch — plus, since v38.133, the ESC answer: a non-zero return asks the
 * texture loop to stop (see q3world_render.h). */
static int q3arena_progress_hook(int done, int total) {
    static int last_inv_ms;
    (void)done; (void)total;
    if (a_win >= 0) {
        /* v38.138: one invalidate every 400 ms, not one per texture.
         *
         * The invalidate is what sets the idle loop's needs_redraw, and that
         * loop composites the WHOLE desktop: a 1024x768 draw plus a full-screen
         * copy into VGA memory, which QEMU then pushes to the player's window.
         * 86 textures arriving faster than 60 fps therefore kept the single
         * guest core compositing at 60 fps for a screen whose only moving parts
         * are a seconds counter and this bar — and every composite is host work
         * too. Measured: the same ISO that loads in 17 s here took 179 s on the
         * host that reported it, with the VM phase alone at 139 s against 3.4 s
         * (the loading screen changing once a second the entire time). */
        int now = a_ms();
        if (now - last_inv_ms >= 400) {
            last_inv_ms = now;
            wm_invalidate(a_win);
        }
        q3arena_pump();
    }
    return a_load_cancel ? 1 : 0;
}

/* v38.134: the loading screen's liveness while the loader owns the CPU.
 *
 * wm_invalidate() alone was not enough, and the reason is scheduling, not
 * drawing: the desktop is composited in the idle loop, and a kernel task that
 * never blocks (this one: ~10 s of Com_Init, then the whole VM/GAME_INIT
 * phase) outranks it the whole time it stays runnable. Screendumped pixel for
 * pixel, the boot window showed ZERO changed pixels for twelve seconds of
 * engine wait and again for the entire VM phase — which is exactly the wait
 * that was reported three times as "QEMU-nya freeze / gak bisa masuk game".
 * The invalidate was set the whole time; nothing was drawing it.
 *
 * So the loader asks for a composite itself, at the three points it owns: the
 * per-texture progress hook above, every engine file read (q3_kernel.c's
 * q3_fread) and every engine line printed (q3_vprintf — GAME_INIT talks a lot,
 * and that is exactly the stretch with no reads to hook).
 *
 * The rate is 2.5 Hz, and that number is the whole point: the first cut ran at
 * ~8 Hz and cost a measured 65% of the load (textures 17.9 -> 30.0 s, VM
 * 40.1 -> 73.7 s, total 67.7 -> 112.0 s on the same class of host) because
 * every pump is a full desktop composite — desktop, both windows, swap. At
 * 2.5 Hz the seconds counter and the sweep bar still read as motion, and the
 * cost is a couple of percent. desktop_pump() keeps the idle loop's own 60 fps
 * ceiling on top. */
void q3arena_pump(void) {
    extern void desktop_pump(void);
    extern void task_sleep(int ticks);
    static int last_ms;
    int now;
    if (a_boot_ready || a_win < 0) return;   /* the game presents itself */
    now = a_ms();
    /* v38.137: 2.5 Hz -> 1 Hz, and the cost is now measured instead of assumed.
     * The loading screen's only live elements are the seconds counter (whole
     * seconds by construction) and the texture bar, so a full desktop composite
     * 2.5 times a second bought nothing but load time — and the sessions that
     * reported it read the VM phase at 40 s (no pump yet), then 73, 82, 90, 106
     * and 139 s as the pump went in and the host stayed busy. That ballooning
     * IS the "loadingnya freeze" report: the wait grew while the screen was
     * still mostly still. Every pump is timed now and printed in the load line
     * as `pump=Nms/C`, so the next regression has a number instead of a guess. */
    if (now - last_ms < 1000) return;
    last_ms = now;
    {
        int t0 = now;
        desktop_pump();
        a_pump_ms += a_ms() - t0;
        a_pump_calls++;
    }
    /* v38.134: and hand the desktop loop a timeslice. desktop_pump() draws from
     * THIS task, but the frame it draws still has to be swapped by the idle
     * loop next time that loop runs — and while this task never blocks, it is
     * the task the scheduler keeps picking. Measured on the session that
     * produced the frozen screenshot: the boot window took exactly 368 paints
     * and then not one more for the rest of the load, the taskbar clock stayed
     * at 22:16, and the shell's `[quake]` poll printed zero times. 1 ms per
     * pump is a rounding error on a 100-second load. */
    task_sleep(1);
}

/* v38.133: decimal into a caller's buffer. Text composed on the compositor side
 * cannot go through reportf (that scratch buffer belongs to the frame task),
 * and the loading screen has to print numbers. Returns chars written; a
 * negative or absurd value is clamped rather than run off the end. */
static int q3arena_put_int(char *b, int v) {
    char t[12];
    int n = 0, i = 0;
    if (v < 0) v = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0 && n < 12) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) b[i++] = t[--n];
    return i;
}
/* the one window's geometry, computed once at boot and reused by the late
 * phase's canonical window line (the suites parse that exact shape) */
static int a_win_x, a_win_y, a_win_w, a_win_h;

/* v38.132: `buf` is uint32_t, not char.
 *
 * It was declared char* while doing uint32 pointer arithmetic on it
 * (`line = buf + (by+row)*bw`), so every glyph row landed at byte offset
 * (by+row)*bw instead of pixel row (by+row) — i.e. 4x too high and sheared
 * 80 px right per row. All boot text smeared into ~4-row diagonal fragments:
 * the title meant for y=60 showed at y~15, the stage meant for y=120 at y~30,
 * the counter meant for y=170 at y~42. Measured pixel-for-pixel off a live
 * screendump before fixing. The loading screen's words — the one thing meant
 * to say "not frozen" — were illegible the whole time, which is most of why
 * the wait read as a hang. One type change; the arithmetic was already right
 * for a pixel pointer. */
static void bootwin_text(uint32_t *buf, int bw, int bx, int by,
                         const char *s, uint32_t col) {
    for (int k = 0; s[k] && (bx + 8 * (k + 1)) < bw - 4; k++) {
        unsigned char c = (unsigned char)s[k];
        const unsigned char *g = font8x16_data[c];
        for (int row = 0; row < 16; row++) {
            uint32_t *line = buf + (size_t)(by + row) * bw;
            unsigned bits = g[row];
            for (int bit = 0; bit < 8; bit++)
                if (bits & (0x80u >> bit))
                    line[bx + 8 * k + bit] = col;
        }
    }
}

static void q3arena_boot_draw(int cw, int ch) {
    const uint32_t bg    = 0x00141418;
    const uint32_t bar   = 0x00D08A2A;   /* Q3 amber */
    const uint32_t frame = 0x003A3A44;
    const uint32_t txt   = 0x00E8E2D0;
    int idx = get_win_index(a_win);
    uint32_t *buf;
    if (idx < 0) return;
    buf = wm_wins[idx].content_buffer;
    if (!buf) return;

    for (int y = 0; y < ch; y++) {
        uint32_t *line = buf + (size_t)y * cw;
        for (int x = 0; x < cw; x++) line[x] = bg;
    }
    for (int x = 0; x < cw; x++) {
        buf[x] = frame; buf[(ch - 1) * cw + x] = frame;
    }
    for (int y = 0; y < ch; y++) {
        buf[y * cw] = frame; buf[y * cw + cw - 1] = frame;
    }
    bootwin_text(buf, cw, 16, ch / 2 - 60,
                 "Quake III (qagame VM)", txt);
    bootwin_text(buf, cw, 16, ch / 2 - 40,
                 "Mectov OS — id Software's own game code", 0x00909098);
    /* v38.132: say the quiet part out loud. A 91%-black window for ~20 s reads
     * as frozen even with a moving bar — the report that started this release
     * was exactly that. One explicit prefix on the stage line fixes it, with
     * no layout change: 14 chars at 8 px = 112, so the stage keeps its amber
     * colour starting at bx 128. */
    bootwin_text(buf, cw, 16, ch / 2, "please wait - ", 0x00909098);
    bootwin_text(buf, cw, 16 + 14 * 8, ch / 2, a_load_stage, bar);
    {
        int bx = 16, by = ch / 2 + 30, bw2 = cw - 32, bh = 12;
        if (q3w_progress_stage == 2 && q3w_progress_total > 0) {
            int fill = (int)((long long)bw2 * q3w_progress_done /
                             q3w_progress_total);
            for (int y = 0; y < bh; y++) {
                uint32_t *line = buf + (size_t)(by + y) * cw;
                for (int x = 0; x < bw2; x++)
                    line[bx + x] = (x < fill) ? bar : 0x00242428;
            }
            {
                char l4[48];
                int n = 0, v = q3w_progress_done;
                const char *s = "textures ";
                for (int k = 0; s[k] && n < 40; k++) l4[n++] = s[k];
                l4[n++] = '0' + (v / 100) % 10; l4[n++] = '0' + (v / 10) % 10;
                l4[n++] = '0' + v % 10; l4[n++] = '/';
                v = q3w_progress_total;
                l4[n++] = '0' + (v / 100) % 10; l4[n++] = '0' + (v / 10) % 10;
                l4[n++] = '0' + v % 10;
                l4[n] = 0;
                bootwin_text(buf, cw, 16, ch / 2 + 50, l4, txt);
            }
        } else {
            static int sweep;
            sweep = (sweep + 4) % (bw2 > 60 ? bw2 - 40 : 1);
            for (int y = 0; y < bh; y++) {
                uint32_t *line = buf + (size_t)(by + y) * cw;
                for (int x = 0; x < bw2; x++)
                    line[bx + x] = (x >= sweep && x < sweep + 40) ? bar : 0x00242428;
            }
        }
    }
    /* v38.133: a clock and an exit, the two things the load was missing. The
     * sweep above animates, but the collision stage has no granularity at all
     * (one id call, tens of seconds) and a bar that moves while nothing else
     * does is still a still life on a slow host — a SECOND COUNTER cannot be
     * mistaken for a hang. The hint names the one key this screen answers:
     * before v38.133 the boot handler dropped every key including ESC, which
     * is exactly what the user tried. */
    {
        char el[40];
        int n = 0, secs = a_ms_since(a_load_t0) / 1000;
        const char *s = "loading ";
        while (*s && n < 30) el[n++] = *s++;
        n += q3arena_put_int(el + n, secs);
        el[n++] = 's';
        el[n] = '\0';
        bootwin_text(buf, cw, 16, ch / 2 + 70, el, txt);
    }
    bootwin_text(buf, cw, 16, ch / 2 + 90,
                 a_load_cancel ? "cancelling at the next stage..."
                               : "ESC: cancel load, back to the desktop",
                 0x00909098);
}

/* v38.132: what the window is ACTUALLY painting, in the log.
 *
 * The renderer log proves frames are drawn into buffers; it cannot prove what
 * reaches the screen. This callback runs on the compositor side, so it is the
 * only witness that matters — and it is the place a stuck window would show
 * up: boot-branch paints after the flip, or no paints at all.
 *
 * Two line shapes, both rate-limited. Transitions always print (boot->game is
 * the flip; game->boot afterwards would be the smoking gun it never was, so
 * far). Otherwise one heartbeat per 600 paints with both counters. Composed
 * into a local buffer and written once: this runs on the compositor task while
 * reportf's shared buffer belongs to whoever gets there first. */
static int wdraw_boot_n, wdraw_game_n, wdraw_last_boot = -1;
static void q3arena_win_draw(int id, int cx, int cy, int cw, int ch) {
    int idx = get_win_index(id);
    (void)cx; (void)cy;
    if (idx < 0) return;
    if (wm_wins[idx].resizing) return;
    if (!wm_wins[idx].content_buffer) return;
    {
        int is_boot = !a_boot_ready;
        if (is_boot != wdraw_last_boot) {
            static char tb[64];
            int p = 0;
            const char *s = "[Q3ARENA] win_draw: now painting ";
            while (*s && p < 40) tb[p++] = *s++;
            s = is_boot ? "BOOT (loading screen)" : "GAME (3D world)";
            while (*s && p < 60) tb[p++] = *s++;
            tb[p++] = '\n'; tb[p] = 0;
            write_serial_string(tb);
            wdraw_last_boot = is_boot;
        }
        if (is_boot) wdraw_boot_n++; else wdraw_game_n++;
        if ((wdraw_boot_n + wdraw_game_n) % 600 == 0) {
            static char hb[96];
            int p = 0, v;
            const char *s = "[Q3ARENA] win_draw paints: boot=";
            while (*s && p < 60) hb[p++] = *s++;
            v = wdraw_boot_n;
            if (v >= 100000) { hb[p++] = '0' + (v / 100000) % 10; }
            if (v >= 10000)  { hb[p++] = '0' + (v / 10000) % 10; }
            if (v >= 1000)   { hb[p++] = '0' + (v / 1000) % 10; }
            hb[p++] = '0' + (v / 100) % 10;
            hb[p++] = '0' + (v / 10) % 10;
            hb[p++] = '0' + v % 10;
            s = " game=";
            while (*s && p < 80) hb[p++] = *s++;
            v = wdraw_game_n;
            if (v >= 100000) { hb[p++] = '0' + (v / 100000) % 10; }
            if (v >= 10000)  { hb[p++] = '0' + (v / 10000) % 10; }
            if (v >= 1000)   { hb[p++] = '0' + (v / 1000) % 10; }
            hb[p++] = '0' + (v / 100) % 10;
            hb[p++] = '0' + (v / 10) % 10;
            hb[p++] = '0' + v % 10;
            hb[p++] = '\n'; hb[p] = 0;
            write_serial_string(hb);
        }
    }
    if (!a_boot_ready) { q3arena_boot_draw(cw, ch); return; }
    q3ref_blit(wm_wins[idx].content_buffer, cw, ch);
}

/* v38.135: input is also a request to be SEEN.
 *
 * The window raises itself twice — when the loading screen opens and when the
 * first game frame appears — but the terminal the command was typed in is one
 * click away from the front, and that click is exactly what a user does to
 * focus the VM window. The mouse stays under the game's capture and the
 * scancodes keep reaching this window, so the session carries on at full rate
 * into a window nobody can see: the report is "the game never showed up",
 * while the log counts thousands of frames and rising paint heartbeats (this
 * is the half of the v38.133 report that was NOT the loading screen being
 * static). The user's own words when asked what the window showed: a
 * terminal, with no 3D anywhere on it.
 *
 * So every key and every mouse event asks for the window to come forward
 * again. The request is a flag, not a call: the key handler runs in the
 * compositor's context, and restacking the window list from inside a paint is
 * not something the WM promises to survive. The frame loop — the task that
 * already raises the window at game start — consumes it, throttled to twice a
 * second so holding a movement key does not restack on every event. */
static int a_raise_wanted;
static int a_raise_last_ms;
static int a_raise_logged;              /* the first re-raise is the marker */
static void q3arena_note_input(void) {
    int now;
    /* The loading screen raises itself and answers only ESC: while the boot
     * window owns the screen there is nothing to bring forward. */
    if (a_win < 0 || !a_boot_ready) return;
    if (a_raise_wanted) return;
    now = a_ms();
    if (a_raise_last_ms && now - a_raise_last_ms < 500) return;
    a_raise_last_ms = now;
    a_raise_wanted = 1;
}

/* The WM delivers raw scancodes to this window (wm_request_scancodes), press
 * and release alike, which is the only way to hold a movement key. */
static void q3arena_win_key(int id, char c, uint8_t sc) {
    uint8_t base = sc & 0x7F;
    int down = !(sc & 0x80);
    (void)id; (void)c;
    if (base >= 128) return;
    /* v38.131: during the boot/loading phase the window takes NO input at
     * all — the user is still typing in the terminal, and a swallowed key
     * (worst case ESC) would hit nothing but the driver's state arrays. */
    /* v38.133: the loading screen answers exactly one key, and it is the one
     * the user presses when a load looks stuck. Non-ESC keys stay ignored —
     * they cannot be movement (there is no game yet) and the terminal may
     * still want them — but ESC sets the cancel flag the game task reads at
     * every stage boundary and once per texture. Writes here come from the
     * compositor's context, so the log line goes out with write_serial_string,
     * like the ESC-at-quit branch below. */
    if (!a_boot_ready) {
        if (base == SC_ESC && down && !a_load_cancel) {
            a_load_cancel = 1;
            write_serial_string("[Q3ARENA] load cancel requested (ESC)\n");
            if (a_win >= 0) wm_invalidate(a_win);
        }
        return;
    }
    /* v38.136: taking the controls is INTENT — a movement key or a click. ESC
     * (quit) and F3 (perf panel) are commands, not steering, and mouse motion
     * is not intent either; none of them may end the walking tour. */
    if (down && base != SC_ESC && base != SC_F3) a_human_move = 1;
    if (down != a_keys[base]) a_keys_down += down ? 1 : -1;
    a_key_ev++;
    a_idle_frames = 0;      /* v38.149: see a_idle_frames above */
    a_last_input_ms = a_ms();
    a_keys[base] = (unsigned char)down;
    a_input_seen = 1;
    q3arena_note_input();
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
    /* Split-brain guard: capture survives focus loss, so this handler can
     * fire for a window that no longer owns input. Drop before touching any
     * intent state (a_btn/a_human_move/look counters), or an unfocused click
     * flips the tour and fire state. */
    if (wm_capture_owner() != a_win) return;
    /* v38.122: button bit 0 = left = BUTTON_ATTACK, press and release alike
     * (the kernel main loop only calls wm_capture_event when the button state
     * CHANGES or the pointer moved, so this is edge-correct). */
    if (btn & 1) a_btn |=  Q3VM_BUTTON_ATTACK;
    else         a_btn &= ~Q3VM_BUTTON_ATTACK;
    /* v38.136: a click is intent (fire). Motion is not — a click on this window
     * to focus it ARRIVES as motion, and treating that as a takeover is what
     * killed the tour and left the camera on the floor. */
    if (btn) a_human_move = 1;
    a_look_ev++;
    a_idle_frames = 0;      /* v38.149: see a_idle_frames above */
    a_last_input_ms = a_ms();
    a_input_seen = 1;
    q3arena_note_input();
    if (a_mouse_first) {
        /* v38.136: the pointer's travel to this window, not aiming. Dropping it
         * is the difference between capturing the mouse and yanking the view.
         * Measured before this guard: one event, pitch 0 -> 82. */
        a_mouse_first = 0;
        return;
    }
    /* A single event past this is a host artefact too, not a flick. */
    if (dx >  Q3ARENA_MOUSE_CLAMP) dx =  Q3ARENA_MOUSE_CLAMP;
    if (dx < -Q3ARENA_MOUSE_CLAMP) dx = -Q3ARENA_MOUSE_CLAMP;
    if (dy >  Q3ARENA_MOUSE_CLAMP) dy =  Q3ARENA_MOUSE_CLAMP;
    if (dy < -Q3ARENA_MOUSE_CLAMP) dy = -Q3ARENA_MOUSE_CLAMP;
    q3vm_cmd_yaw   -= (float)dx * q3arena_sens_value();
    /* v38.118 sign fix: the driver feeds id's REAL qagame, whose pitch is
     * positive-DOWN (AngleVectors: forward[2] = -sin(pitch)), and the mouse
     * driver delivers +dy for downward motion (mouse.c: "PS/2 +y is up;
     * screen space is +y down"). Down-mouse must therefore ADD to pitch —
     * the old minus made every look direction vertical-inverted, exactly
     * the "mouse ke bawah, gamenya nengok ke atas" the user reported. */
    q3vm_cmd_pitch += (float)dy * q3arena_sens_value();
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
    /* v38.128: the sky pass is a property of the renderer, not of the map, so it
     * is set once per run here rather than per frame. `nosky` turns it off and
     * the sky faces go back to being drawn as ordinary geometry — the A/B this
     * release is measured against. */
    q3ref_set_sky(a_sky);

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

/* v38.131: the boot-phase window. Opened BEFORE the world load, it is the
 * loading screen: title, stage line and progress bar (q3arena_boot_draw).
 * Without it a q3dm1 launch shows nothing but the terminal prompt for two to
 * four minutes — the user's screenshots recorded exactly that as "the game
 * never appears". The final geometry is known up front, so the very window
 * that shows the loading screen is the one the game later renders into; the
 * late phase only raises it and takes the input routes. */
static void q3arena_win_tick(int id) {
    (void)id;
    if (a_boot_ready) return;
    /* pulse the boot screen while the stages run (the stage line changes
     * text, the sweep/percent moves) */
    if (a_win >= 0) wm_invalidate(a_win);
}

static void q3arena_boot_window_open(void) {
    int ww, wh, wx, wy;
    extern uint32_t fb_width, fb_height;
#ifndef Q3ARENA_SCALE
#define Q3ARENA_SCALE 1
#endif
#if Q3ARENA_SCALE > 1
    q3ref_set_blit_scale(Q3ARENA_SCALE);
    q3w_set_sort(a_sort);   /* v38.121: `nosort` restores the v38.120 order */
#endif
    ww = Q3ARENA_W * Q3ARENA_SCALE + 2;
    wh = Q3ARENA_H * Q3ARENA_SCALE + TITLEBAR_H + 2;
    wx = ((int)fb_width - ww) / 2;  if (wx < 0) wx = 0;
    wy = ((int)fb_height - TASKBAR_H_PX - wh) / 2; if (wy < 0) wy = 0;
    a_win_x = wx; a_win_y = wy; a_win_w = ww; a_win_h = wh;

    a_boot_ready = 0;
    a_load_stage = "collision world";
    a_win = wm_open(wx, wy, ww, wh, "Quake III — official qagame VM",
                    q3arena_win_draw, q3arena_win_key, q3arena_win_tick,
                    q3arena_win_mouse);
    if (a_win < 0) {
        write_serial_string("[Q3ARENA] boot window: wm_open failed, "
                            "continuing without one\n");
        return;
    }
    /* v38.133: the raw-scancode route is taken HERE, not at game start, so the
     * loading screen can hear ESC from its first second. Focus decides where
     * the keys actually go (wm_scancode_focus): with the boot window in front
     * they arrive at the handler above, and clicking the terminal — which does
     * not want scancodes — hands them back to the shell. The MOUSE is the half
     * that is deliberately NOT taken here: capturing it for a two-minute load
     * is what made the cursor disappear with no game on screen, so that stays
     * at game start (see the frame loop). */
    wm_request_scancodes(a_win, 1);
    if (a_load_phase == 0) { a_load_phase = 1; a_load_t0 = a_ms(); }
    write_serial_string("[Q3ARENA] load screen: ESC cancels (scancodes routed)\n");

    /* v38.110: perf accounting, unchanged from the old open path. */
    wm_tag_q3_game(a_win);
    reportf("[Q3ARENA] boot window id=0x%x rect=%d,%d %dx%d content=%dx%d "
            "title=\"Quake III — official qagame VM\"",
            (unsigned)a_win, wx, wy, ww, wh, Q3ARENA_W, Q3ARENA_H);
    /* v38.133: the loading screen comes to the front as it opens. Until now
     * the window could sit UNDER the terminal it was launched from — the late
     * phase raised it, but the load is the part the user is watching. */
    wm_raise(a_win);
    wm_invalidate(a_win);
}

static void q3arena_open(void) {
    /* v38.142: the sound bridge (v38.129) lived here and is gone with the
     * mixer — open proceeds straight to the renderer. */
    if (q3ref_init(Q3ARENA_W, Q3ARENA_H) != 0) {
        write_serial_string("[Q3ARENA] FATAL: TinyGL renderer init failed\n");
        return;
    }
    write_serial_string("[Q3ARENA] TinyGL renderer ready w=");
    vm_ser_int(Q3ARENA_W);
    write_serial_string(" h=");
    vm_ser_int(Q3ARENA_H);
    write_serial_string("\n");

    /* v38.131: the window itself opened at boot — it has been the loading
     * screen this whole time. This is the moment the first frame is about to
     * exist: raise it above the terminal and print the canonical line.
     * v38.133: and NOTHING else. This is where the mouse capture and the
     * scancode request used to be taken — i.e. before q3arena_world_mesh()
     * below, the 94-texture decode, the longest stage of the load — so for
     * two minutes the cursor was gone and every key was swallowed while there
     * was still no game to play. The scancodes now come from the boot window's
     * open (the loading screen answers ESC) and the mouse capture from the
     * first game frame (see the frame loop). */
    if (a_win >= 0) {
        wm_raise(a_win);
        /* The canonical window line, byte-identical to the pre-v38.131 shape
         * (the suites' RECT_RE parses every field). The boot window's own
         * line above says when it opened; this one says the game is about
         * to draw into it. */
        reportf("[Q3ARENA] window id=0x%x rect=%d,%d %dx%d content=%dx%d "
                "title=\"Quake III — official qagame VM\"",
                (unsigned)a_win, a_win_x, a_win_y, a_win_w, a_win_h,
                Q3ARENA_W, Q3ARENA_H);
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

/* q3vm_park() is defined far below, next to the reset shim; every driver exit
 * goes through it (it never returns, and since v38.133 it also reports the
 * session's end to the shell's status poll). */
static void q3vm_park(void);

/* v38.133: leave the load early. Two shapes, because two very different
 * amounts of state exist at the cancel points: before the renderer is built
 * there is nothing but the window, and after it the renderer, the mesh, the
 * cache and the SB16 mixer are all up — where the ordinary teardown is exactly
 * the right thing. Never returns: every exit from this driver parks, and the
 * park is also what tells the shell's progress poll (q3arena_status) that the
 * session is over. */
static void q3arena_load_cancelled(int have_renderer) {
    write_serial_string("[Q3ARENA] load cancelled (ESC) — ");
    write_serial_string(have_renderer ? "releasing the renderer and the window\n"
                                      : "closing the loading screen\n");
    reportf("[Q3ARENA] load cancelled: stage='%s' at %dms (engine=%dms "
            "collision=%dms)",
            a_load_stage, a_ms_since(a_load_t0),
            a_load_t[0], a_load_t[1] - a_load_t[0]);
    if (have_renderer) {
        q3arena_close();
        q3bsp_free(&a_mesh);
    } else if (a_win >= 0) {
        wm_capture_mouse(a_win, 0);
        wm_request_scancodes(a_win, 0);
        wm_close(a_win);
        a_win = -1;
    }
    write_serial_string("[Q3ARENA] cancelled: back to the desktop\n");
    q3vm_park();
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

    /* v38.124: the view model loads before this frame's clock starts. */
    q3arena_viewmodel(vm);

    int f_t0 = a_us();
    /* v38.116: the pixel histogram and the culling counters are read once per
     * SAMPLED frame (frame % 20), not every frame — on q3dm1 the histogram
     * alone was 7.4 ms of "other" per frame at 20 fps, i.e. ~1.5 fps of pure
     * bookkeeping. Locals now keep their LAST sampled values so the lines
     * emitted in the sample block stay byte-identical with the old shape. */
    /* v38.139 correction: `sample` gates the HISTOGRAM only. The pose and
     * pixels lines are NOT spaced by it — they live in the `(frame % 100) == 0`
     * perf block below. Verified two ways: by brace-matching this file (the pose
     * reportf's enclosing openers are q3arena_frame, `if ((frame % 100) == 0)`
     * and `if (ps)` — `if (sample)` is not among them), and off the player's own
     * session, where every pose line, every pixels line and every perf line is
     * exactly 100 frames after the last one. The v38.147 note that used to sit
     * here claimed "sampled every 40th frame (was 20th)"; no such change was
     * applied and it would not have touched those lines if it had. */
    int sample = (frame % 20) == 0;

    /* --- input -> usercmd ------------------------------------------------ */
    /* v38.139: take the mouse packet HERE, once per rendered frame.
     *
     * The camera reads its view angle live (v38.143): `delta_angles +
     * q3vm_cmd_yaw`, and q3vm_cmd_yaw only moves in q3arena_win_mouse. So the
     * aim moves exactly as often as that handler runs — and until now the only
     * caller of the drain was the desktop's idle loop, which has no relationship
     * to this loop at all. A slow mouse turn then arrived in clumps whenever the
     * compositor happened to get a timeslice: the "nengok pelan pake mouse patah
     * patah" report. desktop_capture_pump() is atomic (see kernel.c), so the
     * desktop loop keeps its own call and neither caller can lose a packet.
     * Doing it before the usercmd is built means the tick that consumes it sees
     * the newest angle, not one up to a frame old. */
    {
        extern int desktop_capture_pump(void);
        desktop_capture_pump();
    }
    {
        int fm = 0, rm = 0;
        if (a_keys[SC_W] || a_keys[SC_UP]) fm += 127;
        if (a_keys[SC_S] || a_keys[SC_DOWN]) fm -= 127;
        if (a_keys[SC_D]) rm += 127;
        if (a_keys[SC_A]) rm -= 127;
        if (!a_human_move) {
            /* v38.132: demo mode — forward, plus a turn if either detector has
             * queued one (see the a_ad_* statics). Neither ever fights a human:
             * this whole branch stops existing the moment the controls are
             * taken. v38.136: it is INTENT that takes them (a movement key or a
             * click) — a focus click counting as a takeover is what stopped the
             * tour and left a static near-black frame on screen. */
            fm = 127;
            if (a_ad_pending_turn) {
                q3vm_cmd_yaw += (float)a_ad_pending_turn;
                a_ad_pending_turn = 0;
            } else if (a_ad_peak >= Q3ARENA_AD_PEAK &&
                       a_ad_drawn * Q3ARENA_AD_COLLAPSE < a_ad_peak &&
                       ++a_ad_blind >= Q3ARENA_AD_BLIND_FRAMES) {
                /* The view has collapsed to a fraction of what this level was
                 * showing a moment ago, and has stayed that way: the camera is
                 * against a wall. Turn and keep walking — an empty-looking frame
                 * is the one that reads as "the game isn't running". */
                a_ad_pending_turn = (a_ad_turns & 1) ? -90 : 90;
                a_ad_turns++;
                a_ad_blind = 0;
            }
        } else if (!a_keys_down && !a_btn && a_last_input_ms &&
                   a_ms() - a_last_input_ms > Q3ARENA_IDLE_MS &&
                   ++a_idle_frames >= Q3ARENA_AD_BLIND_FRAMES) {
            /* v38.137: attract mode. Nothing held and nothing moved for five
             * seconds means the picture has stopped changing, and a picture
             * that stops changing has now been reported as a freeze five times
             * — including once from a player who had simply stopped pressing
             * keys. So the tour takes the camera back, and the next key or
             * click takes it right back again. v38.136 tested for a collapsed
             * view here as well; that turned out to be about the wrong thing
             * (where the player stands, not whether the screen is alive), and
             * it silently did nothing for a player parked at spawn looking at a
             * perfectly good wall. Holding a key, or moving the mouse, is
             * enough to keep this from ever firing.
             *
             * v38.149: `a_idle_frames`, not `a_ad_blind`. The two versions
             * before this one counted into the collapse detector's counter,
             * which the per-frame bookkeeping zeroes on any frame with a
             * healthy view — i.e. on every frame of exactly the case this
             * rescue exists for. It had never fired. It counts its own frames
             * now, and only input clears it (see the two handlers below). */
            a_human_move = 0;
            a_idle_frames = 0;
            a_ad_blind = 0;
            a_ad_stuck = 0;
            if (!a_rescue_logged) {
                a_rescue_logged = 1;
                write_serial_string("[Q3ARENA] tour resumed: no input for 5s "
                                    "(the screen had stopped changing)\n");
            }
        }
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

    /* v38.132: did the demo's own forward walk actually get anywhere?
     *
     * Read AFTER the VM call so it judges the position the module just
     * produced, not last frame's. Only while `!a_input_seen`: a human who stops
     * moving on purpose must not have the camera spun out from under them.
     *
     * Alternating turn direction matters — turning the same way every time
     * walks the player around the wall in a tight circle, which looks like the
     * same freeze with a moving texture. */
    if (!a_human_move && ps && a_srv_ticks) {
        float dxp = ps->origin[0] - a_ad_lastx;
        float dyp = ps->origin[1] - a_ad_lasty;
        if (dxp * dxp + dyp * dyp < Q3ARENA_AD_STEP * Q3ARENA_AD_STEP) {
            if (++a_ad_stuck >= Q3ARENA_AD_STUCK) {
                a_ad_pending_turn = (a_ad_turns & 1) ? -90 : 90;
                a_ad_turns++;
                a_ad_stuck = 0;
                /* Re-anchor, so the turn is not itself read as "no progress"
                 * on the very next tick and the counter starts clean. */
                a_ad_lastx = ps->origin[0];
                a_ad_lasty = ps->origin[1];
            }
        } else {
            a_ad_stuck = 0;
            a_ad_lastx = ps->origin[0];
            a_ad_lasty = ps->origin[1];
        }
    }

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
        }
        if (a_srv_ticks) {
            VectorCopy(a_cam_cur_pos, a_cam_prev_pos);
            VectorCopy(ps->origin, a_cam_cur_pos);
            a_srv_ticks = 0;
        }
        {
            float alpha = (float)a_srv_acc * (1.0f / (float)Q3VM_FRAMETIME);
            vec3_t e;
            if (alpha < 0.0f) alpha = 0.0f;
            if (alpha > 1.0f) alpha = 1.0f;
            e[0] = a_cam_prev_pos[0] + (a_cam_cur_pos[0] - a_cam_prev_pos[0]) * alpha;
            e[1] = a_cam_prev_pos[1] + (a_cam_cur_pos[1] - a_cam_prev_pos[1]) * alpha;
            e[2] = a_cam_prev_pos[2] + (a_cam_cur_pos[2] - a_cam_prev_pos[2]) * alpha;
            e[2] += (float)ps->viewheight;
            {
                /* Reuse the module's own AngleVectors through a temp angle
                 * vector - no freestanding trig, identical convention.
                 * v38.143: the view direction is LIVE, not lerped. The lerp
                 * above sampled the last two tick snapshots, so the eye sat
                 * ~one tick behind the mouse even between renders: smooth
                 * but late (the "lag saat noleh" report). Aim is free —
                 * only the sim position must stay on the tick rails — so
                 * the direction tracks the pending command (what the next
                 * tick will consume) plus the module's delta_angles (spawn /
                 * teleporter snaps, same sum Pmove applies). Mouse idle:
                 * live == tick angles, pixels byte-identical to before. */
                vec3_t va;
                float live_yaw = (float)SHORT2ANGLE(ps->delta_angles[YAW]) +
                                 q3vm_cmd_yaw;
                float live_pitch = (float)SHORT2ANGLE(ps->delta_angles[PITCH]) +
                                   q3vm_cmd_pitch;
                while (live_yaw > 180.0f) live_yaw -= 360.0f;
                while (live_yaw < -180.0f) live_yaw += 360.0f;
                if (live_pitch > 85.0f) live_pitch = 85.0f;
                if (live_pitch < -85.0f) live_pitch = -85.0f;
                va[YAW] = live_yaw;
                va[PITCH] = live_pitch;
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
        a_tsc_calibrate();      /* v38.126: no-op after the first ~6 frames */
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
            /* v38.127: id's own status bar rides the same buffer, drawn last so
             * it sits on top of the world and the gun. Every value is the
             * module's own playerState — the reads the retail server makes —
             * and `firing` is id's exact expression for the grey ammo field,
             * weaponstate == WEAPON_FIRING && weaponTime > 100 (cg_draw.c:625).
             * Like the perf panel this is painted AFTER the frame histogram
             * read above it, so the suites' pixel evidence stays the 3D pass;
             * `nohud` is the A/B knob. */
            if (a_hud) {
                extern void q3ref_set_hud(int, int, int, int, int, int, int);
                extern void q3ref_draw_hud(void);
                q3ref_set_hud(
                    (int)ps->stats[Q3VM_STAT_HEALTH],
                    (int)ps->stats[Q3VM_STAT_ARMOR],
                    (int)ps->ammo[ps->weapon],
                    (int)ps->weapon,
                    (int)ps->persistant[Q3VM_PERS_SCORE],
                    ps->weaponstate == 3 /* WEAPON_FIRING, bg_public.h */ &&
                        ps->weaponTime > 100,
                    q3vm_frame_time);
                q3ref_draw_hud();
            }
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
            /* v38.139: NO time gate here. v38.138 tried one (40 ms):
             * with the sim stepping at 50 ms the 40 ms present grid beat
             * against it and the game juddered ("patah-patah") while the
             * log showed a locked 20 Hz sim with clean audio (skip=0
             * drop=0). Presenting every rendered frame (~40 Hz on KVM) is
             * an even ~2:1 over the sim with no beat, and under the SDL
             * display backend it costs nothing measurable (main thread
             * ~6 % vs ~96 % under gtk). */
            if (a_win >= 0 && !a_fullscreen) {
                /* v38.150: the game presents its OWN frame, instead of leaving
                 * the swap to the desktop loop and hoping it gets there before
                 * the next frame. Measured on this exact session: the renderer
                 * held an even 40 Hz (fps=40, idle_ms=1696 per 100 frames =
                 * 17 ms of a 25 ms frame spent waiting, ~9 ms of real work),
                 * while the desktop loop's capture branch reached the swap only
                 * 29-34 times a second (pres_fps, comp_s). Ten rendered frames a
                 * second were therefore never shown, and the picture advanced in
                 * one-or-two-frame steps — the "fps stays 40 but the camera goes
                 * patah-patah" report. It is not a vblank beat: wait_for_vsync()
                 * is a no-op in this port (0x3DA polling is disabled under
                 * QEMU) and measured 10 us. It is also not the input path: the
                 * view angle is live (v38.143) and drained once per rendered
                 * frame (v38.139).
                 *
                 * desktop_present_window() takes the present lock, so the
                 * desktop loop cannot be inside the back buffer at the same
                 * time; if it loses the race it does nothing, and the invalidate
                 * below still stands so the window is correct a moment later
                 * either way. The cost is the composite that was happening
                 * anyway (~3 ms, comp_ms), moved from the desktop loop's
                 * schedule onto this frame — and this frame has ~9 ms of slack. */
                extern int desktop_present_window(int id);
                /* v38.150: invalidate FIRST, present second — in that order, and
                 * unconditionally. The app draw (q3arena_win_draw, which copies
                 * this frame's snapshot into the content buffer) only runs when
                 * buffer_dirty is set, and only wm_invalidate sets it. The first
                 * cut did it backwards — present, invalidate only on failure —
                 * and the content buffer then never saw a new frame: the driver
                 * reported "in the game" while the window sat on the loading
                 * screen, exactly the v38.118 failure this comment block sits
                 * under ("repainted its own buffer forever while the screen
                 * showed it at mouse/HUD cadence"). The harness never caught it
                 * because its constant mouse motion kept invalidating the window
                 * through the input path. The desktop loop may additionally
                 * composite the same fresh content; that is wasted CPU (~2 ms),
                 * not a wrong picture. */
                wm_invalidate(a_win);
                extern int desktop_present_window(int id);
                desktop_present_window(a_win);
            }
        }
        a_us_other += a_us_since(t0);
    }

    a_frames++;
    a_faces_drawn += drawn;
    a_tris_drawn += tris;

    /* v38.154: PER-FRAME present-to-present interval, and a log line for the
     * outliers only. Every measurement so far has been a 100-frame AVERAGE,
     * and an average is exactly the wrong instrument for "tersendat-sendat":
     * 19 frames at 50 ms plus one at 200 ms still averages 52 ms and reads as
     * "20 fps, mulus", while the picture visibly stalls on that one frame. So
     * log a frame whenever its own interval breaks the pattern — >1.6x the
     * running median, or simply long enough to be seen (70 ms at the 50 ms
     * grid, 40 ms at the 25 ms one). Sparse by construction: a smooth session
     * prints nothing, a stuttering one prints exactly the frames that hurt.
     * The running median (not the mean) is the reference because it is what
     * the eye adapts to; it is updated every frame but only *logged* when the
     * frame is an outlier. */
    {
        static unsigned h_last_us = 0;
        static unsigned h_median = 50000;   /* starts at the 50 ms grid */
        static unsigned h_short[8];
        static int h_n = 0, h_reported = 0;
        /* Snapshot of the phase accumulators at the END of the previous frame.
         * Deltas from here to the next frame are that frame's OWN work, which
         * is the number a 100-frame average can never give you. */
        static int pv = 0, pg = 0, po = 0, pi = 0;
        int cv = a_us_vm, cg = a_us_gl, co = a_us_other, ci = a_us_idle;
        unsigned now_us = (unsigned)a_us();
        if (h_last_us) {
            unsigned iv = now_us - h_last_us;
            int dv = cv - pv, dg = cg - pg, do_ = co - po, di = ci - pi;
            int work = dv + dg + do_ + di;
            int i;
            /* v38.155: publish this frame's real work for the next frame's
             * pacer decision. Idle is excluded on purpose — the wait is the
             * pacer's own doing, so counting it would make a slow grid look
             * like a slow frame and the step-up could never fire. */
            pace_work = dv + dg + do_;
            h_short[h_n++ & 7] = iv;
            if (h_n >= 8) {
                unsigned s[8];
                memcpy(s, h_short, sizeof(s));
                for (i = 1; i < 8; i++) {          /* insertion sort, 8 elems */
                    unsigned v = s[i]; int j = i - 1;
                    while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
                    s[j + 1] = v;
                }
                h_median = (s[3] + s[4]) >> 1;
            }
            if (iv > 70000u || iv > h_median + h_median * 3 / 5) {
                if (h_reported < 30) {
                    h_reported++;
                    /* `slack` is the part of the interval no phase accounts
                     * for. Big slack with small work = the guest was not
                     * running (vCPU descheduled under KVM, or a wait the
                     * meters do not wrap) — NOT something a renderer change
                     * can fix, and worth saying out loud rather than chasing. */
                    reportf("[Q3HIT] f=%d iv=%dms med=%dms work=%dms "
                            "vm=%d gl=%d oth=%d idle=%d slack=%dms drawn=%d",
                            (int)a_frames, (int)(iv / 1000),
                            (int)(h_median / 1000), work / 1000,
                            dv / 1000, dg / 1000, do_ / 1000, di / 1000,
                            (int)((iv > (unsigned)work ? iv - work : 0) / 1000),
                            drawn);
                }
            }
        }
        pv = cv; pg = cg; po = co; pi = ci;
        h_last_us = now_us;
    }

    /* v38.132: remember what this frame actually showed, so the demo walk can
     * steer by it next frame (see a_ad_* and the input block).
     *
     * This is the signal that matters, and it is not "did the player move" —
     * the corner walk MOVES the whole way in, from drawn=725 down to drawn=3
     * over 180 frames, so a stuck detector never sees a stop to react to. What
     * the player experiences is a view that has narrowed to a wall. So the demo
     * steers on visibility: an empty-looking frame IS the thing to avoid, and it
     * is measurable while the player is still moving. */
    a_ad_drawn = drawn;
    if (drawn > a_ad_peak) a_ad_peak = drawn;
    if (drawn * Q3ARENA_AD_COLLAPSE >= a_ad_peak) a_ad_blind = 0;

    /* v38.145: the perf line is now a 100-FRAME window, not a 20-frame one.
     * It is ~150 bytes of text and reportf() costs far more than the wire
     * time: the per-character path through the serial driver made the whole
     * block ~13 ms/frame of the 16 ms `other` bucket that nothing named. The
     * suite only needs the line to exist (q3arena_test.py PERF_RE), and a
     * 3600-frame run still yields 36 of them. */
    /* v38.148: the OUTER guard is 20 frames, the inner one is 100.
     *
     * Only one of the two things in this block wants 20: the pose and pixels
     * lines at its end, which are the suite's only record of what the camera
     * saw and of what the frame contained. The perf line and the dev
     * diagnostics keep their own 100-frame guard (`if ((frame % 100) == 0)`
     * right below), so the window the perf line reports is unchanged.
     *
     * Splitting them is what the v38.145 note asked for — "this line STAYS on
     * the 20-frame grid" — and the code had drifted to 100 for the whole
     * block. The cost of that drift is measured, not argued: the player's own
     * suite run reached frame 100 in 96 s, so the "level is being submitted"
     * gate got exactly ONE sample and read `drawn=2` off it (their own sessions
     * read drawn=408 and 653 at that same frame). One sample is not evidence;
     * five is. The serial bill for the extra samples is paid down by running
     * the UART at 115200 instead of 38400 (see init_serial in serial.c):
     * ~3500 bytes per 100 frames costs ~3 ms/frame there versus ~1.9 ms at the
     * old rate and spacing, and the window composite it competes with was
     * buying back ~5 ms/frame (kernel.c). */
    if ((frame % 20) == 0) {
        /* v38.110: one atomic perf line — phase costs over the window since
         * the last sampled frame (20 game frames = 1 s of game time), plus
         * the compositor's charges for this window (accumulated since the
         * last sample, then drained). sum_ms is that window's wall time, so
         * the parts are directly comparable to the whole. */
        if ((frame % 100) == 0) {
            int pass_ms = 0, blit_ms = 0, draw_ms = 0;
            int fps = 0;
            int win_ms = 0;
            /* (guard note: this inner block is the 100-frame one, see above) */
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
                a_last_fps_win = (wms > 0) ? (int)((100u * 1000u) / wms) : 0;
                win_ms = (int)wms;
            }
            wm_q3_times(&pass_ms, &blit_ms, &draw_ms);
            /* v38.146: pres_fps = taskbar HUD value = main-loop present
             * rate. Appended LAST so the suite's PERF_RE (which ends at
             * sum_ms) still matches. Permanent: 10 bytes per 100 frames. */
            extern int kernel_present_fps(void);
            {
                /* v38.139: the look path, measured. Appended AFTER pres_fps so
                 * the suite's PERF_RE (which ends at sum_ms) still matches.
                 *
                 *   look_s / key_s - events that actually KNEW about the game
                 *                    window per second. Zero while the mouse is
                 *                    moving means the host window had no keyboard
                 *                    focus and QEMU delivered nothing to the
                 *                    guest — a different bug from a slow one.
                 *   comp_s / comp_ms - what the desktop's composite path cost
                 *                    while the capture was held (read+cleared in
                 *                    kernel.c). This is the number that used to
                 *                    be an inference from other_ms.
                 *   hm / kd / btn / idle_in - the four gates the attract-mode
                 *                    rescue is behind. The frozen-camera report
                 *                    could not say which one had closed; now it
                 *                    can. idle_in = -1 means "no input yet". */
                extern void kernel_capture_comp_stats(int *calls, int *us);
                extern unsigned mouse_pkt_count(void);
                int cap_calls = 0, cap_us = 0, look_s, key_s, comp_s, since_in;
                int pkts_s;
                unsigned pkt_now, pkt_d;
                kernel_capture_comp_stats(&cap_calls, &cap_us);
                pkt_now = mouse_pkt_count();
                pkt_d = pkt_now - a_pkt_last;
                a_pkt_last = pkt_now;
                pkts_s = win_ms > 0 ? (int)(pkt_d * 1000u / (unsigned)win_ms) : 0;
                look_s   = win_ms > 0 ? (int)((unsigned)a_look_ev  * 1000u / (unsigned)win_ms) : 0;
                key_s    = win_ms > 0 ? (int)((unsigned)a_key_ev   * 1000u / (unsigned)win_ms) : 0;
                comp_s   = win_ms > 0 ? (int)((unsigned)cap_calls  * 1000u / (unsigned)win_ms) : 0;
                since_in = (a_last_input_ms && a_ms() > a_last_input_ms)
                         ? a_ms() - a_last_input_ms : -1;
                reportf("[Q3ARENA] perf frame=%d fps=%d vm_ms=%d gl_ms=%d "
                        "blit_ms=%d draw_ms=%d wm_ms=%d other_ms=%d idle_ms=%d "
                        "sum_ms=%d pres_fps=%d look_s=%d key_s=%d comp_s=%d "
                        "comp_ms=%d hm=%d kd=%d btn=%d idle_in=%d pkts_s=%d",
                        frame, fps,
                        vm_ms, gl_ms, blit_ms, draw_ms, pass_ms,
                        other_ms, idle_ms, sum_ms, kernel_present_fps(),
                        look_s, key_s, comp_s, cap_us / 1000,
                        a_human_move, a_keys_down, (int)(a_btn & 1), since_in,
                        pkts_s);
                a_look_ev = 0;
                a_key_ev = 0;
            }
            /* The in-window HUD shows the same numbers the log carries: the
             * window-1s phase costs and the run's fps. The overlay itself is
             * painted every frame (after this frame's histogram was taken, so
             * the suite's pixel evidence stays exactly what the 3D pass made). */
            q3ref_set_perf_overlay(a_last_fps_win, vm_ms, gl_ms,
                                   blit_ms, pass_ms, other_ms);
            a_perf_frames += 100;
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
             * the last 20 frames exactly as before.
             *
             * v38.145: even 1-in-5 was still ~600 bytes on the wire every
             * 100 frames. These lines are how the raster split was diagnosed,
             * not how the game is played, and the measurement is done — so
             * they are compiled OUT unless Q3_DEEP_DIAG is defined. The
             * accumulators below still run (they are a few adds) so turning
             * the define back on restores the whole picture. */
#ifdef Q3_DEEP_DIAG
            if ((frame % 100) == 0) {
                {
                    int s_en = 0, s_pool = 0, s_sorted = 0;
                    q3w_sort_stats(&s_en, &s_pool, &s_sorted);
                    reportf("[Q3ARENA] f2b frame=%d sort=%d sorted=%d pool=%d",
                            frame, s_en, s_sorted, s_pool);
                }
                reportf("[Q3ARENA] glsplit frame=%d begin=%d draw=%d end=%d "
                        "srv_ticks=%d srv_ms=%d glfps=%d stats=%d hist=%d "
                        "hud=%d pres=%d fps_win=%d detail=%d recon=%d tsc_khz=%d",
                        frame, a_us_gl_begin / 1000, a_us_gl_draw / 1000,
                        a_us_gl_end / 1000,
                        a_srv_ticks_win, a_srv_ms, a_last_glfps,
                        a_us_o_stats / 1000, a_us_o_hist / 1000,
                        a_us_o_hud / 1000, a_us_o_pres / 1000,
                        a_last_fps_win, a_perf_detail, a_us_recon / 1000,
                        a_tsc_khz);
            }
#endif /* Q3_DEEP_DIAG */
            a_us_o_stats = a_us_o_hist = a_us_o_hud = a_us_o_pres = 0;
            a_us_recon = 0;
            /* v38.116: and where INSIDE draw the time is. The 10 ms kernel
             * clock cannot split a 35 ms GL phase, so TinyGL accumulates TSC
             * cycles around the glVertex path (transform + clip + the raster
             * nested in it) and around the raster alone; the two are drained
             * here as per-frame kcycles plus the fill share. `tris` is the
             * number the raster saw, which is what makes the cycles
             * comparable across frames that drew different views. */
            {
                unsigned long long vc = 0, fc = 0;
                unsigned int tn = 0, wfrag = 0;
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
                    unsigned long long vmv = 0, vmf = 0, wprep = 0, wtot = 0;
                    unsigned int vmt = 0, vmg = 0;
                    q3ref_viewmodel_cycles(&vmv, &vmf, &vmt);
                    q3ref_viewmodel_frag(&vmg);
                    q3ref_world_split(&wfrag, &wprep, &wtot);
                    /* v38.126: the pass's own split — see q3ref_world_split.
                     * `emit_kc` is the part of the world draw that is neither
                     * the per-face preparation nor the vertex path: the emit
                     * loops' glBegin/glColor/glBind work and their bookkeeping.
                     * `ovr_pct` is shaded fragments over the 320x240 frame,
                     * i.e. how much of the fill was spent on pixels that were
                     * going to be overdrawn anyway. */
                    a_last_frag = (int)(wfrag / 20u);
                    a_last_vm_frag = (int)(vmg / 20u);
                    int vmvc = (int)(((unsigned)(vmv >> 20)) * 1000u / 20u);
                    a_last_prep_kc = (int)(((unsigned)(wprep >> 20)) * 1000u / 20u);
                    /* The vertex accumulator covers the WHOLE window, and the
                     * view model drew through it too (the gun is another
                     * glBegin/glVertex pass on the same context), so the
                     * world's own vertex path is the window total minus the
                     * gun's — see q3ref_viewmodel_cycles. Subtracting the raw
                     * total made `emit` read NEGATIVE on the first run of
                     * this instrument: the gun's vert alone was bigger than
                     * everything a light view spends around the transform. */
                    a_last_emit_kc = (int)(((unsigned)(wtot >> 20)) * 1000u / 20u)
                                     - a_last_prep_kc - (vkc - vmvc);
                    int pup = a_tsc_khz > 0 ? a_last_prep_kc * 1000 / a_tsc_khz : -1;
                    int eup = a_tsc_khz > 0 ? a_last_emit_kc * 1000 / a_tsc_khz : -1;
                    int tup = a_tsc_khz > 0 ?
                              (int)(((unsigned)(wtot >> 20)) * 1000u / 20u) * 1000 / a_tsc_khz
                              : -1;
                    #ifdef Q3_DEEP_DIAG
                    reportf("[Q3ARENA] glcyc frame=%d tris=%d vert_kc=%d "
                            "fill_kc=%d fill_pct=%d vm_tris=%d vm_fill_kc=%d "
                            "prep_kc=%d emit_kc=%d tot_kc=%d frag=%d ovr_pct=%d "
                            "vm_frag=%d vm_ovr_pct=%d",
                            frame, (int)tn, vkc, fkc, fpc, (int)vmt,
                            (int)(((unsigned)(vmf >> 20)) * 1000u / 20u),
                            a_last_prep_kc, a_last_emit_kc,
                            (int)(((unsigned)(wtot >> 20)) * 1000u / 20u),
                            a_last_frag,
                            a_last_frag * 100 / 76800,
                            a_last_vm_frag, a_last_vm_frag * 100 / 15000);
                    /* v38.126: and the same split in MICROSECONDS PER FRAME,
                     * which is the unit the phase meters in `perf` speak — the
                     * kc fields above are a ratio, these can be compared
                     * against gl_ms / draw_ms directly. Only meaningful where
                     * the TSC is (KVM); under TCG tsc_khz stays 0 and these
                     * read -1 rather than a made-up number. */
                    reportf("[Q3ARENA] glms frame=%d tsc_khz=%d vert_us=%d "
                            "fill_us=%d prep_us=%d emit_us=%d tot_us=%d "
                            "vm_us=%d vm_vert_us=%d w_vert_us=%d w_fill_us=%d "
                            "srv_ms=%d",
                            frame, a_tsc_khz,
                            a_tsc_khz > 0 ? vkc * 1000 / a_tsc_khz : -1,
                            a_tsc_khz > 0 ? fkc * 1000 / a_tsc_khz : -1,
                            pup, eup, tup,
                            a_tsc_khz > 0 ?
                              (int)(((unsigned)(vmf >> 20)) * 1000u / 20u) * 1000 / a_tsc_khz
                              : -1,
                            a_tsc_khz > 0 ?
                              (int)(((unsigned)(vmv >> 20)) * 1000u / 20u) * 1000 / a_tsc_khz
                              : -1,
                            a_tsc_khz > 0 ?
                              (int)(((unsigned)((vc - vmv) >> 20)) * 1000u / 20u) * 1000 / a_tsc_khz
                              : -1,
                            a_tsc_khz > 0 ?
                              (int)(((unsigned)((fc - vmf) >> 20)) * 1000u / 20u) * 1000 / a_tsc_khz
                              : -1,
                            a_srv_ms);
#endif /* Q3_DEEP_DIAG */
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
            /* v38.158: this block is SUITE EVIDENCE, not dev chatter, and it
             * is compiled into every MECTOV_Q3=1 build.
             *
             * v38.145 folded the whole play/hud/sky/viewmodel block into
             * `#ifdef Q3_DEEP_DIAG` on the belief that no suite parsed it.
             * That belief was wrong: q3hud_test.py parses the `hud` line into
             * every one of its assertions, q3sky_test.py the `sky` line, and
             * q3jump/q3viewmodel/q3cull the `play`/`viewmodel` pair. No build
             * anywhere defines Q3_DEEP_DIAG, so the lines existed in NO
             * binary at all — and CI's quake3 job died at `[FAIL] the status
             * bar was never sampled` (run 37189377311, 4 Oct) with the game
             * plainly running behind it. A gate that removes the evidence a
             * test asserts on is not an optimization; it is a broken test.
             *
             * The serial cost that motivated v38.145 (~600 bytes per 100
             * frames) is also gone as a concern: since v38.157 the writer
             * only appends to a 16 KB ring that the per-core timer tick
             * drains, so no frame blocks on the UART for these lines.
             * The lines NOTHING parses (f2b/glsplit up top, glcyc/glms in
             * the middle, ps below) stay behind the macro — those are how
             * the raster split was diagnosed, and the measurement is done. */
            if ((frame % 100) == 0 && ps) {
                /* bg_public.h's enums are not in this unit's include path
                 * (g_public.h does not pull it), and the values are stable
                 * across every Q3A derivative: weapon 2 = WP_MACHINEGUN,
                 * stats[0] = STAT_HEALTH, ammo is indexed by the same weapon
                 * enum. Spell the numbers, cite the header. */
                reportf("[Q3ARENA] play frame=%d up=%d btn=%d weapon=%d "
                        "ammo_mg=%d health=%d ground=%d jumps=%d shots=%d "
                        "vel_z=%d ad_turns=%d ad_stuck=%d",
                        frame, a_up, a_btn, (int)ps->weapon,
                        (int)ps->ammo[Q3VM_WP_MACHINEGUN],
                        (int)ps->stats[Q3VM_STAT_HEALTH],
                        (int)ps->groundEntityNum,
                        a_jumps, a_shots, (int)ps->velocity[2],
                        /* v38.132: the demo walk's own state. ad_turns climbing
                         * while the pose moves means it is touring instead of
                         * pressing into a corner — the thing whose absence made
                         * the level look empty. */
                        a_ad_turns, a_ad_stuck);
                /* v38.127: the status bar's own line — what it was given and
                 * what it made of it. `images`/`missing` are id's picture set
                 * found (or not) on this volume; `draws`/`digits` are what the
                 * last frame actually composited, so "the HUD has art" and "a
                 * HUD was drawn" stay two different numbers — the same
                 * distinction the viewmodel's model/drawn pair exists for. */
                {
                    int h_img = 0, h_mis = 0, h_draw = 0, h_dig = 0;
                    extern void q3ref_hud_stats(int *, int *, int *, int *);
                    q3ref_hud_stats(&h_img, &h_mis, &h_draw, &h_dig);
                    reportf("[Q3ARENA] hud frame=%d on=%d health=%d armor=%d "
                            "ammo=%d score=%d weapon=%d images=%d missing=%d "
                            "draws=%d digits=%d",
                            frame, a_hud,
                            (int)ps->stats[Q3VM_STAT_HEALTH],
                            (int)ps->stats[Q3VM_STAT_ARMOR],
                            (int)ps->ammo[ps->weapon],
                            (int)ps->persistant[Q3VM_PERS_SCORE],
                            (int)ps->weapon,
                            h_img, h_mis, h_draw, h_dig);
                }
                /* v38.128: the sky's own line — what the cloud box was built
                 * from and what it did, plus the world draw's split of the sky
                 * faces. `on` is the effective `nosky` state, `reg` the sky
                 * shaders found at load, `cloud` the height the box was
                 * projected for, `stages`/`sides`/`tris` the last frame's box,
                 * and `box`/`fallback` how the world draw placed the sky faces
                 * (drawn by the box versus kept as ordinary geometry). All zero
                 * on a map with no sky — which is every fixture arena, and what
                 * lets "this level has a cloud sky" be asserted from the log
                 * without a pixel. */
                {
                    int k_on = 0, k_reg = 0, k_cloud = 0, k_stages = 0;
                    int k_sides = 0, k_tris = 0, k_trun = 0;
                    int k_box = 0, k_boxrun = 0, k_fb = 0, k_fbrun = 0;
                    extern void q3ref_sky_stats(int *, int *, int *, int *,
                                                int *, int *, int *);
                    extern void q3ref_sky_face_stats(int *, int *, int *, int *);
                    q3ref_sky_stats(&k_on, &k_reg, &k_cloud, &k_stages,
                                    &k_sides, &k_tris, &k_trun);
                    q3ref_sky_face_stats(&k_box, &k_boxrun, &k_fb, &k_fbrun);
                    reportf("[Q3ARENA] sky frame=%d on=%d reg=%d cloud=%d "
                            "stages=%d sides=%d tris=%d tris_run=%d box=%d "
                            "box_run=%d fallback=%d fallback_run=%d",
                            frame, k_on, k_reg, k_cloud, k_stages, k_sides,
                            k_tris, k_trun, k_box, k_boxrun, k_fb, k_fbrun);
                }
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
        /* v38.148: this line and the pixels line below run every 20 frames —
         * they are the suite's evidence and they were starving for it.
         *
         * How it was found, because the note that used to sit here got it
         * backwards twice. Brace-matching this file says the pose reportf's
         * enclosing openers are q3arena_frame, `if ((frame % 20) == 0)` and
         * this `if (ps)` — and it said `if ((frame % 100) == 0)` before this
         * release, which the player's own log confirms: every pose line was
         * exactly 100 frames after the last (28 of them over 8300 frames).
         * Then the suite failed its "level is being submitted" gate with
         * `drawn=2` on the one and only sample a 96-second run produced, at
         * frame 100. The v38.145 note had predicted exactly that — "it read
         * drawn=2 off the single early frame it did get" — and specified the
         * fix: keep this line on the 20-frame grid and move the cost to the
         * lines behind Q3_DEEP_DIAG, which no suite reads. The note was right
         * and the code was wrong; this release puts them back together.
         *
         * Serial cost, measured rather than guessed: one sample frame emits
         * ~550 bytes (this line ~420, the pixels line ~130), so 20-frame
         * spacing is ~2750 bytes per 100 frames. At the old 38400 baud that
         * would be ~7 ms/frame of `other`; at 115200 it is ~2.4 ms, and it is
         * what buys the gate five samples instead of one. */
        reportf("[Q3ARENA] frame=%d t=%d pos=(%d,%d,%d) eye_z=%d yaw=%d "
                "pitch=%d drawn=%d tris=%d culled=%d vis=%d/%d cluster=%d "
                "cull_pvs=%d cull_frustum=%d planes=%d back=%d cam_alpha=%d "
                "untrusted=%d cull=%d",
                frame, q3vm_frame_time,
                (int)ps->origin[0], (int)ps->origin[1], (int)ps->origin[2],
                (int)eye[2], (int)ps->viewangles[YAW], (int)ps->viewangles[PITCH],
                drawn, tris, culled, vis_marked, vis_total, vis_cluster,
                cull_pvs, cull_frustum, cull_planes, cull_back,
                a_last_cam_alpha,
                /* v38.132: `untrusted` is the number of faces the backface
                 * reject looked at and deliberately did NOT reject, because
                 * the file's own vertex normals disagree about which way the
                 * front side faces. Read it next to `back`: on q3dm1's retail
                 * data the reject used to take back=1927 of 1930 and leave
                 * drawn=3, a black screen with every counter frozen. If
                 * `untrusted` is large, the reject has little left to win here
                 * and `nocull` is the honest setting. */
                q3w_cull_untrusted(),
                q3w_cull_enabled());
        /* v38.113 diagnostics: WHY is the player where it is? The walk gate in
         * the module is `msec = cmd.serverTime - ps.commandTime; if (msec < 1)
         * return;` — and the counters below say whether that gate ever opens
         * (commandTime stuck at 0), whether Pmove is even in play (velocity
         * and groundEntity stuck at their spawn values), and whose turn it
         * is (pm_type). No suite reads this line and it is one of the longest
         * in the block, so v38.119 prints it once every fifth window; the
         * information a pixel/suite assertion needs is in the frame line.
         * v38.145: behind Q3_DEEP_DIAG entirely — no suite reads it. */
#ifdef Q3_DEEP_DIAG
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
#endif /* Q3_DEEP_DIAG (ps line) */
        /* v38.144 TEMP-DIAG (mouse_ev / main_iters / srv ticks) REMOVED in
         * v38.145: the stage rates it measured are known now, and the kernel
         * counter it read is gone from kernel.c with it. */
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

    /* v38.140: phase-lock the render loop to 40 Hz — two renders per 50 ms
     * server tick — so the camera lerp (alpha = acc/50) is sampled evenly.
     * The 15 ms pacer targeted 66 Hz the host never reached (renders cost
     * ~13-20 ms, plus the 10 ms yield below) and, being permanently behind,
     * never waited at all: the loop free-ran at ~20-28 Hz against the
     * 20 Hz ticks, a 7:5-ish beat that reads as judder ("patah-patah")
     * despite even frame times and clean audio (skip=0 drop=0). This
     * accumulator runs on the microsecond clock.
     * v38.147: the wait is now hlt-to-2ms + spin, not hlt-to-10ms +
     * task_sleep(1). task_sleep quantizes to the 10 ms scheduler tick (0–20
     * ms actual, worse under load), so every paced frame paid a 0–10 ms
     * overshoot lottery — that lottery, not the work, is what held typical
     * frames at 35 ms instead of 25 ms. hlt still covers the bulk of the
     * idle (the compositor runs then); only the last 2 ms spin, precise to
     * ~0.1 ms on the latched-PIT us clock. The 25 ms target is UNCHANGED:
     * it is the 2:1 lock to the 50 ms server tick the camera lerp samples,
     * and a 20 ms target would drift against the tick grid (2.5 renders
     * per tick) and reintroduce lerp judder.
     * Late frames skip the wait but still yield every 4th frame, so the
     * desktop keeps its timeslice without breaking the lock. A
     * stall over 2 s re-syncs instead of dragging minutes of lag. */
    {
        int t0 = a_us();
        unsigned now0 = (unsigned)a_us();
        /* v38.150: adaptive {25 ms, 50 ms} grid. The pacer above locks render
         * STARTS to a grid, but presents happen at render COMPLETION — so a
         * host whose frames straddle the 25 ms budget (some 20 ms, some
         * 30 ms) presents at 25/50/30/30 ms intervals: even production,
         * uneven glass, which is the "fps moves between 20 and 30 while
         * turning" report. The 50 ms grid is the only other rate that locks
         * to the sim tick (v38.140: 33 ms would drift 0.66 ticks per render
         * and reintroduce lerp judder), and at 50 ms the camera position sits
         * exactly on ticks (alpha ~= 0), so motion is even by construction.
         * Three overruns in the last eight frames step down (consecutive or
         * alternating — a view that straddles the budget every other frame
         * judders just the same); two hundred consecutive comfortable frames
         * step back up (10 s of headroom before trusting it — flapping
         * between grids would be worse than either). Fast hosts
         * never leave 25 ms, so suites and fast sessions are untouched, and
         * under massive overrun (TCG) both grids skip their waits identically
         * to before. */
        static int pace_slow = 0, pace_good = 0;
        static unsigned pace_hist = 0;   /* overrun bits, last 8 frames */
        unsigned slot = pace_slow ? 50000u : 25000u;
        int overran = !(now0 < a_next_us - 2000u);
        int i, nbad;
        pace_hist = ((pace_hist << 1) | (overran ? 1u : 0u)) & 0xFFu;
        for (i = nbad = 0; i < 8; i++)
            if (pace_hist & (1u << i)) nbad++;
        if (!pace_slow && nbad >= 3) {
            pace_slow = 1;
            pace_good = 0;
            write_serial_string("[Q3ARENA] pacer: 25 ms -> 50 ms grid "
                                "(sustained overrun)\n");
        } else if (pace_slow) {
            /* v38.155: the step-up test used to be "arrived on the 50 ms grid
             * with >30 ms to spare". That is off by construction: a frame whose
             * work is 20 ms leaves EXACTLY 30 ms, fails a >30 ms test, and the
             * grid never leaves 50 ms — so the session ran at a locked 20 fps
             * while the log showed 30.7 ms of idle in every 50 ms frame, i.e.
             * 60% of the budget was going unused. Ask the question directly
             * instead: would a 25 ms frame have FIT? That is the previous
             * frame's own measured work, not a leftover computed from the grid
             * it is trying to leave. 18 ms leaves headroom for the present
             * and the sim step inside 25 ms. */
            int prev_work = pace_work;
            if (prev_work < 18000) {
                if (++pace_good >= 90) {          /* ~2-4 s of proven headroom */
                    pace_slow = 0;
                    pace_good = 0;
                    pace_hist = 0;
                    write_serial_string("[Q3ARENA] pacer: 50 ms -> 25 ms grid "
                                        "(headroom back)\n");
                }
            } else {
                pace_good = 0;
            }
        }
        if ((unsigned)(now0 - a_next_us) > 2000000u) a_next_us = now0;
        a_next_us += slot;
        if (now0 < a_next_us - 2000u) {
            unsigned wt = a_next_us - 2000u;
            for (;;) {
                unsigned now = (unsigned)a_us();
                if (now >= wt || (unsigned)(wt - now) > 2000000u || a_quit) break;
                __asm__ __volatile__("hlt");
            }
            /* Precise landing: spin the last 2 ms (a_us is the µs latched-PIT
             * clock). Burns ~2 ms of this core per paced frame instead of
             * handing it to the scheduler lottery; the compositor already had
             * the whole hlt stretch above plus the every-4th yield below. */
            for (;;) {
                unsigned now = (unsigned)a_us();
                if (now >= a_next_us || (unsigned)(a_next_us - now) > 2000000u || a_quit) break;
                __asm__ __volatile__("rep; nop");
            }
        } else if ((a_frames % 4) == 0 && a_win >= 0) {
            /* v38.147: every 4th frame's guaranteed slice (was every 2nd).
             * The pacer above leaves ~13 ms of hlt idle per typical frame
             * and the main loop runs freely in it, so the explicit yield is
             * only the safety net for over-budget (heavy-view) frames where
             * no idle exists. Every 4th (~6% share) halves the sleep-lottery
             * tax versus every 2nd; pres_fps in the log confirms presents
             * still track renders. */
            extern void task_sleep(int ticks);
            task_sleep(1);
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
        /* v38.126: how much of `other` is this reconciliation rather than a
         * named phase. It was 3-5 ms/frame in the user's own session — the
         * server tick catch-up, the serial writes and this frame's own
         * bookkeeping — and it is the one part of the budget the phase meters
         * could not name. */
        if (spent > known) {
            a_us_other += spent - known;
            a_us_recon += spent - known;
        }
    }
    (void)shaders; (void)from_disk; (void)ph;
    if (a_win >= 0 && wm_is_open(a_win) == 0) {
        write_serial_string("[Q3ARENA] window closed\n");
        a_quit = 1;
    }
}

/* ===== driver ========================================================== */

/* v38.130: every static in this driver describes ONE run of the game — and
 * between the first and second `q3arena` in a session nothing here was
 * cleared. The second launch therefore began with run 1's leftover state: most
 * visibly a_quit still 1 (so the frame loop broke after a single frame) and
 * a_frames still counting (render totals showed 103 vs 102). The renderer and
 * sound mixer reset themselves per run; this driver did not. */
static void q3arena_reset_driver_state(void) {
    int k;
    a_win = -1;
    a_quit = 0;
    a_fullscreen = 0;
    a_sort = 1;
    a_hud = 1;
    a_sky = 1;
    a_nolimit = 0;
    a_up = 0;
    a_btn = 0;
    a_jump_left = 0;
    a_fire_left = 0;
    a_fire_t = 0;
    a_jump_at = 0;
    a_fire_at = 0;
    a_jumps = 0;
    a_shots = 0;
    vm_firing_diag = 0;
    a_jump_seen = 0;
    a_fire_seen = 0;
    a_jump_gap = 40;
    a_fire_gap = 20;
    a_jump_t = 0;
    a_jump_tick0 = 0;
    a_pose_set = 0;
    a_input_seen = 0;
    /* v38.136: a second launch starts in the tour again, with a clean guard on
     * the first click. */
    a_human_move = 0;
    a_mouse_first = 0;
    a_keys_down = 0;
    a_last_input_ms = 0;
    a_pump_ms = 0;
    a_pump_calls = 0;
    a_present_ms = 0;
    a_rescue_logged = 0;
    a_look_ev = 0;
    a_key_ev = 0;
    a_pkt_last = 0;         /* v38.149: rate is per window, not across runs */
    a_ad_lastx = a_ad_lasty = 0.0f;
    a_ad_stuck = 0;
    a_ad_pending_turn = 0;
    a_ad_turns = 0;
    a_ad_drawn = 0;
    a_ad_peak = 0;
    a_ad_blind = 0;
    a_idle_frames = 0;
    a_frames = 0;
    a_faces_drawn = 0;
    a_tris_drawn = 0;
    for (k = 0; k < 128; k++) a_keys[k] = 0;
    a_next_us = 0;
    a_start_ms = 0;
    a_us_vm = a_us_gl = a_us_other = a_us_idle = 0;
    a_us_recon = 0;
    a_us_gl_begin = a_us_gl_draw = a_us_gl_end = 0;
    a_us_o_stats = a_us_o_hist = a_us_o_hud = a_us_o_pres = 0;
    a_srv_last_ms = 0;
    a_srv_acc = 0;
    a_srv_ticks = 0;
    a_srv_ticks_win = 0;
    a_srv_ticks_total = 0;
    a_srv_ms = 0;
    a_last_glfps = 0;
    a_perf_frames = 0;
    a_last_fps = 0;
    a_last_fps_win = 0;
    a_last_frag = a_last_vm_frag = 0;
    a_last_prep_kc = a_last_emit_kc = 0;
    a_win_ms = 0;
    a_perf_detail = 0;
    a_cam_have = 0;
    a_last_cam_alpha = 0;
    a_vm_tried = 0;
    q3vm_cmd_live = 0;
    q3vm_cmd_forward = 0;
    q3vm_cmd_right = 0;
    q3vm_cmd_yaw = 0;
    q3vm_cmd_pitch = 0;
    vm_gentity_ofs = -1;
    vm_ps_ofs = -1;
    vm_num_entities = 0;
    vm_sizeof_gentity = 0;
    /* v38.133: the load's own bookkeeping. A relaunch must not inherit the
     * previous run's clock, cancel request or phase — the phase in particular,
     * which a shell poll would otherwise read as "ended" before the new load
     * has even started. */
    a_load_t0 = 0;
    a_load_t[0] = a_load_t[1] = a_load_t[2] = a_load_t[3] = 0;
    a_load_cancel = 0;
    a_load_phase = 0;
    /* a_tsc_khz / a_tsc_cal0 keep their values: the TSC rate is a property of
     * the machine, measured once, still true for the next run. */
}

static void q3vm_park(void) {
    /* A forked kernel task entry must never return (v38.99). */
    /* v38.133: whatever brought the task here — a finished session, a failed
     * load, a cancelled one — the load is over, and the shell's progress poll
     * (q3arena_status) reads this to stop reporting and hand the prompt back. */
    a_load_phase = 3;
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
            if (wl == 6) {
                static const char kw[] = "nocull";
                int k = 0;
                while (kw[k] && arg[w0 + k] == kw[k]) k++;
                /* v38.132: A/B the backface reject on a retail map without a
                 * rebuild. The reject is already fail-safe (a face whose file
                 * normals disagree is drawn), so this is only for measuring how
                 * much the cull actually contributes — `untrusted=` in the
                 * frame line then equals the whole candidate set. */
                if (k == 6) { q3w_set_cull(0); continue; }
            }
            if (wl == 5) {
                static const char kw[] = "nohud";
                int k = 0;
                while (kw[k] && arg[w0 + k] == kw[k]) k++;
                if (k == 5) { a_hud = 0; continue; }    /* v38.127 A/B knob */
            }
            if (wl == 5) {
                static const char kw[] = "nosky";
                int k = 0;
                while (kw[k] && arg[w0 + k] == kw[k]) k++;
                if (k == 5) { a_sky = 0; continue; }    /* v38.128 A/B knob */
            }
            if (wl == 7) {
                static const char kw[] = "nolimit";
                int k = 0;
                while (kw[k] && arg[w0 + k] == kw[k]) k++;
                /* v38.131: play until ESC — no frame cap, no wall clock. The
                 * frame cap exists so CI suites terminate on their own; a
                 * human player at the keyboard does not need it. */
                if (k == 7) { a_nolimit = 1; continue; }
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
            "jump=%d fire=%d sky=%d nolimit=%d",
            n ? q3vm_bsp_name : "(generated arena)",
            a_fullscreen, a_pose_set, a_sort, a_jump_left, a_fire_left, a_sky,
            a_nolimit);
    if (a_pose_set)
        reportf("[Q3ARENA] pose pinned: eye=(%d,%d,%d) yaw=%d pitch=%d",
                (int)a_pose_x, (int)a_pose_y, (int)a_pose_z,
                (int)a_pose_yaw, (int)a_pose_pitch);
    if (!a_sky)
        reportf("[Q3ARENA] sky: the cloud box is OFF (nosky) — sky faces are "
                "drawn as geometry");


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

    /* v38.130: make a second launch in one session possible. The quit path
     * leaves id's engine holding its whole first session: fs_searchpaths,
     * the cvar/cmd zones, the hunk and — the one Com_Init actually CHECKS —
     * fs_loadStack, the count of files still on Hunk_AllocateTempMemory.
     * Com_InitHunkMemory fatal-errors ("File system load stack not zero") on
     * a non-zero stack, so since v38.121 a re-open could not boot. id resets
     * all of this itself only in FS_Restart / Com_Quit_f, which this path
     * never runs; so do what FS_Restart does, before the re-init:
     *   - FS_Shutdown: closes handles, frees the searchpath/pack list, and
     *     NULLs fs_searchpaths so FS_Startup rebuilds the world from scratch;
     *   - Hunk_Clear + Hunk_ClearTempMemory: drop the previous session's
     *     permanent and temp allocations (the qvm, the collision world, the
     *     level's textures) instead of leaking them;
     *   - Z_Malloc-fallback temporaries live in the ZONE, so the small/main
     *     zones the re-init would re-calloc over a stale one must be freed
     *     too — free them and NULL the pointers, Com_Init re-allocates.
     * The FS load stack itself counts Hunk temp blocks and reaches zero the
     * moment those are cleared; the FS_*File calls below report what the
     * stack was on entry, so a session that quit with a file open is VISIBLE
     * in the log rather than silently papered over. */
    {
        extern void FS_Shutdown(qboolean closemfp);
        extern int  FS_LoadStack();
        extern void Hunk_Clear(void);
        extern void Hunk_ClearTempMemory(void);
        /* v38.130: the zone reset shim lives in q3_kernel.c (it needs the
         * memzone_t definition and both zone pointers, which common.c does
         * not expose through qcommon.h), and it also clears files.c's
         * fs_loadStack through the one function the vendored tree gained for
         * this (see files.c's v38.130 reset shim). */
        extern void q3_reset_engine_state(void);
        int stack0 = FS_LoadStack();
        if (stack0 != 0)
            reportf("[Q3VM] relaunch: FS load stack was %d on entry (a file "
                    "stayed allocated through quit)", stack0);
        /* v38.130: the driver's own per-run state (a_quit, a_frames, the
         * input channels, the perf meters) — see the comment on the function. */
        q3arena_reset_driver_state();
        reportf("[Q3VM] relaunch: driver state reset");
        FS_Shutdown(qfalse);
        Hunk_ClearTempMemory();
        Hunk_Clear();
        q3_reset_engine_state();
    }

    /* v38.133: the load's clock starts AFTER the reset above, before id's
     * engine is touched at all — "engine" in the summary line is everything
     * from this stamp to Com_Init returning, which is where a launch spends
     * its first silent seconds (12 of them on the 14:51 session, with the
     * window not open yet). Resetting the clock is part of
     * q3arena_reset_driver_state(), so a relaunch starts its own. */
    if (windowed) {
        a_load_t0 = a_ms();
        a_load_phase = 1;
        a_load_stage = "engine startup";
    }

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
    }	Com_Init(cmdline);    write_serial_string("[Q3VM] Com_Init done\n");
    if (windowed) a_load_t[0] = a_ms_since(a_load_t0);

	/* VM_LoadSymbols() bails unless com_developer is set. That cvar is named
	 * "developer" (common.c: com_developer = Cvar_Get("developer", ...)), and
	 * id ties symbol loading to it, so make sure it is on before the loader
	 * runs — belt and braces on top of the +set above. */
	Cvar_Set("developer", "1");

    /* v38.107: load the collision world through id's own loader. It has to
     * happen before the module runs — G_InitGame walks the level's entity
     * string during GAME_INIT, and ClientSpawn needs the brushes by
     * CLIENT_BEGIN. */
    /* v38.131: windowed launches open the game window BEFORE the world load,
     * as a loading screen — see q3arena_boot_window_open. */
    if (windowed) q3arena_boot_window_open();

    /* v38.132: paint "collision world" BEFORE the 12-second CM_LoadMap, or the
     * user watches the previous stage's text for the whole of it. The window was
     * opened with that stage already set, but only its own open-time invalidate
     * had run, so without this the collision phase shows nothing new at all. */
    if (windowed && a_win >= 0) wm_invalidate(a_win);

    /* v38.133: the first cancel point. Nothing exists yet but the window, so
     * an ESC pressed during the engine wait (the twelve silent seconds) lands
     * here. */
    if (windowed && a_load_cancel) q3arena_load_cancelled(0);

    q3vm_world_load();
    q3vm_world_report_spawn();
    if (windowed) {
        a_load_t[1] = a_ms_since(a_load_t0);
        /* v38.133: second cancel point — the collision world is up, the
         * renderer is not, so this is still the window-only teardown. */
        if (a_load_cancel) q3arena_load_cancelled(0);
    }

    /* Windowed only: the renderer, the window and the level's textures come up
     * before the module runs, so GAME_INIT and the connect sequence happen with
     * the world already drawable. A failure here (no renderer, no window) is
     * reported and the session continues headless rather than dying.
     *
     * v38.132: every stage change now REPAINTS the window.
     *
     * The bug this fixes is not cosmetic. The window was painted exactly once,
     * by q3arena_boot_window_open(), and then nothing marked it dirty again
     * until the frame loop started — which is ~20 seconds later on q3dm1
     * (12 s collision world + 8 s for 94 textures). So the compositor kept
     * re-blitting that one stale image: a black window with a progress bar at
     * zero, for the entire load. The user's report was "the game doesn't
     * appear / it's frozen", and it was faithfully re-rendering a picture from
     * before any work had happened. `a_load_stage` and `q3w_progress_done`
     * were both updating the whole time — they simply had no path to the
     * screen.
     *
     * wm_invalidate() only sets a dirty flag and the compositor runs on its own
     * schedule, so calling it from this task is exactly what the frame loop
     * does 40 times a second at q3_vm.c:2240. */
    if (windowed) {
        a_load_stage = "decoding textures";
        q3w_set_progress_hook(q3arena_progress_hook);
        wm_invalidate(a_win);
        write_serial_string("[Q3ARENA] official qagame VM world rendered through "
                            "TinyGL (id Software source)\n");
        q3arena_open();
        q3w_set_progress_hook(0);
        a_load_stage = "starting the game VM";
        wm_invalidate(a_win);
        q3vm_cmd_live = 1;
        a_load_t[2] = a_ms_since(a_load_t0);
        /* v38.133: third cancel point — the renderer, the mesh and the mixer
         * are all up now, so this is the ordinary teardown. The decode itself
         * may also have stopped early: the hook's non-zero return breaks
         * q3w_load's loop, which is what makes ESC during the texture stage
         * land within one texture instead of one stage. */
        if (a_load_cancel) q3arena_load_cancelled(1);
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
                windowed ? (a_nolimit ? 0 : Q3ARENA_FRAMES) : Q3VM_FRAME_COUNT,
                Q3VM_FRAMETIME);
        q3vm_frame_time = 0;

        /* v38.133: last cancel point — the module is created and the connect
         * sequence has run, so a cancel here tears the session down exactly
         * the way a session's own end does (window, renderer, then the
         * module's own shutdown), instead of leaving a half-entered game
         * behind. */
        if (windowed && a_load_cancel) {
            reportf("[Q3ARENA] load cancelled (ESC) before the first frame at "
                    "%dms", a_ms_since(a_load_t0));
            q3arena_close();
            q3bsp_free(&a_mesh);
            write_serial_string("[Q3ARENA] done\n");
            VM_Call(vm, GAME_SHUTDOWN, qfalse);
            write_serial_string("[Q3VM] GAME_SHUTDOWN done\n");
            VM_Free(vm);
            write_serial_string("[Q3VM] done\n");
            q3vm_park();
        }

    if (windowed) {
        a_start_ms = t_start;
        a_next_us = (unsigned)a_us();
        a_win_ms  = t_start;   /* v38.126: the fps window starts here too */
        a_srv_last_ms = t_start;   /* v38.116: both clocks start together */
        a_srv_acc = 0;
        a_cam_have = 0;
        /* v38.131: first game frame is about to render — flip the window
         * from the loading screen to the game and bring it to the front
         * once, here, where the user is looking.
         * v38.133: THIS is where the input routes are taken. The mouse comes
         * under capture now and not a minute earlier (the load is over; a
         * captured cursor with no game on screen was half of the "freeze"
         * report), and the scancode route was taken back at the boot window so
         * ESC could end the wait. Re-requesting it is idempotent, and doing it
         * here keeps the suites' marker line in the phase it belongs to. */
        if (!a_boot_ready) {
            a_boot_ready = 1;
            a_next_us = (unsigned)a_us();  /* v38.140: lock the 40 Hz loop from here */
            a_load_t[3] = a_ms_since(a_load_t0);
            a_load_phase = 2;
            write_serial_string("[Q3ARENA] boot screen -> game (window raised)\n");
            if (a_win >= 0) wm_raise(a_win);
            wm_request_scancodes(a_win, 1);
            if (wm_capture_mouse(a_win, 1)) {
                int cx = 0, cy = 0;
                wm_capture_center(&cx, &cy);
                /* v38.136: the pointer's travel to this window arrives as the
                 * next motion event. Drop it: applying it aimed the player at
                 * the floor in one event (pitch 0 -> 82 in the 23:39 session). */
                a_mouse_first = 1;
                write_serial_string("[Q3ARENA] mouse captured (WASD/arrows move, mouse looks, ESC quits)\n");
            } else {
                write_serial_string("[Q3ARENA] WARNING: mouse capture refused\n");
            }
            reportf("[Q3ARENA] input: scancodes+mouse capture taken at game "
                    "start (load %dms)", a_load_t[3]);
            reportf("[Q3ARENA] load: engine=%dms collision=%dms textures=%dms "
                    "vm=%dms total=%dms pump=%dms/%d",
                    a_load_t[0], a_load_t[1] - a_load_t[0],
                    a_load_t[2] - a_load_t[1], a_load_t[3] - a_load_t[2],
                    a_load_t[3], a_pump_ms, a_pump_calls);
        }
            /* v38.131: `nolimit` runs until ESC — the frame cap exists so
             * CI sessions end themselves, not because a player needs one. */
            for (frame = 1; a_nolimit || frame <= Q3ARENA_FRAMES; frame++) {
                q3arena_frame(vm, frame);
                /* v38.135: input asked for the window back (see
                 * q3arena_note_input) — restack here, in the game's own task. */
                /* v38.137: and make sure the frame is ON THE GLASS, not just in
                 * the back buffer. Once a second this task claims the whole
                 * screen and presents it (see desktop_present_now in kernel.c):
                 * mark_dirty() ignores damage recorded while some window's
                 * content buffer is the render target, and swap_buffers()
                 * resets the damage rect it just copied — so a rectangle that
                 * misses its mark keeps whatever was on the glass before. That
                 * race is what left the loading screen (`loading 8s`) on screen
                 * for minutes while this loop rendered frame 380 into it.
                 * Once a second is enough to make the game always appear, and
                 * far too cheap to matter (one composite in ~15 frames). */
                if (a_ms() - a_present_ms >= 1000) {
                    /* v38.137: once a second this task claims the whole screen
                     * and presents it (see desktop_present_now in kernel.c).
                     * v38.150: but NOT synchronously any more. That call put a
                     * full-desktop composite inside this frame — tens of ms on
                     * a slow host, once a second, exactly the metronome hitch
                     * folded into the "kadang patah" report (the log's other_ms
                     * carried it). Now this only raises the flag; the desktop
                     * loop spends its own time on the compose below, and this
                     * frame stays on its budget. Chrome (taskbar clock, corner
                     * readout) goes through the same compose, so it stays live
                     * — at most a blink late, never frozen. */
                    extern volatile int want_full_present;
                    a_present_ms = a_ms();
                    want_full_present = 1;
                }
                if (a_raise_wanted && a_win >= 0) {
                    a_raise_wanted = 0;
                    /* v38.150: raise only when there is something to bring
                     * forward. Every mouse motion re-arms this flag (throttled
                     * to twice a second), and wm_raise unconditionally restacks
                     * the window list and dirties the WHOLE screen — on a slow
                     * host that is a full-desktop composite twice a second,
                     * exactly while the mouse is moving, which is the shape of
                     * the "patah-patah whenever I move the mouse" report. When
                     * focus is already here and this window is topmost, the
                     * raise would change nothing and cost a full compose, so
                     * skip it. If the check ever misfires, the next input
                     * re-arms the flag half a second later — self-healing. */
                    {
                        extern int wm_focused, wm_zcount, wm_zorder[];
                        extern int get_win_index(int wid);
                        int topmost = 0;
                        if (wm_focused == a_win && wm_zcount > 0)
                            topmost = (wm_zorder[wm_zcount - 1] ==
                                       get_win_index(a_win));
                        if (!topmost) {
                            wm_raise(a_win);
                            if (!a_raise_logged) {
                                a_raise_logged = 1;
                                write_serial_string("[Q3ARENA] window raised again on "
                                                    "input (the terminal was in front)\n");
                            }
                        }
                    }
                }
                if (a_quit) break;
                if (!a_nolimit &&
                    (unsigned)Com_Milliseconds() - t_start > Q3ARENA_WALL_MS) {
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

/* ---- what the shell prints while this task loads ------------------------ */
/* v38.133: the load runs in a task of its own, and the window it opens is not
 * where the user is looking — the terminal is, because that is where the
 * command was typed. `q3arena`'s builtin polls this and prints the same story
 * there: a load that says nothing for two minutes is indistinguishable from a
 * hang, which is exactly the report this release answers.
 *
 * Returns 0 = nothing launched, 1 = loading, 2 = in the game, 3 = ended. Every
 * park sets 3, so a load that failed ends the poll as surely as a finished
 * session. `buf`, when non-NULL, gets a short line, always NUL-terminated.
 * Lock-free on purpose: every field is an int written by one task, and the
 * worst a race can do is report a stage name one poll late. */
int q3arena_status(char *buf, int n) {
    int ph = a_load_phase;
    if (buf && n > 0) {
        int i = 0;
        const char *s;
        if (ph == 1) {
            s = "loading: "; while (*s && i < n - 24) buf[i++] = *s++;
            for (s = a_load_stage; s && *s && i < n - 24; s++) buf[i++] = *s;
            if (q3w_progress_stage == 2 && q3w_progress_total > 0) {
                if (i < n - 20) buf[i++] = ' ';
                i += q3arena_put_int(buf + i, q3w_progress_done);
                if (i < n - 2) buf[i++] = '/';
                i += q3arena_put_int(buf + i, q3w_progress_total);
            }
            if (i < n - 14) buf[i++] = ' ';
            if (i < n - 13) buf[i++] = '(';
            i += q3arena_put_int(buf + i, a_ms_since(a_load_t0) / 1000);
            if (i < n - 3) buf[i++] = 's';
            if (i < n - 2) buf[i++] = ')';
        } else if (ph == 2) {
            s = "in the game: frame "; while (*s && i < n - 30) buf[i++] = *s++;
            i += q3arena_put_int(buf + i, a_frames);
            s = ", "; while (*s && i < n - 30) buf[i++] = *s++;
            i += q3arena_put_int(buf + i, a_last_fps);
            s = " fps - ESC in the game window quits";
            while (*s && i < n - 2) buf[i++] = *s++;
        } else if (ph == 3) {
            s = "session ended - back to the terminal";
            while (*s && i < n - 2) buf[i++] = *s++;
        }
        buf[i < n ? i : n - 1] = '\0';
    }
    return ph;
}

/* ---- the two entry points the shell commands fork into ---------------- */
/* `q3vm` (v38.105) stays headless: the evidence for the VM milestone is the
 * serial log, and CI's regression for it must not depend on a window.
 * `q3arena` (v38.108) is the same session with the level drawn. */
void q3vm_start(void)   { q3_drive(0); }
void q3arena_start(void) { q3_drive(1); }
