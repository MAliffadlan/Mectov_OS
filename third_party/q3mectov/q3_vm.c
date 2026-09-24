/* q3_vm.c — Quake III Arena, from id Software's OFFICIAL source (v38.105).
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
 * No game data is involved: bytecode needs none. The game module's trap_*
 * calls come back into the kernel through Q3VM_SystemCalls below, which is
 * the kernel-side twin of the retail engine's own SV_GameSystemCalls().
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

/* The game's trap_LocateGameData hands us where gentities live in the VM's
 * data segment. Nothing else in this demo walks entities, so the values are
 * recorded for the log — the call itself is what matters (it is how the
 * module learns where the server will look for its entity array). */
static int vm_gentity_ofs = -1;
static int vm_num_entities;
static int vm_sizeof_gentity;

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
        return 0;

    /* ---- the entity string: what the retail server gets from the .bsp ----
     * G_SpawnEntitiesFromString() tokenises the level's entity text through
     * G_GET_ENTITY_TOKEN, exactly like SV_InitGameVM does via
     * CM_EntityString(). With no .bsp we hand the module a minimal worldspawn
     * — the same thing a real map file begins with — so the module walks its
     * real spawn path instead of erroring out. COM_Parse over a static buffer
     * mirrors id's own sv.entityParsePoint mechanic. */
    case G_GET_ENTITY_TOKEN: {
        static char entbuf[] = "{ \"classname\" \"worldspawn\" }\n";
        static char *entp = 0;
        const char *s;
        if (!entp) entp = entbuf;
        s = COM_Parse(&entp);
        Q_strncpyz((char *)VMA(1), s, args[2]);
        if (!entp && !s[0]) return 0;   /* end of spawn string */
        return 1;
    }

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
    case G_GET_USERINFO:
    case G_SET_USERINFO:
    case G_SET_BRUSH_MODEL:
    case G_LINKENTITY:
    case G_UNLINKENTITY:
    case G_TRACE:
    case G_TRACECAPSULE:
    case G_ADJUST_AREA_PORTAL_STATE:
    case G_DROP_CLIENT:
    case G_SEND_SERVER_COMMAND:
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
    case G_GET_USERCMD:
        memset(VMA(1), 0, args[2]);
        return 0;
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
    write_serial_string("\n");

    /* id's own shut-down order (sv_main.c SV_ShutdownGameProgs): the module
     * gets GAME_SHUTDOWN before its VM goes away. */
    VM_Call(vm, GAME_SHUTDOWN, qfalse);
    write_serial_string("[Q3VM] GAME_SHUTDOWN done\n");

    VM_Free(vm);
    write_serial_string("[Q3VM] done\n");
    q3vm_park();
}
