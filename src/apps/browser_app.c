#include "../include/vga.h"
#include "../include/utils.h"
#include "../include/wm.h"
#include "../include/net.h"

int browser_win_id = -1;
int browser_open = 0;

static char url_buf[128] = "";
static int url_len = 0;
static int focused_url = 1;

#define MAX_BROWSER_TEXT 4096
static char page_text[MAX_BROWSER_TEXT];
static int page_len = 0;
static int loading = 0;
static int browser_state = 0; // 0=Idle, 1=DNS, 2=TCP connecting, 3=TCP connected

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
            if (c >= 32 && c <= 126) line[line_len++] = c;
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
                
                // Start TCP/IP flow!
                // 1. Resolve DNS
                strcpy(page_text, "Resolving DNS...\n");
                page_len = strlen(page_text);
                browser_state = 1;
                net_send_dns_query(url_buf);
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
    
    if (browser_state == 1) {
        if (dns_resolved) {
            char buf[64];
            strcpy(page_text, "DNS Resolved: ");
            page_len = strlen(page_text);
            p_int(dns_resolved_ip[0], 0); print(".", 0);
            p_int(dns_resolved_ip[1], 0); print(".", 0);
            p_int(dns_resolved_ip[2], 0); print(".", 0);
            p_int(dns_resolved_ip[3], 0); print("\n", 0);
            
            // Initiate TCP connection (Port 80)
            tcp_rx_len = 0;
            net_tcp_connect(dns_resolved_ip, 80);
            browser_state = 2;
        }
    } else if (browser_state == 2) {
        if (tcp_state == TCP_ESTABLISHED) {
            browser_state = 3;
            // Send HTTP GET
            char req[256];
            strcpy(req, "GET / HTTP/1.1\r\nHost: ");
            int rl = strlen(req);
            strcpy(req + rl, url_buf);
            rl += strlen(url_buf);
            strcpy(req + rl, "\r\nConnection: close\r\n\r\n");
            rl += strlen("\r\nConnection: close\r\n\r\n");
            
            net_tcp_send((uint8_t*)req, rl);
        }
    } else if (browser_state == 3) {
        if (tcp_rx_len > 0) {
            loading = 0; // Got data!
            // Append incoming TCP payload to page_text
            int copy_len = tcp_rx_len;
            if (page_len + copy_len >= MAX_BROWSER_TEXT - 1) {
                copy_len = MAX_BROWSER_TEXT - 1 - page_len;
            }
            if (copy_len > 0) {
                memcpy(page_text + page_len, tcp_rx_buf, copy_len);
                page_len += copy_len;
                page_text[page_len] = '\0';
                // Reset rx buffer
                tcp_rx_len = 0;
            }
        }
    }
}

void open_browser_app() {
    if (browser_open && wm_is_open(browser_win_id)) { wm_raise(browser_win_id); return; }
    
    // Set initial text
    if (url_len == 0) {
        strcpy(url_buf, "example.com");
        url_len = 11;
        strcpy(page_text, "Mectov Mini-Browser v1.0\nType URL and press ENTER.");
        page_len = strlen(page_text);
    }
    
    browser_win_id = wm_open(50, 50, 600, 450, "Mectov Mini-Browser", browser_draw, browser_key, browser_tick, browser_mouse);
    browser_open = (browser_win_id >= 0);
}
