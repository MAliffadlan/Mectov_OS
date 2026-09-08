// apps/browser.c — Mectov Mini-Browser v3.0 [Ring 3] (v38.70)
//
// Real HTTP fetch + simple HTML rendering:
//   - URL parsing: [http://]host[:port][/path]. IP literals (a.b.c.d) skip
//     DNS and connect directly — deterministic in CI via QEMU slirp host
//     gateway 10.0.2.2; hostnames go through SYS_DNS_RESOLVE as before.
//   - HTTP/1.0 GET with Host + User-Agent (HTTP/1.0 => no chunked encoding,
//     server close = end of body).
//   - Response parsing: status line kept for the status bar, headers stripped
//     at the first CRLFCRLF, only the body is rendered. A response that does
//     not start with "HTTP/" is shown as plain text (simple test servers).
//   - HTML -> text: <script>/<style> dropped, block tags produce newlines,
//     common entities decoded, whitespace runs collapsed, <title> captured.
//   - Status bar: progress while loading, "HTTP <code> — <bytes> — <title>"
//     when done, error reasons on failure.

#include "src/include/syscall.h"

typedef struct {
    int type;
    int x, y;
    int key;
} gui_event_t;

#define CW 520
#define CH 380
#define URL_MAX 120
#define RAW_MAX 24576   // raw HTTP response
#define PAGE_MAX 16384  // rendered text
#define TITLE_MAX 64

static char url_buf[URL_MAX + 1] = "example.com";
static int url_len = 11;
static int focused_url = 1;

static char raw_buf[RAW_MAX];
static int raw_len = 0;
static char page_text[PAGE_MAX];
static int page_len = 0;
static int total_lines = 1;
static char page_title[TITLE_MAX] = "";

static int loading = 0;
static int browser_state = 0; // 0=Idle 1=DNS wait 2=TCP connect wait 3=Receiving
static int conn_id = -1;
static int scroll_offset = 0;
static char status_msg[128] = "Ready - type a URL and press ENTER";
// Actual client-area size as reported by the WM (event type 5). The window
// is win_cw x win_ch on screen, but the WM carves out a 20px titlebar + 1px frame on
// every side, so drawing at fixed win_cw/win_ch coordinates clips anything below
// client y = win_ch-22 (the status bar was invisible because of exactly this).
static int win_cw = CW - 2;
static int win_ch = CH - 22;
static uint32_t request_started_at = 0;
static const uint32_t REQUEST_TIMEOUT_MS = 15000;

// Parsed URL pieces
static char req_host[URL_MAX + 1];
static char req_path[URL_MAX + 1];
static int  req_port = 80;
static uint8_t req_ip[4];
static int  req_is_literal = 0;

// ---------------------------------------------------------------- helpers

static int my_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static void my_strcpy(char* d, const char* s) { while (*s) *d++ = *s++; *d = '\0'; }
static void my_strcat(char* d, const char* s) { while (*d) d++; while (*s) *d++ = *s++; *d = '\0'; }
static void my_memset(void* p, int v, int n) {
    unsigned char* b = (unsigned char*)p;
    while (n--) *b++ = (unsigned char)v;
}
static int my_itoa(int v, char* out) {
    char tmp[12]; int n = 0, m = 0;
    if (v < 0) { *out++ = '-'; v = -v; }
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) out[m++] = tmp[--n];
    out[m] = '\0';
    return m;
}
static int my_strcmp_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int ci_starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        char a = *s, b = *prefix;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b || a == '\0') return 0;
        s++; prefix++;
    }
    return 1;
}

// ------------------------------------------------------------- URL parsing

static int is_digit(char c) { return c >= '0' && c <= '9'; }

// Try to interpret s[0..len) as an IPv4 literal. Returns 1 and fills ip.
static int parse_ip4(const char* s, int len, uint8_t* ip) {
    int part = 0, pos = 0;
    if (len <= 0 || len > 15) return 0;
    for (part = 0; part < 4; part++) {
        int val = 0, digits = 0;
        while (pos < len && is_digit(s[pos])) {
            val = val * 10 + (s[pos] - '0');
            pos++; digits++;
            if (digits > 3) return 0;
        }
        if (digits == 0 || val > 255) return 0;
        ip[part] = (uint8_t)val;
        if (part < 3) {
            if (pos >= len || s[pos] != '.') return 0;
            pos++;
        }
    }
    return pos == len;
}

// Parse "url_buf" into req_host / req_port / req_path / req_ip.
// Returns 0 on success, -1 on a malformed URL.
static int parse_url(void) {
    const char* p = url_buf;
    int host_len = 0;
    req_port = 80;
    req_is_literal = 0;

    if (ci_starts_with(p, "http://")) p += 7;

    // host = up to ':' or '/'
    while (p[host_len] && p[host_len] != ':' && p[host_len] != '/') {
        if (host_len >= URL_MAX) return -1;
        host_len++;
    }
    if (host_len == 0) return -1;
    for (int i = 0; i < host_len; i++) req_host[i] = p[i];
    req_host[host_len] = '\0';

    // optional :port
    int idx = host_len;
    if (p[idx] == ':') {
        idx++;
        int port = 0, digits = 0;
        while (is_digit(p[idx])) {
            port = port * 10 + (p[idx] - '0');
            idx++; digits++;
            if (digits > 5 || port > 65535) return -1;
        }
        if (digits == 0) return -1;
        req_port = port;
    }

    // path = rest (default "/")
    if (p[idx] == '/') {
        int pl = 0;
        while (p[idx] && pl < URL_MAX) req_path[pl++] = p[idx++];
        req_path[pl] = '\0';
    } else {
        my_strcpy(req_path, "/");
    }

    if (parse_ip4(req_host, host_len, req_ip)) {
        req_is_literal = 1;
    }
    return 0;
}

// -------------------------------------------------------------- rendering

// Block-level tags: both open and close produce a line break.
static const char* const newline_tags[] = {
    "p", "div", "li", "tr", "ul", "ol", "table", "blockquote", "pre",
    "h1", "h2", "h3", "h4", "h5", "h6", "section", "header", "footer",
    "article", "aside", "nav", "form", "dl", "dt", "dd", 0
};

static void to_lower(char* s) {
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32;
}

static int tag_in_list(const char* tag) {
    for (int i = 0; newline_tags[i]; i++)
        if (my_strcmp_eq(tag, newline_tags[i])) return 1;
    return 0;
}

// Decode one entity at in[i] (in[i] == '&'). Appends to out, returns the
// number of input chars consumed (at least 1).
static int decode_entity(const char* in, int i, int in_len, char* out, int* out_len, int out_max) {
    struct { const char* name; char c; } ents[] = {
        { "amp;",  '&' }, { "lt;",   '<' }, { "gt;",   '>' },
        { "quot;", '"' }, { "apos;", '\'' }, { "nbsp;", ' ' },
        { "copy;", ' ' }, { "mdash;", '-' }, { "ndash;", '-' },
        { "hellip;", '.' }, { "rsquo;", '\'' }, { "lsquo;", '\'' },
        { "rdquo;", '"' }, { "ldquo;", '"' }, { 0, 0 }
    };
    for (int e = 0; ents[e].name; e++) {
        const char* n = ents[e].name;
        int j = i + 1, k = 0;
        while (n[k] && j < in_len && in[j] == n[k]) { j++; k++; }
        if (n[k] == '\0' && *out_len < out_max - 1) {
            out[(*out_len)++] = ents[e].c;
            return j - i;
        }
    }
    // numeric &#NN; / &#xHH;
    if (i + 2 < in_len && in[i + 1] == '#') {
        int j = i + 2, val = 0, digits = 0;
        int hex = (j < in_len && (in[j] == 'x' || in[j] == 'X'));
        if (hex) j++;
        while (j < in_len && digits < 5) {
            char c = in[j];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            val = val * (hex ? 16 : 10) + d;
            j++; digits++;
        }
        if (digits > 0 && j < in_len && in[j] == ';' && val >= 32 && val <= 126) {
            if (*out_len < out_max - 1) out[(*out_len)++] = (char)val;
            return j + 1 - i;
        }
    }
    if (*out_len < out_max - 1) out[(*out_len)++] = '&';
    return 1;
}

// Render an HTML document (or plain text) into page_text. Extracts <title>.
static void html_to_text(const char* in, int in_len) {
    my_memset(page_text, 0, PAGE_MAX);
    my_memset(page_title, 0, TITLE_MAX);
    page_len = 0;
    total_lines = 1;

    char tag[24];
    int in_tag = 0, tag_len = 0;
    int in_skip = 0;        // inside <script>/<style>
    int title_on = 0;
    int pending_space = 0;
    int last_nl = 1;        // treat start as after-newline (strip leading blanks)

    for (int i = 0; i < in_len; i++) {
        char c = in[i];

        if (in_tag) {
            if (c == '>') {
                tag[tag_len < 23 ? tag_len : 23] = '\0';
                // split closing slash
                const char* name = tag;
                int closing = 0;
                if (tag[0] == '/') { closing = 1; name = tag + 1; }
                char low[24];
                my_strcpy(low, name);
                to_lower(low);
                if (in_skip) {
                    if (closing && (my_strcmp_eq(low, "script") || my_strcmp_eq(low, "style")))
                        in_skip = 0;
                } else if (my_strcmp_eq(low, "script") || my_strcmp_eq(low, "style")) {
                    in_skip = 1;
                } else if (my_strcmp_eq(low, "br") || my_strcmp_eq(low, "hr")) {
                    if (!last_nl && page_len < PAGE_MAX - 1) { page_text[page_len++] = '\n'; last_nl = 1; total_lines++; }
                    pending_space = 0;
                } else if (tag_in_list(low)) {
                    if (!last_nl && page_len < PAGE_MAX - 1) { page_text[page_len++] = '\n'; last_nl = 1; total_lines++; }
                    pending_space = 0;
                } else if (my_strcmp_eq(low, "title")) {
                    title_on = !closing;
                }
                in_tag = 0; tag_len = 0;
            } else if (tag_len < 23) {
                tag[tag_len++] = c;
            }
            continue;
        }

        if (in_skip) {
            if (c == '<') { in_tag = 1; tag_len = 0; }
            continue;
        }

        if (c == '<') { in_tag = 1; tag_len = 0; pending_space = 0; continue; }
        if (c == '&') {
            i += decode_entity(in, i, in_len, page_text, &page_len, PAGE_MAX) - 1;
            last_nl = 0;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (title_on && c == ' ') {
                int tl = my_strlen(page_title);
                if (tl > 0 && page_title[tl - 1] != ' ' && tl < TITLE_MAX - 1)
                    page_title[tl] = ' ';
            }
            if (!last_nl) pending_space = 1;
            continue;
        }
        if (c < 32 || c > 126) continue;

        if (title_on) {
            int tl = my_strlen(page_title);
            if (tl < TITLE_MAX - 1) page_title[tl] = c;
        }
        if (page_len >= PAGE_MAX - 2) break;
        if (last_nl) {
            // trim leading spaces on a fresh line
        } else if (pending_space) {
            page_text[page_len++] = ' ';
            pending_space = 0;
        }
        page_text[page_len++] = c;
        last_nl = 0;
    }
    if (page_len < PAGE_MAX) page_text[page_len] = '\0';
    // trim trailing blank lines
    while (page_len > 0 && (page_text[page_len - 1] == '\n' || page_text[page_len - 1] == ' '))
        page_len--;
    page_text[page_len] = '\0';
    if (page_len == 0) { my_strcpy(page_text, "(empty page)"); page_len = my_strlen(page_text); }
}

// ------------------------------------------------------------ HTTP parsing

// Parse the raw response: strip headers, keep body, capture status code.
static void parse_response(void) {
    int code = 0;
    char code_str[8];

    if (raw_len > 5 && raw_buf[0] == 'H' && raw_buf[1] == 'T' && raw_buf[2] == 'T' &&
        raw_buf[3] == 'P' && raw_buf[4] == '/') {
        // "HTTP/1.x NNN reason"
        int i = 0;
        while (i < raw_len && raw_buf[i] != ' ') i++;
        i++;
        int d = 0;
        while (i < raw_len && raw_buf[i] >= '0' && raw_buf[i] <= '9' && d < 3) {
            code = code * 10 + (raw_buf[i] - '0');
            i++; d++;
        }
        // body starts after the first CRLFCRLF
        int body = -1;
        for (int j = 0; j < raw_len - 3; j++) {
            if (raw_buf[j] == '\r' && raw_buf[j+1] == '\n' &&
                raw_buf[j+2] == '\r' && raw_buf[j+3] == '\n') { body = j + 4; break; }
        }
        if (body < 0) body = raw_len; // malformed: show what we have
        html_to_text(raw_buf + body, raw_len - body);
    } else {
        html_to_text(raw_buf, raw_len);
        code = 0;
    }

    // status summary
    my_strcpy(status_msg, "");
    if (code > 0) {
        my_itoa(code, code_str);
        my_strcat(status_msg, "HTTP ");
        my_strcat(status_msg, code_str);
        my_strcat(status_msg, " - ");
    }
    char nbuf[12];
    my_itoa(raw_len, nbuf);
    my_strcat(status_msg, nbuf);
    my_strcat(status_msg, " bytes");
    if (page_title[0]) {
        my_strcat(status_msg, " - ");
        my_strcat(status_msg, page_title);
    }
    if (code >= 300 && code < 400)
        my_strcat(status_msg, " [redirect not followed]");
}

// ------------------------------------------------------------------ draw

static int visible_rows(void) {
    int v = (win_ch - 20 - 40) / 16;
    return v > 1 ? v : 1;
}

static void clamp_scroll(void) {
    int max = total_lines - visible_rows();
    if (max < 0) max = 0;
    if (scroll_offset > max) scroll_offset = max;
    if (scroll_offset < 0) scroll_offset = 0;
}

static void draw_browser(int wid) {
    // White page background
    sys_draw_rect(wid, 0, 0, win_cw, win_ch, 0x00FFFFFF);

    // Address bar (dark header)
    sys_draw_rect(wid, 0, 0, win_cw, 30, 0x00313244);
    sys_draw_text(wid, 8, 8, "URL:", 0x006C7086);
    sys_draw_rect(wid, 40, 4, win_cw - 50, 22, focused_url ? 0x00FFFFFF : 0x00CCCCCC);
    sys_draw_text(wid, 44, 8, url_buf, 0x00111111);
    if (loading) sys_draw_rect(wid, win_cw - 20, 8, 10, 10, 0x00FF3333);
    sys_draw_text(wid, win_cw - 60, 34, "Ring 3", 0x00F9E2AF);

    // Scrollbar
    sys_draw_rect(wid, win_cw - 12, 30, 12, win_ch - 30, 0x00E0E0E0);
    sys_draw_rect(wid, win_cw - 12, 30, 12, 12, 0x00C0C0C0);
    sys_draw_text(wid, win_cw - 10, 29, "^", 0x00111111);
    sys_draw_rect(wid, win_cw - 12, win_ch - 12, 12, 12, 0x00C0C0C0);
    sys_draw_text(wid, win_cw - 10, win_ch - 14, "v", 0x00111111);

    // Page text
    int lx = 8, ly = 40;
    int line_len = 0;
    char line[128];
    int max_ch = (win_cw - 28) / 8 - 1;
    int current_line = 0;

    for (int i = 0; i <= page_len; i++) {
        char c = (i < page_len) ? page_text[i] : '\n';
        if (c == '\n' || line_len >= max_ch) {
            line[line_len] = '\0';
            if (current_line >= scroll_offset) {
                if (line_len > 0) sys_draw_text(wid, lx, ly, line, 0x00111111);
                ly += 16;
                if (ly > win_ch - 20) break;
            }
            current_line++;
            line_len = 0;
            if (c != '\n' && i < page_len) line[line_len++] = c;
        } else if (c >= 32 && c <= 126) {
            line[line_len++] = c;
        }
    }

    // Status bar
    sys_draw_rect(wid, 0, win_ch - 16, win_cw - 12, 16, 0x00313244);
    sys_draw_text(wid, 6, win_ch - 13, status_msg, 0x00A6E3A1);

    sys_update_window(wid);
}

// ------------------------------------------------------------ state machine

static void set_status(const char* a, const char* b) {
    my_strcpy(status_msg, a);
    if (b) my_strcat(status_msg, b);
}

static void start_request(int wid) {
    raw_len = 0;
    page_len = 0;
    scroll_offset = 0;
    loading = 1;
    conn_id = -1;
    request_started_at = sys_get_ticks();

    if (parse_url() != 0) {
        loading = 0;
        set_status("Bad URL - use host[:port][/path]", 0);
        draw_browser(wid);
        return;
    }    if (req_is_literal) {
        conn_id = sys_tcp_connect(req_ip, req_port);
        if (conn_id < 0) {
            loading = 0;
            set_status("No free TCP connection slots", 0);
        } else {
            browser_state = 2;
            set_status("Connecting ", req_host);
        }
    } else {
        browser_state = 1;
        set_status("Resolving ", req_host);
        sys_dns_resolve(req_host);
    }
    draw_browser(wid);
}

static void send_request(void) {
    char req[512];
    my_strcpy(req, "GET ");
    my_strcat(req, req_path);
    my_strcat(req, " HTTP/1.0\r\nHost: ");
    my_strcat(req, req_host);
    if (req_port != 80) {
        char pb[8];
        my_strcat(req, ":");
        my_itoa(req_port, pb);
        my_strcat(req, pb);
    }
    my_strcat(req, "\r\nUser-Agent: MectovBrowser/3.0\r\nConnection: close\r\n\r\n");
    sys_tcp_send(conn_id, req, my_strlen(req));
    browser_state = 3;
    request_started_at = sys_get_ticks();
    set_status("Loading ", req_host);
}

static void finish_ok(int wid) {
    if (conn_id >= 0) { sys_tcp_close(conn_id); conn_id = -1; }
    loading = 0;
    browser_state = 0;
    focused_url = 0;
    parse_response();
    clamp_scroll();
    draw_browser(wid);
}

static void finish_err(int wid, const char* msg) {
    if (conn_id >= 0) { sys_tcp_close(conn_id); conn_id = -1; }
    loading = 0;
    browser_state = 0;
    focused_url = 1;
    my_strcpy(page_text, msg);
    page_len = my_strlen(page_text);
    total_lines = 1;
    set_status("Failed", 0);
    draw_browser(wid);
}

void _start() {
    url_len = my_strlen(url_buf);
    int wid = sys_create_window(50, 50, CW, CH, "Mini Browser");
    if (wid < 0) sys_exit();

    my_strcpy(page_text, "Mectov Mini-Browser v3.0 [Ring 3]\nReal HTTP fetch + simple HTML rendering.\nType a URL and press ENTER.");
    page_len = my_strlen(page_text);
    total_lines = 3;
    draw_browser(wid);

    gui_event_t ev;
    int tick = 0;

    while (1) {
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 1) { // Paint
                draw_browser(wid);
            } else if (ev.type == 2) { // Key
                if (ev.key == 27) { // ESC
                    if (conn_id >= 0) sys_tcp_close(conn_id);
                    sys_exit();
                }
                if (focused_url) {
                    if (ev.key == '\b') {
                        if (url_len > 0) { url_len--; url_buf[url_len] = '\0'; draw_browser(wid); }
                    } else if (ev.key == '\n') {
                        if (url_len > 0) start_request(wid);
                    } else if (ev.key >= 32 && ev.key <= 126 && url_len < URL_MAX - 1) {
                        url_buf[url_len++] = (char)ev.key;
                        url_buf[url_len] = '\0';
                        draw_browser(wid);
                    }
                } else {
                    if (ev.key == ' ') {
                        focused_url = 1;
                        draw_browser(wid);
                    } else if (ev.key == 'w' || ev.key == 'W') {
                        if (scroll_offset > 0) scroll_offset--;
                        draw_browser(wid);
                    } else if (ev.key == 's' || ev.key == 'S') {
                        scroll_offset++;
                        clamp_scroll();
                        draw_browser(wid);
                    }
                }
            } else if (ev.type == 3) { // Mouse
                if (ev.key == 1) { // Left click
                    if (ev.y < 30) {
                        focused_url = 1;
                    } else if (ev.x > win_cw - 12) {
                        focused_url = 0;
                        if (ev.y < 30 + 12) {
                            if (scroll_offset > 0) scroll_offset--;
                        } else if (ev.y > win_ch - 12) {
                            scroll_offset++;
                            clamp_scroll();
                        } else {
                            if (ev.y < win_ch / 2) { if (scroll_offset > 0) scroll_offset--; }
                            else { scroll_offset++; clamp_scroll(); }
                        }
                    } else {
                        focused_url = 0;
                    }
                    draw_browser(wid);
                }
            } else if (ev.type == 5) { // Client size (WM reports real cw/ch)
                if (ev.x > 40 && ev.y > 40) {
                    win_cw = ev.x;
                    win_ch = ev.y;
                    clamp_scroll();
                    draw_browser(wid);
                }
            } else if (ev.type == 4) { // Scroll wheel
                if (ev.key > 0) {
                    for (int s = 0; s < 3 && scroll_offset > 0; s++) scroll_offset--;
                } else if (ev.key < 0) {
                    scroll_offset += 3;
                    clamp_scroll();
                }
                draw_browser(wid);
            }
        }

        // Poll network state machine
        if (browser_state > 0) {
            tick++;
            if (sys_get_ticks() - request_started_at > REQUEST_TIMEOUT_MS) {
                finish_err(wid, "Request timed out.");
            } else if (tick % 100 == 0) {
                net_status_t ns;
                sys_net_status(&ns);

                if (browser_state == 1) {
                    if (ns.dns_resolved) {
                        conn_id = sys_tcp_connect(ns.dns_ip, req_port);
                        if (conn_id < 0) {
                            finish_err(wid, "No free TCP connection slots.");
                        } else {
                            browser_state = 2;
                            set_status("Connecting ", req_host);
                            draw_browser(wid);
                        }
                    }
                } else if (browser_state == 2) {
                    if (ns.tcp_state == 2) { // TCP_ESTABLISHED
                        send_request();
                        draw_browser(wid);
                    }
                } else if (browser_state == 3) {
                    char rx_buf[1024];
                    int rx_len = sys_tcp_recv(conn_id, rx_buf, 1024);
                    if (rx_len > 0) {
                        int copy = rx_len;
                        if (raw_len + copy >= RAW_MAX - 1) copy = RAW_MAX - 1 - raw_len;
                        if (copy > 0) {
                            for (int i = 0; i < copy; i++) raw_buf[raw_len + i] = rx_buf[i];
                            raw_len += copy;
                            raw_buf[raw_len] = '\0';
                        }
                        char nb[12];
                        my_strcpy(status_msg, "Loading ");
                        my_itoa(raw_len, nb);
                        my_strcat(status_msg, nb);
                        my_strcat(status_msg, " bytes");
                        draw_browser(wid);
                    } else if (rx_len == -1) {
                        finish_ok(wid);
                    } else if (rx_len == -2) {
                        finish_err(wid, "Connection lost.");
                    }
                }
            }
        }

        sys_yield();
    }
}
