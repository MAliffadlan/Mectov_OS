/* q3_uivm.c — host for id's official Quake III UI (q3ui.qvm) as Quake VM
 * bytecode. See q3_uivm.h for what this runs and why it is not pak0's
 * ui.qvm. v38.150.
 *
 * The shape of this file is deliberately the same as q3_vm.c's game bridge:
 * one system-call function with the module's own import numbering, id's
 * engine services behind it, and an explicit "inert" case for everything this
 * port does not have. What is NEW here is only the 2D draw path (q3ui2d) and
 * the input plumbing.
 */
#include <stdint.h>
#include <string.h>

#include "../q3a/code/game/q_shared.h"
#include "../q3a/code/qcommon/qcommon.h"
#include "../q3a/code/ui/ui_public.h"
#include "../q3a/code/cgame/tr_types.h"
#include "../q3a/code/qcommon/vm_local.h"

#include "q3_uivm.h"
#include "../tinygl/q3ui2d.h"

extern void write_serial_string(const char *s);
extern void write_serial_hex(uint32_t v);
extern uint32_t ext2_inode_size(uint32_t ino);

#define VMA(x)  VM_ArgPtr(args[x])

/* ------------------------------------------------------------------ */
/* State.                                                              */
/* ------------------------------------------------------------------ */
static vm_t *ui_vm;
static int   ui_ready;
static int   ui_traps, ui_errors, ui_unhandled;
static int   ui_frames, ui_keyevents, ui_mouseevents, ui_cmds;
static int   ui_catcher;
static int   ui_active_menu = UIMENU_MAIN;
static int   ui_keys[256];               /* UI_KEY_ISDOWN snapshot, K_ codes */
static q3uivm_clientstate_t ui_cs;

#define VMA_STR(x) ((const char *)VMA(x))

/* Defined with the command ring, below the trap table; used by UI_CMD_EXECUTETEXT. */
static void ui_route_text(int when, const char *text);

static int bits_to_float_i(int i) { union { int i; float f; } u; u.i = i; return u.i; }
static float args_float(int *args, int n) {
    union { int i; float f; } u;
    u.i = args[n];
    return u.f;
}
static int float_arg(float f) {
    union { int i; float f; } u;
    u.f = f;
    return u.i;
}

/* The kernel has no libm (it builds -msoft-float), so the UI's five maths
 * traps get the same home-grown series q3_vm.c uses for the game module:
 * range reduction + short polynomials, ~1e-6 accurate, which is far finer
 * than anything a menu's scaling or a scrollbar's tween needs. */
static float ui_fsqrt(float v) {
    float x;
    if (!(v > 0.0f)) return 0.0f;
    x = (v > 1.0f) ? v : 1.0f;
    for (int i = 0; i < 32; i++) x = 0.5f * (x + v / x);
    return x;
}
static float ui_fsin(float x) {
    const float PI = 3.14159265358979f;
    const float TWO_PI = 6.28318530717959f;
    float sign, a, x2;
    if (!(x > -1e9f && x < 1e9f)) return 0.0f;
    x = x - TWO_PI * (float)(int)(x * (1.0f / TWO_PI)
                                  + (x >= 0.0f ? 0.5f : -0.5f));
    if (x > PI)  x -= TWO_PI;
    if (x < -PI) x += TWO_PI;
    sign = (x < 0.0f) ? -1.0f : 1.0f;
    a = x * sign;
    if (a > PI * 0.5f) a = PI - a;
    x2 = a * a;
    return sign * (a * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f
          + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f
          + x2 * (-1.0f / 39916800.0f)))))));
}
static float ui_fcos(float x) { return ui_fsin(x + 1.57079632679490f); }
static float ui_fatan2(float y, float x) {
    const float PI = 3.14159265358979f;
    float a, u2, r;
    int big;
    if (x == 0.0f) {
        if (y == 0.0f) return 0.0f;
        return (y > 0.0f) ? PI * 0.5f : -PI * 0.5f;
    }
    a = (x < 0.0f) ? -y / x : y / x;
    big = (a > 1.0f);
    if (big) a = 1.0f / a;
    a = a / (1.0f + ui_fsqrt(1.0f + a * a));
    a = a / (1.0f + ui_fsqrt(1.0f + a * a));
    u2 = a * a;
    r = a * (1.0f + u2 * (-1.0f / 3.0f + u2 * (1.0f / 5.0f
        + u2 * (-1.0f / 7.0f + u2 * (1.0f / 9.0f)))));
    r = 4.0f * r;
    if (big) r = 1.57079632679490f - r;
    return (x < 0.0f) ? ((y >= 0.0f) ? r + PI : r - PI) : r;
}
static float ui_ffloor(float v) {
    int i = (int)v;
    return (float)((v < 0.0f && (float)i != v) ? i - 1 : i);
}
static float ui_fceil(float v) {
    int i = (int)v;
    return (float)((v > 0.0f && (float)i != v) ? i + 1 : i);
}

/* ------------------------------------------------------------------ */
/* The trap table.                                                     */
/* ------------------------------------------------------------------ */
static int q3ui_SystemCalls(int *args) {
    ui_traps++;
    switch (args[0]) {

    /* ---- util ---------------------------------------------------- */
    case UI_ERROR:
        ui_errors++;
        write_serial_string("[Q3UI] ERROR: ");
        write_serial_string(VMA_STR(1));
        write_serial_string("\n");
        return 0;
    case UI_PRINT:
        Com_Printf("%s", VMA_STR(1));
        return 0;
    case UI_MILLISECONDS:
    case UI_REAL_TIME:
        return Sys_Milliseconds();

    /* ---- cvars: the same services the game module's cvars ride on - */
    case UI_CVAR_SET:
        Cvar_Set(VMA_STR(1), VMA_STR(2));
        return 0;
    case UI_CVAR_SETVALUE:
        Cvar_SetValue(VMA_STR(1), args_float(args, 2));
        return 0;
    case UI_CVAR_VARIABLEVALUE:
        return float_arg(Cvar_VariableValue(VMA_STR(1)));
    case UI_CVAR_VARIABLESTRINGBUFFER:
        Cvar_VariableStringBuffer(VMA_STR(1), (char *)VMA(2), args[3]);
        return 0;
    case UI_CVAR_RESET:
        Cvar_Reset(VMA_STR(1));
        return 0;
    case UI_CVAR_CREATE:
        Cvar_Get(VMA_STR(1), VMA_STR(2), args[3]);
        return 0;
    case UI_CVAR_INFOSTRINGBUFFER:
        Cvar_InfoStringBuffer(args[1], (char *)VMA(2), args[3]);
        return 0;
    case UI_CVAR_REGISTER:
        Cvar_Register((vmCvar_t *)VMA(1), VMA_STR(2), VMA_STR(3), args[4]);
        return 0;
    case UI_CVAR_UPDATE:
        Cvar_Update((vmCvar_t *)VMA(1));
        return 0;

    /* ---- command line -------------------------------------------- */
    case UI_ARGC:
        return Cmd_Argc();
    case UI_ARGV:
        Cmd_ArgvBuffer(args[1], (char *)VMA(2), args[3]);
        return 0;
    case UI_CMD_EXECUTETEXT:
        ui_cmds++;
        ui_route_text(args[1], VMA_STR(2));
        return 0;

    /* ---- filesystem: id's own FS over the game volume -------------- */
    case UI_FS_FOPENFILE:
        return FS_FOpenFileByMode(VMA_STR(1), (fileHandle_t *)VMA(2),
                                  (fsMode_t)args[3]);
    case UI_FS_READ: {
        /* v38.150: FS_Read, NOT FS_Read2. FS_FOpenFileByMode flags EVERY
         * FS_READ handle as "streamed" (files.c: Sys_BeginStreamedFile, then
         * streamed = qtrue), and FS_Read2 routes the read through
         * Sys_StreamedRead — which this port has no read-ahead for, so its
         * stub returns 0 and the buffer comes back EMPTY. That is what the UI
         * was living with: scripts/arenas.txt and scripts/bots.txt opened
         * fine (438 and 379 bytes) and read as zero bytes, so
         * UI_LoadArenas printed "0 arenas parsed" and the CD-key gate
         * (which needs exactly 4 arenas to call itself a demo install) threw
         * up the retail CD-key screen instead of the main menu.
         * FS_Read has no streamed branch: it reads the handle's FILE at its
         * current position, which is where FS_FOpenFileByMode left it. */
        FS_Read(VMA(1), args[2], args[3]);
        return 0;
    }
    case UI_FS_WRITE:
        FS_Write(VMA(1), args[2], args[3]);
        return 0;
    case UI_FS_FCLOSEFILE:
        FS_FCloseFile(args[1]);
        return 0;
    case UI_FS_GETFILELIST:
        return FS_GetFileList(VMA_STR(1), VMA_STR(2), (char *)VMA(3), args[4]);
    case UI_FS_SEEK:
        return FS_Seek(args[1], args[2], args[3]);

    /* ---- the 2D draw path ----------------------------------------- */
    case UI_R_REGISTERSHADERNOMIP:
        return q3ui2d_register_shader(VMA_STR(1));
    case UI_R_SETCOLOR:
        q3ui2d_set_color((const float *)VMA(1));
        return 0;
    case UI_R_DRAWSTRETCHPIC:
        q3ui2d_draw_stretch_pic(args_float(args, 1), args_float(args, 2),
                                args_float(args, 3), args_float(args, 4),
                                args_float(args, 5), args_float(args, 6),
                                args_float(args, 7), args_float(args, 8),
                                args[9], 0.0f);
        return 0;
    case UI_R_REMAP_SHADER:
        return args[1];                 /* the UI passes a handle through */
    case UI_R_REGISTERFONT:
        /* 1.32 draws no TrueType text — its font is the gfx/2d/bigchars
         * bitmap, indexed glyph by glyph (ui_atoms.c UI_DrawString2). Only
         * Team Arena's rewrite asks for fonts. Report "no font" and the UI
         * falls back to its own charset. */
        return 0;
    case UI_R_MODELBOUNDS:
        if (VMA(1)) memset(VMA(1), 0, 3 * sizeof(float));
        if (VMA(2)) memset(VMA(2), 0, 3 * sizeof(float));
        return 0;

    /* ---- presentation: the driver owns the window ---------------- */
    case UI_UPDATESCREEN:
        return 0;

    /* ---- keyboard ------------------------------------------------ */
    case UI_KEY_ISDOWN:
        return (args[1] >= 0 && args[1] < 256 && ui_keys[args[1]]) ? 1 : 0;
    case UI_KEY_SETCATCHER:
        ui_catcher = args[1] | args[2];
        return ui_catcher;
    case UI_KEY_GETCATCHER:
        return ui_catcher;
    case UI_KEY_CLEARSTATES: {
        int i;
        for (i = 0; i < 256; i++) ui_keys[i] = 0;
        return 0;
    }
    case UI_KEY_SETOVERSTRIKEMODE:
        return 0;                      /* the menu has no text entry yet */
    case UI_KEY_GETOVERSTRIKEMODE:
        return 0;
    case UI_KEY_SETBINDING:
        /* No binding configuration exists on this volume to write (the Q3
         * config lives in the engine's tmpfs home and nothing reads it back),
         * so a bind from SETUP/controls is accepted and dropped. Said here
         * rather than in a comment only, because "the key does nothing" is
         * exactly the kind of silence that reads as a bug later. */
        ui_cmds++;
        return 0;
    case UI_KEY_GETBINDINGBUF:
        if (VMA(2)) ((char *)VMA(2))[0] = '\0';
        return 0;
    case UI_KEY_KEYNUMTOSTRINGBUF:
        if (VMA(2)) ((char *)VMA(2))[0] = '\0';
        return 0;

    /* ---- client state -------------------------------------------- */
    case UI_GETGLCONFIG: {
        glconfig_t *cfg = (glconfig_t *)VMA(1);
        if (cfg) {
            /* The UI virtualizes everything through this (uis.scale =
             * vidWidth/640), so reporting the virtualized size keeps its
             * layout on id's pixel grid; q3ui2d area-averages down to the
             * window on the way out. See q3ui2d.h. */
            cfg->vidWidth = Q3UI_W;
            cfg->vidHeight = Q3UI_H;
            cfg->windowAspect = (float)Q3UI_W / (float)Q3UI_H;
            cfg->isFullscreen = qfalse;
            cfg->stereoEnabled = qfalse;
            cfg->smpActive = qfalse;
        }
        return 0;
    }
    case UI_GETCLIENTSTATE: {
        uiClientState_t *cs = (uiClientState_t *)VMA(1);
        if (cs) {
            memset(cs, 0, sizeof(*cs));
            cs->connState = (connstate_t)ui_cs.conn_state;
            cs->clientNum = ui_cs.client_num;
            if (ui_cs.servername)
                Q_strncpyz(cs->servername, ui_cs.servername, sizeof(cs->servername));
            if (ui_cs.update_info)
                Q_strncpyz(cs->updateInfoString, ui_cs.update_info,
                           sizeof(cs->updateInfoString));
            if (ui_cs.message)
                Q_strncpyz(cs->messageString, ui_cs.message,
                           sizeof(cs->messageString));
        }
        return 0;
    }
    case UI_GETCONFIGSTRING:
        /* The 1.32 UI reads CS_LEVEL (the map's name) while it is in-game;
         * the driver puts that in a cvar of its own and the menu's own copy
         * comes from the game module's configstrings, which this port does
         * not publish to the VM. Empty is the honest answer; the in-game menu
         * still draws. */
        if (VMA(2)) ((char *)VMA(2))[0] = '\0';
        return 0;
    case UI_GETCLIPBOARDDATA:
        if (VMA(2)) ((char *)VMA(2))[0] = '\0';
        return 0;

    /* ---- sound: the menu is silent (v38.142 removed the bridge) ----- */
    case UI_S_REGISTERSOUND:
        return 0;
    case UI_S_STARTLOCALSOUND:
    case UI_S_STOPBACKGROUNDTRACK:
    case UI_S_STARTBACKGROUNDTRACK:
        return 0;

    /* ---- no 3D, no net, no cinematics, no CD key ------------------ */
    case UI_R_REGISTERMODEL:
    case UI_R_REGISTERSKIN:
    case UI_R_CLEARSCENE:
    case UI_R_ADDREFENTITYTOSCENE:
    case UI_R_ADDPOLYTOSCENE:
    case UI_R_ADDLIGHTTOSCENE:
    case UI_R_RENDERSCENE:
    case UI_CM_LERPTAG:
    case UI_CM_LOADMODEL:
        return 0;
    case UI_GET_CDKEY:
    case UI_SET_CDKEY:
    case UI_VERIFY_CDKEY:
    case UI_SET_PBCLSTATUS:
        return 0;

    /* ---- in-VM libc/maths (ui_syscalls.asm: memset -101 …) -------- */
    case UI_MEMSET:
        memset(VMA(1), args[2], args[3]);
        return args[1];
    case UI_MEMCPY:
        memcpy(VMA(1), VMA(2), args[3]);
        return args[1];
    case UI_STRNCPY:
        strncpy((char *)VMA(1), VMA_STR(2), args[3]);
        if (args[3] > 0) ((char *)VMA(1))[args[3] - 1] = '\0';
        return args[1];
    case UI_SIN:
        return float_arg(ui_fsin(args_float(args, 1)));
    case UI_COS:
        return float_arg(ui_fcos(args_float(args, 1)));
    case UI_ATAN2:
        return float_arg(ui_fatan2(args_float(args, 1), args_float(args, 2)));
    case UI_SQRT:
        return float_arg(ui_fsqrt(args_float(args, 1)));
    case UI_FLOOR:
        return float_arg(ui_ffloor(args_float(args, 1)));
    case UI_CEIL:
        return float_arg(ui_fceil(args_float(args, 1)));

    default:
        /* Anything else is a trap this port has not been asked for yet
         * (the LAN set, the Team Arena PC_* parser, the cinematics). Count
         * it and answer zero rather than fall through into whatever the
         * module does with the result. */
        ui_unhandled++;
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* The commands the UI asks for.                                       */
/* ------------------------------------------------------------------ */
/* id's UI has no way to call the host except the console: choosing a level
 * makes ui_sparena.c execute "spmap q3dm1", leaving a match executes
 * "disconnect", quitting executes "quit", the add-bot screen executes
 * "addbot <name> <skill> <team>". In retail those land in the client, which
 * forwards them to a server; this port has no server, and the game module
 * registers no console commands of its own (in the VM build it is entered
 * through GAME_CLIENT_COMMAND, which nothing here calls).
 *
 * So the verbs that MEAN something to this port are captured here, in order,
 * for the driver to act on (q3uivm_take_command), and everything else — the
 * cvars the setup screen writes, `exec`, `set`, the cvar_restart family — goes
 * to id's own command buffer exactly as before. A captured command is NOT
 * forwarded: the driver owns what happens next, and forwarding would execute
 * a second, competing action.
 *
 * The ring holds eight: the UI issues one command per action, and a driver
 * that has not drained it by the next frame gets the oldest, not the newest. */
#define UI_CMD_RING 8
static char          ui_ring[UI_CMD_RING][64];
static int           ui_ring_head, ui_ring_count;

static int ui_verb_is(const char *line, const char *verb) {
    int i = 0;
    while (verb[i] && line[i] == verb[i]) i++;
    if (!verb[i]) return 1;
    return line[i] == '\0' || line[i] == '\n' || line[i] == ' ' || line[i] == '\t';
}

static int ui_verb_capture(const char *line) {
    /* Verb table, in the order they appear in the UI's source. Each is a
     * thing only the driver can do; none of them exists as an engine command
     * here, so forwarding them would be a no-op at best. */
    static const char *const verbs[] = {
        "spmap",        /* ui_sparena.c: start single player on this map   */
        "addbot",       /* ui_addbots.c: the bot list (phase 6: botlib)    */
        "removebot", "clientkick",
        "disconnect",   /* every "leave match" path                       */
        "quit",         /* every "exit" path                              */
        "demo", "cinematic", "screenshot", "record", "playmovie",
        "vid_restart", "snd_restart", "in_restart"
    };
    unsigned i;
    for (i = 0; i < sizeof(verbs) / sizeof(verbs[0]); i++)
        if (ui_verb_is(line, verbs[i])) return 1;
    return 0;
}

/* Split on ';' and newline (id's UI uses both: "disconnect; cinematic x.RoQ"),
 * route each piece, ignore empty ones. */
static void ui_route_text(int when, const char *text) {
    char line[80];
    int n = 0;
    if (!text) return;
    for (;;) {
        char ch = *text++;
        if (ch == '\0' || ch == '\n' || ch == '\r' || ch == ';') {
            line[n] = '\0';
            if (n > 0) {
                if (ui_verb_capture(line)) {
                    int slot = (ui_ring_head + ui_ring_count) % UI_CMD_RING;
                    int i;
                    for (i = 0; line[i] && i < (int)sizeof(ui_ring[0]) - 1; i++)
                        ui_ring[slot][i] = line[i];
                    ui_ring[slot][i] = '\0';
                    if (ui_ring_count < UI_CMD_RING) ui_ring_count++;
                } else {
                    Cbuf_ExecuteText(when, line);
                }
            }
            n = 0;
            if (ch == '\0') break;
            continue;
        }
        if (n < (int)sizeof(line) - 1) line[n++] = ch;
    }
}

int q3uivm_take_command(char *out, int out_size) {
    if (ui_ring_count <= 0 || !out || out_size < 2) return 0;
    {
        int i;
        for (i = 0; i < (int)sizeof(ui_ring[0]) - 1 && ui_ring[ui_ring_head][i]; i++)
            out[i] = ui_ring[ui_ring_head][i];
        out[i] = '\0';
        ui_ring_head = (ui_ring_head + 1) % UI_CMD_RING;
        ui_ring_count--;
        return 1;
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle.                                                          */
/* ------------------------------------------------------------------ */
int q3uivm_init(void) {
    int api;

    if (ui_ready) return 0;
    if (q3ui2d_init() != 0) {
        write_serial_string("[Q3UI] no memory for the 640x480 UI surface\n");
        return -1;
    }
    ui_vm = VM_Create("q3ui", q3ui_SystemCalls, VMI_BYTECODE);
    if (!ui_vm) {
        write_serial_string("[Q3UI] no vm/q3ui.qvm on the game volume "
                            "(scripts/build_qvm_ui.sh + seed_ext2.sh)\n");
        q3ui2d_shutdown();
        return -1;
    }
    api = VM_Call(ui_vm, UI_GETAPIVERSION);
    write_serial_string("[Q3UI] module api=");
    write_serial_hex((uint32_t)api);
    write_serial_string("\n");
    /* UI_INIT takes the in-game flag in Team Arena and nothing in 1.32;
     * passing 0 is correct for the module this port runs. */
    VM_Call(ui_vm, UI_INIT, 0);
    /* UI_Refresh() returns before drawing anything unless the engine says the
     * UI has the keyboard (ui_atoms.c: "if (!(trap_Key_GetCatcher() &
     * KEYCATCH_UI)) return"), so the catcher is set HERE, at the same point
     * id's client sets it: while the menu owns the input. The console shares
     * the catcher, exactly as in retail. */
    ui_catcher = KEYCATCH_UI | KEYCATCH_CONSOLE;
    ui_ready = 1;
    return 0;
}

void q3uivm_shutdown(void) {
    if (ui_ready) {
        VM_Call(ui_vm, UI_SHUTDOWN);
        VM_Free(ui_vm);
        ui_vm = NULL;
        ui_ready = 0;
    }
    q3ui2d_shutdown();
}

int q3uivm_ready(void) { return ui_ready; }

void q3uivm_frame(int realtime_ms) {
    if (!ui_ready) return;
    q3ui2d_frame_reset();
    VM_Call(ui_vm, UI_REFRESH, realtime_ms);
    ui_frames++;
}

void q3uivm_key(int key, int down) {
    if (!ui_ready) return;
    if (key >= 0 && key < 256) ui_keys[key] = down ? 1 : 0;
    VM_Call(ui_vm, UI_KEY_EVENT, key, down ? 1 : 0);
    ui_keyevents++;
}

void q3uivm_mouse(int dx, int dy) {
    if (!ui_ready) return;
    VM_Call(ui_vm, UI_MOUSE_EVENT, dx, dy);
    ui_mouseevents++;
}

int q3uivm_is_fullscreen(void) { return 0; }

int q3uivm_active_menu(void) { return ui_active_menu; }

void q3uivm_set_active_menu(int menu) {
    ui_active_menu = menu;
    if (ui_ready) VM_Call(ui_vm, UI_SET_ACTIVE_MENU, menu);
}

void q3uivm_set_clientstate(const q3uivm_clientstate_t *cs) {
    if (cs) ui_cs = *cs;
}

void q3uivm_set_key_state(int key, int down) {
    if (key >= 0 && key < 256) ui_keys[key] = down ? 1 : 0;
}

void q3uivm_clear_key_states(void) {
    int i;
    for (i = 0; i < 256; i++) ui_keys[i] = 0;
}

void q3uivm_stats(int *traps, int *errors, int *unhandled, int *frames,
                  int *keyevents, int *mouseevents, int *cmds) {
    if (traps) *traps = ui_traps;
    if (errors) *errors = ui_errors;
    if (unhandled) *unhandled = ui_unhandled;
    if (frames) *frames = ui_frames;
    if (keyevents) *keyevents = ui_keyevents;
    if (mouseevents) *mouseevents = ui_mouseevents;
    if (cmds) *cmds = ui_cmds;
}