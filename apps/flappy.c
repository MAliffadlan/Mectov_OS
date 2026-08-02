#include "src/include/syscall.h"

// --- Basic stdlib ---
static uint32_t seed = 98765;
static int rand() {
    seed = (1103515245 * seed + 12345);
    return (int)(seed & 0x7FFFFFFF);
}

static void itoa(int val, char* buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char temp[12]; int len = 0;
    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    while (val > 0) { temp[len++] = '0' + (val % 10); val /= 10; }
    int pos = 0;
    if (neg) buf[pos++] = '-';
    for (int i = len - 1; i >= 0; i--) buf[pos++] = temp[i];
    buf[pos] = '\0';
}

typedef struct {
    int type; // 1 = paint, 2 = key, 3 = mouse
    int x, y;
    int key;
} gui_event_t;

// --- Game Constants ---
#define WIN_W 280
#define WIN_H 420

#define BIRD_X 70
#define BIRD_W 22
#define BIRD_H 18
#define GRAVITY 1
#define JUMP_VEL -8
#define MAX_FALL_VEL 12

#define PIPE_W 36
#define PIPE_GAP_H 140       // Celah lebih lebar = lebih gampang
#define PIPE_SPEED 3          // Pipa lebih lambat = lebih santai
#define PIPE_SPACING 220      // Jarak antar pipa lebih jauh

#define NUM_PIPES 2           // Cuma 2 pipa = rintangan lebih dikit

#define GROUND_H 30

// --- Colors (Premium Palette) ---
#define COL_SKY_TOP    0x004EC5F1   // Light cyan sky
#define COL_SKY_BOT    0x0087CEEB   // Deeper sky blue
#define COL_CLOUD      0x00E8F4FD   // Wispy cloud white
#define COL_PIPE_BODY  0x005DBE5D   // Pipe green
#define COL_PIPE_DARK  0x004A9E4A   // Pipe dark edge
#define COL_PIPE_LIP   0x006ED86E   // Pipe lip highlight
#define COL_PIPE_SHADE 0x003D8B3D   // Pipe inner shadow
#define COL_GROUND     0x00DEB887   // Sand
#define COL_GRASS1     0x005CBF2A   // Bright grass
#define COL_GRASS2     0x004AA022   // Dark grass
#define COL_DIRT       0x00A0764E   // Dark dirt stripe
#define COL_BIRD_BODY  0x00F5C842   // Golden yellow
#define COL_BIRD_BELLY 0x00FCE38A   // Light belly
#define COL_BIRD_WING  0x00E8A317   // Orange wing
#define COL_BIRD_EYE_W 0x00FFFFFF   // Eye white
#define COL_BIRD_EYE_P 0x00111111   // Eye pupil
#define COL_BIRD_BEAK  0x00E8572A   // Red-orange beak
#define COL_BIRD_DEAD  0x00CC3333   // Dead bird
#define COL_SCORE_BG   0x00000000   // Score shadow
#define COL_SCORE_FG   0x00FFFFFF   // Score text
#define COL_PANEL_BG   0x001A1A2E   // Dark panel
#define COL_PANEL_BD   0x003A3A5E   // Panel border
#define COL_TITLE      0x00F5C842   // Title gold
#define COL_SUBTITLE   0x00AAAACC   // Subtitle grey

// --- Game State ---
static int bird_y = 200;
static int bird_vy = 0;
static int best_score = 0;
static int started = 0;       // 0 = Title screen, 1 = Playing

typedef struct {
    int x;
    int gap_y;
    int passed;
} pipe_t;

static pipe_t pipes[NUM_PIPES];
static int score = 0;
static int game_over = 0;
static int frame_count = 0;

// Cloud positions for parallax
static int cloud_x[3] = { 30, 150, 260 };
static int cloud_y[3] = { 35, 70, 20 };

static void reset_game() {
    bird_y = WIN_H / 3;
    bird_vy = 0;
    score = 0;
    game_over = 0;
    started = 0;
    frame_count = 0;
    
    for (int i = 0; i < NUM_PIPES; i++) {
        pipes[i].x = WIN_W + 60 + (i * PIPE_SPACING);
        pipes[i].gap_y = 60 + (rand() % (WIN_H - PIPE_GAP_H - GROUND_H - 80));
        pipes[i].passed = 0;
    }
}

// --- Drawing Helpers ---

static void draw_cloud(int wid, int cx, int cy) {
    // Simple fluffy cloud with overlapping circles (rects)
    sys_draw_rect(wid, cx, cy, 28, 10, COL_CLOUD);
    sys_draw_rect(wid, cx + 4, cy - 4, 20, 6, COL_CLOUD);
    sys_draw_rect(wid, cx + 8, cy - 7, 12, 5, COL_CLOUD);
}

static void draw_pipe(int wid, int px, int gap_y) {
    if (px >= WIN_W || px + PIPE_W <= 0) return;
    
    int lip_w = PIPE_W + 8;
    int lip_x = px - 4;
    int lip_h = 14;
    
    // ---- Top pipe ----
    // Body
    sys_draw_rect(wid, px, 0, PIPE_W, gap_y - lip_h, COL_PIPE_BODY);
    // Dark left edge
    sys_draw_rect(wid, px, 0, 3, gap_y - lip_h, COL_PIPE_DARK);
    // Highlight right edge
    sys_draw_rect(wid, px + PIPE_W - 4, 0, 4, gap_y - lip_h, COL_PIPE_LIP);
    // Lip (wider cap)
    sys_draw_rect(wid, lip_x, gap_y - lip_h, lip_w, lip_h, COL_PIPE_LIP);
    sys_draw_rect(wid, lip_x, gap_y - lip_h, 3, lip_h, COL_PIPE_DARK);
    sys_draw_rect(wid, lip_x, gap_y - 3, lip_w, 3, COL_PIPE_SHADE);
    
    // ---- Bottom pipe ----
    int bottom_y = gap_y + PIPE_GAP_H;
    // Lip (wider cap)
    sys_draw_rect(wid, lip_x, bottom_y, lip_w, lip_h, COL_PIPE_LIP);
    sys_draw_rect(wid, lip_x, bottom_y, 3, lip_h, COL_PIPE_DARK);
    sys_draw_rect(wid, lip_x, bottom_y, lip_w, 3, COL_PIPE_SHADE);
    // Body
    sys_draw_rect(wid, px, bottom_y + lip_h, PIPE_W, WIN_H - bottom_y - lip_h, COL_PIPE_BODY);
    sys_draw_rect(wid, px, bottom_y + lip_h, 3, WIN_H - bottom_y - lip_h, COL_PIPE_DARK);
    sys_draw_rect(wid, px + PIPE_W - 4, bottom_y + lip_h, 4, WIN_H - bottom_y - lip_h, COL_PIPE_LIP);
}

static void draw_bird(int wid, int by, int vy, int dead) {
    int bx = BIRD_X;
    
    if (dead) {
        // Dead bird — red tint, X eyes
        sys_draw_rect(wid, bx, by, BIRD_W, BIRD_H, COL_BIRD_DEAD);
        sys_draw_rect(wid, bx + 2, by + 2, BIRD_W - 4, BIRD_H - 4, 0x00AA2222);
        sys_draw_text(wid, bx + 6, by + 2, "XX", COL_BIRD_EYE_W);
        return;
    }
    
    // Body (rounded feel via layered rects)
    sys_draw_rect(wid, bx + 2, by, BIRD_W - 4, BIRD_H, COL_BIRD_BODY);
    sys_draw_rect(wid, bx, by + 2, BIRD_W, BIRD_H - 4, COL_BIRD_BODY);
    
    // Belly highlight
    sys_draw_rect(wid, bx + 4, by + BIRD_H/2, BIRD_W - 8, BIRD_H/2 - 3, COL_BIRD_BELLY);
    
    // Wing (animates based on velocity)
    int wing_y = by + 4;
    if (vy < -2) wing_y = by + 1;  // Wing up when jumping
    else if (vy > 4) wing_y = by + 8; // Wing down when falling
    sys_draw_rect(wid, bx - 2, wing_y, 8, 6, COL_BIRD_WING);
    sys_draw_rect(wid, bx - 4, wing_y + 1, 4, 4, COL_BIRD_WING);
    
    // Eye (white circle with pupil)
    sys_draw_rect(wid, bx + 13, by + 3, 6, 6, COL_BIRD_EYE_W);
    sys_draw_rect(wid, bx + 16, by + 4, 3, 3, COL_BIRD_EYE_P);
    
    // Beak
    sys_draw_rect(wid, bx + BIRD_W - 1, by + 7, 7, 4, COL_BIRD_BEAK);
    sys_draw_rect(wid, bx + BIRD_W + 2, by + 8, 4, 2, 0x00CC4422);
}

static void draw_ground(int wid) {
    int gy = WIN_H - GROUND_H;
    // Grass top strip
    sys_draw_rect(wid, 0, gy, WIN_W, 5, COL_GRASS1);
    sys_draw_rect(wid, 0, gy + 5, WIN_W, 3, COL_GRASS2);
    // Dirt body
    sys_draw_rect(wid, 0, gy + 8, WIN_W, GROUND_H - 8, COL_GROUND);
    // Dirt texture stripes
    sys_draw_rect(wid, 0, gy + 14, WIN_W, 2, COL_DIRT);
    sys_draw_rect(wid, 0, gy + 22, WIN_W, 2, COL_DIRT);
}

static void draw_score_big(int wid, int sc) {
    char sbuf[16];
    itoa(sc, sbuf);
    int len = 0;
    while (sbuf[len]) len++;
    int sx = WIN_W / 2 - (len * 4);
    // Shadow
    sys_draw_text(wid, sx + 1, 31, sbuf, COL_SCORE_BG);
    // Main
    sys_draw_text(wid, sx, 30, sbuf, COL_SCORE_FG);
}

static void draw_game(int wid) {
    // === Sky gradient (simulated with 2 bands) ===
    sys_draw_rect(wid, 0, 0, WIN_W, WIN_H / 2, COL_SKY_TOP);
    sys_draw_rect(wid, 0, WIN_H / 2, WIN_W, WIN_H / 2, COL_SKY_BOT);
    
    // === Clouds (parallax decoration) ===
    for (int i = 0; i < 3; i++) {
        draw_cloud(wid, cloud_x[i], cloud_y[i]);
    }
    
    // === Pipes ===
    for (int i = 0; i < NUM_PIPES; i++) {
        draw_pipe(wid, pipes[i].x, pipes[i].gap_y);
    }
    
    // === Ground ===
    draw_ground(wid);
    
    // === Bird ===
    draw_bird(wid, bird_y, bird_vy, game_over);
    
    // === Score ===
    if (started && !game_over) {
        draw_score_big(wid, score);
    }
    
    // === Title Screen ===
    if (!started && !game_over) {
        // Title panel
        int px = WIN_W/2 - 90;
        int py = WIN_H/2 - 70;
        sys_draw_rect(wid, px, py, 180, 120, COL_PANEL_BG);
        sys_draw_rect(wid, px, py, 180, 2, COL_PANEL_BD);
        sys_draw_rect(wid, px, py + 118, 180, 2, COL_PANEL_BD);
        sys_draw_rect(wid, px, py, 2, 120, COL_PANEL_BD);
        sys_draw_rect(wid, px + 178, py, 2, 120, COL_PANEL_BD);
        
        sys_draw_text(wid, px + 24, py + 16, "FLAPPY MECTOV", COL_TITLE);
        sys_draw_text(wid, px + 20, py + 45, "Space / Click", COL_SUBTITLE);
        sys_draw_text(wid, px + 36, py + 60, "to Jump!", COL_SUBTITLE);
        
        // Best score
        sys_draw_text(wid, px + 36, py + 88, "Best: ", 0x0088AACC);
        char bbuf[16];
        itoa(best_score, bbuf);
        sys_draw_text(wid, px + 84, py + 88, bbuf, COL_TITLE);
    }
    
    // === Game Over panel ===
    if (game_over) {
        int px = WIN_W/2 - 90;
        int py = WIN_H/2 - 60;
        // Panel bg
        sys_draw_rect(wid, px, py, 180, 110, COL_PANEL_BG);
        sys_draw_rect(wid, px, py, 180, 2, 0x00FF5F56);
        sys_draw_rect(wid, px, py + 108, 180, 2, COL_PANEL_BD);
        sys_draw_rect(wid, px, py, 2, 110, COL_PANEL_BD);
        sys_draw_rect(wid, px + 178, py, 2, 110, COL_PANEL_BD);
        
        sys_draw_text(wid, px + 40, py + 14, "GAME OVER", 0x00FF5F56);
        
        // Score
        sys_draw_text(wid, px + 24, py + 40, "Score: ", COL_SUBTITLE);
        char s1[16]; itoa(score, s1);
        sys_draw_text(wid, px + 80, py + 40, s1, COL_SCORE_FG);
        
        // Best
        sys_draw_text(wid, px + 24, py + 58, "Best:  ", 0x0088AACC);
        char s2[16]; itoa(best_score, s2);
        sys_draw_text(wid, px + 80, py + 58, s2, COL_TITLE);
        
        sys_draw_text(wid, px + 16, py + 84, "Space to Retry", COL_SUBTITLE);
    }

    sys_update_window(wid);
}

static void jump() {
    if (game_over) {
        reset_game();
        return;
    }
    if (!started) {
        started = 1;
    }
    bird_vy = JUMP_VEL;
    sys_play_sound(700, 15);
}

void _start() {
    int wid = sys_create_window(100, 80, WIN_W, WIN_H, "Flappy Mectov");
    if (wid < 0) sys_exit();
    
    rtc_time_t tm;
    sys_get_time(&tm);
    seed = tm.second * 1000 + tm.minute * 37 + 42;
    
    reset_game();
    draw_game(wid);
    
    gui_event_t ev;
    
    while (1) {
        // Process UI events
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 1) {
                draw_game(wid);
            } else if (ev.type == 2) { // Key
                if (ev.key == 27) { // ESC
                    sys_exit();
                } else if (ev.key == ' ' || ev.key == 'w' || ev.key == 'W') {
                    jump();
                }
            } else if (ev.type == 3) { // Mouse click
                jump();
            }
        }
        
        if (started && !game_over) {
            frame_count++;
            
            if (frame_count % 25 == 0) {
                
                // Gravity
                bird_vy += GRAVITY;
                if (bird_vy > MAX_FALL_VEL) bird_vy = MAX_FALL_VEL;
                bird_y += bird_vy;
                
                // Ceiling
                if (bird_y < 0) { bird_y = 0; bird_vy = 0; }
                
                // Floor collision
                if (bird_y + BIRD_H > WIN_H - GROUND_H) {
                    bird_y = WIN_H - GROUND_H - BIRD_H;
                    game_over = 1;
                    if (score > best_score) best_score = score;
                    sys_play_sound(120, 400);
                }
                
                // Cloud parallax (slow scroll)
                if (frame_count % 75 == 0) {
                    for (int i = 0; i < 3; i++) {
                        cloud_x[i] -= 1;
                        if (cloud_x[i] < -40) cloud_x[i] = WIN_W + 10 + (rand() % 40);
                    }
                }
                
                // Pipes update
                for (int i = 0; i < NUM_PIPES; i++) {
                    pipes[i].x -= PIPE_SPEED;
                    
                    // Reset pipe when off screen
                    if (pipes[i].x + PIPE_W < -10) {
                        int max_x = 0;
                        for (int j = 0; j < NUM_PIPES; j++) {
                            if (pipes[j].x > max_x) max_x = pipes[j].x;
                        }
                        pipes[i].x = max_x + PIPE_SPACING;
                        pipes[i].gap_y = 60 + (rand() % (WIN_H - PIPE_GAP_H - GROUND_H - 80));
                        pipes[i].passed = 0;
                    }
                    
                    // Collision
                    if (BIRD_X + BIRD_W > pipes[i].x && BIRD_X < pipes[i].x + PIPE_W) {
                        if (bird_y < pipes[i].gap_y || bird_y + BIRD_H > pipes[i].gap_y + PIPE_GAP_H) {
                            game_over = 1;
                            if (score > best_score) best_score = score;
                            sys_play_sound(120, 400);
                        }
                    }
                    
                    // Score
                    if (!pipes[i].passed && pipes[i].x + PIPE_W < BIRD_X) {
                        pipes[i].passed = 1;
                        score++;
                        sys_play_sound(880, 25);
                    }
                }
                
                draw_game(wid);
            }
        }
        
        // Yield ~1ms
        sys_yield();
    }
}
