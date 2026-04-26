#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/wm.h"
#include "../include/serial.h"

int browser_win_id = -1;
int browser_open = 0;

static char url_buf[128] = "";
static int url_len = 0;
static int focused_url = 1;

#define MAX_BROWSER_TEXT 4096
static char page_text[MAX_BROWSER_TEXT];
static int page_len = 0;
static int loading = 0;

static void browser_draw(int id, int cx, int cy, int cw, int ch) {
    (void)id;
    // Background
    draw_rect(cx, cy, cw, ch, 0xFFFFFFFF); // White page background

    // Address bar area
    draw_rect(cx, cy, cw, 30, GUI_BORDER2);
    draw_string_px(cx + 8, cy + 8, "URL:", GUI_DIM, GUI_BORDER2);
    
    uint32_t url_bg = focused_url ? 0xFFFFFFFF : 0xFFDDDDDD;
    draw_rect(cx + 40, cy + 4, cw - 50, 22, url_bg);
    draw_rect_border(cx + 40, cy + 4, cw - 50, 22, GUI_BORDER);
    
    draw_string_px(cx + 44, cy + 8, url_buf, GUI_TEXT, url_bg);

    // If loading, show a little indicator
    if (loading) {
        draw_rect(cx + cw - 20, cy + 8, 10, 10, 0x00FF0000);
    }

    // Draw text body
    int lx = cx + 8;
    int ly = cy + 40;
    
    char line[128];
    int line_len = 0;
    
    for (int i = 0; i < page_len; i++) {
        char c = page_text[i];
        if (c == '\n' || line_len >= ((cw - 16) / 8) - 1) {
            line[line_len] = '\0';
            draw_string_px(lx, ly, line, GUI_TEXT, 0xFFFFFFFF);
            ly += 16;
            line_len = 0;
            if (c != '\n') { line[line_len++] = c; }
            if (ly > cy + ch - 16) break; // Screen full
        } else {
            line[line_len++] = c;
        }
    }
    if (line_len > 0 && ly <= cy + ch - 16) {
        line[line_len] = '\0';
        draw_string_px(lx, ly, line, GUI_TEXT, 0xFFFFFFFF);
    }
}

static void browser_key(int id, char c, uint8_t sc) {
    (void)id;
    if (focused_url) {
        if (sc == 0x0E) { // Backspace
            if (url_len > 0) {
                url_len--;
                url_buf[url_len] = '\0';
            }
        } else if (sc == 0x1C) { // Enter
            if (url_len > 0) {
                // Clear page
                page_len = 0;
                page_text[0] = '\0';
                loading = 1;
                focused_url = 0;
                
                // Send over serial
                write_serial_string("GET ");
                write_serial_string(url_buf);
                write_serial_string("\n");
            }
        } else if (c >= 32 && c <= 126 && url_len < 120) {
            url_buf[url_len++] = c;
            url_buf[url_len] = '\0';
        }
    } else {
        if (sc == 0x39) { // Space - re-focus URL bar
            focused_url = 1;
        }
    }
}

static void browser_mouse(int id, int cx, int cy, int btn) {
    (void)id; (void)cx; (void)cy; (void)btn;
    if (btn == 1 && cy < 30) {
        focused_url = 1;
    } else if (btn == 1) {
        focused_url = 0;
    }
}

static void browser_tick(int id) {
    if (!wm_is_open(id)) {
        browser_open = 0;
        browser_win_id = -1;
        return;
    }
    
    // Check serial for incoming HTML text
    int new_data = 0;
    while (serial_received()) {
        char c = read_serial();
        if (page_len < MAX_BROWSER_TEXT - 1) {
            page_text[page_len++] = c;
            page_text[page_len] = '\0';
            new_data = 1;
        }
    }
    if (new_data) {
        loading = 0; // Got at least some data
    }
}

void open_browser_app() {
    if (browser_open && wm_is_open(browser_win_id)) { wm_raise(browser_win_id); return; }
    
    // Set initial text
    if (url_len == 0) {
        strcpy(url_buf, "google.com");
        url_len = 10;
        strcpy(page_text, "Mectov Mini-Browser v1.0\nType URL and press ENTER.");
        page_len = strlen(page_text);
    }
    
    browser_win_id = wm_open(50, 50, 600, 450, "Mectov Mini-Browser", browser_draw, browser_key, browser_tick, browser_mouse);
    browser_open = (browser_win_id >= 0);
}
