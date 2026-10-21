// apps/fetch.c — Mectov OS HTTPS fetcher (v38.169)
//
// A shell-facing HTTP/HTTPS client. The TLS 1.3 engine in apps/lib/tls is
// reused as-is (same link group as browser.mct): certificate chain
// verification against the 121-CA store, hostname matching, ChaCha20-Poly1305
// / AES-128-GCM — nothing new is implemented here.
//
// Usage:  run fetch.mct <url>
//   https://host[:port][/path]  -> TLS 1.3 on 443 (or the given port)
//   http://host[:port][/path]   -> plain HTTP
//   host[:port][/path]          -> https (the modern default)
//
// Unlike the kernel's `fetch` builtin (plain HTTP only, port 80 hardcoded),
// this app speaks TLS, parses the URL, and reports the negotiated cipher.
//
// The flow is linear and blocking-with-deadlines, because a shell has no
// event loop to pump: DNS (bounded) -> TCP connect (bounded) -> TLS
// handshake (bounded) -> request -> drain body (idle-bounded).

#include "lib/tls/tls.h"
#include "../src/include/syscall.h"

#define HOST_MAX   256
#define PATH_MAX   512
#define REQ_MAX    1024
#define BODY_CHUNK 1024

// Deadlines, in ticks (the kernel's millisecond counter).
#define DNS_TIMEOUT_TICKS   3000
#define CONNECT_TIMEOUT_TICKS 5000
#define TLS_TIMEOUT_TICKS   15000
#define IDLE_TIMEOUT_TICKS  15000

// ---- tiny string helpers (no libc dependency for the app itself) ----
static int  str_len(const char* s) { int n = 0; while (s[n]) n++; return n; }
static void str_cpy(char* d, const char* s) { int i = 0; while (s[i]) { d[i] = s[i]; i++; } d[i] = 0; }
static int  str_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static int  str_starts(const char* s, const char* pre) {
    while (*pre) { if (*s != *pre) return 0; s++; pre++; }
    return 1;
}
static int  is_digit(char c) { return c >= '0' && c <= '9'; }

static void out(const char* s) { sys_print(s, 0x0F); }

// The app links no libc, so a byte copy needs its own.
static void tls_memcpy_local(void* d, const void* s, unsigned n) {
    unsigned char* dd = (unsigned char*)d;
    const unsigned char* ss = (const unsigned char*)s;
    for (unsigned i = 0; i < n; i++) dd[i] = ss[i];
}
static void out_err(const char* s) { sys_print(s, 0x0C); }

static void die(const char* msg) {
    out("fetch: ");
    out_err(msg);
    out("\n");
    sys_exit();
}

// ---- URL state ----
static char host[HOST_MAX];
static char path[PATH_MAX];
static int  port = 443;
static int  use_tls = 1;

static void parse_url(const char* url) {
    const char* p = url;
    if (str_starts(p, "https://")) { use_tls = 1; port = 443; p += 8; }
    else if (str_starts(p, "http://")) { use_tls = 0; port = 80; p += 7; }
    else { use_tls = 1; port = 443; }   // bare host -> https

    int hl = 0;
    while (p[hl] && p[hl] != ':' && p[hl] != '/') {
        if (hl >= HOST_MAX - 1) die("host too long");
        hl++;
    }
    if (hl == 0) die("no host in url");
    for (int i = 0; i < hl; i++) host[i] = p[i];
    host[hl] = 0;

    int idx = hl;
    if (p[idx] == ':') {
        idx++;
        int pv = 0, digits = 0;
        while (is_digit(p[idx])) {
            pv = pv * 10 + (p[idx] - '0');
            idx++; digits++;
            if (digits > 5 || pv > 65535) die("bad port");
        }
        if (digits == 0) die("bad port");
        port = pv;
    }

    if (p[idx] == '/') {
        int pl = 0;
        while (p[idx + pl] && pl < PATH_MAX - 1) pl++;
        for (int i = 0; i < pl; i++) path[i] = p[idx + i];
        path[pl] = 0;
    } else {
        str_cpy(path, "/");
    }
}

// ---- TLS transport glue (same shape as apps/browser.c) ----
static int conn_id = -1;

static int tls_send_cb(void* ctx, const uint8_t* buf, uint32_t len) {
    (void)ctx;
    if (conn_id < 0) return -1;
    // sys_tcp_send clamps a call at 1400 bytes and returns what it took, so a
    // short write is normal here and the engine simply calls again.
    int n = sys_tcp_send(conn_id, buf, (int)len);
    return n < 0 ? -1 : n;
}

static int tls_recv_cb(void* ctx, uint8_t* buf, uint32_t len) {
    (void)ctx;
    if (conn_id < 0) return -1;
    int n = sys_tcp_recv(conn_id, buf, (int)len);
    return n < 0 ? -1 : n;
}

static void tls_log_cb(void* ctx, const char* msg) {
    (void)ctx;
    sys_print("[TLS] ", 0x0E);
    sys_print(msg, 0x0E);
    sys_print("\n", 0x0E);
}

static const char* tls_err_text(int rc) {
    switch (rc) {
        case -30: return "DEBUG hostname mismatch";
        case -31: return "DEBUG signature link failed";
        case -32: return "DEBUG no trust anchor";
        case -33: return "DEBUG cert parse failed";
        case -34: return "DEBUG cert time invalid";
        case TLS_ERR_IO:          return "transport closed";
        case TLS_ERR_PROTOCOL:    return "protocol error";
        case TLS_ERR_VERIFY:      return "certificate not trusted";
        case TLS_ERR_UNSUPPORTED: return "no shared cipher suite";
        case TLS_ERR_ENTROPY:     return "no entropy available";
        case TLS_ERR_OVERFLOW:    return "peer message too large";
        default:                  return "handshake failed";
    }
}

// The engine wants a millisecond clock for its own deadline checks.
static uint32_t now_ms_cb(void* ctx) {
    (void)ctx;
    return sys_get_ticks();
}

void _start(void) {
    char url[REQ_MAX];
    int alen = sys_get_launch_arg(url, sizeof(url));
    if (alen <= 0 || url[0] == '\0') {
        out("fetch: usage: fetch.mct <https://host[/path]>\n");
        sys_exit();
    }
    // A .mct path can arrive with the .mct suffix still attached (the shell
    // passes the launch argument through); strip it so the URL is clean.
    if (alen > 4 && str_eq(url + alen - 4, ".mct")) url[alen - 4] = 0;

    parse_url(url);

    out("fetch: ");
    out(host);
    out(use_tls ? " (https)" : " (http)");
    out("\n");

    // ---- 1. DNS ----
    uint32_t start = sys_get_ticks();
    sys_dns_resolve(host);
    net_status_t ns;
    int resolved = 0;
    for (;;) {
        sys_net_status(&ns);
        if (ns.dns_resolved) { resolved = 1; break; }
        if (sys_get_ticks() - start >= DNS_TIMEOUT_TICKS) break;
        sys_sleep(20);
    }
    if (!resolved) die("host not found (DNS timeout)");

    // ---- 2. TCP connect ----
    conn_id = sys_tcp_connect(ns.dns_ip, port);
    if (conn_id < 0) die("no free TCP connection slots");

    start = sys_get_ticks();
    for (;;) {
        sys_net_status(&ns);
        if (ns.tcp_state == 2) break;        // TCP_ESTABLISHED
        if (sys_get_ticks() - start >= CONNECT_TIMEOUT_TICKS) {
            sys_tcp_close(conn_id);
            die("connect timeout");
        }
        sys_sleep(20);
    }

    // ---- 3. TLS handshake (https only) ----
    static tls_conn_t tls;
    if (use_tls) {
        tls_init(&tls, host);
        tls_set_transport(&tls, tls_send_cb, tls_recv_cb, 0, tls_log_cb);
        int rc = tls_handshake(&tls, now_ms_cb, 0, TLS_TIMEOUT_TICKS);
        if (rc != TLS_OK) {
            const char* detail = tls_err_text(rc);
            const char* alert = tls_last_alert(&tls);
            sys_tcp_close(conn_id);
            out("fetch: TLS ");
            out_err(detail);
            if (alert && alert[0]) { out(" (alert "); out(alert); out(")"); }
            out("\n");
            sys_exit();
        }
        out("[TLS] ");
        out(tls_cipher_name(tls_cipher_id(&tls)));
        out(", certificate verified\n");
    }

    // ---- 4. Request ----
    char req[REQ_MAX];
    int n = 0;
    const char* m = "GET ";
    while (m[n]) { req[n] = m[n]; n++; }
    for (int i = 0; path[i] && n < REQ_MAX - 200; i++) req[n++] = path[i];
    m = " HTTP/1.1\r\nHost: ";
    for (int i = 0; m[i] && n < REQ_MAX - 200; i++) req[n++] = m[i];
    for (int i = 0; host[i] && n < REQ_MAX - 200; i++) req[n++] = host[i];
    // Omit the port when it is the scheme's default: "example.com:443" is
    // legal but makes some virtual hosts answer a different site than the
    // certificate was issued for.
    int default_port = use_tls ? 443 : 80;
    if (port != default_port) {
        req[n++] = ':';
        char pb[8];
        int pn = 0;
        int pv = port;
        if (pv == 0) pb[pn++] = '0';
        while (pv > 0) { pb[pn++] = '0' + (pv % 10); pv /= 10; }
        for (int i = pn - 1; i >= 0 && n < REQ_MAX - 100; i--) req[n++] = pb[i];
    }
    m = "\r\nUser-Agent: MectovFetch/1.0\r\nConnection: close\r\n\r\n";
    for (int i = 0; m[i] && n < REQ_MAX - 2; i++) req[n++] = m[i];
    req[n] = 0;

    if (use_tls) {
        if (tls_write(&tls, req, (uint32_t)n) != TLS_OK) {
            sys_tcp_close(conn_id);
            die("could not send request");
        }
        tls_flush(&tls);
    } else {
        sys_tcp_send(conn_id, req, n);
    }

    // ---- 5. Drain the response ----
    char buf[BODY_CHUNK + 1];
    start = sys_get_ticks();
    for (;;) {
        int r;
        if (use_tls) {
            r = tls_read(&tls, buf, BODY_CHUNK);
            if (r == TLS_CLOSED) break;
            if (r < 0) { out("\n[fetch] connection lost.\n"); break; }
            if (r == 0) {
                if (sys_get_ticks() - start >= IDLE_TIMEOUT_TICKS) {
                    out("\n[fetch] idle timeout.\n");
                    break;
                }
                sys_sleep(20);
                continue;
            }
        } else {
            r = sys_tcp_recv(conn_id, buf, BODY_CHUNK);
            if (r < 0) break;                 // EOF / reset
            if (r == 0) {
                if (sys_get_ticks() - start >= IDLE_TIMEOUT_TICKS) {
                    out("\n[fetch] idle timeout.\n");
                    break;
                }
                sys_sleep(20);
                continue;
            }
        }
        // sys_print takes a NUL-terminated string and the kernel refuses
        // anything longer than 256 bytes, so a page body has to go out in
        // chunks. Printing the whole thing at once silently dropped it.
        int off = 0;
        while (off < r) {
            int chunk = r - off;
            if (chunk > 200) chunk = 200;
            char tmp[201];
            tls_memcpy_local(tmp, buf + off, chunk);
            tmp[chunk] = 0;
            out(tmp);
            off += chunk;
        }
        start = sys_get_ticks();             // activity resets the idle clock
    }

    out("\n[fetch] done.\n");
    if (use_tls) tls_send_close_notify(&tls);
    sys_tcp_close(conn_id);
    sys_exit();
}
