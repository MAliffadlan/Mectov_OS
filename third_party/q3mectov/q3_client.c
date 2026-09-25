/* q3_client.c — the Mectov OS client layer for the ioquake3 subset
 * (v38.103, Q3 phase 3: CL_Init/CL_Frame + renderer + WM input).
 *
 * Phase 1 (`q3`) ran Com_Init/Com_Frame with the upstream *null* client
 * (code/null/null_client.c): the engine core booted, but CL_Frame was an
 * empty function. Phase 2 (`q3gl`) proved TinyGL renders into a WM window.
 * Phase 3 replaces the null client with a real one that lives in this file:
 *
 *   desktop loop                client kernel task (q3play)
 *   -----------------------     -----------------------------------------
 *   wm_handle_scancode()  ---\
 *   wm_capture_event()    ---/-> ring buffer (own spinlock)
 *                                  |
 *                                  +-> clq_pump(): Com_QueueEvent(SE_KEY /
 *                                  |   SE_CHAR / SE_MOUSE) — the ENGINE's own
 *                                  |   event queue
 *                                  |
 *                                  +-> Com_Frame()
 *                                        Com_EventLoop()  (engine dispatch:
 *                                          SE_KEY  -> CL_KeyEvent   -.
 *                                          SE_CHAR -> CL_CharEvent   |- this
 *                                          SE_MOUSE-> CL_MouseEvent  |  file
 *                                          ...)                     -'
 *                                        SV_Frame()  (stub)
 *                                        CL_Frame(msec) -> movement +
 *                                          q3ref_* TinyGL render ->
 *                                          wm_invalidate() -> compositor
 *
 * Two decisions worth spelling out:
 *
 * 1. Input goes through Com_QueueEvent, not straight into our own handlers.
 *    The engine's Com_EventLoop() already dispatches SE_KEY/SE_CHAR/SE_MOUSE
 *    to CL_KeyEvent/CL_CharEvent/CL_MouseEvent, so the WM events take the same
 *    path a real ioquake3 build's SDL events do — including the chance for any
 *    other engine subscriber to see them. The ring buffer exists only because
 *    the producer runs on the compositor's context and the consumer on this
 *    task, so the hand-off needs its own lock (wm_lock is held by the caller
 *    when the callbacks fire, and the engine's event queue is not reentrant).
 *
 * 2. The engine still boots with `+set dedicated 0` but the *client* is ours.
 *    Upstream code/client/ is not vendored: a real ioquake3 client needs the
 *    ui and cgame QVMs plus pak0 assets, none of which exist in this tree
 *    (STANDALONE, no game data). So CL_Init/CL_Frame/CL_KeyEvent... are
 *    implemented here against the same qcommon engine, the same event queue
 *    and the same key-binding/command system (bind / +forward / Cbuf), which
 *    is what "connect the client loop to the renderer" means for a port that
 *    ships no game data.
 *
 * Movement is the Q3 model, simplified: the camera is a point with yaw/pitch
 * in Quake units (z up), input sets movement flags through +commands, and the
 * frame integrates position. Mouse look is applied at event time exactly like
 * CL_MouseEvent does upstream, so aiming latency is one frame at most.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

/* ioq3 types + engine API. Included by relative path, like q3_platform.c, so
 * the libc stub headers never shadow the engine's own headers. */
#include "../q3a/code/game/q_shared.h"
#include "../q3a/code/qcommon/qcommon.h"

#include "../../src/include/wm.h"
#include "../../src/include/theme.h"
#include "../../src/include/spinlock.h"
#include "../tinygl/q3cl_render.h"
#include "q3_map.h"   /* phase 4: the world + player start come from data */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Lock-up tracing. The client is the first code in this kernel that is
 * re-entered from two contexts at once (compositor callbacks pushing into a
 * ring the client task drains), so when something wedges, "which side was
 * holding what" is the only useful question. Flip to 1 to get one line per
 * phase per frame around the pump/engine/render seams. */
#define Q3CL_TRACE 0
#if Q3CL_TRACE
#define CLTR(s) write_serial_string("[CL] " s "\n")
#else
#define CLTR(s) do { } while (0)
#endif

/* --- kernel services -------------------------------------------------- */
extern void     write_serial_string(const char *s);
extern void     write_serial_hex(uint32_t v);
extern uint32_t get_ticks(void);
extern int      get_win_index(int wid);
extern volatile int needs_redraw;
extern uint32_t fb_width, fb_height;

/* Serial helpers: markers are hex elsewhere in this kernel, but the client's
 * markers carry signed camera coordinates, so print decimal. */
static void ser_int(int v) {
    static char buf[13];
    int n = 0;
    unsigned u;
    if (v < 0) { write_serial_string("-"); u = (unsigned)(-v); } else u = (unsigned)v;
    if (u == 0) {
        write_serial_string("0");
        return;
    }
    while (u && n < 12) { buf[n++] = (char)('0' + (u % 10)); u /= 10; }
    for (int i = 0; i < n / 2; i++) {   /* digits are LSB-first: reverse */
        char t = buf[i];
        buf[i] = buf[n - 1 - i];
        buf[n - 1 - i] = t;
    }
    buf[n] = '\0';
    write_serial_string(buf);
}

/* --- key numbers (subset of ioquake3's keycodes.h) -------------------- */
#define K_TAB        9
#define K_ENTER      13
#define K_ESCAPE     27
#define K_SPACE      32
#define K_BACKSPACE  127
#define K_UPARROW    132
#define K_DOWNARROW  133
#define K_LEFTARROW  134
#define K_RIGHTARROW 135
#define K_ALT        136
#define K_CTRL       137
#define K_SHIFT      138
#define K_INS        139
#define K_DEL        140
#define K_PGDN       141
#define K_PGUP       142
#define K_HOME       143
#define K_END        144
#define K_F1         145
#define K_MOUSE1     200
#define K_MOUSE2     201
#define K_MOUSE3     202

static const char *cl_keynames[256] = {
    [K_TAB] = "TAB", [K_ENTER] = "ENTER", [K_ESCAPE] = "ESCAPE",
    [K_SPACE] = "SPACE", [K_BACKSPACE] = "BACKSPACE",
    [K_UPARROW] = "UPARROW", [K_DOWNARROW] = "DOWNARROW",
    [K_LEFTARROW] = "LEFTARROW", [K_RIGHTARROW] = "RIGHTARROW",
    [K_ALT] = "ALT", [K_CTRL] = "CTRL", [K_SHIFT] = "SHIFT",
    [K_INS] = "INS", [K_DEL] = "DEL", [K_PGDN] = "PGDN",
    [K_PGUP] = "PGUP", [K_HOME] = "HOME", [K_END] = "END",
    [K_F1] = "F1", [K_F1 + 1] = "F2", [K_F1 + 2] = "F3", [K_F1 + 3] = "F4",
    [K_F1 + 4] = "F5", [K_F1 + 5] = "F6", [K_F1 + 6] = "F7", [K_F1 + 7] = "F8",
    [K_F1 + 8] = "F9", [K_F1 + 9] = "F10", [K_F1 + 10] = "F11", [K_F1 + 11] = "F12",
    [K_MOUSE1] = "MOUSE1", [K_MOUSE2] = "MOUSE2", [K_MOUSE3] = "MOUSE3",
};

static char cl_keyname_ascii[2];

const char *Key_KeynumToString(int keynum) {
    if (keynum < 0 || keynum > 255) return "<KEYNOTFOUND>";
    if (keynum == ' ') return "SPACE";
    if (keynum > 32 && keynum < 127) {
        cl_keyname_ascii[0] = (char)keynum;
        cl_keyname_ascii[1] = '\0';
        return cl_keyname_ascii;
    }
    if (cl_keynames[keynum] && cl_keynames[keynum][0]) return cl_keynames[keynum];
    return "<KEYNOTFOUND>";
}

static int Key_StringToKeynum(const char *str) {
    if (!str || !str[0]) return -1;
    if (!str[1]) return (unsigned char)str[0];        /* single character */
    for (int i = 0; i < 256; i++) {
        const char *n = cl_keynames[i];
        if (!n || !n[0]) continue;
        if (!strcmp(str, n)) return i;
    }
    /* accept lowercase names too ("mouse1") */
    for (int i = 0; i < 256; i++) {
        const char *n = cl_keynames[i];
        if (!n || !n[0]) continue;
        size_t l = strlen(n);
        if (strlen(str) != l) continue;
        int same = 1;
        for (size_t k = 0; k < l; k++)
            if ((str[k] & ~0x20) != (n[k] & ~0x20)) { same = 0; break; }
        if (same) return i;
    }
    return -1;
}

/* PS/2 set-1 scancode -> key number. Only what a game needs is mapped; the
 * kernel already resolved characters for the desktop, but the client wants
 * physical keys (a held 'w' is not the character 'w'). */
static const int cl_scmap[0x80] = {
    [0x01] = K_ESCAPE,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=',
    [0x0E] = K_BACKSPACE, [0x0F] = K_TAB,
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']', [0x1C] = K_ENTER, [0x1D] = K_CTRL,
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
    [0x28] = '\'', [0x29] = '`', [0x2A] = K_SHIFT, [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x36] = K_SHIFT, [0x38] = K_ALT, [0x39] = K_SPACE,
    [0x3B] = K_F1, [0x3C] = K_F1 + 1, [0x3D] = K_F1 + 2, [0x3E] = K_F1 + 3,
    [0x3F] = K_F1 + 4, [0x40] = K_F1 + 5, [0x41] = K_F1 + 6, [0x42] = K_F1 + 7,
    [0x43] = K_F1 + 8, [0x44] = K_F1 + 9, [0x57] = K_F1 + 10, [0x58] = K_F1 + 11,
    [0x47] = K_HOME, [0x48] = K_UPARROW, [0x49] = K_PGUP,
    [0x4B] = K_LEFTARROW, [0x4D] = K_RIGHTARROW,
    [0x4F] = K_END, [0x50] = K_DOWNARROW, [0x51] = K_PGDN,
    [0x52] = K_INS, [0x53] = K_DEL,
};

/* --- client state ----------------------------------------------------- */
#define Q3CL_W 320
#define Q3CL_H 240

static int          cl_win_id = -1;
static volatile int cl_running;
static int          cl_inited;
/* msg.c reads this global directly (it is the client's network-dump cvar). */
cvar_t             *cl_shownet;

/* movement flags, set by the +commands the bindings execute */
#define MF_FORWARD    0x0001
#define MF_BACK       0x0002
#define MF_MOVELEFT   0x0004
#define MF_MOVERIGHT  0x0008
#define MF_MOVEUP     0x0010
#define MF_MOVEDOWN   0x0020
#define MF_SPEED      0x0040
#define MF_ATTACK     0x0080
#define MF_TURNLEFT   0x0100
#define MF_TURNRIGHT  0x0200
#define MF_LOOKUP     0x0400
#define MF_LOOKDOWN   0x0800
static unsigned cl_move;

/* camera / player (Quake units, z up). yaw 0 = +Y, positive yaw = left;
 * positive pitch = UP (the renderer's convention — see q3cl_render.h). */
static float cl_x = 0.0f, cl_y = 180.0f, cl_z = Q3REF_EYE_H;
static float cl_yaw = 180.0f, cl_pitch = 0.0f;

static uint32_t cl_frames;
static double   cl_time_base;
static uint32_t cl_log_console_budget = 64;
/* phase 4: consecutive blocked-frame counter while the player tries to walk
 * into a map brush — drives the one-shot "move blocked brush" marker. */
static unsigned cl_blocked_frames;

static char     bindings[256][64];

/* --- WM -> client event ring ----------------------------------------- */
#define CLQ_SIZE 128
#define CLQ_KEY   1
#define CLQ_MOUSE 2

typedef struct { int type; int v1, v2, v3; } clq_ev_t;

static clq_ev_t  clq[CLQ_SIZE];
static volatile int clq_head, clq_tail;
static spinlock_t clq_lock = SPINLOCK_INIT;
static uint8_t   cl_key_held[128];   /* producer-side typematic filter */
static int       cl_prev_btn;        /* consumer-side button edge state */

static void clq_push(int type, int v1, int v2, int v3) {
    uint32_t ef = spin_lock_irqsave(&clq_lock);
    int nxt = (clq_head + 1) % CLQ_SIZE;
    if (nxt != clq_tail) {
        clq[clq_head].type = type;
        clq[clq_head].v1 = v1;
        clq[clq_head].v2 = v2;
        clq[clq_head].v3 = v3;
        clq_head = nxt;
    }
    spin_unlock_irqrestore(&clq_lock, ef);
}

static int clq_pop(clq_ev_t *out) {
    uint32_t ef = spin_lock_irqsave(&clq_lock);
    int got = 0;
    if (clq_tail != clq_head) {
        *out = clq[clq_tail];
        clq_tail = (clq_tail + 1) % CLQ_SIZE;
        got = 1;
    }
    spin_unlock_irqrestore(&clq_lock, ef);
    return got;
}

/* --- window callbacks (compositor context) --------------------------- */

/* Raw scancodes come in press AND release. The kernel's scancode queue keeps
 * repeating a held key (typematic), so filter by state before queueing: this
 * is the same guard the DOOM port uses, and it keeps the serial log honest. */
static void cl_win_key(int id, char c, uint8_t sc) {
    (void)id;
    uint8_t base = sc & 0x7F;
    int down = !(sc & 0x80);
    if (base >= 128) return;
    if (cl_scmap[base] == 0) return;   /* unmapped (keypad, caps, 0xE0 prefix) */
    if (cl_key_held[base] == (uint8_t)down) return;
    cl_key_held[base] = (uint8_t)down;
    clq_push(CLQ_KEY, sc, (int)(unsigned char)c, 0);
}

/* Relative motion while the window owns the capture (see wm_capture_mouse);
 * absolute clicks otherwise, which still deliver button edges. */
static void cl_win_mouse(int id, int cx, int cy, int btn) {
    (void)id;
    if (wm_capture_owner() != cl_win_id) {
        clq_push(CLQ_MOUSE, 0, 0, btn);
        return;
    }
    clq_push(CLQ_MOUSE, cx, cy, btn);
}

static void cl_win_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id; (void)cx; (void)cy;
    int idx = get_win_index(id);
    if (idx < 0) return;
    if (wm_wins[idx].resizing) return;
    uint32_t *dst = wm_wins[idx].content_buffer;
    if (!dst) return;
    q3ref_blit(dst, cw, ch);
}

/* --- engine event pump: ring -> Com_QueueEvent ----------------------- */
static void clq_pump(void) {
    clq_ev_t ev;
    while (clq_pop(&ev)) {
        if (ev.type == CLQ_KEY) {
            int sc = ev.v1;
            int down = !(sc & 0x80);
            int kn = cl_scmap[sc & 0x7F];
            if (kn) Com_QueueEvent(0, SE_KEY, kn, down, 0, NULL);
            if (down && ev.v2) Com_QueueEvent(0, SE_CHAR, ev.v2, 0, 0, NULL);
        } else if (ev.type == CLQ_MOUSE) {
            if (ev.v1 || ev.v2) Com_QueueEvent(0, SE_MOUSE, ev.v1, ev.v2, 0, NULL);
            if (ev.v3 != cl_prev_btn) {
                for (int b = 0; b < 3; b++) {
                    int now = (ev.v3 >> b) & 1, was = (cl_prev_btn >> b) & 1;
                    if (now != was)
                        Com_QueueEvent(0, SE_KEY, K_MOUSE1 + b, now ? qtrue : qfalse, 0, NULL);
                }
                cl_prev_btn = ev.v3;
            }
        }
    }
}

/* --- bindings --------------------------------------------------------- */
static void cl_log_bind(int key, const char *cmd) {
    write_serial_string("[Q3CL] bind ");
    write_serial_string(Key_KeynumToString(key));
    write_serial_string(" ");
    write_serial_string(cmd);
    write_serial_string("\n");
}

static void CL_SetBinding(int key, const char *cmd) {
    if (key < 0 || key > 255) return;
    if (cmd) {
        int i = 0;
        while (cmd[i] && i < 63) { bindings[key][i] = cmd[i]; i++; }
        bindings[key][i] = '\0';
    } else {
        bindings[key][0] = '\0';
    }
    cl_log_bind(key, bindings[key][0] ? bindings[key] : "(unbound)");
}

static void CL_Bind_f(void) {
    int argc = Cmd_Argc();
    if (argc < 2) {
        for (int i = 0; i < 256; i++)
            if (bindings[i][0])
                Com_Printf("\"%s\" = \"%s\"\n", Key_KeynumToString(i), bindings[i]);
        return;
    }
    int key = Key_StringToKeynum(Cmd_Argv(1));
    if (key < 0) {
        Com_Printf("%s: unknown key \"%s\"\n", Cmd_Argv(0), Cmd_Argv(1));
        return;
    }
    if (argc == 2) {
        Com_Printf("\"%s\" = \"%s\"\n", Key_KeynumToString(key), bindings[key]);
        return;
    }
    /* join the rest of the line, so `bind x "say hi there"` works */
    static char cmd[64];
    int n = 0;
    for (int i = 2; i < argc; i++) {
        const char *a = Cmd_Argv(i);
        if (i > 2 && n < 62) cmd[n++] = ' ';
        for (int k = 0; a[k] && n < 62; k++) cmd[n++] = a[k];
    }
    cmd[n] = '\0';
    CL_SetBinding(key, cmd);
}

static void CL_Unbind_f(void) {
    if (Cmd_Argc() != 2) { Com_Printf("usage: unbind <key>\n"); return; }
    int key = Key_StringToKeynum(Cmd_Argv(1));
    if (key < 0) { Com_Printf("unbind: unknown key \"%s\"\n", Cmd_Argv(1)); return; }
    CL_SetBinding(key, NULL);
}

static void CL_UnbindAll_f(void) {
    for (int i = 0; i < 256; i++) bindings[i][0] = '\0';
    write_serial_string("[Q3CL] unbindall\n");
}

static void CL_BindList_f(void) {
    for (int i = 0; i < 256; i++)
        if (bindings[i][0])
            Com_Printf("%s \"%s\"\n", Key_KeynumToString(i), bindings[i]);
}

void Key_KeynameCompletion(void (*callback)(const char *s)) {
    if (!callback) return;
    for (int i = 0; i < 256; i++) {
        const char *n = cl_keynames[i];
        if (n && n[0]) callback(n);
    }
}

/* The engine writes the user's binds through this while saving q3config.cfg. */
void Key_WriteBindings(fileHandle_t f) {
    if (f <= 0) return;
    for (int i = 0; i < 256; i++) {
        if (!bindings[i][0]) continue;
        char line[128];
        int n = 0;
        const char *k = Key_KeynumToString(i);
        line[n++] = 'b'; line[n++] = 'i'; line[n++] = 'n'; line[n++] = 'd';
        line[n++] = ' ';
        for (int j = 0; k[j] && n < 40; j++) line[n++] = k[j];
        line[n++] = ' ';
        line[n++] = '"';
        for (int j = 0; bindings[i][j] && n < 120; j++) line[n++] = bindings[i][j];
        line[n++] = '"';
        line[n++] = '\n';
        FS_Write(line, n, f);
    }
}

void CL_InitKeyCommands(void) {
    Cmd_AddCommand("bind", CL_Bind_f);
    Cmd_AddCommand("unbind", CL_Unbind_f);
    Cmd_AddCommand("unbindall", CL_UnbindAll_f);
    Cmd_AddCommand("bindlist", CL_BindList_f);
    write_serial_string("[Q3CL] key commands registered\n");
}

/* --- +commands ------------------------------------------------------- */
static void cl_plus_cmd(const char *name, int down) {
    unsigned bit = 0;
    if      (!strcmp(name, "forward"))   bit = MF_FORWARD;
    else if (!strcmp(name, "back"))      bit = MF_BACK;
    else if (!strcmp(name, "moveleft"))  bit = MF_MOVELEFT;
    else if (!strcmp(name, "moveright")) bit = MF_MOVERIGHT;
    else if (!strcmp(name, "moveup"))    bit = MF_MOVEUP;
    else if (!strcmp(name, "movedown"))  bit = MF_MOVEDOWN;
    else if (!strcmp(name, "speed"))     bit = MF_SPEED;
    else if (!strcmp(name, "attack"))    bit = MF_ATTACK;
    else if (!strcmp(name, "left"))      bit = MF_TURNLEFT;
    else if (!strcmp(name, "right"))     bit = MF_TURNRIGHT;
    else if (!strcmp(name, "lookup"))    bit = MF_LOOKUP;
    else if (!strcmp(name, "lookdown"))  bit = MF_LOOKDOWN;
    else return;   /* unknown +command: ignore, like a missing client command */
    if (down) cl_move |= bit; else cl_move &= ~bit;
}

/* --- input events (dispatched by the ENGINE's Com_EventLoop) --------- */

void CL_KeyEvent(int key, qboolean down, unsigned time) {
    (void)time;
    if (key < 0 || key > 255) return;

    const char *b = bindings[key];
    write_serial_string(down ? "[Q3CL] key down " : "[Q3CL] key up ");
    write_serial_string(Key_KeynumToString(key));
    if (b[0]) {
        write_serial_string(" bind=");
        write_serial_string(b);
    }
    write_serial_string("\n");

    if (key == K_ESCAPE && down) {
        write_serial_string("[Q3CL] ESC -> quit\n");
        cl_running = 0;
        return;
    }

    if (b[0] == '+') {
        cl_plus_cmd(b + 1, down);
    } else if (b[0] && down) {
        /* plain bindings go through the engine's command buffer, exactly like
         * a key press does in the real client */
        Cbuf_AddText(b);
        Cbuf_AddText("\n");
    }
}

void CL_CharEvent(int key) {
    (void)key;   /* the client layer has no chat/console input surface yet */
}

void CL_MouseEvent(int dx, int dy, int time) {
    (void)time;
    float sens = Cvar_VariableValue("sensitivity");
    float myaw = Cvar_VariableValue("m_yaw");
    float mpit = Cvar_VariableValue("m_pitch");
    if (sens <= 0.0f) sens = 5.0f;
    if (myaw <= 0.0f) myaw = 0.022f;
    if (mpit <= 0.0f) mpit = 0.022f;

    /* Q3: viewangles[YAW] -= m_yaw * mx (mouse right turns right). Pitch is
     * negated here because this client's pitch is positive-up. */
    cl_yaw   -= (float)dx * myaw * (sens / 5.0f);
    cl_pitch -= (float)dy * mpit * (sens / 5.0f);
    while (cl_yaw > 180.0f)  cl_yaw -= 360.0f;
    while (cl_yaw < -180.0f) cl_yaw += 360.0f;
    if (cl_pitch > 85.0f)  cl_pitch = 85.0f;
    if (cl_pitch < -85.0f) cl_pitch = -85.0f;

    write_serial_string("[Q3CL] mouse dx=");
    ser_int(dx);
    write_serial_string(" dy=");
    ser_int(dy);
    write_serial_string(" yaw=");
    ser_int((int)cl_yaw);
    write_serial_string(" pitch=");
    ser_int((int)cl_pitch);
    write_serial_string("\n");
}

void CL_JoystickEvent(int axis, int value, int time) {
    (void)axis; (void)value; (void)time;
}

/* --- engine plumbing stubs the client does not need yet --------------- */

void CL_PacketEvent(netadr_t from, msg_t *msg) { (void)from; (void)msg; }
void CL_MapLoading(void) { write_serial_string("[Q3CL] map loading\n"); }
qboolean CL_GameCommand(void) { return qfalse; }
qboolean UI_GameCommand(void) { return qfalse; }
void CL_CDDialog(void) {}
void CL_FlushMemory(void) {}
/* id's qcommon declares these without the fork's extra parameters (v38.105). */
void CL_ShutdownAll(void) {}
void CL_InitRef(void) {
    /* The renderer is not a refexport_t module here: q3ref_* is a direct
     * TinyGL backend (see q3cl_render.c), started by CL_StartHunkUsers(). */
}
void CL_Snd_Shutdown(void) {}
qboolean CL_CDKeyValidate(const char *key, const char *checksum) {
    (void)key; (void)checksum;
    return qtrue;   /* STANDALONE: no CD key */
}
void CL_ForwardCommandToServer(const char *string) {
    write_serial_string("[Q3CL] cmd> ");
    write_serial_string(string ? string : "");
    write_serial_string("\n");
}
void CL_ConsolePrint(char *txt) {
    if (!txt || cl_log_console_budget == 0) return;
    cl_log_console_budget--;
    write_serial_string("[Q3CL] con> ");
    write_serial_string(txt);
    write_serial_string("\n");
}
void CL_Disconnect(qboolean showMainMenu) {
    (void)showMainMenu;
    write_serial_string("[Q3CL] disconnect\n");
}

/* --- phase 4: map-driven world ----------------------------------------- */

/* The engine writes and reads the mod dir under fs_homepath (/tmp/q3home —
 * tmpfs, v38.99); the volume is on the search path as the CD equivalent but
 * only read-only data belongs there, so whatever the source — the seeded
 * /ext2/mectov1.map or, when ext2 has none, the embedded fallback text —
 * it is staged ONCE at startup as baseq3/maps/mectov1.map. One loader path
 * downstream: the engine's own FS (the same entry cm_load.c uses for real
 * .bsp files). Whole-file tmpfs writes, zero ATA (v38.98 lesson). */
#define Q3MAP_QPATH "maps/mectov1.map"

static void q3_stage_map(void) {
    extern int  vfs_mkdir(const char *path);
    extern int  vfs_create_file(const char *path);
    extern int  vfs_write_file(const char *path, const char *data, int size);
    extern int  vfs_read_file(const char *path, char *buf, int max_size);
    extern int  vfs_get_node(const char *path);
    extern unsigned int vfs_get_file_size(int node);

    static char buf[8192];
    const char *dst = "/tmp/q3home/baseq3/" Q3MAP_QPATH;
    int n = 0;

    vfs_mkdir("/tmp/q3home/baseq3/maps");

    if (vfs_get_node("/ext2/mectov1.map") >= 0) {
        int sz = (int)vfs_get_file_size(vfs_get_node("/ext2/mectov1.map"));
        if (sz > 0 && sz < (int)sizeof(buf) - 1) {
            n = vfs_read_file("/ext2/mectov1.map", buf, sizeof(buf) - 1);
        }
    }
    if (n > 0) {
        write_serial_string("[Q3CL] map staged from /ext2/mectov1.map\n");
    } else {
        const char *s = Q3MAP_EMBEDDED_MECTOV1;
        while (s[n]) { buf[n] = s[n]; n++; }
        write_serial_string("[Q3CL] map staged from embedded fallback\n");
    }
    buf[n] = '\0';
    if (vfs_create_file(dst) >= 0)
        vfs_write_file(dst, buf, n);
}

/* Spawn the player from the map's own start state (phase 4 — the pose was
 * hardcoded before). The marker carries the exact start pose for the test. */
static void q3map_spawn_player(const q3map_t *m) {
    cl_x = m->spawn[0];
    cl_y = m->spawn[1];
    cl_z = m->spawn[2];
    cl_yaw = m->spawn[3];
    cl_pitch = m->spawn[4];
    while (cl_yaw > 180.0f)  cl_yaw -= 360.0f;
    while (cl_yaw < -180.0f) cl_yaw += 360.0f;
    if (cl_pitch > 85.0f)  cl_pitch = 85.0f;
    if (cl_pitch < -85.0f) cl_pitch = -85.0f;
    write_serial_string("[Q3CL] spawn x=");
    ser_int((int)cl_x); write_serial_string(","); ser_int((int)cl_y);
    write_serial_string(" z="); ser_int((int)cl_z);
    write_serial_string(" yaw="); ser_int((int)cl_yaw);
    write_serial_string(" pitch="); ser_int((int)cl_pitch);
    write_serial_string("\n");
}

/* --- phase-4 map collision ---------------------------------------------- */

/* Eye stand-off, same hygiene as the phase-3 arena clamp's 64 units: a wall
 * face reaching the eye's own depth is the one case TinyGL's clipper sees.
 * 32 keeps the eye clear of every wall inner face while still letting the
 * player walk right up to geometry. */
#define CL_STANDOFF 32.0f

/* Is map position (x,y) solid for the eye at cl_z? An AABB test over the
 * loaded brushes whose z-range overlaps the eye band (±16). */
static int cl_point_blocked(const q3map_t *m, float x, float y) {
    for (int i = 0; i < m->n_brushes; i++) {
        const float *mn = m->brushes[i].mins;
        const float *mx = m->brushes[i].maxs;
        if (mn[2] > cl_z + 16.0f || mx[2] < cl_z - 16.0f) continue;
        if (x > mn[0] - CL_STANDOFF && x < mx[0] + CL_STANDOFF &&
            y > mn[1] - CL_STANDOFF && y < mx[1] + CL_STANDOFF)
            return 1;
    }
    return 0;
}

/* Axis-separated slide move (Q3-style: an X pass then a Y pass, the other
 * axis keeping its position), against the loaded map. Returns 1 when the
 * move was blocked. */
static int cl_slide_move(float *org, const float *dir, float dist) {
    const q3map_t *m = q3map_current();
    float lim = m->bounds_half;   /* backstop; the perimeter brushes bind first */
    int blocked = 0;

    for (int a = 0; a < 2; a++) {
        float d = dir[a] * dist;
        float *o = (a == 0) ? &org[0] : &org[1];
        if (d == 0.0f) continue;
        if (*o + d >  lim) { *o =  lim; blocked = 1; continue; }
        if (*o + d < -lim) { *o = -lim; blocked = 1; continue; }
        if (cl_point_blocked(m, (a == 0) ? *o + d : org[0],
                                (a == 0) ? org[1] : *o + d)) {
            blocked = 1;              /* this axis is solid: no move on it */
            continue;
        }
        *o += d;
    }
    return blocked;
}

/* --- life cycle ------------------------------------------------------- */

void CL_Init(void) {
    cl_shownet = Cvar_Get("cl_shownet", "0", CVAR_TEMP);

    /* Client cvars: the real names, so config files and muscle memory work. */
    Cvar_Get("sensitivity",  "5",     CVAR_ARCHIVE);
    Cvar_Get("m_yaw",        "0.022", CVAR_ARCHIVE);
    Cvar_Get("m_pitch",      "0.022", CVAR_ARCHIVE);
    Cvar_Get("cl_yawspeed",  "140",   0);
    Cvar_Get("cl_pitchspeed","140",   0);
    Cvar_Get("cl_run",       "1",     CVAR_ARCHIVE);
    Cvar_Get("cl_freelook",  "1",     CVAR_ARCHIVE);

    com_cl_running = Cvar_Get("cl_running", "0", CVAR_ROM);

    /* The `q3` command boots the engine as a headless core (dedicated 1) and
     * must keep behaving exactly like phase 1: no window, no capture. The
     * client only comes up when the engine was started as a client. */
    if (com_dedicated && com_dedicated->integer) {
        write_serial_string("[Q3CL] client init (engine is dedicated: client inert)\n");
        return;
    }

    if (com_cl_running && !com_cl_running->integer) Cvar_Set("cl_running", "1");
    cl_running = 1;
    cl_inited = 1;

    /* Default binds, the Q3 set (WASD + arrows + mouse) — applied only when
     * the config did not already bind them. */
    if (!bindings['w'][0]) {
        CL_SetBinding('w', "+forward");
        CL_SetBinding('s', "+back");
        CL_SetBinding('a', "+moveleft");
        CL_SetBinding('d', "+moveright");
        CL_SetBinding(K_SPACE, "+moveup");
        CL_SetBinding(K_CTRL, "+movedown");
        CL_SetBinding(K_SHIFT, "+speed");
        CL_SetBinding(K_UPARROW, "+forward");
        CL_SetBinding(K_DOWNARROW, "+back");
        CL_SetBinding(K_LEFTARROW, "+left");
        CL_SetBinding(K_RIGHTARROW, "+right");
        CL_SetBinding(K_MOUSE1, "+attack");
    }

    write_serial_string("[Q3CL] client init: binds ready, cvar cl_running=1\n");
}

/* Renderer + window come up here, called from Com_Init once the filesystem
 * and the command buffer exist (same place ioquake3 starts its ref/UI). */
void CL_StartHunkUsers(void) {
    if (!cl_inited || cl_win_id >= 0) return;

    if (q3ref_init(Q3CL_W, Q3CL_H) != 0) {
        write_serial_string("[Q3CL] FATAL: TinyGL renderer init failed\n");
        cl_running = 0;
        return;
    }
    write_serial_string("[Q3CL] TinyGL renderer ready w=");
    ser_int(q3ref_width());
    write_serial_string(" h=");
    ser_int(q3ref_height());
    write_serial_string("\n");

    int ww = Q3CL_W + 2;
    int wh = Q3CL_H + TITLEBAR_H + 2;
    int wx = ((int)fb_width - ww) / 2;  if (wx < 0) wx = 0;
    int wy = ((int)fb_height - TASKBAR_H_PX - wh) / 2; if (wy < 0) wy = 0;

    // The title says what the window actually is (v38.107): id's engine drawing
    // the Mectov arena, not the retail game. It used to read "Quake III (Mectov)"
    // with nothing marking the arena as ours.
    cl_win_id = wm_open(wx, wy, ww, wh, "Quake III engine (Mectov arena)",
                        cl_win_draw, cl_win_key, NULL, cl_win_mouse);
    if (cl_win_id < 0) {
        write_serial_string("[Q3CL] FATAL: could not open WM window\n");
        q3ref_shutdown();
        cl_running = 0;
        return;
    }
    write_serial_string("[Q3CL] window id=");
    write_serial_hex((uint32_t)cl_win_id);
    write_serial_string("\n");

    /* Ask the WM for the game input paths (v38.103): raw scancodes with
     * releases, and relative-motion mouse capture. */
    wm_request_scancodes(cl_win_id, 1);
    if (wm_capture_mouse(cl_win_id, 1)) {
        int cx = 0, cy = 0;
        wm_capture_center(&cx, &cy);
        write_serial_string("[Q3CL] mouse captured, cursor pinned at ");
        ser_int(cx); write_serial_string(","); ser_int(cy);
        write_serial_string("\n");
    } else {
        write_serial_string("[Q3CL] WARNING: mouse capture refused\n");
    }

    /* Phase 4: the world comes from a map file. Load it through the engine
     * FS, hand it to the renderer, and spawn the player from the map's own
     * start state. A failed load keeps the phase-3 hardcoded arena. */
    if (q3map_load(Q3MAP_QPATH) == 0) {
        q3ref_set_map(q3map_current());
        q3map_spawn_player(q3map_current());
        write_serial_string("[Q3CL] map world active\n");
    } else {
        cl_x = 0.0f; cl_y = 180.0f; cl_z = Q3REF_EYE_H;
        cl_yaw = 180.0f; cl_pitch = 0.0f;
        write_serial_string("[Q3CL] map world FAILED - phase-3 arena fallback\n");
    }
    cl_time_base = get_ticks() / 1000.0;

    needs_redraw = 1;
    write_serial_string("[Q3CL] world ready\n");
}

void CL_Shutdown(void) {
    if (cl_win_id >= 0) {
        wm_capture_mouse(cl_win_id, 0);
        wm_request_scancodes(cl_win_id, 0);
        wm_close(cl_win_id);
        cl_win_id = -1;
    }
    q3ref_shutdown();
    cl_running = 0;
    write_serial_string("[Q3CL] shutdown\n");
}

/* --- the client frame ------------------------------------------------- */
void CL_Frame(int msec) {
    if (!cl_running || cl_win_id < 0) return;
    CLTR("clframe-in");

    if (wm_is_open(cl_win_id) == 0) {
        write_serial_string("[Q3CL] window closed\n");
        cl_running = 0;
        return;
    }

    if (msec < 0) msec = 0;
    if (msec > 200) msec = 200;        /* a stall must not teleport the player */
    float dt = (float)msec / 1000.0f;

    /* keyboard turning / looking */
    float yawspeed = Cvar_VariableValue("cl_yawspeed");
    float pitkspeed = Cvar_VariableValue("cl_pitchspeed");
    if (yawspeed <= 0.0f) yawspeed = 140.0f;
    if (pitkspeed <= 0.0f) pitkspeed = 140.0f;
    if (cl_move & MF_TURNLEFT)  cl_yaw += yawspeed * dt;
    if (cl_move & MF_TURNRIGHT) cl_yaw -= yawspeed * dt;
    if (cl_move & MF_LOOKUP)    cl_pitch += pitkspeed * dt;
    if (cl_move & MF_LOOKDOWN)  cl_pitch -= pitkspeed * dt;
    while (cl_yaw > 180.0f)  cl_yaw -= 360.0f;
    while (cl_yaw < -180.0f) cl_yaw += 360.0f;
    if (cl_pitch > 85.0f)  cl_pitch = 85.0f;
    if (cl_pitch < -85.0f) cl_pitch = -85.0f;

    /* horizontal basis: yaw 0 faces +Y, right is +X at yaw 0 */
    float yr = cl_yaw * (float)M_PI / 180.0f;
    float fx = -sinf(yr), fy = cosf(yr);
    float rx =  cosf(yr), ry = sinf(yr);

    float vx = 0.0f, vy = 0.0f;
    if (cl_move & MF_FORWARD)   { vx += fx; vy += fy; }
    if (cl_move & MF_BACK)      { vx -= fx; vy -= fy; }
    if (cl_move & MF_MOVERIGHT) { vx += rx; vy += ry; }
    if (cl_move & MF_MOVELEFT)  { vx -= rx; vy -= ry; }
    if (vx != 0.0f || vy != 0.0f) {
        float len = sqrtf(vx * vx + vy * vy);
        vx /= len; vy /= len;
    }
    int blocked = 0;
    float speed = (cl_move & MF_SPEED) ? 320.0f : 200.0f;
    if (cl_move & MF_MOVEUP)   cl_z += 60.0f * dt;
    if (cl_move & MF_MOVEDOWN) cl_z -= 60.0f * dt;
    if (cl_z < 12.0f) cl_z = 12.0f;
    if (cl_z > 90.0f) cl_z = 90.0f;

    if (q3map_current()) {
        /* Phase 4: collide against the loaded map — axis-separated movement
         * with a 32-unit eye stand-off, then push out of any map bot the
         * eye would end up inside (same near-plane hygiene as below). */
        float org[2] = { cl_x, cl_y };
        float dir[2] = { vx, vy };
        const q3map_t *mp = q3map_current();
        blocked = cl_slide_move(org, dir, speed * dt);
        cl_x = org[0];
        cl_y = org[1];
        for (int i = 0; i < mp->n_bots; i++) {
            float hx = mp->bots[i].w * 0.5f + 12.0f;
            float hy = mp->bots[i].d * 0.5f + 12.0f;
            float bz0 = mp->bots[i].z, bz1 = bz0 + mp->bots[i].h;
            if (bz0 > cl_z + 16.0f || bz1 < cl_z - 16.0f) continue;
            float dx = cl_x - mp->bots[i].x, dy = cl_y - mp->bots[i].y;
            if (dx > -hx && dx < hx && dy > -hy && dy < hy) {
                float px = hx - (dx < 0 ? -dx : dx);
                float py = hy - (dy < 0 ? -dy : dy);
                if (px < py) cl_x = mp->bots[i].x + (dx < 0 ? -hx : hx);
                else         cl_y = mp->bots[i].y + (dy < 0 ? -hy : hy);
            }
        }
    } else {
        /* No map: exactly the v38.103 behaviour — flat arena clamp plus the
         * hardcoded-bot pushout. 64 units of standoff, not 48: a polygon
         * that straddles the near plane is the one case TinyGL's clipper
         * sees, and this client has no collision system to keep the eye out
         * of anything. */
        float lim = Q3REF_ARENA_HALF - 64.0f;
        if (cl_x >  lim) cl_x =  lim;
        if (cl_x < -lim) cl_x = -lim;
        if (cl_y >  lim) cl_y =  lim;
        if (cl_y < -lim) cl_y = -lim;
        {
            static const float bx[3] = {    0.0f, -140.0f,  150.0f };
            static const float by[3] = { -200.0f, -320.0f, -330.0f };
            const float keep = 36.0f;   /* 24 (half box) + 12 clearance */
            for (int i = 0; i < 3; i++) {
                float dx = cl_x - bx[i], dy = cl_y - by[i];
                if (dx > -keep && dx < keep && dy > -keep && dy < keep) {
                    float px = keep - (dx < 0 ? -dx : dx);
                    float py = keep - (dy < 0 ? -dy : dy);
                    if (px < py) cl_x = bx[i] + (dx < 0 ? -keep : keep);
                    else         cl_y = by[i] + (dy < 0 ? -keep : keep);
                }
            }
        }
    }

    /* One-shot serial marker: the test's proof that map collision actually
     * stops the player. Fires on the 5th consecutive blocked frame while a
     * move input is held; re-arms the moment movement is free again. */
    if (blocked && (cl_move & (MF_FORWARD | MF_BACK | MF_MOVELEFT |
                               MF_MOVERIGHT))) {
        if (cl_blocked_frames < 1000) cl_blocked_frames++;
        if (cl_blocked_frames == 5)
            write_serial_string("[Q3CL] move blocked brush\n");
    } else if (!blocked) {
        cl_blocked_frames = 0;
    }

    /* render + present through the compositor */
    double t = get_ticks() / 1000.0 - cl_time_base;
    q3ref_set_camera(cl_x, cl_y, cl_z, cl_yaw, cl_pitch);
    q3ref_begin_frame();
    q3ref_draw_world(t);
    q3ref_end_frame();
    CLTR("draw-done");
    wm_invalidate(cl_win_id);
    needs_redraw = 1;

    cl_frames++;
    if ((cl_frames % 30) == 0) {
        write_serial_string("[Q3CL] frame=");
        ser_int((int)cl_frames);
        write_serial_string(" t=");
        ser_int((int)t);
        write_serial_string(" pos=");
        ser_int((int)cl_x); write_serial_string(","); ser_int((int)cl_y);
        write_serial_string(" z="); ser_int((int)cl_z);
        write_serial_string(" yaw="); ser_int((int)cl_yaw);
        write_serial_string(" pitch="); ser_int((int)cl_pitch);
        write_serial_string(" move=");
        write_serial_hex(cl_move);
        write_serial_string("\n");
    }
}

/* --- entry point: the `q3play` shell command's kernel task ------------- */
static void q3play_park(void) {
    /* A forked kernel task entry must never return (v38.99: returning pops a
     * garbage address and page-faults). Park with interrupts on so the
     * desktop keeps ticking. */
    for (;;) __asm__ __volatile__("hlt");
}

void q3play_start(void) {
    write_serial_string("[Q3CL] Starting Quake III client (id Software source + TinyGL)...\n");

    static char cmdline[160];
    static const char args[] =
        "+set dedicated 0 "       /* client: enables CL_Init/CL_Frame below */
        "+set com_hunkMegs 20 "   /* 20MB is also the client-mode minimum */
        "+set com_zoneMegs 6 "
        "+set com_maxfps 20 "     /* the engine's limiter paces the client */
        "+set com_busyWait 0 "    /* real NET_Sleep wait instead of spinning */
        "+set fs_game baseq3";
    for (int i = 0; i < (int)sizeof(args); i++) cmdline[i] = args[i];

    /* Home path in tmpfs: engine config I/O must never touch the ATA driver
     * (v38.99 — per-frame whole-file writes stalled the disk). */
    {
        extern int vfs_mkdir(const char *path);
        extern int vfs_create_file(const char *path);
        extern int vfs_write_file(const char *path, const char *data, int size);
        vfs_mkdir("/tmp/q3home");
        vfs_mkdir("/tmp/q3home/baseq3");
        if (vfs_create_file("/tmp/q3home/baseq3/default.cfg") >= 0)
            vfs_write_file("/tmp/q3home/baseq3/default.cfg",
                           "// mectov client cfg\n", 22);
    }

    /* Stage the map data into the engine's tmpfs homepath BEFORE Com_Init —
     * the engine FS cannot see the file otherwise. */
    q3_stage_map();

    Com_Init(cmdline);
    write_serial_string("[Q3CL] Com_Init done, entering client frame loop\n");

    if (!cl_running) {
        write_serial_string("[Q3CL] FATAL: client did not start\n");
        q3play_park();
    }

    while (cl_running) {
        CLTR("pump-in");
        clq_pump();      /* WM input -> the engine's event queue */
        CLTR("pump-out");
        CLTR("com-in");
        Com_Frame();     /* engine dispatch (CL_KeyEvent/CL_MouseEvent) + CL_Frame */
        CLTR("com-out");
    }

    if (cl_win_id >= 0) {
        wm_capture_mouse(cl_win_id, 0);
        wm_request_scancodes(cl_win_id, 0);
        wm_close(cl_win_id);
        cl_win_id = -1;
    }
    q3ref_shutdown();

    write_serial_string("[Q3CL] loop done, frames=");
    ser_int((int)cl_frames);
    write_serial_string("\n");
    write_serial_string("[Q3CL] done\n");
    q3play_park();
}
