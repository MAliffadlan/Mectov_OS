/* q3_uivm.h — host for id's official Quake III user interface, running as
 * Quake VM bytecode (v38.150).
 *
 * WHAT THIS RUNS. scripts/build_qvm_ui.sh compiles code/q3_ui (id's classic
 * Quake III Arena 1.32 interface) into build/vm/q3ui.qvm with id's own lcc +
 * q3asm, and the kernel executes it with id's own interpreter
 * (code/qcommon/vm.c + vm_interpreted.c) — the same machinery that runs
 * qagame.qvm. The menu that appears is therefore id's: ui_menu.c builds it
 * with Menu_AddItem, ui_qmenu.c paints it with the bitmap charset and the
 * gradient bars, ui_splevel.c lists the levels out of the map's own entity
 * string. Nothing here draws a menu; this file only answers the ~90 traps the
 * bytecode makes and forwards input.
 *
 * WHY NOT THE ui.qvm IN THE PAK (measured, not assumed). pak0's vm/ui.qvm is
 * Team Arena's UI rewrite: a menu engine whose menus are DATA (ui/*.menu,
 * listed in ui/menus.txt). The demo pak has no ui/ directory, no .menu file
 * and no .txt beyond scripts/arenas.txt + scripts/bots.txt, and the GPL source
 * drop ships ui/menus.txt but not the ~37 .menu files it names. That bytecode
 * would boot into an empty menu. code/q3_ui is the 1.32 interface, whose menus
 * are code, and its art (menu/art/*.tga, gfx/2d/bigchars.tga) is all in the pak.
 *
 * WHAT THE TRAPS COST. Roughly half are free: cvar, command, filesystem, argc/
 * argv and the maths helpers are the same engine services the game module
 * already uses (Cvar_*, Cmd_*, Cbuf_*, FS_*, and the same fixed-point maths
 * q3_vm.c implements, because the kernel has no libm). The 2D draws land in
 * q3ui2d (see third_party/tinygl/q3ui2d.h). What is deliberately INERT, each
 * with the reason, and each reported through q3uivm_stats() rather than
 * silently pretended:
 *   - sound (UI_S_*, 4 traps): the Q3 mixer bridge was removed in v38.142; the
 *     menu is silent. A null sound handle makes id's StartLocalSound a no-op.
 *   - the 3D preview (UI_R_REGISTERMODEL/SKIN, CLEARSCENE, ADDREFENTITY,
 *     ADDPOLY, ADDLIGHT, RENDERSCENE, CM_*): no model renderer exists, so the
 *     player-model window in "change your player model" is empty.
 *   - LAN (13 traps) and cinematics (5): no network stack, no roq player.
 *   - CD key (2) and the Team Arena PC_* parser (5): this is the 1.32 module,
 *     which makes none of those calls, and we ship no id game data.
 *   - key BINDING get/set: the SETUP screen shows and edits bindings; without a
 *     binding config to write, a get returns empty and a set is dropped. The
 *     UI reads its bindings back from the cvar it wrote first, so the screen
 *     behaves; only the change is not kept across sessions.
 */
#ifndef Q3UIVM_H
#define Q3UIVM_H

/* 0 on success. Loads vm/q3ui.qvm through id's FS (so the file must be staged
 * on the game volume — scripts/seed_ext2.sh does it), runs UI_GETAPIVERSION
 * and UI_INIT. -1 if the module is missing or the 2D surface cannot be
 * allocated; the caller reports that and carries on without a menu. */
int  q3uivm_init(void);
void q3uivm_shutdown(void);
int  q3uivm_ready(void);

/* One UI frame: UI_REFRESH with the given millisecond clock. */
void q3uivm_frame(int realtime_ms);

/* Input, in the UI's own vocabulary: Q3 key codes from
 * third_party/q3a/code/ui/keycodes.h (K_ESCAPE, K_MOUSE1, …). `down` is
 * qboolean. Relative mouse motion, exactly as id delivers it. */
void q3uivm_key(int key, int down);
void q3uivm_mouse(int dx, int dy);

/* UI_IS_FULLSCREEN — false: this port's menu is windowed, always. */
int  q3uivm_is_fullscreen(void);

/* The menu the UI should be showing (uiMenuCommand_t). This is a HOST-side
 * decision — id's client drives it from its own state (a key press opens
 * UIMENU_MAIN, ESC in a game opens UIMENU_INGAME, closing it goes back to
 * UIMENU_NONE) and tells the module through UI_SET_ACTIVE_MENU, which is the
 * only way the 1.32 menu system comes up (ui_atoms.c UI_SetActiveMenu). */
int  q3uivm_active_menu(void);
void q3uivm_set_active_menu(int menu);

/* Client state for UI_GETCLIENTSTATE. The driver's own truth: in-game vs at
 * the menu, and (once a level runs) the client number and the level name. */
typedef struct {
    int conn_state;          /* CA_* — the values are in q_shared.h */
    int client_num;
    const char *servername;
    const char *update_info;
    const char *message;
} q3uivm_clientstate_t;
void q3uivm_set_clientstate(const q3uivm_clientstate_t *cs);

/* Held keys and mouse buttons, for UI_KEY_ISDOWN. The UI polls this every
 * frame, so it must be a snapshot the caller updates as events arrive. */
void q3uivm_set_key_state(int key, int down);
void q3uivm_clear_key_states(void);

/* One command the UI asked for, oldest first, as id's UI wrote it: "spmap
 * q3dm1", "disconnect", "quit", "addbot …". These are the verbs the menu uses
 * to reach the host (in retail they went through the client to a server; this
 * port has no server), so the host owns what happens next and they are not
 * forwarded to id's command buffer. Returns 1 and fills `out` when one was
 * pending, 0 when the queue is empty. Anything the UI executes that is NOT in
 * that set — the cvars the setup screen writes, exec, set, cvar_restart —
 * still goes to id's own command buffer untouched. */
int q3uivm_take_command(char *out, int out_size);

/* Every trap, counted by name, so an unexpected one is a number in a test
 * rather than a guess. */
void q3uivm_stats(int *traps, int *errors, int *unhandled, int *frames,
                  int *keyevents, int *mouseevents, int *cmds);

#endif /* Q3UIVM_H */