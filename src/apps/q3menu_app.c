// src/apps/q3menu_app.c — the `q3menu` shell command's window: Quake III's
// main menu, Mectov style. NOT id's ui.qvm (that needs ~50 UI traps, botlib
// for skirmish and a network stack for multiplayer — none of which exist);
// this is a native window that reuses parts already proven in this tree:
//   - wm_open + draw/key/mouse callbacks (the taskmgr pattern),
//   - levelshot JPGs via q3jpeg_decode (the texture path's own decoder),
//   - the q3arena fork path (same task_fork_kernel payload as the shell),
//   - runtime mouse sensitivity (q3arena_set_sens, v38.149).
// v38.149 scope, stated plainly: SINGLE PLAYER map picker (the 4 staged
// retail maps with their levelshots) + mouse sensitivity + MAIN/KELUAR.
// No multiplayer (no network stack), no mods (no VM mods), no bots note in
// the UI beyond honest absence. Demos/cinematics playback is a later step
// (the .dm3/.roq files are already staged).
//
// Layout is fixed-geometry (480x380 outer); a smaller window clips via the
// compositor, a bigger one leaves empty margin — both harmless. All hit
// rects live in the same content-local space the draw uses, so clicks and
// pixels can never disagree. Built always; without MECTOV_Q3 the window
// never opens (the shell command says why instead).

#include "../include/wm.h"
#include "../include/vga.h"
#include "../include/theme.h"
#include "../include/vfs.h"

#ifdef MECTOV_Q3

/* vga/font theme bits live in vga.h/theme.h; the JPEG decoder and the Q3
 * driver API come from their own TUs (declared at first use, the codebase's
 * usual extern-at-site style). */
extern void *kmalloc(uint32_t);
extern void kfree(void*);
extern int q3jpeg_dims(const unsigned char *data, int len, int *out_w, int *out_h);
extern int q3jpeg_decode(const unsigned char *data, int len,
                         unsigned char *out, int *out_w, int *out_h);
extern int q3arena_launch_map(const char *map);
extern void q3arena_set_sens_hund(int h);
extern int q3arena_sens_hund(void);
extern int q3arena_status(char *buf, int n);

#define QM_NMAPS 4
static const char *qm_bsp[QM_NMAPS]   = { "q3dm1", "q3dm7", "q3dm17", "q3tourney2" };
static const char *qm_name[QM_NMAPS]  = { "Arena Gate", "Temple of Retribution",
                                          "The Longest Yard", "The Proving Grounds" };

/* Geometry (content-local, 8px font cells). */
#define QM_W        480
#define QM_H        380
#define QM_LIST_X   12
#define QM_LIST_Y   40
#define QM_LIST_W   268
#define QM_ROW_H    30
#define QM_THUMB    148     /* thumbnail box, right side */
#define QM_TH_X     (QM_W - QM_THUMB - 16)
#define QM_SENS_Y   212
#define QM_BTN_Y    292
#define QM_BTN_W    150
#define QM_BTN_H    34

/* Palette: Mectov dark + one amber accent (the boot screen's Q3 amber). */
#define QM_BG       0x001E1E2E
#define QM_ROW      0x00262638
#define QM_ROWBD    0x00585870
#define QM_SELB     0x00402A10
#define QM_AMBER    0x00E0A94F
#define QM_Q3AMB    0x00D08A2A
#define QM_TXT      0x00CDD6F4
#define QM_DIM      0x00808080
#define QM_GREEN    0x0027C93F
#define QM_GREENBG  0x001E3A24
#define QM_RED      0x00FF5555

static int    qm_win = -1;
static int    qm_open = 0;
static int    qm_sel = 0;
static int    qm_hover = -1;
static int    qm_sens = 10;         /* hundredths of deg/px (v38.150/151: was 18,
                                     * the driver default is 0.10 now) */
static int    qm_prev_btn = 0;
static int    qm_msg_ticks = 0;         /* transient hint countdown */
static char   qm_msg_text[40];         /* what the hint says (v38.150) */
static unsigned char *qm_shot;         /* decoded RGB888 thumbnail */
static int    qm_shot_w, qm_shot_h, qm_shot_map = -1;
static int    qm_cw = QM_W, qm_ch = QM_H;   /* last drawn content size */
/* Hit zones, written by draw (same geometry it paints) and read by mouse. */
static int    qm_minus_x, qm_plus_x, qm_btn_y;

static void qm_free_shot(void) {
    if (qm_shot) { kfree(qm_shot); qm_shot = 0; }
    qm_shot_w = qm_shot_h = 0;
    qm_shot_map = -1;
}

/* Decode this map's levelshot (baseq3/levelshots/<bsp>.jpg) into qm_shot.
 * Runs on selection change, never in draw: one JPEG decode is ~50-200 ms and
 * must not land inside the compositor's measured pass. Failure is sticky
 * per map (buf stays NULL with map recorded) so a missing file draws the
 * placeholder instead of retrying every frame. */
static void qm_load_shot(int idx) {
    char path[64];
    unsigned char *stage;
    int i, n;
    static const char pre[] = "/baseq3/levelshots/";
    static const char suf[] = ".jpg";
    qm_free_shot();
    if (idx < 0 || idx >= QM_NMAPS) return;
    for (i = 0; pre[i] && i < 40; i++) path[i] = pre[i];
    n = i;
    for (i = 0; qm_bsp[idx][i] && n < 48; i++, n++) path[n] = qm_bsp[idx][i];
    for (i = 0; suf[i] && n < 60; i++, n++) path[n] = suf[i];
    path[n] = '\0';
    stage = (unsigned char *)kmalloc(192 * 1024);
    if (!stage) { qm_shot_map = idx; return; }
    {
        extern int vfs_read_file(const char *path, char *buf, int max_size);
        int len = vfs_read_file(path, (char *)stage, 192 * 1024);
        if (len <= 0) { kfree(stage); qm_shot_map = idx; return; }
        {
            int dw = 0, dh = 0;
            if (q3jpeg_dims(stage, len, &dw, &dh) != 0 || dw <= 0 || dh <= 0 ||
                dw > 1024 || dh > 1024) {
                kfree(stage); qm_shot_map = idx; return;
            }
            {
                unsigned char *rgb = (unsigned char *)kmalloc((uint32_t)(dw * dh * 3));
                int ow = dw, oh = dh;
                if (!rgb || q3jpeg_decode(stage, len, rgb, &ow, &oh) != 0) {
                    if (rgb) kfree(rgb);
                    kfree(stage); qm_shot_map = idx; return;
                }
                kfree(stage);
                qm_shot = rgb; qm_shot_w = ow; qm_shot_h = oh; qm_shot_map = idx;
            }
        }
    }
}

static void qm_draw_thumb(int bx, int by) {
    int x, y;
    /* Box + border. */
    for (y = 0; y < QM_THUMB; y++)
        for (x = 0; x < QM_THUMB; x++)
            put_pixel(bx + x, by + y, QM_ROW);
    draw_rounded_rect_border(bx, by, QM_THUMB, QM_THUMB, 3, QM_ROWBD);
    if (!qm_shot || qm_shot_w <= 0 || qm_shot_h <= 0) {
        draw_string_px(bx + 44, by + 66, "no pic", QM_DIM, QM_ROW);
        return;
    }
    {
        /* Fit inside 136x136, keep aspect, nearest neighbour. */
        int m = qm_shot_w > qm_shot_h ? qm_shot_w : qm_shot_h;
        int dw = (136 * qm_shot_w) / m, dh = (136 * qm_shot_h) / m;
        int ox = bx + (QM_THUMB - dw) / 2, oy = by + (QM_THUMB - dh) / 2;
        for (y = 0; y < dh; y++) {
            int sy = (y * qm_shot_h) / dh;
            for (x = 0; x < dw; x++) {
                int sx = (x * qm_shot_w) / dw;
                unsigned char *p = qm_shot + (unsigned)(sy * qm_shot_w + sx) * 3;
                put_pixel(ox + x, oy + y,
                          ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]);
            }
        }
    }
}

/* sens 0.05..1.00 as "0.30" without any libc formatting. */
static void qm_sens_text(char *out) {
    int v = qm_sens;
    if (v < 5) v = 5;
    if (v > 100) v = 100;
    out[0] = (char)('0' + v / 100);
    out[1] = '.';
    out[2] = (char)('0' + (v / 10) % 10);
    out[3] = (char)('0' + v % 10);
    out[4] = '\0';
}

static void qm_draw(int id, int cx, int cy, int cw, int ch) {
    int i;
    (void)id; (void)cx; (void)cy;
    qm_cw = cw; qm_ch = ch;
    draw_rect(0, 0, cw, ch, QM_BG);
    draw_string_px(12, 10, "SINGLE PLAYER  -  pilih arena", QM_Q3AMB, QM_BG);
    for (i = 0; i < QM_NMAPS; i++) {
        int ry = QM_LIST_Y + i * QM_ROW_H;
        int sel = (i == qm_sel), hov = (i == qm_hover);
        uint32_t bg = sel ? QM_SELB : QM_ROW;
        uint32_t bd = sel ? QM_AMBER : (hov ? QM_TXT : QM_ROWBD);
        int r;
        for (r = 0; r < QM_ROW_H - 4; r++) {
            int y = ry + r, x;
            if (y >= ch) break;
            for (x = 0; x < QM_LIST_W && x < cw; x++)
                put_pixel(QM_LIST_X + x, y, bg);
        }
        draw_rounded_rect_border(QM_LIST_X, ry, QM_LIST_W, QM_ROW_H - 4, 3, bd);
        {
            /* "1. Arena Gate" in text color, "(q3dm1)" dimmed right after. */
            char row[28];
            int k = 0, x0 = QM_LIST_X + 10;
            row[k++] = (char)('1' + i); row[k++] = '.'; row[k++] = ' ';
            for (r = 0; qm_name[i][r] && k < 24; r++, k++) row[k] = qm_name[i][r];
            row[k] = '\0';
            draw_string_px(x0, ry + 5, row, QM_TXT, bg);
            x0 += k * 8 + 4;
            row[0] = '('; k = 1;
            for (r = 0; qm_bsp[i][r] && k < 12; r++, k++) row[k] = qm_bsp[i][r];
            row[k++] = ')'; row[k] = '\0';
            draw_string_px(x0, ry + 5, row, QM_DIM, bg);
        }
    }
    if (qm_shot_map != qm_sel) qm_load_shot(qm_sel);
    qm_draw_thumb(QM_TH_X, QM_LIST_Y);
    /* Sensitivity row. */
    {
        char val[8];
        int bx = 12, by = QM_SENS_Y, bw = 120, p, x;
        qm_sens_text(val);
        draw_string_px(bx, by, "Mouse sens:", QM_TXT, QM_BG);
        draw_rounded_rect_border(bx + 108, by - 4, 26, 22, 3, QM_ROWBD);
        draw_string_px(bx + 116, by, "-", QM_TXT, QM_BG);
        for (x = 0; x < bw; x++)
            for (p = 0; p < 8; p++)
                put_pixel(bx + 142 + x, by + 4 + p,
                          x < (qm_sens - 5) * bw / 95 ?
                          QM_AMBER : QM_ROW);
        draw_rounded_rect_border(bx + 270, by - 4, 26, 22, 3, QM_ROWBD);
        draw_string_px(bx + 278, by, "+", QM_TXT, QM_BG);
        draw_string_px(bx + 306, by, val, QM_Q3AMB, QM_BG);
        qm_minus_x = bx + 108; qm_plus_x = bx + 270;
    }
    /* Buttons. */
    {
        int by = ch - 64;
        draw_rounded_rect(12, by, QM_BTN_W, QM_BTN_H, 4, QM_GREENBG);
        draw_rounded_rect_border(12, by, QM_BTN_W, QM_BTN_H, 4, QM_GREEN);
        draw_string_px(12 + (QM_BTN_W - 4 * 8) / 2, by + 10, "MAIN", QM_TXT, QM_GREENBG);
        draw_rounded_rect(12 + QM_BTN_W + 14, by, QM_BTN_W, QM_BTN_H, 4, QM_ROW);
        draw_rounded_rect_border(12 + QM_BTN_W + 14, by, QM_BTN_W, QM_BTN_H, 4, QM_ROWBD);
        draw_string_px(12 + QM_BTN_W + 14 + (QM_BTN_W - 6 * 8) / 2, by + 10, "KELUAR", QM_TXT, QM_ROW);
        qm_btn_y = by;
    }
    draw_string_px(12, ch - 22, "klik / panah+Enter | ESC tutup", QM_DIM, QM_BG);
    if (qm_msg_ticks > 0 && qm_msg_text[0])
        draw_string_px(12, ch - 44, qm_msg_text, QM_RED, QM_BG);
}

/* MAIN/KELUAR actions. */

static void qm_do_main(void) {
    char st[96];
    int ph = q3arena_status(st, (int)sizeof(st));
    /* v38.150: launch unless a game is LOADING (1) or RUNNING (2) right now —
     * a second engine on top of a live one would trample its global state.
     * Phase 0 (never launched) and phase 3 (previous session parked after
     * GAME_SHUTDOWN) both go through the driver's own relaunch path, the same
     * one typing `q3arena` twice uses (v38.130: main -> ESC -> main is safe).
     * The old `>= 1` check also blocked phase 3, so after quitting one game
     * MAIN silently did nothing until reboot. */
    if (ph == 1 || ph == 2) {
        const char *m = (ph == 1) ? "masih loading..." : "game sudah jalan";
        int i = 0;
        while (m[i] && i < (int)sizeof(qm_msg_text) - 1) {
            qm_msg_text[i] = m[i];
            i++;
        }
        qm_msg_text[i] = '\0';
        qm_msg_ticks = 90;   /* ~3 s at the tick cadence */
        wm_invalidate(qm_win);
        return;
    }
    {
        int id = qm_win;
        q3arena_set_sens_hund(qm_sens);
        qm_open = 0; qm_win = -1; qm_hover = -1; qm_msg_ticks = 0;
        qm_free_shot();
        if (id >= 0) wm_close(id);
        q3arena_launch_map(qm_bsp[qm_sel]);
    }
}

static void qm_do_close(void) {
    int id = qm_win;
    qm_open = 0; qm_win = -1; qm_hover = -1; qm_msg_ticks = 0;
    qm_free_shot();
    if (id >= 0) wm_close(id);
}

static void qm_key(int id, char c, uint8_t sc) {
    (void)id;
    if (sc & 0x80) return;   /* releases do nothing in a menu */
    if (sc == 0x48) { qm_sel = (qm_sel + QM_NMAPS - 1) % QM_NMAPS; wm_invalidate(qm_win); return; }
    if (sc == 0x50) { qm_sel = (qm_sel + 1) % QM_NMAPS; wm_invalidate(qm_win); return; }
    if (sc == 0x4B) { qm_sens -= 5; if (qm_sens < 5) qm_sens = 5; wm_invalidate(qm_win); return; }
    if (sc == 0x4D) { qm_sens += 5; if (qm_sens > 100) qm_sens = 100; wm_invalidate(qm_win); return; }
    if (sc == 0x1C || c == '\r' || c == '\n') { qm_do_main(); return; }
    if (sc == 0x01) { qm_do_close(); return; }
    if (c >= '1' && c <= '4') { qm_sel = c - '1'; wm_invalidate(qm_win); return; }
}

static void qm_mouse(int id, int cx, int cy, int btn) {
    int i, press;
    (void)id;
    /* Hover highlight from any motion event. */
    qm_hover = -1;
    for (i = 0; i < QM_NMAPS; i++) {
        int ry = QM_LIST_Y + i * QM_ROW_H;
        if (cx >= QM_LIST_X && cx <= QM_LIST_X + QM_LIST_W &&
            cy >= ry && cy <= ry + QM_ROW_H - 4) { qm_hover = i; break; }
    }
    press = (btn == 1 && qm_prev_btn != 1);
    qm_prev_btn = (btn == 1) ? 1 : 0;
    if (!press) {
        static int last_hov = -2;
        if (qm_hover != last_hov) { last_hov = qm_hover; wm_invalidate(qm_win); }
        return;
    }
    if (qm_hover >= 0) { qm_sel = qm_hover; wm_invalidate(qm_win); return; }
    if (cy >= QM_SENS_Y - 4 && cy <= QM_SENS_Y + 18) {
        if (cx >= qm_minus_x && cx <= qm_minus_x + 26) {
            qm_sens -= 5; if (qm_sens < 5) qm_sens = 5;
            wm_invalidate(qm_win); return;
        }
        if (cx >= qm_plus_x && cx <= qm_plus_x + 26) {
            qm_sens += 5; if (qm_sens > 100) qm_sens = 100;
            wm_invalidate(qm_win); return;
        }
    }
    {
        int by = qm_btn_y;
        if (cy >= by && cy <= by + QM_BTN_H) {
            if (cx >= 12 && cx <= 12 + QM_BTN_W) { qm_do_main(); return; }
            if (cx >= 12 + QM_BTN_W + 14 && cx <= 12 + QM_BTN_W + 14 + QM_BTN_W) {
                qm_do_close(); return;
            }
        }
    }
}

static void qm_tick(int id) {
    if (!wm_is_open(id)) {
        qm_open = 0; qm_win = -1; qm_hover = -1; qm_msg_ticks = 0;
        qm_free_shot();
        return;
    }
    if (qm_msg_ticks > 0 && --qm_msg_ticks == 0) wm_invalidate(id);
}

void open_q3menu_app(void) {
    if (qm_open && wm_is_open(qm_win)) { wm_raise(qm_win); return; }
    qm_free_shot();
    qm_sel = 0; qm_hover = -1; qm_prev_btn = 0; qm_msg_ticks = 0;
    qm_sens = q3arena_sens_hund();
    qm_win = wm_open(120, 90, QM_W, QM_H, "Quake III - Mectov Edition",
                     qm_draw, qm_key, qm_tick, qm_mouse);
    if (qm_win < 0) return;
    qm_open = 1;
    wm_request_scancodes(qm_win, 1);
    qm_load_shot(qm_sel);
    wm_invalidate(qm_win);
}

#else /* !MECTOV_Q3 */

void open_q3menu_app(void) { /* engine not compiled in; the shell says why */ }

#endif /* MECTOV_Q3 */
