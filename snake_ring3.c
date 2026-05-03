#include "src/include/syscall.h"

// --- Basic stdlib ---
static uint32_t seed = 12345;
static int rand() {
    seed = (1103515245 * seed + 12345);
    return (int)(seed & 0x7FFFFFFF);
}

static void itoa(int val, char* buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char temp[12]; int len = 0;
    while (val > 0) { temp[len++] = '0' + (val % 10); val /= 10; }
    int pos = 0;
    for (int i = len - 1; i >= 0; i--) buf[pos++] = temp[i];
    buf[pos] = '\0';
}

typedef struct {
    int type; // 1 = paint, 2 = key, 3 = mouse
    int x, y;
    int key;
} gui_event_t;

// --- Game Logic ---
#define GRID_W   20
#define GRID_H   15
#define CELL     12
#define MAX_LEN  100
#define WIN_CW   (GRID_W * CELL)
#define WIN_CH   (GRID_H * CELL + 24)

static int sx[MAX_LEN], sy[MAX_LEN];
static int slen = 3;
static int fx, fy;
static int dir = 1; // 0=up 1=right 2=down 3=left
static int score = 0;
static int game_over = 0;
static int move_interval = 150; 
static uint32_t last_tick = 0;

static void reset_game() {
    slen = 3;
    dir = 1;
    score = 0;
    game_over = 0;
    last_tick = 0;
    for (int i = 0; i < slen; i++) {
        sx[i] = GRID_W / 2 - i;
        sy[i] = GRID_H / 2;
    }
    fx = 5 + (rand() % (GRID_W - 6));
    fy = 2 + (rand() % (GRID_H - 4));
}

static void draw_game(int wid) {
    // Bg
    sys_draw_rect(wid, 0, 0, WIN_CW, WIN_CH, 0x001A1A2E);
    
    int gy = 24; // grid Y offset
    
    // Top bar
    sys_draw_rect(wid, 0, 0, WIN_CW, 24, 0x0016202A);
    sys_draw_text(wid, 8, 4, "Score: ", 0x0027C93F);
    char sbuf[16]; itoa(score, sbuf);
    sys_draw_text(wid, 64, 4, sbuf, 0x0027C93F);
    
    sys_draw_text(wid, WIN_CW - 80, 4, "Ring 3", 0x00FFBD2E);
    
    if (game_over) {
        sys_draw_rect(wid, WIN_CW/2 - 60, WIN_CH/2 - 20, 120, 40, 0x00111111);
        sys_draw_text(wid, WIN_CW/2 - 40, WIN_CH/2 - 12, "GAME OVER", 0x00FF5F56);
        sys_draw_text(wid, WIN_CW/2 - 45, WIN_CH/2 + 4, "Press ENTER", 0x00FFFFFF);
        sys_update_window(wid);
        return;
    }

    // Food
    sys_draw_rect(wid, fx*CELL+2, gy + fy*CELL+2, CELL-4, CELL-4, 0x00FF5F56);
    
    // Snake
    for (int i = 0; i < slen; i++) {
        uint32_t col = (i == 0) ? 0x0027C93F : 0x001B9A2F;
        sys_draw_rect(wid, sx[i]*CELL+1, gy + sy[i]*CELL+1, CELL-2, CELL-2, col);
    }
    
    sys_update_window(wid);
}

void _start() {
    int wid = sys_create_window(250, 150, WIN_CW, WIN_CH, "Snake (Ring 3)");
    if (wid < 0) sys_exit();
    
    rtc_time_t tm;
    sys_get_time(&tm);
    seed = tm.second * 1000 + tm.minute;
    
    reset_game();
    draw_game(wid);
    
    gui_event_t ev;
    int tick_count = 0;
    
    while (1) {
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 1) {
                draw_game(wid);
            } else if (ev.type == 2) {
                if (ev.key == 27) { // ESC ASCII
                    sys_exit();
                } else if (game_over && ev.key == '\n') { // ENTER ASCII
                    reset_game();
                    draw_game(wid);
                } else if (!game_over) {
                    if ((ev.key == 'w' || ev.key == 'W') && dir != 2) dir = 0; // UP
                    else if ((ev.key == 's' || ev.key == 'S') && dir != 0) dir = 2; // DOWN
                    else if ((ev.key == 'a' || ev.key == 'A') && dir != 1) dir = 3; // LEFT
                    else if ((ev.key == 'd' || ev.key == 'D') && dir != 3) dir = 1; // RIGHT
                }
            }
        }
        
        tick_count++;
        if (!game_over && (tick_count - last_tick > move_interval)) {
            last_tick = tick_count;
            
            int nx = sx[0];
            int ny = sy[0];
            if (dir == 0) ny--; else if (dir == 1) nx++;
            else if (dir == 2) ny++; else if (dir == 3) nx--;
            
            // Collision walls
            if (nx < 0 || nx >= GRID_W || ny < 0 || ny >= GRID_H) {
                game_over = 1;
                sys_play_sound(150, 500);
            }
            
            // Collision self
            for (int i = 1; i < slen; i++) {
                if (nx == sx[i] && ny == sy[i]) {
                    game_over = 1;
                    sys_play_sound(150, 500);
                }
            }
            
            if (!game_over) {
                // Move body
                for (int i = slen - 1; i > 0; i--) {
                    sx[i] = sx[i-1];
                    sy[i] = sy[i-1];
                }
                sx[0] = nx; sy[0] = ny;
                
                // Eat food
                if (nx == fx && ny == fy) {
                    if (slen < MAX_LEN) slen++;
                    score += 10;
                    fx = rand() % GRID_W;
                    fy = rand() % GRID_H;
                    sys_play_sound(800, 50);
                    
                    if (move_interval > 50) move_interval -= 2;
                }
            }
            draw_game(wid);
        }
        
        // Wait 1ms effectively
        sys_yield();
    }
}
