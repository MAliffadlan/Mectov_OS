#include "../include/apps.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/speaker.h"
#include "../include/utils.h"
#include "../include/mem.h"
#include "../include/timer.h"
#include "../include/wm.h"
#include "../include/io.h"

// ============================================================
// Snake Game — Window Manager App
// ============================================================

#define GRID_W   20
#define GRID_H   15
#define CELL     12
#define MAX_LEN  100

// Window dimensions (content area)
#define WIN_CW   (GRID_W * CELL)
#define WIN_CH   (GRID_H * CELL + 24)  // +24 for score bar

static int snake_win_id = -1;
static int snake_open = 0;

// Game state
static int sx[MAX_LEN], sy[MAX_LEN];
static int slen = 3;
static int fx, fy;        // food position
static int dir = 1;       // 0=up 1=right 2=down 3=left
static int score = 0;
static int game_over = 0;
static uint32_t last_move_tick = 0;
static int move_interval = 150; // ms between moves

static void snake_reset() {
    slen = 3;
    dir = 1;
    score = 0;
    game_over = 0;
    last_move_tick = 0;
    for (int i = 0; i < slen; i++) {
        sx[i] = GRID_W / 2 - i;
        sy[i] = GRID_H / 2;
    }
    fx = 5 + (rand() % (GRID_W - 6));
    fy = 2 + (rand() % (GRID_H - 4));
}

static void snake_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id; (void)cw; (void)ch;

    // Background
    draw_rect(cx, cy, WIN_CW, WIN_CH, 0x001A1A2E);

    // Grid area
    int gx = cx;
    int gy = cy + 24;  // below score bar

    // Draw grid background with subtle checkerboard
    for (int r = 0; r < GRID_H; r++) {
        for (int c = 0; c < GRID_W; c++) {
            uint32_t bg = ((r + c) % 2 == 0) ? 0x00222240 : 0x001E1E38;
            draw_rect(gx + c * CELL, gy + r * CELL, CELL, CELL, bg);
        }
    }

    // Draw food (red apple)
    draw_rounded_rect(gx + fx * CELL + 1, gy + fy * CELL + 1, CELL - 2, CELL - 2, 3, 0x00FF4444);
    // Food highlight
    draw_rect(gx + fx * CELL + 3, gy + fy * CELL + 3, 3, 2, 0x00FF8888);

    // Draw snake
    for (int i = 0; i < slen; i++) {
        uint32_t col;
        if (i == 0) {
            col = 0x0000FF88; // Head: bright green
        } else {
            // Body: gradient from bright to dark green
            int fade = 180 - (i * 120 / slen);
            if (fade < 60) fade = 60;
            col = (fade << 8) | 0x00004400;
        }
        draw_rounded_rect(gx + sx[i] * CELL + 1, gy + sy[i] * CELL + 1,
                          CELL - 2, CELL - 2, 3, col);
    }

    // Draw eyes on head
    if (slen > 0) {
        int hx = gx + sx[0] * CELL;
        int hy = gy + sy[0] * CELL;
        if (dir == 1) { // right
            draw_rect(hx + 7, hy + 3, 2, 2, 0x00FFFFFF);
            draw_rect(hx + 7, hy + 7, 2, 2, 0x00FFFFFF);
        } else if (dir == 3) { // left
            draw_rect(hx + 3, hy + 3, 2, 2, 0x00FFFFFF);
            draw_rect(hx + 3, hy + 7, 2, 2, 0x00FFFFFF);
        } else if (dir == 0) { // up
            draw_rect(hx + 3, hy + 3, 2, 2, 0x00FFFFFF);
            draw_rect(hx + 7, hy + 3, 2, 2, 0x00FFFFFF);
        } else { // down
            draw_rect(hx + 3, hy + 7, 2, 2, 0x00FFFFFF);
            draw_rect(hx + 7, hy + 7, 2, 2, 0x00FFFFFF);
        }
    }

    // Score bar (top)
    draw_rect(cx, cy, WIN_CW, 22, 0x00111128);
    draw_rect(cx, cy + 22, WIN_CW, 2, 0x00333366);

    char sbuf[32];
    int si = 0;
    sbuf[si++] = 'S'; sbuf[si++] = 'c'; sbuf[si++] = 'o';
    sbuf[si++] = 'r'; sbuf[si++] = 'e'; sbuf[si++] = ':';
    sbuf[si++] = ' ';
    // Convert score to string
    int tmp = score;
    if (tmp == 0) { sbuf[si++] = '0'; }
    else {
        char rev[8]; int rl = 0;
        while (tmp > 0) { rev[rl++] = '0' + tmp % 10; tmp /= 10; }
        while (rl > 0) sbuf[si++] = rev[--rl];
    }
    sbuf[si] = '\0';
    draw_string_px(cx + 8, cy + 3, sbuf, 0x0000FFAA, 0x00111128);

    // Speed indicator
    char spd[16];
    int spi = 0;
    spd[spi++] = 'S'; spd[spi++] = 'p'; spd[spi++] = 'e';
    spd[spi++] = 'e'; spd[spi++] = 'd'; spd[spi++] = ':';
    spd[spi++] = ' ';
    tmp = 1000 / move_interval;
    if (tmp == 0) { spd[spi++] = '0'; }
    else {
        char rev[8]; int rl = 0;
        while (tmp > 0) { rev[rl++] = '0' + tmp % 10; tmp /= 10; }
        while (rl > 0) spd[spi++] = rev[--rl];
    }
    spd[spi] = '\0';
    draw_string_px(cx + WIN_CW - spi * 8 - 8, cy + 3, spd, 0x00AAAACC, 0x00111128);

    // Game over overlay
    if (game_over) {
        // Semi-transparent overlay
        draw_rect_alpha(gx + 20, gy + GRID_H * CELL / 2 - 24, WIN_CW - 40, 48, 0x00000000);
        draw_string_px(gx + WIN_CW / 2 - 40, gy + GRID_H * CELL / 2 - 14,
                       "GAME OVER", 0x00FF4444, 0x00000000);
        draw_string_px(gx + WIN_CW / 2 - 56, gy + GRID_H * CELL / 2 + 6,
                       "Press ENTER", 0x00AAAACC, 0x00000000);
    }
}

static void snake_key(int id, char c, uint8_t sc) {
    (void)id; (void)c;

    if (game_over) {
        if (sc == 0x1C) { // ENTER
            snake_reset();
        }
        return;
    }

    // WASD + Arrow keys
    if ((sc == 0x11 || sc == 0x48) && dir != 2) dir = 0; // W / UP
    if ((sc == 0x1F || sc == 0x50) && dir != 0) dir = 2; // S / DOWN
    if ((sc == 0x1E || sc == 0x4B) && dir != 1) dir = 3; // A / LEFT
    if ((sc == 0x20 || sc == 0x4D) && dir != 3) dir = 1; // D / RIGHT
}

static void snake_tick(int id) {
    (void)id;
    if (game_over) return;

    uint32_t now = get_ticks();
    if (now - last_move_tick < (uint32_t)move_interval) return;
    last_move_tick = now;

    // Move body
    for (int i = slen - 1; i > 0; i--) {
        sx[i] = sx[i-1]; sy[i] = sy[i-1];
    }

    // Move head
    if (dir == 0) sy[0]--;
    else if (dir == 1) sx[0]++;
    else if (dir == 2) sy[0]++;
    else if (dir == 3) sx[0]--;

    // Wall collision
    if (sx[0] < 0 || sx[0] >= GRID_W || sy[0] < 0 || sy[0] >= GRID_H) {
        game_over = 1;
        beep();
        return;
    }

    // Self collision
    for (int i = 1; i < slen; i++) {
        if (sx[0] == sx[i] && sy[0] == sy[i]) {
            game_over = 1;
            beep();
            return;
        }
    }

    // Food collision
    if (sx[0] == fx && sy[0] == fy) {
        score += 10;
        if (slen < MAX_LEN) slen++;
        // Speed up every 50 points
        if (move_interval > 60 && score % 50 == 0) move_interval -= 15;
        beep();
        // New food position (avoid snake body)
        int valid;
        do {
            fx = rand() % GRID_W;
            fy = rand() % GRID_H;
            valid = 1;
            for (int i = 0; i < slen; i++) {
                if (sx[i] == fx && sy[i] == fy) { valid = 0; break; }
            }
        } while (!valid);
    }
}

void start_ular() {
    if (snake_open && wm_is_open(snake_win_id)) {
        wm_raise(snake_win_id);
        return;
    }
    snake_reset();
    snake_win_id = wm_open(150, 100, WIN_CW + 2, WIN_CH + 2, "Snake",
                           snake_draw, snake_key, snake_tick, NULL);
    snake_open = (snake_win_id >= 0);
}
