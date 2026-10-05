// scripts/tls_host_client.c — run the SHIPPED TLS 1.3 client on the host,
// against a real server, and report what it did in lines a test can read.
//
// Why this exists
// ---------------
// apps/tlsselftest.mct drives the engine's primitives (SHA-2, ChaCha20-Poly1305,
// AES-GCM, X25519, RSA, ECDSA, X.509) with committed RFC/OpenSSL vectors. It
// never drives a live handshake, because a guest has no server to talk to that
// would present a chain the image trusts. So the record layer, the key
// schedule, the certificate-list framing and the handshake state machine had no
// test at all until this driver: nine defects lived in them, and every one of
// them was invisible to the self-test.
//
// This is almost the same code as tlsselftest.c -- the engine and a transport
// of its own -- with two differences that only a host build can have:
//
//   * SYS_GETRANDOM and the RTC come from libc (scripts/tls_host_shim.h); and
//   * the trust store is the caller's, not the image's, so a throwaway CA can
//     stand in for a public one and BOTH the accepted and the refused path can
//     be driven end to end.
//
// The engine is compiled here with TLS_DEBUG_SECRETS so it logs the secrets it
// derives; scripts/tls_handshake_test.py compares them against the SERVER's
// keylog. That comparison is the point: it is the only check that can tell a
// correct-looking handshake with a wrong key schedule from a correct one.
//
// Output (stdout, one line each, all prefixed):
//   [TLS] ...                      the engine's own log
//   [DRV] handshake=<0|err> cipher=<name>
//   [DRV] request=<bytes sent>
//   [DRV] body=<bytes read> sha256=<hex>
//   [DRV] result=<ok|refused|fail>
//
// Always exits 0: the caller reads the lines, so a refusal is data, not a
// crash. (A host tool that exited non-zero on the expected outcome would force
// every caller to set -e special cases.)
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "apps/lib/tls/tls.h"

static int g_fd = -1;

static int s_send(void* io, const void* buf, uint32_t n) {
    (void)io;
    ssize_t r = send(g_fd, buf, n, 0);
    if (r < 0) return -1;
    return (int)r;
}

static int s_recv(void* io, void* buf, uint32_t n) {
    (void)io;
    ssize_t r = recv(g_fd, buf, n, 0);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    // EOF is a transport failure, not "nothing yet": returning 0 here would
    // make the engine's pump wait forever for bytes that are never coming.
    if (r == 0) return -1;
    return (int)r;
}

static void s_log(void* io, const char* m) {
    (void)io;
    printf("[TLS] %s\n", m);
    fflush(stdout);
}

static uint32_t now_ms(void* ctx) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <host> <port> <sni> [path]\n", argv[0]);
        return 2;
    }
    const char* host = argv[1];
    int port = atoi(argv[2]);
    const char* sni = argv[3];
    const char* path = argc > 4 ? argv[4] : "/";

    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portbuf, &hints, &res) != 0 || !res) {
        printf("[DRV] result=fail\n");
        return 0;
    }
    g_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (g_fd < 0 || connect(g_fd, res->ai_addr, res->ai_addrlen) != 0) {
        printf("[DRV] result=fail\n");
        return 0;
    }
    freeaddrinfo(res);
    struct timeval tv = { 5, 0 };
    setsockopt(g_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Static: the connection is ~40 KB plus the chain buffers, well past what
    // belongs on a stack.
    static tls_conn_t tls;
    tls_init(&tls, sni);
    tls_set_transport(&tls, s_send, s_recv, NULL, s_log);

    int rc = tls_handshake(&tls, now_ms, NULL, 20000);
    printf("[DRV] handshake=%d cipher=%s\n", rc, tls_cipher_name(tls_cipher_id(&tls)));
    if (rc != TLS_OK) {
        printf("[DRV] alert=%s\n", tls_last_alert(&tls));
        fflush(stdout);
        return 0;
    }

    char req[256];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                      path, sni);
    if (tls_write(&tls, req, (uint32_t)rl) != TLS_OK || tls_flush(&tls) != TLS_OK) {
        printf("[DRV] result=fail\n");
        return 0;
    }
    printf("[DRV] request=%d\n", rl);

    static char body[65536];
    uint32_t total = 0;
    int idle = 0;
    for (;;) {
        if (total >= sizeof(body)) break;
        int n = tls_read(&tls, body + total, (uint32_t)(sizeof(body) - total));
        if (n == 0) {
            // No data yet. The peer may still be writing the reply (or may have
            // closed already), so wait briefly rather than spinning.
            if (++idle > 60) break;
            usleep(100000);
            continue;
        }
        idle = 0;
        if (n < 0) break;
        total += (uint32_t)n;
    }

    uint8_t digest[32];
    tls_sha256(body, total, digest);
    printf("[DRV] body=%u sha256=", total);
    for (int i = 0; i < 32; i++) printf("%02x", digest[i]);
    printf("\n");
    printf("[DRV] result=%s\n", total > 0 ? "ok" : "fail");
    fflush(stdout);
    return 0;
}
