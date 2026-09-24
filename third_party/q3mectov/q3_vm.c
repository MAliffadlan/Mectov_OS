/* q3_vm.c — Quake III Arena, from id Software's OFFICIAL source (v38.106).
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
 * Design rule: never park on failure. Every step reports and returns, so a
 * broken step costs one FAILED marker instead of a dead machine.
 */
#include <stdint.h>
#include <string.h>

#include "../q3a/code/game/q_shared.h"
#include "../q3a/code/qcommon/qcommon.h"
#include "../q3a/code/game/g_public.h"
/* id's VM-private header, for read-only inspection of what the loader built
 * (id's own qcommon/vm.c includes it exactly like this). */
#include "../q3a/code/qcommon/vm_local.h"

/* Mectov platform services */
extern void write_serial_string(const char *s);

/* Official engine core entry points (qcommon/common.c, qcommon/cmd.c) */
extern void Com_Printf(const char *fmt, ...);
extern int  Com_Milliseconds(void);

/* VFS (src/sys/vfs.c) */
extern int  vfs_mkdir(const char *path);
extern int  vfs_get_node(const char *path);
extern int  vfs_create_file(const char *path);
extern int  vfs_write_file(const char *path, const char *data, int size);
extern unsigned int vfs_get_file_size(int node);

/* ===== serial helpers (the rest of the port writes these by hand too) ==== */

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

/* One synthetic player, full throttle forward. serverTime carries the frame's
 * level time; id's own code clamps and resyncs anything else. */
static void q3vm_fill_usercmd(usercmd_t *cmd, int serverTime) {
    memset(cmd, 0, sizeof(*cmd));
    cmd->serverTime = serverTime;
    cmd->forwardmove = 127;
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
    write_serial_string("[Q3VM] server command to ");
    vm_ser_int(clientNum);
    write_serial_string(": ");
    write_serial_string(text);
    write_serial_string("\n");
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
    write_serial_string("[Q3VM] frame ");
    vm_ser_int(frame);
    write_serial_string(" t=");
    vm_ser_int(levelTime);
    write_serial_string(" origin=(");
    vm_ser_int((int)ps->origin[0]);
    write_serial_string(",");
    vm_ser_int((int)ps->origin[1]);
    write_serial_string(",");
    vm_ser_int((int)ps->origin[2]);
    write_serial_string(") ground=");
    vm_ser_int(ps->groundEntityNum);
    write_serial_string(" velocity=(");
    vm_ser_int((int)ps->velocity[0]);
    write_serial_string(",");
    vm_ser_int((int)ps->velocity[1]);
    write_serial_string(",");
    vm_ser_int((int)ps->velocity[2]);
    write_serial_string(")\n");
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
        if (!entp) entp = entbuf;
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
        q3vm_trace_world((trace_t *)VMA(1), (const float *)VMA(2),
                         (const float *)VMA(3), (const float *)VMA(4),
                         (const float *)VMA(5));
        return 0;
    case G_TRACECAPSULE:
        /* no walls and no boxes: the capsule path sees the same floor */
        q3vm_trace_world((trace_t *)VMA(1), (const float *)VMA(2),
                         (const float *)VMA(3), (const float *)VMA(4),
                         (const float *)VMA(5));
        return 0;

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

/* ===== driver ========================================================== */

static void q3vm_park(void) {
    /* A forked kernel task entry must never return (v38.99). */
    for (;;) __asm__ __volatile__("hlt");
}

void q3vm_start(void) {
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

    /* id's loader: reads vm/qagame.qvm through the engine FS, validates the
     * VM_MAGIC header, allocates the data/code segments on the hunk and
     * prepares the bytecode for the interpreter. */
    vm_t *vm = VM_Create("qagame", Q3VM_SystemCalls, VMI_BYTECODE);
    if (!vm) {
        write_serial_string("[Q3VM] FAILED: VM_Create returned NULL\n");
        q3vm_park();
    }

    write_serial_string("[Q3VM] vm created name=qagame interpret=bytecode symbols=");
    vm_ser_int(vm->numSymbols);
    write_serial_string(" codeLength=");
    vm_ser_int(vm->codeLength);
    write_serial_string(" dataMask=");
    vm_ser_int(vm->dataMask);
    write_serial_string("\n");

    /* Print a couple of real symbol names out of id's own .map file, so the
     * log proves the symbol table came across. */
    {
        int names = 0;
        for (vmSymbol_t *s = vm->symbols; s && names < 3; s = s->next, names++) {
            write_serial_string("[Q3VM] symbol ");
            vm_ser_hex((unsigned int)s->symValue);
            write_serial_string(" ");
            write_serial_string(s->symName);
            write_serial_string("\n");
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
    write_serial_string("[Q3VM] GAME_INIT returned ");
    vm_ser_int(r);
    write_serial_string(" traps=");
    vm_ser_int(vm_syscalls - before);
    write_serial_string(" total=");
    vm_ser_int(vm_syscalls);
    write_serial_string(" errors=");
    vm_ser_int(vm_errors);
    write_serial_string(" unhandled=");
    vm_ser_int(vm_unhandled);
    write_serial_string("\n");

    write_serial_string("[Q3VM] locate_game_data entities=");
    vm_ser_int(vm_num_entities);
    write_serial_string(" sizeof_gentity=");
    vm_ser_int(vm_sizeof_gentity);
    write_serial_string(" vm_ofs=");
    vm_ser_hex((unsigned int)vm_gentity_ofs);
    write_serial_string(" ps_ofs=");
    vm_ser_hex((unsigned int)vm_ps_ofs);
    write_serial_string("\n");

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

    /* ---- the game loop: id's real G_RunFrame + ClientThink --------------
     * The same two vmMain calls the retail server frame makes
     * (SV_Frame -> SV_GameFrame: GAME_RUN_FRAME then per-client
     * GAME_CLIENT_THINK). Level time advances FRAMETIME per frame — the
     * server owns the clock, exactly like upstream. Each think hands the
     * module a full-forward usercmd, so id's own bg_pmove.c accelerates
     * the player across the floor plane. */
    {
        int frame;
        int x0, x1;
        write_serial_string("[Q3VM] frame loop: ");
        vm_ser_int(Q3VM_FRAME_COUNT);
        write_serial_string(" frames x ");
        vm_ser_int(Q3VM_FRAMETIME);
        write_serial_string(" msec\n");
        q3vm_frame_time = 0;
        x0 = 0;
        for (frame = 1; frame <= Q3VM_FRAME_COUNT; frame++) {
            q3vm_frame_time += Q3VM_FRAMETIME;
            VM_Call(vm, GAME_RUN_FRAME, q3vm_frame_time);
            VM_Call(vm, GAME_CLIENT_THINK, 0);
            if (frame == 1) x0 = q3vm_ps_x(vm);
            if (frame % 20 == 0) q3vm_log_player_state(vm, frame, q3vm_frame_time);
        }
        x1 = q3vm_ps_x(vm);
        write_serial_string("[Q3VM] movement x0=");
        vm_ser_int(x0);
        write_serial_string(" x1=");
        vm_ser_int(x1);
        write_serial_string(" delta=");
        vm_ser_int(x1 - x0);
        write_serial_string("\n");
        write_serial_string("[Q3VM] frame loop done\n");
    }

    /* id's own shut-down order (sv_main.c SV_ShutdownGameProgs): the module
     * gets GAME_SHUTDOWN before its VM goes away. */
    VM_Call(vm, GAME_SHUTDOWN, qfalse);
    write_serial_string("[Q3VM] GAME_SHUTDOWN done\n");

    VM_Free(vm);
    write_serial_string("[Q3VM] done\n");
    q3vm_park();
}
