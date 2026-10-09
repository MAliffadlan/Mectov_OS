// apps/browser.c — Mectov Mini-Browser v3.1 [Ring 3] (v38.163)
//
// Real HTTP/HTTPS fetch + simple HTML rendering:
//   - URL parsing: [http://|https://]host[:port][/path]. IP literals (a.b.c.d)
//     skip DNS and connect directly — deterministic in CI via QEMU slirp host
//     gateway 10.0.2.2; hostnames go through SYS_DNS_RESOLVE as before.
//     Only the scheme selects TLS: http:// is plain, https:// is TLS 1.3 with
//     the connection's SNI and certificate hostname both taken from the URL.
//     A bare `host:443/path` stays plain HTTP on port 443 — guessing TLS from
//     the port number would silently downgrade nothing and upgrade something
//     the user did not ask for.
//   - HTTP/1.0 GET with Host + User-Agent (HTTP/1.0 => no chunked encoding,
//     server close = end of body). Content-Length and Transfer-Encoding:
//     chunked from a 1.1 server are still honoured: Content-Length ends the
//     read as soon as the promised body arrives instead of waiting for the
//     peer to close, and a chunked body is de-chunked before rendering.
//   - TLS runs through apps/lib/tls (the same library scripts/tls_selftest.py
//     gates offline); the handshake is driven from the poll loop via
//     tls_step(), never by blocking, so the window keeps painting while it
//     completes.
//   - Response parsing: status line kept for the status bar, headers stripped
//     at the first CRLFCRLF, only the body is rendered. A response that does
//     not start with "HTTP/" is shown as plain text (simple test servers).
//   - HTML -> text: <script>/<style> dropped, block tags produce newlines,
//     common entities decoded, whitespace runs collapsed, <title> captured.
//   - Status bar: progress while loading, "HTTP <code> — <bytes> — <title>"
//     when done, error reasons on failure.

#include "src/include/syscall.h"
#include "lib/tls/tls.h"

typedef struct {
    int type;
    int x, y;
    int key;
} gui_event_t;

#define CW 520
#define CH 400   // +20 for the bottom tab strip (URL/page/status keep coordinates)
#define URL_MAX 120
#define RAW_MAX 24576   // raw HTTP response (single in-flight fetch, global)
#define PAGE_MAX 16384  // rendered text, per tab
#define TITLE_MAX 64
#define MAX_TABS 4
#define HIST_MAX 10
#define TAB_H 20     // bottom tab strip height (above the status bar)
#define BM_FILE "/ext2/bookmarks.txt"

// Per-tab state. Network/TLS framing (raw_buf, conn_id, tls, req_*) stays
// global: one in-flight fetch at a time (tls_conn_t is far too big x4).
// Switching tabs mid-load cancels the load — deterministic, no cross-tab
// connection confusion.
typedef struct {
    char url_buf[URL_MAX + 1];
    int url_len;
    int focused_url;
    char page_text[PAGE_MAX];
    int page_len;
    int total_lines;
    char page_title[TITLE_MAX];
    int scroll_offset;
    char status_msg[128];
    char hist[HIST_MAX][URL_MAX + 1];
    int hist_len;   // entries used
    int hist_pos;   // index of the current page (-1 = none yet)
    int used;
} tab_t;

static tab_t tabs[MAX_TABS];
static int cur_tab = 0;
#define T (&tabs[cur_tab])

static char raw_buf[RAW_MAX];
static int raw_len = 0;

static int loading = 0;
static int browser_state = 0; // 0=Idle 1=DNS wait 2=TCP connect wait 3=Receiving
static int conn_id = -1;
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

// ---- TLS (v38.163). `req_tls` is what the URL asked for; `using_tls` is what
// this request is actually doing, because the two differ for the whole time a
// request is in flight and every branch below keys on `using_tls`.
static tls_conn_t tls;
static int req_tls = 0;
static int using_tls = 0;

// ---- Response framing, sniffed from the headers once they are complete.
static int body_start = -1;     // offset of the first body byte, -1 until known
static int body_expected = -1;  // Content-Length, or -1 when close-delimited
static int body_chunked = 0;    // Transfer-Encoding: chunked
static int body_decoded_len = 0; // body bytes actually rendered (post de-chunk)

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

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// ------------------------------------------------------- TLS transport glue
//
// apps/lib/tls talks to the network only through these two callbacks, so the
// browser decides when the TLS engine is allowed to touch a socket — and it is
// the poll loop, never the engine itself. The return conventions are the
// engine's: send returns bytes accepted (0 = try later, <0 = error) and recv
// returns >0 bytes, 0 = nothing yet, <0 = closed or error.
static int tls_send_cb(void* ctx, const uint8_t* buf, uint32_t len) {
    (void)ctx;
    if (conn_id < 0) return -1;
    // net_tcp_send() clamps a call at 1400 bytes and returns what it took, so
    // a short write is normal here and tls_flush() simply calls again.
    int n = sys_tcp_send(conn_id, buf, (int)len);
    return n < 0 ? -1 : n;
}

static int tls_recv_cb(void* ctx, uint8_t* buf, uint32_t len) {
    (void)ctx;
    if (conn_id < 0) return -1;
    // -1 closed, -2 lost: both are "no more bytes, ever" to the engine.
    int n = sys_tcp_recv(conn_id, buf, (int)len);
    return n < 0 ? -1 : n;
}

// Milestones go to the serial log, which is what a gate reads: the handshake
// outcome is otherwise only visible as a status bar on a screenshot.
static void tls_log_cb(void* ctx, const char* msg) {
    (void)ctx;
    sys_print("[TLS] ", 0x0E);
    sys_print(msg, 0x0E);
    sys_print("\n", 0x0E);
}

static const char* tls_err_text(int rc) {
    switch (rc) {
        case TLS_ERR_IO:          return "TLS transport closed";
        case TLS_ERR_PROTOCOL:    return "TLS protocol error";
        case TLS_ERR_VERIFY:      return "TLS certificate not trusted";
        case TLS_ERR_UNSUPPORTED: return "TLS suite not offered by server";
        case TLS_ERR_ENTROPY:     return "TLS aborted: no entropy";
        case TLS_ERR_OVERFLOW:    return "TLS message too large";
        default:                  return "TLS handshake failed";
    }
}

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

// Parse "T->url_buf" into req_host / req_port / req_path / req_ip.
// Returns 0 on success, -1 on a malformed URL.
static int parse_url(void) {
    const char* p = T->url_buf;
    int host_len = 0;
    req_port = 80;
    req_is_literal = 0;
    req_tls = 0;

    if (ci_starts_with(p, "https://")) { req_tls = 1; req_port = 443; p += 8; }
    else if (ci_starts_with(p, "http://")) p += 7;

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

// Render an HTML document (or plain text) into T->page_text. Extracts <title>.
static void html_to_text(const char* in, int in_len) {
    my_memset(T->page_text, 0, PAGE_MAX);
    my_memset(T->page_title, 0, TITLE_MAX);
    T->page_len = 0;
    T->total_lines = 1;

    char tag[24];
    int in_tag = 0, tag_len = 0;
    int in_skip = 0;        // inside <script>/<style>
    int title_on = 0;
    int pending_space = 0;
    int last_nl = 1;        // treat start as after-newline (strip leading blanks)
    int saw_tag = 0;        // any '<' seen? plain-text input keeps its newlines

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
                    if (!last_nl && T->page_len < PAGE_MAX - 1) { T->page_text[T->page_len++] = '\n'; last_nl = 1; T->total_lines++; }
                    pending_space = 0;
                } else if (tag_in_list(low)) {
                    if (!last_nl && T->page_len < PAGE_MAX - 1) { T->page_text[T->page_len++] = '\n'; last_nl = 1; T->total_lines++; }
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
            if (c == '<') { in_tag = 1; tag_len = 0; saw_tag = 1; }
            continue;
        }

        if (c == '<') { in_tag = 1; tag_len = 0; pending_space = 0; saw_tag = 1; continue; }
        if (c == '&') {
            i += decode_entity(in, i, in_len, T->page_text, &T->page_len, PAGE_MAX) - 1;
            last_nl = 0;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (title_on && c == ' ') {
                int tl = my_strlen(T->page_title);
                if (tl > 0 && T->page_title[tl - 1] != ' ' && tl < TITLE_MAX - 1)
                    T->page_title[tl] = ' ';
            }
            if (c == '\n' && !saw_tag) {
                // Plain-text input (no HTML tags seen — e.g. the Web Gateway
                // Proxy's line-structured replies): preserve real line breaks
                // instead of collapsing them into one paragraph.
                if (!last_nl && T->page_len < PAGE_MAX - 1) {
                    T->page_text[T->page_len++] = '\n';
                    last_nl = 1;
                    T->total_lines++;
                }
                pending_space = 0;
            } else if (!last_nl) {
                pending_space = 1;
            }
            continue;
        }
        if (c < 32 || c > 126) continue;

        if (title_on) {
            int tl = my_strlen(T->page_title);
            if (tl < TITLE_MAX - 1) T->page_title[tl] = c;
        }
        if (T->page_len >= PAGE_MAX - 2) break;
        if (last_nl) {
            // trim leading spaces on a fresh line
        } else if (pending_space) {
            T->page_text[T->page_len++] = ' ';
            pending_space = 0;
        }
        T->page_text[T->page_len++] = c;
        last_nl = 0;
    }
    if (T->page_len < PAGE_MAX) T->page_text[T->page_len] = '\0';
    // trim trailing blank lines
    while (T->page_len > 0 && (T->page_text[T->page_len - 1] == '\n' || T->page_text[T->page_len - 1] == ' '))
        T->page_len--;
    T->page_text[T->page_len] = '\0';
    if (T->page_len == 0) { my_strcpy(T->page_text, "(empty page)"); T->page_len = my_strlen(T->page_text); }
}

// ------------------------------------------------------------ HTTP parsing

// Re-scan the response head for framing. Runs after every read, so it is a
// no-op once the body's start is known.
static void sniff_headers(void) {
    if (body_start >= 0) return;
    if (raw_len < 5 || !ci_starts_with(raw_buf, "HTTP/")) return;

    int hdr_end = -1;
    for (int j = 0; j < raw_len - 3; j++) {
        if (raw_buf[j] == '\r' && raw_buf[j+1] == '\n' &&
            raw_buf[j+2] == '\r' && raw_buf[j+3] == '\n') { hdr_end = j + 4; break; }
    }
    if (hdr_end < 0) return;      // headers are still arriving
    body_start = hdr_end;

    int line = 0;
    while (line < hdr_end) {
        int eol = line;
        while (eol < hdr_end && raw_buf[eol] != '\r' && raw_buf[eol] != '\n') eol++;
        const char* h = raw_buf + line;
        int hl = eol - line;

        if (hl >= 15 && ci_starts_with(h, "content-length:")) {
            int i = 15, v = 0, d = 0;
            while (i < hl && h[i] == ' ') i++;
            while (i < hl && is_digit(h[i]) && d <= 9) { v = v * 10 + (h[i] - '0'); i++; d++; }
            if (d > 0) body_expected = v;
        } else if (hl >= 18 && ci_starts_with(h, "transfer-encoding:")) {
            for (int i = 18; i + 7 <= hl; i++) {
                if (ci_starts_with(h + i, "chunked")) { body_chunked = 1; break; }
            }
        }

        while (line < hdr_end && raw_buf[line] != '\n') line++;
        line++;
    }
}

// Walk a chunked body at raw_buf[from..end). Copies the decoded bytes to `dst`
// unless dst is NULL, in which case it only measures. Returns 1 once the
// terminating zero-length chunk has been seen, 0 while the stream is still
// incomplete or is not a chunk stream at all, and stores the decoded length in
// *out_len. Writing to `dst = raw_buf + from` is safe: the decoded prefix is
// never longer than the encoded prefix it came from, so no unread byte is
// overwritten.
static int chunk_walk(int from, int end, char* dst, int* out_len) {
    int r = from, w = 0;
    for (;;) {
        while (r + 1 < end && raw_buf[r] == '\r' && raw_buf[r+1] == '\n') r += 2;
        if (r >= end) break;

        int sz = 0, digits = 0;
        while (r < end) {
            int d = hexval(raw_buf[r]);
            if (d < 0) break;
            sz = sz * 16 + d; r++; digits++;
            if (digits > 8) { if (out_len) *out_len = w; return 0; }
        }
        if (digits == 0) { if (out_len) *out_len = w; return 0; }

        while (r < end && raw_buf[r] != '\n') r++;   // chunk extensions
        if (r >= end) break;
        r++;

        if (sz == 0) { if (out_len) *out_len = w; return 1; }
        if (r + sz > end) {                          // truncated final chunk
            if (dst) for (int i = 0; i < end - r; i++) dst[w + i] = raw_buf[r + i];
            w += end - r;
            break;
        }
        if (dst) for (int i = 0; i < sz; i++) dst[w + i] = raw_buf[r + i];
        w += sz;
        r += sz;
    }
    if (out_len) *out_len = w;
    return 0;
}

// Is the body complete? Content-Length lets the read stop as soon as the
// promised bytes have arrived instead of waiting for the peer to hang up, and
// a chunked body is complete at its terminating zero-length chunk. Anything
// else is delimited by the close that Connection: close asks for.
static int response_complete(void) {
    sniff_headers();
    if (body_start < 0) return 0;
    if (body_chunked) return chunk_walk(body_start, raw_len, 0, 0);
    if (body_expected >= 0) return raw_len - body_start >= body_expected;
    return 0;
}

// Parse the raw response: strip headers, keep body, capture status code.
static void parse_response(void) {
    int code = 0;
    char code_str[8];

    sniff_headers();

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
        if (body_chunked) {
            // De-chunk in place, then render the decoded bytes; a response that
            // is still truncated simply renders what arrived.
            int dec = 0;
            chunk_walk(body, raw_len, raw_buf + body, &dec);
            body_decoded_len = dec;
            html_to_text(raw_buf + body, dec);
        } else {
            int blen = raw_len - body;
            if (body_expected >= 0 && body_expected < blen) blen = body_expected;
            body_decoded_len = blen;
            html_to_text(raw_buf + body, blen);
        }
    } else {
        html_to_text(raw_buf, raw_len);
        code = 0;
    }

    // Serial summary: the suites assert on this instead of on pixels, because
    // a page that rendered from a cached or downgraded reply looks identical.
    {
        char nb[12];
        sys_print("[BROWSER] done tls=", 0x0A);
        sys_print(using_tls ? "1" : "0", 0x0A);
        sys_print(" code=", 0x0A);
        my_itoa(code, nb);
        sys_print(nb, 0x0A);
        sys_print(" bytes=", 0x0A);
        my_itoa(raw_len, nb);
        sys_print(nb, 0x0A);
        // Wire bytes and rendered body bytes differ by the header block and,
        // for a chunked reply, by the chunk framing -- which is exactly how a
        // suite can tell de-chunking happened instead of guessing at pixels.
        sys_print(" body=", 0x0A);
        my_itoa(body_decoded_len, nb);
        sys_print(nb, 0x0A);
        sys_print(" host=", 0x0A);
        sys_print(req_host, 0x0A);
        sys_print("\n", 0x0A);
    }

    // status summary
    my_strcpy(T->status_msg, "");
    if (code > 0) {
        my_itoa(code, code_str);
        my_strcat(T->status_msg, "HTTP ");
        my_strcat(T->status_msg, code_str);
        my_strcat(T->status_msg, " - ");
    }
    char nbuf[12];
    my_itoa(raw_len, nbuf);
    my_strcat(T->status_msg, nbuf);
    my_strcat(T->status_msg, " bytes");
    if (using_tls) {
        // Which suite was negotiated belongs on screen: "it loaded" does not
        // tell you whether it loaded over TLS, or over which cipher.
        my_strcat(T->status_msg, " - ");
        my_strcat(T->status_msg, tls_cipher_name(tls_cipher_id(&tls)));
    }
    if (T->page_title[0]) {
        my_strcat(T->status_msg, " - ");
        my_strcat(T->status_msg, T->page_title);
    }
    if (code >= 300 && code < 400)
        my_strcat(T->status_msg, " [redirect not followed]");
}

// ------------------------------------------------------- tabs + history

static void set_status(const char* a, const char* b);
static void start_request(int wid);

static int tab_used_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_TABS; i++) if (tabs[i].used) n++;
    return n;
}

static void tab_fresh(int i) {
    my_memset((void*)&tabs[i], 0, (int)sizeof(tab_t));
    tabs[i].used = 1;
    tabs[i].focused_url = 1;
    tabs[i].hist_pos = -1;
    my_strcpy(tabs[i].status_msg, "Ready - type a URL and press ENTER");
    my_strcpy(tabs[i].page_text, "(new tab)");
    tabs[i].page_len = my_strlen(tabs[i].page_text);
    tabs[i].total_lines = 1;
}

// One in-flight fetch at a time (global net state): leaving a tab mid-load
// cancels it instead of stranding a connection no pump will ever drain.
static void cancel_load(const char* why) {
    if (conn_id >= 0) { sys_tcp_close(conn_id); conn_id = -1; }
    loading = 0;
    browser_state = 0;
    if (why) set_status(why, 0);
}

static void tab_switch(int i) {
    if (i < 0 || i >= MAX_TABS || !tabs[i].used || i == cur_tab) return;
    if (loading) cancel_load("Load cancelled (tab switch)");
    cur_tab = i;
    sys_print("[BROWSER] tab switch=", 0x0A);
    char nb[12];
    my_itoa(i, nb);
    sys_print(nb, 0x0A);
    sys_print("\n", 0x0A);
}

static void tab_new(void) {
    for (int i = 0; i < MAX_TABS; i++) {
        if (!tabs[i].used) {
            if (loading) cancel_load("Load cancelled (new tab)");
            tab_fresh(i);
            cur_tab = i;
            sys_print("[BROWSER] tab new=", 0x0A);
            char nb[12];
            my_itoa(i, nb);
            sys_print(nb, 0x0A);
            sys_print(" count=", 0x0A);
            my_itoa(tab_used_count(), nb);
            sys_print(nb, 0x0A);
            sys_print("\n", 0x0A);
            return;
        }
    }
    set_status("Tab limit (4)", 0);
}

static void tab_close(int i) {
    if (i < 0 || i >= MAX_TABS || !tabs[i].used) return;
    if (tab_used_count() <= 1) { tab_fresh(i); cur_tab = i; return; }
    if (loading && i == cur_tab) cancel_load(0);
    tabs[i].used = 0;
    if (i == cur_tab) {
        for (int j = 0; j < MAX_TABS; j++) {
            if (tabs[j].used) { cur_tab = j; break; }
        }
    }
    sys_print("[BROWSER] tab close=", 0x0A);
    {
        char nb[12];
        my_itoa(i, nb);
        sys_print(nb, 0x0A);
        sys_print(" count=", 0x0A);
        my_itoa(tab_used_count(), nb);
        sys_print(nb, 0x0A);
        sys_print("\n", 0x0A);
    }
}

// History: plain URL stack per tab. New navigations truncate any forward
// entries; back/forward reload without pushing (no duplicates, no loops).
static void hist_push(const char* url) {
    tab_t* t = T;
    if (t->hist_pos >= 0 && my_strcmp_eq(t->hist[t->hist_pos], url)) return;
    if (t->hist_pos < t->hist_len - 1) t->hist_len = t->hist_pos + 1;
    if (t->hist_len >= HIST_MAX) {
        for (int i = 1; i < HIST_MAX; i++) my_strcpy(t->hist[i - 1], t->hist[i]);
        t->hist_len = HIST_MAX - 1;
    }
    my_strcpy(t->hist[t->hist_len], url);
    t->hist_len++;
    t->hist_pos = t->hist_len - 1;
}

static void nav_to_url(int wid, const char* url, int push) {
    my_strcpy(T->url_buf, url);
    T->url_len = my_strlen(T->url_buf);
    if (push) hist_push(url);
    start_request(wid);
}

static void hist_back(int wid) {
    if (T->hist_pos > 0) {
        if (loading) cancel_load(0);
        T->hist_pos--;
        nav_to_url(wid, T->hist[T->hist_pos], 0);
    }
}

static void hist_forward(int wid) {
    if (T->hist_pos >= 0 && T->hist_pos < T->hist_len - 1) {
        if (loading) cancel_load(0);
        T->hist_pos++;
        nav_to_url(wid, T->hist[T->hist_pos], 0);
    }
}

static void bookmark_save(int wid) {
    (void)wid;
    if (T->url_len == 0) return;
    sys_create_file(BM_FILE);
    int fd = sys_open_mode(BM_FILE, O_APPEND);
    if (fd < 0) { set_status("Bookmark failed", 0); return; }
    sys_write(fd, T->url_buf, T->url_len);
    sys_write(fd, "\n", 1);
    sys_close(fd);
    set_status("Bookmarked ", T->url_buf);
    sys_print("[BROWSER] bookmark ", 0x0A);
    sys_print(T->url_buf, 0x0A);
    sys_print("\n", 0x0A);
}

// Tab strip click: back/forward/bookmark buttons, tab switch/close, new.
// Geometry mirrors the strip drawn in draw_browser (same x constants).
// Returns 1 when the click landed on a strip control (consumed).
static int tab_strip_click(int wid, int mx, int my) {
    int strip_y = win_ch - 16 - TAB_H;
    if (my < strip_y || my >= win_ch - 16) return 0;
    if (mx >= 4 && mx < 34) { hist_back(wid); return 1; }
    if (mx >= 34 && mx < 64) { hist_forward(wid); return 1; }
    if (mx >= 64 && mx < 94) { bookmark_save(wid); return 1; }
    int tx = 94;
    for (int i = 0; i < MAX_TABS; i++) {
        if (!tabs[i].used) continue;
        if (mx >= tx && mx < tx + 96) {
            if (mx >= tx + 96 - 16) tab_close(i);
            else tab_switch(i);
            return 1;
        }
        tx += 100;
    }
    if (tab_used_count() < MAX_TABS && mx >= tx && mx < tx + 28) {
        tab_new();
        return 1;
    }
    return 0;
}

// ------------------------------------------------------------------ draw

static int visible_rows(void) {
    int v = (win_ch - 20 - TAB_H - 40) / 16;
    return v > 1 ? v : 1;
}

static void clamp_scroll(void) {
    int max = T->total_lines - visible_rows();
    if (max < 0) max = 0;
    if (T->scroll_offset > max) T->scroll_offset = max;
    if (T->scroll_offset < 0) T->scroll_offset = 0;
}

static void draw_browser(int wid) {
    // White page background
    sys_draw_rect(wid, 0, 0, win_cw, win_ch, 0x00FFFFFF);

    // Address bar (dark header)
    sys_draw_rect(wid, 0, 0, win_cw, 30, 0x00313244);
    sys_draw_text(wid, 8, 8, "URL:", 0x006C7086);
    sys_draw_rect(wid, 40, 4, win_cw - 50, 22, T->focused_url ? 0x00FFFFFF : 0x00CCCCCC);
    sys_draw_text(wid, 44, 8, T->url_buf, 0x00111111);
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

    for (int i = 0; i <= T->page_len; i++) {
        char c = (i < T->page_len) ? T->page_text[i] : '\n';
        if (c == '\n' || line_len >= max_ch) {
            line[line_len] = '\0';
            if (current_line >= T->scroll_offset) {
                if (line_len > 0) sys_draw_text(wid, lx, ly, line, 0x00111111);
                ly += 16;
                if (ly > win_ch - 20 - TAB_H) break;
            }
            current_line++;
            line_len = 0;
            if (c != '\n' && i < T->page_len) line[line_len++] = c;
        } else if (c >= 32 && c <= 126) {
            line[line_len++] = c;
        }
    }

    // Tab strip (above the status bar): back/forward/bookmark buttons,
    // one header per tab (x = close), "+" for a new tab.
    {
        int strip_y = win_ch - 16 - TAB_H;
        sys_draw_rect(wid, 0, strip_y, win_cw, TAB_H, 0x00313244);
        int can_back = (T->hist_pos > 0);
        int can_fwd = (T->hist_pos >= 0 && T->hist_pos < T->hist_len - 1);
        sys_draw_text(wid, 4 + 8, strip_y + 3, "<", can_back ? 0x00FFFFFF : 0x00666666);
        sys_draw_text(wid, 34 + 8, strip_y + 3, ">", can_fwd ? 0x00FFFFFF : 0x00666666);
        sys_draw_text(wid, 64 + 8, strip_y + 3, "*", 0x00FFD94D);
        int tx = 94;
        for (int i = 0; i < MAX_TABS; i++) {
            if (!tabs[i].used) continue;
            sys_draw_rect(wid, tx, strip_y + 2, 96, TAB_H - 4,
                          (i == cur_tab) ? 0x00FFFFFF : 0x00CCCCCC);
            char tlabel[12];
            const char* tsrc = tabs[i].page_title[0] ? tabs[i].page_title : tabs[i].url_buf;
            int li = 0;
            while (tsrc[li] && li < 10) { tlabel[li] = tsrc[li]; li++; }
            tlabel[li] = '\0';
            sys_draw_text(wid, tx + 4, strip_y + 4, tlabel, 0x00111111);
            sys_draw_text(wid, tx + 96 - 14, strip_y + 4, "x", 0x00111111);
            tx += 100;
        }
        if (tab_used_count() < MAX_TABS)
            sys_draw_text(wid, tx + 6, strip_y + 4, "+", 0x00FFFFFF);
    }

    // Status bar
    sys_draw_rect(wid, 0, win_ch - 16, win_cw - 12, 16, 0x00313244);
    sys_draw_text(wid, 6, win_ch - 13, T->status_msg, 0x00A6E3A1);

    sys_update_window(wid);
}

// ------------------------------------------------------------ state machine

static void set_status(const char* a, const char* b) {
    my_strcpy(T->status_msg, a);
    if (b) my_strcat(T->status_msg, b);
}

static void start_request(int wid) {
    raw_len = 0;
    T->page_len = 0;
    T->scroll_offset = 0;
    loading = 1;
    conn_id = -1;
    body_start = -1;
    body_expected = -1;
    body_chunked = 0;
    body_decoded_len = 0;
    request_started_at = sys_get_ticks();

    if (parse_url() != 0) {
        loading = 0;
        set_status("Bad URL - use host[:port][/path]", 0);
        draw_browser(wid);
        return;
    }

    using_tls = req_tls;
    if (using_tls) {
        // The URL's host is both the SNI name and the name the leaf
        // certificate must match; there is no way to make those differ.
        tls_init(&tls, req_host);
        tls_set_transport(&tls, tls_send_cb, tls_recv_cb, 0, tls_log_cb);
    }

    if (req_is_literal) {
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
    // The port is omitted when it is the scheme's default: a Host header of
    // "example.com:443" is legal but it makes some virtual hosts answer a
    // different site than the certificate was issued for.
    int default_port = using_tls ? 443 : 80;
    if (req_port != default_port) {
        char pb[8];
        my_strcat(req, ":");
        my_itoa(req_port, pb);
        my_strcat(req, pb);
    }
    my_strcat(req, "\r\nUser-Agent: MectovBrowser/3.1\r\nConnection: close\r\n\r\n");

    int len = my_strlen(req);
    if (using_tls) {
        // tls_write() encrypts and queues; tls_flush() hands the ciphertext to
        // the transport in whatever sizes the socket accepts.
        if (tls_write(&tls, req, (uint32_t)len) != TLS_OK) {
            browser_state = 0;
            return;
        }
        tls_flush(&tls);
    } else {
        sys_tcp_send(conn_id, req, len);
    }
    browser_state = 3;
    request_started_at = sys_get_ticks();
    set_status("Loading ", req_host);
}

static void finish_ok(int wid) {
    if (using_tls) tls_send_close_notify(&tls);
    if (conn_id >= 0) { sys_tcp_close(conn_id); conn_id = -1; }
    loading = 0;
    browser_state = 0;
    T->focused_url = 0;
    parse_response();
    clamp_scroll();
    draw_browser(wid);
}

static void finish_err(int wid, const char* msg) {
    if (using_tls) tls_send_close_notify(&tls);
    if (conn_id >= 0) { sys_tcp_close(conn_id); conn_id = -1; }
    loading = 0;
    browser_state = 0;
    T->focused_url = 1;
    my_strcpy(T->page_text, msg);
    T->page_len = my_strlen(T->page_text);
    T->total_lines = 1;
    set_status("Failed", 0);
    draw_browser(wid);
}

void _start() {
    // Tab 0 is the initial tab (replaces the old file-scope initializers).
    tabs[0].used = 1;
    my_strcpy(tabs[0].url_buf, "example.com");
    tabs[0].url_len = my_strlen(tabs[0].url_buf);
    tabs[0].focused_url = 1;
    tabs[0].hist_len = 0;
    tabs[0].hist_pos = -1;
    my_strcpy(tabs[0].status_msg, "Ready - type a URL and press ENTER");
    int wid = sys_create_window(50, 50, CW, CH, "Mini Browser");
    if (wid < 0) sys_exit();

    my_strcpy(T->page_text, "Mectov Mini-Browser v3.0 [Ring 3]\nReal HTTP fetch + simple HTML rendering.\nType a URL and press ENTER.");
    T->page_len = my_strlen(T->page_text);
    T->total_lines = 3;
    draw_browser(wid);

    gui_event_t ev;
    uint32_t last_net_poll = 0;   // ms-gated network polling (see below)

    while (1) {
        while (sys_get_event(wid, &ev)) {
            if (ev.type == 1) { // Paint
                draw_browser(wid);
            } else if (ev.type == 2) { // Key
                if (ev.key == 27) { // ESC
                    if (conn_id >= 0) sys_tcp_close(conn_id);
                    sys_exit();
                }
                if (T->focused_url) {
                    if (ev.key == '\b') {
                        if (T->url_len > 0) { T->url_len--; T->url_buf[T->url_len] = '\0'; draw_browser(wid); }
                    } else if (ev.key == '\n') {
                        if (T->url_len > 0) nav_to_url(wid, T->url_buf, 1);
                    } else if (ev.key >= 32 && ev.key <= 126 && T->url_len < URL_MAX - 1) {
                        T->url_buf[T->url_len++] = (char)ev.key;
                        T->url_buf[T->url_len] = '\0';
                        draw_browser(wid);
                    }
                } else {
                    if (ev.key == ' ') {
                        T->focused_url = 1;
                        draw_browser(wid);
                    } else if (ev.key == 'w' || ev.key == 'W') {
                        if (T->scroll_offset > 0) T->scroll_offset--;
                        draw_browser(wid);
                    } else if (ev.key == 's' || ev.key == 'S') {
                        T->scroll_offset++;
                        clamp_scroll();
                        draw_browser(wid);
                    }
                }
            } else if (ev.type == 3) { // Mouse
                if (ev.key == 1) { // Left click
                    if (tab_strip_click(wid, ev.x, ev.y)) {
                        draw_browser(wid);
                    } else if (ev.y < 30) {
                        T->focused_url = 1;
                    } else if (ev.x > win_cw - 12) {
                        T->focused_url = 0;
                        if (ev.y < 30 + 12) {
                            if (T->scroll_offset > 0) T->scroll_offset--;
                        } else if (ev.y > win_ch - 12) {
                            T->scroll_offset++;
                            clamp_scroll();
                        } else {
                            if (ev.y < win_ch / 2) { if (T->scroll_offset > 0) T->scroll_offset--; }
                            else { T->scroll_offset++; clamp_scroll(); }
                        }
                    } else {
                        T->focused_url = 0;
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
                    for (int s = 0; s < 3 && T->scroll_offset > 0; s++) T->scroll_offset--;
                } else if (ev.key < 0) {
                    T->scroll_offset += 3;
                    clamp_scroll();
                }
                draw_browser(wid);
            }
        }

        // Poll network state machine. Time-based (not iteration-counted):
        // loop iterations now track wakeups (~100/s), so a tick%N gate
        // would poll slower and slower the idler the loop gets (v38.83).
        if (browser_state > 0) {
            uint32_t now = sys_get_ticks();
            if (now - request_started_at > REQUEST_TIMEOUT_MS) {
                finish_err(wid, "Request timed out.");
            } else if (now - last_net_poll >= 50) {
                last_net_poll = now;
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
                        if (using_tls) {
                            // The handshake is driven from here, one step per
                            // poll, so the window keeps painting while it runs.
                            browser_state = 4;
                            set_status("TLS handshake ", req_host);
                        } else {
                            send_request();
                        }
                        draw_browser(wid);
                    }
                } else if (browser_state == 4) {
                    int rc = tls_step(&tls);
                    if (rc == TLS_OK) {
                        // Serial marker: a page rendering over HTTPS is
                        // indistinguishable on screen from one rendering over
                        // HTTP, so the suites read the negotiated suite here.
                        sys_print("[BROWSER] tls-ok cipher=", 0x0A);
                        sys_print(tls_cipher_name(tls_cipher_id(&tls)), 0x0A);
                        sys_print(" host=", 0x0A);
                        sys_print(req_host, 0x0A);
                        sys_print("\n", 0x0A);
                        send_request();
                        draw_browser(wid);
                    } else if (rc < 0 && rc != TLS_WANT_READ && rc != TLS_WANT_WRITE) {
                        // Negative and not a "call again": the handshake is
                        // over. TLS_ERR_VERIFY here means the chain or the
                        // hostname was rejected, which is the answer the user
                        // needs to see verbatim.
                        sys_print("[BROWSER] tls-fail ", 0x0C);
                        sys_print(tls_err_text(rc), 0x0C);
                        sys_print("\n", 0x0C);
                        finish_err(wid, tls_err_text(rc));
                    }
                } else if (browser_state == 3 && using_tls) {
                    char rx_buf[1024];
                    int rx_len;
                    while ((rx_len = tls_read(&tls, rx_buf, 1024)) > 0) {
                        int copy = rx_len;
                        if (raw_len + copy >= RAW_MAX - 1) copy = RAW_MAX - 1 - raw_len;
                        if (copy > 0) {
                            for (int i = 0; i < copy; i++) raw_buf[raw_len + i] = rx_buf[i];
                            raw_len += copy;
                            raw_buf[raw_len] = '\0';
                        }
                        if (response_complete()) break;
                        if (raw_len >= RAW_MAX - 1) break;
                        if (copy < rx_len) break;   // buffer full
                    }
                    if (rx_len == TLS_CLOSED) {
                        finish_ok(wid);
                    } else if (rx_len < 0) {
                        // An abrupt close without close_notify: if a body
                        // already arrived, render it (a close-delimited HTTP/1.0
                        // response is exactly this shape) but say so, because
                        // silently accepting a truncated stream is the bug
                        // TLS exists to prevent.
                        if (raw_len > 0) {
                            sys_print("[TLS] peer closed without close_notify\n", 0x0E);
                            finish_ok(wid);
                        } else {
                            sys_print("[BROWSER] tls-fail ", 0x0C);
                            sys_print(tls_err_text(rx_len), 0x0C);
                            sys_print("\n", 0x0C);
                            finish_err(wid, tls_err_text(rx_len));
                        }
                    } else if (response_complete()) {
                        finish_ok(wid);
                    } else {
                        tls_flush(&tls);   // push anything the engine queued
                    }
                } else if (browser_state == 3) {
                    // Drain everything available (recv is non-blocking:
                    // >0 data, 0 empty, -1 closed, -2 lost). Single-shot
                    // recvs paced the drain to the poll rate and stalled
                    // bulk pages once iterations slowed (v38.83).
                    char rx_buf[1024];
                    int rx_len;
                    while ((rx_len = sys_tcp_recv(conn_id, rx_buf, 1024)) > 0) {
                        int copy = rx_len;
                        if (raw_len + copy >= RAW_MAX - 1) copy = RAW_MAX - 1 - raw_len;
                        if (copy > 0) {
                            for (int i = 0; i < copy; i++) raw_buf[raw_len + i] = rx_buf[i];
                            raw_len += copy;
                            raw_buf[raw_len] = '\0';
                        }
                        char nb[12];
                        my_strcpy(T->status_msg, "Loading ");
                        my_itoa(raw_len, nb);
                        my_strcat(T->status_msg, nb);
                        my_strcat(T->status_msg, " bytes");
                        // Repaint at most every 10th received chunk: each
                        // draw costs a full WM re-composite, and repainting
                        // per chunk doubled QEMU's host CPU during a fetch
                        // (perceived as desktop lag while a page loads).
                        // finish_ok() repaints the final page regardless.
                        static int rx_paint_div = 0;
                        if (++rx_paint_div >= 10) { rx_paint_div = 0; draw_browser(wid); }
                        if (raw_len >= RAW_MAX - 1) break;
                    }
                    if (rx_len == -1) {
                        finish_ok(wid);
                    } else if (rx_len == -2) {
                        finish_err(wid, "Connection lost.");
                    } else if (response_complete()) {
                        // Content-Length satisfied: no need to wait for the
                        // peer to close, which is what used to make every
                        // fixed-length page sit until the server hung up.
                        finish_ok(wid);
                    }
                }
            }
        }

        sys_yield();
    }
}
