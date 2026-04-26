#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/wm.h"
#include "../include/timer.h"
#include "../include/font8x16.h"

static int clock_win_id = -1;
static int clock_open = 0;

static void clock_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    draw_rect(cx, cy, cw, ch, GUI_BG);

    unsigned char sec  = bcd_to_bin(read_cmos(0x00));
    unsigned char min  = bcd_to_bin(read_cmos(0x02));
    unsigned char hour = bcd_to_bin(read_cmos(0x04));
    unsigned char day  = bcd_to_bin(read_cmos(0x07));
    unsigned char mon  = bcd_to_bin(read_cmos(0x08));
    unsigned char yr   = bcd_to_bin(read_cmos(0x09));

    hour = (hour + 7) % 24; // WIB (UTC+7)

    // Big time display (2x scale via double rows)
    char tbuf[9];
    tbuf[0] = '0' + hour/10; tbuf[1] = '0' + hour%10;
    tbuf[2] = ':';
    tbuf[3] = '0' + min/10;  tbuf[4] = '0' + min%10;
    tbuf[5] = ':';
    tbuf[6] = '0' + sec/10;  tbuf[7] = '0' + sec%10;
    tbuf[8] = '\0';

    // Draw time large (2x by drawing each char twice the height)
    int tx = cx + (cw - 8*8*2) / 2;
    int ty = cy + 20;
    for (int i = 0; i < 8; i++) {
        for (int row = 0; row < 16; row++) {
            unsigned char bits = font8x16_data[(unsigned char)tbuf[i]][row];
            for (int col2 = 0; col2 < 8; col2++) {
                uint32_t color = (bits & (0x80 >> col2)) ?
                    (tbuf[i] == ':' ? GUI_CYAN : GUI_TEXT) : GUI_BG;
                put_pixel(tx + i*16 + col2*2,     ty + row*2,     color);
                put_pixel(tx + i*16 + col2*2 + 1, ty + row*2,     color);
                put_pixel(tx + i*16 + col2*2,     ty + row*2 + 1, color);
                put_pixel(tx + i*16 + col2*2 + 1, ty + row*2 + 1, color);
            }
        }
    }

    // Date line
    char dbuf[16];
    dbuf[0]='2'; dbuf[1]='0'; dbuf[2]='0'+yr/10; dbuf[3]='0'+yr%10; dbuf[4]='-';
    dbuf[5]='0'+mon/10; dbuf[6]='0'+mon%10; dbuf[7]='-';
    dbuf[8]='0'+day/10; dbuf[9]='0'+day%10; dbuf[10]='\0';
    int dx = cx + (cw - 10*8) / 2;
    draw_string_px(dx, cy + ch - 30, dbuf, GUI_DIM, 0xFFFFFFFF);

    // Decorative ring
    draw_rect_border(cx + 10, cy + 10, cw - 20, ch - 20, GUI_BORDER2);
}

static void clock_key(int id, char c, uint8_t sc) { (void)id; (void)c; (void)sc; }
static void clock_tick(int id) { (void)id; }

void open_clock_app() {
    if (clock_open && wm_is_open(clock_win_id)) { wm_raise(clock_win_id); return; }
    clock_win_id = wm_open(200, 150, 300, 160, "Clock", clock_draw, clock_key, clock_tick);
    clock_open = (clock_win_id >= 0);
}
