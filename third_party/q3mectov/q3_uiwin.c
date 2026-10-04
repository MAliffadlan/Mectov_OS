/* q3_uiwin.c — the `q3menu` app's new face: id's own Quake III main menu
 * running as bytecode in a window (v38.150).
 *
 * This file is the driver, and it is deliberately thin. The menu is drawn by
 * q3ui.qvm (id's classic 1.32 UI module, built by scripts/build_qvm_ui.sh and
 * executed by id's own interpreter), its pictures come out of id's own
 * filesystem, and its pixels land in the software 2D surface in
 * third_party/tinygl/q3ui2d.c. What is left for the host is the four things
 * bytecode cannot do for itself:
 *
 *   1. ENGINE BOOT. The UI needs Cvar_*, Cmd_*, FS_* and above all the
 *      filesystem that carries menu/art/*.tga and gfx/2d/bigchars.tga, so
 *      id's engine core is initialised exactly as the game driver does it
 *      (Com_Init with the same +set line; see q3_drive in q3_vm.c).
 *   2. A WINDOW AND A FRAME CLOCK. The UI draws at 40 Hz like the game: a
 *      frame is UI_REFRESH, then the surface is area-averaged into the
 *      window's content buffer and the desktop is composited.
 *   3. INPUT. PS/2 scancodes become id key numbers (q3_client.c's own table —
 *      the UI speaks K_* codes, not scancodes), and the mouse arrives as
 *      relative motion because that is what id's UI_MouseEvent consumes.
 *   4. THE COMMANDS. "spmap q3dm1" from the arena picker is the user's answer
 *      to "play": the menu window closes and the game is forked on that map.
 *
 * Concurrency, stated because it is the one sharp edge: id's engine core is
 * process-global state (cvar table, FS search paths, the hunk). A menu and a
 * running game would trample each other, so the menu refuses to open while a
 * game is up (q3arena_status) and, when it launches a game, closes and parks
 * FIRST. The game task then owns the engine. That is the same order the
 * Mectov-styled menu used, and it is why the menu is its own kernel task
 * rather than a mode inside the game driver.
 */
#include <stdint.h>
#include <stddef.h>

#include "../q3a/code/game/q_shared.h"
#include "../q3a/code/qcommon/qcommon.h"
#include "../q3a/code/ui/ui_public.h"
/* id's key numbers, from the same header the UI module was compiled against.
 * v38.150: these MUST be id's, not a local copy — the bytecode's idea of
 * K_MOUSE1 is whatever this header says (measured: 178, where q3_client.c's
 * private defines say 200), and a menu that polls the wrong number sees no
 * clicks at all. */
#include "../q3a/code/ui/keycodes.h"

#include "q3_uivm.h"
#include "../tinygl/q3ui2d.h"

extern void write_serial_string(const char *s);
extern void write_serial_hex(uint32_t v);
extern uint32_t get_ticks(void);
extern void *kmalloc(uint32_t size);

/* WM + theme, same includes the other window drivers use. */
#include "../../src/include/theme.h"
#include "../../src/include/wm.h"
extern int get_win_index(int wid);
extern volatile int needs_redraw;

/* From q3_client.c (v38.150): the PS/2 set-1 scancode -> id key number table
 * the client already keeps for movement. The UI needs the same mapping, and
 * duplicating 0x80 entries here would be a second thing to get wrong. */
extern int q3client_sc_to_key(int sc);
/* From cmd_q3arena.c: forks the game driver task on a named map. */
extern int  q3arena_launch_map(const char *map);
extern int  q3arena_status(char *buf, int n);

/* Window geometry: the UI is virtualized 640x480 (q3ui2d.h) and presented into
 * the same 320x240 content area the game window uses, area-averaged 2:1. That
 * is the same picture the user already sees from quake.sh at Q3ARENA_SCALE=2,
 * at half the window size. */
#define UIWIN_W 320
#define UIWIN_H 240

static int ui_win = -1;
static volatile int ui_running;
static int ui_frames;
static int ui_last_menu = -1;

static void ui_park(void) {
    for (;;) __asm__ __volatile__("hlt");
}

static void ui_report(int v) {
    write_serial_string("[Q3UI] ");
    write_serial_hex((uint32_t)v);
    write_serial_string("\n");
}

/* "quit" matches "quit", never "quitnow": the verb has to end at a separator,
 * because the UI's own arguments matter (spmap's map name especially). */
static int ui_verb(const char *line, const char *verb) {
    int i = 0;
    while (verb[i] && line[i] == verb[i]) i++;
    if (!verb[i]) return 1;
    return line[i] == '\0' || line[i] == ' ' || line[i] == '\t';
}

/* --- window callbacks (compositor context) ------------------------- */
static void ui_win_draw(int id, int cx, int cy, int cw, int ch) {
    int idx = get_win_index(id);
    (void)cx; (void)cy;
    if (idx < 0) return;
    if (wm_wins[idx].resizing) return;
    if (!wm_wins[idx].content_buffer) return;
    {
        int dw = (cw < UIWIN_W) ? cw : UIWIN_W;
        int dh = (ch < UIWIN_H) ? ch : UIWIN_H;
        int ox = (cw - dw) / 2, oy = (ch - dh) / 2;
        if (ox < 0) ox = 0;
        if (oy < 0) oy = 0;
        /* The letterbox strips outside the 4:3 picture keep whatever the WM
         * cleared them to, so the picture is not distorted to fill. */
        q3ui2d_present(wm_wins[idx].content_buffer + (size_t)oy * cw + ox,
                       dw, dh, cw);
    }
}

static void ui_win_key(int id, char c, uint8_t sc) {
    int base = sc & 0x7F;
    int down = !(sc & 0x80);
    int key;
    (void)id;
    if (base >= 0x80) return;
    /* The window was opened with wm_request_scancodes, so this handler gets
     * presses AND releases — the UI's UI_KeyEvent takes a `down` flag and the
     * menu's own repeat logic depends on seeing both. */
    key = q3client_sc_to_key(base);
    if (key < 0) {
        /* No physical mapping: a printable ASCII key still arrives as the
         * character the desktop resolved, which is exactly what id's client
         * sends (keycodes.h: "normal keys should be passed as lowercased
         * ascii"). */
        if (c >= 32 && c < 127) key = (int)(unsigned char)c;
        else return;
    }
    if (key == K_ESCAPE && down) {
        /* ESC leaves the menu the way it leaves a game: the window closes and
         * the task parks, putting the user back on the desktop. */
        ui_running = 0;
        return;
    }
    q3uivm_key(key, down);
    wm_invalidate(ui_win);
}

static void ui_win_mouse(int id, int dx, int dy, int btn) {
    (void)id;
    /* The UI's own cursor is drawn by id's UI_Refresh from the accumulated
     * motion (uis.cursorx/cursory in ui_atoms.c), which is why the window
     * takes RELATIVE mouse packets: same as retail, and the arrow never gets
     * stuck at a screen edge. The BUTTONS are different — the menu polls
     * trap_Key_IsDown(K_MOUSE1..), so they are pushed into the key snapshot
     * rather than sent as key events. */
    int down = (btn != 0);
    q3uivm_set_key_state(K_MOUSE1, down);
    q3uivm_set_key_state(K_MOUSE2, btn == 2 || btn == 3);
    q3uivm_set_key_state(K_MOUSE3, btn == 3);
    q3uivm_mouse(dx, dy);
    wm_invalidate(ui_win);
}

static void ui_win_tick(int id) {
    if (!wm_is_open(id)) ui_running = 0;
}

/* --- the task ------------------------------------------------------ */
void q3ui_start(void) {
    static char cmdline[176];
    static const char args[] =
        "+set dedicated 1 "
        "+set com_hunkMegs 20 "
        "+set com_zoneMegs 6 "
        "+set com_maxfps 0 "
        "+set com_busyWait 1 "
        "+set developer 1 "
        "+set fs_game baseq3 "
        "+set fs_cdpath /ext2";
    char st[96];
    char cmd[80];
    char mapname[64];
    uint32_t next_ms = 0;
    int i;

    mapname[0] = '\0';
    write_serial_string("[Q3UI] task started (official Quake III menu)\n");

    /* One engine at a time. A running game owns the cvar table, the FS search
     * paths and the hunk; a second Com_Init would land on top of them. */
    if (q3arena_status(st, (int)sizeof(st)) >= 2) {
        write_serial_string("[Q3UI] a game is already running — ");
        write_serial_string(st);
        write_serial_string("\n");
        ui_park();
    }

    /* The engine's home directory, on tmpfs, with the default.cfg that
     * FS_InitFilesystem insists on (files.c: "Couldn't load default.cfg").
     * Same two lines the game driver writes. */
    {
        extern int vfs_mkdir(const char *path);
        extern int vfs_create_file(const char *path);
        extern int vfs_write_file(const char *path, const char *data, int size);
        vfs_mkdir("/tmp/q3home");
        vfs_mkdir("/tmp/q3home/baseq3");
        if (vfs_create_file("/tmp/q3home/baseq3/default.cfg") >= 0)
            vfs_write_file("/tmp/q3home/baseq3/default.cfg",
                           "// mectov q3ui cfg\n", 18);
    }

    for (i = 0; i < (int)sizeof(args); i++) cmdline[i] = args[i];
    Com_Init(cmdline);
    write_serial_string("[Q3UI] engine up\n");

    if (q3uivm_init() != 0) {
        write_serial_string("[Q3UI] menu unavailable (see the lines above)\n");
        ui_park();
    }
    q3uivm_set_active_menu(UIMENU_MAIN);
    ui_last_menu = UIMENU_MAIN;

    {
        int ww = UIWIN_W + 2;
        int wh = UIWIN_H + TITLEBAR_H + 2;
        extern uint32_t fb_width, fb_height;
        int wx = ((int)fb_width - ww) / 2;  if (wx < 0) wx = 0;
        int wy = ((int)fb_height - TASKBAR_H_PX - wh) / 2; if (wy < 0) wy = 0;
        ui_win = wm_open(wx, wy, ww, wh, "Quake III Arena",
                         ui_win_draw, ui_win_key, ui_win_tick, ui_win_mouse);
    }
    if (ui_win < 0) {
        write_serial_string("[Q3UI] FATAL: no window\n");
        q3uivm_shutdown();
        ui_park();
    }
    wm_request_scancodes(ui_win, 1);
    /* Relative mouse, like the game window: id's menu consumes motion, not
     * absolute position. */
    wm_capture_mouse(ui_win, 1);
    write_serial_string("[Q3UI] window id=");
    ui_report(ui_win);
    write_serial_string("[Q3UI] entering the menu\n");

    ui_running = 1;
    while (ui_running) {
        uint32_t now = get_ticks();

        /* Frame the menu: one UI_REFRESH at the 40 Hz the game uses. */
        q3uivm_frame((int)now);
        ui_frames++;

        wm_invalidate(ui_win);
        needs_redraw = 1;
        /* This task owns the screen while it runs, so it composites the
         * desktop itself (v38.134: a task that never blocks starves the
         * idle loop and the picture freezes — the loading screen did exactly
         * that for a whole phase). */
        desktop_pump();

        /* Drain the mouse accumulator. The compositor collects packets for
         * whichever window owns the capture and desktop_capture_pump() hands
         * them over atomically (wm.h); without this call the menu's cursor
         * would only move whenever the desktop's own idle loop happened to
         * run. The desktop loop keeps its own call — neither caller can lose
         * a packet. The return value is the current button state, which the
         * window callback has already turned into K_MOUSE* key state. */
        desktop_capture_pump();

        /* Anything the menu asked the host to do. */
        while (q3uivm_take_command(cmd, (int)sizeof(cmd))) {
            write_serial_string("[Q3UI] menu command: ");
            write_serial_string(cmd);
            write_serial_string("\n");
            if (!ui_verb(cmd, "spmap") && !ui_verb(cmd, "map")) {
                if (ui_verb(cmd, "disconnect") || ui_verb(cmd, "quit")) {
                    ui_running = 0;
                    break;
                }
                continue;      /* addbot/cinematic/demo/… : nothing to do yet */
            }
            {
                int n = 0;
                const char *p = cmd;
                while (*p && *p != ' ') p++;
                while (*p == ' ') p++;
                while (p[n] && p[n] != ' ' && p[n] != '\n' && n < 63) {
                    char ch = p[n];
                    if (!((ch >= 'a' && ch <= 'z') ||
                          (ch >= '0' && ch <= '9') || ch == '_')) {
                        write_serial_string("[Q3UI] rejected map name\n");
                        n = 0;
                        break;
                    }
                    mapname[n] = ch;
                    n++;
                }
                mapname[n] = '\0';
            }
            if (mapname[0]) {
                /* Close before forking: the game task runs its own Com_Init,
                 * and two engines must never be live at once. */
                wm_capture_mouse(ui_win, 0);
                wm_close(ui_win);
                ui_win = -1;
                q3uivm_shutdown();
                write_serial_string("[Q3UI] starting ");
                write_serial_string(mapname);
                write_serial_string("\n");
                if (q3arena_launch_map(mapname) == 0) ui_park();
                write_serial_string("[Q3UI] could not start the game\n");
                ui_park();
            }
        }

        /* 40 Hz cap, the same cadence the game driver paces at. */
        now = get_ticks();
        if ((int32_t)(now - next_ms) > 0) next_ms = now + 25;
        while ((int)(get_ticks() - next_ms) < 0)
            __asm__ __volatile__("hlt");
    }

    if (ui_win >= 0) {
        wm_capture_mouse(ui_win, 0);
        wm_close(ui_win);
        ui_win = -1;
    }
    q3uivm_shutdown();
    {
        int shaders, images, bytes, draws, pixels, missing, rotated;
        q3ui2d_stats(&shaders, &images, &bytes, &draws, &pixels, &missing, &rotated);
        write_serial_string("[Q3UI] frames=");
        ui_report(ui_frames);
        write_serial_string("[Q3UI] shaders=");
        ui_report(shaders);
        write_serial_string("[Q3UI] cached bytes=");
        ui_report(bytes);
        write_serial_string("[Q3UI] draws/frame=");
        ui_report(draws);
        write_serial_string("[Q3UI] dest pixels/frame=");
        ui_report(pixels);
        write_serial_string("[Q3UI] missing pictures=");
        ui_report(missing);
        write_serial_string("[Q3UI] rotations requested=");
        ui_report(rotated);
    }
    write_serial_string("[Q3UI] done\n");
    ui_park();
}