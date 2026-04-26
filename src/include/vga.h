#ifndef VGA_H
#define VGA_H

#define WIN_X 2
#define WIN_Y 2
#define WIN_W 76
#define WIN_H 20
#define CX (WIN_X + 1)
#define CY (WIN_Y + 1)
#define CW (WIN_W - 2)
#define CH (WIN_H - 2)

extern volatile char* video_m;
extern unsigned char cur_col;
extern int cx, cy;

void d_char(int x, int y, char c, unsigned char col);
void d_desktop();
void d_win(int x, int y, int w, int h, const char* t);
void c_work();
void s_work();
void print(const char* s, unsigned char col);
void p_char(char c, unsigned char col);
void update_hw_cursor(int x, int y);
void wait_retrace();
void update_marquee();
void update_hud();

#endif
