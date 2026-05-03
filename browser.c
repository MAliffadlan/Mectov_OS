#include "src/include/syscall.h"

typedef struct {
    int type;
    int x, y;
    int key;
} gui_event_t;

static char url_buf[128] = "example.com";
static int url_len = 11;
static int focused_url = 1;

#define MAX_PAGE 4096
static char page_text[MAX_PAGE];
static int page_len = 0;
static int loading = 0;
static int browser_state = 0; // 0=Idle, 1=DNS, 2=TCP connect, 3=Connected

static int my_strlen(const char* s) {
    int n = 0; while(s[n]) n++; return n;
}
static void my_strcpy(char* d, const char* s) {
    while(*s) *d++ = *s++; *d = '\0';
}
static void my_strcat(char* d, const char* s) {
    while(*d) d++;
    while(*s) *d++ = *s++;
    *d = '\0';
}

static void draw_browser(int wid) {
    int cw = 520, ch = 380;
    
    // White page background
    sys_draw_rect(wid, 0, 0, cw, ch, 0x00FFFFFF);
    
    // Address bar area (dark header)
    sys_draw_rect(wid, 0, 0, cw, 30, 0x00313244);
    sys_draw_text(wid, 8, 8, "URL:", 0x006C7086);
    
    // URL input field
    sys_draw_rect(wid, 40, 4, cw - 50, 22, focused_url ? 0x00FFFFFF : 0x00CCCCCC);
    sys_draw_text(wid, 44, 8, url_buf, 0x00111111);
    
    // Loading indicator
    if (loading) {
        sys_draw_rect(wid, cw - 20, 8, 10, 10, 0x00FF3333);
    }
    
    // Ring 3 badge
    sys_draw_text(wid, cw - 60, 34, "Ring 3", 0x00F9E2AF);
    
    // Draw page text
    int lx = 8, ly = 40;
    int line_len = 0;
    char line[128];
    int max_ch = (cw - 16) / 8 - 1;
    
    for (int i = 0; i < page_len; i++) {
        char c = page_text[i];
        if (c == '\n' || line_len >= max_ch) {
            line[line_len] = '\0';
            sys_draw_text(wid, lx, ly, line, 0x00111111);
            ly += 16;
            line_len = 0;
            if (c != '\n') line[line_len++] = c;
            if (ly > ch - 16) break;
        } else {
            if (c >= 32 && c <= 126) line[line_len++] = c;
        }
    }
    if (line_len > 0 && ly <= ch - 16) {
        line[line_len] = '\0';
        sys_draw_text(wid, lx, ly, line, 0x00111111);
    }
    
    sys_update_window(wid);
}

void _start() {
    int wid = sys_create_window(50, 50, 520, 380, "Browser (Ring 3)");
    if (wid < 0) sys_exit();
    
    my_strcpy(page_text, "Mectov Mini-Browser v2.0 [Ring 3]\nType URL and press ENTER.");
    page_len = my_strlen(page_text);
    
    draw_browser(wid);
    
    gui_event_t ev;
    int tick = 0;
    
    while (1) {
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 1) { // Paint
                draw_browser(wid);
            } else if (ev.type == 2) { // Key
                if (ev.key == 27) sys_exit(); // ESC
                
                if (focused_url) {
                    if (ev.key == '\b') {
                        if (url_len > 0) {
                            url_len--;
                            url_buf[url_len] = '\0';
                            draw_browser(wid);
                        }
                    } else if (ev.key == '\n') {
                        if (url_len > 0) {
                            // Start DNS resolution
                            page_len = 0;
                            my_strcpy(page_text, "Resolving DNS...\n");
                            page_len = my_strlen(page_text);
                            loading = 1;
                            focused_url = 0;
                            browser_state = 1;
                            sys_dns_resolve(url_buf);
                            draw_browser(wid);
                        }
                    } else if (ev.key >= 32 && ev.key <= 126 && url_len < 120) {
                        url_buf[url_len++] = (char)ev.key;
                        url_buf[url_len] = '\0';
                        draw_browser(wid);
                    }
                } else {
                    if (ev.key == ' ') {
                        focused_url = 1;
                        draw_browser(wid);
                    }
                }
            } else if (ev.type == 3) { // Mouse
                if (ev.key == 1) {
                    if (ev.y < 30) focused_url = 1;
                    else focused_url = 0;
                    draw_browser(wid);
                }
            }
        }
        
        // Poll network state machine
        if (browser_state > 0) {
            tick++;
            if (tick % 100 == 0) {
                net_status_t ns;
                sys_net_status(&ns);
                
                if (browser_state == 1) {
                    // Waiting for DNS
                    if (ns.dns_resolved) {
                        // DNS resolved!
                        my_strcpy(page_text, "DNS Resolved. Connecting...\n");
                        page_len = my_strlen(page_text);
                        browser_state = 2;
                        sys_tcp_connect(ns.dns_ip, 80);
                        draw_browser(wid);
                    }
                } else if (browser_state == 2) {
                    // Waiting for TCP connect
                    if (ns.tcp_state == 2) { // TCP_ESTABLISHED
                        browser_state = 3;
                        // Send HTTP GET
                        char req[256];
                        my_strcpy(req, "GET / HTTP/1.1\r\nHost: ");
                        my_strcat(req, url_buf);
                        my_strcat(req, "\r\nConnection: close\r\n\r\n");
                        sys_tcp_send(req, my_strlen(req));
                        
                        my_strcpy(page_text, "Connected! Waiting for response...\n");
                        page_len = my_strlen(page_text);
                        draw_browser(wid);
                    }
                } else if (browser_state == 3) {
                    // Check for incoming data
                    char rx_buf[1024];
                    int rx_len = sys_tcp_recv(rx_buf, 1024);
                    if (rx_len > 0) {
                        loading = 0;
                        // Append to page text
                        int copy = rx_len;
                        if (page_len + copy >= MAX_PAGE - 1)
                            copy = MAX_PAGE - 1 - page_len;
                        if (copy > 0) {
                            for (int i = 0; i < copy; i++)
                                page_text[page_len + i] = rx_buf[i];
                            page_len += copy;
                            page_text[page_len] = '\0';
                        }
                        draw_browser(wid);
                    }
                }
            }
        }
        
        sys_yield();
    }
}
